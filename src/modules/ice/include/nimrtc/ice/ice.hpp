#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/time.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/transport.hpp>
#include <nimrtc/plugins/ice_transport.hpp>

// =============================================================================
// nimrtc::ice
// -----------------------------------------------------------------------------//
// ICE (Interactive Connectivity Establishment, RFC 8445) transport backed by
// libjuice (vendored under third_party/libjuice/, MPL-2.0).
//
// Architectural role (per ADR-001-plugin-system):
//   IceTransport is the production ITransport implementation that handles
//   all network I/O. It performs STUN/TURN candidate gathering, connectivity
//   checks, and selected-pair tracking internally via libjuice, then exposes
//   a simple send/recv interface to the engine.
//
// ICE-specific operations that are not part of ITransport (SDP exchange,
// candidate inspection, credential access) are provided as additional methods
// on IceTransport and on the IceTransportFactory.
//
// Thread model:
//   - libjuice is configured in MUX concurrency mode by default, so all
//     packet reception and STUN/TURN I/O happens on libjuice's internal
//     thread.
//   - RecvCallback / ErrorCallback are invoked from that thread; the engine
//     must be thread-safe with respect to those callbacks (the ITransport
//     contract already requires this for send/recv).
//   - All other IceTransport methods are expected to be called from the
//     engine thread.
// =============================================================================
namespace nimrtc::ice {

// -----------------------------------------------------------------------------
// ICE state, role, candidate type
// -----------------------------------------------------------------------------

// `IceState` IS `plugins::IceState` — same type, same underlying storage.
//
// Why an alias rather than a fresh enum? Because `ice::IceTransport` must
// override the pure-virtual `plugins::IICETransport::state()` and that
// override has to return the *exact* same C++ type as the base declaration
// (C++ does not allow return-type-only overloading). Keeping the two names
// as aliases means the concrete `IceTransport::state()` overrides the
// interface method without an explicit conversion at every call site, and
// every consumer that imports `using nimrtc::ice::IceState` keeps working
// unchanged.
//
// The numeric values mirror juice_state_t 1:1; any new state added here
// MUST also be added to `plugins::IceState`, and vice versa.
using IceState = plugins::IceState;

enum class Role : std::uint8_t {
    Controlling,
    Controlled,
};

enum class CandidateType : std::uint8_t {
    Host,
    Srflx,
    Prflx,
    Relay,
};

// -----------------------------------------------------------------------------
// Candidate — parsed SDP-form candidate info.
// -----------------------------------------------------------------------------

struct Candidate {
    CandidateType type   = CandidateType::Host;
    std::string   foundation;
    std::uint8_t  component_id = 0;
    std::string   transport;        // "UDP" / "TCP"
    std::uint32_t priority = 0;
    std::string   host;             // IP literal
    std::uint16_t port = 0;
    std::string   related_address;
    std::uint16_t related_port = 0;

    /** SDP attribute line (without leading "a="), e.g.
     *  "candidate:1 1 UDP 2113929471 192.0.2.1 5000 typ host". */
    std::string to_sdp() const;
};

/** Parse a single candidate SDP line. Accepts both "candidate:..." and the
 *  full "a=candidate:..." form. */
std::optional<Candidate> parse_candidate(std::string_view sdp);

// -----------------------------------------------------------------------------
// TURN server config
// -----------------------------------------------------------------------------

struct TurnServer {
    std::string   host;
    std::uint16_t port = 0;
    std::string   username;
    std::string   password;
};

// -----------------------------------------------------------------------------
// IceConfig — what the application passes to IceTransport.
// -----------------------------------------------------------------------------

struct IceConfig {
    /** Controlling = initiates nomination / sends STUN binding requests
     *  with USE-CANDIDATE. Determined by SDP offer/answer in a real
     *  WebRTC flow; the application sets this before gathering. */
    Role role = Role::Controlling;

    /** ICE-Lite: skip connectivity checks, accept incoming checks only.
     *  Maps to libjuice's `ice_controlling`/`ice_controlled` toggle. */
    bool lite_mode = false;

    /** Network interface to bind. "" = any IPv4 ("0.0.0.0"). */
    std::string bind_address;

    /** Local UDP port range libjuice will try to bind. 0 = OS picks. */
    std::uint16_t local_port_range_begin = 0;
    std::uint16_t local_port_range_end   = 0;

    /** Optional STUN server for srflx candidate gathering.
     *  Leave host empty to skip STUN gathering. */
    std::string   stun_server_host;
    std::uint16_t stun_server_port = 3478;

    /** Optional TURN servers for relay candidates. */
    std::vector<TurnServer> turn_servers;

    /** If non-empty, override auto-generated local ICE credentials. */
    std::string local_ufrag;
    std::string local_password;

    /** Concurrency model. MUX is the recommended default: one libjuice
     *  thread multiplexes all connections on a single UDP socket. */
    enum class Mode : std::uint8_t { Poll, Mux, Thread };
    Mode mode = Mode::Mux;

    // -------------------------------------------------------------------------
    // ICE consent freshness (RFC 7675 / RFC 8445 §10)
    // -------------------------------------------------------------------------

    /** Period (ms) between STUN Binding request transmissions used to keep
     *  ICE consent fresh on the selected pair. Per RFC 7675 the actual
     *  transmission is performed by libjuice (randomized ~5s base); this
     *  field sizes NimRTC's app-level consent tracker that decides when
     *  on_consent_lost() fires, and it can be tuned independently.
     *  Default: 5000. */
    std::int64_t consent_interval_ms = 5000;

    /** Timeout (ms) without receiving any peer STUN Binding request before
     *  consent is considered lost. Default: 30000 (RFC 7675 §3). */
    std::int64_t consent_timeout_ms = 30000;

    /** Disable NimRTC's app-level consent tracker entirely.
     *  When false, set_on_consent_lost() will never fire on this transport.
     *  Default: true (full ICE implementation). ICE-Lite peers should keep
     *  this true — they still track consent per RFC 7675 §5. */
    bool enable_consent_freshness = true;
};

// -----------------------------------------------------------------------------
// IceTransport — concrete IICETransport.
//
// Inherits from `plugins::IICETransport` (which virtually inherits
// `plugins::ITransport`) so that the engine and other consumers can talk to
// it through the plugin seam without reaching past the interface with
// `dynamic_cast<ice::IceTransport*>`.
// -----------------------------------------------------------------------------

class IceTransport : public plugins::IICETransport {
public:
    explicit IceTransport(IceConfig config);
    ~IceTransport() override;

    IceTransport(const IceTransport&)            = delete;
    IceTransport& operator=(const IceTransport&) = delete;
    IceTransport(IceTransport&&)                 noexcept;
    IceTransport& operator=(IceTransport&&)      noexcept;

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override;

    /** Creates the underlying juice_agent_t and starts gathering.
     *  After open(), state() will transition through Gathering -> Connecting
     *  (or Connected if remote description was set first). */
    plugins::Status open() noexcept override;

    /** Destroys the agent. After close(), open() may be called again. */
    void close() noexcept override;

    // ---- plugins::ITransport ----------------------------------------------

    void set_callbacks(plugins::RecvCallback on_recv,
                       plugins::ErrorCallback on_error) noexcept override;

    plugins::Status send(plugins::BufferView view,
                         plugins::Addr dst_addr = {}) noexcept override;

    /** Drain any packets queued by libjuice's recv thread, invoking
     *  on_recv for each. Returns number of packets dispatched. */
    int recv() noexcept override;

    plugins::Addr local_addr() const noexcept override;
    plugins::Addr remote_addr() const noexcept override;

    // ---- ICE consent freshness (RFC 7675 / RFC 8445 §10) ------------------

    /** Register a callback fired (on the ice consent tracker thread) when
     *  ICE consent freshness is lost — i.e. no peer STUN Binding request
     *  has been observed within `consent_timeout_ms`.
     *
     *  The callback fires at most once per loss event; the tracker is
     *  automatically rearmed and the callback may fire again on the next
     *  loss. Pass a null callback to clear. */
    void set_on_consent_lost(plugins::OnConsentLost cb) noexcept override;

    /** Explicitly notify the app-level consent tracker that a STUN Binding
     *  request has been received from the peer.
     *
     *  In production, ICE transports rely on libjuice's built-in consent
     *  freshness (RFC 7675 §3 in libjuice, CONSENT_TIMEOUT = 30s) for the
     *  wire-level side; this method is the visible entry point at the
     *  NimRTC wrapper layer that resets the timer.
     *
     *  Tests use this directly to simulate peer activity without spinning
     *  up a full STUN relay.
     *
     *  No-op if enable_consent_freshness is false in IceConfig, or if the
     *  transport is not yet Connected.
     *
     *  @note Thread-safe. */
    void notify_binding_received() noexcept;

    /** Returns true if a single on_consent_lost event has been fired since
     *  the last reset (or since open()), or since the last reset triggered
     *  by notify_binding_received().
     *
     *  Useful for tests that want a one-shot signal instead of a callback.
     *  Cleared by notify_binding_received() (or by close() / open()). */
    bool consent_lost_pending() const noexcept;

    /** Test-only: arm the consent-freshness tracker as if ICE has reached
     *  Connected state, without having to wait for an actual libjuice
     *  connectivity check.
     *
     *  Production code MUST NOT call this; consent tracking is normally
     *  enabled only after a successful candidate-pair selection so we don't
     *  generate false-positive on_consent_lost() events during slow handshakes
     *  on lossy networks.
     *
     *  Used by `tests/test_consent_freshness.cpp` to keep the unit-test
     *  runtime well under 1 second per case; without it, each test would
     *  need ~5–10 s of loopback connectivity before the timer becomes
     *  meaningful.
     *
     *  No-op if `enable_consent_freshness` is `false` in IceConfig. */
    void force_consent_armed_for_testing() noexcept;

    // ICE-specific ------------------------------------------------------

    /** Get the local candidates gathered so far (SDP-ready, full
     *  "a=candidate:<rest>" lines).  Empty if gathering hasn't started. */
    std::vector<std::string> gathered_local_candidates() const noexcept override;

    /** Wait (with timeout) until gathering completes or ICE reaches
     *  Connected/Completed state.  Returns true if finished within
     *  `timeout_ms`, false otherwise. */
    bool wait_for_gathering(int timeout_ms) noexcept override;

    plugins::IceState state() const noexcept override;
    Role               role()   const noexcept;

    /** Returns the local SDP description (a=ice-ufrag/pwd, a=candidate lines).
     *  Should be embedded in the SDP "media" section. May be called multiple
     *  times during gathering; results differ as new candidates arrive. */
    std::string local_description() const;

    /** Set the remote ICE description (SDP media block containing a=ice-ufrag,
     *  a=ice-pwd, and optionally a=candidate lines).  Can be called before
     *  open() (preferred — sets the ICE role correctly before gathering) or
     *  after open() (applied immediately via juice_set_remote_description()).
     *
     *  The SDP should contain at minimum the ICE ufrag and pwd lines, e.g.:
     *    a=ice-ufrag:xxx\n
     *    a=ice-pwd:yyy\n
     *  The full SDP media block (with candidates) is also accepted.
     *
     *  @return plugins::kOk on success;
     *          plugins::kErrCorrupt if the SDP was syntactically invalid;
     *          plugins::kErrNotReady if the agent isn't initialised and
     *          we couldn't even cache the SDP for deferred apply. */
    plugins::Status set_remote_description(std::string_view sdp) noexcept override;

    /** Add a single remote candidate (full SDP candidate line including the
     *  "candidate:" prefix is fine; "a=candidate:..." also accepted). */
    plugins::Status add_remote_candidate(std::string_view sdp) noexcept override;

    /** Override configuration before open() (EngineConfig → IceConfig bridge). */
    void set_config(const IceConfig& cfg) noexcept;

    /** Override binding address (e.g. from EngineConfig.local_bind_address).
     *  Only effective before open() — libjuice binds at create-time. */
    void set_bind_address(std::string_view addr) noexcept override;

    /** Override STUN server. Same precondition as set_bind_address. */
    void set_stun_server(std::string_view host, std::uint16_t port) noexcept override;

    /** Override local UDP port range. Same precondition as above. */
    void set_local_port_range(std::uint16_t begin, std::uint16_t end) noexcept override;

    /** ICE credentials as generated by libjuice (or as overridden via config). */
    std::string local_ufrag()    const noexcept override;
    std::string local_password() const noexcept override;
    /** ICE credentials parsed from the remote SDP (set after set_remote_description).
     *  Empty before that. */
    std::string remote_ufrag()    const noexcept;
    std::string remote_password() const noexcept;

    /** Selected candidates after the agent reaches Connected/Completed.
     *  Empty until then. */
    std::optional<Candidate> selected_local()  const noexcept;
    std::optional<Candidate> selected_remote() const noexcept;

    /** Trigger a fresh gathering round. open() calls this automatically. */
    plugins::Status gather_candidates() noexcept;

    // ---- BWE / Scheduler injection (IICETransport override) ---------------

    /** @override plugins::IICETransport */
    void set_bwe(plugins::IBwe* bwe) noexcept override;

    /** @override plugins::IICETransport */
    void set_scheduler(plugins::IScheduler* sched) noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// -----------------------------------------------------------------------------
// IceTransportFactory — registers an IICETransport under the id "ice".
//
// Inherits from `plugins::IICETransportFactory` (which virtually inherits
// `plugins::ITransportFactory`) so the same instance satisfies both
// `core::PluginRegistry::register_transport()` and
// `core::PluginRegistry::register_ice_transport()` — consumers can pick
// the strongest typed view that matches their needs.
// -----------------------------------------------------------------------------

class IceTransportFactory : public plugins::IICETransportFactory {
public:
    /** Factory with a default IceConfig. Use create() to get a fresh transport
     *  per connection. */
    IceTransportFactory();

    /** Factory with a shared config (every transport starts from this base;
     *  the application can clone + customise IceConfig before creating each
     *  transport, since create() returns an unconfigured instance). */
    explicit IceTransportFactory(IceConfig default_config);

    std::string_view id() const noexcept override;
    std::string_view display_name() const noexcept override;

    /** Returns a new IceTransport with default_config. Caller owns the
     *  returned pointer; will not be the same as default_config.role/etc —
     *  call open() then configure per-connection ICE state. */
    plugins::IICETransport* create_ice() const override;

private:
    IceConfig default_config_;
};

// ---------------------------------------------------------------------------
// Public registration entry point (MSVC static-link workaround)
// ---------------------------------------------------------------------------

namespace detail {
/** Defined in ice.cpp. Forces the .obj (and its static IceRegistrar) into
 *  the consumer's link when register_default_plugins() is called. */
void do_register_default_plugins() noexcept;
} // namespace detail

/**
 * @brief Register the built-in "ice" transport factory with core::PluginRegistry.
 *
 * MUST be called once at program startup before any
 * `core::PluginRegistry::instance().get_transport(...)` lookup of the "ice"
 * factory. Idempotent (Meyer's-singleton latch inside ice.cpp).
 *
 * @note Not `inline` because the static-local latch would otherwise be
 *       emitted as a weak external symbol that the static lib doesn't
 *       carry; non-inline ensures the symbol is in `nimrtc_ice.lib`.
 */
void register_default_plugins() noexcept;

} // namespace nimrtc::ice
