/**
 * @file nimrtc/plugins/ice_transport.hpp
 * @brief IICETransport — ICE-aware transport interface on top of ITransport.
 *
 * IICETransport extends the generic ITransport contract with the operations
 * NimRTC needs to drive ICE (RFC 8445) end-to-end:
 *
 *   - credential access (local_ufrag / local_password)
 *   - state inspection (state())
 *   - waiting for the candidate gathering round (wait_for_gathering)
 *   - inspecting what ICE actually gathered (gathered_local_candidates)
 *   - applying the remote ICE description / candidates
 *     (set_remote_description / add_remote_candidate)
 *   - pre-open configuration (set_bind_address / set_stun_server /
 *     set_local_port_range)
 *
 * These were previously reachable only via `dynamic_cast<ice::IceTransport*>`
 * in `NimRTCEngine` — the plugin seam was effectively broken. With this
 * header in place, consumers resolve an `IICETransport*` through the plugin
 * registry, store it alongside the generic `ITransport*`, and call the
 * ICE-specific methods through the virtual interface.
 *
 * ## Implementing a custom ICE transport
 *
 * 1. Implement `IICETransport` (you also get the full `ITransport` contract
 *    from this class — implement both sides).
 * 2. Implement `IICETransportFactory`; the convenience base class
 *    `IICETransportFactory::create()` already returns `create_ice()` so the
 *    factory can be registered via either the ICE-specific or generic
 *    transport registry entry points.
 * 3. Register: `PluginRegistry::instance().register_ice_transport("my_ice", factory);`
 * 4. Pass name to `NimRTCEngine::Config::transport_name`.
 *
 * ## Backward compatibility
 *
 * The built-in `nimrtc::ice::IceTransport` already exposes every method on
 * this interface and additionally extends it with concrete-only helpers
 * (selected_local / selected_remote / force_consent_armed_for_testing /
 * etc.) that the engine has no business calling through the plugin seam.
 * Tests that need those helpers continue to use the concrete class.
 *
 * @note P1.2 — interface added to repair the `dynamic_cast<IceTransport*>`
 *       seam. The underlying `ice::IceState` enum values are mirrored 1:1
 *       onto `plugins::IceState` so engine code can stay in the plugin
 *       namespace.
 */

// base.hpp must be included BEFORE the include guard.
// This ensures base types are always available regardless of include order.
#include <nimrtc/core/api_version.hpp>
#include "nimrtc/plugins/transport.hpp"
#include "nimrtc/plugins/bwe.hpp"
#include "nimrtc/plugins/scheduler.hpp"

#ifndef NIMRTC_PLUGINS_ICE_TRANSPORT_HPP
#define NIMRTC_PLUGINS_ICE_TRANSPORT_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// OnIceRestart — fired when the ICE credentials changed underneath the
// transport because the *peer* restarted ICE.
// ---------------------------------------------------------------------------
//
// The distinction matters for the host:
//
//   - a *local* restart is initiated by the host (it calls restart() and
//     then produces a new offer);
//   - a *remote* restart is detected when the peer's description carries
//     different ICE credentials.  RFC 8445 §9.1.1.1 requires the answerer
//     to answer with a fresh credential pair as well, so the transport
//     regenerates its own credentials and fires this callback: the host
//     has to send the new answer (which carries the new local credentials)
//     to the peer.
//
// The callback runs on the thread that called set_remote_description().
//
// STABLE (v1.2.0): the ICE-restart seam is part of the frozen
// `IICETransport` contract (ADR-015); a host that observes a peer
// restart depends on this exact callback type.
using NIMRTC_API_STABLE OnIceRestart = std::function<void()>;

// ---------------------------------------------------------------------------
// IceState — mirrors ice::IceState 1:1.
// ---------------------------------------------------------------------------
//
// The values are intentionally identical to `nimrtc::ice::IceState` so the
// concrete `IceTransport` can cast trivially and engine code never has to
// reach back into the concrete module just to read state. Adding new states
// here MUST also update `nimrtc::ice::IceState` and the conversion in
// `nimrtc::ice::IceTransport::state()` (or vice versa).

enum class NIMRTC_API_STABLE IceState : std::uint8_t {
    Disconnected = 0,
    Gathering    = 1,
    Connecting   = 2,
    Connected    = 3,
    Completed    = 4,
    Failed       = 5,
};

// ---------------------------------------------------------------------------
// TurnTransport — the transport a TURN server is reached over.
//
// Mirrors `nimrtc::ice::TurnTransport` 1:1, so the concrete ICE transport can
// cast trivially and engine code never has to reach back into the concrete
// module to pick a TURN transport.
//
//   - `Udp` — plain TURN over UDP (RFC 5766).  Works everywhere, but a
//             firewall that blocks UDP also blocks it.
//   - `Tcp` — TURN over TCP (RFC 8656 § 12): the *control connection* to
//             the server is a TCP stream, while the relayed transport
//             address allocated by the server stays a UDP transport towards
//             the peer.  Traverses enterprise firewalls that only let TCP
//             through, at the cost of head-of-line blocking.
//   - `Tls` — TURN over TLS (RFC 6062 § 3.2).  The only option that
//             survives a middlebox that terminates TLS for anything but the
//             SNI it is configured with.
//
// STABLE (v1.2.0): part of the frozen `IICETransport` contract (ADR-015).
enum class NIMRTC_API_STABLE TurnTransport : std::uint8_t {
    Udp = 0,
    Tcp,
    Tls,
};

// ---------------------------------------------------------------------------
// IICETransport
// ---------------------------------------------------------------------------

/** ICE-aware transport contract. Inherits the full ITransport surface so
 *  callers can substitute an IICETransport* anywhere an ITransport* is
 *  expected.
 *
 *  ITransport is virtually inherited so concrete transports may inherit
 *  directly from IICETransport without incurring a duplicate ITransport
 *  base subobject (this keeps `dynamic_cast<ITransport*>(t)` and the
 *  upcast through IICETransport well-defined when a transport is
 *  constructed as `IceTransport`).
 */
class NIMRTC_API_STABLE IICETransport : public virtual ITransport {
public:
    // ---- State / credentials ----------------------------------------------

    /** Current ICE state. Mirrors juice_state_t 1:1. */
    virtual IceState state() const noexcept = 0;

    /** Local ICE ufrag (RFC 5245 §15.4). Empty before open(). */
    virtual std::string local_ufrag()    const noexcept = 0;

    /** Local ICE password (RFC 5245 §15.5). Empty before open(). */
    virtual std::string local_password() const noexcept = 0;

    // ---- Gathering --------------------------------------------------------

    /** Block up to `timeout_ms` for candidate gathering to settle. Returns
     *  true if gathering completed (or the agent already reached
     *  Connected/Completed), false on timeout. */
    virtual bool wait_for_gathering(int timeout_ms) noexcept = 0;

    /** Vector of gathered local candidates, formatted as SDP attribute
     *  bodies (each entry starts with "candidate:" or "a=candidate:"). */
    virtual std::vector<std::string> gathered_local_candidates() const noexcept = 0;

    // ---- Remote SDP / candidates -----------------------------------------

    /** Apply a remote ICE description block. `ice_block` is the raw SDP
     *  attribute lines (a=ice-ufrag / a=ice-pwd / a=candidate:...) for the
     *  m= section. Safe to call before open() (deferred until the underlying
     *  agent is created) or after open() (applied immediately). */
    virtual Status set_remote_description(std::string_view ice_block) noexcept = 0;

    /** Add a single remote ICE candidate (accepts "a=candidate:..." or
     *  "candidate:..." prefixes). */
    virtual Status add_remote_candidate(std::string_view candidate_sdp) noexcept = 0;

    // ---- ICE restart (RFC 8445 §9.1.1.1) -----------------------------------

    /** Perform an ICE restart (RFC 8445 §9.1.1.1).
     *
     *  A restart re-runs the whole ICE session with a *fresh* credential
     *  pair: the local ufrag / password are regenerated, the candidates are
     *  gathered again (which is what picks up a changed network
     *  interface, e.g. WiFi -> 4G), and the connectivity checks start over
     *  with the new credentials.
     *
     *  The peer can only observe a restart through the new credentials, so
     *  the host has to send a fresh offer afterwards:
     *
     *      ice->restart();
     *      ice->wait_for_gathering(2000);
     *      send_to_peer(create_offer());   // carries the new ice-ufrag/pwd
     *
     *  The previously known remote candidates are re-applied to the new
     *  session; the peer's own credentials are re-sent in its answer.
     *
     *  @return kOk         the restart was performed;
     *          kErrNotReady the transport is not open;
     *          kErrInternal the restart failed (the transport stays down).
     *
     *  @note Thread-safety: not safe to call concurrently with open() /
     *        close() or with the ICE callbacks.
     */
    virtual Status restart() noexcept { return kErrNotReady; }

    /** Number of ICE restarts performed on this transport.
     *
     *  0 means the transport has never been restarted. */
    virtual std::uint32_t restart_count() const noexcept { return 0; }

    /** Install the callback fired when the *peer* restarts ICE.
     *
     *  The transport detects the restart (the peer's description carries
     *  different ICE credentials), regenerates its own credentials as
     *  RFC 8445 §9.1.1.1 requires, and then fires this callback so the
     *  host can answer with the new credentials.
     *
     *  Pass a null callback to clear. */
    virtual void set_on_ice_restart(OnIceRestart cb) noexcept { (void)cb; }

    // ---- Local config (must be called before open()) ----------------------

    /** Override the local bind address (e.g. "127.0.0.1"). */
    virtual void set_bind_address(std::string_view host) noexcept = 0;

    /** Override the STUN server used for srflx candidate gathering. */
    virtual void set_stun_server(std::string_view host, std::uint16_t port) noexcept = 0;

    /** Override the local UDP port range (begin..end, inclusive). */
    virtual void set_local_port_range(std::uint16_t begin, std::uint16_t end) noexcept = 0;

    /** Add a TURN server for relay candidate gathering.
     *
     *  Pass `host` (IPv4, IPv6, or hostname), `port`, and the credentials
     *  used to allocate a relay.  Empty username/password are allowed for
     *  TURN servers with anonymous allocation.  Must be called before
     *  open() so libjuice can begin the Allocate request when gathering
     *  candidates.
     *
     *  Thread-safety: not safe to call concurrently with open(); intended
     *  for use during the same setup phase as set_stun_server().
     */
    virtual void add_turn_server(std::string_view host,
                                std::uint16_t port,
                                std::string_view username,
                                std::string_view password) noexcept = 0;

    /** Add a TURN server with an explicit transport.
     *
     *  `add_turn_server()` above reaches the server over plain TURN/UDP.
     *  This one lets the host pick TURN over TCP (RFC 8656 § 12), which is
     *  what traverses an enterprise firewall that only lets TCP through.
     *
     *  Same precondition as `add_turn_server()`: call before open().
     *
     *  @note The name is deliberately *not* an overload of
     *        `add_turn_server()`.  A second virtual with the same name
     *        would be hidden by every existing `add_turn_server()`
     *        declaration in a derived class, which GCC reports as
     *        -Woverloaded-virtual (an error under -Werror) and which
     *        would break every out-of-tree plugin that implements the
     *        interface.  A separate name keeps the frozen interface
     *        (ADR-015) additive.
     *
     *  @note TURN over TLS (RFC 6062) is accepted and reported, but the
     *        underlying vendor build does not negotiate it yet; it falls
     *        back to TCP.  The concrete transport exposes
     *        `turn_transport_supported()` for the authoritative answer.
     */
    virtual void add_turn_server_with_transport(std::string_view host,
                                                 std::uint16_t port,
                                                 std::string_view username,
                                                 std::string_view password,
                                                 TurnTransport transport) noexcept {
        // Default implementation: a transport that cannot select the
        // transport falls back to plain TURN/UDP, which every TURN server
        // supports.  The concrete ICE transport overrides this.
        (void)transport;
        add_turn_server(host, port, username, password);
    }

    /** True when the transport can actually speak the given TURN
     *  transport.  `Tls` is false today: the vendored libjuice build has no
     *  TLS code path for TURN (RFC 6062). */
    virtual bool turn_transport_supported(TurnTransport transport) const noexcept {
        (void)transport;
        return true;
    }

    // ---- BWE / Scheduler injection -------------------------------------------
    //
    // The engine creates BWE and Scheduler plugin instances at open() time
    // (via PluginRegistry) and injects them here.  This keeps the plugin
    // seam clean — the ICE transport does not know about specific BWE or
    // Scheduler implementations; it only receives the plugin interfaces.
    //
    // Ownership: the engine retains the owning unique_ptr; the ICE transport
    // stores a non-owning raw pointer.  Callers must ensure the plugins
    // outlive the ICE transport (guaranteed by engine lifecycle ordering:
    // plugins are closed / destroyed in engine::close() before ice_t_ is
    // reset).

    /** Inject the BWE plugin instance.  The ICE transport calls
     *  `bwe->on_feedback()` when it receives RTCP receiver reports, and
     *  reads `bwe->estimate()` to drive pacing.  Pass nullptr to detach
     *  (e.g. when the engine is configured without a BWE).
     *
     *  Thread-safety: the caller guarantees no concurrent calls while
     *  the ICE transport is open. */
    virtual void set_bwe(plugins::IBwe* bwe) noexcept = 0;

    /** Inject the Scheduler plugin instance.  The ICE transport calls
     *  `sched->drain_with()` from its send path to let the scheduler
     *  apply priority queuing before packets hit the wire.  Pass nullptr
     *  to bypass the scheduler (raw send — used by the "transport" profile).
     *
     *  Thread-safety: the caller guarantees no concurrent calls while
     *  the ICE transport is open. */
    virtual void set_scheduler(plugins::IScheduler* sched) noexcept = 0;

    // ---- Stack demux accessor (v1.1 Ts2, additive) ---------------------------
    //
    // Non-pure-virtual with a safe empty default so existing backends keep
    // compiling. Pops one queued inbound packet (FIFO) without invoking the
    // recv callback, so WebRtcClassicStack can drive its own demux loop.
    // Returns nullopt when the queue is empty.
    virtual std::optional<std::vector<std::uint8_t>> last_recv_packet() noexcept {
        return std::nullopt;
    }
};

// ---------------------------------------------------------------------------
// IICETransportFactory
// ---------------------------------------------------------------------------

/** Factory contract for ICE-aware transports. The base `create()` simply
 *  forwards to `create_ice()`, so a single factory instance is registerable
 *  via BOTH the generic transport registry entry point
 *  (`register_transport`) and the ICE-specific one
 *  (`register_ice_transport`). */
class NIMRTC_API_STABLE IICETransportFactory : public ITransportFactory {
public:
    /** Same as ITransportFactory::create() but typed as IICETransport*. */
    virtual IICETransport* create_ice() const = 0;

    /** Default forwarder — calls create_ice(). Override only if you need to
     *  intentionally return a base-class pointer (e.g. for binary-compat
     *  adapters). */
    ITransport* create() const override { return create_ice(); }
};

} // namespace nimrtc::plugins

#endif // NIMRTC_PLUGINS_ICE_TRANSPORT_HPP
