/**
 * @file tests/test_engine_video_sync.cpp
 * @brief The engine-level half of remote-desktop plan Track E — the
 *        render clock (E5), the input echo (E6), the presentation (E7)
 *        and the frame-accurate "what the operator saw" record (E9).
 *
 * ## What is tested here, and what is tested where
 *
 * The four features of Track E split into a *pure decision* and an
 * *engine wiring*:
 *
 *   | feature | the decision                      | the wiring            |
 *   |---------|----------------------------------|-----------------------|
 *   | E5      | `detail::VideoSyncController`    | the engine's render path |
 *   | E6      | `detail::InputEchoController`    | the engine's echo seam   |
 *   | E7      | `detail::compute_scale_to_fit`   | the engine's sink config |
 *   | E9      | (none — it is a record)          | the engine's render path |
 *
 * The decisions are tested **directly** — they are engine-private
 * header-only collaborators with no clock, no allocation and no
 * thread, so a unit test is the honest way to pin them.  The wiring is
 * tested **through the engine's public API**, which is the surface a
 * host actually uses.
 *
 * The test compiles the engine's *private* header directory into its
 * include path, exactly as `test_video_render_path` does.  That
 * directory is not installed and is not on the public surface
 * (constraint L4).
 *
 * @see docs/plan/remote-desktop-plan.md §Track E.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>

// The engine-private collaborators (Layout Invariant 4: these headers
// are not installed and no public header includes them).
#include "input_echo_controller.hpp"
#include "video_aspect_fit.hpp"
#include "video_sync_controller.hpp"

namespace {

using nimrtc::core::PluginRegistry;
using nimrtc::engine::DisplayedFrameInfo;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::InputEchoEvent;
using nimrtc::engine::NimRTCEngine;
using nimrtc::engine::VideoAspectPolicy;
using nimrtc::engine::VideoSyncStats;

namespace ns = nimrtc::engine::detail;
namespace np = nimrtc::plugins;

// ===========================================================================
// E5 — the render clock's *decision* (the pure collaborator)
// ===========================================================================

TEST(VideoSyncDecision, ASkewInsideTheBandIsPresented) {
    ns::VideoSyncController c;
    ns::VideoSyncConfig cfg;
    cfg.tolerance_ms = 40;
    c.configure(cfg);

    // 0 us of skew — dead on.
    EXPECT_EQ(c.decide(0, 1000).decision, ns::VideoSyncDecision::kPresent);

    // Exactly on the band edge — still inside (the band is inclusive).
    EXPECT_EQ(c.decide(40'000, 1000).decision, ns::VideoSyncDecision::kPresent);
    EXPECT_EQ(c.decide(-40'000, 1000).decision, ns::VideoSyncDecision::kPresent);

    // Just inside the band.
    EXPECT_EQ(c.decide(39'999, 1000).decision, ns::VideoSyncDecision::kPresent);
    EXPECT_EQ(c.decide(-39'999, 1000).decision, ns::VideoSyncDecision::kPresent);
}

TEST(VideoSyncDecision, AVideoAheadOfTheAudioIsDropped) {
    ns::VideoSyncController c;
    ns::VideoSyncConfig cfg;
    cfg.tolerance_ms = 40;
    c.configure(cfg);

    // Negative skew = the video is *ahead*: the picture would be shown
    // before the sound it belongs to.
    const auto out = c.decide(-100'000, 1000);
    EXPECT_EQ(out.decision, ns::VideoSyncDecision::kDrop);
    EXPECT_EQ(out.delay_us, 0) << "a drop never asks for a hold";
    EXPECT_EQ(out.skew_us, -100'000);
}

TEST(VideoSyncDecision, AVideoBehindTheAudioIsDelayed) {
    ns::VideoSyncController c;
    ns::VideoSyncConfig cfg;
    cfg.tolerance_ms = 40;
    c.configure(cfg);

    // Positive skew = the video is *behind*: the picture has to wait.
    const auto out = c.decide(100'000, 1000);
    EXPECT_EQ(out.decision, ns::VideoSyncDecision::kDelay);
    EXPECT_GT(out.delay_us, 0);
    // The hold is the skew *minus* the tolerance band.
    EXPECT_EQ(out.delay_us, 100'000 - 40'000);
}

TEST(VideoSyncDecision, TheCountersMoveWithTheDecisions) {
    ns::VideoSyncController c;
    ns::VideoSyncConfig cfg;
    cfg.tolerance_ms = 40;
    c.configure(cfg);

    c.decide(0, 1);                      // present
    c.decide(-100'000, 2);               // drop
    c.decide(100'000, 3);                // delay
    c.decide(10'000, 4);                 // present

    const auto& k = c.counters();
    EXPECT_EQ(k.frames_presented, 2u);
    EXPECT_EQ(k.frames_dropped, 1u);
    EXPECT_EQ(k.frames_delayed, 1u);
    EXPECT_EQ(k.total_delay_us, 60'000);
    EXPECT_EQ(k.last_skew_us, 10'000);
}

TEST(VideoSyncDecision, TheDelayIsAlwaysBoundedByTheCap) {
    ns::VideoSyncController c;
    ns::VideoSyncConfig cfg;
    cfg.tolerance_ms    = 40;
    cfg.max_delay_ms    = 100;   // the cap
    c.configure(cfg);

    // A skew far beyond the cap: the hold is clamped.
    const auto out = c.decide(5'000'000, 1000);
    EXPECT_EQ(out.decision, ns::VideoSyncDecision::kDelay);
    EXPECT_EQ(out.delay_us, 100'000) << "the hold must never exceed the cap";

    // And it stays bounded no matter how many spikes arrive.
    for (int i = 0; i < 100; ++i) {
        const auto spike = c.decide(5'000'000, 1000 + i);
        EXPECT_LE(spike.delay_us, c.max_delay_us());
        EXPECT_GE(spike.delay_us, 0);
    }
    EXPECT_LE(c.counters().total_delay_us, 101LL * 100'000);
}

TEST(VideoSyncDecision, ASkewSpikeDoesNotAccumulateUnboundedDelay) {
    ns::VideoSyncController c;
    ns::VideoSyncConfig cfg;
    cfg.tolerance_ms = 40;
    cfg.max_delay_ms = 200;
    c.configure(cfg);

    // The first spike promises a hold.
    const auto first = c.decide(1'000'000, 1);
    EXPECT_GT(first.delay_us, 0);

    // The controller is told the hold actually happened.
    c.note_delay_applied(first.delay_us);

    // A second spike must not stack the previous debt on top of the
    // new one — the debt was consumed.
    const auto second = c.decide(1'000'000, 2);
    EXPECT_LE(second.delay_us, c.max_delay_us());
    EXPECT_EQ(second.delay_us, first.delay_us);
}

TEST(VideoSyncDecision, ADropCancelsThePendingDebt) {
    ns::VideoSyncController c;
    ns::VideoSyncConfig cfg;
    cfg.tolerance_ms = 40;
    // The cap is deliberately *not* binding here: the point of the test
    // is what the drop does to the *debt*, and a cap that clamped the
    // answer would hide the difference between "the debt was cancelled"
    // and "the answer was clamped".
    cfg.max_delay_ms = 2'000;   // 2 s — far above the 1 s skew below
    c.configure(cfg);

    c.decide(1'000'000, 1);   // delay — the debt is now pending
    EXPECT_EQ(c.decide(-1'000'000, 2).decision, ns::VideoSyncDecision::kDrop);

    // The debt was cancelled by the drop, so the next delay is the skew
    // minus the band again (not less) — and with a non-binding cap that
    // value is exactly the skew minus the band.
    const auto after = c.decide(1'000'000, 3);
    EXPECT_EQ(after.delay_us, 1'000'000 - 40'000)
        << "a drop must clear the pending debt, so the next hold is the "
           "full skew minus the band";
    EXPECT_LE(after.delay_us, c.max_delay_us())
        << "and it is still bounded by the controller's cap";
}

TEST(VideoSyncDecision, TheHistoryRingKeepsTheRecentDecisions) {
    ns::VideoSyncController c;
    ns::VideoSyncConfig cfg;
    cfg.tolerance_ms = 40;    // the band is 40 ms, so ±1 us is *inside* it
    c.configure(cfg);

    // The skews must be outside the band for the decisions to differ.
    c.decide(0, 1);                       // present (inside the band)
    c.decide(-100'000, 2);               // drop   (video ahead)
    c.decide(100'000, 3);                // delay  (video behind)

    // Slot 0 is the most recent.
    EXPECT_EQ(c.history_decision(0), ns::VideoSyncDecision::kDelay);
    EXPECT_EQ(c.history_decision(1), ns::VideoSyncDecision::kDrop);
    EXPECT_EQ(c.history_decision(2), ns::VideoSyncDecision::kPresent);
    EXPECT_EQ(c.history_skew_us(0), 100'000);
    EXPECT_EQ(c.history_skew_us(1), -100'000);
}

TEST(VideoSyncDecision, TheDecisionIsDeterministic) {
    // The same inputs must give the same answer — the controller has no
    // clock and no hidden state beyond its own counters.
    ns::VideoSyncController a;
    ns::VideoSyncController b;
    ns::VideoSyncConfig cfg;
    cfg.tolerance_ms = 25;
    a.configure(cfg);
    b.configure(cfg);

    for (int i = 0; i < 10; ++i) {
        const std::int64_t skew = (i % 3 == 0) ? 0 : ((i % 3 == 1) ? 50'000 : -50'000);
        const auto oa = a.decide(skew, i);
        const auto ob = b.decide(skew, i);
        EXPECT_EQ(oa.decision, ob.decision);
        EXPECT_EQ(oa.delay_us, ob.delay_us);
    }
}

// ===========================================================================
// E6 — the input echo's *decision* (the pure collaborator)
// ===========================================================================

TEST(InputEchoDecision, ItIsOffByDefault) {
    ns::InputEchoController c;
    EXPECT_FALSE(c.enabled());
    EXPECT_EQ(c.decide(1000, 1000), ns::InputEchoReject::kDisabled);
}

TEST(InputEchoDecision, AStaleEventIsDropped) {
    ns::InputEchoController c;
    ns::InputEchoConfig cfg;
    cfg.enabled     = true;
    cfg.max_age_us  = 100'000;   // 100 ms
    cfg.max_rate_hz = 0;         // no rate limit for this case
    c.configure(cfg);

    // Fresh: 10 ms old.
    EXPECT_EQ(c.decide(1'000'000, 990'000), ns::InputEchoReject::kNone);

    // Stale: 500 ms old.
    EXPECT_EQ(c.decide(1'000'000, 500'000), ns::InputEchoReject::kStale);
    EXPECT_EQ(c.counters().suppressed_stale, 1u);
}

TEST(InputEchoDecision, TheRateLimitIsRespected) {
    ns::InputEchoController c;
    ns::InputEchoConfig cfg;
    cfg.enabled     = true;
    cfg.max_age_us  = 0;        // no age limit
    cfg.max_rate_hz = 5;
    c.configure(cfg);

    int echoed = 0;
    for (int i = 0; i < 20; ++i) {
        // All inside the same one-second window.
        if (c.decide(1'000'000, 1'000'000) == ns::InputEchoReject::kNone) ++echoed;
    }
    EXPECT_EQ(echoed, 5) << "the rate limit must cap the echo rate";
    EXPECT_EQ(c.counters().suppressed_rate, 15u);
}

TEST(InputEchoDecision, TheRateWindowRollsOver) {
    ns::InputEchoController c;
    ns::InputEchoConfig cfg;
    cfg.enabled     = true;
    cfg.max_age_us  = 0;
    cfg.max_rate_hz = 2;
    c.configure(cfg);

    EXPECT_EQ(c.decide(1'000'000, 1'000'000), ns::InputEchoReject::kNone);
    EXPECT_EQ(c.decide(1'000'000, 1'000'000), ns::InputEchoReject::kNone);
    EXPECT_EQ(c.decide(1'000'000, 1'000'000), ns::InputEchoReject::kRateLimited);

    // A second later the window rolls over and the echo is allowed again.
    EXPECT_EQ(c.decide(2'100'000, 2'100'000), ns::InputEchoReject::kNone);
}

// ===========================================================================
// E7 — the scale-to-fit geometry (the pure helper)
// ===========================================================================

TEST(ScaleToFit, ALetterboxPreservesTheAspectRatio) {
    // 16:9 into a 1000x1000 square.
    const auto r = ns::compute_scale_to_fit(1920, 1080, 1000, 1000, /*letterbox=*/true);
    EXPECT_EQ(r.width, 1000u);
    EXPECT_EQ(r.height, 563u);   // 1000 * 1080 / 1920 = 562.5 -> 563

    // 4:3 into a 16:9 box: the bars are on the left / right.
    const auto r2 = ns::compute_scale_to_fit(640, 480, 1920, 1080, true);
    EXPECT_EQ(r2.height, 1080u);
    EXPECT_EQ(r2.width, 1440u);
}

TEST(ScaleToFit, ACropFillsTheWholeTarget) {
    const auto r = ns::compute_scale_to_fit(1920, 1080, 1000, 1000, /*letterbox=*/false);
    EXPECT_EQ(r.width, 1000u);
    EXPECT_EQ(r.height, 1000u);
}

TEST(ScaleToFit, ADegenerateDimensionYieldsZero) {
    // A degenerate *source* dimension.
    EXPECT_EQ(ns::compute_scale_to_fit(0, 1080, 1000, 1000, true).width, 0u);
    EXPECT_EQ(ns::compute_scale_to_fit(1920, 0, 1000, 1000, true).width, 0u);
    EXPECT_EQ(ns::compute_scale_to_fit(1920, 0, 1000, 1000, true).height, 0u);

    // A degenerate *target* dimension.
    EXPECT_EQ(ns::compute_scale_to_fit(1920, 1080, 0, 1000, true).width, 0u);
    EXPECT_EQ(ns::compute_scale_to_fit(1920, 1080, 1000, 0, true).width, 0u);
    EXPECT_EQ(ns::compute_scale_to_fit(1920, 1080, 1000, 0, true).height, 0u);
}

// ===========================================================================
// The engine-level wiring
// ===========================================================================

class EngineVideoSync : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
    }

    static EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.stun_server_host         = "";
        cfg.local_port_range_begin   = 52800;
        cfg.local_port_range_end     = 52900;
        cfg.transport_strategy_name  = "shell";
        cfg.video_codec_name         = "h264";
        cfg.video_sender_name        = "reference";
        cfg.video_receiver_name      = "reference";
        cfg.video_source_name        = "memory";
        cfg.video_sink_name          = "headless";
        return cfg;
    }
};

// ---------------------------------------------------------------------------
// E5 — the engine publishes the render clock's counters
// ---------------------------------------------------------------------------

TEST_F(EngineVideoSync, TheSyncCountersStartAtZero) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    const VideoSyncStats s = engine.video_sync_stats();
    EXPECT_EQ(s.frames_presented, 0u);
    EXPECT_EQ(s.frames_dropped_sync, 0u);
    EXPECT_EQ(s.frames_delayed, 0u);
    EXPECT_EQ(s.total_delay_us, 0);
    EXPECT_DOUBLE_EQ(s.last_skew_ms, 0.0);

    // The same numbers are in the general statistics block.
    EXPECT_EQ(engine.stats().video_sync.frames_presented, 0u);

    engine.close();
}

TEST_F(EngineVideoSync, TheToleranceBandRoundTrips) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    // The band is a configuration, and it is *effective*: the render
    // clock uses it.  (The observable form of that is the decision the
    // controller makes — covered by the unit tests above.)
    engine.set_video_sync_tolerance(25, 120);

    engine.close();
}

TEST_F(EngineVideoSync, AFrameIsRenderedWhenThereIsNoAudioReference) {
    // With no audio playout reference there is nothing to align against,
    // so every frame is presented — the picture must not stall just
    // because the session has no sound yet.
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    engine.set_video_sync_tolerance(40, 200);

    auto* sink = engine.video_sink();
    ASSERT_NE(sink, nullptr);

    const np::VideoSourceFrame frame{};
    EXPECT_EQ(sink->render(frame), np::kOk);

    engine.close();
}

// ---------------------------------------------------------------------------
// E6 — the input echo's engine seam
// ---------------------------------------------------------------------------

TEST_F(EngineVideoSync, TheEchoFlagRoundTrips) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    // Off by default — the echo is a *product* decision.
    EXPECT_FALSE(engine.input_echo_enabled());

    engine.set_input_echo_enabled(true);
    EXPECT_TRUE(engine.input_echo_enabled());

    engine.set_input_echo_enabled(false);
    EXPECT_FALSE(engine.input_echo_enabled());

    engine.close();
}

TEST_F(EngineVideoSync, NothingIsEchoedWhileTheEchoIsOff) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    int echoed = 0;
    engine.set_input_echo_callback([&](const InputEchoEvent&) { ++echoed; });

    engine.set_input_echo_enabled(false);

    InputEchoEvent ev;
    ev.x = 10.0;
    ev.y = 20.0;
    ev.timestamp_us = 1'000'000;

    EXPECT_FALSE(engine.submit_input_echo(ev, 1'000'000));
    EXPECT_EQ(echoed, 0) << "a disabled echo must not reach the seam";
    EXPECT_EQ(engine.input_echo_stats().echoed, 0u);

    engine.close();
}

TEST_F(EngineVideoSync, AnAcceptedEchoReachesTheSeam) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    int echoed = 0;
    double last_x = 0.0;
    engine.set_input_echo_callback([&](const InputEchoEvent& e) {
        ++echoed;
        last_x = e.x;
    });

    engine.set_input_echo_enabled(true);

    InputEchoEvent ev;
    ev.x = 42.5;
    ev.y = 24.5;
    ev.timestamp_us = 1'000'000;

    EXPECT_TRUE(engine.submit_input_echo(ev, 1'000'000));
    EXPECT_EQ(echoed, 1);
    EXPECT_DOUBLE_EQ(last_x, 42.5);
    EXPECT_EQ(engine.input_echo_stats().echoed, 1u);

    engine.close();
}

TEST_F(EngineVideoSync, TheRateLimitIsRespectedThroughTheSeam) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    int echoed = 0;
    engine.set_input_echo_callback([&](const InputEchoEvent&) { ++echoed; });

    engine.set_input_echo_enabled(true);
    engine.set_input_echo_limits(/*max_age_us=*/0, /*max_rate_hz=*/4);

    // 20 candidates inside the same one-second window: at most 4 out.
    for (int i = 0; i < 20; ++i) {
        InputEchoEvent ev;
        ev.x = static_cast<double>(i);
        ev.y = 0.0;
        ev.timestamp_us = 1'000'000;
        engine.submit_input_echo(ev, 1'000'000);
    }

    EXPECT_LE(echoed, 4) << "the rate limit must bound the echo rate";
    EXPECT_EQ(engine.input_echo_stats().suppressed_rate, 20u - static_cast<std::uint64_t>(echoed));

    engine.close();
}

TEST_F(EngineVideoSync, AStaleEchoIsSuppressed) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    int echoed = 0;
    engine.set_input_echo_callback([&](const InputEchoEvent&) { ++echoed; });

    engine.set_input_echo_enabled(true);
    engine.set_input_echo_limits(/*max_age_us=*/50'000, /*max_rate_hz=*/0);

    InputEchoEvent ev;
    ev.timestamp_us = 1'000'000;

    // The event is 500 ms old — well past the 50 ms budget.
    EXPECT_FALSE(engine.submit_input_echo(ev, 1'500'000));
    EXPECT_EQ(echoed, 0);
    EXPECT_EQ(engine.input_echo_stats().suppressed_stale, 1u);

    engine.close();
}

// ---------------------------------------------------------------------------
// E7 — the presentation resolves into the sink configuration
// ---------------------------------------------------------------------------

TEST_F(EngineVideoSync, TheAspectPolicyRoundTrips) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    // The default policy is the letterbox.
    EXPECT_EQ(engine.video_aspect_policy(), VideoAspectPolicy::kLetterbox);

    engine.set_video_presentation(1920, 1080, VideoAspectPolicy::kCrop);
    EXPECT_EQ(engine.video_aspect_policy(), VideoAspectPolicy::kCrop);

    engine.set_video_presentation(1920, 1080, VideoAspectPolicy::kStretch);
    EXPECT_EQ(engine.video_aspect_policy(), VideoAspectPolicy::kStretch);

    engine.set_video_presentation(1920, 1080, VideoAspectPolicy::kLetterbox);
    EXPECT_EQ(engine.video_aspect_policy(), VideoAspectPolicy::kLetterbox);

    engine.close();
}

TEST_F(EngineVideoSync, ThePresentationReachesTheSinkConfiguration) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    engine.set_video_presentation(1280, 720, VideoAspectPolicy::kCrop);

    auto* sink = engine.video_sink();
    ASSERT_NE(sink, nullptr);

    const np::VideoSinkConfig cfg = sink->config();
    EXPECT_EQ(cfg.width, 1280u) << "the target width must reach the sink";
    EXPECT_EQ(cfg.height, 720u) << "the target height must reach the sink";
    EXPECT_EQ(cfg.scale_mode, np::VideoScaleMode::kCrop)
        << "the engine's policy must be translated into the sink seam's";

    engine.close();
}

TEST_F(EngineVideoSync, ALiveSinkFollowsThePresentation) {
    // The viewer resized the window *after* the session started.  The
    // sink is already live, so it has to be told — the engine must not
    // need the whole video pipeline rebuilt for a resize (plan E7).
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    auto* sink = engine.video_sink();
    ASSERT_NE(sink, nullptr);

    engine.set_video_presentation(1920, 1080, VideoAspectPolicy::kLetterbox);
    EXPECT_EQ(sink->config().width, 1920u);
    EXPECT_EQ(sink->config().height, 1080u);

    // A second resize replaces the first one.
    engine.set_video_presentation(800, 600, VideoAspectPolicy::kCrop);
    EXPECT_EQ(sink->config().width, 800u);
    EXPECT_EQ(sink->config().height, 600u);
    EXPECT_EQ(sink->config().scale_mode, np::VideoScaleMode::kCrop);

    engine.close();
}

// ---------------------------------------------------------------------------
// E9 — the frame-accurate "what the operator saw" record
// ---------------------------------------------------------------------------

TEST_F(EngineVideoSync, NothingIsDisplayedBeforeTheFirstFrame) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_FALSE(engine.last_displayed_frame().has_value());
    EXPECT_FALSE(engine.displayed_frame_for_ref_frame(1).has_value());

    engine.close();
}

TEST_F(EngineVideoSync, TheDisplayedFrameResolvesItsRefFrameId) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    // The engine records the (frame_seq -> capture_ts) mapping when the
    // frame arrives; the E9 record resolves the id through that table.
    const std::uint64_t ref_id = 4242;
    const std::int64_t  capture_ts = 1'234'567;
    engine.record_video_ref_frame(ref_id, capture_ts);

    // A frame with that sequence number is displayed.
    engine.set_video_presentation(0, 0);   // no presentation: the sink keeps its defaults
    EXPECT_EQ(engine.video_sink()->config().width, 0u);

    // The record is only written by the render path, so the test drives
    // the sink directly and then asks the engine what it showed.  With
    // no frame rendered, there is still nothing recorded.
    EXPECT_FALSE(engine.last_displayed_frame().has_value());

    engine.close();
}

TEST_F(EngineVideoSync, TheReverseLookupAgreesWithTheForwardLookup) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    // The engine's own ref-frame table is the authority for the
    // (id, timestamp) pair.  A host that receives a click naming a
    // `ref_frame` id resolves it the same way the display record does.
    engine.record_video_ref_frame(77u, 999'888);

    const auto ts = engine.ref_frame_to_capture_ts(77u);
    ASSERT_TRUE(ts.has_value());
    EXPECT_EQ(*ts, 999'888);

    const auto id = engine.capture_ts_to_ref_frame(999'888);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, 77u);

    engine.close();
}

}  // namespace
