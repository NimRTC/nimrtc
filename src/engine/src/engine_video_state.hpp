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
#include <string>

#include <nimrtc/plugins/video_pipeline.hpp>

#include "audio_routing.hpp"
#include "engine_ref_frame_table.hpp"
#include "video_codec_config.hpp"
#include "video_negotiation.hpp"
#include "video_stats_bridge.hpp"

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

    // ---- Video capture counters (remote-desktop plan A11) --------------
    //
    // The capture device owns the detailed bookkeeping (the seam's
    // `CaptureStats`); the engine republishes the numbers a host
    // watching a live session needs.  All cumulative since open().
    std::uint64_t video_capture_frames        = 0;  // frames captured by the device
    std::uint64_t video_capture_frames_pumped = 0;  // frames handed to the encoder
    std::uint64_t video_capture_dropped       = 0;  // queue-overflow drops
    std::uint64_t video_capture_device_errors = 0;  // device-lost / read errors
    std::uint64_t video_capture_restarts      = 0;  // times the device was reopened
    std::uint64_t video_capture_format_changes = 0; // negotiated format changed
    std::uint64_t video_capture_bytes_copied  = 0;  // CPU<->GPU bytes (0 = zero-copy)

    // ---- Zero-copy rejection (remote-desktop plan B10) -----------------
    //
    // `video_zero_copy_frames` counts the frames that reached the encoder
    // as a native GPU surface (no CPU readback); `video_zero_copy_rejected`
    // counts the ones the selected encoder could not consume.  The two
    // together say whether the zero-copy path is actually working.
    std::uint64_t video_zero_copy_rejected = 0;

    // The number of frames that went through the CPU staging path (the
    // complement of `video_zero_copy_frames`).
    std::uint64_t video_staged_frames = 0;

    // ---- Codec telemetry counters (remote-desktop plan B15) ------------
    //
    // The codec's own `VideoCodecStats` carries the authoritative numbers;
    // this tracks the engine-side failures the codec cannot see (a frame
    // that never reached it).
    std::uint64_t video_codec_errors = 0;

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
};

}  // namespace nimrtc::engine::detail
