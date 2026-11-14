/**
 * @file nimrtc/video_pipeline/video_pipeline.hpp
 * @brief Video receive / send pipeline glue layer.
 *
 * Per ARCHITECTURE.md:
 *   - §6 pipeline pieces
 *   - §8.4 media-data unified timeline (capture_ts at frame level)
 *   - §11.6 pipelining responsibility
 *
 * Pipeline order (both directions):
 *
 *   Receive:  RTP packet -> payload decode (NACK list) -> JB
 *                  -> frame emit -> codec decode -> raw VideoFrame
 *
 *   Send:     EncodedVideoFrame -> bitstream split -> packet_callback
 *                  (RTP header is prepended by the caller; we emit RTP
 *                   payloads only)
 *
 * This module is a **reference implementation** sitting on top of the
 * existing `video_payload`, `video_jb`, `video_frame`, `rtp`, and `core`
 * modules.  The engine instantiates the reference receiver/sender
 * through the `video_pipeline` plugin seam (`reference` factory) and
 * forwards `EngineConfig::video_receiver_tuning` (including the opt-in
 * `adaptive_playout` switch for the shared `nimrtc::jb_strategy`
 * playout delay).
 *
 * ## Important contract notes
 *
 *   - The pipeline does NOT construct RTP headers.  Each emitted packet
 *     callback receives the RTP payload bytes (the bytes that would go
 *     between the 12-byte fixed RTP header and the next packet).  The
 *     caller is responsible for prepending an RTP header with seq,
 *     timestamp, SSRC, marker bit, and (optionally) extension data.
 *   - The pipeline does NOT own the underlying memory of packets it
 *     receives (it copies what it needs into the jitter buffer).  See
 *     `video_jb::InboundPacket::payload` for the convention.
 *   - The pipeline is synchronous and single-threaded.  Callbacks fire on
 *     the same thread that called `push_rtp()`.  Concurrent access is
 *     the caller's responsibility.
 *   - For H.264, the pipeline itself runs the FU-A reassembly state
 *     machine (RFC 6184 §5.8).  When a complete NAL unit is produced,
 *     it is handed to the jitter buffer as a single-NAL packet.
 *
 * ## Feedback loop (v1.1 Q3)
 *
 * The receiver tells the sender what it is missing: the sequence
 * numbers of lost packets (RFC 4585 §6.2.1 NACK) and the request for a
 * new key frame when picture data was lost (RFC 4585 §6.3.1 PLI /
 * RFC 5104 §4.3.1 FIR).  The sender keeps the packets it sent in a
 * bounded cache so a NACK can be answered with a retransmission of the
 * exact packet that was lost.
 *
 * The wire format of the feedback itself is *not* built here — that is
 * the job of the `nimrtc::video_rtcp_feedback` module (RTCP PSFB /
 * RTPFB).  This module only produces and consumes the plain data
 * (sequence numbers, key-frame requests) so that the two can be wired
 * together by whatever owns the RTCP transport.
 *
 * @note P1. Reference implementation; wired into the engine through
 * the `video_pipeline` plugin seam (`reference` factory).
 */

#pragma once


// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>
#include <nimrtc/video_frame/frame.hpp>
#include <nimrtc/video_jb/video_jitter_buffer.hpp>
#include <nimrtc/video_pipeline/keyframe_limiter.hpp>
#include <nimrtc/video_rtcp_feedback/video_rtcp_feedback.hpp>
#include <nimrtc/rtp/packet.hpp>

namespace nimrtc::video_pipeline {

// ---------------------------------------------------------------------------
// H.264 packetization mode (RFC 6184 §6 / §8.2).  We re-declare this enum
// rather than depending on video_sdp_attributes so that consumers of
// `video_pipeline` don't need to pull SDP-format helpers.
// ---------------------------------------------------------------------------

enum class NIMRTC_API_STABLE PacketizationMode : std::uint8_t {
    kSingleNal     = 0,   ///< only single-NALU packets allowed
    kNonInterleaved = 1,  ///< Single NAL + STAP-A + FU-A
    kInterleaved    = 2,  ///< STAP-B + FU-B; not implemented (RFC reserved)
};

// ---------------------------------------------------------------------------
// Receiver configuration
// ---------------------------------------------------------------------------

struct NIMRTC_API_STABLE ReceiverConfig {
    /** Codec of the inbound stream.  Determines which depacketizer path
     *  the receiver uses. */
    video_frame::CodecKind codec         = video_frame::CodecKind::kH264;

    /** RTP SSRC of the inbound stream (used for validation / JB bucket
     *  pre-population).  If the packet's SSRC differs, we still accept it. */
    std::uint32_t          ssrc          = 0;

    /** RTP payload type (e.g. 102 for H.264 in WebRTC). */
    std::uint8_t           payload_type  = 0;

    /** Forwarded to the underlying video jitter buffer. */
    video_jb::Config       jitter_buffer_config{};

    /** If true, the receiver runs FU-A reassembly itself (recommended). */
    bool                   expect_fu_a   = true;
};

// ---------------------------------------------------------------------------
// Receiver statistics
// ---------------------------------------------------------------------------

struct NIMRTC_API_STABLE ReceiverStats {
    std::uint64_t packets_pushed       = 0;
    std::uint64_t frames_emitted       = 0;
    std::uint64_t frames_incomplete    = 0;
    std::uint64_t fua_fragments_seen   = 0;
    std::uint64_t stap_a_packets_seen  = 0;
    std::uint64_t nack_entries_emitted = 0;
    std::uint64_t sequence_gaps_seen   = 0;
    /** Number of key-frame requests the receiver raised because picture
     *  data was lost (v1.1 Q3). */
    std::uint64_t keyframe_requests_emitted = 0;
};

// ---------------------------------------------------------------------------
// Receiver feedback (v1.1 Q3)
// ---------------------------------------------------------------------------

/** Feedback the receiver wants to send back to the sender.
 *
 *  `missing_packets` holds the RTP sequence numbers the receiver
 *  detected as lost — the NACK candidates of RFC 4585 §6.2.1.
 *  `keyframe_requested` is set when the receiver lost picture data and
 *  therefore needs a new key frame (RFC 4585 §6.3.1 PLI, or
 *  RFC 5104 §4.3.1 FIR).
 *
 *  The RTCP wire format itself lives in `nimrtc::video_rtcp_feedback`;
 *  this structure carries the plain data between the two ends. */
struct NIMRTC_API_STABLE ReceiverFeedback {
    std::vector<std::uint16_t> missing_packets;
    bool                       keyframe_requested = false;
};

/** Called when the receiver has feedback for the sender.
 *  Fired synchronously on the thread that called push_rtp() / tick(). */
using NIMRTC_API_STABLE ReceiverFeedbackCallback =
    std::function<void(const ReceiverFeedback& feedback)>;

// ---------------------------------------------------------------------------
// Receiver callback
// ---------------------------------------------------------------------------

/** Called for every assembled video frame that the receiver emits.
 *  The frame's `bitstream` is owned by the receiver and is only valid
 *  for the duration of the callback; copy out if you need to keep it.
 *  - `frame` is the assembled compressed access unit.
 *  - `now_us` is the arrival time at which the JB finally emitted it. */
using NIMRTC_API_STABLE ReceiverFrameCallback =
    std::function<void(video_frame::EncodedVideoFrame frame,
                       std::int64_t now_us)>;

// ---------------------------------------------------------------------------
// VideoReceiver
// ---------------------------------------------------------------------------

/**
 * @brief Reference video receiver.
 *
 * Pipeline per push:
 *   parse RTP payload -> (H.264) FU-A / Single / STAP-A dispatch
 *                  -> build InboundPacket -> jb_->insert
 *                  -> drain jb_->pop_frame -> fire user callback
 *
 * Sequence number tracking is done in the receiver itself; `pop_nack_entries()`
 * returns the missing seq numbers.  The JB's own NACK list is also drained
 * each `push_rtp()` to surface fragment-level losses (FU-A fragments, etc.)
 * that pass through unchanged when FU-A handling is disabled.
 *
 * Single-threaded.
 */
class NIMRTC_API_STABLE VideoReceiver {
public:
    explicit VideoReceiver(ReceiverConfig cfg);
    ~VideoReceiver();

    VideoReceiver(const VideoReceiver&)            = delete;
    VideoReceiver& operator=(const VideoReceiver&) = delete;
    VideoReceiver(VideoReceiver&&)                 = default;
    VideoReceiver& operator=(VideoReceiver&&)      = default;

    /** Register the frame callback (must be set before push_rtp). */
    void set_frame_callback(ReceiverFrameCallback cb) noexcept;

    /** Register the feedback callback (optional, v1.1 Q3).
     *
     *  The callback fires when the receiver detects a gap in the RTP
     *  sequence numbers (the lost packets of a NACK) and when a frame
     *  was force-emitted without all of its data (the key-frame request
     *  of a PLI / FIR). */
    void set_feedback_callback(ReceiverFeedbackCallback cb) noexcept;

    /** Feed a parsed RTP packet.  The receiver extracts seq, marker,
     *  ssrc, timestamp, and payload, then runs the receive pipeline.
     *  Any assembled frames fire the user callback synchronously.
     *  @param packet  RTP packet (header already parsed by rtp::Parser).
     *  @param now_us  Monotonic-clock arrival time (microseconds).
     *  @return true if the packet was accepted; false on malformed input. */
    bool push_rtp(const rtp::PacketView& packet, std::int64_t now_us) noexcept;

    /** Pop the accumulated NACK list (missing packet sequence numbers). */
    std::vector<std::uint16_t> pop_nack_entries() noexcept;

    /** Drive JB timers (drop frames past max_wait_ms). */
    void tick(std::int64_t now_us) noexcept;

    /** Clear FU-A reassembly state + reset the JB. */
    void reset() noexcept;

    /** Snapshot of receiver counters. */
    ReceiverStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Sender configuration
// ---------------------------------------------------------------------------

struct NIMRTC_API_STABLE SenderConfig {
    video_frame::CodecKind  codec              = video_frame::CodecKind::kH264;
    std::uint32_t           ssrc               = 0;
    std::uint8_t            payload_type       = 0;
    /** Maximum RTP payload size in bytes (default 1200 — well below the
     *  typical path MTU of ~1500 with safety margin for IP/UDP headers). */
    std::uint32_t           mtu                = 1200;
    /** H.264 packetization mode.  Only used when codec == H264. */
    PacketizationMode       packetization_mode = PacketizationMode::kNonInterleaved;

    /** Initial RTP sequence number (randomised by the caller in
     *  production; the pipeline just increments from this base). */
    std::uint16_t           initial_seq        = 0;

    /** Number of sent packets kept for retransmission (v1.1 Q3).
     *
     *  A NACK asks the sender to repeat one packet; the sender can only
     *  do that if it still holds the packet.  The cache is a ring buffer
     *  of the most recently sent packets.  0 disables the cache (the
     *  sender then answers every NACK with a miss). */
    std::size_t             retransmit_cache_packets = 256;
};

// ---------------------------------------------------------------------------
// Sender statistics
// ---------------------------------------------------------------------------

struct NIMRTC_API_STABLE SenderStats {
    std::uint64_t frames_pushed                = 0;
    std::uint64_t packets_emitted              = 0;
    std::uint64_t fua_frames_packetized        = 0;
    std::uint64_t stapa_frames_packetized      = 0;
    std::uint64_t single_nal_frames_packetized = 0;
    std::uint64_t force_keyframe_calls         = 0;
    std::uint64_t frames_marked_as_keyframe    = 0;
    /** Packets re-sent in response to a NACK (v1.1 Q3). */
    std::uint64_t packets_retransmitted        = 0;
    /** NACKs that could not be answered because the packet had already
     *  left the retransmission cache (v1.1 Q3). */
    std::uint64_t packets_retransmit_missed    = 0;
    // ---- The key-frame storm limiter (remote-desktop plan B14) -----------
    /** Key-frame requests the limiter was offered. */
    std::uint64_t keyframe_requests             = 0;
    /** Key-frame requests the limiter granted (a key frame was produced). */
    std::uint64_t keyframes_granted             = 0;
    /** Key-frame requests the limiter deferred (held back, not dropped). */
    std::uint64_t keyframes_deferred            = 0;
    /** Key-frame requests the limiter rejected (the budget was spent). */
    std::uint64_t keyframes_rejected            = 0;
};

// ---------------------------------------------------------------------------
// Sender callback
// ---------------------------------------------------------------------------

/** Called for each emitted RTP packet payload.
 *  - `rtp_packet_payload` is the bytes that go AFTER the 12-byte RTP
 *    fixed header (i.e. the RTP payload field).  The receiver-side
 *    caller is expected to construct the full RTP packet by prepending
 *    its own RTP header.
 *  - `rtp_timestamp` is the 32-bit RTP media clock timestamp.
 *  - `seq` is the per-stream 16-bit RTP sequence number.
 *  - `marker` is the RTP marker bit (true on the last packet of an AU).
 *
 *  The span is only valid for the duration of the callback; the sender
 *  reuses an internal buffer for subsequent packets. */
using NIMRTC_API_STABLE SenderPacketCallback =
    std::function<void(core::ByteSpan rtp_packet_payload,
                       std::uint32_t rtp_timestamp,
                       std::uint16_t seq,
                       bool marker)>;

// ---------------------------------------------------------------------------
// VideoSender
// ---------------------------------------------------------------------------

/**
 * @brief Reference video sender.
 *
 * Pipeline per push:
 *   split bitstream into NALUs / VP8 partitions
 *                  -> (H.264) decide FU-A vs STAP-A vs Single
 *                  -> emit RTP payload via packet_callback
 *
 * Sequence numbers and the seq_base advance monotonically; the RTP
 * timestamp is caller-supplied (e.g. 90000 / fps).
 *
 * Single-threaded.
 */
class NIMRTC_API_STABLE VideoSender {
public:
    explicit VideoSender(SenderConfig cfg);
    ~VideoSender();

    VideoSender(const VideoSender&)            = delete;
    VideoSender& operator=(const VideoSender&) = delete;
    VideoSender(VideoSender&&)                 = default;
    VideoSender& operator=(VideoSender&&)      = default;

    /** Register the per-packet callback. */
    void set_packet_callback(SenderPacketCallback cb) noexcept;

    /** Feed one encoded (compressed) frame.  The pipeline packetizes it
     *  and fires the callback for each emitted RTP packet payload.
     *  @param frame           The compressed access unit.
     *  @param capture_ts_us   Capture time (microseconds).  Recorded on
     *                         the EncodedVideoFrame passed downstream.
     *  @param rtp_timestamp   The 32-bit RTP timestamp (e.g. 90000/fps).
     *  @param frame_seq       Frame sequence number (per SSRC).
     *  @return core::Result<void>; fails with NotImplemented if the
     *          codec + packetization_mode combo is not supported. */
    core::Result<void> push_frame(video_frame::EncodedVideoFrame frame,
                                  std::int64_t  capture_ts_us,
                                  std::uint32_t rtp_timestamp,
                                  std::uint32_t frame_seq) noexcept;

    /** Mark the next frame as a keyframe response (PLI/FIR was received).
     *  The next push_frame() call will record this in stats; the caller
     *  is responsible for actually emitting a keyframe (i.e. for H.264 an
     *  IDR slice) at that point. */
    void force_keyframe() noexcept;

    /** True when a key frame has been requested and the request has not
     *  been consumed by a push_frame() yet (v1.1 Q3). */
    bool keyframe_pending() const noexcept;

    /** Re-emit a previously sent packet (RFC 4585 §6.2.1 NACK).
     *
     *  The packet is re-sent with its original sequence number, RTP
     *  timestamp and marker bit, so the receiver can splice it back into
     *  the frame it belongs to.
     *
     *  @param seq  The RTP sequence number to repeat.
     *  @return false when the packet is no longer in the retransmission
     *          cache (too old, or never sent by this sender). */
    bool retransmit(std::uint16_t seq) noexcept;

    /** Number of packets currently held in the retransmission cache. */
    std::size_t cached_packet_count() const noexcept;

    // ---- The key-frame storm limiter (remote-desktop plan B14) -----------
    //
    // The pipeline is where a key-frame request becomes an actual IDR:
    // `apply_feedback_rtcp()` sees the PLI / FIR, and the *encoder* is
    // the one that has to produce the IDR.  The limiter sits between
    // the two, so a burst of PLIs produces ONE key frame, not N.
    //
    // The limiter's default policy is the same interval the FIR
    // coalescer uses (250 ms — RFC 5104 §4.3.1.1's "about 4 Hz"), so
    // the two ends of the loop agree on the rate.

    /** The limiter's configuration. */
    void set_keyframe_limiter_config(KeyframeLimiterConfig cfg) noexcept;

    /** The limiter's current configuration. */
    KeyframeLimiterConfig keyframe_limiter_config() const noexcept;

    /**
     * @brief Offer a key-frame request to the limiter.
     *
     *  @param reason Why the key frame was asked for.
     *  @param now_us Monotonic timestamp in microseconds.
     *  @param carries_parameter_sets True iff the key frame must also
     *        carry the codec's parameter sets (a resolution change).
     *        Such a request is **never** deferred.
     *  @return The limiter's verdict. */
    KeyframeDecision request_keyframe(KeyframeReason reason,
                                       std::int64_t now_us,
                                       bool carries_parameter_sets = false) noexcept;

    /** The limiter's counters. */
    KeyframeLimiterStats keyframe_limiter_stats() const noexcept;

    /** Snapshot of sender counters. */
    SenderStats stats() const noexcept;

    /** Currently allocated next sequence number (peek for tests). */
    std::uint16_t next_seq() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Feedback bridge (v1.1 Q3) — receiver feedback ↔ RTCP wire format
// ---------------------------------------------------------------------------
// The receiver produces plain data (`ReceiverFeedback`); the owner of the
// RTCP transport needs bytes on the wire.  The two helpers below close that
// gap and form the closed loop:
//
//   receiver --(ReceiverFeedback)--> build_feedback_rtcp --> RTCP bytes
//   RTCP bytes --(apply_feedback_rtcp)--> retransmit / force key frame
//
// The wire format itself is owned by `nimrtc::video_rtcp_feedback`
// (RFC 4585 §6.2.1 NACK, §6.3.1 PLI, RFC 5104 §4.3.1 FIR); this module
// only wires it to the pipeline objects.

/** Number of bytes `build_feedback_rtcp()` needs for a feedback report.
 *  Zero when the report carries neither lost packets nor a key-frame
 *  request. */
NIMRTC_API_STABLE std::size_t feedback_rtcp_size(
    const ReceiverFeedback& feedback) noexcept;

/** Build the RTCP feedback bytes for a receiver feedback report.
 *
 *  A report with lost packets is encoded as a NACK (RFC 4585 §6.2.1);
 *  a report that asks for a key frame is encoded as a PLI (RFC 4585 §6.3.1).
 *  When both are present, the NACK is written first.
 *
 *  @param output      Destination buffer (size it with `feedback_rtcp_size()`).
 *  @param feedback    The feedback report from the receiver.
 *  @param sender_ssrc SSRC of the feedback sender (the receiver).
 *  @param media_ssrc  SSRC of the media source (the sender's stream).
 *  @return The number of bytes written; 0 when `output` is too small. */
NIMRTC_API_STABLE std::size_t build_feedback_rtcp(
    core::MutableByteSpan output,
    const ReceiverFeedback& feedback,
    std::uint32_t sender_ssrc,
    std::uint32_t media_ssrc) noexcept;

/** Apply an incoming RTCP feedback packet to a sender.
 *
 *  Handles the feedback kinds the video pipeline cares about:
 *   - NACK (RFC 4585 §6.2.1) → the listed packets are retransmitted.
 *   - PLI  (RFC 4585 §6.3.1) → the sender is asked for a key frame.
 *   - FIR  (RFC 5104 §4.3.1) → the sender is asked for a key frame.
 *
 *  The input may be a single feedback packet or a compound RTCP packet;
 *  every packet in the compound is processed.
 *
 *  The key-frame requests are arbitrated by the sender's key-frame storm
 *  limiter (remote-desktop plan B14): a burst of PLIs produces ONE key
 *  frame, not N.
 *
 *  @param sender The sender that answers the feedback.
 *  @param rtcp   The received RTCP bytes.
 *  @return The number of actions performed (packets retransmitted plus
 *          key-frame requests honoured); a ProtocolError when the input
 *          is not a well-formed RTCP packet. */
NIMRTC_API_STABLE core::Result<int> apply_feedback_rtcp(
    VideoSender& sender,
    core::ByteSpan rtcp) noexcept;

/** Apply an incoming RTCP feedback packet to a sender, with an explicit
 *  monotonic timestamp.
 *
 *  The timestamp is what the key-frame storm limiter compares against
 *  its minimum interval (plan B14).  A caller that has its own clock
 *  (the engine's tick thread) passes it in; a caller that does not uses
 *  the overload above, which reads the steady clock.
 *
 *  @param sender The sender that answers the feedback.
 *  @param rtcp   The received RTCP bytes.
 *  @param now_us Monotonic timestamp in microseconds.
 *  @return The number of actions performed. */
NIMRTC_API_STABLE core::Result<int> apply_feedback_rtcp(
    VideoSender& sender,
    core::ByteSpan rtcp,
    std::int64_t now_us) noexcept;

/** The monotonic clock, in microseconds.  The same epoch as
 *  `core::SteadyClock`. */
NIMRTC_API_STABLE std::int64_t monotonic_now_us() noexcept;

} // namespace nimrtc::video_pipeline