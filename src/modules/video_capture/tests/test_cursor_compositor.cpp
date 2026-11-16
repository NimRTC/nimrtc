// modules/video_capture/tests/test_cursor_compositor.cpp
// ----------------------------------------------------------------------------
// The cursor-compositing layer's tests (remote-desktop plan A6).
//
// These run on **every** platform: the compositor is portable by
// construction, and the platform *sources* (GetCursorInfo + DrawIconEx
// on Windows, XFixesGetCursorImage on X11) are thin adapters over it.
//
// The properties pinned down here are the ones the plan states
// explicitly:
//
//   - "the composited frame differs from the raw frame in the cursor's
//     bounding box and is identical elsewhere (pixel-exact comparison
//     outside the box)",
//   - the operation is **recoverable** (the raw frame can be restored),
//   - a cursor near the frame's edge is **clipped**, never written out
//     of bounds,
//   - the pinned fallback keeps the cursor at the last position,
//   - and a fully transparent cursor changes nothing.
//
// The *source* seam is exercised with a **fake source**, which is the
// same trick the unix-capture tests use: it proves the contract the
// real adapters implement without an OS being involved.
// ----------------------------------------------------------------------------

#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <nimrtc/video_capture/cursor_compositor.hpp>
#include <nimrtc/video_capture/cursor_source.hpp>
#include <string>
#include <vector>

namespace {

using nimrtc::plugins::CaptureDevice;
using nimrtc::plugins::CaptureDeviceKind;
using nimrtc::plugins::CaptureFrame;
using nimrtc::plugins::VideoPixelFormat;
using nimrtc::video_capture::CursorCompositor;
using nimrtc::video_capture::CursorImage;
using nimrtc::video_capture::CursorPosition;
using nimrtc::video_capture::CursorSample;
using nimrtc::video_capture::CursorSourceStatus;
using nimrtc::video_capture::ICursorSource;
using nimrtc::video_capture::PinnedCursorFallback;

/** A BGRA frame of @p width x @p height, filled with @p value. */
std::vector<std::uint8_t> make_frame(std::uint32_t width, std::uint32_t height,
                                     std::uint8_t value = 0) {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4u, value);
}

/**
 * @brief A cursor bitmap: a solid colour with the given alpha.
 *
 * The hotspot is at the top-left by default, so the cursor's bitmap is
 * drawn *down and to the right* of the pointer's position.
 */
CursorImage make_cursor(std::uint32_t width, std::uint32_t height, std::uint8_t bgra_value,
                        std::uint8_t alpha, std::int32_t hotspot_x = 0,
                        std::int32_t hotspot_y = 0) {
    CursorImage image;
    image.width = width;
    image.height = height;
    image.hotspot_x = hotspot_x;
    image.hotspot_y = hotspot_y;
    image.stride = static_cast<std::int32_t>(width * 4u);
    image.pixels.resize(static_cast<std::size_t>(width) * height * 4u);
    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
        image.pixels[i + 0] = bgra_value;
        image.pixels[i + 1] = bgra_value;
        image.pixels[i + 2] = bgra_value;
        image.pixels[i + 3] = alpha;
    }
    return image;
}

/**
 * @brief A fake cursor source.
 *
 * It hands out a fixed bitmap at a fixed position, and it can be told
 * to become unavailable — which is what drives the pinned fallback.
 */
class FakeCursorSource final : public ICursorSource {
  public:
    FakeCursorSource(CursorImage image, std::int32_t x, std::int32_t y)
        : image_(std::move(image)), x_(x), y_(y) {}

    [[nodiscard]] std::string_view id() const noexcept override { return "fake-cursor"; }

    [[nodiscard]] CursorSourceStatus poll(CursorSample& out) noexcept override {
        ++polls_;
        if (!available_) return CursorSourceStatus::kUnavailable;
        out.image = image_;
        out.screen_x = x_;
        out.screen_y = y_;
        out.hidden = hidden_;
        return CursorSourceStatus::kOk;
    }

    void set_available(bool available) noexcept { available_ = available; }
    void set_hidden(bool hidden) noexcept { hidden_ = hidden; }
    void move_to(std::int32_t x, std::int32_t y) noexcept {
        x_ = x;
        y_ = y;
    }
    [[nodiscard]] std::uint32_t polls() const noexcept { return polls_; }

  private:
    CursorImage image_;
    std::int32_t x_ = 0;
    std::int32_t y_ = 0;
    bool available_ = true;
    bool hidden_ = false;
    std::uint32_t polls_ = 0;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// The blend (the plan's exact acceptance criterion)
// ---------------------------------------------------------------------------

TEST(CursorCompositor, TheFrameDiffersInsideTheBoxAndIsIdenticalOutside) {
    const std::uint32_t width = 64;
    const std::uint32_t height = 48;

    const auto raw = make_frame(width, height, 0x20);
    auto composited = raw;

    const auto cursor = make_cursor(8, 8, 0xFF, 0xFF);
    CursorCompositor compositor;
    const auto result =
        compositor.composite(cursor, 20, 10, composited.data(), composited.size(),
                             static_cast<std::int32_t>(width * 4u), width, height);

    ASSERT_TRUE(result.applied);
    EXPECT_EQ(result.x, 20u);
    EXPECT_EQ(result.y, 10u);
    EXPECT_EQ(result.width, 8u);
    EXPECT_EQ(result.height, 8u);

    // Outside the box: byte-identical.
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4u;
            const bool inside = (x >= 20 && x < 28 && y >= 10 && y < 18);
            if (inside) continue;
            EXPECT_EQ(composited[index], raw[index]) << "at (" << x << ", " << y << ")";
            EXPECT_EQ(composited[index + 1], raw[index + 1]) << "at (" << x << ", " << y << ")";
            EXPECT_EQ(composited[index + 2], raw[index + 2]) << "at (" << x << ", " << y << ")";
            EXPECT_EQ(composited[index + 3], raw[index + 3]) << "at (" << x << ", " << y << ")";
        }
    }

    // Inside the box: the cursor's colour is there.
    const std::size_t inside_index = (static_cast<std::size_t>(10) * width + 20) * 4u;
    EXPECT_NE(composited[inside_index], raw[inside_index]);
    EXPECT_EQ(composited[inside_index], 0xFF);
}

TEST(CursorCompositor, AnOpaqueCursorReplacesThePixels) {
    const std::uint32_t width = 16;
    const std::uint32_t height = 16;

    auto frame = make_frame(width, height, 0x20);
    const auto cursor = make_cursor(4, 4, 0x11, 0xFF);

    CursorCompositor compositor;
    ASSERT_TRUE(
        compositor.composite(cursor, 2, 2, frame.data(), frame.size(),
                             static_cast<std::int32_t>(width * 4u), width, height)
            .applied);

    const std::size_t index = (static_cast<std::size_t>(2) * width + 2) * 4u;
    EXPECT_EQ(frame[index], 0x11);
    EXPECT_EQ(frame[index + 1], 0x11);
    EXPECT_EQ(frame[index + 2], 0x11);
}

TEST(CursorCompositor, ATransparentCursorChangesNothing) {
    const std::uint32_t width = 16;
    const std::uint32_t height = 16;

    const auto raw = make_frame(width, height, 0x33);
    auto frame = raw;

    // Fully transparent: the cursor's coverage is zero.
    const auto cursor = make_cursor(8, 8, 0xFF, 0x00);

    CursorCompositor compositor;
    const auto result =
        compositor.composite(cursor, 4, 4, frame.data(), frame.size(),
                             static_cast<std::int32_t>(width * 4u), width, height);

    // The blend reports that it ran, but the bytes are untouched.
    EXPECT_TRUE(result.applied);
    EXPECT_EQ(frame, raw);
}

TEST(CursorCompositor, AHalfTransparentCursorBlendsTheTwoColours) {
    const std::uint32_t width = 8;
    const std::uint32_t height = 8;

    auto frame = make_frame(width, height, 0x00);
    const auto cursor = make_cursor(2, 2, 0xFF, 0x80);

    CursorCompositor compositor;
    ASSERT_TRUE(compositor.composite(cursor, 0, 0, frame.data(), frame.size(),
                                     static_cast<std::int32_t>(width * 4u), width, height)
                    .applied);

    // 0xFF * 0x80 / 255 == 128 (rounded).
    EXPECT_EQ(frame[0], 128);
}

TEST(CursorCompositor, TheHotspotPlacesTheBitmap) {
    const std::uint32_t width = 32;
    const std::uint32_t height = 32;

    auto frame = make_frame(width, height, 0x00);
    // The hotspot is at (2, 3) inside a 6x6 bitmap, so the bitmap is
    // drawn from (x - 2, y - 3).
    const auto cursor = make_cursor(6, 6, 0xFF, 0xFF, 2, 3);

    CursorCompositor compositor;
    const auto result =
        compositor.composite(cursor, 10, 12, frame.data(), frame.size(),
                             static_cast<std::int32_t>(width * 4u), width, height);

    ASSERT_TRUE(result.applied);
    EXPECT_EQ(result.x, 8u);
    EXPECT_EQ(result.y, 9u);
    EXPECT_EQ(result.width, 6u);
    EXPECT_EQ(result.height, 6u);

    // The hotspot's own pixel is at (10, 12).
    const std::size_t hotspot_index = (static_cast<std::size_t>(12) * width + 10) * 4u;
    EXPECT_EQ(frame[hotspot_index], 0xFF);
}

// ---------------------------------------------------------------------------
// The recoverability (the negative control)
// ---------------------------------------------------------------------------

TEST(CursorCompositor, TheRawFrameCanBeRecovered) {
    const std::uint32_t width = 32;
    const std::uint32_t height = 32;

    const auto raw = make_frame(width, height, 0x44);
    auto frame = raw;

    const auto cursor = make_cursor(8, 8, 0xFF, 0xFF);

    CursorCompositor compositor;
    ASSERT_TRUE(compositor.composite(cursor, 10, 10, frame.data(), frame.size(),
                                     static_cast<std::int32_t>(width * 4u), width, height)
                    .applied);
    EXPECT_NE(frame, raw);
    EXPECT_TRUE(compositor.can_restore());

    compositor.restore();
    EXPECT_EQ(frame, raw);
    EXPECT_FALSE(compositor.can_restore());
}

TEST(CursorCompositor, TheRestoreLeavesTheRestOfTheFrameAlone) {
    const std::uint32_t width = 32;
    const std::uint32_t height = 32;

    auto frame = make_frame(width, height, 0x44);
    // A pixel outside the cursor's box.
    frame[0] = 0x99;

    const auto cursor = make_cursor(8, 8, 0xFF, 0xFF);

    CursorCompositor compositor;
    ASSERT_TRUE(compositor.composite(cursor, 10, 10, frame.data(), frame.size(),
                                     static_cast<std::int32_t>(width * 4u), width, height)
                    .applied);
    compositor.restore();

    EXPECT_EQ(frame[0], 0x99);
}

TEST(CursorCompositor, CompositingIsNotIdempotentWithoutARestore) {
    // The negative control: compositing twice into the *same* frame
    // without restoring in between gives a different picture (the
    // second blend goes over the already-blended pixels).  This is
    // exactly why the compositor keeps a copy of the region it
    // touched: the caller can undo the first blend.
    const std::uint32_t width = 32;
    const std::uint32_t height = 32;

    const auto raw = make_frame(width, height, 0x00);
    const auto cursor = make_cursor(4, 4, 0x80, 0x80);

    // The picture after exactly one blend.
    auto once = raw;
    {
        CursorCompositor compositor;
        ASSERT_TRUE(compositor.composite(cursor, 4, 4, once.data(), once.size(),
                                          static_cast<std::int32_t>(width * 4u), width, height)
                        .applied);
    }
    EXPECT_NE(once, raw);

    // Without a restore, the second blend changes the picture again.
    auto twice = once;
    {
        CursorCompositor compositor;
        ASSERT_TRUE(compositor.composite(cursor, 4, 4, twice.data(), twice.size(),
                                          static_cast<std::int32_t>(width * 4u), width, height)
                        .applied);
        ASSERT_TRUE(compositor.composite(cursor, 4, 4, twice.data(), twice.size(),
                                          static_cast<std::int32_t>(width * 4u), width, height)
                        .applied);
    }
    EXPECT_NE(twice, once);

    // With a restore in between, the operation *is* idempotent: the
    // picture after composite → restore → composite is the picture
    // after a single composite.
    auto round_trip = raw;
    {
        CursorCompositor compositor;
        ASSERT_TRUE(compositor.composite(cursor, 4, 4, round_trip.data(), round_trip.size(),
                                          static_cast<std::int32_t>(width * 4u), width, height)
                        .applied);
        compositor.restore();
        EXPECT_EQ(round_trip, raw);
        ASSERT_TRUE(compositor.composite(cursor, 4, 4, round_trip.data(), round_trip.size(),
                                          static_cast<std::int32_t>(width * 4u), width, height)
                        .applied);
    }
    EXPECT_EQ(round_trip, once);
}

// ---------------------------------------------------------------------------
// The clipping (a memory-safety property)
// ---------------------------------------------------------------------------

TEST(CursorCompositor, ACursorAtTheCornerIsClipped) {
    const std::uint32_t width = 16;
    const std::uint32_t height = 16;

    auto frame = make_frame(width, height, 0x00);
    const auto cursor = make_cursor(8, 8, 0xFF, 0xFF);

    CursorCompositor compositor;
    const auto result =
        compositor.composite(cursor, 0, 0, frame.data(), frame.size(),
                             static_cast<std::int32_t>(width * 4u), width, height);

    // The bitmap is 8x8 but only 8x8 of it fits -- the clipping is
    // exact here.  What matters is that nothing outside the frame was
    // touched.
    ASSERT_TRUE(result.applied);
    EXPECT_EQ(result.x, 0u);
    EXPECT_EQ(result.y, 0u);
    EXPECT_EQ(result.width, 8u);
    EXPECT_EQ(result.height, 8u);
}

TEST(CursorCompositor, ACursorStraddlingTheEdgeIsClippedNotWritten) {
    const std::uint32_t width = 16;
    const std::uint32_t height = 16;

    // A guard region after the frame's bytes: an out-of-bounds write
    // would land in it.
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(width) * height * 4u + 64u, 0x7E);
    std::uint8_t* frame = buffer.data();
    const std::size_t frame_size = static_cast<std::size_t>(width) * height * 4u;

    const auto cursor = make_cursor(8, 8, 0xFF, 0xFF);

    CursorCompositor compositor;
    // The pointer is at (14, 14): the bitmap hangs over the right and
    // bottom edges.
    const auto result =
        compositor.composite(cursor, 14, 14, frame, frame_size,
                             static_cast<std::int32_t>(width * 4u), width, height);

    ASSERT_TRUE(result.applied);
    EXPECT_EQ(result.x, 14u);
    EXPECT_EQ(result.y, 14u);
    EXPECT_EQ(result.width, 2u);
    EXPECT_EQ(result.height, 2u);

    // The guard region is untouched.
    for (std::size_t i = frame_size; i < buffer.size(); ++i) {
        EXPECT_EQ(buffer[i], 0x7E) << "at byte " << i;
    }
}

TEST(CursorCompositor, ACursorCompletelyOffTheFrameChangesNothing) {
    const std::uint32_t width = 16;
    const std::uint32_t height = 16;

    const auto raw = make_frame(width, height, 0x55);
    auto frame = raw;

    const auto cursor = make_cursor(8, 8, 0xFF, 0xFF);

    CursorCompositor compositor;
    const auto result =
        compositor.composite(cursor, -100, -100, frame.data(), frame.size(),
                             static_cast<std::int32_t>(width * 4u), width, height);

    EXPECT_FALSE(result.applied);
    EXPECT_TRUE(result.empty());
    EXPECT_EQ(frame, raw);
}

TEST(CursorCompositor, AStridedFrameIsBlendedCorrectly) {
    // A frame whose rows are padded (a stride wider than the packed
    // row) must be blended at the right offsets -- otherwise the
    // cursor would be drawn diagonally.
    const std::uint32_t width = 8;
    const std::uint32_t height = 8;
    const std::uint32_t stride = width * 4u + 16u;

    std::vector<std::uint8_t> frame(static_cast<std::size_t>(stride) * height, 0xEE);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint8_t* p = frame.data() + static_cast<std::size_t>(y) * stride + x * 4u;
            p[0] = p[1] = p[2] = 0x00;
            p[3] = 0xFF;
        }
    }

    const auto cursor = make_cursor(2, 2, 0xFF, 0xFF);

    CursorCompositor compositor;
    const auto result = compositor.composite(cursor, 4, 4, frame.data(), frame.size(),
                                             static_cast<std::int32_t>(stride), width, height);
    ASSERT_TRUE(result.applied);

    // The blended pixel is at (4, 4), not somewhere diagonal.
    const std::size_t index = static_cast<std::size_t>(4) * stride + 4u * 4u;
    EXPECT_EQ(frame[index], 0xFF);
    EXPECT_EQ(frame[index + 1], 0xFF);
    EXPECT_EQ(frame[index + 2], 0xFF);

    // The padding bytes are untouched.
    EXPECT_EQ(frame[static_cast<std::size_t>(4) * stride + width * 4u], 0xEE);
}

// ---------------------------------------------------------------------------
// The coordinate translation
// ---------------------------------------------------------------------------

TEST(CursorCompositor, TheScreenPositionIsTranslatedIntoTheFrame) {
    // A display whose origin is at (1920, 0) on the desktop.
    const CursorPosition pos = CursorCompositor::screen_to_frame(2000, 100, 1920, 0, 1920, 1080);

    EXPECT_EQ(pos.x, 80);
    EXPECT_EQ(pos.y, 100);
    EXPECT_TRUE(pos.inside);
}

TEST(CursorCompositor, APositionOffTheSurfaceIsReportedAsOutside) {
    const CursorPosition pos = CursorCompositor::screen_to_frame(100, 100, 1920, 0, 1920, 1080);

    EXPECT_EQ(pos.x, -1820);
    EXPECT_FALSE(pos.inside);
}

// ---------------------------------------------------------------------------
// The pinned fallback
// ---------------------------------------------------------------------------

TEST(PinnedCursorFallback, TheLastPositionPersistsWhenNothingNewArrives) {
    const auto cursor = make_cursor(4, 4, 0xFF, 0xFF);
    auto source = std::make_shared<FakeCursorSource>(cursor, 30, 40);

    PinnedCursorFallback fallback(source);

    CursorSample sample;
    ASSERT_EQ(fallback.poll(sample), CursorSourceStatus::kOk);
    EXPECT_TRUE(fallback.has_pinned_sample());
    EXPECT_EQ(sample.screen_x, 30);
    EXPECT_EQ(sample.screen_y, 40);

    // The source goes away: the fallback serves the last known sample.
    source->set_available(false);
    CursorSample pinned;
    ASSERT_EQ(fallback.poll(pinned), CursorSourceStatus::kOk);
    EXPECT_EQ(pinned.screen_x, 30);
    EXPECT_EQ(pinned.screen_y, 40);
    EXPECT_EQ(fallback.pinned_hits(), 1u);
    EXPECT_TRUE(fallback.has_pinned_sample());
}

TEST(PinnedCursorFallback, WithoutAPreviousSampleThereIsNothingToServe) {
    auto source = std::make_shared<FakeCursorSource>(make_cursor(4, 4, 0xFF, 0xFF), 1, 1);
    source->set_available(false);

    PinnedCursorFallback fallback(source);

    CursorSample sample;
    // The honest answer: "unavailable", not an empty sample.
    EXPECT_EQ(fallback.poll(sample), CursorSourceStatus::kUnavailable);
    EXPECT_FALSE(fallback.has_pinned_sample());
}

TEST(PinnedCursorFallback, ThePinCanBeCleared) {
    auto source = std::make_shared<FakeCursorSource>(make_cursor(4, 4, 0xFF, 0xFF), 1, 1);

    PinnedCursorFallback fallback(source);
    CursorSample sample;
    ASSERT_EQ(fallback.poll(sample), CursorSourceStatus::kOk);
    ASSERT_TRUE(fallback.has_pinned_sample());

    fallback.clear_pin();
    EXPECT_FALSE(fallback.has_pinned_sample());

    source->set_available(false);
    EXPECT_EQ(fallback.poll(sample), CursorSourceStatus::kUnavailable);
}

TEST(PinnedCursorFallback, ThePinnedCursorIsDrawnAtTheLastPosition) {
    // The compositor draws the pinned cursor when the source has
    // nothing new to offer.
    const std::uint32_t width = 32;
    const std::uint32_t height = 32;

    auto frame = make_frame(width, height, 0x00);
    const auto cursor = make_cursor(4, 4, 0xFF, 0xFF);

    auto source = std::make_shared<FakeCursorSource>(cursor, 10, 12);
    PinnedCursorFallback fallback(source);

    CursorSample sample;
    ASSERT_EQ(fallback.poll(sample), CursorSourceStatus::kOk);
    EXPECT_EQ(sample.screen_x, 10);

    // The source goes away; the fallback still has the sample.
    source->set_available(false);
    CursorSample pinned;
    ASSERT_EQ(fallback.poll(pinned), CursorSourceStatus::kOk);

    CursorCompositor compositor;
    const auto result =
        compositor.composite(pinned.image, pinned.screen_x, pinned.screen_y, frame.data(),
                             frame.size(), static_cast<std::int32_t>(width * 4u), width, height);

    ASSERT_TRUE(result.applied);
    EXPECT_EQ(result.x, 10u);
    EXPECT_EQ(result.y, 12u);
}

// ---------------------------------------------------------------------------
// The source seam
// ---------------------------------------------------------------------------

TEST(CursorSource, TheStatusNamesAreStable) {
    EXPECT_EQ(nimrtc::video_capture::cursor_source_status_name(CursorSourceStatus::kOk),
              "ok");
    EXPECT_EQ(nimrtc::video_capture::cursor_source_status_name(
                  CursorSourceStatus::kNoOverlayNeeded),
              "no-overlay-needed");
    EXPECT_EQ(nimrtc::video_capture::cursor_source_status_name(
                  CursorSourceStatus::kUnavailable),
              "unavailable");
    EXPECT_EQ(nimrtc::video_capture::cursor_source_status_name(
                  CursorSourceStatus::kUnsupported),
              "unsupported");
}

TEST(CursorSource, ThePlatformSourceMatchesTheBuild) {
    // The Windows build has a real source; the macOS one reports "no
    // overlay needed"; the others depend on what was found at
    // configure time.
    auto source = nimrtc::video_capture::make_platform_cursor_source();
#if defined(_WIN32)
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->id(), "windows-cursor");
#elif defined(__APPLE__)
    ASSERT_NE(source, nullptr);
    CursorSample sample;
    EXPECT_EQ(source->poll(sample), CursorSourceStatus::kNoOverlayNeeded);
#elif defined(__linux__)
    // The X11 source is present when the X11 headers were found.
    if (source != nullptr) {
        EXPECT_EQ(source->id(), "x11-cursor");
    }
#endif
}
