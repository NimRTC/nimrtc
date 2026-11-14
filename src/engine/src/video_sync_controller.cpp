/**
 * @file src/engine/src/video_sync_controller.cpp
 * @brief The out-of-line parts of the render clock (remote-desktop plan
 *        E5).
 *
 * The controller's *rule* is small; it lives in its own translation
 * unit so the engine's internal headers stay free of a second
 * implementation of it (the same reason `bandwidth_allocation.hpp` and
 * `video_render_path.hpp` are engine-private pure functions).
 *
 * @see video_sync_controller.hpp — the contract and the rationale.
 */
#include "video_sync_controller.hpp"

namespace nimrtc::engine::detail {

VideoSyncController::VideoSyncController(const VideoSyncConfig& cfg) noexcept
    : cfg_(cfg) {
    normalize();
}

void VideoSyncController::configure(const VideoSyncConfig& cfg) noexcept {
    cfg_ = cfg;
    normalize();
}

std::int64_t VideoSyncController::tolerance_us() const noexcept {
    return static_cast<std::int64_t>(cfg_.tolerance_ms) * 1000;
}

std::int64_t VideoSyncController::max_delay_us() const noexcept {
    const std::int32_t cap_ms =
        cfg_.max_delay_ms > 0 ? cfg_.max_delay_ms
                              : VideoSyncConfig::kDefaultMaxDelayCapMs;
    return static_cast<std::int64_t>(cap_ms) * 1000;
}

VideoSyncOutcome VideoSyncController::decide(std::int64_t skew_us,
                                             std::int64_t capture_ts_us) noexcept {
    VideoSyncOutcome out{};
    out.skew_us       = skew_us;
    out.capture_ts_us = capture_ts_us;

    const std::int64_t tol = tolerance_us();

    if (skew_us > tol) {
        // The video is behind the audio: hold the frame.
        //
        // The hold is the skew *minus* the tolerance band, minus the
        // debt the controller is already carrying — so a skew spike
        // cannot make the delay grow without bound — and the result is
        // then capped.
        std::int64_t want = skew_us - tol;
        want -= pending_delay_us_;
        if (want < 0) want = 0;
        if (want > max_delay_us()) want = max_delay_us();

        out.decision = VideoSyncDecision::kDelay;
        out.delay_us = want;
        pending_delay_us_ = want;

        ++counters_.frames_delayed;
        counters_.total_delay_us += want;
    } else if (skew_us < -tol) {
        // The video is ahead of the audio: drop the frame.
        //
        // A drop *cancels* the pending debt rather than adding to it:
        // the frames we dropped were the ones that were early, so the
        // debt we had accumulated for them is gone.
        pending_delay_us_ = 0;
        out.decision = VideoSyncDecision::kDrop;
        ++counters_.frames_dropped;
    } else {
        // Inside the band: present the frame unchanged.
        //
        // The pending debt is *not* cleared here — it is the hold we
        // already promised to the frames we are presenting, and
        // clearing it would let the accumulated hold grow again.
        out.decision = VideoSyncDecision::kPresent;
        ++counters_.frames_presented;
    }

    counters_.last_skew_us = skew_us;
    push_history(out);
    return out;
}

void VideoSyncController::note_delay_applied(std::int64_t actual_delay_us) noexcept {
    if (actual_delay_us < 0) actual_delay_us = 0;
    pending_delay_us_ -= actual_delay_us;
    if (pending_delay_us_ < 0) pending_delay_us_ = 0;
}

void VideoSyncController::reset_counters() noexcept {
    counters_ = VideoSyncCounters{};
    pending_delay_us_ = 0;
}

VideoSyncDecision
VideoSyncController::history_decision(std::size_t index) const noexcept {
    if (index >= VideoSyncConfig::kHistorySize) {
        return VideoSyncDecision::kPresent;
    }
    const std::size_t slot =
        (head_ + VideoSyncConfig::kHistorySize - 1 - index) %
        VideoSyncConfig::kHistorySize;
    return history_[slot].decision;
}

std::int64_t
VideoSyncController::history_skew_us(std::size_t index) const noexcept {
    if (index >= VideoSyncConfig::kHistorySize) return 0;
    const std::size_t slot =
        (head_ + VideoSyncConfig::kHistorySize - 1 - index) %
        VideoSyncConfig::kHistorySize;
    return history_[slot].skew_us;
}

void VideoSyncController::normalize() noexcept {
    if (cfg_.tolerance_ms < 0) cfg_.tolerance_ms = 0;
    if (cfg_.max_delay_ms < 0) cfg_.max_delay_ms = 0;
}

void VideoSyncController::push_history(const VideoSyncOutcome& out) noexcept {
    history_[head_].decision      = out.decision;
    history_[head_].skew_us       = out.skew_us;
    history_[head_].capture_ts_us = out.capture_ts_us;
    head_ = (head_ + 1) % VideoSyncConfig::kHistorySize;
}

}  // namespace nimrtc::engine::detail
