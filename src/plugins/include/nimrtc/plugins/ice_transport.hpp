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
#include "nimrtc/plugins/transport.hpp"
#include "nimrtc/plugins/bwe.hpp"
#include "nimrtc/plugins/scheduler.hpp"

#ifndef NIMRTC_PLUGINS_ICE_TRANSPORT_HPP
#define NIMRTC_PLUGINS_ICE_TRANSPORT_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// IceState — mirrors ice::IceState 1:1.
// ---------------------------------------------------------------------------
//
// The values are intentionally identical to `nimrtc::ice::IceState` so the
// concrete `IceTransport` can cast trivially and engine code never has to
// reach back into the concrete module just to read state. Adding new states
// here MUST also update `nimrtc::ice::IceState` and the conversion in
// `nimrtc::ice::IceTransport::state()` (or vice versa).

enum class IceState : std::uint8_t {
    Disconnected = 0,
    Gathering    = 1,
    Connecting   = 2,
    Connected    = 3,
    Completed    = 4,
    Failed       = 5,
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
class IICETransport : public virtual ITransport {
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
};

// ---------------------------------------------------------------------------
// IICETransportFactory
// ---------------------------------------------------------------------------

/** Factory contract for ICE-aware transports. The base `create()` simply
 *  forwards to `create_ice()`, so a single factory instance is registerable
 *  via BOTH the generic transport registry entry point
 *  (`register_transport`) and the ICE-specific one
 *  (`register_ice_transport`). */
class IICETransportFactory : public ITransportFactory {
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
