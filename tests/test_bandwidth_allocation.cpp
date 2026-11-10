/**
 * @file tests/test_bandwidth_allocation.cpp
 * @brief F8 — the per-stream bandwidth share rule.
 *
 * The rule itself is an engine-private, header-only pure function
 * (`src/engine/src/bandwidth_allocation.hpp`).  This test is the unit
 * test the video plan asks for: it exercises the split directly, without
 * an engine, a transport or a clock — the rule is a pure function of
 * (estimate, demand list), so that is exactly how it is tested.
 *
 * The coverage below is organised around the rule's parts:
 *
 *   - the reserved stream's clamped share,
 *   - the hard floor, and the deficit the floor can cause,
 *   - the ceiling, and the rule that a ceiling above the floor wins,
 *   - the elastic pool and its proportional (Hamilton) split,
 *   - the edge cases (no estimate, zero weight, empty demand list).
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "bandwidth_allocation.hpp"

namespace {

using nimrtc::engine::detail::allocate_bandwidth;
using nimrtc::engine::detail::BandwidthAllocation;
using nimrtc::engine::detail::BandwidthDemand;

/** A reserved stream (the audio): a share of the estimate, a floor and a
 *  ceiling. */
BandwidthDemand reserved(double share, std::uint32_t floor_bps,
                         std::uint32_t ceiling_bps = 0) {
    BandwidthDemand d;
    d.reserved = true;
    d.share = share;
    d.floor_bps = floor_bps;
    d.ceiling_bps = ceiling_bps;
    return d;
}

/** An elastic stream (the video): a weight in the pool split, a ceiling. */
BandwidthDemand elastic(std::uint32_t weight, std::uint32_t ceiling_bps = 0) {
    BandwidthDemand d;
    d.reserved = false;
    d.weight = weight;
    d.ceiling_bps = ceiling_bps;
    return d;
}

// ---------------------------------------------------------------------------
// No estimate
// ---------------------------------------------------------------------------

TEST(BandwidthAllocation, NoEstimateAllocatesNothing) {
    const std::vector<BandwidthDemand> demands{reserved(0.1, 48'000),
                                                elastic(1)};
    const BandwidthAllocation out = allocate_bandwidth(0, demands);

    ASSERT_EQ(out.targets.size(), demands.size());
    EXPECT_EQ(out.targets[0], 0u);
    EXPECT_EQ(out.targets[1], 0u);
    EXPECT_EQ(out.reserved_bps, 0u);
    EXPECT_EQ(out.elastic_pool_bps, 0u);
    // No estimate is not a deficit: it is the absence of an opinion.
    EXPECT_FALSE(out.deficit);
}

TEST(BandwidthAllocation, EmptyDemandListSpendsNothing) {
    const BandwidthAllocation out = allocate_bandwidth(1'000'000, {});
    EXPECT_TRUE(out.targets.empty());
    EXPECT_EQ(out.reserved_bps, 0u);
    // Nothing was reserved, so the whole estimate is left for the
    // elastic streams — and there are none, so none of it is spent.
    EXPECT_EQ(out.elastic_pool_bps, 1'000'000u);
    EXPECT_FALSE(out.deficit);
}

// ---------------------------------------------------------------------------
// The reserved stream
// ---------------------------------------------------------------------------

TEST(BandwidthAllocation, ReservedTakesItsShareFirst) {
    const std::vector<BandwidthDemand> demands{reserved(0.1, 0), elastic(1)};
    const BandwidthAllocation out = allocate_bandwidth(1'000'000, demands);

    EXPECT_EQ(out.targets[0], 100'000u);
    EXPECT_EQ(out.reserved_bps, 100'000u);
    EXPECT_EQ(out.elastic_pool_bps, 900'000u);
    EXPECT_EQ(out.targets[1], 900'000u);
    EXPECT_FALSE(out.deficit);
}

TEST(BandwidthAllocation, ReservedShareIsClampedIntoRange) {
    // A share above 1.0 must not hand out more than the estimate.
    const std::vector<BandwidthDemand> over{reserved(2.0, 0), elastic(1)};
    const BandwidthAllocation out_over = allocate_bandwidth(1'000'000, over);
    EXPECT_EQ(out_over.targets[0], 1'000'000u);
    EXPECT_EQ(out_over.elastic_pool_bps, 0u);
    EXPECT_FALSE(out_over.deficit);

    // A share below 0.0 is treated as 0.
    const std::vector<BandwidthDemand> under{reserved(-1.0, 0), elastic(1)};
    const BandwidthAllocation out_under = allocate_bandwidth(1'000'000, under);
    EXPECT_EQ(out_under.targets[0], 0u);
    EXPECT_EQ(out_under.elastic_pool_bps, 1'000'000u);
}

TEST(BandwidthAllocation, ReservedKeepsItsFloorWhenTheEstimateCollapses) {
    const std::vector<BandwidthDemand> demands{reserved(0.1, 48'000),
                                                elastic(1)};
    const BandwidthAllocation out = allocate_bandwidth(100'000, demands);

    // The share would have been 10 kbps; the floor is hard.
    EXPECT_EQ(out.targets[0], 48'000u);
    EXPECT_EQ(out.reserved_bps, 48'000u);
}

TEST(BandwidthAllocation, FloorAboveTheEstimateIsADeficit) {
    const std::vector<BandwidthDemand> demands{reserved(0.1, 48'000),
                                                elastic(1)};
    const BandwidthAllocation out = allocate_bandwidth(10'000, demands);

    EXPECT_TRUE(out.deficit);
    // The reserved stream keeps its floor even in deficit.
    EXPECT_EQ(out.targets[0], 48'000u);
    EXPECT_EQ(out.reserved_bps, 48'000u);
    // The elastic stream gets nothing: the pool is empty.
    EXPECT_EQ(out.elastic_pool_bps, 0u);
    EXPECT_EQ(out.targets[1], 0u);
}

TEST(BandwidthAllocation, ReservedCeilingBoundsTheShare) {
    const std::vector<BandwidthDemand> demands{reserved(0.5, 0, 64'000),
                                                elastic(1)};
    const BandwidthAllocation out = allocate_bandwidth(1'000'000, demands);

    EXPECT_EQ(out.targets[0], 64'000u);
    EXPECT_EQ(out.elastic_pool_bps, 936'000u);
    EXPECT_EQ(out.targets[1], 936'000u);
}

TEST(BandwidthAllocation, CeilingAboveTheFloorWins) {
    // A misconfigured demand: the floor sits above the ceiling.  The
    // ceiling is the outer bound of what the caller said it can use.
    const std::vector<BandwidthDemand> demands{reserved(0.5, 128'000, 64'000),
                                                elastic(1)};
    const BandwidthAllocation out = allocate_bandwidth(1'000'000, demands);

    EXPECT_EQ(out.targets[0], 64'000u);
}

// ---------------------------------------------------------------------------
// The elastic pool
// ---------------------------------------------------------------------------

TEST(BandwidthAllocation, ElasticPoolIsSplitByWeight) {
    const std::vector<BandwidthDemand> demands{reserved(0.1, 0), elastic(1),
                                                elastic(3)};
    const BandwidthAllocation out = allocate_bandwidth(1'000'000, demands);

    // The pool is 900 kbps, split 1:3 → 225 kbps and 675 kbps.
    EXPECT_EQ(out.targets[1], 225'000u);
    EXPECT_EQ(out.targets[2], 675'000u);
    EXPECT_EQ(out.targets[1] + out.targets[2], out.elastic_pool_bps);
}

TEST(BandwidthAllocation, SplitLosesNoBitToIntegerDivision) {
    // 10 bits over three equal weights: 3/3/3 plus one bit to the
    // stream with the largest remainder — the first one, the tie-break
    // is the stream order.
    const std::vector<BandwidthDemand> demands{elastic(1), elastic(1),
                                                elastic(1)};
    const BandwidthAllocation out = allocate_bandwidth(10, demands);

    EXPECT_EQ(out.targets[0], 4u);
    EXPECT_EQ(out.targets[1], 3u);
    EXPECT_EQ(out.targets[2], 3u);
    EXPECT_EQ(out.targets[0] + out.targets[1] + out.targets[2], 10u);
}

TEST(BandwidthAllocation, SplitIsDeterministic) {
    const std::vector<BandwidthDemand> demands{elastic(2), elastic(3),
                                                elastic(5)};
    const BandwidthAllocation a = allocate_bandwidth(1'000'003, demands);
    const BandwidthAllocation b = allocate_bandwidth(1'000'003, demands);

    EXPECT_EQ(a.targets, b.targets);
    EXPECT_EQ(a.targets[0] + a.targets[1] + a.targets[2], a.elastic_pool_bps);
}

TEST(BandwidthAllocation, ZeroWeightTakesNothingAndDoesNotDilute) {
    const std::vector<BandwidthDemand> demands{elastic(0), elastic(1)};
    const BandwidthAllocation out = allocate_bandwidth(1'000, demands);

    EXPECT_EQ(out.targets[0], 0u);
    EXPECT_EQ(out.targets[1], 1'000u);
}

TEST(BandwidthAllocation, AllZeroWeightsLeaveThePoolUnspent) {
    const std::vector<BandwidthDemand> demands{elastic(0), elastic(0)};
    const BandwidthAllocation out = allocate_bandwidth(1'000, demands);

    EXPECT_EQ(out.targets[0], 0u);
    EXPECT_EQ(out.targets[1], 0u);
    EXPECT_EQ(out.elastic_pool_bps, 1'000u);
}

TEST(BandwidthAllocation, ElasticCeilingIsNotRedistributed) {
    // The first stream asks for less than its share; the surplus does
    // not go to the second one.
    const std::vector<BandwidthDemand> demands{elastic(1, 200'000),
                                                elastic(1)};
    const BandwidthAllocation out = allocate_bandwidth(1'000'000, demands);

    EXPECT_EQ(out.targets[0], 200'000u);
    EXPECT_EQ(out.targets[1], 500'000u);
    EXPECT_EQ(out.targets[0] + out.targets[1], 700'000u);
    EXPECT_EQ(out.elastic_pool_bps, 1'000'000u);
}

// ---------------------------------------------------------------------------
// The rule in the shape the engine uses it
// ---------------------------------------------------------------------------

TEST(BandwidthAllocation, AudioAndVideoSplitOfOneEstimate) {
    // The engine's own shape: the audio is reserved at 10 % with a floor,
    // the video is elastic and takes the rest.
    const std::vector<BandwidthDemand> demands{reserved(0.1, 16'000),
                                                elastic(1)};
    const BandwidthAllocation out = allocate_bandwidth(2'000'000, demands);

    EXPECT_EQ(out.targets[0], 200'000u);
    EXPECT_EQ(out.targets[1], 1'800'000u);
    EXPECT_FALSE(out.deficit);
}

TEST(BandwidthAllocation, LargeEstimatesDoNotOverflow) {
    const std::vector<BandwidthDemand> demands{reserved(0.5, 0), elastic(1),
                                                elastic(1)};
    const BandwidthAllocation out = allocate_bandwidth(0xFFFFFFFFu, demands);

    // Half of 0xFFFFFFFF rounds to 2^31 (the .5 rounds up).
    EXPECT_EQ(out.targets[0], 2'147'483'648u);
    EXPECT_EQ(out.targets[1] + out.targets[2], out.elastic_pool_bps);
    EXPECT_FALSE(out.deficit);
}

}  // namespace
