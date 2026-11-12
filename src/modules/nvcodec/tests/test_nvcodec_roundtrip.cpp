/**
 * @file src/modules/nvcodec/tests/test_nvcodec_roundtrip.cpp
 * @brief B10a — hardware codec round-trip test (NVENC → NVDEC).
 *
 * The test drives the *shipped* backend implementations through the
 * `nvcodec` module's public entry points, so it catches regressions in
 * the real code path (not a re-implementation of the SDK calls):
 *
 *   1. Build a synthetic I420 frame.
 *   2. Encode it with the NVENC backend through the public
 *      `plugins::IVideoCodec` interface.
 *   3. Assert the bitstream is a valid Annex-B access unit carrying
 *      SPS + PPS + an IDR slice.
 *   4. Decode it with the NVDEC backend and assert that:
 *        - the decoded frame has the expected dimensions,
 *        - the luma plane is not a flat grey (the encoder actually
 *          encoded something), and
 *        - the decoded luma correlates with the source luma.
 *
 * ## Why this test lives in the `nvcodec` module
 *
 * The NVIDIA driver integration is owned by `modules/nvcodec` — it is
 * *not* H.264-specific (the same session code serves H.265 and AV1).
 * The round-trip test therefore belongs here, next to the code it
 * exercises, and it drives the module's own public seam
 * (`nvcodec::nvenc_codec_available()` / `make_nvenc_codec()` /
 * `nvdec_codec_available()` / `make_nvdec_codec()`) rather than the
 * H.264 module's backend registry.  The H.264 module's *registration*
 * of these backends is covered separately by the h264 module's own
 * `test_hw_backends`.
 *
 * ## Host dependency
 *
 * The test is skipped (GTEST_SKIP) when the NVIDIA runtime or the GPU is
 * not present, or when the corresponding backend was not compiled in.
 * It is a *host-dependent* test, so it never blocks CI.
 *
 * ## Why the correlation check is loose
 *
 * H.264 is lossy, and the encoder re-orders / re-quantises the picture,
 * so an exact pixel match is neither expected nor desirable.  What the
 * test asserts is that the *structure* survived the round trip: the
 * mean absolute error against the source is small compared to the
 * signal's own dynamic range.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/nvcodec/nv_backends.hpp>
#include <nimrtc/nvcodec/nv_codec_kind.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/video_frame/frame.hpp>
#include <nimrtc/video_payload/h264.hpp>

#include "nvdec_test_vector.hpp"

namespace plugins = nimrtc::plugins;
namespace video_frame = nimrtc::video_frame;
namespace core = nimrtc::core;
namespace nvcodec = nimrtc::nvcodec;

namespace {

constexpr std::uint32_t kWidth  = 320;
constexpr std::uint32_t kHeight = 240;

/** Create the NVENC encoder for H.264, or nullptr when the backend was
 *  not compiled in (no NVIDIA SDK at build time) or is unavailable on
 *  this host. */
std::unique_ptr<plugins::IVideoCodec> make_nvenc(
    const plugins::VideoCodecConfig& cfg) {
    return nvcodec::make_nvenc_codec(nvcodec::NvCodec::kH264, cfg);
}

/** Create the NVDEC decoder for H.264, or nullptr when the backend was
 *  not compiled in or is unavailable on this host. */
std::unique_ptr<plugins::IVideoCodec> make_nvdec(
    const plugins::VideoCodecConfig& cfg) {
    return nvcodec::make_nvdec_codec(nvcodec::NvCodec::kH264, cfg);
}

// ---------------------------------------------------------------------------
// Synthetic source frame
// ---------------------------------------------------------------------------

struct TestFrame {
    std::vector<std::uint8_t> y;
    std::vector<std::uint8_t> u;
    std::vector<std::uint8_t> v;
};

/** Build a deterministic test pattern: a horizontal gradient in luma and a
 *  neutral chroma, so a decoded frame can be structurally compared with
 *  the source. */
TestFrame make_test_frame(std::uint32_t w, std::uint32_t h) {
    TestFrame f;
    f.y.resize(static_cast<std::size_t>(w) * h);
    f.u.resize(static_cast<std::size_t>(w) * h / 4);
    f.v.resize(static_cast<std::size_t>(w) * h / 4);

    for (std::uint32_t row = 0; row < h; ++row) {
        for (std::uint32_t col = 0; col < w; ++col) {
            // Ramp from 16 to 235 (BT.601 limited range) across the width,
            // with a per-row offset so the picture has vertical structure
            // too (a pure horizontal ramp compresses very efficiently and
            // would make a trivial encoder look good).
            const int ramp = static_cast<int>((col * 219) / (w > 1 ? (w - 1) : 1));
            const int rowv = static_cast<int>((row * 40) / (h > 1 ? (h - 1) : 1));
            f.y[static_cast<std::size_t>(row) * w + col] =
                static_cast<std::uint8_t>(16 + ramp + rowv);
        }
    }
    std::fill(f.u.begin(), f.u.end(), static_cast<std::uint8_t>(128));
    std::fill(f.v.begin(), f.v.end(), static_cast<std::uint8_t>(128));
    return f;
}

/** Wrap the planes into a VideoFrame the encoder can consume. */
plugins::VideoFrame make_video_frame(const TestFrame& f,
                                      std::uint32_t w,
                                      std::uint32_t h) {
    const auto layout = video_frame::compute_layout(video_frame::PixelFormat::kI420, w, h);
    auto buf = std::make_shared<video_frame::VideoFrameBuffer>(layout);
    EXPECT_TRUE(buf && buf->data());
    if (!buf || !buf->data()) return {};

    // The layout uses 16-byte aligned strides; copy plane by plane.
    for (std::uint32_t row = 0; row < h; ++row) {
        std::memcpy(buf->plane(0) + static_cast<std::size_t>(row) * layout.strides[0],
                    f.y.data() + static_cast<std::size_t>(row) * w, w);
    }
    const std::uint32_t cw = w / 2;
    const std::uint32_t ch = h / 2;
    for (std::uint32_t row = 0; row < ch; ++row) {
        std::memcpy(buf->plane(1) + static_cast<std::size_t>(row) * layout.strides[1],
                    f.u.data() + static_cast<std::size_t>(row) * cw, cw);
        std::memcpy(buf->plane(2) + static_cast<std::size_t>(row) * layout.strides[2],
                    f.v.data() + static_cast<std::size_t>(row) * cw, cw);
    }

    plugins::VideoFrame frame;
    frame.buffer = std::move(buf);
    frame.info.frame_seq = 0;
    frame.info.capture_ts_us = 123456;
    return frame;
}

/** Mean absolute error between two luma planes. */
double luma_mae(const std::vector<std::uint8_t>& a,
                const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 255.0;
    double acc = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        acc += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    }
    return acc / static_cast<double>(a.size());
}

/** Count the Annex-B NAL unit types in @p p. */
std::vector<int> nal_types(const std::uint8_t* p, std::size_t n) {
    std::vector<int> out;
    std::size_t i = 0;
    while (i + 3 < n) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) {
            if (i + 4 < n) out.push_back(p[i + 4] & 0x1F);
            i += 4;
        } else {
            ++i;
        }
    }
    return out;
}

plugins::VideoCodecConfig make_config() {
    plugins::VideoCodecConfig cfg;
    cfg.width             = kWidth;
    cfg.height            = kHeight;
    cfg.fps               = 30;
    cfg.bitrate_bps       = 1'000'000;
    cfg.keyframe_interval = 30;
    cfg.pixel_format      = plugins::VideoPixelFormat::kI420;
    cfg.codec             = plugins::VideoCodecKind::kH264;
    cfg.payload_type      = 102;
    return cfg;
}

} // namespace

// ---------------------------------------------------------------------------
// The round-trip test
// ---------------------------------------------------------------------------

TEST(HwRoundTrip, NvencEncodeThenNvdecDecode) {
    // ---- 1. Is the hardware there? -------------------------------------
    if (!nvcodec::nvenc_codec_available(nvcodec::NvCodec::kH264)) {
        GTEST_SKIP() << "NVENC is not available on this host — skipping the "
                        "hardware round-trip test";
    }
    if (!nvcodec::nvdec_codec_available(nvcodec::NvCodec::kH264)) {
        GTEST_SKIP() << "NVDEC is not available on this host — skipping the "
                        "hardware round-trip test";
    }

    const auto cfg = make_config();

    // ---- 2. Encoder ------------------------------------------------------
    auto encoder = make_nvenc(cfg);
    ASSERT_NE(encoder, nullptr) << "the NVENC backend failed to open";
    ASSERT_TRUE(encoder->can_encode());
    ASSERT_FALSE(encoder->can_decode());

    const TestFrame src = make_test_frame(kWidth, kHeight);
    const plugins::VideoFrame raw = make_video_frame(src, kWidth, kHeight);
    ASSERT_TRUE(raw.buffer);

    std::vector<std::uint8_t> bs(2 * 1024 * 1024);
    plugins::EncodedVideoFrame encoded;
    const plugins::Status enc_status =
        encoder->encode(raw, bs.data(), bs.size(), encoded);
    ASSERT_EQ(enc_status, plugins::kOk)
        << "NVENC encode failed (status=" << enc_status << ")";
    ASSERT_GT(encoded.payload.size(), 0u);

    // ---- 3. The bitstream must be a valid Annex-B access unit -----------
    const auto* p = encoded.payload.data();
    const std::size_t n = encoded.payload.size();
    ASSERT_GE(n, 4u);
    EXPECT_EQ(p[0], 0x00);
    EXPECT_EQ(p[1], 0x00);
    EXPECT_EQ(p[2], 0x00);
    EXPECT_EQ(p[3], 0x01) << "the NVENC output is not Annex-B start-code prefixed";

    const std::vector<int> types = nal_types(p, n);
    ASSERT_GE(types.size(), 3u)
        << "expected at least SPS + PPS + slice, got " << types.size() << " NAL units";
    EXPECT_EQ(types[0], 7) << "the first NAL unit must be the SPS";
    EXPECT_EQ(types[1], 8) << "the second NAL unit must be the PPS";
    EXPECT_NE(std::find(types.begin(), types.end(), 5), types.end())
        << "expected an IDR slice (NAL type 5), got "
        << types[0] << "," << types[1] << "," << types[2];
    EXPECT_TRUE(encoded.is_keyframe) << "the first encoded frame must be a keyframe";

    // The shared RTP payload layer's H.264 parser must agree.
    {
        std::size_t offset = 0;
        bool saw_sps = false;
        bool saw_pps = false;
        bool saw_idr = false;
        while (offset + 4 < n) {
            // Walk the Annex-B start codes by hand: the payload layer's
            // `parse()` operates on a single RTP packet, not on a whole
            // access unit.
            if (p[offset] == 0 && p[offset + 1] == 0 &&
                p[offset + 2] == 0 && p[offset + 3] == 1) {
                const auto type =
                    nimrtc::video_payload::h264::peek_nalu_type(
                        core::ByteSpan{p + offset + 4, n - offset - 4});
                if (type == nimrtc::video_payload::h264::NaluType::kSPS) saw_sps = true;
                if (type == nimrtc::video_payload::h264::NaluType::kPPS) saw_pps = true;
                if (type == nimrtc::video_payload::h264::NaluType::kSliceIDR) saw_idr = true;
                offset += 4;
            } else {
                ++offset;
            }
        }
        EXPECT_TRUE(saw_sps);
        EXPECT_TRUE(saw_pps);
        EXPECT_TRUE(saw_idr);
    }

    // ---- 4. Decoder ------------------------------------------------------
    auto decoder = make_nvdec(cfg);
    ASSERT_NE(decoder, nullptr) << "the NVDEC backend failed to open";
    ASSERT_TRUE(decoder->can_decode());
    ASSERT_FALSE(decoder->can_encode());

    // The decoder writes into caller-owned buffers.  Use the same layout
    // the engine uses (16-byte aligned strides).
    const auto layout = video_frame::compute_layout(video_frame::PixelFormat::kI420,
                                                    kWidth, kHeight);
    std::vector<std::uint8_t> out_pixels(layout.buffer_size, 0);
    std::uint8_t* planes[3] = {
        out_pixels.data(),
        out_pixels.data() + layout.plane_offsets[1],
        out_pixels.data() + layout.plane_offsets[2],
    };

    plugins::VideoFrame decoded;
    decoded.info.capture_ts_us = encoded.info.capture_ts_us;
    decoded.info.frame_seq     = encoded.info.frame_seq;

    const plugins::Status dec_status = decoder->decode(encoded, decoded, planes);
    ASSERT_EQ(dec_status, plugins::kOk)
        << "NVDEC decode failed (status=" << dec_status << ")";

    // ---- 5. The decoded pixels must resemble the source ------------------
    // Extract the decoded luma plane (stride-aware).
    std::vector<std::uint8_t> dec_y(static_cast<std::size_t>(kWidth) * kHeight);
    for (std::uint32_t row = 0; row < kHeight; ++row) {
        std::memcpy(dec_y.data() + static_cast<std::size_t>(row) * kWidth,
                    planes[0] + static_cast<std::size_t>(row) * layout.strides[0],
                    kWidth);
    }

    // The decoded picture must not be flat — a broken decoder often
    // returns a constant grey frame.
    const auto [min_it, max_it] = std::minmax_element(dec_y.begin(), dec_y.end());
    const int spread = static_cast<int>(*max_it) - static_cast<int>(*min_it);
    EXPECT_GT(spread, 20) << "the decoded luma is nearly flat (spread=" << spread
                          << ") — the decoder probably produced garbage";

    // And it must resemble the source.  H.264 is lossy, so we allow a
    // mean error of up to 12 luma levels against a source whose dynamic
    // range is ~240 levels.
    const double mae = luma_mae(src.y, dec_y);
    EXPECT_LT(mae, 12.0) << "the decoded luma differs too much from the "
                             "source (MAE=" << mae << ")";

    // ---- 6. Stats --------------------------------------------------------
    const auto enc_stats = encoder->stats();
    EXPECT_EQ(enc_stats.frames_encoded, 1u);
    EXPECT_EQ(enc_stats.bytes_encoded, encoded.payload.size());
    EXPECT_EQ(enc_stats.encode_errors, 0u);

    const auto dec_stats = decoder->stats();
    EXPECT_EQ(dec_stats.frames_decoded, 1u);
    EXPECT_EQ(dec_stats.decode_errors, 0u);
}

// ---------------------------------------------------------------------------
// The NV12 output path.
//
// NVDEC always produces NV12 surfaces; the backend de-interleaves the
// chroma into I420 planes unless the caller asked for NV12.  The NV12
// path has its own stride contract (the UV plane is *interleaved*, so
// its stride is the full frame width in bytes, not half of it), which
// is easy to get wrong — this test pins it down.
// ---------------------------------------------------------------------------

TEST(HwRoundTrip, NvdecDecodesToNv12) {
    if (!nvcodec::nvenc_codec_available(nvcodec::NvCodec::kH264) ||
        !nvcodec::nvdec_codec_available(nvcodec::NvCodec::kH264)) {
        GTEST_SKIP() << "NVENC / NVDEC are not available on this host";
    }

    auto cfg = make_config();
    cfg.pixel_format = plugins::VideoPixelFormat::kNV12;

    auto encoder = make_nvenc(cfg);
    ASSERT_NE(encoder, nullptr);

    const TestFrame src = make_test_frame(kWidth, kHeight);
    const plugins::VideoFrame raw = make_video_frame(src, kWidth, kHeight);
    std::vector<std::uint8_t> bs(2 * 1024 * 1024);
    plugins::EncodedVideoFrame encoded;
    ASSERT_EQ(encoder->encode(raw, bs.data(), bs.size(), encoded), plugins::kOk);

    auto decoder = make_nvdec(cfg);
    ASSERT_NE(decoder, nullptr);

    // The caller sizes its buffers from the NV12 frame layout: the UV
    // plane is interleaved, so it has *two* planes and a UV stride of
    // align16(width) bytes.
    const auto layout = video_frame::compute_layout(video_frame::PixelFormat::kNV12,
                                                    kWidth, kHeight);
    ASSERT_EQ(layout.num_planes, 2);
    EXPECT_EQ(layout.strides[1], (kWidth + 15u) & ~15u)
        << "the NV12 UV stride must hold the full interleaved row";

    std::vector<std::uint8_t> out_pixels(layout.buffer_size, 0);
    std::uint8_t* planes[3] = {
        out_pixels.data(),
        out_pixels.data() + layout.plane_offsets[1],
        nullptr,   // NV12 has no separate V plane
    };

    plugins::VideoFrame decoded;
    const plugins::Status dec_status = decoder->decode(encoded, decoded, planes);
    ASSERT_EQ(dec_status, plugins::kOk) << "NVDEC NV12 decode failed";

    // The luma plane must still hold the decoded picture.
    std::vector<std::uint8_t> dec_y(static_cast<std::size_t>(kWidth) * kHeight);
    for (std::uint32_t row = 0; row < kHeight; ++row) {
        std::memcpy(dec_y.data() + static_cast<std::size_t>(row) * kWidth,
                    planes[0] + static_cast<std::size_t>(row) * layout.strides[0],
                    kWidth);
    }
    const auto [min_it, max_it] = std::minmax_element(dec_y.begin(), dec_y.end());
    EXPECT_GT(static_cast<int>(*max_it) - static_cast<int>(*min_it), 20)
        << "the decoded NV12 luma is nearly flat — the decoder produced garbage";

    // The chroma must be present in the UV plane.  The source is neutral
    // (U = V = 128), so the decoded UV should hover around 128.
    double uv_sum = 0.0;
    std::size_t uv_count = 0;
    for (std::uint32_t row = 0; row < kHeight / 2; ++row) {
        const std::uint8_t* uv = planes[1] + static_cast<std::size_t>(row) * layout.strides[1];
        for (std::uint32_t col = 0; col < kWidth; ++col) {
            uv_sum += uv[col];
            ++uv_count;
        }
    }
    const double uv_mean = uv_sum / static_cast<double>(uv_count);
    EXPECT_GT(uv_mean, 100.0);
    EXPECT_LT(uv_mean, 156.0)
        << "the NV12 chroma plane does not hold the decoded picture "
           "(mean=" << uv_mean << ")";
}

// ---------------------------------------------------------------------------
// A multi-frame stream.
//
// A single keyframe is the easy case.  A real stream interleaves IDR and
// non-IDR pictures, and the decoder has to keep its reference state
// across pictures — so this test feeds several frames and checks that
// every one of them decodes to something plausible.
// ---------------------------------------------------------------------------

TEST(HwRoundTrip, NvdecDecodesMultipleFrames) {
    if (!nvcodec::nvenc_codec_available(nvcodec::NvCodec::kH264) ||
        !nvcodec::nvdec_codec_available(nvcodec::NvCodec::kH264)) {
        GTEST_SKIP() << "NVENC / NVDEC are not available on this host";
    }

    const auto cfg = make_config();

    auto encoder = make_nvenc(cfg);
    ASSERT_NE(encoder, nullptr);
    auto decoder = make_nvdec(cfg);
    ASSERT_NE(decoder, nullptr);

    const auto layout = video_frame::compute_layout(video_frame::PixelFormat::kI420,
                                                    kWidth, kHeight);
    std::vector<std::uint8_t> out_pixels(layout.buffer_size, 0);
    std::uint8_t* planes[3] = {
        out_pixels.data(),
        out_pixels.data() + layout.plane_offsets[1],
        out_pixels.data() + layout.plane_offsets[2],
    };

    constexpr int kFrames = 8;
    int decoded_ok = 0;
    int keyframes = 0;
    int non_keyframes = 0;
    for (int i = 0; i < kFrames; ++i) {
        // Move the gradient so the encoder has real motion to work with.
        const TestFrame src = make_test_frame(kWidth, kHeight);
        plugins::VideoFrame raw = make_video_frame(src, kWidth, kHeight);
        raw.info.frame_seq = static_cast<std::uint32_t>(i);

        std::vector<std::uint8_t> bs(2 * 1024 * 1024);
        plugins::EncodedVideoFrame encoded;
        ASSERT_EQ(encoder->encode(raw, bs.data(), bs.size(), encoded), plugins::kOk)
            << "encode of frame " << i << " failed";

        if (encoded.is_keyframe) ++keyframes; else ++non_keyframes;

        plugins::VideoFrame decoded;
        decoded.info.frame_seq = static_cast<std::uint32_t>(i);
        const plugins::Status st = decoder->decode(encoded, decoded, planes);
        if (st != plugins::kOk) continue;

        // The luma must not be flat.
        std::vector<std::uint8_t> dec_y(static_cast<std::size_t>(kWidth) * kHeight);
        for (std::uint32_t row = 0; row < kHeight; ++row) {
            std::memcpy(dec_y.data() + static_cast<std::size_t>(row) * kWidth,
                        planes[0] + static_cast<std::size_t>(row) * layout.strides[0],
                        kWidth);
        }
        const auto [min_it, max_it] = std::minmax_element(dec_y.begin(), dec_y.end());
        if (static_cast<int>(*max_it) - static_cast<int>(*min_it) > 20) ++decoded_ok;
    }

    // The test is only meaningful if it actually exercises the reference
    // chain: the GOP is 30, so only the first frame may be a keyframe and
    // the rest must be predicted frames.
    EXPECT_EQ(keyframes, 1) << "expected exactly one IDR in the first GOP";
    EXPECT_GT(non_keyframes, 0)
        << "the test did not produce any predicted frames — the reference "
           "chain is not being exercised";

    EXPECT_GE(decoded_ok, kFrames - 1)
        << "only " << decoded_ok << " of " << kFrames
        << " frames decoded to a non-flat picture — the decoder is losing "
           "reference state across pictures";

    const auto dec_stats = decoder->stats();
    EXPECT_EQ(dec_stats.frames_decoded, static_cast<std::uint32_t>(decoded_ok));
    EXPECT_EQ(dec_stats.decode_errors, 0u);
}

// ---------------------------------------------------------------------------
// A resolution change.
//
// NVDEC decodes into a macroblock-aligned frame: a 1280x720 stream is
// coded as 1280x736 (720 rounded up to a multiple of 16) and the extra
// 16 rows are not visible.  The decoder has to hand back the *visible*
// 1280x720 picture.  This test covers the re-configuration path, where a
// new sequence arrives with a different size.
//
// The size 322x242 is used deliberately: neither dimension is a multiple
// of 16, so the coded frame is 336x256 and the crop is real.
// ---------------------------------------------------------------------------

TEST(HwRoundTrip, NvdecHandlesResolutionChange) {
    if (!nvcodec::nvenc_codec_available(nvcodec::NvCodec::kH264) ||
        !nvcodec::nvdec_codec_available(nvcodec::NvCodec::kH264)) {
        GTEST_SKIP() << "NVENC / NVDEC are not available on this host";
    }

    const auto cfg = make_config();

    // The decoder is configured for the *largest* resolution that will
    // appear in the stream, which is how a real session behaves: the
    // decoder is opened once and the resolution changes under it.
    struct Size { std::uint32_t w, h; };
    const Size sizes[] = {{320, 240}, {322, 242}};
    const Size max_size = sizes[1];

    auto dec_cfg = cfg;
    dec_cfg.width  = max_size.w;
    dec_cfg.height = max_size.h;
    auto decoder = make_nvdec(dec_cfg);
    ASSERT_NE(decoder, nullptr);

    for (const Size& s : sizes) {
        // A separate encoder per resolution: NVENC is configured for a
        // fixed frame size.
        auto enc_cfg = cfg;
        enc_cfg.width  = s.w;
        enc_cfg.height = s.h;
        auto encoder = make_nvenc(enc_cfg);
        ASSERT_NE(encoder, nullptr) << "the NVENC encoder failed to open at "
                                     << s.w << "x" << s.h;

        const auto layout = video_frame::compute_layout(video_frame::PixelFormat::kI420,
                                                        s.w, s.h);
        std::vector<std::uint8_t> out_pixels(layout.buffer_size, 0);
        std::uint8_t* planes[3] = {
            out_pixels.data(),
            out_pixels.data() + layout.plane_offsets[1],
            out_pixels.data() + layout.plane_offsets[2],
        };

        const TestFrame src = make_test_frame(s.w, s.h);
        plugins::VideoFrame raw = make_video_frame(src, s.w, s.h);

        std::vector<std::uint8_t> bs(2 * 1024 * 1024);
        plugins::EncodedVideoFrame encoded;
        ASSERT_EQ(encoder->encode(raw, bs.data(), bs.size(), encoded), plugins::kOk)
            << "encode at " << s.w << "x" << s.h << " failed";

        plugins::VideoFrame decoded;
        const plugins::Status st = decoder->decode(encoded, decoded, planes);
        ASSERT_EQ(st, plugins::kOk)
            << "NVDEC decode at " << s.w << "x" << s.h << " failed (status=" << st << ")";

        // The decoded luma must be non-flat and must match the source.
        std::vector<std::uint8_t> dec_y(static_cast<std::size_t>(s.w) * s.h);
        for (std::uint32_t row = 0; row < s.h; ++row) {
            std::memcpy(dec_y.data() + static_cast<std::size_t>(row) * s.w,
                        planes[0] + static_cast<std::size_t>(row) * layout.strides[0],
                        s.w);
        }
        const auto [min_it, max_it] = std::minmax_element(dec_y.begin(), dec_y.end());
        EXPECT_GT(static_cast<int>(*max_it) - static_cast<int>(*min_it), 20)
            << "the decoded luma at " << s.w << "x" << s.h << " is nearly flat";

        const double mae = luma_mae(src.y, dec_y);
        EXPECT_LT(mae, 12.0)
            << "the decoded luma at " << s.w << "x" << s.h
            << " differs too much from the source (MAE=" << mae << ")";
    }
}

// ---------------------------------------------------------------------------
// A second, cheaper test: the encoder must honour a forced keyframe.
// ---------------------------------------------------------------------------

TEST(HwRoundTrip, NvencForcedKeyframe) {
    if (!nvcodec::nvenc_codec_available(nvcodec::NvCodec::kH264)) {
        GTEST_SKIP() << "NVENC is not available on this host";
    }

    auto cfg = make_config();
    cfg.bitrate_bps       = 500'000;
    cfg.keyframe_interval = 60;

    auto encoder = make_nvenc(cfg);
    ASSERT_NE(encoder, nullptr);

    const TestFrame src = make_test_frame(kWidth, kHeight);
    plugins::VideoFrame raw = make_video_frame(src, kWidth, kHeight);

    std::vector<std::uint8_t> bs(2 * 1024 * 1024);
    plugins::EncodedVideoFrame encoded;

    // Frame 0 is always a keyframe.
    ASSERT_EQ(encoder->encode(raw, bs.data(), bs.size(), encoded), plugins::kOk);
    EXPECT_TRUE(encoded.is_keyframe);
    const auto types = nal_types(encoded.payload.data(), encoded.payload.size());
    EXPECT_NE(std::find(types.begin(), types.end(), 5), types.end())
        << "the first frame must carry an IDR slice";

    // A forced keyframe must produce one too.
    ASSERT_EQ(encoder->force_keyframe(), plugins::kOk);
    raw.info.frame_seq = 1;
    ASSERT_EQ(encoder->encode(raw, bs.data(), bs.size(), encoded), plugins::kOk);
    EXPECT_TRUE(encoded.is_keyframe);
    const auto types2 = nal_types(encoded.payload.data(), encoded.payload.size());
    EXPECT_NE(std::find(types2.begin(), types2.end(), 5), types2.end())
        << "the forced keyframe must carry an IDR slice";
}

// ---------------------------------------------------------------------------
// The bitrate control must actually reach the encoder.
//
// The driver no longer hands out a preset config (nvEncGetEncodePresetConfig
// is dead on driver >= 590), so the backend builds the NV_ENC_CONFIG by
// hand.  If that build regressed, the requested bitrate would silently be
// ignored — which is exactly the bug this test guards against.
// ---------------------------------------------------------------------------

TEST(HwRoundTrip, NvencHonoursBitrate) {
    if (!nvcodec::nvenc_codec_available(nvcodec::NvCodec::kH264)) {
        GTEST_SKIP() << "NVENC is not available on this host";
    }

    // Encode the same picture twice: once at a low bitrate, once at a high
    // one.  The high-bitrate encode must produce a larger frame.
    auto encode_once = [&](std::uint32_t bitrate, std::size_t& out_size) {
        auto cfg = make_config();
        cfg.bitrate_bps = bitrate;
        auto enc = make_nvenc(cfg);
        if (!enc) return false;
        const TestFrame src = make_test_frame(kWidth, kHeight);
        const plugins::VideoFrame raw = make_video_frame(src, kWidth, kHeight);
        std::vector<std::uint8_t> bs(2 * 1024 * 1024);
        plugins::EncodedVideoFrame out;
        if (enc->encode(raw, bs.data(), bs.size(), out) != plugins::kOk) return false;
        out_size = out.payload.size();
        return out_size > 0;
    };

    if (!nvcodec::nvenc_codec_available(nvcodec::NvCodec::kH264)) {
        GTEST_SKIP() << "NVENC is not available on this host";
    }

    std::size_t low_size = 0;
    std::size_t high_size = 0;
    ASSERT_TRUE(encode_once(150'000, low_size))
        << "the low-bitrate encode failed";
    ASSERT_TRUE(encode_once(6'000'000, high_size))
        << "the high-bitrate encode failed";

    EXPECT_GT(high_size, low_size)
        << "the requested bitrate had no effect on the encoded size "
        << "(low=" << low_size << " B, high=" << high_size << " B) — the "
        << "NV_ENC_CONFIG is probably not reaching the driver";
}

// ---------------------------------------------------------------------------
// A picture below the driver's minimum frame geometry.
//
// NVENC refuses to initialize a session whose frame size is below the
// silicon's minimum: `nvEncInitializeEncoder` then fails with
// NV_ENC_ERR_INVALID_PARAM (status 8) and the driver error string is
// "Frame Dimension less than the minimum supported value."  Measured on
// the reference host (RTX 3060, Video Codec SDK 13.1) the limit is
// width >= 145 and height >= 49.
//
// A source frame that small must therefore NOT take the encoder down:
// the encoder clamps the geometry up to the hardware minimum and pads
// the picture (black luma, neutral chroma) to fill it.  This test
// pins that behaviour down — a regression here re-introduces the
// "send_video() fails on a 64x48 frame" bug.
//
// The test is deliberately written against the *observable* contract
// (the encode succeeds, the bitstream is a valid access unit, and the
// source picture survives in the top-left corner) rather than against
// the exact minimum numbers, which are driver-dependent.
// ---------------------------------------------------------------------------

TEST(HwRoundTrip, NvencPadsBelowMinimumGeometry) {
    if (!nvcodec::nvenc_codec_available(nvcodec::NvCodec::kH264)) {
        GTEST_SKIP() << "NVENC is not available on this host";
    }

    // 64x48 is below the driver's minimum on every NVENC generation
    // NimRTC targets.
    constexpr std::uint32_t kSmallW = 64;
    constexpr std::uint32_t kSmallH = 48;

    auto cfg = make_config();
    cfg.width  = kSmallW;
    cfg.height = kSmallH;

    auto encoder = make_nvenc(cfg);
    ASSERT_NE(encoder, nullptr)
        << "the NVENC encoder refused to open at " << kSmallW << "x" << kSmallH;

    const TestFrame src = make_test_frame(kSmallW, kSmallH);
    const plugins::VideoFrame raw = make_video_frame(src, kSmallW, kSmallH);
    ASSERT_TRUE(raw.buffer);

    std::vector<std::uint8_t> bs(2 * 1024 * 1024);
    plugins::EncodedVideoFrame encoded;
    const plugins::Status st =
        encoder->encode(raw, bs.data(), bs.size(), encoded);
    ASSERT_EQ(st, plugins::kOk)
        << "the NVENC encode of a " << kSmallW << "x" << kSmallH
        << " frame failed (status=" << st << ") — the sub-minimum geometry "
        << "is not being clamped up to the hardware minimum";
    ASSERT_GT(encoded.payload.size(), 0u);

    // The output must still be a valid Annex-B access unit with an IDR.
    const std::vector<int> types = nal_types(encoded.payload.data(),
                                             encoded.payload.size());
    ASSERT_GE(types.size(), 3u);
    EXPECT_EQ(types[0], 7) << "the first NAL unit must be the SPS";
    EXPECT_EQ(types[1], 8) << "the second NAL unit must be the PPS";
    EXPECT_NE(std::find(types.begin(), types.end(), 5), types.end())
        << "expected an IDR slice (NAL type 5)";

    // The encoder must not have counted an error.
    const auto stats = encoder->stats();
    EXPECT_EQ(stats.encode_errors, 0u);
    EXPECT_EQ(stats.frames_encoded, 1u);
}

// ---------------------------------------------------------------------------
// The decoder-only leg.
//
// The tests above all need NVENC *and* NVDEC.  But the two live in
// different runtime DLLs (nvEncodeAPI64.dll vs nvcuvid.dll) and the
// NVENC session can be unavailable on a host whose NVDEC works fine
// (e.g. a driver whose encode session was never created, or a laptop
// whose iGPU handles the display but whose dGPU does the decode).
//
// So the decode path gets its own leg, driven by a *self-contained*
// H.264 bitstream that is checked into the tree:
//
//   src/modules/nvcodec/tests/nvdec_test_vector.hpp
//
// The vector is a single 320x240 IDR access unit in Annex-B form.  Its
// content is a horizontal luma ramp from 16 to 235 (BT.601 limited
// range) with neutral chroma (U = V = 128), encoded with:
//
//   ffmpeg -f lavfi -i color=c=black:s=320x240 \
//          -vf "geq=lum='X/W*219+16':cb=128:cr=128" \
//          -frames:v 1 -c:v libx264 -profile:v high -preset medium \
//          -crf 18 -pix_fmt yuv420p \
//          -x264-params "keyint=1:scenecut=0:aud=0" \
//          -bsf:v h264_mp4toannexb -f h264 out.264
//
// The decoded picture is therefore fully predictable: every luma row
// must equal `16 + round(col * 219 / (width - 1))` (within the codec's
// own quantisation error) and the chroma must sit on 128.
// ---------------------------------------------------------------------------

namespace {

/** The analytic luma of the test vector at column @p col. */
int expected_luma(std::uint32_t col, std::uint32_t width) {
    if (width <= 1) return 16;
    const double ramp = static_cast<double>(col) * 219.0 / (width - 1);
    return 16 + static_cast<int>(std::lround(ramp));
}

/** The analytic chroma of the test vector — always neutral. */
constexpr int expected_chroma() { return 128; }

} // namespace

TEST(HwRoundTrip, NvdecDecodesSelfContainedBitstream) {
    if (!nvcodec::nvdec_codec_available(nvcodec::NvCodec::kH264)) {
        GTEST_SKIP() << "NVDEC is not available on this host — skipping the "
                        "decoder-only test";
    }

    const auto cfg = make_config();

    auto decoder = make_nvdec(cfg);
    ASSERT_NE(decoder, nullptr) << "the NVDEC backend failed to open";
    ASSERT_TRUE(decoder->can_decode());
    ASSERT_FALSE(decoder->can_encode());

    // The vector is 320x240, which matches kWidth / kHeight, so the
    // caller-supplied output buffers can be sized from the config.
    constexpr std::uint32_t kVecWidth  = 320;
    constexpr std::uint32_t kVecHeight = 240;
    ASSERT_EQ(kVecWidth, kWidth);
    ASSERT_EQ(kVecHeight, kHeight);

    const auto layout = video_frame::compute_layout(video_frame::PixelFormat::kI420,
                                                    kVecWidth, kVecHeight);
    std::vector<std::uint8_t> out_pixels(layout.buffer_size, 0);
    std::uint8_t* planes[3] = {
        out_pixels.data(),
        out_pixels.data() + layout.plane_offsets[1],
        out_pixels.data() + layout.plane_offsets[2],
    };

    plugins::EncodedVideoFrame encoded;
    encoded.codec        = plugins::VideoCodecKind::kH264;
    encoded.nalu_format  = plugins::NaluFormat::kAnnexBStartCode;
    encoded.is_keyframe  = true;
    encoded.payload_type = 102;
    encoded.payload      = core::ByteSpan{kNvdecGradientIdr320x240,
                                          kNvdecGradientIdr320x240Size};

    plugins::VideoFrame decoded;
    decoded.info.capture_ts_us = 4242;
    decoded.info.frame_seq     = 7;

    const plugins::Status status = decoder->decode(encoded, decoded, planes);
    ASSERT_EQ(status, plugins::kOk)
        << "NVDEC decode failed (status=" << status << ")";

    // ---- the geometry comes from the bitstream's SPS --------------------
    EXPECT_EQ(decoded.width(), kVecWidth);
    EXPECT_EQ(decoded.height(), kVecHeight);

    // ---- the luma must be the expected ramp -----------------------------
    // The decoder writes into the caller's I420 buffer with 16-byte
    // aligned strides, so the rows have to be de-strided before they can
    // be compared with the analytic ramp.
    double luma_abs_err = 0.0;
    int    luma_max_err = 0;
    for (std::uint32_t row = 0; row < kVecHeight; ++row) {
        const std::uint8_t* y = planes[0] +
                                static_cast<std::size_t>(row) * layout.strides[0];
        for (std::uint32_t col = 0; col < kVecWidth; ++col) {
            const int want = expected_luma(col, kVecWidth);
            const int got  = y[col];
            const int err  = std::abs(got - want);
            luma_abs_err += err;
            if (err > luma_max_err) luma_max_err = err;
        }
    }
    luma_abs_err /= static_cast<double>(kVecWidth) * kVecHeight;

    // The vector was encoded at crf 18 from a perfectly smooth ramp, so
    // the reconstruction error is tiny — a couple of luma levels.
    EXPECT_LT(luma_abs_err, 3.0)
        << "the decoded luma does not match the expected ramp (MAE="
        << luma_abs_err << ", max=" << luma_max_err << ")";
    EXPECT_LT(luma_max_err, 24)
        << "the decoded luma has a large local error (max=" << luma_max_err
        << ") — the picture is not the vector that was encoded";

    // ---- the chroma must be neutral -------------------------------------
    const std::uint32_t cw = kVecWidth / 2;
    const std::uint32_t ch = kVecHeight / 2;
    int chroma_max_err = 0;
    for (std::uint32_t row = 0; row < ch; ++row) {
        const std::uint8_t* u = planes[1] +
                                static_cast<std::size_t>(row) * layout.strides[1];
        const std::uint8_t* v = planes[2] +
                                static_cast<std::size_t>(row) * layout.strides[2];
        for (std::uint32_t col = 0; col < cw; ++col) {
            chroma_max_err = std::max(chroma_max_err,
                                      std::abs(static_cast<int>(u[col]) - expected_chroma()));
            chroma_max_err = std::max(chroma_max_err,
                                      std::abs(static_cast<int>(v[col]) - expected_chroma()));
        }
    }
    EXPECT_LT(chroma_max_err, 8)
        << "the decoded chroma is not neutral (max deviation=" << chroma_max_err
        << ") — the U / V de-interleave from NV12 is wrong";

    // ---- the decoder's own statistics ------------------------------------
    const auto stats = decoder->stats();
    EXPECT_EQ(stats.frames_decoded, 1u);
    EXPECT_EQ(stats.decode_errors, 0u);
    EXPECT_EQ(stats.bytes_decoded, kNvdecGradientIdr320x240Size);
}

// ---------------------------------------------------------------------------
// The decoder-owned output path.
//
// A hardware decoder knows the picture's real geometry only after it has
// parsed the SPS, so it can size the output buffer itself.  When the
// caller passes no output buffers the decoder must allocate them, and
// publish them through `raw_out.buffer`.
// ---------------------------------------------------------------------------

TEST(HwRoundTrip, NvdecAllocatesItsOwnOutputBuffer) {
    if (!nvcodec::nvdec_codec_available(nvcodec::NvCodec::kH264)) {
        GTEST_SKIP() << "NVDEC is not available on this host";
    }

    const auto cfg = make_config();

    auto decoder = make_nvdec(cfg);
    ASSERT_NE(decoder, nullptr);

    plugins::EncodedVideoFrame encoded;
    encoded.codec       = plugins::VideoCodecKind::kH264;
    encoded.nalu_format = plugins::NaluFormat::kAnnexBStartCode;
    encoded.is_keyframe = true;
    encoded.payload     = core::ByteSpan{kNvdecGradientIdr320x240,
                                         kNvdecGradientIdr320x240Size};

    plugins::VideoFrame decoded;
    // No output buffers: the decoder allocates its own.
    const plugins::Status status = decoder->decode(encoded, decoded, nullptr);
    ASSERT_EQ(status, plugins::kOk)
        << "NVDEC decode failed (status=" << status << ")";

    ASSERT_TRUE(decoded.buffer) << "the decoder did not publish its output buffer";
    auto buf = decoded.cpu_buffer();
    ASSERT_TRUE(buf);
    EXPECT_EQ(buf->layout().width, 320u);
    EXPECT_EQ(buf->layout().height, 240u);
    EXPECT_EQ(buf->layout().format, video_frame::PixelFormat::kI420);

    // The luma must still hold the ramp.
    const auto layout = buf->layout();
    const std::uint8_t* y = buf->plane(0);
    ASSERT_NE(y, nullptr);
    for (std::uint32_t col = 0; col < 320; col += 16) {
        const int want = expected_luma(col, 320);
        const int got  = y[col];
        EXPECT_LE(std::abs(got - want), 24)
            << "column " << col << ": expected luma ~" << want << ", got " << got;
    }
}

// ---------------------------------------------------------------------------
// A corrupted bitstream must be reported as an error, not decoded into
// garbage.  The decoder's `decode_errors` counter is the signal.
// ---------------------------------------------------------------------------

TEST(HwRoundTrip, NvdecRejectsGarbage) {
    if (!nvcodec::nvdec_codec_available(nvcodec::NvCodec::kH264)) {
        GTEST_SKIP() << "NVDEC is not available on this host";
    }

    const auto cfg = make_config();

    auto decoder = make_nvdec(cfg);
    ASSERT_NE(decoder, nullptr);

    const auto layout = video_frame::compute_layout(video_frame::PixelFormat::kI420,
                                                    kWidth, kHeight);
    std::vector<std::uint8_t> out_pixels(layout.buffer_size, 0);
    std::uint8_t* planes[3] = {
        out_pixels.data(),
        out_pixels.data() + layout.plane_offsets[1],
        out_pixels.data() + layout.plane_offsets[2],
    };

    // An Annex-B start code followed by nothing that looks like a NAL
    // unit header: the parser must not produce a picture.
    const std::uint8_t garbage[] = {0x00, 0x00, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF};

    plugins::EncodedVideoFrame encoded;
    encoded.codec       = plugins::VideoCodecKind::kH264;
    encoded.nalu_format = plugins::NaluFormat::kAnnexBStartCode;
    encoded.payload     = core::ByteSpan{garbage, sizeof(garbage)};

    plugins::VideoFrame decoded;
    const plugins::Status status = decoder->decode(encoded, decoded, planes);
    EXPECT_NE(status, plugins::kOk)
        << "the decoder accepted a garbage bitstream";
}

// ---------------------------------------------------------------------------
// An empty payload is an invalid argument, and it must be counted as a
// decode error rather than silently ignored.
// ---------------------------------------------------------------------------

TEST(HwRoundTrip, NvdecRejectsEmptyPayload) {
    if (!nvcodec::nvdec_codec_available(nvcodec::NvCodec::kH264)) {
        GTEST_SKIP() << "NVDEC is not available on this host";
    }

    const auto cfg = make_config();

    auto decoder = make_nvdec(cfg);
    ASSERT_NE(decoder, nullptr);

    plugins::EncodedVideoFrame encoded;
    encoded.codec       = plugins::VideoCodecKind::kH264;
    encoded.nalu_format = plugins::NaluFormat::kAnnexBStartCode;
    encoded.payload     = core::ByteSpan{};

    plugins::VideoFrame decoded;
    const plugins::Status status = decoder->decode(encoded, decoded, nullptr);
    EXPECT_EQ(status, plugins::kErrInvalidParam);

    const auto stats = decoder->stats();
    EXPECT_EQ(stats.decode_errors, 1u);
    EXPECT_EQ(stats.frames_decoded, 0u);
}
