/**
 * @file tests/test_engine_rate_window.cpp
 * @brief The windowed outbound rate measurement (v1.2 plan Track E / E6).
 *
 * ## What this file covers
 *
 * `IVideoBitrateController::on_measured_bitrate(bps)` is the seam the
 * rate controller is designed around: the link's estimate says what the
 * path can carry, and the measurement says what the encoder is
 * *actually* putting on the wire.  The controller compares the two and
 * applies a multiplicative decrease when the encoder overruns the
 * estimate (see `BitrateController::budget()`).
 *
 * Until this work, **nothing in the engine ever called that seam**.  The
 * engine had the encoder's outbound byte counters and the controller
 * had the seam, and the two were never joined — so the closed loop the
 * saturation benchmark measures was not the loop the product runs.
 *
 * The *rule* — which interval, which direction, what a broken counter
 * means — lives in `engine_rate_window.hpp`, and this file is where it
 * is pinned.  The rule is deliberately tested **deterministically**:
 * the window is driven by an injected timestamp, so the test asserts
 * the arithmetic rather than sleeping and hoping.
 *
 * The engine-level half — that the engine actually *calls* the seam —
 * is in `test_engine_video_bitrate_control.cpp`.
 */

#include <gtest/gtest.h>

#include <cstdint>

// The implementation under test: the same header the engine's video
// state includes, so the rule and the test are the same rule.
#include "engine_rate_window.hpp"

namespace {

using nimrtc::engine::detail::RateWindow;

constexpr std::int64_t kMs = 1'000;

// ---------------------------------------------------------------------------
// The first observation has no interval, so it has no rate
// ---------------------------------------------------------------------------

TEST(RateWindow, TheFirstObservationReportsNoRate) {
    RateWindow w;
    std::uint32_t bps = 0;

    EXPECT_FALSE(w.observe(0, 0, bps))
        << "there is no interval yet, so no window has closed";
    EXPECT_EQ(bps, 0u);
}

// ---------------------------------------------------------------------------
// A window that has not elapsed reports nothing
// ---------------------------------------------------------------------------

TEST(RateWindow, AWindowThatHasNotElapsedReportsNothing) {
    RateWindow w;
    std::uint32_t bps = 0;

    w.observe(0, 0, bps);
    // 999 ms later: still inside the window.
    EXPECT_FALSE(w.observe(100'000, 999 * kMs, bps))
        << "the window is one second wide; 999 ms is not a window";
    EXPECT_EQ(bps, 0u);
}

// ---------------------------------------------------------------------------
// A closed window reports the rate it spanned
// ---------------------------------------------------------------------------

TEST(RateWindow, AClosedWindowReportsTheRateItSpanned) {
    RateWindow w;
    std::uint32_t bps = 0;

    w.observe(0, 0, bps);
    // 125 000 bytes in one second = 1 000 000 bits per second.
    EXPECT_TRUE(w.observe(125'000, 1'000 * kMs, bps));
    EXPECT_EQ(bps, 1'000'000u);
}

TEST(RateWindow, TheWindowMeasuresBitsNotBytes) {
    RateWindow w;
    std::uint32_t bps = 0;

    w.observe(0, 0, bps);
    // 1 000 bytes in one second = 8 000 bits per second.
    EXPECT_TRUE(w.observe(1'000, 1'000 * kMs, bps));
    EXPECT_EQ(bps, 8'000u);
}

// ---------------------------------------------------------------------------
// The window is *closed*, not sliding
// ---------------------------------------------------------------------------

TEST(RateWindow, TheAnchorDoesNotMoveWhileTheWindowIsOpen) {
    RateWindow w;
    std::uint32_t bps = 0;

    w.observe(0, 0, bps);
    // A burst at 500 ms.
    w.observe(50'000, 500 * kMs, bps);
    EXPECT_EQ(bps, 0u) << "the window has not closed yet";

    // At 1 000 ms the window closes and spans the *whole* second, so
    // the burst is inside the measurement rather than starting a new
    // window at the burst.
    EXPECT_TRUE(w.observe(50'000, 1'000 * kMs, bps));
    EXPECT_EQ(bps, 50'000u * 8u) << "500 000 bits over one second";
}

TEST(RateWindow, EachWindowIsMeasuredFromItsOwnAnchor) {
    RateWindow w;
    std::uint32_t bps = 0;

    w.observe(0, 0, bps);
    EXPECT_TRUE(w.observe(10'000, 1'000 * kMs, bps));
    EXPECT_EQ(bps, 80'000u);

    // The second window measures only what happened *after* the first
    // one closed: 10 000 more bytes over one second.
    EXPECT_TRUE(w.observe(20'000, 2'000 * kMs, bps));
    EXPECT_EQ(bps, 80'000u) << "the second window must not count the first one's bytes";
}

// ---------------------------------------------------------------------------
// A counter that does not move reports zero
// ---------------------------------------------------------------------------

TEST(RateWindow, AStreamThatSentNothingReportsZero) {
    RateWindow w;
    std::uint32_t bps = 0;

    w.observe(0, 0, bps);
    EXPECT_TRUE(w.observe(0, 1'000 * kMs, bps));
    EXPECT_EQ(bps, 0u) << "a quiet stream is not a stream that is overrunning the link";
}

// ---------------------------------------------------------------------------
// A counter that goes backwards reports zero, and does not wedge
// ---------------------------------------------------------------------------

TEST(RateWindow, ACounterThatWentBackwardsReportsZero) {
    RateWindow w;
    std::uint32_t bps = 0;

    w.observe(100'000, 0, bps);
    // The counter went backwards — the session was reset underneath
    // the measurement.  The rate is unknown, not a negative number
    // and not a plausible-looking wrong number.
    EXPECT_TRUE(w.observe(50'000, 1'000 * kMs, bps));
    EXPECT_EQ(bps, 0u);
}

TEST(RateWindow, AResetCounterDoesNotWedgeTheMeasurement) {
    RateWindow w;
    std::uint32_t bps = 0;

    w.observe(100'000, 0, bps);
    EXPECT_TRUE(w.observe(50'000, 1'000 * kMs, bps));
    EXPECT_EQ(bps, 0u);

    // The next window measures from the *new* baseline, so the
    // measurement recovers on its own.
    EXPECT_TRUE(w.observe(60'000, 2'000 * kMs, bps));
    EXPECT_EQ(bps, 80'000u) << "10 000 bytes over one second";
}

// ---------------------------------------------------------------------------
// The reported rate is clamped, not wrapped
// ---------------------------------------------------------------------------

TEST(RateWindow, AnAbsurdRateIsClampedRatherThanWrapped) {
    RateWindow w;
    std::uint32_t bps = 0;

    w.observe(0, 0, bps);
    // 2^32 bytes in one second is far beyond any link, and far beyond
    // what fits in 32 bits as a bitrate.
    EXPECT_TRUE(w.observe(0x1'0000'0000ull, 1'000 * kMs, bps));
    EXPECT_EQ(bps, 0xFFFFFFFFu);
}

// ---------------------------------------------------------------------------
// The window length is one second
// ---------------------------------------------------------------------------

TEST(RateWindow, TheWindowIsOneSecondWide) {
    EXPECT_EQ(RateWindow::kWindowUs, 1'000'000);
}

}  // namespace
