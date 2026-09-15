/**
 * @file nimrtc/raw_udp/raw_udp_factory.hpp
 * @brief ArqRawUdpFactory — concrete factory for the ARQ raw-UDP datagram.
 *
 * Slice 6 (transport-selection §6.3). This factory is what the Slice 7
 * Selector will eventually look up by id "arq" once stack integration
 * lands. For now, register via `nimrtc::raw_udp::register_default_plugins()`.
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
 */
#pragma once

#include <memory>
#include <string_view>

#include <nimrtc/raw_udp/raw_udp_datagram.hpp>

namespace nimrtc::raw_udp {

/** Factory contract: `id() == "arq"` and `create()` yields a fresh
 *  ArqRawUdp configured with the factory's default config. */
class ArqRawUdpFactory {
public:
    ArqRawUdpFactory() = default;
    explicit ArqRawUdpFactory(RawUdpConfig default_config) noexcept;

    /** Slice-6 id. Used by `Profile.transport.control.stack = "arq"` and
     *  by `core::PluginRegistry::get_raw_udp("arq")` (registry lookup is
     *  Slice 7 work — not added here). */
    std::string_view id() const noexcept;

    std::string_view display_name() const noexcept;

    /** Create a fresh IRawUdpDatagram. Caller owns the returned pointer. */
    std::unique_ptr<IRawUdpDatagram> create() const;

private:
    RawUdpConfig default_config_;
};

} // namespace nimrtc::raw_udp
