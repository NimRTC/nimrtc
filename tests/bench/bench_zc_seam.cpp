// bench_zc_seam.cpp -- the zero-copy *seam* probe (remote-desktop M1).
//
// ## What this measures
//
// The remote-desktop plan's M1 exit criterion says "the DXGI / WGC
// surface handed straight into NVENC with no staging, decoded and
// rendered on one real desktop".  The capture side of that is already
// proven (`bench_capture_encode`'s `zero_copy` block: `bytes_copied ==
// 0`).  This probe answers the *other* half: what does the hardware
// encoder actually accept on its DirectX input path?
//
// It answers three questions, each in its own process (the driver
// aborts the process on some combinations, and a crash would hide the
// answers that follow):
//
//   1. **Same device, NV12.**  The shape the shipped
//      `NvEncEncoder::encode_zero_copy()` expects: a `DXGI_FORMAT_NV12`
//      texture created on the *encoder's own* D3D11 device.
//   2. **Same device, BGRA.**  The shape the capture device actually
//      produces: `DXGI_FORMAT_B8G8R8A8_UNORM`.
//   3. **Other device, NV12.**  The shape a *seam* would produce: the
//      capture device's texture, handed to an encoder that owns a
//      different D3D11 device.
//
// ## Why the answers matter
//
// Question 3 is the decisive one for the seam's *shape*.  The NVIDIA
// driver registers a DirectX input resource **per encode session**, and
// the session is opened on one device — so a texture that lives on
// another device may not be registrable at all.  If that is the case,
// then a capture → encoder zero-copy seam on Windows cannot be a
// "hand the texture across" seam: the two sides have to *share* the
// device (or the surface has to be copied on the device, which is still
// a zero-copy frame but not a zero-copy *handoff*).
//
// The output is a JSON document on stdout, in the same shape as the
// other benchmarks, so `tools/` can capture it into an artefact.

#include <windows.h>
#include <d3d11.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/nvcodec/nv_backends.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/video_frame/frame.hpp>

namespace {

using nimrtc::plugins::EncodedVideoFrame;
using nimrtc::plugins::GpuBuffer;
using nimrtc::plugins::VideoCodecConfig;
using nimrtc::plugins::VideoCodecKind;
using nimrtc::plugins::VideoPixelFormat;
using nimrtc::video_frame::GpuBackend;
using nimrtc::video_frame::PixelFormat;

/** A `GpuBuffer` that owns the D3D11 texture it points at.
 *
 *  `GpuBuffer::handle()` is a raw `void*`, so something has to keep the
 *  texture alive.  This subclass does, so the buffer can never dangle. */
class OwnedTextureBuffer final : public GpuBuffer {
public:
    OwnedTextureBuffer(ID3D11Texture2D* tex, std::uint32_t w,
                       std::uint32_t h, PixelFormat fmt) noexcept
        : GpuBuffer(GpuBackend::kWindowsD3D11Texture, tex, w, h, fmt),
          tex_(tex) {}

    ~OwnedTextureBuffer() override {
        if (tex_) tex_->Release();
    }

private:
    ID3D11Texture2D* tex_ = nullptr;
};

/** The outcome of one probe case. */
struct CaseResult {
    std::string name;
    bool        ran          = false;
    bool        accepted     = false;
    std::size_t payload_bytes = 0;
    bool        keyframe     = false;
    std::string detail;
};

/** The D3D11 device the probe creates its textures on. */
ID3D11Device*        g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;

bool create_device() {
    return SUCCEEDED(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &g_device, nullptr, &g_context));
}

/** Create a texture of the given format on @p device. */
ID3D11Texture2D* make_texture(ID3D11Device* device,
                               std::uint32_t w, std::uint32_t h,
                               DXGI_FORMAT fmt) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width          = w;
    d.Height         = h;
    d.MipLevels      = 1;
    d.ArraySize      = 1;
    d.Format         = fmt;
    d.SampleDesc.Count = 1;
    d.Usage          = D3D11_USAGE_DEFAULT;
    d.BindFlags      = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &tex))) return nullptr;
    return tex;
}

/** Paint the texture with a recognisable gradient.
 *
 *  The point is not the picture but the *proof*: a bitstream produced
 *  from this texture encodes something, so the driver really read the
 *  surface. */
void fill_texture(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex,
                  std::uint32_t w, std::uint32_t h, DXGI_FORMAT fmt) {
    const std::uint32_t bpp =
        (fmt == DXGI_FORMAT_B8G8R8A8_UNORM) ? 4u : 1u;
    std::vector<std::uint8_t> px(
        static_cast<std::size_t>(w) * h * bpp, 0x40);

    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p =
                px.data() + (static_cast<std::size_t>(y) * w + x) * bpp;
            if (bpp == 4) {
                p[0] = static_cast<std::uint8_t>(x & 0xFF);  // B
                p[1] = static_cast<std::uint8_t>(y & 0xFF);  // G
                p[2] = 0x80;                                 // R
                p[3] = 0xFF;                                 // A
            } else {
                p[0] = static_cast<std::uint8_t>(16 + (x * 219) / w);
            }
        }
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    for (std::uint32_t y = 0; y < h; ++y) {
        std::memcpy(static_cast<std::uint8_t*>(mapped.pData) +
                        static_cast<std::size_t>(y) * mapped.RowPitch,
                    px.data() + static_cast<std::size_t>(y) * w * bpp,
                    static_cast<std::size_t>(w) * bpp);
    }
    ctx->Unmap(tex, 0);
}

/** Drive the shipped NVENC encoder's zero-copy path once.
 *
 *  @param share_device When true, the encoder is given the texture's
 *         device through `VideoCodecConfig::gpu_context` — which is
 *         exactly what the shipped capture → encoder seam does (the
 *         capture device publishes its device, the encoder adopts it).
 *         When false the encoder keeps its own device, which is what
 *         the *cross-device* case needs. */
CaseResult run_case(const std::string& name,
                     DXGI_FORMAT dxgi_format,
                     PixelFormat pixel_format,
                     std::uint32_t w, std::uint32_t h,
                     ID3D11Device* texture_device,
                     bool share_device) {
    CaseResult r;
    r.name = name;

    VideoCodecConfig cfg;
    cfg.width        = w;
    cfg.height       = h;
    cfg.fps          = 30;
    cfg.bitrate_bps  = 2'000'000;
    cfg.codec        = VideoCodecKind::kH264;

    // The shared-device seam: the encoder creates its session on the
    // *producer's* device, so the texture it is handed is registrable.
    if (share_device) {
        cfg.gpu_context.backend = GpuBackend::kWindowsD3D11Texture;
        cfg.gpu_context.device  = texture_device;
    }

    auto codec = nimrtc::nvcodec::make_nvenc_codec(
        nimrtc::nvcodec::NvCodec::kH264, cfg);
    if (!codec) {
        r.detail = "the NVENC backend is not available on this host";
        return r;
    }
    if (codec->open() != nimrtc::plugins::kOk) {
        r.detail = "the NVENC session refused to open";
        return r;
    }

    ID3D11Texture2D* tex = make_texture(texture_device, w, h, dxgi_format);
    if (!tex) {
        r.detail = "the texture could not be created";
        codec->close();
        return r;
    }
    fill_texture(g_context, tex, w, h, dxgi_format);

    auto gpu = std::make_shared<OwnedTextureBuffer>(tex, w, h, pixel_format);

    EncodedVideoFrame out{};
    const auto rc = codec->encode_zero_copy(*gpu, out);
    r.ran = true;

    if (rc == nimrtc::plugins::kOk) {
        r.accepted      = true;
        r.payload_bytes = out.payload.size();
        r.keyframe      = out.is_keyframe;
        r.detail        = "the encoder consumed the surface and emitted a "
                          "bitstream";
    } else {
        r.detail = "the encoder refused the surface";
    }

    codec->close();
    return r;
}

} // namespace

int main(int argc, char** argv) {
    const std::string which = (argc > 1) ? argv[1] : "all";

    if (!create_device()) {
        std::fprintf(stderr, "probe: D3D11CreateDevice failed\n");
        return 1;
    }

    const std::uint32_t W = 640, H = 480;

    std::vector<CaseResult> results;

    if (which == "all" || which == "nv12") {
        results.push_back(run_case("nv12_same_device", DXGI_FORMAT_NV12,
                                    PixelFormat::kNV12, W, H, g_device, true));
    }
    if (which == "all" || which == "bgra") {
        results.push_back(run_case("bgra_same_device",
                                    DXGI_FORMAT_B8G8R8A8_UNORM,
                                    PixelFormat::kBGRA, W, H, g_device, true));
    }
    if (which == "all" || which == "cross") {
        ID3D11Device*        dev2 = nullptr;
        ID3D11DeviceContext* ctx2 = nullptr;
        if (SUCCEEDED(D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION, &dev2, nullptr, &ctx2))) {
            results.push_back(run_case("nv12_other_device", DXGI_FORMAT_NV12,
                                        PixelFormat::kNV12, W, H, dev2, false));
            ctx2->Release();
            dev2->Release();
        } else {
            CaseResult r;
            r.name   = "nv12_other_device";
            r.detail = "a second D3D11 device could not be created";
            results.push_back(r);
        }
    }

    // ---- JSON on stdout -------------------------------------------------
    std::printf("{\n");
    std::printf("  \"probe\": \"zc_seam\",\n");
    std::printf("  \"cases\": [\n");
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        std::printf("    {");
        std::printf("\"name\": \"%s\", ", r.name.c_str());
        std::printf("\"ran\": %s, ", r.ran ? "true" : "false");
        std::printf("\"accepted\": %s, ", r.accepted ? "true" : "false");
        std::printf("\"payload_bytes\": %zu, ", r.payload_bytes);
        std::printf("\"keyframe\": %s, ", r.keyframe ? "true" : "false");
        std::printf("\"detail\": \"%s\"}", r.detail.c_str());
        if (i + 1 < results.size()) std::printf(",");
        std::printf("\n");
        std::fflush(stdout);
    }
    std::printf("  ]\n");
    std::printf("}\n");

    g_context->Release();
    g_device->Release();
    return 0;
}
