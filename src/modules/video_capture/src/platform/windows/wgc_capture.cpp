// modules/video_capture/src/platform/windows/wgc_capture.cpp
// ----------------------------------------------------------------------------
// The Windows Graphics Capture backend.
//
// See wgc_capture.hpp for the contract.
// ----------------------------------------------------------------------------

#ifdef _WIN32

#include "wgc_capture.hpp"

#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace nimrtc::video_capture::wcap {

namespace {

using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

using winrt::Windows::Graphics::SizeInt32;
using winrt::Windows::Security::Authorization::AppCapabilityAccess::
    AppCapabilityAccessStatus;

/**
 * @brief Check the capture consent.
 *
 * Two kinds exist and they mean different things:
 *
 *   - `Programmatic` 鈥?the app may capture a window's content without
 *     the user picking it in a dialog (Windows 11 22H2+).
 *   - `Borderless` 鈥?the app may capture a *borderless* window (a game,
 *     a video player), which Windows has always gated behind the
 *     user's consent.
 *
 * A session that needs *either* is accepted as long as *one* of them
 * is granted: a monitor capture is granted through `Programmatic`, a
 * window capture through `Borderless`.  When neither is granted the
 * capture is refused, and the refusal is reported as
 * `kConsentDenied` 鈥?a distinct condition the host can act on (ask
 * the user to grant screen recording) rather than a generic failure.
 */
bool has_consent(WindowsCaptureError& out_error) noexcept {
    const auto status_of = [](GraphicsCaptureAccessKind kind) {
        try {
            return GraphicsCaptureAccess::RequestAccessAsync(kind).get();
        } catch (const winrt::hresult_error&) {
            return AppCapabilityAccessStatus::DeniedBySystem;
        }
    };

    const AppCapabilityAccessStatus programmatic =
        status_of(GraphicsCaptureAccessKind::Programmatic);
    if (programmatic == AppCapabilityAccessStatus::Allowed) return true;

    const AppCapabilityAccessStatus borderless =
        status_of(GraphicsCaptureAccessKind::Borderless);
    if (borderless == AppCapabilityAccessStatus::Allowed) return true;

    out_error = WindowsCaptureError::kConsentDenied;
    return false;
}

/** Create the capture item for a monitor or a window. */
bool create_capture_item(WgcTargetKind target,
                         void* handle,
                         GraphicsCaptureItem& out_item) noexcept {
    try {
        auto interop =
            winrt::get_activation_factory<GraphicsCaptureItem,
                                         IGraphicsCaptureItemInterop>();
        IUnknown* raw = nullptr;
        HRESULT hr = E_FAIL;
        if (target == WgcTargetKind::kMonitor) {
            hr = interop->CreateForMonitor(static_cast<HMONITOR>(handle),
                                           winrt::guid_of<GraphicsCaptureItem>(),
                                           winrt::put_abi(out_item));
        } else {
            hr = interop->CreateForWindow(static_cast<HWND>(handle),
                                           winrt::guid_of<GraphicsCaptureItem>(),
                                           winrt::put_abi(out_item));
        }
        (void)raw;
        return SUCCEEDED(hr);
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

} // anonymous namespace

WgcCapture::~WgcCapture() { close(); }

bool WgcCapture::open(WgcTargetKind target,
                       void* handle,
                       std::uint32_t width,
                       std::uint32_t height,
                       bool want_cursor,
                       bool require_consent,
                       WindowsCaptureError& out_error) noexcept {
    close();

    if (handle == nullptr) {
        out_error = WindowsCaptureError::kDeviceNotFound;
        return false;
    }

    if (require_consent) {
        WindowsCaptureError consent_error = WindowsCaptureError::kNone;
        if (!has_consent(consent_error)) {
            out_error = consent_error;
            return false;
        }
    }

    HRESULT hr = d3d_.create_default();
    if (FAILED(hr)) {
        out_error = WindowsCaptureError::kD3dDeviceFailed;
        return false;
    }

    GraphicsCaptureItem item = nullptr;
    if (!create_capture_item(target, handle, item)) {
        out_error = WindowsCaptureError::kDeviceNotFound;
        return false;
    }

    // The WinRT device wraps the same D3D11 device the encoder will
    // use, so the frame surfaces and the encoder live on one adapter.
    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(d3d_.device.As(&dxgi_device))) {
        out_error = WindowsCaptureError::kD3dDeviceFailed;
        return false;
    }

    IInspectable* raw_device = nullptr;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), &raw_device);
    if (FAILED(hr)) {
        out_error = WindowsCaptureError::kD3dDeviceFailed;
        return false;
    }
    IDirect3DDevice winrt_device = nullptr;
    try {
        winrt_device = IDirect3DDevice(raw_device, winrt::take_ownership_from_abi);
    } catch (const winrt::hresult_error&) {
        out_error = WindowsCaptureError::kD3dDeviceFailed;
        return false;
    }

    const std::uint32_t pool_width = (width == 0) ? 1280u : width;
    const std::uint32_t pool_height = (height == 0) ? 720u : height;

    try {
        // A free-threaded pool has its own thread, so the capture does
        // not depend on the caller's message loop.
        pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrt_device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
            SizeInt32{static_cast<std::int32_t>(pool_width),
                      static_cast<std::int32_t>(pool_height)});

        session_ = pool_.CreateCaptureSession(item);
        // The OS composites the pointer into the frame 鈥?this is the
        // reason to prefer WGC over DXGI for a window capture.
        session_.IsCursorCaptureEnabled(want_cursor);
        session_.StartCapture();
    } catch (const winrt::hresult_error&) {
        out_error = WindowsCaptureError::kCaptureFailed;
        close();
        return false;
    }

    item_ = item;
    width_ = pool_width;
    height_ = pool_height;
    out_error = WindowsCaptureError::kNone;
    return true;
}

void WgcCapture::close() noexcept {
    // There is no `StopCapture` in this SDK's projection (it arrived
    // with a later Windows 11 release), and none is needed: dropping the
    // session stops the capture.  The pool and the item are released
    // right after, so the compositor stops handing out frames.
    session_ = nullptr;
    pool_ = nullptr;
    item_ = nullptr;
    d3d_.reset();
    width_ = 0;
    height_ = 0;
}

bool WgcCapture::try_get_next(ComPtr<ID3D11Texture2D>& out_texture,
                              WindowsCaptureError& out_error) noexcept {
    if (!pool_) {
        out_error = WindowsCaptureError::kDeviceLost;
        return false;
    }

    Direct3D11CaptureFrame frame = nullptr;
    try {
        frame = pool_.TryGetNextFrame();
    } catch (const winrt::hresult_error&) {
        out_error = WindowsCaptureError::kCaptureFailed;
        return false;
    }

    if (!frame) {
        // No new frame yet 鈥?not an error.
        out_error = WindowsCaptureError::kNone;
        return false;
    }

    auto surface = frame.Surface();
    if (!surface) {
        out_error = WindowsCaptureError::kCaptureFailed;
        return false;
    }

    ComPtr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>
        access;
    HRESULT hr = reinterpret_cast<IInspectable*>(surface.as<::IInspectable>().get())
                     ->QueryInterface(IID_PPV_ARGS(&access));
    if (FAILED(hr)) {
        out_error = WindowsCaptureError::kCaptureFailed;
        return false;
    }

    ComPtr<ID3D11Texture2D> texture;
    hr = access->GetInterface(IID_PPV_ARGS(&texture));
    if (FAILED(hr)) {
        out_error = WindowsCaptureError::kCaptureFailed;
        return false;
    }

    out_texture = texture;
    out_error = WindowsCaptureError::kNone;
    return true;
}

} // namespace nimrtc::video_capture::wcap

#endif // _WIN32
