/**
 * @file modules/video_sink/tests/test_d3d11_renderer.cpp
 * @brief Unit tests for the Windows D3D11 / DXGI presentation sink.
 *
 * Remote-desktop plan E2 (Windows renderer) + E8 (back-pressure).
 *
 * ## What is tested here
 *
 *   1. **The factory is registered** under the id `"d3d11"`, so a
 *      configuration that names it resolves through the registry.
 *   2. **The presentation clock paces the presenter.**  The renderer runs
 *      its presenter thread with a *deadline* wait, so a target of
 *      N frames per second yields N presents in about a second — not
 *      N presents in a burst followed by a spin.
 *   3. **The back-pressure policy is latest-wins and bounded.**  When the
 *      producer pushes faster than the presenter consumes, the *older*
 *      frame is dropped and the drop is counted — the mailbox never grows
 *      past one frame, so the memory is bounded by construction.
 *   4. **The render call never blocks.**  `render()` hands the frame to
 *      the presenter and returns; the engine's tick thread is not parked
 *      by the GPU.
 *
 * ## The offscreen mode
 *
 * A swap chain needs a window, and a CI machine has none.  The renderer
 * therefore supports an **offscreen** mode: it renders into a plain
 * texture and *still* runs the presentation clock.  That is what makes
 * the frame-rate and drop-accounting assertions testable without a
 * display — which is exactly what plan E2's acceptance criterion needs.
 *
 * ## The no-GPU case
 *
 * `open()` reports `kErrHardwareError` on a machine with no Direct3D 11
 * device (a headless CI container, a VM without a GPU).  The renderer
 * reports that honestly rather than pretending to present, so the tests
 * that need a device are skipped in that case — they are *not* failed,
 * because "this machine has no GPU" is not a renderer bug.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/plugins/video_sink.hpp>
#include <nimrtc/video_sink/d3d11_renderer.hpp>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <d3d11.h>
#endif

namespace {

namespace np = nimrtc::plugins;

/** A packed BGRA picture (the format the desktop compositors produce).
 *
 *  The pixels have to be *real*: the sink's CPU path copies them into
 *  the upload texture, and a frame with a null `plane_y` is refused
 *  (it is a programming error, not a frame). */
class BgraFrame {
public:
    BgraFrame(std::uint32_t w, std::uint32_t h, std::int64_t capture_ts_us)
        : pixels_(static_cast<std::size_t>(w) * h * 4u, 0x20u) {
        frame_.width  = w;
        frame_.height = h;
        frame_.format = np::VideoPixelFormat::kBGRA;
        frame_.stride_y = static_cast<std::int32_t>(w * 4u);
        frame_.capture_ts_us = static_cast<np::TimestampUs>(capture_ts_us);
        frame_.frame_seq = static_cast<std::uint32_t>(capture_ts_us / 1000);
        frame_.buffer_size = pixels_.size();
        frame_.plane_y = pixels_.data();
    }

    /** The frame descriptor the sink consumes. */
    const np::VideoSourceFrame& get() const noexcept { return frame_; }

private:
    std::vector<std::uint8_t> pixels_;
    np::VideoSourceFrame frame_;
};

/** Sleep for @p ms milliseconds. */
void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/** Wait until @p cond holds, polling every millisecond, up to @p budget_ms.
 *
 *  A fixed `sleep_ms()` before an assertion encodes an assumption about
 *  how fast the presenter thread gets scheduled.  On a loaded CI runner
 *  the assumption is simply false: `GpuSurfaceIsCopiedDeviceSide`
 *  asserted `frames_presented > 0` after a fixed 150 ms and failed on
 *  `windows-2022` while passing locally.  Waiting for the *condition*
 *  rather than for a duration tests the same property and cannot lose a
 *  race with the scheduler.
 *
 *  @return whether the condition held before the budget ran out.
 */
template <typename Predicate>
bool wait_until(Predicate cond, int budget_ms) {
    for (int elapsed = 0; elapsed < budget_ms; ++elapsed) {
        if (cond()) return true;
        sleep_ms(1);
    }
    return cond();
}

// ---------------------------------------------------------------------------
// The factory / the registry
// ---------------------------------------------------------------------------

TEST(D3D11RendererFactory, HasTheExpectedId) {
    nimrtc::video_sink::D3D11RendererFactory fac;
    EXPECT_EQ(fac.id(), "d3d11");
    EXPECT_FALSE(fac.display_name().empty());
}

TEST(D3D11RendererFactory, CreatesASink) {
    nimrtc::video_sink::D3D11RendererFactory fac;
    std::unique_ptr<np::IVideoSink> sink(fac.create(np::VideoSinkConfig{}));
    ASSERT_NE(sink, nullptr);
    EXPECT_TRUE(sink->supports_cpu_render());
    EXPECT_TRUE(sink->supports_gpu_render());
    EXPECT_TRUE(sink->is_hardware_accelerated());
    EXPECT_EQ(sink->hardware_backend(), "d3d11");
}

TEST(D3D11RendererRegistration, IsRegistered) {
    nimrtc::video_sink::register_default_plugins();
    const auto* f = nimrtc::core::PluginRegistry::instance().get_video_sink("d3d11");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->id(), "d3d11");
}

// ---------------------------------------------------------------------------
// The viewport window is opt-in
// ---------------------------------------------------------------------------

#if defined(_WIN32)
// The renderer creates a `NimRTCD3D11Viewport` window when the host gave
// it no `native_window` and asked for a viewport.  A window that nobody
// asked for is a surprise — and a test suite that flashes one window per
// test case on the developer's desktop is the worst version of that
// surprise.  These two tests are the regression guard for exactly that:
// the default configuration must not put a window on the screen, and the
// opt-in must put one there.
namespace {

/** The number of visible `NimRTCD3D11Viewport` windows *this process*
 *  owns right now.
 *
 *  The count is scoped to the owning process on purpose: the window class
 *  is registered per process, so two processes never share a viewport,
 *  and a count taken across the whole desktop would be at the mercy of
 *  whatever else the machine happens to be running. */
int visible_viewport_windows() {
    struct Counter {
        int count = 0;

        static BOOL CALLBACK visit(HWND hwnd, LPARAM param) {
            auto* self = reinterpret_cast<Counter*>(param);
            if (!::IsWindowVisible(hwnd)) return TRUE;
            DWORD owner = 0;
            ::GetWindowThreadProcessId(hwnd, &owner);
            if (owner != ::GetCurrentProcessId()) return TRUE;
            wchar_t cls[64]{};
            const int n = ::GetClassNameW(hwnd, cls, 64);
            if (n > 0 && std::wstring(cls, static_cast<std::size_t>(n)) ==
                             L"NimRTCD3D11Viewport") {
                self->count++;
            }
            return TRUE;
        }
    };
    Counter counter;
    ::EnumWindows(&Counter::visit, reinterpret_cast<LPARAM>(&counter));
    return counter.count;
}

}  // namespace

TEST(D3D11Renderer, DoesNotCreateAViewportWindowByDefault) {
    const int before = visible_viewport_windows();

    np::VideoSinkConfig cfg{};
    // Neither a host window nor a request for one: the renderer must
    // fall back to its offscreen target instead of opening a window.
    cfg.native_window = nullptr;
    cfg.offscreen     = false;
    cfg.create_viewport_window = false;
    cfg.width  = 64;
    cfg.height = 64;

    nimrtc::video_sink::D3D11Renderer renderer(cfg);
    const np::Status rc = renderer.open();
    if (rc == np::kErrHardwareError || rc == np::kErrUnsupported) {
        GTEST_SKIP() << "no Direct3D 11 device on this machine";
    }
    ASSERT_EQ(rc, np::kOk);

    EXPECT_EQ(visible_viewport_windows(), before)
        << "the renderer opened a viewport window nobody asked for";

    renderer.close();
    EXPECT_EQ(visible_viewport_windows(), before)
        << "close() left a viewport window behind";
}

TEST(D3D11Renderer, CreatesTheViewportWindowWhenAsked) {
    np::VideoSinkConfig cfg{};
    cfg.native_window = nullptr;
    cfg.offscreen     = false;
    cfg.create_viewport_window = true;
    cfg.width  = 64;
    cfg.height = 64;

    const int before = visible_viewport_windows();

    nimrtc::video_sink::D3D11Renderer renderer(cfg);
    const np::Status rc = renderer.open();
    if (rc == np::kErrHardwareError || rc == np::kErrUnsupported) {
        GTEST_SKIP() << "no Direct3D 11 device on this machine";
    }
    ASSERT_EQ(rc, np::kOk);

    EXPECT_GT(visible_viewport_windows(), before)
        << "create_viewport_window=true did not open a viewport window";

    renderer.close();
    EXPECT_EQ(visible_viewport_windows(), before)
        << "close() left a viewport window behind";
}
#endif  // _WIN32

// ---------------------------------------------------------------------------
// The presentation clock
// ---------------------------------------------------------------------------

// The presenter paces itself to `target_fps`.  At 20 fps a window of
// ~600 ms must yield roughly 12 presents — the tolerance is wide
// because the measurement includes the presenter's wake-up jitter and
// the machine's scheduling.
TEST(D3D11Renderer, PresentationClockPacesThePresenter) {
    np::VideoSinkConfig cfg{};
    cfg.offscreen = true;
    cfg.width  = 64;
    cfg.height = 64;
    cfg.target_fps = 20;

    nimrtc::video_sink::D3D11Renderer renderer(cfg);
    const np::Status rc = renderer.open();
    if (rc == np::kErrHardwareError || rc == np::kErrUnsupported) {
        GTEST_SKIP() << "no Direct3D 11 device on this machine";
    }
    ASSERT_EQ(rc, np::kOk);

    const auto start = std::chrono::steady_clock::now();
    const auto window = std::chrono::milliseconds(600);

    // Push frames until the window closes.  The presenter consumes them
    // at its own rate, so the *accepted* count grows faster than the
    // *presented* count — the difference is the mailbox's back-pressure
    // drops, and it is exactly what the "bounded memory" property is
    // made of.
    std::uint64_t pushed = 0;
    while (std::chrono::steady_clock::now() - start < window) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();
        const BgraFrame f(64, 64, elapsed);
        renderer.render(f.get());
        ++pushed;
        sleep_ms(1);
    }

    // Give the presenter a moment to drain what it already holds.  Wait
    // for a present rather than for a fixed delay: the runner's
    // scheduler is what decides when the thread wakes up.
    wait_until([&] { return renderer.stats().frames_presented > 0; }, 5000);

    const np::VideoSinkStats st = renderer.stats();
    renderer.close();

    // The presenter must have presented *something* — the clock works.
    EXPECT_GT(st.frames_presented, 0u);
    // ... and it must not have presented *more* than the clock allows.
    // (A spin-wait presenter would present every pushed frame.)
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    const std::uint64_t budget = static_cast<std::uint64_t>(elapsed_ms) * 20u / 1000u + 4u;
    EXPECT_LE(st.frames_presented, budget);
    // ... and it must have presented *fewer* than were pushed: the
    // clock dropped the surplus, which is what the mailbox's
    // back-pressure is for.
    EXPECT_LT(st.frames_presented, pushed);
}

// ---------------------------------------------------------------------------
// The back-pressure policy (plan E8)
// ---------------------------------------------------------------------------

// The mailbox is a *single slot* with a latest-wins policy: a frame
// that arrives while another is still waiting replaces it.  The replaced
// frame is counted as a drop.
//
// The test drives the producer *much* faster than the presenter (a
// 1 fps target, so the presenter consumes one frame per second) and
// asserts that (a) the drop counter moved and (b) the mailbox never
// held more than one frame.
TEST(D3D11Renderer, LatestWinsBackPressureCountsDrops) {
    np::VideoSinkConfig cfg{};
    cfg.offscreen = true;
    cfg.width  = 64;
    cfg.height = 64;
    cfg.target_fps = 1;   // deliberately far slower than the producer

    nimrtc::video_sink::D3D11Renderer renderer(cfg);
    const np::Status rc = renderer.open();
    if (rc == np::kErrHardwareError || rc == np::kErrUnsupported) {
        GTEST_SKIP() << "no Direct3D 11 device on this machine";
    }
    ASSERT_EQ(rc, np::kOk);

    // 40 frames pushed back-to-back: the presenter (at 1 fps) cannot
    // keep up, so the mailbox replaces its occupant over and over.
    for (std::uint64_t i = 0; i < 40u; ++i) {
        const BgraFrame f(64, 64, static_cast<std::int64_t>(i) * 1000);
        renderer.render(f.get());
    }

    const np::VideoSinkStats st = renderer.stats();
    renderer.close();

    EXPECT_EQ(st.frames_rendered, 40u);
    EXPECT_GT(st.frames_dropped_present, 0u);
    // The memory bound is *structural*: the mailbox holds one frame.
    EXPECT_LE(st.presenter_queue_depth, 1u);
}

// ---------------------------------------------------------------------------
// The render call never blocks
// ---------------------------------------------------------------------------

// The engine's tick thread hands the frame over and returns.  The
// presenter's GPU work happens on the sink's own thread (RD-6: a device
// plug-in owns its threads; the engine stays a single-threaded
// orchestrator).
TEST(D3D11Renderer, RenderDoesNotBlock) {
    np::VideoSinkConfig cfg{};
    cfg.offscreen = true;
    cfg.width  = 64;
    cfg.height = 64;
    cfg.target_fps = 30;

    nimrtc::video_sink::D3D11Renderer renderer(cfg);
    const np::Status rc = renderer.open();
    if (rc == np::kErrHardwareError || rc == np::kErrUnsupported) {
        GTEST_SKIP() << "no Direct3D 11 device on this machine";
    }
    ASSERT_EQ(rc, np::kOk);

    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < 20u; ++i) {
        const BgraFrame f(64, 64, static_cast<std::int64_t>(i) * 1000);
        renderer.render(f.get());
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    renderer.close();

    // 20 hand-offs must not take anywhere near a frame interval of GPU
    // time each; a blocking render would take at least 20 / 30 s.
    EXPECT_LT(elapsed, 1000);
}

// ---------------------------------------------------------------------------
// The GPU path
// ---------------------------------------------------------------------------

// A GPU surface that is not a D3D11 texture is refused rather than
// misinterpreted.
TEST(D3D11Renderer, RejectsAForeignGpuBackend) {
    np::VideoSinkConfig cfg{};
    cfg.offscreen = true;

    nimrtc::video_sink::D3D11Renderer renderer(cfg);
    const np::Status rc = renderer.open();
    if (rc == np::kErrHardwareError || rc == np::kErrUnsupported) {
        GTEST_SKIP() << "no Direct3D 11 device on this machine";
    }
    ASSERT_EQ(rc, np::kOk);

    np::GpuHandle h{};
    h.backend = np::GpuBackend::kAppleCFPixelBuffer;
    h.handle  = reinterpret_cast<void*>(0x1234);
    np::GpuBuffer surface(h, 64, 64, np::VideoPixelFormat::kBGRA);

    EXPECT_EQ(renderer.render(surface), np::kErrUnsupported);
    renderer.close();
}

// A frame with no dimensions is a programming error, not a frame.
TEST(D3D11Renderer, RejectsAnEmptyFrame) {
    np::VideoSinkConfig cfg{};
    cfg.offscreen = true;

    nimrtc::video_sink::D3D11Renderer renderer(cfg);
    const np::Status rc = renderer.open();
    if (rc == np::kErrHardwareError || rc == np::kErrUnsupported) {
        GTEST_SKIP() << "no Direct3D 11 device on this machine";
    }
    ASSERT_EQ(rc, np::kOk);

    np::VideoSourceFrame empty{};
    EXPECT_EQ(renderer.render(empty), np::kErrInvalidParam);
    renderer.close();
}

// ---------------------------------------------------------------------------
// The GPU surface path (plan E5)
// ---------------------------------------------------------------------------

#if defined(_WIN32)

namespace {

/** A real `ID3D11Texture2D` the GPU path can actually consume.
 *
 *  The renderer validates the handle and copies the texture on the
 *  presenter thread, so a fake pointer would crash rather than test
 *  anything.  The texture is created on the *default* device — the same
 *  one the capture backend produces — with `CopyResource` as a source
 *  requirement (no bind flags, no CPU access). */
struct D3D11TextureFixture {
    ID3D11Device*        device  = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D*     texture = nullptr;

    bool create(DXGI_FORMAT format, std::uint32_t w, std::uint32_t h) {
        if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
                                       nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION, &device,
                                       nullptr, &context))) {
            return false;
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width  = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        return SUCCEEDED(device->CreateTexture2D(&td, nullptr, &texture));
    }

    ~D3D11TextureFixture() {
        if (texture) texture->Release();
        if (context) context->Release();
        if (device)  device->Release();
    }
};

/** A `GpuBuffer` wrapping a live D3D11 texture. */
np::GpuBuffer wrap(ID3D11Texture2D* tex, std::uint32_t w, std::uint32_t h,
                    np::VideoPixelFormat fmt) {
    np::GpuHandle handle{};
    handle.backend = np::GpuBackend::kWindowsD3D11Texture;
    handle.handle  = tex;
    return np::GpuBuffer(handle, w, h, fmt);
}

} // namespace

// A BGRA8 surface goes from the mailbox to the present target with a
// device-to-device copy: the picture never enters host memory.  The
// sink reports the copy through `gpu_device_copies`, and nothing lands
// in the staging-fallback counter.
TEST(D3D11Renderer, GpuSurfaceIsCopiedDeviceSide) {
    D3D11TextureFixture fx;
    if (!fx.create(DXGI_FORMAT_B8G8R8A8_UNORM, 64, 64)) {
        GTEST_SKIP() << "no Direct3D 11 device on this machine";
    }

    np::VideoSinkConfig cfg{};
    cfg.offscreen = true;
    cfg.width  = 64;
    cfg.height = 64;
    cfg.target_fps = 30;

    nimrtc::video_sink::D3D11Renderer renderer(cfg);
    const np::Status rc = renderer.open();
    if (rc == np::kErrHardwareError || rc == np::kErrUnsupported) {
        GTEST_SKIP() << "no Direct3D 11 device on this machine";
    }
    ASSERT_EQ(rc, np::kOk);

    np::GpuBuffer surface = wrap(fx.texture, 64, 64, np::VideoPixelFormat::kBGRA);
    ASSERT_EQ(renderer.render(surface), np::kOk);
    // `render()` only posts to the mailbox; the copy happens on the
    // presenter's next deadline.  Wait for the copy, not for a fixed
    // duration -- a loaded runner can take longer than any constant.
    const bool copied = wait_until(
        [&] { return renderer.stats().gpu_device_copies > 0; }, 5000);

    const np::VideoSinkStats st = renderer.stats();
    renderer.close();

    ASSERT_TRUE(copied) << "the presenter never copied the surface";
    EXPECT_GT(st.gpu_device_copies, 0u);
    EXPECT_EQ(st.gpu_staging_fallbacks, 0u);
    EXPECT_EQ(st.gpu_surfaces_refused, 0u);
    EXPECT_GT(st.frames_presented, 0u);
    // A GPU surface that fits the clock is not a back-pressure drop.
    EXPECT_EQ(st.frames_dropped_present, 0u);
}

// A surface that is not the present format cannot be copied into a BGRA8
// target; without a CPU mirror there is nothing to present, so the sink
// says so (`kErrUnsupported`) and counts the staging fallback — it does
// not copy garbage into the target (plan E5's documented fallback).
TEST(D3D11Renderer, NonBgraSurfaceTakesTheDocumentedFallback) {
    D3D11TextureFixture fx;
    if (!fx.create(DXGI_FORMAT_NV12, 64, 64)) {
        GTEST_SKIP() << "no Direct3D 11 device on this machine";
    }

    np::VideoSinkConfig cfg{};
    cfg.offscreen = true;
    cfg.width  = 64;
    cfg.height = 64;

    nimrtc::video_sink::D3D11Renderer renderer(cfg);
    const np::Status rc = renderer.open();
    if (rc == np::kErrHardwareError || rc == np::kErrUnsupported) {
        GTEST_SKIP() << "no Direct3D 11 device on this machine";
    }
    ASSERT_EQ(rc, np::kOk);

    np::GpuBuffer surface = wrap(fx.texture, 64, 64, np::VideoPixelFormat::kNV12);
    EXPECT_EQ(renderer.render(surface), np::kErrUnsupported);
    // The timeline overload must refuse the same frame for the same reason.
    np::VideoFrameInfo info{};
    EXPECT_EQ(renderer.render(surface, info), np::kErrUnsupported);

    const np::VideoSinkStats st = renderer.stats();
    renderer.close();

    EXPECT_EQ(st.gpu_device_copies, 0u);
    EXPECT_GE(st.gpu_staging_fallbacks, 2u);
    EXPECT_EQ(st.frames_presented, 0u);
}

// A non-BGRA surface that *does* carry a CPU mirror is presented from the
// mirror (I420 → BGRA conversion) instead of a GPU readback — the frame
// still reaches the display, and only the conversion is counted.
TEST(D3D11Renderer, NonBgraSurfaceWithCpuMirrorPresents) {
    D3D11TextureFixture fx;
    if (!fx.create(DXGI_FORMAT_NV12, 64, 64)) {
        GTEST_SKIP() << "no Direct3D 11 device on this machine";
    }

    const std::uint32_t w = 64, h = 64;
    const std::int32_t stride_c = static_cast<std::int32_t>(w / 2u);
    std::vector<std::uint8_t> y(static_cast<std::size_t>(w) * h, 0x80u);
    std::vector<std::uint8_t> u(static_cast<std::size_t>(stride_c) * (h / 2u), 0x80u);
    std::vector<std::uint8_t> v(static_cast<std::size_t>(stride_c) * (h / 2u), 0x80u);

    np::GpuBuffer surface = wrap(fx.texture, w, h, np::VideoPixelFormat::kI420);
    surface.set_cpu_mirror(y.data(), u.data(), v.data(),
                           static_cast<std::int32_t>(w), stride_c, stride_c);

    np::VideoSinkConfig cfg{};
    cfg.offscreen = true;
    cfg.width  = w;
    cfg.height = h;
    cfg.target_fps = 30;

    nimrtc::video_sink::D3D11Renderer renderer(cfg);
    const np::Status rc = renderer.open();
    if (rc == np::kErrHardwareError || rc == np::kErrUnsupported) {
        GTEST_SKIP() << "no Direct3D 11 device on this machine";
    }
    ASSERT_EQ(rc, np::kOk);

    ASSERT_EQ(renderer.render(surface), np::kOk);
    // The fallback counter and the present counter are two separate
    // events on the presenter thread: the fallback is recorded when the
    // surface is rejected, the present when the converted frame reaches
    // the target.  Waiting for the fallback alone returns before the
    // present, so wait for both.
    const bool presented = wait_until(
        [&] {
            const np::VideoSinkStats s = renderer.stats();
            return s.gpu_staging_fallbacks > 0 && s.frames_presented > 0;
        },
        5000);

    const np::VideoSinkStats st = renderer.stats();
    renderer.close();

    ASSERT_TRUE(presented) << "the staging fallback never reached the display";
    EXPECT_GE(st.gpu_staging_fallbacks, 1u);
    // The mirror is planar I420, so the CPU path's I420 → BGRA
    // conversion ran; the frame reached the display from the mirror
    // rather than from a GPU readback.
    EXPECT_GE(st.format_conversions, 1u);
    EXPECT_GT(st.frames_presented, 0u);
}

// The headline property of the GPU path: the *producer* never does the
// copy.  Two thousand `render()` calls on a GPU surface must cost about
// as much as two thousand CPU-path calls that are dropped in the
// mailbox — i.e. the caller's thread is not serialised on the GPU.  The
// assertion is loose on purpose (it is a smoke test, not a benchmark):
// it fails only if `render()` started doing per-frame device work.
TEST(D3D11Renderer, GpuRenderDoesNotDoDeviceWorkOnTheCaller) {
    D3D11TextureFixture fx;
    if (!fx.create(DXGI_FORMAT_B8G8R8A8_UNORM, 1280, 720)) {
        GTEST_SKIP() << "no Direct3D 11 device on this machine";
    }

    np::VideoSinkConfig cfg{};
    cfg.offscreen = true;
    cfg.width  = 1280;
    cfg.height = 720;
    cfg.target_fps = 30;

    nimrtc::video_sink::D3D11Renderer renderer(cfg);
    const np::Status rc = renderer.open();
    if (rc == np::kErrHardwareError || rc == np::kErrUnsupported) {
        GTEST_SKIP() << "no Direct3D 11 device on this machine";
    }
    ASSERT_EQ(rc, np::kOk);

    // The copy the producer must *not* pay for: a 720p BGRA surface is
    // ~3.7 MB.  Two thousand of them are ~7.4 GB — no loop that copies
    // each one through the GPU (let alone through the host) finishes in
    // the 2 s budget below, while reference-count bumps do.
    np::GpuBuffer surface = wrap(fx.texture, 1280, 720, np::VideoPixelFormat::kBGRA);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 2000; ++i) {
        ASSERT_EQ(renderer.render(surface), np::kOk);
    }
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    const np::VideoSinkStats st = renderer.stats();
    renderer.close();

    EXPECT_EQ(st.frames_rendered, 2000u);
    EXPECT_GT(st.frames_dropped_present, 1900u);   // the mailbox replaced them
    EXPECT_LT(elapsed_ms, 2000);                   // not one device copy per call
}

#endif  // _WIN32

} // namespace
