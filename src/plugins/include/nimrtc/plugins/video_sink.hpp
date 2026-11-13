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
 * ## The two delivery paths (remote-desktop plan E1)
 *
 * A sink may implement **either** delivery path, or both:
 *
 *   - the **CPU path**: `render(const VideoSourceFrame&)` — the decoded
 *     pixels arrive as host pointers;
 *   - the **GPU path**: `render(const GpuBuffer&)` — the decoded picture
 *     arrives as a native surface handle (D3D11 texture, CVPixelBuffer,
 *     VASurface, ...) and never touches host memory.
 *
 * The GPU path is the *preferred* one on a hardware decode path (RD-4):
 * the decoder hands back a GPU surface and the sink presents it without
 * a readback.  The CPU path remains the universal fallback: it is what a
 * software decoder produces, and what a software renderer consumes.
 *
 * Both `render()` overloads have a default implementation that returns
 * `kErrUnsupported`, so a third-party sink that implements only one side
 * compiles unchanged — the engine probes the sink's capabilities
 * (`supports_gpu_render()` / `supports_cpu_render()`) and routes to the
 * side that exists.
 *
 * @note Overriding one `render()` overload in a derived class hides the
 *       others *for calls made on the derived type* (the usual C++
 *       name-hiding rule).  The engine always calls through
 *       `IVideoSink&`, where the virtual dispatch is unaffected.  A sink
 *       that wants to call the other overload on itself adds
 *       `using IVideoSink::render;` to its class body.
 *
 * ### The GPU path's lifetime contract
 *
 * The `GpuBuffer` handed to `render(const GpuBuffer&)` is **borrowed**:
 * the engine holds a reference on the surface across the call and
 * releases it when `render()` returns.  A sink that needs to keep the
 * surface past the call MUST take its own reference
 * (`GpuBuffer::acquire_ref()`) and release it later
 * (`GpuBuffer::release_ref()`) — the same contract
 * `IVideoCodec::decode_zero_copy()` documents on the decode side.
 *
 * @note P1 (R2). Interface is stable; binary layout TBD P3.
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include <nimrtc/core/api_version.hpp>
#include "nimrtc/plugins/base.hpp"
// Reuse VideoPixelFormat / VideoSourceFrame from the source plugin header
// (so the decoder→sink handoff doesn't need a translation layer).
#include "nimrtc/plugins/video_source.hpp"
// The GPU delivery path hands the sink a `GpuBuffer` — the same
// type the codec seam uses for `encode_zero_copy()` /
// `decode_zero_copy()`.  It is re-exported from `video_codec.hpp`,
// which `video_source.hpp` already includes; the explicit include
// here states the dependency instead of relying on the transitive one.
#include "nimrtc/plugins/video_codec.hpp"

#ifndef NIMRTC_PLUGINS_VIDEO_SINK_HPP
#define NIMRTC_PLUGINS_VIDEO_SINK_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.

#include <cstdint>
#include <cstddef>
#include <string_view>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/** Per-sink configuration. */
struct NIMRTC_API_STABLE VideoSinkConfig {
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

struct NIMRTC_API_STABLE VideoSinkStats {
    std::uint64_t frames_rendered      = 0;
    std::uint64_t frames_dropped       = 0;   ///< back-pressure drops
    std::uint64_t format_conversions   = 0;   ///< I420→BGRA etc.
    std::uint64_t render_errors        = 0;
    std::uint64_t last_render_age_us   = 0;   ///< now - frame.capture_ts_us
};

// ---------------------------------------------------------------------------
// IVideoSink
// ---------------------------------------------------------------------------

/** Frame information published by a sink after a successful render. */
struct NIMRTC_API_STABLE VideoFrameInfo {
    std::int64_t  capture_ts_us = 0;       ///< monotonic capture timestamp
    std::uint32_t frame_seq     = 0;       ///< per-SSRC frame counter
    std::uint32_t rtp_timestamp = 0;       ///< 32-bit media clock
    std::int64_t  render_time_us = 0;      ///< local monotonic at render()
};

/**
 * @brief Decoded-video consumer (renderer / recorder / inference).
 *
 * Thread safety: NOT thread-safe — `render()` must be serialized externally.
 * One `IVideoSink` instance is owned by one receive stream (or shared
 * between streams for a multi-stream composite renderer).
 */
class NIMRTC_API_STABLE IVideoSink : public IPlugin {
public:
    /** Render one decoded frame from **host memory**.
     *
     *  The default implementation returns `kErrUnsupported`: a sink that
     *  only implements the GPU path does not have to stub this one out.
     *  A sink that overrides it MUST finish reading (or copy out) the
     *  pixels before returning — the `plane_*` pointers are borrowed
     *  for the duration of the call.
     *
     *  @param frame  Decoded pixel descriptor (see VideoSourceFrame).
     *  @return kOk on success; error code on failure. */
    virtual Status render(const VideoSourceFrame& frame) noexcept {
        (void)frame;
        return kErrUnsupported;
    }

    /** Render one decoded frame from a **GPU surface**.
     *
     *  The GPU path: the decoded picture never leaves video memory.  The
     *  surface is **borrowed** — the caller holds a reference on it for
     *  the duration of the call and releases it afterwards.  A sink that
     *  wants to keep the surface past the call takes its own reference
     *  (`GpuBuffer::acquire_ref()`).
     *
     *  The default implementation returns `kErrUnsupported`: a sink that
     *  only implements the CPU path does not have to stub this one out.
     *
     *  @param surface  The decoded surface (see GpuBuffer).
     *  @return kOk on success; error code on failure. */
    virtual Status render(const GpuBuffer& surface) noexcept {
        (void)surface;
        return kErrUnsupported;
    }

    /** Render one decoded GPU surface **with its timeline metadata**.
     *
     *  The default implementation forwards to `render(surface)` and
     *  ignores the metadata, so a sink that implements only the
     *  one-argument GPU path keeps working unchanged.  A sink that
     *  needs the capture timestamp (a renderer that paces frames, a
     *  recorder that timestamps its samples) overrides this one.
     *
     *  @param surface  The decoded surface (see GpuBuffer).
     *  @param info     Timeline metadata for this frame.
     *  @return kOk on success; error code on failure. */
    virtual Status render(const GpuBuffer& surface,
                          const VideoFrameInfo& info) noexcept {
        (void)info;
        return render(surface);
    }

    /** True iff this sink can consume a **GPU surface**.
     *
     *  The engine routes the decode output to the GPU path only when
     *  the sink says it can take a surface *and* the decoder says it
     *  can produce one (`IVideoCodec::supports_zero_copy_decode()`).
     *  Default = false (the sink implements the CPU path only).
     *
     *  A sink that overrides `render(const GpuBuffer&)` MUST also
     *  override this to return true — otherwise the engine never
     *  reaches its GPU implementation. */
    virtual bool supports_gpu_render() const noexcept { return false; }

    /** True iff this sink can consume **host-memory** frames.
     *
     *  Default = true: the CPU path is the historical contract and a
     *  sink that does not say otherwise keeps receiving CPU frames.
     *  A GPU-only sink overrides this to return false, which lets the
     *  engine report the incompatibility at `open()` time instead of
     *  failing on the first frame. */
    virtual bool supports_cpu_render() const noexcept { return true; }

    /** Snapshot of sink statistics (frames rendered, drops, errors). */
    virtual VideoSinkStats stats() const noexcept = 0;

    /** Current sink configuration (for diagnostics / logs). */
    virtual VideoSinkConfig config() const noexcept = 0;

    // ---- Most-recently-rendered-frame query (ADR-017, v1.0.0) ----
    // Default = nullptr (sink doesn't track frame metadata).
    // Useful for ASR / agent pipelines that need "what frame is on
    // screen right now?". Returns a pointer to sink-internal state
    // that is valid until the next render() call. Pointer stability
    // is implementation-defined; copies are cheap and ABI-safe.
    virtual const VideoFrameInfo* current_frame_info() const noexcept {
        return nullptr;
    }

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
class NIMRTC_API_STABLE IVideoSinkFactory {
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
class NIMRTC_API_STABLE SimpleVideoSinkFactory : public IVideoSinkFactory {
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
