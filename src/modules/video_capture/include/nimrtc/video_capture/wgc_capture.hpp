/**
 * @file nimrtc/video_capture/wgc_capture.hpp
 * @brief The Windows Graphics Capture (WGC) capture device.
 *
 * WGC is the *second* Windows capture path, and it is the one the plan's
 * A3 row points at for windows:
 *
 *   - a *display* → **DXGI Desktop Duplication** (fast, no consent, but
 *     the pointer is *not* in the frame)
 *   - a *window*  → **WGC** (the pointer *is* composited by the OS, and
 *     a single window can be captured)
 *
 * The two are complementary, and the selection rule is the plan's:
 * "WGC for windows and for cursor fidelity, DXGI for displays when the
 * caller does not need the cursor."
 *
 * ## What WGC gives the engine that DXGI does not
 *
 *  1. **The pointer is already in the frame.**  WGC asks the desktop
 *     compositor to draw the cursor into the captured surface, so the
 *     separate cursor layer (plan A6) is not needed on this path — the
 *     backend reports `cursor_composited = true` and the engine's
 *     capture pump skips the compositor entirely.
 *  2. **A single window can be captured.**  DXGI can only duplicate a
 *     whole display.
 *  3. **It works on a protected window** — a window that renders with
 *     `DisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` is *omitted* rather
 *     than blacked out, which is the behaviour a secure application
 *     expects.
 *
 * ## What it costs
 *
 *  1. **Consent.**  Windows 10 2004 and later let an application
 *     capture a window without a picker, but the *user* must have
 *     granted the capability: on Windows 11 24H2 and later the OS draws
 *     a border around the captured window and the capture is refused
 *     outright if the application has not declared the
 *     `graphicsCapture` capability.  A monitor capture
 *     (`CreateForMonitor`) needs no consent at all.
 *  2. **It is not the fast path.**  WGC delivers a GPU texture, so the
 *     CPU path still needs a readback — the same cost DXGI pays.
 *
 * ## The frames
 *
 * The frame pool is created at the requested size, and the compositor
 * *scales* the source into it.  When the caller asked for a region, the
 * backend therefore reads the region back from the *source* texture and
 * crops it, rather than asking the compositor to scale the region away.
 * The backend therefore keeps the pool at the *source* size and does
 * the region crop itself, so the pixels the consumer sees are the
 * source's pixels and not a resampling of them.
 *
 * ## Threading
 *
 * The device owns a **capture thread**.  It creates the frame pool and
 * the session there and then waits; the frames arrive on a WinRT
 * thread-pool thread (the pool is created *free-threaded*, so no
 * `DispatcherQueue` is needed — see below), and that thread does the
 * GPU readback and pushes into the shared bounded queue.
 *
 * The D3D11 device is created **without**
 * `D3D11_CREATE_DEVICE_SINGLETHREADED`, so it may be used from more
 * than one thread — which it must be, because the readback happens on
 * the frame-arrival thread while the capture thread owns the session.
 *
 * ## The free-threaded frame pool
 *
 * `Direct3D11CaptureFramePool::Create` requires the calling thread to
 * own a `DispatcherQueue`, which would mean the capture thread needs a
 * message pump.  `CreateFreeThreaded` (Windows 10 1809 and later)
 * removes that requirement, and it is the right choice for a *library*:
 * a library must not impose a message loop on its host.  The cost is
 * that the `FrameArrived` event is raised on a thread-pool thread
 * rather than on the capture thread — which the multithreaded D3D11
 * device already accommodates.
 *
 * @note Remote-desktop plan A3.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/video_capture/dirty_rect_diff.hpp>

namespace nimrtc::video_capture {

/**
 * @brief The Windows Graphics Capture device.
 *
 * The class exists on every platform so that a host can refer to it
 * portably; on a non-Windows host `open()` always fails with
 * `kErrUnsupported`, and the factory is simply not registered there.
 */
class NIMRTC_API_STABLE WgcCapture final : public plugins::IVideoCapture {
public:
    explicit WgcCapture(plugins::CaptureConfig cfg) noexcept;
    ~WgcCapture() override;

    WgcCapture(const WgcCapture&)            = delete;
    WgcCapture& operator=(const WgcCapture&) = delete;

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override { return "wgc-capture"; }

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

    /**
     * @brief Install the frame-delivery callback.
     *
     * The callback runs on the capture thread (the WinRT thread-pool
     * thread for WGC), *not* on the caller's thread.  It must not
     * block, and it must not call back into the device.
     *
     * The callback may be installed before or after `open()`: when the
     * device is already running, the callback is handed to the live
     * capture state under its lock, so a frame that arrives
     * concurrently sees either the old callback or the new one — never
     * a half-installed one.
     */
    void set_frame_callback(plugins::CaptureFrameCallback cb) noexcept override;

    [[nodiscard]] plugins::CaptureStats stats() const noexcept override;

    // ---- WGC-specific -----------------------------------------------------

    /** True iff this build can talk to WGC at all.  False on a
     *  non-Windows host. */
    [[nodiscard]] static bool available() noexcept;

    /** The captured item's description, for diagnostics. */
    [[nodiscard]] std::string device_description() const noexcept {
        return description_;
    }

private:
    /** The capture thread's body: create, start, wait, tear down.
     *
     * The state is passed in rather than read from the member: the
     * thread must not depend on the owning device, which may be
     * destroyed while the thread is still running.  It is *static* for
     * the same reason — it touches nothing but the state. */
    static void capture_loop(
        std::shared_ptr<class WgcCaptureState> state) noexcept;

    /** Handle the frame that just arrived (on the WinRT thread).
     *
     * The state is passed in for the same reason as above: the handler
     * may outlive the owning device.  It is *static* for the same
     * reason — it must not touch the device at all. */
    static void on_frame_arrived(
        const std::shared_ptr<class WgcCaptureState>& state) noexcept;

    plugins::CaptureConfig cfg_;

    /** The delivery callback.  Copied into the live capture state
     *  whenever the state exists, so that the capture thread never
     *  touches the owning device. */
    plugins::CaptureFrameCallback cb_;

    /** The human-readable description of the captured item. */
    std::string description_;

    /** The WGC objects and the capture thread.
     *
     * Shared, not unique: the frame-arrival handler runs on a thread
     * the device does not own and may outlive the device itself, so it
     * takes its own reference and keeps the state — and everything it
     * touches — alive for as long as it needs it. */
    std::shared_ptr<class WgcCaptureState> state_;

    bool open_ = false;
};

// ---------------------------------------------------------------------------
// The factory
// ---------------------------------------------------------------------------

/**
 * @brief The Windows Graphics Capture factory.
 *
 * Registers under the id `"wgc"`.  On a non-Windows host the factory is
 * never registered (there is no WGC to talk to), so a host that
 * configures `"wgc"` there gets the engine's "capture plugin missing"
 * error rather than a silent no-op.
 */
class NIMRTC_API_STABLE WgcCaptureFactory final
    : public plugins::IVideoCaptureFactory {
public:
    [[nodiscard]] std::string_view id() const noexcept override {
        return "wgc";
    }

    [[nodiscard]] std::string_view display_name() const noexcept override {
        return "Windows Graphics Capture (window and display capture)";
    }

    [[nodiscard]] plugins::IVideoCapture* create(
        plugins::CaptureConfig cfg) const override {
        return new WgcCapture(std::move(cfg));
    }
};

/**
 * @brief Publish the WGC factory into the plugin registry.
 *
 * A no-op on a non-Windows host.  Called by the module's
 * `register_default_plugins()`; it is separate so that the *platform*
 * file owns the platform's registration and the portable file stays
 * portable.
 */
NIMRTC_API_STABLE void register_wgc_capture() noexcept;

} // namespace nimrtc::video_capture
