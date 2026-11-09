/**
 * @file src/modules/rtp/src/rtcp.cpp
 * @brief RTCP compound demultiplexing and the REMB feedback message.
 *
 * v1.2.0 M1 (F1 / F7 groundwork).  See nimrtc/rtp/rtcp.hpp for the
 * rationale and the wire diagrams.
 */
#include <nimrtc/rtp/rtcp.hpp>

#include <nimrtc/rtp/packet.hpp>

#include <algorithm>
#include <limits>
#include <string>

namespace nimrtc::rtp {

namespace {

/// The RTCP payload types this build classifies (RFC 3550 §6.1,
/// RFC 4585 §6.4).
constexpr std::uint8_t kPtSr   = 200;
constexpr std::uint8_t kPtRr   = 201;
constexpr std::uint8_t kPtSdes = 202;
constexpr std::uint8_t kPtBye  = 203;
constexpr std::uint8_t kPtApp  = 204;
constexpr std::uint8_t kPtRtpfb = 205;
constexpr std::uint8_t kPtPsfb = 206;
constexpr std::uint8_t kPtXr   = 207;

/// REMB specifics (draft-alvestrand-rmcat-remb-03 §2.2, and the
/// encoding libwebrtc implements in `rtcp_packet/remb.cc`).
///
/// The payload of the PSFB message, counted from the byte after the
/// 4-byte RTCP header:
constexpr std::size_t kRembOffsetMagic      = 8;   // 'R' 'E' 'M' 'B'
constexpr std::size_t kRembOffsetNumSsrcs   = 12;  // number of SSRC entries
constexpr std::size_t kRembOffsetExpMantissa = 13; // BR Exp | BR Mantissa hi
constexpr std::size_t kRembOffsetSsrcs      = 16;  // the SSRC feedback list
constexpr std::size_t kRembMinPayloadBytes  = 16;  // up to and incl. the exp

/// The 'R''E''M''B' identifier, big-endian.
constexpr std::uint32_t kRembMagic = 0x52454D42u;

inline std::uint16_t read_be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(p[0]) << 8) |
         static_cast<std::uint16_t>(p[1]));
}

inline std::uint32_t read_be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8)  |
            static_cast<std::uint32_t>(p[3]);
}

RtcpBlockKind classify(std::uint8_t payload_type) noexcept {
    switch (payload_type) {
        case kPtSr:    return RtcpBlockKind::kSenderReport;
        case kPtRr:    return RtcpBlockKind::kReceiverReport;
        case kPtSdes:  return RtcpBlockKind::kSourceDescription;
        case kPtBye:   return RtcpBlockKind::kGoodbye;
        case kPtApp:   return RtcpBlockKind::kApplicationDefined;
        case kPtRtpfb: return RtcpBlockKind::kRtpFeedback;
        case kPtPsfb:  return RtcpBlockKind::kPayloadSpecificFeedback;
        case kPtXr:    return RtcpBlockKind::kExtendedReport;
        default:       return RtcpBlockKind::kUnknown;
    }
}

}  // namespace


// ---------------------------------------------------------------------------
// demux_rtcp — RFC 3550 §6.1
// ---------------------------------------------------------------------------

std::vector<RtcpBlock> demux_rtcp(core::ByteSpan compound) noexcept {
    std::vector<RtcpBlock> blocks;
    if (compound.empty()) return blocks;

    const std::uint8_t* base = compound.data();
    std::size_t offset = 0;

    while (offset + kRtcpHeaderSize <= compound.size()) {
        const std::uint8_t b0 = base[offset];

        // The version must be 2 (RFC 3550 §6.1).  Anything else means
        // the walk lost its place; stop rather than guess.
        const std::uint8_t version = static_cast<std::uint8_t>((b0 >> 6) & 0x03u);
        if (version != 2) break;

        // The length field counts 32-bit words minus one (RFC 3550 §6.1).
        const std::uint16_t words = read_be16(base + offset + 2);
        const std::size_t sub_len = (static_cast<std::size_t>(words) + 1u) * 4u;

        // A sub-packet is at least its own header, and it must fit in
        // what is left of the datagram.
        if (sub_len < kRtcpHeaderSize) break;
        if (offset + sub_len > compound.size()) break;

        const std::uint8_t payload_type = base[offset + 1];

        RtcpBlock block;
        block.kind         = classify(payload_type);
        block.fmt          = static_cast<std::uint8_t>(b0 & 0x1Fu);
        block.payload_type = payload_type;
        block.packet       = compound.subspan(offset, sub_len);
        blocks.push_back(block);

        offset += sub_len;
    }

    return blocks;
}


// ---------------------------------------------------------------------------
// parse_remb — draft-alvestrand-rmcat-remb-03
// ---------------------------------------------------------------------------

core::Result<Remb> parse_remb(core::ByteSpan raw) noexcept {
    if (raw.size() < kRtcpHeaderSize) {
        return core::Result<Remb>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP REMB too small: " + std::to_string(raw.size()) +
            " < 4 bytes header");
    }

    const std::uint8_t b0 = raw[0];
    const std::uint8_t version = static_cast<std::uint8_t>((b0 >> 6) & 0x03u);
    if (version != 2) {
        return core::Result<Remb>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP REMB version != 2: got " + std::to_string(version));
    }
    if (raw[1] != kPtPsfb) {
        return core::Result<Remb>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP REMB payload type != 206 (PSFB): got " +
            std::to_string(static_cast<int>(raw[1])));
    }
    const std::uint8_t fmt = static_cast<std::uint8_t>(b0 & 0x1Fu);
    if (fmt != 15) {
        return core::Result<Remb>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP REMB FMT != 15: got " + std::to_string(fmt));
    }

    // The RTCP length field delimits the message; never read past it.
    const std::uint16_t words = read_be16(raw.data() + 2);
    const std::size_t total =
        std::min(raw.size(), (static_cast<std::size_t>(words) + 1u) * 4u);

    // payload = everything after the 4-byte RTCP header.
    const std::uint8_t* payload = raw.data() + kRtcpHeaderSize;
    const std::size_t payload_bytes = total - kRtcpHeaderSize;

    if (payload_bytes < kRembMinPayloadBytes) {
        return core::Result<Remb>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP REMB payload truncated: " + std::to_string(payload_bytes) +
            " < " + std::to_string(kRembMinPayloadBytes) + " bytes");
    }

    if (read_be32(payload + kRembOffsetMagic) != kRembMagic) {
        return core::Result<Remb>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP REMB is missing the 'REMB' identifier");
    }

    const std::uint8_t num_ssrcs = payload[kRembOffsetNumSsrcs];
    const std::size_t needed =
        kRembOffsetSsrcs + static_cast<std::size_t>(num_ssrcs) * 4u;
    if (payload_bytes < needed) {
        return core::Result<Remb>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP REMB SSRC list truncated: need " + std::to_string(needed) +
            " B, have " + std::to_string(payload_bytes) + " B");
    }

    // The exponent is the 6-bit "BR Exp" field and the mantissa is the
    // 18-bit "BR Mantissa" field; the mantissa's two most significant
    // bits share the exponent's byte.
    const std::uint8_t exp_mantissa = payload[kRembOffsetExpMantissa];
    const std::uint8_t exponent = static_cast<std::uint8_t>(exp_mantissa >> 2);
    const std::uint64_t mantissa =
        (static_cast<std::uint64_t>(exp_mantissa & 0x03u) << 16) |
         static_cast<std::uint64_t>(read_be16(payload + kRembOffsetExpMantissa + 1));

    // mantissa << exponent must not overflow 64 bits.  The exponent
    // is 6 bits wide (0..63), so the shift itself is always defined;
    // what has to be checked is whether the *value* fits.  The
    // mantissa is 18 bits, so the check only bites for a large
    // exponent.
    if (mantissa > (std::numeric_limits<std::uint64_t>::max() >> exponent)) {
        return core::Result<Remb>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP REMB bitrate overflows: mantissa " + std::to_string(mantissa) +
            " << " + std::to_string(exponent));
    }

    Remb remb;
    remb.sender_ssrc = read_be32(payload);
    remb.media_ssrc  = read_be32(payload + 4);
    remb.bitrate_bps = mantissa << exponent;
    remb.ssrcs.reserve(num_ssrcs);
    for (std::uint8_t i = 0; i < num_ssrcs; ++i) {
        remb.ssrcs.push_back(
            read_be32(payload + kRembOffsetSsrcs + static_cast<std::size_t>(i) * 4u));
    }

    return core::Result<Remb>::ok(std::move(remb));
}

}  // namespace nimrtc::rtp
