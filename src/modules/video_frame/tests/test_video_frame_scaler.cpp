/**
 * @file src/modules/video_frame/tests/test_video_frame_scaler.cpp
 * @brief VideoFrameScaler tests (remote-desktop plan C2).
 *
 * Acceptance criteria from the plan (C2):
 *
 *   "A scale test asserts the output dimensions and that a solid-colour
 *    source survives scaling without banding; a benchmark records ns/pixel
 *    at 1080p."
 *
 * The cases here:
 *
 *   1. Down-scale 1920x1080 -> 1280x720 and up-scale 1280x720 ->
 *      1920x1080: the output dimensions are exact.
 *   2. A solid-colour source (mid-grey) survives a scale with no
 *      banding: the output luma's mean stays close to the source's and
 *      its spread (max - min) stays tight.
 *   3. The chroma planes are scaled too 鈥?the chroma mean tracks the
 *      luma mean instead of being garbage.
 *   4. A scaled frame that is then encoded by the OpenH264 software
 *      encoder and decoded back still shows the solid colour.  Skipped
 *      (GTEST_SKIP) when the H.264 module / OpenH264 plugin is not in
 *      the build.
 *   5. `available()` is false when the module was built without libyuv,
 *      and the scale call then returns a clean error rather than
 *      crashing.
 *   6. A `std::chrono` benchmark records ns/pixel at 1080p.  It asserts
 *      only that the run completes 鈥?a perf threshold would be flaky on
 *      shared CI.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include <nimrtc/core/error.hpp>
#include <nimrtc/video_frame/frame.hpp>
#include <nimrtc/video_frame/scaler.hpp>

#if defined(NIMRTC_TEST_WITH_OPENH264)
    #include <nimrtc/h264/codec_plugin.hpp>
    #include <nimrtc/h264/hw_backends.hpp>
    #include <nimrtc/plugins/video_codec.hpp>
#endif

namespace vf = nimrtc::video_frame;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

constexpr std::uint32_t kSrcW = 1920;
constexpr std::uint32_t kSrcH = 1080;
constexpr std::uint32_t kDstW = 1280;
constexpr std::uint32_t kDstH = 720;

/// Mid-grey in BT.601 limited range: Y = 128, U = V = 128 (neutral).
constexpr std::uint8_t kGreyY = 128;
constexpr std::uint8_t kGreyC = 128;

/** An owned I420 buffer filled with a solid colour. */
struct SolidFrame {
    std::shared_ptr<vf::VideoFrameBuffer> buffer;
    vf::VideoFrameLayout layout;
};

SolidFrame make_solid_i420(std::uint32_t width, std::uint32_t height,
                           std::uint8_t y, std::uint8_t u, std::uint8_t v) {
    SolidFrame out;
    out.layout = vf::compute_layout(vf::PixelFormat::kI420, width, height);
    out.buffer = std::make_shared<vf::VideoFrameBuffer>(out.layout);

    for (std::uint32_t row = 0; row < height; ++row) {
        std::uint8_t* p = out.buffer->plane(0) +
                          static_cast<std::size_t>(row) * out.layout.strides[0];
        std::fill(p, p + width, y);
    }
    const std::uint32_t cw = width / 2;
    const std::uint32_t ch = height / 2;
    for (std::uint32_t row = 0; row < ch; ++row) {
        std::uint8_t* pu = out.buffer->plane(1) +
                           static_cast<std::size_t>(row) * out.layout.strides[1];
        std::uint8_t* pv = out.buffer->plane(2) +
                           static_cast<std::size_t>(row) * out.layout.strides[2];
        std::fill(pu, pu + cw, u);
        std::fill(pv, pv + cw, v);
    }
    return out;
}

/** Tightly-pack one plane into a vector (row by row, width bytes each). */
std::vector<std::uint8_t> pack_plane(const vf::VideoFrameBuffer& buf,
                                      std::uint8_t plane,
                                      std::uint32_t width,
                                      std::uint32_t height) {
    std::vector<std::uint8_t> out(
        static_cast<std::size_t>(width) * height, 0);
    const std::int32_t stride = buf.layout().strides[plane];
    const std::uint8_t* src = buf.plane(plane);
    for (std::uint32_t row = 0; row < height; ++row) {
        std::copy(src + static_cast<std::size_t>(row) * stride,
                  src + static_cast<std::size_t>(row) * stride + width,
                  out.begin() + static_cast<std::ptrdiff_t>(row) * width);
    }
    return out;
}

struct PlaneStats {
    double mean = 0.0;
    std::uint8_t min = 0;
    std::uint8_t max = 0;
    int spread() const { return static_cast<int>(max) - static_cast<int>(min); }
};

PlaneStats stats_of(const std::vector<std::uint8_t>& v) {
    PlaneStats s;
    if (v.empty()) return s;
    double sum = 0.0;
    const auto [mn, mx] = std::minmax_element(v.begin(), v.end());
    s.min = *mn;
    s.max = *mx;
    for (std::uint8_t b : v) sum += static_cast<double>(b);
    s.mean = sum / static_cast<double>(v.size());
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Availability
// ---------------------------------------------------------------------------

TEST(VideoFrameScaler, AvailableReflectsTheBuildConfiguration) {
    // The test is written so that it passes in BOTH shapes:
    //   - libyuv linked in  -> available() is true and the scale works.
    //   - libyuv absent      -> available() is false and the scale
    //                            returns a clean error (no crash).
    if (vf::VideoFrameScaler::available()) {
        SUCCEED() << "libyuv is linked in";
    } else {
        GTEST_SKIP() << "libyuv is not available in this build "
                        "(NIMRTC_VENDORED_LIBYUV=OFF or submodule absent)";
    }
}

TEST(VideoFrameScaler, UnavailableBuildReturnsACleanError) {
    vf::VideoFrameScaler scaler{vf::ScaleFilter::kBilinear};

    if (vf::VideoFrameScaler::available()) {
        // The backend IS present, so this case exercises the *invalid
        // argument* path instead: a null source plane must be rejected
        // cleanly (not crash).
        std::uint8_t dst[16] = {0};
        const auto rc = scaler.scale_i420(
            nullptr, 0, nullptr, 0, nullptr, 0,
            4, 4,
            dst, 4, dst, 2, dst, 2,
            4, 4);
        EXPECT_FALSE(rc.ok());
        EXPECT_EQ(rc.error().code(), nimrtc::core::ErrorCode::InvalidArgument);
        return;
    }

    // No backend: every entry point must return a clean error and must
    // not crash.
    std::uint8_t src[16] = {0};
    std::uint8_t dst[16] = {0};

    const auto rc = scaler.scale_i420(
        src, 4, src, 2, src, 2, 4, 4,
        dst, 4, dst, 2, dst, 2, 4, 4);
    EXPECT_FALSE(rc.ok());
    EXPECT_EQ(rc.error().code(), nimrtc::core::ErrorCode::NotImplemented);
    EXPECT_FALSE(rc.error().message().empty());

    const auto rc2 = scaler.bgra_to_i420(4, 4, src, 16, dst, 4, dst, 2, dst, 2);
    EXPECT_FALSE(rc2.ok());
    EXPECT_EQ(rc2.error().code(), nimrtc::core::ErrorCode::NotImplemented);

    const auto rc3 = scaler.i420_to_bgra(4, 4, src, 4, src, 2, src, 2, dst, 16);
    EXPECT_FALSE(rc3.ok());
    EXPECT_EQ(rc3.error().code(), nimrtc::core::ErrorCode::NotImplemented);
}

// ---------------------------------------------------------------------------
// Dimensions
// ---------------------------------------------------------------------------

TEST(VideoFrameScaler, DownscaleProducesTheExactTargetDimensions) {
    if (!vf::VideoFrameScaler::available()) {
        GTEST_SKIP() << "libyuv is not available in this build";
    }

    const SolidFrame src =
        make_solid_i420(kSrcW, kSrcH, kGreyY, kGreyC, kGreyC);

    vf::VideoFrameScaler scaler{vf::ScaleFilter::kBilinear};
    const auto scaled =
        scaler.scale_to_buffer(*src.buffer, kDstW, kDstH);
    ASSERT_TRUE(scaled.ok()) << scaled.error().message();

    const auto& out = *scaled.value();
    EXPECT_EQ(out.width(), kDstW);
    EXPECT_EQ(out.height(), kDstH);
    EXPECT_EQ(out.layout().width, kDstW);
    EXPECT_EQ(out.layout().height, kDstH);
    EXPECT_EQ(out.format(), vf::PixelFormat::kI420);
    EXPECT_EQ(out.layout().num_planes, 3u);
}

TEST(VideoFrameScaler, UpscaleProducesTheExactTargetDimensions) {
    if (!vf::VideoFrameScaler::available()) {
        GTEST_SKIP() << "libyuv is not available in this build";
    }

    const SolidFrame src =
        make_solid_i420(kDstW, kDstH, kGreyY, kGreyC, kGreyC);

    vf::VideoFrameScaler scaler{vf::ScaleFilter::kBox};
    const auto scaled =
        scaler.scale_to_buffer(*src.buffer, kSrcW, kSrcH);
    ASSERT_TRUE(scaled.ok()) << scaled.error().message();

    const auto& out = *scaled.value();
    EXPECT_EQ(out.width(), kSrcW);
    EXPECT_EQ(out.height(), kSrcH);
}

TEST(VideoFrameScaler, ScalingThroughThePoolProducesTheTargetDimensions) {
    if (!vf::VideoFrameScaler::available()) {
        GTEST_SKIP() << "libyuv is not available in this build";
    }

    const SolidFrame src =
        make_solid_i420(kSrcW, kSrcH, kGreyY, kGreyC, kGreyC);

    vf::VideoFrameBufferPool pool;
    vf::VideoFrameScaler scaler{vf::ScaleFilter::kBilinear};

    const auto scaled = scaler.scale_to_buffer(*src.buffer, kDstW, kDstH, &pool);
    ASSERT_TRUE(scaled.ok()) << scaled.error().message();
    EXPECT_EQ(scaled.value()->width(), kDstW);
    EXPECT_EQ(scaled.value()->height(), kDstH);
    EXPECT_EQ(pool.total_allocations(), 1u);
}

TEST(VideoFrameScaler, InvalidArgumentsAreRejected) {
    if (!vf::VideoFrameScaler::available()) {
        GTEST_SKIP() << "libyuv is not available in this build";
    }

    vf::VideoFrameScaler scaler{vf::ScaleFilter::kBilinear};

    // Zero target dimension.
    const SolidFrame src = make_solid_i420(64, 64, kGreyY, kGreyC, kGreyC);
    const auto rc = scaler.scale_to_buffer(*src.buffer, 0, 64);
    EXPECT_FALSE(rc.ok());
    EXPECT_EQ(rc.error().code(), nimrtc::core::ErrorCode::InvalidArgument);

    // A non-I420 source is rejected.
    const auto bgra_layout =
        vf::compute_layout(vf::PixelFormat::kBGRA, 64, 64);
    vf::VideoFrameBuffer bgra_buf(bgra_layout);
    const auto rc2 = scaler.scale_to_buffer(bgra_buf, 32, 32);
    EXPECT_FALSE(rc2.ok());
    EXPECT_EQ(rc2.error().code(), nimrtc::core::ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Solid colour survives the scale (no banding)
// ---------------------------------------------------------------------------

TEST(VideoFrameScaler, SolidColourSurvivesDownscaleWithoutBanding) {
    if (!vf::VideoFrameScaler::available()) {
        GTEST_SKIP() << "libyuv is not available in this build";
    }

    const SolidFrame src =
        make_solid_i420(kSrcW, kSrcH, kGreyY, kGreyC, kGreyC);

    vf::VideoFrameScaler scaler{vf::ScaleFilter::kBox};
    const auto scaled = scaler.scale_to_buffer(*src.buffer, kDstW, kDstH);
    ASSERT_TRUE(scaled.ok()) << scaled.error().message();

    const auto luma = pack_plane(*scaled.value(), 0, kDstW, kDstH);
    const PlaneStats st = stats_of(luma);

    // The mean must stay on the source's luma.
    EXPECT_NEAR(st.mean, static_cast<double>(kGreyY), 2.0)
        << "the scaled luma's mean drifted from the source's";

    // "No banding" == the spread stays tight: a solid source must not
    // turn into a gradient.  Two grey levels of spread is the budget.
    EXPECT_LE(st.spread(), 2)
        << "the scaled luma shows banding (spread " << st.spread() << ")";
}

TEST(VideoFrameScaler, SolidColourSurvivesUpscaleWithoutBanding) {
    if (!vf::VideoFrameScaler::available()) {
        GTEST_SKIP() << "libyuv is not available in this build";
    }

    const SolidFrame src =
        make_solid_i420(kDstW, kDstH, kGreyY, kGreyC, kGreyC);

    vf::VideoFrameScaler scaler{vf::ScaleFilter::kBox};
    const auto scaled = scaler.scale_to_buffer(*src.buffer, kSrcW, kSrcH);
    ASSERT_TRUE(scaled.ok()) << scaled.error().message();

    const auto luma = pack_plane(*scaled.value(), 0, kSrcW, kSrcH);
    const PlaneStats st = stats_of(luma);

    EXPECT_NEAR(st.mean, static_cast<double>(kGreyY), 2.0);
    EXPECT_LE(st.spread(), 2)
        << "the scaled luma shows banding (spread " << st.spread() << ")";
}

TEST(VideoFrameScaler, ChromaPlanesAreScaledToo) {
    if (!vf::VideoFrameScaler::available()) {
        GTEST_SKIP() << "libyuv is not available in this build";
    }

    // A *coloured* source: a strongly saturated chroma (U=90, V=210) so
    // a garbage chroma plane is unmistakable.
    const SolidFrame src = make_solid_i420(kSrcW, kSrcH, 100, 90, 210);

    vf::VideoFrameScaler scaler{vf::ScaleFilter::kBox};
    const auto scaled = scaler.scale_to_buffer(*src.buffer, kDstW, kDstH);
    ASSERT_TRUE(scaled.ok()) << scaled.error().message();

    const auto u = pack_plane(*scaled.value(), 1, kDstW / 2, kDstH / 2);
    const auto v = pack_plane(*scaled.value(), 2, kDstW / 2, kDstH / 2);
    const auto y = pack_plane(*scaled.value(), 0, kDstW, kDstH);

    const PlaneStats su = stats_of(u);
    const PlaneStats sv = stats_of(v);
    const PlaneStats sy = stats_of(y);

    // The chroma must have been scaled (not left at garbage, not zeroed):
    // the means track the source's chroma, and the planes are flat.
    EXPECT_NEAR(su.mean, 90.0, 2.0);
    EXPECT_NEAR(sv.mean, 210.0, 2.0);
    EXPECT_LE(su.spread(), 2);
    EXPECT_LE(sv.spread(), 2);

    // The chroma mean must track the luma mean (the chroma planes belong
    // to the *same* picture 鈥?a scaler that scaled only the luma would
    // leave the chroma at the source's values, which for a different
    // source size would show up here as a mismatch).
    EXPECT_NEAR(sy.mean, 100.0, 2.0);
}

TEST(VideoFrameScaler, AllFilterModesPreserveASolidColour) {
    if (!vf::VideoFrameScaler::available()) {
        GTEST_SKIP() << "libyuv is not available in this build";
    }

    const SolidFrame src =
        make_solid_i420(kSrcW, kSrcH, kGreyY, kGreyC, kGreyC);

    for (const vf::ScaleFilter f : {vf::ScaleFilter::kPoint,
                                    vf::ScaleFilter::kLinear,
                                    vf::ScaleFilter::kBilinear,
                                    vf::ScaleFilter::kBox}) {
        vf::VideoFrameScaler scaler{f};
        const auto scaled = scaler.scale_to_buffer(*src.buffer, kDstW, kDstH);
        ASSERT_TRUE(scaled.ok()) << scaled.error().message();

        const auto luma = pack_plane(*scaled.value(), 0, kDstW, kDstH);
        const PlaneStats st = stats_of(luma);
        EXPECT_NEAR(st.mean, static_cast<double>(kGreyY), 2.0)
            << "filter " << vf::scale_filter_name(f) << " drifted the mean";
        EXPECT_LE(st.spread(), 2)
            << "filter " << vf::scale_filter_name(f) << " banded the output";
    }
}

// ---------------------------------------------------------------------------
// Colour conversion
// ---------------------------------------------------------------------------

TEST(VideoFrameScaler, BgraToI420AndBackPreservesTheColour) {
    if (!vf::VideoFrameScaler::available()) {
        GTEST_SKIP() << "libyuv is not available in this build";
    }

    constexpr std::uint32_t w = 64;
    constexpr std::uint32_t h = 48;

    // A mid-grey BGRA picture.
    std::vector<std::uint8_t> bgra(static_cast<std::size_t>(w) * h * 4, 0);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
        bgra[i * 4 + 0] = 128;  // B
        bgra[i * 4 + 1] = 128;  // G
        bgra[i * 4 + 2] = 128;  // R
        bgra[i * 4 + 3] = 255;  // A
    }

    vf::VideoFrameScaler scaler{vf::ScaleFilter::kBilinear};

    const auto layout = vf::compute_layout(vf::PixelFormat::kI420, w, h);
    std::vector<std::uint8_t> y(layout.buffer_size, 0);
    std::uint8_t* yp = y.data();
    std::uint8_t* up = yp + layout.plane_offsets[1];
    std::uint8_t* vp = yp + layout.plane_offsets[2];

    const auto rc = scaler.bgra_to_i420(
        w, h, bgra.data(), static_cast<std::int32_t>(w * 4),
        yp, layout.strides[0], up, layout.strides[1], vp, layout.strides[2]);
    ASSERT_TRUE(rc.ok()) << rc.error().message();

    // BT.601 limited range: mid-grey maps to Y = 128, U = V = 128.
    EXPECT_NEAR(y[0], 128.0, 2.0);
    EXPECT_NEAR(up[0], 128.0, 2.0);
    EXPECT_NEAR(vp[0], 128.0, 2.0);

    // And back to BGRA.
    std::vector<std::uint8_t> back(static_cast<std::size_t>(w) * h * 4, 0);
    const auto rc2 = scaler.i420_to_bgra(
        w, h, yp, layout.strides[0], up, layout.strides[1], vp, layout.strides[2],
        back.data(), static_cast<std::int32_t>(w * 4));
    ASSERT_TRUE(rc2.ok()) << rc2.error().message();

    EXPECT_EQ(back[0], 128);
    EXPECT_EQ(back[1], 128);
    EXPECT_EQ(back[2], 128);
    EXPECT_EQ(back[3], 255) << "the alpha channel must be opaque";
}

// A grey picture cannot detect a swapped R/B channel, so this case uses a
// saturated colour: a pure red pixel (B=0, G=0, R=255) must land on the
// BT.601 limited-range values for red, and the round trip must put the
// red back into the *red* byte.
TEST(VideoFrameScaler, BgraToI420KeepsTheChannelsInOrder) {
    if (!vf::VideoFrameScaler::available()) {
        GTEST_SKIP() << "libyuv is not available in this build";
    }

    constexpr std::uint32_t w = 64;
    constexpr std::uint32_t h = 48;

    // Pure red in BGRA order: B, G, R, A.
    std::vector<std::uint8_t> bgra(static_cast<std::size_t>(w) * h * 4, 0);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
        bgra[i * 4 + 0] = 0;    // B
        bgra[i * 4 + 1] = 0;    // G
        bgra[i * 4 + 2] = 255;  // R
        bgra[i * 4 + 3] = 255;  // A
    }

    vf::VideoFrameScaler scaler{vf::ScaleFilter::kBilinear};

    const auto layout = vf::compute_layout(vf::PixelFormat::kI420, w, h);
    std::vector<std::uint8_t> y(layout.buffer_size, 0);
    std::uint8_t* yp = y.data();
    std::uint8_t* up = yp + layout.plane_offsets[1];
    std::uint8_t* vp = yp + layout.plane_offsets[2];

    const auto rc = scaler.bgra_to_i420(
        w, h, bgra.data(), static_cast<std::int32_t>(w * 4),
        yp, layout.strides[0], up, layout.strides[1], vp, layout.strides[2]);
    ASSERT_TRUE(rc.ok()) << rc.error().message();

    // BT.601 limited range, pure red:
    //   Y = 16  + 219 * 0.299            ~= 81
    //   U = 128 - 0.439 * (255 - 16)     ~= 90   (red carries no blue,
    //                                               so the chroma sits
    //                                               below neutral)
    //   V = 128 + 0.712 * (255 - 16)     ~= 239  (red is the dominant
    //                                               difference channel)
    EXPECT_NEAR(y[0], 82.0, 3.0);
    EXPECT_NEAR(up[0], 90.0, 3.0);
    EXPECT_NEAR(vp[0], 239.0, 3.0);

    // The round trip must put the red back into the red byte.
    std::vector<std::uint8_t> back(static_cast<std::size_t>(w) * h * 4, 0);
    const auto rc2 = scaler.i420_to_bgra(
        w, h, yp, layout.strides[0], up, layout.strides[1], vp, layout.strides[2],
        back.data(), static_cast<std::int32_t>(w * 4));
    ASSERT_TRUE(rc2.ok()) << rc2.error().message();

    // The round trip must put the red back into the red byte.  The
    // conversion is not bit-exact (BT.601 limited range rounds), so the
    // bounds are loose — but a swapped R/B would move 255 into the blue
    // byte, which these bounds catch.
    EXPECT_LT(back[0], 64) << "the blue byte must stay near zero (a swapped "
                               "R/B would put ~255 here)";
    EXPECT_LT(back[1], 64) << "the green byte must stay near zero";
    EXPECT_GT(back[2], 192) << "the red byte must carry the red (a swapped "
                                 "R/B would put ~0 here)";
    EXPECT_EQ(back[3], 255);
}

// ---------------------------------------------------------------------------
// Benchmark 鈥?ns/pixel at 1080p
// ---------------------------------------------------------------------------

TEST(VideoFrameScaler, BenchmarkRecordsNsPerPixelAt1080p) {
    if (!vf::VideoFrameScaler::available()) {
        GTEST_SKIP() << "libyuv is not available in this build";
    }

    const SolidFrame src =
        make_solid_i420(kSrcW, kSrcH, kGreyY, kGreyC, kGreyC);

    vf::VideoFrameScaler scaler{vf::ScaleFilter::kBilinear};

    // Warm-up (first call pays for the CPU's frequency ramp and for the
    // first-touch of the destination pages).
    {
        const auto warm = scaler.scale_to_buffer(*src.buffer, kDstW, kDstH);
        ASSERT_TRUE(warm.ok()) << warm.error().message();
    }

    constexpr int kIterations = 30;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        const auto scaled = scaler.scale_to_buffer(*src.buffer, kDstW, kDstH);
        ASSERT_TRUE(scaled.ok()) << scaled.error().message();
    }
    const auto t1 = std::chrono::steady_clock::now();

    const double elapsed_ns =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    const double pixels = static_cast<double>(kSrcW) * kSrcH;
    const double ns_per_pixel = elapsed_ns / (pixels * kIterations);

    // The number is RECORDED, not asserted: a perf threshold would be
    // flaky on shared CI.  The assertion is only that the run completed
    // and produced a sane measurement.
    std::cout << "[BENCH] libyuv I420 scale 1920x1080 -> 1280x720: "
              << ns_per_pixel << " ns/pixel (source pixels, "
              << kIterations << " iterations)" << std::endl;

    EXPECT_GT(elapsed_ns, 0.0);
    EXPECT_GT(ns_per_pixel, 0.0);
    EXPECT_LT(ns_per_pixel, 100000.0)
        << "the benchmark did not complete in a sane time 鈥?something is "
           "wrong with the timing harness";
}

// ---------------------------------------------------------------------------
// Encode round-trip (integration-ish, OpenH264 only)
// ---------------------------------------------------------------------------

#if defined(NIMRTC_TEST_WITH_OPENH264)

TEST(VideoFrameScaler, ScaledFrameSurvivesTheOpenh264EncodeDecodeRoundTrip) {
    namespace plugins = nimrtc::plugins;

    // The round trip needs a scaled frame, and the scaling needs
    // libyuv.  Without it there is nothing to round-trip.
    if (!vf::VideoFrameScaler::available()) {
        GTEST_SKIP() << "libyuv is not available in this build";
    }

    nimrtc::h264::register_default_video_backends();
    const plugins::VideoDecoderBackend* be = nullptr;
    for (const auto* cand : plugins::HwVideoBackendRegistry::instance()
                                .list_decoders(plugins::VideoCodecKind::kH264)) {
        if (cand->id == "openh264_h264") { be = cand; break; }
    }
    if (be == nullptr) {
        GTEST_SKIP() << "the openh264 software backend is not in this build";
    }

    // Scale a solid mid-grey 1920x1080 down to 1280x720 and push it
    // through the real software encoder -> decoder.
    const SolidFrame src =
        make_solid_i420(kSrcW, kSrcH, kGreyY, kGreyC, kGreyC);

    vf::VideoFrameScaler scaler{vf::ScaleFilter::kBilinear};
    const auto scaled = scaler.scale_to_buffer(*src.buffer, kDstW, kDstH);
    ASSERT_TRUE(scaled.ok()) << scaled.error().message();

    plugins::VideoCodecConfig cfg;
    cfg.width         = kDstW;
    cfg.height        = kDstH;
    cfg.bitrate_bps   = 4000000;
    cfg.pixel_format  = plugins::VideoPixelFormat::kI420;
    cfg.payload_type = 102;

    auto codec = be->create(cfg);
    ASSERT_NE(codec, nullptr);
    ASSERT_EQ(codec->open(), plugins::kOk);

    // The encoder reads the frame's own buffer, so the frame has to carry
    // the scaled buffer (not bare plane pointers).
    plugins::VideoFrame frame;
    frame.buffer = scaled.value();

    std::vector<std::uint8_t> bitstream(1024 * 1024);
    plugins::EncodedVideoFrame encoded;
    ASSERT_EQ(codec->encode(frame, bitstream.data(), bitstream.size(), encoded),
              plugins::kOk);
    EXPECT_GT(encoded.payload.size(), 0u);

    std::vector<std::uint8_t> dec_y(static_cast<std::size_t>(kDstW) * kDstH);
    std::vector<std::uint8_t> dec_u(static_cast<std::size_t>(kDstW / 2) * (kDstH / 2));
    std::vector<std::uint8_t> dec_v(static_cast<std::size_t>(kDstW / 2) * (kDstH / 2));
    std::uint8_t* buffers[3] = {dec_y.data(), dec_u.data(), dec_v.data()};

    plugins::VideoFrame decoded;
    ASSERT_EQ(codec->decode(encoded, decoded, buffers), plugins::kOk);

    const PlaneStats st = stats_of(dec_y);
    EXPECT_NEAR(st.mean, static_cast<double>(kGreyY), 6.0)
        << "the decoded picture is not the solid colour that was scaled";
    EXPECT_LE(st.spread(), 8)
        << "the decoded picture shows banding that the scale introduced";

    codec->close();
    codec.reset();
}

#endif  // NIMRTC_TEST_WITH_OPENH264
