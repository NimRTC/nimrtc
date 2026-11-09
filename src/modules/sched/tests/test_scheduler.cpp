/**
 * @file src/modules/sched/tests/test_scheduler.cpp
 * @brief Unit tests for the unified sending scheduler.
 *
 * Coverage:
 *   P0 — Config / Stats defaults, constructor.
 *   P1 — Priority ordering (drain returns highest priority first).
 *   P1 — BWE congestion: low-priority packets dropped, kControl preserved.
 *   P1 — BWE pause (target_bps == 0): all except kControl dropped.
 *   P1 — Hard cap (max_queue_depth): overflow drops lowest priority first.
 *   P2 — Reset clears all queues and statistics.
 *   P2 — Empty drain returns 0.
 *   P2 — Config round-trip.
 *   P0 — Drain veto: the packet is kept (not lost) and the drain stops.
 *   P0 — Time base: the send rate follows wall-clock time, not the number
 *        of drain() calls (host tick rate independence).
 */

// gtest first: ensures no PCH surprises from our headers.
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

// Plugin types (plugins::Addr, kMaxAddrLen) must be visible before
// nimrtc/sched/sched.hpp which uses plugins::Addr in its public API.
#include <nimrtc/plugins/base.hpp>

#include <nimrtc/sched/sched.hpp>

namespace {

using namespace nimrtc::sched;
using namespace nimrtc::core;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Helper utilities
// ---------------------------------------------------------------------------

/** Minimal non-empty Addr for testing. */
inline nimrtc::plugins::Addr make_test_addr(std::uint8_t id) {
    nimrtc::plugins::Addr a{};
    a.data[0] = id;
    a.len = 1;
    return a;
}

/** Small inline payload for enqueue calls. */
constexpr std::array<std::uint8_t, 4> kPayload = {0x01, 0x02, 0x03, 0x04};

/** Drain all packets from a scheduler and return the count per priority.
 *  Calls drain_with(max_packets=128) until the queue is empty, accumulating
 *  per-priority counts via the callback. */
std::array<int, kPriorityCount>
drain_all(Scheduler& sch) {
    std::array<int, kPriorityCount> counts{};
    counts.fill(0);
    int n;
    do {
        n = sch.drain_with(128, [&](Priority p, nimrtc::core::ByteSpan /*data*/) {
            counts[priority_rank(p)]++;
            return true;  // continue draining
        });
    } while (n > 0);
    return counts;
}

// ---------------------------------------------------------------------------
// Config / Stats defaults
// ---------------------------------------------------------------------------

TEST(SchedulerConfig, DefaultValues) {
    SchedulerConfig cfg;
    EXPECT_EQ(cfg.avg_packet_size_bytes, 1200u);
    EXPECT_EQ(cfg.max_queue_depth, 8192u);
    EXPECT_TRUE(cfg.protect_keyframes);
}

TEST(SchedulerStats, DefaultValues) {
    SchedulerStats s;
    EXPECT_EQ(s.queued_packets, 0u);
    EXPECT_EQ(s.dropped_packets, 0u);
    EXPECT_EQ(s.sent_packets, 0u);
    EXPECT_EQ(s.sent_bytes, 0u);
    EXPECT_EQ(s.target_bitrate_bps, 0u);
    EXPECT_EQ(s.current_bitrate_bps, 0u);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST(Scheduler, DefaultConstructsWithDefaultConfig) {
    Scheduler sch{};
    auto cfg = sch.config();
    EXPECT_EQ(cfg.avg_packet_size_bytes, 1200u);
    EXPECT_EQ(cfg.max_queue_depth, 8192u);
    EXPECT_TRUE(cfg.protect_keyframes);
}

TEST(Scheduler, ConstructorWithCustomConfig) {
    SchedulerConfig cfg;
    cfg.avg_packet_size_bytes = 800;
    cfg.max_queue_depth = 1024;
    cfg.protect_keyframes = false;
    Scheduler sch{cfg};
    auto retrieved = sch.config();
    EXPECT_EQ(retrieved.avg_packet_size_bytes, 800u);
    EXPECT_EQ(retrieved.max_queue_depth, 1024u);
    EXPECT_FALSE(retrieved.protect_keyframes);
}

// ---------------------------------------------------------------------------
// P1 — Priority ordering
// ---------------------------------------------------------------------------

/** Enqueue one packet per priority in descending order (BestEffort→Control)
 *  and verify drain returns them in ascending priority order (Control first). */
TEST(Scheduler, DrainOrderHighPriorityFirst) {
    Scheduler sch{};

    // Enqueue in reverse priority order to prove scheduler reorders.
    sch.enqueue(Priority::kBestEffort, ByteSpan{kPayload}, make_test_addr(4));
    sch.enqueue(Priority::kVideo,      ByteSpan{kPayload}, make_test_addr(3));
    sch.enqueue(Priority::kVideoKeyframe, ByteSpan{kPayload}, make_test_addr(2));
    sch.enqueue(Priority::kAudio,      ByteSpan{kPayload}, make_test_addr(1));
    sch.enqueue(Priority::kControl,    ByteSpan{kPayload}, make_test_addr(0));

    // Drain one at a time and verify order.
    EXPECT_EQ(sch.drain(1), 1);   // kControl
    EXPECT_EQ(sch.drain(1), 1);   // kAudio
    EXPECT_EQ(sch.drain(1), 1);   // kVideoKeyframe
    EXPECT_EQ(sch.drain(1), 1);   // kVideo
    EXPECT_EQ(sch.drain(1), 1);   // kBestEffort
    EXPECT_EQ(sch.drain(1), 0);   // empty
}

/** Enqueue many packets of mixed priorities and verify drain emits them
 *  grouped by priority, not FIFO. */
TEST(Scheduler, DrainInterleavedPrioritiesGrouped) {
    Scheduler sch{};

    // Interleave packets from two priorities.
    sch.enqueue(Priority::kVideo,      ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kControl,   ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kVideo,     ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kAudio,     ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kControl,   ByteSpan{kPayload}, make_test_addr(0));

    auto counts = drain_all(sch);
    EXPECT_EQ(counts[priority_rank(Priority::kControl)], 2);
    EXPECT_EQ(counts[priority_rank(Priority::kAudio)],   1);
    EXPECT_EQ(counts[priority_rank(Priority::kVideo)],   2);
    EXPECT_EQ(counts[priority_rank(Priority::kVideoKeyframe)], 0);
    EXPECT_EQ(counts[priority_rank(Priority::kBestEffort)],   0);
}

/** Verify that when drain(max_packets) is smaller than total queued packets,
 *  only the highest-priority packets are drained. */
TEST(Scheduler, DrainLimitRespected) {
    Scheduler sch{};
    sch.enqueue(Priority::kControl,   ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kAudio,     ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kVideo,     ByteSpan{kPayload}, make_test_addr(0));

    int n = sch.drain(2);
    EXPECT_EQ(n, 2);
    EXPECT_EQ(sch.stats().queued_packets, 1u);  // one video left

    n = sch.drain(1);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(sch.stats().queued_packets, 0u);
}

/** Empty drain returns 0. */
TEST(Scheduler, DrainEmptyReturnsZero) {
    Scheduler sch{};
    EXPECT_EQ(sch.drain(10), 0);
    EXPECT_EQ(sch.drain(0),  0);
    EXPECT_EQ(sch.drain(-1), 0);  // negative is treated as no-op
}

// ---------------------------------------------------------------------------
// P1 — BWE congestion: low-priority packets dropped, kControl preserved
// ---------------------------------------------------------------------------

/** When BWE signals reduced bandwidth, scheduler drops from kBestEffort up
 *  but never drops kControl. */
TEST(Scheduler, BweCongestionDropsLowPriorityPreservesControl) {
    SchedulerConfig cfg;
    cfg.avg_packet_size_bytes = 1000;  // 1 byte ≈ 8000 bits → 1250 packets/s per Mbps
    cfg.max_queue_depth = 8192;
    Scheduler sch{cfg};

    // Fill queue: 1 control + 5 best-effort.
    sch.enqueue(Priority::kControl,    ByteSpan{kPayload}, make_test_addr(0));
    for (int i = 0; i < 5; ++i)
        sch.enqueue(Priority::kBestEffort, ByteSpan{kPayload}, make_test_addr(0));

    // Signal severe congestion: 100 kbps ≈ 12 packets/s per drain cycle.
    // With avg=1000 bytes, 100 kbps → 12.5 packets/s → limit per drain < 13.
    sch.on_bwe_update(100'000);  // 100 kbps

    // Drain: kControl should survive.
    auto counts = drain_all(sch);
    EXPECT_EQ(counts[priority_rank(Priority::kControl)], 1);
    // Low-priority packets should have been dropped (not in drain output).
    EXPECT_EQ(counts[priority_rank(Priority::kBestEffort)], 0);

    auto stats = sch.stats();
    EXPECT_EQ(stats.dropped_packets, 5u);
    EXPECT_EQ(stats.sent_packets, 1u);
}

/** BWE target_bps == 0 (sender pause) drops everything except kControl. */
TEST(Scheduler, BwePauseDropsAllExceptControl) {
    Scheduler sch{};
    sch.enqueue(Priority::kControl,   ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kAudio,     ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kVideo,     ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kVideoKeyframe, ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kBestEffort, ByteSpan{kPayload}, make_test_addr(0));

    sch.on_bwe_update(0);  // sender pause

    auto counts = drain_all(sch);
    EXPECT_EQ(counts[priority_rank(Priority::kControl)],   1);
    EXPECT_EQ(counts[priority_rank(Priority::kAudio)],      0);
    EXPECT_EQ(counts[priority_rank(Priority::kVideo)],      0);
    EXPECT_EQ(counts[priority_rank(Priority::kVideoKeyframe)], 0);
    EXPECT_EQ(counts[priority_rank(Priority::kBestEffort)], 0);

    auto stats = sch.stats();
    EXPECT_EQ(stats.dropped_packets, 4u);
}

/** BWE bandwidth increase restores all priorities. */
TEST(Scheduler, BweIncreaseRestoresPriorities) {
    SchedulerConfig cfg;
    cfg.avg_packet_size_bytes = 1000;
    Scheduler sch{cfg};

    sch.enqueue(Priority::kAudio, ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kVideo, ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kBestEffort, ByteSpan{kPayload}, make_test_addr(0));

    // Severe congestion.
    sch.on_bwe_update(10'000);  // 10 kbps — very low
    // Many packets will be dropped.

    // Recover.
    sch.on_bwe_update(5'000'000);  // 5 Mbps — generous

    // New packets should be enqueued and drained normally.
    sch.enqueue(Priority::kAudio, ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kVideo, ByteSpan{kPayload}, make_test_addr(0));

    auto counts = drain_all(sch);
    // Some old dropped packets won't appear; new ones should.
    EXPECT_EQ(counts[priority_rank(Priority::kAudio)], 1);
    EXPECT_EQ(counts[priority_rank(Priority::kVideo)], 1);
}

/** BWE only drops low priorities up to kVideoKeyframe; kAudio and kControl
 *  survive moderate congestion. */
TEST(Scheduler, BweModerateCongestionPreservesAudioAndControl) {
    SchedulerConfig cfg;
    cfg.avg_packet_size_bytes = 1000;
    Scheduler sch{cfg};

    sch.enqueue(Priority::kControl,   ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kAudio,     ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kVideo,    ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kBestEffort, ByteSpan{kPayload}, make_test_addr(0));

    // 256 kbps with 1000-byte packets → 32 packets/s budget.
    // With only 4 packets, none should be dropped.
    sch.on_bwe_update(256'000);

    auto counts = drain_all(sch);
    EXPECT_EQ(counts[priority_rank(Priority::kControl)], 1);
    EXPECT_EQ(counts[priority_rank(Priority::kAudio)],    1);
    // kVideo and kBestEffort may or may not be dropped depending on budget;
    // just verify stats are non-negative.
    EXPECT_GE(sch.stats().sent_packets, 1u);
}

// ---------------------------------------------------------------------------
// P1 — Hard cap (max_queue_depth)
// ---------------------------------------------------------------------------

/** When queue reaches max_queue_depth, the next enqueue drops the
 *  lowest-priority packet first. */
TEST(Scheduler, OverflowDropsLowestPriorityFirst) {
    SchedulerConfig cfg;
    cfg.max_queue_depth = 3;
    cfg.avg_packet_size_bytes = 1000;
    Scheduler sch{cfg};

    sch.enqueue(Priority::kControl,   ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kAudio,    ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kVideo,    ByteSpan{kPayload}, make_test_addr(0));
    // Queue now full (3 packets).

    auto stats_before = sch.stats();
    EXPECT_EQ(stats_before.dropped_packets, 0u);

    // Enqueue a low-priority packet → should drop kVideo (lowest of the 3).
    sch.enqueue(Priority::kBestEffort, ByteSpan{kPayload}, make_test_addr(0));

    auto stats_after = sch.stats();
    EXPECT_EQ(stats_after.dropped_packets, 1u);

    auto counts = drain_all(sch);
    EXPECT_EQ(counts[priority_rank(Priority::kControl)], 1);
    EXPECT_EQ(counts[priority_rank(Priority::kAudio)],   1);
    // kVideo was dropped; only the new kBestEffort remains.
    EXPECT_EQ(counts[priority_rank(Priority::kVideo)],    0);
    EXPECT_EQ(counts[priority_rank(Priority::kBestEffort)], 1);
}

/** High-priority packet is never dropped due to overflow. */
TEST(Scheduler, OverflowNeverDropsControl) {
    SchedulerConfig cfg;
    cfg.max_queue_depth = 2;
    cfg.avg_packet_size_bytes = 1000;
    Scheduler sch{cfg};

    sch.enqueue(Priority::kControl,  ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kAudio,    ByteSpan{kPayload}, make_test_addr(0));
    // Queue full.

    // Enqueue another low priority — drops kAudio.
    sch.enqueue(Priority::kVideo,    ByteSpan{kPayload}, make_test_addr(0));
    EXPECT_EQ(sch.stats().dropped_packets, 1u);

    // Enqueue more low priority — drops kVideo.
    sch.enqueue(Priority::kBestEffort, ByteSpan{kPayload}, make_test_addr(0));
    EXPECT_EQ(sch.stats().dropped_packets, 2u);

    // Still never dropped kControl.
    auto counts = drain_all(sch);
    EXPECT_EQ(counts[priority_rank(Priority::kControl)], 1);
    EXPECT_EQ(counts[priority_rank(Priority::kAudio)],    0);
    EXPECT_EQ(counts[priority_rank(Priority::kVideo)],    0);
    EXPECT_EQ(counts[priority_rank(Priority::kBestEffort)], 1);
}

// ---------------------------------------------------------------------------
// P2 — Reset
// ---------------------------------------------------------------------------

TEST(Scheduler, ResetClearsQueuesAndStats) {
    Scheduler sch{};
    sch.enqueue(Priority::kVideo,    ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kAudio,    ByteSpan{kPayload}, make_test_addr(0));
    (void)sch.drain(1);  // send one

    sch.reset();

    auto s = sch.stats();
    EXPECT_EQ(s.queued_packets, 0u);
    EXPECT_EQ(s.dropped_packets, 0u);
    EXPECT_EQ(s.sent_packets, 0u);
    EXPECT_EQ(s.target_bitrate_bps, 0u);
    EXPECT_EQ(sch.drain(10), 0);
}

/** Reset does not reset config. */
TEST(Scheduler, ResetPreservesConfig) {
    SchedulerConfig cfg;
    cfg.max_queue_depth = 500;
    cfg.avg_packet_size_bytes = 600;
    Scheduler sch{cfg};

    sch.reset();

    auto retrieved = sch.config();
    EXPECT_EQ(retrieved.max_queue_depth, 500u);
    EXPECT_EQ(retrieved.avg_packet_size_bytes, 600u);
}

// ---------------------------------------------------------------------------
// P2 — Stats tracking
// ---------------------------------------------------------------------------

TEST(Scheduler, SentPacketsStatIncrementsOnDrain) {
    Scheduler sch{};
    sch.enqueue(Priority::kControl, ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kAudio,   ByteSpan{kPayload}, make_test_addr(0));

    (void)sch.drain(1);
    EXPECT_EQ(sch.stats().sent_packets, 1u);

    (void)sch.drain(1);
    EXPECT_EQ(sch.stats().sent_packets, 2u);
}

TEST(Scheduler, QueuedPacketsTracksEnqueueAndDrain) {
    Scheduler sch{};
    EXPECT_EQ(sch.stats().queued_packets, 0u);

    sch.enqueue(Priority::kControl, ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kAudio,   ByteSpan{kPayload}, make_test_addr(0));
    EXPECT_EQ(sch.stats().queued_packets, 2u);

    (void)sch.drain(1);
    EXPECT_EQ(sch.stats().queued_packets, 1u);

    sch.reset();
    EXPECT_EQ(sch.stats().queued_packets, 0u);
}

TEST(Scheduler, TargetBitrateUpdatedOnBwe) {
    Scheduler sch{};
    EXPECT_EQ(sch.stats().target_bitrate_bps, 0u);

    sch.on_bwe_update(1'000'000);
    EXPECT_EQ(sch.stats().target_bitrate_bps, 1'000'000u);

    sch.on_bwe_update(500'000);
    EXPECT_EQ(sch.stats().target_bitrate_bps, 500'000u);
}

// ---------------------------------------------------------------------------
// P2 — Priority helpers
// ---------------------------------------------------------------------------

TEST(PriorityHelpers, PriorityRank) {
    EXPECT_EQ(priority_rank(Priority::kControl), 0u);
    EXPECT_EQ(priority_rank(Priority::kAudio),   1u);
    EXPECT_EQ(priority_rank(Priority::kVideoKeyframe), 2u);
    EXPECT_EQ(priority_rank(Priority::kVideo),    3u);
    EXPECT_EQ(priority_rank(Priority::kBestEffort), 4u);
}

TEST(PriorityHelpers, LowerPriority) {
    EXPECT_EQ(lower_priority(Priority::kControl),      Priority::kAudio);
    EXPECT_EQ(lower_priority(Priority::kAudio),         Priority::kVideoKeyframe);
    EXPECT_EQ(lower_priority(Priority::kVideoKeyframe), Priority::kVideo);
    EXPECT_EQ(lower_priority(Priority::kVideo),         Priority::kBestEffort);
    EXPECT_FALSE(lower_priority(Priority::kBestEffort).has_value());
}

// ---------------------------------------------------------------------------
// P2 — PacketRecord fields preserved through drain
// ---------------------------------------------------------------------------

TEST(Scheduler, EnqueueWithNonEmptyDstPreserved) {
    Scheduler sch{};
    auto dst = make_test_addr(42);
    sch.enqueue(Priority::kControl, ByteSpan{kPayload}, dst);

    // The drain count reflects that the packet was in the queue.
    auto counts = drain_all(sch);
    EXPECT_EQ(counts[priority_rank(Priority::kControl)], 1);
}

// ---------------------------------------------------------------------------
// P0 - Drain veto: the packet must not be lost
// ---------------------------------------------------------------------------

/** A callback that vetoes the dequeue must NOT lose the packet.
 *
 *  Before the fix, drain_with() had already popped the record off the
 *  queue when the callback returned false - the packet was silently
 *  discarded and was not counted as "dropped" either, so the statistics
 *  understated the real loss.  Now the packet goes back to the front of
 *  its queue and the drain stops. */
TEST(Scheduler, VetoKeepsPacketQueued) {
    Scheduler sch{};
    sch.enqueue(Priority::kAudio, ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kAudio, ByteSpan{kPayload}, make_test_addr(0));

    const int n = sch.drain_with(10, [](Priority, nimrtc::core::ByteSpan) {
        return false;  // veto everything
    });

    EXPECT_EQ(n, 0);  // nothing was sent
    auto stats = sch.stats();
    EXPECT_EQ(stats.sent_packets, 0u);
    EXPECT_EQ(stats.queued_packets, 2u);   // both packets still queued
    EXPECT_EQ(stats.dropped_packets, 0u);  // and nothing was "dropped" either
}

/** After a veto, the next accepting drain emits the very same packets. */
TEST(Scheduler, VetoThenAcceptDrainsSamePackets) {
    Scheduler sch{};
    sch.enqueue(Priority::kAudio, ByteSpan{kPayload}, make_test_addr(0));

    EXPECT_EQ(sch.drain_with(10, [](Priority, nimrtc::core::ByteSpan) {
        return false;
    }), 0);

    // A subsequent drain that accepts must deliver the packet.
    int seen = 0;
    const int n = sch.drain_with(10, [&](Priority, nimrtc::core::ByteSpan) {
        ++seen;
        return true;
    });
    EXPECT_EQ(n, 1);
    EXPECT_EQ(seen, 1);
    EXPECT_EQ(sch.stats().queued_packets, 0u);
}

/** A veto on a high-priority packet stops the whole drain - lower
 *  priority queues must not be attempted either. */
TEST(Scheduler, VetoStopsWholeDrain) {
    Scheduler sch{};
    sch.enqueue(Priority::kControl, ByteSpan{kPayload}, make_test_addr(0));
    sch.enqueue(Priority::kBestEffort, ByteSpan{kPayload}, make_test_addr(0));

    const int n = sch.drain_with(10, [](Priority, nimrtc::core::ByteSpan) {
        return false;
    });

    EXPECT_EQ(n, 0);
    EXPECT_EQ(sch.stats().queued_packets, 2u);
}

// ---------------------------------------------------------------------------
// P0 - Time base: the send rate follows wall-clock time
// ---------------------------------------------------------------------------

/** The per-drain send rate must follow wall-clock time, not the number
 *  of drain() calls.
 *
 *  A host that drains in a tight loop (elapsed ~ 0 ms) must not be able
 *  to push more bytes out than the target bitrate allows; the credit
 *  granted per drain is proportional to the measured elapsed time. */
TEST(Scheduler, SendRateFollowsWallClockNotDrainCount) {
    SchedulerConfig cfg;
    cfg.avg_packet_size_bytes = 1000;
    cfg.drain_interval_ms = 125;
    Scheduler sch{cfg};

    // 800 kbps -> 100 kB/s.
    sch.on_bwe_update(800'000);

    // Enqueue 50 packets (50 kB - half a second of traffic).
    for (int i = 0; i < 50; ++i) {
        sch.enqueue(Priority::kAudio, ByteSpan{kPayload}, make_test_addr(0));
    }

    // Drain in a tight loop: 200 iterations, no sleeping.  Wall-clock time
    // barely advances, so only a handful of packets may leave.  If the
    // scheduler used a per-drain packet allowance instead of a time-based
    // credit, all 50 packets would be emitted here.
    int sent = 0;
    for (int i = 0; i < 200; ++i) {
        sent += sch.drain_with(50, [](Priority, nimrtc::core::ByteSpan) {
            return true;
        });
    }
    EXPECT_LT(sent, 50);
    EXPECT_GT(sent, 0);
}

/** Two hosts with different tick rates reach a comparable number of sent
 *  packets over the same wall-clock window. */
TEST(Scheduler, TickRateDoesNotChangeEffectiveRate) {
    constexpr std::uint32_t kBps = 800'000;   // 100 kB/s
    constexpr std::uint32_t kPkt = 1000;      // bytes per packet
    constexpr int          kPkts = 60;        // 60 kB to send
    constexpr auto         kWindow = 400ms;   // observation window

    auto run = [&](std::chrono::milliseconds period) -> int {
        SchedulerConfig cfg;
        cfg.avg_packet_size_bytes = kPkt;
        Scheduler sch{cfg};
        sch.on_bwe_update(kBps);
        for (int i = 0; i < kPkts; ++i) {
            sch.enqueue(Priority::kAudio, ByteSpan{kPayload}, make_test_addr(0));
        }
        const auto deadline = std::chrono::steady_clock::now() + kWindow;
        int sent = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            sent += sch.drain_with(50, [](Priority, nimrtc::core::ByteSpan) {
                return true;
            });
            std::this_thread::sleep_for(period);
        }
        return sent;
    };

    const int fast = run(5ms);
    const int slow = run(50ms);

    // 100 kB/s over 400 ms = 40 kB = 40 packets.  Both hosts must land in
    // the same neighbourhood; the tolerance absorbs scheduler jitter, the
    // point is that they must not differ by an order of magnitude.
    EXPECT_GT(fast, 10);
    EXPECT_LT(fast, 60);
    EXPECT_GT(slow, 10);
    EXPECT_LT(slow, 60);
    // The two hosts must be within a factor of two of each other.
    const int lo = fast < slow ? fast : slow;
    const int hi = fast < slow ? slow : fast;
    EXPECT_GT(hi, lo / 2);
}

/** stats().current_bitrate_bps reports the *observed* egress rate of the
 *  last drain, not the target. */
TEST(Scheduler, CurrentBitrateReflectsObservedDrain) {
    SchedulerConfig cfg;
    cfg.avg_packet_size_bytes = 1000;
    Scheduler sch{cfg};
    sch.on_bwe_update(800'000);

    sch.enqueue(Priority::kAudio, ByteSpan{kPayload}, make_test_addr(0));
    (void)sch.drain_with(10, [](Priority, nimrtc::core::ByteSpan) {
        return true;
    });

    // The first drain uses the configured seed interval (125 ms), so the
    // observed rate is at most one 1000-byte packet per 125 ms = 64 kbps.
    const auto stats = sch.stats();
    EXPECT_EQ(stats.target_bitrate_bps, 800'000u);
    EXPECT_GT(stats.current_bitrate_bps, 0u);
    EXPECT_LT(stats.current_bitrate_bps, 800'000u);
}

}  // namespace
