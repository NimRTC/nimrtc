/**
 * @file src/modules/sched/tests/test_pacing_integration.cpp
 * @brief The per-class pacing integration (v1.2.0 M3, video plan P2).
 *
 * ## What is tested here
 *
 * The token bucket itself is tested in `modules/pacing/tests`.  This file
 * covers the *integration*: the scheduler owns one bucket per traffic
 * class, the buckets are independent of each other, and the control
 * plane bypasses the pacer entirely.
 *
 * | # | Scenario                            | Key invariant verified                |
 * |---|-------------------------------------|---------------------------------------|
 * | 1 | VideoClassIsPaced                   | a saturated video class holds its packets back |
 * | 2 | ControlBypassesThePacer            | a control packet leaves even when the video class is empty |
 * | 3 | AudioIsNotStarvedByVideo            | the audio class keeps its own budget   |
 * | 4 | UnpacedByDefault                    | a class with rate 0 is not paced      |
 * | 5 | PacingRateRoundTrip                 | set_pacing_rate / pacing_rate agree   |
 * | 6 | RateChangeKeepsTheCredit            | re-applying the same rate keeps the bucket alive |
 * | 7 | BlockedPacketsAreCounted            | the pacing counter reports the stalls  |
 *
 * The scheduler paces against the **wall clock**, so the assertions are
 * written to be robust against a slow machine: they check that the
 * number of packets that left is *bounded* (and that the control plane
 * is not), never that it is exactly a number that depends on timing.
 *
 * Stability classification: NIMRTC_API_STABLE (regression test for the
 * send-path contract).
 */

// gtest first: ensures no PCH surprises from our headers.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <nimrtc/sched/sched.hpp>

using nimrtc::plugins::PacedClass;
using nimrtc::plugins::Priority;
using nimrtc::sched::Scheduler;
using nimrtc::sched::SchedulerConfig;

namespace {

constexpr std::size_t kPayloadSize = 100;

/** A payload big enough to be visible in the byte counters. */
const std::vector<std::uint8_t> kPayload(kPayloadSize, 0x5A);

/** A destination address (the scheduler never sends it anywhere itself).
 *
 *  `plugins::Addr` is opaque — the transport owns the wire format — so the
 *  test address is just a one-byte identifier. */
nimrtc::plugins::Addr make_addr(std::uint8_t id) {
    nimrtc::plugins::Addr a{};
    a.data[0] = id;
    a.len = 1;
    return a;
}

/** A scheduler whose packets are 100 bytes, so a 100 ms burst at
 *  8000 bps holds exactly one packet. */
SchedulerConfig small_packet_config() {
    SchedulerConfig cfg;
    cfg.avg_packet_size_bytes = static_cast<std::uint32_t>(kPayloadSize);
    cfg.max_queue_depth = 8192;
    return cfg;
}

/** Drain everything and report how many packets of each priority left. */
std::vector<std::uint32_t> drain_all(Scheduler& sch, int max_packets = 200) {
    std::vector<std::uint32_t> counts(nimrtc::plugins::kPriorityCount, 0);
    sch.drain_with(max_packets, [&counts](Priority p, nimrtc::core::ByteSpan) {
        ++counts[nimrtc::plugins::priority_rank(p)];
        return true;
    });
    return counts;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1 — a class with a rate is paced
// ---------------------------------------------------------------------------

/** A video class with a 1000 byte/s budget cannot release more than a
 *  burst's worth of packets, however many are queued. */
TEST(SchedulerPacing, VideoClassIsPaced) {
    Scheduler sch{small_packet_config()};

    // 8000 bps = 1000 bytes/s; the default burst is 100 ms = 100 bytes,
    // which is exactly one packet.
    sch.set_pacing_rate(PacedClass::kVideo, 8000);

    for (int i = 0; i < 50; ++i) {
        sch.enqueue(Priority::kVideo, nimrtc::core::ByteSpan{kPayload},
                    make_addr(1));
    }

    const auto counts = drain_all(sch);
    const std::uint32_t video = counts[nimrtc::plugins::priority_rank(Priority::kVideo)];

    // Far fewer than the 50 queued packets may leave: the class is
    // paced.  The bound is loose on purpose -- a slow machine may let a
    // few more through, but never more than a handful.
    EXPECT_LT(video, 10u);
    EXPECT_GT(video, 0u);
    EXPECT_EQ(sch.stats().queued_packets, 50u - video);
}

// ---------------------------------------------------------------------------
// 2 — the control plane bypasses the pacer
// ---------------------------------------------------------------------------

/** A saturated video class must not delay the control plane: RTX, NACK
 *  and RTCP feedback have to get out. */
TEST(SchedulerPacing, ControlBypassesThePacer) {
    Scheduler sch{small_packet_config()};

    sch.set_pacing_rate(PacedClass::kVideo, 8000);

    // The video queue is saturated first.
    for (int i = 0; i < 50; ++i) {
        sch.enqueue(Priority::kVideo, nimrtc::core::ByteSpan{kPayload},
                    make_addr(1));
    }
    // ... and the control packet arrives afterwards.  It must not wait
    // for the video class to earn credit.
    sch.enqueue(Priority::kControl, nimrtc::core::ByteSpan{kPayload},
                make_addr(0));

    const auto counts = drain_all(sch);

    EXPECT_EQ(counts[nimrtc::plugins::priority_rank(Priority::kControl)], 1u)
        << "the control plane must not be delayed by a saturated video class";
}

/** The control plane also bypasses the *session* budget: the aggregate
 *  guard exists to shape the media traffic, not to throttle feedback. */
TEST(SchedulerPacing, ControlBypassesTheSessionBudget) {
    Scheduler sch{small_packet_config()};

    // A tiny session budget: 8000 bps = 1000 bytes/s, and the first
    // drain seeds the credit with one drain interval (125 ms) = 125 bytes.
    sch.on_bwe_update(8000);

    for (int i = 0; i < 10; ++i) {
        sch.enqueue(Priority::kVideo, nimrtc::core::ByteSpan{kPayload},
                    make_addr(1));
    }
    sch.enqueue(Priority::kControl, nimrtc::core::ByteSpan{kPayload},
                make_addr(0));

    const auto counts = drain_all(sch);
    EXPECT_EQ(counts[nimrtc::plugins::priority_rank(Priority::kControl)], 1u);
}

// ---------------------------------------------------------------------------
// 3 — the classes are independent
// ---------------------------------------------------------------------------

/** A saturated video class must not eat the audio's budget: each class
 *  has its own bucket. */
TEST(SchedulerPacing, AudioIsNotStarvedByVideo) {
    Scheduler sch{small_packet_config()};

    sch.set_pacing_rate(PacedClass::kVideo, 8000);

    for (int i = 0; i < 50; ++i) {
        sch.enqueue(Priority::kVideo, nimrtc::core::ByteSpan{kPayload},
                    make_addr(1));
    }
    for (int i = 0; i < 10; ++i) {
        sch.enqueue(Priority::kAudio, nimrtc::core::ByteSpan{kPayload},
                    make_addr(2));
    }

    const auto counts = drain_all(sch);

    // The audio class is not paced (its rate is 0), so all ten audio
    // packets leave even though the video class is out of credit.
    EXPECT_EQ(counts[nimrtc::plugins::priority_rank(Priority::kAudio)], 10u)
        << "the audio class must not be starved by the video class";
}

// ---------------------------------------------------------------------------
// 4 — the default is "not paced"
// ---------------------------------------------------------------------------

/** A scheduler nobody configured paces nothing: the behaviour of the
 *  existing implementations is unchanged. */
TEST(SchedulerPacing, UnpacedByDefault) {
    Scheduler sch{small_packet_config()};

    EXPECT_EQ(sch.pacing_rate(PacedClass::kAudio), 0u);
    EXPECT_EQ(sch.pacing_rate(PacedClass::kVideo), 0u);
    EXPECT_EQ(sch.pacing_rate(PacedClass::kData), 0u);

    for (int i = 0; i < 50; ++i) {
        sch.enqueue(Priority::kVideo, nimrtc::core::ByteSpan{kPayload},
                    make_addr(1));
    }

    const auto counts = drain_all(sch);
    EXPECT_EQ(counts[nimrtc::plugins::priority_rank(Priority::kVideo)], 50u);
    EXPECT_EQ(sch.stats().paced_blocked_packets, 0u);
}

// ---------------------------------------------------------------------------
// 5 — the rate round-trips
// ---------------------------------------------------------------------------

TEST(SchedulerPacing, PacingRateRoundTrip) {
    Scheduler sch{small_packet_config()};

    sch.set_pacing_rate(PacedClass::kAudio, 64'000);
    sch.set_pacing_rate(PacedClass::kVideo, 900'000);
    sch.set_pacing_rate(PacedClass::kData, 32'000);

    EXPECT_EQ(sch.pacing_rate(PacedClass::kAudio), 64'000u);
    EXPECT_EQ(sch.pacing_rate(PacedClass::kVideo), 900'000u);
    EXPECT_EQ(sch.pacing_rate(PacedClass::kData), 32'000u);

    // 0 turns the pacing off again.
    sch.set_pacing_rate(PacedClass::kVideo, 0);
    EXPECT_EQ(sch.pacing_rate(PacedClass::kVideo), 0u);
}

// ---------------------------------------------------------------------------
// 6 — re-applying the same rate keeps the bucket alive
// ---------------------------------------------------------------------------

/** The engine pushes the pacing rates on every tick.  Re-applying the
 *  *same* rate must not reset the bucket, or the credit would be thrown
 *  away 30 times a second and nothing would ever be sent. */
TEST(SchedulerPacing, RateChangeKeepsTheCredit) {
    Scheduler sch{small_packet_config()};

    sch.set_pacing_rate(PacedClass::kVideo, 8000);

    for (int i = 0; i < 5; ++i) {
        sch.enqueue(Priority::kVideo, nimrtc::core::ByteSpan{kPayload},
                    make_addr(1));
    }

    // The first drain spends the burst.
    drain_all(sch);

    // The engine pushes the same rate again -- 30 times a second in
    // production.
    for (int i = 0; i < 30; ++i) {
        sch.set_pacing_rate(PacedClass::kVideo, 8000);
    }

    // The bucket still works: it hands out its next credit as the clock
    // advances.  The packet that was held back leaves eventually.
    int sent = 0;
    for (int i = 0; i < 40 && sent == 0; ++i) {
        sent += sch.drain_with(10, [](Priority, nimrtc::core::ByteSpan) {
            return true;
        });
        if (sent == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    EXPECT_GT(sent, 0)
        << "re-applying the same pacing rate must not throw the credit away";
}

// ---------------------------------------------------------------------------
// 7 — the stalls are counted
// ---------------------------------------------------------------------------

TEST(SchedulerPacing, BlockedPacketsAreCounted) {
    Scheduler sch{small_packet_config()};

    sch.set_pacing_rate(PacedClass::kVideo, 8000);

    for (int i = 0; i < 20; ++i) {
        sch.enqueue(Priority::kVideo, nimrtc::core::ByteSpan{kPayload},
                    make_addr(1));
    }

    drain_all(sch);

    EXPECT_GT(sch.stats().paced_blocked_packets, 0u)
        << "a stalled class must be visible in the statistics";
}
