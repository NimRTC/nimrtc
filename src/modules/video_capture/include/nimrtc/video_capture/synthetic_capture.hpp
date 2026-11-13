/**
 * @file nimrtc/video_capture/synthetic_capture.hpp
 * @brief A synthetic capture device -the portable layer's test backend.
 *
 * This is the device that lets the *entire* portable capture layer be
 * exercised on every CI leg, on every platform, with no display, no
 * camera and no permission prompt.  It is the mechanism the platform
 * backends are written against: a Windows DXGI adapter, a macOS
 * ScreenCaptureKit adapter and a Linux PipeWire adapter are all thin
 * wrappers that produce the same thing this produces.
 *
 * ## What it produces
 *
 * A moving gradient whose brightness advances with the frame number, in
 * a plain 8-bit BGRA buffer (the format every desktop compositor
 * produces natively, so a platform backend's conversion path is the
 * same one this exercises).  The frame is deliberately *not* static:
 * the dirty-rect machinery has to see change, and a test that asserts
 * "a static desktop produces an empty dirty rect" needs a way to make
 * the desktop static on demand -hence `set_static_frame()`.
 *
 * ## The dirty-rect model
 *
 * The synthetic device tracks a *moving highlight box* whose position
 * advances with the frame number, and reports the box's rectangle as the
 * dirty region.  That gives the tests a dirty rect they can assert on
 * exactly, rather than a fuzzy pixel-diff:
 *
 *   - `set_static_frame(true)` freezes the box, and the device then
 *     reports an **empty** dirty rect (nothing changed) -the property a
 *     remote-desktop encoder cares about most.
 *   - `set_static_frame(false)` resumes motion, and the dirty rect is
 *     the box's old and new positions.
 *
 * @note Remote-desktop plan A2b. The portable layer's reference device.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/video_capture/capture_queue.hpp>
#include <nimrtc/video_capture/capture_source_adapter.hpp>

namespace nimrtc::video_capture {

// ---------------------------------------------------------------------------
// SyntheticCapture
// ---------------------------------------------------------------------------

/**
 * @brief A capture device that synthesises its frames.
 *
 * Implements `plugins::IVideoCapture`.  The frames are produced on the
 * caller's thread by `produce_one()` -a synthetic device has no
 * separate capture thread, which is exactly what makes it
 * deterministic for a test.
 */
class NIMRTC_API_STABLE SyntheticCapture final : public plugins::IVideoCapture {
public:
    /** Construct a synthetic device with the given configuration. */
    explicit SyntheticCapture(plugins::CaptureConfig cfg) noexcept;

    ~SyntheticCapture() override;

    SyntheticCapture(const SyntheticCapture&)            = delete;
    SyntheticCapture& operator=(const SyntheticCapture&) = delete;

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override { return "synthetic-capture"; }

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

    [[nodiscard]] plugins::CaptureStats stats() const noexcept override {
        return stats_;
    }

    // ---- Synthetic-specific controls -------------------------------------

    /**
     * Produce one frame into the queue.
     *
     * The frame callback (if set) fires synchronously, before this
     * method returns -so a test that uses the callback sees the frame
     * at a deterministic point.
     *
     * @return kOk when a frame was produced, kErrNotReady when the
     *         device is not open.
     */
    plugins::Status produce_one() noexcept;

    /**
     * Freeze the picture.  While frozen, the device reports an **empty**
     * dirty rect (nothing changed) and the pixel content stays the
     * same -the property a remote-desktop encoder relies on.
     */
    void set_static_frame(bool frozen) noexcept { static_ = frozen; }

    /** True iff the picture is frozen. */
    [[nodiscard]] bool is_static_frame() const noexcept { return static_; }

    /** The number of frames the device will produce before reporting
     *  exhaustion. 0 = unlimited (the default). Used by tests to drive
     *  the "device lost" path. */
    void set_frame_budget(std::uint64_t n) noexcept { budget_ = n; }

    /** The queue depth the device uses. */
    void set_queue_depth(std::uint32_t depth) noexcept {
        queue_.set_depth(depth);
    }

    /** The drop policy the queue uses. */
    void set_drop_policy(plugins::CaptureDropPolicy p) noexcept {
        queue_.set_drop_policy(p);
    }

private:
    void render_frame(std::uint8_t* dst, std::size_t size,
                      std::uint32_t frame_index) const noexcept;

    plugins::CaptureConfig cfg_;
    CaptureQueue queue_{2};
    plugins::CaptureFrameCallback cb_;
    plugins::CaptureStats stats_{};
    std::vector<std::uint8_t> scratch_;
    std::uint32_t frame_index_ = 0;
    std::uint64_t budget_      = 0;
    bool open_    = false;
    bool static_  = false;
};

// ---------------------------------------------------------------------------
// SyntheticCaptureFactory
// ---------------------------------------------------------------------------

/** Factory for the synthetic capture device.  Registers under the id
 *  `"synthetic"`. */
class NIMRTC_API_STABLE SyntheticCaptureFactory final
    : public plugins::IVideoCaptureFactory {
public:
    [[nodiscard]] std::string_view id() const noexcept override {
        return "synthetic";
    }

    [[nodiscard]] std::string_view display_name() const noexcept override {
        return "Synthetic capture device (CI-testable)";
    }

    [[nodiscard]] plugins::IVideoCapture* create(
        plugins::CaptureConfig cfg) const override {
        return new SyntheticCapture(std::move(cfg));
    }

    [[nodiscard]] plugins::IVideoSource* create_source(
        plugins::IVideoCapture* device,
        plugins::VideoSourceConfig cfg) const override {
        return make_capture_source(device, std::move(cfg));
    }
};

/** Register the synthetic capture device with core::PluginRegistry.
 *  Must be called once at program startup before any capture lookup.
 *  Idempotent. */
NIMRTC_API_STABLE void register_default_plugins() noexcept;

} // namespace nimrtc::video_capture
