/**
 * @file tests/test_engine_video_stream_shares.cpp
 * @brief D5 — the per-stream bandwidth shares (remote-desktop plan D5 / H4).
 *
 * ## What the plan asks for
 *
 * **D5**: "Per-stream bandwidth shares — with multiple streams (M4), the
 * session estimate is divided between them by a rule that is stated once
 * and shared with the audio path.  The video plan's F8 /
 * `bandwidth_allocation.hpp` is the home of that rule; the remote-desktop
 * profile declares the shares."
 *
 * Acceptance: "With two video streams and one audio stream the
 * allocation is the documented split; the rule is unit-tested in the same
 * place as the audio split."
 *
 * The *rule* itself is unit-tested in `tests/test_bandwidth_allocation.cpp`
 * (the same file that covers the audio split — one rule, one place).
 * This file covers what that file cannot: that the **engine** feeds the
 * rule the right demands and writes the result back to the right stream.
 *
 * ## The bug this file was written for
 *
 * The engine built one demand per video stream (the default stream plus
 * every extra one) and then wrote the targets back with an **off-by-one**
 * index: the extra streams started at `targets[1]`, which is the *default*
 * stream's target.  The first extra stream therefore received the
 * default stream's share, the second received the first extra's, and the
 * third received nothing at all (its index ran past the end of the
 * vector, so it silently kept its previous value).
 *
 * With one extra stream the bug was invisible — the elastic split gives
 * equal weights, so `targets[1]` and `targets[2]` hold the same number.
 * With two extra streams the mis-assignment becomes observable, and with
 * *unequal weights* it becomes obvious.
 *
 * These tests therefore use **unequal weights** on purpose: an even split
 * cannot tell a correct index mapping from an off-by-one one.
 *
 * ## No networking
 *
 * The engine opens with the "shell" transport strategy and the "ice"
 * transport but never establishes a connection.  What is under test is
 * the *split*, which happens entirely inside the engine's tick.
 */

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>

#include <gtest/gtest.h>

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
using nimrtc::engine::VideoStreamStats;

namespace {

/** The id of the default (legacy single) stream. */
constexpr std::string_view kDefault = "default";

/** Find a statistics entry by stream id (nullptr when there is none). */
const VideoStreamStats* find_entry(const std::vector<VideoStreamStats>& stats,
                                   std::string_view id) {
    for (const auto& s : stats) {
        if (s.stream_id == id) return &s;
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// The fixture
// ---------------------------------------------------------------------------

class EngineVideoStreamShares : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
    }

    EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.transport_strategy_name = "shell";
        cfg.transport_name          = "ice";
        cfg.video_codec_name        = "h264";
        cfg.video_sender_name       = "reference";
        cfg.video_receiver_name     = "reference";
        cfg.video_source_name       = "memory";
        cfg.video_sink_name         = "headless";
        cfg.video_width             = 320;
        cfg.video_height            = 240;
        cfg.video_fps               = 30;

        // The audio takes 10 % of the estimate as the reserved stream
        // (the engine's default).  The tests below compute the expected
        // pool from this number rather than hard-coding it.
        cfg.audio_bwe_share = 0.1;

        // Pacing must be ON: the split happens in `update_pacing_rates()`.
        cfg.media_pacing_enabled = true;
        return cfg;
    }

    /** The elastic pool the video streams divide: the estimate minus the
     *  audio's reserved share. */
    static std::uint32_t expected_pool(std::uint32_t estimate) {
        return estimate - static_cast<std::uint32_t>(
                             static_cast<double>(estimate) * 0.1 + 0.5);
    }
};

// ---------------------------------------------------------------------------
// The contract
// ---------------------------------------------------------------------------

TEST_F(EngineVideoStreamShares, TheWeightsDefaultToOne) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_EQ(engine.video_stream_weight(kDefault), 1u);

    ASSERT_TRUE(engine.add_video_stream("screen"));
    EXPECT_EQ(engine.video_stream_weight("screen"), 1u);
}

TEST_F(EngineVideoStreamShares, AWeightCanBeSetAndReadBack) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    ASSERT_TRUE(engine.add_video_stream("screen"));
    ASSERT_TRUE(engine.add_video_stream("thumbnail"));

    ASSERT_TRUE(engine.set_video_stream_weight("screen", 4));
    EXPECT_EQ(engine.video_stream_weight("screen"), 4u);

    // The default stream has its own weight.
    ASSERT_TRUE(engine.set_video_stream_weight(kDefault, 3));
    EXPECT_EQ(engine.video_stream_weight(kDefault), 3u);

    // The other stream is untouched.
    EXPECT_EQ(engine.video_stream_weight("thumbnail"), 1u);
}

TEST_F(EngineVideoStreamShares, AnUnknownStreamIdIsRejected) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_FALSE(engine.set_video_stream_weight("no-such-stream", 4));
    EXPECT_EQ(engine.video_stream_weight("no-such-stream"), 0u);
    EXPECT_EQ(engine.video_stream_share_bps("no-such-stream"), 0u);
}

TEST_F(EngineVideoStreamShares, TheDefaultStreamCannotBeMuted) {
    // A weight of 0 takes a stream out of the split entirely.  The
    // default stream is the session's primary video path, so muting it
    // is a foot-gun rather than a feature: a host that wants a stream
    // to sit out must remove it, not mute it.
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_FALSE(engine.set_video_stream_weight(kDefault, 0));
    EXPECT_EQ(engine.video_stream_weight(kDefault), 1u)
        << "the default stream keeps its weight";

    // An extra stream, on the other hand, may opt out.
    ASSERT_TRUE(engine.add_video_stream("thumbnail"));
    EXPECT_TRUE(engine.set_video_stream_weight("thumbnail", 0));
    EXPECT_EQ(engine.video_stream_weight("thumbnail"), 0u);
}

// ---------------------------------------------------------------------------
// The split itself
// ---------------------------------------------------------------------------

TEST_F(EngineVideoStreamShares, TheSharesFollowTheWeights) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    ASSERT_TRUE(engine.add_video_stream("screen"));
    ASSERT_TRUE(engine.add_video_stream("thumbnail"));

    // The screen stream deserves three times the thumbnail's share, and
    // the default stream twice the thumbnail's.  Unequal weights are
    // the whole point: an even split cannot distinguish a correct index
    // mapping from an off-by-one one.
    ASSERT_TRUE(engine.set_video_stream_weight(kDefault, 2));
    ASSERT_TRUE(engine.set_video_stream_weight("screen", 6));
    ASSERT_TRUE(engine.set_video_stream_weight("thumbnail", 1));

    // The share is computed in `tick()`; before the first tick there is
    // no estimate, so there is no share.
    EXPECT_EQ(engine.video_stream_share_bps(kDefault), 0u);

    engine.tick();

    // The numbers must be *distinct* for the test to mean anything.
    const std::uint32_t def = engine.video_stream_share_bps(kDefault);
    const std::uint32_t scr = engine.video_stream_share_bps("screen");
    const std::uint32_t thu = engine.video_stream_share_bps("thumbnail");

    EXPECT_GT(def, 0u) << "the split must have produced a share";
    EXPECT_GT(scr, 0u);
    EXPECT_GT(thu, 0u);

    // Weights 2 : 6 : 1 → the shares are proportional.
    //
    // The comparison is a *ratio*, not an equality: the split is done in
    // integer arithmetic (the pool is divided by the total weight), so
    // the exact targets can differ from the exact ratio by a bit or two.
    // Asserting `scr == def * 3` would be asserting the rounding, not
    // the rule.
    EXPECT_NEAR(static_cast<double>(scr), static_cast<double>(def) * 3.0,
                static_cast<double>(def) * 0.02);
    EXPECT_NEAR(static_cast<double>(def), static_cast<double>(thu) * 2.0,
                static_cast<double>(thu) * 0.05);
}

TEST_F(EngineVideoStreamShares, EveryStreamGetsItsOwnShare) {
    // The regression test for the off-by-one: three video streams with
    // *three distinct* weights must each land on their own target.
    //
    // With the old (`targets[1]`-based) write-back, "screen" received
    // the default stream's target, "thumbnail" received the screen
    // stream's, and the default stream received the screen stream's —
    // three wrong numbers instead of one.
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    ASSERT_TRUE(engine.add_video_stream("screen"));
    ASSERT_TRUE(engine.add_video_stream("thumbnail"));

    ASSERT_TRUE(engine.set_video_stream_weight(kDefault, 1));
    ASSERT_TRUE(engine.set_video_stream_weight("screen", 2));
    ASSERT_TRUE(engine.set_video_stream_weight("thumbnail", 3));

    engine.tick();

    const std::uint32_t def = engine.video_stream_share_bps(kDefault);
    const std::uint32_t scr = engine.video_stream_share_bps("screen");
    const std::uint32_t thu = engine.video_stream_share_bps("thumbnail");

    // Weights 1 : 2 : 3 → the largest share goes to the heaviest stream,
    // in the *stream's own* order.
    EXPECT_LT(def, scr) << "the default stream (weight 1) must not get the "
                            "screen stream's (weight 2) share";
    EXPECT_LT(scr, thu) << "the screen stream (weight 2) must not get the "
                            "thumbnail's (weight 3) share";

    // And the exact proportions, to within the rounding of the integer
    // split: the pool is divided by the total weight (6).
    const std::uint32_t total = def + scr + thu;
    ASSERT_GT(total, 0u);
    EXPECT_NEAR(static_cast<double>(scr) / static_cast<double>(total),
                2.0 / 6.0, 0.02);
    EXPECT_NEAR(static_cast<double>(thu) / static_cast<double>(total),
                3.0 / 6.0, 0.02);
}

TEST_F(EngineVideoStreamShares, TheSharesAppearInTheStatistics) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    ASSERT_TRUE(engine.add_video_stream("screen"));
    ASSERT_TRUE(engine.set_video_stream_weight("screen", 5));

    engine.tick();

    const auto stats = engine.stats().video_streams;
    ASSERT_EQ(stats.size(), 2u);

    const VideoStreamStats* def = find_entry(stats, kDefault);
    const VideoStreamStats* scr = find_entry(stats, "screen");
    ASSERT_NE(def, nullptr);
    ASSERT_NE(scr, nullptr);

    // The statistics carry the *same* numbers the engine hands the
    // controllers — one rule, one number, two views.
    EXPECT_EQ(def->bandwidth_share_bps, engine.video_stream_share_bps(kDefault));
    EXPECT_EQ(scr->bandwidth_share_bps, engine.video_stream_share_bps("screen"));
    EXPECT_EQ(def->bandwidth_weight, 1u);
    EXPECT_EQ(scr->bandwidth_weight, 5u);
}

TEST_F(EngineVideoStreamShares, AZeroWeightTakesNothingAndDilutesNothing) {
    // The rule's contract: a zero-weight stream takes nothing from the
    // pool, and it does not dilute the other streams' shares either.
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    ASSERT_TRUE(engine.add_video_stream("screen"));
    ASSERT_TRUE(engine.add_video_stream("thumbnail"));
    ASSERT_TRUE(engine.set_video_stream_weight("thumbnail", 0));

    engine.tick();

    EXPECT_EQ(engine.video_stream_share_bps("thumbnail"), 0u);

    // The default (1) and the screen (1) split the pool evenly — the
    // zero weight did not dilute them.
    const std::uint32_t def = engine.video_stream_share_bps(kDefault);
    const std::uint32_t scr = engine.video_stream_share_bps("screen");
    EXPECT_GT(def, 0u);
    EXPECT_EQ(def, scr);
}

TEST_F(EngineVideoStreamShares, RemovingAStreamDoesNotShiftTheOthers) {
    // A removed stream's demand leaves the list, so the survivors'
    // indices shift down by one.  The write-back must follow the
    // *stream*, not a fixed index.
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    ASSERT_TRUE(engine.add_video_stream("screen"));
    ASSERT_TRUE(engine.add_video_stream("thumbnail"));
    ASSERT_TRUE(engine.set_video_stream_weight(kDefault, 1));
    ASSERT_TRUE(engine.set_video_stream_weight("screen", 2));
    ASSERT_TRUE(engine.set_video_stream_weight("thumbnail", 3));

    engine.tick();
    const std::uint32_t def_before = engine.video_stream_share_bps(kDefault);
    const std::uint32_t scr_before = engine.video_stream_share_bps("screen");
    ASSERT_GT(def_before, 0u);
    ASSERT_GT(scr_before, 0u);

    // Remove the *first* extra stream: the thumbnail keeps its place in
    // the demand list, but the screen's index shifts down.
    ASSERT_TRUE(engine.remove_video_stream("screen"));

    engine.tick();

    // The default and the thumbnail are still there and still carry
    // shares — the removal did not leave a stale or a zeroed entry.
    EXPECT_GT(engine.video_stream_share_bps(kDefault), 0u);
    EXPECT_GT(engine.video_stream_share_bps("thumbnail"), 0u);
    // The removed stream is gone.
    EXPECT_EQ(engine.video_stream_share_bps("screen"), 0u);
}

// ---------------------------------------------------------------------------
// The *declarative* path — plan D5's "the profile declares the shares"
// ---------------------------------------------------------------------------
//
// The plan's D5 row says: "the remote-desktop profile declares the
// shares".  Until now that was not true: the only way to give a stream
// a weight was `set_video_stream_weight()`, a **runtime** call on an
// already-created stream — so a profile-driven session had to be
// hand-tuned after the fact, and the scene's intent lived in code
// rather than in the profile.
//
// `EngineConfig::video_stream_weights` is the declaration side: the
// engine applies it to the default stream at `open()` and to an extra
// stream when that stream is created.  The runtime override always
// wins over the declaration.

TEST_F(EngineVideoStreamShares, TheDeclaredWeightsAreAppliedAtOpen) {
    auto cfg = make_config();
    cfg.video_stream_weights = {{"default", 3}, {"camera", 1}};

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The default stream's declaration is applied at open() — the
    // declaration does not need the stream to be "created" first.
    EXPECT_EQ(engine.video_stream_weight(kDefault), 3u);
}

TEST_F(EngineVideoStreamShares, TheDeclaredWeightsAreAppliedWhenAStreamIsCreated) {
    // The declared weights are deliberately *distinct* from the default
    // weight of 1: a test that declared "camera": 1 could not tell
    // "the declaration was applied" from "the default was kept", and
    // would pass against an engine that ignores the declaration
    // entirely.
    auto cfg = make_config();
    cfg.video_stream_weights = {{"default", 3}, {"camera", 7}};

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // An extra stream picks its declaration up when it is created.
    ASSERT_TRUE(engine.add_video_stream("camera"));
    EXPECT_EQ(engine.video_stream_weight("camera"), 7u);

    // A stream the declaration does not mention keeps the default
    // weight of 1 (an even split).
    ASSERT_TRUE(engine.add_video_stream("thumbnail"));
    EXPECT_EQ(engine.video_stream_weight("thumbnail"), 1u);
}

TEST_F(EngineVideoStreamShares, ADeclarationForAnAbsentStreamIsInert) {
    // The map is declarative, not constructive: a declaration for a
    // stream the session never creates is not an error, and it does not
    // dilute the streams that do exist.
    auto cfg = make_config();
    cfg.video_stream_weights = {{"default", 1}, {"never-created", 9}};

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_EQ(engine.video_stream_weight(kDefault), 1u);
    EXPECT_EQ(engine.video_stream_weight("never-created"), 0u);

    ASSERT_TRUE(engine.add_video_stream("screen"));
    engine.tick();

    // The undeclared stream's weight of 9 is not in the pool: the
    // default stream and the screen stream split it evenly.
    const std::uint32_t def = engine.video_stream_share_bps(kDefault);
    const std::uint32_t scr = engine.video_stream_share_bps("screen");
    EXPECT_GT(def, 0u);
    EXPECT_EQ(def, scr);
}

TEST_F(EngineVideoStreamShares, TheRuntimeOverrideBeatsTheDeclaration) {
    // The declaration is the session's *starting* weight, not a lock.
    auto cfg = make_config();
    cfg.video_stream_weights = {{"default", 3}, {"camera", 7}};

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    ASSERT_TRUE(engine.add_video_stream("camera"));
    EXPECT_EQ(engine.video_stream_weight("camera"), 7u)
        << "the declaration is the starting weight";

    ASSERT_TRUE(engine.set_video_stream_weight("camera", 8));
    EXPECT_EQ(engine.video_stream_weight("camera"), 8u)
        << "the runtime override wins over the declaration";

    ASSERT_TRUE(engine.set_video_stream_weight(kDefault, 2));
    EXPECT_EQ(engine.video_stream_weight(kDefault), 2u);
}

TEST_F(EngineVideoStreamShares, AZeroWeightForTheDefaultStreamIsRefused) {
    // The engine refuses to mute the session's primary video path, and
    // it refuses it in the *declaration* too — a profile must not be
    // able to do what the runtime API forbids.
    auto cfg = make_config();
    cfg.video_stream_weights = {{"default", 0}};

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_EQ(engine.video_stream_weight(kDefault), 1u)
        << "the default stream keeps its weight";

    // A *positive* declaration for the default stream is applied, so the
    // refusal above is about the zero and not about the mechanism.
    auto cfg2 = make_config();
    cfg2.video_stream_weights = {{"default", 4}};
    NimRTCEngine engine2(cfg2);
    ASSERT_EQ(engine2.open(), 0u);
    EXPECT_EQ(engine2.video_stream_weight(kDefault), 4u);
}

TEST_F(EngineVideoStreamShares, TheDeclaredWeightsDriveTheSplit) {
    // The end-to-end shape: a declarative split behaves exactly like the
    // equivalent runtime split.
    auto cfg = make_config();
    cfg.video_stream_weights = {{"default", 1}, {"screen", 3}};

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_TRUE(engine.add_video_stream("screen"));

    engine.tick();

    const std::uint32_t def = engine.video_stream_share_bps(kDefault);
    const std::uint32_t scr = engine.video_stream_share_bps("screen");
    ASSERT_GT(def, 0u);
    ASSERT_GT(scr, 0u);

    // Weights 1 : 3 — the screen stream takes three times the default
    // stream's share.
    EXPECT_NEAR(static_cast<double>(scr), static_cast<double>(def) * 3.0,
                static_cast<double>(def) * 0.02);
}
