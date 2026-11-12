// modules/video_capture/tests/test_wgc_capture.cpp
// ----------------------------------------------------------------------------
// The Windows Graphics Capture backend's tests.
//
// These run on *every* platform, including the ones with no WGC at all:
// the backend's portable half (the class exists everywhere, the factory
// registers only where WGC exists, and `open()` fails with
// `kErrUnsupported` where it does not) is what can be tested
// portably, and that is exactly the part that is easy to get wrong.
//
// On a Windows host with a display the tests *additionally* open the
// real device and check that it produces frames — but they never
// *require* it, because a CI agent without an interactive session
// cannot capture a display, and a test that fails there is a test
// that lies.
//
// See include/nimrtc/video_capture/wgc_capture.hpp for the contract,
// and the remote-desktop plan's A3 row.
// ----------------------------------------------------------------------------

#include <nimrtc/core/registry.hpp>
#include <nimrtc/video_capture/wgc_capture.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using nimrtc::plugins::CaptureConfig;
using nimrtc::plugins::CaptureFrame;
using nimrtc::video_capture::WgcCapture;
using nimrtc::video_capture::WgcCaptureFactory;

// ---------------------------------------------------------------------------
// The factory
// ---------------------------------------------------------------------------

TEST(WgcCapture, TheFactoryHasItsOwnId) {
    WgcCaptureFactory f;
    EXPECT_EQ(f.id(), "wgc");
    EXPECT_FALSE(f.display_name().empty());
}

TEST(WgcCapture, TheFactoryCreatesADevice) {
    WgcCaptureFactory f;
    CaptureConfig cfg;
    std::unique_ptr<nimrtc::plugins::IVideoCapture> dev(f.create(cfg));
    ASSERT_NE(dev, nullptr);
    EXPECT_EQ(std::string(dev->name()), "wgc-capture");
    EXPECT_FALSE(dev->is_open());
}

TEST(WgcCapture, TheFactoryIsRegisteredOnWindows) {
    // The module's `register_default_plugins()` publishes it.  On a
    // non-Windows host the id is *absent* — a host that asks for "wgc"
    // there gets the engine's "capture plugin missing" error rather
    // than a device that silently does nothing.
    nimrtc::video_capture::register_wgc_capture();
    const auto& reg = nimrtc::core::PluginRegistry::instance();
    const auto* found = reg.get_video_capture("wgc");
#if defined(_WIN32)
    EXPECT_NE(found, nullptr);
#else
    EXPECT_EQ(found, nullptr);
#endif
}

// ---------------------------------------------------------------------------
// The device (portable half)
// ---------------------------------------------------------------------------

TEST(WgcCapture, TheDeviceIsNotOpenBeforeOpen) {
    CaptureConfig cfg;
    WgcCapture dev(cfg);
    EXPECT_FALSE(dev.is_open());
}

TEST(WgcCapture, TheCapabilitiesDescribeTheBackend) {
    CaptureConfig cfg;
    WgcCapture dev(cfg);
    const auto caps = dev.capabilities();
    EXPECT_EQ(caps.backend, "wgc-capture");
    // The frame arrives as a GPU texture, so the CPU path needs a
    // readback: this is *not* the zero-copy path.
    EXPECT_FALSE(caps.zero_copy);
    // WGC does not report changed regions.
    EXPECT_FALSE(caps.dirty_rects);
    // This is the whole point of WGC: the OS composites the pointer
    // into the frame, so the cursor layer is not needed on this path.
    EXPECT_TRUE(caps.cursor_composited);
    // The backend crops a region out of the readback itself.
    EXPECT_TRUE(caps.region_selection);
    EXPECT_TRUE(caps.enumeration);
}

TEST(WgcCapture, PoppingBeforeOpenReportsNotReady) {
    CaptureConfig cfg;
    WgcCapture dev(cfg);
    CaptureFrame f{};
    // The device is closed, so there is nothing to pop.  On Windows
    // this is kErrNotReady; elsewhere the whole backend is
    // unsupported.
    const auto st = dev.pop_frame(f);
    EXPECT_NE(st, nimrtc::plugins::kOk);
}

TEST(WgcCapture, OpeningOnANonWindowsHostReportsUnsupported) {
    CaptureConfig cfg;
    WgcCapture dev(cfg);
#if defined(_WIN32)
    // On Windows the outcome depends on the machine (there may be no
    // display, or the capture capability may not have been granted),
    // so only the "it did not crash" part is asserted.
    const auto st = dev.open();
    dev.close();
    EXPECT_TRUE(st == nimrtc::plugins::kOk ||
                st == nimrtc::plugins::kErrHardwareError ||
                st == nimrtc::plugins::kErrInvalidParam);
#else
    EXPECT_EQ(dev.open(), nimrtc::plugins::kErrUnsupported);
#endif
}

TEST(WgcCapture, ClosingIsIdempotent) {
    CaptureConfig cfg;
    WgcCapture dev(cfg);
    dev.close();
    dev.close();
    EXPECT_FALSE(dev.is_open());
}

TEST(WgcCapture, AnUnknownDeviceIdIsRejected) {
    CaptureConfig cfg;
    cfg.device_id = "wgc:monitor:not-a-number";
    WgcCapture dev(cfg);
#if defined(_WIN32)
    EXPECT_EQ(dev.open(), nimrtc::plugins::kErrInvalidParam);
#else
    EXPECT_EQ(dev.open(), nimrtc::plugins::kErrUnsupported);
#endif
}

// ---------------------------------------------------------------------------
// The device (Windows only, and only when a display can be captured)
// ---------------------------------------------------------------------------

#if defined(_WIN32)

TEST(WgcCapture, TheDeviceEnumeratesDisplaysAndWindows) {
    // A Windows host without a display (a headless CI agent) has no
    // displays at all.  That is a legitimate state, not a failure.
    CaptureConfig cfg;
    WgcCapture dev(cfg);
    const auto devices = dev.enumerate_devices();
    for (const auto& d : devices) {
        EXPECT_FALSE(d.id.empty());
        EXPECT_FALSE(d.name.empty());
        EXPECT_TRUE(d.kind == nimrtc::plugins::CaptureDeviceKind::kScreen ||
                    d.kind == nimrtc::plugins::CaptureDeviceKind::kWindow);
        EXPECT_GT(d.max_width, 0u);
        EXPECT_GT(d.max_height, 0u);
        // Every id is prefixed with the backend's namespace, so a host
        // cannot confuse a WGC device with a DXGI one.
        EXPECT_EQ(d.id.rfind("wgc:", 0), 0u);
    }
    if (!devices.empty()) {
        const auto formats = dev.supported_formats(devices[0].id);
        ASSERT_FALSE(formats.empty());
        // The BGRA format is what the compositor delivers.
        EXPECT_EQ(formats[0].tag, "bgra");
        // The compositor hands over a whole surface every time.
        EXPECT_FALSE(formats[0].has_dirty_rects);
    }
}

TEST(WgcCapture, ARealDeviceProducesFrames) {
    // This test needs an *interactive* session: a capturable display
    // and a desktop that actually changes.  On a headless CI agent
    // there is none, so the test reports that it was skipped rather
    // than failing — a test that fails for a reason that has nothing
    // to do with the code is worse than no test.
    CaptureConfig cfg;
    WgcCapture dev(cfg);
    if (dev.open() != nimrtc::plugins::kOk) {
        GTEST_SKIP() << "no capturable display on this host";
    }

    // Pop for a while.  The compositor delivers a frame when the
    // captured content *changes*, so the loop waits between pops
    // rather than spinning: a spin would drain the queue long before
    // the first frame arrived.
    const auto before = dev.stats();
    bool saw_frame = false;
    for (int i = 0; i < 60 && !saw_frame; ++i) {
        CaptureFrame f{};
        const auto st = dev.pop_frame(f);
        EXPECT_TRUE(st == nimrtc::plugins::kOk ||
                    st == nimrtc::plugins::kErrNotReady);
        if (st == nimrtc::plugins::kOk) {
            saw_frame = true;
            EXPECT_NE(f.pixels, nullptr);
            EXPECT_GT(f.size, 0u);
            EXPECT_GT(f.width, 0u);
            EXPECT_GT(f.height, 0u);
            // WGC does not report changed regions, so the whole frame
            // is the changed region.
            EXPECT_TRUE(f.dirty.full_frame);

            // The frame must not be black.  A black frame means the
            // readback silently failed, and a black frame that looks
            // like a valid one is the worst possible failure for a
            // remote desktop.
            bool non_black = false;
            for (std::size_t p = 0; p + 3 < f.size; p += 4) {
                if (f.pixels[p] != 0 || f.pixels[p + 1] != 0 ||
                    f.pixels[p + 2] != 0) {
                    non_black = true;
                    break;
                }
            }
            EXPECT_TRUE(non_black) << "the captured frame was entirely black";
        }
        if (!saw_frame) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto after = dev.stats();
    EXPECT_GE(after.frames_captured, before.frames_captured);
    EXPECT_GE(after.frames_dropped, before.frames_dropped);
    EXPECT_GE(after.device_errors, before.device_errors);

    dev.close();
    EXPECT_FALSE(dev.is_open());

    if (!saw_frame) {
        GTEST_SKIP() << "the captured display produced no frame";
    }

    // The readback happened, so the byte counter moved.
    EXPECT_GT(after.frames_captured, 0u);
    EXPECT_GT(after.bytes_copied, 0u);
}

TEST(WgcCapture, ARegionIsCroppedOutOfTheFrame) {
    // The region is in *source* pixels, and the backend crops it out
    // of the readback itself — the consumer must never see a frame
    // whose size is the whole surface when a region was asked for.
    CaptureConfig cfg;
    cfg.region.x = 10;
    cfg.region.y = 20;
    cfg.region.width = 320;
    cfg.region.height = 240;
    WgcCapture dev(cfg);
    if (dev.open() != nimrtc::plugins::kOk) {
        GTEST_SKIP() << "no capturable display on this host";
    }

    bool saw_frame = false;
    for (int i = 0; i < 60 && !saw_frame; ++i) {
        CaptureFrame f{};
        if (dev.pop_frame(f) == nimrtc::plugins::kOk) {
            saw_frame = true;
            EXPECT_EQ(f.width, 320u);
            EXPECT_EQ(f.height, 240u);
            EXPECT_EQ(f.stride, 320 * 4);
        }
        if (!saw_frame) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!saw_frame) {
        // A static desktop may deliver no frame at all.  That is a
        // legitimate state, not a failure — but it is not a *pass*
        // either, so the test says so instead of quietly succeeding.
        GTEST_SKIP() << "the captured display produced no frame to crop";
    }

    dev.close();
}

#endif // _WIN32
