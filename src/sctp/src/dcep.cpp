/**
 * @file src/sctp/src/dcep.cpp
 * @brief DCEP (RFC 8832) encode/decode implementation.
 */

#include <nimrtc/sctp/dcep.hpp>

#include <cstring>

namespace nimrtc::sctp {

std::vector<std::uint8_t> dcep_encode_ack() {
    return std::vector<std::uint8_t>{kDcepAck};
}

std::vector<std::uint8_t> dcep_encode_open(
    const plugins::DataChannelConfig& cfg) {
    std::vector<std::uint8_t> out;
    const std::size_t label_len = cfg.label.size() > 65535 ? 65535 : cfg.label.size();
    out.reserve(12 + label_len);
    out.push_back(kDcepOpen);

    std::uint8_t channel_type = kDcepReliable;
    std::uint16_t reliability_param = 0;
    switch (cfg.reliability) {
        case plugins::DataChannelReliability::kReliableOrdered:
            channel_type = kDcepReliable;
            break;
        case plugins::DataChannelReliability::kUnreliable:
            channel_type = kDcepReliable;
            break;
        case plugins::DataChannelReliability::kPartialReliableTTL:
            channel_type = kDcepPartialReliableTimed;
            reliability_param = cfg.max_packet_lifetime_ms;
            break;
        case plugins::DataChannelReliability::kPartialReliableCount:
            channel_type = kDcepPartialReliableRexmit;
            reliability_param = cfg.max_retransmits;
            break;
    }
    out.push_back(channel_type);
    // Priority (u16 BE) — cfg.priority is 0..255, high byte stays 0.
    out.push_back(0x00);
    out.push_back(static_cast<std::uint8_t>(cfg.priority));
    // Reliability parameter (u32 BE).
    out.push_back(static_cast<std::uint8_t>((reliability_param >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((reliability_param >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((reliability_param >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(reliability_param & 0xFF));
    // Label length + protocol length (both u16 BE). Protocol is empty.
    const auto ll = static_cast<std::uint16_t>(label_len);
    out.push_back(static_cast<std::uint8_t>((ll >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(ll & 0xFF));
    out.push_back(0x00);
    out.push_back(0x00);
    out.insert(out.end(),
               reinterpret_cast<const std::uint8_t*>(cfg.label.data()),
               reinterpret_cast<const std::uint8_t*>(cfg.label.data()) + label_len);
    return out;
}

std::optional<DcepOpenParams> dcep_decode_open(
    const std::uint8_t* data, std::size_t len) noexcept {
    if (data == nullptr || len < 12) return std::nullopt;
    if (data[0] != kDcepOpen) return std::nullopt;
    const std::uint8_t channel_type = data[1];
    // bytes 2..3: priority (BE), bytes 4..7: reliability param (BE).
    const std::uint32_t reliability_param =
        (static_cast<std::uint32_t>(data[4]) << 24) |
        (static_cast<std::uint32_t>(data[5]) << 16) |
        (static_cast<std::uint32_t>(data[6]) << 8) |
        static_cast<std::uint32_t>(data[7]);
    const auto label_len =
        static_cast<std::size_t>((static_cast<std::uint16_t>(data[8]) << 8) |
                                 static_cast<std::uint16_t>(data[9]));
    const auto proto_len =
        static_cast<std::size_t>((static_cast<std::uint16_t>(data[10]) << 8) |
                                 static_cast<std::uint16_t>(data[11]));
    if (12 + label_len + proto_len != len) return std::nullopt;
    if (label_len > 65535) return std::nullopt;

    DcepOpenParams params;
    switch (channel_type) {
        case kDcepReliable:
            params.reliability = plugins::DataChannelReliability::kReliableOrdered;
            params.ordered = true;
            break;
        case kDcepPartialReliableTimed:
            params.reliability = plugins::DataChannelReliability::kPartialReliableTTL;
            params.max_packet_lifetime_ms =
                static_cast<std::uint16_t>(reliability_param > 65535 ? 65535 : reliability_param);
            break;
        case kDcepPartialReliableRexmit:
            params.reliability = plugins::DataChannelReliability::kPartialReliableCount;
            params.max_retransmits =
                static_cast<std::uint16_t>(reliability_param > 65535 ? 65535 : reliability_param);
            break;
        default:
            return std::nullopt;
    }
    params.label.assign(reinterpret_cast<const char*>(data + 12), label_len);
    return params;
}

}  // namespace nimrtc::sctp
