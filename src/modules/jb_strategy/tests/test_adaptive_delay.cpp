// modules/jb_strategy/tests/test_adaptive_delay.cpp
// =============================================================================
// Shared adaptive jitter-buffer strategy — unit tests.
//
// Covers the acceptance criteria of v1.1 track Q2:
//   - the jitter estimate converges,
//   - the target delay follows the jitter estimate,
//   - the [min_delay, max_delay] window bounds hold.
// =============================================================================

#include <gtest/gtest.h>

#include <nimrtc/jb_strategy/adaptive_delay.hpp>

namespace {

using nimrtc::core::Milliseconds;
using nimrtc::jb_strategy::AdaptiveDelayTracker;
using nimrtc::jb_strategy::DelayPolicy;

constexpr std::int64_t kMs = 1000;

// Feed `count` packets whose media clock advances by `media_step_us` while
// the arrival time advances by `arrival_step_us`.  The difference between
// the two is the injected jitter.
void feed(AdaptiveDelayTracker& tracker,
          std::int64_t arrival_step_us,
          std::int64_t media_step_us,
          int count) {
    std::int64_t arrival_us = 0;
    std::int64_t media_us   = 0;
    for (int i = 0; i < count; ++i) {
        tracker.observe(arrival_us, media_us);
        arrival_us += arrival_step_us;
        media_us   += media_step_us;
    }
}

TEST(JbStrategy, DefaultPolicy) {
    const DelayPolicy p;
    EXPECT_EQ(p.base_delay,      Milliseconds{20});
    EXPECT_EQ(p.min_delay,       Milliseconds{20});
    EXPECT_EQ(p.max_delay,       Milliseconds{400});
    EXPECT_DOUBLE_EQ(p.jitter_multiplier, 4.0);
    EXPECT_TRUE(p.adaptive);
}

TEST(JbStrategy, FreshTrackerHasNoJitter) {
    AdaptiveDelayTracker tracker;
    EXPECT_EQ(tracker.jitter(), Milliseconds{0});
    EXPECT_EQ(tracker.observations(), 0u);
    // A fixed base delay is already reported before any observation.
    EXPECT_EQ(tracker.target_delay(), Milliseconds{20});
}

TEST(JbStrategy, FirstObservationHasNoJitter) {
    // The first packet has no predecessor, so no transit difference exists.
    AdaptiveDelayTracker tracker;
    tracker.observe(/*arrival_us=*/0, /*media_clock_us=*/0);
    EXPECT_EQ(tracker.jitter(), Milliseconds{0});
    EXPECT_EQ(tracker.observations(), 1u);
}

TEST(JbStrategy, SteadyArrivalYieldsZeroJitter) {
    // Sender and receiver advance in lockstep — a perfect link.
    AdaptiveDelayTracker tracker;
    feed(tracker, /*arrival_step_us=*/20 * kMs, /*media_step_us=*/20 * kMs, 40);
    EXPECT_EQ(tracker.jitter(), Milliseconds{0});
    EXPECT_EQ(tracker.target_delay(), Milliseconds{20});
}

TEST(JbStrategy, JitterEstimateConverges) {
    // Sender advances 20 ms per packet; the receiver's arrival alternates
    // between 10 ms and 30 ms steps (±10 ms jitter around the nominal).
    AdaptiveDelayTracker tracker;
    std::int64_t arrival_us = 0;
    std::int64_t media_us   = 0;
    for (int i = 0; i < 200; ++i) {
        tracker.observe(arrival_us, media_us);
        media_us += 20 * kMs;
        arrival_us += (i % 2 == 0) ? 10 * kMs : 30 * kMs;
    }
    // The estimate must settle near the mean absolute transit difference
    // (10 ms), not near the peak (20 ms) and not near zero.
    const auto jitter = tracker.jitter();
    EXPECT_GE(jitter.count(), 8);
    EXPECT_LE(jitter.count(), 12);
}

TEST(JbStrategy, TargetDelayFollowsJitter) {
    AdaptiveDelayTracker tracker;
    const auto quiet_delay = tracker.target_delay();

    // Inject 10 ms of alternating jitter and let the estimate converge.
    std::int64_t arrival_us = 0;
    std::int64_t media_us   = 0;
    for (int i = 0; i < 200; ++i) {
        tracker.observe(arrival_us, media_us);
        media_us += 20 * kMs;
        arrival_us += (i % 2 == 0) ? 10 * kMs : 30 * kMs;
    }

    const auto noisy_delay = tracker.target_delay();
    EXPECT_GT(noisy_delay.count(), quiet_delay.count());
    // target = base_delay (20) + 4 * jitter (10) = 60 ms.
    EXPECT_EQ(noisy_delay.count(), 60);
}

TEST(JbStrategy, MinDelayLowerBoundHolds) {
    DelayPolicy policy;
    policy.base_delay = Milliseconds{5};
    policy.min_delay  = Milliseconds{30};
    policy.max_delay  = Milliseconds{400};

    AdaptiveDelayTracker tracker(policy);
    // Even with zero jitter the base delay is raised to min_delay.
    EXPECT_EQ(tracker.target_delay(), Milliseconds{30});
}

TEST(JbStrategy, MaxDelayUpperBoundHolds) {
    DelayPolicy policy;
    policy.base_delay      = Milliseconds{20};
    policy.min_delay       = Milliseconds{20};
    policy.max_delay       = Milliseconds{50};
    policy.jitter_multiplier = 4.0;

    AdaptiveDelayTracker tracker(policy);
    // 40 ms of jitter would ask for 20 + 4*40 = 180 ms — clamped to 50 ms.
    std::int64_t arrival_us = 0;
    std::int64_t media_us   = 0;
    for (int i = 0; i < 200; ++i) {
        tracker.observe(arrival_us, media_us);
        media_us += 20 * kMs;
        arrival_us += (i % 2 == 0) ? 0 : 40 * kMs;
    }
    EXPECT_EQ(tracker.target_delay(), Milliseconds{50});
}

TEST(JbStrategy, FixedModeIgnoresJitter) {
    DelayPolicy policy;
    policy.adaptive = false;
    policy.base_delay = Milliseconds{55};

    AdaptiveDelayTracker tracker(policy);
    std::int64_t arrival_us = 0;
    std::int64_t media_us   = 0;
    for (int i = 0; i < 100; ++i) {
        tracker.observe(arrival_us, media_us);
        media_us += 20 * kMs;
        arrival_us += (i % 2 == 0) ? 0 : 40 * kMs;
    }
    EXPECT_GT(tracker.jitter().count(), 0);
    EXPECT_EQ(tracker.target_delay(), Milliseconds{55});
}

TEST(JbStrategy, ResetClearsState) {
    AdaptiveDelayTracker tracker;
    std::int64_t arrival_us = 0;
    std::int64_t media_us   = 0;
    for (int i = 0; i < 50; ++i) {
        tracker.observe(arrival_us, media_us);
        media_us += 20 * kMs;
        arrival_us += (i % 2 == 0) ? 10 * kMs : 30 * kMs;
    }
    EXPECT_GT(tracker.jitter().count(), 0);

    tracker.reset();
    EXPECT_EQ(tracker.jitter(), Milliseconds{0});
    EXPECT_EQ(tracker.observations(), 0u);

    // After the reset the next packet is again the first one — no transit
    // difference is computed from the stale predecessor.
    tracker.observe(0, 0);
    EXPECT_EQ(tracker.jitter(), Milliseconds{0});
}

TEST(JbStrategy, SetPolicyKeepsJitterEstimate) {
    AdaptiveDelayTracker tracker;
    std::int64_t arrival_us = 0;
    std::int64_t media_us   = 0;
    for (int i = 0; i < 50; ++i) {
        tracker.observe(arrival_us, media_us);
        media_us += 20 * kMs;
        arrival_us += (i % 2 == 0) ? 10 * kMs : 30 * kMs;
    }
    const auto jitter_before = tracker.jitter();

    DelayPolicy policy;
    policy.max_delay = Milliseconds{1000};
    tracker.set_policy(policy);

    EXPECT_EQ(tracker.jitter(), jitter_before);
    EXPECT_EQ(tracker.policy().max_delay, Milliseconds{1000});
}

} // namespace
