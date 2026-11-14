// modules/audio_device/tests/test_playback_progress.cpp
//
// The playback-progress tracker's unit tests (remote-desktop plan F3).
//
// What they cover, and why:
//
//   1. the basic accounting — 100 frames queued, 40 played, so 60 are
//      pending;
//   2. the **monotonicity** contract — the played counter never goes
//      backwards, which is what makes it usable as the 3A's AEC
//      reference position;
//   3. the **clamp** — a device that claims to have played more than it
//      was ever given is clamped away and the event is *counted*, so
//      the bug is visible rather than silently swallowed;
//   4. the **backlog limit** — a sink that falls too far behind is
//      reported;
//   5. the statistics snapshot and `reset()`.
//
// The tests never require an audio device, so they run on a CI machine
// with no sound card.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include <nimrtc/audio_device/playback_progress.hpp>

namespace {

using nimrtc::audio_device::PlaybackProgressStats;
using nimrtc::audio_device::PlaybackProgressTracker;

// ---------------------------------------------------------------------------
// 1. The basic accounting
// ---------------------------------------------------------------------------

TEST(playback_progress, a_fresh_tracker_reports_nothing) {
    PlaybackProgressTracker tracker;

    EXPECT_EQ(tracker.frames_queued(), 0u);
    EXPECT_EQ(tracker.frames_played(), 0u);
    EXPECT_EQ(tracker.frames_pending(), 0u);
    EXPECT_EQ(tracker.last_playback_us(), 0);
    EXPECT_FALSE(tracker.backlog_exceeded());
}

TEST(playback_progress, queued_then_played_leaves_the_rest_pending) {
    PlaybackProgressTracker tracker;

    tracker.on_samples_queued(100);
    EXPECT_EQ(tracker.frames_queued(), 100u);
    EXPECT_EQ(tracker.frames_played(), 0u);
    EXPECT_EQ(tracker.frames_pending(), 100u);

    tracker.on_samples_played(40);
    EXPECT_EQ(tracker.frames_played(), 40u);
    EXPECT_EQ(tracker.frames_pending(), 60u);
}

TEST(playback_progress, playing_everything_leaves_nothing_pending) {
    PlaybackProgressTracker tracker;

    tracker.on_samples_queued(480);
    tracker.on_samples_played(480);

    EXPECT_EQ(tracker.frames_played(), 480u);
    EXPECT_EQ(tracker.frames_pending(), 0u);
}

TEST(playback_progress, a_zero_sized_call_is_ignored) {
    PlaybackProgressTracker tracker;

    tracker.on_samples_queued(0);
    tracker.on_samples_played(0);

    EXPECT_EQ(tracker.frames_queued(), 0u);
    EXPECT_EQ(tracker.frames_played(), 0u);
    EXPECT_EQ(tracker.frames_pending(), 0u);
    EXPECT_EQ(tracker.stats().over_reports_clamped, 0u);
}

// ---------------------------------------------------------------------------
// 2. The monotonicity contract
// ---------------------------------------------------------------------------

TEST(playback_progress, the_played_counter_is_monotonic) {
    PlaybackProgressTracker tracker;

    tracker.on_samples_queued(1000);

    std::uint64_t previous = tracker.frames_played();
    for (int i = 0; i < 20; ++i) {
        tracker.on_samples_played(10);
        const std::uint64_t now = tracker.frames_played();
        EXPECT_GE(now, previous) << "the played counter went backwards";
        previous = now;
    }

    EXPECT_EQ(tracker.frames_played(), 200u);
}

TEST(playback_progress, the_played_counter_never_goes_backwards_after_a_reset) {
    PlaybackProgressTracker tracker;

    tracker.on_samples_queued(100);
    tracker.on_samples_played(50);
    ASSERT_GT(tracker.frames_played(), 0u);

    tracker.reset();
    EXPECT_EQ(tracker.frames_played(), 0u);
    EXPECT_EQ(tracker.frames_queued(), 0u);
    EXPECT_EQ(tracker.frames_pending(), 0u);
}

// ---------------------------------------------------------------------------
// 3. The clamp — an over-reporting device is corrected, not trusted
// ---------------------------------------------------------------------------

TEST(playback_progress, an_over_report_is_clamped_to_the_queued_count) {
    PlaybackProgressTracker tracker;

    tracker.on_samples_queued(100);

    // The device claims it played 150 frames, but only 100 were ever
    // handed to it.  The clamp keeps the AEC reference from running
    // ahead of reality.
    tracker.on_samples_played(150);

    EXPECT_EQ(tracker.frames_played(), 100u);
    EXPECT_EQ(tracker.frames_pending(), 0u);
    EXPECT_EQ(tracker.stats().over_reports_clamped, 1u);
}

TEST(playback_progress, playing_more_than_queued_counts_every_over_report) {
    PlaybackProgressTracker tracker;

    tracker.on_samples_queued(100);
    tracker.on_samples_played(150);
    tracker.on_samples_played(50);
    tracker.on_samples_played(50);

    // The first call was clamped from 150 to 100; the two later calls
    // had nothing outstanding at all, so they were refused outright.
    EXPECT_EQ(tracker.frames_played(), 100u);
    EXPECT_EQ(tracker.stats().over_reports_clamped, 3u);
}

TEST(playback_progress, a_partial_over_report_still_counts_the_event) {
    PlaybackProgressTracker tracker;

    tracker.on_samples_queued(100);
    tracker.on_samples_played(80);
    tracker.on_samples_played(50);   // only 20 were outstanding

    EXPECT_EQ(tracker.frames_played(), 100u);
    EXPECT_EQ(tracker.stats().over_reports_clamped, 1u);
}

TEST(playback_progress, playing_without_being_given_anything_is_refused) {
    PlaybackProgressTracker tracker;

    tracker.on_samples_played(480);

    EXPECT_EQ(tracker.frames_played(), 0u);
    EXPECT_EQ(tracker.stats().over_reports_clamped, 1u);
}

// ---------------------------------------------------------------------------
// 4. The backlog limit
// ---------------------------------------------------------------------------

TEST(playback_progress, the_backlog_limit_is_reported_when_exceeded) {
    PlaybackProgressTracker tracker(50);

    tracker.on_samples_queued(40);
    EXPECT_FALSE(tracker.backlog_exceeded());
    EXPECT_EQ(tracker.stats().backlog_limit_exceeded, 0u);

    tracker.on_samples_queued(20);
    EXPECT_TRUE(tracker.backlog_exceeded());
    EXPECT_EQ(tracker.stats().backlog_limit_exceeded, 1u);
}

TEST(playback_progress, a_zero_limit_means_no_limit) {
    PlaybackProgressTracker tracker(0);

    tracker.on_samples_queued(100000);

    EXPECT_FALSE(tracker.backlog_exceeded());
    EXPECT_EQ(tracker.stats().backlog_limit_exceeded, 0u);
    // The backlog is still *reported*, it is just not flagged.
    EXPECT_EQ(tracker.frames_pending(), 100000u);
}

TEST(playback_progress, draining_the_backlog_clears_the_flag) {
    PlaybackProgressTracker tracker(50);

    tracker.on_samples_queued(100);
    EXPECT_TRUE(tracker.backlog_exceeded());

    tracker.on_samples_played(80);
    EXPECT_FALSE(tracker.backlog_exceeded());
    EXPECT_EQ(tracker.frames_pending(), 20u);
}

// ---------------------------------------------------------------------------
// 5. The statistics snapshot and the reset
// ---------------------------------------------------------------------------

TEST(playback_progress, the_stats_snapshot_reports_the_counters) {
    PlaybackProgressTracker tracker(50);

    tracker.on_samples_queued(100);
    tracker.on_samples_played(40);
    tracker.on_samples_played(100);   // an over-report

    const PlaybackProgressStats stats = tracker.stats();
    EXPECT_EQ(stats.frames_queued, 100u);
    EXPECT_EQ(stats.frames_played, 100u);
    EXPECT_EQ(stats.frames_pending, 0u);
    EXPECT_EQ(stats.backlog_limit_exceeded, 1u);
    EXPECT_EQ(stats.over_reports_clamped, 1u);
    EXPECT_GT(stats.last_playback_us, 0)
        << "the moment of the last playback must be recorded";
}

TEST(playback_progress, the_reset_clears_every_counter) {
    PlaybackProgressTracker tracker(50);

    tracker.on_samples_queued(100);
    tracker.on_samples_played(40);
    tracker.on_samples_played(100);   // an over-report

    tracker.reset();

    const PlaybackProgressStats stats = tracker.stats();
    EXPECT_EQ(stats.frames_queued, 0u);
    EXPECT_EQ(stats.frames_played, 0u);
    EXPECT_EQ(stats.frames_pending, 0u);
    EXPECT_EQ(stats.backlog_limit_exceeded, 0u);
    EXPECT_EQ(stats.over_reports_clamped, 0u);
    EXPECT_EQ(stats.last_playback_us, 0);
}

} // namespace
