/**
 * @file src/modules/video_pipeline/src/keyframe_limiter.cpp
 * @brief Keyframe-storm limiter — remote-desktop plan B14.
 */

#include <nimrtc/video_pipeline/keyframe_limiter.hpp>

namespace nimrtc::video_pipeline {

// ---------------------------------------------------------------------------
// The vocabulary
// ---------------------------------------------------------------------------

std::string_view keyframe_reason_name(KeyframeReason reason) noexcept {
    switch (reason) {
        case KeyframeReason::kPli:               return "pli";
        case KeyframeReason::kFir:               return "fir";
        case KeyframeReason::kResolutionChange:  return "resolution-change";
        case KeyframeReason::kCodecSwitch:       return "codec-switch";
        case KeyframeReason::kEncoderRequest:    return "encoder-request";
    }
    return "unknown";
}

std::string_view keyframe_decision_name(KeyframeDecision decision) noexcept {
    switch (decision) {
        case KeyframeDecision::kGranted:  return "granted";
        case KeyframeDecision::kDeferred: return "deferred";
        case KeyframeDecision::kRejected: return "rejected";
    }
    return "unknown";
}

std::string_view keyframe_decision_reason_name(
        KeyframeDecisionReason reason) noexcept {
    switch (reason) {
        case KeyframeDecisionReason::kNone:             return "none";
        case KeyframeDecisionReason::kNoLimiter:        return "no-limiter";
        case KeyframeDecisionReason::kMinInterval:      return "min-interval";
        case KeyframeDecisionReason::kCostBudget:       return "cost-budget";
        case KeyframeDecisionReason::kHardCap:          return "hard-cap";
        case KeyframeDecisionReason::kResolutionChange: return "resolution-change";
        case KeyframeDecisionReason::kDisabled:         return "disabled";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// KeyframeLimiter
// ---------------------------------------------------------------------------

KeyframeLimiter::KeyframeLimiter() noexcept
    : KeyframeLimiter(KeyframeLimiterConfig{}) {}

KeyframeLimiter::KeyframeLimiter(KeyframeLimiterConfig cfg) noexcept
    : cfg_(cfg) {
    if (cfg_.min_interval_us < 0)  cfg_.min_interval_us = 0;
    if (cfg_.window_us < 0)        cfg_.window_us = 0;
    if (cfg_.keyframe_cost_ratio == 0) cfg_.keyframe_cost_ratio = 1;
}

KeyframeLimiter::KeyframeLimiter(video_rtcp_feedback::FirCoalescer& coalescer,
                                 KeyframeLimiterConfig cfg) noexcept
    : cfg_(cfg), coalescer_(&coalescer) {
    if (cfg_.min_interval_us < 0)  cfg_.min_interval_us = 0;
    if (cfg_.window_us < 0)        cfg_.window_us = 0;
    if (cfg_.keyframe_cost_ratio == 0) cfg_.keyframe_cost_ratio = 1;
    // The coalescer's gate and the limiter's gate must carry the same
    // interval: the coalescer decides whether a key-frame *request* may
    // be sent, the limiter whether a key *frame* may be produced.  If
    // they disagreed, one of the two would be the only real limit.
    if (coalescer_->min_interval_us() > 0 &&
        cfg_.min_interval_us != coalescer_->min_interval_us()) {
        cfg_.min_interval_us = coalescer_->min_interval_us();
    }
}

void KeyframeLimiter::configure(KeyframeLimiterConfig cfg) noexcept {
    if (cfg.min_interval_us < 0)  cfg.min_interval_us = 0;
    if (cfg.window_us < 0)        cfg.window_us = 0;
    if (cfg.keyframe_cost_ratio == 0) cfg.keyframe_cost_ratio = 1;
    cfg_ = cfg;
}

void KeyframeLimiter::reset() noexcept {
    stats_ = KeyframeLimiterStats{};
    has_granted_ = false;
    last_granted_us_ = 0;
    has_window_ = false;
    window_start_us_ = 0;
    last_carries_parameter_sets_ = false;
    pending_ = false;
    last_decision_ = KeyframeDecision::kGranted;
    last_reason_   = KeyframeDecisionReason::kNone;
}

void KeyframeLimiter::roll_window(std::int64_t now_us) noexcept {
    if (cfg_.window_us <= 0) return;
    if (!has_window_) {
        has_window_ = true;
        window_start_us_ = now_us;
        stats_.keyframes_in_window = 0;
        return;
    }
    if (now_us - window_start_us_ >= cfg_.window_us) {
        window_start_us_ = now_us;
        stats_.keyframes_in_window = 0;
    }
}

KeyframeDecision KeyframeLimiter::request(KeyframeReason reason,
                                           std::int64_t now_us,
                                           bool carries_parameter_sets) noexcept {
    ++stats_.requests;
    last_carries_parameter_sets_ = false;

    // ---- The one thing that is never deferred ----------------------------
    //
    // A resolution change forces a key frame *and* the new parameter
    // sets (plan B13).  Deferring it breaks the decode: the peer
    // receives a frame whose geometry its parameter sets do not
    // describe, and it cannot decode the frame at all.  The same is
    // true of a codec switch: the new codec cannot decode the old
    // stream.
    //
    // So the request bypasses every limiter, and the counters say so
    // (`forced_grants`), because "the limiter granted this" and "the
    // limiter was bypassed for this" are different facts and a host
    // reading the counters must be able to tell them apart.
    const bool mandatory =
        reason == KeyframeReason::kResolutionChange ||
        reason == KeyframeReason::kCodecSwitch   ||
        carries_parameter_sets;

    if (mandatory) {
        ++stats_.granted;
        ++stats_.forced_grants;
        ++stats_.keyframes_in_window;
        last_granted_us_ = now_us;
        has_granted_ = true;
        pending_ = false;
        last_carries_parameter_sets_ = carries_parameter_sets ||
            reason == KeyframeReason::kResolutionChange;
        last_decision_ = KeyframeDecision::kGranted;
        last_reason_   = KeyframeDecisionReason::kResolutionChange;
        if (coalescer_) coalescer_->mark_sent(cfg_.media_ssrc, now_us);
        return KeyframeDecision::kGranted;
    }

    if (cfg_.disabled) {
        ++stats_.granted;
        ++stats_.keyframes_in_window;
        last_granted_us_ = now_us;
        has_granted_ = true;
        pending_ = false;
        last_decision_ = KeyframeDecision::kGranted;
        last_reason_   = KeyframeDecisionReason::kDisabled;
        if (coalescer_) coalescer_->mark_sent(cfg_.media_ssrc, now_us);
        return KeyframeDecision::kGranted;
    }

    roll_window(now_us);

    // ---- The interval gate ------------------------------------------------
    //
    // The same policy the FIR coalescer applies (RFC 5104 §4.3.1.1), on
    // the encoder side: two key frames closer than the minimum interval
    // are one key frame too many.
    //
    // When the limiter was built around a `FirCoalescer`, the coalescer
    // is the *authority* on the interval — the coalescer already
    // remembers when the last key-frame request went out for this media
    // SSRC, and the plan's B14 says the coalescer is what "is finally
    // wired into the engine".  Asking the coalescer (rather than
    // keeping a second, parallel clock) is what makes the wiring real
    // instead of a copy.
    if (coalescer_) {
        if (!coalescer_->should_send(cfg_.media_ssrc, now_us)) {
            ++stats_.deferred;
            pending_ = true;
            last_decision_ = KeyframeDecision::kDeferred;
            last_reason_   = KeyframeDecisionReason::kMinInterval;
            return KeyframeDecision::kDeferred;
        }
    } else if (cfg_.min_interval_us > 0 && has_granted_ &&
               (now_us - last_granted_us_) < cfg_.min_interval_us) {
        ++stats_.deferred;
        pending_ = true;
        last_decision_ = KeyframeDecision::kDeferred;
        last_reason_   = KeyframeDecisionReason::kMinInterval;
        return KeyframeDecision::kDeferred;
    }

    // ---- The hard cap -----------------------------------------------------
    //
    // Whatever the cost budget says, a window may not contain more than
    // `max_keyframes_per_window` key frames.
    if (cfg_.max_keyframes_per_window > 0 &&
        stats_.keyframes_in_window >= cfg_.max_keyframes_per_window) {
        ++stats_.rejected;
        pending_ = false;
        last_decision_ = KeyframeDecision::kRejected;
        last_reason_   = KeyframeDecisionReason::kHardCap;
        return KeyframeDecision::kRejected;
    }

    // ---- The cost budget --------------------------------------------------
    //
    // A key frame costs `keyframe_cost_ratio` times an inter frame, so
    // the budget is spent in "inter frame equivalents".  The budget is
    // per window: `keyframe_budget` inter-frame equivalents.
    if (cfg_.keyframe_budget > 0) {
        const std::uint64_t spent =
            static_cast<std::uint64_t>(stats_.keyframes_in_window) *
            static_cast<std::uint64_t>(cfg_.keyframe_cost_ratio);
        if (spent + cfg_.keyframe_cost_ratio > cfg_.keyframe_budget) {
            ++stats_.rejected;
            pending_ = false;
            last_decision_ = KeyframeDecision::kRejected;
            last_reason_   = KeyframeDecisionReason::kCostBudget;
            return KeyframeDecision::kRejected;
        }
    }

    // ---- Granted -----------------------------------------------------------
    ++stats_.granted;
    ++stats_.keyframes_in_window;
    last_granted_us_ = now_us;
    has_granted_ = true;
    pending_ = false;
    last_decision_ = KeyframeDecision::kGranted;
    last_reason_   = KeyframeDecisionReason::kNoLimiter;
    // Tell the coalescer the key frame went out, so its interval
    // accounting and the limiter's stay in step.
    if (coalescer_) coalescer_->mark_sent(cfg_.media_ssrc, now_us);
    return KeyframeDecision::kGranted;
}

} // namespace nimrtc::video_pipeline
