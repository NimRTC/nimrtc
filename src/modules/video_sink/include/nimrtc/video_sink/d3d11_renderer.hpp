/**
 * @file nimrtc/video_sink/d3d11_renderer.hpp
 * @brief The Windows D3D11 / DXGI presentation sink (remote-desktop plan E2).
 *
 * ## What this is
 *
 * A `plugins::IVideoSink` that presents decoded frames through a DXGI
 * swap chain.  It is the Windows leg of the renderer track; the macOS
 * (Metal) and Linux (Wayland / X11) legs are separate modules that
 * implement the same seam.
 *
 * ## The three properties that matter (plan E2)
 *
 * 1. **`Present(0)` + a presentation clock.**  The swap chain is
 *    presented with the *non-blocking* flag, so the present call never
 *    waits for the display's vertical blank.  The pacing is done by the
 *    renderer instead: a presentation clock owns a deadline, and the
 *    render thread sleeps until that deadline.  A spin-wait would peg a
 *    core for the whole session; a deadline wait costs nothing.
 *
 * 2. **The render thread never blocks the engine's tick thread.**  The
 *    sink's `render()` hands the frame to a *presenter thread* the sink
 *    owns and returns immediately.  RD-6 says the engine is a
 *    single-threaded orchestrator and that a device plug-in owns its
 *    own threads — a renderer is exactly such a device plug-in.
 *
 * 3. **Bounded memory under a sustained overrun (plan E8).**  The
 *    hand-off is a *single-slot mailbox* with a **latest-wins** policy:
 *    when a new frame arrives while the previous one has not been
 *    presented yet, the *previous* frame is dropped.  The mailbox can
 *    therefore never hold more than one frame, whatever the producer
 *    does, and the drop is counted in `VideoSinkStats`.
 *
 * ## The offscreen mode
 *
 * A swap chain needs a window, and a CI machine has none.  So the
 * renderer supports an **offscreen** mode: it renders into a plain
 * texture instead of a swap chain and *still* runs the presentation
 * clock.  The frame rate the presenter achieves and the drops it takes
 * are therefore measurable without a display — which is exactly what
 * plan E2's acceptance criterion ("presents N frames and asserts the
 * frame rate matches the source cadence") needs.
 *
 * ## The two delivery paths
 *
 * The sink implements both:
 *
 *   - the **GPU path** (`render(const GpuBuffer&)`): a D3D11 texture is
 *     copied device-side into the present target — no host copy at all;
 *   - the **CPU path** (`render(const VideoSourceFrame&)`): the decoded
 *     pixels are converted to the present format and uploaded.
 *
 * The GPU path only accepts a BGRA8 texture (the format the present
 * target uses); any other format falls back to the CPU path, because
 * converting an NV12 surface would need a video-processing shader that
 * this module does not carry.
 *
 * @note Remote-desktop plan E1 / E2 / E5 / E8.
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
// D3D11Renderer
// ---------------------------------------------------------------------------

/**
 * @brief A DXGI swap-chain presentation sink.
 *
 * Thread safety: `render()` may be called from the engine's tick thread
 * (the CPU path) or from a decode callback (the GPU path).  The sink
 * serialises both internally — the presenter thread is the only one that
 * touches the D3D11 device's present path — so the caller does not have
 * to.
 *
 * The counters are atomic, so `stats()` may be read from any thread.
 */
class NIMRTC_API_STABLE D3D11Renderer final : public plugins::IVideoSink {
public:
    explicit D3D11Renderer(plugins::VideoSinkConfig cfg) noexcept;

    ~D3D11Renderer() override;

    D3D11Renderer(const D3D11Renderer&)            = delete;
    D3D11Renderer& operator=(const D3D11Renderer&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------

    const char*     name() const noexcept override;
    plugins::Status open() noexcept override;
    void            close() noexcept override;

    // ---- plugins::IVideoSink -----------------------------------------------

    /** The CPU path: convert the decoded pixels and hand them to the
     *  presenter.  Returns immediately — it never waits for the display. */
    plugins::Status render(const plugins::VideoSourceFrame& frame) noexcept override;

    /** The GPU path: copy a D3D11 texture into the present target
     *  device-side.  No host copy occurs on this path. */
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
 * @brief The plugin factory for the D3D11 renderer.
 *
 * Registered under the id `"d3d11"`.  On a non-Windows build the
 * factory still exists (so a configuration that names it resolves), but
 * every instance reports `kErrUnsupported` from `open()`.
 */
class NIMRTC_API_STABLE D3D11RendererFactory final
    : public plugins::IVideoSinkFactory {
public:
    std::string_view id()           const noexcept override;
    std::string_view display_name() const noexcept override;
    plugins::IVideoSink* create(plugins::VideoSinkConfig cfg) const override;
};

} // namespace nimrtc::video_sink
