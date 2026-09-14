/**
 * @file nimrtc/video_stats/video_stats.hpp
 * @brief Per-stream video statistics (frames, bitrate, freeze, RTCP feedback).
 *
 * Per ARCHITECTURE.md §9: "每路会话可独立拉取：RTT、丢包、抖动、码率、
 * BWE 决策轨迹" — this module is the P1 video stat scaffolding that
 * provides the "码率" (bitrate), "丢包" (drop), and frame timeline
 * primitives without depending on the BWE or transport layer.
 *
 * Per §6 "P3 弱网基线" — this module is the P1 stat foundation that will
 * feed the P3 weak-network quality-of-experience metrics.
 *
 * ## Design
 *
 * The public interface is `IVideoStatsSink` — callers (jitter buffer,
 * decoder, renderer) push events into it.  The implementation collects
 * counters internally and exposes a snapshot via `VideoStreamStats`.
 *
 * No codec dependency, no network I/O, no engine integration.
 * Thread-safe: counters are `std::atomic`; the caller is responsible
 * for serialising calls to each sink instance.
 *
 * @note P1. Standalone module; not yet wired into the engine.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace nimrtc::video_stats {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/** Tunable parameters for a stats sink. */
struct StatsConfig {
    /** Gap (ms) between two rendered frames that counts as a freeze. */
    std::uint32_t freeze_threshold_ms = 150;

    /** Sliding window size (ms) for the current bitrate estimate. */
    std::uint32_t bitrate_window_ms = 1000;

    /** EMA smoothing factor for the running mean of inter-keyframe distance. */
    double ema_alpha = 0.1;
};

// ---------------------------------------------------------------------------
// Drop reason
// ---------------------------------------------------------------------------

enum class DropReason : std::uint8_t {
    kJitterBufferTimeout = 0,
    kDecoderError        = 1,
    kDependencyLost     = 2,
};

// ---------------------------------------------------------------------------
// RTCP feedback kind
// ---------------------------------------------------------------------------

enum class RtcpFeedbackKind : std::uint8_t {
    kPLI  = 0,
    kFIR  = 1,
    kNACK = 2,
    kREMB = 3,
};

// ---------------------------------------------------------------------------
// VideoStreamStats — snapshot returned to callers
// ---------------------------------------------------------------------------

/** A consistent snapshot of all counters.  All fields are
 *  std::uint64_t unless noted. */
struct VideoStreamStats {
    // Frame counters
    std::uint64_t frames_received       = 0;
    std::uint64_t frames_decoded        = 0;
    std::uint64_t frames_rendered      = 0;
    std::uint64_t frames_dropped_jb    = 0;  // dropped at jitter buffer
    std::uint64_t frames_dropped_decoder = 0; // dropped by decoder
    std::uint64_t keyframes_received    = 0;

    // Keyframe timing
    double       keyframe_interval_avg_ms = 0.0; // EMA over inter-keyframe gap

    // Byte / bitrate
    std::uint64_t bytes_received      = 0;
    std::uint32_t current_bitrate_bps = 0;  // EMA over last 1s window
    std::uint32_t avg_bitrate_bps     = 0;  // session average

    // Freeze / quality
    std::uint64_t freeze_count    = 0;
    std::uint64_t freeze_total_ms = 0;
    std::uint64_t last_freeze_ms  = 0;

    // Timeline
    std::int64_t first_frame_ts_us = -1;  // -1 means no frame seen yet
    std::int64_t last_frame_ts_us  = -1;

    // QP
    std::uint64_t qp_sum   = 0;
    std::uint64_t qp_count = 0;
};

// ---------------------------------------------------------------------------
// IVideoStatsSink — push interface (called by JB, decoder, renderer)
// ---------------------------------------------------------------------------

class IVideoStatsSink {
public:
    virtual ~IVideoStatsSink() = default;

    /** Called for every received encoded frame (before decode). */
    virtual void on_frame_received(std::int64_t capture_ts_us,
                                   std::uint32_t frame_size_bytes,
                                   bool          is_keyframe,
                                   int           qp) = 0;

    /** Called after successful decode of a frame. */
    virtual void on_frame_decoded(std::int64_t capture_ts_us) = 0;

    /** Called when a frame is presented to the viewer. */
    virtual void on_frame_rendered(std::int64_t capture_ts_us) = 0;

    /** Called when a frame is dropped at any stage. */
    virtual void on_frame_dropped(DropReason reason) = 0;

    /** Called when a RTCP feedback packet is sent or received. */
    virtual void on_rtcp_feedback(RtcpFeedbackKind kind) = 0;

    /** Called with a per-frame QP sample (0–63 for H.264). */
    virtual void on_qp_sample(int qp) = 0;

    /** Drive timers: freeze detection, bitrate window management.
     *  Call once per second from the caller's timer thread. */
    virtual void tick(std::int64_t now_us) = 0;

    /** Return a consistent snapshot of all counters. */
    virtual VideoStreamStats snapshot() const = 0;

    /** Reset all counters to zero. */
    virtual void reset() = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/** Create a new stats sink with the given config. */
[[nodiscard]] std::unique_ptr<IVideoStatsSink>
create_stats_sink(StatsConfig cfg = {});

} // namespace nimrtc::video_stats
