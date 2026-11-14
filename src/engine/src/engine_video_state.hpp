/**
 * @file src/engine/src/engine_video_state.hpp
 * @brief PRIVATE engine collaborator: the video plane's per-engine state.
 *
 * Architecture remediation plan §4.3 (D1 — engine split): this state used
 * to live as thirty-odd loose members inside `NimRTCEngine::Impl`, which
 * made the engine's internal header a catch-all and scattered one
 * feature's state across the file.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * ## What lives here
 *
 *   - the **send** state: the keyframe-pending flag, the outbound
 *     counters and the zero-copy counters;
 *   - the **receive** state: the negotiated codec / payload type, the
 *     payload-type routing table, the receive counters and the
 *     "the peer offered a codec we cannot decode" flags;
 *   - the **RTCP feedback** state: the remote video SSRC, our feedback
 *     SSRC and the queue of pending feedback reports;
 *   - the **quality statistics** bridge (the `nimrtc::video_stats` sink);
 *   - the **ref-frame timeline** table (the local ref-frame → capture
 *     timestamp registry);
 *   - the video half of the outbound Sender Report counters.
 *
 * ## Threading
 *
 * `negotiated_video_pt` is an atomic: the negotiation runs on the
 * signalling thread while the encoder callback that stamps the outbound
 * RTP header runs on another.  Everything else follows the engine's
 * serialisation contract.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include <nimrtc/plugins/video_pipeline.hpp>

#include "audio_routing.hpp"
#include "engine_ref_frame_table.hpp"
#include "engine_render_state.hpp"
#include "engine_stage_timing.hpp"
#include "video_codec_config.hpp"
#include "video_negotiation.hpp"
#include "video_render_path.hpp"
#include "video_stats_bridge.hpp"
#include "video_stream_set.hpp"

namespace nimrtc::engine::detail {

/** The video plane's per-engine state. */
struct VideoState {
    // ---- Video counters (v1.1 V1, cumulative since open()) ------------
    std::uint64_t video_frames_sent       = 0;
    std::uint64_t video_packets_sent      = 0;
    std::uint64_t video_bytes_sent        = 0;
    std::uint64_t video_frames_received   = 0;
    std::uint64_t video_nack_seqs_queued  = 0;
    std::uint64_t video_keyframes_q       = 0;
    std::uint64_t video_feedback_applied  = 0;
    std::uint64_t video_keyframes_applied = 0;

    // ---- Video zero-copy counters (Phase 3 / C3) ------------------------
    //
    // `video_zero_copy_frames` counts the frames that took the GPU path
    // (the engine handed the native handle to the codec instead of
    // staging CPU pixels).  `video_zero_copy_fallbacks` counts the frames
    // that carried a GPU handle but could NOT take the zero-copy path —
    // either the codec does not support it, or `encode_zero_copy()`
    // returned an error.  The two counters make the flag observable: a
    // host that turns `EngineConfig::video_zero_copy` on can see whether
    // the path is actually being taken.
    std::uint64_t video_zero_copy_frames    = 0;
    std::uint64_t video_zero_copy_fallbacks = 0;

    // ---- Video receive-side delivery path (remote-desktop plan E1) -------
    //
    // Which route a decoded frame takes from the decoder to the sink.
    // Decided ONCE, at `open()` time, from the decoder's
    // `supports_zero_copy_decode()` and the sink's
    // `supports_gpu_render()` / `supports_cpu_render()` — see
    // `src/engine/src/video_render_path.hpp` for the rule and for why
    // it lives in a pure, unit-tested function.
    //
    // `kGpuSurface` means the decoder hands back a native surface and
    // the sink presents it (no host copy).  `kCpuStaging` is the
    // universal fallback.  `kIncompatible` means neither path is
    // possible — the engine reports `kVideoSinkIncompatible` at
    // `open()` time and the receive pipeline renders nothing.
    detail::VideoRenderPath video_render_path = detail::VideoRenderPath::kCpuStaging;

    /** The decoder's zero-copy decode output pool, when the GPU path is
     *  in use.  The engine acquires a surface from it per frame and
     *  hands it to the sink.  Null when the GPU path is not in use. */
    std::shared_ptr<plugins::GpuBufferPool> video_decode_pool;

    /** The receive-side zero-copy counters.  `video_rx_gpu_frames` counts
     *  the frames that reached the sink as a GPU surface;
     *  `video_rx_cpu_frames` counts the frames that were staged to host
     *  memory first.  The two make the decision observable from the
     *  outside — a host that expects the GPU path can see whether it is
     *  actually being taken. */
    std::uint64_t video_rx_gpu_frames = 0;
    std::uint64_t video_rx_cpu_frames = 0;

    // ---- Video send pipeline state -------------------------------------
    // Tracks whether the next encoded frame is a keyframe (forced by
    // force_keyframe() or signalled by the codec).  Consumed by the sender's
    // packet callback to set kVideoKeyframe priority on the marker packet.
    std::atomic<bool> video_keyframe_pending_{false};

    // ---- RTCP feedback state (v1.1 Q3) --------------------------------
    //
    // The feedback loop has two halves:
    //
    //   outbound  receiver detects a loss -> the receiver plugin encodes a
    //             NACK / PLI -> the engine protects it (SRTCP) and sends it.
    //             The RTCP *sender* SSRC identifies us; the *media* SSRC is
    //             the peer's video stream the feedback is about.
    //
    //   inbound   the peer sends a NACK / PLI -> the engine hands the bytes
    //             to the sender plugin -> retransmission / forced IDR.
    //
    // `remote_video_ssrc_` is learned from the inbound RTP stream (the peer
    // picks its own SSRC; our config only carries the *expected* one), so
    // the NACK we send points at the right media source.
    std::uint32_t remote_video_ssrc_    = 0;
    /** Our own SSRC as the sender of the RTCP feedback (RFC 3550 §6.1). */
    std::uint32_t video_feedback_ssrc_  = 0;
    /** Feedback reports waiting to be put on the wire.  The receiver
     *  raises them from inside the ICE receive path, so the engine queues
     *  them and sends them from `tick()` — the same place the Sender
     *  Report is sent from.  Re-entering the transport's send() from
     *  inside its recv() would be a needless risk. */
    plugins::VideoFeedbackReport pending_video_feedback_{};
    bool                        has_pending_video_feedback_ = false;

    // ---- Negotiated payload-type routing (B-4) -------------------------
    //
    // The payload types the peer declared on the video m-line of its
    // description, and which we therefore accept on the wire.
    std::map<std::uint8_t, AudioPayloadRoute> video_pt_routes;

    /** The negotiated video codec and payload type (C9 / C11) — see
     *  `src/engine/src/video_negotiation.hpp` for the shape and for what
     *  "negotiated" means.  The video side is the counterpart of
     *  `negotiated_audio_pt`, but it carries the codec identity
     *  and the fmtp as well, because the video fmtp is what the
     *  receiver needs in order to interpret the bitstream. */
    VideoNegotiationResult negotiated_video;

    /** The negotiated video payload type, mirrored into an atomic.
     *
     *  The negotiation runs on the signaling thread while the packet
     *  path (the encoder callback that stamps the outbound RTP header)
     *  runs on another, so the number the wire uses is published
     *  through an atomic rather than read from the struct.  0 means
     *  "nothing was negotiated" — the caller then falls back to its
     *  locally configured payload type. */
    std::atomic<std::uint8_t> negotiated_video_pt{0};

    /** The video codec family this session uses.
     *
     *  Resolved once when the video plugins are created (see
     *  `init_video_plugins()`), from the instantiated
     *  `IVideoCodec::kind()`.  The SDP layer reads it to name the
     *  `a=rtpmap:` encoding and to emit the matching `a=fmtp:` body;
     *  the negotiation layer reads it to decide whether the peer's
     *  offered encoding is one this session can decode.
     *
     *  `kUnknown` means "no video codec" — the session has no video
     *  m-line at all. */
    VideoCodecFamily video_codec_family = VideoCodecFamily::kUnknown;

    /** The effective RTP payload type of the video stream.
     *
     *  Resolved from the host's configured payload type and the codec
     *  family (see `detail::resolve_payload_type`).  This is the number
     *  the offer advertises, the answer echoes, and the outbound RTP
     *  header stamps. */
    std::uint8_t video_payload_type = 0;

    // ---- Video quality statistics (Phase 4.2 / D2) --------------------
    //
    // The engine drives the `nimrtc::video_stats` sink from the receive
    // pipeline: a frame arriving from the receiver is `on_frame_received`,
    // a successful decode is `on_frame_decoded`, a frame handed to the
    // sink is `on_frame_rendered`, and the drops the pipeline reports are
    // `on_frame_dropped`.
    //
    // The sink lives here (not in the engine class) because it is
    // per-session state that outlives a single callback, and because the
    // `stats()` accessor has to be callable from a `const` method.
    //
    // The bridge owns the sink and makes the module dependency optional
    // (see `video_stats_bridge.hpp`).
    VideoStatsBridge video_stats;

    /** The video receiver's `frames_incomplete` counter as of the last
     *  observation, and the value it had the previous time.
     *
     *  The receiver plugin reports *cumulative* counters, so the
     *  per-interval drop count is the difference between two snapshots.
     *  The engine polls the receiver in `tick()` (the receiver has no
     *  push-style drop callback) and feeds the difference into the
     *  statistics sink as `kJitterBufferTimeout` drops.
     *
     *  A frame that never assembled because its packets were lost is
     *  exactly a jitter-buffer timeout from the caller's point of view:
     *  the media never became a frame. */
    std::uint64_t video_jb_incomplete_frames    = 0;
    std::uint64_t video_jb_incomplete_frames_prev = 0;

    /** The negotiation concluded that we cannot decode the peer's video.
     *
     *  Set when every payload type the peer offered names an encoding
     *  NimRTC has no decoder for (the video counterpart of A-9 / B-4).
     *  The receive track is then disabled and the answer rejects the
     *  video m-line (RFC 3264 §6.1: port 0, no format list). */
    bool video_rx_disabled = false;

    /** The "the peer offered a video codec we cannot decode" error has
     *  already been reported to the host.  Reported once per
     *  negotiation, not once per packet. */
    bool video_codec_unsupported_reported = false;

    // ---- Ref-frame timeline map (v1.1 T1/T2, UNSTABLE) -----------------
    //
    // Local registry: ref_frame id (== VideoSourceFrame::frame_seq) ->
    // capture_ts_us.  Bounded; insertion-order drives eviction so the
    // registry cannot grow without bound.  Single-threaded by engine
    // contract (same as sr_tracker); no locking.
    //
    // The table is a collaborator class of its own
    // (`detail::RefFrameTable`): it owns its storage and knows nothing
    // about the engine, so it can be reasoned about — and tested — on
    // its own.  Phase 3 replaced a `std::map` + a `std::deque` pair
    // with it, because those allocated twice per sent frame, forever.
    RefFrameTable ref_frames;

    // ---- The video half of the outbound Sender Report ------------------
    std::uint32_t video_rtp_timestamp   = 0;
    std::uint32_t video_packet_count    = 0;
    std::uint32_t video_octet_count     = 0;

    // ---- Video: the send instant of the last video packet --------------
    //
    // The video counterpart of `audio_last_send_unix_us`.  The video
    // path stamps the abs-send-time extension (ADR-017) on every
    // outbound video packet; the Sender Report for the video stream
    // reuses the same instant so the report's NTP field denotes the
    // exact send instant of the packet whose RTP timestamp it
    // carries — the same pairing the audio path already had.  Without
    // it the video stream's timeline reconstruction inherits the
    // report-cadence bias the audio path fixed.
    //
    // Zero means "no packet sent yet".
    std::int64_t video_last_send_unix_us = 0;

    // ---- The capture → engine pump (remote-desktop plan A10) -------------
    //
    // The pump pops frames from the capture device's bounded queue and
    // drives them through the encoder **on the tick thread**.  The
    // counters below make the pump observable: a host that expects an
    // automatic stream sees `video_capture_pumped_frames` move; a
    // session where the pump never runs sees it stuck at zero, which
    // is the difference between "the device produced nothing" and
    // "the pump is not wired".

    /** Frames the pump took from the capture device and pushed onto the
     *  wire. */
    std::uint64_t video_capture_pumped_frames = 0;

    /** Frames the pump took but could not encode (or could not push). */
    std::uint64_t video_capture_pump_errors = 0;

    /** Metadata-only frames the pump skipped (no pixels to encode). */
    std::uint64_t video_capture_pumped_metadata = 0;

    /** True once the pump has established its media-clock origin. */
    bool capture_rtp_base_valid = false;

    // ---- The format normalisation counters (remote-desktop plan C3) -----
    //
    // The capture path may deliver BGRA / NV12 / I420; the encoder path
    // wants one canonical format.  The engine does the conversion and
    // counts what it did, so a host can see the cost.
    /** Frames that needed a colour-space conversion (BGRA → I420, ...). */
    std::uint64_t video_format_conversions = 0;
    /** Frames that were copied without a colour conversion. */
    std::uint64_t video_format_copies = 0;
    /** Frames that reached the encoder as a native GPU surface. */
    std::uint64_t video_format_zero_copy = 0;
    /** The canonical format the encoder is fed. */
    plugins::VideoPixelFormat video_format_canonical =
        plugins::VideoPixelFormat::kUnknown;
    /** The format the capture device most recently delivered. */
    plugins::VideoPixelFormat video_format_source =
        plugins::VideoPixelFormat::kUnknown;

    // ---- The backend preference (remote-desktop plan B2) ----------------
    //
    // The class of backend the host asked for.  The engine reports it in
    // the stats next to the backend it actually got, so a mismatch is
    // visible without reading a log.
    plugins::VideoCodecBackendPreference video_codec_backend_preference =
        plugins::VideoCodecBackendPreference::kAuto;

    /** The capture timestamp (microseconds) the media clock's origin is
     *  anchored at. */
    std::int64_t capture_rtp_base_us = 0;

    /** The RTP timestamp (90 kHz) the media clock's origin is anchored
     *  at. */
    std::uint32_t capture_rtp_base_ts = 0;

    // ---- The encoder's parameter set (remote-desktop plan B12 / B13) -----
    //
    // The engine computes the parameter set in one place (see
    // `engine_video_params.hpp`) and pushes it through
    // `IVideoCodec::set_params()`.  These two fields are the memory of
    // that: what the encoder was last told, and what geometry it was
    // last told about.
    //
    // The geometry matters separately because a *geometry* change is a
    // parameter-set change (B13): the encoder must restart and emit the
    // new SPS / PPS with the next frame, so the peer decodes it without
    // asking for a keyframe.
    /** The parameter set currently in force (the last one the codec
     *  accepted). */
    plugins::VideoEncoderParams video_encoder_params{};

    /** True once the engine has pushed a parameter set to the codec. */
    bool video_encoder_params_set = false;
    /** The number of times the engine had to restart the encoder because
     *  the geometry changed.  A non-zero value means the ladder (or the
     *  capture device) changed resolution mid-session. */
    std::uint64_t video_encoder_restarts = 0;

    // ---- The per-stage CPU-time instrumentation (plan J1) -----------------
    //
    // The plan's exit measurement is written in terms of time ("CPU time
    // per frame (capture → encode → packetize)"), so the engine measures
    // each stage of the pipeline.  The accumulator lives here so it is
    // reset with the rest of the session state.
    StageTimings stage_timings{};

    // ---- The video stream set (remote-desktop plan H1 / H2) -------------
    //
    // The set owns the *extra* video streams — everything except the
    // default one, whose plugin instances stay in the engine's own
    // members (`video_codec_`, `video_sender_`, ...; see the plan's R-3
    // row: the refactor is incremental and revertible).
    //
    // It is the single view the iteration, the statistics (plan H4) and
    // the inbound routing use, so a host that never touches the
    // multi-stream API sees exactly the behaviour it saw before.
    VideoStreamSet video_streams;

    // ---- The receive / presentation plane (remote-desktop plan E5-E9) ---
    //
    // The render clock (E5), the displayed-frame record (E9), the
    // presentation configuration (E7) and the input-echo decision (E6)
    // are one concern: what the *viewer* sees and how it is driven.
    // They live in a named collaborator so this header stays a state
    // list rather than a catch-all.
    RenderState render;
};

}  // namespace nimrtc::engine::detail
