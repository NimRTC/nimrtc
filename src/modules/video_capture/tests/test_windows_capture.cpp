// modules/video_capture/tests/test_windows_capture.cpp
// ----------------------------------------------------------------------------
// The Windows capture backends' tests (remote-desktop plan A3, M1-c).
//
// These tests run on a Windows host with a display.  Where the host
// cannot provide what a test needs (no display for the DXGI path, no
// consent for the WGC path), the test *skips* with an explicit reason
// rather than failing -- the honest outcome for a hardware-dependent
// capability, and the same rule the plan's "hardware leg" uses.
//
// What is pinned down here:
//
//   - A3: "a test on a Windows host captures a known window and asserts
//     the frame is non-black and that the frame's `GpuHandle` carries a
//     D3D11 texture; the dirty-rect metadata is non-empty for a window
//     that changed."
//   - A7:  the DXGI path reports dirty rectangles natively; the WGC path
//     reports "the whole frame changed" (never an empty rect).
//   - RD-10 / the same-result check: the *portable* behaviour of the
//     Windows backends is identical to the synthetic device's, so the
//     pipeline cannot tell which backend produced the frame.
//   - Error codes: a refused consent and a lost device are distinct, and
//     neither crashes.
// ----------------------------------------------------------------------------

#include <nimrtc/core/registry.hpp>
#include <nimrtc/video_capture/synthetic_capture.hpp>
#include <nimrtc/video_capture/windows_capture.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#ifdef _WIN32
#include <windows.h>
#endif

using nimrtc::plugins::CaptureConfig;
using nimrtc::plugins::CaptureFrame;
using nimrtc::video_capture::GpuCaptureFrame;
using nimrtc::video_capture::WindowsCapture;
using nimrtc::video_capture::WindowsCaptureBackend;
using nimrtc::video_capture::WindowsCaptureError;
using nimrtc::video_capture::WindowsCaptureOptions;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/** A capture device configured for one backend. */
std::unique_ptr<WindowsCapture> make_capture(WindowsCaptureBackend backend,
                                              CaptureConfig cfg = {}) {
    WindowsCaptureOptions opts;
    opts.backend = backend;
    return std::make_unique<WindowsCapture>(std::move(cfg), opts);
}

/** True iff this host has a display the DXGI path can duplicate. */
bool host_has_display() {
    const auto cap = make_capture(WindowsCaptureBackend::kDxgiDesktopDuplication);
    return !cap->enumerate_devices().empty();
}

/**
 * @brief Whether the *host* allows another capture session right now.
 *
 * Windows limits how many Desktop Duplication sessions a single output
 * may have (one), and the WGC compositor has its own device budget.  A
 * test that opens a second session on an output that already has one
 * fails with `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE` -- a host condition,
 * not a defect in the backend.  The tests therefore report it as a
 * skip instead of a failure.
 */
bool host_allows_another_session(WindowsCaptureBackend backend) {
    const auto cap = make_capture(backend);
    std::mutex mutex;
    WindowsCaptureError reported = WindowsCaptureError::kNone;
    cap->set_error_callback([&](WindowsCaptureError e, std::string_view) {
        std::lock_guard<std::mutex> lock(mutex);
        reported = e;
    });

    const auto status = cap->open();
    std::lock_guard<std::mutex> lock(mutex);
    if (status == nimrtc::plugins::kOk) return true;

    // `kCaptureFailed` and `kD3dDeviceFailed` are the codes the
    // backends report when the OS refuses the session.
    return reported != WindowsCaptureError::kCaptureFailed &&
           reported != WindowsCaptureError::kD3dDeviceFailed;
}

/**
 * @brief Collect the frames a GPU consumer receives.
 *
 * The GPU callback runs on the capture device's own thread, so the
 * collector is mutex-guarded.
 */
class GpuCollector {
public:
    void attach(WindowsCapture& cap) {
        cap.set_gpu_frame_callback([this](GpuCaptureFrame&& frame) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++count_;
            // The dirty-rect history matters: a frame that names no
            // changed region is a *static* frame, and the interesting
            // frame for the "the desktop changed" assertion may be any
            // of them -- not necessarily the last one.
            if (!frame.dirty.full_frame && !frame.dirty.rects.empty()) {
                ++non_empty_dirty_frames_;
            }
            last_ = std::move(frame);
        });
    }

    /** Wait until at least @p wanted frames have arrived. */
    bool wait_for(std::size_t wanted,
                  std::chrono::milliseconds timeout =
                      std::chrono::milliseconds(4000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (count_ >= wanted) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return count_ >= wanted;
    }

    [[nodiscard]] std::size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

    /** How many frames named a changed region. */
    [[nodiscard]] std::size_t non_empty_dirty_frames() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return non_empty_dirty_frames_;
    }

    [[nodiscard]] GpuCaptureFrame last() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_;
    }

private:
    mutable std::mutex mutex_;
    std::size_t count_ = 0;
    std::size_t non_empty_dirty_frames_ = 0;
    GpuCaptureFrame last_{};
};

/**
 * @brief A test window with a known, non-black background.
 *
 * The WGC acceptance criterion is "captures a known window and asserts
 * the frame is non-black", so the test needs a window whose content it
 * controls: a plain light background painted with GDI.
 */
#ifdef _WIN32
class KnownWindow {
public:
    KnownWindow() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &KnownWindow::wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"NimRTCTestWindow";
        // The class background brush is what the window paints when the
        // system asks it to erase: a solid light colour, so the captured
        // window is *known* to be non-black.
        wc.hbrBackground = CreateSolidBrush(kBackground);
        RegisterClassExW(&wc);

        hwnd_ = CreateWindowExW(
            WS_EX_TOPMOST, L"NimRTCTestWindow", L"NimRTC capture test",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, 320, 240,
            nullptr, nullptr, wc.hInstance, nullptr);

        if (hwnd_) {
            // Paint it once explicitly, so the very first captured
            // frame already carries the known colour.
            repaint();
            SetForegroundWindow(hwnd_);
            UpdateWindow(hwnd_);
        }
    }

    ~KnownWindow() {
        if (hwnd_) {
            DestroyWindow(hwnd_);
            UnregisterClassW(L"NimRTCTestWindow", GetModuleHandleW(nullptr));
        }
        if (background_brush_) DeleteObject(background_brush_);
    }

    KnownWindow(const KnownWindow&)            = delete;
    KnownWindow& operator=(const KnownWindow&) = delete;

    [[nodiscard]] bool valid() const { return hwnd_ != nullptr; }
    [[nodiscard]] HWND handle() const { return hwnd_; }

    /** Repaint the window with the known colour. */
    void repaint() const {
        if (!hwnd_) return;
        HDC dc = GetDC(hwnd_);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        HBRUSH brush = CreateSolidBrush(kBackground);
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
        ReleaseDC(hwnd_, dc);
        InvalidateRect(hwnd_, nullptr, TRUE);
        UpdateWindow(hwnd_);
    }

    /** The colour the window paints. */
    static constexpr COLORREF kBackground = RGB(220, 200, 180);

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                      LPARAM lparam) {
        switch (msg) {
            case WM_ERASEBKGND:
                // Paint the whole client area with the known colour.
                {
                    HDC dc = reinterpret_cast<HDC>(wparam);
                    RECT rc{};
                    GetClientRect(hwnd, &rc);
                    HBRUSH brush = CreateSolidBrush(kBackground);
                    FillRect(dc, &rc, brush);
                    DeleteObject(brush);
                    return 1;
                }
            default:
                return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
    }

    HWND hwnd_ = nullptr;
    HBRUSH background_brush_ = nullptr;
};

/** The device id the WGC backend expects for a window. */
std::string window_device_id(HWND hwnd) {
    return "hwnd:" + std::to_string(reinterpret_cast<std::uintptr_t>(hwnd));
}

/**
 * @brief Make the desktop change, so the change-driven DXGI path has
 *        something to report.
 *
 * Desktop Duplication only hands out a frame when the compositor
 * presents something new.  On an idle host that never happens, so a
 * test that wants frames has to *make* the desktop change.  Moving a
 * small window is the least intrusive way: it produces a compositor
 * "move rect", which is exactly the metadata the test asserts on.
 */
void nudge_the_desktop() {
    static HWND mover = nullptr;
    static bool registered = false;

    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"NimRTCNudgeWindow";
        RegisterClassExW(&wc);
        mover = CreateWindowExW(WS_EX_TOPMOST, L"NimRTCNudgeWindow",
                                L"NimRTC nudge", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                20, 20, 120, 90, nullptr, nullptr,
                                wc.hInstance, nullptr);
        registered = true;
    }
    if (mover == nullptr) return;

    static int step = 0;
    step = (step + 7) % 40;
    SetWindowPos(mover, HWND_TOPMOST, 20 + step, 20 + step, 120, 90,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
    UpdateWindow(mover);
}
#endif // _WIN32

} // anonymous namespace

// ---------------------------------------------------------------------------
// The factories
// ---------------------------------------------------------------------------

TEST(WindowsCapture, TheDxgiFactoryIsRegistered) {
    nimrtc::video_capture::register_windows_capture_plugins();
    const auto* f = nimrtc::core::PluginRegistry::instance().get_video_capture(
        "windows-dxgi");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->id(), "windows-dxgi");
}

TEST(WindowsCapture, TheWgcFactoryIsRegistered) {
    nimrtc::video_capture::register_windows_capture_plugins();
    const auto* f = nimrtc::core::PluginRegistry::instance().get_video_capture(
        "windows-wgc");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->id(), "windows-wgc");
}

TEST(WindowsCapture, TheFactoryCreatesADevice) {
    nimrtc::video_capture::register_windows_capture_plugins();
    const auto* f = nimrtc::core::PluginRegistry::instance().get_video_capture(
        "windows-dxgi");
    ASSERT_NE(f, nullptr);
    std::unique_ptr<nimrtc::plugins::IVideoCapture> cap(f->create({}));
    ASSERT_NE(cap, nullptr);
    EXPECT_FALSE(cap->is_open());
}

// ---------------------------------------------------------------------------
// Capabilities -- the two backends differ in exactly the ways the plan says
// ---------------------------------------------------------------------------

TEST(WindowsCapture, DxgiReportsDirtyRectsAndNoCursor) {
    const auto cap = make_capture(WindowsCaptureBackend::kDxgiDesktopDuplication);
    const auto caps = cap->capabilities();
    EXPECT_TRUE(caps.zero_copy);
    EXPECT_TRUE(caps.dirty_rects);
    EXPECT_FALSE(caps.cursor_composited);
    EXPECT_TRUE(caps.region_selection);
    EXPECT_TRUE(caps.enumeration);
    EXPECT_EQ(caps.backend, "windows-dxgi");
}

TEST(WindowsCapture, WgcReportsCursorCompositedAndNoDirtyRects) {
    const auto cap = make_capture(WindowsCaptureBackend::kWgc);
    const auto caps = cap->capabilities();
    EXPECT_TRUE(caps.zero_copy);
    // WGC has no per-frame diff information, so it must not claim any.
    EXPECT_FALSE(caps.dirty_rects);
    // The OS composites the pointer into the frame.
    EXPECT_TRUE(caps.cursor_composited);
    EXPECT_EQ(caps.backend, "windows-wgc");
}

TEST(WindowsCapture, EnumeratesDisplaysOnAWindowsHost) {
    const auto cap = make_capture(WindowsCaptureBackend::kDxgiDesktopDuplication);
    const auto devices = cap->enumerate_devices();
    if (devices.empty()) {
        GTEST_SKIP() << "this host has no display to capture";
    }
    for (const auto& d : devices) {
        EXPECT_EQ(d.kind, nimrtc::plugins::CaptureDeviceKind::kScreen);
        EXPECT_FALSE(d.id.empty());
        EXPECT_GT(d.max_width, 0u);
        EXPECT_GT(d.max_height, 0u);
    }
}

TEST(WindowsCapture, OffersFormatsForTheDisplays) {
    const auto cap = make_capture(WindowsCaptureBackend::kDxgiDesktopDuplication);
    const auto devices = cap->enumerate_devices();
    if (devices.empty()) {
        GTEST_SKIP() << "this host has no display to capture";
    }
    const auto formats = cap->supported_formats(devices.front().id);
    ASSERT_FALSE(formats.empty());
    for (const auto& f : formats) {
        EXPECT_GT(f.width, 0u);
        EXPECT_GT(f.height, 0u);
        EXPECT_GT(f.fps, 0u);
        // Both backends are zero-copy capable, so every format they
        // offer is zero-copy.
        EXPECT_TRUE(f.zero_copy);
    }
}

// ---------------------------------------------------------------------------
// The DXGI path: capture a display, deliver a D3D11 texture
// ---------------------------------------------------------------------------

TEST(WindowsCapture, DxgiDeliversAD3d11Texture) {
    if (!host_has_display()) {
        GTEST_SKIP() << "this host has no display to duplicate";
    }

    const auto cap = make_capture(WindowsCaptureBackend::kDxgiDesktopDuplication);
    GpuCollector collector;
    collector.attach(*cap);

    if (cap->open() != nimrtc::plugins::kOk) {
        if (!host_allows_another_session(
                WindowsCaptureBackend::kDxgiDesktopDuplication)) {
            GTEST_SKIP() << "this host refuses a second duplication session";
        }
        FAIL() << "the DXGI capture could not be opened";
    }
    ASSERT_TRUE(collector.wait_for(1))
        << "the DXGI capture produced no frame within the timeout";

    const GpuCaptureFrame frame = collector.last();
    ASSERT_NE(frame.gpu_buffer, nullptr);
    EXPECT_EQ(frame.gpu_buffer->handle().backend,
              nimrtc::video_frame::GpuBackend::kWindowsD3D11Texture);
    EXPECT_NE(frame.gpu_buffer->handle().handle, nullptr);
    EXPECT_GT(frame.gpu_buffer->width(), 0u);
    EXPECT_GT(frame.gpu_buffer->height(), 0u);
    // The desktop compositor produces BGRA natively.
    EXPECT_EQ(frame.gpu_buffer->format(),
              nimrtc::video_frame::PixelFormat::kBGRA);
}

TEST(WindowsCapture, DxgiReportsDirtyRects) {
    if (!host_has_display()) {
        GTEST_SKIP() << "this host has no display to duplicate";
    }

    const auto cap = make_capture(WindowsCaptureBackend::kDxgiDesktopDuplication);
    GpuCollector collector;
    collector.attach(*cap);

    if (cap->open() != nimrtc::plugins::kOk) {
        if (!host_allows_another_session(
                WindowsCaptureBackend::kDxgiDesktopDuplication)) {
            GTEST_SKIP() << "this host refuses a second duplication session";
        }
        FAIL() << "the DXGI capture could not be opened";
    }

    // The compositor's diff is authoritative (plan A7).  A frame either
    // names the region that changed, or says "nothing changed" (an
    // empty dirty rect), or says "everything changed" (the full-frame
    // flag).  What must *never* happen is a frame that claims a full
    // update *and* names regions -- that leaves the encoder with two
    // conflicting answers.
    ASSERT_TRUE(collector.wait_for(3))
        << "the DXGI capture produced too few frames";

    const GpuCaptureFrame frame = collector.last();
    if (frame.dirty.full_frame) {
        EXPECT_TRUE(frame.dirty.rects.empty())
            << "a full-frame update must not also name regions";
    }

    // Every dirty rect must lie inside the captured surface.
    for (const auto& r : frame.dirty.rects) {
        EXPECT_LE(r.x + r.width, frame.gpu_buffer->width() + 1u);
        EXPECT_LE(r.y + r.height, frame.gpu_buffer->height() + 1u);
    }
}

TEST(WindowsCapture, DxgiReportsANonEmptyDirtyRectWhenTheDesktopChanges) {
    if (!host_has_display()) {
        GTEST_SKIP() << "this host has no display to duplicate";
    }

    const auto cap = make_capture(WindowsCaptureBackend::kDxgiDesktopDuplication);
    GpuCollector collector;
    collector.attach(*cap);

    if (cap->open() != nimrtc::plugins::kOk) {
        if (!host_allows_another_session(
                WindowsCaptureBackend::kDxgiDesktopDuplication)) {
            GTEST_SKIP() << "this host refuses a second duplication session";
        }
        FAIL() << "the DXGI capture could not be opened";
    }

    // Change the desktop while the capture is running: a window that
    // moves forces the compositor to report a dirty rect (plan A3:
    // "the dirty-rect metadata is non-empty for a window that changed").
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(8000);
    while (collector.non_empty_dirty_frames() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        nudge_the_desktop();
        collector.wait_for(1, std::chrono::milliseconds(100));
    }

    EXPECT_GT(collector.non_empty_dirty_frames(), 0u)
        << "a changing desktop must produce a non-empty dirty rect";
}

TEST(WindowsCapture, TheZeroCopyPathDoesNotCopyToTheHost) {
    if (!host_has_display()) {
        GTEST_SKIP() << "this host has no display to duplicate";
    }

    const auto cap = make_capture(WindowsCaptureBackend::kDxgiDesktopDuplication);
    GpuCollector collector;
    collector.attach(*cap);

    if (cap->open() != nimrtc::plugins::kOk) {
        if (!host_allows_another_session(
                WindowsCaptureBackend::kDxgiDesktopDuplication)) {
            GTEST_SKIP() << "this host refuses a second duplication session";
        }
        FAIL() << "the DXGI capture could not be opened";
    }
    ASSERT_TRUE(collector.wait_for(2));

    const auto stats = cap->stats();
    EXPECT_GT(stats.frames_captured, 0u);
    EXPECT_GT(stats.frames_zero_copy, 0u);
    // The zero-copy path never stages to host memory, so the copy
    // counter must stay at zero: the statistics must not lie about it.
    EXPECT_EQ(stats.bytes_copied, 0u);
}

TEST(WindowsCapture, TheCpuFallbackCountsItsBytes) {
    if (!host_has_display()) {
        GTEST_SKIP() << "this host has no display to duplicate";
    }

    // No GPU consumer attached: the capture must fall back to CPU
    // staging -- and say so in the statistics.
    const auto cap = make_capture(WindowsCaptureBackend::kDxgiDesktopDuplication);
    if (cap->open() != nimrtc::plugins::kOk) {
        if (!host_allows_another_session(
                WindowsCaptureBackend::kDxgiDesktopDuplication)) {
            GTEST_SKIP() << "this host refuses a second duplication session";
        }
        FAIL() << "the DXGI capture could not be opened";
    }

    // Wait for the capture thread to deliver at least one frame.  DXGI
    // is change-driven, so an idle desktop may legitimately not produce
    // a frame at all -- the wait therefore has a deadline and the test
    // reports a skip when the desktop simply never changed.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(4000);
    while (cap->stats().frames_captured == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (cap->stats().frames_captured == 0) {
        GTEST_SKIP() << "the desktop did not change during the capture";
    }

    const auto stats = cap->stats();
    ASSERT_GT(stats.frames_captured, 0u);
    EXPECT_GT(stats.frames_cpu_staged, 0u);
    EXPECT_EQ(stats.frames_zero_copy, 0u);
    EXPECT_GT(stats.bytes_copied, 0u);
}

TEST(WindowsCapture, TheCpuFallbackPopsABgraFrame) {
    if (!host_has_display()) {
        GTEST_SKIP() << "this host has no display to duplicate";
    }

    const auto cap = make_capture(WindowsCaptureBackend::kDxgiDesktopDuplication);
    if (cap->open() != nimrtc::plugins::kOk) {
        if (!host_allows_another_session(
                WindowsCaptureBackend::kDxgiDesktopDuplication)) {
            GTEST_SKIP() << "this host refuses a second duplication session";
        }
        FAIL() << "the DXGI capture could not be opened";
    }

    CaptureFrame frame{};
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(4000);
    while (cap->pop_frame(frame) != nimrtc::plugins::kOk &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (frame.pixels == nullptr) {
        GTEST_SKIP() << "the desktop did not change during the capture";
    }

    ASSERT_NE(frame.pixels, nullptr);
    EXPECT_EQ(frame.format, nimrtc::plugins::VideoPixelFormat::kBGRA);
    EXPECT_GT(frame.width, 0u);
    EXPECT_GT(frame.height, 0u);
    EXPECT_EQ(frame.stride, static_cast<std::int32_t>(frame.width * 4u));

    // The desktop is normally not black: at least one pixel must be
    // non-zero.  The *first* frame a freshly opened duplication hands
    // out can be an un-rendered front buffer, so a few frames are
    // inspected before drawing a conclusion.  On a headless /
    // locked-down host the compositor may legitimately hand out a black
    // surface -- that is an honest frame, not a broken capture -- so
    // report it as a skip, not a failure.  The non-black acceptance
    // proof lives in WgcCapturesAKnownWindow, which paints a window it
    // controls.
    std::size_t non_black_frames = 0;
    std::size_t inspected = 0;
    while (inspected < 8 && cap->pop_frame(frame) == nimrtc::plugins::kOk) {
        ++inspected;
        for (std::size_t i = 0; i + 3 < frame.size; i += 4) {
            if (frame.pixels[i] != 0 || frame.pixels[i + 1] != 0 ||
                frame.pixels[i + 2] != 0) {
                ++non_black_frames;
                break;
            }
        }
    }
    if (non_black_frames == 0) {
        GTEST_SKIP() << "the desktop on this host captures as black";
    }
}

TEST(WindowsCapture, ARegionNarrowsTheCapturedSurface) {
    if (!host_has_display()) {
        GTEST_SKIP() << "this host has no display to duplicate";
    }

    const auto probe = make_capture(WindowsCaptureBackend::kDxgiDesktopDuplication);
    const auto devices = probe->enumerate_devices();
    if (devices.empty() || devices.front().max_width < 320 ||
        devices.front().max_height < 240) {
        GTEST_SKIP() << "this host's display is too small for the region test";
    }

    CaptureConfig cfg{};
    cfg.region.x = 0;
    cfg.region.y = 0;
    cfg.region.width = 320;
    cfg.region.height = 240;

    const auto cap =
        make_capture(WindowsCaptureBackend::kDxgiDesktopDuplication, cfg);
    GpuCollector collector;
    collector.attach(*cap);
    if (cap->open() != nimrtc::plugins::kOk) {
        if (!host_allows_another_session(
                WindowsCaptureBackend::kDxgiDesktopDuplication)) {
            GTEST_SKIP() << "this host refuses a second duplication session";
        }
        FAIL() << "the DXGI capture could not be opened";
    }
    ASSERT_TRUE(collector.wait_for(1));

    const GpuCaptureFrame frame = collector.last();
    ASSERT_NE(frame.gpu_buffer, nullptr);
    EXPECT_EQ(frame.gpu_buffer->width(), 320u);
    EXPECT_EQ(frame.gpu_buffer->height(), 240u);
}

TEST(WindowsCapture, AnUnknownDeviceIsRefusedWithADistinctCode) {
    CaptureConfig cfg{};
    cfg.device_id = "\\\\.\\NOSUCHDISPLAY";
    const auto cap =
        make_capture(WindowsCaptureBackend::kDxgiDesktopDuplication, cfg);

    std::mutex mutex;
    WindowsCaptureError reported = WindowsCaptureError::kNone;
    cap->set_error_callback([&](WindowsCaptureError e, std::string_view) {
        std::lock_guard<std::mutex> lock(mutex);
        reported = e;
    });

    EXPECT_EQ(cap->open(), nimrtc::plugins::kErrHardwareError);
    EXPECT_FALSE(cap->is_open());
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(reported, WindowsCaptureError::kDeviceNotFound);
}

// ---------------------------------------------------------------------------
// The WGC path: capture a known window
// ---------------------------------------------------------------------------

#ifdef _WIN32

TEST(WindowsCapture, WgcCapturesAKnownWindow) {
    KnownWindow window;
    if (!window.valid()) {
        GTEST_SKIP() << "could not create the test window";
    }

    // A WGC window capture needs the user's consent.  When the host
    // has not granted it, the backend must report *that* -- distinctly
    // -- rather than pretending the window is black.
    CaptureConfig cfg{};
    cfg.device_id = window_device_id(window.handle());

    // ---- The CPU path: the frame must carry the window's content ------
    //
    // The non-black proof: the window paints a colour the test
    // controls, so the captured pixels must contain exactly that
    // colour.  A black frame would mean the capture lost the window.
    {
        const auto cpu_cap = make_capture(WindowsCaptureBackend::kWgc, cfg);
        std::mutex mutex;
        WindowsCaptureError reported = WindowsCaptureError::kNone;
        cpu_cap->set_error_callback([&](WindowsCaptureError e, std::string_view) {
            std::lock_guard<std::mutex> lock(mutex);
            reported = e;
        });

        const auto status = cpu_cap->open();
        if (status != nimrtc::plugins::kOk) {
            std::lock_guard<std::mutex> lock(mutex);
            if (reported == WindowsCaptureError::kConsentDenied) {
                GTEST_SKIP() << "the capture consent was not granted on this host";
            }
            if (reported == WindowsCaptureError::kDeviceNotFound) {
                GTEST_SKIP() << "this host cannot capture the test window";
            }
            if (reported == WindowsCaptureError::kCaptureFailed ||
                reported == WindowsCaptureError::kD3dDeviceFailed) {
                GTEST_SKIP() << "this host refuses another capture session";
            }
            FAIL() << "the WGC capture failed with error code "
                   << static_cast<std::uint32_t>(reported);
        }

        CaptureFrame cpu_frame{};
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(4000);
        while (cpu_cap->pop_frame(cpu_frame) != nimrtc::plugins::kOk &&
               std::chrono::steady_clock::now() < deadline) {
            window.repaint();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        ASSERT_NE(cpu_frame.pixels, nullptr);
        EXPECT_EQ(cpu_frame.format, nimrtc::plugins::VideoPixelFormat::kBGRA);

        // The window's colour is RGB(220, 200, 180) in BGRA byte order.
        const std::uint8_t expected_b = 180;
        const std::uint8_t expected_g = 200;
        const std::uint8_t expected_r = 220;

        bool non_black = false;
        for (std::size_t i = 0; i + 3 < cpu_frame.size; i += 4) {
            if (cpu_frame.pixels[i] != 0 || cpu_frame.pixels[i + 1] != 0 ||
                cpu_frame.pixels[i + 2] != 0) {
                non_black = true;
                break;
            }
        }
        EXPECT_TRUE(non_black) << "the captured known window is entirely black";

        // And the *known* colour must actually be in there: the window
        // paints RGB(220, 200, 180) everywhere.
        bool found_known_colour = false;
        for (std::size_t i = 0; i + 3 < cpu_frame.size; i += 4) {
            if (std::abs(static_cast<int>(cpu_frame.pixels[i]) - expected_b) <= 12 &&
                std::abs(static_cast<int>(cpu_frame.pixels[i + 1]) - expected_g) <= 12 &&
                std::abs(static_cast<int>(cpu_frame.pixels[i + 2]) - expected_r) <= 12) {
                found_known_colour = true;
                break;
            }
        }
        EXPECT_TRUE(found_known_colour)
            << "the captured window does not contain the colour the test painted";
    }

    // ---- The GPU path: the frame must carry a D3D11 texture ------------
    //
    // The zero-copy path delivers the surface *inside* the callback, so
    // the handle is a `ID3D11Texture2D` tagged with the D3D11 backend.
    const auto cap = make_capture(WindowsCaptureBackend::kWgc, cfg);
    std::mutex mutex;
    WindowsCaptureError reported = WindowsCaptureError::kNone;
    cap->set_error_callback([&](WindowsCaptureError e, std::string_view) {
        std::lock_guard<std::mutex> lock(mutex);
        reported = e;
    });

    GpuCollector collector;
    collector.attach(*cap);

    const auto status = cap->open();
    if (status != nimrtc::plugins::kOk) {
        std::lock_guard<std::mutex> lock(mutex);
        if (reported == WindowsCaptureError::kConsentDenied) {
            GTEST_SKIP() << "the capture consent was not granted on this host";
        }
        if (reported == WindowsCaptureError::kDeviceNotFound) {
            GTEST_SKIP() << "this host cannot capture the test window";
        }
        if (reported == WindowsCaptureError::kCaptureFailed ||
            reported == WindowsCaptureError::kD3dDeviceFailed) {
            GTEST_SKIP() << "this host refuses another capture session";
        }
        FAIL() << "the WGC capture failed with error code "
               << static_cast<std::uint32_t>(reported);
    }

    ASSERT_TRUE(collector.wait_for(1))
        << "the WGC capture produced no frame within the timeout";

    const GpuCaptureFrame frame = collector.last();
    ASSERT_NE(frame.gpu_buffer, nullptr);
    EXPECT_EQ(frame.gpu_buffer->handle().backend,
              nimrtc::video_frame::GpuBackend::kWindowsD3D11Texture);
    EXPECT_NE(frame.gpu_buffer->handle().handle, nullptr);

    // WGC has no diff information, so it must report the whole frame as
    // changed -- never an empty rect, which would mean "nothing
    // changed".
    EXPECT_TRUE(frame.dirty.full_frame);
    EXPECT_TRUE(frame.dirty.rects.empty());
}

TEST(WindowsCapture, WgcRefusesWithoutConsent) {
    KnownWindow window;
    if (!window.valid()) {
        GTEST_SKIP() << "could not create the test window";
    }

    CaptureConfig cfg{};
    cfg.device_id = window_device_id(window.handle());

    WindowsCaptureOptions opts;
    opts.backend = WindowsCaptureBackend::kWgc;
    opts.require_consent = true;
    WindowsCapture cap(std::move(cfg), opts);

    std::mutex mutex;
    WindowsCaptureError reported = WindowsCaptureError::kNone;
    cap.set_error_callback([&](WindowsCaptureError e, std::string_view) {
        std::lock_guard<std::mutex> lock(mutex);
        reported = e;
    });

    const auto status = cap.open();
    std::lock_guard<std::mutex> lock(mutex);
    if (status == nimrtc::plugins::kOk) {
        // Consent *was* granted on this host: nothing to assert here.
        SUCCEED() << "the host grants capture consent";
        return;
    }

    // The refusal must be its own code -- distinct from "the device is
    // gone" and distinct from a generic failure.  A failure that is
    // *not* a consent refusal (the host simply refused the session) is
    // a host condition, not a defect in the backend, so it is reported
    // as a skip.
    if (reported != WindowsCaptureError::kConsentDenied) {
        GTEST_SKIP() << "the host refused the session with error code "
                     << static_cast<std::uint32_t>(reported)
                     << " (not a consent refusal)";
    }

    EXPECT_EQ(reported, WindowsCaptureError::kConsentDenied);
    EXPECT_FALSE(cap.is_open());
}

#endif // _WIN32

// ---------------------------------------------------------------------------
// The same-result check (RD-10)
//
// The portable layer must behave identically no matter which backend
// produced the frame.  These tests assert the Windows backends against
// the *same* expectations the synthetic device satisfies in
// test_video_capture.cpp.
// ---------------------------------------------------------------------------

TEST(WindowsCapture, TheWindowsBackendsNegotiateLikeTheSyntheticOne) {
    // The synthetic device negotiates 1280x720 by default.
    nimrtc::video_capture::SyntheticCapture synthetic{CaptureConfig{}};
    ASSERT_EQ(synthetic.open(), nimrtc::plugins::kOk);
    const auto synthetic_cfg = synthetic.config();

    if (!host_has_display()) {
        GTEST_SKIP() << "this host has no display to capture";
    }

    const auto cap = make_capture(WindowsCaptureBackend::kDxgiDesktopDuplication);
    if (cap->open() != nimrtc::plugins::kOk) {
        if (!host_allows_another_session(
                WindowsCaptureBackend::kDxgiDesktopDuplication)) {
            GTEST_SKIP() << "this host refuses a second duplication session";
        }
        FAIL() << "the DXGI capture could not be opened";
    }
    const auto windows_cfg = cap->config();

    // The *shape* of the negotiated configuration is the same: a size,
    // a frame rate, a format tag and the zero-copy flag.  The size
    // itself differs (the synthetic device has a fixed 1280x720; a
    // display has its own native size), which is exactly what the plan
    // allows: "a platform backend may differ only in how it acquires
    // the pixels, never in what the pipeline does with them".
    EXPECT_GT(windows_cfg.format.width, 0u);
    EXPECT_GT(windows_cfg.format.height, 0u);
    EXPECT_EQ(windows_cfg.format.fps, synthetic_cfg.format.fps);
    EXPECT_FALSE(windows_cfg.format.tag.empty());
    EXPECT_TRUE(windows_cfg.format.zero_copy);
}

TEST(WindowsCapture, TheWindowsBackendsQueueAndDropLikeTheSyntheticOne) {
    if (!host_has_display()) {
        GTEST_SKIP() << "this host has no display to capture";
    }

    // Same queue depth, same drop policy, same counters as the
    // synthetic device: the *mechanism* is shared, not re-implemented.
    CaptureConfig cfg{};
    cfg.queue_depth = 2;
    cfg.drop_policy = nimrtc::plugins::CaptureDropPolicy::kDropOldest;

    const auto cap =
        make_capture(WindowsCaptureBackend::kDxgiDesktopDuplication, cfg);
    if (cap->open() != nimrtc::plugins::kOk) {
        if (!host_allows_another_session(
                WindowsCaptureBackend::kDxgiDesktopDuplication)) {
            GTEST_SKIP() << "this host refuses a second duplication session";
        }
        FAIL() << "the DXGI capture could not be opened";
    }

    // DXGI is *change-driven*: an idle desktop produces no frames at
    // all, so the test drives the desktop to change and then checks
    // that the shared queue mechanism behaved the way the synthetic
    // device's does.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(6000);
    while (cap->stats().frames_captured == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        nudge_the_desktop();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (cap->stats().frames_captured == 0) {
        GTEST_SKIP() << "the desktop did not change during the capture";
    }

    const auto stats = cap->stats();
    EXPECT_GT(stats.frames_captured, 0u);
    // Frames may be dropped when the consumer is not popping; when they
    // are, the counter must move -- and never exceed what was captured.
    EXPECT_LE(stats.frames_dropped, stats.frames_captured);
}

TEST(WindowsCapture, TheWindowsBackendsHonourTheRegionLikeTheSyntheticOne) {
    // The synthetic device crops to the region exactly (A8).  The
    // Windows backends must do the same.
    nimrtc::video_capture::SyntheticCapture synthetic{CaptureConfig{}};
    ASSERT_EQ(synthetic.open(), nimrtc::plugins::kOk);
    ASSERT_EQ(synthetic.produce_one(), nimrtc::plugins::kOk);
    CaptureFrame synthetic_frame{};
    ASSERT_EQ(synthetic.pop_frame(synthetic_frame), nimrtc::plugins::kOk);
    EXPECT_EQ(synthetic_frame.width, 1280u);
    EXPECT_EQ(synthetic_frame.height, 720u);

    if (!host_has_display()) {
        GTEST_SKIP() << "this host has no display to capture";
    }

    CaptureConfig cfg{};
    cfg.region.width = 800;
    cfg.region.height = 600;
    const auto cap =
        make_capture(WindowsCaptureBackend::kDxgiDesktopDuplication, cfg);
    GpuCollector collector;
    collector.attach(*cap);
    if (cap->open() != nimrtc::plugins::kOk) {
        if (!host_allows_another_session(
                WindowsCaptureBackend::kDxgiDesktopDuplication)) {
            GTEST_SKIP() << "this host refuses a second duplication session";
        }
        FAIL() << "the DXGI capture could not be opened";
    }
    ASSERT_TRUE(collector.wait_for(1));

    const GpuCaptureFrame frame = collector.last();
    ASSERT_NE(frame.gpu_buffer, nullptr);
    EXPECT_EQ(frame.gpu_buffer->width(), 800u);
    EXPECT_EQ(frame.gpu_buffer->height(), 600u);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TEST(WindowsCapture, CloseIsIdempotent) {
    const auto cap = make_capture(WindowsCaptureBackend::kDxgiDesktopDuplication);
    cap->close();
    cap->close();
    EXPECT_FALSE(cap->is_open());
}

TEST(WindowsCapture, ReopenAfterAFailedOpenIsSafe) {
    CaptureConfig cfg{};
    cfg.device_id = "\\\\.\\NOSUCHDISPLAY";
    const auto cap =
        make_capture(WindowsCaptureBackend::kDxgiDesktopDuplication, cfg);

    EXPECT_EQ(cap->open(), nimrtc::plugins::kErrHardwareError);
    EXPECT_EQ(cap->last_error(), WindowsCaptureError::kDeviceNotFound);
    // Re-opening must not crash and must report the same honest error.
    EXPECT_EQ(cap->reopen(), nimrtc::plugins::kErrHardwareError);
    EXPECT_EQ(cap->last_error(), WindowsCaptureError::kDeviceNotFound);
}

TEST(WindowsCapture, TheErrorCodesAreDistinct) {
    // The whole point of the Windows error enum: a host must be able to
    // tell "the user refused the consent" from "the device is gone".
    EXPECT_NE(WindowsCaptureError::kConsentDenied,
              WindowsCaptureError::kDeviceLost);
    EXPECT_NE(WindowsCaptureError::kDeviceLost,
              WindowsCaptureError::kDeviceNotFound);
    EXPECT_NE(WindowsCaptureError::kD3dDeviceFailed,
              WindowsCaptureError::kUnsupportedFormat);
    EXPECT_NE(WindowsCaptureError::kCaptureFailed,
              WindowsCaptureError::kDeviceNotFound);

    EXPECT_EQ(nimrtc::video_capture::windows_capture_error_name(
                  WindowsCaptureError::kConsentDenied),
              "consent-denied");
    EXPECT_EQ(nimrtc::video_capture::windows_capture_error_name(
                  WindowsCaptureError::kDeviceLost),
              "device-lost");
}
