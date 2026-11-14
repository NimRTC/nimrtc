/**
 * @file modules/video_capture/src/platform/windows/dxgi_desktop_duplication.hpp
 * @brief The DXGI Desktop Duplication backend — display capture.
 *
 * Desktop Duplication is the cheapest way to capture a *display* on
 * Windows: the compositor hands out the front buffer directly, with no
 * copy through a staging surface and no compositor round-trip.  It
 * reports the changed region natively (`GetFrameDirtyRects` /
 * `GetFrameMoveRects`), which is what makes a static desktop nearly
 * free to send.
 *
 * What it does **not** do is composite the pointer: the cursor is a
 * separate layer (remote-desktop plan item A6).
 *
 * The surface is `DXGI_FORMAT_B8G8R8A8_UNORM` — BGRA, the format
 * every desktop compositor produces natively.
 *
 * @note Internal to the video_capture module. Not a public header.
 */

#pragma once

#ifdef _WIN32

#include <memory>
#include <string>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "d3d11_support.hpp"
#include "nimrtc/video_capture/windows_capture.hpp"

namespace nimrtc::video_capture::wcap {

/** One display, as DXGI sees it. */
struct DxgiOutput {
    /** Stable device id — the DXGI device name, e.g. `\\.\DISPLAY1`. */
    std::string device_id;
    /** Human-readable name. */
    std::string name;
    /** The adapter this output belongs to. */
    ComPtr<IDXGIAdapter1> adapter;
    /** The output itself. */
    ComPtr<IDXGIOutput1> output;
    /** The `HMONITOR` of this output (used by the WGC backend). */
    HMONITOR monitor = nullptr;
    /** Desktop coordinates of the output in the virtual desktop. */
    std::uint32_t desktop_x = 0;
    std::uint32_t desktop_y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t refresh_hz = 0;
};

/**
 * @brief The stable device id of a DXGI output — its device name, e.g.
 *  `"\\\\.\\DISPLAY1"`.
 */
std::string output_device_id(const DXGI_OUTPUT_DESC& desc) noexcept;

/**
 * @brief Enumerate the displays DXGI can duplicate.
 *
 * Returns an empty vector on a machine with no display (a headless CI
 * leg) — the caller reports "no device" rather than crashing.
 */
std::vector<DxgiOutput> enumerate_dxgi_outputs() noexcept;

/**
 * @brief A live Desktop Duplication session on one output.
 *
 * The session owns the D3D11 device, the duplication object, and the
 * texture pool.  `acquire_next()` blocks on the compositor (with a
 * timeout) until a frame is available — it is called from the capture
 * device's own thread, never from the engine's tick thread.
 */
class DxgiDesktopDuplication {
public:
    DxgiDesktopDuplication() = default;
    ~DxgiDesktopDuplication();

    DxgiDesktopDuplication(const DxgiDesktopDuplication&)            = delete;
    DxgiDesktopDuplication& operator=(const DxgiDesktopDuplication&) = delete;

    /** Open a duplication session on @p output. */
    bool open(const DxgiOutput& output) noexcept;

    /** Release everything. Idempotent. */
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return duplication_ != nullptr; }

    /** The output's width in pixels. */
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }

    /** The output's height in pixels. */
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

    /**
     * @brief Acquire the next frame.
     *
     * @param out_texture Receives the acquired texture.  It is valid
     *        only until `release_frame()` is called.
     * @param out_dirty   Receives the changed region(s).
     * @param out_frame_index Receives the frame's index.
     * @param timeout_ms  How long to wait for a frame.
     * @param out_error   Receives the failure reason on failure.
     * @return true when a frame was acquired.
     */
    bool acquire_next(ComPtr<ID3D11Texture2D>& out_texture,
                      plugins::CaptureDirtyRects& out_dirty,
                      std::uint64_t& out_frame_index,
                      std::uint32_t timeout_ms,
                      WindowsCaptureError& out_error) noexcept;

    /** Release the acquired frame. */
    void release_frame() noexcept;

    /** The D3D11 state (device + immediate context). */
    [[nodiscard]] const D3D11State& d3d() const noexcept { return d3d_; }

private:
    D3D11State d3d_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    bool frame_acquired_ = false;
    /** True once a frame has been acquired on this session.
     *
     *  The *first* frame after the duplication is opened carries the
     *  whole front buffer, and the compositor does not always describe
     *  it with dirty rects -- so that one frame is reported as a full
     *  frame.  Every later frame is described by the compositor's own
     *  diff. */
    bool seen_frame_ = false;
};

} // namespace nimrtc::video_capture::wcap

#endif // _WIN32
