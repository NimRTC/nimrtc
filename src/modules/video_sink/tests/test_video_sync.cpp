// src/modules/video_sink/tests/test_video_sync.cpp
// ----------------------------------------------------------------------------
// The video half of the A/V sync decision (remote-desktop plan E5) and
// the "what the operator saw" record (plan E9).
//
// Both are pure bookkeeping -- no GPU, no display, no platform SDK -- so
// they are tested on every platform, including the CI legs that have no
// screen at all.
// ----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstdint>

#include "nimrtc/video_sink/displayed_frame.hpp"
#include "nimrtc/video_sink/video_sync.hpp"

namespace nimrtc::video_sink {
namespace {

// ---------------------------------------------------------------------------
// E5 — the sync controller
// ---------------------------------------------------------------------------

TEST(VideoSync, AFrameWithinTheToleranceIsPresented) {
    VideoSyncController controller;
    const auto decision = controller.decide(0);
    EXPECT_EQ(decision.action, SyncAction::kPresent);
    EXPECT_EQ(decision.delay_us, 0);
    EXPECT_EQ(decision.skew_us, 0);
}

TEST(VideoSync, ASmallLeadIsPresented) {
    VideoSyncController controller;
    // 10 ms of lead is inside the 40 ms tolerance.
    const auto decision = controller.decide(10'000);
    EXPECT_EQ(decision.action, SyncAction::kPresent);
}

TEST(VideoSync, ASmallLagIsPresented) {
    VideoSyncController controller;
    const auto decision = controller.decide(-10'000);
    EXPECT_EQ(decision.action, SyncAction::kPresent);
}

TEST(VideoSync, AFrameThatLeadsTheAudioIsDelayed) {
    VideoSyncController controller;
    // 60 ms of lead: over the 40 ms tolerance, under the 100 ms maximum.
    const auto decision = controller.decide(60'000);
    EXPECT_EQ(decision.action, SyncAction::kDelay);
    EXPECT_EQ(decision.delay_us, 20'000);
    EXPECT_EQ(decision.skew_us, 60'000);
}

TEST(VideoSync, AFrameThatLagsTheAudioIsDropped) {
    VideoSyncController controller;
    // 60 ms of lag: the operator has already heard it.
    const auto decision = controller.decide(-60'000);
    EXPECT_EQ(decision.action, SyncAction::kDrop);
    EXPECT_EQ(decision.delay_us, 0);
}

TEST(VideoSync, AFrameThatIsFarTooFarAheadIsDroppedNotDelayed) {
    VideoSyncController controller;
    // 500 ms of lead: waiting that long would itself be visible.
    const auto decision = controller.decide(500'000);
    EXPECT_EQ(decision.action, SyncAction::kDrop);
}

TEST(VideoSync, TheToleranceIsConfigurable) {
    SyncConfig config;
    config.tolerance_us = 5'000;
    VideoSyncController controller{config};

    EXPECT_EQ(controller.decide(4'000).action, SyncAction::kPresent);
    EXPECT_EQ(controller.decide(6'000).action, SyncAction::kDelay);
}

TEST(VideoSync, TheMaximumDelayIsConfigurable) {
    SyncConfig config;
    config.tolerance_us = 10'000;
    config.max_delay_us = 20'000;
    VideoSyncController controller{config};

    // 25 ms of lead: the hold is 15 ms, which is inside the 20 ms
    // maximum -- so the frame is delayed.
    EXPECT_EQ(controller.decide(25'000).action, SyncAction::kDelay);
    // 35 ms of lead: the hold is 25 ms, over the maximum -- so the frame
    // is dropped rather than held.
    EXPECT_EQ(controller.decide(35'000).action, SyncAction::kDrop);
}

TEST(VideoSync, TheCountersProveTheCorrectionHappens) {
    VideoSyncController controller;
    controller.decide(0);       // present
    controller.decide(60'000);  // delay
    controller.decide(-60'000); // drop

    const auto stats = controller.stats();
    EXPECT_EQ(stats.total, 3u);
    EXPECT_EQ(stats.presented, 1u);
    EXPECT_EQ(stats.delayed, 1u);
    EXPECT_EQ(stats.dropped, 1u);
    EXPECT_EQ(stats.last_skew_us, -60'000);
}

TEST(VideoSync, TheControllerCanBeDisabled) {
    SyncConfig config;
    config.enabled = false;
    VideoSyncController controller{config};

    // Even a wildly skewed frame is presented when the controller is off.
    EXPECT_EQ(controller.decide(500'000).action, SyncAction::kPresent);
    EXPECT_EQ(controller.decide(-500'000).action, SyncAction::kPresent);
}

TEST(VideoSync, ResetClearsTheCounters) {
    VideoSyncController controller;
    controller.decide(0);
    controller.reset();
    EXPECT_EQ(controller.stats().total, 0u);
}

// ---------------------------------------------------------------------------
// E9 — the displayed-frame tracker
// ---------------------------------------------------------------------------

namespace {

DisplayedFrameInfo make_frame(std::uint64_t id, std::int64_t capture_ts) {
    DisplayedFrameInfo info;
    info.ref_frame_id = id;
    info.capture_ts_us = capture_ts;
    info.present_ts_us = capture_ts + 20'000;
    info.frame_seq = static_cast<std::uint32_t>(id);
    return info;
}

} // namespace

TEST(DisplayedFrame, TheLastFrameIsTheOneOnScreen) {
    DisplayedFrameTracker tracker;
    EXPECT_FALSE(tracker.last().has_value());

    tracker.record(make_frame(1, 1000));
    tracker.record(make_frame(2, 2000));

    ASSERT_TRUE(tracker.last().has_value());
    EXPECT_EQ(tracker.last()->ref_frame_id, 2u);
    EXPECT_EQ(tracker.last()->capture_ts_us, 2000);
}

TEST(DisplayedFrame, AFrameIsFoundByItsRefFrameId) {
    DisplayedFrameTracker tracker;
    tracker.record(make_frame(1, 1000));
    tracker.record(make_frame(2, 2000));

    const auto found = tracker.find(1);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->capture_ts_us, 1000);

    const auto ts = tracker.capture_ts_for(2);
    ASSERT_TRUE(ts.has_value());
    EXPECT_EQ(*ts, 2000);
}

TEST(DisplayedFrame, AnUnknownIdIsNotFound) {
    DisplayedFrameTracker tracker;
    tracker.record(make_frame(1, 1000));
    EXPECT_FALSE(tracker.find(99).has_value());
    EXPECT_FALSE(tracker.capture_ts_for(99).has_value());
}

TEST(DisplayedFrame, TheHistoryIsBounded) {
    DisplayedFrameTracker tracker{4};
    EXPECT_EQ(tracker.capacity(), 4u);

    for (std::uint64_t i = 1; i <= 10; ++i) {
        tracker.record(make_frame(i, static_cast<std::int64_t>(i) * 1000));
    }

    EXPECT_EQ(tracker.size(), 4u);
    EXPECT_EQ(tracker.recorded_total(), 10u);
    EXPECT_EQ(tracker.evicted_total(), 6u);

    // The oldest entries fell out of the window.
    EXPECT_FALSE(tracker.find(1).has_value());
    EXPECT_FALSE(tracker.find(6).has_value());
    EXPECT_TRUE(tracker.find(7).has_value());
    EXPECT_TRUE(tracker.find(10).has_value());
}

TEST(DisplayedFrame, TheNewestMatchWins) {
    DisplayedFrameTracker tracker;
    tracker.record(make_frame(5, 1000));
    tracker.record(make_frame(5, 2000));

    const auto found = tracker.find(5);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->capture_ts_us, 2000);
}

TEST(DisplayedFrame, ClearEmptiesTheWindow) {
    DisplayedFrameTracker tracker;
    tracker.record(make_frame(1, 1000));
    tracker.clear();
    EXPECT_EQ(tracker.size(), 0u);
    EXPECT_EQ(tracker.recorded_total(), 0u);
    EXPECT_FALSE(tracker.last().has_value());
}

TEST(DisplayedFrame, AZeroCapacityTrackerStillRemembersOne) {
    DisplayedFrameTracker tracker{0};
    EXPECT_EQ(tracker.capacity(), 1u);
    tracker.record(make_frame(1, 1000));
    tracker.record(make_frame(2, 2000));
    EXPECT_EQ(tracker.size(), 1u);
    EXPECT_TRUE(tracker.find(2).has_value());
}

} // namespace
} // namespace nimrtc::video_sink
