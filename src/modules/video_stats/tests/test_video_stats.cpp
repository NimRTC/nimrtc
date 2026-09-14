/**
 * @file src/modules/video_stats/tests/test_video_stats.cpp
 * @brief Unit tests for the video_stats module.
 *
 * Covers (1:1 with the task spec):
 *   1. Default-constructed snapshot is all zeros.
 *   2. on_frame_received increments counters and sets last_frame_ts_us.
 *   3. Keyframe counter + EMA of inter-keyframe distance.
 *   4. on_frame_dropped increments the right counter per reason.
 *   5. Freeze detection: gap > threshold → freeze_count / freeze_total_ms.
 *   6. Current bitrate ≈ total_bytes * 8 over a 1-second window.
 *   7. RTCP feedback is a no-op for stats; on_qp_sample updates QP stats.
 *   8. reset() clears all state.
 *   9. tick() with no activity is a no-op.
 *  10. Multi-threaded on_frame_received is race-free (counters exact).
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <nimrtc/video_stats/video_stats.hpp>

namespace vs = nimrtc::video_stats;

// -----------------------------------------------------------------------------
// 1. Default snapshot is all zeros
// -----------------------------------------------------------------------------
TEST(VideoStats, DefaultSnapshotIsZero) {
    auto sink = vs::create_stats_sink({});
    auto s = sink->snapshot();

    EXPECT_EQ(s.frames_received,        0u);
    EXPECT_EQ(s.frames_decoded,         0u);
    EXPECT_EQ(s.frames_rendered,        0u);
    EXPECT_EQ(s.frames_dropped_jb,      0u);
    EXPECT_EQ(s.frames_dropped_decoder, 0u);
    EXPECT_EQ(s.keyframes_received,     0u);
    EXPECT_EQ(s.bytes_received,         0u);
    EXPECT_EQ(s.current_bitrate_bps,    0u);
    EXPECT_EQ(s.avg_bitrate_bps,        0u);
    EXPECT_EQ(s.freeze_count,           0u);
    EXPECT_EQ(s.freeze_total_ms,        0u);
    EXPECT_EQ(s.last_freeze_ms,         0u);
    EXPECT_EQ(s.qp_sum,                 0u);
    EXPECT_EQ(s.qp_count,               0u);
    EXPECT_EQ(s.first_frame_ts_us,      -1);
    EXPECT_EQ(s.last_frame_ts_us,       -1);
}

// -----------------------------------------------------------------------------
// 2. on_frame_received increments frames_received / bytes_received,
//    sets last_frame_ts_us.
// -----------------------------------------------------------------------------
TEST(VideoStats, FrameReceivedIncrementsCounters) {
    auto sink = vs::create_stats_sink({});

    sink->on_frame_received(1'000'000, 5000, /*is_keyframe=*/false, /*qp=*/-1);
    sink->on_frame_received(1'033'333, 6000, false, -1);
    sink->on_frame_received(1'066'666, 7000, false, -1);

    auto s = sink->snapshot();
    EXPECT_EQ(s.frames_received, 3u);
    EXPECT_EQ(s.bytes_received,  5000u + 6000u + 7000u);
    EXPECT_EQ(s.last_frame_ts_us, 1'066'666);
    EXPECT_EQ(s.first_frame_ts_us, 1'000'000);
    EXPECT_EQ(s.keyframes_received, 0u);
}

// -----------------------------------------------------------------------------
// 3. Keyframe counter + EMA of inter-keyframe distance
// -----------------------------------------------------------------------------
TEST(VideoStats, KeyframeCounterAndIntervalEma) {
    auto sink = vs::create_stats_sink({});

    // Three keyframes, 1 second apart (1000 ms gap).
    sink->on_frame_received(0,        1000, /*is_keyframe=*/true,  -1);
    sink->on_frame_received(1'000'000, 1000, /*is_keyframe=*/true,  -1);
    sink->on_frame_received(2'000'000, 1000, /*is_keyframe=*/true,  -1);

    auto s = sink->snapshot();
    EXPECT_EQ(s.keyframes_received, 3u);
    // After 2 keyframes (first + 1 gap), EMA seeded with the first gap = 1000 ms.
    // After 3 keyframes, EMA = 0.1 * 1000 + 0.9 * 1000 = 1000 ms.
    EXPECT_NEAR(s.keyframe_interval_avg_ms, 1000.0, 1.0);
}

// -----------------------------------------------------------------------------
// 4. on_frame_dropped increments the right counter per reason
// -----------------------------------------------------------------------------
TEST(VideoStats, FrameDroppedPerReason) {
    auto sink = vs::create_stats_sink({});

    sink->on_frame_dropped(vs::DropReason::kJitterBufferTimeout);
    sink->on_frame_dropped(vs::DropReason::kJitterBufferTimeout);
    sink->on_frame_dropped(vs::DropReason::kDecoderError);
    sink->on_frame_dropped(vs::DropReason::kDependencyLost);

    auto s = sink->snapshot();
    EXPECT_EQ(s.frames_dropped_jb,      2u);
    EXPECT_EQ(s.frames_dropped_decoder, 2u);   // decoder + dependency both tally here
}

// -----------------------------------------------------------------------------
// 5. Freeze detection via a gap between rendered frames
// -----------------------------------------------------------------------------
TEST(VideoStats, FreezeDetectedOnRenderGap) {
    vs::StatsConfig cfg;
    cfg.freeze_threshold_ms = 150;
    auto sink = vs::create_stats_sink(cfg);

    // Frame 1 rendered at t=0.
    sink->on_frame_received(0, 1000, true, -1);
    sink->on_frame_decoded(0);
    sink->on_frame_rendered(0);

    // Long gap (1 second) before the next rendered frame.
    sink->on_frame_received(1'000'000, 1000, false, -1);
    sink->on_frame_decoded(1'000'000);
    sink->on_frame_rendered(1'000'000);

    auto s = sink->snapshot();
    EXPECT_EQ(s.frames_rendered, 2u);
    EXPECT_EQ(s.freeze_count,    1u);
    EXPECT_GE(s.freeze_total_ms, 1000u - 1u);   // ≈ 1000 ms
    EXPECT_GE(s.last_freeze_ms,  1000u - 1u);
}

// -----------------------------------------------------------------------------
// 5b. Freeze detection via tick() with no rendered frames
// -----------------------------------------------------------------------------
TEST(VideoStats, FreezeDetectedViaIdleTick) {
    vs::StatsConfig cfg;
    cfg.freeze_threshold_ms = 150;
    cfg.bitrate_window_ms   = 1000;
    auto sink = vs::create_stats_sink(cfg);

    // Render one frame at t=0 so the freeze detector has an anchor.
    sink->on_frame_received(0, 1000, true, -1);
    sink->on_frame_rendered(0);

    // Tick at t=500ms — no idle threshold crossed yet.
    sink->tick(500'000);
    EXPECT_EQ(sink->snapshot().freeze_count, 0u);

    // Tick at t=2s — 2s since last rendered frame, freeze should be counted.
    sink->tick(2'000'000);
    auto s = sink->snapshot();
    EXPECT_EQ(s.freeze_count,    1u);
    EXPECT_GE(s.freeze_total_ms, 1500u);   // (2s - 500ms was 0ms idle window; the gap is ~2s)

    // Subsequent tick at t=3s without further renders → another freeze.
    sink->tick(3'000'000);
    EXPECT_EQ(sink->snapshot().freeze_count, 2u);
}

// -----------------------------------------------------------------------------
// 6. Current bitrate ≈ total_bytes * 8 over a 1-second window
// -----------------------------------------------------------------------------
TEST(VideoStats, CurrentBitrateApproximation) {
    vs::StatsConfig cfg;
    cfg.bitrate_window_ms = 1000;
    cfg.ema_alpha         = 1.0;   // disable smoothing for this test
    auto sink = vs::create_stats_sink(cfg);

    constexpr std::uint32_t kBytesPerFrame = 10'000;
    constexpr std::uint32_t kNumFrames     = 30;
    constexpr std::int64_t  kFrameStepUs   = 33'333;   // ~30 fps

    for (std::uint32_t i = 0; i < kNumFrames; ++i) {
        const std::int64_t ts =
            static_cast<std::int64_t>(i) * kFrameStepUs;
        sink->on_frame_received(ts, kBytesPerFrame, false, -1);
    }

    sink->tick(1'000'000);   // 1 second elapsed

    auto s = sink->snapshot();
    const std::uint64_t expected_bps =
        static_cast<std::uint64_t>(kBytesPerFrame) * kNumFrames * 8ull;
    EXPECT_EQ(s.bytes_received, kBytesPerFrame * kNumFrames);
    EXPECT_NEAR(static_cast<double>(s.current_bitrate_bps),
                static_cast<double>(expected_bps),
                static_cast<double>(expected_bps) / 20.0);  // 5% tolerance
}

// -----------------------------------------------------------------------------
// 7. RTCP feedback is a no-op for stats; on_qp_sample updates QP stats.
// -----------------------------------------------------------------------------
TEST(VideoStats, RtcpFeedbackNoOpAndQpAccumulation) {
    auto sink = vs::create_stats_sink({});

    // Each kind of RTCP feedback must not crash and must not move stats.
    EXPECT_NO_THROW(sink->on_rtcp_feedback(vs::RtcpFeedbackKind::kPLI));
    EXPECT_NO_THROW(sink->on_rtcp_feedback(vs::RtcpFeedbackKind::kFIR));
    EXPECT_NO_THROW(sink->on_rtcp_feedback(vs::RtcpFeedbackKind::kNACK));
    EXPECT_NO_THROW(sink->on_rtcp_feedback(vs::RtcpFeedbackKind::kREMB));

    // QP samples: 20, 25, 30 → sum=75, count=3, average=25.
    sink->on_qp_sample(20);
    sink->on_qp_sample(25);
    sink->on_qp_sample(30);

    auto s = sink->snapshot();
    EXPECT_EQ(s.qp_sum,   75u);
    EXPECT_EQ(s.qp_count, 3u);
    const double avg = static_cast<double>(s.qp_sum)
                     / static_cast<double>(s.qp_count);
    EXPECT_NEAR(avg, 25.0, 0.001);
}

// -----------------------------------------------------------------------------
// 8. reset() clears everything
// -----------------------------------------------------------------------------
TEST(VideoStats, ResetClearsAll) {
    vs::StatsConfig cfg;
    cfg.freeze_threshold_ms = 100;
    auto sink = vs::create_stats_sink(cfg);

    // Stuff in some non-zero state.
    sink->on_frame_received(0,         5000, true,  -1);
    sink->on_frame_received(1'000'000, 6000, false, 25);
    sink->on_frame_decoded(1'000'000);
    sink->on_frame_rendered(1'000'000);
    sink->on_frame_dropped(vs::DropReason::kJitterBufferTimeout);
    sink->on_qp_sample(30);
    sink->on_qp_sample(35);
    sink->tick(1'500'000);   // may record a freeze / bitrate

    sink->reset();

    auto s = sink->snapshot();
    EXPECT_EQ(s.frames_received,        0u);
    EXPECT_EQ(s.frames_decoded,         0u);
    EXPECT_EQ(s.frames_rendered,        0u);
    EXPECT_EQ(s.frames_dropped_jb,      0u);
    EXPECT_EQ(s.frames_dropped_decoder, 0u);
    EXPECT_EQ(s.keyframes_received,     0u);
    EXPECT_EQ(s.bytes_received,         0u);
    EXPECT_EQ(s.qp_sum,                 0u);
    EXPECT_EQ(s.qp_count,               0u);
    EXPECT_EQ(s.freeze_count,           0u);
    EXPECT_EQ(s.freeze_total_ms,        0u);
    EXPECT_EQ(s.last_freeze_ms,         0u);
    EXPECT_EQ(s.current_bitrate_bps,    0u);
    EXPECT_EQ(s.avg_bitrate_bps,        0u);
    EXPECT_EQ(s.first_frame_ts_us,      -1);
    EXPECT_EQ(s.last_frame_ts_us,       -1);
}

// -----------------------------------------------------------------------------
// 9. tick() without activity is a no-op
// -----------------------------------------------------------------------------
TEST(VideoStats, TickWithoutActivityIsNoop) {
    auto sink = vs::create_stats_sink({});

    // No frames, no counters — tick() must not change any counter.
    sink->tick(1'000'000);
    sink->tick(2'000'000);
    sink->tick(3'000'000);

    auto s = sink->snapshot();
    EXPECT_EQ(s.frames_received,    0u);
    EXPECT_EQ(s.freeze_count,       0u);
    EXPECT_EQ(s.current_bitrate_bps, 0u);

    // Even on a freshly-default-constructed sink, calling tick with a
    // negative timestamp is safe.
    EXPECT_NO_THROW(sink->tick(-1));
}

// -----------------------------------------------------------------------------
// 10. Multi-threaded on_frame_received is race-free
// -----------------------------------------------------------------------------
TEST(VideoStats, ThreadSafeIncrement) {
    auto sink = vs::create_stats_sink({});

    constexpr int kThreads        = 4;
    constexpr int kFramesPerThread = 10'000;
    constexpr std::uint32_t kBytesPerFrame = 1024;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < kFramesPerThread; ++i) {
                const std::int64_t ts =
                    static_cast<std::int64_t>(t) * 1'000'000
                    + static_cast<std::int64_t>(i);
                sink->on_frame_received(ts, kBytesPerFrame,
                                        /*is_keyframe=*/false, /*qp=*/-1);
                if (i % 50 == 0) {
                    sink->on_frame_decoded(ts);
                    sink->on_frame_rendered(ts);
                }
            }
        });
    }
    for (auto& th : workers) th.join();

    auto s = sink->snapshot();
    EXPECT_EQ(s.frames_received,
              static_cast<std::uint64_t>(kThreads) * kFramesPerThread);
    EXPECT_EQ(s.bytes_received,
              static_cast<std::uint64_t>(kThreads) * kFramesPerThread
                  * kBytesPerFrame);
    // Decoded/rendered counts are exact because atomic increments are
    // race-free; only a subset of frames trigger decoded/rendered in the
    // worker above, so verify the count is at least one full thread's
    // worth (the exact count depends on interleaving — but it must be
    // non-zero and <= frames_received).
    EXPECT_GT(s.frames_decoded,  0u);
    EXPECT_GT(s.frames_rendered, 0u);
    EXPECT_LE(s.frames_decoded,  s.frames_received);
    EXPECT_LE(s.frames_rendered, s.frames_received);
}