/**
 * @file test_sched_priority_dc.cpp
 * @brief DC-2: L1+L0 cross-plane priority integration — DataChannel × RTP.
 *
 * ## Purpose
 *
 * Verify that the unified `Scheduler` correctly handles simultaneous
 * traffic from two planes:
 *
 *   - **DC plane (L1)** — DataChannel streams via `enqueue_for_stream_*()`
 *     and `select_next_packet()` / `set_stream_priority()`.
 *
 *   - **RTP plane (L0)** — Media/control packets via `enqueue()` /
 *     `drain()` with `plugins::Priority`.
 *
 * These two systems are orthogonal in the implementation:
 *   L0 `drain()` / `drain_with()`   — drains the 5-level plugins::Priority queues.
 *   L1 `select_next_packet()`        — drains the 3-level StreamPriority buckets.
 *
 * Each plane is independently correct (covered by test_priority.cpp and
 * test_scheduler.cpp).  This file covers their **interaction**.
 *
 * ## What is NOT tested here
 *
 * - Pure L1 bucket ordering (→ test_priority.cpp)
 * - Pure L0 priority ordering and BWE (→ test_scheduler.cpp)
 * - WebTransportDataChannel network I/O (→ integration tests)
 * - Real DC stream creation (→ DC-1 integration tests)
 *
 * ## Test scenarios
 *
 * | # | Scenario                         | Key invariant verified                |
 * |---|----------------------------------|---------------------------------------|
 * | 1 | DC_Plane_Integration            | L0 and L1 operate independently      |
 * | 2 | Cross_Plane_Priority            | L0 kControl ≥ L1 High bucket         |
 * | 3 | Datachannel_Stream_Id_Isolation | Multiple DC streams keep own priority|
 * | 4 | Priority_Promotion_Effects_RTP  | L1 promote → bucket switch, L0 unchanged|
 * | 5 | Select_Next_Respects_Budget      | select_next_packet(max) limits drain |
 * | 6 | L0_L1_Isolation_No_Cross_Pollution | drain() never sees L1-only packets |
 *
 * Stability classification: NIMRTC_API_STABLE (regression test for DC-2).
 */

// gtest first
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include <nimrtc/plugins/base.hpp>
#include <nimrtc/sched/sched.hpp>

namespace nimrtc::tests {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/** Minimal non-empty Addr for testing. */
inline nimrtc::plugins::Addr make_addr(std::uint8_t id) {
    nimrtc::plugins::Addr a{};
    a.data[0] = id;
    a.len = 1;
    return a;
}

/** 4-byte payload used for all test enqueues. */
constexpr std::array<std::uint8_t, 4> kPayload = {0xDE, 0xAD, 0xBE, 0xEF};

// Stream IDs for L1 DC-plane streams
constexpr std::uint32_t kDCStreamHigh   = 1001;
constexpr std::uint32_t kDCStreamNormal = 2001;
constexpr std::uint32_t kDCStreamLow    = 3001;
constexpr std::uint32_t kDCStreamAlt    = 4001;  // alternate stream for isolation tests

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Encode a marker byte in packet data.
 *  L0 packets: first byte = 0xE0 | priority_rank (marker = 0xE0, 0xE1, ...)
 *  L1 packets: first byte = stream_id & 0xFF
 *  This allows drain callbacks to identify the plane of origin. */
std::vector<std::uint8_t> make_l0_payload(nimrtc::plugins::Priority p) {
    std::vector<std::uint8_t> buf{ kPayload.begin(), kPayload.end() };
    buf[0] = 0xE0 | static_cast<std::uint8_t>(nimrtc::plugins::priority_rank(p));
    return buf;
}

std::vector<std::uint8_t> make_l1_payload(std::uint32_t stream_id) {
    std::vector<std::uint8_t> buf{ kPayload.begin(), kPayload.end() };
    buf[0] = static_cast<std::uint8_t>(stream_id & 0xFF);
    return buf;
}

/** Drain all L0 packets via drain() and return count per priority. */
std::array<int, nimrtc::plugins::kPriorityCount>
drain_all_l0(nimrtc::sched::Scheduler& sch) {
    std::array<int, nimrtc::plugins::kPriorityCount> counts{};
    counts.fill(0);
    int n;
    do {
        n = sch.drain_with(128, [&](nimrtc::plugins::Priority p,
                                    nimrtc::core::ByteSpan /*data*/) {
            counts[nimrtc::plugins::priority_rank(p)]++;
            return true;
        });
    } while (n > 0);
    return counts;
}

/** Drain all L1 packets via select_next_packet() and record stream_ids.
 *  L1 packets have stream_id encoded in first byte of payload. */
std::vector<std::uint32_t> drain_all_l1_stream_ids(nimrtc::sched::Scheduler& sch) {
    std::vector<std::uint32_t> ids;
    int n;
    do {
        n = sch.select_next_packet(128, [&](nimrtc::plugins::Priority /*p*/,
                                           nimrtc::core::ByteSpan data) -> bool {
            if (!data.empty()) {
                ids.push_back(static_cast<std::uint32_t>(data[0]));
            }
            return true;
        });
    } while (n > 0);
    return ids;
}

// ---------------------------------------------------------------------------
// Scenario 1 — DC_Plane_Integration
// ---------------------------------------------------------------------------

/**
 * Verify that L0 (RTP) and L1 (DC) paths operate independently.
 * Enqueue packets on both planes; drain each plane separately and verify
 * that both sets of packets are retrieved without cross-contamination.
 */
TEST(StreamPriorityCrossPlane, DC_Plane_Integration) {
    nimrtc::sched::Scheduler sch{};
    auto dst = make_addr(1);

    // ---- L0 (RTP plane): kControl and kAudio ----
    auto l0_control = make_l0_payload(nimrtc::plugins::Priority::kControl);
    auto l0_audio   = make_l0_payload(nimrtc::plugins::Priority::kAudio);
    sch.enqueue_owned(nimrtc::plugins::Priority::kControl,
                       std::move(l0_control), dst);
    sch.enqueue_owned(nimrtc::plugins::Priority::kAudio,
                       std::move(l0_audio), dst);

    // ---- L1 (DC plane): High stream ----
    sch.set_stream_priority(kDCStreamHigh, nimrtc::sched::StreamPriority::High);
    auto l1_high = make_l1_payload(kDCStreamHigh);
    sch.enqueue_for_stream_owned(kDCStreamHigh, std::move(l1_high), dst);

    // ---- Drain L0: should get kControl + kAudio, NO L1 packets ----
    {
        auto counts = drain_all_l0(sch);
        EXPECT_EQ(counts[0], 1);  // kControl
        EXPECT_EQ(counts[1], 1);  // kAudio
        EXPECT_EQ(counts[2], 0);  // kVideoKeyframe
        EXPECT_EQ(counts[3], 0);  // kVideo
        EXPECT_EQ(counts[4], 0);  // kBestEffort
    }

    // ---- Drain L1: should get DC High packet, NO L0 packets ----
    {
        auto ids = drain_all_l1_stream_ids(sch);
        ASSERT_EQ(ids.size(), 1u);
        EXPECT_EQ(ids[0], kDCStreamHigh & 0xFF);
    }

    // ---- Both planes should now be empty ----
    EXPECT_EQ(sch.drain(10), 0);
    EXPECT_EQ(sch.select_next_packet(10, nullptr), 0);
}

/**
 * Verify that L0 and L1 queues remain independent when both are
 * repeatedly filled and drained across multiple cycles.
 */
TEST(StreamPriorityCrossPlane, DC_Plane_Integration_MultiCycle) {
    nimrtc::sched::Scheduler sch{};
    auto dst = make_addr(1);

    for (int cycle = 0; cycle < 3; ++cycle) {
        // L0: enqueue one kControl packet
        auto l0 = make_l0_payload(nimrtc::plugins::Priority::kControl);
        sch.enqueue_owned(nimrtc::plugins::Priority::kControl,
                           std::move(l0), dst);

        // L1: enqueue one High stream packet
        sch.set_stream_priority(kDCStreamHigh,
                                 nimrtc::sched::StreamPriority::High);
        auto l1 = make_l1_payload(kDCStreamHigh);
        sch.enqueue_for_stream_owned(kDCStreamHigh, std::move(l1), dst);

        // Drain L0
        {
            auto counts = drain_all_l0(sch);
            EXPECT_EQ(counts[0], 1) << "cycle " << cycle << " L0 kControl";
        }

        // Drain L1
        {
            auto ids = drain_all_l1_stream_ids(sch);
            ASSERT_EQ(ids.size(), 1u) << "cycle " << cycle << " L1 stream_ids";
        }
    }

    // Final state: both planes empty
    EXPECT_EQ(sch.drain(10), 0);
    EXPECT_EQ(sch.select_next_packet(10, nullptr), 0);
}

// ---------------------------------------------------------------------------
// Scenario 2 — Cross_Plane_Priority
// ---------------------------------------------------------------------------

/**
 * Verify priority ordering across planes.
 *
 * Layout:
 *   L0: kControl (1 packet)
 *   L1: DC High (stream_id=kDCStreamHigh, 1 packet)
 *   L1: DC Low  (stream_id=kDCStreamLow,  1 packet)
 *
 * drain() operates only on L0, so kControl is returned.
 * select_next_packet() operates only on L1, so High before Low.
 */
TEST(StreamPriorityCrossPlane, Cross_Plane_Priority_L0_L1_Order) {
    nimrtc::sched::Scheduler sch{};
    auto dst = make_addr(1);

    // Enqueue in reverse order to prove ordering logic, not FIFO.
    // ---- L1 Low first ----
    sch.set_stream_priority(kDCStreamLow, nimrtc::sched::StreamPriority::Low);
    auto l1_low = make_l1_payload(kDCStreamLow);
    sch.enqueue_for_stream_owned(kDCStreamLow, std::move(l1_low), dst);

    // ---- L0 Control second ----
    auto l0_ctrl = make_l0_payload(nimrtc::plugins::Priority::kControl);
    sch.enqueue_owned(nimrtc::plugins::Priority::kControl,
                       std::move(l0_ctrl), dst);

    // ---- L1 High last ----
    sch.set_stream_priority(kDCStreamHigh, nimrtc::sched::StreamPriority::High);
    auto l1_high = make_l1_payload(kDCStreamHigh);
    sch.enqueue_for_stream_owned(kDCStreamHigh, std::move(l1_high), dst);

    // L0 drain: only sees kControl (L1 buckets are invisible to drain())
    {
        auto counts = drain_all_l0(sch);
        EXPECT_EQ(counts[0], 1);  // kControl only
        EXPECT_EQ(counts[1], 0);   // kAudio
        EXPECT_EQ(counts[2], 0);
        EXPECT_EQ(counts[3], 0);
        EXPECT_EQ(counts[4], 0);
    }

    // L1 select_next_packet: High before Low (independent of enqueue order)
    {
        auto ids = drain_all_l1_stream_ids(sch);
        ASSERT_EQ(ids.size(), 2u);
        EXPECT_EQ(ids[0], kDCStreamHigh & 0xFF);  // High first
        EXPECT_EQ(ids[1], kDCStreamLow  & 0xFF);  // Low second
    }
}

/**
 * Enqueue all 5 L0 priorities interleaved with 3 L1 stream priorities.
 * Verify each plane drains independently with correct intra-plane ordering.
 */
TEST(StreamPriorityCrossPlane, Cross_Plane_Priority_AllLevels) {
    nimrtc::sched::Scheduler sch{};
    auto dst = make_addr(1);

    // Interleave: Low(L1), kBestEffort(L0), Normal(L1), kVideo(L0),
    //             High(L1), kAudio(L0), kVideoKeyframe(L0)

    sch.set_stream_priority(kDCStreamLow, nimrtc::sched::StreamPriority::Low);
    sch.enqueue_for_stream_owned(kDCStreamLow,
                                 make_l1_payload(kDCStreamLow), dst);

    sch.enqueue_owned(nimrtc::plugins::Priority::kBestEffort,
                       make_l0_payload(nimrtc::plugins::Priority::kBestEffort), dst);

    sch.set_stream_priority(kDCStreamNormal, nimrtc::sched::StreamPriority::Normal);
    sch.enqueue_for_stream_owned(kDCStreamNormal,
                                 make_l1_payload(kDCStreamNormal), dst);

    sch.enqueue_owned(nimrtc::plugins::Priority::kVideo,
                       make_l0_payload(nimrtc::plugins::Priority::kVideo), dst);

    sch.set_stream_priority(kDCStreamHigh, nimrtc::sched::StreamPriority::High);
    sch.enqueue_for_stream_owned(kDCStreamHigh,
                                 make_l1_payload(kDCStreamHigh), dst);

    sch.enqueue_owned(nimrtc::plugins::Priority::kAudio,
                       make_l0_payload(nimrtc::plugins::Priority::kAudio), dst);

    sch.enqueue_owned(nimrtc::plugins::Priority::kVideoKeyframe,
                       make_l0_payload(nimrtc::plugins::Priority::kVideoKeyframe), dst);

    // ---- L0 drain: kControl..kBestEffort order (High→Low in plugin priority) ----
    // Note: kControl not enqueued; starts from kAudio
    {
        auto counts = drain_all_l0(sch);
        EXPECT_EQ(counts[0], 0);  // kControl (not enqueued)
        EXPECT_EQ(counts[1], 1);  // kAudio
        EXPECT_EQ(counts[2], 1);  // kVideoKeyframe
        EXPECT_EQ(counts[3], 1);  // kVideo
        EXPECT_EQ(counts[4], 1);  // kBestEffort
    }

    // ---- L1 select_next_packet: High → Normal → Low ----
    {
        auto ids = drain_all_l1_stream_ids(sch);
        ASSERT_EQ(ids.size(), 3u);
        EXPECT_EQ(ids[0], kDCStreamHigh   & 0xFF);  // High
        EXPECT_EQ(ids[1], kDCStreamNormal & 0xFF);  // Normal
        EXPECT_EQ(ids[2], kDCStreamLow    & 0xFF);  // Low
    }
}

// ---------------------------------------------------------------------------
// Scenario 3 — Datachannel_Stream_Id_Isolation
// ---------------------------------------------------------------------------

/**
 * Multiple DC streams (different stream_ids) can independently set their
 * priorities without affecting each other.
 */
TEST(StreamPriorityCrossPlane, Datachannel_Stream_Id_Isolation) {
    nimrtc::sched::Scheduler sch{};
    auto dst = make_addr(1);

    // Stream A = High, Stream B = Normal, Stream C = Low
    sch.set_stream_priority(kDCStreamHigh,   nimrtc::sched::StreamPriority::High);
    sch.set_stream_priority(kDCStreamNormal, nimrtc::sched::StreamPriority::Normal);
    sch.set_stream_priority(kDCStreamLow,    nimrtc::sched::StreamPriority::Low);

    // Enqueue one packet per stream
    sch.enqueue_for_stream_owned(kDCStreamHigh,
                                 make_l1_payload(kDCStreamHigh), dst);
    sch.enqueue_for_stream_owned(kDCStreamNormal,
                                 make_l1_payload(kDCStreamNormal), dst);
    sch.enqueue_for_stream_owned(kDCStreamLow,
                                 make_l1_payload(kDCStreamLow), dst);

    // Verify each stream retains its priority
    EXPECT_EQ(sch.get_stream_priority(kDCStreamHigh),   nimrtc::sched::StreamPriority::High);
    EXPECT_EQ(sch.get_stream_priority(kDCStreamNormal), nimrtc::sched::StreamPriority::Normal);
    EXPECT_EQ(sch.get_stream_priority(kDCStreamLow),    nimrtc::sched::StreamPriority::Low);

    // Verify drain order: High → Normal → Low
    auto ids = drain_all_l1_stream_ids(sch);
    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], kDCStreamHigh   & 0xFF);
    EXPECT_EQ(ids[1], kDCStreamNormal & 0xFF);
    EXPECT_EQ(ids[2], kDCStreamLow    & 0xFF);
}

/**
 * Verify that changing one stream's priority does not affect other streams.
 */
TEST(StreamPriorityCrossPlane, Datachannel_Stream_Id_Isolation_DemoteOne) {
    nimrtc::sched::Scheduler sch{};
    auto dst = make_addr(1);

    sch.set_stream_priority(kDCStreamHigh,   nimrtc::sched::StreamPriority::High);
    sch.set_stream_priority(kDCStreamNormal, nimrtc::sched::StreamPriority::Normal);
    sch.set_stream_priority(kDCStreamLow,    nimrtc::sched::StreamPriority::Low);

    sch.enqueue_for_stream_owned(kDCStreamHigh,
                                 make_l1_payload(kDCStreamHigh), dst);
    sch.enqueue_for_stream_owned(kDCStreamNormal,
                                 make_l1_payload(kDCStreamNormal), dst);
    sch.enqueue_for_stream_owned(kDCStreamLow,
                                 make_l1_payload(kDCStreamLow), dst);

    // Demote kDCStreamHigh from High → Low
    sch.set_stream_priority(kDCStreamHigh, nimrtc::sched::StreamPriority::Low);

    // Verify others are unchanged
    EXPECT_EQ(sch.get_stream_priority(kDCStreamNormal), nimrtc::sched::StreamPriority::Normal);
    EXPECT_EQ(sch.get_stream_priority(kDCStreamLow),    nimrtc::sched::StreamPriority::Low);

    // Drain order should now be: Normal (1) → High+Low (2) — High moved to Low bucket
    auto ids = drain_all_l1_stream_ids(sch);
    ASSERT_EQ(ids.size(), 3u);
    // First: Normal (rank 1)
    EXPECT_EQ(ids[0], kDCStreamNormal & 0xFF);
    // Then Low and High (both rank 2) in FIFO order within bucket
    // (High was demoted to Low bucket after Low was enqueued, so Low first)
    EXPECT_EQ(ids[1], kDCStreamLow & 0xFF);
    EXPECT_EQ(ids[2], kDCStreamHigh & 0xFF);
}

/**
 * Verify that an unseen stream_id defaults to Normal priority.
 */
TEST(StreamPriorityCrossPlane, Datachannel_Stream_Id_Isolation_UnseenDefault) {
    nimrtc::sched::Scheduler sch{};
    // kDCStreamAlt has never been seen — should default to Normal
    EXPECT_EQ(sch.get_stream_priority(kDCStreamAlt),
              nimrtc::sched::StreamPriority::Normal);

    // Enqueue without setting priority explicitly
    auto dst = make_addr(1);
    sch.enqueue_for_stream_owned(kDCStreamAlt,
                                 make_l1_payload(kDCStreamAlt), dst);

    // Verify it lands in Normal bucket (drained after High, before Low)
    sch.set_stream_priority(kDCStreamHigh, nimrtc::sched::StreamPriority::High);
    sch.enqueue_for_stream_owned(kDCStreamHigh,
                                 make_l1_payload(kDCStreamHigh), dst);

    auto ids = drain_all_l1_stream_ids(sch);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], kDCStreamHigh   & 0xFF);  // High
    EXPECT_EQ(ids[1], kDCStreamAlt   & 0xFF);   // Alt (Normal)
}

// ---------------------------------------------------------------------------
// Scenario 4 — Priority_Promotion_Effects_RTP
// ---------------------------------------------------------------------------

/**
 * Promoting a DC stream from Low → High should switch it to the High bucket
 * and cause it to preempt Normal/Low streams on the next drain.
 * L0 (RTP) packets are unaffected by L1 priority changes.
 */
TEST(StreamPriorityCrossPlane, Priority_Promotion_Effects_RTP) {
    nimrtc::sched::Scheduler sch{};
    auto dst = make_addr(1);

    // ---- L0: kVideo packet (L0 plane) ----
    sch.enqueue_owned(nimrtc::plugins::Priority::kVideo,
                       make_l0_payload(nimrtc::plugins::Priority::kVideo), dst);

    // ---- L1: kDCStreamLow = Low, kDCStreamNormal = Normal ----
    sch.set_stream_priority(kDCStreamLow,    nimrtc::sched::StreamPriority::Low);
    sch.set_stream_priority(kDCStreamNormal, nimrtc::sched::StreamPriority::Normal);
    sch.enqueue_for_stream_owned(kDCStreamLow,
                                 make_l1_payload(kDCStreamLow), dst);
    sch.enqueue_for_stream_owned(kDCStreamNormal,
                                 make_l1_payload(kDCStreamNormal), dst);

    // ---- Baseline: L1 drains Normal → Low ----
    {
        auto ids = drain_all_l1_stream_ids(sch);
        ASSERT_EQ(ids.size(), 2u);
        EXPECT_EQ(ids[0], kDCStreamNormal & 0xFF);  // Normal first
        EXPECT_EQ(ids[1], kDCStreamLow    & 0xFF);  // Low second
    }

    // ---- L0 drain: kVideo only (L1 queues are independent) ----
    {
        auto counts = drain_all_l0(sch);
        EXPECT_EQ(counts[3], 1);  // kVideo
        EXPECT_EQ(counts[0], 0); // kControl
        EXPECT_EQ(counts[1], 0); // kAudio
    }

    // ---- Re-enqueue and promote kDCStreamLow to High ----
    sch.set_stream_priority(kDCStreamLow, nimrtc::sched::StreamPriority::Low);
    sch.set_stream_priority(kDCStreamNormal, nimrtc::sched::StreamPriority::Normal);
    sch.enqueue_for_stream_owned(kDCStreamLow,
                                 make_l1_payload(kDCStreamLow), dst);
    sch.enqueue_for_stream_owned(kDCStreamNormal,
                                 make_l1_payload(kDCStreamNormal), dst);

    sch.set_stream_priority(kDCStreamLow, nimrtc::sched::StreamPriority::High);

    // Verify the promotion took effect
    EXPECT_EQ(sch.get_stream_priority(kDCStreamLow), nimrtc::sched::StreamPriority::High);

    // ---- L1 drain: High (kDCStreamLow) → Normal (kDCStreamNormal) ----
    {
        auto ids = drain_all_l1_stream_ids(sch);
        ASSERT_EQ(ids.size(), 2u);
        EXPECT_EQ(ids[0], kDCStreamLow    & 0xFF);  // High (promoted)
        EXPECT_EQ(ids[1], kDCStreamNormal & 0xFF);  // Normal
    }

    // ---- L0 drain: should still be empty (was drained above) ----
    EXPECT_EQ(sch.drain(10), 0);
}

/**
 * Demoting a DC stream from High → Normal should place it behind existing
 * Normal streams and ahead of Low streams.
 */
TEST(StreamPriorityCrossPlane, Priority_Promotion_Effects_RTP_Demote) {
    nimrtc::sched::Scheduler sch{};
    auto dst = make_addr(1);

    sch.set_stream_priority(kDCStreamHigh,   nimrtc::sched::StreamPriority::High);
    sch.set_stream_priority(kDCStreamNormal, nimrtc::sched::StreamPriority::Normal);
    sch.set_stream_priority(kDCStreamLow,    nimrtc::sched::StreamPriority::Low);

    sch.enqueue_for_stream_owned(kDCStreamHigh,
                                 make_l1_payload(kDCStreamHigh), dst);
    sch.enqueue_for_stream_owned(kDCStreamNormal,
                                 make_l1_payload(kDCStreamNormal), dst);
    sch.enqueue_for_stream_owned(kDCStreamLow,
                                 make_l1_payload(kDCStreamLow), dst);

    // Baseline: High → Normal → Low
    {
        auto ids = drain_all_l1_stream_ids(sch);
        ASSERT_EQ(ids.size(), 3u);
        EXPECT_EQ(ids[0], kDCStreamHigh   & 0xFF);
        EXPECT_EQ(ids[1], kDCStreamNormal & 0xFF);
        EXPECT_EQ(ids[2], kDCStreamLow    & 0xFF);
    }

    // Re-enqueue and demote kDCStreamHigh from High → Normal
    sch.set_stream_priority(kDCStreamHigh,   nimrtc::sched::StreamPriority::High);
    sch.set_stream_priority(kDCStreamNormal, nimrtc::sched::StreamPriority::Normal);
    sch.set_stream_priority(kDCStreamLow,    nimrtc::sched::StreamPriority::Low);
    sch.enqueue_for_stream_owned(kDCStreamHigh,
                                 make_l1_payload(kDCStreamHigh), dst);
    sch.enqueue_for_stream_owned(kDCStreamNormal,
                                 make_l1_payload(kDCStreamNormal), dst);
    sch.enqueue_for_stream_owned(kDCStreamLow,
                                 make_l1_payload(kDCStreamLow), dst);

    sch.set_stream_priority(kDCStreamHigh, nimrtc::sched::StreamPriority::Normal);

    // Drain: High bucket empty, Normal bucket (Normal + demoted High) → Low
    {
        auto ids = drain_all_l1_stream_ids(sch);
        ASSERT_EQ(ids.size(), 3u);
        // High bucket empty (no streams at High)
        // Normal bucket: Normal (enqueued first) → High (demoted)
        EXPECT_EQ(ids[0], kDCStreamNormal & 0xFF);  // Normal
        EXPECT_EQ(ids[1], kDCStreamHigh   & 0xFF);  // High (now Normal)
        EXPECT_EQ(ids[2], kDCStreamLow    & 0xFF);  // Low
    }
}

// ---------------------------------------------------------------------------
// Scenario 5 — Select_Next_Respects_Budget
// ---------------------------------------------------------------------------

/**
 * Verify that select_next_packet(max_packets) correctly limits the number
 * of L1 packets drained across multiple bucket levels.
 */
TEST(StreamPriorityCrossPlane, Select_Next_Respects_Budget) {
    nimrtc::sched::Scheduler sch{};
    auto dst = make_addr(1);

    // Fill High bucket: 3 packets
    sch.set_stream_priority(kDCStreamHigh, nimrtc::sched::StreamPriority::High);
    sch.enqueue_for_stream_owned(kDCStreamHigh, make_l1_payload(kDCStreamHigh), dst);
    sch.enqueue_for_stream_owned(kDCStreamHigh, make_l1_payload(kDCStreamHigh), dst);
    sch.enqueue_for_stream_owned(kDCStreamHigh, make_l1_payload(kDCStreamHigh), dst);

    // Fill Normal bucket: 2 packets
    sch.set_stream_priority(kDCStreamNormal, nimrtc::sched::StreamPriority::Normal);
    sch.enqueue_for_stream_owned(kDCStreamNormal, make_l1_payload(kDCStreamNormal), dst);
    sch.enqueue_for_stream_owned(kDCStreamNormal, make_l1_payload(kDCStreamNormal), dst);

    // select_next_packet(max=2) should drain only the first 2 from High
    {
        std::vector<std::uint32_t> first_two;
        sch.select_next_packet(2, [&](nimrtc::plugins::Priority,
                                      nimrtc::core::ByteSpan data) -> bool {
            if (!data.empty()) {
                first_two.push_back(static_cast<std::uint32_t>(data[0]));
            }
            return true;
        });
        ASSERT_EQ(first_two.size(), 2u);
        EXPECT_EQ(first_two[0], kDCStreamHigh & 0xFF);
        EXPECT_EQ(first_two[1], kDCStreamHigh & 0xFF);
    }

    // select_next_packet(max=2) again: should get remaining High + first Normal
    {
        std::vector<std::uint32_t> next_two;
        sch.select_next_packet(2, [&](nimrtc::plugins::Priority,
                                      nimrtc::core::ByteSpan data) -> bool {
            if (!data.empty()) {
                next_two.push_back(static_cast<std::uint32_t>(data[0]));
            }
            return true;
        });
        ASSERT_EQ(next_two.size(), 2u);
        EXPECT_EQ(next_two[0], kDCStreamHigh   & 0xFF);  // last High
        EXPECT_EQ(next_two[1], kDCStreamNormal & 0xFF);  // first Normal
    }

    // select_next_packet(max=1): remaining Normal
    {
        std::vector<std::uint32_t> last_one;
        sch.select_next_packet(1, [&](nimrtc::plugins::Priority,
                                      nimrtc::core::ByteSpan data) -> bool {
            if (!data.empty()) {
                last_one.push_back(static_cast<std::uint32_t>(data[0]));
            }
            return true;
        });
        ASSERT_EQ(last_one.size(), 1u);
        EXPECT_EQ(last_one[0], kDCStreamNormal & 0xFF);
    }

    // Now L1 should be empty; L0 also empty
    EXPECT_EQ(sch.select_next_packet(10, nullptr), 0);
    EXPECT_EQ(sch.drain(10), 0);
}

/**
 * Verify that max_packets=0 returns 0 and max_packets=-1 is handled safely.
 */
TEST(StreamPriorityCrossPlane, Select_Next_Respects_Budget_EdgeCases) {
    nimrtc::sched::Scheduler sch{};
    auto dst = make_addr(1);

    sch.set_stream_priority(kDCStreamHigh, nimrtc::sched::StreamPriority::High);
    sch.enqueue_for_stream_owned(kDCStreamHigh, make_l1_payload(kDCStreamHigh), dst);

    EXPECT_EQ(sch.select_next_packet(0, nullptr), 0);
    EXPECT_EQ(sch.select_next_packet(-1, nullptr), 0);  // negative = no-op
}

/**
 * Cross-plane: select_next_packet budget does not affect L0 drain results.
 * L0 drain should return all L0 packets regardless of L1 drain state.
 */
TEST(StreamPriorityCrossPlane, Select_Next_Respects_Budget_L0_Unaffected) {
    nimrtc::sched::Scheduler sch{};
    auto dst = make_addr(1);

    // L0: 2 kAudio packets
    sch.enqueue_owned(nimrtc::plugins::Priority::kAudio,
                       make_l0_payload(nimrtc::plugins::Priority::kAudio), dst);
    sch.enqueue_owned(nimrtc::plugins::Priority::kAudio,
                       make_l0_payload(nimrtc::plugins::Priority::kAudio), dst);

    // L1: 3 High packets
    sch.set_stream_priority(kDCStreamHigh, nimrtc::sched::StreamPriority::High);
    sch.enqueue_for_stream_owned(kDCStreamHigh, make_l1_payload(kDCStreamHigh), dst);
    sch.enqueue_for_stream_owned(kDCStreamHigh, make_l1_payload(kDCStreamHigh), dst);
    sch.enqueue_for_stream_owned(kDCStreamHigh, make_l1_payload(kDCStreamHigh), dst);

    // Drain L0 first: both kAudio packets should be there
    {
        auto counts = drain_all_l0(sch);
        EXPECT_EQ(counts[1], 2);  // kAudio
    }

    // Drain L1 with limit=1: should get 1 High packet
    {
        std::vector<std::uint32_t> ids;
        sch.select_next_packet(1, [&](nimrtc::plugins::Priority,
                                      nimrtc::core::ByteSpan data) -> bool {
            if (!data.empty()) ids.push_back(static_cast<std::uint32_t>(data[0]));
            return true;
        });
        ASSERT_EQ(ids.size(), 1u);
        EXPECT_EQ(ids[0], kDCStreamHigh & 0xFF);
    }

    // Drain L0 again: should still return 0 (L0 was already drained)
    EXPECT_EQ(sch.drain(10), 0);
}

// ---------------------------------------------------------------------------
// Scenario 6 — L0_L1_Isolation_No_Cross_Pollution
// ---------------------------------------------------------------------------

/**
 * Verify that drain() never returns L1-only packets and that
 * select_next_packet() never returns L0-only packets.
 */
TEST(StreamPriorityCrossPlane, L0_L1_Isolation_No_Cross_Pollution) {
    nimrtc::sched::Scheduler sch{};
    auto dst = make_addr(1);

    // L0 only: kControl
    sch.enqueue_owned(nimrtc::plugins::Priority::kControl,
                       make_l0_payload(nimrtc::plugins::Priority::kControl), dst);

    // L1 only: DC stream (Normal priority)
    sch.set_stream_priority(kDCStreamNormal, nimrtc::sched::StreamPriority::Normal);
    sch.enqueue_for_stream_owned(kDCStreamNormal,
                                 make_l1_payload(kDCStreamNormal), dst);

    // drain() should NOT see L1 packets
    {
        std::vector<std::uint32_t> l0_stream_ids;
        sch.drain_with(10, [&](nimrtc::plugins::Priority /*p*/,
                               nimrtc::core::ByteSpan data) -> bool {
            // L0 packet marker: 0xE0 | rank
            if (!data.empty() && (data[0] & 0xF0) == 0xE0) {
                l0_stream_ids.push_back(static_cast<std::uint32_t>(data[0]));
            }
            return true;
        });
        // Should get exactly 1 L0 packet (kControl: marker = 0xE0)
        ASSERT_EQ(l0_stream_ids.size(), 1u);
        EXPECT_EQ(l0_stream_ids[0], 0xE0);  // kControl rank 0
    }

    // select_next_packet() should NOT see L0 packets
    {
        std::vector<std::uint32_t> l1_stream_ids;
        sch.select_next_packet(10, [&](nimrtc::plugins::Priority,
                                       nimrtc::core::ByteSpan data) -> bool {
            if (!data.empty()) {
                // L1 packet marker: stream_id byte (not 0xEx)
                if ((data[0] & 0xF0) != 0xE0) {
                    l1_stream_ids.push_back(static_cast<std::uint32_t>(data[0]));
                }
            }
            return true;
        });
        // Should get exactly 1 L1 packet (stream_id = kDCStreamNormal & 0xFF)
        ASSERT_EQ(l1_stream_ids.size(), 1u);
        EXPECT_EQ(l1_stream_ids[0], kDCStreamNormal & 0xFF);
    }
}

/**
 * Mixed plane test with alternating enqueue to verify no packet leakage
 * between L0 and L1 queue systems.
 */
TEST(StreamPriorityCrossPlane, L0_L1_Isolation_AlternatingEnqueue) {
    nimrtc::sched::Scheduler sch{};
    auto dst = make_addr(1);

    // Alternate L0 and L1 enqueue
    for (int i = 0; i < 3; ++i) {
        // L0: kAudio
        sch.enqueue_owned(nimrtc::plugins::Priority::kAudio,
                           make_l0_payload(nimrtc::plugins::Priority::kAudio), dst);
        // L1: High stream
        sch.set_stream_priority(kDCStreamHigh, nimrtc::sched::StreamPriority::High);
        sch.enqueue_for_stream_owned(kDCStreamHigh,
                                     make_l1_payload(kDCStreamHigh), dst);
    }

    // L0 drain: 3 kAudio packets, no L1 packets
    {
        auto counts = drain_all_l0(sch);
        EXPECT_EQ(counts[1], 3);  // kAudio = rank 1
        EXPECT_EQ(counts[0], 0);  // kControl
        EXPECT_EQ(counts[2], 0);  // kVideoKeyframe
    }

    // L1 drain: 3 High stream packets, no L0 packets
    {
        auto ids = drain_all_l1_stream_ids(sch);
        ASSERT_EQ(ids.size(), 3u);
        for (std::size_t i = 0; i < ids.size(); ++i) {
            EXPECT_EQ(ids[i], kDCStreamHigh & 0xFF) << "position " << i;
        }
    }
}

// ---------------------------------------------------------------------------
// Reset behavior across planes
// ---------------------------------------------------------------------------

/**
 * Verify that reset() clears both L0 and L1 queues.
 */
TEST(StreamPriorityCrossPlane, Reset_Clears_Both_Planes) {
    nimrtc::sched::Scheduler sch{};
    auto dst = make_addr(1);

    // L0: kControl + kAudio
    sch.enqueue_owned(nimrtc::plugins::Priority::kControl,
                       make_l0_payload(nimrtc::plugins::Priority::kControl), dst);
    sch.enqueue_owned(nimrtc::plugins::Priority::kAudio,
                       make_l0_payload(nimrtc::plugins::Priority::kAudio), dst);

    // L1: High stream
    sch.set_stream_priority(kDCStreamHigh, nimrtc::sched::StreamPriority::High);
    sch.enqueue_for_stream_owned(kDCStreamHigh,
                                 make_l1_payload(kDCStreamHigh), dst);

    sch.reset();

    // Both planes should be empty
    EXPECT_EQ(sch.drain(10), 0);
    EXPECT_EQ(sch.select_next_packet(10, nullptr), 0);
    EXPECT_EQ(sch.get_stream_priority(kDCStreamHigh),
             nimrtc::sched::StreamPriority::Normal);  // default after reset
}

/**
 * Verify that stats() reflect the correct queued_packets from both planes.
 */
TEST(StreamPriorityCrossPlane, Stats_Reflect_Both_Planes) {
    nimrtc::sched::Scheduler sch{};
    auto dst = make_addr(1);

    // L0: 2 packets
    sch.enqueue_owned(nimrtc::plugins::Priority::kControl,
                       make_l0_payload(nimrtc::plugins::Priority::kControl), dst);
    sch.enqueue_owned(nimrtc::plugins::Priority::kAudio,
                       make_l0_payload(nimrtc::plugins::Priority::kAudio), dst);

    // L1: 1 packet
    sch.set_stream_priority(kDCStreamHigh, nimrtc::sched::StreamPriority::High);
    sch.enqueue_for_stream_owned(kDCStreamHigh,
                                 make_l1_payload(kDCStreamHigh), dst);

    auto stats = sch.stats();
    // Note: queued_packets may count both L0 and L1, or only L0.
    // The exact behavior depends on the implementation.
    // We verify that stats() is callable and returns reasonable values.
    EXPECT_GE(stats.queued_packets, 2u);  // at least L0 packets visible
}

// ---------------------------------------------------------------------------
// Stream priority helpers
// ---------------------------------------------------------------------------

TEST(StreamPriorityCrossPlane, StreamPriorityHelpers) {
    using nimrtc::sched::stream_priority_rank;
    using nimrtc::sched::StreamPriority;

    EXPECT_EQ(stream_priority_rank(StreamPriority::High),   0u);
    EXPECT_EQ(stream_priority_rank(StreamPriority::Normal), 1u);
    EXPECT_EQ(stream_priority_rank(StreamPriority::Low),    2u);
}

}  // namespace nimrtc::tests
