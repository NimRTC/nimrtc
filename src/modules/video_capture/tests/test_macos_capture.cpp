// modules/video_capture/tests/test_macos_capture.cpp
// ----------------------------------------------------------------------------
// The macOS ScreenCaptureKit backend's tests (remote-desktop plan A4).
//
// These run on *every* platform, including the ones with no macOS at
// all: the backend's portable half (the class exists everywhere, the
// factory registers only where ScreenCaptureKit exists, `open()` fails
// with `kErrUnsupported` where it does not, and the permission probe
// answers without prompting) is what can be tested portably, and that
// is exactly the part that is easy to get wrong.
//
// On a macOS host the tests *additionally* check the permission
// behaviour — but they never *require* the permission to be granted,
// because a CI agent cannot grant it and a test that fails there is a
// test that lies.
//
// See include/nimrtc/video_capture/screen_capture_kit.hpp for the
// contract.
// ----------------------------------------------------------------------------

#include <nimrtc/core/registry.hpp>
#include <nimrtc/video_capture/screen_capture_kit.hpp>

#include <string>
#include <vector>

#include <gtest/gtest.h>

using nimrtc::plugins::CaptureConfig;
using nimrtc::plugins::CaptureFrame;
using nimrtc::video_capture::ScreenCaptureKitCapture;
using nimrtc::video_capture::ScreenCaptureKitFactory;

// ---------------------------------------------------------------------------
// The factory
// ---------------------------------------------------------------------------

TEST(ScreenCaptureKit, TheFactoryHasItsOwnId) {
    ScreenCaptureKitFactory f;
    EXPECT_EQ(f.id(), "sck");
    EXPECT_FALSE(f.display_name().empty());
}

TEST(ScreenCaptureKit, TheFactoryCreatesADevice) {
    ScreenCaptureKitFactory f;
    CaptureConfig cfg;
    std::unique_ptr<nimrtc::plugins::IVideoCapture> dev(f.create(cfg));
    ASSERT_NE(dev, nullptr);
    EXPECT_EQ(std::string(dev->name()), "sck-capture");
    EXPECT_FALSE(dev->is_open());
}

TEST(ScreenCaptureKit, TheFactoryIsRegisteredOnlyWhereScreenCaptureKitExists) {
    // The module's `register_default_plugins()` publishes it.  Off
    // macOS the id is *absent* — a host that asks for "sck" there gets
    // the engine's "capture plugin missing" error rather than a device
    // that silently does nothing.
    nimrtc::video_capture::register_screen_capture_kit();
    const auto& reg = nimrtc::core::PluginRegistry::instance();
    const auto* found = reg.get_video_capture("sck");
#if defined(__APPLE__)
    EXPECT_NE(found, nullptr);
#else
    EXPECT_EQ(found, nullptr);
#endif
}

// ---------------------------------------------------------------------------
// The device (portable half)
// ---------------------------------------------------------------------------

TEST(ScreenCaptureKit, TheDeviceIsNotOpenBeforeOpen) {
    CaptureConfig cfg;
    ScreenCaptureKitCapture dev(cfg);
    EXPECT_FALSE(dev.is_open());
}

TEST(ScreenCaptureKit, TheCapabilitiesDescribeTheBackend) {
    CaptureConfig cfg;
    ScreenCaptureKitCapture dev(cfg);
    const auto caps = dev.capabilities();
    EXPECT_EQ(caps.backend, "sck-capture");
    // The frame arrives as an IOSurface-backed CVPixelBuffer; the seam
    // delivers CPU bytes, so the seam's path is not zero-copy.
    EXPECT_FALSE(caps.zero_copy);
    // The changed region comes from the portable pixel-diff, so the
    // frames do carry the metadata.
    EXPECT_TRUE(caps.dirty_rects);
    // This is the whole point of ScreenCaptureKit: the OS composites
    // the pointer into the frame, so the cursor layer is not needed on
    // this path.
    EXPECT_TRUE(caps.cursor_composited);
    // The backend crops a region out of the readback itself.
    EXPECT_TRUE(caps.region_selection);
    EXPECT_TRUE(caps.enumeration);
}

TEST(ScreenCaptureKit, PoppingBeforeOpenReportsNotReady) {
    CaptureConfig cfg;
    ScreenCaptureKitCapture dev(cfg);
    CaptureFrame f{};
    const auto st = dev.pop_frame(f);
    EXPECT_NE(st, nimrtc::plugins::kOk);
}

TEST(ScreenCaptureKit, TheEnumerationIsEmptyWithoutThePermission) {
    // The enumeration must *never* prompt: a library that showed a
    // modal dialog during an enumeration would be unusable in a server
    // process.  Without the Screen Recording permission the honest
    // answer is an empty list.
    CaptureConfig cfg;
    ScreenCaptureKitCapture dev(cfg);
    if (!ScreenCaptureKitCapture::has_permission()) {
        EXPECT_TRUE(dev.enumerate_devices().empty());
    } else {
        // With the permission the list holds the machine's displays
        // (and windows).  The exact count is the machine's business;
        // what matters is that the ids are well-formed.
        for (const auto& d : dev.enumerate_devices()) {
            EXPECT_FALSE(d.id.empty());
            EXPECT_NE(d.kind, nimrtc::plugins::CaptureDeviceKind::kUnknown);
        }
    }
}

TEST(ScreenCaptureKit, ThePermissionProbeNeverPrompts) {
    // `has_permission()` is a *report*, not a request: it must answer
    // the same way twice in a row, and it must not change the state of
    // the permission.
    const bool first = ScreenCaptureKitCapture::has_permission();
    const bool second = ScreenCaptureKitCapture::has_permission();
    EXPECT_EQ(first, second);
}

TEST(ScreenCaptureKit, OpeningOnANonMacHostReportsUnsupported) {
    CaptureConfig cfg;
    ScreenCaptureKitCapture dev(cfg);
#if defined(__APPLE__)
    // On macOS the outcome depends on the machine (the permission may
    // not be granted, and granting it would prompt the user — which a
    // test must never do), so only the "it did not crash" part is
    // asserted.
    const auto st = dev.open();
    dev.close();
    EXPECT_TRUE(st == nimrtc::plugins::kOk ||
                st == nimrtc::plugins::kErrPermissionDenied ||
                st == nimrtc::plugins::kErrHardwareError ||
                st == nimrtc::plugins::kErrInvalidParam);
#else
    EXPECT_EQ(dev.open(), nimrtc::plugins::kErrUnsupported);
#endif
}

TEST(ScreenCaptureKit, ClosingIsIdempotent) {
    CaptureConfig cfg;
    ScreenCaptureKitCapture dev(cfg);
    dev.close();
    dev.close();
    EXPECT_FALSE(dev.is_open());
}

// ---------------------------------------------------------------------------
// The permission denial is a *distinct* outcome
// ---------------------------------------------------------------------------

TEST(ScreenCaptureKit, ThePermissionDenialHasItsOwnStatus) {
    // The plan's A4 acceptance criterion: "a permission denial is
    // reported through the engine error callback with a distinct
    // code".  The distinct *status* is what the engine maps onto that
    // code, so it must exist and must differ from the generic
    // hardware error.
    EXPECT_NE(nimrtc::plugins::kErrPermissionDenied,
              nimrtc::plugins::kErrHardwareError);
    EXPECT_NE(nimrtc::plugins::kErrPermissionDenied,
              nimrtc::plugins::kErrUnsupported);
    EXPECT_STREQ(nimrtc::plugins::status_string(
                     nimrtc::plugins::kErrPermissionDenied),
                 "permission denied");
}
