/**
 * @file nimrtc/video_sink/metal_renderer.hpp
 * @brief The macOS Metal / AVSampleBufferDisplayLayer presentation sink
 *        (remote-desktop plan E3).
 *
 * ## What this is
 *
 * A `plugins::IVideoSink` that presents decoded frames on macOS.  It is the
 * macOS leg of the renderer track; the Windows leg is the DXGI swap chain
 * (`d3d11_renderer.hpp`) and the Linux leg is the EGL / Wayland / X11
 * surface (`egl_renderer.hpp`).  All three implement the same seam and
 * share the same **portable** presenter core
 * (`presenter_core.hpp`) — the mailbox, the presentation clock and the
 * drop accounting are *identical* on every platform.
 *
 * ## The two delivery paths
 *
 *   - the **GPU path** (`render(const GpuBuffer&)`): a Metal texture
 *     (`GpuBackend::kAppleMTLTexture`) or a `CVPixelBuffer`
 *     (`GpuBackend::kAppleCFPixelBuffer`) is drawn into the layer's
 *     drawable without a host copy;
 *   - the **CPU path** (`render(const VideoSourceFrame&)`): the decoded
 *     pixels are converted to BGRA and handed to the presenter.
 *
 * ## The two presentation surfaces
 *
 *   - **Metal** (`CAMetalLayer`): the GPU path.  A command buffer renders
 *     the source texture into the layer's drawable and is committed with
 *     the *non-blocking* policy — the pacing is the presentation clock,
 *     not the drawable's availability.
 *   - **AVSampleBufferDisplayLayer**: the CPU path.  It takes a
 *     `CMSampleBuffer`, so the presenter wraps the BGRA picture in one
 *     and enqueues it.  The layer does its own timing, but the *drop*
 *     policy is still ours.
 *
 * ## The offscreen mode
 *
 * Exactly as on Windows: with no native window the renderer runs its
 * presentation clock but renders into an offscreen Metal texture, so the
 * frame rate and the drop accounting are measurable without a display.
 *
 * @note Remote-desktop plan E1 / E3 / E5 / E8.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <memory>
#include <string_view>

#include <nimrtc/core/error.hpp>
#include <nimrtc/plugins/video_sink.hpp>

namespace nimrtc::video_sink {

// ---------------------------------------------------------------------------
// MetalRenderer
// ---------------------------------------------------------------------------

/**
 * @brief A Metal / AVSampleBufferDisplayLayer presentation sink.
 *
 * Thread safety: `render()` may be called from the engine's tick thread
 * (the CPU path) or from a decode callback (the GPU path).  The sink
 * serialises both internally — the presenter thread is the only one that
 * touches the Metal / AVFoundation objects — so the caller does not have
 * to.
 *
 * The counters are atomic, so `stats()` may be read from any thread.
 */
class NIMRTC_API_STABLE MetalRenderer final : public plugins::IVideoSink {
public:
    explicit MetalRenderer(plugins::VideoSinkConfig cfg) noexcept;

    ~MetalRenderer() override;

    MetalRenderer(const MetalRenderer&)            = delete;
    MetalRenderer& operator=(const MetalRenderer&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------

    const char*     name() const noexcept override;
    plugins::Status open() noexcept override;
    void            close() noexcept override;

    // ---- plugins::IVideoSink -----------------------------------------------

    /** The CPU path: convert the decoded pixels and hand them to the
     *  presenter.  Returns immediately — it never waits for the display. */
    plugins::Status render(const plugins::VideoSourceFrame& frame) noexcept override;

    /** The GPU path: draw a Metal texture / CVPixelBuffer into the
     *  layer's drawable.  No host copy occurs on this path. */
    plugins::Status render(const plugins::GpuBuffer& surface) noexcept override;

    /** The GPU path with timeline metadata. */
    plugins::Status render(const plugins::GpuBuffer& surface,
                            const plugins::VideoFrameInfo& info) noexcept override;

    /** True — this sink consumes a GPU surface. */
    bool supports_gpu_render() const noexcept override;

    /** True — the CPU path is implemented as the universal fallback. */
    bool supports_cpu_render() const noexcept override;

    plugins::VideoSinkStats stats() const noexcept override;
    plugins::VideoSinkConfig config() const noexcept override;
    plugins::Status set_config(plugins::VideoSinkConfig cfg) noexcept override;

    const plugins::VideoFrameInfo* current_frame_info() const noexcept override;

    bool is_hardware_accelerated() const noexcept override;

    std::string_view hardware_backend() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/**
 * @brief The plugin factory for the Metal renderer.
 *
 * Registered under the id `"metal"`.  On a non-Apple build the factory
 * still exists (so a configuration that names it resolves), but every
 * instance reports `kErrUnsupported` from `open()`.
 */
class NIMRTC_API_STABLE MetalRendererFactory final
    : public plugins::IVideoSinkFactory {
public:
    std::string_view id()           const noexcept override;
    std::string_view display_name() const noexcept override;
    plugins::IVideoSink* create(plugins::VideoSinkConfig cfg) const override;
};

} // namespace nimrtc::video_sink
