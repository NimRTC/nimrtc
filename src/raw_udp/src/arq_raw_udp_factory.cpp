/**
 * @file src/raw_udp/src/arq_raw_udp_factory.cpp
 * @brief ArqRawUdpFactory — concrete factory for ArqRawUdp, id="arq".
 *
 * Slice 6 (transport-selection §6.3).
 */
#include <nimrtc/raw_udp/arq_raw_udp.hpp>
#include <nimrtc/raw_udp/raw_udp_factory.hpp>

namespace nimrtc::raw_udp {

ArqRawUdpFactory::ArqRawUdpFactory(RawUdpConfig default_config) noexcept
    : default_config_(std::move(default_config)) {}

std::string_view ArqRawUdpFactory::id() const noexcept {
    return "arq";
}

std::string_view ArqRawUdpFactory::display_name() const noexcept {
    return "Raw UDP + Selective-Repeat ARQ + DTLS-PSK (Slice 6 skeleton)";
}

std::unique_ptr<IRawUdpDatagram> ArqRawUdpFactory::create() const {
    return std::make_unique<ArqRawUdp>(default_config_);
}

} // namespace nimrtc::raw_udp
