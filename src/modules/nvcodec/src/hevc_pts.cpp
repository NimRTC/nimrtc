/**
 * @file src/modules/nvcodec/src/hevc_pts.cpp
 * @brief HEVC parameter-set helpers (v1.3 plan item Q1b) — see the header.
 *
 * The syntax follows ITU-T H.265 §7.3.3 (profile_tier_level) and
 * §7.3.2.2.1 (sps_seq_parameter_set_rbsp) line by line.  The only
 * departure from the tables is that the loops *after*
 * `conf_win_bottom_offset` are not parsed at all: the parser stops there,
 * which is why every field it does need but nobody consumes is still
 * *read* (to advance the bit position) and then discarded.
 *
 * The `ProfileTierLevel` / `SpsInfo` layout is itself part of Q1b: the
 * NVDEC backend uses the *same* type to remember the profile of the
 * sequence it created its decoder for, and the test uses it to read the
 * profile out of the bitstream.  One struct, one meaning.
 */

#include "hevc_pts.hpp"

#include <algorithm>
#include <array>

namespace nimrtc::nvcodec::hevc_detail {
namespace {

constexpr std::uint8_t kSpsType = 33;
constexpr std::uint8_t kPpsType = 34;

// ---------------------------------------------------------------------------
// nalu_to_rbsp
// ---------------------------------------------------------------------------

} // namespace

std::vector<std::uint8_t> nalu_to_rbsp(core::ByteSpan nalu) {
    std::vector<std::uint8_t> out;
    if (nalu.size() <= 2) return out;   // the two-byte NAL header is required

    out.reserve(nalu.size() - 2);
    // Copy the payload, then drop the emulation-prevention bytes (00 00 03
    // -> 00 00).  The RBSP is the NAL unit *without* its header.
    const auto* p = nalu.data();
    std::size_t n = nalu.size();
    // A start code may still be attached: `core::ByteSpan(log.x, x.size())`
    // on an `std::array` keeps its full extent, so the span a caller hands
    // over can be longer than the unit it means.  Skip the prefix when one
    // is present (a 0x00 first byte is never a NAL header, ITU-T H.265
    // §7.3.1.2); otherwise the two-byte header sits at the front.
    if (p[0] == 0) {
        if (n >= 4 && p[1] == 0 && p[2] == 1) {
            p += 3;
            n -= 3;
        } else if (n >= 5 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
            p += 4;
            n -= 4;
        }
    }
    if (n <= 2) return out;
    out.reserve(n - 2);
    for (std::size_t i = 2; i < n; ++i) out.push_back(p[i]);
    std::size_t zeros = 0;
    std::size_t w     = 0;
    for (std::size_t r = 0; r < out.size(); ++r) {
        const std::uint8_t b = out[r];
        if (zeros >= 2 && b == 0x03) {
            zeros = 0;
            continue;   // drop the emulation-prevention byte
        }
        zeros = (b == 0) ? zeros + 1 : 0;
        out[w++] = b;
    }
    out.erase(out.begin() + static_cast<std::ptrdiff_t>(w), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// profile_tier_level
// ---------------------------------------------------------------------------

/** Skip the per-sub-layer PTL blocks and record each sub-layer's
 *  profile_idc (ITU-T H.265 §7.3.3: the `for (i = 0; i < maxSubLayersMinus1;
 *  i++)` loop over profile_present_flag / level_present_flag /
 *  sub_layer_profile_idc[i] ... sub_layer_level_idc[i]). */
template <typename Buf>
void skip_sub_layer_blocks(BasicBitReader<Buf>& r,
                           std::uint8_t max_num_sub_layers_minus1,
                           ProfileTierLevel& out) noexcept {
    auto& ids = out.sub_layer_profile_idc;
    ids.clear();
    // The two presence flags run `i < maxNumSubLayersMinus1` (the top
    // sub-layer is described by the general_* fields above), and when
    // max > 0 they are followed by `reserved_zero_2bits[i]` for
    // `i = maxNumSubLayersMinus1 .. 7` — a variable-length pad, not a
    // fixed eight words.  Both loops must be read in full: a missing
    // bit here shifts every SPS field after the PTL.
    std::array<bool, 7> profile_present{};
    std::array<bool, 7> level_present{};
    for (std::uint32_t i = 0; i < max_num_sub_layers_minus1 && i < 7u; ++i) {
        profile_present[i] = r.u(1) != 0;   // sub_layer_profile_present_flag[i]
        level_present[i]   = r.u(1) != 0;   // sub_layer_level_present_flag[i]
    }
    if (max_num_sub_layers_minus1 > 0) {
        r.skip(static_cast<std::size_t>(8u - max_num_sub_layers_minus1) * 2u);
    }

    for (std::uint32_t i = 0; i < max_num_sub_layers_minus1 && i < 7u; ++i) {
        const bool have_profile = profile_present[i];
        const bool have_level   = level_present[i];
        std::uint8_t idc = 0;
        if (have_profile) {
            r.skip(2);   // sub_layer_profile_space[i]
            r.skip(1);   // sub_layer_tier_flag[i]
            idc = static_cast<std::uint8_t>(r.u(5));   // sub_layer_profile_idc[i]
            r.skip(32);  // sub_layer_profile_compatibility_flag[i][j]
            r.skip(4);   // progressive / interlaced / non_packed / frame_only
            r.skip(44);  // sub_layer_reserved_zero_44bits[i]
        }
        if (have_level) r.skip(8);   // sub_layer_level_idc[i]
        ids.push_back(idc);
    }
}

template <typename Buf>
bool parse_profile_tier_level(BasicBitReader<Buf>& r,
                              std::uint8_t max_num_sub_layers_minus1,
                              ProfileTierLevel& out) noexcept {
    out.max_sub_layers_minus1 = max_num_sub_layers_minus1;

    // ---- general profile / tier / level ----------------------------------
    r.skip(2);   // general_profile_space
    r.skip(1);   // general_tier_flag
    out.general_profile_idc = static_cast<std::uint8_t>(r.u(5));

    // general_profile_compatibility_flag[j] — bit j set means "profile j is
    // also conformed to".  The flags are what a decoder must widen its
    // session for (see `has_compatible_profiles`).
    std::uint32_t compat[32] = {};
    for (std::size_t j = 0; j < 32; ++j) {
        compat[j] = r.u(1);
        // H.264 Table E-2 requires flag[profile_idc] == 1 at every
        // conformance point, so that bit alone says nothing: a MAIN
        // stream that sets only its own flag is still MAIN.  Any *other*
        // set flag is a widening claim (MAIN10 also conforming to MAIN).
        if (compat[j] && j != out.general_profile_idc) {
            out.has_compatible_profiles = true;
        }
    }
    r.skip(4);    // general_progressive_source_flag, interlaced, non_packed, frame_only
    r.skip(44);   // general_reserved_zero_44bits (ITU-T H.265 §7.3.3)
    r.skip(8);    // general_level_idc

    // ---- sub-layers ------------------------------------------------------
    skip_sub_layer_blocks(r, max_num_sub_layers_minus1, out);
    if (r.overflowed()) return false;

    // ---- the profile list -------------------------------------------------
    std::vector<std::uint8_t> profiles;
    profiles.push_back(out.general_profile_idc);
    for (std::size_t j = 0; j < 32; ++j) {
        if (compat[j]) profiles.push_back(static_cast<std::uint8_t>(j));
    }
    for (const std::uint8_t p : out.sub_layer_profile_idc) {
        if (p != out.general_profile_idc) out.has_compatible_profiles = true;
        profiles.push_back(p);
    }
    std::sort(profiles.begin(), profiles.end());
    profiles.erase(std::unique(profiles.begin(), profiles.end()), profiles.end());
    out.profiles = std::move(profiles);
    return true;
}

std::uint8_t highest_profile(const ProfileTierLevel& ptl) noexcept {
    std::uint8_t hi = ptl.general_profile_idc;
    for (const std::uint8_t p : ptl.sub_layer_profile_idc) {
        if (p > hi) hi = p;
    }
    return hi;
}

// ---------------------------------------------------------------------------
// SPS
// ---------------------------------------------------------------------------

bool parse_sps(core::ByteSpan sps_nalu, SpsInfo& out) noexcept {
    if (sps_nalu.data() == nullptr || sps_nalu.size() < 3) return false;

    // Accept both framings the callers hand over: a bare Annex-B body
    // (`NALUnit::data()`, NAL header included, no start code) and a
    // complete NAL unit with its start code (`summarize_hevc_pts` below).
    // The two-byte H.265 NAL header is what tells them apart — a 0x00
    // first byte is never a NAL header (ITU-T H.265 §7.3.1.2).
    core::ByteSpan body = sps_nalu;
    if (body.data()[0] == 0) {
        if (body.size() >= 6 && body.data()[1] == 0 && body.data()[2] == 1) {
            body = core::ByteSpan{body.data() + 3, body.size() - 3};
        } else if (body.size() >= 7 && body.data()[1] == 0 && body.data()[2] == 0 &&
                   body.data()[3] == 1) {
            body = core::ByteSpan{body.data() + 4, body.size() - 4};
        } else {
            return false;   // starts with a zero but carries no start code
        }
    }
    if (body.size() < 4) return false;
    // nuh_unit_type must be SPS (ITU-T H.265 Table 7-1).  The layer id is
    // not filtered on: the syntax below is the same for every layer.
    if (((body.data()[0] >> 1) & 0x3Fu) != 33u) return false;

    return parse_sps_body(nalu_to_rbsp(body), out);
}

bool parse_sps_body(core::ByteSpan rbsp_in, SpsInfo& out) noexcept {
    if (rbsp_in.data() == nullptr || rbsp_in.size() < 2) return false;

    // Copy through a vector so `BasicBitReader`'s buffer type is the one
    // the explicit instantiation in the test covers.
    const std::vector<std::uint8_t> rbsp(rbsp_in.data(),
                                         rbsp_in.data() + rbsp_in.size());
    BitReader r{rbsp};

    out.sps_video_parameter_set_id = static_cast<std::uint8_t>(r.u(4));
    out.sps_max_sub_layers_minus1  = static_cast<std::uint8_t>(r.u(3));
    r.skip(1);   // sps_temporal_id_nesting_flag
    if (!parse_profile_tier_level(r, out.sps_max_sub_layers_minus1, out.ptl)) {
        return false;
    }

    out.sps_seq_parameter_set_id = r.ue();
    out.chroma_format_idc        = r.ue();
    if (out.chroma_format_idc == 3) r.skip(1);   // separate_colour_plane_flag

    out.pic_width_in_luma_samples  = r.ue();
    out.pic_height_in_luma_samples = r.ue();

    out.conformance_window_flag = r.u(1) != 0;
    if (out.conformance_window_flag) {
        out.conf_win_left_offset   = r.ue();
        out.conf_win_right_offset  = r.ue();
        out.conf_win_top_offset    = r.ue();
        out.conf_win_bottom_offset = r.ue();
    } else {
        out.conf_win_left_offset   = 0;
        out.conf_win_right_offset  = 0;
        out.conf_win_top_offset    = 0;
        out.conf_win_bottom_offset = 0;
    }

    return !r.overflowed();
}

std::uint32_t visible_width(const SpsInfo& sps) noexcept {
    const std::uint32_t sub_width_c =
        (sps.chroma_format_idc == 1 || sps.chroma_format_idc == 2) ? 2u : 1u;
    return sps.pic_width_in_luma_samples -
           sub_width_c * (sps.conf_win_left_offset + sps.conf_win_right_offset);
}

std::uint32_t visible_height(const SpsInfo& sps) noexcept {
    const std::uint32_t sub_height_c = (sps.chroma_format_idc == 1) ? 2u : 1u;
    return sps.pic_height_in_luma_samples -
           sub_height_c * (sps.conf_win_top_offset + sps.conf_win_bottom_offset);
}

// ---------------------------------------------------------------------------
// summarize_hevc_pts
// ---------------------------------------------------------------------------

namespace {

/** One NAL unit located in an Annex B byte stream. */
struct NalSpan {
    std::uint8_t        type = 0;   ///< the 6-bit nuh_unit_type
    const std::uint8_t* data = nullptr;
    std::size_t         size = 0;
};

/** Find the next start code (3- or 4-byte) at or after @p from.
 *
 *  Returns the offset of the first zero of the start code and, in
 *  @p sc_len, how long the prefix is; `npos` when there is none.  The
 *  scan is deliberately local: the summary is a *second opinion* on the
 *  same bytes the H.265 module's parser frames, so a bug in one must not
 *  be able to hide in the other. */
std::size_t find_start_code(core::ByteSpan b, std::size_t from,
                            std::size_t& sc_len) noexcept {
    const std::size_t n = b.size();
    for (std::size_t i = from; i + 2 < n; ++i) {
        if (b.data()[i] != 0 || b.data()[i + 1] != 0) continue;
        if (b.data()[i + 2] == 1) { sc_len = 3; return i; }
        if (i + 3 < n && b.data()[i + 2] == 0 && b.data()[i + 3] == 1) {
            sc_len = 4;
            return i;
        }
    }
    return std::size_t(-1);
}

/** Walk @p b and hand each NAL unit (header included, start code not) to
 *  @p visit.  Stops early when @p visit returns false. */
template <typename Visit>
void for_each_nalu(core::ByteSpan b, Visit&& visit) {
    std::size_t pos = 0;
    std::size_t len = 0;
    std::size_t sc  = find_start_code(b, pos, len);
    while (sc != std::size_t(-1)) {
        const std::size_t body     = sc + len;
        std::size_t       next_len = 0;
        const std::size_t next     = find_start_code(b, body, next_len);
        const std::size_t end      = (next == std::size_t(-1)) ? b.size() : next;
        if (end >= body + 2) {
            NalSpan n;
            n.data = b.data() + body;
            n.size = end - body;
            // Trailing zero bytes belong to the *next* four-byte start
            // code prefix, not to this NAL unit.
            while (n.size > 2 && n.data[n.size - 1] == 0 && n.data[n.size - 2] == 0) {
                --n.size;
            }
            n.type = static_cast<std::uint8_t>((n.data[0] >> 1) & 0x3Fu);
            if (!visit(n)) return;
        }
        if (next == std::size_t(-1)) return;
        pos = next;
        sc  = next;
        len = next_len;
    }
}

/** FNV-1a 64 over a byte range -- a stable, dependency-free content hash. */
std::uint64_t fnv1a64(const std::uint8_t* p, std::size_t n) noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace

plugins::HevcPtsSummary summarize_hevc_pts(core::ByteSpan access_unit) {
    plugins::HevcPtsSummary s;
    if (access_unit.data() == nullptr || access_unit.size() < 4) return s;

    for_each_nalu(access_unit, [&](const NalSpan& n) {
        const core::ByteSpan span{n.data, n.size};
        if (n.type == kSpsType && !s.has_sps) {
            s.has_sps                 = true;
            const auto rbsp           = nalu_to_rbsp(span);
            s.sps_hash                = fnv1a64(rbsp.data(), rbsp.size());
            // Parse from the RBSP too: `span` still carries the NAL header
            // so `parse_sps` can recognise the unit, while `rbsp` is the
            // payload with the two header bytes already removed.
            SpsInfo info;
            if (parse_sps_body(rbsp, info)) {
                s.sps_width               = visible_width(info);
                s.sps_height              = visible_height(info);
                s.profile_idc             = info.ptl.general_profile_idc;
                s.has_compatible_profiles = info.ptl.has_compatible_profiles;
            }
        } else if (n.type == kPpsType && !s.has_pps) {
            s.has_pps        = true;
            const auto rbsp  = nalu_to_rbsp(span);
            s.pps_hash       = fnv1a64(rbsp.data(), rbsp.size());
        }
        // Stop once both sets are summarised: the slices that follow carry
        // nothing the summary needs.
        return !(s.has_sps && s.has_pps);
    });
    return s;
}

} // namespace nimrtc::nvcodec::hevc_detail
