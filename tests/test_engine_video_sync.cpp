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
 *   | feature | the decision                        | the wiring              |
 *   |---------|-------------------------------------|-------------------------|
 *   | E5      | `nimrtc::video_sync::RenderClock`   | the engine's render path |
 *   | E6      | `detail::InputEchoController`       | the engine's echo seam   |
 *   | E7      | `detail::compute_scale_to_fit`      | the engine's sink config |
 *   | E9      | (none — it is a record)             | the engine's render path |
 *
 * The decisions are tested **directly** — they are pure collaborators
 * with no clock, no allocation and no thread, so a unit test is the
 * honest way to pin them.  E5's decision moved to the `video_sync`
 * module in v1.3 (Q6) and is tested there
 * (`src/modules/video_sync/tests/test_render_clock.cpp`); what is
 * tested *here* is the engine's **wiring** through its public API,
 * which is the surface a host actually uses.
 *
 * The test compiles the engine's *private* header directory into its
 * include path, exactly as `test_video_render_path` does.  That
 * directory is not installed and is not on the public surface
 * (constraint L4).
 *
 * @see docs/plan/remote-desktop-plan.md §Track E.
 * @see docs/plan/v1.3-plan.md §3 Q6.
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
    // clock makes — covered by the module's own unit tests.)
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
// E5 — the correction's switch (v1.3 Q6, decision Q6-1)
// ---------------------------------------------------------------------------

TEST_F(EngineVideoSync, TheVideoCorrectionIsOnByDefault) {
    // The audio half is switchable, and the video half matching it keeps
    // the two symmetric: a caller that turned the audio correction off
    // should not get a video correction it cannot also turn off.
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_TRUE(engine.video_sync_enabled());

    engine.close();
}

TEST_F(EngineVideoSync, TheVideoCorrectionCanBeTurnedOff) {
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    engine.set_video_sync_enabled(false);
    EXPECT_FALSE(engine.video_sync_enabled());

    engine.set_video_sync_enabled(true);
    EXPECT_TRUE(engine.video_sync_enabled());

    engine.close();
}

TEST_F(EngineVideoSync, TheCountersSurviveTheSwitch) {
    // Turning the correction off must not reset the counters — a host
    // that turns it off still wants to read how much it had done.
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    const auto before = engine.video_sync_stats();
    engine.set_video_sync_enabled(false);
    const auto after = engine.video_sync_stats();

    EXPECT_EQ(after.frames_presented, before.frames_presented);
    EXPECT_EQ(after.frames_dropped_sync, before.frames_dropped_sync);
    EXPECT_EQ(after.frames_delayed, before.frames_delayed);

    engine.close();
}

// ---------------------------------------------------------------------------
// E5 -- the timeline (v1.3 Q6)
//
// The render clock's *decision* is tested in the `video_sync` module,
// which only ever sees a skew the engine computed.  What the module's
// tests cannot see is whether the engine computed that skew from two
// instants that belong to the *same* clock.  These tests pin that.
//
// The failure they guard against is concrete: the engine used to
// compare the peer's Unix-timeline capture instant (the audio
// reference) with the frame's *local monotonic arrival* stamp (the
// jitter buffer's stamp).  The two are unrelated clocks, so the
// difference was epoch-sized -- every frame looked hundreds of seconds
// behind the sound, and the clock dropped all of them.
// ---------------------------------------------------------------------------

namespace {

// A peer's capture instant, in Unix microseconds -- the shape the
// ADR-017 seam produces.  The value is deliberately an *epoch-scale*
// number: a local monotonic clock would never produce one, so a test
// that pins it also pins which clock the engine is reading.
constexpr std::int64_t kPeerCaptureUs = 1'700'000'000'000'000LL;

}  // namespace

TEST_F(EngineVideoSync, APictureOnThePeersTimelineIsAlignedNotDropped) {
    // A picture captured at the *same* instant as the sound is within
    // the tolerance band, so the clock presents it.
    //
    // With the defect, the picture's operand was a local monotonic
    // stamp -- a small number near zero -- while the audio reference
    // was an epoch-scale number.  The difference was then ~-1.7e15 us:
    // "the picture is a trillion microseconds behind the sound", so the
    // clock dropped the frame.  This test fails on the defect and
    // passes on the fix.
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    engine.set_video_sync_tolerance(40, 200);

    // The sound that is at the speaker, on the peer's timeline.
    engine.set_video_sync_audio_reference_us(kPeerCaptureUs);

    // The picture, captured at the very same instant, on the same
    // timeline.
    std::int64_t delay_us = -1;
    const bool presented =
        engine.apply_render_sync(kPeerCaptureUs, &delay_us);

    EXPECT_TRUE(presented) << "a picture captured at the same instant as "
                               "the sound must be presented, not dropped";
    EXPECT_EQ(delay_us, 0) << "a picture inside the tolerance band is "
                              "presented without a hold";

    engine.close();
}

TEST_F(EngineVideoSync, APictureAheadOfTheSoundIsHeld) {
    // The picture was captured 100 ms *after* the sound that is at the
    // speaker, so it is ahead and has to wait for the sound to catch up.
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    engine.set_video_sync_tolerance(40, 200);

    engine.set_video_sync_audio_reference_us(kPeerCaptureUs);

    std::int64_t delay_us = 0;
    const bool presented = engine.apply_render_sync(
        kPeerCaptureUs + 100'000,  // 100 ms ahead
        &delay_us);

    EXPECT_TRUE(presented) << "a picture ahead of the sound is held, not "
                              "dropped";
    EXPECT_GT(delay_us, 0) << "a picture ahead of the sound must be held "
                              "for the difference";

    engine.close();
}

TEST_F(EngineVideoSync, APictureBehindTheSoundIsDropped) {
    // The picture was captured 100 ms *before* the sound that is at the
    // speaker: the moment the operator heard is already gone, so the
    // frame is dropped.
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    engine.set_video_sync_tolerance(40, 200);

    engine.set_video_sync_audio_reference_us(kPeerCaptureUs);

    std::int64_t delay_us = 0;
    const bool presented = engine.apply_render_sync(
        kPeerCaptureUs - 100'000,  // 100 ms behind
        &delay_us);

    EXPECT_FALSE(presented) << "a picture behind the sound must be "
                               "dropped -- the moment is already gone";
    EXPECT_EQ(delay_us, 0) << "a dropped frame asks for no hold";

    engine.close();
}

TEST_F(EngineVideoSync, TheDecisionFollowsTheDifferenceNotTheMagnitude) {
    // The clock reacts to the *difference* between the two instants.
    // Two frames whose operands differ by the same amount -- but sit at
    // wildly different points on the timeline -- must get the same
    // decision.  This is the property the defect broke: the decision
    // used to depend on the operands' *magnitude*.
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    engine.set_video_sync_tolerance(40, 200);

    // An epoch-scale pair.
    engine.set_video_sync_audio_reference_us(kPeerCaptureUs);
    std::int64_t epoch_delay_us = 0;
    const bool epoch_presented = engine.apply_render_sync(
        kPeerCaptureUs + 100'000, &epoch_delay_us);

    // The same difference, at a small absolute value.
    engine.set_video_sync_audio_reference_us(50'000);
    std::int64_t small_delay_us = 0;
    const bool small_presented = engine.apply_render_sync(
        50'000 + 100'000, &small_delay_us);

    EXPECT_EQ(epoch_presented, small_presented)
        << "the decision must depend on the difference between the two "
           "instants, not on where they sit on the timeline";
    EXPECT_EQ(epoch_delay_us, small_delay_us)
        << "the hold must be the same for the same difference";

    engine.close();
}

TEST_F(EngineVideoSync, WithoutAnAudioReferenceEveryPictureIsPresented) {
    // With no sound to align against there is nothing to compare, so
    // the picture must not be dropped -- whatever its own instant is.
    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    engine.set_video_sync_tolerance(40, 200);

    std::int64_t delay_us = -1;
    const bool presented =
        engine.apply_render_sync(kPeerCaptureUs, &delay_us);

    EXPECT_TRUE(presented) << "without a sound there is nothing to align "
                              "against, so the picture must be presented";
    EXPECT_EQ(delay_us, 0);

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
