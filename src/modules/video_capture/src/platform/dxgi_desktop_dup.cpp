// modules/video_capture/src/platform/dxgi_desktop_dup.cpp
// ----------------------------------------------------------------------------
// The Windows DXGI Desktop Duplication capture device (remote-desktop
// plan A3).
//
// This is the *display* capture path on Windows.  It is a thin adapter
// over the portable capture layer: it fills the shared bounded queue
// with BGRA frames and the dirty rectangles DXGI reports, and it does
// nothing else.  All the shared machinery (the queue, the drop policy,
// the format negotiation, the cursor layer, the capture -> frame-stream
// adapter) lives in the portable layer and is shared with the macOS and
// Linux backends.
//
// See include/nimrtc/video_capture/dxgi_desktop_dup.hpp for the contract.
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/dxgi_desktop_dup.hpp>

#include <algorithm>
#include <cstring>
#include <thread>
#include <utility>

#if defined(_WIN32)
#  define NIMRTC_DXGI_CAPTURE 1
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <d3d11.h>
#  include <dxgi1_2.h>
#  include <wrl/client.h>
#endif

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/video_capture/capture_queue.hpp>

namespace nimrtc::video_capture {

#if defined(NIMRTC_DXGI_CAPTURE)

namespace {

using Microsoft::WRL::ComPtr;

/** How long `AcquireNextFrame` waits before reporting "nothing
 *  changed".  A short timeout keeps the thread responsive to the
 *  device being closed. */
constexpr UINT kAcquireTimeoutMs = 200;

/** The output index used when the caller did not name a device. */
constexpr UINT kDefaultOutputIndex = 0;

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

} // anonymous namespace

// ---------------------------------------------------------------------------
// The platform state (PIMPL)
// ---------------------------------------------------------------------------

/**
 * @brief The DXGI objects the device owns, plus the capture thread.
 *
 * Kept out of the public header so that the header stays free of the
 * Windows SDK (and so that the class exists on every platform).
 */
class DxgiDuplicationState {
public:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<IDXGIOutput> output_;
    ComPtr<IDXGIOutputDuplication> duplication_;

    /** The staging texture the GPU->CPU readback goes through. */
    ComPtr<ID3D11Texture2D> staging_;

    /** The staging texture's size — it has to be recreated when the
     *  desktop's resolution changes. */
    UINT staging_width_  = 0;
    UINT staging_height_ = 0;

    /** The output's mode, used to size the staging texture. */
    UINT out_width_  = 0;
    UINT out_height_ = 0;

    /** The output's place in the virtual desktop.  The pointer is
     *  reported in virtual-desktop coordinates, so the cursor layer
     *  needs this to translate it into the captured output's own space
     *  (plan A6). */
    LONG origin_x_ = 0;
    LONG origin_y_ = 0;

    /** The bounded frame queue the capture thread fills. */
    CaptureQueue queue_{2};

    /** The capture thread. */
    std::thread thread_;

    /** Set to false to ask the capture thread to stop. */
    std::atomic<bool> running_{false};

    ~DxgiDuplicationState() {
        stop_thread();
    }

    void stop_thread() noexcept {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }
};

namespace {

/**
 * @brief Read an output's current desktop area and refresh rate.
 *
 * `IDXGIOutput` has no `GetModeDesc`; the current mode comes from
 * `GetDisplayModeList`, and the *desktop* area (which is what a
 * duplicated frame covers) comes from `GetDesc`.  The mode that
 * matches the desktop area is the current one.
 *
 * Two API details this function has to respect:
 *
 *   - `GetDisplayModeList` takes **four** arguments — the format, the
 *     flags, the mode count (in/out) and the mode array.  The count is
 *     an in/out parameter: the first call passes `nullptr` for the
 *     array and the callee writes the number of modes into it; the
 *     second call passes the array.
 *   - `DXGI_MODE_DESC` has **no** `DesktopCoordinates` member.  The
 *     mode's position on the desktop is not part of the descriptor; the
 *     *mode list* is already scoped to the output, so every mode in it
 *     belongs to that output.  The desktop area therefore comes from
 *     `GetDesc` alone, and the mode list is only consulted for the
 *     refresh rate.
 */
bool query_output_mode(IDXGIOutput* output,
                       UINT& width, UINT& height, UINT& fps,
                       LONG& origin_x, LONG& origin_y) {
    DXGI_OUTPUT_DESC desc{};
    if (FAILED(output->GetDesc(&desc))) return false;

    width  = static_cast<UINT>(desc.DesktopCoordinates.right -
                               desc.DesktopCoordinates.left);
    height = static_cast<UINT>(desc.DesktopCoordinates.bottom -
                               desc.DesktopCoordinates.top);

    // The output's place in the *virtual desktop*.  The pointer is
    // reported in virtual-desktop coordinates, so a capture of the
    // second monitor needs this to translate the pointer into the
    // captured display's own space (plan A6).
    origin_x = desc.DesktopCoordinates.left;
    origin_y = desc.DesktopCoordinates.top;

    // The refresh rate: the *highest* rate the output advertises is the
    // one the desktop compositor runs at (the list is sorted by the
    // driver, but taking the maximum is the safe reading).
    UINT count = 0;
    if (FAILED(output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM,
                                           0, &count, nullptr))) {
        return false;
    }
    if (count == 0) return false;

    std::vector<DXGI_MODE_DESC> modes(count);
    if (FAILED(output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM,
                                           0, &count, modes.data()))) {
        return false;
    }

    fps = 0;
    for (UINT i = 0; i < count; ++i) {
        const DXGI_MODE_DESC& m = modes[i];
        const UINT denom =
            m.RefreshRate.Denominator == 0 ? 1u : m.RefreshRate.Denominator;
        const UINT hz = m.RefreshRate.Numerator / denom;
        if (hz > fps) fps = hz;
    }
    if (fps == 0) fps = 60;
    return true;
}

/** Enumerate the display adapters and their outputs. */
bool enumerate_outputs(std::vector<plugins::CaptureDevice>& devices) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT ai = 0;
         factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
        ComPtr<IDXGIOutput> output;
        for (UINT oi = 0;
             adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; ++oi) {
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc))) continue;

            UINT w = 0;
            UINT h = 0;
            UINT fps = 0;
            LONG ox = 0;
            LONG oy = 0;
            if (!query_output_mode(output.Get(), w, h, fps, ox, oy)) continue;

            plugins::CaptureDevice d;
            d.id = std::string{"dxgi:"} +
                   std::to_string(static_cast<unsigned long>(ai)) + ":" +
                   std::to_string(static_cast<unsigned long>(oi));

            const std::string dev_name = wide_to_narrow(desc.DeviceName);
            d.name = dev_name.empty() ? d.id : dev_name;

            d.kind  = plugins::CaptureDeviceKind::kScreen;
            d.max_width  = w;
            d.max_height = h;
            d.max_fps    = fps;
            devices.push_back(std::move(d));
        }
    }
    return !devices.empty();
}

/** Parse "dxgi:<adapter>:<output>" into the two indices. */
bool parse_device_id(const std::string& id, UINT& adapter, UINT& output) {
    if (id.rfind("dxgi:", 0) != 0) return false;
    const std::size_t colon = id.find(':', 5);
    if (colon == std::string::npos) return false;
    try {
        adapter = static_cast<UINT>(std::stoul(id.substr(5, colon - 5)));
        output  = static_cast<UINT>(std::stoul(id.substr(colon + 1)));
    } catch (...) {
        return false;
    }
    return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// DxgiDesktopDupCapture
// ---------------------------------------------------------------------------

DxgiDesktopDupCapture::DxgiDesktopDupCapture(
        plugins::CaptureConfig cfg) noexcept
    : cfg_(std::move(cfg)) {}

DxgiDesktopDupCapture::~DxgiDesktopDupCapture() {
    close();
}

bool DxgiDesktopDupCapture::available() noexcept {
    return true;
}

plugins::Status DxgiDesktopDupCapture::open() noexcept {
    if (open_) return plugins::kOk;

    // ---- Find the output ------------------------------------------------
    std::vector<plugins::CaptureDevice> devices;
    if (!enumerate_outputs(devices)) {
        core::log::Logger::instance().error(
            "dxgi-desktop-dup: no output to duplicate");
        return plugins::kErrHardwareError;
    }

    UINT adapter_index = 0;
    UINT output_index  = kDefaultOutputIndex;
    if (!cfg_.device_id.empty()) {
        if (!parse_device_id(cfg_.device_id, adapter_index, output_index)) {
            core::log::Logger::instance().error(
                "dxgi-desktop-dup: '" + cfg_.device_id +
                "' is not a DXGI device id");
            return plugins::kErrInvalidParam;
        }
    }
    if (adapter_index >= devices.size()) {
        return plugins::kErrInvalidParam;
    }

    // ---- Create the device ----------------------------------------------
    auto state = std::make_unique<DxgiDuplicationState>();

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return plugins::kErrHardwareError;
    }

    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(adapter_index, &adapter))) {
        return plugins::kErrHardwareError;
    }

    // The duplication object needs a device that was created with
    // D3D11_CREATE_DEVICE_BGRA_SUPPORT, because the desktop
    // compositor's texture is a BGRA texture.
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                        D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0,
                                        D3D_FEATURE_LEVEL_9_3};
    const HRESULT hr = D3D11CreateDevice(
        adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
        static_cast<UINT>(sizeof(levels) / sizeof(levels[0])),
        D3D11_SDK_VERSION, &state->device_, nullptr, &state->context_);
    if (FAILED(hr)) {
        core::log::Logger::instance().error(
            "dxgi-desktop-dup: D3D11CreateDevice failed");
        return plugins::kErrHardwareError;
    }

    ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(output_index, &output))) {
        return plugins::kErrInvalidParam;
    }

    UINT out_w = 0;
    UINT out_h = 0;
    UINT out_fps = 0;
    LONG out_origin_x = 0;
    LONG out_origin_y = 0;
    if (!query_output_mode(output.Get(), out_w, out_h, out_fps,
                           out_origin_x, out_origin_y)) {
        return plugins::kErrHardwareError;
    }
    state->out_width_  = out_w;
    state->out_height_ = out_h;
    state->origin_x_   = out_origin_x;
    state->origin_y_   = out_origin_y;

    // ---- Create the duplication object ----------------------------------
    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output.As(&output1))) {
        return plugins::kErrHardwareError;
    }
    if (FAILED(output1->DuplicateOutput(state->device_.Get(),
                                         &state->duplication_))) {
        // DXGI_ERROR_ACCESS_LOST is the common one: too many
        // duplication objects are alive (another app is capturing the
        // same display).  It is a *resource* problem, not a bug.
        core::log::Logger::instance().error(
            "dxgi-desktop-dup: DuplicateOutput failed (another application "
            "may already be capturing this display)");
        return plugins::kErrHardwareError;
    }

    // ---- Negotiate the format -------------------------------------------
    //
    // The desktop compositor always delivers the *display's*
    // resolution, so the device reports that: the requested size is
    // honoured by the region (a sub-region of the display), and any
    // further scaling is the scaler's job (plan C1).
    cfg_.device_id = devices[adapter_index].id;
    cfg_.format.width  = state->out_width_;
    cfg_.format.height = state->out_height_;
    if (cfg_.format.fps == 0) {
        cfg_.format.fps = out_fps;
    }
    cfg_.format.tag = "bgra";
    cfg_.format.zero_copy = false;
    cfg_.format.has_dirty_rects = true;

    // The output's place in the virtual desktop.  The pointer is
    // reported in virtual-desktop coordinates, so the cursor layer
    // (plan A6) needs this to translate the pointer into the captured
    // output's own space.
    cfg_.surface_origin_x = static_cast<std::int32_t>(state->origin_x_);
    cfg_.surface_origin_y = static_cast<std::int32_t>(state->origin_y_);

    description_ = devices[adapter_index].name;

    // ---- Create the staging texture -------------------------------------
    //
    // The readback goes through a CPU-readable staging texture.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width            = state->out_width_;
    desc.Height           = state->out_height_;
    desc.MipLevels         = 1;
    desc.ArraySize         = 1;
    desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_STAGING;
    desc.BindFlags        = 0;
    desc.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
    if (FAILED(state->device_->CreateTexture2D(&desc, nullptr,
                                               &state->staging_))) {
        return plugins::kErrHardwareError;
    }
    state->staging_width_  = desc.Width;
    state->staging_height_ = desc.Height;

    const std::size_t bytes = static_cast<std::size_t>(state->out_width_) *
                              static_cast<std::size_t>(state->out_height_) * 4u;
    scratch_.assign(bytes, 0u);

    // ---- Configure the queue --------------------------------------------
    state->queue_.set_drop_policy(cfg_.drop_policy);
    if (cfg_.queue_depth > 0) state->queue_.set_depth(cfg_.queue_depth);

    // ---- Start the capture thread ---------------------------------------
    frame_index_ = 0;
    state_ = std::move(state);
    state_->running_.store(true, std::memory_order_release);
    state_->thread_ = std::thread([this]() { capture_loop(); });

    open_ = true;
    return plugins::kOk;
}

void DxgiDesktopDupCapture::close() noexcept {
    if (!open_) return;
    open_ = false;
    if (state_) {
        // Ask the capture thread to stop and wait for it, so the DXGI
        // objects are not released underneath it.
        state_->stop_thread();
        state_->duplication_.Reset();
        state_->staging_.Reset();
        state_->context_.Reset();
        state_->device_.Reset();
        state_->adapter_.Reset();
        state_->output_.Reset();
    }
    state_.reset();
    scratch_.clear();
    scratch_.shrink_to_fit();
    region_scratch_.clear();
    region_scratch_.shrink_to_fit();
}

std::vector<plugins::CaptureDevice>
DxgiDesktopDupCapture::enumerate_devices() const noexcept {
    std::vector<plugins::CaptureDevice> devices;
    enumerate_outputs(devices);
    return devices;
}

std::vector<plugins::CaptureFormat>
DxgiDesktopDupCapture::supported_formats(
        const std::string& device_id) const noexcept {
    std::vector<plugins::CaptureDevice> devices;
    if (!enumerate_outputs(devices)) return {};

    std::vector<plugins::CaptureFormat> out;
    for (const auto& d : devices) {
        if (!device_id.empty() && d.id != device_id) continue;
        plugins::CaptureFormat f;
        f.width  = d.max_width;
        f.height = d.max_height;
        f.fps    = d.max_fps;
        f.tag    = "bgra";
        f.zero_copy = false;
        f.has_dirty_rects = true;
        out.push_back(std::move(f));
    }
    return out;
}

plugins::CaptureCapabilities
DxgiDesktopDupCapture::capabilities() const noexcept {
    plugins::CaptureCapabilities c;
    // The duplication API hands the frame over as a GPU texture, so the
    // CPU path needs a readback: this is not the zero-copy path.
    c.zero_copy = false;
    // DXGI reports the changed regions natively.
    c.dirty_rects = true;
    // Desktop Duplication does NOT composite the pointer — that is
    // what the cursor layer (plan A6) is for.
    c.cursor_composited = false;
    // The device can capture a sub-region of the display.
    c.region_selection = true;
    c.enumeration = true;
    c.max_pixels = 0;
    c.backend = "dxgi-desktop-dup";
    return c;
}

/**
 * @brief Read the dirty (and move) rectangles of the current frame.
 *
 * DXGI hands the rectangles over in *output* coordinates.  They are
 * clipped against the captured region, because the consumer's frame is
 * the region, not the whole output.
 */
void collect_dirty_rects(IDXGIOutputDuplication* dup,
                          const DXGI_OUTDUPL_FRAME_INFO& info,
                          const plugins::CaptureRegion& region,
                          bool has_region,
                          plugins::CaptureDirtyRects& dirty) {
    // A RECT list is reported in *bytes*.  The first call asks for the
    // size the buffer needs; the second fills it.
    auto read_rects = [&](auto& sink) -> bool {
        UINT needed = 0;
        const HRESULT hr = dup->GetFrameDirtyRects(0, nullptr, &needed);
        if (hr != DXGI_ERROR_MORE_DATA && FAILED(hr)) return false;
        if (needed == 0) return false;
        std::vector<RECT> rects(needed / sizeof(RECT) + 1u);
        UINT used = 0;
        if (FAILED(dup->GetFrameDirtyRects(
                static_cast<UINT>(rects.size() * sizeof(RECT)),
                rects.data(), &used))) {
            return false;
        }
        for (UINT i = 0; i < used / sizeof(RECT); ++i) {
            sink(rects[i]);
        }
        return true;
    };

    // Move rects first: DXGI requires the application to process the
    // move rects before the dirty rects to get an accurate picture.
    // A move rect is a `DXGI_OUTDUPL_MOVE_RECT` (a source and a
    // destination rectangle), not a plain `RECT`.
    auto read_move_rects = [&](auto& sink) -> bool {
        UINT needed = 0;
        const HRESULT hr = dup->GetFrameMoveRects(0, nullptr, &needed);
        if (hr != DXGI_ERROR_MORE_DATA && FAILED(hr)) return false;
        if (needed == 0) return false;
        std::vector<DXGI_OUTDUPL_MOVE_RECT> rects(
            needed / sizeof(DXGI_OUTDUPL_MOVE_RECT) + 1u);
        UINT used = 0;
        if (FAILED(dup->GetFrameMoveRects(
                static_cast<UINT>(rects.size() * sizeof(DXGI_OUTDUPL_MOVE_RECT)),
                rects.data(), &used))) {
            return false;
        }
        for (UINT i = 0; i < used / sizeof(DXGI_OUTDUPL_MOVE_RECT); ++i) {
            // `DXGI_OUTDUPL_MOVE_RECT` carries a `POINT SourcePoint`
            // and a `RECT DestinationRect`: the content that used to
            // be at the source point now lives in the destination
            // rectangle.  The destination is the area whose pixels are
            // new in this frame, so it is the one worth reporting as
            // changed — the source point is a degenerate rect and
            // carries no area at all.
            sink(rects[i].DestinationRect);
        }
        return true;
    };

    // Clip a source-space rectangle against the region and rebase it
    // onto the region's origin.
    auto clip = [&](const RECT& r) {
        LONG x0 = r.left;
        LONG y0 = r.top;
        LONG x1 = r.right;
        LONG y1 = r.bottom;
        if (has_region) {
            x0 = std::max<LONG>(x0, static_cast<LONG>(region.x));
            y0 = std::max<LONG>(y0, static_cast<LONG>(region.y));
            x1 = std::min<LONG>(x1, static_cast<LONG>(region.x + region.width));
            y1 = std::min<LONG>(y1, static_cast<LONG>(region.y + region.height));
        }
        if (x1 <= x0 || y1 <= y0) return;
        plugins::CaptureRegion c;
        // The consumer's frame starts at the region's origin, so the
        // dirty rect is relative to it.
        c.x = static_cast<std::uint32_t>(x0 - (has_region ? region.x : 0));
        c.y = static_cast<std::uint32_t>(y0 - (has_region ? region.y : 0));
        c.width  = static_cast<std::uint32_t>(x1 - x0);
        c.height = static_cast<std::uint32_t>(y1 - y0);
        dirty.rects.push_back(c);
    };

    if (info.AccumulatedFrames != 0) {
        read_move_rects(clip);
        read_rects(clip);
    }
}

void DxgiDesktopDupCapture::capture_loop() noexcept {
    if (!state_) return;

    while (state_->running_.load(std::memory_order_acquire)) {
        // ---- Acquire the next frame ------------------------------------
        ComPtr<IDXGIResource> resource;
        DXGI_OUTDUPL_FRAME_INFO info{};
        IDXGIResource* raw_resource = nullptr;
        const HRESULT hr = state_->duplication_->AcquireNextFrame(
            kAcquireTimeoutMs, &info, &raw_resource);
        if (SUCCEEDED(hr) && raw_resource != nullptr) {
            resource.Attach(raw_resource);
        }
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // Nothing changed on the desktop.  That is the *normal*
            // case for a static screen, and it is not an error.
            continue;
        }
        if (FAILED(hr)) {
            if (hr == DXGI_ERROR_ACCESS_LOST) {
                // The desktop was resized, the display was switched, or
                // the session was locked.  The device is lost: count it
                // and let the host reopen it.
                device_errors_.fetch_add(1, std::memory_order_relaxed);
                core::log::Logger::instance().warn(
                    "dxgi-desktop-dup: the duplication object was lost "
                    "(the display changed?)");
            } else {
                device_errors_.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }

        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(resource.As(&texture))) {
            state_->duplication_->ReleaseFrame();
            device_errors_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // ---- Read the texture back --------------------------------------
        state_->context_->CopyResource(state_->staging_.Get(), texture.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT map_hr = state_->context_->Map(
            state_->staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(map_hr)) {
            state_->duplication_->ReleaseFrame();
            device_errors_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // The mapped rows are padded to 256 bytes; the scratch buffer
        // is packed.  This is the one memcpy the CPU path needs.
        const std::size_t row_bytes =
            static_cast<std::size_t>(state_->out_width_) * 4u;
        const std::size_t src_pitch = mapped.RowPitch;
        for (UINT row = 0; row < state_->out_height_; ++row) {
            std::memcpy(scratch_.data() +
                            static_cast<std::size_t>(row) * row_bytes,
                        static_cast<const std::uint8_t*>(mapped.pData) +
                            static_cast<std::size_t>(row) * src_pitch,
                        row_bytes);
        }
        state_->context_->Unmap(state_->staging_.Get(), 0);
        bytes_copied_.fetch_add(
            static_cast<std::uint64_t>(row_bytes) *
                static_cast<std::uint64_t>(state_->out_height_),
            std::memory_order_relaxed);

        // ---- Collect the dirty rectangles --------------------------------
        //
        // DXGI reports them in the *output's* coordinate space.  They
        // are clipped against the captured region, because the
        // consumer's frame is the region, not the whole output.
        plugins::CaptureDirtyRects dirty;
        dirty.full_frame = false;
        collect_dirty_rects(state_->duplication_.Get(), info, cfg_.region,
                            cfg_.region.width != 0 && cfg_.region.height != 0,
                            dirty);

        state_->duplication_->ReleaseFrame();

        if (dirty.rects.empty()) {
            // No change was reported.  The frame is identical to the
            // last one, so the consumer can skip it entirely.
            continue;
        }

        // ---- Crop to the requested region --------------------------------
        //
        // The region is in *source* (output) pixels.  The consumer's
        // frame is the region, so the pixels are cropped here rather
        // than handed over whole.
        const bool has_region = cfg_.region.width != 0 && cfg_.region.height != 0;
        const std::uint8_t* pixels = scratch_.data();
        std::uint32_t frame_w = state_->out_width_;
        std::uint32_t frame_h = state_->out_height_;
        std::int32_t  stride  = static_cast<std::int32_t>(row_bytes);

        if (has_region) {
            const std::uint32_t rx = std::min(cfg_.region.x, state_->out_width_);
            const std::uint32_t ry = std::min(cfg_.region.y, state_->out_height_);
            const std::uint32_t rw =
                std::min(cfg_.region.width, state_->out_width_ - rx);
            const std::uint32_t rh =
                std::min(cfg_.region.height, state_->out_height_ - ry);
            if (rw == 0 || rh == 0) continue;

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
                                (static_cast<std::size_t>(ry + row) *
                                 state_->out_width_ + rx) * 4u,
                            region_row);
            }
            pixels = region_scratch_.data();
            frame_w = rw;
            frame_h = rh;
            stride = static_cast<std::int32_t>(region_row);
        }

        // ---- Queue the frame ---------------------------------------------
        const std::int64_t ts = static_cast<std::int64_t>(
            info.LastPresentTime.QuadPart / 1000);
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
                // The frame itself is queued, but the oldest one was
                // evicted to make room — that eviction is the drop.
                frames_captured_.fetch_add(1, std::memory_order_relaxed);
                frames_dropped_.fetch_add(1, std::memory_order_relaxed);
                break;
            case CaptureQueue::PushResult::kRejected:
                frames_dropped_.fetch_add(1, std::memory_order_relaxed);
                break;
        }

        if (cb_) {
            // The callback fires with a view of the frame that was just
            // captured.  The queue has already copied the bytes, so the
            // scratch buffer stays valid until the next iteration.
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
}

plugins::Status DxgiDesktopDupCapture::pop_frame(
        plugins::CaptureFrame& out) noexcept {
    if (!open_ || !state_) return plugins::kErrNotReady;
    // The capture thread filled the queue; the engine's tick thread
    // only takes what is already there.  It never waits.
    if (!state_->queue_.pop(out)) {
        return plugins::kErrNotReady;
    }
    return plugins::kOk;
}

plugins::CaptureStats DxgiDesktopDupCapture::stats() const noexcept {
    plugins::CaptureStats s;
    s.frames_captured = frames_captured_.load(std::memory_order_relaxed);
    s.frames_dropped  = frames_dropped_.load(std::memory_order_relaxed);
    s.device_errors   = device_errors_.load(std::memory_order_relaxed);
    s.bytes_copied    = bytes_copied_.load(std::memory_order_relaxed);
    return s;
}

#else // !NIMRTC_DXGI_CAPTURE

// ---------------------------------------------------------------------------
// The non-Windows stub
// ---------------------------------------------------------------------------

class DxgiDuplicationState {};

DxgiDesktopDupCapture::DxgiDesktopDupCapture(
        plugins::CaptureConfig cfg) noexcept
    : cfg_(std::move(cfg)) {}

DxgiDesktopDupCapture::~DxgiDesktopDupCapture() = default;

bool DxgiDesktopDupCapture::available() noexcept {
    return false;
}

plugins::Status DxgiDesktopDupCapture::open() noexcept {
    // DXGI is a Windows API.  On any other host this device simply
    // does not exist, and saying so is clearer than pretending.
    return plugins::kErrUnsupported;
}

void DxgiDesktopDupCapture::close() noexcept {}

std::vector<plugins::CaptureDevice>
DxgiDesktopDupCapture::enumerate_devices() const noexcept {
    return {};
}

std::vector<plugins::CaptureFormat>
DxgiDesktopDupCapture::supported_formats(const std::string&) const noexcept {
    return {};
}

plugins::CaptureCapabilities
DxgiDesktopDupCapture::capabilities() const noexcept {
    plugins::CaptureCapabilities c;
    // The stub reports the *same* static design flags as the real
    // backend: the platform's capabilities are a property of the
    // backend, not of the host it runs on.  A query must not answer
    // differently depending on where it is asked from.
    c.zero_copy = false;
    c.dirty_rects = true;
    c.cursor_composited = false;
    c.region_selection = true;
    c.enumeration = true;
    c.max_pixels = 0;
    c.backend = "dxgi-desktop-dup";
    return c;
}

void DxgiDesktopDupCapture::capture_loop() noexcept {}

plugins::Status DxgiDesktopDupCapture::pop_frame(
        plugins::CaptureFrame&) noexcept {
    return plugins::kErrUnsupported;
}

plugins::CaptureStats DxgiDesktopDupCapture::stats() const noexcept {
    return plugins::CaptureStats{};
}

#endif // NIMRTC_DXGI_CAPTURE

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void register_dxgi_desktop_dup() noexcept {
#if defined(NIMRTC_DXGI_CAPTURE)
    static const DxgiDesktopDupFactory kFactory{};
    core::PluginRegistry::instance().register_video_capture(kFactory.id(),
                                                            &kFactory);
#endif
}

} // namespace nimrtc::video_capture
