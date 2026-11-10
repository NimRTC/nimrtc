/**
 * @file src/modules/pacing/tests/test_pacing.cpp
 * @brief Unit tests for the token-bucket pacer (v1.2.0 M3, video plan P1).
 *
 * ## Test scenarios
 *
 * | # | Scenario                          | Key invariant verified                       |
 * |---|-----------------------------------|---------------------------------------------|
 * | 1 | UnlimitedBucket                   | rate 0 lets every byte through             |
 * | 2 | RateLimitsOverOneSecond           | 1 s of simulated time releases ~rate/8 bytes |
 * | 3 | BurstCeiling                      | a long idle grants at most one bucket      |
 * | 4 | RefusalTakesNothing               | a short bucket refuses and consumes nothing|
 * | 5 | ExactSpend                        | spending the credit exactly leaves zero    |
 * | 6 | RateChangeKeepsCredit             | a rate change keeps (and clamps) the credit|
 * | 7 | ClockNeverRunsBackwards           | an older time point grants no credit       |
 * | 8 | TelemetryCounters                 | sent / blocked counters are truthful       |
 * | 9 | HugeRequestIsRefused              | an absurd request is refused, not overflow |
 * |10 | Determinism                       | the same trace gives the same result       |
 * |11 | DefaultBurstIsAHundredMillis      | the default burst is 100 ms of the rate    |
 *
 * The clock is synthetic: every case advances a `core::TimePoint` by an
 * exact number of milliseconds, so the assertions are byte-exact and no
 * test sleeps.
 *
 * Stability classification: NIMRTC_API_STABLE (the module is new, but the
 * behaviour it guarantees is part of the send-path contract).
 */

// gtest first: ensures no PCH surprises from our headers.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>

#include <nimrtc/pacing/pacing.hpp>

using nimrtc::core::TimePoint;
using nimrtc::pacing::PacerConfig;
using nimrtc::pacing::TokenBucket;

namespace {

/** A synthetic time point `ms` milliseconds after the epoch. */
TimePoint at_ms(std::int64_t ms) {
    return TimePoint{} + std::chrono::milliseconds(ms);
}

}  // namespace

// ---------------------------------------------------------------------------
// 1 — an unlimited bucket
// ---------------------------------------------------------------------------

TEST(TokenBucket, UnlimitedBucketLetsEverythingThrough) {
    TokenBucket bucket;  // rate_bps == 0 by default

    EXPECT_EQ(bucket.rate_bps(), 0u);
    EXPECT_EQ(bucket.burst_bytes(), 0u);

    const auto now = at_ms(0);
    EXPECT_TRUE(bucket.try_consume(1'000'000, now));
    EXPECT_TRUE(bucket.try_consume(1'000'000, at_ms(1000)));
    EXPECT_EQ(bucket.bytes_sent(), 2'000'000u);
    EXPECT_EQ(bucket.bytes_blocked(), 0u);
}

// ---------------------------------------------------------------------------
// 2 — the rate is the limit
// ---------------------------------------------------------------------------

/** Over one second of simulated time the bucket must release the rate's
 *  worth of bytes — plus at most one burst of slack, because the bucket
 *  starts full.  This is the acceptance criterion of the plan item P1. */
TEST(TokenBucket, RateLimitsOverOneSecond) {
    // 8000 bps = 1000 bytes/s.  The default burst is 100 ms = 100 bytes.
    TokenBucket bucket{PacerConfig{.rate_bps = 8000}};

    std::uint64_t released = 0;
    // Ten 100 ms steps: each step may release 100 bytes of new credit.
    for (int step = 1; step <= 10; ++step) {
        const auto now = at_ms(step * 100);
        while (bucket.try_consume(100, now)) {
            released += 100;
        }
    }

    // 1000 bytes of rate + at most one burst (100 bytes) of slack.
    EXPECT_LE(released, 1100u);
    EXPECT_GE(released, 1000u);
}

/** The same rate, but the packets are small: the total must not depend
 *  on the packet size. */
TEST(TokenBucket, PacketSizeDoesNotChangeTheReleasedTotal) {
    TokenBucket bucket{PacerConfig{.rate_bps = 8000}};

    std::uint64_t released = 0;
    for (int step = 1; step <= 10; ++step) {
        const auto now = at_ms(step * 100);
        while (bucket.try_consume(10, now)) {
            released += 10;
        }
    }

    EXPECT_LE(released, 1100u);
    EXPECT_GE(released, 1000u);
}

// ---------------------------------------------------------------------------
// 3 — the burst ceiling
// ---------------------------------------------------------------------------

/** After a long idle the bucket holds at most one burst: the idle time
 *  does not buy an unbounded burst. */
TEST(TokenBucket, BurstCeilingAfterLongIdle) {
    TokenBucket bucket{PacerConfig{.rate_bps = 8000}};  // 1000 B/s, 100 B burst

    // One hour of idleness.
    const auto now = at_ms(3'600'000);
    EXPECT_LE(bucket.available(now), 100u);
    EXPECT_EQ(bucket.available(now), 100u);
}

/** An explicit burst is honoured verbatim. */
TEST(TokenBucket, ExplicitBurstIsHonoured) {
    TokenBucket bucket{PacerConfig{.rate_bps = 8000, .burst_bytes = 250}};
    EXPECT_EQ(bucket.burst_bytes(), 250u);
    EXPECT_EQ(bucket.available(at_ms(3'600'000)), 250u);
}

// ---------------------------------------------------------------------------
// 4 — a refusal takes nothing
// ---------------------------------------------------------------------------

TEST(TokenBucket, RefusalTakesNothing) {
    TokenBucket bucket{PacerConfig{.rate_bps = 8000, .burst_bytes = 100}};

    const auto now = at_ms(0);
    // The bucket holds 100 bytes; asking for 101 must fail and must not
    // take the 100 bytes it does hold.
    EXPECT_FALSE(bucket.try_consume(101, now));
    EXPECT_EQ(bucket.available(now), 100u);
    EXPECT_EQ(bucket.bytes_sent(), 0u);
    EXPECT_EQ(bucket.bytes_blocked(), 1u);
}

// ---------------------------------------------------------------------------
// 5 — spending the credit exactly
// ---------------------------------------------------------------------------

TEST(TokenBucket, ExactSpendLeavesNothing) {
    TokenBucket bucket{PacerConfig{.rate_bps = 8000, .burst_bytes = 100}};

    const auto now = at_ms(0);
    EXPECT_TRUE(bucket.try_consume(100, now));
    EXPECT_EQ(bucket.available(now), 0u);
    EXPECT_FALSE(bucket.try_consume(1, now));
}

// ---------------------------------------------------------------------------
// 6 — a rate change
// ---------------------------------------------------------------------------

/** Lowering the rate keeps the credit that was already earned, but the
 *  credit may not exceed the new burst ceiling. */
TEST(TokenBucket, RateChangeKeepsCredit) {
    TokenBucket bucket{PacerConfig{.rate_bps = 8000, .burst_bytes = 100}};

    const auto now = at_ms(0);
    EXPECT_EQ(bucket.available(now), 100u);

    // Same burst, lower rate: the credit survives.
    bucket.set_rate_bps(4000);
    EXPECT_EQ(bucket.rate_bps(), 4000u);
    EXPECT_EQ(bucket.available(now), 100u);

    // A smaller burst clamps the credit.
    bucket.set_config(PacerConfig{.rate_bps = 4000, .burst_bytes = 10});
    EXPECT_EQ(bucket.available(now), 10u);
}

// ---------------------------------------------------------------------------
// 7 — the clock never runs backwards
// ---------------------------------------------------------------------------

TEST(TokenBucket, ClockNeverRunsBackwards) {
    TokenBucket bucket{PacerConfig{.rate_bps = 8000, .burst_bytes = 100}};

    const auto now = at_ms(1000);
    EXPECT_EQ(bucket.available(now), 100u);  // starts full

    // A time point that is older than the last one grants nothing.
    EXPECT_EQ(bucket.available(at_ms(500)), 100u);
    // ... and the clock is still at 1000 ms, so the next forward step
    // grants only the time since 1000 ms.
    EXPECT_EQ(bucket.available(at_ms(1100)), 100u);  // capped by the burst
}

// ---------------------------------------------------------------------------
// 8 — telemetry
// ---------------------------------------------------------------------------

TEST(TokenBucket, TelemetryCounters) {
    TokenBucket bucket{PacerConfig{.rate_bps = 8000, .burst_bytes = 100}};

    const auto now = at_ms(0);
    EXPECT_TRUE(bucket.try_consume(50, now));
    EXPECT_FALSE(bucket.try_consume(500, now));

    EXPECT_EQ(bucket.bytes_sent(), 50u);
    EXPECT_EQ(bucket.bytes_blocked(), 1u);

    bucket.reset(now);
    EXPECT_EQ(bucket.bytes_sent(), 0u);
    EXPECT_EQ(bucket.bytes_blocked(), 0u);
    EXPECT_EQ(bucket.available(now), 0u);
}

// ---------------------------------------------------------------------------
// 9 — a huge request
// ---------------------------------------------------------------------------

TEST(TokenBucket, HugeRequestIsRefused) {
    TokenBucket bucket{PacerConfig{.rate_bps = 8000, .burst_bytes = 100}};

    const auto now = at_ms(0);
    EXPECT_FALSE(bucket.try_consume(std::numeric_limits<std::uint64_t>::max(), now));
    EXPECT_EQ(bucket.bytes_sent(), 0u);
    EXPECT_EQ(bucket.bytes_blocked(), 1u);
}

// ---------------------------------------------------------------------------
// 10 — determinism
// ---------------------------------------------------------------------------

/** The same trace must produce the same result on every run and on every
 *  machine: the bucket never reads a clock. */
TEST(TokenBucket, Deterministic) {
    auto run = []() -> std::uint64_t {
        TokenBucket bucket{PacerConfig{.rate_bps = 1'000'000}};
        std::uint64_t released = 0;
        for (int step = 1; step <= 50; ++step) {
            const auto now = at_ms(step * 20);
            // 125 bytes divides the 2500 bytes a 20 ms step is worth, so
            // no credit is lost to the packet boundary.
            while (bucket.try_consume(125, now)) {
                released += 125;
            }
        }
        return released;
    };

    const std::uint64_t first = run();
    EXPECT_EQ(first, run());
    // 1 Mbit/s = 125000 B/s; 1 s of simulated time is 125000 bytes, plus
    // the one burst the bucket starts with (100 ms = 12500 bytes).
    EXPECT_LE(first, 125000u + 12500u);
    EXPECT_GE(first, 125000u);
}

// ---------------------------------------------------------------------------
// 11 — the default burst
// ---------------------------------------------------------------------------

TEST(TokenBucket, DefaultBurstIsAHundredMillis) {
    // 8 Mbit/s = 1 MB/s; 100 ms of that is 100000 bytes.
    TokenBucket bucket{PacerConfig{.rate_bps = 8'000'000}};
    EXPECT_EQ(bucket.burst_bytes(), 100'000u);
}

TEST(TokenBucket, TinyRateStillHoldsOneByte) {
    // 8 bps = 1 byte/s; 100 ms of that rounds down to zero, but a bucket
    // that holds nothing would stall the stream.
    TokenBucket bucket{PacerConfig{.rate_bps = 8}};
    EXPECT_EQ(bucket.burst_bytes(), 1u);
}
