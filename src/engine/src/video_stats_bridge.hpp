/**
 * @file src/engine/src/video_stats_bridge.hpp
 * @brief Internal bridge between the engine and the `nimrtc::video_stats`
 *        module (architecture remediation plan D2 / Phase 4.2).
 *
 * ## Why a bridge instead of a direct call
 *
 * The `video_stats` module is an *optional* module: the build may be
 * configured with `-DNIMRTC_MODULE_VIDEO_STATS=OFF`.  The engine, on the
 * other hand, is always built.  So the engine cannot simply include
 * `<nimrtc/video_stats/video_stats.hpp>` — the header may not be on the
 * include path.
 *
 * This bridge makes the dependency optional *and* keeps the call sites
 * readable:
 *
 *   - with the module enabled, the bridge forwards to a real
 *     `IVideoStatsSink`;
 *   - with it disabled, every call compiles to nothing and the
 *     `Engine::Stats` quality fields stay zero.
 *
 * The second case is honest: the numbers are absent, not wrong.  A host
 * that needs them must build with the module enabled — and the CHANGELOG
 * says so.
 *
 * ## Two sinks: one per direction
 *
 * The engine keeps **two** sinks, mirroring the split Chrome exposes as
 * `inbound-rtp` and `outbound-rtp`:
 *
 *   - `rx()` — the *inbound* stream: frames received / decoded /
 *     rendered, the bitrate window, freeze detection, and the drops
 *     attributed to the jitter buffer, the decoder, or a lost reference
 *     frame.
 *   - `tx()` — the *outbound* stream: the frames the encoder produced
 *     and the QP it chose for them.
 *
 * Keeping them apart matters: the inbound freeze count and the outbound
 * QP describe two different things, and merging them would produce a
 * number that means nothing.  (The QP of an *inbound* frame is only
 * known if the codec parses the bitstream — none of the current codecs
 * do, so `EncodedVideoFrame::qp` stays -1 on the receive path.)
 *
 * ## Ownership
 *
 * The bridge owns both sinks.  They are created by the engine when the
 * video plugins are initialised and reset when they are shut down.
 *
 * ## Thread-safety
 *
 * The sinks' counters are `std::atomic`, so the receive path (which runs
 * on the transport thread) and the tick path (which runs on the engine
 * thread) may both call them concurrently.  The *bridge's* own state
 * (the sink pointers) is only mutated on the engine thread during
 * open()/close(), which happen before / after any media flows.
 *
 * @note Private to `src/engine/` — not a public header (Layout Invariant 4).
 */

#pragma once

#include <cstdint>
#include <memory>

#ifdef NIMRTC_HAS_VIDEO_STATS
#include <nimrtc/video_stats/video_stats.hpp>
#endif

namespace nimrtc::engine::detail {

// ---------------------------------------------------------------------------
// The quality counters the engine publishes (Phase 4.2 / D2).
//
// These mirror the `Engine::Stats` fields.  They are collected here so
// the engine does not need the module's types in its *public* header —
// the public header stays free of concrete module includes (Layout
// Invariant 4).
// ---------------------------------------------------------------------------

/** The snapshot of the video-quality counters. */
struct VideoQualityStats {
    std::uint64_t frames_received          = 0;
    std::uint64_t frames_decoded           = 0;
    std::uint64_t frames_rendered          = 0;
    std::uint64_t frames_dropped_jb        = 0;
    std::uint64_t frames_dropped_decoder   = 0;
    std::uint64_t frames_dropped_dependency = 0;
    std::uint64_t bytes_received           = 0;
    std::uint64_t keyframes_received       = 0;
    std::uint64_t freeze_count             = 0;
    std::uint64_t freeze_total_ms          = 0;
    std::uint64_t last_freeze_ms           = 0;
    std::uint64_t qp_sum                   = 0;
    std::uint64_t qp_count                 = 0;
    double        keyframe_interval_avg_ms = 0.0;
    std::uint32_t current_bitrate_bps      = 0;
    std::uint32_t avg_bitrate_bps          = 0;
};

/** Why a frame was dropped.  Mirrors `video_stats::DropReason`. */
enum class VideoDropReason {
    kJitterBufferTimeout = 0,
    kDecoderError        = 1,
    kDependencyLost      = 2,
};

/** Which kind of RTCP feedback was seen.  Mirrors
 *  `video_stats::RtcpFeedbackKind`. */
enum class VideoFeedbackKind {
    kPLI  = 0,
    kFIR  = 1,
    kNACK = 2,
    kREMB = 3,
};

// ---------------------------------------------------------------------------
// The bridge
// ---------------------------------------------------------------------------

/** Owns the `video_stats` sinks and forwards the engine's media events.
 *
 *  With the module disabled every method is a no-op and `snapshot()`
 *  returns an all-zero struct — the engine compiles and runs exactly as
 *  before, minus the quality counters.
 */
class VideoStatsBridge {
public:
    VideoStatsBridge() = default;

    VideoStatsBridge(const VideoStatsBridge&) = delete;
    VideoStatsBridge& operator=(const VideoStatsBridge&) = delete;

    ~VideoStatsBridge() = default;

    /** Create the sinks.  Idempotent: a second call keeps the existing
     *  sinks (and their counters). */
    void ensure_created() {
#ifdef NIMRTC_HAS_VIDEO_STATS
        if (!rx_) rx_ = video_stats::create_stats_sink({});
        if (!tx_) tx_ = video_stats::create_stats_sink({});
#endif
    }

    /** Clear every counter (used when the video plugins are shut down). */
    void reset() {
#ifdef NIMRTC_HAS_VIDEO_STATS
        if (rx_) rx_->reset();
        if (tx_) tx_->reset();
#endif
    }

    /** True when a real sink is present — i.e. the module is enabled. */
    [[nodiscard]] bool active() const noexcept {
#ifdef NIMRTC_HAS_VIDEO_STATS
        return static_cast<bool>(rx_);
#else
        return false;
#endif
    }

    // =====================================================================
    // The inbound stream
    // =====================================================================

    /** An encoded frame arrived from the peer (pre-decode). */
    void on_frame_received(std::int64_t capture_ts_us,
                           std::uint32_t frame_size_bytes,
                           bool          is_keyframe,
                           int           qp) {
#ifdef NIMRTC_HAS_VIDEO_STATS
        if (rx_) {
            rx_->on_frame_received(capture_ts_us, frame_size_bytes,
                                   is_keyframe, qp);
        }
#else
        (void)capture_ts_us; (void)frame_size_bytes;
        (void)is_keyframe; (void)qp;
#endif
    }

    /** A frame decoded successfully. */
    void on_frame_decoded(std::int64_t capture_ts_us) {
#ifdef NIMRTC_HAS_VIDEO_STATS
        if (rx_) rx_->on_frame_decoded(capture_ts_us);
#else
        (void)capture_ts_us;
#endif
    }

    /** A frame reached the sink (rendered). */
    void on_frame_rendered(std::int64_t capture_ts_us) {
#ifdef NIMRTC_HAS_VIDEO_STATS
        if (rx_) rx_->on_frame_rendered(capture_ts_us);
#else
        (void)capture_ts_us;
#endif
    }

    /** A frame was dropped somewhere on the inbound path. */
    void on_frame_dropped(VideoDropReason reason) {
#ifdef NIMRTC_HAS_VIDEO_STATS
        if (rx_) {
            rx_->on_frame_dropped(static_cast<video_stats::DropReason>(
                static_cast<std::uint8_t>(reason)));
        }
#else
        (void)reason;
#endif
    }

    /** An RTCP feedback packet was seen (inbound or outbound). */
    void on_rtcp_feedback(VideoFeedbackKind kind) {
#ifdef NIMRTC_HAS_VIDEO_STATS
        if (rx_) {
            rx_->on_rtcp_feedback(static_cast<video_stats::RtcpFeedbackKind>(
                static_cast<std::uint8_t>(kind)));
        }
#else
        (void)kind;
#endif
    }

    /** An RTCP feedback packet was seen about the **outbound** stream.
     *
     *  A NACK / PLI / FIR the *peer* sends us is about what we sent: the
     *  peer could not decode our pictures.  Counting that on the inbound
     *  sink would say "the peer's receive quality is bad", which is the
     *  wrong direction — the peer's loss is our *transmit* loss. */
    void on_rtcp_feedback_tx(VideoFeedbackKind kind) {
#ifdef NIMRTC_HAS_VIDEO_STATS
        if (tx_) {
            tx_->on_rtcp_feedback(static_cast<video_stats::RtcpFeedbackKind>(
                static_cast<std::uint8_t>(kind)));
        }
#else
        (void)kind;
#endif
    }

    /** Drive the inbound timers (freeze detection, bitrate window). */
    void tick(std::int64_t now_us) {
#ifdef NIMRTC_HAS_VIDEO_STATS
        if (rx_) rx_->tick(now_us);
#endif
    }

    /** The inbound snapshot (all-zero when the module is disabled). */
    [[nodiscard]] VideoQualityStats snapshot_rx() const {
        VideoQualityStats out;
#ifdef NIMRTC_HAS_VIDEO_STATS
        if (!rx_) return out;
        const auto s = rx_->snapshot();
        out.frames_received           = s.frames_received;
        out.frames_decoded            = s.frames_decoded;
        out.frames_rendered           = s.frames_rendered;
        out.frames_dropped_jb         = s.frames_dropped_jb;
        out.frames_dropped_decoder    = s.frames_dropped_decoder;
        out.frames_dropped_dependency = s.frames_dropped_dependency;
        out.bytes_received            = s.bytes_received;
        out.keyframes_received        = s.keyframes_received;
        out.freeze_count              = s.freeze_count;
        out.freeze_total_ms           = s.freeze_total_ms;
        out.last_freeze_ms            = s.last_freeze_ms;
        out.keyframe_interval_avg_ms  = s.keyframe_interval_avg_ms;
        out.current_bitrate_bps       = s.current_bitrate_bps;
        out.avg_bitrate_bps           = s.avg_bitrate_bps;
#endif
        return out;
    }

    // =====================================================================
    // The outbound stream
    // =====================================================================

    /** The encoder produced a frame of @p frame_size_bytes bytes with the
     *  given QP.  @p qp is -1 when the codec did not report one.
     *
     *  The QP is accumulated by the sink's `on_frame_received` (which
     *  ignores a QP outside [0, 63] — the range every current codec
     *  uses), so this must NOT also call `on_qp_sample`: that would
     *  count every sample twice. */
    void on_frame_encoded(std::int64_t capture_ts_us,
                          std::uint32_t frame_size_bytes,
                          bool          is_keyframe,
                          int           qp) {
#ifdef NIMRTC_HAS_VIDEO_STATS
        if (tx_) {
            tx_->on_frame_received(capture_ts_us, frame_size_bytes,
                                   is_keyframe, qp);
        }
#else
        (void)capture_ts_us; (void)frame_size_bytes;
        (void)is_keyframe; (void)qp;
#endif
    }

    /** The outbound snapshot (all-zero when the module is disabled). */
    [[nodiscard]] VideoQualityStats snapshot_tx() const {
        VideoQualityStats out;
#ifdef NIMRTC_HAS_VIDEO_STATS
        if (!tx_) return out;
        const auto s = tx_->snapshot();
        out.frames_received          = s.frames_received;
        out.bytes_received           = s.bytes_received;
        out.keyframes_received       = s.keyframes_received;
        out.qp_sum                   = s.qp_sum;
        out.qp_count                 = s.qp_count;
        out.frames_dropped_dependency = s.frames_dropped_dependency;
        out.keyframe_interval_avg_ms = s.keyframe_interval_avg_ms;
        out.current_bitrate_bps      = s.current_bitrate_bps;
        out.avg_bitrate_bps          = s.avg_bitrate_bps;
#endif
        return out;
    }

private:
#ifdef NIMRTC_HAS_VIDEO_STATS
    std::unique_ptr<video_stats::IVideoStatsSink> rx_;
    std::unique_ptr<video_stats::IVideoStatsSink> tx_;
#endif
};

} // namespace nimrtc::engine::detail
