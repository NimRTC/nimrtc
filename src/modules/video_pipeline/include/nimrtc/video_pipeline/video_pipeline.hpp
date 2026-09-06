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
 * modules.  It is **not yet wired into the engine**; the engine can later
 * adopt these classes (or replace them with an optimized version that
 * shares state with the engine's send/receive paths).
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
 * @note P1. Standalone module; not yet wired into the engine.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>
#include <nimrtc/video_frame/frame.hpp>
#include <nimrtc/video_jb/video_jitter_buffer.hpp>
#include <nimrtc/rtp/packet.hpp>

namespace nimrtc::video_pipeline {

// ---------------------------------------------------------------------------
// H.264 packetization mode (RFC 6184 §6 / §8.2).  We re-declare this enum
// rather than depending on video_sdp_attributes so that consumers of
// `video_pipeline` don't need to pull SDP-format helpers.
// ---------------------------------------------------------------------------

enum class PacketizationMode : std::uint8_t {
    kSingleNal     = 0,   ///< only single-NALU packets allowed
    kNonInterleaved = 1,  ///< Single NAL + STAP-A + FU-A
    kInterleaved    = 2,  ///< STAP-B + FU-B; not implemented (RFC reserved)
};

// ---------------------------------------------------------------------------
// Receiver configuration
// ---------------------------------------------------------------------------

struct ReceiverConfig {
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

struct ReceiverStats {
    std::uint64_t packets_pushed       = 0;
    std::uint64_t frames_emitted       = 0;
    std::uint64_t frames_incomplete    = 0;
    std::uint64_t fua_fragments_seen   = 0;
    std::uint64_t stap_a_packets_seen  = 0;
    std::uint64_t nack_entries_emitted = 0;
    std::uint64_t sequence_gaps_seen   = 0;
};

// ---------------------------------------------------------------------------
// Receiver callback
// ---------------------------------------------------------------------------

/** Called for every assembled video frame that the receiver emits.
 *  The frame's `bitstream` is owned by the receiver and is only valid
 *  for the duration of the callback; copy out if you need to keep it.
 *  - `frame` is the assembled compressed access unit.
 *  - `now_us` is the arrival time at which the JB finally emitted it. */
using ReceiverFrameCallback =
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
class VideoReceiver {
public:
    explicit VideoReceiver(ReceiverConfig cfg);
    ~VideoReceiver();

    VideoReceiver(const VideoReceiver&)            = delete;
    VideoReceiver& operator=(const VideoReceiver&) = delete;
    VideoReceiver(VideoReceiver&&)                 = default;
    VideoReceiver& operator=(VideoReceiver&&)      = default;

    /** Register the frame callback (must be set before push_rtp). */
    void set_frame_callback(ReceiverFrameCallback cb) noexcept;

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

struct SenderConfig {
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
};

// ---------------------------------------------------------------------------
// Sender statistics
// ---------------------------------------------------------------------------

struct SenderStats {
    std::uint64_t frames_pushed                = 0;
    std::uint64_t packets_emitted              = 0;
    std::uint64_t fua_frames_packetized        = 0;
    std::uint64_t stapa_frames_packetized      = 0;
    std::uint64_t single_nal_frames_packetized = 0;
    std::uint64_t force_keyframe_calls         = 0;
    std::uint64_t frames_marked_as_keyframe    = 0;
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
using SenderPacketCallback =
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
class VideoSender {
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

    /** Snapshot of sender counters. */
    SenderStats stats() const noexcept;

    /** Currently allocated next sequence number (peek for tests). */
    std::uint16_t next_seq() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nimrtc::video_pipeline