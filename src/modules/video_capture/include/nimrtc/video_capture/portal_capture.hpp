/**
 * @file nimrtc/video_capture/portal_capture.hpp
 * @brief The XDG Desktop Portal + PipeWire capture device (remote-desktop
 *        plan A5).
 *
 * On Wayland, a process may not read the screen: the compositor owns
 * the framebuffer and only hands out what the **portal** decides to
 * share.  The portal's `ScreenCast` interface asks the *user* to pick
 * a source (a monitor, a window, a region) and then hands back a
 * **PipeWire node id** that the client connects a PipeWire stream to.
 *
 * So this backend is two halves:
 *
 *   1. **The portal half** (D-Bus, `org.freedesktop.portal.Desktop`):
 *      asks the user, gets a node id back.  This is the part that
 *      *interacts with the user*, and it is the part that makes the
 *      Wayland path different in kind from the X11 path.
 *   2. **The PipeWire half**: connects a capture stream to that node
 *      and pulls the frames out of it.
 *
 * ## The enumeration is a portal limitation, not an engine bug
 *
 * The plan's A5 row says it explicitly: *"Wayland's security model
 * means the portal owns source selection: the seam exposes 'the
 * platform asks the user' rather than 'the engine enumerates
 * monitors'."*
 *
 * So `enumerate_devices()` on this backend returns **the devices the
 * portal has already granted** — which, before a `ScreenCast` request
 * has been answered, is an empty list.  The engine cannot enumerate a
 * monitor it was not given, and pretending otherwise would be a lie.
 *
 * ## The pixels
 *
 * PipeWire delivers the frames in the format the *source* negotiated,
 * which is normally **BGRA** (`SPA_VIDEO_FORMAT_BGRA`) but can be NV12
 * or I420 depending on the compositor and the portal's settings.  The
 * backend converts what arrives into the portable layer's BGRA.
 *
 * ## Threading
 *
 * The device owns a **capture thread** running a PipeWire main loop
 * (RD-6).  The frames arrive on that loop's data thread; the engine's
 * tick thread only pops from the shared bounded queue.
 *
 * ## The consent is a *value*, not a crash
 *
 * The plan's A4 row says "screen-recording permission is requested;
 * denial is a clean error, not a crash", and the same rule applies
 * here.  A user who dismisses the portal dialog, or a session with no
 * portal at all, produces `kErrNotReady` / `kErrUnsupported` — never
 * a crash and never a hang.
 *
 * @note Remote-desktop plan A5.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/video_capture/capture_source_adapter.hpp>
#include <nimrtc/video_capture/dirty_rect_diff.hpp>

namespace nimrtc::video_capture {

/**
 * @brief The XDG Desktop Portal + PipeWire capture device.
 *
 * The class exists on every platform so that a host can refer to it
 * portably; on a non-Linux host `open()` always fails with
 * `kErrUnsupported`, and the factory is simply not registered there.
 */
class NIMRTC_API_STABLE PortalCapture final : public plugins::IVideoCapture {
public:
    explicit PortalCapture(plugins::CaptureConfig cfg) noexcept;
    ~PortalCapture() override;

    PortalCapture(const PortalCapture&)            = delete;
    PortalCapture& operator=(const PortalCapture&) = delete;

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override { return "portal-capture"; }

    plugins::Status open() noexcept override;
    void close() noexcept override;

    // ---- IVideoCapture ----------------------------------------------------
    [[nodiscard]] std::vector<plugins::CaptureDevice>
    enumerate_devices() const noexcept override;

    [[nodiscard]] std::vector<plugins::CaptureFormat>
    supported_formats(const std::string& device_id) const noexcept override;

    [[nodiscard]] plugins::CaptureCapabilities
    capabilities() const noexcept override;

    [[nodiscard]] bool is_open() const noexcept override { return open_; }

    [[nodiscard]] plugins::CaptureConfig config() const noexcept override {
        return cfg_;
    }

    plugins::Status pop_frame(plugins::CaptureFrame& out) noexcept override;

    void set_frame_callback(plugins::CaptureFrameCallback cb) noexcept override {
        cb_ = std::move(cb);
    }

    [[nodiscard]] plugins::CaptureStats stats() const noexcept override;

    // ---- Portal-specific ---------------------------------------------------

    /** True iff this build can talk to the portal at all.  False on a
     *  non-Linux host (and on a Linux host without PipeWire / GIO). */
    [[nodiscard]] static bool available() noexcept;

    /** The captured source's description, for diagnostics. */
    [[nodiscard]] std::string device_description() const noexcept {
        return description_;
    }

    /**
     * @brief True iff a portal is reachable on this host.
     *
     * This is a *runtime* question (is the session bus there, is the
     * portal service registered?), and it is the question the host
     * should ask before it decides between the portal path and the X11
     * fallback.
     */
    [[nodiscard]] static bool portal_available() noexcept;

private:
    /** The capture thread's body: run the PipeWire loop, feed the
     * queue. */
    void capture_loop() noexcept;

    /**
     * @brief Handle the format the compositor settled on.
     *
     * PipeWire reports the negotiated pixel layout through the
     * stream's `param_changed` event — a buffer carries only the
     * memory type, never the layout — so this is where the backend
     * learns what it is looking at.
     *
     * Declared with plain types so that the public header stays free
     * of the PipeWire headers.
     *
     * @param id     The parameter id (`SPA_PARAM_Format`).
     * @param param  The parameter, as an opaque pointer.
     */
    void on_format_changed(std::uint32_t id, const void* param) noexcept;

    /**
     * @brief Drain the PipeWire stream and push what came out of it.
     *
     * @param owner  The `PortalCapture` the stream belongs to, as an
     *               opaque pointer.  The header cannot name the type
     *               without pulling in the PipeWire headers, so it
     *               stays `void*`.
     */
    void on_pipewire_process(void* owner) noexcept;

    /**
     * @brief Handle one frame that came out of the PipeWire stream.
     *
     * Declared with plain types so that the public header stays free
     * of the PipeWire and GIO headers.
     *
     * @param data     The frame's pixels.
     * @param stride   The frame's row stride in bytes.
     * @param width    The frame's width in pixels.
     * @param height   The frame's height in pixels.
     * @param bgra     True iff the pixels are already in BGRA order;
     *                 false means they are RGB (the two packed
     *                 layouts a compositor produces).
     */
    void on_frame(const std::uint8_t* data,
                  std::size_t stride,
                  std::uint32_t width,
                  std::uint32_t height,
                  bool bgra) noexcept;

    plugins::CaptureConfig cfg_;
    plugins::CaptureFrameCallback cb_;

    /** The counters.  They are written by the capture thread and read
     *  by the engine's tick thread, so they are atomic. */
    std::atomic<std::uint64_t> frames_captured_{0};
    std::atomic<std::uint64_t> frames_dropped_{0};
    std::atomic<std::uint64_t> device_errors_{0};
    std::atomic<std::uint64_t> bytes_copied_{0};

    /** The pixel buffer the capture thread fills (the whole surface). */
    std::vector<std::uint8_t> scratch_;

    /** The cropped buffer, used when a sub-region was requested. */
    std::vector<std::uint8_t> region_scratch_;

    /** The changed-region detector (remote-desktop plan A7).  PipeWire
     *  reports no damage, so the region is computed by comparing the
     *  frame with the previous one. */
    DirtyRectDetector dirty_detector_{4};

    /** The human-readable description of the captured source. */
    std::string description_;

    /** The portal / PipeWire objects and the capture thread. */
    std::unique_ptr<class PortalCaptureState> state_;

    /** The frame sequence counter.  Only the capture thread touches
     *  it. */
    std::uint32_t frame_index_ = 0;
    bool open_ = false;
};

// ---------------------------------------------------------------------------
// The factory
// ---------------------------------------------------------------------------

/**
 * @brief The XDG Desktop Portal + PipeWire factory.
 *
 * Registers under the id `"xdg_portal"`.  On a non-Linux host (or a
 * Linux build without PipeWire / GIO) the factory is never registered,
 * so a host that configures `"xdg_portal"` there gets the engine's
 * "capture plugin missing" error rather than a silent no-op.
 */
class NIMRTC_API_STABLE PortalCaptureFactory final
    : public plugins::IVideoCaptureFactory {
public:
    [[nodiscard]] std::string_view id() const noexcept override {
        return "xdg_portal";
    }

    [[nodiscard]] std::string_view display_name() const noexcept override {
        return "XDG Desktop Portal + PipeWire (Wayland screen capture)";
    }

    [[nodiscard]] plugins::IVideoCapture* create(
        plugins::CaptureConfig cfg) const override {
        return new PortalCapture(std::move(cfg));
    }

    [[nodiscard]] plugins::IVideoSource* create_source(
        plugins::IVideoCapture* device,
        plugins::VideoSourceConfig cfg) const override {
        return make_capture_source(device, std::move(cfg));
    }
};

/**
 * @brief Publish the portal factory into the plugin registry.
 *
 * A no-op on a non-Linux host (and on a Linux host without PipeWire /
 * GIO).  Called by the module's `register_default_plugins()`; it is
 * separate so that the *platform* file owns the platform's registration
 * and the portable file stays portable.
 */
NIMRTC_API_STABLE void register_portal_capture() noexcept;

} // namespace nimrtc::video_capture
