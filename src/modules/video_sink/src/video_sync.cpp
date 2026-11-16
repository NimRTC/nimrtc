// src/modules/video_sink/src/video_sync.cpp
// ----------------------------------------------------------------------------
// The video half of the A/V sync decision (remote-desktop plan E5).
// ----------------------------------------------------------------------------

#include "nimrtc/video_sink/video_sync.hpp"

namespace nimrtc::video_sink {

VideoSyncController::VideoSyncController(SyncConfig config) : config_{config} {}

SyncDecision VideoSyncController::decide(std::int64_t skew_us) noexcept {
    ++stats_.total;
    stats_.last_skew_us = skew_us;

    SyncDecision decision;
    decision.skew_us = skew_us;

    if (!config_.enabled) {
        decision.action = SyncAction::kPresent;
        ++stats_.presented;
        return decision;
    }

    const std::int64_t tolerance = config_.tolerance_us;

    if (skew_us > tolerance) {
        // The video leads the audio.  Hold it back, unless the hold would
        // be longer than the maximum worth waiting for.
        const std::int64_t hold_us = skew_us - tolerance;
        if (hold_us > config_.max_delay_us) {
            decision.action = SyncAction::kDrop;
            ++stats_.dropped;
            return decision;
        }
        decision.action = SyncAction::kDelay;
        decision.delay_us = hold_us;
        ++stats_.delayed;
        return decision;
    }

    if (skew_us < -tolerance) {
        // The video lags the audio: the moment the operator heard is
        // already gone, so the frame is dropped.
        decision.action = SyncAction::kDrop;
        ++stats_.dropped;
        return decision;
    }

    decision.action = SyncAction::kPresent;
    ++stats_.presented;
    return decision;
}

VideoSyncController::Stats VideoSyncController::stats() const noexcept {
    return stats_;
}

const SyncConfig& VideoSyncController::config() const noexcept {
    return config_;
}

void VideoSyncController::reset() noexcept {
    stats_ = Stats{};
}

} // namespace nimrtc::video_sink
