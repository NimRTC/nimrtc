/**
 * @file src/sctp/src/dcep.cpp
 * @brief DCEP (RFC 8832) encode/decode implementation.
 *
 * ## DATA_CHANNEL_OPEN layout (RFC 8832 §5.1)
 *
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  | Message Type |  Channel Type |            Priority           |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                    Reliability Parameter                      |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |         Label Length          |       Protocol Length         |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |         Label                ...
 *
 * All multi-byte fields are big-endian.  The fixed header is 12 bytes;
 *  the label and the protocol follow it.
 *
 * ## Channel Type
 *
 * The Channel Type byte carries the reliability class in its low bits
 * and the unordered flag in its high bit (RFC 8832 §8.2.2).  See
 * `dcep.hpp` for the six registered values.
 */

#include <nimrtc/sctp/dcep.hpp>

#include <cstring>

namespace nimrtc::sctp {

namespace {

// Fixed header size of DATA_CHANNEL_OPEN: message type (1) + channel
// type (1) + priority (2) + reliability parameter (4) + label length
// (2) + protocol length (2).
constexpr std::size_t kOpenHeaderLen = 12;

/** True when the channel delivers messages out of order.
 *
 *  `plugins::DataChannelReliability::kUnreliable` always implies
 *  unordered delivery — the send path marks those messages with
 *  SCTP_UNORDERED — so the wire Channel Type has to say so as well,
 *  whatever `cfg.ordered` happens to be. */
bool channel_is_unordered(const plugins::DataChannelConfig& cfg) noexcept {
    return !cfg.ordered ||
           cfg.reliability == plugins::DataChannelReliability::kUnreliable;
}

/** Pick the RFC 8832 Channel Type for a channel config.
 *
 *  The reliability class comes from `cfg.reliability`; the unordered
 *  flag comes from `channel_is_unordered()`. */
std::uint8_t channel_type_for(const plugins::DataChannelConfig& cfg) noexcept {
    std::uint8_t base = kDcepReliable;

    switch (cfg.reliability) {
        case plugins::DataChannelReliability::kReliableOrdered:
            base = kDcepReliable;
            break;
        case plugins::DataChannelReliability::kUnreliable:
            // NimRTC's "unreliable" is the SCTP_UNORDERED flag on a
            // still-reliable association, so the wire semantics are
            // "reliable, unordered" — not a partial-reliability class.
            base = kDcepReliable;
            break;
        case plugins::DataChannelReliability::kPartialReliableTTL:
            base = kDcepPartialReliableTimed;
            break;
        case plugins::DataChannelReliability::kPartialReliableCount:
            base = kDcepPartialReliableRexmit;
            break;
    }

    if (channel_is_unordered(cfg)) {
        base = static_cast<std::uint8_t>(base | kDcepUnorderedFlag);
    }
    return base;
}

/** Reliability Parameter (u32 BE) for a channel config.
 *
 *  RFC 8832 §5.1: for reliable channels the field MUST be 0 and MUST
 *  be ignored on the receiving side; for the partial-reliability
 *  classes it carries the retransmission count (rexmit) or the
 *  lifetime in milliseconds (timed). */
std::uint32_t reliability_param_for(
    const plugins::DataChannelConfig& cfg) noexcept {
    switch (cfg.reliability) {
        case plugins::DataChannelReliability::kPartialReliableTTL:
            return cfg.max_packet_lifetime_ms;
        case plugins::DataChannelReliability::kPartialReliableCount:
            return cfg.max_retransmits;
        case plugins::DataChannelReliability::kReliableOrdered:
        case plugins::DataChannelReliability::kUnreliable:
        default:
            return 0;
    }
}

} // namespace

std::vector<std::uint8_t> dcep_encode_ack() {
    return std::vector<std::uint8_t>{kDcepAck};
}

std::vector<std::uint8_t> dcep_encode_open(
    const plugins::DataChannelConfig& cfg) {
    std::vector<std::uint8_t> out;
    const std::size_t label_len = cfg.label.size() > 65535 ? 65535 : cfg.label.size();
    out.reserve(kOpenHeaderLen + label_len);
    out.push_back(kDcepOpen);

    out.push_back(channel_type_for(cfg));

    // Priority (u16 BE) — cfg.priority is 0..255, high byte stays 0.
    out.push_back(0x00);
    out.push_back(static_cast<std::uint8_t>(cfg.priority));

    // Reliability Parameter (u32 BE).
    const std::uint32_t reliability_param = reliability_param_for(cfg);
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
    if (data == nullptr || len < kOpenHeaderLen) return std::nullopt;
    if (data[0] != kDcepOpen) return std::nullopt;
    const std::uint8_t channel_type = data[1];
    // bytes 2..3: priority (BE), bytes 4..7: reliability param (BE).
    const std::uint16_t priority = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[2]) << 8) |
         static_cast<std::uint16_t>(data[3]));
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
    if (kOpenHeaderLen + label_len + proto_len != len) return std::nullopt;

    DcepOpenParams params;
    params.priority = priority;

    // The unordered flag is the high bit of the Channel Type; the low
    // bits select the reliability class (RFC 8832 §8.2.2).
    const bool unordered = (channel_type & kDcepUnorderedFlag) != 0;
    const std::uint8_t base =
        static_cast<std::uint8_t>(channel_type & ~kDcepUnorderedFlag);

    switch (base) {
        case kDcepReliable:
            // "Reliable, unordered" is NimRTC's kUnreliable; the
            // in-order flavour is kReliableOrdered.
            params.reliability = unordered
                ? plugins::DataChannelReliability::kUnreliable
                : plugins::DataChannelReliability::kReliableOrdered;
            params.ordered = !unordered;
            break;
        case kDcepPartialReliableTimed:
            params.reliability = plugins::DataChannelReliability::kPartialReliableTTL;
            params.ordered = !unordered;
            params.max_packet_lifetime_ms =
                static_cast<std::uint16_t>(reliability_param > 65535 ? 65535 : reliability_param);
            break;
        case kDcepPartialReliableRexmit:
            params.reliability = plugins::DataChannelReliability::kPartialReliableCount;
            params.ordered = !unordered;
            params.max_retransmits =
                static_cast<std::uint16_t>(reliability_param > 65535 ? 65535 : reliability_param);
            break;
        default:
            // 0x7f / 0xff are Reserved by the RFC; anything else is
            // unassigned.  A malformed OPEN must be rejected (the
            // caller then closes the channel, RFC 8832 §6).
            return std::nullopt;
    }
    params.label.assign(reinterpret_cast<const char*>(data + kOpenHeaderLen), label_len);
    return params;
}

}  // namespace nimrtc::sctp
