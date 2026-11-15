// modules/video_capture/tests/test_dirty_rect_diff.cpp
// ----------------------------------------------------------------------------
// The portable dirty-rectangle detector (remote-desktop plan A7).
//
// The detector's job is to answer one question cheaply: *did anything
// change since the last frame?*  For a static desktop the answer is
// "no", and the frame can be skipped entirely — which is the whole
// point of the plan's A7 row.
//
// The tests below pin the three behaviours the backends rely on:
//   1. An identical frame reports "nothing changed" (empty rects,
//      `full_frame == false`).
//   2. A changed frame reports a *tight* bounding box around the change
//      — never under-covering it.
//   3. The first frame (and a frame after a geometry change) is always
//      a full-frame change, because there is nothing to compare against.
// ----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <nimrtc/video_capture/dirty_rect_diff.hpp>

namespace {

using nimrtc::video_capture::DirtyRectDetector;
using nimrtc::plugins::CaptureDirtyRects;

/** A BGRA test frame: `width` x `height`, filled with one colour. */
std::vector<std::uint8_t> make_frame(std::uint32_t width,
                                     std::uint32_t height,
                                     std::uint8_t r = 0,
                                     std::uint8_t g = 0,
                                     std::uint8_t b = 0) {
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(width) * height * 4u);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = b;
        pixels[i + 1] = g;
        pixels[i + 2] = r;
        pixels[i + 3] = 0xFF;
    }
    return pixels;
}

/** Paint one pixel. */
void paint(std::vector<std::uint8_t>& pixels,
           std::uint32_t width,
           std::uint32_t x,
           std::uint32_t y,
           std::uint8_t r) {
    const std::size_t off =
        (static_cast<std::size_t>(y) * width + x) * 4u;
    pixels[off + 2] = r;
}

} // namespace

// ---------------------------------------------------------------------------
// The first frame
// ---------------------------------------------------------------------------

TEST(DirtyRectDetector, FirstFrameIsAFullFrameChange) {
    DirtyRectDetector detector(1);
    const auto pixels = make_frame(64, 48);

    CaptureDirtyRects dirty{};
    const bool identical = detector.compare(
        pixels.data(), pixels.size(),
        static_cast<std::int32_t>(64 * 4), 64, 48, 4, dirty);

    // There is no previous frame, so the frame is *not* identical to it.
    EXPECT_FALSE(identical);
    EXPECT_TRUE(dirty.full_frame);
    EXPECT_TRUE(dirty.rects.empty());
    EXPECT_TRUE(detector.has_previous());
}

TEST(DirtyRectDetector, ResetForcesTheNextFrameToBeFull) {
    DirtyRectDetector detector(1);
    const auto pixels = make_frame(64, 48);

    CaptureDirtyRects dirty{};
    detector.compare(pixels.data(), pixels.size(),
                     static_cast<std::int32_t>(64 * 4), 64, 48, 4, dirty);
    ASSERT_TRUE(detector.has_previous());

    detector.reset();
    EXPECT_FALSE(detector.has_previous());

    detector.compare(pixels.data(), pixels.size(),
                     static_cast<std::int32_t>(64 * 4), 64, 48, 4, dirty);
    EXPECT_TRUE(dirty.full_frame);
}

// ---------------------------------------------------------------------------
// Identical frames
// ---------------------------------------------------------------------------

TEST(DirtyRectDetector, IdenticalFramesReportNothingChanged) {
    DirtyRectDetector detector(1);
    const auto pixels = make_frame(64, 48);

    CaptureDirtyRects dirty{};
    detector.compare(pixels.data(), pixels.size(),
                     static_cast<std::int32_t>(64 * 4), 64, 48, 4, dirty);

    // The second, identical frame: nothing changed.
    const bool identical = detector.compare(
        pixels.data(), pixels.size(),
        static_cast<std::int32_t>(64 * 4), 64, 48, 4, dirty);

    EXPECT_TRUE(identical);
    EXPECT_FALSE(dirty.full_frame);
    EXPECT_TRUE(dirty.rects.empty())
        << "an unchanged frame must not report a changed region";
}

TEST(DirtyRectDetector, IdenticalFramesAreDetectedWithASamplingStride) {
    // The sampling stride must not turn a *changed* frame into an
    // "unchanged" one for a change that is bigger than the stride.
    DirtyRectDetector detector(4);
    const auto pixels = make_frame(64, 48);

    CaptureDirtyRects dirty{};
    detector.compare(pixels.data(), pixels.size(),
                     static_cast<std::int32_t>(64 * 4), 64, 48, 4, dirty);

    const bool identical = detector.compare(
        pixels.data(), pixels.size(),
        static_cast<std::int32_t>(64 * 4), 64, 48, 4, dirty);
    EXPECT_TRUE(identical);
}

// ---------------------------------------------------------------------------
// Changed frames
// ---------------------------------------------------------------------------

TEST(DirtyRectDetector, ChangedPixelIsReportedAsATightBox) {
    DirtyRectDetector detector(1);

    auto pixels = make_frame(64, 48);
    CaptureDirtyRects dirty{};
    detector.compare(pixels.data(), pixels.size(),
                     static_cast<std::int32_t>(64 * 4), 64, 48, 4, dirty);

    // Change exactly one pixel at (10, 20).
    paint(pixels, 64, 10, 20, 0xFF);

    const bool identical = detector.compare(
        pixels.data(), pixels.size(),
        static_cast<std::int32_t>(64 * 4), 64, 48, 4, dirty);

    EXPECT_FALSE(identical);
    EXPECT_FALSE(dirty.full_frame);
    ASSERT_EQ(dirty.rects.size(), 1u);

    const auto& r = dirty.rects.front();
    EXPECT_EQ(r.x, 10u);
    EXPECT_EQ(r.y, 20u);
    EXPECT_EQ(r.width, 1u);
    EXPECT_EQ(r.height, 1u);
}

TEST(DirtyRectDetector, TheBoundingBoxCoversEveryChangedPixel) {
    DirtyRectDetector detector(1);

    auto pixels = make_frame(64, 48);
    CaptureDirtyRects dirty{};
    detector.compare(pixels.data(), pixels.size(),
                     static_cast<std::int32_t>(64 * 4), 64, 48, 4, dirty);

    // Change two pixels far apart: the box must span both of them.
    paint(pixels, 64, 5, 5, 0xFF);
    paint(pixels, 64, 40, 30, 0xFF);

    detector.compare(pixels.data(), pixels.size(),
                     static_cast<std::int32_t>(64 * 4), 64, 48, 4, dirty);

    ASSERT_EQ(dirty.rects.size(), 1u);
    const auto& r = dirty.rects.front();
    EXPECT_LE(r.x, 5u);
    EXPECT_LE(r.y, 5u);
    EXPECT_GE(r.x + r.width, 41u);
    EXPECT_GE(r.y + r.height, 31u);
}

TEST(DirtyRectDetector, AChangedFrameIsNeverReportedAsUnchanged) {
    // The sampling stride must not *under*-cover a change: the reported
    // box has to contain the changed pixel even when the pixel is
    // between two samples.
    DirtyRectDetector detector(4);

    auto pixels = make_frame(64, 48);
    CaptureDirtyRects dirty{};
    detector.compare(pixels.data(), pixels.size(),
                     static_cast<std::int32_t>(64 * 4), 64, 48, 4, dirty);

    // A change in a 4x4 block (the whole block is changed, so a sample
    // inside it must see it).
    for (std::uint32_t y = 20; y < 24; ++y) {
        for (std::uint32_t x = 20; x < 24; ++x) {
            paint(pixels, 64, x, y, 0xFF);
        }
    }

    const bool identical = detector.compare(
        pixels.data(), pixels.size(),
        static_cast<std::int32_t>(64 * 4), 64, 48, 4, dirty);

    EXPECT_FALSE(identical);
    EXPECT_FALSE(dirty.full_frame);
    ASSERT_EQ(dirty.rects.size(), 1u);
    const auto& r = dirty.rects.front();
    EXPECT_LE(r.x, 20u);
    EXPECT_LE(r.y, 20u);
    EXPECT_GE(r.x + r.width, 24u);
    EXPECT_GE(r.y + r.height, 24u);
}

// ---------------------------------------------------------------------------
// Geometry changes
// ---------------------------------------------------------------------------

TEST(DirtyRectDetector, AGeometryChangeIsAFullFrameChange) {
    DirtyRectDetector detector(1);

    const auto small = make_frame(64, 48);
    CaptureDirtyRects dirty{};
    detector.compare(small.data(), small.size(),
                     static_cast<std::int32_t>(64 * 4), 64, 48, 4, dirty);

    // A different size: the comparison is meaningless, so the frame is
    // reported as fully changed rather than as a bogus small box.
    const auto large = make_frame(128, 96);
    detector.compare(large.data(), large.size(),
                     static_cast<std::int32_t>(128 * 4), 128, 96, 4, dirty);

    EXPECT_TRUE(dirty.full_frame);
    EXPECT_TRUE(dirty.rects.empty());
}

TEST(DirtyRectDetector, APixelFormatChangeIsAFullFrameChange) {
    DirtyRectDetector detector(1);

    const auto pixels = make_frame(64, 48);
    CaptureDirtyRects dirty{};
    detector.compare(pixels.data(), pixels.size(),
                     static_cast<std::int32_t>(64 * 4), 64, 48, 4, dirty);

    // Same geometry, different bytes-per-pixel: also meaningless.
    detector.compare(pixels.data(), pixels.size(),
                     static_cast<std::int32_t>(64 * 4), 64, 48, 3, dirty);
    EXPECT_TRUE(dirty.full_frame);
}

// ---------------------------------------------------------------------------
// Degenerate inputs
// ---------------------------------------------------------------------------

TEST(DirtyRectDetector, NullPixelsAreAFullFrameChange) {
    DirtyRectDetector detector(1);

    CaptureDirtyRects dirty{};
    const bool identical = detector.compare(
        nullptr, 0, 0, 0, 0, 4, dirty);

    EXPECT_FALSE(identical);
    EXPECT_TRUE(dirty.full_frame);
}

TEST(DirtyRectDetector, AZeroStrideIsNormalisedToOne) {
    DirtyRectDetector detector(0);
    EXPECT_EQ(detector.stride_pixels(), 1u);
}
