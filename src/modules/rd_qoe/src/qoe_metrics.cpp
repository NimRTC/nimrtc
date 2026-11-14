// src/modules/rd_qoe/src/qoe_metrics.cpp
// ----------------------------------------------------------------------------
// The remote-desktop QoE metrics (plan Track J, J3 / J6).
//
// The header documents the semantics; this file is the implementation.
// ----------------------------------------------------------------------------

#include "nimrtc/rd_qoe/qoe_metrics.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace nimrtc::rd_qoe {

// ---------------------------------------------------------------------------
// PercentileEstimator
// ---------------------------------------------------------------------------

PercentileEstimator::PercentileEstimator(std::size_t capacity)
    : capacity_{capacity == 0 ? 1 : capacity} {
    samples_.reserve(capacity_);
}

void PercentileEstimator::add(std::int64_t value_us) {
    if (samples_.size() < capacity_) {
        samples_.push_back(value_us);
    } else {
        // The reservoir is full: the oldest slot is recycled.  `next_`
        // walks the ring, so the samples are kept in *insertion* order
        // (modulo the wrap), which is what "the most recent N" means.
        samples_[next_] = value_us;
        next_ = (next_ + 1) % capacity_;
    }

    ++total_;
    sum_ += static_cast<double>(value_us);

    if (!seen_) {
        max_ = value_us;
        min_ = value_us;
        seen_ = true;
    } else {
        if (value_us > max_) max_ = value_us;
        if (value_us < min_) min_ = value_us;
    }
}

void PercentileEstimator::reset() noexcept {
    samples_.clear();
    next_ = 0;
    total_ = 0;
    max_ = 0;
    min_ = 0;
    sum_ = 0.0;
    seen_ = false;
}

std::int64_t PercentileEstimator::percentile(double q) const {
    if (samples_.empty()) return 0;
    if (q < 0.0) q = 0.0;
    if (q > 1.0) q = 1.0;

    std::vector<std::int64_t> sorted(samples_);
    std::sort(sorted.begin(), sorted.end());

    // Nearest-rank: the smallest value whose rank is at least q·n.
    const std::size_t n = sorted.size();
    auto rank = static_cast<std::size_t>(
        std::ceil(q * static_cast<double>(n)));
    if (rank == 0) rank = 1;
    if (rank > n) rank = n;
    return sorted[rank - 1];
}

double PercentileEstimator::mean() const noexcept {
    if (total_ == 0) return 0.0;
    return sum_ / static_cast<double>(total_);
}

// ---------------------------------------------------------------------------
// SharpnessEstimator
// ---------------------------------------------------------------------------

double SharpnessEstimator::compute(const std::uint8_t* luma,
                                   std::size_t stride,
                                   std::size_t width,
                                   std::size_t height) {
    if (luma == nullptr || width < 3 || height < 3) return 0.0;
    // A stride shorter than the width would read outside a row.
    if (stride < width) return 0.0;

    double sum = 0.0;
    std::size_t count = 0;

    // The interior only: the 3×3 cross of a border pixel reaches outside
    // the picture.
    for (std::size_t y = 1; y + 1 < height; ++y) {
        const std::uint8_t* row = luma + y * stride;
        const std::uint8_t* up = row - stride;
        const std::uint8_t* down = row + stride;
        for (std::size_t x = 1; x + 1 < width; ++x) {
            const int center = row[x];
            const int lap = 4 * center - up[x] - down[x] - row[x - 1] - row[x + 1];
            sum += std::abs(static_cast<int>(lap));
            ++count;
        }
    }

    if (count == 0) return 0.0;
    return sum / static_cast<double>(count);
}

double SharpnessEstimator::observe(const std::uint8_t* luma,
                                   std::size_t stride,
                                   std::size_t width,
                                   std::size_t height) {
    last_ = compute(luma, stride, width, height);
    sum_ += last_;
    ++count_;
    return last_;
}

double SharpnessEstimator::observe_value(double value) {
    last_ = value;
    sum_ += last_;
    ++count_;
    return last_;
}

void SharpnessEstimator::reset() noexcept {
    last_ = 0.0;
    sum_ = 0.0;
    count_ = 0;
}

double SharpnessEstimator::mean() const noexcept {
    if (count_ == 0) return 0.0;
    return sum_ / static_cast<double>(count_);
}

// ---------------------------------------------------------------------------
// QoeCollector
// ---------------------------------------------------------------------------

QoeCollector::QoeCollector(const QoeConfig& config)
    : config_{config},
      latency_{config.latency_reservoir},
      input_to_photon_{config.input_reservoir} {}

void QoeCollector::on_frame_presented(const FrameObservation& observation) {
    ++frames_presented_;

    // ---- the end-to-end latency ------------------------------------------
    const std::int64_t latency = observation.render_ts_us - observation.capture_ts_us;
    if (latency >= 0) {
        latency_.add(latency);
    }

    // ---- the sliding window for the presented frame rate -----------------
    const std::int64_t ts = observation.render_ts_us;
    fps_window_.push_back(ts);
    const std::int64_t cutoff = ts - config_.fps_window_us;
    while (!fps_window_.empty() && fps_window_.front() < cutoff) {
        fps_window_.pop_front();
    }

    if (!have_frame_) {
        first_render_ts_us_ = ts;
        have_frame_ = true;
    }
    last_render_ts_us_ = ts;

    if (observation.source_fps > 0.0) {
        source_fps_ = observation.source_fps;
    }
}

void QoeCollector::on_frame_dropped(std::uint64_t count) {
    frames_dropped_ += count;
}

void QoeCollector::on_freeze_detected(std::int64_t duration_ms) {
    if (duration_ms < 0) return;
    ++freeze_count_;
    freeze_total_ms_ += duration_ms;
    if (duration_ms > freeze_longest_ms_) freeze_longest_ms_ = duration_ms;
}

void QoeCollector::on_input_event_sent(std::int64_t ts_us, std::uint64_t event_id) {
    // The event is forgotten when the pending map is full: the oldest
    // waiting event is evicted first (FIFO), because an input event whose
    // frame never arrives would otherwise pin an entry forever.
    while (pending_inputs_.size() >= config_.max_pending_inputs
           && !pending_order_.empty()) {
        const std::uint64_t oldest = pending_order_.front();
        pending_order_.pop_front();
        pending_inputs_.erase(oldest);
    }

    auto it = pending_inputs_.find(event_id);
    if (it != pending_inputs_.end()) {
        // The same id re-sent: refresh the timestamp in place rather than
        // growing the pending set.
        it->second = ts_us;
        return;
    }

    pending_inputs_.emplace(event_id, ts_us);
    pending_order_.push_back(event_id);
}

void QoeCollector::on_frame_presented_for_input(std::int64_t ts_us,
                                                 std::uint64_t event_id) {
    auto it = pending_inputs_.find(event_id);
    if (it == pending_inputs_.end()) return;  // never sent -> no honest latency

    const std::int64_t sent_ts = it->second;
    pending_inputs_.erase(it);
    for (auto oit = pending_order_.begin(); oit != pending_order_.end(); ++oit) {
        if (*oit == event_id) {
            pending_order_.erase(oit);
            break;
        }
    }

    const std::int64_t latency = ts_us - sent_ts;
    if (latency >= 0) {
        input_to_photon_.add(latency);
    }
}

double QoeCollector::observe_sharpness(const std::uint8_t* luma,
                                       std::size_t stride,
                                       std::size_t width,
                                       std::size_t height) {
    return sharpness_.observe(luma, stride, width, height);
}

void QoeCollector::on_sharpness_observed(double value) {
    sharpness_.observe_value(value);
}

double QoeCollector::compute_presented_fps() const {
    if (fps_window_.size() < 2) return 0.0;
    const std::int64_t first = fps_window_.front();
    const std::int64_t last = fps_window_.back();
    const std::int64_t span = last - first;
    if (span <= 0) return 0.0;
    const double frames = static_cast<double>(fps_window_.size() - 1);
    return frames * 1000000.0 / static_cast<double>(span);
}

QoeSnapshot QoeCollector::snapshot() const {
    QoeSnapshot s;

    s.frames_presented = frames_presented_;
    s.frames_dropped = frames_dropped_;
    s.freeze_count = freeze_count_;
    s.freeze_total_ms = freeze_total_ms_;
    s.freeze_longest_ms = freeze_longest_ms_;

    s.presented_fps = compute_presented_fps();

    s.latency_p50_us = latency_.percentile(0.50);
    s.latency_p95_us = latency_.percentile(0.95);
    s.latency_p99_us = latency_.percentile(0.99);
    s.latency_max_us = latency_.max();

    s.input_to_photon_p50_us = input_to_photon_.percentile(0.50);
    s.input_to_photon_p95_us = input_to_photon_.percentile(0.95);
    s.input_to_photon_max_us = input_to_photon_.max();

    s.sharpness_proxy = sharpness_.last();
    s.source_fps = source_fps_;

    if (have_frame_ && last_render_ts_us_ > first_render_ts_us_) {
        s.session_duration_ms = (last_render_ts_us_ - first_render_ts_us_) / 1000;
    }
    if (s.session_duration_ms > 0) {
        s.freeze_ratio = static_cast<double>(freeze_total_ms_)
                         / static_cast<double>(s.session_duration_ms);
        if (s.freeze_ratio < 0.0) s.freeze_ratio = 0.0;
        if (s.freeze_ratio > 1.0) s.freeze_ratio = 1.0;
    }

    return s;
}

void QoeCollector::reset() noexcept {
    latency_.reset();
    input_to_photon_.reset();
    sharpness_.reset();

    frames_presented_ = 0;
    frames_dropped_ = 0;
    freeze_count_ = 0;
    freeze_total_ms_ = 0;
    freeze_longest_ms_ = 0;

    fps_window_.clear();
    first_render_ts_us_ = 0;
    last_render_ts_us_ = 0;
    have_frame_ = false;
    source_fps_ = 0.0;

    pending_inputs_.clear();
    pending_order_.clear();
}

} // namespace nimrtc::rd_qoe
