/**
 * @file nimrtc/plugins/video_source.hpp
 * @brief IVideoSource — pluggable raw-frame video source interface.
 *
 * Per ARCHITECTURE.md §2.3 (devices = pluginized later) and the technical
 * document §5 (P1: "内存帧媒体源（demo/互通用）"), NimRTC ships a
 * `MemoryVideoSource` for demo / interop testing. The shipped source is
 * one possible implementation behind this interface; users can register
 * their own source (camera capture, screen capture, file reader, GPU
 * texture, etc.) via:
 *
 * @code
 * class MyCam : public plugins::IVideoSource { ... };
 * class MyCamFactory : public plugins::IVideoSourceFactory { ... };
 * static nimrtc::plugins::SimpleVideoSourceFactory<MyCam>
 *     factory{"my_cam", "My camera"};
 * NIMRTC_REGISTER_VIDEO_SOURCE(my_cam, &factory);
 * @endcode
 *
 * ## Scope of the interface
 *
 *   - One SSRC, one frame format, one fixed cadence (or step-driven).
 *   - Pure C++20 at the interface boundary (no OS / device dependency).
 *   - No codec integration — the encoder wrapper is the consumer of the
 *     produced `VideoSourceFrame` (typically: codec adapter → IVideoCodec).
 *
 * ## Frame ownership
 *
 *   The frame descriptor's `plane_*` pointers are owned by the source and
 *   remain valid only for the duration of the callback. The caller
 *   (encoder adapter, SFU forwarder, etc.) MUST copy the pixels into its
 *   own buffer before the callback returns, OR keep a reference to the
 *   source's internal buffer (some implementations keep a pool). The
 *   `VideoSourceFrame` is intentionally a view, not a refcounted object,
 *   to keep this header free of `<memory>` dependencies.
 *
 * @note P1 (R2). Interface is stable; binary layout TBD P3.
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include "nimrtc/plugins/base.hpp"
// Reuse VideoPixelFormat already defined here to keep one canonical enum.
#include "nimrtc/plugins/video_codec.hpp"

#ifndef NIMRTC_PLUGINS_VIDEO_SOURCE_HPP
#define NIMRTC_PLUGINS_VIDEO_SOURCE_HPP

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string_view>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Test/demo patterns (concrete sources may add custom content via
// `VideoSourcePattern::kCustom` and source-specific configuration)
// ---------------------------------------------------------------------------

/** Source content pattern. Real sources (camera, screen capture) typically
 *  use `kCustom`; demo / interop sources use the named patterns. */
enum class VideoSourcePattern : std::uint8_t {
    kCustom    = 0,   ///< plugin-specific content (real camera, screen, file)
    kSolid     = 1,   ///< Solid colour fill (configurable Y/U/V)
    kColorBars = 2,   ///< SMPTE-like colour bars (good for manual eyeballing)
    kGradient  = 3,   ///< Vertical gradient (R→B)
    kFrameCount = 4,  ///< Animated gradient + frame counter overlay
};

/** Human-readable pattern name (for logging). */
inline std::string_view video_source_pattern_name(VideoSourcePattern p) noexcept {
    switch (p) {
        case VideoSourcePattern::kCustom:     return "custom";
        case VideoSourcePattern::kSolid:      return "solid";
        case VideoSourcePattern::kColorBars:  return "color_bars";
        case VideoSourcePattern::kGradient:   return "gradient";
        case VideoSourcePattern::kFrameCount: return "frame_count";
        default:                              return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/** Per-source configuration. Mirrors the concrete `video_source::SourceConfig`
 *  but uses plugin types (`TimestampUs`, `VideoPixelFormat`). */
struct VideoSourceConfig {
    /** Frame width in pixels. */
    std::uint32_t width  = 640;

    /** Frame height in pixels. */
    std::uint32_t height = 480;

    /** Pixel format emitted (currently I420 / NV12 / BGRA supported;
     *  other formats are 0-filled for non-matching patterns). */
    VideoPixelFormat format = VideoPixelFormat::kI420;

    /** Target frame rate (frames per second). */
    std::uint32_t fps = 30;

    /** RTP SSRC of the produced stream (for RTP framing downstream). */
    std::uint32_t ssrc = 0x12345678;

    /** Initial pattern. Real sources should set this to `kCustom`. */
    VideoSourcePattern pattern = VideoSourcePattern::kColorBars;

    /** Solid colour Y plane value (kSolid, kFrameCount backgrounds). */
    std::uint8_t solid_y = 128;
    /** Solid colour U / Cb plane value. */
    std::uint8_t solid_u = 128;
    /** Solid colour V / Cr plane value. */
    std::uint8_t solid_v = 128;

    /** Monotonic starting timestamp (microseconds). */
    TimestampUs   start_capture_ts_us = 0;

    /** Per-SSRC frame sequence number start. */
    std::uint32_t start_frame_seq = 0;
};

// ---------------------------------------------------------------------------
// Frame descriptor (interface-level — decoupled from video_frame module)
// ---------------------------------------------------------------------------

/** Raw (uncompressed) video frame descriptor. The source owns `plane_y/u/v`
 *  for the duration of the callback; the callback consumer MUST finish
 *  reading before returning OR keep a reference to the source's internal
 *  pixel buffer (sources may pool). */
struct VideoSourceFrame {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    VideoPixelFormat format = VideoPixelFormat::kUnknown;

    /** Stride (bytes per row) for plane 0 (Y / R). Must be >= width. */
    std::int32_t stride_y = 0;
    /** Stride for plane 1 (U / UV). 0 if not applicable (e.g. BGRA). */
    std::int32_t stride_u = 0;
    /** Stride for plane 2 (V). 0 if not applicable. */
    std::int32_t stride_v = 0;

    /** Offset of plane 0 from the start of `plane_y`. */
    std::size_t   plane_offset_y = 0;
    /** Offset of plane 1 from the start of `plane_u`. */
    std::size_t   plane_offset_u = 0;
    /** Offset of plane 2 from the start of `plane_v`. */
    std::size_t   plane_offset_v = 0;

    /** Pointer to plane 0 bytes (Y or BGRA). Owned by the source. */
    const std::uint8_t* plane_y = nullptr;
    /** Pointer to plane 1 bytes (U for I420, UV for NV12). nullptr if N/A. */
    const std::uint8_t* plane_u = nullptr;
    /** Pointer to plane 2 bytes (V for I420). nullptr if N/A. */
    const std::uint8_t* plane_v = nullptr;

    /** Total size of the pixel buffer (in bytes). */
    std::size_t buffer_size = 0;

    // -- Timeline metadata (§8.4) ------------------------------------------

    /** Monotonic-clock capture timestamp (microseconds). */
    TimestampUs   capture_ts_us = 0;

    /** Per-SSRC frame sequence number (strictly increasing). */
    std::uint32_t frame_seq = 0;

    /** RTP timestamp (32-bit, media clock). */
    std::uint32_t rtp_timestamp = 0;
};

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

/** Frame callback: invoked on the producing thread for every emitted frame.
 *  - @p frame: pixel descriptor (see `VideoSourceFrame` for ownership). */
using VideoSourceFrameCallback =
    std::function<void(const VideoSourceFrame& frame)>;

// ---------------------------------------------------------------------------
// IVideoSource
// ---------------------------------------------------------------------------

/**
 * @brief Raw-frame video source (camera / screen / file / test pattern).
 *
 * One `IVideoSource` instance owns one SSRC stream. The engine instantiates
 * one source per outgoing video track and wires the frame callback to the
 * encoder / forwarder.
 *
 * Thread safety: NOT thread-safe — start/stop/set_* methods must be
 * serialized externally. The frame callback fires on whichever thread
 * drives the source (internal cadence thread for kLowLatency, or the
 * caller's thread if `produce_one()` is used).
 */
class IVideoSource : public IPlugin {
public:
    /** Start emitting frames at the configured cadence.
     *  @return kOk on success; error code if already running / config invalid. */
    virtual Status start() noexcept = 0;

    /** Stop emitting frames. Pending callbacks are dropped. Idempotent. */
    virtual void stop() noexcept = 0;

    /** True if currently running. */
    virtual bool running() const noexcept = 0;

    /** Manually produce one frame (skips cadence). The frame callback (if
     *  set) fires synchronously before this method returns. */
    virtual void produce_one() noexcept = 0;

    /** Get the current source config (for diagnostics / logs). */
    virtual VideoSourceConfig config() const noexcept = 0;

    /** Replace the pattern at runtime. Real sources (`kCustom`) ignore this. */
    virtual void set_pattern(VideoSourcePattern p) noexcept = 0;

    /** Set the frame callback. The callback is invoked on whichever thread
     *  calls `produce_one()` or drives the internal cadence. */
    virtual void set_callback(VideoSourceFrameCallback cb) noexcept = 0;

    /** Emission statistics (since start). */
    struct Stats {
        std::uint64_t frames_produced = 0;
        std::uint64_t frames_dropped  = 0;  ///< cadence overruns (real source only)
        std::uint64_t ticks           = 0;  ///< cadence ticks (real source only)
    };
    virtual Stats stats() const noexcept = 0;

    // ---- HW capability flag (R3-Batch) -----------------------------------
    // Default = false (software). HW plugins (V4L2, AVFoundation, NDK
    // Camera2, DirectShow, ...) override to true.
    virtual bool is_hardware_accelerated() const noexcept { return false; }

    // ---- HW backend name (R3-Batch) --------------------------------------
    // Returns a short identifier (e.g. "v4l2", "avfoundation", "android-camera2",
    // "directshow", "mediacodec", "software"). Used for diagnostics +
    // capabilities introspection. Default = "software".
    virtual std::string_view hardware_backend() const noexcept {
        return "software";
    }
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/** Factory for video-source plugin instances. */
class IVideoSourceFactory {
public:
    virtual ~IVideoSourceFactory() = default;

    /** Unique identifier, e.g. "memory", "camera_v4l2", "screen". */
    virtual std::string_view id() const noexcept = 0;

    /** Short human-readable name, e.g. "In-memory test pattern source". */
    virtual std::string_view display_name() const noexcept = 0;

    /** Create a new source instance. The factory does NOT take ownership of
     *  @p cb; the returned source keeps a copy via set_callback() during
     *  open(). @return IVideoSource in kConstructed state — caller must
     *  call `start()` after registering the callback. */
    virtual IVideoSource* create(VideoSourceConfig cfg) const = 0;
};

/** Helper to create a factory for a concrete IVideoSource type. */
template<class T>
class SimpleVideoSourceFactory : public IVideoSourceFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleVideoSourceFactory(std::string_view id,
                                      std::string_view name) noexcept
        : id_(id), name_(name) {}

    std::string_view id()           const noexcept override { return id_; }
    std::string_view display_name() const noexcept override { return name_; }

    IVideoSource* create(VideoSourceConfig cfg) const override {
        return new T(std::move(cfg));
    }
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_VIDEO_SOURCE_HPP
