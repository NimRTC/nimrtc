/**
 * @file modules/video_capture/src/platform/windows/wgc_capture.hpp
 * @brief The Windows Graphics Capture backend 鈥?display or window capture.
 *
 * WGC is the *composited* path: the OS hands us exactly what the user
 * sees, pointer included (`GraphicsCaptureSession::
 * IsCursorCaptureEnabled`), for a display or for a single window.
 *
 * Two things distinguish it from DXGI Desktop Duplication:
 *
 *   - it needs the user's **consent** (`GraphicsCaptureAccess`), and a
 *     refusal has to be reported as a distinct error 鈥?not as "no
 *     frames", which is indistinguishable from a broken peer;
 *   - it reports **no dirty rectangles**.  The frame is a whole-frame
 *     picture, so the honest dirty-rect answer is "everything
 *     changed" 鈥?never an empty rect, which an incremental encoder
 *     would read as "nothing changed".
 *
 * The surface is `B8G8R8A8_UNORM` (BGRA).
 *
 * @note Internal to the video_capture module. Not a public header.
 */

#pragma once

#ifdef _WIN32

#include <memory>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include "d3d11_support.hpp"
#include "nimrtc/video_capture/windows_capture.hpp"

namespace nimrtc::video_capture::wcap {

/** What a WGC session captures. */
enum class WgcTargetKind : std::uint8_t {
    /** A whole display (the `HMONITOR` of a DXGI output). */
    kMonitor = 0,
    /** A single application window (`HWND`). */
    kWindow = 1,
};

/**
 * @brief A live Windows Graphics Capture session.
 *
 * Owns the D3D11 device, the frame pool, the capture session and the
 * capture item.  `try_get_next()` is non-blocking: it returns null when
 * the compositor has not produced a new frame yet.
 */
class WgcCapture {
public:
    WgcCapture() = default;
    ~WgcCapture();

    WgcCapture(const WgcCapture&)            = delete;
    WgcCapture& operator=(const WgcCapture&) = delete;

    /**
     * @brief Open a capture session.
     *
     * @param target      What to capture.
     * @param handle      The `HMONITOR` or `HWND`, depending on @p target.
     * @param width       Requested width (0 = the target's natural size).
     * @param height      Requested height (0 = the target's natural size).
     * @param want_cursor Whether the OS should composite the pointer.
     * @param require_consent Whether to verify the capture consent first.
     * @param out_error   Receives the failure reason on failure.
     * @return true on success.
     */
    bool open(WgcTargetKind target,
              void* handle,
              std::uint32_t width,
              std::uint32_t height,
              bool want_cursor,
              bool require_consent,
              WindowsCaptureError& out_error) noexcept;

    /** Tear the session down. Idempotent. */
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return session_ != nullptr; }

    /** The size of the frames the pool produces. */
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

    /**
     * @brief Try to take the next frame.
     *
     * @param out_texture Receives the frame's D3D11 texture.
     * @param out_error   Receives the failure reason on failure.
     * @return true when a frame was available.
     *
     * The texture stays valid until the next call 鈥?the caller must be
     * done with it (or have copied it) before calling again.
     */
    bool try_get_next(ComPtr<ID3D11Texture2D>& out_texture,
                      WindowsCaptureError& out_error) noexcept;

    /** The D3D11 state (device + immediate context). */
    [[nodiscard]] const D3D11State& d3d() const noexcept { return d3d_; }

private:
    D3D11State d3d_;
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

} // namespace nimrtc::video_capture::wcap

#endif // _WIN32
