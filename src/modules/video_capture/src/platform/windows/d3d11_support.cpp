// modules/video_capture/src/platform/windows/d3d11_support.cpp
// ----------------------------------------------------------------------------
// The D3D11 plumbing shared by the DXGI and WGC backends.
//
// See d3d11_support.hpp for the contract.
// ----------------------------------------------------------------------------

#ifdef _WIN32

#include "d3d11_support.hpp"

namespace nimrtc::video_capture::wcap {

HRESULT D3D11State::create_on(const ComPtr<IDXGIAdapter1>& adapter_in) noexcept {
    reset();
    if (!adapter_in) return E_INVALIDARG;

    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        adapter_in.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &device, &feature_level, &immediate);
    if (FAILED(hr)) {
        reset();
        return hr;
    }
    adapter = adapter_in;
    return S_OK;
}
HRESULT D3D11State::create_default() noexcept {
    reset();

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;

    ComPtr<IDXGIAdapter1> adapter0;
    hr = factory->EnumAdapters1(0, &adapter0);
    if (FAILED(hr)) return hr;

    return create_on(adapter0);
}

void D3D11State::reset() noexcept {
    immediate.Reset();
    device.Reset();
    adapter.Reset();
}

video_frame::PixelFormat pixel_format_from_dxgi(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return video_frame::PixelFormat::kBGRA;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return video_frame::PixelFormat::kRGBA;
        default:
            return video_frame::PixelFormat::kUnknown;
    }
}

void GpuTexturePool::set_device(const ComPtr<ID3D11Device>& device) noexcept {
    device_ = device;
    textures_.clear();
    next_ = 0;
}

ComPtr<ID3D11Texture2D> GpuTexturePool::acquire(std::uint32_t width,
                                               std::uint32_t height,
                                               DXGI_FORMAT format) noexcept {
    if (!device_ || width == 0 || height == 0) return nullptr;

    // Round-robin over the textures that match the request: the caller
    // copies the freshly captured frame into the texture it gets, so a
    // texture the consumer may still be reading must not be handed out
    // again immediately.  With a ring of N textures, the consumer has
    // N frames of slack before a texture is overwritten.
    const std::size_t count = textures_.size();
    for (std::size_t offset = 0; offset < count; ++offset) {
        const std::size_t index = (next_ + offset) % count;
        ComPtr<ID3D11Texture2D>& tex = textures_[index];
        if (!tex) continue;
        D3D11_TEXTURE2D_DESC desc{};
        tex->GetDesc(&desc);
        if (desc.Width == width && desc.Height == height &&
            desc.Format == format) {
            next_ = (index + 1) % count;
            return tex;
        }
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr)) return nullptr;

    textures_.push_back(texture);
    next_ = textures_.size();
    return texture;
}

namespace {

/**
 * @brief A `GpuBuffer` that owns the D3D11 texture it points at.
 *
 * `GpuBuffer::handle()` is a raw `void*`, so *something* has to keep
 * the texture alive.  That something is this subclass: it holds the
 * `ComPtr`, and the buffer is destroyed when the last reference to it
 * goes away.  The handle is only ever read while the buffer is alive,
 * so it can never dangle.
 */
class D3D11GpuBuffer final : public video_frame::GpuBuffer {
public:
    D3D11GpuBuffer(const ComPtr<ID3D11Texture2D>& texture,
                   std::uint32_t width,
                   std::uint32_t height,
                   video_frame::PixelFormat format) noexcept
        : video_frame::GpuBuffer(video_frame::GpuBackend::kWindowsD3D11Texture,
                                 texture.Get(), width, height, format),
          texture_(texture) {}

    D3D11GpuBuffer(const D3D11GpuBuffer&)            = delete;
    D3D11GpuBuffer& operator=(const D3D11GpuBuffer&) = delete;

private:
    ComPtr<ID3D11Texture2D> texture_;
};

} // anonymous namespace

std::shared_ptr<video_frame::GpuBuffer>
make_gpu_buffer(const ComPtr<ID3D11Texture2D>& texture,
                std::uint32_t width,
                std::uint32_t height,
                video_frame::PixelFormat format) noexcept {
    if (!texture) return nullptr;
    return std::make_shared<D3D11GpuBuffer>(texture, width, height, format);
}

bool read_texture_to_cpu(const ComPtr<ID3D11DeviceContext>& immediate,
                          ID3D11Texture2D* source,
                          const SourceRegion& region,
                          std::vector<std::uint8_t>& out,
                          std::uint32_t& out_width,
                          std::uint32_t& out_height,
                          std::int32_t& out_stride) noexcept {
    if (!immediate || !source) return false;

    D3D11_TEXTURE2D_DESC src_desc{};
    source->GetDesc(&src_desc);

    const std::uint32_t sx = region.x;
    const std::uint32_t sy = region.y;
    const std::uint32_t sw =
        (region.width == 0) ? src_desc.Width : region.width;
    const std::uint32_t sh =
        (region.height == 0) ? src_desc.Height : region.height;
    if (sx + sw > src_desc.Width || sy + sh > src_desc.Height) return false;
    if (sw == 0 || sh == 0) return false;

    // A staging texture is the only way to read a GPU texture back into
    // host memory.  It is created per call here: the fallback path is
    // the slow path, and a cached staging texture would have to be
    // resized whenever the negotiated format changes.
    D3D11_TEXTURE2D_DESC staging_desc = src_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;

    ComPtr<ID3D11Device> device;
    immediate->GetDevice(device.GetAddressOf());

    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
    if (FAILED(hr)) return false;

    immediate->CopyResource(staging.Get(), source);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    const std::size_t row_bytes = static_cast<std::size_t>(sw) * 4u;
    out.assign(row_bytes * static_cast<std::size_t>(sh), 0u);
    for (std::uint32_t row = 0; row < sh; ++row) {
        const auto* src_row = static_cast<const std::uint8_t*>(mapped.pData) +
                              static_cast<std::size_t>(row) * mapped.RowPitch +
                              static_cast<std::size_t>(sx) * 4u;
        std::memcpy(out.data() + static_cast<std::size_t>(row) * row_bytes,
                    src_row, row_bytes);
    }
    immediate->Unmap(staging.Get(), 0);

    out_width = sw;
    out_height = sh;
    out_stride = static_cast<std::int32_t>(row_bytes);
    return true;
}

bool copy_texture_on_device(const ComPtr<ID3D11DeviceContext>& immediate,
                            ID3D11Texture2D* source,
                            const SourceRegion& region,
                            ID3D11Texture2D* destination) noexcept {
    if (!immediate || !source || !destination) return false;

    D3D11_TEXTURE2D_DESC src_desc{};
    source->GetDesc(&src_desc);
    D3D11_TEXTURE2D_DESC dst_desc{};
    destination->GetDesc(&dst_desc);

    const std::uint32_t sx = region.x;
    const std::uint32_t sy = region.y;
    const std::uint32_t sw =
        (region.width == 0) ? src_desc.Width : region.width;
    const std::uint32_t sh =
        (region.height == 0) ? src_desc.Height : region.height;
    if (sx + sw > src_desc.Width || sy + sh > src_desc.Height) return false;
    if (sw != dst_desc.Width || sh != dst_desc.Height) return false;

    if (region.is_full(src_desc.Width, src_desc.Height)) {
        immediate->CopyResource(destination, source);
        return true;
    }

    D3D11_BOX box{};
    box.left = sx;
    box.top = sy;
    box.right = sx + sw;
    box.bottom = sy + sh;
    box.back = 0;
    box.front = 1;
    immediate->CopySubresourceRegion(destination, 0, 0, 0, 0, source, 0, &box);
    return true;
}

} // namespace nimrtc::video_capture::wcap

#endif // _WIN32
