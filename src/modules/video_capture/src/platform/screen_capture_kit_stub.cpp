// modules/video_capture/src/platform/screen_capture_kit_stub.cpp
// ----------------------------------------------------------------------------
// The portable stub of the macOS ScreenCaptureKit capture device
// (remote-desktop plan A4).
//
// The real backend is Objective-C++ (`screen_capture_kit.mm`): the
// ScreenCaptureKit framework is an Objective-C one, and its `SCStream`
// delivers IOSurface-backed `CVPixelBuffer`s.  That translation unit
// can only be compiled where the macOS SDK exists.
//
// This file carries the *same* class on every other platform, so that:
//
//   - a host can name the backend ("sck") in a cross-platform
//     configuration and get a clean "unsupported" rather than "plugin
//     missing",
//   - the class's portable behaviour (the capability query, the
//     lifecycle, the registration) is testable on every CI leg.
//
// The stub reports the *same* static capability flags as the real
// backend: a platform's capabilities are a property of the backend, not
// of the host it runs on, so a query must not answer differently
// depending on where it is asked from.
//
// See include/nimrtc/video_capture/screen_capture_kit.hpp for the contract.
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/screen_capture_kit.hpp>

#include <utility>

#include <nimrtc/core/registry.hpp>

namespace nimrtc::video_capture {

/**
 * @brief The stub's platform state.
 *
 * It must live in *this* namespace (not an anonymous one): the public
 * header forward-declares `ScreenCaptureKitState`, and the
 * `std::unique_ptr` member needs the complete type to match.
 */
class ScreenCaptureKitState {};

// ---------------------------------------------------------------------------
// ScreenCaptureKitCapture
// ---------------------------------------------------------------------------

ScreenCaptureKitCapture::ScreenCaptureKitCapture(
        plugins::CaptureConfig cfg) noexcept
    : cfg_(std::move(cfg)) {}

ScreenCaptureKitCapture::~ScreenCaptureKitCapture() = default;

bool ScreenCaptureKitCapture::available() noexcept {
    return false;
}

bool ScreenCaptureKitCapture::has_permission() noexcept {
    // No ScreenCaptureKit: there is no permission to hold.
    return false;
}

plugins::Status ScreenCaptureKitCapture::open() noexcept {
    // ScreenCaptureKit is a macOS API.  On any other host this device
    // simply does not exist, and saying so is clearer than pretending.
    return plugins::kErrUnsupported;
}

void ScreenCaptureKitCapture::close() noexcept {}

std::vector<plugins::CaptureDevice>
ScreenCaptureKitCapture::enumerate_devices() const noexcept {
    return {};
}

std::vector<plugins::CaptureFormat>
ScreenCaptureKitCapture::supported_formats(const std::string&) const noexcept {
    return {};
}

plugins::CaptureCapabilities
ScreenCaptureKitCapture::capabilities() const noexcept {
    plugins::CaptureCapabilities c;
    // The stub reports the *same* static design flags as the real
    // backend: the platform's capabilities are a property of the
    // backend, not of the host it runs on.  A query must not answer
    // differently depending on where it is asked from.
    c.zero_copy = false;
    c.dirty_rects = true;
    c.cursor_composited = true;
    c.region_selection = true;
    c.enumeration = true;
    c.max_pixels = 0;
    c.backend = "sck-capture";
    return c;
}

void ScreenCaptureKitCapture::capture_loop() noexcept {
    // The stub has no ScreenCaptureKit to drive, so there is no sample
    // to deliver and the loop is empty.  The frame counter is still
    // *touched* here: it is written and read on this configuration, and
    // a private field that no translation unit of the build reads is
    // rejected by Clang's -Wunused-private-field under -Werror.
    //
    // `[[maybe_unused]]` is not an alternative: GCC ignores that
    // attribute on a non-static data member and rejects the build
    // under -Werror=attributes instead.
    frame_index_ += 1;
    (void)frame_index_;
}

void ScreenCaptureKitCapture::dispatch_sample(void*, void*) noexcept {}

plugins::Status ScreenCaptureKitCapture::pop_frame(
        plugins::CaptureFrame&) noexcept {
    return plugins::kErrUnsupported;
}

plugins::CaptureStats ScreenCaptureKitCapture::stats() const noexcept {
    return plugins::CaptureStats{};
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void register_screen_capture_kit() noexcept {
    // A no-op off macOS: there is no ScreenCaptureKit to talk to, and a
    // host that asks for "sck" there gets the engine's "capture plugin
    // missing" error rather than a silent no-op device.
}

} // namespace nimrtc::video_capture
