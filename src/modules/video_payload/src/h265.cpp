/**
 * @file src/modules/video_payload/src/h265.cpp
 * @brief H.265 / HEVC RTP payload packetizer / depacketizer (RFC 7798).
 */

#include <nimrtc/video_payload/h265.hpp>

#include <cstring>

namespace nimrtc::video_payload::h265 {

namespace {

constexpr std::uint8_t kApType      = 48;
constexpr std::uint8_t kFuType      = 49;
constexpr std::uint8_t kTypeMask    = 0x3F;   // 6 bits
constexpr std::uint8_t kFuTypeMask  = 0x3F;

} // namespace

// ---------------------------------------------------------------------------
// The two-byte NAL unit header
// ---------------------------------------------------------------------------

std::optional<NaluHeader> parse_nalu_header(core::ByteSpan payload) noexcept {
    if (payload.size() < 2) return std::nullopt;

    NaluHeader h;
    const std::uint8_t b0 = payload[0];
    const std::uint8_t b1 = payload[1];

    h.forbidden = (b0 >> 7) & 0x01;
    h.type      = static_cast<NaluType>((b0 >> 1) & kTypeMask);
    h.layer_id  = static_cast<std::uint8_t>(((b0 & 0x01) << 5) | ((b1 >> 3) & 0x1F));
    h.tid       = static_cast<std::uint8_t>((b1 & 0x07) + 1);
    return h;
}

std::size_t build_payload_header(NaluType type,
                                 std::uint8_t layer_id,
                                 std::uint8_t tid,
                                 core::MutableByteSpan output) noexcept {
    if (output.size() < 2) return 0;

    const std::uint8_t t  = static_cast<std::uint8_t>(type) & kTypeMask;
    const std::uint8_t lid = static_cast<std::uint8_t>(layer_id) & 0x3F;
    const std::uint8_t tpl1 = static_cast<std::uint8_t>(tid > 0 ? tid : 1) & 0x07;

    // Byte 0: F(1) | Type(6) | LayerId MSB(1)
    // Byte 1: LayerId low 5 bits(5) | TID(3)
    //
    // RFC 7798 §1.1.4: the 6-bit LayerId *straddles* the two bytes — its
    // most significant bit is the last bit of byte 0 and its five
    // least significant bits are the top five bits of byte 1.
    output[0] = static_cast<std::uint8_t>(((t & kTypeMask) << 1) |
                                          ((lid >> 5) & 0x01));
    output[1] = static_cast<std::uint8_t>(((lid & 0x1F) << 3) |
                                          ((tpl1 - 1) & 0x07));
    return 2;
}

// ---------------------------------------------------------------------------
// Single NAL unit packet
// ---------------------------------------------------------------------------

std::optional<Nalu> parse_single(core::ByteSpan payload) noexcept {
    if (payload.size() < 2) return std::nullopt;

    const std::uint8_t type = (payload[0] >> 1) & kTypeMask;
    // 0..47 are the "regular" NAL unit types.  48..63 are the RTP
    // payload wrappers (AP / FU / PACI), which parse_single does not
    // handle.
    if (type > 47) return std::nullopt;

    Nalu n;
    n.type = static_cast<NaluType>(type);
    n.data = payload;
    return n;
}

// ---------------------------------------------------------------------------
// AP — aggregation packet
// ---------------------------------------------------------------------------

bool parse_ap(core::ByteSpan payload,
              bool has_donl,
              std::vector<Nalu>& nalus) noexcept {
    nalus.clear();
    if (payload.size() < 2) return false;
    if (((payload[0] >> 1) & kTypeMask) != kApType) return false;

    std::size_t pos = 2;
    if (has_donl) {
        // The DONL field.  NimRTC does not use it (it is only negotiated
        // for temporal-layer scalability), but it has to be skipped when
        // the peer says it is present.
        if (pos + 2 > payload.size()) return false;
        pos += 2;
    }

    while (pos + 2 <= payload.size()) {
        const std::uint16_t nalu_len = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(payload[pos])     << 8)
          |  static_cast<std::uint16_t>(payload[pos + 1]));
        pos += 2;
        if (nalu_len < 2 || pos + nalu_len > payload.size()) break;

        Nalu n;
        n.type = static_cast<NaluType>((payload[pos] >> 1) & kTypeMask);
        n.data = core::ByteSpan{payload.data() + pos, nalu_len};
        nalus.push_back(n);
        pos += nalu_len;
    }
    return !nalus.empty();
}

std::size_t build_ap(const std::vector<Nalu>& nalus,
                      std::uint8_t layer_id,
                      std::uint8_t tid,
                      core::MutableByteSpan output) noexcept {
    if (nalus.empty() || output.size() < 2) return 0;

    // The AP payload header: type = 48.
    std::size_t pos = build_payload_header(NaluType::kAP, layer_id, tid, output);
    if (pos == 0) return 0;

    for (const auto& n : nalus) {
        // Each aggregated NAL unit needs its own 16-bit length prefix and
        // must carry at least a two-byte NAL header.
        if (n.data.size() < 2) continue;
        if (pos + 2 + n.data.size() > output.size()) return 0;
        output[pos + 0] = static_cast<std::uint8_t>((n.data.size() >> 8) & 0xFF);
        output[pos + 1] = static_cast<std::uint8_t>( n.data.size()       & 0xFF);
        std::memcpy(output.data() + pos + 2, n.data.data(), n.data.size());
        pos += 2 + n.data.size();
    }
    return pos;
}

// ---------------------------------------------------------------------------
// FU — fragmentation unit
// ---------------------------------------------------------------------------

bool parse_fu_header(core::ByteSpan payload, FuHeader& out) noexcept {
    if (payload.size() < 3) return false;
    if (((payload[0] >> 1) & kTypeMask) != kFuType) return false;

    const std::uint8_t fu_header = payload[2];
    out.fu_header = fu_header;
    out.fu_type   = static_cast<NaluType>(fu_header & kFuTypeMask);
    out.start     = (fu_header & 0x80) != 0;
    out.end       = (fu_header & 0x40) != 0;
    return true;
}

std::size_t build_fu(NaluType original_nalu_type,
                     std::uint8_t layer_id,
                     std::uint8_t tid,
                     core::ByteSpan fragment_data,
                     bool start,
                     bool end,
                     core::MutableByteSpan output) noexcept {
    // payload header (2 bytes) + FU header (1 byte) + fragment
    if (output.size() < 3 + fragment_data.size()) return 0;

    std::size_t pos = build_payload_header(NaluType::kFU, layer_id, tid, output);
    if (pos == 0) return 0;

    std::uint8_t fu_header = static_cast<std::uint8_t>(original_nalu_type) & kFuTypeMask;
    if (start) fu_header |= 0x80;
    if (end)   fu_header |= 0x40;
    output[pos++] = fu_header;

    if (!fragment_data.empty()) {
        std::memcpy(output.data() + pos, fragment_data.data(), fragment_data.size());
        pos += fragment_data.size();
    }
    return pos;
}

// ---------------------------------------------------------------------------
// Convenience — top-level dispatcher
// ---------------------------------------------------------------------------

NaluType peek_nalu_type(core::ByteSpan payload) noexcept {
    if (payload.size() < 2) return NaluType::kUnspecified;
    return static_cast<NaluType>((payload[0] >> 1) & kTypeMask);
}

NaluType peek_payload_type(core::ByteSpan payload) noexcept {
    return peek_nalu_type(payload);
}

} // namespace nimrtc::video_payload::h265
