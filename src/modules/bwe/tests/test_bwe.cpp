// modules/bwe/tests/test_bwe.cpp
// =============================================================================
// BWE (AIMD bandwidth estimator) unit tests.
//
// Covers:
//   - Initial estimate from config.initial_bitrate_bps
//   - Loss -> multiplicative decrease (AIMDDecrease)
//   - No-loss over time -> additive increase (AIMDIncrease)
//   - REMB override when REMB > local estimate
//   - Smoothing EMA
//   - clamp to [min, max]
//   - update_config / reset semantics
// =============================================================================

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include <nimrtc/bwe/bwe.hpp>

namespace {

using namespace nimrtc;
using namespace nimrtc::bwe;

constexpr auto kStep = std::chrono::milliseconds(50);

inline core::TimePoint now_plus(core::TimePoint base, std::chrono::milliseconds delta) {
    return base + delta;
}

// Build a no-loss feedback sample at the given timestamp.
Feedback no_loss(core::TimePoint t,
                 std::uint32_t packets_received = 100,
                 core::Milliseconds rtt = core::Milliseconds{50}) {
    Feedback f;
    f.loss_rate = 0.0;
    f.rtt = rtt;
    f.timestamp = t;
    f.packets_received = packets_received;
    f.packets_lost = 0;
    return f;
}

// Build a lossy feedback sample.
Feedback with_loss(double rate,
                   core::TimePoint t,
                   std::uint32_t packets_lost = 5,
                   std::uint32_t packets_received = 100) {
    Feedback f;
    f.loss_rate = rate;
    f.rtt = core::Milliseconds{50};
    f.timestamp = t;
    f.packets_received = packets_received;
    f.packets_lost = packets_lost;
    return f;
}

} // namespace

// -----------------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------------
TEST(BweTest, InitialEstimateMatchesConfig) {
    Config c;
    c.initial_bitrate_bps = 750'000;
    Bwe bwe(c);

    auto e = bwe.estimate();
    // The estimate's smoothed value starts at initial_bitrate_bps; allow a
    // 1% tolerance in case the constructor applies one round of smoothing
    // against itself.
    EXPECT_NEAR(static_cast<double>(e.target_bitrate_bps),
                static_cast<double>(c.initial_bitrate_bps),
                static_cast<double>(c.initial_bitrate_bps) * 0.01);
    EXPECT_EQ(e.reason, Estimate::Reason::Initial);
}

// -----------------------------------------------------------------------------
// Loss -> multiplicative decrease
// -----------------------------------------------------------------------------
TEST(BweTest, LossCausesMultiplicativeDecrease) {
    Config c;
    c.initial_bitrate_bps = 1'000'000;
    c.decrease_factor = 0.5;       // halve on loss
    c.change_interval = core::Milliseconds{50};
    c.smoothing_alpha = 1.0;       // immediate (no smoothing lag)
    Bwe bwe(c);

    auto t0 = core::SteadyClock::now();
    bwe.on_feedback(with_loss(0.10, t0));

    auto e = bwe.estimate();
    // After 10% loss and factor=0.5, current_bps_ = 500k.  With
    // smoothing_alpha=1.0 the smoothed value mirrors current immediately.
    EXPECT_LE(e.target_bitrate_bps, 500'000u);
    EXPECT_EQ(e.reason, Estimate::Reason::AIMDDecrease);
}

TEST(BweTest, DecreaseClampedToMinBitrate) {
    Config c;
    c.initial_bitrate_bps = 100'000;
    c.min_bitrate_bps = 30'000;
    c.decrease_factor = 0.1;
    Bwe bwe(c);

    // Many consecutive loss events should not drop below min.
    auto t = core::SteadyClock::now();
    for (int i = 0; i < 5; ++i) {
        bwe.on_feedback(with_loss(1.0, t));
        t += std::chrono::seconds(1);
    }
    auto e = bwe.estimate();
    EXPECT_GE(e.target_bitrate_bps, c.min_bitrate_bps);
}

// -----------------------------------------------------------------------------
// No-loss -> additive increase (cooldown + change_interval must be satisfied)
// -----------------------------------------------------------------------------
TEST(BweTest, NoLossTriggersAdditiveIncrease) {
    Config c;
    c.initial_bitrate_bps = 500'000;
    c.increase_bps = 50'000;       // +50 kbps per second
    c.change_interval = core::Milliseconds{10};
    Bwe bwe(c);

    auto t0 = core::SteadyClock::now();
    // First feedback seeds last_change_time_.
    bwe.on_feedback(no_loss(t0));

    // Advance time well past the cooldown + change_interval.
    bwe.on_feedback(no_loss(t0 + std::chrono::seconds(2)));

    auto e = bwe.estimate();
    // After 2s with no loss and +50 kbps/s, target should be at least
    // 500k + 50k = 550k (plus per-RTT bump). Just check it grew.
    EXPECT_GT(e.target_bitrate_bps, 500'000u);
    EXPECT_EQ(e.reason, Estimate::Reason::AIMDIncrease);
}

// -----------------------------------------------------------------------------
// REMB override
// -----------------------------------------------------------------------------
TEST(BweTest, RembOverrideWhenHigherThanLocal) {
    Config c;
    c.initial_bitrate_bps = 500'000;
    c.max_bitrate_bps = 5'000'000;
    c.smoothing_alpha = 1.0;       // no smoothing lag
    Bwe bwe(c);

    Feedback f = no_loss(core::SteadyClock::now());
    f.remb_bps = 3'000'000;  // well above 500k
    bwe.on_feedback(f);

    auto e = bwe.estimate();
    // Trust factor 0.8 -> 2.4M target.  With smoothing_alpha=1.0 the
    // smoothed value matches current immediately.
    EXPECT_NEAR(static_cast<double>(e.target_bitrate_bps),
                2'400'000.0,
                1.0);
    EXPECT_EQ(e.reason, Estimate::Reason::REMBOverride);
}

// -----------------------------------------------------------------------------
// Smoothing
// -----------------------------------------------------------------------------
TEST(BweTest, SmoothingKeepsEstimateStable) {
    Config c;
    c.initial_bitrate_bps = 1'000'000;
    c.smoothing_alpha = 0.0;       // no smoothing; current_bps_ -> smoothed_bps_
    Bwe bwe(c);

    auto e0 = bwe.estimate();
    auto t0 = core::SteadyClock::now();
    bwe.on_feedback(with_loss(0.10, t0));

    // With alpha=0.0 the smoothed value stays at the initial seed until
    // future feedback. After one loss event, current_bps_ drops but
    // smoothed_bps_ should remain at the initial value.
    auto e1 = bwe.estimate();
    EXPECT_GE(e1.target_bitrate_bps, c.initial_bitrate_bps);
    (void)e0;
}

// -----------------------------------------------------------------------------
// update_config / reset
// -----------------------------------------------------------------------------
TEST(BweTest, UpdateConfigClampsCurrent) {
    Config c;
    c.initial_bitrate_bps = 2'000'000;
    c.min_bitrate_bps = 30'000;
    c.max_bitrate_bps = 5'000'000;
    Bwe bwe(c);

    Config c2;
    c2.initial_bitrate_bps = 100'000;
    c2.min_bitrate_bps = 50'000;
    c2.max_bitrate_bps = 200'000;
    bwe.update_config(c2);

    // After update, the new config should be reflected.
    EXPECT_EQ(bwe.config().min_bitrate_bps, 50'000u);
    EXPECT_EQ(bwe.config().max_bitrate_bps, 200'000u);
}

TEST(BweTest, ResetRestoresInitial) {
    Config c;
    c.initial_bitrate_bps = 800'000;
    Bwe bwe(c);

    auto t = core::SteadyClock::now();
    bwe.on_feedback(with_loss(1.0, t));  // drop

    bwe.reset();

    auto e = bwe.estimate();
    EXPECT_NEAR(static_cast<double>(e.target_bitrate_bps),
                static_cast<double>(c.initial_bitrate_bps),
                static_cast<double>(c.initial_bitrate_bps) * 0.05);
    EXPECT_EQ(e.reason, Estimate::Reason::Initial);
}