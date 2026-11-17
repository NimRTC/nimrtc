/**
 * @file tests/test_core_stats_registry.cpp
 * @brief The core statistics registry (plan Track S / S1, design plan
 *        DS-1): the named, snapshot-able view of a session's counters.
 *
 * The engine-side test (`tests/test_engine_stats_snapshot.cpp`) pins the
 * *behaviour* a host depends on: the delta between two readings is the
 * increment that happened in between, and the timestamp distinguishes
 * "the same instant" from "two different instants".
 *
 * This file pins the **type** those behaviours rest on.  The registry
 * is the piece every layer will speak, so its contract has to be
 * exact:
 *
 *   1. **Declaration** — an entry is declared once; a second
 *      declaration keeps the first semantics.
 *   2. **Semantics** — a counter is diffable, a gauge is not, and the
 *      delta API refuses to mix them up.  This is the classic
 *      statistics bug (subtracting two EMA bitrates and calling the
 *      result a rate), and the refusal is the whole point.
 *   3. **Sequencing** — the sequence number increases by one per
 *      reading and orders two readings even when their timestamps are
 *      identical (a coarse clock).
 *   4. **Ownership** — a snapshot outlives the registry that produced
 *      it, so a host may keep it as a baseline.
 *   5. **Isolation** — the registry is an instance, not a singleton
 *      (Layout Invariant 6: no global mutable state).
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <nimrtc/core/stats_registry.hpp>

namespace {

using nimrtc::core::StatDeltaEntry;
using nimrtc::core::StatEntry;
using nimrtc::core::StatSemantics;
using nimrtc::core::StatSnapshot;
using nimrtc::core::StatsDelta;
using nimrtc::core::StatsRegistry;

// ============================================================================
// 1. Declaration
// ============================================================================

TEST(CoreStatsRegistry, AnUndeclaredEntryHasNoValue) {
    StatsRegistry reg;

    EXPECT_EQ(reg.entry_count(), 0u);
    EXPECT_EQ(reg.value_of("", "video_frames_sent"), 0);

    // Setting an entry that was never declared must fail loudly rather
    // than silently create it (a typo in an entry name would otherwise
    // hide forever).
    EXPECT_FALSE(reg.set("", "video_frames_sent", 42));
    EXPECT_EQ(reg.value_of("", "video_frames_sent"), 0);
}

TEST(CoreStatsRegistry, DeclaringAnEntryTwiceKeepsTheFirstSemantics) {
    StatsRegistry reg;

    EXPECT_TRUE(reg.declare("", "frames", StatSemantics::kCumulative));
    EXPECT_EQ(reg.entry_count(), 1u);

    // The second declaration is refused: the semantics of an existing
    // entry must not change under a host's feet.
    EXPECT_FALSE(reg.declare("", "frames", StatSemantics::kGauge));
    EXPECT_EQ(reg.entry_count(), 1u);

    // The entry is still a counter.
    reg.set("", "frames", 10);
    const auto snap = reg.snapshot(100);
    StatEntry e;
    ASSERT_TRUE(snap.find("", "frames", e));
    EXPECT_EQ(e.semantics, StatSemantics::kCumulative);
}

TEST(CoreStatsRegistry, GroupsSeparateEntriesWithTheSameName) {
    StatsRegistry reg;

    EXPECT_TRUE(reg.declare("", "packets", StatSemantics::kCumulative));
    EXPECT_TRUE(reg.declare("inbound-rtp", "packets", StatSemantics::kCumulative));
    EXPECT_EQ(reg.entry_count(), 2u);

    reg.set("", "packets", 10);
    reg.set("inbound-rtp", "packets", 20);

    const auto snap = reg.snapshot(100);
    std::int64_t v = 0;
    ASSERT_TRUE(snap.value_of("", "packets", v));
    EXPECT_EQ(v, 10);
    ASSERT_TRUE(snap.value_of("inbound-rtp", "packets", v));
    EXPECT_EQ(v, 20);
}

TEST(CoreStatsRegistry, TheKeyNamesTheGroupAndTheEntry) {
    StatEntry e;
    e.name = "packets";
    EXPECT_EQ(e.key(), "packets");

    e.group = "inbound-rtp";
    EXPECT_EQ(e.key(), "inbound-rtp/packets");
}

// ============================================================================
// 2. Semantics
// ============================================================================

TEST(CoreStatsRegistry, ACounterIsDiffableAndAGaugeIsNot) {
    StatsRegistry reg;
    reg.declare("", "frames", StatSemantics::kCumulative);
    reg.declare("", "bitrate", StatSemantics::kGauge);

    reg.set("", "frames", 100);
    reg.set("", "bitrate", 50000);
    const auto first = reg.snapshot(1000);

    reg.set("", "frames", 250);
    reg.set("", "bitrate", 80000);
    const auto second = reg.snapshot(2000);

    const auto delta = reg.diff(first.seq(), first.time_us(),
                                [&first](std::string_view g, std::string_view n,
                                         std::int64_t& out) {
                                    return first.value_of(g, n, out);
                                },
                                second.seq(), second.time_us(),
                                [&second](std::string_view g, std::string_view n,
                                          std::int64_t& out) {
                                    return second.value_of(g, n, out);
                                });

    ASSERT_TRUE(delta.valid());

    // A counter's increment is the difference of the two readings.
    std::int64_t frames = 0;
    ASSERT_TRUE(delta.counter_delta("", "frames", frames));
    EXPECT_EQ(frames, 150);

    // A gauge has no increment: the difference of two instantaneous
    // bitrates is not a bitrate.
    std::int64_t bitrate = 0;
    EXPECT_FALSE(delta.counter_delta("", "bitrate", bitrate));

    // The gauge's *value* at the end of the interval is the answer.
    ASSERT_TRUE(delta.gauge_value("", "bitrate", bitrate));
    EXPECT_EQ(bitrate, 80000);

    // And the mirror image: a counter is not a gauge.
    EXPECT_FALSE(delta.gauge_value("", "frames", bitrate));
}

TEST(CoreStatsRegistry, TheDeltaCarriesTheIntervalDuration) {
    StatsRegistry reg;
    reg.declare("", "frames", StatSemantics::kCumulative);

    reg.set("", "frames", 10);
    const auto first = reg.snapshot(1'000'000);

    reg.set("", "frames", 60);
    const auto second = reg.snapshot(3'500'000);

    const auto delta = reg.diff(first.seq(), first.time_us(),
                                [&first](std::string_view g, std::string_view n,
                                         std::int64_t& out) {
                                    return first.value_of(g, n, out);
                                },
                                second.seq(), second.time_us(),
                                [&second](std::string_view g, std::string_view n,
                                          std::int64_t& out) {
                                    return second.value_of(g, n, out);
                                });

    ASSERT_TRUE(delta.valid());
    EXPECT_EQ(delta.duration_us(), 2'500'000);
    EXPECT_EQ(delta.from_seq(), first.seq());
    EXPECT_EQ(delta.to_seq(), second.seq());
    EXPECT_EQ(delta.from_time_us(), 1'000'000);
    EXPECT_EQ(delta.to_time_us(), 3'500'000);
}

TEST(CoreStatsRegistry, ADeltaWithoutABaselineIsInvalid) {
    StatsRegistry reg;
    reg.declare("", "frames", StatSemantics::kCumulative);
    reg.set("", "frames", 10);

    // A default-constructed snapshot means "no baseline": the delta
    // would be measured against a made-up zero, so it is refused.
    const auto delta = reg.delta_since(StatSnapshot{}, 1000);
    EXPECT_FALSE(delta.valid());

    std::int64_t frames = 0;
    EXPECT_FALSE(delta.counter_delta("", "frames", frames));
}

TEST(CoreStatsRegistry, ADeltaWithoutAnEntryOnTheBaselineSideIsInvalid) {
    StatsRegistry reg;
    reg.declare("", "frames", StatSemantics::kCumulative);

    // The baseline knows about one entry, the registry about two.
    // The second entry has no baseline value, so the delta would be
    // measured against a made-up zero.
    StatsRegistry other;
    other.declare("", "frames", StatSemantics::kCumulative);
    other.set("", "frames", 10);
    const auto baseline = other.snapshot(1000);

    reg.set("", "frames", 20);
    reg.declare("", "bytes", StatSemantics::kCumulative);
    reg.set("", "bytes", 100);

    const auto delta = reg.delta_since(baseline, 2000);
    EXPECT_FALSE(delta.valid());
}

// ============================================================================
// 3. Sequencing
// ============================================================================

TEST(CoreStatsRegistry, TheSequenceNumberIncreasesByOnePerReading) {
    StatsRegistry reg;
    reg.declare("", "frames", StatSemantics::kCumulative);

    EXPECT_EQ(reg.snapshot_seq(), 0u);

    const auto a = reg.snapshot(1000);
    const auto b = reg.snapshot(1000);
    const auto c = reg.snapshot(1000);

    EXPECT_EQ(a.seq(), 1u);
    EXPECT_EQ(b.seq(), 2u);
    EXPECT_EQ(c.seq(), 3u);
    EXPECT_EQ(reg.snapshot_seq(), 3u);
}

TEST(CoreStatsRegistry, TheSequenceNumberOrdersTwoReadingsOfTheSameInstant) {
    StatsRegistry reg;
    reg.declare("", "frames", StatSemantics::kCumulative);

    // Two readings taken inside the same microsecond: the timestamps
    // cannot order them, but the sequence number can.
    const auto a = reg.snapshot(5000);
    const auto b = reg.snapshot(5000);

    EXPECT_EQ(a.time_us(), b.time_us());
    EXPECT_LT(a.seq(), b.seq());
}

TEST(CoreStatsRegistry, AdvanceOrdersWithoutCopyingTheValues) {
    StatsRegistry reg;
    reg.declare("", "frames", StatSemantics::kCumulative);

    EXPECT_EQ(reg.advance(1000), 1u);
    EXPECT_EQ(reg.advance(2000), 2u);
    EXPECT_EQ(reg.snapshot_seq(), 2u);
}

// ============================================================================
// 4. Ownership
// ============================================================================

TEST(CoreStatsRegistry, ASnapshotOutlivesTheRegistry) {
    StatSnapshot kept;
    {
        StatsRegistry reg;
        reg.declare("", "frames", StatSemantics::kCumulative);
        reg.set("", "frames", 4242);
        kept = reg.snapshot(1000);
    }

    // The registry is gone; the reading is still readable.
    std::int64_t v = 0;
    ASSERT_TRUE(kept.value_of("", "frames", v));
    EXPECT_EQ(v, 4242);
    EXPECT_EQ(kept.seq(), 1u);
    EXPECT_EQ(kept.time_us(), 1000);
}

// ============================================================================
// 5. Isolation
// ============================================================================

TEST(CoreStatsRegistry, TwoRegistriesAreIndependent) {
    StatsRegistry a;
    StatsRegistry b;

    a.declare("", "frames", StatSemantics::kCumulative);
    a.set("", "frames", 10);

    // The second registry knows nothing about the first's entry.
    EXPECT_EQ(b.entry_count(), 0u);
    EXPECT_EQ(b.value_of("", "frames"), 0);
    EXPECT_FALSE(b.set("", "frames", 99));
}

// ============================================================================
// 6. The index fast path
// ============================================================================

TEST(CoreStatsRegistry, TheIndexIsStableAndAddressesTheEntry) {
    StatsRegistry reg;
    reg.declare("", "a", StatSemantics::kCumulative);
    reg.declare("", "b", StatSemantics::kGauge);

    const std::size_t ia = reg.index_of("", "a");
    const std::size_t ib = reg.index_of("", "b");
    ASSERT_NE(ia, static_cast<std::size_t>(-1));
    ASSERT_NE(ib, static_cast<std::size_t>(-1));
    EXPECT_NE(ia, ib);

    reg.set_at(ia, 11);
    reg.set_at(ib, 22);
    EXPECT_EQ(reg.value_of("", "a"), 11);
    EXPECT_EQ(reg.value_of("", "b"), 22);
    EXPECT_EQ(reg.value_at(ia), 11);

    // An unknown entry has no index.
    EXPECT_EQ(reg.index_of("", "nope"), static_cast<std::size_t>(-1));
}

}  // namespace
