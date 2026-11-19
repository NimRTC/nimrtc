/**
 * @file src/modules/nvcodec/src/hevc_pts.hpp
 * @brief Read-only HEVC parameter-set helpers (v1.3 plan item **Q1b**).
 *
 * ## Why this lives here
 *
 * Q1b's assertion is about an **H.265 mid-stream parameter-set change**:
 * a stream that switches geometry must be decoded by the *same* NVDEC
 * session, with the new picture's geometry, and without the receiver
 * having to ask for a key frame.  To prove that, the test has to know
 * two things the bitstream does not tell it for free:
 *
 *   1. **Which picture geometry a parameter set describes.**  NVDEC
 *      reports the geometry it *decoded*, but that is also what a stale
 *      decoder would report if it silently reused the old sequence — so
 *      the expected values must come from the bitstream the encoder
 *      produced, parsed independently of the driver.
 *   2. **Whether one access unit's parameter sets differ from the
 *      previous one's.**  A test that only compares the decoded size
 *      after a switch cannot tell "the decoder re-initialised" from "the
 *      encoder happened to emit a frame of the right size".
 *
 * Both are pure byte work over ITU-T H.265 §7.3.3 (profile_tier_level)
 * and §7.3.2.2.1 (sps_seq_parameter_set_rbsp).  Keeping them in this
 * module's *private* `src/` tree means they are not part of the stable
 * surface (ADR-015), and the test reaches them the same way
 * `test_vt_common.cpp` reaches `vt_common.hpp`.
 *
 * ## What is parsed, and what is not
 *
 * Only the fields between the NAL header and `conf_win_bottom_offset`
 * are read — enough for the picture width / height and for a
 * byte-for-byte comparison of two parameter sets' *content*.  The
 * high-level-syntax loops after that (scaling lists, short-term
 * reference picture sets, VUI) are skipped, not parsed.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/plugins/video_codec.hpp>   // plugins::HevcPtsSummary

namespace nimrtc::nvcodec::hevc_detail {

// ---------------------------------------------------------------------------
// RBSP access
// ---------------------------------------------------------------------------

/** Remove the emulation-prevention bytes (00 00 03 -> 00 00) from @p nalu.
 *
 *  The returned vector is the RBSP *without* the two-byte NAL header —
 *  i.e. the payload the syntax tables in ITU-T H.265 §7.3 describe. */
std::vector<std::uint8_t> nalu_to_rbsp(core::ByteSpan nalu);

/** A bit reader over an RBSP buffer (MSB-first, ITU-T H.265 §7.2).
 *
 *  A template on the buffer type so a caller may hand it a
 *  `std::vector` or any other buffer with `data()` and `size()`. */
template <typename Buf>
class BasicBitReader {
public:
    explicit BasicBitReader(const Buf& rbsp) noexcept
        : data_(rbsp) {}

    /** Read @p n bits, MSB-first.  @p n may exceed 32 (the syntax tables
     *  skip 44-bit fields); only the last 32 bits stay in the value, but
     *  every bit is consumed, which is what keeps `skip()` on the right
     *  boundary.  Reads past the end yield zero bits and latch
     *  `overflowed()`; callers that care check it. */
    std::uint32_t u(std::size_t n) noexcept {
        std::uint32_t v = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t byte = pos_ >> 3;
            const std::size_t bit  = 7u - (pos_ & 7u);
            std::uint32_t b = 0;
            if (byte < data_.size()) {
                b = (data_[byte] >> bit) & 1u;
            } else {
                overflowed_ = true;
            }
            v = (v << 1) | b;
            ++pos_;
        }
        return v;
    }

    /** One bit — the inner loop of `ue()` without a per-bit call. */
    std::uint32_t u1() noexcept {
        const std::size_t byte = pos_ >> 3;
        const std::size_t bit  = 7u - (pos_ & 7u);
        std::uint32_t b = 0;
        if (byte < data_.size()) {
            b = (data_[byte] >> bit) & 1u;
        } else {
            overflowed_ = true;
        }
        ++pos_;
        return b;
    }

    /** Unsigned Exp-Golomb (ue(v)). */
    std::uint32_t ue() noexcept {
        std::size_t leading = 0;
        // Count leading zero bits (bounded so a corrupt buffer cannot spin).
        while (leading < 32) {
            if (overflowed_) return 0;
            if (u1() == 1) break;
            ++leading;
        }
        if (leading == 0) return 0;
        if (leading >= 32) return 0;
        // ue(v) = (2^leading - 1) + suffix, where `suffix` is the
        // `leading`-bit number that follows the stop bit (H.265 §9.1).
        std::uint32_t rest = 0;
        for (std::size_t i = 0; i < leading; ++i) rest = (rest << 1) | u1();
        return ((1u << leading) - 1u) + rest;
    }

    /** Skip @p n bits. */
    void skip(std::size_t n) noexcept { (void)u(n); }

    /** Align to the next byte boundary. */
    void byte_align() noexcept { pos_ = (pos_ + 7u) & ~std::size_t{7u}; }

    /** True iff a read ran past the end of the buffer. */
    bool overflowed() const noexcept { return overflowed_; }

private:
    const Buf&  data_;
    std::size_t pos_        = 0;
    bool        overflowed_ = false;
};

using BitReader = BasicBitReader<std::vector<std::uint8_t>>;

// ---------------------------------------------------------------------------
// Profile / tier / level
// ---------------------------------------------------------------------------

/** The fields of profile_tier_level(1, ...) that matter to a caller: the
 *  sub-layer structure and the values of general_profile_idc that are
 *  present. */
struct ProfileTierLevel {
    std::uint8_t max_sub_layers_minus1 = 0;
    std::uint8_t general_profile_idc   = 0;
    /** general_profile_idc of each sub-layer that has one. */
    std::vector<std::uint8_t> sub_layer_profile_idc;

    /** True iff at least one *other* profile is advertised through the
     *  compatibility flags or a sub-layer's profile_idc.  This is what
     *  an NVDEC decoder session must account for: a session created for
     *  `general_profile_idc` alone cannot decode a picture that actually
     *  uses a compatible profile later in the stream (ITU-T H.265
     *  §A.3.2), so the decoder asks the driver for a wider profile when
     *  this is set. */
    bool  has_compatible_profiles = false;
    /** Every profile idc this parameter set advertises, including the
     *  primary one.  Sorted, unique. */
    std::vector<std::uint8_t> profiles;
};

/** The highest profile idc in @p ptl, or @p ptl.general_profile_idc when
 *  no compatibility flag is set.  Profiles 4/5/6 (Rext / SCC / MV-HEVC)
 *  are *not* ordered with respect to 1..3, so the caller pairs this with
 *  `has_compatible_profiles` rather than trusting the number alone. */
std::uint8_t highest_profile(const ProfileTierLevel& ptl) noexcept;

/** Parse profile_tier_level(profilePresentFlag = 1, maxNumSubLayersMinus1).
 *
 *  Returns false when the RBSP is too short; the partial result is then
 *  meaningless.  The reserved bits are skipped exactly as
 *  ITU-T H.265 §7.3.3 specifies, which is what makes the reader land on
 *  the right bit after each of the two blocks.
 *
 *  Not a template, and not a `BasicBitReader` template parameter: the
 *  definition lives in the .cpp, and a function *template* whose
 *  definition is not visible to its caller cannot be explicitly
 *  instantiated from another translation unit -- GCC rejects the
 *  instantiation outright ("explicit instantiation of ... but no
 *  definition available"), while MSVC accepts it.  That is a
 *  platform-dependent compile failure in a test, which is the worst
 *  shape a defect can have.  `BasicBitReader` stays a template (the
 *  reader itself is header-only and fully visible), but the two
 *  functions that *consume* a reader take the concrete `BitReader`
 *  alias, so no explicit instantiation is needed anywhere. */
bool parse_profile_tier_level(BitReader& r,
                              std::uint8_t max_num_sub_layers_minus1,
                              ProfileTierLevel& out) noexcept;

// ---------------------------------------------------------------------------
// Sequence parameter set
// ---------------------------------------------------------------------------

/** The geometry-relevant part of an SPS. */
struct SpsInfo {
    std::uint8_t     sps_video_parameter_set_id = 0;
    std::uint8_t     sps_max_sub_layers_minus1  = 0;
    ProfileTierLevel ptl;

    std::uint32_t sps_seq_parameter_set_id = 0;
    std::uint32_t chroma_format_idc        = 1;

    /** pic_width_in_luma_samples / pic_height_in_luma_samples. */
    std::uint32_t pic_width_in_luma_samples  = 0;
    std::uint32_t pic_height_in_luma_samples = 0;

    /** conformance_window_flag and its four offsets. */
    bool          conformance_window_flag = false;
    std::uint32_t conf_win_left_offset    = 0;
    std::uint32_t conf_win_right_offset   = 0;
    std::uint32_t conf_win_top_offset     = 0;
    std::uint32_t conf_win_bottom_offset  = 0;
};

/** Parse an SPS NAL unit.  @p sps_nalu may be either a bare NAL unit
 *  (two-byte NAL header first — what `NALUnit::data()` returns) or a
 *  complete Annex-B NAL unit with a 3- or 4-byte start code.
 *
 *  @return true when the fields up to `conf_win_bottom_offset` were read;
 *          false when the input is not an SPS NAL unit or is truncated. */
bool parse_sps(core::ByteSpan sps_nalu, SpsInfo& out) noexcept;

/** Parse an SPS from its RBSP — the NAL payload *without* the two-byte
 *  NAL header and with emulation-prevention bytes already removed.
 *  `summarize_hevc_pts` uses this: it already built the RBSP for the
 *  hash and must parse exactly the bytes it hashed, not a re-stripped
 *  view. */
bool parse_sps_body(core::ByteSpan rbsp, SpsInfo& out) noexcept;

/** The *visible* picture width / height of @p sps (ITU-T H.265
 *  §7.4.3.2.1: SubWidthC / SubHeightC are 1 for monochrome and 4:4:4,
 *  2 otherwise). */
std::uint32_t visible_width(const SpsInfo& sps) noexcept;
std::uint32_t visible_height(const SpsInfo& sps) noexcept;

// ---------------------------------------------------------------------------
// One access unit -> a summary
// ---------------------------------------------------------------------------

/** Build the `plugins::HevcPtsSummary` of the first SPS and PPS an
 *  Annex-B access unit carries.
 *
 *  The access unit is walked with this file's own Annex-B scan (see
 *  `find_start_code` in the .cpp).  It is a deliberate *second opinion*
 *  on the same bytes the H.265 module's parser frames: a bug in one
 *  must not be able to hide the same bug in the other. */
plugins::HevcPtsSummary summarize_hevc_pts(core::ByteSpan access_unit);

} // namespace nimrtc::nvcodec::hevc_detail
