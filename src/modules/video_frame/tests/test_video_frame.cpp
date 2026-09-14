/**
 * @file src/modules/video_frame/tests/test_video_frame.cpp
 * @brief Unit tests for the video_frame module.
 *
 * Covers:
 *   1. Pixel format helper names and classifications.
 *   2. Layout computation (I420 / NV12 / BGRA / odd dimensions).
 *   3. VideoFrameBuffer: allocation, refcount, plane pointers.
 *   4. Pixel format conversions (I420 ↔ NV12 / I420 → BGRA roundtrips).
 *   5. EncodedVideoFrame metadata helpers.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <vector>

#include <nimrtc/video_frame/frame.hpp>

using nimrtc::video_frame::PixelFormat;
using nimrtc::video_frame::VideoFrameBuffer;
using nimrtc::video_frame::VideoFrameLayout;
using nimrtc::video_frame::compute_layout;
using nimrtc::video_frame::convert_i420_to_bgra;
using nimrtc::video_frame::convert_nv12_to_i420;
using nimrtc::video_frame::convert_i420_to_nv12;

// -----------------------------------------------------------------------------
// Pixel format helpers
// -----------------------------------------------------------------------------

TEST(PixelFormat, NameStrings) {
    EXPECT_EQ(nimrtc::video_frame::pixel_format_name(PixelFormat::kI420),  "I420");
    EXPECT_EQ(nimrtc::video_frame::pixel_format_name(PixelFormat::kNV12),  "NV12");
    EXPECT_EQ(nimrtc::video_frame::pixel_format_name(PixelFormat::kBGRA),  "BGRA");
    EXPECT_EQ(nimrtc::video_frame::pixel_format_name(PixelFormat::kUnknown), "Unknown");
}

TEST(PixelFormat, IsPlanarYuv420) {
    EXPECT_TRUE (nimrtc::video_frame::is_planar_yuv_420(PixelFormat::kI420));
    EXPECT_TRUE (nimrtc::video_frame::is_planar_yuv_420(PixelFormat::kNV12));
    EXPECT_TRUE (nimrtc::video_frame::is_planar_yuv_420(PixelFormat::kYV12));
    EXPECT_FALSE(nimrtc::video_frame::is_planar_yuv_420(PixelFormat::kBGRA));
    EXPECT_FALSE(nimrtc::video_frame::is_planar_yuv_420(PixelFormat::kRGB24));
}

TEST(PixelFormat, BitsPerPixel) {
    EXPECT_EQ(nimrtc::video_frame::bits_per_pixel(PixelFormat::kBGRA),  32);
    EXPECT_EQ(nimrtc::video_frame::bits_per_pixel(PixelFormat::kRGBA),  32);
    EXPECT_EQ(nimrtc::video_frame::bits_per_pixel(PixelFormat::kRGB24), 24);
    EXPECT_EQ(nimrtc::video_frame::bits_per_pixel(PixelFormat::kI420),  0);   // planar
}

// -----------------------------------------------------------------------------
// Layout computation
// -----------------------------------------------------------------------------

TEST(VideoFrameLayout, I420StandardSize) {
    auto l = compute_layout(PixelFormat::kI420, 1280, 720);
    EXPECT_EQ(l.format,  PixelFormat::kI420);
    EXPECT_EQ(l.width,   1280u);
    EXPECT_EQ(l.height,  720u);
    EXPECT_EQ(l.num_planes, 3);
    // Y plane stride is aligned to 16.
    EXPECT_GE(l.strides[0], 1280);
    EXPECT_EQ(l.strides[0] % 16, 0);
    // U/V strides = half Y stride (4:2:0), aligned to 16.
    EXPECT_GE(l.strides[1], 640);
    EXPECT_EQ(l.strides[1], l.strides[2]);
    // U plane starts after Y plane.
    EXPECT_EQ(l.plane_offsets[1], static_cast<std::size_t>(l.strides[0]) * 720);
    EXPECT_GT(l.plane_offsets[2], l.plane_offsets[1]);
    EXPECT_GE(l.buffer_size, l.plane_offsets[2]);
}

TEST(VideoFrameLayout, NV12Has2Planes) {
    auto l = compute_layout(PixelFormat::kNV12, 640, 480);
    EXPECT_EQ(l.num_planes, 2);
    EXPECT_GT(l.strides[0],  0);
    EXPECT_GT(l.strides[1],  0);
    EXPECT_EQ(l.strides[2],  0);
    EXPECT_EQ(l.plane_offsets[1],
              static_cast<std::size_t>(l.strides[0]) * 480);
}

TEST(VideoFrameLayout, BGRAIsSinglePlane) {
    auto l = compute_layout(PixelFormat::kBGRA, 100, 100);
    EXPECT_EQ(l.num_planes, 1);
    EXPECT_GE(l.strides[0], 100 * 4);   // 4 bytes/pixel
    EXPECT_EQ(l.plane_offsets[0], 0u);
    EXPECT_EQ(l.plane_offsets[1], 0u);
    EXPECT_EQ(l.plane_offsets[2], 0u);
}

TEST(VideoFrameLayout, OddDimensions) {
    // 3x3 frame — half-resolution chroma is (3+1)/2 = 2.
    auto l = compute_layout(PixelFormat::kI420, 3, 3);
    EXPECT_EQ(l.width,  3u);
    EXPECT_EQ(l.height, 3u);
    // Stride must round up to alignment.
    EXPECT_GE(l.strides[0], 3);
    EXPECT_GE(l.strides[1], 2);
    EXPECT_EQ(l.strides[1] % 16, 0);
}

TEST(VideoFrameLayout, ZeroDimensionsYieldEmpty) {
    auto l = compute_layout(PixelFormat::kI420, 0, 0);
    EXPECT_EQ(l.buffer_size, 0u);
    EXPECT_EQ(l.num_planes,  0);
}

TEST(VideoFrameLayout, CustomAlignment) {
    auto l = compute_layout_aligned(PixelFormat::kI420, 100, 100, 32);
    EXPECT_EQ(l.strides[0] % 32, 0);
}

// -----------------------------------------------------------------------------
// VideoFrameBuffer
// -----------------------------------------------------------------------------

TEST(VideoFrameBuffer, DefaultConstructedIsEmpty) {
    VideoFrameBuffer buf;
    EXPECT_FALSE(buf.valid());
    EXPECT_EQ  (buf.width(),  0u);
    EXPECT_EQ  (buf.height(), 0u);
    EXPECT_EQ  (buf.size(),   0u);
    EXPECT_EQ  (buf.data(),   nullptr);
}

TEST(VideoFrameBuffer, AllocatedHasValidPlanes) {
    auto layout = compute_layout(PixelFormat::kI420, 64, 48);
    VideoFrameBuffer buf(layout);

    EXPECT_TRUE(buf.valid());
    EXPECT_EQ  (buf.width(),  64u);
    EXPECT_EQ  (buf.height(), 48u);
    EXPECT_EQ  (buf.format(), PixelFormat::kI420);
    EXPECT_NE  (buf.data(),   nullptr);

    EXPECT_NE(buf.plane(0), nullptr);
    EXPECT_NE(buf.plane(1), nullptr);
    EXPECT_NE(buf.plane(2), nullptr);

    // Plane offsets must be monotonically increasing.
    EXPECT_LT(buf.plane(0), buf.plane(1));
    EXPECT_LT(buf.plane(1), buf.plane(2));
}

TEST(VideoFrameBuffer, RefCountIsThreadSafe) {
    auto layout = compute_layout(PixelFormat::kNV12, 32, 32);
    VideoFrameBuffer buf(layout);
    EXPECT_EQ(buf.ref_count(), 1u);

    {
        VideoFrameBuffer copy(buf);   // explicit copy ctor
        EXPECT_EQ(buf.ref_count(),    2u);
        EXPECT_EQ(copy.ref_count(),   2u);
    }
    EXPECT_EQ(buf.ref_count(), 1u);
}

TEST(VideoFrameBuffer, CaptureTimestampAndFrameSeq) {
    auto layout = compute_layout(PixelFormat::kI420, 16, 16);
    VideoFrameBuffer buf(layout);
    EXPECT_EQ(buf.capture_ts_us(), 0);
    EXPECT_EQ(buf.frame_seq(),     0u);

    buf.set_capture_ts_us(1234567890);
    buf.set_frame_seq(42);

    EXPECT_EQ(buf.capture_ts_us(), 1234567890);
    EXPECT_EQ(buf.frame_seq(),     42u);
}

TEST(VideoFrameBuffer, ResetDropsRef) {
    auto layout = compute_layout(PixelFormat::kI420, 32, 32);
    VideoFrameBuffer buf(layout);
    EXPECT_TRUE(buf.valid());

    buf.reset();
    EXPECT_FALSE(buf.valid());
    EXPECT_EQ(buf.ref_count(), 0u);
}

// -----------------------------------------------------------------------------
// I420 → BGRA
// -----------------------------------------------------------------------------

TEST(ConvertI420ToBGRA, SolidGray) {
    constexpr std::uint32_t W = 16, H = 16;
    // Y=128 (mid-gray), U=V=128 (neutral chroma).
    std::vector<std::uint8_t> y(W * H, 128);
    std::vector<std::uint8_t> u((W / 2) * (H / 2), 128);
    std::vector<std::uint8_t> v((W / 2) * (H / 2), 128);
    std::vector<std::uint8_t> bgra(W * H * 4, 0);

    convert_i420_to_bgra(W, H,
                         y.data(), W,
                         u.data(), W / 2,
                         v.data(), W / 2,
                         bgra.data(), W * 4);

    // All pixels should have R=G=B=128 (gray) and A=255.
    for (std::uint32_t j = 0; j < H; ++j) {
        for (std::uint32_t i = 0; i < W; ++i) {
            const std::uint8_t* px = bgra.data() + (j * W + i) * 4;
            EXPECT_GE(px[0], 120u); EXPECT_LE(px[0], 135u) << "B at " << i << "," << j;
            EXPECT_GE(px[1], 120u); EXPECT_LE(px[1], 135u) << "G at " << i << "," << j;
            EXPECT_GE(px[2], 120u); EXPECT_LE(px[2], 135u) << "R at " << i << "," << j;
            EXPECT_EQ(px[3], 0xFFu);
        }
    }
}

TEST(ConvertI420ToBGRA, WhiteIsWhite) {
    constexpr std::uint32_t W = 8, H = 8;
    std::vector<std::uint8_t> y(W * H, 235);     // luma for white
    std::vector<std::uint8_t> u((W / 2) * (H / 2), 128);
    std::vector<std::uint8_t> v((W / 2) * (H / 2), 128);
    std::vector<std::uint8_t> bgra(W * H * 4, 0);

    convert_i420_to_bgra(W, H,
                         y.data(), W,
                         u.data(), W / 2,
                         v.data(), W / 2,
                         bgra.data(), W * 4);

    // White-ish pixel.
    EXPECT_GT(bgra[2], 200u);  // R
    EXPECT_GT(bgra[1], 200u);  // G
    EXPECT_GT(bgra[0], 200u);  // B
}

// -----------------------------------------------------------------------------
// NV12 ↔ I420 roundtrip
// -----------------------------------------------------------------------------

TEST(ConvertNV12ToI420, RoundtripPreservesData) {
    constexpr std::uint32_t W = 32, H = 32;
    // Source: I420 with pattern.
    std::vector<std::uint8_t> y_src(W * H);
    std::vector<std::uint8_t> u_src((W / 2) * (H / 2));
    std::vector<std::uint8_t> v_src((W / 2) * (H / 2));
    std::iota(y_src.begin(), y_src.end(), uint8_t{0});
    std::iota(u_src.begin(), u_src.end(), uint8_t{50});
    std::iota(v_src.begin(), v_src.end(), uint8_t{100});

    // Pack into NV12.
    std::vector<std::uint8_t> uv_src((W / 2) * (H / 2) * 2);
    convert_i420_to_nv12(W, H,
                         y_src.data(), W,
                         u_src.data(), W / 2,
                         v_src.data(), W / 2,
                         y_src.data(), W,                       // overwrite y in place
                         uv_src.data(), W);

    // Now split NV12 → I420.
    std::vector<std::uint8_t> y_dst(W * H, 0);
    std::vector<std::uint8_t> u_dst((W / 2) * (H / 2), 0);
    std::vector<std::uint8_t> v_dst((W / 2) * (H / 2), 0);
    convert_nv12_to_i420(W, H,
                         y_src.data(), W,
                         uv_src.data(), W,
                         y_dst.data(), W,
                         u_dst.data(), W / 2,
                         v_dst.data(), W / 2);

    EXPECT_EQ(y_dst, y_src);  // y_src was overwritten but still contains original
    EXPECT_EQ(u_dst, u_src);
    EXPECT_EQ(v_dst, v_src);
}

// -----------------------------------------------------------------------------
// EncodedVideoFrame helpers
// -----------------------------------------------------------------------------

TEST(EncodedVideoFrame, CodecKindName) {
    EXPECT_EQ(nimrtc::video_frame::codec_name(nimrtc::video_frame::CodecKind::kH264),
              "H264");
    EXPECT_EQ(nimrtc::video_frame::codec_name(nimrtc::video_frame::CodecKind::kVP8),
              "VP8");
    EXPECT_EQ(nimrtc::video_frame::codec_name(nimrtc::video_frame::CodecKind::kVP9),
              "VP9");
}

TEST(EncodedVideoFrame, DefaultFields) {
    nimrtc::video_frame::EncodedVideoFrame f;
    EXPECT_EQ(f.codec, nimrtc::video_frame::CodecKind::kUnknown);
    EXPECT_FALSE(f.is_keyframe);
    EXPECT_EQ(f.payload.size(), 0u);
}
