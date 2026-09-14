/**
 * @file nimrtc/video_jb/video_jitter_buffer.hpp
 * @brief Video-aware jitter buffer (RFC 6184 / 7741 / 9559).
 *
 * Per ARCHITECTURE.md §6:
 *   - JitterBuffer: 音/视频策略抽象（低延迟通话 / 直播 / 对讲三档默认策略），
 *     P1 先交付固定窗口实现，P3 替换为自适应 JB。
 *   - Audio and video share the *interface* (IJB) but have different
 *     "frame" semantics — for audio a frame is fixed-size PCM; for video
 *     a frame is one compressed access unit (one or more NAL units for H.264,
 *     one partition for VP8).
 *
 * This module implements a video-aware jitter buffer on top of the existing
 * audio JB (modules/jb) infrastructure:
 *
 *   - Reorders packets by RTP sequence number (gap detection → NACK list)
 *   - Reassembles FU-A fragments into complete NAL units
 *   - Detects frame boundaries (H.264 marker bit / VP8 partition index /
 *     VP9 picture ID)
 *   - Emits complete EncodedVideoFrame(s) when a frame is fully assembled
 *   - Maintains frame-level capture_ts / frame_seq / rtp_timestamp for
 *     timeline alignment (§8.4)
 *
 * The JB is frame-aware: it holds packets of multiple frames in flight
 * simultaneously (since one frame can span many RTP packets). When all
 * packets of frame N have arrived, frame N is emitted and removed from
 * the JB; when frame N+1 is partially assembled, those packets are kept.
 *
 * ## Failure modes
 *
 *   - Packet loss: tracked per frame; the JB emits a NACK list (sequence
 *     numbers) for the receiver to send back via RTCP.
 *   - Frame timeout: if a frame's first packet arrived >max_wait_ms ago
 *     and the frame isn't complete, the frame is dropped (encoder will
 *     send a keyframe).
 *   - Buffer overflow: oldest in-flight frame is dropped (audio JB does
 *     the same; we mirror the policy here for consistency).
 *
 * @note P1. Standalone module; not yet wired into the engine.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>
#include <nimrtc/video_frame/frame.hpp>

namespace nimrtc::video_jb {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

enum class Mode : std::uint8_t {
    kLowLatency  = 0,   ///< minimum delay; for call / agent / teleop
    kLive        = 1,   ///< moderate delay; for live broadcast
    kInteractive = 2,   ///< max robustness; for hostile networks
};

struct Config {
    Mode mode = Mode::kLowLatency;

    /** Maximum number of in-flight frames before dropping the oldest. */
    std::uint32_t max_inflight_frames = 8;

    /** Maximum time to wait for a missing packet before declaring the
     *  frame complete and emitting it (or dropping it if non-keyframe). */
    std::uint32_t max_wait_ms = 200;

    /** Whether to report lost packets via `pop_nack_entries()`. */
    bool emit_nacks = true;
};

// ---------------------------------------------------------------------------
// Incoming packet descriptor
// ---------------------------------------------------------------------------

/** One RTP packet handed to the jitter buffer. The buffer does NOT take
 *  ownership of `payload` — it copies the bytes internally. */
struct InboundPacket {
    std::uint32_t ssrc          = 0;
    std::uint16_t seq           = 0;
    std::uint32_t rtp_timestamp = 0;
    std::int64_t  arrival_us    = 0;     ///< monotonic-clock arrival time
    bool          marker        = false; ///< RTP marker bit
    std::uint8_t  payload_type  = 0;

    /** Codec kind of this stream. The JB only fully supports H.264 + FU-A
     *  reassembly; VP8 / VP9 packets are treated as opaque partitions. */
    video_frame::CodecKind codec = video_frame::CodecKind::kUnknown;

    /** Compressed payload (RTP payload field, header already stripped). */
    core::ByteSpan          payload;
};

// ---------------------------------------------------------------------------
// Assembled frame
// ---------------------------------------------------------------------------

struct AssembledFrame {
    std::uint32_t              ssrc          = 0;
    std::uint32_t              rtp_timestamp = 0;
    std::uint32_t              frame_seq     = 0;
    std::uint64_t              capture_ts_us = 0;
    bool                       is_keyframe   = false;
    video_frame::CodecKind     codec         = video_frame::CodecKind::kUnknown;

    /** Concatenated NAL units (or partition 0 bitstream for VP8). */
    std::vector<std::uint8_t>  bitstream;

    /** Whether the frame was complete when emitted. False ⇒ we gave up
     *  waiting and dropped some packets (still emit for diagnostics). */
    bool                       complete      = true;
};

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

struct Stats {
    std::uint64_t packets_inserted  = 0;
    std::uint64_t packets_reordered = 0;
    std::uint64_t packets_dropped   = 0;
    std::uint64_t frames_emitted    = 0;
    std::uint64_t frames_incomplete = 0;
    std::uint64_t nacks_emitted     = 0;
};

// ---------------------------------------------------------------------------
// IVideoJitterBuffer — abstract interface (matches plugins/jb IJB pattern)
// ---------------------------------------------------------------------------

class IVideoJitterBuffer {
public:
    virtual ~IVideoJitterBuffer() = default;

    /** Insert one packet. Returns true if the packet was accepted (always
     *  true except for malformed input). The JB may emit zero or more
     *  frames after this call. */
    virtual bool insert(const InboundPacket& pkt) noexcept = 0;

    /** Pop the next available completed frame. Returns nullptr if none. */
    virtual std::unique_ptr<AssembledFrame> pop_frame() noexcept = 0;

    /** Pop any pending NACK entries (lost packet seq numbers). */
    virtual std::vector<std::uint16_t> pop_nack_entries() noexcept = 0;

    /** Drive timers: drop frames that exceeded max_wait_ms. */
    virtual void tick(std::int64_t now_us) noexcept = 0;

    /** Reset state (new stream). */
    virtual void reset() noexcept = 0;

    /** Snapshot stats. */
    virtual Stats stats() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/** Create a video jitter buffer with the given config. */
std::unique_ptr<IVideoJitterBuffer> create(Config cfg);

} // namespace nimrtc::video_jb
