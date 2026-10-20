/**
 * @file src/modules/video_source/tests/test_video_source.cpp
 * @brief Unit tests for the video_source module.
 *
 * Covers:
 *   1. Pattern name lookup
 *   2. render_pattern with solid color
 *   3. render_pattern with color bars (pixel signature)
 *   4. render_pattern with gradient
 *   5. render_pattern with frame counter overlay
 *   6. Buffer dimension / format mismatch → error
 *   7. MemoryVideoSource lifecycle: start / produce_one / stop
 *   8. Callback fires with correct metadata
 *   9. set_pattern at runtime
 *  10. set_callback replaces the previous callback
 *  11. Stats counters update
 *  12. Cadence (start + sleep + stats)
 *  13. Multiple consecutive frames have monotonically increasing frame_seq
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>
#include <nimrtc/video_frame/frame.hpp>
#include <nimrtc/video_source/video_source.hpp>

using nimrtc::video_source::Pattern;
using nimrtc::video_source::SourceConfig;
using nimrtc::video_source::create_memory_source;
using nimrtc::video_source::render_pattern;
using nimrtc::video_source::pattern_name;
using nimrtc::video_frame::VideoFrameBuffer;
using nimrtc::video_frame::VideoFrameLayout;
using nimrtc::video_frame::compute_layout;
using nimrtc::video_frame::PixelFormat;

// -----------------------------------------------------------------------------
// 1. Pattern names
// -----------------------------------------------------------------------------

TEST(PatternName, AllKnown) {
    EXPECT_EQ(pattern_name(Pattern::kSolidColor), "SolidColor");
    EXPECT_EQ(pattern_name(Pattern::kColorBars),  "ColorBars");
    EXPECT_EQ(pattern_name(Pattern::kGradient),   "Gradient");
    EXPECT_EQ(pattern_name(Pattern::kFrameCount), "FrameCount");
}

// -----------------------------------------------------------------------------
// 2. Solid color pattern
// -----------------------------------------------------------------------------

TEST(RenderPattern, SolidColor) {
    SourceConfig cfg;
    cfg.width = 32;
    cfg.height = 24;
    cfg.format = PixelFormat::kI420;
    cfg.pattern = Pattern::kSolidColor;
    cfg.solid_y = 200;
    cfg.solid_u = 100;
    cfg.solid_v = 50;
    cfg.start_frame_seq = 0;

    auto layout = compute_layout(cfg.format, cfg.width, cfg.height);
    VideoFrameBuffer buf(layout);
    nimrtc::video_frame::VideoFrameInfo info{};
    auto st = render_pattern(cfg, buf, info);
    ASSERT_TRUE(st.ok()) << st.error().message();

    EXPECT_EQ(info.frame_seq, 0u);
    auto* y = buf.plane(0);
    ASSERT_NE(y, nullptr);
    for (std::uint32_t i = 0; i < cfg.width * cfg.height; ++i) {
        EXPECT_EQ(y[i], 200);
    }
    auto* u = buf.plane(1);
    for (std::uint32_t i = 0; i < (cfg.width / 2) * (cfg.height / 2); ++i) {
        EXPECT_EQ(u[i], 100);
    }
    auto* v = buf.plane(2);
    for (std::uint32_t i = 0; i < (cfg.width / 2) * (cfg.height / 2); ++i) {
        EXPECT_EQ(v[i], 50);
    }
}

// -----------------------------------------------------------------------------
// 3. Color bars — sample a few pixels
// -----------------------------------------------------------------------------

TEST(RenderPattern, ColorBarsHasDistinctBars) {
    SourceConfig cfg;
    cfg.width = 64;       // 7 bars ~9px each
    cfg.height = 48;
    cfg.format = PixelFormat::kI420;
    cfg.pattern = Pattern::kColorBars;
    cfg.start_frame_seq = 0;

    auto layout = compute_layout(cfg.format, cfg.width, cfg.height);
    VideoFrameBuffer buf(layout);
    nimrtc::video_frame::VideoFrameInfo info{};
    ASSERT_TRUE(render_pattern(cfg, buf, info).ok());

    auto* y = buf.plane(0);
    // Sample a Y pixel from the first row of each bar.
    auto sample_y = [&](std::uint32_t bar) {
        return y[(cfg.width / 7) * bar + cfg.width / 14];
    };
    std::uint8_t prev = sample_y(0);
    for (std::uint32_t b = 1; b < 7; ++b) {
        std::uint8_t cur = sample_y(b);
        EXPECT_NE(cur, prev) << "bar " << b << " matches bar " << (b - 1);
        prev = cur;
    }
}

// -----------------------------------------------------------------------------
// 4. Gradient
// -----------------------------------------------------------------------------

TEST(RenderPattern, GradientIsMonotonic) {
    SourceConfig cfg;
    cfg.width = 32;
    cfg.height = 24;
    cfg.format = PixelFormat::kI420;
    cfg.pattern = Pattern::kGradient;
    cfg.start_frame_seq = 0;

    auto layout = compute_layout(cfg.format, cfg.width, cfg.height);
    VideoFrameBuffer buf(layout);
    nimrtc::video_frame::VideoFrameInfo info{};
    ASSERT_TRUE(render_pattern(cfg, buf, info).ok());

    auto* y = buf.plane(0);
    std::uint8_t first = y[0];
    std::uint8_t last  = y[(cfg.width - 1)];
    // Gradient should have black-ish on the left and white-ish on the right.
    EXPECT_LE(first, 32u);
    EXPECT_GE(last, 200u);
}

// -----------------------------------------------------------------------------
// 5. Frame counter overlay
// -----------------------------------------------------------------------------

TEST(RenderPattern, FrameCountOverlay) {
    SourceConfig cfg;
    cfg.width = 64;
    cfg.height = 48;
    cfg.format = PixelFormat::kI420;
    cfg.pattern = Pattern::kFrameCount;
    cfg.start_frame_seq = 1234;

    auto layout = compute_layout(cfg.format, cfg.width, cfg.height);
    VideoFrameBuffer buf(layout);
    nimrtc::video_frame::VideoFrameInfo info{};
    ASSERT_TRUE(render_pattern(cfg, buf, info).ok());
    EXPECT_EQ(info.frame_seq, 1234u);
    // The overlay writes white (235) pixels at the top-left — check at least
    // one white pixel exists.
    auto* y = buf.plane(0);
    bool found_white = false;
    for (std::uint32_t i = 0; i < cfg.width * 16; ++i) {  // top 16 rows
        if (y[i] >= 200) { found_white = true; break; }
    }
    EXPECT_TRUE(found_white);
}

// -----------------------------------------------------------------------------
// 6. Format / dimension mismatch
// -----------------------------------------------------------------------------

TEST(RenderPattern, FormatMismatchReturnsError) {
    SourceConfig cfg;
    cfg.width = 32;
    cfg.height = 24;
    cfg.format = PixelFormat::kI420;
    cfg.start_frame_seq = 0;

    auto wrong_layout = compute_layout(PixelFormat::kBGRA, 32, 24);
    VideoFrameBuffer buf(wrong_layout);
    nimrtc::video_frame::VideoFrameInfo info{};
    auto st = render_pattern(cfg, buf, info);
    EXPECT_FALSE(st.ok());
    EXPECT_EQ(st.error().code(), nimrtc::core::ErrorCode::InvalidArgument);
}

// -----------------------------------------------------------------------------
// 7. Lifecycle: start / produce_one / stop
//
// The producer thread is woken from its `cv_.wait_until()` immediately
// on stop() via `cv_.notify_all()` (see `cadence_loop()` in
// `video_source.cpp`), so it exits deterministically within a few
// scheduling slices rather than after at most one frame interval
// (33 ms at 30 fps). Tests verify the producer has fully stopped via
// `wait_until_idle(500ms)` instead of the old "sleep 1.5 s and
// re-read the counter" approach, which was a recognised flake on busy
// CI runners (see the comment block that used to live here in
// v0.11.0–v0.13.0).
// -----------------------------------------------------------------------------

TEST(MemorySource, StartProduceStopLifecycle) {
    SourceConfig cfg;
    cfg.width = 32;
    cfg.height = 24;
    cfg.format = PixelFormat::kI420;
    cfg.fps = 30;
    cfg.pattern = Pattern::kSolidColor;
    cfg.solid_y = 100;

    std::atomic<int> count{0};
    auto src = create_memory_source(cfg,
        [&](VideoFrameBuffer, nimrtc::video_frame::VideoFrameInfo) {
            count.fetch_add(1);
        });
    ASSERT_NE(src, nullptr);
    EXPECT_FALSE(src->running());
    src->produce_one();
    EXPECT_EQ(count.load(), 1);
    src->start();
    EXPECT_TRUE(src->running());
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    src->stop();
    // Deterministic: the cadence loop wakes immediately on stop() and
    // exits within one or two scheduling slices. 500 ms is a generous
    // upper bound; in practice wait_until_idle() returns in well under
    // 1 ms on any modern host.
    const bool exited = src->wait_until_idle(std::chrono::milliseconds(500));
    EXPECT_TRUE(exited)
        << "cadence loop did not exit within 500 ms of stop(); the "
           "stop() → cv_.notify_all() → exit path is broken";
    int after_stop = count.load();
    EXPECT_GE(after_stop, 1);
    // The producer thread has exited, so the count is guaranteed stable
    // — no more frames will be produced (the loop has terminated, so
    // there is no race with future ticks).
    EXPECT_EQ(count.load(), after_stop)
        << "count changed after wait_until_idle(); the producer thread "
           "isn't actually stopped";
}

// -----------------------------------------------------------------------------
// 8. Callback receives correct metadata
// -----------------------------------------------------------------------------

TEST(MemorySource, CallbackReceivesCorrectMetadata) {
    SourceConfig cfg;
    cfg.width = 32;
    cfg.height = 24;
    cfg.format = PixelFormat::kI420;
    cfg.fps = 30;
    cfg.pattern = Pattern::kColorBars;
    cfg.start_frame_seq = 100;
    cfg.start_capture_ts_us = 12345;

    nimrtc::video_frame::VideoFrameInfo last_info{};
    VideoFrameBuffer last_buf;
    auto src = create_memory_source(cfg,
        [&](VideoFrameBuffer b, nimrtc::video_frame::VideoFrameInfo info) {
            last_buf = std::move(b);
            last_info = info;
        });
    src->produce_one();
    EXPECT_EQ(last_info.frame_seq, 100u);
    EXPECT_EQ(last_info.capture_ts_us, 12345);
    EXPECT_GT(last_info.rtp_timestamp, 0u);
    EXPECT_TRUE(last_buf.valid());
    EXPECT_EQ(last_buf.width(), 32u);
    EXPECT_EQ(last_buf.height(), 24u);
}

// -----------------------------------------------------------------------------
// 9. set_pattern at runtime
// -----------------------------------------------------------------------------

TEST(MemorySource, SetPatternAtRuntime) {
    SourceConfig cfg;
    cfg.width = 32;
    cfg.height = 24;
    cfg.format = PixelFormat::kI420;
    cfg.fps = 30;
    cfg.pattern = Pattern::kSolidColor;
    cfg.solid_y = 50;

    auto src = create_memory_source(cfg, [](VideoFrameBuffer, auto) {});
    src->set_pattern(Pattern::kGradient);
    EXPECT_EQ(src->config().pattern, Pattern::kGradient);
}

// -----------------------------------------------------------------------------
// 10. set_callback replaces the previous callback
// -----------------------------------------------------------------------------

TEST(MemorySource, SetCallbackReplacesPrevious) {
    SourceConfig cfg;
    cfg.width = 32;
    cfg.height = 24;
    cfg.fps = 30;
    cfg.pattern = Pattern::kSolidColor;
    cfg.solid_y = 50;

    int a = 0, b = 0;
    auto src = create_memory_source(cfg, [&](VideoFrameBuffer, auto) { ++a; });
    src->set_callback([&](VideoFrameBuffer, auto) { ++b; });
    src->produce_one();
    src->produce_one();
    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 2);
}

// -----------------------------------------------------------------------------
// 11. Stats counters
// -----------------------------------------------------------------------------

TEST(MemorySource, StatsUpdate) {
    SourceConfig cfg;
    cfg.width = 32;
    cfg.height = 24;
    cfg.fps = 30;
    cfg.pattern = Pattern::kSolidColor;
    cfg.solid_y = 50;

    auto src = create_memory_source(cfg, [](VideoFrameBuffer, auto) {});
    EXPECT_EQ(src->stats().frames_produced, 0u);
    for (int i = 0; i < 5; ++i) src->produce_one();
    EXPECT_EQ(src->stats().frames_produced, 5u);
}

// -----------------------------------------------------------------------------
// 12. Cadence — start, sleep, count frames
// -----------------------------------------------------------------------------

TEST(MemorySource, CadenceProducesApproxFps) {
    SourceConfig cfg;
    cfg.width = 32;
    cfg.height = 24;
    cfg.fps = 50;
    cfg.pattern = Pattern::kSolidColor;
    cfg.solid_y = 50;

    std::atomic<int> count{0};
    auto src = create_memory_source(cfg,
        [&](VideoFrameBuffer, auto) { count.fetch_add(1); });
    src->start();
    // Wait for the producer thread to deliver its first frame before
    // starting the production window. macOS CI runners have high
    // std::thread + sleep_until warm-up latency (observed up to ~360 ms
    // on aarch64 hosted GitHub Actions), so a fixed-duration sleep after
    // start() races against cold-start and produces 0-7 frames even when
    // the cadence is correct. Polling for the first frame removes the
    // cold-start variable: once we observe a frame, the producer is
    // running and we can time a stable window of cadence.
    using clk = std::chrono::steady_clock;
    const auto warm_deadline = clk::now() + std::chrono::seconds(2);
    while (count.load(std::memory_order_acquire) == 0 &&
           clk::now() < warm_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GE(count.load(std::memory_order_acquire), 1)
        << "Producer did not deliver a frame within 2 s of start()";
    // Reset the counter and time a known production window. 50 fps ×
    // 500 ms = 25 frames nominal; allow a wide range so the test catches
    // a dead producer (n=0) and a runaway producer (n huge) without
    // flaking on slow CI.
    count.store(0, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    src->stop();
    int n = count.load(std::memory_order_acquire);
    EXPECT_GE(n, 8);
    EXPECT_LE(n, 60);
}

// -----------------------------------------------------------------------------
// 13. frame_seq is monotonically increasing
// -----------------------------------------------------------------------------

TEST(MemorySource, FrameSeqMonotonic) {
    SourceConfig cfg;
    cfg.width = 32;
    cfg.height = 24;
    cfg.fps = 30;
    cfg.pattern = Pattern::kSolidColor;
    cfg.solid_y = 50;
    cfg.start_frame_seq = 1000;

    std::vector<std::uint32_t> seqs;
    auto src = create_memory_source(cfg,
        [&](VideoFrameBuffer, nimrtc::video_frame::VideoFrameInfo info) {
            seqs.push_back(info.frame_seq);
        });
    for (int i = 0; i < 10; ++i) src->produce_one();
    ASSERT_EQ(seqs.size(), 10u);
    for (std::size_t i = 1; i < seqs.size(); ++i) {
        EXPECT_EQ(seqs[i], seqs[i - 1] + 1) << "non-monotonic at " << i;
    }
    EXPECT_EQ(seqs.front(), 1000u);
    EXPECT_EQ(seqs.back(),  1009u);
}
