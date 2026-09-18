/**
 * @file nimrtc/raw_udp/raw_udp_factory_iface.hpp
 * @brief IRawUdpFactory — abstract factory for IRawUdpDatagram backends.
 *
 * Transport PAL Slice 6 — promoted from concrete-class-only to
 * abstract-factory pattern in Slice 8 (v0.10.2) so that
 * `core::PluginRegistry::register_raw_udp_datagram(id, factory*)`
 * can hold a typed factory pointer for resolution by
 * `ITransportStack::raw_control()` / `CapabilitySelector` / engine
 * integration.
 *
 * The built-in `ArqRawUdpFactory` (src/raw_udp/src/raw_udp_factory.cpp)
 * implements this interface and registers under id `"arq"`.
 *
 * ## Why an interface, not just the concrete factory
 *
 * Before Slice 8, raw_udp exposed only the concrete class
 * `ArqRawUdpFactory`. That worked for tests but blocked two things:
 *   (a) the core::PluginRegistry typed slot (Slice 6 §6.3 / §6.5
 *       follow-up gap) — a typed `const IRawUdpFactory*` slot needs an
 *       abstract base so we can register different backends under
 *       different ids;
 *   (b) the ITransportStack factory composition (Slice 7) — when the
 *       Selector picks `webrtc-classic` + `raw-udp-arq`, it needs to
 *       hand the raw-UDP factory to the stack factory by interface so
 *       the stack factory can call `factory->create()` without
 *       dragging in the raw_udp module's full transitive set.
 *
 * Pattern parity:
 *   - dtls::IDtlsSessionFactory   (dtls/dtls_session_factory.hpp)
 *   - sctp::ISctpSocketFactory    (sctp/sctp_socket_factory.hpp)
 *   - raw_udp::IRawUdpFactory     (this header)
 *
 * ## Slice 6.5 → Slice 8 migration
 *
 * The concrete `ArqRawUdpFactory` is preserved verbatim — existing
 * consumers that take `const ArqRawUdpFactory*` (none ship today, but
 * tests in `tests/test_raw_udp_arq.cpp` import the type) continue to
 * compile. The Slice 8 change is purely additive: the factory now
 * publicly inherits `IRawUdpFactory` and overrides `id()`,
 * `display_name()`, and `create()` virtually.
 *
 * ## v0.11.0 follow-up (out of Slice 8 scope)
 *
 * Add a `RawUdpConfig` parameter to `create()` (currently no-arg) so
 * the Slice 7 Stack factory can pass per-stack tuning (local_port,
 * peer_endpoint hints) without going through the factory-level
 * default config. Slice 8 keeps the no-arg signature for source-
 * compatibility with the existing slice-6 factory.
 */

#ifndef NIMRTC_RAW_UDP_FACTORY_IFACE_HPP
#define NIMRTC_RAW_UDP_FACTORY_IFACE_HPP

#include <memory>
#include <string_view>

#include <nimrtc/raw_udp/raw_udp_datagram.hpp>   // IRawUdpDatagram

namespace nimrtc::raw_udp {

/**
 * @brief Abstract factory contract for IRawUdpDatagram backends.
 *
 * Mirrors `dtls::IDtlsSessionFactory` and `sctp::ISctpSocketFactory`
 * — the three transport-layer seam factories share the same shape so
 * the registry can hold a `const IFooFactory*` slot for each.
 *
 * Implementations MUST be cheap to construct (registry looks them up
 * on every NimRTCEngine::open() so a heavy constructor would slow
 * startup). Implementations MUST be safe to call `create()` from
 * multiple threads concurrently — each call returns a fresh
 * `IRawUdpDatagram` instance.
 */
class IRawUdpFactory {
public:
    virtual ~IRawUdpFactory() = default;

    /**
     * @brief Stable backend identifier.
     *
     * Used as the lookup key in:
     *   - `core::PluginRegistry::register_raw_udp_datagram(id, factory)`
     *   - transport stack `StackConfig::raw_id` (Slice 7)
     *   - JSON Profile `transport.control.stack` (Slice 7)
     *
     * The built-in factory returns `"arq"`. Empty string is reserved
     * for "unset / unconfigured" and is rejected at compile time by
     * PAL Slice 3's `NIMRTC_PLUGIN_ID(name)` validator.
     *
     * @return std::string_view with program-lifetime storage (typically
     *         a string literal). MUST be non-empty.
     */
    virtual std::string_view id() const noexcept = 0;

    /**
     * @brief Short human-readable display name.
     *
     * Surfaced in log lines and registry listings, matching
     * `dtls::IDtlsSessionFactory::display_name()` and
     * `sctp::ISctpSocketFactory::display_name()`.
     */
    virtual std::string_view display_name() const noexcept = 0;

    /**
     * @brief Create a new IRawUdpDatagram instance.
     *
     * Each call returns a fresh datagram configured with the
     * factory's default `RawUdpConfig`. The caller owns the returned
     * pointer; pass it to the transport stack / engine as a
     * `unique_ptr`. v0.11.0 will widen this signature with a
     * per-stack `RawUdpConfig` argument (see file header).
     *
     * `const`-qualified to match `dtls::IDtlsSessionFactory::create()`
     * — registries that hold a `const IRawUdpFactory*` can still call
     * create().
     *
     * @return A new IRawUdpDatagram; never nullptr on a well-formed
     *         factory.
     */
    virtual std::unique_ptr<IRawUdpDatagram> create() const = 0;
};

} // namespace nimrtc::raw_udp

#endif // NIMRTC_RAW_UDP_FACTORY_IFACE_HPP
