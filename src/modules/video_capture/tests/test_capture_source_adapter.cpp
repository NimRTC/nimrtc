// modules/video_capture/tests/test_capture_source_adapter.cpp
// ----------------------------------------------------------------------------
// The capture -> frame-stream adapter's tests.
//
// The adapter is the piece that turns a *device* into the plain frame
// stream the engine's video path already consumes.  These tests pin
// down the contract the remote-desktop plan's A2b row names:
//
//   - the adapter opens and closes the device with the source,
//   - it converts the device's pixel layout into the requested one,
//   - it carries the capture timestamp and frame sequence number
//     through to the `VideoSourceFrame`,
//   - it forwards the dirty-rect metadata,
//   - and an empty capture queue is a no-op, not an error.
//
// The device under test is the synthetic one, so these run on every CI
// leg on every platform.
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/capture_source_adapter.hpp>
#include <nimrtc/video_capture/synthetic_capture.hpp>

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

using nimrtc::plugins::CaptureConfig;
using nimrtc::plugins::VideoPixelFormat;
using nimrtc::plugins::VideoSourceConfig;
using nimrtc::plugins::VideoSourceFrame;
using nimrtc::video_capture::CapturePixelFormat;
using nimrtc::video_capture::CaptureSourceAdapter;
using nimrtc::video_capture::SyntheticCapture;
using nimrtc::video_capture::capture_pixel_format_from_tag;
using nimrtc::video_capture::capture_pixel_format_name;

// ---------------------------------------------------------------------------
// The tag mapping
// ---------------------------------------------------------------------------

TEST(CaptureTag, MapsTheKnownLayouts) {
    EXPECT_EQ(capture_pixel_format_from_tag("bgra"), CapturePixelFormat::kBGRA);
    EXPECT_EQ(capture_pixel_format_from_tag("rgba"), CapturePixelFormat::kRGBA);
    EXPECT_EQ(capture_pixel_format_from_tag("nv12"), CapturePixelFormat::kNV12);
    EXPECT_EQ(capture_pixel_format_from_tag("i420"), CapturePixelFormat::kI420);
}

TEST(CaptureTag, IsCaseInsensitive) {
    EXPECT_EQ(capture_pixel_format_from_tag("BGRA"), CapturePixelFormat::kBGRA);
    EXPECT_EQ(capture_pixel_format_from_tag("Nv12"), CapturePixelFormat::kNV12);
}

TEST(CaptureTag, RefusesAnUnknownLayout) {
    // Guessing a pixel layout from an unknown tag would produce
    // garbage pixels rather than an error, so the mapping is strict.
    EXPECT_EQ(capture_pixel_format_from_tag("yuy2"), CapturePixelFormat::kUnknown);
    EXPECT_EQ(capture_pixel_format_from_tag(""), CapturePixelFormat::kUnknown);
    EXPECT_EQ(capture_pixel_format_from_tag("bgra8"), CapturePixelFormat::kUnknown);
}

TEST(CaptureTag, HasNames) {
    EXPECT_EQ(capture_pixel_format_name(CapturePixelFormat::kBGRA), "BGRA");
    EXPECT_EQ(capture_pixel_format_name(CapturePixelFormat::kUnknown), "Unknown");
}

// ---------------------------------------------------------------------------
// The adapter
// ---------------------------------------------------------------------------

namespace {

/** A capture config that produces a small, deterministic picture. */
CaptureConfig small_capture_config() {
    CaptureConfig cfg{};
    cfg.format.width  = 64;
    cfg.format.height = 48;
    cfg.queue_depth   = 4;
    return cfg;
}

} // anonymous namespace

TEST(CaptureSourceAdapter, StartsAndStopsTheDevice) {
    SyntheticCapture cap{small_capture_config()};
    CaptureSourceAdapter adapter{&cap, VideoSourceConfig{}};

    EXPECT_FALSE(adapter.running());
    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);
    EXPECT_TRUE(adapter.running());
    EXPECT_TRUE(cap.is_open());

    adapter.stop();
    EXPECT_FALSE(adapter.running());
    EXPECT_FALSE(cap.is_open());
}

TEST(CaptureSourceAdapter, ReadsTheNegotiatedFormatBack) {
    SyntheticCapture cap{small_capture_config()};
    VideoSourceConfig cfg{};
    cfg.format = VideoPixelFormat::kI420;
    CaptureSourceAdapter adapter{&cap, cfg};

    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);
    // The dimensions come from what the device actually negotiated,
    // not from what the caller guessed: the device is the authority on
    // its own geometry.
    EXPECT_EQ(adapter.config().width, 64u);
    EXPECT_EQ(adapter.config().height, 48u);
    EXPECT_GT(adapter.config().fps, 0u);
}

TEST(CaptureSourceAdapter, ConvertsBgraIntoI420) {
    SyntheticCapture cap{small_capture_config()};
    VideoSourceConfig cfg{};
    cfg.format = VideoPixelFormat::kI420;
    CaptureSourceAdapter adapter{&cap, cfg};

    bool called = false;
    VideoSourceFrame seen{};
    adapter.set_callback([&](const VideoSourceFrame& f) {
        called = true;
        seen = f;
    });

    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);
    ASSERT_EQ(cap.produce_one(), nimrtc::plugins::kOk);
    adapter.produce_one();

    ASSERT_TRUE(called);
    EXPECT_EQ(seen.width, 64u);
    EXPECT_EQ(seen.height, 48u);
    EXPECT_EQ(seen.format, VideoPixelFormat::kI420);
    ASSERT_NE(seen.plane_y, nullptr);
    ASSERT_NE(seen.plane_u, nullptr);
    ASSERT_NE(seen.plane_v, nullptr);
    EXPECT_GE(seen.stride_y, 64);
    EXPECT_GE(seen.stride_u, 32);
    EXPECT_GT(seen.buffer_size, 0u);
}

TEST(CaptureSourceAdapter, PassesTheDeviceLayoutThroughUnchanged) {
    SyntheticCapture cap{small_capture_config()};
    VideoSourceConfig cfg{};
    cfg.format = VideoPixelFormat::kBGRA;
    CaptureSourceAdapter adapter{&cap, cfg};

    bool called = false;
    VideoSourceFrame seen{};
    adapter.set_callback([&](const VideoSourceFrame& f) {
        called = true;
        seen = f;
    });

    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);
    ASSERT_EQ(cap.produce_one(), nimrtc::plugins::kOk);
    adapter.produce_one();

    ASSERT_TRUE(called);
    EXPECT_EQ(seen.format, VideoPixelFormat::kBGRA);
    ASSERT_NE(seen.plane_y, nullptr);
    EXPECT_EQ(seen.stride_y, 64 * 4);
}

TEST(CaptureSourceAdapter, CarriesTheTimestampAndSequence) {
    SyntheticCapture cap{small_capture_config()};
    CaptureSourceAdapter adapter{&cap, VideoSourceConfig{}};

    bool called = false;
    VideoSourceFrame seen{};
    adapter.set_callback([&](const VideoSourceFrame& f) {
        called = true;
        seen = f;
    });

    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(cap.produce_one(), nimrtc::plugins::kOk);
    }
    adapter.produce_one();

    ASSERT_TRUE(called);
    EXPECT_EQ(seen.frame_seq, 0u);
    EXPECT_GT(seen.capture_ts_us, 0);
}

TEST(CaptureSourceAdapter, ForwardsTheDirtyRects) {
    SyntheticCapture cap{small_capture_config()};
    CaptureSourceAdapter adapter{&cap, VideoSourceConfig{}};

    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);

    // A moving desktop: the device reports a changed region.
    ASSERT_EQ(cap.produce_one(), nimrtc::plugins::kOk);
    ASSERT_EQ(cap.produce_one(), nimrtc::plugins::kOk);
    adapter.produce_one();
    EXPECT_FALSE(adapter.last_dirty().full_frame);
    EXPECT_FALSE(adapter.last_dirty().rects.empty());

    // Drain the queue: the capture queue is FIFO, so the frames that
    // were produced while the desktop was still moving are still
    // waiting at the head.
    nimrtc::plugins::CaptureFrame drained{};
    while (cap.pop_frame(drained) == nimrtc::plugins::kOk) {
    }

    // A static desktop: the device reports *no* changed region, which
    // is the property a remote-desktop encoder cares about most.
    cap.set_static_frame(true);
    ASSERT_EQ(cap.produce_one(), nimrtc::plugins::kOk);
    adapter.produce_one();
    EXPECT_FALSE(adapter.last_dirty().full_frame);
    EXPECT_TRUE(adapter.last_dirty().rects.empty());
}

TEST(CaptureSourceAdapter, AnEmptyQueueIsANoOp) {
    SyntheticCapture cap{small_capture_config()};
    CaptureSourceAdapter adapter{&cap, VideoSourceConfig{}};

    bool called = false;
    adapter.set_callback([&](const VideoSourceFrame&) { called = true; });

    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);
    // Nothing was produced, so there is nothing to publish.
    adapter.produce_one();
    EXPECT_FALSE(called);
    EXPECT_EQ(adapter.stats().frames_produced, 0u);
}

TEST(CaptureSourceAdapter, CountsTheFramesItPublished) {
    SyntheticCapture cap{small_capture_config()};
    CaptureSourceAdapter adapter{&cap, VideoSourceConfig{}};

    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(cap.produce_one(), nimrtc::plugins::kOk);
        adapter.produce_one();
    }
    EXPECT_EQ(adapter.stats().frames_produced, 3u);
    EXPECT_EQ(adapter.stats().frames_dropped, 0u);
}

TEST(CaptureSourceAdapter, ReportsTheBackend) {
    SyntheticCapture cap{small_capture_config()};
    CaptureSourceAdapter adapter{&cap, VideoSourceConfig{}};

    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);
    EXPECT_EQ(adapter.hardware_backend(), "synthetic");
    // The synthetic device has no GPU surface, so it is not a
    // hardware-accelerated path.
    EXPECT_FALSE(adapter.is_hardware_accelerated());
}

TEST(CaptureSourceAdapter, StoppingTwiceIsHarmless) {
    SyntheticCapture cap{small_capture_config()};
    CaptureSourceAdapter adapter{&cap, VideoSourceConfig{}};

    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);
    adapter.stop();
    adapter.stop();
    EXPECT_FALSE(adapter.running());
}

TEST(CaptureSourceAdapter, ProducingWhileStoppedIsHarmless) {
    SyntheticCapture cap{small_capture_config()};
    CaptureSourceAdapter adapter{&cap, VideoSourceConfig{}};

    bool called = false;
    adapter.set_callback([&](const VideoSourceFrame&) { called = true; });

    adapter.produce_one();
    EXPECT_FALSE(called);
}
