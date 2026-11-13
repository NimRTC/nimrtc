/**
 * @file src/engine/src/engine_capture.cpp
 * @brief The cross-end capture timestamp of an inbound audio frame.
 *
 * v1.2.0 (audio pipeline, WP-9; spec B-17).
 *
 * The capture timestamp answers the question "when did the *peer*
 * capture this frame?" — not "when did we play it out?".  It is the
 * audio half of the A/V-sync contract (C-4): a host that has to line
 * a voice frame up with a video frame needs both on the *peer's*
 * timeline, and the playout clock is not that timeline.
 *
 * The reconstruction itself is the ADR-017 seam
 * (`nimrtc::rtp::SenderReportTracker`): the peer's Sender Report pairs
 * its wall clock with its media clock, and the tracker extrapolates
 * from that pair to the RTP timestamp of the frame in question.
 *
 * Two rules are absolute here (spec O-9):
 *
 *   * **Never fake it.**  Without a usable Sender Report the value is
 *     0, and 0 means "not reconstructible".  It is never derived from
 *     the local playout clock — that would silently mix two different
 *     clocks (the peer's wall clock and ours) and produce a number
 *     that looks plausible and is wrong.
 *
 *   * **Never guess the clock rate.**  The media clock rate comes from
 *     the peer's own `a=rtpmap:` line; the engine pins it on the
 *     tracker when the first packet of an SSRC arrives (see
 *     `handle_rtp()`).
 */
#include "engine_impl.hpp"

namespace nimrtc::engine {

std::int64_t NimRTCEngine::audio_capture_time_us(
        std::uint32_t ssrc,
        std::uint32_t rtp_timestamp) const noexcept {
    if (impl_ == nullptr) return 0;
    if (ssrc == 0) return 0;

    // The timeline seam is the only source of a capture instant.  With
    // the feature switched off there is no anchor at all, and the
    // answer is 0 — not a guess.
    if (!config_.timeline_enabled) return 0;

    // `clock_rate_hz = 0` means "use the rate the engine configured for
    // this SSRC" (the peer's `a=rtpmap:` rate).  Passing a rate here
    // would override it, and the tracker would extrapolate with the
    // wrong slope — a 48 kHz stream measured at 90 kHz is off by
    // 87.5 %.
    //
    // The abs-send-time argument is 0 and the "has abs-send-time" flag
    // is false on purpose: this is a *frame* lookup, not a packet
    // observation.  The tracker must not record an observation here
    // (the observation is recorded by `observe_timeline()`, on the
    // packet path, where the element was actually parsed).
    auto cap = impl_->timeline.sr_tracker.capture_ts_us_for(
        ssrc,
        rtp_timestamp,
        /*abs_send_time_24=*/0,
        /*clock_rate_hz=*/0,
        /*local_recv_us_since_epoch=*/detail::steady_now_us(),
        /*out_sender_recv_skew_us=*/nullptr);

    if (!cap) return 0;   // No usable Sender Report for this source.
    return *cap;
}

}  // namespace nimrtc::engine
