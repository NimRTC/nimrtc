// modules/video_capture/tests/test_unix_capture_core.cpp
// ----------------------------------------------------------------------------
// The portable capture core's tests (remote-desktop plan A4 / A5 / A7 /
// A8, building on A2b).
//
// These are the tests that matter most for the macOS and Linux
// backends, because they run on **every** platform -- including the
// Windows CI leg, where the macOS and Linux adapters cannot even be
// compiled.  Everything the adapters delegate to the portable core is
// verified here:
//
//   - the pixel-diff dirty-rect detection (plan A7),
//   - the region crop (plan A8),
//   - the format negotiation (plan A1's frozen semantics),
//   - the queue integration (the drop policy, the sequence numbers, the
//     statistics),
//   - and the whole `IVideoCapture` contract, driven by a **fake
//     platform implementation** -- which proves the base class works
//     end-to-end without any OS.
//
// The fake platform is the important one: it is the same shape the
// real adapters implement, so a test that passes here proves the
// adapters' contract is right even though the adapters themselves are
// not compiled on this leg.
// ----------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <nimrtc/video_capture/unix_capture.hpp>
#include <nimrtc/video_capture/unix_capture_core.hpp>
#include <string>
#include <thread>
#include <vector>

namespace plugins = nimrtc::plugins;

using nimrtc::plugins::CaptureCapabilities;
using nimrtc::plugins::CaptureConfig;
using nimrtc::plugins::CaptureDevice;
using nimrtc::plugins::CaptureDeviceKind;
using nimrtc::plugins::CaptureDirtyRects;
using nimrtc::plugins::CaptureDropPolicy;
using nimrtc::plugins::CaptureFormat;
using nimrtc::plugins::CaptureFrame;
using nimrtc::plugins::CaptureRegion;
using nimrtc::video_capture::clamp_region;
using nimrtc::video_capture::compute_pixel_dirty_rects;
using nimrtc::video_capture::crop_frame_bgra;
using nimrtc::video_capture::negotiate_capture_format;
using nimrtc::video_capture::UnixCaptureCore;
using nimrtc::video_capture::UnixCaptureDevice;
using nimrtc::video_capture::UnixCaptureDiffOptions;
using nimrtc::video_capture::UnixCaptureError;
using nimrtc::video_capture::UnixCaptureOptions;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/** A BGRA frame of @p width x @p height, filled with @p value. */
std::vector<std::uint8_t> make_frame(std::uint32_t width, std::uint32_t height,
                                     std::uint8_t value = 0) {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4u, value);
}

/** Paint a rectangle into a BGRA frame. */
void paint_rect(std::vector<std::uint8_t>& frame, std::uint32_t width, std::uint32_t x,
                std::uint32_t y, std::uint32_t w, std::uint32_t h, std::uint8_t value) {
    const std::uint32_t height =
        static_cast<std::uint32_t>(frame.size() / (static_cast<std::size_t>(width) * 4u));
    for (std::uint32_t row = y; row < y + h && row < height; ++row) {
        for (std::uint32_t col = x; col < x + w; ++col) {
            std::uint8_t* p = frame.data() + (static_cast<std::size_t>(row) * width + col) * 4u;
            p[0] = value;
            p[1] = value;
            p[2] = value;
            p[3] = 0xFF;
        }
    }
}

/** The union of the rectangles in a dirty-rect set. */
CaptureRegion union_rects(const CaptureDirtyRects& dirty) {
    CaptureRegion out{};
    for (const auto& r : dirty.rects) {
        if (out.width == 0 && out.height == 0) {
            out = r;
            continue;
        }
        const std::uint32_t x0 = out.x < r.x ? out.x : r.x;
        const std::uint32_t y0 = out.y < r.y ? out.y : r.y;
        const std::uint32_t x1 =
            (out.x + out.width) > (r.x + r.width) ? (out.x + out.width) : (r.x + r.width);
        const std::uint32_t y1 =
            (out.y + out.height) > (r.y + r.height) ? (out.y + out.height) : (r.y + r.height);
        out.x = x0;
        out.y = y0;
        out.width = x1 - x0;
        out.height = y1 - y0;
    }
    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// The pixel-diff (plan A7)
// ---------------------------------------------------------------------------

TEST(UnixCaptureDiff, AnUnchangedFrameReportsNoChange) {
    const auto a = make_frame(64, 48, 0x40);
    const auto b = make_frame(64, 48, 0x40);

    UnixCaptureDiffOptions opts;
    const CaptureDirtyRects dirty = compute_pixel_dirty_rects(a.data(), b.data(), 64, 48, opts);

    EXPECT_FALSE(dirty.full_frame);
    EXPECT_TRUE(dirty.rects.empty());
}

TEST(UnixCaptureDiff, AChangedRectangleIsReported) {
    auto a = make_frame(64, 48, 0x40);
    auto b = a;
    // A 10x8 rectangle at (20, 12).
    paint_rect(b, 64, 20, 12, 10, 8, 0xFF);

    UnixCaptureDiffOptions opts;
    opts.merge_threshold = 0; // merge only directly-adjacent runs
    const CaptureDirtyRects dirty = compute_pixel_dirty_rects(a.data(), b.data(), 64, 48, opts);

    ASSERT_FALSE(dirty.full_frame);
    ASSERT_EQ(dirty.rects.size(), 1u);
    EXPECT_EQ(dirty.rects[0].x, 20u);
    EXPECT_EQ(dirty.rects[0].y, 12u);
    EXPECT_EQ(dirty.rects[0].width, 10u);
    EXPECT_EQ(dirty.rects[0].height, 8u);
}

TEST(UnixCaptureDiff, ALargeChangeIsMergedIntoOneBox) {
    auto a = make_frame(64, 48, 0x40);
    auto b = a;
    // Two small rectangles 4 pixels apart: with a merge threshold of
    // 16 they collapse into one box that covers both.
    paint_rect(b, 64, 10, 10, 4, 4, 0xFF);
    paint_rect(b, 64, 18, 10, 4, 4, 0xFF);

    UnixCaptureDiffOptions opts;
    opts.merge_threshold = 16;
    const CaptureDirtyRects dirty = compute_pixel_dirty_rects(a.data(), b.data(), 64, 48, opts);

    ASSERT_FALSE(dirty.full_frame);
    ASSERT_EQ(dirty.rects.size(), 1u);
    // The merged box covers both rectangles.
    EXPECT_LE(dirty.rects[0].x, 10u);
    EXPECT_GE(dirty.rects[0].x + dirty.rects[0].width, 22u);
    EXPECT_EQ(dirty.rects[0].y, 10u);
    EXPECT_EQ(dirty.rects[0].height, 4u);
}

TEST(UnixCaptureDiff, FarApartChangesAreNotMerged) {
    auto a = make_frame(64, 48, 0x40);
    auto b = a;
    paint_rect(b, 64, 2, 2, 4, 4, 0xFF);
    paint_rect(b, 64, 50, 40, 4, 4, 0xFF);

    UnixCaptureDiffOptions opts;
    opts.merge_threshold = 4;
    const CaptureDirtyRects dirty = compute_pixel_dirty_rects(a.data(), b.data(), 64, 48, opts);

    ASSERT_FALSE(dirty.full_frame);
    EXPECT_EQ(dirty.rects.size(), 2u);
}

TEST(UnixCaptureDiff, TheDisabledDiffReportsTheFullFrame) {
    auto a = make_frame(64, 48, 0x40);
    auto b = a;

    UnixCaptureDiffOptions opts;
    opts.enabled = false;
    const CaptureDirtyRects dirty = compute_pixel_dirty_rects(a.data(), b.data(), 64, 48, opts);

    // The honest answer: "the whole frame changed".  Never "nothing
    // changed" -- that is a claim the encoder acts on.
    EXPECT_TRUE(dirty.full_frame);
}

TEST(UnixCaptureDiff, NoPreviousFrameReportsTheFullFrame) {
    const auto b = make_frame(64, 48, 0x40);

    UnixCaptureDiffOptions opts;
    const CaptureDirtyRects dirty = compute_pixel_dirty_rects(nullptr, b.data(), 64, 48, opts);

    EXPECT_TRUE(dirty.full_frame);
}

TEST(UnixCaptureDiff, AStridedFrameIsComparedCorrectly) {
    // A frame whose rows are padded (a stride wider than the packed
    // row) must still be compared correctly -- otherwise the diff
    // would read across the padding and report phantom changes.
    const std::uint32_t width = 8;
    const std::uint32_t height = 4;
    const std::uint32_t packed = width * 4u;
    const std::uint32_t stride = packed + 16u;

    auto make_strided = [&](std::uint8_t value) {
        std::vector<std::uint8_t> out(static_cast<std::size_t>(stride) * height, 0xEE);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                std::uint8_t* p = out.data() + static_cast<std::size_t>(y) * stride + x * 4u;
                p[0] = value;
                p[1] = value;
                p[2] = value;
                p[3] = 0xFF;
            }
        }
        return out;
    };

    const auto a = make_strided(0x10);
    const auto b = make_strided(0x10);

    UnixCaptureDiffOptions opts;
    opts.stride = static_cast<std::int32_t>(stride);
    opts.previous_stride = static_cast<std::int32_t>(stride);
    const CaptureDirtyRects dirty =
        compute_pixel_dirty_rects(a.data(), b.data(), width, height, opts);
    EXPECT_FALSE(dirty.full_frame);
    EXPECT_TRUE(dirty.rects.empty());
}

TEST(UnixCaptureDiff, TheReportedRectanglesCoverEveryChangedPixel) {
    auto a = make_frame(32, 32, 0x10);
    auto b = a;
    paint_rect(b, 32, 5, 6, 7, 9, 0xEE);

    UnixCaptureDiffOptions opts;
    opts.merge_threshold = 0;
    const CaptureDirtyRects dirty = compute_pixel_dirty_rects(a.data(), b.data(), 32, 32, opts);

    ASSERT_FALSE(dirty.full_frame);
    ASSERT_FALSE(dirty.rects.empty());
    for (const auto& r : dirty.rects) {
        EXPECT_LE(r.x, 5u);
        EXPECT_LE(r.y, 6u);
        EXPECT_GE(r.x + r.width, 12u);
        EXPECT_GE(r.y + r.height, 15u);
    }
}

// ---------------------------------------------------------------------------
// The region crop (plan A8)
// ---------------------------------------------------------------------------

TEST(UnixCaptureCrop, ProducesExactlyTheRequestedSize) {
    const auto src = make_frame(100, 80, 0x33);

    CaptureRegion region;
    region.x = 10;
    region.y = 20;
    region.width = 30;
    region.height = 40;

    std::vector<std::uint8_t> out;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    std::int32_t stride = 0;
    ASSERT_TRUE(crop_frame_bgra(src.data(), src.size(), 100, 80, static_cast<std::int32_t>(100 * 4),
                                region, out, w, h, stride));
    EXPECT_EQ(w, 30u);
    EXPECT_EQ(h, 40u);
    EXPECT_EQ(stride, static_cast<std::int32_t>(30 * 4));
    EXPECT_EQ(out.size(), static_cast<std::size_t>(30 * 40 * 4));
}

TEST(UnixCaptureCrop, TheContentIsTheRequestedWindow) {
    auto src = make_frame(100, 80, 0x00);
    // Mark the region so it is distinguishable from the background.
    paint_rect(src, 100, 10, 20, 30, 40, 0x77);

    CaptureRegion region;
    region.x = 10;
    region.y = 20;
    region.width = 30;
    region.height = 40;

    std::vector<std::uint8_t> out;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    std::int32_t stride = 0;
    ASSERT_TRUE(crop_frame_bgra(src.data(), src.size(), 100, 80, static_cast<std::int32_t>(100 * 4),
                                region, out, w, h, stride));

    // The first pixel of the crop is the marked pixel.
    EXPECT_EQ(out[0], 0x77);
    EXPECT_EQ(out[3], 0xFF); // the alpha is forced opaque
}

TEST(UnixCaptureCrop, AZeroSizedRegionPassesTheFrameThrough) {
    const auto src = make_frame(16, 16, 0x22);

    CaptureRegion region; // zero-sized: "the whole surface"

    std::vector<std::uint8_t> out;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    std::int32_t stride = 0;
    ASSERT_TRUE(crop_frame_bgra(src.data(), src.size(), 16, 16, 64, region, out, w, h, stride));
    EXPECT_EQ(w, 16u);
    EXPECT_EQ(h, 16u);
    EXPECT_EQ(out.size(), src.size());
}

TEST(UnixCaptureCrop, ARegionOutsideTheFrameIsClamped) {
    CaptureRegion region;
    region.x = 90;
    region.y = 70;
    region.width = 100;
    region.height = 100;

    const CaptureRegion clamped = clamp_region(region, 100, 80);
    EXPECT_EQ(clamped.x, 90u);
    EXPECT_EQ(clamped.width, 10u);
    EXPECT_EQ(clamped.height, 10u);
}

TEST(UnixCaptureCrop, ARegionFullyOutsideTheFrameCollapses) {
    CaptureRegion region;
    region.x = 200;
    region.y = 200;
    region.width = 10;
    region.height = 10;

    const CaptureRegion clamped = clamp_region(region, 100, 80);
    EXPECT_EQ(clamped.width, 0u);
    EXPECT_EQ(clamped.height, 0u);
}

// ---------------------------------------------------------------------------
// The format negotiation
// ---------------------------------------------------------------------------

TEST(UnixCaptureNegotiation, ARequestLargerThanTheDeviceIsClamped) {
    CaptureDevice device;
    device.id = "test:0";
    device.max_width = 1920;
    device.max_height = 1080;

    CaptureFormat requested;
    requested.width = 3840;
    requested.height = 2160;

    const CaptureFormat negotiated =
        negotiate_capture_format(device, requested, "bgra", false, false);

    // Never upscale.
    EXPECT_EQ(negotiated.width, 1920u);
    EXPECT_EQ(negotiated.height, 1080u);
}

TEST(UnixCaptureNegotiation, AZeroSizedRequestMeansTheDevicesBest) {
    CaptureDevice device;
    device.id = "test:0";
    device.max_width = 2560;
    device.max_height = 1440;

    const CaptureFormat negotiated =
        negotiate_capture_format(device, CaptureFormat{}, "bgra", false, false);

    EXPECT_EQ(negotiated.width, 2560u);
    EXPECT_EQ(negotiated.height, 1440u);
}

TEST(UnixCaptureNegotiation, TheTagIsFilledIn) {
    CaptureDevice device;
    device.id = "test:0";
    device.max_width = 800;
    device.max_height = 600;

    CaptureFormat requested;
    requested.tag = "custom-tag";

    // An explicit tag is preserved...
    const CaptureFormat kept = negotiate_capture_format(device, requested, "bgra", false, false);
    EXPECT_EQ(kept.tag, "custom-tag");

    // ...and an empty one is filled in with the backend's default.
    const CaptureFormat filled =
        negotiate_capture_format(device, CaptureFormat{}, "bgra-x11", false, false);
    EXPECT_EQ(filled.tag, "bgra-x11");
}

TEST(UnixCaptureNegotiation, ZeroCopyIsReportedHonestly) {
    CaptureDevice device;
    device.id = "test:0";
    device.max_width = 800;
    device.max_height = 600;

    // A backend that cannot do zero-copy says so, even when the caller
    // asked for it.
    CaptureFormat requested;
    requested.zero_copy = true;
    const CaptureFormat cpu_only =
        negotiate_capture_format(device, requested, "bgra", false, false);
    EXPECT_FALSE(cpu_only.zero_copy);

    const CaptureFormat gpu = negotiate_capture_format(device, requested, "bgra", true, false);
    EXPECT_TRUE(gpu.zero_copy);
}

TEST(UnixCaptureNegotiation, DirtyRectsAreReportedHonestly) {
    CaptureDevice device;
    device.id = "test:0";
    device.max_width = 800;
    device.max_height = 600;

    const CaptureFormat with =
        negotiate_capture_format(device, CaptureFormat{}, "bgra", false, true);
    EXPECT_TRUE(with.has_dirty_rects);

    const CaptureFormat without =
        negotiate_capture_format(device, CaptureFormat{}, "bgra", false, false);
    EXPECT_FALSE(without.has_dirty_rects);
}

// ---------------------------------------------------------------------------
// The core: the queue integration
// ---------------------------------------------------------------------------

TEST(UnixCaptureCore, TheFramesComeOutInOrderWithTheirMetadata) {
    UnixCaptureCore core{CaptureConfig{}, UnixCaptureOptions{}};

    const auto frame = make_frame(16, 16, 0x55);
    ASSERT_EQ(core.submit_frame(frame.data(), frame.size(), 64, 16, 16, 1000, nullptr),
              nimrtc::plugins::kOk);
    ASSERT_EQ(core.submit_frame(frame.data(), frame.size(), 64, 16, 16, 2000, nullptr),
              nimrtc::plugins::kOk);

    CaptureFrame out{};
    ASSERT_EQ(core.pop_frame(out), nimrtc::plugins::kOk);
    EXPECT_EQ(out.capture_ts_us, 1000);
    EXPECT_EQ(out.frame_seq, 0u);
    EXPECT_EQ(out.width, 16u);
    EXPECT_EQ(out.height, 16u);
    EXPECT_EQ(out.stride, 64);

    ASSERT_EQ(core.pop_frame(out), nimrtc::plugins::kOk);
    EXPECT_EQ(out.capture_ts_us, 2000);
    EXPECT_EQ(out.frame_seq, 1u);

    EXPECT_EQ(core.pop_frame(out), nimrtc::plugins::kErrNotReady);
}

TEST(UnixCaptureCore, TheFrameDeclaresItsPixelFormat) {
    UnixCaptureCore core{CaptureConfig{}, UnixCaptureOptions{}};
    const auto frame = make_frame(8, 8, 0x01);
    ASSERT_EQ(core.submit_frame(frame.data(), frame.size(), 32, 8, 8, 1, nullptr),
              nimrtc::plugins::kOk);

    CaptureFrame out{};
    ASSERT_EQ(core.pop_frame(out), nimrtc::plugins::kOk);
    EXPECT_EQ(out.format, nimrtc::plugins::VideoPixelFormat::kBGRA);
    EXPECT_FALSE(out.has_gpu_handle);
    EXPECT_EQ(out.gpu_backend, nimrtc::plugins::GpuBackend::kNone);
}

TEST(UnixCaptureCore, TheDropPolicyIsHonoured) {
    CaptureConfig cfg{};
    cfg.queue_depth = 2;
    cfg.drop_policy = CaptureDropPolicy::kDropOldest;
    UnixCaptureCore core{cfg, UnixCaptureOptions{}};

    const auto frame = make_frame(8, 8, 0x01);
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(core.submit_frame(frame.data(), frame.size(), 32, 8, 8, i, nullptr),
                  nimrtc::plugins::kOk);
    }

    // The queue holds 2; the other 3 were dropped as the oldest.
    EXPECT_EQ(core.stats().frames_dropped, 3u);
    EXPECT_EQ(core.stats().frames_captured, 5u);

    CaptureFrame out{};
    ASSERT_EQ(core.pop_frame(out), nimrtc::plugins::kOk);
    EXPECT_EQ(out.frame_seq, 3u);
    ASSERT_EQ(core.pop_frame(out), nimrtc::plugins::kOk);
    EXPECT_EQ(out.frame_seq, 4u);
    EXPECT_EQ(core.pop_frame(out), nimrtc::plugins::kErrNotReady);
}

TEST(UnixCaptureCore, TheStatisticsMove) {
    UnixCaptureCore core{CaptureConfig{}, UnixCaptureOptions{}};
    const auto frame = make_frame(8, 8, 0x01);

    EXPECT_EQ(core.stats().frames_captured, 0u);
    ASSERT_EQ(core.submit_frame(frame.data(), frame.size(), 32, 8, 8, 1, nullptr),
              nimrtc::plugins::kOk);
    EXPECT_EQ(core.stats().frames_captured, 1u);
    EXPECT_EQ(core.stats().frames_cpu_staged, 1u);
    EXPECT_EQ(core.stats().frames_zero_copy, 0u);

    core.note_device_error();
    core.note_device_restart();
    core.note_format_change();
    EXPECT_EQ(core.stats().device_errors, 1u);
    EXPECT_EQ(core.stats().device_restarts, 1u);
    EXPECT_EQ(core.stats().format_changes, 1u);

    core.reset_stats();
    EXPECT_EQ(core.stats().frames_captured, 0u);
    EXPECT_EQ(core.stats().device_errors, 0u);
}

TEST(UnixCaptureCore, TheRegionSelectsTheFrameSize) {
    CaptureConfig cfg{};
    cfg.region.x = 4;
    cfg.region.y = 4;
    cfg.region.width = 8;
    cfg.region.height = 6;

    UnixCaptureCore core{cfg, UnixCaptureOptions{}};
    const auto frame = make_frame(32, 32, 0x33);
    ASSERT_EQ(core.submit_frame(frame.data(), frame.size(), 128, 32, 32, 1, nullptr),
              nimrtc::plugins::kOk);

    CaptureFrame out{};
    ASSERT_EQ(core.pop_frame(out), nimrtc::plugins::kOk);
    EXPECT_EQ(out.width, 8u);
    EXPECT_EQ(out.height, 6u);
    EXPECT_EQ(out.size, static_cast<std::size_t>(8 * 6 * 4));
}

TEST(UnixCaptureCore, TheCropIsCountedAsACopy) {
    CaptureConfig cfg{};
    cfg.region.width = 8;
    cfg.region.height = 8;

    UnixCaptureCore core{cfg, UnixCaptureOptions{}};
    const auto frame = make_frame(32, 32, 0x33);
    ASSERT_EQ(core.submit_frame(frame.data(), frame.size(), 128, 32, 32, 1, nullptr),
              nimrtc::plugins::kOk);

    // The crop is a real copy, so the statistics say so.
    EXPECT_GT(core.stats().bytes_copied, 0u);
}

TEST(UnixCaptureCore, TheDiffSuppliesTheDirtyRectangles) {
    UnixCaptureCore core{CaptureConfig{}, UnixCaptureOptions{}};

    auto frame = make_frame(32, 32, 0x10);
    ASSERT_EQ(core.submit_frame(frame.data(), frame.size(), 128, 32, 32, 1, nullptr),
              nimrtc::plugins::kOk);

    // The second frame has a changed rectangle.
    auto changed = frame;
    paint_rect(changed, 32, 4, 4, 6, 6, 0xFF);
    ASSERT_EQ(core.submit_frame(changed.data(), changed.size(), 128, 32, 32, 2, nullptr),
              nimrtc::plugins::kOk);

    CaptureFrame out{};
    ASSERT_EQ(core.pop_frame(out), nimrtc::plugins::kOk);
    ASSERT_EQ(core.pop_frame(out), nimrtc::plugins::kOk);

    // The second frame carries the diff rectangle.
    EXPECT_FALSE(out.dirty.full_frame);
    ASSERT_FALSE(out.dirty.rects.empty());
    EXPECT_EQ(out.dirty.rects[0].x, 4u);
    EXPECT_EQ(out.dirty.rects[0].y, 4u);
}

TEST(UnixCaptureCore, ADisabledDiffReportsTheFullFrame) {
    UnixCaptureOptions opts{};
    opts.enable_pixel_diff = false;
    UnixCaptureCore core{CaptureConfig{}, opts};

    const auto frame = make_frame(16, 16, 0x10);
    ASSERT_EQ(core.submit_frame(frame.data(), frame.size(), 64, 16, 16, 1, nullptr),
              nimrtc::plugins::kOk);

    CaptureFrame out{};
    ASSERT_EQ(core.pop_frame(out), nimrtc::plugins::kOk);
    EXPECT_TRUE(out.dirty.full_frame);
}

TEST(UnixCaptureCore, TheCallbackSeesTheQueuedFrame) {
    UnixCaptureCore core{CaptureConfig{}, UnixCaptureOptions{}};

    std::atomic<int> calls{0};
    std::uint32_t seen_seq = 0;
    core.set_frame_callback([&](const CaptureFrame& f) {
        ++calls;
        seen_seq = f.frame_seq;
    });

    const auto frame = make_frame(8, 8, 0x01);
    ASSERT_EQ(core.submit_frame(frame.data(), frame.size(), 32, 8, 8, 1, nullptr),
              nimrtc::plugins::kOk);
    EXPECT_EQ(calls.load(), 1);
    EXPECT_EQ(seen_seq, 0u);
}

TEST(UnixCaptureCore, TheZeroCopyPathCarriesTheGpuHandle) {
    UnixCaptureCore core{CaptureConfig{}, UnixCaptureOptions{}};

    int handle = 0;
    auto owner = std::shared_ptr<void>(&handle, [](void*) {});
    ASSERT_EQ(core.submit_gpu_frame(nullptr, 0, 0, 16, 16, 1,
                                    nimrtc::plugins::GpuBackend::kAppleCFPixelBuffer, &handle, 0,
                                    nullptr, std::move(owner)),
              nimrtc::plugins::kOk);

    CaptureFrame out{};
    ASSERT_EQ(core.pop_frame(out), nimrtc::plugins::kOk);
    EXPECT_TRUE(out.has_gpu_handle);
    EXPECT_EQ(out.gpu_backend, nimrtc::plugins::GpuBackend::kAppleCFPixelBuffer);
    EXPECT_EQ(out.gpu_handle, &handle);
    EXPECT_EQ(core.stats().frames_zero_copy, 1u);
    EXPECT_EQ(core.stats().frames_cpu_staged, 0u);
}

// ---------------------------------------------------------------------------
// The UnixCaptureDevice base class, driven by a fake platform
// ---------------------------------------------------------------------------

namespace {

/**
 * @brief A fake platform backend.
 *
 * It implements the six `platform_*` hooks exactly the way a real
 * adapter does, and it produces synthetic frames.  It is the proof that
 * the base class satisfies the whole `IVideoCapture` contract without
 * any OS being involved.
 */
class FakePlatformCapture final : public UnixCaptureDevice {
  public:
    FakePlatformCapture(CaptureConfig cfg, UnixCaptureOptions opts = {})
        : UnixCaptureDevice(cfg, opts) {}

    void set_produce(bool produce) noexcept { produce_.store(produce, std::memory_order_release); }

    [[nodiscard]] std::uint32_t poll_count() const noexcept {
        return polls_.load(std::memory_order_acquire);
    }

  protected:
    [[nodiscard]] std::vector<CaptureDevice> platform_enumerate_devices() const noexcept override {
        CaptureDevice device;
        device.id = "fake:0";
        device.name = "Fake display";
        device.kind = CaptureDeviceKind::kScreen;
        device.max_width = 640;
        device.max_height = 480;
        device.max_fps = 30;
        return {device};
    }

    [[nodiscard]] std::vector<CaptureFormat>
    platform_supported_formats(const std::string& device_id) const noexcept override {
        (void)device_id;
        std::vector<CaptureFormat> formats;
        const std::uint32_t sizes[][2] = {{320, 240}, {640, 480}};
        for (const auto& size : sizes) {
            CaptureFormat format;
            format.width = size[0];
            format.height = size[1];
            format.fps = 30;
            format.tag = "bgra-fake";
            format.zero_copy = false;
            format.has_dirty_rects = true;
            formats.push_back(std::move(format));
        }
        return formats;
    }

    plugins::Status platform_open_device(UnixCaptureError& error) noexcept override {
        if (!open_ok_) {
            error = UnixCaptureError::kPermissionDenied;
            return nimrtc::plugins::kErrHardwareError;
        }
        CaptureFormat negotiated;
        negotiated.width = 640;
        negotiated.height = 480;
        negotiated.fps = 30;
        negotiated.tag = "bgra-fake";
        negotiated.zero_copy = false;
        negotiated.has_dirty_rects = true;
        set_negotiated_format(negotiated);
        return nimrtc::plugins::kOk;
    }

    void platform_close_device() noexcept override { closed_ = true; }

    [[nodiscard]] plugins::CaptureCapabilities platform_capabilities() const noexcept override {
        plugins::CaptureCapabilities caps;
        caps.zero_copy = false;
        caps.dirty_rects = true;
        caps.cursor_composited = false;
        caps.region_selection = true;
        caps.enumeration = true;
        caps.max_pixels = 640ull * 480ull;
        caps.max_fps = 30;
        caps.backend = "fake";
        return caps;
    }

    void platform_start() noexcept override { started_ = true; }

    void platform_stop() noexcept override { stopped_ = true; }

    plugins::Status platform_poll_frame() noexcept override {
        ++polls_;
        if (!produce_.load(std::memory_order_acquire)) {
            return nimrtc::plugins::kErrNotReady;
        }
        const std::uint32_t w = 640;
        const std::uint32_t h = 480;
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4u,
                                         static_cast<std::uint8_t>(frame_));
        ++frame_;
        return core().submit_frame(pixels.data(), pixels.size(), static_cast<std::int32_t>(w * 4),
                                   w, h, static_cast<std::int64_t>(frame_), nullptr);
    }

    [[nodiscard]] std::string_view plugin_name() const noexcept override { return "fake-capture"; }

    [[nodiscard]] std::string_view backend_id() const noexcept override { return "fake"; }

  public:
    void set_open_ok(bool ok) noexcept { open_ok_ = ok; }
    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] bool stopped() const noexcept { return stopped_; }
    [[nodiscard]] bool closed() const noexcept { return closed_; }

  private:
    std::atomic<bool> produce_{false};
    std::atomic<std::uint32_t> polls_{0};
    std::uint8_t frame_ = 0x20;
    bool open_ok_ = true;
    bool started_ = false;
    bool stopped_ = false;
    bool closed_ = false;
};

} // anonymous namespace

TEST(UnixCaptureDevice, EnumeratesTheDevicesThePlatformOffers) {
    FakePlatformCapture cap{CaptureConfig{}};
    const auto devices = cap.enumerate_devices();
    ASSERT_EQ(devices.size(), 1u);
    EXPECT_EQ(devices[0].id, "fake:0");
    EXPECT_EQ(devices[0].kind, CaptureDeviceKind::kScreen);
}

TEST(UnixCaptureDevice, OffersThePlatformsFormats) {
    FakePlatformCapture cap{CaptureConfig{}};
    const auto formats = cap.supported_formats("fake:0");
    ASSERT_EQ(formats.size(), 2u);
    EXPECT_EQ(formats[0].width, 320u);
    EXPECT_EQ(formats[1].width, 640u);
    EXPECT_EQ(formats[0].tag, "bgra-fake");
}

TEST(UnixCaptureDevice, ReportsItsCapabilities) {
    FakePlatformCapture cap{CaptureConfig{}};
    const auto caps = cap.capabilities();
    EXPECT_TRUE(caps.dirty_rects);
    EXPECT_TRUE(caps.region_selection);
    EXPECT_TRUE(caps.enumeration);
    EXPECT_FALSE(caps.zero_copy);
    EXPECT_EQ(caps.backend, "fake");
}

TEST(UnixCaptureDevice, PopBeforeOpenReportsNotReady) {
    FakePlatformCapture cap{CaptureConfig{}};
    CaptureFrame frame{};
    EXPECT_EQ(cap.pop_frame(frame), nimrtc::plugins::kErrNotReady);
}

TEST(UnixCaptureDevice, OpenStartsThePlatformAndTheThread) {
    FakePlatformCapture cap{CaptureConfig{}};
    ASSERT_EQ(cap.open(), nimrtc::plugins::kOk);
    EXPECT_TRUE(cap.is_open());
    EXPECT_TRUE(cap.started());
    EXPECT_EQ(cap.name(), std::string_view("fake-capture"));
    cap.close();
    EXPECT_TRUE(cap.stopped());
    EXPECT_TRUE(cap.closed());
    EXPECT_FALSE(cap.is_open());
}

TEST(UnixCaptureDevice, TheNegotiatedFormatIsReportedBack) {
    FakePlatformCapture cap{CaptureConfig{}};
    ASSERT_EQ(cap.open(), nimrtc::plugins::kOk);
    const auto cfg = cap.config();
    EXPECT_EQ(cfg.format.width, 640u);
    EXPECT_EQ(cfg.format.height, 480u);
    EXPECT_EQ(cfg.format.tag, "bgra-fake");
    cap.close();
}

TEST(UnixCaptureDevice, TheCaptureThreadFeedsTheQueue) {
    FakePlatformCapture cap{CaptureConfig{}};
    ASSERT_EQ(cap.open(), nimrtc::plugins::kOk);
    cap.set_produce(true);

    // Wait for the capture thread to deliver a frame.
    CaptureFrame frame{};
    nimrtc::plugins::Status status = nimrtc::plugins::kErrNotReady;
    for (int i = 0; i < 200 && status != nimrtc::plugins::kOk; ++i) {
        status = cap.pop_frame(frame);
        if (status != nimrtc::plugins::kOk) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ASSERT_EQ(status, nimrtc::plugins::kOk);
    EXPECT_EQ(frame.width, 640u);
    EXPECT_EQ(frame.height, 480u);
    EXPECT_EQ(frame.format, nimrtc::plugins::VideoPixelFormat::kBGRA);
    EXPECT_GT(cap.stats().frames_captured, 0u);
    cap.close();
}

TEST(UnixCaptureDevice, ThePermissionDenialIsACleanError) {
    FakePlatformCapture cap{CaptureConfig{}};
    cap.set_open_ok(false);

    UnixCaptureError error = UnixCaptureError::kNone;
    std::string message;
    cap.set_error_callback([&](UnixCaptureError e, std::string_view m) {
        error = e;
        message = std::string(m);
    });

    EXPECT_EQ(cap.open(), nimrtc::plugins::kErrHardwareError);
    EXPECT_FALSE(cap.is_open());
    EXPECT_EQ(error, UnixCaptureError::kPermissionDenied);
    EXPECT_EQ(cap.last_error(), UnixCaptureError::kPermissionDenied);
    EXPECT_FALSE(message.empty());
}

TEST(UnixCaptureDevice, TheCallbackFiresOnTheCaptureThread) {
    FakePlatformCapture cap{CaptureConfig{}};
    ASSERT_EQ(cap.open(), nimrtc::plugins::kOk);

    std::atomic<int> calls{0};
    cap.set_frame_callback([&](const CaptureFrame&) { ++calls; });
    cap.set_produce(true);

    for (int i = 0; i < 200 && calls.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_GT(calls.load(), 0);
    cap.close();
}

TEST(UnixCaptureDevice, ReopenAfterAFailureWorks) {
    FakePlatformCapture cap{CaptureConfig{}};
    cap.set_open_ok(false);
    EXPECT_EQ(cap.open(), nimrtc::plugins::kErrHardwareError);

    cap.set_open_ok(true);
    EXPECT_EQ(cap.reopen(), nimrtc::plugins::kOk);
    EXPECT_TRUE(cap.is_open());
    EXPECT_EQ(cap.stats().device_restarts, 1u);
    cap.close();
}

// ---------------------------------------------------------------------------
// The availability probes and the factories
// ---------------------------------------------------------------------------

TEST(UnixCaptureFactories, TheAvailabilityProbesAgreeWithTheBuild) {
    // On a Windows build none of the Unix backends exist; on macOS the
    // ScreenCaptureKit one does; on Linux the X11 / PipeWire ones do
    // (when the development packages were found).
#if defined(__APPLE__)
    EXPECT_TRUE(nimrtc::video_capture::screen_capture_kit_available());
    EXPECT_FALSE(nimrtc::video_capture::pipewire_capture_available());
    EXPECT_FALSE(nimrtc::video_capture::x11_capture_available());
#elif defined(__linux__)
    EXPECT_FALSE(nimrtc::video_capture::screen_capture_kit_available());
#else
    EXPECT_FALSE(nimrtc::video_capture::screen_capture_kit_available());
    EXPECT_FALSE(nimrtc::video_capture::pipewire_capture_available());
    EXPECT_FALSE(nimrtc::video_capture::x11_capture_available());
#endif
}

TEST(UnixCaptureFactories, TheIdsAreStable) {
    EXPECT_EQ(nimrtc::video_capture::ScreenCaptureKitFactory{}.id(), "macos-screencapturekit");
    EXPECT_EQ(nimrtc::video_capture::PipeWireCaptureFactory{}.id(), "linux-pipewire");
    EXPECT_EQ(nimrtc::video_capture::X11CaptureFactory{}.id(), "linux-x11");
}

TEST(UnixCaptureFactories, AnUnavailableBackendFailsCleanly) {
    // On a build without the backend, `create()` still returns a device
    // -- and that device fails `open()` with "unsupported" rather than
    // crashing or producing black frames.
    const auto* factory = new nimrtc::video_capture::PipeWireCaptureFactory{};
    std::unique_ptr<nimrtc::plugins::IVideoCapture> device{factory->create(CaptureConfig{})};
    ASSERT_NE(device, nullptr);

    if (!nimrtc::video_capture::pipewire_capture_available()) {
        EXPECT_EQ(device->open(), nimrtc::plugins::kErrUnsupported);
        EXPECT_TRUE(device->enumerate_devices().empty());
    }
    delete factory;
}
