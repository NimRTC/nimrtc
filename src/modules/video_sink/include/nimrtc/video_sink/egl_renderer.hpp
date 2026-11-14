/**
 * @file nimrtc/video_sink/egl_renderer.hpp
 * @brief The Linux EGL / Wayland / X11 presentation sink
 *        (remote-desktop plan E4).
 *
 * ## What this is
 *
 * A `plugins::IVideoSink` that presents decoded frames on Linux.  It is the
 * Linux leg of the renderer track; the Windows leg is the DXGI swap chain
 * (`d3d11_renderer.hpp`) and the macOS leg is the Metal layer
 * (`metal_renderer.hpp`).  All three share the same **portable** presenter
 * core (`presenter_core.hpp`) — the mailbox, the presentation clock and
 * the drop accounting are *identical* on every platform.
 *
 * ## The three window-system paths
 *
 *   - **Wayland + EGL** (`wl_display_connect`, `eglGetPlatformDisplayEXT`
 *     with `WL_DISPLAY`): the modern path.  A `wl_surface` is created
 *     through the `xdg-shell` protocol when it is available; the EGL
 *     context renders into a `wl_egl_window`.
 *   - **Wayland + `wl_shm`**: the CPU fallback when EGL is unavailable.
 *     The picture is copied into a shared-memory pool and handed to the
 *     compositor as a `wl_buffer`.
 *   - **X11**: the fallback for an X session.  EGL is tried first
 *     (`eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_EXT, ...)`); when it is
 *     unavailable the picture is pushed with `XShmPutImage` (or
 *     `XPutImage`).
 *
 * The **zero-copy** route (`zwp_linux_dmabuf`) is used when the compositor
 * advertises the `linux-dmabuf` extension: the DMA-BUF file descriptor
 * is handed to the compositor and the picture never enters host memory.
 *
 * ## The offscreen mode
 *
 * Exactly as on Windows: with no native window the renderer runs its
 * presentation clock but renders into an offscreen texture, so the frame
 * rate and the drop accounting are measurable without a display.
 *
 * @note Remote-desktop plan E1 / E4 / E5 / E8.
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
// EglRenderer
// ---------------------------------------------------------------------------

/**
 * @brief An EGL / Wayland / X11 presentation sink.
 *
 * Thread safety: `render()` may be called from the engine's tick thread
 * (the CPU path) or from a decode callback (the GPU path).  The sink
 * serialises both internally — the presenter thread is the only one that
 * touches the EGL context — so the caller does not have to.
 *
 * The counters are atomic, so `stats()` may be read from any thread.
 */
class NIMRTC_API_STABLE EglRenderer final : public plugins::IVideoSink {
public:
    explicit EglRenderer(plugins::VideoSinkConfig cfg) noexcept;

    ~EglRenderer() override;

    EglRenderer(const EglRenderer&)            = delete;
    EglRenderer& operator=(const EglRenderer&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------

    const char*     name() const noexcept override;
    plugins::Status open() noexcept override;
    void            close() noexcept override;

    // ---- plugins::IVideoSink -----------------------------------------------

    /** The CPU path: convert the decoded pixels and hand them to the
     *  presenter.  Returns immediately — it never waits for the display. */
    plugins::Status render(const plugins::VideoSourceFrame& frame) noexcept override;

    /** The GPU path: present a GPU surface.  On Linux the zero-copy route
     *  is the DMA-BUF handle (`GpuBackend::kLinuxVaapiDRM`). */
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
 * @brief The plugin factory for the EGL renderer.
 *
 * Registered under the id `"egl"`.  On a non-Linux build the factory still
 * exists (so a configuration that names it resolves), but every instance
 * reports `kErrUnsupported` from `open()`.
 */
class NIMRTC_API_STABLE EglRendererFactory final
    : public plugins::IVideoSinkFactory {
public:
    std::string_view id()           const noexcept override;
    std::string_view display_name() const noexcept override;
    plugins::IVideoSink* create(plugins::VideoSinkConfig cfg) const override;
};

} // namespace nimrtc::video_sink
