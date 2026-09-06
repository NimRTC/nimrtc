/**
 * @file src/modules/video_payload/src/h264.cpp
 * @brief H.264 RTP payload packetizer / depacketizer (RFC 6184).
 */

#include <nimrtc/video_payload/h264.hpp>

#include <cstring>

namespace nimrtc::video_payload::h264 {

namespace {

constexpr std::uint8_t kFuAType      = 28;
constexpr std::uint8_t kStapAType    = 24;
constexpr std::uint8_t kStapBType    = 25;
constexpr std::uint8_t kFuBType      = 29;
constexpr std::uint8_t kNaluTypeMask = 0x1F;
constexpr std::uint8_t kNriMask      = 0x60;
constexpr std::uint8_t kForbiddenBit = 0x80;

inline std::uint8_t make_indicator(std::uint8_t nri, std::uint8_t type) noexcept {
    return static_cast<std::uint8_t>((nri & 0x03) << 5) | (type & kNaluTypeMask);
}

} // namespace

// ---------------------------------------------------------------------------
// Single NAL Unit Packet
// ---------------------------------------------------------------------------

std::optional<Nalu> parse_single(core::ByteSpan payload) noexcept {
    if (payload.size() < 1) return std::nullopt;
    const std::uint8_t first = payload[0];
    const std::uint8_t type  = first & kNaluTypeMask;
    if (type >= 1 && type <= 23) {
        Nalu n;
        n.type = static_cast<NaluType>(type);
        n.data = payload;
        return n;
    }
    return std::nullopt;
}

std::size_t build_single(NaluType type,
                         core::ByteSpan nalu_data,
                         core::MutableByteSpan output) noexcept {
    // nalu_data is the complete NAL unit (1-byte NALU header + RBSP body);
    // see parse_single which returns Nalu::data referencing this same range.
    // Just copy the bytes as-is. If output is too small, return 0.
    if (output.size() < nalu_data.size()) return 0;
    (void)type;   // type is implicit in nalu_data[0] & 0x1F; kept for API symmetry
    if (!nalu_data.empty()) {
        std::memcpy(output.data(), nalu_data.data(), nalu_data.size());
    }
    return nalu_data.size();
}

// ---------------------------------------------------------------------------
// STAP-A
// ---------------------------------------------------------------------------

StapAResult parse_stap_a(core::ByteSpan payload) noexcept {
    StapAResult r;
    if (payload.size() < 1) return r;
    const std::uint8_t first = payload[0];
    if ((first & kNaluTypeMask) != kStapAType) return r;

    std::size_t pos = 1;
    while (pos + 2 <= payload.size()) {
        const std::uint16_t nalu_len =
            (static_cast<std::uint16_t>(payload[pos])     << 8)
          |  static_cast<std::uint16_t>(payload[pos + 1]);
        pos += 2;
        if (nalu_len == 0 || pos + nalu_len > payload.size()) break;

        Nalu n;
        n.type = static_cast<NaluType>(payload[pos] & kNaluTypeMask);
        n.data = core::ByteSpan{payload.data() + pos, nalu_len};
        r.nalus.push_back(n);
        pos += nalu_len;
    }
    r.parsed_ok = !r.nalus.empty();
    return r;
}

std::size_t build_stap_a(core::ByteSpan* nalu_datas,
                         std::size_t nalu_count,
                         core::MutableByteSpan output) noexcept {
    if (nalu_count == 0 || nalu_datas == nullptr) return 0;
    if (output.size() < 1) return 0;

    // Write STAP-A indicator (NRI=0 placeholder, type=24).
    std::size_t pos = 1;

    for (std::size_t i = 0; i < nalu_count; ++i) {
        const core::ByteSpan& n = nalu_datas[i];
        if (n.empty()) continue;
        const std::uint8_t nri = (n[0] & kNriMask) >> 5;
        if (pos == 1) {
            // First NAL unit's NRI becomes the STAP-A indicator's NRI.
            output[0] = make_indicator(nri, kStapAType);
        }
        if (pos + 2 + n.size() > output.size()) return 0;
        output[pos + 0] = static_cast<std::uint8_t>((n.size() >> 8) & 0xFF);
        output[pos + 1] = static_cast<std::uint8_t>( n.size()       & 0xFF);
        std::memcpy(output.data() + pos + 2, n.data(), n.size());
        pos += 2 + n.size();
    }
    return pos;
}

// ---------------------------------------------------------------------------
// FU-A
// ---------------------------------------------------------------------------

bool parse_fu_a_header(core::ByteSpan payload, FuAHeader& out) noexcept {
    if (payload.size() < 2) return false;
    const std::uint8_t indicator = payload[0];
    if ((indicator & kNaluTypeMask) != kFuAType) return false;
    const std::uint8_t header_byte = payload[1];

    out.fu_indicator = indicator;
    out.fu_header    = header_byte;
    out.fragment     = core::ByteSpan{payload.data() + 2, payload.size() - 2};
    return true;
}

std::size_t build_fu_a(NaluType original_nalu_type,
                       core::ByteSpan fragment_data,
                       bool start,
                       bool end,
                       core::MutableByteSpan output) noexcept {
    if (output.size() < fragment_data.size() + 2) return 0;
    // NRI of the FU-A indicator comes from the original NAL header.  We don't
    // know that here, so we use 0 (caller can override the indicator byte).
    const std::uint8_t nri = 0;
    output[0] = make_indicator(nri, kFuAType);

    std::uint8_t header = static_cast<std::uint8_t>(original_nalu_type) & kNaluTypeMask;
    if (start) header |= 0x80;
    if (end)   header |= 0x40;
    // Bit 5 (R) is reserved and must be 0.
    output[1] = header;
    if (!fragment_data.empty()) {
        std::memcpy(output.data() + 2, fragment_data.data(), fragment_data.size());
    }
    return fragment_data.size() + 2;
}

// ---------------------------------------------------------------------------
// Convenience — top-level dispatcher
// ---------------------------------------------------------------------------

NaluType peek_nalu_type(core::ByteSpan payload) noexcept {
    if (payload.empty()) return NaluType::kUnspecified;
    return static_cast<NaluType>(payload[0] & kNaluTypeMask);
}

ParseResult parse(core::ByteSpan payload) noexcept {
    ParseResult r;
    if (payload.empty()) return r;

    const std::uint8_t type = payload[0] & kNaluTypeMask;

    if (type >= 1 && type <= 23) {
        if (auto n = parse_single(payload)) {
            r.kind      = ParseResult::Kind::kSingle;
            r.primary   = *n;
            r.parsed_ok = true;
        }
    } else if (type == kStapAType) {
        auto stap = parse_stap_a(payload);
        if (stap.parsed_ok && !stap.nalus.empty()) {
            r.kind      = ParseResult::Kind::kStapA;
            r.primary   = stap.nalus.front();
            r.parsed_ok = true;
        }
    } else if (type == kFuAType) {
        if (parse_fu_a_header(payload, r.fu_a_header)) {
            r.kind        = ParseResult::Kind::kFuAFragment;
            r.fu_a_start  = (r.fu_a_header.fu_header & 0x80) != 0;
            r.fu_a_end    = (r.fu_a_header.fu_header & 0x40) != 0;
            const std::uint8_t inner = r.fu_a_header.fu_header & kNaluTypeMask;
            r.primary.type = static_cast<NaluType>(inner);
            r.primary.data = r.fu_a_header.fragment;
            r.parsed_ok    = true;
        }
    } else if (type == kFuBType) {
        // FU-B: parse indicator + header (2 bytes) + DON (2 bytes) + fragment.
        if (payload.size() >= 4) {
            r.kind         = ParseResult::Kind::kFuBFragment;
            r.fu_a_header.fu_indicator = payload[0];
            r.fu_a_header.fu_header    = payload[1];
            const std::uint8_t inner = r.fu_a_header.fu_header & kNaluTypeMask;
            r.primary.type = static_cast<NaluType>(inner);
            r.primary.data = core::ByteSpan{payload.data() + 4, payload.size() - 4};
            r.fu_a_start   = (r.fu_a_header.fu_header & 0x80) != 0;
            r.fu_a_end     = (r.fu_a_header.fu_header & 0x40) != 0;
            r.parsed_ok    = true;
        }
    } else {
        r.kind = ParseResult::Kind::kUnsupported;
    }
    return r;
}

} // namespace nimrtc::video_payload::h264
