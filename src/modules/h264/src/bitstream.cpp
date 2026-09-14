/**
 * @file src/modules/h264/src/bitstream.cpp
 * @brief H.264 Annex B bitstream parser + builder.
 */

#include <nimrtc/h264/bitstream.hpp>
#include <nimrtc/h264/decoder.hpp>  // SpsSummary

#include <cstring>
#include <vector>

namespace nimrtc::h264 {

// ---------------------------------------------------------------------------
// BitstreamParser
// ---------------------------------------------------------------------------
// Implementation lives in the header (single-line methods).
// We keep the include in this .cpp for CMake target consistency.

// ---------------------------------------------------------------------------
// SPS / PPS / IDR extraction
// ---------------------------------------------------------------------------

struct SpsSummary;     // forward; full declaration in decoder.hpp

std::optional<ParsedNalu> find_nalu(core::ByteSpan bs,
                                    video_payload::h264::NaluType target) noexcept {
    BitstreamParser p;
    ParsedNalu n;
    p.reset(bs);
    while (p.next(n)) {
        if (n.type == target) return n;
    }
    return std::nullopt;
}

std::optional<ParsedNalu> find_sps(core::ByteSpan bitstream) noexcept {
    return find_nalu(bitstream, video_payload::h264::NaluType::kSPS);
}

std::optional<ParsedNalu> find_pps(core::ByteSpan bitstream) noexcept {
    return find_nalu(bitstream, video_payload::h264::NaluType::kPPS);
}

std::optional<ParsedNalu> find_idr_slice(core::ByteSpan bitstream) noexcept {
    return find_nalu(bitstream, video_payload::h264::NaluType::kSliceIDR);
}

// ---------------------------------------------------------------------------
// Annex B builder
// ---------------------------------------------------------------------------

std::size_t build_annex_b(core::ByteSpan* nalu_datas,
                          std::size_t nalu_count,
                          std::uint8_t* output,
                          std::size_t output_capacity) noexcept {
    if (nalu_count == 0 || nalu_datas == nullptr || output == nullptr) return 0;

    constexpr std::uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};
    std::size_t pos = 0;

    for (std::size_t i = 0; i < nalu_count; ++i) {
        const core::ByteSpan& n = nalu_datas[i];
        if (n.empty()) continue;
        if (pos + 4 + n.size() > output_capacity) return 0;
        std::memcpy(output + pos, kStartCode, 4);
        std::memcpy(output + pos + 4, n.data(), n.size());
        pos += 4 + n.size();
    }
    return pos;
}

// ---------------------------------------------------------------------------
// SPS summary (best-effort)
// ---------------------------------------------------------------------------

namespace {

// Read N bits from a bitstream at the given bit offset.  MSB-first within
// each byte (H.264 convention).
inline std::uint64_t read_bits(const std::uint8_t* data, std::size_t& bit_offset,
                               std::uint32_t n) noexcept {
    std::uint64_t v = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::size_t byte_idx = (bit_offset + i) / 8;
        const std::uint32_t bit_in_byte = 7 - static_cast<std::uint32_t>((bit_offset + i) % 8);
        v = (v << 1) | ((data[byte_idx] >> bit_in_byte) & 0x01);
    }
    bit_offset += n;
    return v;
}

} // namespace

SpsSummary parse_sps(core::ByteSpan sps_nalu) noexcept {
    SpsSummary out;
    if (sps_nalu.size() < 4) return out;
    // Skip the 1-byte NAL header.
    const std::uint8_t* data = sps_nalu.data() + 1;
    std::size_t cap = sps_nalu.size() - 1;

    // profile_idc is byte 1 of the SPS body (after NAL header).
    out.profile_idc = data[0];

    // RBSP / emulation prevention: replace 00 00 03 → 00 00 in a scratch buffer.
    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(cap);
    for (std::size_t i = 0; i < cap; ++i) {
        if (i + 2 < cap && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0x03) {
            rbsp.push_back(0);
            rbsp.push_back(0);
            i += 2;
        } else {
            rbsp.push_back(data[i]);
        }
    }

    if (rbsp.size() < 4) return out;

    std::size_t bit = 0;
    // profile_idc (8) — we already read it from byte 0, skip 8 bits.
    bit = 8;
    // constraint_setN flags (3) + reserved (5)
    bit += 8;
    // level_idc (8)
    if (bit + 8 > rbsp.size() * 8) return out;
    out.level_idc = static_cast<std::uint8_t>(read_bits(rbsp.data(), bit, 8));

    // seq_parameter_set_id (ue(v))
    // log2_max_frame_num_minus4 (ue)
    // pic_order_cnt_type (ue)
    // ... (variable) ...
    // Then: max_frame_num, pic_width_in_mbs_minus1, pic_height_in_map_units_minus1
    // The exact location is profile-dependent; we use a defensive scan.
    //
    // Simplified: search for two consecutive exp-golomb-coded values that
    // match "reasonable" frame dimensions.  This is good enough for SDP
    // generation where we just need a hint; not robust enough for production.
    // For production SDP generation, parse the full SPS via libopenh264.

    // Skip ahead to scan for size fields.  Profile_idc 100/244 etc. add
    // extra bytes (chroma_format_idc etc.).  We try a few fixed offsets.
    std::size_t scan_bit = bit;
    // Skip up to 64 bits (profile-specific extensions are <64 bits typically).
    if (scan_bit + 64 > rbsp.size() * 8) return out;
    scan_bit += 64;

    // At this point we expect pic_width_in_mbs_minus1 (ue) and
    // pic_height_in_map_units_minus1 (ue).  We don't fully decode; we
    // leave width/height as 0 and signal "not enough SPS info" via
    // valid=false.  This is the conservative behaviour — host plugins
    // should override this with a real SPS parser.
    (void)scan_bit;
    out.valid = false;
    return out;
}

} // namespace nimrtc::h264
