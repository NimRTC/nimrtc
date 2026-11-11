/**
 * @file src/modules/rtp/src/rtcp_xr.cpp
 * @brief RTCP Extended Report (XR) — RFC 3611 block parsing and building.
 *
 * v1.2.0 (session capability plan X1 / X2).  See nimrtc/rtp/rtcp_xr.hpp
 * for the rationale and the wire diagrams.
 */
#include <nimrtc/rtp/rtcp_xr.hpp>

#include <algorithm>
#include <limits>
#include <string>

namespace nimrtc::rtp {

namespace {

/// The XR payload type (RFC 3611 §2).
constexpr std::uint8_t kPtXr = 207;

/// The XR packet header is two 32-bit words (RFC 3611 §2) — one word
/// more than the header of every other RTCP packet, because the XR
/// packet carries an SSRC.
constexpr std::size_t kXrHeaderSize = 8;

/// The extended report block header: BT, type-specific, block length
/// (RFC 3611 §3).
constexpr std::size_t kXrBlockHeaderSize = 4;

/// The Receiver Reference Time block's fixed size (RFC 3611 §4.4):
/// 4-byte header + 8-byte NTP timestamp.
constexpr std::size_t kXrReceiverReferenceTimeSize = 12;

/// The Statistics Summary block's fixed size (RFC 3611 §4.6):
/// 4-byte header + 36 bytes of fields.
constexpr std::size_t kXrStatisticsSummarySize = 40;

/// One DLRR sub-block (RFC 3611 §4.5): SSRC, LRR, DLRR.
constexpr std::size_t kXrDlrrSubReportSize = 12;

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

inline std::uint64_t read_be64(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint64_t>(read_be32(p)) << 32) |
            static_cast<std::uint64_t>(read_be32(p + 4));
}

inline void write_be16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<std::uint8_t>(v & 0xFF);
}

inline void write_be32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<std::uint8_t>(v & 0xFF);
}

inline void write_be64(std::uint8_t* p, std::uint64_t v) noexcept {
    write_be32(p,     static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFULL));
    write_be32(p + 4, static_cast<std::uint32_t>(v & 0xFFFFFFFFULL));
}

/// Map the wire block type onto the enum (RFC 3611 §6.2).
XrBlockType classify_block_type(std::uint8_t bt) noexcept {
    switch (bt) {
        case 1: return XrBlockType::kLossRle;
        case 2: return XrBlockType::kDuplicateRle;
        case 3: return XrBlockType::kPacketReceiptTimes;
        case 4: return XrBlockType::kReceiverReferenceTime;
        case 5: return XrBlockType::kDlrr;
        case 6: return XrBlockType::kStatisticsSummary;
        case 7: return XrBlockType::kVoipMetrics;
        default: return XrBlockType::kUnknown;
    }
}

/// The block's *total* size in bytes, from its length field: the field
/// counts 32-bit words **minus one** (RFC 3611 §3).
std::size_t block_size_from_length(std::uint16_t length_field) noexcept {
    return (static_cast<std::size_t>(length_field) + 1u) * 4u;
}

}  // namespace


// ---------------------------------------------------------------------------
// parse_xr_block — RFC 3611 §3/§4
// ---------------------------------------------------------------------------

core::Result<XrBlock> parse_xr_block(core::ByteSpan raw) noexcept {
    if (raw.size() < kXrBlockHeaderSize) {
        return core::Result<XrBlock>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP XR block too small: " + std::to_string(raw.size()) +
            " < " + std::to_string(kXrBlockHeaderSize) + " bytes");
    }

    const std::uint8_t bt = raw[0];
    const std::uint8_t type_specific = raw[1];
    const std::uint16_t length_field = read_be16(raw.data() + 2);
    const std::size_t declared = block_size_from_length(length_field);

    // The block's declared length must fit in what we were handed.  A
    // block that claims to be longer than its container is malformed,
    // and there is no safe way to find the next block.
    if (declared > raw.size()) {
        return core::Result<XrBlock>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP XR block length " + std::to_string(declared) +
            " B exceeds the " + std::to_string(raw.size()) + " B available");
    }

    // The contents are what the block declares, not what the caller
    // happens to have handed us: the walk uses the declared length to
    // find the next block.
    const core::ByteSpan body = raw.subspan(0, declared);

    XrBlock block;
    block.type = classify_block_type(bt);
    block.type_specific = type_specific;

    switch (block.type) {
        case XrBlockType::kReceiverReferenceTime: {
            // RFC 3611 §4.4: the block is 3 words, the last of which
            // is the 64-bit NTP timestamp.
            if (declared != kXrReceiverReferenceTimeSize) {
                return core::Result<XrBlock>::fail(
                    core::ErrorCode::ProtocolError,
                    "RTCP XR Receiver Reference Time block length " +
                    std::to_string(declared) + " B, expected " +
                    std::to_string(kXrReceiverReferenceTimeSize) + " B");
            }
            block.receiver_reference_time.ntp_timestamp =
                read_be64(body.data() + kXrBlockHeaderSize);
            break;
        }

        case XrBlockType::kDlrr: {
            // RFC 3611 §4.5: the block is a header plus a whole
            // number of 3-word sub-blocks.
            const std::size_t payload = declared - kXrBlockHeaderSize;
            if (payload % kXrDlrrSubReportSize != 0) {
                return core::Result<XrBlock>::fail(
                    core::ErrorCode::ProtocolError,
                    "RTCP XR DLRR block payload " + std::to_string(payload) +
                    " B is not a multiple of " +
                    std::to_string(kXrDlrrSubReportSize) + " B");
            }
            const std::size_t count = payload / kXrDlrrSubReportSize;
            block.dlrr.sub_reports.reserve(count);
            const std::uint8_t* p = body.data() + kXrBlockHeaderSize;
            for (std::size_t i = 0; i < count; ++i) {
                XrDlrrSubReport sub;
                sub.ssrc    = read_be32(p);
                sub.last_rr = read_be32(p + 4);
                sub.dlrr    = read_be32(p + 8);
                block.dlrr.sub_reports.push_back(sub);
                p += kXrDlrrSubReportSize;
            }
            break;
        }

        case XrBlockType::kStatisticsSummary: {
            // RFC 3611 §4.6: the block is 10 words.  The flag bits
            // live in the type-specific byte.
            if (declared != kXrStatisticsSummarySize) {
                return core::Result<XrBlock>::fail(
                    core::ErrorCode::ProtocolError,
                    "RTCP XR Statistics Summary block length " +
                    std::to_string(declared) + " B, expected " +
                    std::to_string(kXrStatisticsSummarySize) + " B");
            }
            // The type-specific byte carries L (bit 7), D (bit 6),
            // J (bit 5) and ToH (bits 4..3) (RFC 3611 §4.6).
            const std::uint8_t flags = type_specific;
            XrStatisticsSummary& s = block.statistics_summary;
            s.has_loss      = (flags & 0x80u) != 0;
            s.has_duplicates = (flags & 0x40u) != 0;
            s.has_jitter    = (flags & 0x20u) != 0;
            s.ttl_or_hop_limit_kind = static_cast<std::uint8_t>((flags >> 3) & 0x03u);

            const std::uint8_t* p = body.data() + kXrBlockHeaderSize;
            s.ssrc      = read_be32(p);
            s.begin_seq = read_be16(p + 4);
            s.end_seq   = read_be16(p + 6);
            s.lost_packets = read_be32(p + 8);
            s.dup_packets  = read_be32(p + 12);
            s.min_jitter   = read_be32(p + 16);
            s.max_jitter   = read_be32(p + 20);
            s.mean_jitter  = read_be32(p + 24);
            s.dev_jitter   = read_be32(p + 28);
            s.min_ttl_or_hl  = p[32];
            s.max_ttl_or_hl  = p[33];
            s.mean_ttl_or_hl = p[34];
            s.dev_ttl_or_hl  = p[35];
            break;
        }

        default:
            // A block type this build does not parse.  Its length is
            // still validated above, so the walk can skip it safely.
            break;
    }

    return core::Result<XrBlock>::ok(std::move(block));
}


// ---------------------------------------------------------------------------
// parse_xr — RFC 3611 §2
// ---------------------------------------------------------------------------

core::Result<std::vector<XrBlock>> parse_xr(core::ByteSpan raw) noexcept {
    auto packet = parse_xr_packet(raw);
    if (!packet) {
        return core::Result<std::vector<XrBlock>>::fail(packet.error());
    }
    return core::Result<std::vector<XrBlock>>::ok(std::move(packet.value().blocks));
}

core::Result<XrPacket> parse_xr_packet(core::ByteSpan raw) noexcept {
    if (raw.size() < kXrHeaderSize) {
        return core::Result<XrPacket>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP XR packet too small: " + std::to_string(raw.size()) +
            " < " + std::to_string(kXrHeaderSize) + " bytes header");
    }

    const std::uint8_t b0 = raw[0];
    const std::uint8_t version = static_cast<std::uint8_t>((b0 >> 6) & 0x03u);
    if (version != 2) {
        return core::Result<XrPacket>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP XR version != 2: got " + std::to_string(version));
    }
    if (raw[1] != kPtXr) {
        return core::Result<XrPacket>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP XR payload type != 207: got " +
            std::to_string(static_cast<int>(raw[1])));
    }

    // The XR header's length field counts 32-bit words minus one and
    // covers the header and every block (RFC 3611 §2/§3).
    const std::uint16_t words = read_be16(raw.data() + 2);
    const std::size_t declared =
        (static_cast<std::size_t>(words) + 1u) * 4u;
    if (declared > raw.size()) {
        return core::Result<XrPacket>::fail(
            core::ErrorCode::ProtocolError,
            "RTCP XR length field claims " + std::to_string(declared) +
            " B, only " + std::to_string(raw.size()) + " B available");
    }

    XrPacket packet;
    packet.ssrc = read_be32(raw.data() + 4);

    std::vector<XrBlock> blocks;
    std::size_t offset = kXrHeaderSize;

    while (offset < declared) {
        const std::size_t left = declared - offset;
        if (left < kXrBlockHeaderSize) {
            return core::Result<XrPacket>::fail(
                core::ErrorCode::ProtocolError,
                "RTCP XR trailing bytes are not a block header: " +
                std::to_string(left) + " B");
        }

        const std::uint16_t length_field =
            read_be16(raw.data() + offset + 2);
        const std::size_t block_size = block_size_from_length(length_field);
        if (block_size > left) {
            return core::Result<XrPacket>::fail(
                core::ErrorCode::ProtocolError,
                "RTCP XR block length " + std::to_string(block_size) +
                " B exceeds the " + std::to_string(left) + " B left");
        }

        auto block = parse_xr_block(raw.subspan(offset, block_size));
        if (!block) {
            return core::Result<XrPacket>::fail(block.error());
        }
        blocks.push_back(std::move(block.value()));

        offset += block_size;
    }

    packet.blocks = std::move(blocks);
    return core::Result<XrPacket>::ok(std::move(packet));
}


// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

namespace {

/// Write a block header (RFC 3611 §3).  `total_size` is the block's
/// size in bytes, header included.
void write_block_header(std::uint8_t* p, std::uint8_t bt,
                        std::uint8_t type_specific,
                        std::size_t total_size) noexcept {
    p[0] = bt;
    p[1] = type_specific;
    // The length field counts 32-bit words minus one.
    write_be16(p + 2, static_cast<std::uint16_t>(total_size / 4u - 1u));
}

}  // namespace


core::ByteBuffer build_xr_receiver_reference_time(std::uint64_t ntp_timestamp) {
    core::ByteBuffer out(kXrReceiverReferenceTimeSize, 0);
    write_block_header(out.data(), 4, 0, out.size());
    write_be64(out.data() + kXrBlockHeaderSize, ntp_timestamp);
    return out;
}


core::ByteBuffer build_xr_dlrr(const std::vector<XrDlrrSubReport>& sub_reports) {
    core::ByteBuffer out(kXrBlockHeaderSize +
                              sub_reports.size() * kXrDlrrSubReportSize,
                          0);
    write_block_header(out.data(), 5, 0, out.size());
    std::uint8_t* p = out.data() + kXrBlockHeaderSize;
    for (const XrDlrrSubReport& sub : sub_reports) {
        write_be32(p,      sub.ssrc);
        write_be32(p + 4,  sub.last_rr);
        write_be32(p + 8,  sub.dlrr);
        p += kXrDlrrSubReportSize;
    }
    return out;
}


core::ByteBuffer build_xr_statistics_summary(const XrStatisticsSummary& summary) {
    core::ByteBuffer out(kXrStatisticsSummarySize, 0);

    // The flag bits (RFC 3611 §4.6): L (loss), D (duplicates),
    // J (jitter) and ToH (the TTL / Hop Limit kind, 2 bits).
    std::uint8_t flags = 0;
    if (summary.has_loss)       flags |= 0x80u;
    if (summary.has_duplicates) flags |= 0x40u;
    if (summary.has_jitter)     flags |= 0x20u;
    flags |= static_cast<std::uint8_t>(
        (summary.ttl_or_hop_limit_kind & 0x03u) << 3);

    write_block_header(out.data(), 6, flags, out.size());

    std::uint8_t* p = out.data() + kXrBlockHeaderSize;
    write_be32(p,      summary.ssrc);
    write_be16(p + 4,  summary.begin_seq);
    write_be16(p + 6,  summary.end_seq);
    write_be32(p + 8,  summary.lost_packets);
    write_be32(p + 12, summary.dup_packets);
    write_be32(p + 16, summary.min_jitter);
    write_be32(p + 20, summary.max_jitter);
    write_be32(p + 24, summary.mean_jitter);
    write_be32(p + 28, summary.dev_jitter);
    p[32] = summary.min_ttl_or_hl;
    p[33] = summary.max_ttl_or_hl;
    p[34] = summary.mean_ttl_or_hl;
    p[35] = summary.dev_ttl_or_hl;

    return out;
}


core::ByteBuffer build_xr(std::uint32_t ssrc, const std::vector<XrBlock>& blocks) {
    // Serialise the blocks first: the packet's length field depends on
    // their total size.
    core::ByteBuffer body;
    for (const XrBlock& block : blocks) {
        core::ByteBuffer encoded;
        switch (block.type) {
            case XrBlockType::kReceiverReferenceTime:
                encoded = build_xr_receiver_reference_time(
                    block.receiver_reference_time.ntp_timestamp);
                break;
            case XrBlockType::kDlrr:
                encoded = build_xr_dlrr(block.dlrr.sub_reports);
                break;
            case XrBlockType::kStatisticsSummary:
                encoded = build_xr_statistics_summary(block.statistics_summary);
                break;
            default:
                // A block whose type this build does not parse cannot
                // be built — there is no wire format to emit for it.
                // Skip it rather than emitting a malformed block.
                continue;
        }
        body.insert(body.end(), encoded.begin(), encoded.end());
    }

    const std::size_t total = kXrHeaderSize + body.size();
    // The RTCP length field is 16 bits counting 32-bit words minus one
    // (RFC 3550 §6.1).  An XR packet that does not fit cannot be sent.
    if (total / 4u - 1u > 0xFFFFu) return {};

    core::ByteBuffer out(kXrHeaderSize, 0);
    // V=2, no padding, reserved bits zero (RFC 3611 §2).
    out[0] = 0x80;
    out[1] = kPtXr;
    write_be16(out.data() + 2, static_cast<std::uint16_t>(total / 4u - 1u));
    write_be32(out.data() + 4, ssrc);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

}  // namespace nimrtc::rtp
