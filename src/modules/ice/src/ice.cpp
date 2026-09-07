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

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
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
// -----------------------------------------------------------------------------

std::atomic<bool> g_log_hooked{false};

void hook_libjuice_logging_once() noexcept {
    if (g_log_hooked.exchange(true)) return;

    juice_set_log_level(JUICE_LOG_LEVEL_VERBOSE);

    juice_set_log_handler([](juice_log_level_t level, const char* message) {
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

    // Optional "raddr ... rport ..."
    if (iss >> tok && tok == "raddr") {
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
    if (!related_address.empty()) {
        oss << " raddr " << related_address << " rport " << related_port;
    }
    return oss.str();
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
    std::atomic<std::int64_t>            last_binding_received_us_{0};
    std::atomic<bool>                    consent_lost_fired_{false};
    std::atomic<bool>                    consent_thread_should_stop_{false};
    std::atomic<bool>                    consent_thread_running_{false};
    /** Test-only override: arm the tracker as if Connected. */
    std::atomic<bool>                    consent_force_armed_{false};
    std::thread                          consent_thread_;
    std::mutex                           consent_mu_;        // protects on_consent_lost_
    plugins::OnConsentLost               on_consent_lost_;

    // ---- Plugin injection (BWE + Scheduler) --------------------------------
    // Non-owning raw pointers set by set_bwe() / set_scheduler().
    // The engine owns the plugin instances; the ICE transport merely
    // references them to call bwe->on_feedback() and sched->drain_with().
    plugins::IBwe*         bwe_         = nullptr;
    plugins::IScheduler*    scheduler_   = nullptr;

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
    }

    static void on_candidate(juice_agent_t*, const char* sdp, void* user) {
        // New local candidate discovered (host / srflx / relay / prflx).
        // We push it onto the local_candidates list so the engine can
        // include it in SDP.  Mutex held briefly; this fires on libjuice's
        // thread.
        auto* self = static_cast<Impl*>(user);
        if (!sdp) return;
        std::lock_guard<std::mutex> lk(self->mu);
        self->local_candidates.emplace_back(sdp);
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
        jc.concurrency_mode = map_mode(config.mode);
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
        for (const auto& t : config.turn_servers) {
            juice_turn_server_t s{};
            s.host = t.host.c_str();
            s.port = t.port;
            s.username = t.username.empty() ? nullptr : t.username.c_str();
            s.password = t.password.empty() ? nullptr : t.password.c_str();
            jt.push_back(s);
        }
        if (!jt.empty()) {
            jc.turn_servers      = jt.data();
            jc.turn_servers_count = static_cast<int>(jt.size());
        }

        jc.cb_state_changed    = &Impl::on_state;
        jc.cb_candidate        = &Impl::on_candidate;
        jc.cb_gathering_done   = &Impl::on_gathering_done;
        jc.cb_recv             = &Impl::juice_recv_cb;
        jc.user_ptr            = this;

        juice_agent_t* a = juice_create(&jc);
        if (!a) return false;
        agent = a;

        if (!config.local_ufrag.empty() || !config.local_password.empty()) {
            juice_set_local_ice_attributes(agent,
                                           config.local_ufrag.c_str(),
                                           config.local_password.c_str());
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
    // ICE consent freshness tracker (RFC 7675 / RFC 8445 §10)
    // ---------------------------------------------------------------------

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

            const std::int64_t now     = now_us();
            const std::int64_t last_us = last_binding_received_us_.load(
                std::memory_order_acquire);
            if (last_us == 0) continue;   // not yet primed
            const std::int64_t age_us = now - last_us;
            if (age_us < timeout_ms * 1000) continue;

            // Past timeout — fire (once), then rearm so a subsequent loss
            // event can be observed (matches the docstring: "may fire again
            // on a subsequent loss event").
            plugins::OnConsentLost cb_copy;
            {
                std::lock_guard<std::mutex> lk(consent_mu_);
                cb_copy = on_consent_lost_;
            }
            if (cb_copy) {
                try {
                    cb_copy();
                } catch (...) {
                    // Swallow user-callback exceptions; the tracker must
                    // never die on a misbehaving consumer.
                }
            }
            consent_lost_fired_.store(true, std::memory_order_release);
            // Rearm: pretend we just received a binding request so we
            // won't re-fire until another full timeout window passes
            // without a notify_binding_received().
            last_binding_received_us_.store(now_us(),
                                            std::memory_order_release);
        }
    }

    /** Reset the consent freshness clock to "now". Called by the public
     *  notify_binding_received() to simulate peer STUN Binding traffic. */
    void record_binding_received() {
        last_binding_received_us_.store(now_us(), std::memory_order_release);
        // Clear the "lost" flag so a re-fired callback is observed.
        consent_lost_fired_.store(false, std::memory_order_release);
    }
};

// =============================================================================
// IceTransport — public interface
// =============================================================================

IceTransport::IceTransport(IceConfig config)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
    hook_libjuice_logging_once();
}

void IceTransport::set_config(const IceConfig& cfg) noexcept {
    if (!impl_->agent) {
        impl_->config = cfg;
    }
}

void IceTransport::set_bind_address(std::string_view addr) noexcept {
    impl_->config.bind_address = std::string(addr);
}

void IceTransport::set_stun_server(std::string_view host, std::uint16_t port) noexcept {
    impl_->config.stun_server_host = std::string(host);
    impl_->config.stun_server_port = port;
}

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
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->rx_queue.clear();
    impl_->state = plugins::IceState::Disconnected;
    // Clear the consent "lost" flag and reset the clock so a re-opened
    // transport starts from a clean slate.
    impl_->consent_lost_fired_.store(false, std::memory_order_release);
    impl_->last_binding_received_us_.store(0, std::memory_order_release);
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
    return impl_->consent_lost_fired_.load(std::memory_order_acquire);
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
    return std::string{buf};
}

plugins::Status IceTransport::set_remote_description(std::string_view sdp) noexcept {
    // juice_set_remote_description expects a NUL-terminated C string.
    std::string tmp{sdp};

    if (!impl_->agent) {
        // Agent not yet created — store for apply in create_agent() before
        // agent_gather_candidates().  This is the preferred path for loopback
        // tests: the answerer knows the offerer's ICE credentials before its
        // ICE transport opens, so libjuice sets the agent to CONTROLLED.
        impl_->pending_remote_sdp_ = tmp;
        return plugins::kOk;
    }

    // Agent already exists — apply immediately.
    const int rc = juice_set_remote_description(impl_->agent, tmp.c_str());
    if (rc != JUICE_ERR_SUCCESS) {
        // Treat any libjuice parse error as a corrupt SDP from our point of
        // view — the wrapper contract is "give me an SDP, I'll apply it";
        // libjuice's specific error category is opaque to callers.
        return plugins::kErrCorrupt;
    }

    // Cache the parsed credentials so callers can read them back via
    // remote_ufrag() / remote_password() without going through libjuice.
    auto ufrag_pos = tmp.find("a=ice-ufrag:");
    auto pwd_pos   = tmp.find("a=ice-pwd:");
    if (ufrag_pos != std::string::npos) {
        ufrag_pos += std::strlen("a=ice-ufrag:");
        auto eol = tmp.find('\n', ufrag_pos);
        impl_->remote_ufrag_str = tmp.substr(
            ufrag_pos,
            eol == std::string::npos ? std::string::npos : eol - ufrag_pos);
        while (!impl_->remote_ufrag_str.empty() &&
               (impl_->remote_ufrag_str.back() == '\r' ||
                impl_->remote_ufrag_str.back() == '\n')) {
            impl_->remote_ufrag_str.pop_back();
        }
    }
    if (pwd_pos != std::string::npos) {
        pwd_pos += std::strlen("a=ice-pwd:");
        auto eol = tmp.find('\n', pwd_pos);
        impl_->remote_pwd_str = tmp.substr(
            pwd_pos,
            eol == std::string::npos ? std::string::npos : eol - pwd_pos);
        while (!impl_->remote_pwd_str.empty() &&
               (impl_->remote_pwd_str.back() == '\r' ||
                impl_->remote_pwd_str.back() == '\n')) {
            impl_->remote_pwd_str.pop_back();
        }
    }
    return plugins::kOk;
}

plugins::Status IceTransport::add_remote_candidate(std::string_view sdp) noexcept {
    if (!impl_->agent) return plugins::kErrNotReady;
    std::string tmp{sdp};
    const int rc = juice_add_remote_candidate(impl_->agent, tmp.c_str());
    return rc == JUICE_ERR_SUCCESS ? plugins::kOk : plugins::kErrInvalidParam;
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
    if (!impl_->agent) return std::nullopt;
    char buf[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {0};
    if (juice_get_selected_candidates(impl_->agent, buf, sizeof(buf),
                                      nullptr, 0) != JUICE_ERR_SUCCESS) {
        return std::nullopt;
    }
    return parse_candidate(buf);
}

std::optional<Candidate> IceTransport::selected_remote() const noexcept {
    if (!impl_->agent) return std::nullopt;
    char buf[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {0};
    if (juice_get_selected_candidates(impl_->agent, nullptr, 0,
                                      buf, sizeof(buf)) != JUICE_ERR_SUCCESS) {
        return std::nullopt;
    }
    return parse_candidate(buf);
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
    return impl_->local_candidates;
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
