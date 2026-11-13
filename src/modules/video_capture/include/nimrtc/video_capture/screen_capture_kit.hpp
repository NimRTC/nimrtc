/**
 * @file nimrtc/video_capture/screen_capture_kit.hpp
 * @brief The macOS ScreenCaptureKit capture device (remote-desktop plan A4).
 *
 * ScreenCaptureKit is *the* macOS screen-capture API: it captures a
 * display or a window, composites the pointer into the frame, and
 * delivers the result as an IOSurface-backed `CVPixelBuffer` — the
 * surface a VideoToolbox encoder can consume without a CPU round-trip.
 *
 * ## What it gives the engine that the other backends do not
 *
 *   1. **The pointer is already in the frame.**  The OS composites the
 *      cursor into the captured surface, so the separate cursor layer
 *      (plan A6) is not needed on this path — the backend reports
 *      `cursor_composited = true`.
 *   2. **A single window can be captured**, not just a whole display.
 *   3. **The frame is a GPU surface.**  `CVPixelBufferRef` is the
 *      `GpuBackend::kAppleCFPixelBuffer` handle the plan's A4 names.
 *
 * ## The permission model — the thing that makes macOS different
 *
 * macOS gates screen capture behind the **Screen Recording**
 * permission: the *host application* must be granted it, or every
 * capture fails.  `CGPreflightScreenCaptureAccess()` reports whether
 * the grant is already there, and `CGRequestScreenCaptureAccess()`
 * asks for it (which shows the user a system prompt and, on the first
 * request, makes the host appear in *System Settings → Privacy &
 * Security → Screen Recording*).
 *
 * The backend treats the permission as a **first-class, distinguishable
 * outcome**:
 *
 *   - `enumerate_devices()` never *prompts*.  It preflights and, if the
 *     permission has not been granted, returns an empty list.  A
 *     library that popped a modal dialog during an enumeration would
 *     be unusable in a server process.
 *   - `open()` is where the request belongs: it asks, waits for the
 *     user to answer, and — if the answer is no — returns
 *     `plugins::kErrPermissionDenied`.
 *
 * The engine maps that distinct status onto
 * `core::kVideoCapturePermissionDenied`, so a host can tell "the user
 * said no" from "the device is broken" without parsing a message.
 * This is the plan's A4 acceptance criterion: *"a permission denial is
 * reported through the engine error callback with a distinct code."*
 *
 * ## Threading
 *
 * The device owns a **capture thread** (RD-6).  ScreenCaptureKit's
 * `SCShareableContent` fetch is *asynchronous*, and the stream delivers
 * its sample buffers on a dispatch queue the backend owns — so the
 * capture thread starts the stream, waits for the samples, and the
 * engine's tick thread only ever pops from the shared bounded queue.
 *
 * ## The CPU path and the zero-copy path
 *
 * The *seam* (`plugins::IVideoCapture`) delivers CPU-visible bytes plus
 * a `has_gpu_handle` flag; it has no slot for a `GpuBuffer` yet.  This
 * backend therefore takes the **CPU path**: it copies the
 * `CVPixelBuffer`'s BGRA bytes into the shared queue, exactly like the
 * Windows and Linux backends do today, and reports
 * `zero_copy = false`.
 *
 * The `CVPixelBufferRef` itself is the handle the *hardware* path
 * (B10: capture surface → VideoToolbox encoder without a round-trip)
 * will consume; that path is the encoder's half and lands with it.  The
 * backend keeps the pixel buffer alive for the duration of the frame
 * callback so that the half can be wired without changing the seam.
 *
 * @note Remote-desktop plan A4.
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
 * @brief The macOS ScreenCaptureKit capture device.
 *
 * The class exists on every platform so that a host can refer to it
 * portably; on a non-macOS host `open()` always fails with
 * `kErrUnsupported`, and the factory is simply not registered there.
 *
 * The class is deliberately free of every Apple type: the public header
 * is pure C++20, so a host that only *names* the device (in a
 * cross-platform configuration file) does not need the macOS SDK.
 */
class NIMRTC_API_STABLE ScreenCaptureKitCapture final
    : public plugins::IVideoCapture {
public:
    explicit ScreenCaptureKitCapture(plugins::CaptureConfig cfg) noexcept;
    ~ScreenCaptureKitCapture() override;

    ScreenCaptureKitCapture(const ScreenCaptureKitCapture&)            = delete;
    ScreenCaptureKitCapture& operator=(const ScreenCaptureKitCapture&) = delete;

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override { return "sck-capture"; }

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

    // ---- ScreenCaptureKit-specific ---------------------------------------

    /** True iff this build can talk to ScreenCaptureKit at all.  False
     *  on a non-macOS host (and on a macOS older than 12.3, which
     *  predates the API). */
    [[nodiscard]] static bool available() noexcept;

    /** True iff the *host application* already holds the Screen
     *  Recording permission.
     *
     *  This never prompts: it only *reports*.  A host can call it to
     *  decide whether to offer a "grant screen recording" button
     *  before the user ever tries to capture. */
    [[nodiscard]] static bool has_permission() noexcept;

    /** The captured display's description, for diagnostics. */
    [[nodiscard]] std::string device_description() const noexcept {
        return description_;
    }

private:
    /** The capture thread's body: start the stream, wait, tear down. */
    void capture_loop() noexcept;

    /**
     * @brief The sample-buffer callback.
     *
     * The Apple types are erased to `void*` so that this header stays
     * free of the macOS SDK; the definition casts them back.
     *
     * @param ctx           The device (as a `void*`).
     * @param sample_buffer A `CMSampleBufferRef` (as a `void*`).
     */
    static void dispatch_sample(void* ctx, void* sample_buffer) noexcept;

    /**
     * @brief The C function pointer the Objective-C stream output calls.
     *
     * The `@interface` that implements ScreenCaptureKit's
     * `SCStreamOutput` protocol cannot be declared inside a C++
     * namespace, so it lives at file scope in the `.mm` — and it needs
     * a plain function pointer to call back into the C++ device.
     */
    using SampleHandler = void (*)(void*, void*);

    plugins::CaptureConfig cfg_;
    plugins::CaptureFrameCallback cb_;

    /** The counters.  They are written by the sample-delivery thread
     *  and read by the engine's tick thread, so they are atomic. */
    std::atomic<std::uint64_t> frames_captured_{0};
    std::atomic<std::uint64_t> frames_dropped_{0};
    std::atomic<std::uint64_t> device_errors_{0};
    std::atomic<std::uint64_t> bytes_copied_{0};

    /** The pixel buffer the sample handler fills (the captured region). */
    std::vector<std::uint8_t> scratch_;

    /** The changed-region detector (remote-desktop plan A7).
     *  ScreenCaptureKit reports no damage, so the region is computed by
     *  comparing the frame with the previous one. */
    DirtyRectDetector dirty_detector_{4};

    /** The human-readable description of the captured display. */
    std::string description_;

    /** The ScreenCaptureKit objects and the capture thread. */
    std::unique_ptr<class ScreenCaptureKitState> state_;

    /** The frame sequence counter.  Only the sample-delivery thread
     *  touches it. */
    std::uint32_t frame_index_ = 0;
    bool open_ = false;
};

// ---------------------------------------------------------------------------
// The factory
// ---------------------------------------------------------------------------

/**
 * @brief The ScreenCaptureKit factory.
 *
 * Registers under the id `"sck"`.  On a non-macOS host the factory is
 * never registered (there is no ScreenCaptureKit to talk to), so a host
 * that configures `"sck"` there gets the engine's "capture plugin
 * missing" error rather than a silent no-op.
 */
class NIMRTC_API_STABLE ScreenCaptureKitFactory final
    : public plugins::IVideoCaptureFactory {
public:
    [[nodiscard]] std::string_view id() const noexcept override {
        return "sck";
    }

    [[nodiscard]] std::string_view display_name() const noexcept override {
        return "ScreenCaptureKit (macOS display and window capture)";
    }

    [[nodiscard]] plugins::IVideoCapture* create(
        plugins::CaptureConfig cfg) const override {
        return new ScreenCaptureKitCapture(std::move(cfg));
    }
};

/**
 * @brief Publish the ScreenCaptureKit factory into the plugin registry.
 *
 * A no-op on a non-macOS host.  Called by the module's
 * `register_default_plugins()`; it is separate so that the *platform*
 * file owns the platform's registration and the portable file stays
 * portable.
 */
void register_screen_capture_kit() noexcept;

} // namespace nimrtc::video_capture
