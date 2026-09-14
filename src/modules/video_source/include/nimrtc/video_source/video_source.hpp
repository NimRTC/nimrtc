/**
 * @file nimrtc/video_source/video_source.hpp
 * @brief In-memory video frame source for testing / demo interop.
 *
 * Per ARCHITECTURE.md §5 (P1, "内存帧媒体源（demo/互通用）" — "in-memory
 * frame source for demo / interop"):
 *
 *   The engine MUST be exercisable end-to-end without a real camera, even
 *   for interop testing. The `MemoryVideoSource` produces a deterministic
 *   pattern of `VideoFrameBuffer`s (configurable: solid color, color bars,
 *   gradient, frame counter overlay) and is the test/demo "camera" used
 *   by the example apps and the Chrome interop test harness.
 *
 * ## Scope
 *
 *   - One SSRC, one frame format, one fixed cadence (or step-driven).
 *   - Pure C++20, no OS / device dependency.
 *   - No codec integration (the encoder wrapper belongs to `nimrtc::h264`
 *     or the future `vp8` module).  This source produces raw pixels only.
 *
 * ## Production note
 *
 *   In production, replace this with a real `IVideoSource` plugin (camera
 *   capture, screen capture, file).  The interface here is the same shape
 *   the engine will use for the device layer (§2.3, device = pluginized
 *   later).  Keeping the interface simple now means future plugin authors
 *   have a known contract.
 *
 * @note P1. Standalone module; not yet wired into the engine.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nimrtc/core/error.hpp>
#include <nimrtc/video_frame/frame.hpp>

namespace nimrtc::video_source {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/** Test pattern for the source. */
enum class Pattern : std::uint8_t {
    kSolidColor = 0,   ///< Solid fill (configurable color)
    kColorBars  = 1,   ///< SMPTE-like colour bars (good for manual eyeballing)
    kGradient   = 2,   ///< Vertical gradient (R→B)
    kFrameCount = 3,   ///< Animated gradient + frame counter overlay
};

struct SourceConfig {
    std::uint32_t width  = 640;
    std::uint32_t height = 480;

    /** Pixel format emitted (currently I420 supported; others are 0-filled). */
    video_frame::PixelFormat format = video_frame::PixelFormat::kI420;

    /** Target frame rate (frames per second). */
    std::uint32_t fps = 30;

    /** SSRC of the produced RTP stream. */
    std::uint32_t ssrc = 0x12345678;

    /** Initial pattern. */
    Pattern pattern = Pattern::kColorBars;

    /** Solid color (BGRA for kSolidColor) — only Y is meaningful for I420. */
    std::uint8_t solid_y = 128;
    std::uint8_t solid_u = 128;
    std::uint8_t solid_v = 128;

    /** Monotonic starting timestamp (microseconds). */
    std::int64_t  start_capture_ts_us = 0;

    /** Per-SSRC frame sequence number start. */
    std::uint32_t start_frame_seq = 0;
};

// ---------------------------------------------------------------------------
// Callback type
// ---------------------------------------------------------------------------

/** Callback invoked on every produced frame.
 *  - @p frame: refcounted pixel buffer (caller takes ownership of the
 *    local shared_ptr — keeping it alive preserves the buffer).
 *  - @p info:  capture metadata (capture_ts, frame_seq, rtp_timestamp). */
using FrameCallback = std::function<void(video_frame::VideoFrameBuffer frame,
                                         video_frame::VideoFrameInfo info)>;

// ---------------------------------------------------------------------------
// IVideoSource — interface (matches future device plugin shape)
// ---------------------------------------------------------------------------

class IVideoSource {
public:
    virtual ~IVideoSource() = default;

    /** Start emitting frames at the configured rate. */
    virtual core::Result<void> start() noexcept = 0;

    /** Stop emitting frames. Pending callbacks are dropped. */
    virtual void stop() noexcept = 0;

    /** True if currently running. */
    virtual bool running() const noexcept = 0;

    /** Manually produce one frame (skips cadence). Returns the produced
     *  frame + invokes the callback (if any). */
    virtual void produce_one() noexcept = 0;

    /** Get the current source config. */
    virtual SourceConfig config() const noexcept = 0;

    /** Replace the pattern at runtime (caller's responsibility to ensure
     *  this happens at frame boundary). */
    virtual void set_pattern(Pattern p) noexcept = 0;

    /** Set the frame callback. The callback is invoked on whichever thread
     *  calls produce_one() or drives the internal cadence thread. */
    virtual void set_callback(FrameCallback cb) noexcept = 0;

    /** Snapshot of emission statistics. */
    struct Stats {
        std::uint64_t frames_produced = 0;
        std::uint64_t frames_dropped  = 0;
        std::uint64_t ticks           = 0;
    };
    virtual Stats stats() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// MemoryVideoSource — concrete implementation (test/demo only)
// ---------------------------------------------------------------------------

/** Create an in-memory video source.
 *  @param cfg       Source configuration.
 *  @param callback  Frame callback (called on the producing thread).
 *  @return Heap-allocated source. Caller owns. */
std::unique_ptr<IVideoSource> create_memory_source(SourceConfig cfg,
                                                   FrameCallback callback) noexcept;

/** Generate one frame of the requested pattern into the supplied buffer.
 *  Used internally and exposed for tests so they can build "expected"
 *  frames without instantiating a source.
 *  @param cfg     Source config (used for dimensions, pattern, frame_seq).
 *  @param buffer  Destination buffer. Must be at least compute_layout(cfg).
 *  @param info    On success, populated with capture_ts, frame_seq, etc.
 *  @return kOk on success. */
core::Result<void> render_pattern(const SourceConfig& cfg,
                            video_frame::VideoFrameBuffer& buffer,
                            video_frame::VideoFrameInfo& info) noexcept;

/** Pattern name (for logging). */
std::string_view pattern_name(Pattern p) noexcept;

} // namespace nimrtc::video_source
