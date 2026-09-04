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

// Mirrors juice_state_t 1:1 (subset — NimRTC doesn't expose internal state).
enum class IceState : std::uint8_t {
    Disconnected = 0,
    Gathering    = 1,
    Connecting   = 2,
    Connected    = 3,
    Completed    = 4,
    Failed       = 5,
};

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
};

// -----------------------------------------------------------------------------
// IceTransport — concrete ITransport.
// -----------------------------------------------------------------------------

class IceTransport : public plugins::ITransport {
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

    // ICE-specific ------------------------------------------------------

    /** Get the local candidates gathered so far (SDP-ready, full
     *  "a=candidate:<rest>" lines).  Empty if gathering hasn't started. */
    std::vector<std::string> gathered_local_candidates() const;

    /** Wait (with timeout) until gathering completes or ICE reaches
     *  Connected/Completed state.  Returns true if finished within
     *  `timeout_ms`, false otherwise. */
    bool wait_for_gathering(std::int64_t timeout_ms) noexcept;

    IceState state() const noexcept;
    Role     role() const noexcept;

    /** Returns the local SDP description (a=ice-ufrag/pwd, a=candidate lines).
     *  Should be embedded in the SDP "media" section. May be called multiple
     *  times during gathering; results differ as new candidates arrive. */
    std::string local_description() const;

    /** Apply remote SDP description (peer's ufrag / pwd / candidates). */
    plugins::Status set_remote_description(std::string_view sdp) noexcept;

    /** Add a single remote candidate (full SDP candidate line including the
     *  "candidate:" prefix is fine; "a=candidate:..." also accepted). */
    plugins::Status add_remote_candidate(std::string_view sdp) noexcept;

    /** Override configuration before open() (EngineConfig → IceConfig bridge). */
    void set_config(const IceConfig& cfg) noexcept;

    /** Override binding address (e.g. from EngineConfig.local_bind_address).
     *  Only effective before open() — libjuice binds at create-time. */
    void set_bind_address(std::string_view addr) noexcept;

    /** Override STUN server. Same precondition as set_bind_address. */
    void set_stun_server(std::string_view host, std::uint16_t port) noexcept;

    /** Override local UDP port range. Same precondition as above. */
    void set_local_port_range(std::uint16_t begin, std::uint16_t end) noexcept;

    /** ICE credentials as generated by libjuice (or as overridden via config). */
    std::string local_ufrag()    const noexcept;
    std::string local_password() const noexcept;
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// -----------------------------------------------------------------------------
// IceTransportFactory — registers an ITransport under the id "ice".
// -----------------------------------------------------------------------------

class IceTransportFactory : public plugins::ITransportFactory {
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
    plugins::ITransport* create() const override;

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
 * See `nimrtc::audio3a::register_default_plugins()` for the same pattern.
 * A future `nimrtc::core::register_all_default_plugins()` will fold all
 * per-module calls into one entry point.
 */
inline void register_default_plugins() noexcept {
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::ice
