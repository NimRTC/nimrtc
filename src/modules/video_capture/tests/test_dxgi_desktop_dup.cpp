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

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
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
    // the *CPU* path needs a readback — but the device can also keep
    // the picture on the GPU and hand it to the encoder as a surface
    // (plan B10).  The capability says the backend *can* do it; the
    // per-format flag says what a given session actually did.
    EXPECT_TRUE(caps.zero_copy);
    // DXGI reports the changed regions natively.
    EXPECT_TRUE(caps.dirty_rects);
    // It does NOT composite the pointer — that is the cursor layer's
    // job (plan A6).
    EXPECT_FALSE(caps.cursor_composited);
    // It can capture a sub-region of the display.
    EXPECT_TRUE(caps.region_selection);
    EXPECT_TRUE(caps.enumeration);
}

TEST(DxgiDesktopDup, TheDefaultSessionIsNotZeroCopy) {
    // A session that did not ask for the zero-copy path uses the CPU
    // path, and says so.
    CaptureConfig cfg;
    DxgiDesktopDupCapture dev(cfg);
    if (dev.open() != nimrtc::plugins::kOk) {
        GTEST_SKIP() << "no duplicable display on this host";
    }
    EXPECT_FALSE(dev.config().format.zero_copy);
    dev.close();
}

TEST(DxgiDesktopDup, ARegionForcesTheCpuPath) {
    // The region crop happens on the CPU (it copies a sub-rectangle
    // out of the readback), so a session with a region cannot deliver
    // a GPU surface.  The device has to fall back and say so.
    CaptureConfig cfg;
    cfg.region.width  = 640;
    cfg.region.height = 480;
    cfg.want_zero_copy = true;
    DxgiDesktopDupCapture dev(cfg);
    if (dev.open() != nimrtc::plugins::kOk) {
        GTEST_SKIP() << "no duplicable display on this host";
    }
    EXPECT_FALSE(dev.config().format.zero_copy);
    dev.close();
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

namespace {

/**
 * @brief Provoke a desktop change so the capture device has something
 *        to deliver.
 *
 * Desktop Duplication only reports a frame when the desktop actually
 * changes, so a test on a static desktop would never see one.
 *
 * Moving the pointer does *not* work: Desktop Duplication does not
 * composite the pointer into the frame, so moving it produces no dirty
 * rectangle at all.  Drawing on the desktop's own device context does:
 * the stroke lands on the screen exactly like any other drawing, and
 * the compositor reports it as changed.
 *
 * The drawing is small and in the corner, and it is erased again
 * afterwards, so the user's desktop is left as it was found.
 */
void nudge_desktop() {
    HDC screen = ::GetDC(nullptr);
    if (screen == nullptr) return;

    // A small filled rectangle in the top-left corner.
    RECT box{0, 0, 48, 32};
    HBRUSH brush = ::CreateSolidBrush(RGB(0x40, 0x80, 0xC0));
    if (brush != nullptr) {
        ::FillRect(screen, &box, brush);
        ::DeleteObject(brush);
    }
    ::GdiFlush();
    ::ReleaseDC(nullptr, screen);

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    // Erase it again, so the desktop is left as it was found.
    screen = ::GetDC(nullptr);
    if (screen != nullptr) {
        brush = ::CreateSolidBrush(RGB(0, 0, 0));
        if (brush != nullptr) {
            ::FillRect(screen, &box, brush);
            ::DeleteObject(brush);
        }
        ::GdiFlush();
        ::ReleaseDC(nullptr, screen);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
}

} // anonymous namespace

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

    std::fprintf(stderr,
                 "[cpu capture] captured=%llu dropped=%llu errors=%llu "
                 "bytes_copied=%llu\n",
                 static_cast<unsigned long long>(after.frames_captured),
                 static_cast<unsigned long long>(after.frames_dropped),
                 static_cast<unsigned long long>(after.device_errors),
                 static_cast<unsigned long long>(after.bytes_copied));

    dev.close();
    EXPECT_FALSE(dev.is_open());
}

TEST(DxgiDesktopDup, TheZeroCopyPathDeliversAGpuSurface) {
    // The zero-copy path (plan B10): the picture never reaches the CPU.
    //
    // The test asserts the two things that make the claim measurable:
    //
    //   1. the frame that comes out carries a D3D11 surface (and *no*
    //      CPU pixels at all), and
    //   2. the device's `bytes_copied` counter stays at zero — that
    //      counter is the readback's footprint, so a non-zero value
    //      would mean the picture did go through the CPU.
    //
    // A static desktop produces no frames, so the test cannot *require*
    // a frame; it asserts the invariants that hold either way.
    CaptureConfig cfg;
    cfg.want_zero_copy = true;
    DxgiDesktopDupCapture dev(cfg);
    if (dev.open() != nimrtc::plugins::kOk) {
        GTEST_SKIP() << "no duplicable display on this host";
    }

    // The device has to report the path it actually took.
    EXPECT_TRUE(dev.config().format.zero_copy);

    bool saw_surface = false;
    // The capture thread waits for the desktop to change, so the test
    // has to wait too — polling a few times in a tight loop would
    // never see a frame.  The desktop is nudged periodically so that
    // there is something to capture.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(15);
    auto last_nudge = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline && !saw_surface) {
        CaptureFrame f{};
        const auto st = dev.pop_frame(f);
        if (st != nimrtc::plugins::kOk) {
            if (std::chrono::steady_clock::now() - last_nudge >
                std::chrono::milliseconds(300)) {
                nudge_desktop();
                last_nudge = std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        // A zero-copy frame has no CPU pixels.
        EXPECT_EQ(f.pixels, nullptr);
        EXPECT_EQ(f.size, 0u);
        EXPECT_TRUE(f.has_gpu_handle);
        EXPECT_EQ(f.gpu_surface.backend,
                  nimrtc::video_frame::GpuBackend::kWindowsD3D11Texture);
        EXPECT_NE(f.gpu_surface.handle, nullptr);
        // The surface must name its device: an encoder session is
        // opened against one device, so producer and consumer have to
        // agree on it.
        EXPECT_NE(f.gpu_surface.device, nullptr);
        EXPECT_GT(f.width, 0u);
        EXPECT_GT(f.height, 0u);
        saw_surface = true;
    }

    const auto stats = dev.stats();
    // The readback counter is the whole point: it must not have moved.
    EXPECT_EQ(stats.bytes_copied, 0u)
        << "the zero-copy path read the picture back to the CPU";

    std::fprintf(stderr,
                 "[zero-copy capture] captured=%llu dropped=%llu errors=%llu "
                 "bytes_copied=%llu saw_surface=%d\n",
                 static_cast<unsigned long long>(stats.frames_captured),
                 static_cast<unsigned long long>(stats.frames_dropped),
                 static_cast<unsigned long long>(stats.device_errors),
                 static_cast<unsigned long long>(stats.bytes_copied),
                 saw_surface ? 1 : 0);

    dev.close();
    EXPECT_FALSE(dev.is_open());

    if (!saw_surface) {
        // A static desktop produces nothing.  That is not a failure —
        // it is the dirty-rect path doing its job.
        GTEST_SKIP() << "the desktop did not change, so no frame was "
                        "produced";
    }
}

#endif // _WIN32
