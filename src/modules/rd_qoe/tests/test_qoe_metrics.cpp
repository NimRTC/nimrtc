// src/modules/rd_qoe/tests/test_qoe_metrics.cpp
// ----------------------------------------------------------------------------
// The remote-desktop QoE module's unit tests (plan Track J, J3 / J6).
//
// Six suites, one per acceptance criterion:
//
//   qoe_freeze_*             J3: the freeze count / duration accounting
//   qoe_presented_fps_*      J3: the frame rate the *viewer* got
//   qoe_latency_percentile_* J3: the end-to-end latency percentiles and
//                            the estimator's memory bound
//   qoe_input_to_photon_*    J6: the input event -> photon pairing
//   qoe_sharpness_*          J3: the sharpness proxy's ordering
//   qoe_bound_*              J3: the collector's own boundedness
// ----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <nimrtc/rd_qoe/qoe_metrics.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

using nimrtc::rd_qoe::FrameObservation;
using nimrtc::rd_qoe::PercentileEstimator;
using nimrtc::rd_qoe::QoeCollector;
using nimrtc::rd_qoe::QoeConfig;
using nimrtc::rd_qoe::QoeSnapshot;
using nimrtc::rd_qoe::SharpnessEstimator;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// A frame observation with a uniform end-to-end latency.
FrameObservation make_frame(std::int64_t capture_ts_us,
                             std::int64_t render_ts_us,
                             std::uint64_t frame_id = 0) {
    FrameObservation obs;
    obs.capture_ts_us = capture_ts_us;
    obs.render_ts_us = render_ts_us;
    obs.frame_id = frame_id;
    obs.ref_frame_id = frame_id;
    obs.source_fps = 30.0;
    return obs;
}

/// A luma plane of `width` x `height` bytes, initialised to `value`.
std::vector<std::uint8_t> make_plane(std::size_t width, std::size_t height,
                                      std::uint8_t value = 0) {
    return std::vector<std::uint8_t>(width * height, value);
}

/// A hard black/white vertical edge: the left half is 0, the right half
/// is 255.  This is the "sharp" picture.
std::vector<std::uint8_t> make_sharp_edge(std::size_t width, std::size_t height) {
    std::vector<std::uint8_t> plane(width * height, 0);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = width / 2; x < width; ++x) {
            plane[y * width + x] = 255;
        }
    }
    return plane;
}

/// A 5x5 box filter with edge clamping.  The same content, blurred.
std::vector<std::uint8_t> box_blur(const std::vector<std::uint8_t>& src,
                                   std::size_t width, std::size_t height,
                                   std::size_t radius = 2) {
    // Horizontal pass, then vertical pass — separable, and each pass
    // clamps at the edges so the result is exactly what the plain
    // 2-D box filter would give.
    const std::size_t window = 2 * radius + 1;

    std::vector<double> tmp(width * height, 0.0);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            double sum = 0.0;
            for (std::size_t k = 0; k < window; ++k) {
                long sx = static_cast<long>(x) + static_cast<long>(k)
                          - static_cast<long>(radius);
                if (sx < 0) sx = 0;
                if (sx >= static_cast<long>(width)) sx = static_cast<long>(width) - 1;
                sum += src[y * width + static_cast<std::size_t>(sx)];
            }
            tmp[y * width + x] = sum / static_cast<double>(window);
        }
    }

    std::vector<std::uint8_t> dst(width * height, 0);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            double sum = 0.0;
            for (std::size_t k = 0; k < window; ++k) {
                long sy = static_cast<long>(y) + static_cast<long>(k)
                          - static_cast<long>(radius);
                if (sy < 0) sy = 0;
                if (sy >= static_cast<long>(height)) sy = static_cast<long>(height) - 1;
                sum += tmp[static_cast<std::size_t>(sy) * width + x];
            }
            dst[y * width + x] =
                static_cast<std::uint8_t>(sum / static_cast<double>(window));
        }
    }
    return dst;
}

// ---------------------------------------------------------------------------
// J3 -- the freeze accounting
// ---------------------------------------------------------------------------

TEST(qoe_freeze, counts_and_totals_the_reported_freezes) {
    QoeCollector collector;

    collector.on_freeze_detected(100);
    collector.on_freeze_detected(50);

    const QoeSnapshot s = collector.snapshot();
    EXPECT_EQ(s.freeze_count, 2u);
    EXPECT_EQ(s.freeze_total_ms, 150);
    EXPECT_EQ(s.freeze_longest_ms, 100);
}

TEST(qoe_freeze, ignores_a_negative_duration) {
    QoeCollector collector;

    collector.on_freeze_detected(-5);
    collector.on_freeze_detected(20);

    const QoeSnapshot s = collector.snapshot();
    EXPECT_EQ(s.freeze_count, 1u);
    EXPECT_EQ(s.freeze_total_ms, 20);
    EXPECT_EQ(s.freeze_longest_ms, 20);
}

TEST(qoe_freeze, counts_the_dropped_frames_separately) {
    QoeCollector collector;

    collector.on_frame_dropped();
    collector.on_frame_dropped(4);

    const QoeSnapshot s = collector.snapshot();
    EXPECT_EQ(s.frames_dropped, 5u);
    EXPECT_EQ(s.frames_presented, 0u);
}

TEST(qoe_freeze, the_freeze_ratio_is_the_frozen_fraction_of_the_session) {
    QoeCollector collector;

    // A two-second session (2 000 000 us), one 400 ms freeze in it.
    for (int i = 0; i < 60; ++i) {
        const std::int64_t ts = static_cast<std::int64_t>(i) * 33333;
        collector.on_frame_presented(make_frame(ts, ts + 10000));
    }
    collector.on_freeze_detected(400);

    const QoeSnapshot s = collector.snapshot();
    ASSERT_GT(s.session_duration_ms, 0);
    // The session is ~2 s, the freeze is 0.4 s -> ~20 %.  The exact
    // value moves with the frame cadence, so the assertion is a range.
    EXPECT_GT(s.freeze_ratio, 0.15);
    EXPECT_LT(s.freeze_ratio, 0.25);
}

// ---------------------------------------------------------------------------
// J3 -- the presented frame rate
// ---------------------------------------------------------------------------

TEST(qoe_presented_fps, measures_the_rate_the_viewer_got) {
    QoeCollector collector;

    // 60 frames at a 30 fps cadence.
    const std::int64_t interval_us = 1000000 / 30;  // 33 333 us
    for (int i = 0; i < 60; ++i) {
        const std::int64_t ts = static_cast<std::int64_t>(i) * interval_us;
        collector.on_frame_presented(make_frame(ts, ts + 10000));
    }

    const QoeSnapshot s = collector.snapshot();
    EXPECT_EQ(s.frames_presented, 60u);

    // A generous tolerance: the estimator measures a sliding window, and
    // the window's edge moves the count by a frame or two.
    EXPECT_GT(s.presented_fps, 30.0 * 0.8);
    EXPECT_LT(s.presented_fps, 30.0 * 1.2);
}

TEST(qoe_presented_fps, is_zero_before_two_frames) {
    QoeCollector collector;

    EXPECT_DOUBLE_EQ(collector.snapshot().presented_fps, 0.0);

    collector.on_frame_presented(make_frame(0, 1000));
    EXPECT_DOUBLE_EQ(collector.snapshot().presented_fps, 0.0);

    collector.on_frame_presented(make_frame(0, 20000));
    EXPECT_GT(collector.snapshot().presented_fps, 0.0);
}

// ---------------------------------------------------------------------------
// J3 -- the end-to-end latency percentiles
// ---------------------------------------------------------------------------

TEST(qoe_latency_percentiles, p50_and_p95_track_the_distribution) {
    QoeCollector collector;

    // 90 samples at 10 ms, 10 at 100 ms.
    for (int i = 0; i < 90; ++i) {
        collector.on_frame_presented(make_frame(0, 10000));
    }
    for (int i = 0; i < 10; ++i) {
        collector.on_frame_presented(make_frame(0, 100000));
    }

    const QoeSnapshot s = collector.snapshot();

    // The p50 sits in the 10 ms cluster.
    EXPECT_NEAR(static_cast<double>(s.latency_p50_us), 10000.0, 2000.0);
    // The p95 sits in the 100 ms cluster (the 95th percentile of 100
    // samples is the 95th ranked one, which is a 100 ms sample).
    EXPECT_NEAR(static_cast<double>(s.latency_p95_us), 100000.0, 20000.0);
    // The p99 is in the same cluster.
    EXPECT_NEAR(static_cast<double>(s.latency_p99_us), 100000.0, 20000.0);

    // The worst case is reported exactly.
    EXPECT_EQ(s.latency_max_us, 100000);
}

TEST(qoe_latency_percentiles, the_reservoir_is_bounded) {
    PercentileEstimator estimator(/*capacity=*/1024);

    for (int i = 0; i < 100000; ++i) {
        estimator.add(1000 + (i % 97));
    }

    // The memory bound: the retained sample count never exceeds the
    // capacity, no matter how long the session ran.
    EXPECT_LE(estimator.sample_count(), estimator.capacity());
    EXPECT_LE(estimator.sample_count(), 1024u);
    EXPECT_EQ(estimator.capacity(), 1024u);

    // ... but every sample was seen (the total counter is not a sample
    // list, so it does not grow the memory).
    EXPECT_EQ(estimator.total_count(), 100000u);
}

TEST(qoe_latency_percentiles, the_collectors_reservoir_is_bounded) {
    QoeConfig config;
    config.latency_reservoir = 256;
    QoeCollector collector(config);

    for (int i = 0; i < 20000; ++i) {
        collector.on_frame_presented(make_frame(0, 5000 + (i % 11)));
    }

    const QoeSnapshot s = collector.snapshot();
    EXPECT_EQ(s.frames_presented, 20000u);
    // The percentiles still answer — the bound truncates the *history*,
    // not the reporting.
    EXPECT_GT(s.latency_p50_us, 0);
    EXPECT_LE(s.latency_p99_us, 20000);
}

TEST(qoe_latency_percentiles, an_empty_estimator_reports_zero) {
    PercentileEstimator estimator;
    EXPECT_EQ(estimator.percentile(0.5), 0);
    EXPECT_EQ(estimator.max(), 0);
    EXPECT_DOUBLE_EQ(estimator.mean(), 0.0);
}

// ---------------------------------------------------------------------------
// J6 -- the input-to-photon measurement
// ---------------------------------------------------------------------------

TEST(qoe_input_to_photon, pairs_an_input_event_with_the_frame_it_reflects) {
    QoeCollector collector;

    collector.on_input_event_sent(0, 42);
    collector.on_frame_presented_for_input(45000, 42);

    const QoeSnapshot s = collector.snapshot();
    EXPECT_EQ(s.input_to_photon_p50_us, 45000);
    EXPECT_EQ(s.input_to_photon_max_us, 45000);
}

TEST(qoe_input_to_photon, several_events_produce_a_distribution) {
    QoeCollector collector;

    for (std::uint64_t i = 0; i < 10; ++i) {
        collector.on_input_event_sent(static_cast<std::int64_t>(i) * 1000, i);
        collector.on_frame_presented_for_input(
            static_cast<std::int64_t>(i) * 1000 + 45000, i);
    }

    const QoeSnapshot s = collector.snapshot();
    EXPECT_EQ(s.input_to_photon_p50_us, 45000);
    EXPECT_EQ(s.input_to_photon_p95_us, 45000);
}

TEST(qoe_input_to_photon, ignores_an_event_that_was_never_sent) {
    QoeCollector collector;

    collector.on_frame_presented_for_input(45000, 7);

    const QoeSnapshot s = collector.snapshot();
    EXPECT_EQ(s.input_to_photon_p50_us, 0);
    EXPECT_EQ(s.input_to_photon_max_us, 0);
}

TEST(qoe_input_to_photon, an_event_is_consumed_once) {
    QoeCollector collector;

    collector.on_input_event_sent(0, 42);
    collector.on_frame_presented_for_input(45000, 42);
    // The same id arriving again must not produce a second sample.
    collector.on_frame_presented_for_input(90000, 42);

    const QoeSnapshot s = collector.snapshot();
    EXPECT_EQ(s.input_to_photon_p50_us, 45000);
    EXPECT_EQ(s.input_to_photon_max_us, 45000);
}

TEST(qoe_input_to_photon, the_pairing_does_not_inflate_the_frame_rate) {
    QoeCollector collector;

    collector.on_frame_presented(make_frame(0, 10000, 1));
    collector.on_input_event_sent(0, 42);
    collector.on_frame_presented_for_input(10000, 42);

    const QoeSnapshot s = collector.snapshot();
    EXPECT_EQ(s.frames_presented, 1u);
}

// ---------------------------------------------------------------------------
// J3 -- the sharpness proxy
// ---------------------------------------------------------------------------

TEST(qoe_sharpness, the_sharp_picture_scores_above_the_blurred_one) {
    const std::size_t width = 64;
    const std::size_t height = 64;

    const std::vector<std::uint8_t> sharp = make_sharp_edge(width, height);
    const std::vector<std::uint8_t> blurred =
        box_blur(sharp, width, height);

    // The negative control: the two pictures must actually differ, or
    // the ordering assertion below would be vacuous.
    ASSERT_NE(sharp, blurred);

    const double sharp_score =
        SharpnessEstimator::compute(sharp.data(), width, width, height);
    const double blurred_score =
        SharpnessEstimator::compute(blurred.data(), width, width, height);

    EXPECT_GT(sharp_score, blurred_score);
    // The proxy is not a binary verdict: the blurred picture still has
    // *some* edge energy.
    EXPECT_GT(blurred_score, 0.0);
}

TEST(qoe_sharpness, a_flat_picture_scores_zero) {
    const std::size_t width = 32;
    const std::size_t height = 32;
    const std::vector<std::uint8_t> flat = make_plane(width, height, 128);

    EXPECT_DOUBLE_EQ(
        SharpnessEstimator::compute(flat.data(), width, width, height), 0.0);
}

TEST(qoe_sharpness, a_degenerate_picture_scores_zero) {
    EXPECT_DOUBLE_EQ(SharpnessEstimator::compute(nullptr, 16, 16, 16), 0.0);

    const std::vector<std::uint8_t> tiny = make_plane(2, 2, 10);
    EXPECT_DOUBLE_EQ(
        SharpnessEstimator::compute(tiny.data(), 2, 2, 2), 0.0);
}

TEST(qoe_sharpness, the_collector_reports_the_observed_value) {
    QoeCollector collector;

    const std::size_t width = 32;
    const std::size_t height = 32;
    const std::vector<std::uint8_t> sharp = make_sharp_edge(width, height);

    const double value = collector.observe_sharpness(sharp.data(), width,
                                                     width, height);
    EXPECT_GT(value, 0.0);
    EXPECT_DOUBLE_EQ(collector.snapshot().sharpness_proxy, value);
}

// ---------------------------------------------------------------------------
// J3 -- the collector's own boundedness
// ---------------------------------------------------------------------------

TEST(qoe_bound, the_pending_input_set_is_bounded) {
    QoeConfig config;
    config.max_pending_inputs = 16;
    QoeCollector collector(config);

    // 1000 input events whose photon frames never arrive.
    for (std::uint64_t i = 0; i < 1000; ++i) {
        collector.on_input_event_sent(static_cast<std::int64_t>(i), i);
    }

    // The oldest entries were evicted, so only the last few can still
    // be paired.
    collector.on_frame_presented_for_input(1000, 0);
    EXPECT_EQ(collector.snapshot().input_to_photon_p50_us, 0);

    collector.on_frame_presented_for_input(1000, 999);
    EXPECT_EQ(collector.snapshot().input_to_photon_p50_us, 1000 - 999);
}

TEST(qoe_bound, a_reset_clears_every_metric) {
    QoeCollector collector;

    collector.on_frame_presented(make_frame(0, 10000));
    collector.on_frame_dropped(3);
    collector.on_freeze_detected(120);
    collector.on_input_event_sent(0, 1);
    collector.on_frame_presented_for_input(20000, 1);

    collector.reset();

    const QoeSnapshot s = collector.snapshot();
    EXPECT_EQ(s.frames_presented, 0u);
    EXPECT_EQ(s.frames_dropped, 0u);
    EXPECT_EQ(s.freeze_count, 0u);
    EXPECT_EQ(s.freeze_total_ms, 0);
    EXPECT_EQ(s.freeze_longest_ms, 0);
    EXPECT_EQ(s.latency_p50_us, 0);
    EXPECT_EQ(s.input_to_photon_p50_us, 0);
    EXPECT_DOUBLE_EQ(s.presented_fps, 0.0);
    EXPECT_DOUBLE_EQ(s.freeze_ratio, 0.0);
}

} // namespace
