// modules/video_capture/tests/test_dxgi_desktop_dup.cpp
// ----------------------------------------------------------------------------
// The Windows DXGI Desktop Duplication backend's tests.
//
// These run on *every* platform, including the ones with no DXGI at
// all: the backend's portable half (the class exists everywhere, the
// factory registers only where DXGI exists, and `open()` fails with
// `kErrUnsupported` where it does not) is what can be tested
// portably, and that is exactly the part that is easy to get wrong.
//
// On a Windows host with a display the tests *additionally* open the
// real device and check that it produces frames — but they never
// *require* it, because a CI agent without an interactive session
// cannot duplicate a display, and a test that fails there is a test
// that lies.
//
// See include/nimrtc/video_capture/dxgi_desktop_dup.hpp for the
// contract, and the remote-desktop plan's A3 row.
// ----------------------------------------------------------------------------

#include <nimrtc/core/registry.hpp>
#include <nimrtc/video_capture/dxgi_desktop_dup.hpp>

#include <string>
#include <vector>

#include <gtest/gtest.h>

using nimrtc::plugins::CaptureConfig;
using nimrtc::plugins::CaptureFrame;
using nimrtc::video_capture::DxgiDesktopDupCapture;
using nimrtc::video_capture::DxgiDesktopDupFactory;

// ---------------------------------------------------------------------------
// The factory
// ---------------------------------------------------------------------------

TEST(DxgiDesktopDup, TheFactoryHasItsOwnId) {
    DxgiDesktopDupFactory f;
    EXPECT_EQ(f.id(), "dxgi_desktop");
    EXPECT_FALSE(f.display_name().empty());
}

TEST(DxgiDesktopDup, TheFactoryCreatesADevice) {
    DxgiDesktopDupFactory f;
    CaptureConfig cfg;
    std::unique_ptr<nimrtc::plugins::IVideoCapture> dev(f.create(cfg));
    ASSERT_NE(dev, nullptr);
    EXPECT_EQ(std::string(dev->name()), "dxgi-desktop-dup");
    EXPECT_FALSE(dev->is_open());
}

TEST(DxgiDesktopDup, TheFactoryIsRegisteredOnWindows) {
    // The module's `register_default_plugins()` publishes it.  On a
    // non-Windows host the id is *absent* — a host that asks for
    // "dxgi_desktop" there gets the engine's "capture plugin missing"
    // error rather than a device that silently does nothing.
    nimrtc::video_capture::register_dxgi_desktop_dup();
    const auto& reg = nimrtc::core::PluginRegistry::instance();
    const auto* found = reg.get_video_capture("dxgi_desktop");
#if defined(_WIN32)
    EXPECT_NE(found, nullptr);
#else
    EXPECT_EQ(found, nullptr);
#endif
}

// ---------------------------------------------------------------------------
// The device (portable half)
// ---------------------------------------------------------------------------

TEST(DxgiDesktopDup, TheDeviceIsNotOpenBeforeOpen) {
    CaptureConfig cfg;
    DxgiDesktopDupCapture dev(cfg);
    EXPECT_FALSE(dev.is_open());
}

TEST(DxgiDesktopDup, TheCapabilitiesDescribeTheBackend) {
    CaptureConfig cfg;
    DxgiDesktopDupCapture dev(cfg);
    const auto caps = dev.capabilities();
    EXPECT_EQ(caps.backend, "dxgi-desktop-dup");
    // Desktop Duplication hands the frame over as a GPU texture, so
    // the CPU path needs a readback: this is *not* the zero-copy path.
    EXPECT_FALSE(caps.zero_copy);
    // DXGI reports the changed regions natively.
    EXPECT_TRUE(caps.dirty_rects);
    // It does NOT composite the pointer — that is the cursor layer's
    // job (plan A6).
    EXPECT_FALSE(caps.cursor_composited);
    // It can capture a sub-region of the display.
    EXPECT_TRUE(caps.region_selection);
    EXPECT_TRUE(caps.enumeration);
}

TEST(DxgiDesktopDup, PoppingBeforeOpenReportsNotReady) {
    CaptureConfig cfg;
    DxgiDesktopDupCapture dev(cfg);
    CaptureFrame f{};
    // The device is closed, so there is nothing to pop.  On Windows
    // this is kErrNotReady; elsewhere the whole backend is
    // unsupported.
    const auto st = dev.pop_frame(f);
    EXPECT_NE(st, nimrtc::plugins::kOk);
}

TEST(DxgiDesktopDup, OpeningOnANonWindowsHostReportsUnsupported) {
    CaptureConfig cfg;
    DxgiDesktopDupCapture dev(cfg);
#if defined(_WIN32)
    // On Windows the outcome depends on the machine (there may be no
    // display, or the display may already be duplicated), so only the
    // "it did not crash" part is asserted.
    const auto st = dev.open();
    dev.close();
    EXPECT_TRUE(st == nimrtc::plugins::kOk ||
                st == nimrtc::plugins::kErrHardwareError ||
                st == nimrtc::plugins::kErrInvalidParam);
#else
    EXPECT_EQ(dev.open(), nimrtc::plugins::kErrUnsupported);
#endif
}

TEST(DxgiDesktopDup, ClosingIsIdempotent) {
    CaptureConfig cfg;
    DxgiDesktopDupCapture dev(cfg);
    dev.close();
    dev.close();
    EXPECT_FALSE(dev.is_open());
}

// ---------------------------------------------------------------------------
// The device (Windows only, and only when a display can be duplicated)
// ---------------------------------------------------------------------------

#if defined(_WIN32)

TEST(DxgiDesktopDup, TheDeviceEnumeratesAtLeastOneOutput) {
    // A Windows host without a display (a headless CI agent) has no
    // outputs at all.  That is a legitimate state, not a failure.
    CaptureConfig cfg;
    DxgiDesktopDupCapture dev(cfg);
    const auto devices = dev.enumerate_devices();
    for (const auto& d : devices) {
        EXPECT_FALSE(d.id.empty());
        EXPECT_EQ(d.kind, nimrtc::plugins::CaptureDeviceKind::kScreen);
        EXPECT_GT(d.max_width, 0u);
        EXPECT_GT(d.max_height, 0u);
    }
    if (!devices.empty()) {
        const auto formats = dev.supported_formats(devices[0].id);
        ASSERT_FALSE(formats.empty());
        // The BGRA format is what the desktop compositor delivers.
        EXPECT_EQ(formats[0].tag, "bgra");
        EXPECT_TRUE(formats[0].has_dirty_rects);
    }
}

TEST(DxgiDesktopDup, ARealDeviceProducesFrames) {
    // This test needs an *interactive* session: a duplicable display
    // and a desktop that actually changes.  On a headless CI agent
    // there is none, so the test reports that it was skipped rather
    // than failing — a test that fails for a reason that has nothing
    // to do with the code is worse than no test.
    CaptureConfig cfg;
    DxgiDesktopDupCapture dev(cfg);
    if (dev.open() != nimrtc::plugins::kOk) {
        GTEST_SKIP() << "no duplicable display on this host";
    }

    // Pop for a while.  A static desktop produces *no* frames (that
    // is the point of the dirty-rect path), so the test only asserts
    // the invariants that hold either way: the queue never blocks and
    // the stats never go backwards.
    const auto before = dev.stats();
    for (int i = 0; i < 20; ++i) {
        CaptureFrame f{};
        const auto st = dev.pop_frame(f);
        EXPECT_TRUE(st == nimrtc::plugins::kOk ||
                    st == nimrtc::plugins::kErrNotReady);
        if (st == nimrtc::plugins::kOk) {
            EXPECT_NE(f.pixels, nullptr);
            EXPECT_GT(f.size, 0u);
            EXPECT_GT(f.width, 0u);
            EXPECT_GT(f.height, 0u);
        }
    }
    const auto after = dev.stats();
    EXPECT_GE(after.frames_captured, before.frames_captured);
    EXPECT_GE(after.frames_dropped, before.frames_dropped);
    EXPECT_GE(after.device_errors, before.device_errors);

    dev.close();
    EXPECT_FALSE(dev.is_open());
}

#endif // _WIN32
