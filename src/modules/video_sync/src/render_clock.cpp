/**
 * @file src/modules/video_sync/src/render_clock.cpp
 * @brief The render clock's implementation (v1.3 plan Q6).
 *
 * See include/nimrtc/video_sync/render_clock.hpp for the contract and
 * the rationale.
 */

#include "nimrtc/video_sync/render_clock.hpp"

namespace nimrtc::video_sync {

using plugins::VideoSyncAction;
using plugins::VideoSyncConfig;
using plugins::VideoSyncCounters;
using plugins::VideoSyncDecision;

RenderClock::RenderClock(const VideoSyncConfig& cfg) noexcept
    : config_(cfg) {
    normalize();
}

void RenderClock::configure(const VideoSyncConfig& cfg) noexcept {
    config_ = cfg;
    normalize();
}

std::int64_t RenderClock::tolerance_us() const noexcept {
    return config_.tolerance_us;
}

std::int64_t RenderClock::max_delay_us() const noexcept {
    return config_.max_delay_us;
}

VideoSyncDecision RenderClock::decide(std::int64_t skew_us,
                                      std::int64_t capture_ts_us) noexcept {
    VideoSyncDecision out{};
    out.skew_us       = skew_us;
    out.capture_ts_us = capture_ts_us;

    // ---- The switch (Q6-1) ------------------------------------------------
    //
    // A disabled clock presents every frame unchanged.  The counters
    // still move — the frame *was* offered to the clock — so a host can
    // see that the correction is switched off rather than silently
    // mis-reading "no drops" as "the picture and the sound agreed".
    if (!config_.enabled) {
        out.action = VideoSyncAction::kPresent;
        ++counters_.frames_presented;
        counters_.last_skew_us = skew_us;
        push_history(out);
        return out;
    }

    const std::int64_t tol = config_.tolerance_us;

    if (skew_us > tol) {
        // The picture is *ahead* of the sound: showing it would put the
        // image on the screen before the audio it belongs to.  The frame
        // is held back so the sound can catch up.
        //
        // The hold is the skew *minus* the tolerance band.  A hold that
        // would be longer than the cap is not worth waiting for — the
        // frame would still be misaligned when it finally appeared, and
        // the wait itself is visible as a freeze — so the frame is
        // **dropped** instead.
        const std::int64_t hold_us = skew_us - tol;
        if (hold_us > config_.max_delay_us) {
            cancel_promised_hold();
            out.action = VideoSyncAction::kDrop;
            ++counters_.frames_dropped;
        } else {
            out.action  = VideoSyncAction::kDelay;
            out.delay_us = hold_us;
            // The *promised* hold.  `note_delay_applied()` settles it
            // against what the presenter really waited, so the reported
            // total is the real one.
            //
            // The previous promise is settled first: only one hold can
            // be outstanding at a time, so a second hold cannot stack
            // the first one's debt on top of it.  That is what keeps
            // the accumulated hold bounded.
            cancel_promised_hold();
            promised_delay_us_ = hold_us;
            ++counters_.frames_delayed;
            counters_.total_delay_us += hold_us;
        }
    } else if (skew_us < -tol) {
        // The picture is *behind* of the sound: the operator has already
        // heard the moment this frame belongs to, so holding it would
        // only make it later.  The frame is **dropped**.
        cancel_promised_hold();
        out.action = VideoSyncAction::kDrop;
        ++counters_.frames_dropped;
    } else {
        // Inside the band: present the frame unchanged.
        out.action = VideoSyncAction::kPresent;
        ++counters_.frames_presented;
    }

    counters_.last_skew_us = skew_us;
    push_history(out);
    return out;
}

void RenderClock::cancel_promised_hold() noexcept {
    // The hold that was promised will never happen -- the frame it was
    // promised for is not going to be presented -- so it comes off the
    // books.  Without this the reported total would carry a wait that
    // never occurred.
    if (promised_delay_us_ > 0) {
        counters_.total_delay_us -= promised_delay_us_;
        if (counters_.total_delay_us < 0) counters_.total_delay_us = 0;
        promised_delay_us_ = 0;
    }
}

void RenderClock::note_delay_applied(std::int64_t actual_delay_us) noexcept {
    if (actual_delay_us < 0) actual_delay_us = 0;

    // The promise is settled against reality: the reported total is the
    // time the presenter really waited, not the time the clock asked
    // for.  A caller that decided not to wait at all reports 0, and the
    // total reflects that honestly.
    if (promised_delay_us_ > 0) {
        counters_.total_delay_us -= promised_delay_us_;
        counters_.total_delay_us += actual_delay_us;
        promised_delay_us_ = 0;
    }
}

void RenderClock::reset_counters() noexcept {
    counters_ = VideoSyncCounters{};
    promised_delay_us_ = 0;
}

VideoSyncAction RenderClock::history_decision(std::size_t index) const noexcept {
    if (index >= kHistorySize) return VideoSyncAction::kPresent;
    const std::size_t slot =
        (head_ + kHistorySize - 1 - index) % kHistorySize;
    return history_[slot].action;
}

std::int64_t RenderClock::history_skew_us(std::size_t index) const noexcept {
    if (index >= kHistorySize) return 0;
    const std::size_t slot =
        (head_ + kHistorySize - 1 - index) % kHistorySize;
    return history_[slot].skew_us;
}

void RenderClock::normalize() noexcept {
    if (config_.tolerance_us < 0) config_.tolerance_us = 0;
    if (config_.max_delay_us  < 0) config_.max_delay_us  = 0;
}

void RenderClock::push_history(const VideoSyncDecision& out) noexcept {
    history_[head_].action        = out.action;
    history_[head_].skew_us       = out.skew_us;
    history_[head_].capture_ts_us = out.capture_ts_us;
    head_ = (head_ + 1) % kHistorySize;
}

} // namespace nimrtc::video_sync
