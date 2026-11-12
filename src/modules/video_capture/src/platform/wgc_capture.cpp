// modules/video_capture/src/platform/wgc_capture.cpp
// ----------------------------------------------------------------------------
// The Windows Graphics Capture device (remote-desktop plan A3).
//
// This is the *window* (and cursor-faithful display) capture path on
// Windows.  It is a thin adapter over the portable capture layer: it
// fills the shared bounded queue with BGRA frames and it does nothing
// else.  All the shared machinery (the queue, the drop policy, the
// format negotiation, the cursor layer, the capture -> frame-stream
// adapter) lives in the portable layer and is shared with the macOS
// and Linux backends.
//
// The one thing WGC does that DXGI does not is composite the pointer
// into the frame itself, so the backend reports
// `cursor_composited = true` and the engine skips the cursor layer on
// this path.
//
// See include/nimrtc/video_capture/wgc_capture.hpp for the contract.
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/wgc_capture.hpp>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <thread>
#include <utility>

#if defined(_WIN32)
#  define NIMRTC_WGC_CAPTURE 1
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <inspectable.h>
#  include <roapi.h>
#  include <winstring.h>
#  include <windows.graphics.capture.interop.h>
#  include <d3d11.h>
#  include <dxgi1_2.h>
#  include <windows.graphics.directx.direct3d11.interop.h>

// The C++/WinRT projection.  These headers ship with the Windows SDK
// (they are on MSVC's default include path as <winrt/...>), so they
// add no build-time dependency beyond the SDK itself.
#  include <winrt/base.h>
#  include <winrt/Windows.Foundation.h>
#  include <winrt/Windows.Graphics.Capture.h>
#  include <winrt/Windows.Graphics.DirectX.h>
#  include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#endif

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/video_capture/capture_queue.hpp>

namespace nimrtc::video_capture {

#if defined(NIMRTC_WGC_CAPTURE)

namespace {

using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

/** The number of buffers the frame pool holds.  Two is the smallest
 *  number that keeps the compositor from stalling on the consumer. */
constexpr int kPoolBufferCount = 2;

/** The class name of the WGC capture item's activation factory. */
constexpr wchar_t kCaptureItemClass[] =
    L"Windows.Graphics.Capture.GraphicsCaptureItem";

/** Convert a wide string to a narrow one (best effort — the display
 *  name is a diagnostic, not an identity). */
std::string wide_to_narrow(const wchar_t* w) {
    if (w == nullptr || *w == 0) return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0,
                                             nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), needed, nullptr,
                          nullptr);
    return out;
}

/** One display, and the handle WGC needs to capture it. */
struct MonitorEntry {
    plugins::CaptureDevice device;
    HMONITOR handle = nullptr;
};

/**
 * @brief Enumerate the displays, as WGC sees them.
 *
 * WGC has no enumeration of its own — a capture item is created for a
 * monitor handle or a window handle.  The monitor list therefore comes
 * from `EnumDisplayMonitors`, which is the Win32 way of saying "the
 * displays".
 */
void enumerate_monitors(std::vector<MonitorEntry>& out) {
    struct Ctx {
        std::vector<MonitorEntry>* out;
    } ctx{&out};

    ::EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR mon, HDC, LPRECT, LPARAM param) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(param);

            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (::GetMonitorInfoW(mon, &info) == 0) return TRUE;

            MonitorEntry e;
            e.handle = mon;
            e.device.id = std::string{"wgc:monitor:"} +
                          std::to_string(static_cast<unsigned long>(
                              c->out->size()));
            e.device.name = wide_to_narrow(info.szDevice);
            if (e.device.name.empty()) e.device.name = e.device.id;
            e.device.kind = plugins::CaptureDeviceKind::kScreen;

            // The monitor's rectangle is in virtual-desktop
            // coordinates; its size is what the frame will be.
            e.device.max_width = static_cast<std::uint32_t>(
                std::max(0L, info.rcMonitor.right - info.rcMonitor.left));
            e.device.max_height = static_cast<std::uint32_t>(
                std::max(0L, info.rcMonitor.bottom - info.rcMonitor.top));
            e.device.max_fps = 60;
            c->out->push_back(std::move(e));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
}

/** Enumerate the top-level windows, which are WGC's window targets.
 *
 * Only windows that are *visible* and have a *title* are offered: a
 * window without a title is almost always a helper window, and a
 * hidden one is not capturable anyway. */
void enumerate_windows(std::vector<plugins::CaptureDevice>& out) {
    struct Ctx {
        std::vector<plugins::CaptureDevice>* out;
    } ctx{&out};

    ::EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(param);
            if (::IsWindowVisible(hwnd) == FALSE) return TRUE;

            wchar_t title[256]{};
            const int len = ::GetWindowTextW(hwnd, title,
                                             static_cast<int>(std::size(title)));
            if (len <= 0) return TRUE;

            RECT rect{};
            if (::GetWindowRect(hwnd, &rect) == FALSE) return TRUE;
            const LONG w = rect.right - rect.left;
            const LONG h = rect.bottom - rect.top;
            if (w <= 0 || h <= 0) return TRUE;

            plugins::CaptureDevice d;
            d.id = std::string{"wgc:window:"} +
                   std::to_string(reinterpret_cast<std::uintptr_t>(hwnd));
            d.name = wide_to_narrow(title);
            if (d.name.empty()) d.name = d.id;
            d.kind = plugins::CaptureDeviceKind::kWindow;
            d.max_width  = static_cast<std::uint32_t>(w);
            d.max_height = static_cast<std::uint32_t>(h);
            d.max_fps = 60;
            c->out->push_back(std::move(d));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
}

/** The size of a window, in pixels. */
bool window_size(HWND hwnd, UINT& width, UINT& height) {
    RECT rect{};
    if (::GetWindowRect(hwnd, &rect) == FALSE) return false;
    const LONG w = rect.right - rect.left;
    const LONG h = rect.bottom - rect.top;
    if (w <= 0 || h <= 0) return false;
    width  = static_cast<UINT>(w);
    height = static_cast<UINT>(h);
    return true;
}

/** The HWND a `"wgc:window:<handle>"` id names, or nullptr. */
HWND window_from_id(const std::string& id) {
    const std::string prefix = "wgc:window:";
    if (id.rfind(prefix, 0) != 0) return nullptr;
    const std::string suffix = id.substr(prefix.size());
    if (suffix.empty()) return nullptr;
    std::uintptr_t raw = 0;
    try {
        raw = static_cast<std::uintptr_t>(std::stoull(suffix));
    } catch (...) {
        return nullptr;
    }
    HWND hwnd = reinterpret_cast<HWND>(raw);
    if (::IsWindow(hwnd) == FALSE) return nullptr;
    return hwnd;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// The platform state (PIMPL)
// ---------------------------------------------------------------------------

/**
 * @brief The WGC objects the device owns, plus the capture thread.
 *
 * Kept out of the public header so that the header stays free of the
 * Windows SDK (and so that the class exists on every platform).
 *
 * The WinRT objects are held here rather than in the public header, so
 * the C++/WinRT types never leak into it.
 */
class WgcCaptureState {
public:
    /** The D3D11 device and its immediate context.  Created *without*
     *  `D3D11_CREATE_DEVICE_SINGLETHREADED`, because the readback runs
     *  on the frame-arrival thread while the capture thread owns the
     *  session. */
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    /** The staging texture the GPU->CPU readback goes through. */
    ID3D11Texture2D* staging_ = nullptr;
    UINT staging_width_  = 0;
    UINT staging_height_ = 0;

    /** The source size — the size of the surface the compositor
     *  delivers.  The frame pool is created at this size so that the
     *  consumer sees the source's pixels and not a resampling of them;
     *  a requested region is cropped out of the readback afterwards. */
    UINT source_width_  = 0;
    UINT source_height_ = 0;

    /** What to capture.  Exactly one of the two is set. */
    HMONITOR monitor_ = nullptr;
    HWND window_ = nullptr;

    /** The frame pool.  It is a `shared_ptr` because the frame-arrival
     *  handler runs on a WinRT thread-pool thread and the capture
     *  thread tears the pool down: the shared ownership keeps the pool
     *  alive for a handler that is already running. */
    std::shared_ptr<Direct3D11CaptureFramePool> pool_;

    /** The bounded frame queue the capture thread fills. */
    CaptureQueue queue_{2};

    /** The capture thread. */
    std::thread thread_;

    /** Set to false to ask the capture thread to stop. */
    std::atomic<bool> running_{false};

    /** The number of frame-arrival handlers currently running.  The
     *  teardown waits for it to reach zero before the D3D objects are
     *  released. */
    std::atomic<int> in_flight_{0};

    ~WgcCaptureState() {
        stop_thread();
        release_d3d();
    }

    void stop_thread() noexcept {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

    void release_d3d() noexcept {
        if (staging_ != nullptr) {
            staging_->Release();
            staging_ = nullptr;
        }
        if (device_ != nullptr) {
            device_->Release();
            device_ = nullptr;
        }
        if (context_ != nullptr) {
            context_->Release();
            context_ = nullptr;
        }
    }
};

// ---------------------------------------------------------------------------
// WgcCapture
// ---------------------------------------------------------------------------

WgcCapture::WgcCapture(plugins::CaptureConfig cfg) noexcept
    : cfg_(std::move(cfg)) {}

WgcCapture::~WgcCapture() {
    close();
}

bool WgcCapture::available() noexcept {
    return true;
}

plugins::Status WgcCapture::open() noexcept {
    if (open_) return plugins::kOk;

    // ---- Resolve the device ------------------------------------------------
    //
    // WGC has no enumeration of its own: a capture item is created for a
    // monitor handle or a window handle.  The ids below are the ones
    // `enumerate_devices()` produces.
    auto state = std::make_unique<WgcCaptureState>();

    if (!cfg_.device_id.empty()) {
        HWND window = window_from_id(cfg_.device_id);
        if (window != nullptr) {
            state->window_ = window;
        } else {
            std::vector<MonitorEntry> monitors;
            enumerate_monitors(monitors);
            bool found = false;
            for (const auto& m : monitors) {
                if (m.device.id != cfg_.device_id) continue;
                state->monitor_ = m.handle;
                found = true;
                break;
            }
            if (!found) {
                core::log::Logger::instance().error(
                    "wgc: '" + cfg_.device_id + "' is not a WGC device id");
                return plugins::kErrInvalidParam;
            }
        }
    } else {
        // The default is the primary display: a monitor capture needs
        // no consent, so it is the one that always works.
        std::vector<MonitorEntry> monitors;
        enumerate_monitors(monitors);
        if (monitors.empty()) {
            core::log::Logger::instance().error("wgc: no display to capture");
            return plugins::kErrHardwareError;
        }
        state->monitor_ = monitors.front().handle;
        cfg_.device_id = monitors.front().device.id;
    }

    // ---- The source size ---------------------------------------------------
    //
    // The pool is created at the *source* size, so that the pixels the
    // consumer sees are the source's pixels; a requested region is
    // cropped out of the readback rather than scaled by the compositor.
    if (state->window_ != nullptr) {
        if (!window_size(state->window_, state->source_width_,
                         state->source_height_)) {
            return plugins::kErrInvalidParam;
        }
    } else {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (::GetMonitorInfoW(state->monitor_, &mi) == 0) {
            return plugins::kErrHardwareError;
        }
        state->source_width_ = static_cast<UINT>(
            std::max(0L, mi.rcMonitor.right - mi.rcMonitor.left));
        state->source_height_ = static_cast<UINT>(
            std::max(0L, mi.rcMonitor.bottom - mi.rcMonitor.top));
    }
    if (state->source_width_ == 0 || state->source_height_ == 0) {
        return plugins::kErrInvalidParam;
    }

    // ---- Negotiate the format ----------------------------------------------
    cfg_.format.width  = state->source_width_;
    cfg_.format.height = state->source_height_;
    if (cfg_.format.fps == 0) cfg_.format.fps = 60;
    cfg_.format.tag = "bgra";
    cfg_.format.zero_copy = false;
    // WGC does *not* report per-frame changed regions: the compositor
    // hands over a whole surface every time.  The dirty metadata is
    // therefore "the whole frame changed", which is the conservative
    // answer the seam allows.
    cfg_.format.has_dirty_rects = false;

    description_ = (state->window_ != nullptr) ? "window" : "display";

    // ---- Configure the queue ------------------------------------------------
    state->queue_.set_drop_policy(cfg_.drop_policy);
    if (cfg_.queue_depth > 0) state->queue_.set_depth(cfg_.queue_depth);

    const std::size_t bytes = static_cast<std::size_t>(state->source_width_) *
                              static_cast<std::size_t>(state->source_height_) *
                              4u;
    scratch_.assign(bytes, 0u);

    frame_index_ = 0;
    state_ = std::move(state);
    state_->running_.store(true, std::memory_order_release);
    state_->thread_ = std::thread([this]() { capture_loop(); });

    open_ = true;
    return plugins::kOk;
}

void WgcCapture::close() noexcept {
    if (!open_) return;
    open_ = false;
    if (state_) {
        // Ask the capture thread to stop and wait for it, so the WGC
        // objects are not released underneath it.
        state_->stop_thread();
        // A frame-arrival handler may still be running on the WinRT
        // thread-pool thread.  Wait for it: it uses the D3D objects
        // this call is about to release.
        while (state_->in_flight_.load(std::memory_order_acquire) > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        state_->pool_.reset();
        state_->release_d3d();
    }
    state_.reset();
    scratch_.clear();
    scratch_.shrink_to_fit();
    region_scratch_.clear();
    region_scratch_.shrink_to_fit();
}

std::vector<plugins::CaptureDevice>
WgcCapture::enumerate_devices() const noexcept {
    std::vector<plugins::CaptureDevice> devices;
    std::vector<MonitorEntry> monitors;
    enumerate_monitors(monitors);
    for (auto& m : monitors) devices.push_back(std::move(m.device));
    enumerate_windows(devices);
    return devices;
}

std::vector<plugins::CaptureFormat>
WgcCapture::supported_formats(const std::string& device_id) const noexcept {
    std::vector<plugins::CaptureDevice> devices;
    devices = enumerate_devices();

    std::vector<plugins::CaptureFormat> out;
    for (const auto& d : devices) {
        if (!device_id.empty() && d.id != device_id) continue;
        plugins::CaptureFormat f;
        f.width  = d.max_width;
        f.height = d.max_height;
        f.fps    = d.max_fps;
        f.tag    = "bgra";
        f.zero_copy = false;
        // The compositor hands over a whole surface every time.
        f.has_dirty_rects = false;
        out.push_back(std::move(f));
    }
    return out;
}

plugins::CaptureCapabilities
WgcCapture::capabilities() const noexcept {
    plugins::CaptureCapabilities c;
    // The frame arrives as a GPU texture, so the CPU path needs a
    // readback: this is not the zero-copy path.
    c.zero_copy = false;
    // WGC does not report changed regions.
    c.dirty_rects = false;
    // This is the whole point of WGC: the OS composites the pointer
    // into the frame, so the cursor layer is not needed on this path.
    c.cursor_composited = true;
    // The backend crops a region out of the readback itself.
    c.region_selection = true;
    // The backend enumerates displays and windows.
    c.enumeration = true;
    c.max_pixels = 0;
    c.backend = "wgc-capture";
    return c;
}

void WgcCapture::capture_loop() noexcept {
    if (!state_) return;

    // WinRT needs an apartment.  MTA is the right one here: the frame
    // arrives on a thread-pool thread, and the capture thread only
    // drives the session.
    const HRESULT co_hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(co_hr)) {
        core::log::Logger::instance().error("wgc: CoInitializeEx failed");
        device_errors_.fetch_add(1, std::memory_order_relaxed);
        state_->running_.store(false, std::memory_order_release);
        return;
    }

    // ---- The D3D11 device --------------------------------------------------
    //
    // Created *without* D3D11_CREATE_DEVICE_SINGLETHREADED: the
    // readback happens on the frame-arrival thread while this thread
    // owns the session.
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                        D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    const HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
        static_cast<UINT>(sizeof(levels) / sizeof(levels[0])),
        D3D11_SDK_VERSION, &state_->device_, nullptr, &state_->context_);
    if (FAILED(hr) || state_->device_ == nullptr) {
        core::log::Logger::instance().error("wgc: D3D11CreateDevice failed");
        device_errors_.fetch_add(1, std::memory_order_relaxed);
        state_->running_.store(false, std::memory_order_release);
        ::CoUninitialize();
        return;
    }

    // ---- The WinRT Direct3D11 device ---------------------------------------
    IDXGIDevice* dxgi_device = nullptr;
    if (FAILED(state_->device_->QueryInterface(
            __uuidof(IDXGIDevice),
            reinterpret_cast<void**>(&dxgi_device))) ||
        dxgi_device == nullptr) {
        device_errors_.fetch_add(1, std::memory_order_relaxed);
        state_->running_.store(false, std::memory_order_release);
        ::CoUninitialize();
        return;
    }

    IInspectable* raw_device = nullptr;
    const HRESULT dev_hr =
        ::CreateDirect3D11DeviceFromDXGIDevice(dxgi_device, &raw_device);
    dxgi_device->Release();
    if (FAILED(dev_hr) || raw_device == nullptr) {
        core::log::Logger::instance().error(
            "wgc: CreateDirect3D11DeviceFromDXGIDevice failed");
        device_errors_.fetch_add(1, std::memory_order_relaxed);
        state_->running_.store(false, std::memory_order_release);
        ::CoUninitialize();
        return;
    }

    winrt::Windows::Foundation::IInspectable inspectable(
        raw_device, winrt::take_ownership_from_abi);
    auto rt_device = inspectable.as<IDirect3DDevice>();

    // ---- The frame pool ----------------------------------------------------
    //
    // CreateFreeThreaded needs no DispatcherQueue on this thread, which
    // matters because a *library* must not impose a message loop on its
    // host.  Its cost is that the FrameArrived event is raised on a
    // thread-pool thread — which the multithreaded D3D11 device above
    // already accommodates.
    const UINT src_w = state_->source_width_;
    const UINT src_h = state_->source_height_;

    std::shared_ptr<Direct3D11CaptureFramePool> pool;
    try {
        pool = std::make_shared<Direct3D11CaptureFramePool>(
            Direct3D11CaptureFramePool::CreateFreeThreaded(
                rt_device, DirectXPixelFormat::B8G8R8A8UIntNormalized,
                kPoolBufferCount,
                winrt::Windows::Graphics::SizeInt32{
                    static_cast<int32_t>(src_w), static_cast<int32_t>(src_h)}));
    } catch (const winrt::hresult_error&) {
        core::log::Logger::instance().error(
            "wgc: the frame pool could not be created (this build of "
            "Windows may not support free-threaded capture)");
        device_errors_.fetch_add(1, std::memory_order_relaxed);
        state_->running_.store(false, std::memory_order_release);
        ::CoUninitialize();
        return;
    }
    state_->pool_ = pool;

    // ---- The capture item --------------------------------------------------
    //
    // The interop factory is obtained through RoGetActivationFactory:
    // it is the *only* way to create a capture item for a monitor or a
    // window, and it is what makes the window path possible at all.
    IGraphicsCaptureItemInterop* interop = nullptr;
    HSTRING hcls = nullptr;
    ::WindowsCreateString(kCaptureItemClass,
                          static_cast<UINT32>(std::size(kCaptureItemClass) - 1),
                          &hcls);
    const HRESULT factory_hr = ::RoGetActivationFactory(
        hcls, __uuidof(IGraphicsCaptureItemInterop),
        reinterpret_cast<void**>(&interop));
    ::WindowsDeleteString(hcls);
    if (FAILED(factory_hr) || interop == nullptr) {
        core::log::Logger::instance().error(
            "wgc: the GraphicsCaptureItem factory is not available");
        device_errors_.fetch_add(1, std::memory_order_relaxed);
        state_->running_.store(false, std::memory_order_release);
        ::CoUninitialize();
        return;
    }

    GraphicsCaptureItem item = nullptr;
    HRESULT item_hr = E_FAIL;
    if (state_->window_ != nullptr) {
        item_hr = interop->CreateForWindow(
            state_->window_, winrt::guid_of<GraphicsCaptureItem>(),
            winrt::put_abi(item));
    } else {
        item_hr = interop->CreateForMonitor(
            state_->monitor_, winrt::guid_of<GraphicsCaptureItem>(),
            winrt::put_abi(item));
    }
    interop->Release();

    if (FAILED(item_hr) || item == nullptr) {
        core::log::Logger::instance().error(
            "wgc: the capture item could not be created (the user may not "
            "have granted the capture capability)");
        device_errors_.fetch_add(1, std::memory_order_relaxed);
        state_->running_.store(false, std::memory_order_release);
        ::CoUninitialize();
        return;
    }

    // ---- The session --------------------------------------------------------
    GraphicsCaptureSession session = nullptr;
    try {
        session = pool->CreateCaptureSession(item);
    } catch (const winrt::hresult_error&) {
        core::log::Logger::instance().error(
            "wgc: the capture session could not be created");
        device_errors_.fetch_add(1, std::memory_order_relaxed);
        state_->running_.store(false, std::memory_order_release);
        ::CoUninitialize();
        return;
    }

    // The pointer is composited by the OS into the captured surface —
    // this is the reason to choose WGC over DXGI.
    try {
        session.IsCursorCaptureEnabled(cfg_.want_cursor);
    } catch (const winrt::hresult_error&) {
        // The interface is optional; an older Windows simply keeps the
        // default (cursor off).
    }

    // Windows 11 24H2 and later draw a border around a captured window.
    // A remote-desktop client does not want that in the stream.
    if (auto s3 = session.try_as<IGraphicsCaptureSession3>()) {
        try {
            s3.IsBorderRequired(false);
        } catch (const winrt::hresult_error&) {
            // Best effort.
        }
    }

    // ---- The staging texture ------------------------------------------------
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width            = src_w;
        desc.Height           = src_h;
        desc.MipLevels         = 1;
        desc.ArraySize         = 1;
        desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage            = D3D11_USAGE_STAGING;
        desc.BindFlags        = 0;
        desc.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
        if (FAILED(state_->device_->CreateTexture2D(&desc, nullptr,
                                                    &state_->staging_))) {
            core::log::Logger::instance().error(
                "wgc: the staging texture could not be created");
            device_errors_.fetch_add(1, std::memory_order_relaxed);
            state_->running_.store(false, std::memory_order_release);
            ::CoUninitialize();
            return;
        }
        state_->staging_width_  = desc.Width;
        state_->staging_height_ = desc.Height;
    }

    // ---- The frame subscription ----------------------------------------------
    //
    // The handler runs on a WinRT thread-pool thread.  It does the
    // readback and the push; it must not block.
    auto handler = [this](Direct3D11CaptureFramePool const&,
                           winrt::Windows::Foundation::IInspectable const&) {
        on_frame_arrived();
    };
    winrt::event_token token{};
    try {
        token = pool->FrameArrived(handler);
    } catch (const winrt::hresult_error&) {
        core::log::Logger::instance().error(
            "wgc: the FrameArrived subscription failed");
        device_errors_.fetch_add(1, std::memory_order_relaxed);
        state_->running_.store(false, std::memory_order_release);
        ::CoUninitialize();
        return;
    }

    // ---- Start -----------------------------------------------------------------
    try {
        session.StartCapture();
    } catch (const winrt::hresult_error&) {
        core::log::Logger::instance().error("wgc: StartCapture failed");
        device_errors_.fetch_add(1, std::memory_order_relaxed);
        pool->FrameArrived(token);
        state_->running_.store(false, std::memory_order_release);
        ::CoUninitialize();
        return;
    }

    // ---- Wait --------------------------------------------------------------------
    //
    // The capture thread's job is done once the session is started: the
    // frames arrive on the thread-pool thread.  It waits here so that
    // `close()` can join it and the objects stay alive for as long as
    // the session runs.
    while (state_->running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // ---- Tear down ------------------------------------------------------------------
    try {
        session.Close();
    } catch (const winrt::hresult_error&) {
        // Best effort.
    }
    pool->FrameArrived(token);

    ::CoUninitialize();
}

void WgcCapture::on_frame_arrived() noexcept {
    if (!state_) return;

    // Keep the pool alive for the duration of this handler, even if the
    // capture thread tears it down concurrently.
    std::shared_ptr<Direct3D11CaptureFramePool> pool = state_->pool_;
    if (!pool) return;

    state_->in_flight_.fetch_add(1, std::memory_order_acq_rel);
    struct InFlightGuard {
        std::atomic<int>* counter;
        ~InFlightGuard() { counter->fetch_sub(1, std::memory_order_acq_rel); }
    } guard{&state_->in_flight_};

    if (!state_->device_ || !state_->context_) return;

    Direct3D11CaptureFrame frame = nullptr;
    try {
        frame = pool->TryGetNextFrame();
    } catch (const winrt::hresult_error&) {
        device_errors_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!frame) return;

    // ---- The surface -> the D3D11 texture ----------------------------------------
    ID3D11Texture2D* texture = nullptr;
    {
        auto surface = frame.Surface();
        ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess*
            access = nullptr;
        const HRESULT qi = winrt::get_unknown(surface)->QueryInterface(
            __uuidof(::Windows::Graphics::DirectX::Direct3D11::
                         IDirect3DDxgiInterfaceAccess),
            reinterpret_cast<void**>(&access));
        if (FAILED(qi) || access == nullptr) {
            device_errors_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const HRESULT gi = access->GetInterface(
            __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&texture));
        access->Release();
        if (FAILED(gi) || texture == nullptr) {
            device_errors_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    // ---- The readback ------------------------------------------------------------
    //
    // The staging texture is sized for the *source*; if the compositor
    // handed over a differently sized surface (a window that was
    // resized), the staging texture has to follow.
    if (desc.Width != state_->staging_width_ ||
        desc.Height != state_->staging_height_) {
        D3D11_TEXTURE2D_DESC sdesc{};
        sdesc.Width            = desc.Width;
        sdesc.Height           = desc.Height;
        sdesc.MipLevels         = 1;
        sdesc.ArraySize         = 1;
        sdesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        sdesc.SampleDesc.Count = 1;
        sdesc.Usage            = D3D11_USAGE_STAGING;
        sdesc.BindFlags        = 0;
        sdesc.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* fresh = nullptr;
        if (FAILED(state_->device_->CreateTexture2D(&sdesc, nullptr,
                                                    &fresh))) {
            texture->Release();
            device_errors_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (state_->staging_ != nullptr) state_->staging_->Release();
        state_->staging_ = fresh;
        state_->staging_width_  = desc.Width;
        state_->staging_height_ = desc.Height;
    }

    state_->context_->CopyResource(state_->staging_, texture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT map_hr = state_->context_->Map(
        state_->staging_, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(map_hr)) {
        texture->Release();
        device_errors_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // The mapped rows are padded; the scratch buffer is packed.  This
    // is the one memcpy the CPU path needs.
    const std::size_t row_bytes = static_cast<std::size_t>(desc.Width) * 4u;
    const std::size_t needed =
        row_bytes * static_cast<std::size_t>(desc.Height);
    if (scratch_.size() < needed) scratch_.assign(needed, 0u);
    for (UINT row = 0; row < desc.Height; ++row) {
        std::memcpy(scratch_.data() +
                        static_cast<std::size_t>(row) * row_bytes,
                    static_cast<const std::uint8_t*>(mapped.pData) +
                        static_cast<std::size_t>(row) * mapped.RowPitch,
                    row_bytes);
    }
    state_->context_->Unmap(state_->staging_, 0);
    texture->Release();
    bytes_copied_.fetch_add(static_cast<std::uint64_t>(needed),
                             std::memory_order_relaxed);

    // ---- The dirty rectangles ------------------------------------------------------
    //
    // WGC does not report them: the compositor hands over a whole
    // surface every time.  "The whole frame changed" is the
    // conservative answer the seam allows.
    plugins::CaptureDirtyRects dirty;
    dirty.full_frame = true;

    // ---- Crop to the requested region ------------------------------------------------
    const bool has_region = cfg_.region.width != 0 && cfg_.region.height != 0;
    const std::uint8_t* pixels = scratch_.data();
    std::uint32_t frame_w = desc.Width;
    std::uint32_t frame_h = desc.Height;
    std::int32_t stride = static_cast<std::int32_t>(row_bytes);

    if (has_region) {
        const std::uint32_t rx = std::min(cfg_.region.x, desc.Width);
        const std::uint32_t ry = std::min(cfg_.region.y, desc.Height);
        const std::uint32_t rw = std::min(cfg_.region.width, desc.Width - rx);
        const std::uint32_t rh =
            std::min(cfg_.region.height, desc.Height - ry);
        if (rw == 0 || rh == 0) return;

        const std::size_t region_row = static_cast<std::size_t>(rw) * 4u;
        const std::size_t region_bytes =
            region_row * static_cast<std::size_t>(rh);
        if (region_scratch_.size() != region_bytes) {
            region_scratch_.assign(region_bytes, 0u);
        }
        for (std::uint32_t row = 0; row < rh; ++row) {
            std::memcpy(region_scratch_.data() +
                            static_cast<std::size_t>(row) * region_row,
                        scratch_.data() +
                            (static_cast<std::size_t>(ry + row) * desc.Width +
                             rx) * 4u,
                        region_row);
        }
        pixels = region_scratch_.data();
        frame_w = rw;
        frame_h = rh;
        stride = static_cast<std::int32_t>(region_row);
    }

    // ---- Queue the frame ---------------------------------------------------------------
    const std::int64_t ts = static_cast<std::int64_t>(::GetTickCount64() * 1000);
    const std::uint32_t seq = frame_index_++;
    const std::size_t frame_bytes = static_cast<std::size_t>(stride) *
                                   static_cast<std::size_t>(frame_h);
    const auto outcome = state_->queue_.push(
        pixels, frame_bytes, stride, frame_w, frame_h, ts, seq, dirty);
    switch (outcome) {
        case CaptureQueue::PushResult::kQueued:
            frames_captured_.fetch_add(1, std::memory_order_relaxed);
            break;
        case CaptureQueue::PushResult::kDroppedOldest:
            frames_captured_.fetch_add(1, std::memory_order_relaxed);
            frames_dropped_.fetch_add(1, std::memory_order_relaxed);
            break;
        case CaptureQueue::PushResult::kRejected:
            frames_dropped_.fetch_add(1, std::memory_order_relaxed);
            break;
    }

    if (cb_) {
        plugins::CaptureFrame view;
        view.pixels = pixels;
        view.size = frame_bytes;
        view.stride = stride;
        view.width = frame_w;
        view.height = frame_h;
        view.capture_ts_us = ts;
        view.frame_seq = seq;
        view.dirty = dirty;
        view.has_gpu_handle = false;
        cb_(view);
    }
}

plugins::Status WgcCapture::pop_frame(plugins::CaptureFrame& out) noexcept {
    if (!open_ || !state_) return plugins::kErrNotReady;
    // The frame-arrival thread filled the queue; the engine's tick
    // thread only takes what is already there.  It never waits.
    if (!state_->queue_.pop(out)) {
        return plugins::kErrNotReady;
    }
    return plugins::kOk;
}

plugins::CaptureStats WgcCapture::stats() const noexcept {
    plugins::CaptureStats s;
    s.frames_captured = frames_captured_.load(std::memory_order_relaxed);
    s.frames_dropped  = frames_dropped_.load(std::memory_order_relaxed);
    s.device_errors   = device_errors_.load(std::memory_order_relaxed);
    s.bytes_copied    = bytes_copied_.load(std::memory_order_relaxed);
    return s;
}

#else // !NIMRTC_WGC_CAPTURE

// ---------------------------------------------------------------------------
// The non-Windows stub
// ---------------------------------------------------------------------------

class WgcCaptureState {};

WgcCapture::WgcCapture(plugins::CaptureConfig cfg) noexcept
    : cfg_(std::move(cfg)) {}

WgcCapture::~WgcCapture() = default;

bool WgcCapture::available() noexcept {
    return false;
}

plugins::Status WgcCapture::open() noexcept {
    // WGC is a Windows API.  On any other host this device simply does
    // not exist, and saying so is clearer than pretending.
    return plugins::kErrUnsupported;
}

void WgcCapture::close() noexcept {}

std::vector<plugins::CaptureDevice>
WgcCapture::enumerate_devices() const noexcept {
    return {};
}

std::vector<plugins::CaptureFormat>
WgcCapture::supported_formats(const std::string&) const noexcept {
    return {};
}

plugins::CaptureCapabilities WgcCapture::capabilities() const noexcept {
    plugins::CaptureCapabilities c;
    c.backend = "wgc-capture";
    return c;
}

void WgcCapture::capture_loop() noexcept {}

void WgcCapture::on_frame_arrived() noexcept {}

plugins::Status WgcCapture::pop_frame(plugins::CaptureFrame&) noexcept {
    return plugins::kErrUnsupported;
}

plugins::CaptureStats WgcCapture::stats() const noexcept {
    return plugins::CaptureStats{};
}

#endif // NIMRTC_WGC_CAPTURE

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void register_wgc_capture() noexcept {
#if defined(NIMRTC_WGC_CAPTURE)
    static const WgcCaptureFactory kFactory{};
    core::PluginRegistry::instance().register_video_capture(kFactory.id(),
                                                            &kFactory);
#endif
}

} // namespace nimrtc::video_capture
