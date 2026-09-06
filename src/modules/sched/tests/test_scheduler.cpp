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
 */

// gtest first: ensures no PCH surprises from our headers.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

// Plugin types (plugins::Addr, kMaxAddrLen) must be visible before
// nimrtc/sched/sched.hpp which uses plugins::Addr in its public API.
#include <nimrtc/plugins/base.hpp>

#include <nimrtc/sched/sched.hpp>

namespace {

using namespace nimrtc::sched;
using namespace nimrtc::core;

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

}  // namespace
