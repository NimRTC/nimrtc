/**
 * @file nimrtc/raw_udp/raw_udp_factory.hpp
 * @brief ArqRawUdpFactory — concrete factory for the ARQ raw-UDP datagram.
 *
 * Slice 6 (transport-selection §6.3) — promoted to a public
 * `IRawUdpFactory` impl in Slice 8 (v0.10.2) so the
 * `core::PluginRegistry::register_raw_udp_datagram(id, factory*)` hook
 * has a typed factory to register.
 *
 * Factory id = "arq".
 * Display name = "Raw UDP + Selective-Repeat ARQ + DTLS-PSK (Slice 6)".
 *
 * The factory itself is a regular class (not a SimpleTransportFactory-style
 * template) because:
 *   - it must produce an `IRawUdpDatagram` rather than the generic
 *     `ITransport` (raw UDP bypass does not pretend to be a full ICE+DTLS
 *     transport);
 *   - it carries a default `RawUdpConfig` that the application may
 *     override per-instance before calling `create()`.
 *
 * Slice 8: the factory now publicly inherits `IRawUdpFactory`. The
 * existing public surface (`id()`, `display_name()`, `create()`,
 * `ArqRawUdpFactory(RawUdpConfig)`) is preserved verbatim so
 * consumers that take `const ArqRawUdpFactory*` (tests, future
 * downstream callers) compile unchanged.
 */
#pragma once

#include <memory>
#include <string_view>

#include <nimrtc/raw_udp/raw_udp_datagram.hpp>
#include <nimrtc/raw_udp/raw_udp_factory_iface.hpp>   // Slice 8: typed factory iface

namespace nimrtc::raw_udp {

/** Concrete factory. Slice 8: inherits `IRawUdpFactory`. */
class ArqRawUdpFactory : public IRawUdpFactory {
public:
    ArqRawUdpFactory() = default;
    explicit ArqRawUdpFactory(RawUdpConfig default_config) noexcept;

    // ---- IRawUdpFactory (Slice 8 typed slot) -----------------------------
    //
    // `override` is intentional: the Slice 8 interface declarations are
    // already the public surface for raw_udp factories; the concrete
    // methods below are kept (without `override`) so any out-of-tree
    // caller still resolving `ArqRawUdpFactory::id()` via a
    // `const ArqRawUdpFactory*` pointer compiles unchanged.

    /** Slice-6 id. Used by `Profile.transport.control.stack = "arq"` and
     *  by `core::PluginRegistry::get_raw_udp_datagram("arq")` (Slice 8). */
    std::string_view id() const noexcept override;

    std::string_view display_name() const noexcept override;

    /** Create a fresh IRawUdpDatagram. Caller owns the returned pointer. */
    std::unique_ptr<IRawUdpDatagram> create() const override;

private:
    RawUdpConfig default_config_;
};

} // namespace nimrtc::raw_udp
