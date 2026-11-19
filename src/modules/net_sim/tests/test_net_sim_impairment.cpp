// src/modules/net_sim/tests/test_net_sim_impairment.cpp
// ----------------------------------------------------------------------------
// The impairment profile's unit tests.
//
// The profile is what the v1.3 plan's Q5 soak applies to the real RTP
// path, so its behaviour has to be pinned down:
//
//   * a clean profile changes nothing (the default must be a *working*
//     link, not a broken one);
//   * the loss rate is the rate it claims to be;
//   * the jitter spreads *around* the base delay (netem's shape), not
//     from zero;
//   * a reordered packet really does arrive after the packets that
//     followed it -- that is the difference between reorder and loss;
//   * the rate limiter charges the serialization time, and its queue
//     is bounded.
//
// The shaper is deterministic, so every assertion here is exact
// rather than statistical wherever it can be.
// ----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <nimrtc/net_sim/impairment.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using nimrtc::net_sim::ImpairmentConfig;
using nimrtc::net_sim::ImpairmentShaper;
using nimrtc::net_sim::PacketDecision;

// ---------------------------------------------------------------------------
// The clean profile
// ---------------------------------------------------------------------------

TEST(NetSimImpairment, ACleanProfileChangesNothing) {
    ImpairmentConfig cfg;
    ImpairmentShaper shaper(cfg);

    for (int i = 0; i < 1000; ++i) {
        const PacketDecision decision = shaper.shape(1200, static_cast<std::int64_t>(i) * 1000);
        EXPECT_FALSE(decision.dropped) << "a clean profile must not drop anything";
        EXPECT_EQ(decision.delay_us, 0) << "a clean profile must not delay anything";
        EXPECT_FALSE(decision.reordered);
    }
    EXPECT_EQ(shaper.packets_seen(), 1000u);
    EXPECT_EQ(shaper.packets_dropped(), 0u);
}

TEST(NetSimImpairment, TheSameSeedGivesTheSameDecisions) {
    // The soak's numbers are only comparable between runs if the
    // profile is reproducible, so this is a load-bearing property and
    // not a nicety.
    ImpairmentConfig cfg;
    cfg.loss_pct   = 17.0;
    cfg.jitter_ms  = 40.0;
    cfg.reorder_pct = 23.0;
    cfg.seed       = 4242;

    ImpairmentShaper a(cfg);
    ImpairmentShaper b(cfg);

    for (int i = 0; i < 500; ++i) {
        const std::int64_t now = static_cast<std::int64_t>(i) * 1000;
        const PacketDecision da = a.shape(1200, now);
        const PacketDecision db = b.shape(1200, now);
        EXPECT_EQ(da.dropped, db.dropped);
        EXPECT_EQ(da.delay_us, db.delay_us);
        EXPECT_EQ(da.reordered, db.reordered);
    }
}

// ---------------------------------------------------------------------------
// Loss
// ---------------------------------------------------------------------------

TEST(NetSimImpairment, LossDropsRoughlyTheAdvertisedShare) {
    ImpairmentConfig cfg;
    cfg.loss_pct = 25.0;
    cfg.seed     = 12345;

    ImpairmentShaper shaper(cfg);

    std::uint64_t dropped = 0;
    constexpr int kPackets = 20000;
    for (int i = 0; i < kPackets; ++i) {
        if (shaper.shape(1000, static_cast<std::int64_t>(i) * 1000).dropped) ++dropped;
    }

    // The binomial standard deviation at n=20000, p=0.25 is about 61
    // packets, so a +/- 5-sigma window is +/- 305.  A profile that
    // drifts outside that window is broken, not unlucky.
    const double ratio = static_cast<double>(dropped) / kPackets;
    EXPECT_NEAR(ratio, 0.25, 0.02) << "the loss rate must be the advertised one";
    EXPECT_EQ(shaper.packets_dropped(), dropped);
}

TEST(NetSimImpairment, FullLossDropsEverything) {
    ImpairmentConfig cfg;
    cfg.loss_pct = 100.0;

    ImpairmentShaper shaper(cfg);
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(shaper.shape(1000, static_cast<std::int64_t>(i) * 1000).dropped);
    }
    EXPECT_EQ(shaper.packets_dropped(), 100u);
}

// ---------------------------------------------------------------------------
// Delay and jitter
// ---------------------------------------------------------------------------

TEST(NetSimImpairment, TheBaseDelayIsApplied) {
    ImpairmentConfig cfg;
    cfg.delay_ms = 120.0;

    ImpairmentShaper shaper(cfg);
    const PacketDecision decision = shaper.shape(1000, 0);
    EXPECT_FALSE(decision.dropped);
    EXPECT_EQ(decision.delay_us, 120000);
}

TEST(NetSimImpairment, TheJitterSpreadsAroundTheBaseDelay) {
    // Netem's `delay TIME JITTER` spreads the delay uniformly *around*
    // TIME, so the effective delay lies in [TIME - JITTER/2,
    // TIME + JITTER/2].  The shaper uses the same shape, which is what
    // makes the two comparable.
    ImpairmentConfig cfg;
    cfg.delay_ms  = 100.0;
    cfg.jitter_ms = 40.0;
    cfg.seed      = 777;

    ImpairmentShaper shaper(cfg);

    std::int64_t lowest = 1LL << 60;
    std::int64_t highest = -(1LL << 60);
    constexpr int kPackets = 5000;
    for (int i = 0; i < kPackets; ++i) {
        const PacketDecision decision =
            shaper.shape(1000, static_cast<std::int64_t>(i) * 1000);
        ASSERT_FALSE(decision.dropped);
        lowest  = std::min(lowest, decision.delay_us);
        highest = std::max(highest, decision.delay_us);
    }

    // The window is [80 ms, 120 ms]; the samples must land inside it
    // and must reach both ends (a uniform spread that never reaches
    // the ends would be a different distribution).
    EXPECT_GE(lowest, 80000);
    EXPECT_LE(highest, 120000);
    EXPECT_LT(lowest, 90000) << "the spread must reach the lower end";
    EXPECT_GT(highest, 110000) << "the spread must reach the upper end";
}

// ---------------------------------------------------------------------------
// Reorder
// ---------------------------------------------------------------------------

TEST(NetSimImpairment, ReorderHoldsThePacketBack) {
    ImpairmentConfig cfg;
    cfg.delay_ms        = 0.0;
    cfg.reorder_pct     = 100.0;
    cfg.reorder_extra_ms = 30.0;

    ImpairmentShaper shaper(cfg);
    const PacketDecision decision = shaper.shape(1000, 0);
    EXPECT_TRUE(decision.reordered);
    EXPECT_FALSE(decision.dropped) << "a reordered packet is late, not lost";
    EXPECT_EQ(decision.delay_us, 30000);
    EXPECT_EQ(shaper.packets_reordered(), 1u);
}

TEST(NetSimImpairment, ReorderIsRareWhenTheProfileSaysSo) {
    ImpairmentConfig cfg;
    cfg.reorder_pct = 5.0;
    cfg.seed        = 31337;

    ImpairmentShaper shaper(cfg);
    std::uint64_t reordered = 0;
    constexpr int kPackets = 20000;
    for (int i = 0; i < kPackets; ++i) {
        if (shaper.shape(1000, static_cast<std::int64_t>(i) * 1000).reordered) ++reordered;
    }
    const double ratio = static_cast<double>(reordered) / kPackets;
    EXPECT_NEAR(ratio, 0.05, 0.01);
}

// ---------------------------------------------------------------------------
// The rate limit
// ---------------------------------------------------------------------------

TEST(NetSimImpairment, TheRateLimitChargesTheSerializationTime) {
    // A 1 Mbps link puts a 1200-byte packet (9600 bits) on the wire in
    // 9.6 ms.  The first packet is therefore delayed by exactly its
    // serialization time.
    ImpairmentConfig cfg;
    cfg.rate_bps = 1'000'000;

    ImpairmentShaper shaper(cfg);
    const PacketDecision decision = shaper.shape(1200, 0);
    EXPECT_FALSE(decision.dropped);
    EXPECT_EQ(decision.delay_us, 9600);
}

TEST(NetSimImpairment, TheRateLimitQueuesTheSecondPacket) {
    // The second packet cannot go out before the first one is on the
    // wire, so it waits for the link to become free.
    ImpairmentConfig cfg;
    cfg.rate_bps = 1'000'000;

    ImpairmentShaper shaper(cfg);
    const PacketDecision first  = shaper.shape(1200, 0);
    const PacketDecision second = shaper.shape(1200, 0);
    EXPECT_EQ(first.delay_us, 9600);
    EXPECT_EQ(second.delay_us, 19200);
}

TEST(NetSimImpairment, TheRateLimitsQueueIsBounded) {
    // An unbounded queue turns a congestion collapse into an unbounded
    // latency; the profile drops instead.
    ImpairmentConfig cfg;
    cfg.rate_bps         = 100'000;      // 100 kbps
    cfg.max_queue_delay_ms = 50.0;

    ImpairmentShaper shaper(cfg);

    std::uint64_t dropped = 0;
    for (int i = 0; i < 100; ++i) {
        if (shaper.shape(1200, static_cast<std::int64_t>(i) * 1000).dropped) ++dropped;
    }
    EXPECT_GT(dropped, 0u) << "the queue bound must drop something";
    EXPECT_EQ(shaper.packets_dropped_queue_full(), dropped);
    EXPECT_EQ(shaper.packets_dropped(), dropped);
}

TEST(NetSimImpairment, TheRateLimitDoesNotCarryTheDelayOverAnIdlePeriod) {
    // The rate limiter is a *link* property, not a per-packet tax: a
    // packet that arrives long after the link went idle is delayed by
    // its own serialization time only -- the previous packet's
    // serialization is long finished.
    ImpairmentConfig cfg;
    cfg.rate_bps = 1'000'000;

    ImpairmentShaper shaper(cfg);
    const PacketDecision first  = shaper.shape(1200, 0);
    const PacketDecision second = shaper.shape(1200, 10'000'000);
    EXPECT_EQ(first.delay_us, 9600);
    EXPECT_EQ(second.delay_us, 9600);
}

} // anonymous namespace
