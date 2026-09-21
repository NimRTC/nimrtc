/**
 * @file src/modules/sched/tests/test_priority.cpp
 * @brief DC-2: L1 strict-priority scheduler — unit tests.
 *
 * Scenarios:
 *   A — High + Low pending → High packets emitted first until bucket empty.
 *   B — Three streams High / Normal / Low interleaved → strict-priority order
 *       regardless of FIFO enqueue position.
 *   C — Dynamic priority change: Low → High promotion switches emission order.
 *
 * Uses the same gtest + nimrtc_add_test() style as test_scheduler.cpp.
 */

// gtest first
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include <nimrtc/plugins/base.hpp>
#include <nimrtc/sched/sched.hpp>

namespace {

using namespace nimrtc;
using namespace nimrtc::sched;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Minimal non-empty Addr for testing. */
inline nimrtc::plugins::Addr make_addr(std::uint8_t id) {
    nimrtc::plugins::Addr a{};
    a.data[0] = id;
    a.len = 1;
    return a;
}

/** 4-byte payload used for all test enqueues. */
constexpr std::array<std::uint8_t, 4> kPayload = {0xAA, 0xBB, 0xCC, 0xDD};

/** Per-test stream_id assignments.  We keep stream_ids distinct so the
 *  order of drained packets uniquely determines which bucket each one
 *  came from (we then look up the bucket via get_stream_priority()). */
constexpr std::uint32_t kStreamHighA   = 1001;
constexpr std::uint32_t kStreamHighB   = 1002;
constexpr std::uint32_t kStreamNormalA = 2001;
constexpr std::uint32_t kStreamNormalB = 2002;
constexpr std::uint32_t kStreamLowA    = 3001;
constexpr std::uint32_t kStreamLowB    = 3002;

/** Drain via select_next_packet and record the stream_id of each packet
 *  in emission order.  The first byte of each payload encodes the stream_id
 *  (low byte). */
struct DrainResult {
    std::vector<std::uint32_t> stream_id_order;
    int total = 0;
};

DrainResult drain_all_record_ids(Scheduler& sch) {
    DrainResult r{};
    int n;
    do {
        n = sch.select_next_packet(128, [&](sched::Priority /*p*/,
                                            core::ByteSpan data) -> bool {
            if (!data.empty()) {
                // Encode stream_id in first byte for tracking.
                r.stream_id_order.push_back(static_cast<std::uint32_t>(data[0]));
            }
            return true;
        });
        r.total += n;
    } while (n > 0);
    return r;
}

/** Map a stream_id to its expected bucket rank, looked up live. */
static std::uint32_t expected_bucket(Scheduler& sch, std::uint32_t stream_id) {
    return static_cast<std::uint32_t>(sch.get_stream_priority(stream_id));
}

/** Enqueue owned data with stream_id encoded in the first payload byte.
 *  Sets the stream's priority to `level` first so the packet lands in
 *  the right bucket. */
void enqueue_for_stream_with_id(Scheduler& sch,
                                std::uint32_t stream_id,
                                StreamPriority level,
                                plugins::Addr dst) {
    sch.set_stream_priority(stream_id, level);
    std::vector<std::uint8_t> buf;
    buf.push_back(static_cast<std::uint8_t>(stream_id & 0xFF));  // stream_id marker
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(0);
    sch.enqueue_for_stream_owned(stream_id, std::move(buf), dst);
}

// ---------------------------------------------------------------------------
// Scenario A — High + Low pending → High emitted first
// ---------------------------------------------------------------------------

/** When High and Low buckets both have packets, drain emits all High first. */
TEST(StreamPriority, HighBucketDrainedBeforeLow) {
    Scheduler sch{};
    auto dst = make_addr(1);

    // Low stream: 3 packets (enqueued first)
    enqueue_for_stream_with_id(sch, kStreamLowA, StreamPriority::Low, dst);
    enqueue_for_stream_with_id(sch, kStreamLowA, StreamPriority::Low, dst);
    enqueue_for_stream_with_id(sch, kStreamLowA, StreamPriority::Low, dst);

    // High stream: 2 packets (enqueued AFTER Low to prove FIFO doesn't matter)
    enqueue_for_stream_with_id(sch, kStreamHighA, StreamPriority::High, dst);
    enqueue_for_stream_with_id(sch, kStreamHighA, StreamPriority::High, dst);

    auto r = drain_all_record_ids(sch);

    ASSERT_EQ(r.total, 5);
    // First 2 must be High (bucket rank 0 — kStreamHighA's bucket)
    EXPECT_EQ(r.stream_id_order[0], kStreamHighA & 0xFF);
    EXPECT_EQ(r.stream_id_order[1], kStreamHighA & 0xFF);
    EXPECT_EQ(expected_bucket(sch, kStreamHighA), 0u);  // High = 0
    // Last 3 must be Low (bucket rank 2)
    EXPECT_EQ(r.stream_id_order[2], kStreamLowA & 0xFF);
    EXPECT_EQ(r.stream_id_order[3], kStreamLowA & 0xFF);
    EXPECT_EQ(r.stream_id_order[4], kStreamLowA & 0xFF);
    EXPECT_EQ(expected_bucket(sch, kStreamLowA), 2u);   // Low = 2
}

/** drain(max_packets) limit: stops after N even if higher bucket still has more. */
TEST(StreamPriority, DrainLimitRespectedAcrossBuckets) {
    Scheduler sch{};
    auto dst = make_addr(1);

    enqueue_for_stream_with_id(sch, kStreamHighA,   StreamPriority::High,   dst);
    enqueue_for_stream_with_id(sch, kStreamHighB,   StreamPriority::High,   dst);
    enqueue_for_stream_with_id(sch, kStreamHighA,   StreamPriority::High,   dst);
    enqueue_for_stream_with_id(sch, kStreamLowA,    StreamPriority::Low,    dst);
    enqueue_for_stream_with_id(sch, kStreamLowB,    StreamPriority::Low,    dst);

    // Drain only 2 — must be from High bucket.
    std::vector<std::uint32_t> first_two;
    sch.select_next_packet(2, [&](sched::Priority, core::ByteSpan d) -> bool {
        if (!d.empty()) first_two.push_back(static_cast<std::uint32_t>(d[0]));
        return true;
    });
    ASSERT_EQ(first_two.size(), 2u);
    EXPECT_EQ(first_two[0], kStreamHighA & 0xFF);
    EXPECT_EQ(first_two[1], kStreamHighB & 0xFF);

    // Next drain(2) should get the last High + first Low.
    std::vector<std::uint32_t> next_two;
    sch.select_next_packet(2, [&](sched::Priority, core::ByteSpan d) -> bool {
        if (!d.empty()) next_two.push_back(static_cast<std::uint32_t>(d[0]));
        return true;
    });
    ASSERT_EQ(next_two.size(), 2u);
    EXPECT_EQ(next_two[0], kStreamHighA & 0xFF);
    EXPECT_EQ(next_two[1], kStreamLowA & 0xFF);
}

// ---------------------------------------------------------------------------
// Scenario B — Three streams High / Normal / Low interleaved → strict order
// ---------------------------------------------------------------------------

/** Enqueue 9 packets (3 per stream) in round-robin order.
 *  drain_all must return all High first, then all Normal, then all Low. */
TEST(StreamPriority, ThreeStreamsInterleavedStrictOrder) {
    Scheduler sch{};
    auto dst = make_addr(1);

    // Round-robin: Low, Normal, High, Low, Normal, High, Low, Normal, High
    enqueue_for_stream_with_id(sch, kStreamLowA,    StreamPriority::Low,    dst);
    enqueue_for_stream_with_id(sch, kStreamNormalA, StreamPriority::Normal, dst);
    enqueue_for_stream_with_id(sch, kStreamHighA,   StreamPriority::High,   dst);
    enqueue_for_stream_with_id(sch, kStreamLowA,    StreamPriority::Low,    dst);
    enqueue_for_stream_with_id(sch, kStreamNormalA, StreamPriority::Normal, dst);
    enqueue_for_stream_with_id(sch, kStreamHighA,   StreamPriority::High,   dst);
    enqueue_for_stream_with_id(sch, kStreamLowA,    StreamPriority::Low,    dst);
    enqueue_for_stream_with_id(sch, kStreamNormalA, StreamPriority::Normal, dst);
    enqueue_for_stream_with_id(sch, kStreamHighA,   StreamPriority::High,   dst);

    auto r = drain_all_record_ids(sch);
    ASSERT_EQ(r.total, 9);

    // Verify each drained packet's bucket based on its stream's priority.
    for (std::size_t i = 0; i < 9; ++i) {
        const std::uint32_t sid = r.stream_id_order[i];
        std::uint32_t bucket_rank = 99;
        if (sid == (kStreamHighA & 0xFF))   bucket_rank = 0;
        else if (sid == (kStreamNormalA & 0xFF)) bucket_rank = 1;
        else if (sid == (kStreamLowA & 0xFF))    bucket_rank = 2;
        else ADD_FAILURE() << "unknown stream id at position " << i << ": " << sid;
        // Position 0..2: bucket 0 (High)
        // Position 3..5: bucket 1 (Normal)
        // Position 6..8: bucket 2 (Low)
        if (i < 3)  EXPECT_EQ(bucket_rank, 0u) << "position " << i << " should be High";
        else if (i < 6) EXPECT_EQ(bucket_rank, 1u) << "position " << i << " should be Normal";
        else EXPECT_EQ(bucket_rank, 2u) << "position " << i << " should be Low";
    }
}

/** Normal bucket emitted only after High is exhausted. */
TEST(StreamPriority, NormalEmittedAfterHighExhausted) {
    Scheduler sch{};
    auto dst = make_addr(1);

    enqueue_for_stream_with_id(sch, kStreamHighA,   StreamPriority::High,   dst);
    enqueue_for_stream_with_id(sch, kStreamNormalA, StreamPriority::Normal, dst);
    enqueue_for_stream_with_id(sch, kStreamHighA,   StreamPriority::High,   dst);
    enqueue_for_stream_with_id(sch, kStreamNormalA, StreamPriority::Normal, dst);

    auto r = drain_all_record_ids(sch);
    ASSERT_EQ(r.total, 4);
    EXPECT_EQ(r.stream_id_order[0], kStreamHighA   & 0xFF);  // High
    EXPECT_EQ(r.stream_id_order[1], kStreamHighA   & 0xFF);  // High
    EXPECT_EQ(r.stream_id_order[2], kStreamNormalA & 0xFF);  // Normal
    EXPECT_EQ(r.stream_id_order[3], kStreamNormalA & 0xFF);  // Normal
}

/** Low bucket emitted only after High and Normal are exhausted. */
TEST(StreamPriority, LowEmittedOnlyAfterHighAndNormalExhausted) {
    Scheduler sch{};
    auto dst = make_addr(1);

    enqueue_for_stream_with_id(sch, kStreamLowA,    StreamPriority::Low,    dst);
    enqueue_for_stream_with_id(sch, kStreamNormalA, StreamPriority::Normal, dst);
    enqueue_for_stream_with_id(sch, kStreamHighA,   StreamPriority::High,   dst);
    enqueue_for_stream_with_id(sch, kStreamLowA,    StreamPriority::Low,    dst);

    auto r = drain_all_record_ids(sch);
    ASSERT_EQ(r.total, 4);
    EXPECT_EQ(r.stream_id_order[0], kStreamHighA   & 0xFF);  // High
    EXPECT_EQ(r.stream_id_order[1], kStreamNormalA & 0xFF);  // Normal
    EXPECT_EQ(r.stream_id_order[2], kStreamLowA    & 0xFF);  // Low
    EXPECT_EQ(r.stream_id_order[3], kStreamLowA    & 0xFF);  // Low
}

// ---------------------------------------------------------------------------
// Scenario C — Dynamic priority change: promote Low → High
// ---------------------------------------------------------------------------

/** Promoting a stream from Low to High causes it to preempt Normal/Low
 *  on the very next drain call. */
TEST(StreamPriority, DynamicPromoteLowToHighSwitchesOrder) {
    Scheduler sch{};
    auto dst = make_addr(1);

    // Stream kStreamLowA = Low, kStreamNormalA = Normal
    enqueue_for_stream_with_id(sch, kStreamLowA,    StreamPriority::Low,    dst);
    enqueue_for_stream_with_id(sch, kStreamNormalA, StreamPriority::Normal, dst);

    // Baseline: Normal should come before Low
    {
        auto r = drain_all_record_ids(sch);
        ASSERT_EQ(r.total, 2);
        EXPECT_EQ(r.stream_id_order[0], kStreamNormalA & 0xFF);
        EXPECT_EQ(r.stream_id_order[1], kStreamLowA    & 0xFF);
    }

    // Re-enqueue one packet for each stream (at their CURRENT priorities)
    enqueue_for_stream_with_id(sch, kStreamLowA,    StreamPriority::Low,    dst);
    enqueue_for_stream_with_id(sch, kStreamNormalA, StreamPriority::Normal, dst);

    // Promote kStreamLowA from Low → High
    sch.set_stream_priority(kStreamLowA, StreamPriority::High);

    // Confirm the priority change took effect
    EXPECT_EQ(sch.get_stream_priority(kStreamLowA), StreamPriority::High);

    // Now kStreamLowA (High) should preempt kStreamNormalA (Normal)
    {
        auto r = drain_all_record_ids(sch);
        ASSERT_EQ(r.total, 2);
        EXPECT_EQ(r.stream_id_order[0], kStreamLowA    & 0xFF);  // High
        EXPECT_EQ(r.stream_id_order[1], kStreamNormalA & 0xFF);  // Normal
    }
}

/** Demoting a stream from High to Low causes it to be emitted last. */
TEST(StreamPriority, DynamicDemoteHighToLow) {
    Scheduler sch{};
    auto dst = make_addr(1);

    enqueue_for_stream_with_id(sch, kStreamHighA,   StreamPriority::High,   dst);
    enqueue_for_stream_with_id(sch, kStreamNormalA, StreamPriority::Normal, dst);

    // Baseline: High first
    {
        auto r = drain_all_record_ids(sch);
        ASSERT_EQ(r.total, 2);
        EXPECT_EQ(r.stream_id_order[0], kStreamHighA   & 0xFF);
        EXPECT_EQ(r.stream_id_order[1], kStreamNormalA & 0xFF);
    }

    // Re-enqueue and demote kStreamHighA from High → Low
    enqueue_for_stream_with_id(sch, kStreamHighA,   StreamPriority::High,   dst);
    enqueue_for_stream_with_id(sch, kStreamNormalA, StreamPriority::Normal, dst);
    sch.set_stream_priority(kStreamHighA, StreamPriority::Low);

    // Confirm the priority change took effect
    EXPECT_EQ(sch.get_stream_priority(kStreamHighA), StreamPriority::Low);

    // Now kStreamHighA (Low) should come after kStreamNormalA (Normal)
    {
        auto r = drain_all_record_ids(sch);
        ASSERT_EQ(r.total, 2);
        EXPECT_EQ(r.stream_id_order[0], kStreamNormalA & 0xFF);  // Normal
        EXPECT_EQ(r.stream_id_order[1], kStreamHighA   & 0xFF);  // Low (was High)
    }
}

// ---------------------------------------------------------------------------
// Stream priority get/set
// ---------------------------------------------------------------------------

TEST(StreamPriority, DefaultPriorityIsNormal) {
    Scheduler sch{};
    EXPECT_EQ(sch.get_stream_priority(999u), StreamPriority::Normal);
}

TEST(StreamPriority, SetAndGetStreamPriority) {
    Scheduler sch{};
    sch.set_stream_priority(42u, StreamPriority::High);
    EXPECT_EQ(sch.get_stream_priority(42u), StreamPriority::High);

    sch.set_stream_priority(42u, StreamPriority::Low);
    EXPECT_EQ(sch.get_stream_priority(42u), StreamPriority::Low);

    sch.set_stream_priority(42u, StreamPriority::Normal);
    EXPECT_EQ(sch.get_stream_priority(42u), StreamPriority::Normal);
}

TEST(StreamPriority, MultipleStreamsIndependentPriorities) {
    Scheduler sch{};
    sch.set_stream_priority(1u, StreamPriority::High);
    sch.set_stream_priority(2u, StreamPriority::Normal);
    sch.set_stream_priority(3u, StreamPriority::Low);

    EXPECT_EQ(sch.get_stream_priority(1u), StreamPriority::High);
    EXPECT_EQ(sch.get_stream_priority(2u), StreamPriority::Normal);
    EXPECT_EQ(sch.get_stream_priority(3u), StreamPriority::Low);
    EXPECT_EQ(sch.get_stream_priority(999u), StreamPriority::Normal);  // unseen
}

// ---------------------------------------------------------------------------
// Helpers and edge cases
// ---------------------------------------------------------------------------

TEST(StreamPriority, EmptyDrainReturnsZero) {
    Scheduler sch{};
    EXPECT_EQ(sch.select_next_packet(10, nullptr), 0);
    EXPECT_EQ(sch.select_next_packet(0, nullptr), 0);
    EXPECT_EQ(sch.select_next_packet(-1, nullptr), 0);
}

TEST(StreamPriority, ResetClearsStreamBucketsAndMap) {
    Scheduler sch{};
    auto dst = make_addr(1);
    sch.set_stream_priority(kStreamHighA, StreamPriority::High);
    sch.set_stream_priority(kStreamLowA,  StreamPriority::Low);
    enqueue_for_stream_with_id(sch, kStreamHighA, StreamPriority::High, dst);
    enqueue_for_stream_with_id(sch, kStreamLowA,  StreamPriority::Low,  dst);

    sch.reset();

    EXPECT_EQ(sch.select_next_packet(10, nullptr), 0);
    EXPECT_EQ(sch.get_stream_priority(kStreamHighA), StreamPriority::Normal);
    EXPECT_EQ(sch.get_stream_priority(kStreamLowA),  StreamPriority::Normal);
}

TEST(StreamPriority, StreamPriorityHelpers) {
    EXPECT_EQ(stream_priority_rank(StreamPriority::High),   0u);
    EXPECT_EQ(stream_priority_rank(StreamPriority::Normal), 1u);
    EXPECT_EQ(stream_priority_rank(StreamPriority::Low),    2u);
}

TEST(StreamPriority, EnqueueForStreamCopiesData) {
    Scheduler sch{};
    auto dst = make_addr(1);

    // enqueue_for_stream (copy) variant
    sch.set_stream_priority(7u, StreamPriority::High);
    sch.enqueue_for_stream(7u, nimrtc::core::ByteSpan{kPayload}, dst);

    std::vector<std::uint8_t> out;
    sch.select_next_packet(1, [&](sched::Priority, core::ByteSpan d) -> bool {
        out.assign(d.data(), d.data() + d.size());
        return true;
    });

    ASSERT_EQ(out.size(), kPayload.size());
    EXPECT_EQ(out[0], kPayload[0]);  // bucket marker: High=0
    EXPECT_EQ(out[1], kPayload[1]);
}

// ---------------------------------------------------------------------------
// C ABI wrapper smoke test
// ---------------------------------------------------------------------------

TEST(StreamPriority, CABiCreateAndDestroy) {
    auto* h = sched_create(nullptr);
    ASSERT_NE(h, nullptr);
    sched_destroy(h);
}

TEST(StreamPriority, CABiSetAndGetPriority) {
    auto* h = sched_create(nullptr);
    ASSERT_NE(h, nullptr);

    sched_set_stream_priority(h, 5u, StreamPriority::High);
    EXPECT_EQ(sched_get_stream_priority(h, 5u), StreamPriority::High);

    sched_set_stream_priority(h, 5u, StreamPriority::Low);
    EXPECT_EQ(sched_get_stream_priority(h, 5u), StreamPriority::Low);

    sched_destroy(h);
}

TEST(StreamPriority, CABiEnqueueAndDrain) {
    auto* h = sched_create(nullptr);
    ASSERT_NE(h, nullptr);

    sched_set_stream_priority(h, 3u, StreamPriority::High);

    std::vector<std::uint8_t> pkt = {0x01, 0x02, 0x03};
    sched_enqueue_stream_owned(h, 3u, pkt.data(), pkt.size(), nullptr);

    int count = sched_select_next_packet(h, 10, nullptr, nullptr);
    EXPECT_EQ(count, 1);

    sched_destroy(h);
}

}  // namespace
