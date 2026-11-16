/**
 * @file test_video_bitrate_control.cpp
 * @brief The unit tests for the video rate controller.
 *
 * Remote-desktop plan D1 / D2 / D3 / D4; video plan B2 / B3 / B4 / B9.
 *
 * The four acceptance criteria the plan states are covered here:
 *
 *   - **D1** — the engine resolves the controller through the registry
 *     and the mode selects which end of the ladder gives way first;
 *   - **D2** — the resolution floor is never crossed, even under a
 *     synthetic bandwidth collapse;
 *   - **D3** — a sawtooth bandwidth trace does not produce a staircase
 *     that flips level every interval;
 *   - **D4** — the target bitrate the controller reports is what the
 *     engine pushes into the encoder's parameter set.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/plugins/video_bitrate_controller.hpp>
#include <nimrtc/video_bitrate_control/controller.hpp>
#include <nimrtc/video_bitrate_control/factory.hpp>
#include <nimrtc/video_bitrate_control/ladder.hpp>

namespace {

using nimrtc::plugins::VideoBitrateControllerConfig;
using nimrtc::plugins::VideoBitrateControllerStats;
using nimrtc::plugins::VideoLadderLevel;
using nimrtc::plugins::VideoLadderMode;
using nimrtc::video_bitrate_control::BitrateController;
using nimrtc::video_bitrate_control::Ladder;

/** A controller whose clock the test drives. */
class TestController {
public:
    explicit TestController(VideoBitrateControllerConfig cfg)
        : ctrl_(cfg) {
        ctrl_.set_clock([this] { return now_; });
    }

    BitrateController& controller() { return ctrl_; }

    /** Advance the controller's clock by @p ms milliseconds. */
    void advance(std::uint32_t ms) {
        now_ += std::chrono::milliseconds(ms);
    }

private:
    BitrateController ctrl_;
    BitrateController::TimePoint now_{};
};

/** A configuration with a deterministic, monotone ladder. */
VideoBitrateControllerConfig simple_config(VideoLadderMode mode =
                                                VideoLadderMode::kQuality) {
    VideoBitrateControllerConfig cfg;
    cfg.mode = mode;
    cfg.source_width  = 1920;
    cfg.source_height = 1080;
    cfg.source_fps    = 30;
    cfg.source_bitrate_bps = 2'000'000;
    cfg.step_interval_ms = 0;   // no cooldown unless a test asks for one
    cfg.hysteresis = 0.0;
    cfg.max_steps_per_interval = 10;  // a test may narrow it
    cfg.min_bitrate_bps = 100'000;
    cfg.max_bitrate_bps = 8'000'000;
    cfg.keyframe_cost_bps = 0;   // the budget is opt-in
    cfg.ladder = {
        {1920, 1080, 30, 2'000'000, false},
        {1920, 1080, 30, 1'000'000, false},
        {1280,  720, 30,   800'000, true},
        { 960,  540, 24,   400'000, true},
        { 640,  360, 15,   200'000, true},
    };
    return cfg;
}

// ---------------------------------------------------------------------------
// The ladder itself
// ---------------------------------------------------------------------------

TEST(LadderTest, DefaultLadderHasRungs) {
    const Ladder l = Ladder::make_default(1920, 1080, 30, 2'000'000);
    ASSERT_FALSE(l.empty());
    EXPECT_EQ(l.size(), 9u);
    EXPECT_EQ(l.at(0).width, 1920u);
    EXPECT_EQ(l.at(0).height, 1080u);
}

TEST(LadderTest, DefaultLadderIsMonotoneInBitrate) {
    const Ladder l = Ladder::make_default(1920, 1080, 30, 2'000'000);
    for (std::size_t i = 1; i < l.size(); ++i) {
        EXPECT_LT(l.bitrate_for(i), l.bitrate_for(i - 1))
            << "rung " << i << " must cost less than rung " << (i - 1);
    }
}

TEST(LadderTest, QualityModeKeepsTheGeometryLongerThanSmoothness) {
    const Ladder q = Ladder::make_default(1920, 1080, 30, 2'000'000,
                                           VideoLadderMode::kQuality);
    const Ladder s = Ladder::make_default(1920, 1080, 30, 2'000'000,
                                           VideoLadderMode::kSmoothness);

    // Under quality-first the full-size picture survives the first
    // three rungs; under smoothness-first it is gone by the second.
    EXPECT_EQ(q.at(2).width, 1920u);
    EXPECT_LT(s.at(2).width, 1920u);

    // Under smoothness-first the frame rate is held longer.
    const auto first_fps_drop = [](const Ladder& l) {
        for (std::size_t i = 1; i < l.size(); ++i) {
            if (l.at(i).fps < l.at(0).fps) return i;
        }
        return l.size();
    };
    EXPECT_LE(first_fps_drop(s), first_fps_drop(q));
}

TEST(LadderTest, IndexForHeightFindsTheLowestSufficientRung) {
    const Ladder l = Ladder::make_default(1920, 1080, 30, 2'000'000);
    const std::size_t idx = l.index_for_height(720);
    EXPECT_GE(l.at(idx).height, 720u);
    if (idx + 1 < l.size()) {
        EXPECT_LT(l.at(idx + 1).height, 720u);
    }
}

TEST(LadderTest, IndexForHeightIsHonestWhenUnreachable) {
    Ladder l;
    l.set_levels({{640, 360, 30, 500'000, false}});
    EXPECT_EQ(l.index_for_height(1080), 0u);
}

TEST(LadderTest, BitrateIsClampedIntoTheBounds) {
    Ladder l;
    l.set_levels({{1920, 1080, 30, 50'000'000, false}});
    l.set_bitrate_bounds(150'000, 8'000'000);
    EXPECT_EQ(l.bitrate_for(0), 8'000'000u);
}

// ---------------------------------------------------------------------------
// D1 — the ladder moves in response to the bandwidth estimate
// ---------------------------------------------------------------------------

TEST(ControllerTest, StartsAtTheTopRung) {
    TestController t(simple_config());
    EXPECT_EQ(t.controller().current_level().width, 1920u);
    EXPECT_EQ(t.controller().stats().level_index, 0u);
}

TEST(ControllerTest, StepsDownWhenTheBandwidthCollapses) {
    TestController t(simple_config());
    t.controller().on_bandwidth_estimate(400'000);
    // The best rung that fits a 400 kbps budget is the 540p one.
    EXPECT_EQ(t.controller().current_level().height, 540u);
    EXPECT_EQ(t.controller().stats().level_drops, 1u);
}

TEST(ControllerTest, StepsBackUpWhenTheBandwidthReturns) {
    TestController t(simple_config());
    t.controller().on_bandwidth_estimate(400'000);
    ASSERT_EQ(t.controller().current_level().height, 540u);
    t.controller().on_bandwidth_estimate(2'000'000);
    EXPECT_EQ(t.controller().current_level().height, 1080u);
    EXPECT_EQ(t.controller().stats().level_rises, 1u);
}

TEST(ControllerTest, TheMeasuredBitrateTightensTheBudget) {
    TestController t(simple_config());
    // The link says 2 Mbps, but the encoder is producing 3 Mbps — the
    // link is congested even though the estimate has not caught up.
    t.controller().on_bandwidth_estimate(2'000'000);
    t.controller().on_measured_bitrate(3'000'000);
    EXPECT_LT(t.controller().stats().target_bitrate_bps, 2'000'000u);
}

TEST(ControllerTest, NoEstimateKeepsTheCurrentLevel) {
    TestController t(simple_config());
    t.controller().on_measured_bitrate(100'000);
    EXPECT_EQ(t.controller().current_level().height, 1080u);
    EXPECT_EQ(t.controller().stats().level_changes, 0u);
}

TEST(ControllerTest, KeyframeRequestStepsTheLadderDown) {
    TestController t(simple_config());
    t.controller().on_keyframe_requested();
    EXPECT_EQ(t.controller().stats().level_drops, 1u);
    EXPECT_EQ(t.controller().stats().level_index, 1u);
    // The peer asked, so the step is urgent — the keyframe-cost budget
    // does not apply.
    EXPECT_EQ(t.controller().stats().keyframe_cost_blocks, 0u);
}

TEST(ControllerTest, SetLevelRejectsAnOutOfRangeIndex) {
    TestController t(simple_config());
    EXPECT_EQ(t.controller().set_level(99),
              nimrtc::plugins::kErrInvalidParam);
    EXPECT_EQ(t.controller().set_level(2), nimrtc::plugins::kOk);
}

// ---------------------------------------------------------------------------
// D2 — the resolution floor
// ---------------------------------------------------------------------------

TEST(ControllerTest, TheFloorIsNeverCrossed) {
    VideoBitrateControllerConfig cfg = simple_config();
    cfg.floor_level_index = 2;   // 720p — the worst rung allowed
    TestController t(cfg);

    // A total bandwidth collapse.
    t.controller().on_bandwidth_estimate(1);
    EXPECT_GE(t.controller().current_level().height, 720u);
    EXPECT_EQ(t.controller().stats().level_index, 2u);
    EXPECT_GT(t.controller().stats().floor_blocks, 0u);
}

TEST(ControllerTest, TheFloorIsVisibleInTheStats) {
    VideoBitrateControllerConfig cfg = simple_config();
    cfg.floor_level_index = 2;
    TestController t(cfg);
    const VideoBitrateControllerStats s = t.controller().stats();
    EXPECT_EQ(s.resolution_floor_index, 2u);
    EXPECT_EQ(s.resolution_floor_height, 720u);
}

TEST(ControllerTest, AHeightFloorIsResolvedToARung) {
    VideoBitrateControllerConfig cfg = simple_config();
    cfg.min_height = 720;
    TestController t(cfg);
    t.controller().on_bandwidth_estimate(1);
    EXPECT_GE(t.controller().current_level().height, 720u);
}

TEST(ControllerTest, TheFloorBlocksTheHostOverrideToo) {
    VideoBitrateControllerConfig cfg = simple_config();
    cfg.floor_level_index = 2;
    TestController t(cfg);
    EXPECT_EQ(t.controller().set_level(4), nimrtc::plugins::kErrInvalidParam);
    EXPECT_EQ(t.controller().set_level(2), nimrtc::plugins::kOk);
}

TEST(ControllerTest, NoFloorMeansTheLadderMayUseEveryRung) {
    TestController t(simple_config());
    t.controller().on_bandwidth_estimate(1);
    EXPECT_EQ(t.controller().current_level().height, 360u);
    const VideoBitrateControllerStats s = t.controller().stats();
    EXPECT_EQ(s.resolution_floor_index, 0xFFFFFFFFu);
}

// ---------------------------------------------------------------------------
// D3 — the keyframe-cost budget, the cooldown and the hysteresis band
// ---------------------------------------------------------------------------

TEST(ControllerTest, TheKeyframeCostBudgetDefersAGeometryChange) {
    VideoBitrateControllerConfig cfg = simple_config();
    // The IDR costs more than the link can spare: a geometry change
    // must wait for a quiet interval.
    cfg.keyframe_cost_bps = 900'000;
    TestController t(cfg);

    // A budget that fits the 1080p/1 Mbps rung but not the 720p one.
    // The ladder *wants* the 720p rung (it fits the budget), but the
    // geometry change costs an IDR the link cannot spare — so the step
    // is deferred and the encoder simply gets less to spend at 1080p.
    t.controller().on_bandwidth_estimate(900'000);
    EXPECT_EQ(t.controller().current_level().height, 1080u);
    EXPECT_EQ(t.controller().stats().target_bitrate_bps, 900'000u);
    EXPECT_EQ(t.controller().stats().keyframe_cost_blocks, 1u);

    // Now the link cannot carry the current rung either: the geometry
    // has to give and the burst is accepted knowingly.
    t.controller().on_bandwidth_estimate(150'000);
    EXPECT_LT(t.controller().current_level().height, 1080u);
}

TEST(ControllerTest, TheKeyframeCostBudgetBlocksAnUnaffordableIDR) {
    VideoBitrateControllerConfig cfg = simple_config();
    cfg.keyframe_cost_bps = 2'000'000;   // far more than the estimate
    TestController t(cfg);

    // The link is healthy enough for the current rung but not for the
    // IDR a geometry change costs.
    t.controller().on_bandwidth_estimate(1'500'000);
    const std::uint64_t before = t.controller().stats().keyframe_cost_blocks;
    t.controller().on_bandwidth_estimate(700'000);
    EXPECT_GT(t.controller().stats().keyframe_cost_blocks, before);
    // The geometry did not change.
    EXPECT_EQ(t.controller().current_level().height, 1080u);
}

TEST(ControllerTest, ASameGeometryStepCostsNoKeyframe) {
    VideoBitrateControllerConfig cfg = simple_config();
    cfg.keyframe_cost_bps = 2'000'000;   // unaffordable
    TestController t(cfg);

    // 1 Mbps still fits the 1080p rung, so the ladder lowers the
    // *bitrate* without touching the geometry — no IDR, no budget.
    t.controller().on_bandwidth_estimate(1'000'000);
    EXPECT_EQ(t.controller().current_level().height, 1080u);
    EXPECT_EQ(t.controller().stats().keyframe_cost_blocks, 0u);
    EXPECT_EQ(t.controller().stats().target_bitrate_bps, 1'000'000u);
}

TEST(ControllerTest, AnUrgentStepIgnoresTheKeyframeCostBudget) {
    VideoBitrateControllerConfig cfg = simple_config();
    cfg.keyframe_cost_bps = 2'000'000;
    TestController t(cfg);

    // The link has collapsed below even the worst rung: the IDR is
    // accepted knowingly.
    t.controller().on_bandwidth_estimate(50'000);
    EXPECT_LT(t.controller().current_level().height, 1080u);
}

TEST(ControllerTest, TheCooldownSuppressesRapidChanges) {
    VideoBitrateControllerConfig cfg = simple_config();
    cfg.step_interval_ms = 500;
    cfg.max_steps_per_interval = 1;
    TestController t(cfg);

    t.controller().on_bandwidth_estimate(400'000);
    const std::uint64_t changes = t.controller().stats().level_changes;

    // Immediately afterwards another collapse: the cooldown holds it.
    t.controller().on_bandwidth_estimate(1);
    EXPECT_EQ(t.controller().stats().level_changes, changes);
    EXPECT_GT(t.controller().stats().level_changes_suppressed, 0u);

    // After the cooldown the ladder moves again.
    t.advance(600);
    t.controller().on_bandwidth_estimate(1);
    EXPECT_GT(t.controller().stats().level_changes, changes);
}

TEST(ControllerTest, TheMaxStepBoundsOneMove) {
    VideoBitrateControllerConfig cfg = simple_config();
    cfg.max_steps_per_interval = 1;
    TestController t(cfg);

    t.controller().on_bandwidth_estimate(1);
    // From rung 0 the ladder may only cross one rung per step.
    EXPECT_EQ(t.controller().stats().level_index, 1u);
}

TEST(ControllerTest, TheHysteresisBandHoldsTheLadder) {
    VideoBitrateControllerConfig cfg = simple_config();
    cfg.hysteresis = 0.6;   // 60 %
    TestController t(cfg);

    // The current target is 2 Mbps; a 1 Mbps budget is 50 % away,
    // which is inside the band, so the ladder stays.
    t.controller().on_bandwidth_estimate(1'000'000);
    EXPECT_EQ(t.controller().current_level().height, 1080u);

    // A budget far outside the band does move the ladder.
    t.controller().on_bandwidth_estimate(100'000);
    EXPECT_LT(t.controller().current_level().height, 1080u);
}

TEST(ControllerTest, ASawtoothTraceDoesNotFlipTheLevelEveryInterval) {
    VideoBitrateControllerConfig cfg = simple_config();
    cfg.step_interval_ms = 500;
    cfg.hysteresis = 0.15;
    TestController t(cfg);

    std::uint64_t worst = 0;
    for (int i = 0; i < 40; ++i) {
        // A sawtooth: the estimate oscillates by ±20 % around 1.2 Mbps.
        const std::uint32_t estimate =
            (i % 2 == 0) ? 1'200'000u : 960'000u;
        t.controller().on_bandwidth_estimate(estimate);
        worst = std::max(worst, t.controller().stats().level_changes);
        t.advance(600);
    }

    // The level-change rate is bounded: a sawtooth must not produce a
    // flip on every interval.
    EXPECT_LE(worst, 8u);
}

// ---------------------------------------------------------------------------
// D4 — the target bitrate is what the engine pushes into the encoder
// ---------------------------------------------------------------------------

TEST(ControllerTest, TheTargetBitrateFollowsTheLevel) {
    TestController t(simple_config());
    EXPECT_EQ(t.controller().stats().target_bitrate_bps, 2'000'000u);
    t.controller().on_bandwidth_estimate(400'000);
    EXPECT_EQ(t.controller().stats().target_bitrate_bps, 400'000u);
    // The engine reads the level, not the raw rung: the bitrate it
    // pushes into the encoder is the effective one.
    EXPECT_EQ(t.controller().current_level().bitrate_bps, 400'000u);
}

TEST(ControllerTest, TheTargetBitrateNeverLeavesTheBounds) {
    VideoBitrateControllerConfig cfg = simple_config();
    cfg.min_bitrate_bps = 150'000;
    cfg.max_bitrate_bps = 1'500'000;
    TestController t(cfg);
    t.controller().on_bandwidth_estimate(10'000'000);
    EXPECT_LE(t.controller().stats().target_bitrate_bps, 1'500'000u);
    t.controller().on_bandwidth_estimate(1);
    EXPECT_GE(t.controller().stats().target_bitrate_bps, 150'000u);
}

TEST(ControllerTest, SetConfigRebuildsTheDefaultLadderForTheMode) {
    VideoBitrateControllerConfig cfg;
    cfg.mode = VideoLadderMode::kSmoothness;
    cfg.source_width  = 1920;
    cfg.source_height = 1080;
    cfg.source_fps    = 30;
    cfg.source_bitrate_bps = 2'000'000;
    cfg.step_interval_ms = 0;
    cfg.hysteresis = 0.0;
    cfg.keyframe_cost_bps = 0;

    TestController t(cfg);
    t.controller().on_bandwidth_estimate(600'000);
    // Under smoothness-first the resolution goes first, so the picture
    // is already smaller than the source.
    EXPECT_LT(t.controller().current_level().width, 1920u);
}
TEST(ControllerTest, SetConfigRejectsInvertedBounds) {
    TestController t(simple_config());
    VideoBitrateControllerConfig bad = simple_config();
    bad.min_bitrate_bps = 5'000'000;
    bad.max_bitrate_bps = 1'000'000;
    EXPECT_EQ(t.controller().set_config(bad),
              nimrtc::plugins::kErrInvalidParam);
}

// ---------------------------------------------------------------------------
// The registry seam (D1: "the engine resolves the controller through
// the registry")
// ---------------------------------------------------------------------------

TEST(RegistryTest, TheFactoryIsRegistered) {
    nimrtc::video_bitrate_control::register_default_plugins();

    auto& reg = nimrtc::core::PluginRegistry::instance();
    const auto* factory =
        reg.get_video_bitrate_controller("video-bitrate-control");
    ASSERT_NE(factory, nullptr);
    EXPECT_EQ(factory->id(), "video-bitrate-control");

    const auto ids = reg.list_video_bitrate_controllers();
    EXPECT_NE(std::find(ids.begin(), ids.end(),
                        std::string("video-bitrate-control")),
              ids.end());
}

TEST(RegistryTest, TheFactoryCreatesAController) {
    nimrtc::video_bitrate_control::register_default_plugins();
    auto& reg = nimrtc::core::PluginRegistry::instance();
    const auto* factory =
        reg.get_video_bitrate_controller("video-bitrate-control");
    ASSERT_NE(factory, nullptr);

    std::unique_ptr<nimrtc::plugins::IVideoBitrateController> ctrl(
        factory->create(simple_config()));
    ASSERT_NE(ctrl, nullptr);
    EXPECT_STREQ(ctrl->name(), "nimrtc-video-bitrate-control");
    EXPECT_EQ(ctrl->open(), nimrtc::plugins::kOk);
    ctrl->close();
}

} // namespace
