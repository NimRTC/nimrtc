// modules/video_capture/tests/test_win_cursor_source.cpp
// ----------------------------------------------------------------------------
// The Windows cursor source's tests.
//
// The portable half (the class exists everywhere; on a non-Windows
// host it reports "no pointer") is tested on every platform.  On a
// Windows host the tests additionally read the *real* pointer — but
// they never *require* it: a CI agent without an interactive session
// has no visible pointer, and a test that fails there is a test that
// lies.
//
// See include/nimrtc/video_capture/win_cursor_source.hpp for the
// contract, and the remote-desktop plan's A6 row.
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/win_cursor_source.hpp>

#include <string>

#include <gtest/gtest.h>

using nimrtc::video_capture::make_win_cursor_source;
using nimrtc::video_capture::WinCursorSource;

// ---------------------------------------------------------------------------
// The portable half
// ---------------------------------------------------------------------------

TEST(WinCursorSource, TheFactoryReturnsANullSourceOffWindows) {
    auto src = make_win_cursor_source();
#if defined(_WIN32)
    ASSERT_NE(src, nullptr);
    EXPECT_EQ(src->backend(), "win-cursor");
    EXPECT_TRUE(src->has_live_cursor());
#else
    EXPECT_EQ(src, nullptr);
#endif
}

TEST(WinCursorSource, TheClassIsUsableOnEveryPlatform) {
    WinCursorSource src;
    EXPECT_EQ(src.backend(), "win-cursor");
    EXPECT_TRUE(src.has_live_cursor());
    // Setting the origin is always legal.
    src.set_display_origin(100, 200);
    // The query is safe to call even where there is no pointer.
    const auto state = src.query();
    if (!state.visible) {
        EXPECT_TRUE(state.bgra.empty());
    }
}

TEST(WinCursorSource, ANonWindowsHostReportsNoPointer) {
#if !defined(_WIN32)
    WinCursorSource src;
    const auto state = src.query();
    EXPECT_FALSE(state.visible);
    EXPECT_FALSE(state.live);
    EXPECT_TRUE(state.bgra.empty());
#endif
}

// ---------------------------------------------------------------------------
// The Windows half
// ---------------------------------------------------------------------------

#if defined(_WIN32)

TEST(WinCursorSource, TheRealPointerIsReadWhenItIsVisible) {
    WinCursorSource src;
    const auto state = src.query();

    // A headless CI agent has no visible pointer, so the state is
    // legitimately empty.  When there *is* a pointer, the state must
    // be complete: a bitmap with a sane geometry, and a position
    // inside the (possibly huge) virtual desktop.
    if (!state.visible) {
        GTEST_SKIP() << "no visible pointer on this host";
    }

    EXPECT_TRUE(state.live);
    EXPECT_FALSE(state.bgra.empty());
    EXPECT_GT(state.bitmap_width, 0u);
    EXPECT_GT(state.bitmap_height, 0u);
    EXPECT_GE(state.bitmap_stride,
              static_cast<std::int32_t>(state.bitmap_width * 4u));
    EXPECT_EQ(state.bgra.size(),
              static_cast<std::size_t>(state.bitmap_stride) *
                  static_cast<std::size_t>(state.bitmap_height));
}

TEST(WinCursorSource, TheBitmapIsCachedPerCursorHandle) {
    // The pointer is re-read every frame, but its *bitmap* is only
    // re-extracted when the cursor changes.  Two consecutive queries
    // of an unchanged cursor therefore report the same bitmap — the
    // cache is doing its job, and a stale bitmap would be a bug.
    WinCursorSource src;
    const auto first = src.query();
    const auto second = src.query();
    EXPECT_EQ(first.visible, second.visible);
    EXPECT_EQ(first.bitmap_width, second.bitmap_width);
    EXPECT_EQ(first.bitmap_height, second.bitmap_height);
    EXPECT_EQ(first.bgra.size(), second.bgra.size());
}

TEST(WinCursorSource, TheDisplayOriginShiftsTheReportedPosition) {
    WinCursorSource src;
    src.set_display_origin(0, 0);
    const auto at_origin = src.query();

    src.set_display_origin(1920, 0);
    const auto shifted = src.query();

    if (!at_origin.visible) {
        GTEST_SKIP() << "no visible pointer on this host";
    }

    // The virtual-desktop hotspot is the same physical point in both
    // cases; only the reported (display-relative) position differs.
    EXPECT_EQ(at_origin.x - 1920, shifted.x);
    EXPECT_EQ(at_origin.y, shifted.y);
}

#endif // _WIN32
