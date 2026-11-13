/**
 * @file modules/video_capture/src/platform/windows/d3d11_support.hpp
 * @brief The D3D11 plumbing shared by the two Windows capture backends.
 *
 * Both backends (DXGI Desktop Duplication and Windows Graphics Capture)
 * end up with the same thing: an `ID3D11Texture2D` in
 * `DXGI_FORMAT_B8G8R8A8_UNORM` (BGRA) 鈥?the format every desktop
 * compositor produces natively.  This header holds the pieces that are
 * the same for both:
 *
 *   - creating the `ID3D11Device` on the adapter that owns the output,
 *   - turning a texture into a `video_frame::GpuBuffer` whose handle is
 *     that texture (`GpuBackend::kWindowsD3D11Texture`),
 *   - the CPU fallback: copy the texture into a `D3D11_USAGE_STAGING`
 *     texture, map it, and read the BGRA bytes out.
 *
 * ## The copy budget
 *
 * The GPU path copies the source texture *on the device* (a
 * `CopyResource` / `CopySubresourceRegion` into a texture we own), so
 * the pixels never leave the GPU: `bytes_copied` stays at 0 and the
 * frame is honestly a zero-copy frame.  The CPU fallback copies into
 * host memory and *does* count the bytes 鈥?the difference between the
 * two paths is visible in the statistics, never hidden.
 *
 * @note Internal to the video_capture module. Not a public header.
 */

#pragma once

#ifdef _WIN32

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <nimrtc/video_frame/frame.hpp>

namespace nimrtc::video_capture::wcap {

using Microsoft::WRL::ComPtr;

/** A sub-region of the source surface, in source pixels. */
struct SourceRegion {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    /** True iff the region covers the whole source surface. */
    [[nodiscard]] bool is_full(std::uint32_t src_w,
                               std::uint32_t src_h) const noexcept {
        return width == src_w && height == src_h && x == 0 && y == 0;
    }
};

/** The D3D11 device, its immediate context, and the adapter it lives on. */
struct D3D11State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<IDXGIAdapter1> adapter;

    /** Create the device on @p adapter_in. */
    HRESULT create_on(const ComPtr<IDXGIAdapter1>& adapter_in) noexcept;

    /** Create the device on the default adapter. */
    HRESULT create_default() noexcept;

    void reset() noexcept;
};

/** Map a DXGI format onto a `video_frame::PixelFormat`.
 *
 *  Returns `PixelFormat::kUnknown` for a format the capture layer does
 *  not know how to describe 鈥?the caller then reports an unsupported
 *  format rather than mislabelling the pixels. */
NIMRTC_API_STABLE video_frame::PixelFormat
pixel_format_from_dxgi(DXGI_FORMAT fmt) noexcept;

/**
 * @brief A recycling pool of GPU textures.
 *
 * The GPU path needs a texture it *owns* (the one the desktop
 * duplication hands out is recycled by DXGI the moment the frame is
 * released).  The pool keeps a small ring of them and hands them out
 * round-robin, so the steady state allocates nothing.
 */
class GpuTexturePool {
public:
    GpuTexturePool() = default;
    ~GpuTexturePool() = default;

    GpuTexturePool(const GpuTexturePool&)            = delete;
    GpuTexturePool& operator=(const GpuTexturePool&) = delete;

    /** Set the device the pool creates its textures on. */
    void set_device(const ComPtr<ID3D11Device>& device) noexcept;

    /**
     * Acquire a texture of the given size and format.
     *
     * The texture is recycled: handing out the next one may overwrite
     * the content of a previously handed-out texture.  That is exactly
     * the lifetime contract the zero-copy path documents 鈥?the consumer
     * uses the texture *within* the delivery callback.
     */
    ComPtr<ID3D11Texture2D> acquire(std::uint32_t width,
                                    std::uint32_t height,
                                    DXGI_FORMAT format) noexcept;

private:
    ComPtr<ID3D11Device> device_;
    std::vector<ComPtr<ID3D11Texture2D>> textures_;
    std::size_t next_ = 0;
};

/**
 * @brief Wrap a texture in a `GpuBuffer`.
 *
 * The returned buffer keeps the texture alive for as long as the buffer
 * itself is alive, so a consumer that holds the buffer can never end up
 * with a dangling handle.
 */
std::shared_ptr<video_frame::GpuBuffer>
make_gpu_buffer(const ComPtr<ID3D11Texture2D>& texture,
                std::uint32_t width,
                std::uint32_t height,
                video_frame::PixelFormat format) noexcept;

/**
 * @brief Read a texture (or a region of it) into host memory.
 *
 * @param out        Receives the BGRA bytes, tightly packed.
 * @param out_width  Receives the width of the region.
 * @param out_height Receives the height of the region.
 * @param out_stride Receives the row stride of @p out.
 * @return true on success.
 */
bool read_texture_to_cpu(const ComPtr<ID3D11DeviceContext>& immediate,
                          ID3D11Texture2D* source,
                          const SourceRegion& region,
                          std::vector<std::uint8_t>& out,
                          std::uint32_t& out_width,
                          std::uint32_t& out_height,
                          std::int32_t& out_stride) noexcept;

/** Copy a texture (or a region of it) into another texture.
 *
 *  This is the *device-local* copy the GPU path uses: the pixels stay
 *  on the GPU, so the frame is still a zero-copy frame. */
bool copy_texture_on_device(const ComPtr<ID3D11DeviceContext>& immediate,
                            ID3D11Texture2D* source,
                            const SourceRegion& region,
                            ID3D11Texture2D* destination) noexcept;

} // namespace nimrtc::video_capture::wcap

#endif // _WIN32
