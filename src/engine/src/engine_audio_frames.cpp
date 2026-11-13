/**
 * @file src/engine/src/engine_audio_frames.cpp
 * @brief The detailed audio-frame delivery path (v1.2.0, audio pipeline
 *        WP-10, spec E-1 / E-2 / E-4).
 *
 * The *legacy* PCM tap and the 3A render stage live in
 * `engine_media.cpp` (`NimRTCEngine::on_audio_frame_decoded`); this file
 * carries the two things that came with the P1 public SDK:
 *
 *   1. `deliver_audio_frame_info()` — builds the `AudioFrameInfo` the
 *      host receives through `set_on_audio_frame_info()` and fires it.
 *      It runs *after* the legacy callback, so a host that installed
 *      both sees the legacy tap first and the detailed one second
 *      (spec E-2).
 *
 *   2. `remote_audio_format()` — the negotiated format of the inbound
 *      audio stream, once the remote description has been processed
 *      (spec E-4).
 */
#include "engine_impl.hpp"

namespace nimrtc::engine {

// ---------------------------------------------------------------------------
// The detailed audio-frame callback (spec E-1 / E-2)
// ---------------------------------------------------------------------------
//
// The frame's PCM belongs to the receiver and is only valid for the
// duration of the callback (the seam's contract), so the struct is
// filled and handed over without copying the samples.

void NimRTCEngine::deliver_audio_frame_info(
        const plugins::AudioOutputFrame& frame) noexcept {
    if (!on_audio_frame_info_) return;
    if (frame.pcm == nullptr || frame.samples_per_channel == 0) return;

    AudioFrameInfo info{};
    info.pcm                     = frame.pcm;
    info.num_samples_per_channel = frame.samples_per_channel;
    info.sample_rate_hz          = frame.sample_rate_hz;
    info.channels                = frame.channels != 0 ? frame.channels : 1;
    info.rtp_timestamp           = frame.rtp_timestamp;
    info.playout_time_us         = frame.playout_time_us;
    info.concealed               = frame.concealed;
    info.fec_recovered           = frame.fec_recovered;

    // The peer's capture instant (spec B-17).  0 when the cross-end
    // timeline has not reconstructed it yet — the value is *never*
    // faked from the local playout clock, because that would silently
    // mix two different clocks (spec O-9).
    //
    // The source is the inbound audio SSRC the engine learned from the
    // wire: the frame itself does not carry it, and the pipeline
    // handles one stream per receiver.
    info.capture_time_us = audio_capture_time_us(impl_ != nullptr
                                                      ? impl_->audio_state.audio_rx.ssrc
                                                      : 0,
                                                  frame.rtp_timestamp);

    on_audio_frame_info_(info);
}


// ---------------------------------------------------------------------------
// The negotiated remote audio format (spec E-4)
// ---------------------------------------------------------------------------
//
// Filled in once the remote description has been processed: before that
// there is no negotiated audio track and the accessor says so instead of
// guessing.

bool NimRTCEngine::remote_audio_format(AudioFormat* out) const noexcept {
    if (out == nullptr) return false;
    if (impl_ == nullptr) return false;

    // The payload type we accepted.  0 means the negotiation did not
    // conclude an audio track (either no audio m-line, or the peer only
    // offered codecs we cannot decode — spec A-9).
    const std::uint8_t pt = impl_->audio_state.negotiated_audio_pt;
    if (pt == 0) return false;

    out->payload_type      = pt;
    out->sample_rate_hz    = config_.audio_codec.clock_rate;
    out->channels          = config_.audio_downmix_to_pcm_channels
                                 ? static_cast<std::uint8_t>(config_.pcm_channels)
                                 : config_.audio_codec.channels;
    out->frame_duration_ms = config_.audio_ptime_ms;
    return true;
}

} // namespace nimrtc::engine
