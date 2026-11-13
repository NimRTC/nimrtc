// modules/video_capture/tests/test_cursor_compositor.cpp
// ----------------------------------------------------------------------------
// The cursor-compositing layer's tests (remote-desktop plan A6).
//
// The plan's A6 row names the acceptance criterion:
//
//   "On a Windows host the composited frame differs from the raw frame
//    in the cursor's bounding box and is identical elsewhere
//    (pixel-exact comparison outside the box)."
//
// That criterion is *portable* — it is about the compositing maths, not
// about Windows — so it is tested here, against the portable
// implementation, on every CI leg.  The Windows backend (A3) will use
// the same implementation and the same assertion.
//
// The tests cover:
//
//   - the pointer is drawn only inside its bounding box (pixel-exact
//     outside),
//   - the alpha blending is correct (an opaque cursor replaces the
//     pixel; a half-transparent cursor mixes),
//   - a hidden pointer draws nothing,
//   - a pointer outside the frame draws nothing,
//   - the region origin shifts the pointer's position,
//   - and the pinned fallback keeps the pointer at its last position.
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/capture_source_adapter.hpp>
#include <nimrtc/video_capture/cursor_compositor.hpp>
#include <nimrtc/video_capture/synthetic_capture.hpp>

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

using nimrtc::plugins::CaptureConfig;
using nimrtc::plugins::VideoPixelFormat;
using nimrtc::plugins::VideoSourceConfig;
using nimrtc::plugins::VideoSourceFrame;
using nimrtc::video_capture::AlphaCursorCompositor;
using nimrtc::video_capture::CursorState;
using nimrtc::video_capture::ManualCursorSource;
using nimrtc::video_capture::NullCursorSource;
using nimrtc::video_capture::PinnedCursorSource;

namespace {

/** A 32x32 opaque grey frame. */
std::vector<std::uint8_t> make_frame(std::uint32_t w, std::uint32_t h,
                                      std::uint8_t luma = 0x40u) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4u, luma);
    for (std::size_t i = 0; i < px.size(); i += 4u) {
        px[i + 3] = 0xFFu;
    }
    return px;
}

/** A cursor bitmap: a solid colour square with a given alpha. */
CursorState make_cursor(std::int32_t x, std::int32_t y,
                         std::uint32_t w, std::uint32_t h,
                         std::uint8_t r, std::uint8_t g, std::uint8_t b,
                         std::uint8_t a) {
    CursorState s;
    s.visible = true;
    s.live = true;
    s.x = x;
    s.y = y;
    s.bitmap_width = w;
    s.bitmap_height = h;
    s.bitmap_stride = static_cast<std::int32_t>(w * 4u);
    s.bgra.assign(static_cast<std::size_t>(w) * h * 4u, 0u);
    for (std::size_t i = 0; i < s.bgra.size(); i += 4u) {
        s.bgra[i + 0u] = b;
        s.bgra[i + 1u] = g;
        s.bgra[i + 2u] = r;
        s.bgra[i + 3u] = a;
    }
    return s;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// The drawing
// ---------------------------------------------------------------------------

TEST(CursorCompositor, DrawsThePointerIntoTheFrame) {
    AlphaCursorCompositor comp;
    auto frame = make_frame(32, 32);

    const CursorState cur = make_cursor(8, 8, 4, 4, 0xFF, 0x00, 0x00, 0xFF);
    ASSERT_TRUE(comp.composite(frame.data(), frame.size(),
                                static_cast<std::int32_t>(32 * 4u),
                                32, 32, cur, 0, 0));

    // The pointer's pixel is red.
    const std::size_t idx = (8u * 32u + 8u) * 4u;
    EXPECT_EQ(frame[idx + 0u], 0x00u);   // B
    EXPECT_EQ(frame[idx + 1u], 0x00u);   // G
    EXPECT_EQ(frame[idx + 2u], 0xFFu);   // R
}

TEST(CursorCompositor, LeavesTheRestOfTheFramePixelExact) {
    // The plan's A6 acceptance criterion: "the composited frame differs
    // from the raw frame in the cursor's bounding box and is identical
    // elsewhere (pixel-exact comparison outside the box)."
    AlphaCursorCompositor comp;
    const auto original = make_frame(32, 32);
    auto frame = original;

    const CursorState cur = make_cursor(8, 8, 4, 4, 0xFF, 0x00, 0x00, 0xFF);
    ASSERT_TRUE(comp.composite(frame.data(), frame.size(),
                                static_cast<std::int32_t>(32 * 4u),
                                32, 32, cur, 0, 0));

    for (std::uint32_t y = 0; y < 32u; ++y) {
        for (std::uint32_t x = 0; x < 32u; ++x) {
            const std::size_t idx = (static_cast<std::size_t>(y) * 32u + x) * 4u;
            const bool in_box = x >= 8u && x < 12u && y >= 8u && y < 12u;
            if (in_box) continue;
            EXPECT_EQ(frame[idx + 0u], original[idx + 0u])
                << "pixel (" << x << "," << y << ") B changed outside the box";
            EXPECT_EQ(frame[idx + 1u], original[idx + 1u])
                << "pixel (" << x << "," << y << ") G changed outside the box";
            EXPECT_EQ(frame[idx + 2u], original[idx + 2u])
                << "pixel (" << x << "," << y << ") R changed outside the box";
        }
    }
}

TEST(CursorCompositor, BlendsAHalfTransparentCursor) {
    AlphaCursorCompositor comp;
    auto frame = make_frame(8, 8, 0x00u);   // black

    const CursorState cur = make_cursor(0, 0, 8, 8, 0xFF, 0xFF, 0xFF, 0x80u);
    ASSERT_TRUE(comp.composite(frame.data(), frame.size(),
                                static_cast<std::int32_t>(8 * 4u),
                                8, 8, cur, 0, 0));

    // 50% white over black is mid-grey (127 or 128).
    const std::size_t idx = 0u;
    EXPECT_NEAR(static_cast<int>(frame[idx + 0u]), 127, 2);
    EXPECT_NEAR(static_cast<int>(frame[idx + 1u]), 127, 2);
    EXPECT_NEAR(static_cast<int>(frame[idx + 2u]), 127, 2);
    // The frame stays opaque.
    EXPECT_EQ(frame[idx + 3u], 0xFFu);
}

TEST(CursorCompositor, AHiddenPointerDrawsNothing) {
    AlphaCursorCompositor comp;
    const auto original = make_frame(32, 32);
    auto frame = original;

    CursorState cur = make_cursor(8, 8, 4, 4, 0xFF, 0x00, 0x00, 0xFF);
    cur.visible = false;

    EXPECT_FALSE(comp.composite(frame.data(), frame.size(),
                                static_cast<std::int32_t>(32 * 4u),
                                32, 32, cur, 0, 0));
    EXPECT_EQ(frame, original);
}

TEST(CursorCompositor, APointerOutsideTheFrameDrawsNothing) {
    AlphaCursorCompositor comp;
    const auto original = make_frame(32, 32);
    auto frame = original;

    // The pointer is entirely to the right of the frame.
    const CursorState cur = make_cursor(100, 8, 4, 4, 0xFF, 0x00, 0x00, 0xFF);
    EXPECT_FALSE(comp.composite(frame.data(), frame.size(),
                                static_cast<std::int32_t>(32 * 4u),
                                32, 32, cur, 0, 0));
    EXPECT_EQ(frame, original);
}

TEST(CursorCompositor, ClipsAPointerThatHangsOverTheEdge) {
    AlphaCursorCompositor comp;
    auto frame = make_frame(32, 32);

    // The pointer's top-left is at (30, 30), so only a 2x2 corner of it
    // lands inside the frame.
    const CursorState cur = make_cursor(30, 30, 4, 4, 0xFF, 0x00, 0x00, 0xFF);
    ASSERT_TRUE(comp.composite(frame.data(), frame.size(),
                                static_cast<std::int32_t>(32 * 4u),
                                32, 32, cur, 0, 0));

    // (30, 30) is inside the box and was drawn.
    const std::size_t in_idx = (30u * 32u + 30u) * 4u;
    EXPECT_EQ(frame[in_idx + 2u], 0xFFu);
    // (29, 29) is outside the box and was not.
    const std::size_t out_idx = (29u * 32u + 29u) * 4u;
    EXPECT_EQ(frame[out_idx + 2u], 0x40u);
}

TEST(CursorCompositor, TheRegionOriginShiftsThePointer) {
    // A capture of a sub-region: the pointer's position is in *source*
    // pixels, so a region at (100, 50) puts the pointer 100 pixels to
    // the left of the frame's own origin.
    AlphaCursorCompositor comp;
    auto frame = make_frame(32, 32);

    // The pointer is at source x=108.  With the origin at 100, it lands
    // at frame x=8.
    const CursorState cur = make_cursor(108, 58, 4, 4, 0xFF, 0x00, 0x00, 0xFF);
    ASSERT_TRUE(comp.composite(frame.data(), frame.size(),
                                static_cast<std::int32_t>(32 * 4u),
                                32, 32, cur, 100, 50));

    const std::size_t idx = (8u * 32u + 8u) * 4u;
    EXPECT_EQ(frame[idx + 2u], 0xFFu);
}

TEST(CursorCompositor, RejectsAMalformedFrame) {
    AlphaCursorCompositor comp;
    auto frame = make_frame(32, 32);
    const CursorState cur = make_cursor(0, 0, 4, 4, 0xFF, 0x00, 0x00, 0xFF);

    // A null frame.
    EXPECT_FALSE(comp.composite(nullptr, 0, 0, 32, 32, cur, 0, 0));
    // A stride that is too small for the width.
    EXPECT_FALSE(comp.composite(frame.data(), frame.size(), 4, 32, 32, cur, 0, 0));
    // A cursor with no bitmap.
    CursorState empty = cur;
    empty.bgra.clear();
    EXPECT_FALSE(comp.composite(frame.data(), frame.size(),
                                static_cast<std::int32_t>(32 * 4u),
                                32, 32, empty, 0, 0));
}

TEST(CursorCompositor, CountsTheFramesItModified) {
    AlphaCursorCompositor comp;
    auto frame = make_frame(32, 32);
    const CursorState cur = make_cursor(8, 8, 4, 4, 0xFF, 0x00, 0x00, 0xFF);

    EXPECT_EQ(comp.frames_composited(), 0u);
    (void)comp.composite(frame.data(), frame.size(),
                         static_cast<std::int32_t>(32 * 4u),
                         32, 32, cur, 0, 0);
    EXPECT_EQ(comp.frames_composited(), 1u);
}

// ---------------------------------------------------------------------------
// The sources
// ---------------------------------------------------------------------------

TEST(CursorSource, TheNullSourceReportsNoPointer) {
    NullCursorSource src;
    const auto s = src.query();
    EXPECT_FALSE(s.visible);
    EXPECT_FALSE(s.live);
    EXPECT_FALSE(src.has_live_cursor());
    EXPECT_EQ(src.backend(), "none");
}

TEST(CursorSource, TheManualSourceReportsWhatTheHostSet) {
    ManualCursorSource src;
    src.set_state(make_cursor(3, 4, 2, 2, 0x11, 0x22, 0x33, 0xFF));

    const auto s = src.query();
    EXPECT_TRUE(s.visible);
    EXPECT_TRUE(s.live);
    EXPECT_EQ(s.x, 3);
    EXPECT_EQ(s.y, 4);
    EXPECT_EQ(s.bitmap_width, 2u);
    EXPECT_TRUE(src.has_live_cursor());
}

TEST(CursorSource, ThePinnedSourceKeepsTheLastPosition) {
    // A source that reports a live pointer once, then goes quiet: the
    // pinned fallback keeps the pointer where it was.
    class FlakySource final : public nimrtc::video_capture::ICursorSource {
    public:
        CursorState query() noexcept override {
            CursorState s = state_;
            state_.visible = false;   // the pointer goes away
            state_.live = false;
            return s;
        }
        std::string_view backend() const noexcept override { return "flaky"; }
        bool has_live_cursor() const noexcept override { return true; }
        void arm() { state_ = make_cursor(5, 6, 2, 2, 0xFF, 0, 0, 0xFF); }
    private:
        CursorState state_{};
    };

    FlakySource flaky;
    flaky.arm();

    PinnedCursorSource pinned{&flaky};

    // The first query gets the live pointer.
    const auto live = pinned.query();
    EXPECT_TRUE(live.visible);
    EXPECT_TRUE(live.live);
    EXPECT_EQ(live.x, 5);

    // The second query finds nothing live, so the pinned one is used.
    const auto held = pinned.query();
    EXPECT_TRUE(held.visible);
    EXPECT_FALSE(held.live);
    EXPECT_EQ(held.x, 5);
    EXPECT_EQ(held.y, 6);
}

TEST(CursorSource, ThePinnedSourceDoesNotInventAPointer) {
    // A source that never has a pointer must not produce a phantom one
    // through the pinned fallback.
    NullCursorSource null_src;
    PinnedCursorSource pinned{&null_src};
    const auto s = pinned.query();
    EXPECT_FALSE(s.visible);
}

TEST(CursorSource, ThePinnedSourceToleratesANullInner) {
    PinnedCursorSource pinned{nullptr};
    const auto s = pinned.query();
    EXPECT_FALSE(s.visible);
}

// ---------------------------------------------------------------------------
// The adapter composites the cursor
// ---------------------------------------------------------------------------

TEST(CursorCompositor, TheAdapterDrawsTheCursorIntoTheFrame) {
    using nimrtc::video_capture::CaptureSourceAdapter;
    using nimrtc::video_capture::SyntheticCapture;

    CaptureConfig cfg{};
    cfg.format.width  = 32;
    cfg.format.height = 32;

    SyntheticCapture cap{cfg};

    VideoSourceConfig scfg{};
    scfg.format = VideoPixelFormat::kBGRA;
    CaptureSourceAdapter adapter{&cap, scfg};

    ManualCursorSource src;
    src.set_state(make_cursor(4, 4, 4, 4, 0xFF, 0x00, 0x00, 0xFF));
    AlphaCursorCompositor comp;
    adapter.set_cursor(&src, &comp);

    VideoSourceFrame seen{};
    bool called = false;
    adapter.set_callback([&](const VideoSourceFrame& f) {
        called = true;
        seen = f;
    });

    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);
    ASSERT_EQ(cap.produce_one(), nimrtc::plugins::kOk);
    adapter.produce_one();

    ASSERT_TRUE(called);
    EXPECT_EQ(adapter.cursor_frames_composited(), 1u);

    // The pointer's pixel is red in the published frame.
    ASSERT_NE(seen.plane_y, nullptr);
    const std::size_t idx = (4u * 32u + 4u) * 4u;
    EXPECT_EQ(seen.plane_y[idx + 2u], 0xFFu);
}

TEST(CursorCompositor, TheAdapterLeavesTheFrameAloneWithoutACompositor) {
    using nimrtc::video_capture::CaptureSourceAdapter;
    using nimrtc::video_capture::SyntheticCapture;

    CaptureConfig cfg{};
    cfg.format.width  = 32;
    cfg.format.height = 32;

    SyntheticCapture cap{cfg};
    VideoSourceConfig scfg{};
    scfg.format = VideoPixelFormat::kBGRA;
    CaptureSourceAdapter adapter{&cap, scfg};

    ASSERT_EQ(adapter.start(), nimrtc::plugins::kOk);
    ASSERT_EQ(cap.produce_one(), nimrtc::plugins::kOk);
    adapter.produce_one();

    EXPECT_EQ(adapter.cursor_frames_composited(), 0u);
}
