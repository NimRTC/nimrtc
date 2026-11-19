// src/modules/video_sync/tests/test_render_clock.cpp
// ----------------------------------------------------------------------------
// The render clock — the video half of the A/V-sync correction
// (v1.3 plan Q6 / remote-desktop plan R3).
//
// The clock is a pure function of the skew plus its own counters: no
// clock, no allocation, no thread, no GPU, no window-system type.  So
// these cases run on every platform, including the CI legs that have no
// display at all.
// ----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstdint>

#include "nimrtc/video_sync/render_clock.hpp"

namespace nimrtc::video_sync {
namespace {

namespace np = nimrtc::plugins;

/// A clock with the default configuration.
RenderClock make_clock() {
    return RenderClock{};
}

/// A clock with an explicit tolerance band and hold cap.
RenderClock make_clock(std::int64_t tolerance_us, std::int64_t max_delay_us) {
    np::VideoSyncConfig cfg;
    cfg.tolerance_us = tolerance_us;
    cfg.max_delay_us = max_delay_us;
    return RenderClock{cfg};
}

// ---------------------------------------------------------------------------
// The band
// ---------------------------------------------------------------------------

TEST(RenderClock, ASkewInsideTheBandIsPresented) {
    auto clock = make_clock(40'000, 200'000);

    EXPECT_EQ(clock.decide(0, 1000).action, np::VideoSyncAction::kPresent);

    // Exactly on the band edge -- still inside (the band is inclusive).
    EXPECT_EQ(clock.decide(40'000, 1000).action, np::VideoSyncAction::kPresent);
    EXPECT_EQ(clock.decide(-40'000, 1000).action, np::VideoSyncAction::kPresent);

    // Just inside the band.
    EXPECT_EQ(clock.decide(39'999, 1000).action, np::VideoSyncAction::kPresent);
    EXPECT_EQ(clock.decide(-39'999, 1000).action, np::VideoSyncAction::kPresent);
}

TEST(RenderClock, TheSkewAndTheCaptureTimestampAreCarriedThrough) {
    auto clock = make_clock();

    const auto out = clock.decide(123'456, 987'654);
    EXPECT_EQ(out.skew_us, 123'456);
    EXPECT_EQ(out.capture_ts_us, 987'654);
}

// ---------------------------------------------------------------------------
// The sign convention -- the whole point of the module
// ---------------------------------------------------------------------------

TEST(RenderClock, APictureAheadOfTheSoundIsHeld) {
    auto clock = make_clock(40'000, 200'000);

    // Positive skew = the picture is *ahead*: it was captured later than
    // the sound that is at the speaker now, so showing it would put the
    // image on the screen before the sound it belongs to.  It has to wait.
    const auto out = clock.decide(100'000, 1000);
    EXPECT_EQ(out.action, np::VideoSyncAction::kDelay);
    EXPECT_GT(out.delay_us, 0);
    // The hold is the skew *minus* the tolerance band.
    EXPECT_EQ(out.delay_us, 100'000 - 40'000);
}

TEST(RenderClock, APictureBehindTheSoundIsDropped) {
    auto clock = make_clock(40'000, 200'000);

    // Negative skew = the picture is *behind*: the operator has already
    // heard the moment it belongs to, so holding it would only make it
    // later.  It is dropped.
    const auto out = clock.decide(-100'000, 1000);
    EXPECT_EQ(out.action, np::VideoSyncAction::kDrop);
    EXPECT_EQ(out.delay_us, 0) << "a drop never asks for a hold";
}

TEST(RenderClock, AHoldThatIsTooLongBecomesADrop) {
    // The hold would be longer than the cap: waiting that long is not
    // worth it -- the picture would still be misaligned when it finally
    // appeared, and the wait itself is visible as a freeze.  The frame
    // is dropped instead.
    auto clock = make_clock(40'000, 100'000);

    const auto out = clock.decide(500'000, 1000);
    EXPECT_EQ(out.action, np::VideoSyncAction::kDrop);
    EXPECT_EQ(out.delay_us, 0);
}

TEST(RenderClock, TheHoldIsAlwaysBoundedByTheCap) {
    auto clock = make_clock(40'000, 100'000);

    // A skew far beyond the cap: the hold is refused (a drop), and the
    // answer is never a hold beyond the cap.
    const auto out = clock.decide(5'000'000, 1000);
    EXPECT_LE(out.delay_us, clock.max_delay_us());
    EXPECT_GE(out.delay_us, 0);

    // And it stays bounded no matter how many spikes arrive.
    for (int i = 0; i < 100; ++i) {
        const auto spike = clock.decide(5'000'000, 1000 + i);
        EXPECT_LE(spike.delay_us, clock.max_delay_us());
        EXPECT_GE(spike.delay_us, 0);
    }
}

TEST(RenderClock, TheHoldIsSettledAgainstWhatThePresenterReallyWaited) {
    auto clock = make_clock(40'000, 200'000);

    // The clock promises a hold of 60 ms.
    const auto out = clock.decide(100'000, 1);
    ASSERT_EQ(out.action, np::VideoSyncAction::kDelay);
    EXPECT_EQ(out.delay_us, 60'000);

    // The presenter reports what it really waited.  The reported total
    // is that reality, not the promise.
    clock.note_delay_applied(45'000);
    EXPECT_EQ(clock.counters().total_delay_us, 45'000);
}

TEST(RenderClock, AHoldThatNeverHappensIsReportedAsZero) {
    auto clock = make_clock(40'000, 200'000);

    (void)clock.decide(100'000, 1);   // the clock promises 60 ms
    clock.note_delay_applied(0);      // the presenter did not wait

    EXPECT_EQ(clock.counters().total_delay_us, 0)
        << "the reported total must be the realised one, not the promise";
}

TEST(RenderClock, ADropCancelsThePromisedHold) {
    // The cap is deliberately *not* binding here: the point of the test
    // is what a drop does to the outstanding promise, and a cap that
    // clamped the answer would hide the difference between "the promise
    // was cancelled" and "the answer was clamped".
    auto clock = make_clock(40'000, 2'000'000);

    (void)clock.decide(100'000, 1);    // hold -- 60 ms promised
    EXPECT_EQ(clock.decide(-100'000, 2).action, np::VideoSyncAction::kDrop);

    // The promise was cancelled by the drop, so the reported total is
    // back to zero rather than carrying a hold that will never happen.
    EXPECT_EQ(clock.counters().total_delay_us, 0);
}

TEST(RenderClock, ASecondHoldDoesNotStackOnTheFirst) {
    // Only one hold can be outstanding at a time: the second one settles
    // the first.  That is what keeps the accumulated hold bounded, and
    // an unbounded hold is a latency bomb (a single clock glitch would
    // push every later frame into the future).
    auto clock = make_clock(40'000, 200'000);

    (void)clock.decide(100'000, 1);   // hold -- 60 ms promised
    (void)clock.decide(100'000, 2);   // hold -- the first one is settled

    // One hold's worth in the reported total, not two.
    EXPECT_EQ(clock.counters().total_delay_us, 60'000);
    EXPECT_EQ(clock.counters().frames_delayed, 2u);
}

// ---------------------------------------------------------------------------
// The counters
// ---------------------------------------------------------------------------

TEST(RenderClock, TheCountersMoveWithTheDecisions) {
    auto clock = make_clock(40'000, 200'000);

    (void)clock.decide(0, 1);          // present
    (void)clock.decide(-100'000, 2);   // drop   (picture behind)
    (void)clock.decide(100'000, 3);    // hold   (picture ahead)
    (void)clock.decide(10'000, 4);     // present

    const auto k = clock.counters();
    EXPECT_EQ(k.frames_presented, 2u);
    EXPECT_EQ(k.frames_dropped, 1u);
    EXPECT_EQ(k.frames_delayed, 1u);
    EXPECT_EQ(k.total_delay_us, 60'000);
    EXPECT_EQ(k.last_skew_us, 10'000);
}

TEST(RenderClock, TheCountersAreCumulativeAcrossTheFrames) {
    auto clock = make_clock(40'000, 200'000);

    for (int i = 0; i < 50; ++i) {
        (void)clock.decide(100'000, i);
    }
    EXPECT_EQ(clock.counters().frames_delayed, 50u);
    EXPECT_EQ(clock.counters().frames_presented, 0u);
}

TEST(RenderClock, ResetClearsTheCountersAndThePromise) {
    auto clock = make_clock(40'000, 2'000'000);
    (void)clock.decide(100'000, 1);
    clock.reset_counters();

    EXPECT_EQ(clock.counters().frames_delayed, 0u);
    EXPECT_EQ(clock.counters().total_delay_us, 0);

    // The promise went with the counters: the next hold is a fresh one.
    const auto after = clock.decide(100'000, 2);
    EXPECT_EQ(after.delay_us, 100'000 - 40'000);
    EXPECT_EQ(clock.counters().total_delay_us, 100'000 - 40'000);
}

// ---------------------------------------------------------------------------
// The switch (Q6-1)
// ---------------------------------------------------------------------------

TEST(RenderClock, ADisabledClockPresentsEverything) {
    np::VideoSyncConfig cfg;
    cfg.enabled = false;
    RenderClock clock{cfg};

    EXPECT_FALSE(clock.enabled());

    // Even a wildly skewed frame is presented when the clock is off.
    EXPECT_EQ(clock.decide(5'000'000, 1).action, np::VideoSyncAction::kPresent);
    EXPECT_EQ(clock.decide(-5'000'000, 2).action, np::VideoSyncAction::kPresent);
    EXPECT_EQ(clock.counters().frames_dropped, 0u);
    EXPECT_EQ(clock.counters().frames_delayed, 0u);
}

TEST(RenderClock, TheSwitchCanBeFlippedAtRuntime) {
    auto clock = make_clock(40'000, 200'000);

    EXPECT_EQ(clock.decide(-100'000, 1).action, np::VideoSyncAction::kDrop);

    clock.set_enabled(false);
    EXPECT_FALSE(clock.enabled());
    EXPECT_EQ(clock.decide(-100'000, 2).action, np::VideoSyncAction::kPresent);

    clock.set_enabled(true);
    EXPECT_TRUE(clock.enabled());
    EXPECT_EQ(clock.decide(-100'000, 3).action, np::VideoSyncAction::kDrop);
}

// ---------------------------------------------------------------------------
// The configuration
// ---------------------------------------------------------------------------

TEST(RenderClock, TheToleranceIsConfigurable) {
    auto clock = make_clock(5'000, 200'000);

    EXPECT_EQ(clock.tolerance_us(), 5'000);
    EXPECT_EQ(clock.decide(4'000, 1).action, np::VideoSyncAction::kPresent);
    EXPECT_EQ(clock.decide(6'000, 2).action, np::VideoSyncAction::kDelay);
}

TEST(RenderClock, ANegativeToleranceIsClampedToZero) {
    np::VideoSyncConfig cfg;
    cfg.tolerance_us = -1;
    RenderClock clock{cfg};

    EXPECT_EQ(clock.tolerance_us(), 0);
    EXPECT_EQ(clock.decide(0, 1).action, np::VideoSyncAction::kPresent)
        << "with a zero band, a zero skew is still inside it";
    EXPECT_EQ(clock.decide(1, 2).action, np::VideoSyncAction::kDelay);
}

TEST(RenderClock, ANegativeHoldCapIsClampedToZero) {
    np::VideoSyncConfig cfg;
    cfg.max_delay_us = -1;
    RenderClock clock{cfg};

    EXPECT_EQ(clock.max_delay_us(), 0);
    const auto out = clock.decide(100'000, 1);
    EXPECT_EQ(out.action, np::VideoSyncAction::kDrop)
        << "a zero cap means no hold is ever worth waiting for";
}

// ---------------------------------------------------------------------------
// The history ring
// ---------------------------------------------------------------------------

TEST(RenderClock, TheHistoryRingKeepsTheRecentDecisions) {
    auto clock = make_clock(40'000, 200'000);

    // The skews must be outside the band for the decisions to differ.
    (void)clock.decide(0, 1);          // present
    (void)clock.decide(-100'000, 2);   // drop
    (void)clock.decide(100'000, 3);    // hold

    // Slot 0 is the most recent.
    EXPECT_EQ(clock.history_decision(0), np::VideoSyncAction::kDelay);
    EXPECT_EQ(clock.history_decision(1), np::VideoSyncAction::kDrop);
    EXPECT_EQ(clock.history_decision(2), np::VideoSyncAction::kPresent);
    EXPECT_EQ(clock.history_skew_us(0), 100'000);
    EXPECT_EQ(clock.history_skew_us(1), -100'000);
}

TEST(RenderClock, TheHistoryRingIsBounded) {
    auto clock = make_clock(40'000, 200'000);
    EXPECT_EQ(RenderClock::history_size(), 16u);

    for (int i = 0; i < 100; ++i) {
        (void)clock.decide(100'000, i);
    }

    // Slot 0 is still the most recent.
    EXPECT_EQ(clock.history_decision(0), np::VideoSyncAction::kDelay);
    // A slot beyond the ring is a harmless "present".
    EXPECT_EQ(clock.history_decision(999), np::VideoSyncAction::kPresent);
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

TEST(RenderClock, TheDecisionIsDeterministic) {
    // The same inputs must give the same answer -- the clock has no
    // clock of its own and no hidden state beyond its own counters.
    auto a = make_clock(25'000, 200'000);
    auto b = make_clock(25'000, 200'000);

    for (int i = 0; i < 10; ++i) {
        const std::int64_t skew = (i % 3 == 0) ? 0 : ((i % 3 == 1) ? 50'000 : -50'000);
        const auto oa = a.decide(skew, i);
        const auto ob = b.decide(skew, i);
        EXPECT_EQ(oa.action, ob.action);
        EXPECT_EQ(oa.delay_us, ob.delay_us);
    }
}

TEST(RenderClock, TwoClocksWithTheSameInputsAgreeOnTheCounters) {
    auto a = make_clock(25'000, 200'000);
    auto b = make_clock(25'000, 200'000);

    for (int i = 0; i < 10; ++i) {
        const std::int64_t skew = (i % 3 == 0) ? 0 : ((i % 3 == 1) ? 50'000 : -50'000);
        (void)a.decide(skew, i);
        (void)b.decide(skew, i);
    }

    EXPECT_EQ(a.counters().frames_presented, b.counters().frames_presented);
    EXPECT_EQ(a.counters().frames_dropped, b.counters().frames_dropped);
    EXPECT_EQ(a.counters().frames_delayed, b.counters().frames_delayed);
    EXPECT_EQ(a.counters().total_delay_us, b.counters().total_delay_us);
}

// ---------------------------------------------------------------------------
// The end-to-end shape: a synthetic skew produces a documented correction
// ---------------------------------------------------------------------------

TEST(RenderClock, ASyntheticSkewProducesTheDocumentedCorrection) {
    // The scenario the plan names: a session whose picture runs ahead of
    // the sound.  Every frame is held; nothing is dropped.
    auto clock = make_clock(40'000, 200'000);

    for (int i = 0; i < 10; ++i) {
        (void)clock.decide(120'000, 1000 + i);
    }

    const auto k = clock.counters();
    EXPECT_EQ(k.frames_delayed, 10u);
    EXPECT_EQ(k.frames_dropped, 0u);
    EXPECT_EQ(k.frames_presented, 0u);
    EXPECT_EQ(k.last_skew_us, 120'000);
}

TEST(RenderClock, ASyntheticLagProducesTheDocumentedDrop) {
    // The mirror scenario: the picture runs behind the sound, so every
    // frame is dropped -- the moment the operator heard is already gone.
    auto clock = make_clock(40'000, 200'000);

    for (int i = 0; i < 10; ++i) {
        (void)clock.decide(-100'000, 1000 + i);
    }

    const auto k = clock.counters();
    EXPECT_EQ(k.frames_dropped, 10u);
    EXPECT_EQ(k.frames_delayed, 0u);
    EXPECT_EQ(k.total_delay_us, 0);
}

}  // namespace
}  // namespace nimrtc::video_sync
