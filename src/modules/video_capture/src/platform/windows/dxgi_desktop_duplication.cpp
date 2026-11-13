// modules/video_capture/src/platform/windows/dxgi_desktop_duplication.cpp
// ----------------------------------------------------------------------------
// The DXGI Desktop Duplication backend.
//
// See dxgi_desktop_duplication.hpp for the contract.
// ----------------------------------------------------------------------------

#ifdef _WIN32

#include "dxgi_desktop_duplication.hpp"

#include <algorithm>

namespace nimrtc::video_capture::wcap {

std::string output_device_id(const DXGI_OUTPUT_DESC& desc) noexcept {
    // `DeviceName` is a fixed-size WCHAR buffer holding e.g.
    // "\\.\DISPLAY1".  Narrow it for the device list.
    char narrow[64] = {0};
    std::size_t i = 0;
    for (; i + 1 < sizeof(narrow) && desc.DeviceName[i] != L'\0'; ++i) {
        narrow[i] = static_cast<char>(desc.DeviceName[i]);
    }
    return std::string{narrow, i};
}

namespace {

/** Build the human-readable name of an output. */
std::string output_name(const DXGI_OUTPUT_DESC& desc) {
    const std::string base = output_device_id(desc);
    return base + " (" + std::to_string(desc.DesktopCoordinates.right -
                                       desc.DesktopCoordinates.left) +
           "x" + std::to_string(desc.DesktopCoordinates.bottom -
                                desc.DesktopCoordinates.top) +
           ")";
}

/** The refresh rate of an output, in Hz (0 when it cannot be read). */
std::uint32_t output_refresh_hz(IDXGIOutput* output) noexcept {
    if (output == nullptr) return 0;

    // `GetDisplayModeList` writes into the array it is handed, so the
    // count has to be asked for first: asking with a null array and a
    // non-zero count is a null dereference, and asking with a
    // non-null array and a zero count makes the driver write past the
    // buffer.  Both are asked for explicitly here.
    UINT num_modes = 0;
    if (FAILED(output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0,
                                          &num_modes, nullptr)) ||
        num_modes == 0) {
        return 0;
    }

    std::vector<DXGI_MODE_DESC> modes(num_modes);
    UINT written = 0;
    if (FAILED(output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0,
                                          &written, modes.data())) ||
        written == 0) {
        return 0;
    }

    // The compositor lists many modes; the *current* one is the one
    // worth reporting, but the first readable refresh rate is close
    // enough for a capability query and never lies about the order of
    // magnitude.
    for (UINT i = 0; i < written; ++i) {
        if (modes[i].RefreshRate.Denominator == 0) continue;
        return modes[i].RefreshRate.Numerator / modes[i].RefreshRate.Denominator;
    }
    return 0;
}

} // anonymous namespace

std::vector<DxgiOutput> enumerate_dxgi_outputs() noexcept {
    std::vector<DxgiOutput> outputs;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return outputs;

    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapter_index, &adapter) ==
            DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (!adapter) break;

        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(output_index, &output) ==
                DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (!output) break;

            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc))) continue;

            ComPtr<IDXGIOutput1> output1;
            if (FAILED(output.As(&output1))) continue;

            DxgiOutput entry;
            entry.device_id = output_device_id(desc);
            entry.name = output_name(desc);
            entry.adapter = adapter;
            entry.output = output1;
            entry.monitor = desc.Monitor;
            entry.desktop_x = static_cast<std::uint32_t>(
                desc.DesktopCoordinates.left);
            entry.desktop_y = static_cast<std::uint32_t>(
                desc.DesktopCoordinates.top);
            entry.width = static_cast<std::uint32_t>(
                desc.DesktopCoordinates.right - desc.DesktopCoordinates.left);
            entry.height = static_cast<std::uint32_t>(
                desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
            entry.refresh_hz = output_refresh_hz(output.Get());
            outputs.push_back(std::move(entry));
        }
    }
    return outputs;
}

DxgiDesktopDuplication::~DxgiDesktopDuplication() { close(); }

bool DxgiDesktopDuplication::open(const DxgiOutput& output) noexcept {
    close();

    if (!output.adapter || !output.output) return false;

    HRESULT hr = d3d_.create_on(output.adapter);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIOutputDuplication> duplication;
    hr = output.output->DuplicateOutput(d3d_.device.Get(), &duplication);
    if (FAILED(hr)) return false;

    duplication_ = duplication;
    width_ = output.width;
    height_ = output.height;
    return true;
}

void DxgiDesktopDuplication::close() noexcept {
    if (frame_acquired_ && duplication_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
    }
    duplication_.Reset();
    d3d_.reset();
    width_ = 0;
    height_ = 0;
    seen_frame_ = false;
}

bool DxgiDesktopDuplication::acquire_next(
        ComPtr<ID3D11Texture2D>& out_texture,
        plugins::CaptureDirtyRects& out_dirty,
        std::uint64_t& out_frame_index,
        std::uint32_t timeout_ms,
        WindowsCaptureError& out_error) noexcept {
    if (!duplication_) {
        out_error = WindowsCaptureError::kDeviceLost;
        return false;
    }

    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr =
        duplication_->AcquireNextFrame(timeout_ms, &frame_info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // No frame within the timeout - this is not an error, the caller
        // simply has nothing new to deliver.
        out_error = WindowsCaptureError::kNone;
        return false;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        // The duplication became invalid (display change, GPU reset).
        out_error = WindowsCaptureError::kDeviceLost;
        return false;
    }
    if (FAILED(hr)) {
        out_error = WindowsCaptureError::kCaptureFailed;
        return false;
    }

    frame_acquired_ = true;

    ComPtr<ID3D11Texture2D> texture;
    hr = resource.As(&texture);
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
        out_error = WindowsCaptureError::kCaptureFailed;
        return false;
    }
    out_texture = texture;
    // The DXGI frame info has no frame index; the compositor's
    // accumulation counter is the closest stable thing available.
    out_frame_index = frame_info.AccumulatedFrames;

    // ---- The dirty rectangles -------------------------------------------
    //
    // `GetFrameDirtyRects` gives the regions of the *desktop* that
    // changed; `GetFrameMoveRects` gives the regions that were merely
    // moved (a window dragged).  Both matter for an incremental
    // encoder: a moved region is a changed region.
    //
    // The compositor's diff is *authoritative* for DXGI: when it reports
    // no dirty rect and no move rect, the desktop genuinely did not
    // change, and the honest answer is "nothing changed" -- an empty
    // dirty rect, not a full frame.  That is what makes a static
    // desktop nearly free to send (remote-desktop plan A7).
    //
    // The *first* frame after the duplication is opened is the
    // exception: it carries the whole front buffer, and the compositor
    // does not always describe it with rects.  That frame is reported
    // as a full frame.
    // `GetFrameDirtyRects` and `GetFrameMoveRects` describe the current
    // frame's damage in *bytes*: the first argument is the size of the
    // caller's buffer and the third argument receives the number of
    // bytes the API needs (or used).  Passing a null buffer is *not* a
    // supported way to query the size -- it fails with E_INVALIDARG --
    // so we start from a modest heap buffer and grow on
    // DXGI_ERROR_MORE_DATA.
    std::vector<RECT> dirty_rects;
    std::vector<DXGI_OUTDUPL_MOVE_RECT> move_rects;

    out_dirty.full_frame = !seen_frame_;
    out_dirty.rects.clear();
    seen_frame_ = true;

    {
        dirty_rects.resize(64);
        UINT required = 0;
        HRESULT dirty_hr = duplication_->GetFrameDirtyRects(
            static_cast<UINT>(dirty_rects.size() * sizeof(RECT)),
            dirty_rects.data(), &required);
        if (dirty_hr == DXGI_ERROR_MORE_DATA) {
            // The compositor reported more damage than we asked for.
            dirty_rects.resize(static_cast<std::size_t>(required) /
                                   sizeof(RECT) +
                               1);
            UINT required2 = 0;
            if (SUCCEEDED(duplication_->GetFrameDirtyRects(
                    static_cast<UINT>(dirty_rects.size() * sizeof(RECT)),
                    dirty_rects.data(), &required2))) {
                required = required2;
            } else {
                dirty_rects.clear();
                required = 0;
            }
        } else if (!SUCCEEDED(dirty_hr)) {
            dirty_rects.clear();
            required = 0;
        }
        dirty_rects.resize(static_cast<std::size_t>(required) / sizeof(RECT));
    }

    {
        move_rects.resize(16);
        UINT required = 0;
        HRESULT move_hr = duplication_->GetFrameMoveRects(
            static_cast<UINT>(move_rects.size() *
                              sizeof(DXGI_OUTDUPL_MOVE_RECT)),
            move_rects.data(), &required);
        if (move_hr == DXGI_ERROR_MORE_DATA) {
            move_rects.resize(static_cast<std::size_t>(required) /
                                  sizeof(DXGI_OUTDUPL_MOVE_RECT) +
                              1);
            UINT required2 = 0;
            if (SUCCEEDED(duplication_->GetFrameMoveRects(
                    static_cast<UINT>(move_rects.size() *
                                     sizeof(DXGI_OUTDUPL_MOVE_RECT)),
                    move_rects.data(), &required2))) {
                required = required2;
            } else {
                move_rects.clear();
                required = 0;
            }
        } else if (!SUCCEEDED(move_hr)) {
            move_rects.clear();
            required = 0;
        }
        move_rects.resize(static_cast<std::size_t>(required) /
                          sizeof(DXGI_OUTDUPL_MOVE_RECT));
    }

    if (!dirty_rects.empty()) {
        out_dirty.full_frame = false;
        out_dirty.rects.reserve(dirty_rects.size() + move_rects.size());
        for (const RECT& r : dirty_rects) {
            plugins::CaptureRegion region;
            region.x = static_cast<std::uint32_t>(r.left);
            region.y = static_cast<std::uint32_t>(r.top);
            region.width = static_cast<std::uint32_t>(r.right - r.left);
            region.height = static_cast<std::uint32_t>(r.bottom - r.top);
            out_dirty.rects.push_back(region);
        }
    }

    if (!move_rects.empty()) {
        // A moved region *is* a changed region: the encoder has to
        // redraw it.
        out_dirty.full_frame = false;
        for (const DXGI_OUTDUPL_MOVE_RECT& r : move_rects) {
            plugins::CaptureRegion region;
            region.x = static_cast<std::uint32_t>(r.DestinationRect.left);
            region.y = static_cast<std::uint32_t>(r.DestinationRect.top);
            region.width = static_cast<std::uint32_t>(
                r.DestinationRect.right - r.DestinationRect.left);
            region.height = static_cast<std::uint32_t>(
                r.DestinationRect.bottom - r.DestinationRect.top);
            out_dirty.rects.push_back(region);
        }
    }

    out_error = WindowsCaptureError::kNone;
    return true;
}
void DxgiDesktopDuplication::release_frame() noexcept {
    if (frame_acquired_ && duplication_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
    }
}

} // namespace nimrtc::video_capture::wcap

#endif // _WIN32
