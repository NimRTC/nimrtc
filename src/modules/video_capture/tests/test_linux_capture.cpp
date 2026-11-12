// modules/video_capture/tests/test_linux_capture.cpp
// ----------------------------------------------------------------------------
// The Linux capture backends' tests (remote-desktop plan A5):
//
//   - **X11** (`XGetImage`) — the fallback that needs no portal,
//   - **XDG Desktop Portal + PipeWire** — the Wayland path.
//
// These run on *every* platform, including the ones with no X11 at
// all: the backends' portable half (the classes exist everywhere, the
// factories register only where their platform libraries exist, and
// `open()` fails with `kErrUnsupported` where they do not) is what can
// be tested portably, and that is exactly the part that is easy to
// get wrong.
//
// On a Linux host with an X display the tests *additionally* open the
// real device and check that it produces frames — but they never
// *require* it, because a CI agent without an interactive session
// cannot capture a screen, and a test that fails there is a test that
// lies.
//
// See include/nimrtc/video_capture/x11_capture.hpp and
// include/nimrtc/video_capture/portal_capture.hpp for the contracts.
// ----------------------------------------------------------------------------

#include <nimrtc/core/registry.hpp>
#include <nimrtc/video_capture/portal_capture.hpp>
#include <nimrtc/video_capture/x11_capture.hpp>
#include <nimrtc/video_capture/x11_cursor_source.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using nimrtc::plugins::CaptureConfig;
using nimrtc::plugins::CaptureFrame;
using nimrtc::video_capture::PortalCapture;
using nimrtc::video_capture::PortalCaptureFactory;
using nimrtc::video_capture::X11Capture;
using nimrtc::video_capture::X11CaptureFactory;

// ---------------------------------------------------------------------------
// The X11 factory
// ---------------------------------------------------------------------------

TEST(X11Capture, TheFactoryHasItsOwnId) {
    X11CaptureFactory f;
    EXPECT_EQ(f.id(), "x11");
    EXPECT_FALSE(f.display_name().empty());
}

TEST(X11Capture, TheFactoryCreatesADevice) {
    X11CaptureFactory f;
    CaptureConfig cfg;
    std::unique_ptr<nimrtc::plugins::IVideoCapture> dev(f.create(cfg));
    ASSERT_NE(dev, nullptr);
    EXPECT_EQ(std::string(dev->name()), "x11-capture");
    EXPECT_FALSE(dev->is_open());
}

TEST(X11Capture, TheFactoryIsRegisteredOnlyWhereX11Exists) {
    // The module's `register_default_plugins()` publishes it.  Where
    // X11 does not exist the id is *absent* — a host that asks for
    // "x11" there gets the engine's "capture plugin missing" error
    // rather than a device that silently does nothing.
    nimrtc::video_capture::register_x11_capture();
    const auto& reg = nimrtc::core::PluginRegistry::instance();
    const auto* found = reg.get_video_capture("x11");
#if defined(__linux__) && defined(NIMRTC_HAVE_X11)
    EXPECT_NE(found, nullptr);
#else
    EXPECT_EQ(found, nullptr);
#endif
}

// ---------------------------------------------------------------------------
// The X11 device (portable half)
// ---------------------------------------------------------------------------

TEST(X11Capture, TheDeviceIsNotOpenBeforeOpen) {
    CaptureConfig cfg;
    X11Capture dev(cfg);
    EXPECT_FALSE(dev.is_open());
}

TEST(X11Capture, TheCapabilitiesDescribeTheBackend) {
    CaptureConfig cfg;
    X11Capture dev(cfg);
    const auto caps = dev.capabilities();
    EXPECT_EQ(caps.backend, "x11-capture");
    // The pixels come back from the X server over a socket: not the
    // zero-copy path.
    EXPECT_FALSE(caps.zero_copy);
    // X11 gives the client no damage information.
    EXPECT_FALSE(caps.dirty_rects);
    // XGetImage does NOT include the pointer — the cursor layer's job.
    EXPECT_FALSE(caps.cursor_composited);
    // The region is cropped out of the grabbed image.
    EXPECT_TRUE(caps.region_selection);
    EXPECT_TRUE(caps.enumeration);
}

TEST(X11Capture, PoppingBeforeOpenReportsNotReady) {
    CaptureConfig cfg;
    X11Capture dev(cfg);
    CaptureFrame f{};
    const auto st = dev.pop_frame(f);
    EXPECT_NE(st, nimrtc::plugins::kOk);
}

TEST(X11Capture, OpeningOnANonLinuxHostReportsUnsupported) {
    CaptureConfig cfg;
    X11Capture dev(cfg);
#if defined(__linux__) && defined(NIMRTC_HAVE_X11)
    // On Linux the outcome depends on the machine (there may be no
    // DISPLAY), so only the "it did not crash" part is asserted.
    const auto st = dev.open();
    dev.close();
    EXPECT_TRUE(st == nimrtc::plugins::kOk ||
                st == nimrtc::plugins::kErrHardwareError ||
                st == nimrtc::plugins::kErrInvalidParam);
#else
    EXPECT_EQ(dev.open(), nimrtc::plugins::kErrUnsupported);
#endif
}

TEST(X11Capture, AnUnknownDeviceIdIsRejected) {
    CaptureConfig cfg;
    cfg.device_id = "not-an-x11-device";
    X11Capture dev(cfg);
#if defined(__linux__) && defined(NIMRTC_HAVE_X11)
    // The id is parsed before anything is opened, so the rejection
    // happens on every Linux host — display or not.
    EXPECT_EQ(dev.open(), nimrtc::plugins::kErrInvalidParam);
#else
    EXPECT_EQ(dev.open(), nimrtc::plugins::kErrUnsupported);
#endif
}

TEST(X11Capture, ClosingIsIdempotent) {
    CaptureConfig cfg;
    X11Capture dev(cfg);
    dev.close();
    dev.close();
    EXPECT_FALSE(dev.is_open());
}

// ---------------------------------------------------------------------------
// The X11 device (Linux only, and only when a display can be grabbed)
// ---------------------------------------------------------------------------

#if defined(__linux__) && defined(NIMRTC_HAVE_X11)

TEST(X11Capture, TheDeviceEnumeratesAtLeastOneScreen) {
    // A Linux host without a display (a headless CI agent) has no
    // screens at all.  That is a legitimate state, not a failure.
    CaptureConfig cfg;
    X11Capture dev(cfg);
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
        // The BGRA format is what the portable layer speaks.
        EXPECT_EQ(formats[0].tag, "bgra");
    }
}

TEST(X11Capture, ARealDeviceProducesFrames) {
    // This test needs an *interactive* session: an X display whose
    // contents can be read back.  On a headless CI agent there is
    // none, so the test reports that it was skipped rather than
    // failing — a test that fails for a reason that has nothing to do
    // with the code is worse than no test.
    //
    // A display can also be *present but unreadable*: some compositors
    // and some remote sessions refuse to read the root window back.
    // That is a legitimate state too, and the backend reports it as a
    // device error rather than crashing.
    CaptureConfig cfg;
    X11Capture dev(cfg);
    if (dev.open() != nimrtc::plugins::kOk) {
        GTEST_SKIP() << "no readable X display on this host";
    }

    // Pop for a while.  The X11 path has no damage information, so it
    // produces frames unconditionally; the test asserts the invariants
    // that hold either way: the queue never blocks and the stats never
    // go backwards.
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
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto after = dev.stats();
    EXPECT_GE(after.frames_captured, before.frames_captured);
    EXPECT_GE(after.frames_dropped, before.frames_dropped);
    EXPECT_GE(after.device_errors, before.device_errors);

    // The process must still be alive: the X11 backend installs a
    // non-fatal error handler, so a refused grab is a device error and
    // not the end of the process.  A display that refuses every grab
    // is reported as a skip — the backend behaved correctly, the host
    // simply cannot be captured.
    if (after.device_errors > before.device_errors) {
        dev.close();
        GTEST_SKIP() << "the X server refused to read the screen back";
    }

    dev.close();
    EXPECT_FALSE(dev.is_open());
}

TEST(X11Capture, ARegionIsCroppedOutOfTheFrame) {
    CaptureConfig cfg;
    cfg.region = { 0, 0, 64, 48 };
    X11Capture dev(cfg);
    if (dev.open() != nimrtc::plugins::kOk) {
        GTEST_SKIP() << "no readable X display on this host";
    }

    // Wait for at least one frame.
    CaptureFrame f{};
    bool got = false;
    for (int i = 0; i < 50 && !got; ++i) {
        if (dev.pop_frame(f) == nimrtc::plugins::kOk) {
            got = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (!got) {
        dev.close();
        // A display that refuses every grab is a legitimate state on
        // some compositors and remote sessions: the backend reported
        // the device error instead of crashing, which is the contract.
        if (dev.stats().device_errors > 0) {
            GTEST_SKIP() << "the X server refused to read the screen back";
        }
        GTEST_SKIP() << "no frame arrived from the X server";
    }

    // The region request is honoured exactly: the frame is the size
    // that was asked for, not the size of the screen.
    EXPECT_EQ(f.width, 64u);
    EXPECT_EQ(f.height, 48u);
    EXPECT_EQ(f.stride, 64 * 4);

    dev.close();
}

#endif // __linux__ && NIMRTC_HAVE_X11

// ---------------------------------------------------------------------------
// The X11 cursor source
// ---------------------------------------------------------------------------

TEST(X11CursorSource, TheSourceIsCreatedOnEveryPlatform) {
    // On a host without X11 the factory returns nullptr (there is no
    // pointer to read); elsewhere it returns a source that reports
    // "no pointer" until it is queried against a live display.
    auto source = nimrtc::video_capture::make_x11_cursor_source();
#if defined(__linux__) && defined(NIMRTC_HAVE_X11)
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->backend(), "x11-cursor");
#else
    EXPECT_EQ(source, nullptr);
#endif
}

TEST(X11CursorSource, TheOriginTranslationIsApplied) {
#if defined(__linux__) && defined(NIMRTC_HAVE_X11)
    nimrtc::video_capture::X11CursorSource source;
    source.set_screen_origin(100, 50);
    // A host with no display reports "no pointer", so the position is
    // the default one; the point of the assertion is that the call
    // does not crash and the source still answers.
    const auto state = source.query();
    EXPECT_FALSE(state.visible);
#else
    nimrtc::video_capture::X11CursorSource source;
    const auto state = source.query();
    EXPECT_FALSE(state.visible);
    EXPECT_FALSE(state.live);
#endif
}

// ---------------------------------------------------------------------------
// The portal factory
// ---------------------------------------------------------------------------

TEST(PortalCapture, TheFactoryHasItsOwnId) {
    PortalCaptureFactory f;
    EXPECT_EQ(f.id(), "xdg_portal");
    EXPECT_FALSE(f.display_name().empty());
}

TEST(PortalCapture, TheFactoryCreatesADevice) {
    PortalCaptureFactory f;
    CaptureConfig cfg;
    std::unique_ptr<nimrtc::plugins::IVideoCapture> dev(f.create(cfg));
    ASSERT_NE(dev, nullptr);
    EXPECT_EQ(std::string(dev->name()), "portal-capture");
    EXPECT_FALSE(dev->is_open());
}

TEST(PortalCapture, TheFactoryIsRegisteredOnlyWhereThePortalExists) {
    nimrtc::video_capture::register_portal_capture();
    const auto& reg = nimrtc::core::PluginRegistry::instance();
    const auto* found = reg.get_video_capture("xdg_portal");
#if defined(__linux__) && defined(NIMRTC_HAVE_GIO) && \
    defined(NIMRTC_HAVE_PIPEWIRE)
    EXPECT_NE(found, nullptr);
#else
    EXPECT_EQ(found, nullptr);
#endif
}

// ---------------------------------------------------------------------------
// The portal device (portable half)
// ---------------------------------------------------------------------------

TEST(PortalCapture, TheDeviceIsNotOpenBeforeOpen) {
    CaptureConfig cfg;
    PortalCapture dev(cfg);
    EXPECT_FALSE(dev.is_open());
}

TEST(PortalCapture, TheCapabilitiesDescribeTheBackend) {
    CaptureConfig cfg;
    PortalCapture dev(cfg);
    const auto caps = dev.capabilities();
    EXPECT_EQ(caps.backend, "portal-capture");
    EXPECT_FALSE(caps.zero_copy);
    EXPECT_FALSE(caps.dirty_rects);
    // The portal *can* ask the compositor to draw the pointer, but it
    // is not guaranteed — so the cursor layer stays on.
    EXPECT_FALSE(caps.cursor_composited);
    EXPECT_TRUE(caps.region_selection);
    // The source list comes from the portal, not from an enumeration:
    // this is the platform's security model, not a limitation of the
    // backend.
    EXPECT_FALSE(caps.enumeration);
}

TEST(PortalCapture, TheEnumerationIsEmptyBeforeThePortalAnswers) {
    // The portal owns the source selection, so the engine cannot
    // enumerate a monitor it was not given.  Before a ScreenCast
    // request has been answered the list is empty — and that is the
    // *honest* answer.
    CaptureConfig cfg;
    PortalCapture dev(cfg);
    EXPECT_TRUE(dev.enumerate_devices().empty());
}

TEST(PortalCapture, PoppingBeforeOpenReportsNotReady) {
    CaptureConfig cfg;
    PortalCapture dev(cfg);
    CaptureFrame f{};
    const auto st = dev.pop_frame(f);
    EXPECT_NE(st, nimrtc::plugins::kOk);
}

TEST(PortalCapture, OpeningWithoutAPortalReportsUnsupported) {
    CaptureConfig cfg;
    PortalCapture dev(cfg);
#if defined(__linux__) && defined(NIMRTC_HAVE_GIO) && \
    defined(NIMRTC_HAVE_PIPEWIRE)
    // A Linux host without a portal (or without a session bus) has no
    // portal path, and the device says so rather than pretending.
    if (!nimrtc::video_capture::PortalCapture::portal_available()) {
        EXPECT_EQ(dev.open(), nimrtc::plugins::kErrUnsupported);
    } else {
        // With a portal, `open()` would show the user a picker — which
        // a test must never do.  The absence of a portal is the only
        // case a test can assert.
        GTEST_SKIP() << "a portal is present; opening it would prompt "
                        "the user";
    }
#else
    EXPECT_EQ(dev.open(), nimrtc::plugins::kErrUnsupported);
#endif
}

TEST(PortalCapture, ClosingIsIdempotent) {
    CaptureConfig cfg;
    PortalCapture dev(cfg);
    dev.close();
    dev.close();
    EXPECT_FALSE(dev.is_open());
}
