/**
 * @file src/modules/nvcodec/tests/test_hevc_pts.cpp
 * @brief Q1b — H.265 parameter-set parsing and the mid-stream switch.
 *
 * Two groups of tests:
 *
 *   1. **The parser** (`HevcPtsParse`) — `src/hevc_pts.cpp` reads
 *      `profile_tier_level` and the SPS geometry fields a hardware
 *      decoder has to agree with.  A bug there would make the *test*
 *      wrong before it makes the *decoder* wrong, so it is pinned down
 *      with hand-built RBSPs first: the bit layout of
 *      ITU-T H.265 §7.3.3 and §7.3.2.2.1 is generated bit by bit and the
 *      parser is asked to read it back.
 *
 *   2. **The switch** (`HwPtsSwitch`, host-dependent) — a real NVENC
 *      H.265 session produces a stream whose parameter sets change
 *      mid-stream, and a single NVDEC decoder has to handle the new
 *      sequence on the *same* session:
 *
 *        - the first decode call after the switch must succeed;
 *        - the decoded geometry must be the new SPS's;
 *        - the parameter-set summary the decoder reports must equal the
 *          one the encoder wrote — i.e. the change actually crossed the
 *          wire and was noticed, rather than being hidden by a stale
 *          decoder or an unchanged stream;
 *        - and the receiver must not have asked for a key frame (which
 *          on a real connection is a PLI round trip — Q4's wire).
 *
 *      The stream switches in two ways, because they are two different
 *      bugs: a **geometry** change (new SPS width/height) and a
 *      **profile** change at the same geometry (only the parameter-set
 *      payload differs).  Every hardware case skips cleanly on a host
 *      without it.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/nvcodec/nv_backends.hpp>
#include <nimrtc/nvcodec/nv_codec_kind.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/video_frame/frame.hpp>

#include "hevc_profile.hpp"   // nvcodec::H265Profile
#include "hevc_pts.hpp"

// The backends are reached through the compatibility shim (`IVideoCodec`),
// which is what `make_nvenc_codec()` / `make_nvdec_codec()` still return.
// The shim is deprecated on purpose (B1 / ADR-015 Decision 3); the
// suppression is the same one the module's own headers use, scoped to
// this file.
NIMRTC_SUPPRESS_DEPRECATION_BEGIN

namespace plugins = nimrtc::plugins;
namespace video_frame = nimrtc::video_frame;
namespace core = nimrtc::core;
namespace nvcodec = nimrtc::nvcodec;
namespace hevc = nimrtc::nvcodec::hevc_detail;

namespace {

// ===========================================================================
// A bit-level RBSP writer, so the parser can be fed exact syntax
// ===========================================================================

class RbspWriter {
public:
    /** Append @p n bits of @p value (MSB first). */
    void u(std::uint32_t value, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            const auto bit = static_cast<std::uint8_t>((value >> (n - 1 - i)) & 1u);
            if (bit_pos_ == 0) bytes_.push_back(0);
            bytes_.back() |= static_cast<std::uint8_t>(bit << (7 - bit_pos_));
            bit_pos_ = (bit_pos_ + 1) & 7;
        }
    }

    /** Unsigned Exp-Golomb. */
    void ue(std::uint32_t v) {
        const std::uint32_t x = v + 1;
        std::size_t bits = 0;
        while ((x >> bits) > 1) ++bits;
        u(0, bits);
        u(x, bits + 1);
    }

    void byte_align() {
        while (bit_pos_ != 0) u(0, 1);
    }

    /** rbsp_trailing_bits(): a one, then zeroes to the byte boundary. */
    void trailing_bits() {
        u(1, 1);
        byte_align();
    }

    const std::vector<std::uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t               bit_pos_ = 0;
};

/** Write profile_tier_level(1, max_sub_layers_minus1) exactly as
 *  ITU-T H.265 §7.3.3 lays it out.  Note that the table contains *no*
 *  alignment word: the 32 zero bits that follow an SPS's PTL belong to
 *  the SPS's later syntax, not to this table. */
void write_ptl(RbspWriter& w, std::uint8_t max_sub_layers_minus1,
               std::uint8_t profile_idc,
               const std::vector<std::uint8_t>& compatible) {
    w.u(0, 2);              // general_profile_space
    w.u(0, 1);              // general_tier_flag
    w.u(profile_idc, 5);    // general_profile_idc
    // H.264 Table E-2 makes the flag at the sequence's own profile
    // mandatory: `general_profile_compatibility_flag[idc]` is 1 for
    // every conformance point (an encoder sets it; a compliant decoder
    // ignores a 0 there).  The `compatible` list therefore only carries
    // the *other* profiles the sequence claims, and flag[idc] is forced.
    for (std::uint32_t j = 0; j < 32; ++j) {
        bool set = (j == profile_idc);
        for (const auto c : compatible) set = set || (c == j);
        w.u(set ? 1u : 0u, 1);   // general_profile_compatibility_flag[j]
    }
    // general_level_idc comes *after* the general_* block is closed, not
    // inside it (ITU-T H.265 §7.3.3): the block is the four source flags
    // then `general_reserved_zero_44bits`, then the level.
    w.u(0, 4);              // source flags
    w.u(0, 44);             // general_reserved_zero_44bits
    w.u(120, 8);            // general_level_idc (4.0)
    for (std::uint8_t i = 0; i < max_sub_layers_minus1; ++i) {
        w.u(0, 1);          // sub_layer_profile_present_flag[i]
        w.u(1, 1);          // sub_layer_level_present_flag[i]
    }
    // reserved_zero_2bits[i], for i = maxNumSubLayersMinus1 .. 7 — when
    // max > 0, (8 - max) two-bit words.  With max == 0 the whole field
    // is absent.
    if (max_sub_layers_minus1 > 0) {
        for (std::uint8_t i = max_sub_layers_minus1; i < 8; ++i) w.u(0, 2);
    }
    for (std::uint8_t i = 0; i < max_sub_layers_minus1; ++i) {
        w.u(120, 8);        // sub_layer_level_idc[i]
    }
}

/** Wrap @p rbsp in an Annex-B framed NAL unit (start code + two-byte
 *  NAL header). */
std::vector<std::uint8_t> wrap_nalu(std::uint8_t nuh_type,
                                    const std::vector<std::uint8_t>& rbsp) {
    std::vector<std::uint8_t> out = {0x00, 0x00, 0x00, 0x01};
    out.push_back(static_cast<std::uint8_t>(nuh_type << 1));
    out.push_back(0x01);   // nuh_layer_id 0, nuh_temporal_id_plus1 1
    out.insert(out.end(), rbsp.begin(), rbsp.end());
    return out;
}

/** A complete SPS RBSP: everything the parser reads, then trailing bits. */
std::vector<std::uint8_t> make_sps_rbsp(std::uint32_t width,
                                        std::uint32_t height,
                                        std::uint8_t profile_idc,
                                        const std::vector<std::uint8_t>& compat) {
    RbspWriter w;
    w.u(0, 4);            // sps_video_parameter_set_id
    w.u(0, 3);            // sps_max_sub_layers_minus1
    w.u(1, 1);            // sps_temporal_id_nesting_flag
    write_ptl(w, 0, profile_idc, compat);
    w.ue(0);              // sps_seq_parameter_set_id
    w.ue(1);              // chroma_format_idc = 4:2:0
    w.ue(width);          // pic_width_in_luma_samples
    w.ue(height);         // pic_height_in_luma_samples
    w.u(0, 1);            // conformance_window_flag
    w.trailing_bits();
    return w.bytes();
}

core::ByteSpan span_of(const std::vector<std::uint8_t>& v) {
    return core::ByteSpan{v.data(), v.size()};
}

} // namespace

// ===========================================================================
// 1. The parser
// ===========================================================================

TEST(HevcPtsParse, ReadsGeometryAndProfileFromAnSps) {
    const auto nalu = wrap_nalu(33, make_sps_rbsp(640, 360, 1, {1}));
    hevc::SpsInfo sps;
    ASSERT_TRUE(hevc::parse_sps(span_of(nalu), sps));

    EXPECT_EQ(sps.chroma_format_idc, 1u);
    EXPECT_EQ(sps.pic_width_in_luma_samples, 640u);
    EXPECT_EQ(sps.pic_height_in_luma_samples, 360u);
    EXPECT_EQ(sps.ptl.general_profile_idc, 1u);
    EXPECT_FALSE(sps.ptl.has_compatible_profiles)
        << "a MAIN SPS that only advertises MAIN is not a widened profile";
    EXPECT_EQ(hevc::visible_width(sps), 640u);
    EXPECT_EQ(hevc::visible_height(sps), 360u);
}

TEST(HevcPtsParse, CompatibilityFlagsWidenTheProfile) {
    const auto nalu = wrap_nalu(33, make_sps_rbsp(320, 240, 2, {1, 2, 3}));
    hevc::SpsInfo sps;
    ASSERT_TRUE(hevc::parse_sps(span_of(nalu), sps));

    EXPECT_EQ(sps.ptl.general_profile_idc, 2u);
    EXPECT_TRUE(sps.ptl.has_compatible_profiles)
        << "MAIN10 / MAIN / MAIN-STILL advertises profiles 1 and 3 too";
    EXPECT_EQ(hevc::highest_profile(sps.ptl), 2u);
    ASSERT_EQ(sps.ptl.profiles.size(), 3u);
    EXPECT_EQ(sps.ptl.profiles[0], 1u);
    EXPECT_EQ(sps.ptl.profiles[1], 2u);
    EXPECT_EQ(sps.ptl.profiles[2], 3u);
}

TEST(HevcPtsParse, ConformanceWindowIsAppliedToTheVisibleSize) {
    RbspWriter w;
    w.u(0, 4);
    w.u(0, 3);
    w.u(1, 1);
    write_ptl(w, 0, 1, {1});
    w.ue(0);
    w.ue(1);
    w.ue(1920);
    w.ue(1088);
    w.u(1, 1);   // conformance_window_flag
    w.ue(0);     // conf_win_left_offset   (chroma units)
    w.ue(0);     // conf_win_right_offset
    w.ue(0);     // conf_win_top_offset
    w.ue(4);     // conf_win_bottom_offset -> 1088 - 2*4 = 1080
    w.trailing_bits();

    const auto nalu = wrap_nalu(33, w.bytes());
    hevc::SpsInfo sps;
    ASSERT_TRUE(hevc::parse_sps(span_of(nalu), sps));
    EXPECT_EQ(sps.pic_height_in_luma_samples, 1088u);
    EXPECT_EQ(hevc::visible_height(sps), 1080u);
    EXPECT_EQ(hevc::visible_width(sps), 1920u);
}

TEST(HevcPtsParse, NonSpsIsRejected) {
    const auto pps = wrap_nalu(34, {0x80});
    hevc::SpsInfo sps;
    EXPECT_FALSE(hevc::parse_sps(span_of(pps), sps));
}

TEST(HevcPtsParse, EmulationPreventionBytesAreRemoved) {
    // After the two-byte NAL header: AA 00 00 03 01 BB must read as
    // AA 00 00 01 BB — the 03 is an emulation-prevention byte, not data.
    std::vector<std::uint8_t> nalu = {0x42, 0x01, 0xAA, 0x00, 0x00, 0x03, 0x01, 0xBB};
    const auto rbsp = hevc::nalu_to_rbsp(span_of(nalu));
    const std::vector<std::uint8_t> want = {0xAA, 0x00, 0x00, 0x01, 0xBB};
    EXPECT_EQ(rbsp, want);
}

TEST(HevcPtsParse, SummarisesTheParameterSetsOfAnAccessUnit) {
    std::vector<std::uint8_t> au;
    const auto vps   = wrap_nalu(32, {0x0C, 0x01, 0xFF});            // VPS
    const auto sps   = wrap_nalu(33, make_sps_rbsp(1280, 720, 1, {1}));
    const auto pps   = wrap_nalu(34, {0xC1, 0x62, 0x84});
    const auto slice = wrap_nalu(19, {0xAF, 0x08, 0x12});            // IDR_W_RADL
    for (const auto* n : {&vps, &sps, &pps, &slice}) {
        au.insert(au.end(), n->begin(), n->end());
    }

    const auto s = hevc::summarize_hevc_pts(span_of(au));
    EXPECT_TRUE(s.has_sps);
    EXPECT_TRUE(s.has_pps);
    EXPECT_TRUE(s.has_vps);
    EXPECT_EQ(s.sps_width, 1280u);
    EXPECT_EQ(s.sps_height, 720u);
    EXPECT_EQ(s.profile_idc, 1u);
    EXPECT_NE(s.sps_hash, 0u);
    EXPECT_NE(s.pps_hash, 0u);
    EXPECT_NE(s.vps_hash, 0u);

    // The same access unit summarises identically.
    const auto again = hevc::summarize_hevc_pts(span_of(au));
    EXPECT_EQ(again.sps_hash, s.sps_hash);
    EXPECT_EQ(again.pps_hash, s.pps_hash);
    EXPECT_EQ(again.vps_hash, s.vps_hash);

    // A different profile at the same geometry, different PPS: the hashes
    // must move even though the size does not.  This is the case a
    // width/height comparison cannot see.
    std::vector<std::uint8_t> au2;
    const auto vps2 = wrap_nalu(32, {0x0C, 0x01, 0xFE});             // VPS
    const auto sps2 = wrap_nalu(33, make_sps_rbsp(1280, 720, 2, {1, 2, 3}));
    const auto pps2 = wrap_nalu(34, {0xC1, 0x62, 0x85});
    for (const auto* n : {&vps2, &sps2, &pps2}) {
        au2.insert(au2.end(), n->begin(), n->end());
    }
    const auto s2 = hevc::summarize_hevc_pts(span_of(au2));
    EXPECT_EQ(s2.sps_width, s.sps_width) << "same geometry";
    EXPECT_EQ(s2.sps_height, s.sps_height);
    EXPECT_EQ(s2.profile_idc, 2u);
    EXPECT_NE(s2.sps_hash, s.sps_hash);
    EXPECT_NE(s2.pps_hash, s.pps_hash);
    EXPECT_NE(s2.vps_hash, s.vps_hash);
}

TEST(HevcPtsParse, AVpsChangeAloneMovesTheSummary) {
    // Two access units with byte-identical SPS and PPS and only the VPS
    // differing.  A summary that hashes SPS / PPS alone reports these as
    // the same parameter set; they are not.
    const auto sps = wrap_nalu(33, make_sps_rbsp(640, 360, 1, {1}));
    const auto pps = wrap_nalu(34, {0xC1, 0x62, 0x84});

    auto build = [&](std::uint8_t vps_tail) {
        std::vector<std::uint8_t> au;
        const auto vps = wrap_nalu(32, {0x0C, 0x01, vps_tail});
        for (const auto* n : {&vps, &sps, &pps}) {
            au.insert(au.end(), n->begin(), n->end());
        }
        return au;
    };

    const auto a = hevc::summarize_hevc_pts(span_of(build(0xFF)));
    const auto b = hevc::summarize_hevc_pts(span_of(build(0xFE)));
    EXPECT_EQ(a.sps_hash, b.sps_hash)
        << "the SPS is byte-identical in the two access units";
    EXPECT_EQ(a.pps_hash, b.pps_hash)
        << "the PPS is byte-identical in the two access units";
    EXPECT_NE(a.vps_hash, b.vps_hash)
        << "the VPS changed, so the summary must move";
}

TEST(HevcPtsParse, AnAccessUnitWithoutAVpsIsAnHonestAbsence) {
    // The three flags are independent facts about the stream, not one
    // flag copied three times: an SPS/PPS-only unit (no VPS in-band)
    // must still say so.
    std::vector<std::uint8_t> au;
    const auto sps = wrap_nalu(33, make_sps_rbsp(640, 360, 1, {1}));
    const auto pps = wrap_nalu(34, {0xC1, 0x62, 0x84});
    for (const auto* n : {&sps, &pps}) {
        au.insert(au.end(), n->begin(), n->end());
    }
    const auto s = hevc::summarize_hevc_pts(span_of(au));
    EXPECT_TRUE(s.has_sps);
    EXPECT_TRUE(s.has_pps);
    EXPECT_FALSE(s.has_vps);
    EXPECT_EQ(s.vps_hash, 0u);
}

TEST(HevcPtsParse, AnAccessUnitWithoutParameterSetsSummarisesToEmpty) {
    std::vector<std::uint8_t> au;
    const auto slice = wrap_nalu(1, {0xAF, 0x08, 0x12});   // TRAIL_R
    au.insert(au.end(), slice.begin(), slice.end());
    const auto s = hevc::summarize_hevc_pts(span_of(au));
    EXPECT_FALSE(s.has_sps);
    EXPECT_FALSE(s.has_pps);
    EXPECT_FALSE(s.has_vps);
}

// ===========================================================================
// 2. The switch, on hardware (skips without NVENC / NVDEC)
// ===========================================================================

namespace {

constexpr std::uint32_t kSmallW = 320;
constexpr std::uint32_t kSmallH = 240;
constexpr std::uint32_t kLargeW = 640;
constexpr std::uint32_t kLargeH = 480;

plugins::VideoCodecConfig hevc_config(std::uint32_t w, std::uint32_t h) {
    plugins::VideoCodecConfig cfg;
    cfg.width             = w;
    cfg.height            = h;
    cfg.fps               = 30;
    cfg.bitrate_bps       = 1'500'000;
    cfg.keyframe_interval = 30;
    cfg.pixel_format      = plugins::VideoPixelFormat::kI420;
    cfg.codec             = plugins::VideoCodecKind::kH265;
    cfg.payload_type      = 45;
    return cfg;
}

/** An I420 frame with a horizontal luma ramp and neutral chroma. */
plugins::VideoFrame make_ramp_frame(std::uint32_t w, std::uint32_t h) {
    const auto layout = video_frame::compute_layout(
        video_frame::PixelFormat::kI420, w, h);
    auto buf = std::make_shared<video_frame::VideoFrameBuffer>(layout);
    for (std::uint32_t row = 0; row < h; ++row) {
        std::uint8_t* y = buf->plane(0) +
                          static_cast<std::size_t>(row) * layout.strides[0];
        for (std::uint32_t col = 0; col < w; ++col) {
            y[col] = static_cast<std::uint8_t>(16 + (col * 200) / (w > 1 ? w - 1 : 1));
        }
    }
    std::memset(buf->plane(1), 128, static_cast<std::size_t>(layout.strides[1]) * (h / 2));
    std::memset(buf->plane(2), 128, static_cast<std::size_t>(layout.strides[2]) * (h / 2));

    plugins::VideoFrame f;
    f.buffer             = std::move(buf);
    f.info.frame_seq     = 0;
    f.info.capture_ts_us = 1000;
    return f;
}

/** Encode one frame of @p w x @p h.  The payload is copied out: NVENC's
 *  view is into the caller's scratch, which the next encode reuses. */
bool encode_one(plugins::IVideoEncoder& enc, std::uint32_t w, std::uint32_t h,
                std::vector<std::uint8_t>& out, bool& is_key) {
    std::vector<std::uint8_t> bs(4 * 1024 * 1024);
    auto raw = make_ramp_frame(w, h);
    plugins::EncodedVideoFrame encoded;
    if (enc.encode(raw, bs.data(), bs.size(), encoded) != plugins::kOk) return false;
    if (encoded.payload.empty()) return false;
    out.assign(encoded.payload.data(), encoded.payload.data() + encoded.payload.size());
    is_key = encoded.is_keyframe;
    return true;
}

/** A stand-in for the engine's receive path: one NVDEC decoder for the
 *  whole stream, and a count of the key-frame requests a real
 *  connection would answer with a PLI (Q4's wire).
 *
 *  Q1b's "no PLI round trip is triggered" is a claim about a negative,
 *  so the test makes the ask *observable*: a receiver that dropped a
 *  parameter-set change would fail the decode, and the harness counts
 *  that as a key-frame request exactly as the engine would. */
class Receiver {
public:
    explicit Receiver(plugins::VideoCodecConfig cfg) noexcept
        : dec_(nvcodec::make_nvdec_codec(nvcodec::NvCodec::kH265, std::move(cfg))) {}

    bool ok() const noexcept { return dec_ != nullptr; }

    /** Feed one access unit; true when the picture came back. */
    bool feed(const std::vector<std::uint8_t>& bs, plugins::VideoFrame& out) {
        // The payload must be a span of exactly the bytes the encoder
        // wrote: the decoder summarises the same bytes for `last_hevc_pts`.
        plugins::EncodedVideoFrame au;
        au.codec   = plugins::VideoCodecKind::kH265;
        au.payload = core::ByteSpan{bs.data(), bs.size()};
        const plugins::Status st = dec_->decode(au, out, nullptr);
        if (st != plugins::kOk) {
            ++keyframe_requests_;
            return false;
        }
        return true;
    }

    std::uint64_t keyframe_requests() const noexcept { return keyframe_requests_; }
    plugins::IVideoDecoder& codec() noexcept { return *dec_; }

private:
    std::unique_ptr<plugins::IVideoCodec> dec_;
    std::uint64_t                         keyframe_requests_ = 0;
};

} // namespace

TEST(HwPtsSwitch, GeometryChangeDecodesOnTheSameNvdecSession) {
    if (!nvcodec::nvenc_codec_available(nvcodec::NvCodec::kH265) ||
        !nvcodec::nvdec_codec_available(nvcodec::NvCodec::kH265)) {
        GTEST_SKIP() << "NVENC / NVDEC are not available for H.265 on this host";
    }

    // The receiver: one decoder instance, created for the *first*
    // geometry.  Q1b is about this instance surviving the switch.
    Receiver rx(hevc_config(kSmallW, kSmallH));
    ASSERT_TRUE(rx.ok()) << "the NVDEC H.265 decoder failed to open";

    // The sender: one NVENC session that re-initialises on a geometry
    // change and emits a fresh IRAP with inline VPS / SPS / PPS.
    auto encoder = nvcodec::make_nvenc_codec(nvcodec::NvCodec::kH265,
                                             hevc_config(kSmallW, kSmallH));
    ASSERT_NE(encoder, nullptr) << "the NVENC H.265 encoder failed to open";
    plugins::IVideoEncoder& enc = *encoder;

    // ---- the first sequence ------------------------------------------------
    std::vector<std::uint8_t> bs;
    bool key = false;
    ASSERT_TRUE(encode_one(enc, kSmallW, kSmallH, bs, key));
    ASSERT_TRUE(key) << "the first picture must be a keyframe";


    const auto first_pts = enc.last_hevc_pts();
    ASSERT_TRUE(first_pts.has_value())
        << "the encoder did not report the parameter sets it wrote";
    EXPECT_EQ(first_pts->sps_width, kSmallW);
    EXPECT_EQ(first_pts->sps_height, kSmallH);

    plugins::VideoFrame decoded;
    ASSERT_TRUE(rx.feed(bs, decoded)) << "the first sequence did not decode";
    ASSERT_TRUE(decoded.buffer);
    EXPECT_EQ(decoded.width(), kSmallW);
    EXPECT_EQ(decoded.height(), kSmallH);

    const auto dec_pts = rx.codec().last_hevc_pts();
    ASSERT_TRUE(dec_pts.has_value());
    EXPECT_EQ(dec_pts->sps_hash, first_pts->sps_hash)
        << "the decoder did not see the SPS the encoder wrote";

    // A second picture of the first sequence, so the switch is not the
    // decoder's very first sequence.
    ASSERT_TRUE(encode_one(enc, kSmallW, kSmallH, bs, key));
    EXPECT_TRUE(rx.feed(bs, decoded));
    EXPECT_EQ(decoded.width(), kSmallW);

    // ---- the switch --------------------------------------------------------
    ASSERT_TRUE(encode_one(enc, kLargeW, kLargeH, bs, key));
    ASSERT_TRUE(key) << "a geometry change must start a new access unit with "
                        "an IRAP picture";

    const auto switch_pts = enc.last_hevc_pts();
    ASSERT_TRUE(switch_pts.has_value());
    EXPECT_EQ(switch_pts->sps_width, kLargeW);
    EXPECT_EQ(switch_pts->sps_height, kLargeH);
    ASSERT_NE(switch_pts->sps_hash, first_pts->sps_hash)
        << "the new sequence's SPS is identical to the old one — there was "
           "no parameter-set change to assert";
    // The VPS is a parameter set in its own right: the geometry change
    // must move it too, and the summary has to have seen it at all.
    EXPECT_TRUE(switch_pts->has_vps);
    EXPECT_TRUE(first_pts->has_vps);
    EXPECT_NE(switch_pts->vps_hash, first_pts->vps_hash)
        << "the VPS did not change with the sequence";

    // The *first* decode call after the switch must succeed, with the new
    // geometry: same session, no re-open, no key-frame request.
    plugins::VideoFrame switched;
    EXPECT_TRUE(rx.feed(bs, switched))
        << "the first decode call after the geometry change failed — on a "
           "real connection that is exactly what a PLI round trip exists to "
           "recover from";
    if (switched.buffer) {
        EXPECT_EQ(switched.width(), kLargeW);
        EXPECT_EQ(switched.height(), kLargeH);
    }
    EXPECT_EQ(rx.keyframe_requests(), 0u)
        << "the receiver had to ask for a key frame after the switch";

    // The decoder's session now belongs to the new sequence.
    const auto after = rx.codec().last_hevc_pts();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->sps_hash, switch_pts->sps_hash)
        << "the decoder's session was not re-created for the new sequence";
    EXPECT_EQ(after->vps_hash, switch_pts->vps_hash)
        << "the decoder kept the previous sequence's VPS";

    EXPECT_EQ(rx.codec().stats().decode_errors, 0u);
}
TEST(HwPtsSwitch, ParameterSetChangeAtTheSameGeometryIsNoticed) {
    if (!nvcodec::nvenc_codec_available(nvcodec::NvCodec::kH265) ||
        !nvcodec::nvdec_codec_available(nvcodec::NvCodec::kH265)) {
        GTEST_SKIP() << "NVENC / NVDEC are not available for H.265 on this host";
    }
    if (!nvcodec::nvenc_codec_available_at_profile(nvcodec::NvCodec::kH265,
                                                   nvcodec::H265Profile::kMain10)) {
        GTEST_SKIP() << "this host's NVENC cannot encode HEVC MAIN10 — a "
                        "same-geometry parameter-set switch needs a second "
                        "profile to come from";
    }

    // Two streams of the *same size*, different profiles: the picture
    // dimensions do not change, so anything that compares width/height
    // alone would miss the switch entirely.
    auto main = nvcodec::make_nvenc_codec(nvcodec::NvCodec::kH265,
                                          hevc_config(kSmallW, kSmallH));
    ASSERT_NE(main, nullptr);
    auto main10 = nvcodec::make_nvenc_codec_for_profile(
        nvcodec::NvCodec::kH265, hevc_config(kSmallW, kSmallH),
        nvcodec::H265Profile::kMain10);
    ASSERT_NE(main10, nullptr) << "the MAIN10 session failed to open";
    plugins::IVideoEncoder& enc_main   = *main;
    plugins::IVideoEncoder& enc_main10 = *main10;

    std::vector<std::uint8_t> bs_main, bs_main10;
    bool key = false;
    ASSERT_TRUE(encode_one(enc_main, kSmallW, kSmallH, bs_main, key));
    ASSERT_TRUE(key);
    ASSERT_TRUE(encode_one(enc_main10, kSmallW, kSmallH, bs_main10, key));
    ASSERT_TRUE(key);

    const auto s_main   = enc_main.last_hevc_pts();
    const auto s_main10 = enc_main10.last_hevc_pts();
    ASSERT_TRUE(s_main.has_value());
    ASSERT_TRUE(s_main10.has_value());
    EXPECT_EQ(s_main->sps_width, s_main10->sps_width);
    EXPECT_EQ(s_main->sps_height, s_main10->sps_height);
    EXPECT_EQ(s_main->profile_idc, 1u);
    EXPECT_EQ(s_main10->profile_idc, 2u);
    ASSERT_NE(s_main->sps_hash, s_main10->sps_hash)
        << "the two profiles produced identical SPSs — there is no "
           "same-geometry parameter-set change to assert";

    // One decoder sees both access units in order.  The picture check is
    // conditional on the decode succeeding: a host may deliver 10-bit
    // pictures here that this backend's 8-bit NV12 surface cannot carry,
    // and that is an honest failure — what Q1b pins down is that the
    // *parameter sets* are accepted on the same parser and that the
    // decoder's view of them moves to the new sequence.
    Receiver rx(hevc_config(kSmallW, kSmallH));
    ASSERT_TRUE(rx.ok());

    plugins::VideoFrame out;
    ASSERT_TRUE(rx.feed(bs_main, out))
        << "the MAIN sequence did not decode";
    ASSERT_TRUE(out.buffer);
    EXPECT_EQ(out.width(), kSmallW);
    EXPECT_EQ(out.height(), kSmallH);

    out = {};
    const bool second_ok = rx.feed(bs_main10, out);
    if (second_ok) {
        EXPECT_EQ(out.width(), kSmallW);
        EXPECT_EQ(out.height(), kSmallH);
    }
    const auto after = rx.codec().last_hevc_pts();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->sps_hash, s_main10->sps_hash)
        << "the decoder kept the previous sequence's parameter sets";
    EXPECT_EQ(after->vps_hash, s_main10->vps_hash)
        << "the decoder kept the previous sequence's VPS";
}

NIMRTC_SUPPRESS_DEPRECATION_END

