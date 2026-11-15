/**
 * @file src/modules/nvcodec/tests/test_nvcodec_zero_copy.cpp
 * @brief B10 - the zero-copy capture to encode path, exercised on real
 *        hardware.
 *
 * ## What this test proves
 *
 * The zero-copy claim is a claim about a *negative*: "the picture never
 * reached the CPU".  A test that merely encodes a frame cannot prove a
 * negative.  So this test is built to make the negative *observable*:
 *
 *   1. A D3D11 texture is painted on the GPU by a *pixel shader* (a
 *      render target, so the picture never has a CPU-visible copy).
 *      The shader draws a horizontal ramp of pure red.
 *   2. The texture is handed to the encoder through the public
 *      `encode_zero_copy()` seam, as a `GpuBuffer`.
 *   3. The bitstream is decoded by NVDEC and the *decoded* picture is
 *      compared against what a red ramp must look like in YUV.
 *
 * ## Why a ramp of red, and not a flat colour
 *
 * A flat colour would survive a broken colour conversion: a grey, a
 * magenta and a black all decode to *something* plausible.  A ramp of
 * pure red does not, because red is the colour whose YUV signature is
 * asymmetric:
 *
 *   - red   -> the red-difference (V / Cr) *rises* along the ramp and the
 *              blue-difference (U / Cb) *falls*;
 *   - blue  -> exactly the opposite.
 *
 * So the two chroma trends in the decoded picture are a direct test of
 * the red / blue *order* of the registered surface - which is exactly
 * the thing that is easy to get wrong and impossible to see (the driver
 * does not reject a mismatched format, it silently encodes swapped
 * channels) - and a direct test that the encoder received the picture
 * the GPU painted rather than some other buffer.
 *
 * ## Why the surface must come from the encoder's own device
 *
 * NVENC binds a registered resource to the device the encode session was
 * opened against, and the driver rejects a resource from another device
 * with `NV_ENC_ERR_INVALID_RESOURCE` (status 9).  That was measured on
 * the reference host (RTX 3060, driver 616.92): the same texture
 * registers on the session's own device and fails on a second device
 * object created on the same adapter.
 *
 * So the test creates the texture on a device it owns and hands the
 * *device* to the encoder along with the texture.  The encoder adopts
 * it and re-opens its session there - which is exactly the behaviour a
 * real capture backend needs.
 *
 * ## Host dependency
 *
 * The test is skipped (GTEST_SKIP) when NVENC / NVDEC are unavailable,
 * or when the backend was not compiled in.  It never blocks CI.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/nvcodec/nv_backends.hpp>
#include <nimrtc/nvcodec/nv_codec_kind.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/video_frame/frame.hpp>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <d3d11.h>
  #include <d3dcompiler.h>
  #pragma comment(lib, "d3d11.lib")
#endif

namespace plugins  = nimrtc::plugins;
namespace video_frame = nimrtc::video_frame;
namespace core     = nimrtc::core;
namespace nvcodec  = nimrtc::nvcodec;

namespace {

constexpr std::uint32_t kWidth  = 320;
constexpr std::uint32_t kHeight = 240;

/** The red the ramp runs from (left edge) to (right edge).
 *
 *  A ramp rather than a flat colour: see the file header.  The ends
 *  avoid 0 and 255 so that a limited-range interpretation of the input
 *  cannot clamp the ends of the ramp away.
 *
 *  The attribute is needed because both ends of the ramp are used only
 *  by the Windows-only branches of the tests below: on a host without
 *  D3D11 the test bodies take their `GTEST_SKIP()` path and the
 *  constants are never read, which Clang's -Wunused-const-variable
 *  rejects under -Werror.
 *
 *  (Unlike a *data member*, a namespace-scope variable is exactly what
 *  `[[maybe_unused]]` is for, and both GCC and Clang honour it here.) */
[[maybe_unused]] constexpr float kRampLow  = 32.0f;
[[maybe_unused]] constexpr float kRampHigh = 224.0f;

#if defined(_WIN32)

/** Owns a D3D11 device, a texture, and paints the texture on the GPU.
 *
 *  The texture is a render target with `DXGI_FORMAT_B8G8R8A8_UNORM` -
 *  the *same* layout the DXGI Desktop Duplication backend produces - and
 *  the picture is drawn by a pixel shader, so the CPU never has a copy
 *  of the pixels at all.
 *
 *  The device is created with `D3D11_CREATE_DEVICE_VIDEO_SUPPORT` so
 *  that NVENC can open a session against it: the driver refuses a
 *  session on a device without video support. */
class D3D11Painter {
public:
    ~D3D11Painter() {
        if (rtv_)      rtv_->Release();
        if (texture_)  texture_->Release();
        if (context_)  context_->Release();
        if (device_)   device_->Release();
    }

    bool create(std::uint32_t w, std::uint32_t h) {
        const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        const HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION,
            &device_, nullptr, &context_);
        if (FAILED(hr) || !device_) return false;

        D3D11_TEXTURE2D_DESC td{};
        td.Width            = w;
        td.Height           = h;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_RENDER_TARGET |
                              D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &texture_))) {
            return false;
        }
        return SUCCEEDED(device_->CreateRenderTargetView(texture_, nullptr,
                                                         &rtv_));
    }

    /** Paint a horizontal ramp of pure red.  GPU-side only.
     *
     *  The ramp's *red* channel runs from `kRampLow` to `kRampHigh`; the
     *  green and blue channels stay at zero.  The shader's output is
     *  RGBA in HLSL's sense, and the render target's
     *  `B8G8R8A8_UNORM` layout puts the bytes into memory as B, G, R, A
     *  - which is the layout NVENC's `NV_ENC_BUFFER_FORMAT_ARGB`
     *  describes. */
    bool paint() {
        // A passthrough vertex shader, so the pixel shader's SV_Position
        // covers the whole render target.
        static const char* kVertexSource = R"(
struct VSOut {
    float4 pos : SV_Position;
};

VSOut main(uint vid : SV_VertexID) {
    VSOut vs_out;
    // One oversized triangle covering the viewport.
    float2 xy[3] = { float2(-1.0, -3.0), float2(-1.0, 1.0), float2(3.0, 1.0) };
    vs_out.pos = float4(xy[vid], 0.0, 1.0);
    return vs_out;
}
)";

        // The pixel shader's output is a *normalized* colour: HLSL
        // `float4`s in a render target are in [0, 1], not [0, 255].
        // Writing 0-255 values here clamps everything above 1.0 to
        // white, which is what makes a ramp look like a flat colour.
        char source[1024];
        std::snprintf(source, sizeof(source), R"(
float4 main(float4 pos : SV_Position) : SV_Target {
    float t = pos.x / %f;
    return float4((%f + t * (%f - %f)) / 255.0, 0.0, 0.0, 1.0);
}
)",
                      static_cast<double>(kWidth),
                      static_cast<double>(kRampLow),
                      static_cast<double>(kRampHigh),
                      static_cast<double>(kRampLow));

        ID3DBlob* vs_code   = nullptr;
        ID3DBlob* vs_errors = nullptr;
        const HRESULT vs_hr = D3DCompile(kVertexSource, std::strlen(kVertexSource), "vs",
                                          nullptr, nullptr, "main", "vs_4_0", 0, 0,
                                          &vs_code, &vs_errors);
        if (FAILED(vs_hr)) {
            report("vertex shader compile", vs_hr, vs_errors);
            if (vs_errors) vs_errors->Release();
            return false;
        }

        ID3D11VertexShader* vs = nullptr;
        HRESULT hr = device_->CreateVertexShader(vs_code->GetBufferPointer(),
                                                 vs_code->GetBufferSize(),
                                                 nullptr, &vs);
        vs_code->Release();
        if (FAILED(hr)) {
            report("vertex shader create", hr, nullptr);
            return false;
        }

        ID3DBlob* code   = nullptr;
        ID3DBlob* errors = nullptr;
        hr = D3DCompile(source, std::strlen(source), "paint", nullptr, nullptr, "main",
                        "ps_4_0", 0, 0, &code, &errors);
        if (FAILED(hr)) {
            report("pixel shader compile", hr, errors);
            if (errors) errors->Release();
            vs->Release();
            return false;
        }

        ID3D11PixelShader* ps = nullptr;
        hr = device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                                        nullptr, &ps);
        code->Release();
        if (FAILED(hr)) {
            report("pixel shader create", hr, nullptr);
            vs->Release();
            return false;
        }

        D3D11_VIEWPORT vp{};
        vp.Width    = static_cast<float>(kWidth);
        vp.Height   = static_cast<float>(kHeight);
        vp.MaxDepth = 1.0f;

        ID3D11RenderTargetView* rtvs[1] = { rtv_ };
        context_->OMSetRenderTargets(1, rtvs, nullptr);
        context_->RSSetViewports(1, &vp);
        context_->VSSetShader(vs, nullptr, 0);
        context_->PSSetShader(ps, nullptr, 0);
        context_->Draw(3, 0);
        // Unbind the render target: the encoder reads the texture, and
        // it must not be bound for writing at the same time.
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        context_->VSSetShader(nullptr, nullptr, 0);
        context_->PSSetShader(nullptr, nullptr, 0);
        ps->Release();
        vs->Release();
        return true;
    }

    ID3D11Device*        device()  const noexcept { return device_; }
    ID3D11DeviceContext* context() const noexcept { return context_; }
    ID3D11Texture2D*     texture() const noexcept { return texture_; }

    /** Copy the texture back to the CPU and hand back its BGRA bytes.
     *
     *  This is a *diagnostic* path only: the zero-copy encode never
     *  reads the pixels through the CPU.  It exists so that a failing
     *  test can say whether the picture was wrong before the encoder
     *  ever saw it, or whether the encoder lost it. */
    bool readback(std::vector<std::uint8_t>& out) {
        D3D11_TEXTURE2D_DESC desc{};
        texture_->GetDesc(&desc);
        desc.Usage          = D3D11_USAGE_STAGING;
        desc.BindFlags      = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags      = 0;

        ID3D11Texture2D* staging = nullptr;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging))) return false;

        context_->CopyResource(staging, texture_);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            staging->Release();
            return false;
        }

        out.assign(static_cast<std::size_t>(desc.Width) * desc.Height * 4u, 0);
        for (std::uint32_t row = 0; row < desc.Height; ++row) {
            std::memcpy(out.data() + static_cast<std::size_t>(row) * desc.Width * 4u,
                        static_cast<const std::uint8_t*>(mapped.pData) +
                            static_cast<std::size_t>(row) * mapped.RowPitch,
                        static_cast<std::size_t>(desc.Width) * 4u);
        }
        context_->Unmap(staging, 0);
        staging->Release();
        return true;
    }

private:
    /** Print why a D3D11 step failed.  Shader compilation failures are
     *  opaque otherwise, and the whole point of this test is to find
     *  out what the driver actually does. */
    static void report(const char* what, HRESULT hr, ID3DBlob* errors) {
        std::fprintf(stderr, "[zero-copy] %s failed: hr=0x%08lx\n", what,
                     static_cast<unsigned long>(hr));
        if (errors && errors->GetBufferSize() > 0) {
            std::fprintf(stderr, "[zero-copy] %s log:\n%s\n", what,
                         static_cast<const char*>(errors->GetBufferPointer()));
        }
    }

    ID3D11Device*           device_  = nullptr;
    ID3D11DeviceContext*    context_ = nullptr;
    ID3D11Texture2D*        texture_ = nullptr;
    ID3D11RenderTargetView* rtv_     = nullptr;
};

#endif // _WIN32

/** Build the encoder configuration the zero-copy test uses. */
plugins::VideoCodecConfig make_config() {
    plugins::VideoCodecConfig cfg;
    cfg.width             = kWidth;
    cfg.height            = kHeight;
    cfg.fps               = 30;
    cfg.bitrate_bps       = 2'000'000;
    cfg.keyframe_interval = 30;
    cfg.pixel_format      = plugins::VideoPixelFormat::kBGRA;
    cfg.codec             = plugins::VideoCodecKind::kH264;
    cfg.payload_type      = 102;
    return cfg;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// The zero-copy round trip
// ---------------------------------------------------------------------------

TEST(HwZeroCopy, EncodesAD3D11TextureWithoutACpuCopy) {
#if !defined(_WIN32)
    GTEST_SKIP() << "the zero-copy path is a D3D11 / NVENC path (Windows only)";
#else
    if (!nvcodec::nvenc_codec_available(nvcodec::NvCodec::kH264)) {
        GTEST_SKIP() << "NVENC is not available on this host";
    }
    if (!nvcodec::nvdec_codec_available(nvcodec::NvCodec::kH264)) {
        GTEST_SKIP() << "NVDEC is not available on this host";
    }

    // ---- 1. Paint a texture on the GPU ----------------------------------
    D3D11Painter painter;
    ASSERT_TRUE(painter.create(kWidth, kHeight))
        << "could not create the D3D11 device / texture";
    ASSERT_TRUE(painter.paint())
        << "the pixel shader failed to paint the texture";

    // The texture itself must hold the ramp, so that a flat decoded
    // picture can only be the encoder's fault.  This read-back is
    // *test-side* verification: the zero-copy encode below never reads
    // the pixels through the CPU.
    {
        std::vector<std::uint8_t> tex;
        ASSERT_TRUE(painter.readback(tex))
            << "could not read the painted texture back";
        const auto* bgra = tex.data();
        const std::size_t row_bytes = static_cast<std::size_t>(kWidth) * 4u;
        const std::size_t mid = static_cast<std::size_t>(kHeight / 2) * row_bytes;
        // The red channel rises from left to right; green and blue stay
        // at zero.  The tolerance absorbs the 8-bit rounding.
        const int red_left  = bgra[mid + 2];
        const int red_right = bgra[mid + 319 * 4 + 2];
        EXPECT_LT(red_left, red_right)
            << "the painted texture is not a ramp (red " << red_left
            << " -> " << red_right << ")";
        EXPECT_NEAR(red_left, static_cast<int>(kRampLow), 4);
        EXPECT_NEAR(red_right, static_cast<int>(kRampHigh), 4);
        EXPECT_EQ(bgra[mid + 1], 0) << "the green channel must stay zero";
        EXPECT_EQ(bgra[mid + 0], 0) << "the blue channel must stay zero";
    }

    // ---- 2. The encoder --------------------------------------------------
    const auto cfg = make_config();
    auto encoder = nvcodec::make_nvenc_codec(nvcodec::NvCodec::kH264, cfg);
    ASSERT_NE(encoder, nullptr) << "the NVENC backend failed to open";
    ASSERT_TRUE(encoder->supports_zero_copy_encode())
        << "the NVENC backend does not advertise the zero-copy path";

    // ---- 3. Hand the surface over ---------------------------------------
    //
    // The handle carries the *device* as well as the texture: the
    // encoder needs both, because NVENC registers the texture against
    // the session's device.
    video_frame::GpuHandle handle{};
    handle.backend = video_frame::GpuBackend::kWindowsD3D11Texture;
    handle.handle  = painter.texture();
    handle.device  = painter.device();

    video_frame::GpuBuffer gpu(handle, kWidth, kHeight,
                               video_frame::PixelFormat::kBGRA);

    plugins::EncodedVideoFrame encoded;
    const plugins::Status enc_rc = encoder->encode_zero_copy(gpu, encoded);
    ASSERT_EQ(enc_rc, plugins::kOk)
        << "the zero-copy encode failed (status=" << enc_rc << ")";
    ASSERT_GT(encoded.payload.size(), 0u)
        << "the zero-copy encode produced no bitstream";

    // The bitstream must be a real Annex-B access unit.  NVENC emits a
    // *four*-byte start code (`00 00 00 01`); the three-byte form
    // (`00 00 01`) is legal Annex-B too, so both are accepted.
    const auto* p = encoded.payload.data();
    ASSERT_GE(encoded.payload.size(), 4u);
    {
        const bool four = p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x00 && p[3] == 0x01;
        const bool three = p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x01;
        EXPECT_TRUE(four || three)
            << "the bitstream is not Annex-B framed (first bytes: "
            << static_cast<int>(p[0]) << " " << static_cast<int>(p[1]) << " "
            << static_cast<int>(p[2]) << " " << static_cast<int>(p[3]) << ")";
    }

    // ---- 4. The telemetry must say "zero-copy" -------------------------
    //
    // This is the *honest* part of B15: the counter is what makes the
    // claim measurable rather than asserted.
    const auto enc_stats = encoder->stats();
    EXPECT_EQ(enc_stats.zero_copy_frames, 1u);
    EXPECT_EQ(enc_stats.staged_frames, 0u);
    EXPECT_TRUE(enc_stats.zero_copy);
    EXPECT_EQ(enc_stats.frames_encoded, 1u);
    EXPECT_EQ(enc_stats.encode_errors, 0u);

    // ---- 5. Decode it and check the pixels ------------------------------
    auto decoder = nvcodec::make_nvdec_codec(nvcodec::NvCodec::kH264, cfg);
    ASSERT_NE(decoder, nullptr) << "the NVDEC backend failed to open";

    const auto layout = video_frame::compute_layout(
        video_frame::PixelFormat::kI420, kWidth, kHeight);
    std::vector<std::uint8_t> out_pixels(layout.buffer_size, 0);
    std::uint8_t* planes[3] = {
        out_pixels.data(),
        out_pixels.data() + layout.plane_offsets[1],
        out_pixels.data() + layout.plane_offsets[2],
    };

    plugins::VideoFrame decoded{};
    const plugins::Status dec_rc = decoder->decode(encoded, decoded, planes);
    ASSERT_EQ(dec_rc, plugins::kOk)
        << "the NVDEC decode failed (status=" << dec_rc << ")";

    // ---- 6. The chroma trends -------------------------------------------
    //
    // A ramp of pure red: the red-difference (V / Cr) rises along the
    // ramp, the blue-difference (U / Cb) falls.  Swapped red and blue
    // would invert both trends, and so would a picture that never came
    // from the texture at all.
    const auto* y_plane = planes[0];
    const auto* u_plane = planes[1];
    const auto* v_plane = planes[2];
    const auto  y_stride = static_cast<std::int32_t>(layout.strides[0]);
    const auto  u_stride = static_cast<std::int32_t>(layout.strides[1]);
    const auto  v_stride = static_cast<std::int32_t>(layout.strides[2]);

    // Sample a band of rows through the middle and average each column's
    // chroma over the 2x2 blocks, so the subsampling is accounted for.
    constexpr std::uint32_t kBandTop    = kHeight / 4;
    constexpr std::uint32_t kBandBottom = 3 * kHeight / 4;

    std::vector<double> u_col(kWidth, 0.0);
    std::vector<double> v_col(kWidth, 0.0);
    std::vector<double> y_col(kWidth, 0.0);
    std::size_t   chroma_samples = 0;
    std::size_t   luma_samples   = 0;

    for (std::uint32_t row = kBandTop; row < kBandBottom; ++row) {
        const auto* y_row = y_plane + static_cast<std::ptrdiff_t>(row) * y_stride;
        for (std::uint32_t col = 0; col < kWidth; ++col) {
            y_col[col] += y_row[col];
        }
        luma_samples++;

        if ((row % 2u) != 0u) continue;
        const auto* u_row = u_plane +
            static_cast<std::ptrdiff_t>(row / 2u) * u_stride;
        const auto* v_row = v_plane +
            static_cast<std::ptrdiff_t>(row / 2u) * v_stride;
        for (std::uint32_t col = 0; col < kWidth; col += 2u) {
            u_col[col / 2u] += u_row[col / 2u];
            v_col[col / 2u] += v_row[col / 2u];
            chroma_samples++;
        }
    }
    ASSERT_GT(chroma_samples, 0u);
    ASSERT_GT(luma_samples, 0u);

    // The chroma was accumulated once per 2x2 block row, so each entry
    // holds `luma_samples / 2` samples.
    const double chroma_n = static_cast<double>(chroma_samples) /
                            static_cast<double>(kWidth / 2u);
    const double luma_n   = static_cast<double>(luma_samples);

    const double u_left  = u_col[0] / chroma_n;
    const double u_right = u_col[kWidth / 2u - 1u] / chroma_n;
    const double v_left  = v_col[0] / chroma_n;
    const double v_right = v_col[kWidth / 2u - 1u] / chroma_n;

    const double y_left  = y_col[0] / luma_n;
    const double y_right = y_col[kWidth - 1u] / luma_n;

    // The luma must rise along the ramp: the picture is not flat.
    EXPECT_GT(y_right, y_left + 5.0)
        << "the decoded luma does not follow the painted ramp "
           "(left=" << y_left << ", right=" << y_right << ")";

    // The red-difference must RISE along a ramp of red...
    EXPECT_GT(v_right, v_left + 15.0)
        << "the decoded red-difference does not rise along a ramp of red — "
           "the red and the blue channels are probably swapped "
           "(V left=" << v_left << ", right=" << v_right << ")";

    // ...and the blue-difference must FALL.
    EXPECT_LT(u_right, u_left - 8.0)
        << "the decoded blue-difference does not fall along a ramp of red — "
           "the red and the blue channels are probably swapped "
           "(U left=" << u_left << ", right=" << u_right << ")";
#endif
}

// ---------------------------------------------------------------------------
// The device contract.
//
// The driver binds a registered resource to the session's device, so a
// surface from a *different* device cannot be registered.  The encoder
// therefore re-opens its session on the producer's device.
//
// This test pins that behaviour: a surface from a second device object
// is accepted (because the encoder re-opens), and the frame is encoded.
// ---------------------------------------------------------------------------

TEST(HwZeroCopy, AcceptsASurfaceFromADifferentDeviceObject) {
#if !defined(_WIN32)
    GTEST_SKIP() << "the zero-copy path is a D3D11 / NVENC path (Windows only)";
#else
    if (!nvcodec::nvenc_codec_available(nvcodec::NvCodec::kH264)) {
        GTEST_SKIP() << "NVENC is not available on this host";
    }
    if (!nvcodec::nvdec_codec_available(nvcodec::NvCodec::kH264)) {
        GTEST_SKIP() << "NVDEC is not available on this host";
    }

    // Two *distinct* device objects on the same adapter.
    D3D11Painter first;
    ASSERT_TRUE(first.create(kWidth, kHeight));
    D3D11Painter second;
    ASSERT_TRUE(second.create(kWidth, kHeight));
    ASSERT_NE(first.device(), second.device())
        << "the test needs two distinct device objects";

    const auto cfg = make_config();
    auto encoder = nvcodec::make_nvenc_codec(nvcodec::NvCodec::kH264, cfg);
    ASSERT_NE(encoder, nullptr);

    // Feed a surface from the *second* device.  The encoder has to
    // re-open its session on it; the frame must still be encoded.
    ASSERT_TRUE(second.paint());

    video_frame::GpuHandle handle{};
    handle.backend = video_frame::GpuBackend::kWindowsD3D11Texture;
    handle.handle  = second.texture();
    handle.device  = second.device();

    video_frame::GpuBuffer gpu(handle, kWidth, kHeight,
                               video_frame::PixelFormat::kBGRA);

    plugins::EncodedVideoFrame encoded;
    const plugins::Status rc = encoder->encode_zero_copy(gpu, encoded);
    ASSERT_EQ(rc, plugins::kOk)
        << "a surface from a second device object was rejected (status="
        << rc << ")";
    EXPECT_GT(encoded.payload.size(), 0u);

    const auto enc_stats = encoder->stats();
    EXPECT_EQ(enc_stats.zero_copy_frames, 1u);
    EXPECT_EQ(enc_stats.encode_errors, 0u);
#endif
}

// ---------------------------------------------------------------------------
// The surface's geometry.
//
// A surface whose size differs from the encoder's geometry forces the
// session to be re-initialized.  The picture must still be encoded.
// ---------------------------------------------------------------------------

TEST(HwZeroCopy, HandlesASurfaceWhoseSizeDiffers) {
#if !defined(_WIN32)
    GTEST_SKIP() << "the zero-copy path is a D3D11 / NVENC path (Windows only)";
#else
    if (!nvcodec::nvenc_codec_available(nvcodec::NvCodec::kH264)) {
        GTEST_SKIP() << "NVENC is not available on this host";
    }

    // The encoder is configured for one size ...
    const auto cfg = make_config();
    auto encoder = nvcodec::make_nvenc_codec(nvcodec::NvCodec::kH264, cfg);
    ASSERT_NE(encoder, nullptr);

    // ... and the surface is a different one.
    constexpr std::uint32_t kOtherWidth  = 640;
    constexpr std::uint32_t kOtherHeight = 480;

    D3D11Painter painter;
    ASSERT_TRUE(painter.create(kOtherWidth, kOtherHeight));
    ASSERT_TRUE(painter.paint());

    video_frame::GpuHandle handle{};
    handle.backend = video_frame::GpuBackend::kWindowsD3D11Texture;
    handle.handle  = painter.texture();
    handle.device  = painter.device();

    video_frame::GpuBuffer gpu(handle, kOtherWidth, kOtherHeight,
                               video_frame::PixelFormat::kBGRA);

    plugins::EncodedVideoFrame encoded;
    const plugins::Status rc = encoder->encode_zero_copy(gpu, encoded);
    ASSERT_EQ(rc, plugins::kOk)
        << "a surface whose size differs from the encoder's was rejected "
           "(status=" << rc << ")";
    EXPECT_GT(encoded.payload.size(), 0u);
#endif
}

// ---------------------------------------------------------------------------
// The honesty of the capability flag.
//
// A codec that cannot take a surface must say so.  The flag is what the
// engine consults before it hands a GPU frame over, so a wrong `true`
// would make the engine drop frames it could have encoded.
// ---------------------------------------------------------------------------

TEST(HwZeroCopy, TheEncoderAdvertisesTheZeroCopyPath) {
    if (!nvcodec::nvenc_codec_available(nvcodec::NvCodec::kH264)) {
        GTEST_SKIP() << "NVENC is not available on this host";
    }
    const auto cfg = make_config();
    auto encoder = nvcodec::make_nvenc_codec(nvcodec::NvCodec::kH264, cfg);
    ASSERT_NE(encoder, nullptr);
    EXPECT_TRUE(encoder->supports_zero_copy_encode());
    EXPECT_FALSE(encoder->supports_zero_copy_decode())
        << "NVDEC does not expose a GPU-output path in this tree";
}
