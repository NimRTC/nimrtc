/**
 * @file src/modules/video_sink/src/d3d11_renderer.cpp
 * @brief The Windows D3D11 / DXGI presentation sink.
 *
 * See include/nimrtc/video_sink/d3d11_renderer.hpp for the contract.
 *
 * ## The structure of this file
 *
 *   - `Presenter` 鈥?the private state of the sink: the D3D11 device, the
 *     swap chain (or the offscreen target), the presenter thread, the
 *     single-slot latest-wins mailbox and the presentation clock.
 *   - `D3D11Renderer` 鈥?the `plugins::IVideoSink` face.
 *
 * ## Why the presenter owns a thread
 *
 * RD-6: the engine is a single-threaded orchestrator; a *device* plug-in
 * owns its own threads.  A renderer is a device plug-in.  The engine's
 * tick thread hands the frame over and returns; the presenter thread
 * does the GPU work and the pacing.
 *
 * ## Why the mailbox is a single slot
 *
 * The back-pressure policy for a renderer is *latest-wins* (plan E8):
 * the newest picture is the one worth showing, and a stale one is worth
 * nothing.  A single slot with a replace-on-full policy gives exactly
 * that, and 鈥?more importantly 鈥?it makes the memory bound *structural*:
 * the mailbox holds at most one frame, whatever the producer does.  A
 * bounded queue of depth N would also be bounded, but N frames of a 4K
 * surface is a lot of memory; one frame is not.
 *
 * ## The presentation clock
 *
 * The presenter owns a deadline.  After each present it advances the
 * deadline by one frame interval (1 / target_fps) and sleeps until then
 * with `std::condition_variable::wait_until` 鈥?a *timed wait*, not a
 * spin.  When no frame is pending, the wait is indefinite (the mailbox's
 * own condition variable is signalled on a new frame), so an idle
 * renderer costs nothing at all.
 */

#include <nimrtc/video_sink/d3d11_renderer.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <nimrtc/core/log.hpp>
// The CPU path converts a decoded I420 picture into the present target's
// BGRA format, so it needs the shared conversion helpers.  (The include
// arrives transitively through the plugin seam, but the dependency is
// stated explicitly here 鈥?the renderer calls the helpers directly.)
#include <nimrtc/video_frame/frame.hpp>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0A00   // Windows 10
  #endif
  #include <windows.h>
  #include <d3d11.h>
  #include <dxgi1_2.h>
#endif

namespace nimrtc::video_sink {

namespace {

using Clock = std::chrono::steady_clock;

/** The default presentation rate when the configuration names none. */
constexpr std::uint32_t kDefaultPresentFps = 30;

} // namespace

// ===========================================================================
// D3D11Renderer::Impl
// ===========================================================================

struct D3D11Renderer::Impl {
    explicit Impl(plugins::VideoSinkConfig cfg) noexcept
        : cfg_(std::move(cfg)) {}

    ~Impl() { stop_presenter(); }

    plugins::VideoSinkConfig cfg_;

    /// Guards `cfg_`.
    ///
    ///  The configuration is written by `set_config()` (which the engine
    ///  calls from its own thread when the viewer resizes the window)
    ///  and read by `config()`.  The presenter thread does *not* read
    ///  it 鈥?the presentation cadence is captured when the presenter
    ///  starts 鈥?so the lock only ever guards the config itself.
    mutable std::mutex cfg_mutex_;

    // ---- Counters (atomic 鈥?`stats()` may be read from any thread) ----
    std::atomic<std::uint64_t> frames_rendered_{0};
    std::atomic<std::uint64_t> frames_dropped_{0};
    std::atomic<std::uint64_t> frames_dropped_present_{0};
    std::atomic<std::uint64_t> frames_presented_{0};
    std::atomic<std::uint64_t> format_conversions_{0};
    std::atomic<std::uint64_t> render_errors_{0};
    std::atomic<std::int64_t>  last_render_age_us_{0};
    std::atomic<std::uint32_t> present_fps_{0};
    /** GPU surfaces copied device-to-device into the present target. */
    std::atomic<std::uint64_t> gpu_copies_{0};
    /** GPU surfaces that had to go through host staging (plan E5). */
    std::atomic<std::uint64_t> gpu_staging_readbacks_{0};
    /** GPU surfaces refused outright (foreign backend / bad handle). */
    std::atomic<std::uint64_t> gpu_surfaces_refused_{0};

    /** The most recent frame's metadata (for `current_frame_info()`). */
    mutable std::mutex      info_mutex_;
    plugins::VideoFrameInfo last_info_{};

    // ---- The mailbox (single slot, latest-wins) ------------------------
    //
    // `slot_` holds the frame that is *waiting* to be presented.  A new
    // frame replaces it; the replaced one is counted as a drop.  The
    // mutex + condition variable are the only synchronisation the
    // presenter needs: the producer never blocks on the GPU.
    struct Slot {
        /** The pending frame's pixels (BGRA, packed). */
        std::vector<std::uint8_t> pixels;
        /** The pending frame's dimensions. */
        std::uint32_t width  = 0;
        std::uint32_t height = 0;
        /** The pending frame's capture timestamp. */
        std::int64_t capture_ts_us = 0;
        /** The instant the frame must reach the display (local monotonic).
         *  Zero = no deadline (present at the presentation clock's pace).
         *  See `plugins::VideoFrameInfo::present_at_us` (v1.3 Q6). */
        std::int64_t present_at_us = 0;
        std::uint32_t frame_seq     = 0;
        std::uint32_t rtp_timestamp = 0;
        /** Non-null when the frame is a GPU surface: the mailbox holds a
         *  reference on it, so the texture outlives the borrowed
         *  `render()` call.  When set, `pixels` is unused. */
        std::shared_ptr<const plugins::GpuBuffer> surface;
        /** True when a frame is waiting. */
        bool valid = false;
    };

    std::mutex              mtx_;
    std::condition_variable cv_;
    Slot                    slot_;
    bool                    stop_ = false;

    // ---- The presenter thread -------------------------------------------
    std::thread presenter_;

    /** True once the presenter thread has been asked to stop. */
    std::atomic<bool> running_{false};

#if defined(_WIN32)
    // ---- The D3D11 / DXGI objects ---------------------------------------
    ID3D11Device*           device_ = nullptr;
    ID3D11DeviceContext*    context_ = nullptr;
    IDXGISwapChain*         swap_chain_ = nullptr;
    ID3D11Texture2D*        back_buffer_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    /** The offscreen target (used when there is no swap chain). */
    ID3D11Texture2D*        offscreen_ = nullptr;
    ID3D11RenderTargetView* offscreen_rtv_ = nullptr;
    /** The staging texture the CPU path uploads through. */
    ID3D11Texture2D*        upload_ = nullptr;

    /** The back buffer's size (the swap chain's window size). */
    std::uint32_t back_buffer_w_ = 0;
    std::uint32_t back_buffer_h_ = 0;
    /** The offscreen target's current size. */
    std::uint32_t offscreen_w_ = 0;
    std::uint32_t offscreen_h_ = 0;
    /** The upload texture's current size. */
    std::uint32_t upload_w_ = 0;
    std::uint32_t upload_h_ = 0;

    /** The viewport window the renderer created for itself when the host
     *  supplied no `native_window` (v1-video-render-engine D2).  Owned 鈥?     *  destroyed in `destroy_d3d_objects()`; null when the host window
     *  is used instead. */
    HWND self_hwnd_ = nullptr;
#endif

    /** The presentation cadence, in frames per second. */
    std::uint32_t target_fps() const noexcept {
        std::lock_guard<std::mutex> lock(cfg_mutex_);
        if (cfg_.target_fps > 0) return cfg_.target_fps;
        if (cfg_.max_fps > 0) return cfg_.max_fps;
        return kDefaultPresentFps;
    }

    // ---- The presenter ---------------------------------------------------

    /** Start the presenter thread. */
    void start_presenter();

    /** Stop the presenter thread and join it. */
    void stop_presenter();

    /** The presenter thread's body. */
    void present_loop();

    /** Present one frame from the slot. Returns false when nothing was
     *  available. */
    bool present_one(const Slot& slot);

#if defined(_WIN32)
    /** Create the D3D11 device, the swap chain (or the offscreen
     *  target) and the render target view. */
    bool create_d3d_objects();

    /** Create the renderer's own viewport window (used when the host
     *  supplied no `native_window`).  Returns false when no window can
     *  be created 鈥?the caller then falls back to the offscreen target.
     *
     *  Returns false *without* creating anything unless
     *  `cfg_.create_viewport_window` is set: a renderer that pops up a
     *  visible window on its own initiative is a surprise, and a test
     *  run that flashes a window on the developer's desktop is worse.
     */
    bool create_self_window();

    /** Release every D3D11 / DXGI object. */
    void destroy_d3d_objects();

    /** Upload a packed BGRA frame into the present target and present it.
     *
     *  @param pixels  The packed BGRA source.
     *  @param width   The frame width.
     *  @param height  The frame height.
     *  @return true on success. */
    bool present_bgra(const std::uint8_t* pixels,
                      std::uint32_t width,
                      std::uint32_t height);

    /** Resize the offscreen target to the given geometry. */
    bool resize_offscreen(std::uint32_t width, std::uint32_t height);

    /** Copy a D3D11 texture into the present target device-side and
     *  present it.
     *
     *  @param texture  The source texture.
     *  @param width    The source width (sizes / resizes the offscreen
     *                  target).
     *  @param height   The source height.
     *  @param castable True when the source is exactly the present
     *                  format (BGRA8), so a plain `CopyResource` is
     *                  legal; otherwise the copy goes through
     *                  `CopySubresourceRegion`, which clips to the
     *                  target. */
    bool present_texture(ID3D11Texture2D* texture,
                         std::uint32_t width,
                         std::uint32_t height,
                         bool castable);

    /** Present whatever is in the back buffer. */
    bool do_present();
#else
    /** The non-Windows stub: the offscreen "present" just counts. */
    bool present_bgra(const std::uint8_t*, std::uint32_t, std::uint32_t) {
        return true;
    }
#endif
};

// ===========================================================================
// The presenter thread
// ===========================================================================

void D3D11Renderer::Impl::start_presenter() {
    if (running_.exchange(true)) return;
    stop_ = false;
    presenter_ = std::thread([this] { present_loop(); });
}

void D3D11Renderer::Impl::stop_presenter() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    if (presenter_.joinable()) presenter_.join();
    running_.store(false);
}

void D3D11Renderer::Impl::present_loop() {
    // The presentation clock: a deadline that advances by one frame
    // interval per presented frame.  The wait is a *timed* wait 鈥?the
    // thread sleeps until the deadline, it does not spin.
    const auto interval = std::chrono::nanoseconds{
        1'000'000'000LL / static_cast<long long>(target_fps())};
    auto next_deadline = Clock::now() + interval;

    // The frame-rate window: the presenter recomputes the rate over a
    // one-second window so the reported figure reflects what the display
    // actually got.
    std::uint64_t window_frames = 0;
    auto window_start = Clock::now();

    for (;;) {
        Slot frame;
        {
            std::unique_lock<std::mutex> lock(mtx_);

            // ---- The presentation clock --------------------------------
            //
            // The presenter sleeps until the deadline.  It does *not*
            // wake early when a frame arrives: waking early is what a
            // spin-wait presenter does, and it makes the frame rate
            // follow the producer instead of the clock.  The frame
            // waits in the mailbox (which is latest-wins, so the frame
            // the presenter eventually shows is the *newest* one) until
            // the deadline arrives.
            //
            // The wait is a *timed* wait 鈥?the thread is parked in the
            // kernel, it does not burn a core.
            cv_.wait_until(lock, next_deadline, [this] { return stop_; });
            if (stop_) return;

            if (!slot_.valid) {
                // The deadline arrived with nothing to show: the clock
                // keeps running, but there is no frame to present.  The
                // next deadline is advanced so the loop cannot spin on
                // an expired deadline.
                next_deadline = Clock::now() + interval;
                continue;
            }
            // Take the frame out of the slot.  The slot becomes free,
            // so the producer can post the next one immediately.
            frame = std::move(slot_);
            slot_ = Slot{};
        }

        // ---- The A/V-sync hold (v1.3 Q6 / remote-desktop plan R3) ----
        //
        // When the render clock decided the picture was *ahead* of the
        // sound, it asked for the frame to be held back until the sound
        // catches up.  `present_at_us` is the instant the hold ends 鈥?a
        // *local monotonic* deadline, so it is comparable with
        // `Clock::now()` right here.
        //
        // The wait is a *timed* wait on the same condition variable the
        // stop path uses, so a shutdown does not have to wait out the
        // hold.
        if (frame.present_at_us > 0) {
            const auto deadline = Clock::time_point{
                std::chrono::microseconds(frame.present_at_us)};
            std::unique_lock<std::mutex> lock(mtx_);
            if (cv_.wait_until(lock, deadline, [this] { return stop_; })) {
                // The presenter was asked to stop while it waited.
                return;
            }
        }

        const bool ok = present_one(frame);
        if (ok) {
            frames_presented_.fetch_add(1, std::memory_order_relaxed);
            ++window_frames;

            const auto now = Clock::now();
            const auto window_us =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    now - window_start).count();
            if (window_us >= 1'000'000) {
                const auto fps = (static_cast<double>(window_frames) * 1e6) /
                                 static_cast<double>(window_us);
                present_fps_.store(static_cast<std::uint32_t>(fps + 0.5),
                                   std::memory_order_relaxed);
                window_frames = 0;
                window_start = now;
            }
        } else {
            render_errors_.fetch_add(1, std::memory_order_relaxed);
        }

        // Advance the presentation clock.  If the present took longer
        // than the interval (a slow GPU, a vsync wait), the deadline is
        // reset to *now* rather than accumulating a debt 鈥?a renderer
        // that falls behind must not try to "catch up" by presenting a
        // burst of frames.
        const auto after = Clock::now();
        next_deadline += interval;
        if (next_deadline < after) next_deadline = after + interval;
    }
}

bool D3D11Renderer::Impl::present_one(const Slot& slot) {
    if (!slot.valid) return false;
    if (slot.surface) {
#if defined(_WIN32)
        // The GPU path.  The mailbox holds a reference on the surface,
        // so the texture is alive here even though the producer's
        // `render()` call returned long ago.
        auto* texture =
            static_cast<ID3D11Texture2D*>(slot.surface->handle().handle);
        if (!texture) return false;
        const std::uint32_t w  = slot.surface->width();
        const std::uint32_t ht = slot.surface->height();
        if (w == 0 || ht == 0) return false;
        const bool ok = present_texture(
            texture, w, ht,
            slot.surface->format() == plugins::VideoPixelFormat::kBGRA);
        if (ok) gpu_copies_.fetch_add(1, std::memory_order_relaxed);
        return ok;
#else
        return true;
#endif
    }
    if (slot.pixels.empty() || slot.width == 0 || slot.height == 0) {
        return false;
    }
#if defined(_WIN32)
    return present_bgra(slot.pixels.data(), slot.width, slot.height);
#else
    return true;
#endif
}

// ===========================================================================
// The Windows D3D11 implementation
// ===========================================================================

#if defined(_WIN32)

bool D3D11Renderer::Impl::create_d3d_objects() {
    // ---- The device -------------------------------------------------------
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &device_, nullptr, &context_);
    if (FAILED(hr) || !device_) {
        NIMRTC_LOG_ERROR("d3d11 renderer: D3D11CreateDevice failed (hr=0x"
                         << std::hex << hr << std::dec << ")");
        return false;
    }

    // ---- The output surface ----------------------------------------------
    //
    // A swap chain needs a window.  The host may have supplied one; when
    // it did not (and the sink is not forced offscreen) the renderer
    // builds its own viewport window, mirroring the EGL renderer's
    // self-created surface (v1-video-render-engine D2 / ADR-030 decision
    // three 鈥?the engine never touches the window system).  When even
    // that fails (no display device, a headless CI session) the renderer
    // falls through to the offscreen target below, exactly as before:
    // the presentation clock and the drop accounting still work, which
    // is what makes the frame-rate assertion testable without a display.
    const bool want_swap_chain =
        !cfg_.offscreen &&
        (cfg_.native_window != nullptr || create_self_window());

    if (want_swap_chain) {
        HWND hwnd = cfg_.native_window != nullptr
            ? static_cast<HWND>(cfg_.native_window)
            : self_hwnd_;
        RECT rect{};
        if (!::GetClientRect(hwnd, &rect)) {
            NIMRTC_LOG_ERROR("d3d11 renderer: GetClientRect failed");
            destroy_d3d_objects();
            return false;
        }
        const auto w = static_cast<std::uint32_t>(
            std::max<LONG>(rect.right - rect.left, 1));
        const auto h = static_cast<std::uint32_t>(
            std::max<LONG>(rect.bottom - rect.top, 1));

        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width  = w;
        desc.BufferDesc.Height = h;
        desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BufferDesc.RefreshRate.Numerator   = 0;
        desc.BufferDesc.RefreshRate.Denominator = 1;
        // A swap chain needs at least one back buffer; the zero that
        // `DXGI_SWAP_CHAIN_DESC{}` leaves behind is not a valid count,
        // and DXGI rejects the whole chain for it.
        desc.BufferCount = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = hwnd;
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc.Flags = 0;

        // `D3D11CreateDeviceAndSwapChain` takes the *device* as an output
        // parameter, so the device created above is released first 鈥?the
        // swap-chain factory creates its own device (and its own context),
        // and handing it an already-populated pointer would leak the
        // first one.
        if (context_) { context_->Release(); context_ = nullptr; }
        if (device_)  { device_->Release();  device_  = nullptr; }

        D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &desc, &swap_chain_, &device_,
            &feature_level, &context_);
        if (FAILED(hr) || !swap_chain_) {
            NIMRTC_LOG_ERROR("d3d11 renderer: D3D11CreateDeviceAndSwapChain "
                             "failed (hr=0x" << std::hex << hr << std::dec << ")");
            destroy_d3d_objects();
            return false;
        }

        hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer_));
        if (FAILED(hr) || !back_buffer_) {
            NIMRTC_LOG_ERROR("d3d11 renderer: GetBuffer failed");
            destroy_d3d_objects();
            return false;
        }
        hr = device_->CreateRenderTargetView(back_buffer_, nullptr, &rtv_);
        if (FAILED(hr) || !rtv_) {
            NIMRTC_LOG_ERROR("d3d11 renderer: CreateRenderTargetView failed");
            destroy_d3d_objects();
            return false;
        }
        back_buffer_w_ = w;
        back_buffer_h_ = h;
        NIMRTC_LOG_INFO("d3d11 renderer: swap chain created ("
                        << w << "x" << h << ")");
    } else {
        // The offscreen target.  Its size follows the configuration; the
        // renderer resizes it when a frame of a different size arrives.
        const std::uint32_t w = cfg_.width  > 0 ? cfg_.width  : 1280;
        const std::uint32_t h = cfg_.height > 0 ? cfg_.height : 720;

        if (!resize_offscreen(w, h)) {
            destroy_d3d_objects();
            return false;
        }
        NIMRTC_LOG_INFO("d3d11 renderer: offscreen target created ("
                        << w << "x" << h << ")");
    }
    return true;
}

bool D3D11Renderer::Impl::resize_offscreen(std::uint32_t width,
                                            std::uint32_t height) {
    if (!device_) return false;
    if (width == 0 || height == 0) return false;

    if (offscreen_rtv_) { offscreen_rtv_->Release(); offscreen_rtv_ = nullptr; }
    if (offscreen_)      { offscreen_->Release();      offscreen_ = nullptr; }

    D3D11_TEXTURE2D_DESC td{};
    td.Width          = width;
    td.Height         = height;
    td.MipLevels       = 1;
    td.ArraySize       = 1;
    td.Format          = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage           = D3D11_USAGE_DEFAULT;
    td.BindFlags       = D3D11_BIND_RENDER_TARGET;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &offscreen_))) {
        offscreen_ = nullptr;
        return false;
    }
    if (FAILED(device_->CreateRenderTargetView(offscreen_, nullptr,
                                                &offscreen_rtv_))) {
        offscreen_rtv_ = nullptr;
        offscreen_->Release();
        offscreen_ = nullptr;
        return false;
    }
    offscreen_w_ = width;
    offscreen_h_ = height;
    return true;
}

bool D3D11Renderer::Impl::create_self_window() {
    // The viewport window is *opt-in*.  A renderer that pops up a visible
    // window on its own initiative is a surprise, and a test run that
    // flashes a window on the developer's desktop 鈥?once per test case,
    // for as long as the suite runs 鈥?is the worst version of that
    // surprise.  The caller has to ask for it.
    if (!cfg_.create_viewport_window) {
        return false;
    }

    // The window class is registered once per process; a duplicate
    // registration is the normal second-caller case, not an error.
    constexpr const wchar_t* kClassName = L"NimRTCD3D11Viewport";
    static const bool s_class_registered = []() {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = ::GetModuleHandleW(nullptr);
        wc.hCursor       = ::LoadCursorW(
            nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        wc.lpszClassName = kClassName;
        return ::RegisterClassExW(&wc) != 0 ||
               ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    (void)s_class_registered;

    const int w = static_cast<int>(cfg_.width  > 0 ? cfg_.width  : 1280);
    const int h = static_cast<int>(cfg_.height > 0 ? cfg_.height : 720);
    // WS_EX_TOOLWINDOW keeps the viewport out of the taskbar and out of
    // the Alt-Tab list: it is the renderer's own scratch surface, not
    // something the user is meant to interact with.
    self_hwnd_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"NimRTC",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, w, h,
        nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (self_hwnd_ == nullptr) {
        NIMRTC_LOG_ERROR("d3d11 renderer: CreateWindowExW failed (err="
                         << ::GetLastError() << ") 鈥?falling back to "
                         "offscreen");
        return false;
    }
    // Show without activating, so the viewport does not steal focus from
    // the host's own windows.
    ::ShowWindow(self_hwnd_, SW_SHOWNOACTIVATE);
    NIMRTC_LOG_INFO("d3d11 renderer: created a viewport window ("
                    << w << "x" << h << ")");
    return true;
}

void D3D11Renderer::Impl::destroy_d3d_objects() {
    if (upload_)      { upload_->Release();      upload_ = nullptr; }
    if (offscreen_rtv_){ offscreen_rtv_->Release(); offscreen_rtv_ = nullptr; }
    if (offscreen_)   { offscreen_->Release();   offscreen_ = nullptr; }
    if (rtv_)         { rtv_->Release();         rtv_ = nullptr; }
    if (back_buffer_) { back_buffer_->Release(); back_buffer_ = nullptr; }
    if (swap_chain_)  { swap_chain_->Release();  swap_chain_ = nullptr; }
    if (context_)     { context_->Release();     context_ = nullptr; }
    if (device_)      { device_->Release();      device_ = nullptr; }
    // The self-created viewport is destroyed after the swap chain that
    // presented into it (the swap chain is released above).
    if (self_hwnd_)   { ::DestroyWindow(self_hwnd_); self_hwnd_ = nullptr; }
}

bool D3D11Renderer::Impl::do_present() {
    if (swap_chain_) {
        // `Present(0)` 鈥?the NON-BLOCKING flag.  The present call never
        // waits for the vertical blank; the *presentation clock* in the
        // presenter thread is what paces the frames.  (A blocking
        // `Present(1)` would serialise the renderer against the display
        // refresh and make the frame rate depend on the monitor.)
        const HRESULT hr = swap_chain_->Present(0, 0);
        if (hr == DXGI_STATUS_OCCLUDED) {
            // The window is occluded: the frame was not shown, but
            // nothing failed.  This is not an error.
            return true;
        }
        if (FAILED(hr)) {
            NIMRTC_LOG_ERROR("d3d11 renderer: Present failed (hr=0x"
                             << std::hex << hr << std::dec << ")");
            return false;
        }
        return true;
    }
    // The offscreen mode: there is no swap chain to present to, but the
    // frame was still "presented" as far as the clock is concerned.
    return true;
}

bool D3D11Renderer::Impl::present_bgra(const std::uint8_t* pixels,
                                       std::uint32_t width,
                                       std::uint32_t height) {
    if (!device_ || !context_) return false;

    // ---- The present target ------------------------------------------------
    //
    // In the offscreen mode the target is a plain texture whose size we
    // control, so it follows the frame.  With a swap chain the target
    // is the back buffer, whose size is fixed by the window 鈥?the copy
    // is then clipped to the intersection.
    if (offscreen_ &&
        (offscreen_w_ != width || offscreen_h_ != height)) {
        if (!resize_offscreen(width, height)) return false;
    }

    ID3D11Texture2D* target = offscreen_ ? offscreen_ : back_buffer_;
    ID3D11RenderTargetView* target_rtv =
        offscreen_ ? offscreen_rtv_ : rtv_;
    if (!target || !target_rtv) return false;

    const std::uint32_t target_w = offscreen_ ? offscreen_w_ : back_buffer_w_;
    const std::uint32_t target_h = offscreen_ ? offscreen_h_ : back_buffer_h_;
    const std::uint32_t copy_w = std::min(width, target_w);
    const std::uint32_t copy_h = std::min(height, target_h);
    if (copy_w == 0 || copy_h == 0) return false;

    // ---- The upload texture ------------------------------------------------
    //
    // It is created on demand and resized when the frame's geometry
    // changes.
    if (!upload_ || upload_w_ != width || upload_h_ != height) {
        if (upload_) { upload_->Release(); upload_ = nullptr; }
        D3D11_TEXTURE2D_DESC td{};
        td.Width          = width;
        td.Height         = height;
        td.MipLevels       = 1;
        td.ArraySize       = 1;
        td.Format          = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage           = D3D11_USAGE_DYNAMIC;
        td.BindFlags       = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags  = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &upload_))) {
            upload_ = nullptr;
            return false;
        }
        upload_w_ = width;
        upload_h_ = height;
    }

    // Map the upload texture and copy the packed BGRA into it.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = context_->Map(upload_, 0, D3D11_MAP_WRITE_DISCARD, 0,
                                      &mapped);
    if (FAILED(hr)) {
        render_errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4u;
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(static_cast<std::uint8_t*>(mapped.pData) +
                        static_cast<std::ptrdiff_t>(row) * mapped.RowPitch,
                    pixels + static_cast<std::ptrdiff_t>(row) * row_bytes,
                    row_bytes);
    }
    context_->Unmap(upload_, 0);

    // Clear the target to black *first*, then copy the frame over it.
    // (Clearing after the copy would erase the frame again.)
    const FLOAT clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    context_->OMSetRenderTargets(1, &target_rtv, nullptr);
    context_->ClearRenderTargetView(target_rtv, clear_color);

    // The device-side copy: the source is the upload texture, the
    // destination is the present target.  The copy region is clipped to
    // the smaller of the two geometries.
    D3D11_BOX box{};
    box.left = 0;
    box.top = 0;
    box.right  = copy_w;
    box.bottom = copy_h;
    box.back = 1;
    box.front = 0;
    context_->CopySubresourceRegion(target, 0, 0, 0, 0, upload_, 0, &box);

    return do_present();
}

bool D3D11Renderer::Impl::present_texture(ID3D11Texture2D* texture,
                                           std::uint32_t width,
                                           std::uint32_t height,
                                           bool castable) {
    if (!device_ || !context_ || !texture) return false;

    // A surface of a different geometry resizes the offscreen target,
    // exactly as the CPU path does.  (A swap chain's back buffer is the
    // window's, so it keeps its size and the copy is clipped.)
    if (offscreen_ && (offscreen_w_ != width || offscreen_h_ != height)) {
        if (!resize_offscreen(width, height)) return false;
    }

    ID3D11Texture2D* target = offscreen_ ? offscreen_ : back_buffer_;
    ID3D11RenderTargetView* target_rtv =
        offscreen_ ? offscreen_rtv_ : rtv_;
    if (!target || !target_rtv) return false;

    // Clear the target to black *first*, then copy the frame over it.
    const FLOAT clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    context_->OMSetRenderTargets(1, &target_rtv, nullptr);
    context_->ClearRenderTargetView(target_rtv, clear_color);

    // The device-side copy: the source texture goes straight into the
    // present target.  No host copy happens on this path 鈥?that is the
    // whole point of the GPU delivery route (plan E1 / E5).
    //
    // `CopyResource` requires the two textures to agree on format and
    // geometry, so it is the fast path for a BGRA8 surface in the
    // offscreen mode; anything else is clipped through
    // `CopySubresourceRegion`, still device-to-device.
    if (castable && offscreen_) {
        context_->CopyResource(target, texture);
    } else {
        D3D11_BOX box{};
        box.left = 0;
        box.top = 0;
        box.right  = std::min(width,  offscreen_ ? offscreen_w_ : back_buffer_w_);
        box.bottom = std::min(height, offscreen_ ? offscreen_h_ : back_buffer_h_);
        box.back = 1;
        box.front = 0;
        context_->CopySubresourceRegion(target, 0, 0, 0, 0, texture, 0, &box);
    }

    return do_present();
}

#endif  // _WIN32

// ===========================================================================
// D3D11Renderer
// ===========================================================================

D3D11Renderer::D3D11Renderer(plugins::VideoSinkConfig cfg) noexcept
    : impl_(std::make_unique<Impl>(std::move(cfg))) {}

D3D11Renderer::~D3D11Renderer() {
    if (impl_) impl_->stop_presenter();
}

const char* D3D11Renderer::name() const noexcept {
    return "nimrtc::video_sink::D3D11Renderer";
}

plugins::Status D3D11Renderer::open() noexcept {
#if defined(_WIN32)
    if (!impl_->create_d3d_objects()) {
        impl_->destroy_d3d_objects();
        return plugins::kErrHardwareError;
    }
    impl_->start_presenter();
    return plugins::kOk;
#else
    // The seam still exists on a non-Windows build so a configuration
    // that names it resolves 鈥?but the renderer reports honestly that it
    // cannot run here rather than pretending to present.
    NIMRTC_LOG_ERROR("d3d11 renderer: this build has no D3D11 support");
    return plugins::kErrUnsupported;
#endif
}

void D3D11Renderer::close() noexcept {
    impl_->stop_presenter();
#if defined(_WIN32)
    impl_->destroy_d3d_objects();
#endif
}

plugins::Status D3D11Renderer::render(
    const plugins::VideoSourceFrame& frame) noexcept {
    if (!impl_ || !impl_->running_.load(std::memory_order_acquire)) {
        return plugins::kErrNotReady;
    }
    if (frame.width == 0 || frame.height == 0 || !frame.plane_y) {
        impl_->render_errors_.fetch_add(1, std::memory_order_relaxed);
        return plugins::kErrInvalidParam;
    }

    // The source frame's format.  A packed BGRA frame is uploaded
    // as-is; an I420 / NV12 frame is converted to BGRA first (the
    // present target's format) 鈥?that is the sink's one format
    // conversion.
    const bool source_is_bgra =
        frame.format == plugins::VideoPixelFormat::kBGRA ||
        frame.format == plugins::VideoPixelFormat::kUnknown;

    std::vector<std::uint8_t> bgra;
    const std::uint8_t* src = frame.plane_y;
    std::int32_t src_stride = frame.stride_y > 0
        ? frame.stride_y
        : static_cast<std::int32_t>(frame.width * 4u);

    if (!source_is_bgra) {
        // I420 鈫?BGRA through the shared helper.
        const auto layout = video_frame::compute_layout(
            plugins::VideoPixelFormat::kBGRA, frame.width, frame.height);
        bgra.resize(layout.buffer_size);
        if (!frame.plane_u || !frame.plane_v) {
            impl_->render_errors_.fetch_add(1, std::memory_order_relaxed);
            return plugins::kErrInvalidParam;
        }
        const std::int32_t stride_c = frame.stride_u > 0
            ? frame.stride_u
            : static_cast<std::int32_t>((frame.width + 1u) / 2u);
        video_frame::convert_i420_to_bgra(
            frame.width, frame.height,
            frame.plane_y, frame.stride_y > 0 ? frame.stride_y
                                              : static_cast<std::int32_t>(frame.width),
            frame.plane_u, stride_c,
            frame.plane_v, stride_c,
            bgra.data(), layout.strides[0]);
        impl_->format_conversions_.fetch_add(1, std::memory_order_relaxed);
        src = bgra.data();
        src_stride = layout.strides[0];
    }

    // ---- Post the frame to the mailbox (latest-wins) ----------------------
    {
        std::lock_guard<std::mutex> lock(impl_->mtx_);
        if (impl_->slot_.valid) {
            // The previous frame has not been presented yet: it is
            // dropped in favour of the newer one.  The count is what
            // proves the back-pressure policy works.
            impl_->frames_dropped_present_.fetch_add(1,
                                                      std::memory_order_relaxed);
            impl_->frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        impl_->slot_.pixels.assign(src, src + static_cast<std::ptrdiff_t>(
                                                  src_stride) * frame.height);
        impl_->slot_.surface.reset();
        impl_->slot_.width         = frame.width;
        impl_->slot_.height        = frame.height;
        impl_->slot_.capture_ts_us = frame.capture_ts_us;
        impl_->slot_.present_at_us = frame.present_at_us;
        impl_->slot_.frame_seq     = frame.frame_seq;
        impl_->slot_.rtp_timestamp = frame.rtp_timestamp;
        impl_->slot_.valid         = true;
    }
    impl_->cv_.notify_all();

    impl_->frames_rendered_.fetch_add(1, std::memory_order_relaxed);

    const auto now_us = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count());
    impl_->last_render_age_us_.store(now_us - frame.capture_ts_us,
                                      std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(impl_->info_mutex_);
        impl_->last_info_.capture_ts_us = frame.capture_ts_us;
        impl_->last_info_.frame_seq     = frame.frame_seq;
        impl_->last_info_.rtp_timestamp = frame.rtp_timestamp;
        impl_->last_info_.present_at_us = frame.present_at_us;
        impl_->last_info_.render_time_us = now_us;
    }
    return plugins::kOk;
}

plugins::Status D3D11Renderer::render(
    const plugins::GpuBuffer& surface) noexcept {
#if defined(_WIN32)
    if (!impl_ || !impl_->running_.load(std::memory_order_acquire)) {
        return plugins::kErrNotReady;
    }
    if (!surface.has_gpu_handle()) {
        impl_->gpu_surfaces_refused_.fetch_add(1, std::memory_order_relaxed);
        return plugins::kErrInvalidParam;
    }
    if (surface.handle().backend != plugins::GpuBackend::kWindowsD3D11Texture) {
        impl_->gpu_surfaces_refused_.fetch_add(1, std::memory_order_relaxed);
        return plugins::kErrUnsupported;
    }
    auto* texture = static_cast<ID3D11Texture2D*>(surface.handle().handle);
    if (!texture || surface.width() == 0 || surface.height() == 0) {
        impl_->gpu_surfaces_refused_.fetch_add(1, std::memory_order_relaxed);
        return plugins::kErrInvalidParam;
    }

    // ---- The documented fallback (plan E5) --------------------------------
    //
    // A surface that is not the present format (BGRA8) cannot go into the
    // present target with a device-to-device copy, and this module carries
    // no format-converting shader.  If the surface has a CPU mirror, that
    // mirror *is* the picture: present it through the CPU path.  If it has
    // none, there is nothing to present 鈥?say so (`kErrUnsupported`), so
    // the engine's render-path rule decodes to host pixels next time, and
    // count the fallback instead of pretending the frame was shown.
    if (surface.format() != plugins::VideoPixelFormat::kBGRA) {
        impl_->gpu_staging_readbacks_.fetch_add(1, std::memory_order_relaxed);
        if (!surface.has_cpu_mirror()) {
            return plugins::kErrUnsupported;
        }
        plugins::VideoSourceFrame f{};
        f.width   = surface.width();
        f.height  = surface.height();
        f.format  = surface.format();
        f.plane_y = surface.cpu_y();
        f.stride_y = surface.stride_y();
        if (is_planar_yuv_420(surface.format())) {
            f.plane_u  = surface.cpu_u();
            f.plane_v  = surface.cpu_v();
            f.stride_u = surface.stride_u();
            f.stride_v = surface.stride_v();
        }
        return render(f);
    }

    // ---- The GPU path -----------------------------------------------------
    //
    // The frame goes into the *same* latest-wins mailbox as a CPU frame,
    // and the presenter thread does the device-side copy on its own
    // clock.  Doing the copy here instead would (a) block the producer on
    // the GPU and (b) present outside the presentation clock, so GPU and
    // CPU frames mixed on one sink would double the frame rate and
    // corrupt the drop accounting (plan E5).
    //
    // The mailbox takes its own reference: `surface` is borrowed for the
    // duration of this call (the seam's lifetime contract) and the
    // presenter needs it later.
    {
        std::lock_guard<std::mutex> lock(impl_->mtx_);
        if (impl_->slot_.valid) {
            impl_->frames_dropped_present_.fetch_add(1,
                                                      std::memory_order_relaxed);
            impl_->frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        impl_->slot_.pixels.clear();
        impl_->slot_.surface       =
            std::make_shared<plugins::GpuBuffer>(
                surface.handle(), surface.width(), surface.height(),
                surface.format());
        impl_->slot_.width         = surface.width();
        impl_->slot_.height        = surface.height();
        impl_->slot_.capture_ts_us = 0;
        impl_->slot_.present_at_us = 0;
        impl_->slot_.frame_seq     = 0;
        impl_->slot_.rtp_timestamp = 0;
        impl_->slot_.valid         = true;
    }
    impl_->cv_.notify_all();

    impl_->frames_rendered_.fetch_add(1, std::memory_order_relaxed);
    return plugins::kOk;
#else
    (void)surface;
    impl_->gpu_surfaces_refused_.fetch_add(1, std::memory_order_relaxed);
    return plugins::kErrUnsupported;
#endif
}

plugins::Status D3D11Renderer::render(
    const plugins::GpuBuffer& surface,
    const plugins::VideoFrameInfo& info) noexcept {
    const plugins::Status rc = render(surface);
    if (rc != plugins::kOk) return rc;
    {
        // The A/V-sync hold (v1.3 Q6): the render clock's presentation
        // deadline travels with the frame, so the presenter waits for it
        // before it puts the picture on the screen.
        std::lock_guard<std::mutex> lock(impl_->mtx_);
        impl_->slot_.present_at_us = info.present_at_us;
    }
    std::lock_guard<std::mutex> lock(impl_->info_mutex_);
    impl_->last_info_.capture_ts_us = info.capture_ts_us;
    impl_->last_info_.frame_seq     = info.frame_seq;
    impl_->last_info_.rtp_timestamp = info.rtp_timestamp;
    impl_->last_info_.present_at_us = info.present_at_us;
    impl_->last_info_.render_time_us = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count());
    return plugins::kOk;
}

bool D3D11Renderer::supports_gpu_render() const noexcept {
    return true;
}

bool D3D11Renderer::supports_cpu_render() const noexcept {
    return true;
}

plugins::VideoSinkStats D3D11Renderer::stats() const noexcept {
    plugins::VideoSinkStats s;
    s.frames_rendered = impl_->frames_rendered_.load(std::memory_order_relaxed);
    s.frames_dropped  = impl_->frames_dropped_.load(std::memory_order_relaxed);
    s.format_conversions =
        impl_->format_conversions_.load(std::memory_order_relaxed);
    s.render_errors   = impl_->render_errors_.load(std::memory_order_relaxed);
    s.last_render_age_us =
        static_cast<std::uint64_t>(impl_->last_render_age_us_.load(
            std::memory_order_relaxed));
    s.frames_dropped_present =
        impl_->frames_dropped_present_.load(std::memory_order_relaxed);
    s.frames_presented =
        impl_->frames_presented_.load(std::memory_order_relaxed);
    s.present_fps = impl_->present_fps_.load(std::memory_order_relaxed);
    // GPU-path accounting (plan E5): the split between these counters is
    // what tells a host whether the fast path is taken on its hardware.
    s.gpu_device_copies =
        impl_->gpu_copies_.load(std::memory_order_relaxed);
    s.gpu_staging_fallbacks =
        impl_->gpu_staging_readbacks_.load(std::memory_order_relaxed);
    s.gpu_surfaces_refused =
        impl_->gpu_surfaces_refused_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(impl_->mtx_);
        s.presenter_queue_depth = impl_->slot_.valid ? 1u : 0u;
    }
    return s;
}

plugins::VideoSinkConfig D3D11Renderer::config() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->cfg_mutex_);
    return impl_->cfg_;
}

plugins::Status D3D11Renderer::set_config(
        plugins::VideoSinkConfig cfg) noexcept {
    if (!impl_) return plugins::kErrNotReady;

    // The presentation geometry is a *runtime* property: the viewer
    // resizes the window, and the picture has to follow (plan E7).  The
    // offscreen target therefore follows the new size; the swap chain
    // cannot be resized here (its size is the window's), so the new
    // geometry only takes effect there through the window itself.
    std::lock_guard<std::mutex> lock(impl_->cfg_mutex_);
    impl_->cfg_ = std::move(cfg);

#if defined(_WIN32)
    // The offscreen target follows the new geometry.  A failure is not
    // fatal: the presenter resizes the target when the next frame of a
    // different size arrives anyway.
    if (impl_->device_ != nullptr && impl_->cfg_.width > 0 &&
        impl_->cfg_.height > 0) {
        (void)impl_->resize_offscreen(impl_->cfg_.width, impl_->cfg_.height);
    }
#endif
    return plugins::kOk;
}

const plugins::VideoFrameInfo* D3D11Renderer::current_frame_info() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->info_mutex_);
    return &impl_->last_info_;
}

bool D3D11Renderer::is_hardware_accelerated() const noexcept {
    return true;
}

std::string_view D3D11Renderer::hardware_backend() const noexcept {
    return "d3d11";
}

// ===========================================================================
// The factory
// ===========================================================================

std::string_view D3D11RendererFactory::id() const noexcept {
    return "d3d11";
}

std::string_view D3D11RendererFactory::display_name() const noexcept {
    return "Windows D3D11 / DXGI swap-chain renderer";
}

plugins::IVideoSink* D3D11RendererFactory::create(
    plugins::VideoSinkConfig cfg) const {
    return new D3D11Renderer(std::move(cfg));
}

} // namespace nimrtc::video_sink
