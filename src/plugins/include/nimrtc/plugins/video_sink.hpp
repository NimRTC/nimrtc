/**
 * @file nimrtc/plugins/video_sink.hpp
 * @brief IVideoSink — pluggable decoded-video sink interface.
 *
 * The receive-side end of a video pipeline needs somewhere to deliver
 * decoded `VideoSourceFrame`s (camera/peer → decoder → display path).
 * Common replacement points:
 *
 *   - Local renderer (SDL, OpenGL, DirectX, Metal, Vulkan, Android
 *     Surface, iOS Metal)
 *   - Headless recorder (file writer, network sink for a downstream SFU)
 *   - AI inference worker (consumes raw frames without rendering)
 *   - Test harness (frame verifier, e.g. loop-back pixel comparator)
 *
 * All of these share the same interface — accept a decoded frame, run
 * one operation, return.
 *
 * ## Replacing the sink
 *
 * @code
 * class MySdl : public plugins::IVideoSink { ... };
 * static nimrtc::plugins::SimpleVideoSinkFactory<MySdl>
 *     factory{"sdl", "SDL2 renderer"};
 * NIMRTC_REGISTER_VIDEO_SINK(sdl, &factory);
 * @endcode
 *
 * ## Ownership
 *
 * The frame descriptor's `plane_*` pointers are owned by the codec /
 * decoder and remain valid only for the duration of the `render()` call.
 * The sink MUST finish reading (or copy out) before returning.
 *
 * @note P1 (R2). Interface is stable; binary layout TBD P3.
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include "nimrtc/plugins/base.hpp"
// Reuse VideoPixelFormat / VideoSourceFrame from the source plugin header
// (so the decoder→sink handoff doesn't need a translation layer).
#include "nimrtc/plugins/video_source.hpp"

#ifndef NIMRTC_PLUGINS_VIDEO_SINK_HPP
#define NIMRTC_PLUGINS_VIDEO_SINK_HPP

#include <cstdint>
#include <cstddef>
#include <string_view>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/** Per-sink configuration. */
struct VideoSinkConfig {
    /** Target render width. 0 = use frame's native width. */
    std::uint32_t width  = 0;

    /** Target render height. 0 = use frame's native height. */
    std::uint32_t height = 0;

    /** Whether the sink is allowed to convert pixel formats (e.g. I420 →
     *  BGRA for display).  False = pass through the frame's native format;
     *  conversion failures then produce kErrUnsupported. */
    bool allow_format_conversion = true;

    /** Maximum frames per second the sink will accept (back-pressure
     *  hint — implementations may drop frames above this rate).
     *  0 = unlimited. */
    std::uint32_t max_fps = 0;

    /** Human-readable sink identifier (e.g. "preview_window",
     *  "headless_recorder").  Used for logging. */
    std::string_view label;
};

// ---------------------------------------------------------------------------
// Sink statistics
// ---------------------------------------------------------------------------

struct VideoSinkStats {
    std::uint64_t frames_rendered      = 0;
    std::uint64_t frames_dropped       = 0;   ///< back-pressure drops
    std::uint64_t format_conversions   = 0;   ///< I420→BGRA etc.
    std::uint64_t render_errors        = 0;
    std::uint64_t last_render_age_us   = 0;   ///< now - frame.capture_ts_us
};

// ---------------------------------------------------------------------------
// IVideoSink
// ---------------------------------------------------------------------------

/**
 * @brief Decoded-video consumer (renderer / recorder / inference).
 *
 * Thread safety: NOT thread-safe — `render()` must be serialized externally.
 * One `IVideoSink` instance is owned by one receive stream (or shared
 * between streams for a multi-stream composite renderer).
 */
class IVideoSink : public IPlugin {
public:
    /** Render one decoded frame synchronously. Must return before the next
     *  frame can be delivered. Return kOk on success.
     *  @param frame  Decoded pixel descriptor (see VideoSourceFrame). */
    virtual Status render(const VideoSourceFrame& frame) noexcept = 0;

    /** Snapshot of sink statistics (frames rendered, drops, errors). */
    virtual VideoSinkStats stats() const noexcept = 0;

    /** Current sink configuration (for diagnostics / logs). */
    virtual VideoSinkConfig config() const noexcept = 0;

    // ---- HW capability flag (R3-Batch) -----------------------------------
    // Default = false (software). HW renderer plugins (SDL+Vulkan,
    // DirectX, Metal, VideoToolbox output surface, NDK MediaCodec
    // output surface, ...) override to true.
    virtual bool is_hardware_accelerated() const noexcept { return false; }

    // ---- HW backend name (R3-Batch) --------------------------------------
    // Returns a short identifier (e.g. "sdl-vulkan", "metal", "directx",
    // "android-surface", "opengl", "software"). Used for diagnostics +
    // capabilities introspection. Default = "software".
    virtual std::string_view hardware_backend() const noexcept {
        return "software";
    }
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/** Factory for video-sink plugin instances. */
class IVideoSinkFactory {
public:
    virtual ~IVideoSinkFactory() = default;

    /** Unique identifier, e.g. "headless", "sdl", "vulkan", "inference". */
    virtual std::string_view id() const noexcept = 0;

    /** Short human-readable name, e.g. "Headless null sink (drops frames)". */
    virtual std::string_view display_name() const noexcept = 0;

    /** Create a new sink instance with the given configuration.
     *  The returned IVideoSink is in kConstructed state; caller must
     *  call `open()` before render(). */
    virtual IVideoSink* create(VideoSinkConfig cfg) const = 0;
};

template<class T>
class SimpleVideoSinkFactory : public IVideoSinkFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleVideoSinkFactory(std::string_view id,
                                    std::string_view name) noexcept
        : id_(id), name_(name) {}

    std::string_view id()           const noexcept override { return id_; }
    std::string_view display_name() const noexcept override { return name_; }

    IVideoSink* create(VideoSinkConfig cfg) const override {
        return new T(std::move(cfg));
    }
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_VIDEO_SINK_HPP
