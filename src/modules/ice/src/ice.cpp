/**
 * @file src/modules/ice/src/ice.cpp
 * @brief ICE Transport — libjuice-backed ITransport implementation.
 *
 * Wires the libjuice ICE agent (vendored under third_party/libjuice/) into the
 * NimRTC plugin transport interface (ITransport). All STUN/TURN/connectivity
 * check work is delegated to libjuice; this file is a thin adapter.
 *
 * ## Threading
 *
 * - libjuice runs in JUICE_CONCURRENCY_MODE_MUX by default. Its internal
 *   thread owns the UDP socket and dispatches all STUN/TURN/data I/O.
 * - Inbound data packets and state-change/candidate/gathering-done events
 *   arrive on libjuice's thread. We forward them to the engine via the
 *   ITransport callbacks after a mutex-protected handoff.
 * - The engine thread drives `recv()` to drain queued packets and calls
 *   all other IceTransport methods. These are serialized against the
 *   libjuice callbacks via the same mutex.
 */

#include <nimrtc/ice/ice.hpp>

#include "tcp_ice.hpp"
#include "turn_tls.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <juice/juice.h>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>   // PluginRegistry (Layout Invariant 6)

namespace nimrtc::ice {

namespace {

// -----------------------------------------------------------------------------
// libjuice → nimrtc::core::log bridge (one-shot init).
//
// ## Why the level is INFO and not VERBOSE
//
// libjuice's VERBOSE and DEBUG levels are *per-datagram*: "Receiving
// datagram", "Sending datagram, size=%d", "Demultiplexing incoming
// datagram from %s", "Bookkeeping...", "Entering poll for %d ms" and
// their DEBUG siblings are written once per packet in each direction.
//
// The 2026-11-20 Q5 impaired soak measured this: of 772 043 log lines
// in 950 s, 660 620 (85 %) were libjuice's, and 71 % of the log's *bytes*
// were per-datagram noise.  The projection for a 24 h window was ~3.2 GB
// of log, of which ~2.3 GB was libjuice's per-datagram chatter.
//
// That is not a trace, it is a firehose, and it has the same defect the
// engine's own DTLS line had: it buries the lines that matter.  A soak
// log is evidence, and evidence nobody can read is not evidence.
//
// INFO is the right cut.  libjuice's INFO level carries exactly the
// lifecycle lines a reader needs and nothing per-packet:
//
//     Changing state to %s
//     Candidate gathering done
//     STUN server binding successful
//     Got STUN mapped address %s from server
//     STUN entry %d: Consent expired for candidate pair
//     Connectivity timer expired
//     TURN allocation failed
//
// The failure lines that the Q5 analysis needed -- "Consent expired",
// "Lost connectivity", "Changing state to failed" -- are INFO and WARN,
// and they survive this cut.
//
// VERBOSE is not gone: `NIMRTC_ICE_VERBOSE=1` turns it back on for the
// case where a developer *is* chasing a packet-level problem.  The
// default has to be the one a 24 h run can afford.
//
// ## Why the handler promotes rather than filters
//
// The level filter lives in `core::log::Logger`, whose default minimum
// is Info.  libjuice's DEBUG and VERBOSE lines are routed through
// `logger.info()` rather than `logger.debug()` so they would show up in
// test output even at the default level -- that was the point of the
// original hook, and it is why the cut has to happen at the *source*
// (`juice_set_log_level`) rather than in the handler.
//
// A consequence worth stating: the handler's promotion means the
// *core* log level cannot be used to filter libjuice.  Turning the core
// level down to Warn would also silence libjuice's INFO lines.  That is
// the pre-existing behaviour and this change does not alter it.
// -----------------------------------------------------------------------------

std::atomic<bool> g_log_hooked{false};

// Process-wide latch: a configured TURN server rejected the credentials
// during some gathering round.  libjuice exposes the rejection *only*
// through its log handler (agent.c `agent_process_turn_allocate`:
// "TURN authentication failed" once the realm is known), so the fact is
// captured here and each IceTransport clears it when it starts a fresh
// gathering round — see `turn_credentials_rejected()`.  With several
// transports in one process the latch reports the *latest* rejection,
// which is exactly what the sequential acceptance scenarios need.
std::atomic<bool> g_turn_auth_rejected{false};
/// True when the developer asked for libjuice's per-datagram trace.
bool juice_verbose_requested() noexcept {
    static const bool requested = [] {
        const char* value = std::getenv("NIMRTC_ICE_VERBOSE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return requested;
}

void hook_libjuice_logging_once() noexcept {
    if (g_log_hooked.exchange(true)) return;

    // The cut is here, at the source: `juice_log_write()` drops a line
    // whose level is below this one *before* it formats the message, so
    // the per-datagram lines cost nothing but a comparison.
    juice_set_log_level(juice_verbose_requested() ? JUICE_LOG_LEVEL_VERBOSE
                                                   : JUICE_LOG_LEVEL_INFO);

    juice_set_log_handler([](juice_log_level_t level, const char* message) {
        // The one failure libjuice has no public callback for: a TURN
        // server rejecting the credentials.  agent.c logs "TURN
        // authentication failed" (after receiving a 401/403 with a known
        // realm) before failing the allocation entry — see
        // `turn_credentials_rejected()`.
        if (level == JUICE_LOG_LEVEL_ERROR && message != nullptr &&
            std::strstr(message, "TURN authentication failed") != nullptr) {
            g_turn_auth_rejected.store(true, std::memory_order_release);
        }

        auto& logger = core::log::Logger::instance();
        switch (level) {
            case JUICE_LOG_LEVEL_ERROR:
            case JUICE_LOG_LEVEL_FATAL:
                logger.error(message);
                break;
            case JUICE_LOG_LEVEL_WARN:
                logger.warn(message);
                break;
            case JUICE_LOG_LEVEL_INFO:
            case JUICE_LOG_LEVEL_DEBUG:
            case JUICE_LOG_LEVEL_VERBOSE:
                // Route everything else through info() so it bypasses the
                // default Debug-level filter and shows up in test output.
                logger.info(message);
                break;
            default:
                break;
        }
    });
}

// -----------------------------------------------------------------------------
// State / packet handoff (libjuice thread → engine thread).
// -----------------------------------------------------------------------------

struct Packet {
    std::vector<std::uint8_t> data;
    plugins::Addr             src;     // best-effort source (may be empty)
};

plugins::IceState map_juice_state(juice_state_t s) noexcept {
    switch (s) {
        case JUICE_STATE_DISCONNECTED: return plugins::IceState::Disconnected;
        case JUICE_STATE_GATHERING: return plugins::IceState::Gathering;
        case JUICE_STATE_CONNECTING: return plugins::IceState::Connecting;
        case JUICE_STATE_CONNECTED: return plugins::IceState::Connected;
        case JUICE_STATE_COMPLETED: return plugins::IceState::Completed;
        case JUICE_STATE_FAILED: return plugins::IceState::Failed;
    }
    return plugins::IceState::Disconnected;
}

juice_concurrency_mode_t map_mode(IceConfig::Mode m) noexcept {
    switch (m) {
        case IceConfig::Mode::Poll:   return JUICE_CONCURRENCY_MODE_POLL;
        case IceConfig::Mode::Mux:    return JUICE_CONCURRENCY_MODE_MUX;
        case IceConfig::Mode::Thread: return JUICE_CONCURRENCY_MODE_THREAD;
    }
    return JUICE_CONCURRENCY_MODE_MUX;
}

/** Resolve the concurrency mode the transport will actually run with.
 *
 *  libjuice's multiplexer (JUICE_CONCURRENCY_MODE_MUX) owns exactly one
 *  UDP socket and multiplexes every agent on it.  A TURN allocation
 *  needs a *second* socket (the allocation's own relayed port), which
 *  the multiplexer cannot provide — so libjuice warns and then ignores
 *  the configured TURN servers entirely:
 *
 *      // libjuice src/agent.c
 *      if (mode == JUICE_CONCURRENCY_MODE_MUX) {
 *          if (agent->config.turn_servers_count > 0)
 *              JLOG_WARN("TURN servers are not supported in mux mode");
 *      } else if (agent->config.turn_servers_count > 0) { ... }
 *
 *  The consequence was that NimRTC advertised a TURN configuration that
 *  could never produce a relay candidate: the host's `add_turn_server()`
 *  call succeeded, the log line said "TURN server added", and then
 *  gathering finished with no `typ relay` candidate at all.  Because the
 *  warning only reaches libjuice's own log handler, nothing in NimRTC
 *  ever surfaced the contradiction.
 *
 *  The fix: when TURN servers are configured and the caller did not pin
 *  the concurrency mode explicitly, run in POLL mode, which gives each
 *  agent its own socket and therefore its own TURN allocation.  The
 *  caller's explicit choice still wins — an application that pins MUX
 *  keeps MUX, and the (now explicit) limitation is reported to it. */
IceConfig::Mode resolve_mode(const IceConfig& cfg) noexcept {
    const bool needs_turn = !cfg.turn_servers.empty();
    if (!needs_turn || cfg.mode_is_explicit) return cfg.mode;

    if (cfg.mode == IceConfig::Mode::Mux) {
        return IceConfig::Mode::Poll;
    }
    return cfg.mode;
}

namespace sdp_util {

/** Extract the value of an SDP attribute, e.g. "ice-ufrag:" -> "abcd".
 *
 *  Accepts both the "a=<name>:" and the bare "<name>:" form and tolerates
 *  CRLF line endings.  Returns an empty string when the attribute is absent.
 *
 *  Used to spot an ICE restart (RFC 8445 §9.1.1.1): the peer signals it by
 *  publishing a different ice-ufrag / ice-pwd pair. */
std::string extract_sdp_attr(const std::string& sdp, const std::string& name) {
    const std::string prefixed = "a=" + name;

    std::size_t pos = sdp.find(prefixed);
    if (pos == std::string::npos) {
        // Bare form, without the "a=" prefix.
        pos = sdp.find(name);
        if (pos == std::string::npos) return {};
        // Guard against matching a longer attribute that ends with `name`
        // (e.g. "x-ice-ufrag:").
        if (pos > 0) {
            const char prev = sdp[pos - 1];
            if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '-') {
                return {};
            }
        }
    } else {
        pos += 1;   // skip the "a="
    }

    pos += name.size();
    const auto eol = sdp.find('\n', pos);
    std::string value = sdp.substr(
        pos, eol == std::string::npos ? std::string::npos : eol - pos);
    while (!value.empty() &&
           (value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
    }
    return value;
}

} // namespace sdp_util

/** Project a gathered TCP candidate (RFC 6544) onto the concrete
 *  Candidate shape, so the TCP winner flows through the same
 *  selected_pair() projection as the UDP one. */
Candidate tcp_to_candidate(const TcpCandidate& c) {
    Candidate out;
    out.type         = CandidateType::Host;
    out.foundation   = c.foundation;
    out.component_id = c.component_id;
    out.transport    = "TCP";
    out.priority     = c.priority();
    out.host         = c.host;
    out.port         = c.port;
    out.tcptype      = to_string(c.tcptype);
    return out;
}

} // anonymous namespace

// =============================================================================
// Candidate helpers
// =============================================================================

std::optional<Candidate> parse_candidate(std::string_view sdp) {
    // Accept "a=candidate:..." or "candidate:..."; drop the optional prefix.
    constexpr std::string_view kPrefixA = "a=candidate:";
    constexpr std::string_view kPrefix  = "candidate:";
    if (sdp.substr(0, kPrefixA.size()) == kPrefixA) {
        sdp.remove_prefix(kPrefixA.size());
    } else if (sdp.substr(0, kPrefix.size()) == kPrefix) {
        sdp.remove_prefix(kPrefix.size());
    } else {
        return std::nullopt;
    }

    Candidate c;
    std::istringstream iss{std::string{sdp}};

    std::string tok;
    unsigned tmp_component_id = 0;  // uint8_t reads as char otherwise
    iss >> c.foundation;        // 1
    iss >> tmp_component_id;    // 2
    c.component_id = static_cast<std::uint8_t>(tmp_component_id);
    iss >> c.transport;         // 3
    iss >> c.priority;          // 4
    iss >> c.host;              // 5
    iss >> c.port;              // 6
    iss >> tok;                 // "typ"
    std::string type_str;
    iss >> type_str;
    if (type_str == "host")  c.type = CandidateType::Host;
    else if (type_str == "srflx") c.type = CandidateType::Srflx;
    else if (type_str == "prflx") c.type = CandidateType::Prflx;
    else if (type_str == "relay") c.type = CandidateType::Relay;
    else return std::nullopt;

    // Optional "tcptype active|passive|so" (RFC 6544 §4.1).  Comes after
    // the "typ <type>" pair — never confuse it with "typ" itself.
    if (iss >> tok && tok == "tcptype") {
        iss >> c.tcptype;
        iss >> tok;  // step past the value; tok may now be "raddr"
    }

    // Optional "raddr ... rport ..." — when tcptype was absent, the first
    // read above already consumed "raddr" into tok; when present, the read
    // above did.  Either way tok holds the token to inspect now.
    if (tok == "raddr") {
        iss >> c.related_address;
        iss >> tok; // "rport"
        iss >> c.related_port;
    }
    return c;
}

std::string Candidate::to_sdp() const {
    static const char* kTypeNames[] = {"host", "srflx", "prflx", "relay"};
    std::ostringstream oss;
    oss << "candidate:" << foundation
        << ' ' << static_cast<unsigned>(component_id)
        << ' ' << transport
        << ' ' << priority
        << ' ' << host
        << ' ' << port
        << " typ " << kTypeNames[static_cast<int>(type)];
    if (!tcptype.empty()) {
        oss << " tcptype " << tcptype;
    }
    if (!related_address.empty()) {
        oss << " raddr " << related_address << " rport " << related_port;
    }
    return oss.str();
}

// -----------------------------------------------------------------------------
// Related-address repair
// -----------------------------------------------------------------------------
//
// libjuice hardcodes "raddr 0.0.0.0 rport 0" into every server-reflexive and
// relayed candidate it generates:
//
//      // libjuice src/ice.c, ice_generate_candidate_sdp()
//      case ICE_CANDIDATE_TYPE_RELAYED:
//          type = "relay";
//          suffix = "raddr 0.0.0.0 rport 0"; // This is needed for compatibility
//                                            // with Firefox
//
// That is a placeholder, not a real related address.  RFC 8445 §5.1.2 says
// the raddr/rport pair of a candidate names the *related transport address*
// it was derived from — for a relayed candidate that is the server-reflexive
// (mapped) address the TURN allocation was made behind.
//
// NimRTC fills the real value in at the candidate-callback boundary, so the
// SDP that goes out on the wire carries the true related address.
//
// Returns true when the SDP was rewritten.
bool patch_related_address(std::string& sdp,
                           const std::string& addr,
                           std::uint16_t port) {
    if (addr.empty() || port == 0) return false;

    // Only the placeholder needs repair; a real raddr is left alone.
    static constexpr std::string_view kPlaceholder = "raddr 0.0.0.0 rport 0";
    const auto pos = sdp.find(kPlaceholder);
    if (pos == std::string::npos) return false;

    sdp.replace(pos, kPlaceholder.size(),
                "raddr " + addr + " rport " + std::to_string(port));
    return true;
}

// =============================================================================
// IceTransport::Impl
// =============================================================================

struct IceTransport::Impl {
    IceConfig                            config;
    juice_agent_t*                       agent = nullptr;

    // libjuice callbacks fire on libjuice's thread; protect shared state.
    mutable std::mutex                   mu;
    std::condition_variable              cv_gather;

    // Local candidates collected via cb_candidate (each entry is the
    // libjuice-formatted SDP body, e.g.
    //   "candidate:1 1 UDP 2113929471 192.0.2.1 5000 typ host").
    std::vector<std::string>             local_candidates;
    bool                                gathering_done = false;

    // Related-address bookkeeping.
    //
    // libjuice emits "raddr 0.0.0.0 rport 0" for every server-reflexive and
    // relayed candidate.  We remember the real addresses as they are
    // gathered (host first, then srflx, then relay) and substitute them so
    // the emitted SDP is RFC 8445 §5.1.2 conformant.
    std::string                          host_addr;
    std::uint16_t                        host_port = 0;
    std::string                          srflx_addr;
    std::uint16_t                        srflx_port = 0;

    // ---- Connectivity diagnostics (v1.4 plan Track N / N4) ----------------
    //
    // The gathering phase is derived from `agent` and `gathering_done`
    // (above).  The three TURN facts are recorded here because libjuice
    // exposes none of them through a public callback: the relayed-candidate
    // attribution uses the configured-server order (agent.h has no
    // server identity on `agent_add_local_relayed_candidate`), and a
    // credentials rejection is only observable through the module's log
    // hook (the process-wide `g_turn_auth_rejected` latch).
    bool                                 unsupported_turn_transport_ = false;
    unsigned                             relay_candidates_seen_ = 0;
    std::optional<TurnTransport>         turn_transport_used_;

    plugins::IceState                             state = plugins::IceState::Disconnected;
    std::deque<Packet>                   rx_queue;
    plugins::RecvCallback                recv_cb_;     // renamed to avoid collision with static on_recv
    plugins::ErrorCallback               on_error;

    // Latest selected addresses, refreshed on state → Connected/Completed.
    std::string                          selected_local_str;
    std::string                          selected_remote_str;

    // Cached credentials parsed from the remote SDP (for diagnostics).
    std::string                          remote_ufrag_str;
    std::string                          remote_pwd_str;

    // Remote SDP set before the agent was created.  Applied in create_agent()
    // before agent_gather_candidates() so libjuice knows the peer credentials
    // and sets the correct mode (CONTROLLED instead of CONTROLLING).
    std::string                          pending_remote_sdp_;

    // ---- ICE restart state (RFC 8445 §9.1.1.1) ---------------------------
    //
    // A restart replaces the whole libjuice agent:
    //
    //   - juice_set_local_ice_attributes() fails with JUICE_ERR_FAILED once
    //     gathering started, so the local credentials cannot be changed in
    //     place;
    //   - juice_set_remote_description() refuses a description whose ufrag
    //     differs from the current one ("ICE restart is not supported").
    //
    // Rebuilding the agent is therefore the only way to obtain the fresh
    // credential pair RFC 8445 §9.1.1.1 requires, and it additionally gives
    // us a clean connectivity-check state (no half-finished transactions
    // from the previous session).
    //
    // What survives a restart:
    //   - the remote candidates (re-added to the new agent);
    //   - the local candidate list is rebuilt from scratch, but the host
    //     candidates are gathered synchronously inside juice_gather_candidates(),
    //     so the list is already populated when restart() returns.
    //
    // The credentials the application pinned in IceConfig apply to the first
    // session only: a restart always mints new ones, because the peer can
    // only observe a restart by observing changed credentials.
    std::string                          pinned_ufrag_;
    std::string                          pinned_password_;
    bool                                 pinned_creds_valid_ = false;

    /** The remote description currently installed on the agent.  Kept so a
     *  restart can re-apply it (and so a credential change can be detected). */
    std::string                          applied_remote_sdp_;

    /** Remote candidates added through add_remote_candidate().  Re-applied
     *  after a restart. */
    std::vector<std::string>             remote_candidates_;

    std::uint32_t                        restart_count_ = 0;

    /** Fired when the peer restarts ICE (the host must answer with the new
     *  local credentials). */
    plugins::OnIceRestart                on_ice_restart_;

    // ---- ICE consent freshness (RFC 7675 / RFC 8445 §10) ----------------
    //
    // App-level tracker layered on top of libjuice. libjuice owns the
    // protocol side (sends STUN Binding requests on its own schedule and
    // maintains `consent_expiry` per candidate pair). The NimRTC wrapper
    // adds:
    //   - a single OnConsentLost callback that the application can hook;
    //   - a `notify_binding_received()` entry point that resets the timer;
    //   - test-friendly observability via consent_lost_pending().
    //
    // All timestamps are wall-clock microseconds since the steady_clock
    // epoch (monotonic, no leap-second issues).
    //
    // The timer state below (last_binding_received_us_ / consent_lost_fired_)
    // is NOT atomic: it is guarded by consent_mu_ so that the "expired?" check,
    // the "fired" flag update and the rearm happen as a single critical
    // section. With plain atomics the tracker thread could read a stale
    // `last_binding_received_us_` right after notify_binding_received()
    // reset it and fire a second time *inside* a freshly armed timeout
    // window. That race is observable on both x86 (store-buffer visibility
    // lag) and ARM (cache-coherence lag), and it made
    // IceConsentFreshness.T3_BindingReceivedRearmsTimer flaky on CI.
    std::atomic<bool>                    consent_thread_should_stop_{false};
    std::atomic<bool>                    consent_thread_running_{false};
    /** Test-only override: arm the tracker as if Connected. */
    std::atomic<bool>                    consent_force_armed_{false};
    std::thread                          consent_thread_;
    /** Guards on_consent_lost_ + the consent timer state. */
    std::mutex                           consent_mu_;
    std::int64_t                         last_binding_received_us_{0};
    bool                                 consent_lost_fired_{false};
    plugins::OnConsentLost               on_consent_lost_;

    // ---- Plugin injection (BWE + Scheduler) --------------------------------
    // Non-owning raw pointers set by set_bwe() / set_scheduler().
    // The engine owns the plugin instances; the ICE transport merely
    // references them to call bwe->on_feedback() and sched->drain_with().
    plugins::IBwe*         bwe_         = nullptr;
    plugins::IScheduler*    scheduler_   = nullptr;

    // ---- TURN over TLS (RFC 6062) -----------------------------------------
    //
    // The TLS client is owned here and outlives the libjuice agent, which
    // keeps a pointer to it through the transport seam.  It is only
    // created when a TURN server actually asks for the TLS transport:
    // creating a wolfSSL context for a session that never happens would
    // cost memory and load the trust store for nothing.
    std::unique_ptr<TurnTlsClient>  turn_tls_;

    // ---- ICE over TCP (RFC 6544) -------------------------------------------
    //
    // libjuice's ICE layer is UDP-only: it drops every candidate whose
    // transport is not "UDP" on arrival.  The TCP path is therefore
    // NimRTC's own driver (ADR-028, Option A), entered only when the UDP
    // agent has failed (Decision 3).  The seam publishes the driver's
    // gathered candidates alongside libjuice's, intercepts remote TCP
    // candidates, and reports the driver's nominated pair through
    // selected_pair() before libjuice's (Decision 6).
    std::unique_ptr<TcpIceDriver>       tcp_driver_;
    /// Gathered TCP candidates in SDP-body form ("candidate:tcp-1 1 TCP
    /// <pri> <host> <port> typ host tcptype active"), appended to
    /// local_description() / gathered_local_candidates().
    std::vector<std::string>            tcp_local_candidates_;
    /// Remote TCP candidates trickled in through add_remote_candidate()
    /// (RFC 8838).  They can arrive before the UDP path fails, so they
    /// are kept and re-fed to the driver when it is created.
    std::vector<TcpCandidate>           tcp_remote_candidates_;

    /// Enter the TCP path after the UDP agent failed (ADR-028 Decision 3).
    /// Idempotent; caller holds mu_.  Re-runs after a restart once the new
    /// UDP agent fails again.
    void start_tcp_fallback();

    // ---- libjuice static callbacks ---------------------------------------

    static void on_state(juice_agent_t*, juice_state_t st, void* user) {
        auto* self = static_cast<Impl*>(user);
        std::lock_guard<std::mutex> lk(self->mu);
        self->state = map_juice_state(st);

        if (st == JUICE_STATE_CONNECTED || st == JUICE_STATE_COMPLETED) {
            char local[JUICE_MAX_ADDRESS_STRING_LEN] = {0};
            char remote[JUICE_MAX_ADDRESS_STRING_LEN] = {0};
            if (juice_get_selected_addresses(self->agent, local, sizeof(local),
                                             remote, sizeof(remote)) == JUICE_ERR_SUCCESS) {
                self->selected_local_str  = local;
                self->selected_remote_str = remote;
            }
        }

        // The UDP agent gave up (no pair was nominated before the
        // connectivity timer expired, RFC 8863 §7.2.1.3).  Fall back to
        // the TCP driver (RFC 6544) — see start_tcp_fallback().
        if (st == JUICE_STATE_FAILED) {
            self->start_tcp_fallback();
        }
    }

    static void on_candidate(juice_agent_t*, const char* sdp, void* user) {
        // New local candidate discovered (host / srflx / relay / prflx).
        // We push it onto the local_candidates list so the engine can
        // include it in SDP.  Mutex held briefly; this fires on libjuice's
        // thread.
        auto* self = static_cast<Impl*>(user);
        if (!sdp) return;
        std::lock_guard<std::mutex> lk(self->mu);

        std::string line{sdp};

        // Repair the placeholder related address that libjuice hardcodes.
        //
        // The related address of a server-reflexive candidate is the host
        // candidate it was derived from; the related address of a relayed
        // candidate is the server-reflexive address the TURN allocation was
        // made behind (RFC 8445 §5.1.2).
        auto parsed = parse_candidate(line);
        if (parsed) {
            switch (parsed->type) {
            case CandidateType::Host:
                if (self->host_addr.empty()) {
                    self->host_addr  = parsed->host;
                    self->host_port  = parsed->port;
                }
                break;

            case CandidateType::Srflx:
                patch_related_address(line, self->host_addr, self->host_port);
                self->srflx_addr = parsed->host;
                self->srflx_port = parsed->port;
                break;

            case CandidateType::Relay:
                // Prefer the server-reflexive address; fall back to the host
                // address if no srflx candidate was reported.
                if (self->srflx_addr.empty()) {
                    patch_related_address(line, self->host_addr, self->host_port);
                } else {
                    patch_related_address(line, self->srflx_addr, self->srflx_port);
                }
                // Attribute the relayed candidate to its configured TURN
                // server.  libjuice reports relayed candidates in the order
                // the servers produced allocations, which is the config
                // order, so the k-th relayed candidate comes from the k-th
                // configured server.  With a single configured server —
                // every acceptance scenario — the attribution is exact.
                if (!self->config.turn_servers.empty()) {
                    const std::size_t idx =
                        std::min<std::size_t>(self->relay_candidates_seen_,
                                              self->config.turn_servers.size() - 1);
                    self->turn_transport_used_ =
                        self->config.turn_servers[idx].transport;
                    ++self->relay_candidates_seen_;
                }
                break;

            default:
                break;
            }
        }

        self->local_candidates.emplace_back(std::move(line));
    }

    static void on_gathering_done(juice_agent_t*, void* user) {
        auto* self = static_cast<Impl*>(user);
        {
            std::lock_guard<std::mutex> lk(self->mu);
            self->gathering_done = true;
        }
        self->cv_gather.notify_all();
        core::log::Logger::instance().info(
            "nimrtc::ice: local candidate gathering done");
    }

    static void juice_recv_cb(juice_agent_t*, const char* data, std::size_t size, void* user) {
        auto* self = static_cast<Impl*>(user);
        Packet pkt;
        pkt.data.assign(reinterpret_cast<const std::uint8_t*>(data),
                        reinterpret_cast<const std::uint8_t*>(data) + size);
        // src: best effort — in MUX mode the selected remote is the source.
        {
            std::lock_guard<std::mutex> lk(self->mu);
            pkt.src = self->encode_addr(self->selected_remote_str);
            self->rx_queue.push_back(std::move(pkt));
        }
    }

    // ---- helpers ---------------------------------------------------------

    /** Encode a "host:port" string into a plugins::Addr (text-only encoding
     *  internal to IceTransport). */
    plugins::Addr encode_addr(const std::string& s) const noexcept {
        plugins::Addr a{};
        const std::size_t n = std::min(s.size(), sizeof(a.data));
        std::memcpy(a.data, s.data(), n);
        a.len = static_cast<std::uint32_t>(n);
        return a;
    }

    /** Build the juice_config_t from IceConfig and create the agent. */
    bool create_agent() {
        juice_config_t jc{};
        jc.concurrency_mode = map_mode(resolve_mode(config));
        jc.stun_server_host = config.stun_server_host.empty()
                                  ? nullptr
                                  : config.stun_server_host.c_str();
        jc.stun_server_port = config.stun_server_port;
        jc.bind_address = config.bind_address.empty()
                              ? nullptr
                              : config.bind_address.c_str();
        jc.local_port_range_begin = config.local_port_range_begin;
        jc.local_port_range_end   = config.local_port_range_end;

        std::vector<juice_turn_server_t> jt;
        jt.reserve(config.turn_servers.size());
        bool needs_tls = false;
        for (const auto& t : config.turn_servers) {
            juice_turn_server_t s{};
            s.host = t.host.c_str();
            s.port = t.port;
            s.username = t.username.empty() ? nullptr : t.username.c_str();
            s.password = t.password.empty() ? nullptr : t.password.c_str();
            s.transport = static_cast<juice_turn_transport_t>(t.transport);
            s.server_name = t.tls_server_name.empty() ? nullptr : t.tls_server_name.c_str();
            if (t.transport == TurnTransport::Tls) needs_tls = true;
            jt.push_back(s);
        }
        if (!jt.empty()) {
            jc.turn_servers      = jt.data();
            jc.turn_servers_count = static_cast<int>(jt.size());
        }

        // TURN over TLS (RFC 6062): the TLS client lives in this module
        // and is handed to libjuice through the transport seam.  It is
        // created only when a TURN server actually asks for it.
        if (needs_tls) {
            TurnTlsClient::Config tls_config;
            tls_config.ca_file         = config.turn_tls_ca_file;
            tls_config.client_cert_file = config.turn_tls_client_cert_file;
            tls_config.client_key_file  = config.turn_tls_client_key_file;
            tls_config.verify_peer      = config.turn_tls_verify_peer;
            tls_config.check_hostname   = true;

            turn_tls_ = std::make_unique<TurnTlsClient>(std::move(tls_config));
            if (!turn_tls_->usable()) {
                core::log::Logger::instance().error(
                    "ice: the TURN/TLS client is unusable (" + turn_tls_->last_error() +
                    "); the TURN allocation will fail");
            } else {
                jc.turn_tls_ops = turn_tls_->ops();
            }
        }

        jc.cb_state_changed    = &Impl::on_state;
        jc.cb_candidate        = &Impl::on_candidate;
        jc.cb_gathering_done   = &Impl::on_gathering_done;
        jc.cb_recv             = &Impl::juice_recv_cb;
        jc.user_ptr            = this;

        juice_agent_t* a = juice_create(&jc);
        if (!a) return false;
        agent = a;

        // Apply the pinned credentials, but only while they are still valid.
        // An ICE restart clears them so libjuice mints a fresh pair.
        if (pinned_creds_valid_ && !pinned_ufrag_.empty() && !pinned_password_.empty()) {
            juice_set_local_ice_attributes(agent,
                                           pinned_ufrag_.c_str(),
                                           pinned_password_.c_str());
        }

        // Apply any remote SDP set before the agent was created.  This is the
        // key fix for the ICE role conflict on loopback: if the remote ufrag/pwd
        // are known BEFORE agent_gather_candidates() is called, libjuice sets
        // the agent's mode to CONTROLLED (answerer) instead of CONTROLLING.
        if (!pending_remote_sdp_.empty()) {
            std::string sdp_copy{pending_remote_sdp_};
            juice_set_remote_description(agent, sdp_copy.c_str());
            pending_remote_sdp_.clear();
        }

        return true;
    }

    // ---------------------------------------------------------------------
    // ICE restart (RFC 8445 §9.1.1.1)
    // ---------------------------------------------------------------------

    enum class RestartResult {
        kOk,
        kNotReady,
        kFailed,
    };

    /** Rebuild the libjuice agent, i.e. perform an ICE restart.
     *
     *  @param apply_remote_sdp  When non-empty, this remote description is
     *                           installed on the new agent.  Used by the
     *                           remote-initiated path, where the peer already
     *                           sent its new credentials.
     *  @param keep_remote_sdp   When true, the remote description that was
     *                           installed on the old agent is carried over.
     *                           The local-initiated path passes false: the
     *                           peer's old credentials are stale from the
     *                           moment we change ours, so the new agent
     *                           starts without a remote description and
     *                           waits for the peer's answer.
     */
    RestartResult rebuild_agent(std::string_view apply_remote_sdp,
                                bool keep_remote_sdp) {
        if (!agent) return RestartResult::kNotReady;

        // Work out which remote description the new agent starts with.
        std::string remote_sdp;
        if (!apply_remote_sdp.empty()) {
            remote_sdp = std::string(apply_remote_sdp);
        } else if (keep_remote_sdp) {
            remote_sdp = applied_remote_sdp_;
        }

        // Tear the old agent down.  libjuice tears down its socket with it.
        juice_destroy(agent);
        agent = nullptr;

        // Reset the observable state.  The candidate list is cleared as well:
        // the addresses gathered before the restart may belong to a network
        // interface that is gone.  The host candidates of the new session are
        // gathered synchronously inside juice_gather_candidates(), so the
        // list is repopulated before this function returns.
        {
            std::lock_guard<std::mutex> lk(mu);
            local_candidates.clear();
            gathering_done = false;
            // A fresh gathering round starts with the credentials latch
            // and the TURN facts cleared (same as open()).
            g_turn_auth_rejected.store(false, std::memory_order_release);
            turn_transport_used_.reset();
            relay_candidates_seen_ = 0;
            state = plugins::IceState::Disconnected;
            selected_local_str.clear();
            selected_remote_str.clear();
            rx_queue.clear();
            // The related-address bookkeeping belongs to the old session.
            host_addr.clear();
            host_port = 0;
            srflx_addr.clear();
            srflx_port = 0;
            // The TCP fallback belongs to the old session too: a restart
            // mints new local credentials, so the old driver's checks are
            // stale.  The remote TCP candidates survive — they are re-fed
            // to the next driver by start_tcp_fallback().
            if (tcp_driver_) {
                tcp_driver_->stop();
                tcp_driver_.reset();
            }
            tcp_local_candidates_.clear();
        }
        cv_gather.notify_all();

        // A restart always mints a new credential pair (RFC 8445 §9.1.1.1):
        // the peer can only detect the restart by observing new credentials.
        pinned_creds_valid_ = false;

        // Install the remote description *before* gathering so libjuice picks
        // the correct ICE role (CONTROLLED when the peer is the offerer).
        pending_remote_sdp_ = remote_sdp;

        if (!create_agent()) return RestartResult::kFailed;

        // Re-add the remote candidates we already know about.
        for (const auto& c : remote_candidates_) {
            juice_add_remote_candidate(agent, c.c_str());
        }

        if (juice_gather_candidates(agent) != JUICE_ERR_SUCCESS) {
            return RestartResult::kFailed;
        }

        // Re-prime the consent clock: the previous session's timestamps are
        // meaningless for the new one, and a stale clock would fire
        // on_consent_lost() the moment the new session reaches Connected.
        record_binding_received();

        ++restart_count_;
        return RestartResult::kOk;
    }

    /** Monotonic microseconds since the steady_clock epoch. */
    static std::int64_t now_us() noexcept {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    /** Start the consent freshness tracker background thread. Idempotent.
     *  Safe to call before libjuice has reached Connected; the thread just
     *  waits armed and starts ticking when state allows. */
    void start_consent_tracker_if_needed() {
        if (!config.enable_consent_freshness) return;
        bool expected = false;
        if (!consent_thread_running_.compare_exchange_strong(expected, true)) {
            return;   // already running
        }
        consent_thread_should_stop_.store(false);
        consent_thread_ = std::thread([this] {
            this->consent_thread_main();
        });
    }

    /** Stop the consent freshness tracker (joins the thread). Idempotent.
     *  Called from close() and when consent tracking is disabled. */
    void stop_consent_tracker() {
        if (!consent_thread_running_.exchange(false)) return;
        consent_thread_should_stop_.store(true);
        if (consent_thread_.joinable()) {
            consent_thread_.join();
        }
    }

    /** Main loop of the consent tracker. Ticks at `consent_interval_ms`
     *  granularity, never tighter than 1s, never wider than 1s. Each tick:
     *
     *  1. If state is Connected/Completed, check
     *       (now - last_binding_received_us) >= consent_timeout_ms
     *     and if so, fire on_consent_lost() atomically (once per loss
     *     event) and arm the next expiry at the same point.
     *
     *  2. Refresh last_binding_received_us if libjuice hasn't reported
     *     a binding request in the last interval (delegates to libjuice
     *     for protocol-level consent); we only ever fire on_consent_lost
     *     when explicitly notified (or, in production, when libjuice's
     *     consent state has clearly failed) — see comment in
     *     check_expiry() below. */
    void consent_thread_main() {
        using namespace std::chrono_literals;
        // 100ms tick is plenty for ms-level timeouts without burning CPU.
        // consent_interval_ms is used to cap work, not to drive ticks.
        constexpr auto kTick = 100ms;

        std::int64_t interval_ms =
            std::clamp(config.consent_interval_ms,
                       static_cast<std::int64_t>(100),
                       static_cast<std::int64_t>(1000));
        std::int64_t timeout_ms = std::max<std::int64_t>(
            config.consent_timeout_ms, interval_ms);

        while (!consent_thread_should_stop_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(kTick);

            // Only meaningful while ICE is up. Treating Failed/Disconnected
            // as "no consent expected" means we shouldn't fire the callback
            // until the application gets the agent back into a selected
            // state and indicates peer activity again.
            //
            // Tests can override via force_consent_armed_for_testing().
            bool armed = consent_force_armed_.load(std::memory_order_acquire);
            if (!armed) {
                plugins::IceState cur_state;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    cur_state = state;
                }
                if (cur_state != plugins::IceState::Connected &&
                    cur_state != plugins::IceState::Completed) {
                    continue;
                }
            }

            const std::int64_t now = now_us();

            // ---- Expiry check + fire + rearm (one critical section) --------
            //
            // Everything that touches the timer state is serialized by
            // consent_mu_, so a notify_binding_received() that races with
            // this tick can never be lost: it either completes before the
            // check (and is seen by it) or after the rearm (and clears the
            // flag set below).
            plugins::OnConsentLost cb_copy;
            {
                std::lock_guard<std::mutex> lk(consent_mu_);
                const std::int64_t last_us = last_binding_received_us_;
                if (last_us == 0) continue;   // not yet primed
                const std::int64_t age_us = now - last_us;
                if (age_us < timeout_ms * 1000) continue;

                // Past timeout — fire (once per loss event), then rearm so a
                // subsequent loss event can be observed (matches the
                // docstring: "may fire again on a subsequent loss event").
                cb_copy = on_consent_lost_;
                // Publish the "fired" state *before* running the callback:
                // a notify_binding_received() that arrives while the callback
                // is still running must not be overwritten by a late store
                // from this thread.
                consent_lost_fired_ = true;
                // Rearm: pretend we just received a binding request so we
                // won't re-fire until another full timeout window passes
                // without a notify_binding_received().
                last_binding_received_us_ = now;
            }

            // The user callback runs *outside* the lock: it may call back
            // into the transport (e.g. notify_binding_received()), and
            // holding consent_mu_ across that call would deadlock.
            if (cb_copy) {
                try {
                    cb_copy();
                } catch (...) {
                    // Swallow user-callback exceptions; the tracker must
                    // never die on a misbehaving consumer.
                }
            }
        }
    }

    /** Reset the consent freshness clock to "now". Called by the public
     *  notify_binding_received() to simulate peer STUN Binding traffic. */
    void record_binding_received() {
        std::lock_guard<std::mutex> lk(consent_mu_);
        last_binding_received_us_ = now_us();
        // Clear the "lost" flag so a re-fired callback is observed.
        consent_lost_fired_ = false;
    }
};

/** Enter the TCP path (RFC 6544) once the UDP agent has failed.
 *
 *  ADR-028 Decision 3: the TCP fallback runs after UDP fails — here,
 *  when libjuice reports Failed (the remote check list was empty, so the
 *  connectivity timer expired and no pair was nominated).  The driver
 *  gathers fresh host TCP candidates (active + passive), publishes them
 *  through tcp_local_candidates_, and starts its check thread.  Caller
 *  holds mu_; idempotent. */
void IceTransport::Impl::start_tcp_fallback() {
    if (tcp_driver_ || state != plugins::IceState::Failed) return;

    char buf[JUICE_MAX_SDP_STRING_LEN] = {0};
    if (!agent || juice_get_local_description(agent, buf, sizeof(buf))
                      != JUICE_ERR_SUCCESS) {
        return;
    }
    const std::string local_ufrag = sdp_util::extract_sdp_attr(buf, "ice-ufrag:");
    const std::string local_pwd   = sdp_util::extract_sdp_attr(buf, "ice-pwd:");

    TcpIceDriver::Config dcfg;
    dcfg.bind_host      = config.bind_address.empty() ? "127.0.0.1" : config.bind_address;
    dcfg.local_ufrag    = local_ufrag;
    dcfg.local_password = local_pwd;

    auto driver = std::make_unique<TcpIceDriver>(std::move(dcfg));

    std::vector<TcpCandidate> gathered;
    if (!driver->gather(gathered)) {
        core::log::Logger::instance().error(
            "nimrtc::ice: TCP candidate gathering failed; staying on the "
            "UDP failure");
        return;
    }
    for (const auto& c : gathered) {
        tcp_local_candidates_.push_back(c.to_sdp());
    }

    // The peer's credentials were cached from set_remote_description();
    // the driver needs them to authenticate its connectivity checks.
    driver->set_remote_credentials(remote_ufrag_str, remote_pwd_str);
    for (const auto& c : tcp_remote_candidates_) {
        driver->add_remote(c);
    }
    driver->start();

    tcp_driver_ = std::move(driver);
    core::log::Logger::instance().info(
        "nimrtc::ice: UDP failed; TCP fallback gathered " +
        std::to_string(gathered.size()) + " candidate(s)");
}

// =============================================================================
// IceTransport — public interface
// =============================================================================

IceTransport::IceTransport(IceConfig config)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
    // The credentials the application pinned apply to the *first* session.
    // An ICE restart clears them so a fresh pair is minted (RFC 8445
    // §9.1.1.1: the peer detects a restart by observing new credentials).
    impl_->pinned_ufrag_    = impl_->config.local_ufrag;
    impl_->pinned_password_ = impl_->config.local_password;
    impl_->pinned_creds_valid_ = true;
    hook_libjuice_logging_once();
}

void IceTransport::set_config(const IceConfig& cfg) noexcept {
    if (!impl_->agent) {
        impl_->config = cfg;
        impl_->pinned_ufrag_    = cfg.local_ufrag;
        impl_->pinned_password_ = cfg.local_password;
        impl_->pinned_creds_valid_ = true;
    }
}

void IceTransport::set_bind_address(std::string_view addr) noexcept {
    impl_->config.bind_address = std::string(addr);
}

void IceTransport::set_stun_server(std::string_view host, std::uint16_t port) noexcept {
    impl_->config.stun_server_host = std::string(host);
    impl_->config.stun_server_port = port;
}

void IceTransport::add_turn_server(std::string_view host,
                                   std::uint16_t port,
                                   std::string_view username,
                                   std::string_view password) noexcept {
    ice::TurnServer ts;
    ts.host = std::string(host);
    ts.port = port;
    ts.username = std::string(username);
    ts.password = std::string(password);
    add_turn_server(ts);
}

void IceTransport::add_turn_server(const TurnServer& server) noexcept {
    impl_->config.turn_servers.push_back(server);

    // Report the transport honestly.  A transport the build cannot speak
    // would be configured and then silently spoken to over something
    // else, so say so instead.  Both TURN over TCP (RFC 8656 section 12)
    // and TURN over TLS (RFC 6062) are implemented.
    if (!turn_transport_supported(server.transport)) {
        {
            std::lock_guard<std::mutex> lk(impl_->mu);
            // Remember the unsupported transport: the allocation to this
            // server will fail, and the N4 diagnostics surface it as
            // "transport-unsupported" instead of a mysterious no-relay.
            impl_->unsupported_turn_transport_ = true;
        }
        core::log::Logger::instance().warn(
            std::string("ice: TURN transport ") +
            (server.transport == TurnTransport::Tls ? "TLS" : "TCP") +
            " is not supported by this build; the allocation to the server at " +
            server.host + ":" + std::to_string(server.port) +
            " will fail rather than fall back to another transport");
    }

    core::log::Logger::instance().info(
        std::string("ice: TURN server added host=") + server.host +
        ":" + std::to_string(server.port));

    // The MUX/TURN conflict is the reason a TURN configuration could
    // silently produce no relay candidate.  If the caller pinned MUX
    // explicitly, honour it but make the consequence loud.
    if (impl_->agent == nullptr && !impl_->config.turn_servers.empty() &&
        resolve_mode(impl_->config) != impl_->config.mode) {
        core::log::Logger::instance().info(
            "ice: TURN servers configured — switching the ICE concurrency "
            "mode from MUX to POLL (the multiplexer cannot carry a TURN "
            "allocation)");
    }
}

bool IceTransport::turn_transport_supported(TurnTransport transport) noexcept {
    switch (transport) {
        case TurnTransport::Udp:
            return true;
        case TurnTransport::Tcp:
            // RFC 8656 section 12: the TURN control connection is a TCP
            // stream.  The vendored libjuice carries a dedicated
            // `turn_tcp` module that implements the self-delimiting
            // framing of RFC 8656 section 12.4/12.5, so the control
            // connection really is a TCP stream (the relayed transport
            // address allocated by the server stays UDP).
            return true;
        case TurnTransport::Tls:
            // RFC 6062: TURN over TLS.  The TLS client lives in this
            // module (src/turn_tls.cpp) and is handed to libjuice through
            // the transport seam, so the control connection really is
            // TLS-protected.
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Plugin seam (plugins::IICETransport)
// ---------------------------------------------------------------------------

void IceTransport::add_turn_server_with_transport(std::string_view host,
                                                 std::uint16_t port,
                                                 std::string_view username,
                                                 std::string_view password,
                                                 plugins::TurnTransport transport) noexcept {
    // The plugin-layer enum mirrors the module-layer one value for value,
    // so the cast is a plain reinterpretation of the same three cases.
    TurnServer ts;
    ts.host = std::string(host);
    ts.port = port;
    ts.username = std::string(username);
    ts.password = std::string(password);
    ts.transport = static_cast<TurnTransport>(transport);
    add_turn_server(ts);

    // v1-turn-tls-trust (ADR-037): the trust policy a host expressed through
    // the additive 7-argument plugin overload lives in the interface's own
    // members — the concrete class does not override that overload, so this
    // existing seam is where the policy is picked up and copied into the
    // agent config.  `create_agent()` (called by open() *after* this) reads
    // these two fields when it builds the TURN/TLS client.
    impl_->config.turn_tls_verify_peer = turn_tls_verify_peer_;
    impl_->config.turn_tls_ca_file = turn_tls_ca_file_;
}

bool IceTransport::turn_transport_supported(plugins::TurnTransport transport) const noexcept {
    return turn_transport_supported(static_cast<TurnTransport>(transport));
}

IceConfig::Mode IceTransport::resolved_mode() const noexcept {
    return resolve_mode(impl_->config);
}

// ---------------------------------------------------------------------------
void IceTransport::set_local_port_range(std::uint16_t begin, std::uint16_t end) noexcept {
    impl_->config.local_port_range_begin = begin;
    impl_->config.local_port_range_end   = end;
}

void IceTransport::set_bwe(plugins::IBwe* bwe) noexcept {
    impl_->bwe_ = bwe;
}

void IceTransport::set_scheduler(plugins::IScheduler* sched) noexcept {
    impl_->scheduler_ = sched;
}

IceTransport::~IceTransport() {
    close();
}

IceTransport::IceTransport(IceTransport&& other) noexcept = default;
IceTransport& IceTransport::operator=(IceTransport&& other) noexcept = default;

const char* IceTransport::name() const noexcept {
    return "nimrtc::ice::IceTransport (libjuice)";
}

plugins::Status IceTransport::open() noexcept {
    if (impl_->agent) {
        return plugins::kOk;   // already open
    }
    impl_->restart_count_ = 0;
    {
        // A fresh gathering round starts from a clean slate: the
        // process-wide credentials latch and the per-transport TURN facts
        // belong to the session being opened, not the previous one.
        std::lock_guard<std::mutex> lk(impl_->mu);
        g_turn_auth_rejected.store(false, std::memory_order_release);
        impl_->gathering_done = false;
        impl_->turn_transport_used_.reset();
        impl_->relay_candidates_seen_ = 0;
    }
    if (!impl_->create_agent()) {
        return plugins::kErrInternal;
    }
    if (juice_gather_candidates(impl_->agent) != JUICE_ERR_SUCCESS) {
        return plugins::kErrInternal;
    }
    // Start the consent freshness tracker only after a successful open().
    // The thread stays alive until close(); consent_lost_fired_ resets on
    // each notify_binding_received() to support multiple loss events.
    impl_->record_binding_received();      // prime the clock at t=open
    impl_->start_consent_tracker_if_needed();
    return plugins::kOk;
}

void IceTransport::close() noexcept {
    impl_->stop_consent_tracker();
    if (impl_->agent) {
        juice_destroy(impl_->agent);
        impl_->agent = nullptr;
    }
    {
        // Clear the consent "lost" flag and reset the clock so a re-opened
        // transport starts from a clean slate.  Kept in its own scope so
        // consent_mu_ is never held together with mu_.
        std::lock_guard<std::mutex> clk(impl_->consent_mu_);
        impl_->consent_lost_fired_ = false;
        impl_->last_binding_received_us_ = 0;
    }
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->rx_queue.clear();
    impl_->state = plugins::IceState::Disconnected;

    // The diagnostics state belongs to the session being closed.  The
    // gathering phase resets to New (agent is gone above), and the TURN
    // facts start over on the next open().
    impl_->gathering_done = false;
    impl_->turn_transport_used_.reset();
    impl_->relay_candidates_seen_ = 0;

    // The TCP fallback (RFC 6544) is torn down with the UDP agent: its
    // sockets are retired and its check thread joined.
    if (impl_->tcp_driver_) {
        impl_->tcp_driver_->stop();
        impl_->tcp_driver_.reset();
    }
    impl_->tcp_local_candidates_.clear();
    impl_->tcp_remote_candidates_.clear();

    // ---- Reset the ICE restart state ----------------------------------------
    //
    // A closed transport has no session: the remote description, the remote
    // credentials and the remote candidates are gone, and the restart counter
    // starts over.  The credentials the application pinned through
    // IceConfig are restored so the *next* session starts with them again
    // (they only ever apply to the first session of a transport — a restart
    // always mints fresh ones, see rebuild_agent()).
    impl_->applied_remote_sdp_.clear();
    impl_->remote_candidates_.clear();
    impl_->remote_ufrag_str.clear();
    impl_->remote_pwd_str.clear();
    impl_->pending_remote_sdp_.clear();
    impl_->restart_count_ = 0;
    impl_->pinned_ufrag_    = impl_->config.local_ufrag;
    impl_->pinned_password_ = impl_->config.local_password;
    impl_->pinned_creds_valid_ = true;
}

void IceTransport::set_callbacks(plugins::RecvCallback on_recv,
                                 plugins::ErrorCallback on_error) noexcept {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->recv_cb_ = std::move(on_recv);
    impl_->on_error = std::move(on_error);
}

void IceTransport::set_on_consent_lost(plugins::OnConsentLost cb) noexcept {
    std::lock_guard<std::mutex> lk(impl_->consent_mu_);
    impl_->on_consent_lost_ = std::move(cb);
}

void IceTransport::notify_binding_received() noexcept {
    impl_->record_binding_received();
}

bool IceTransport::consent_lost_pending() const noexcept {
    std::lock_guard<std::mutex> lk(impl_->consent_mu_);
    return impl_->consent_lost_fired_;
}

void IceTransport::force_consent_armed_for_testing() noexcept {
    impl_->consent_force_armed_.store(true, std::memory_order_release);
    // Reset the clock to "now" so the timer has a clean baseline.
    impl_->record_binding_received();
}

plugins::Status IceTransport::send(plugins::BufferView view,
                                    plugins::Addr /*dst_addr*/) noexcept {
    if (!impl_->agent) return plugins::kErrNotReady;
    if (view.empty())   return plugins::kOk;

    const int rc = juice_send(impl_->agent,
                              reinterpret_cast<const char*>(view.data()),
                              view.size());
    return rc == JUICE_ERR_SUCCESS ? plugins::kOk : plugins::kErrInternal;
}

int IceTransport::recv() noexcept {
    plugins::RecvCallback cb;
    std::deque<Packet>    drained;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (impl_->rx_queue.empty()) return 0;
        cb = impl_->recv_cb_;
        drained.swap(impl_->rx_queue);
    }

    int dispatched = 0;
    if (cb) {
        for (auto& pkt : drained) {
            plugins::BufferView bv{pkt.data.data(), pkt.data.size()};
            cb(bv);
            ++dispatched;
        }
    }
    return dispatched;
}

std::optional<std::vector<std::uint8_t>> IceTransport::last_recv_packet() noexcept {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->rx_queue.empty()) return std::nullopt;
    auto pkt = std::move(impl_->rx_queue.front());
    impl_->rx_queue.pop_front();
    return pkt.data;
}

plugins::Addr IceTransport::local_addr() const noexcept {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->encode_addr(impl_->selected_local_str);
}

plugins::Addr IceTransport::remote_addr() const noexcept {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->encode_addr(impl_->selected_remote_str);
}

plugins::IceState IceTransport::state() const noexcept {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->state;
}

Role IceTransport::role() const noexcept {
    return impl_->config.role;
}

std::string IceTransport::local_description() const {
    if (!impl_->agent) return {};
    char buf[JUICE_MAX_SDP_STRING_LEN] = {0};
    if (juice_get_local_description(impl_->agent, buf, sizeof(buf))
            != JUICE_ERR_SUCCESS) {
        return {};
    }
    std::string out{buf};
    // libjuice terminates every line with \r\n; append the TCP candidates
    // gathered after UDP failed the same way so the SDP stays uniform.
    std::lock_guard<std::mutex> lk(impl_->mu);
    for (const auto& line : impl_->tcp_local_candidates_) {
        out += "a=";
        out += line;
        out += "\r\n";
    }
    return out;
}

plugins::Status IceTransport::set_remote_description(std::string_view sdp) noexcept {
    // juice_set_remote_description expects a NUL-terminated C string.
    std::string tmp{sdp};

    // Pull the ICE credentials out of the description.  They are needed to
    // detect an ICE restart (RFC 8445 §9.1.1.1): the peer signals a restart
    // by publishing a different ice-ufrag / ice-pwd pair.
    const std::string ufrag = sdp_util::extract_sdp_attr(tmp, "ice-ufrag:");
    const std::string pwd   = sdp_util::extract_sdp_attr(tmp, "ice-pwd:");

    if (!impl_->agent) {
        // Agent not yet created — store for apply in create_agent() before
        // agent_gather_candidates().  This is the preferred path for loopback
        // tests: the answerer knows the offerer's ICE credentials before its
        // ICE transport opens, so libjuice sets the agent to CONTROLLED.
        impl_->pending_remote_sdp_ = tmp;
        impl_->remote_ufrag_str = ufrag;
        impl_->remote_pwd_str = pwd;
        return plugins::kOk;
    }

    // ---- ICE restart detection (RFC 8445 §9.1.1.1) ------------------------
    //
    // The peer published different ICE credentials, i.e. it restarted ICE.
    // libjuice refuses to replace a remote description whose ufrag differs
    // from the current one ("ICE restart is not supported"), and RFC 8445
    // requires *both* sides to change credentials for the restart to be
    // complete.  We therefore rebuild the agent: fresh local credentials,
    // fresh gathering, and the peer's new description installed on it.
    //
    // The distinction with the local-initiated restart matters: there we
    // changed our own credentials and the peer is expected to answer with
    // new ones; here the peer already changed theirs.
    const bool peer_restarted =
        !impl_->remote_ufrag_str.empty() && !ufrag.empty() &&
        (impl_->remote_ufrag_str != ufrag || impl_->remote_pwd_str != pwd);

    if (peer_restarted) {
        core::log::Logger::instance().info(
            "nimrtc::ice: peer restarted ICE (remote ufrag " +
            impl_->remote_ufrag_str + " -> " + ufrag + "), rebuilding agent");

        const auto rc = impl_->rebuild_agent(tmp, /*keep_remote_sdp=*/false);
        if (rc != Impl::RestartResult::kOk) return plugins::kErrInternal;

        impl_->applied_remote_sdp_ = tmp;
        impl_->remote_ufrag_str = ufrag;
        impl_->remote_pwd_str = pwd;

        // The peer has to learn our new credentials, so the host must send a
        // fresh answer.  Run the callback outside of any lock.
        if (impl_->on_ice_restart_) {
            try {
                impl_->on_ice_restart_();
            } catch (...) {
                // A misbehaving consumer must not break the transport.
            }
        }
        return plugins::kOk;
    }

    // Agent already exists — apply immediately via juice_set_remote_description().
    //
    // libjuice has built-in ICE role-conflict resolution: if both agents
    // started as CONTROLLING (because no remote SDP was set before gathering),
    // each side's STUN Binding Request carries the peer's ice_controlling
    // attribute.  On receipt, libjuice compares the two ice_tiebreaker values
    // (RFC 8445 §6.1.2) and the agent with the lower tiebreaker switches to
    // CONTROLLED.  No agent destruction is needed — only juice_set_remote_description
    // must be called so libjuice knows the remote ufrag/pwd.
    //
    // Previously this function destroyed and recreated the agent whenever it was
    // already gathered.  That caused a new ufrag/pwd to be generated on the
    // local side, invalidating every in-flight STUN Binding Request in flight
    // (the peer still used the old ufrag).  The result was "STUN remote ufrag
    // check failed" and ICE connectivity checks cycling forever, never stabilising.
    // Deleting the destroy/recreate branch fixes that regression.
    const int rc = juice_set_remote_description(impl_->agent, tmp.c_str());
    if (rc != JUICE_ERR_SUCCESS) {
        // Treat any libjuice parse error as a corrupt SDP from our point of
        // view — the wrapper contract is "give me an SDP, I'll apply it";
        // libjuice's specific error category is opaque to callers.
        return plugins::kErrCorrupt;
    }

    // Cache the parsed credentials so callers can read them back via
    // remote_ufrag() / remote_password() without going through libjuice.
    if (!ufrag.empty()) impl_->remote_ufrag_str = ufrag;
    if (!pwd.empty())   impl_->remote_pwd_str = pwd;
    impl_->applied_remote_sdp_ = tmp;

    // The TCP driver (RFC 6544), if the UDP agent already failed, needs
    // the peer credentials to authenticate its connectivity checks.
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (impl_->tcp_driver_) {
            impl_->tcp_driver_->set_remote_credentials(ufrag, pwd);
        }
    }
    return plugins::kOk;
}

plugins::Status IceTransport::add_remote_candidate(std::string_view sdp) noexcept {
    if (!impl_->agent) return plugins::kErrNotReady;
    std::string tmp{sdp};

    // A TCP candidate (RFC 6544) belongs to the NimRTC TCP driver, not to
    // libjuice: libjuice's ICE layer is UDP-only and drops it on arrival
    // (ADR-028, Option A).  Trickled TCP candidates (RFC 8838) can arrive
    // before the UDP path fails — remember them and feed the driver when
    // it is created.
    const auto parsed = parse_candidate(tmp);
    if (parsed && !parsed->tcptype.empty()) {
        TcpCandidate c;
        c.foundation   = parsed->foundation;
        c.component_id = parsed->component_id;
        c.host         = parsed->host;
        c.port         = parsed->port;
        // "so" (simultaneous-open) is not gathered by this slice; treat it
        // as active so the driver still attempts the connection.
        c.tcptype      = (parsed->tcptype == "passive")
                             ? TcpCandidateRole::Passive
                             : TcpCandidateRole::Active;
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->tcp_remote_candidates_.push_back(c);
        if (impl_->tcp_driver_) {
            impl_->tcp_driver_->add_remote(c);
        }
        return plugins::kOk;
    }

    const int rc = juice_add_remote_candidate(impl_->agent, tmp.c_str());
    if (rc != JUICE_ERR_SUCCESS) return plugins::kErrInvalidParam;
    // Remember it so an ICE restart can re-apply it to the new agent.
    impl_->remote_candidates_.push_back(tmp);
    return plugins::kOk;
}

plugins::Status IceTransport::restart() noexcept {
    if (!impl_->agent) return plugins::kErrNotReady;

    core::log::Logger::instance().info(
        "nimrtc::ice: ICE restart requested, rebuilding agent");

    // The peer has not sent new credentials yet, so the new agent starts
    // without a remote description and waits for the peer's answer.
    const auto rc = impl_->rebuild_agent({}, /*keep_remote_sdp=*/false);
    if (rc != Impl::RestartResult::kOk) return plugins::kErrInternal;

    // The credentials changed, so the remote description we had is stale.
    impl_->applied_remote_sdp_.clear();
    impl_->remote_ufrag_str.clear();
    impl_->remote_pwd_str.clear();

    core::log::Logger::instance().info(
        "nimrtc::ice: ICE restart done, new local ufrag=" +
        local_ufrag());
    return plugins::kOk;
}

std::uint32_t IceTransport::restart_count() const noexcept {
    return impl_->restart_count_;
}

void IceTransport::set_on_ice_restart(plugins::OnIceRestart cb) noexcept {
    impl_->on_ice_restart_ = std::move(cb);
}

std::string IceTransport::local_ufrag() const noexcept {
    if (!impl_->agent) return impl_->config.local_ufrag;
    char buf[JUICE_MAX_SDP_STRING_LEN] = {0};
    if (juice_get_local_description(impl_->agent, buf, sizeof(buf))
            != JUICE_ERR_SUCCESS) {
        return impl_->config.local_ufrag;
    }
    std::string sdp{buf};
    auto pos = sdp.find("a=ice-ufrag:");
    if (pos == std::string::npos) return impl_->config.local_ufrag;
    pos += std::strlen("a=ice-ufrag:");
    // libjuice uses \r\n line endings — strip the trailing \r.
    auto end = sdp.find('\n', pos);
    auto end_pos = end == std::string::npos ? std::string::npos : end - pos;
    std::string ufrag = sdp.substr(pos, end_pos);
    while (!ufrag.empty() && (ufrag.back() == '\r' || ufrag.back() == '\n')) {
        ufrag.pop_back();
    }
    return ufrag;
}

std::string IceTransport::local_password() const noexcept {
    if (!impl_->agent) return impl_->config.local_password;
    char buf[JUICE_MAX_SDP_STRING_LEN] = {0};
    if (juice_get_local_description(impl_->agent, buf, sizeof(buf))
            != JUICE_ERR_SUCCESS) {
        return impl_->config.local_password;
    }
    std::string sdp{buf};
    auto pos = sdp.find("a=ice-pwd:");
    if (pos == std::string::npos) return impl_->config.local_password;
    pos += std::strlen("a=ice-pwd:");
    auto end = sdp.find('\n', pos);
    auto end_pos = end == std::string::npos ? std::string::npos : end - pos;
    std::string pwd = sdp.substr(pos, end_pos);
    while (!pwd.empty() && (pwd.back() == '\r' || pwd.back() == '\n')) {
        pwd.pop_back();
    }
    return pwd;
}

std::string IceTransport::remote_ufrag() const noexcept {
    return impl_->remote_ufrag_str;
}

std::string IceTransport::remote_password() const noexcept {
    return impl_->remote_pwd_str;
}

std::optional<Candidate> IceTransport::selected_local() const noexcept {
    // The TCP driver nominates independently of libjuice (ADR-028 Decision
    // 6); report its winner first so the two sources never disagree.
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (impl_->tcp_driver_) {
            if (const auto nom = impl_->tcp_driver_->nominated()) {
                return tcp_to_candidate(nom->first);
            }
        }
    }
    if (!impl_->agent) return std::nullopt;
    char buf[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {0};
    if (juice_get_selected_candidates(impl_->agent, buf, sizeof(buf),
                                      nullptr, 0) != JUICE_ERR_SUCCESS) {
        return std::nullopt;
    }
    return parse_candidate(buf);
}

std::optional<Candidate> IceTransport::selected_remote() const noexcept {
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (impl_->tcp_driver_) {
            if (const auto nom = impl_->tcp_driver_->nominated()) {
                return tcp_to_candidate(nom->second);
            }
        }
    }
    if (!impl_->agent) return std::nullopt;
    char buf[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {0};
    if (juice_get_selected_candidates(impl_->agent, nullptr, 0,
                                      buf, sizeof(buf)) != JUICE_ERR_SUCCESS) {
        return std::nullopt;
    }
    return parse_candidate(buf);
}

// ---------------------------------------------------------------------------
// selected_pair — the seam's view of the nominated pair
// ---------------------------------------------------------------------------
//
// The projection is deliberately *lossy*: the seam carries the address,
// the port, the protocol and the candidate type, and drops the priority,
// the foundation and the related address.  Those three are what a
// connectivity diagnosis needs, and the full candidate is still
// available through the concrete `selected_local()` / `selected_remote()`
// accessors.
//
// The pair is read in ONE `juice_get_selected_candidates()` call rather
// than two, because the two calls are not atomic: an ICE restart between
// them would hand back the local end of the *old* pair and the remote
// end of the *new* one.  libjuice fills both buffers from one snapshot
// of the agent's selected entry, so a single call cannot straddle it.
// ---------------------------------------------------------------------------
namespace {

/** Project a concrete candidate onto the seam's shape. */
plugins::IceCandidate to_seam_candidate(const Candidate& c) {
    plugins::IceCandidate out;
    out.address  = c.host;
    out.port     = c.port;

    // The candidate's transport is "UDP" / "TCP" in SDP; the seam
    // publishes it lower-cased so a host can compare it against a
    // lower-cased string without a case-insensitive compare.
    std::string proto = c.transport;
    for (auto& ch : proto) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    out.protocol = proto.empty() ? std::string("udp") : proto;

    switch (c.type) {
        case CandidateType::Host:  out.type = "host";  break;
        case CandidateType::Srflx: out.type = "srflx"; break;
        case CandidateType::Prflx: out.type = "prflx"; break;
        case CandidateType::Relay: out.type = "relay"; break;
    }

    // The tcptype half is already lowercase per RFC 6544 §4.1; pass it
    // through so a host can tell a TCP pair ("active"/"passive") from a
    // UDP one (empty).
    out.tcptype = c.tcptype;
    return out;
}

}  // namespace

std::optional<plugins::IceCandidatePair>
IceTransport::selected_pair() const noexcept {
    // The TCP driver (RFC 6544) nominates its winner independently of
    // libjuice (ADR-028 Decision 6).  Report it first so selected_pair()
    // never flaps between "udp" and "tcp" as the two sources settle.
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (impl_->tcp_driver_) {
            if (const auto nom = impl_->tcp_driver_->nominated()) {
                plugins::IceCandidatePair pair;
                pair.local  = to_seam_candidate(tcp_to_candidate(nom->first));
                pair.remote = to_seam_candidate(tcp_to_candidate(nom->second));
                return pair;
            }
        }
    }
    if (!impl_->agent) return std::nullopt;

    char local_buf[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {0};
    char remote_buf[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {0};
    if (juice_get_selected_candidates(impl_->agent,
                                      local_buf, sizeof(local_buf),
                                      remote_buf, sizeof(remote_buf))
        != JUICE_ERR_SUCCESS) {
        return std::nullopt;
    }

    // libjuice reports the *remote* end of the pair as a candidate that
    // was learned from the peer, so it parses with the same grammar as
    // the local one.  Either end failing to parse means the pair is not
    // describable, and a half-described pair is worse than none: a host
    // would report a selected path whose far end it cannot name.
    const auto local = parse_candidate(local_buf);
    if (!local) return std::nullopt;
    const auto remote = parse_candidate(remote_buf);
    if (!remote) return std::nullopt;

    plugins::IceCandidatePair pair;
    pair.local  = to_seam_candidate(*local);
    pair.remote = to_seam_candidate(*remote);
    return pair;
}

plugins::Status IceTransport::gather_candidates() noexcept {
    if (!impl_->agent) return plugins::kErrNotReady;
    return juice_gather_candidates(impl_->agent) == JUICE_ERR_SUCCESS
               ? plugins::kOk
               : plugins::kErrInternal;
}

std::vector<std::string>
IceTransport::gathered_local_candidates() const noexcept {
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto out = impl_->local_candidates;
    // The TCP candidates gathered after UDP failed (RFC 6544) are part of
    // the local candidate set; the host can trickle them to the peer.
    out.insert(out.end(), impl_->tcp_local_candidates_.begin(),
               impl_->tcp_local_candidates_.end());
    return out;
}

std::string_view IceTransport::gathering_state() const noexcept {
    std::lock_guard<std::mutex> lk(impl_->mu);
    // The collection's own state, kept separate from the agent's state
    // machine: an agent can be `connected` while a reflexive candidate is
    // still arriving (RFC 8445 §5.1.1.5).
    if (impl_->gathering_done) return "complete";
    if (impl_->state == plugins::IceState::Gathering) return "gathering";
    // The agent is gathering before it says so in its state machine: the
    // candidates start arriving as soon as the agent is created.
    if (!impl_->local_candidates.empty()) return "gathering";
    return "new";
}

std::string_view IceTransport::turn_failure_reason() const noexcept {
    if (!impl_->agent) return {};
    switch (juice_get_turn_failure(impl_->agent)) {
        case JUICE_TURN_FAILURE_UNAUTHORIZED: return "credentials-rejected";
        case JUICE_TURN_FAILURE_STALE_NONCE:  return "stale-nonce";
        case JUICE_TURN_FAILURE_TRANSPORT:    return "transport-unavailable";
        case JUICE_TURN_FAILURE_ALLOCATE:     return "allocate-refused";
        case JUICE_TURN_FAILURE_NONE:         break;
    }
    return {};
}

bool IceTransport::wait_for_gathering(int timeout_ms) noexcept {
    if (!impl_->agent) return false;
    std::unique_lock<std::mutex> lk(impl_->mu);
    // Treat Connected/Completed state as gathering-is-sufficient too.
    auto already = [&] {
        return impl_->gathering_done ||
               impl_->state == plugins::IceState::Connected ||
               impl_->state == plugins::IceState::Completed;
    };
    if (already()) return true;

    if (timeout_ms <= 0) return already();

    return impl_->cv_gather.wait_for(lk, std::chrono::milliseconds(timeout_ms), already);
}

// =============================================================================
// IceTransportFactory
// =============================================================================

IceTransportFactory::IceTransportFactory() = default;
IceTransportFactory::IceTransportFactory(IceConfig default_config)
    : default_config_(std::move(default_config)) {}

std::string_view IceTransportFactory::id() const noexcept {
    return "ice";
}

std::string_view IceTransportFactory::display_name() const noexcept {
    return "ICE (libjuice, RFC 8445)";
}

plugins::IICETransport* IceTransportFactory::create_ice() const {
    return new IceTransport(default_config_);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Public registration entry point — replaces the anonymous-namespace static
// registrar that MSVC strips from static libraries (same workaround as
// nimrtc::audio3a::register_default_plugins()).
// ---------------------------------------------------------------------------

namespace detail {

void do_register_default_plugins() noexcept {
    // static ⇒ address stable for process lifetime; runs once at first call.
    static const struct Registrar {
        Registrar() {
            static nimrtc::ice::IceTransportFactory s_factory{};
            // The same factory instance is published under both the generic
            // transport registry and the ICE-specific one. `register_transport`
            // takes `const ITransportFactory*`; `register_ice_transport`
            // takes `const IICETransportFactory*` — `IceTransportFactory`
            // publicly inherits from `IICETransportFactory` (which virtually
            // inherits from `ITransportFactory`), so a single `&s_factory`
            // pointer satisfies both. Consumers can then resolve the
            // strongest typed view through either `get_transport("ice")`
            // or `get_ice_transport("ice")`.
            const auto* tfactory = static_cast<const plugins::ITransportFactory*>(&s_factory);
            const auto* ifactory = static_cast<const plugins::IICETransportFactory*>(&s_factory);
            nimrtc::core::PluginRegistry::instance().register_transport(
                std::string_view{s_factory.id()}, tfactory);
            nimrtc::core::PluginRegistry::instance().register_ice_transport(
                std::string_view{s_factory.id()}, ifactory);
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

// Non-inline (declared in ice.hpp) so the symbol is guaranteed
// in nimrtc_ice.lib for consumers that link via static lib + PluginRegistry.
void register_default_plugins() noexcept {
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::ice
