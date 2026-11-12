/**
 * @file tests/nvenc_probe.cpp
 * @brief Standalone NVENC H.264 encoder smoke test for NVIDIA GPUs.
 *
 * Talks to nvEncodeAPI64.dll via LoadLibrary + GetProcAddress (no .lib
 * needed).  Opens a D3D11 device, opens an NVENC session, calls
 * nvEncInitializeEncoder, and exits.
 *
 * This is the *low-level* driver probe: it exercises the raw NVENC ABI
 * with nothing but the NVIDIA headers, so it can tell "the driver is
 * broken / the ABI does not match" apart from "the NimRTC encoder has a
 * bug".  The end-to-end proof (encode frames, decode them back) lives in
 * `src/modules/nvcodec/tests/test_nvcodec_roundtrip.cpp`.
 *
 * ABI note
 * --------
 * The probe builds against the *vendored* Video Codec SDK 13.1 header
 * pack (`src/third_party/nv_codec_headers/`), which is the ABI NimRTC
 * itself compiles against (see `src/third_party/vendor.json`).  It must
 * therefore use the header's own `NV_ENC_*_VER` constants and a real
 * `NV_ENC_CONFIG` — no hand-patched struct-version magic.  An earlier
 * revision of this probe forced the SDK-11.1.5 struct layout
 * (`0x0100000B | (7u << 16) | ...`) to work around a hybrid-ABI driver;
 * with the SDK-13 headers the driver rejects that layout with
 * `NV_ENC_ERR_INVALID_VERSION` (status 15), so the hack had to go.
 *
 * Exit codes:
 *   0 = success (DLL loaded, function table populated, session opened,
 *       encoder initialized)
 *   1 = the driver was found but rejected the encoder initialization
 *  77 = no GPU / driver issue, or SDK not configured
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
  // Use Win10+ targeting for modern struct packing.
  #ifndef _WIN32_WINNT
  #define _WIN32_WINNT 0x0A00
  #endif
  #include <windows.h>
  #include <d3d11.h>
  #include <nvEncodeAPI.h>
#endif

typedef NVENCSTATUS(NVENCAPI* PfnNvEncodeAPICreateInstance)(
    NV_ENCODE_API_FUNCTION_LIST*);

/** Build the NV_ENC_CONFIG the probe initializes the encoder with.
 *
 *  Mirrors what the NimRTC encoder does (src/modules/nvcodec/src/
 *  nvenc_encoder.cpp): VBR rate control, one temporal layer, no
 *  lookahead, H.264 Main profile. */
static NV_ENC_CONFIG build_config() {
    NV_ENC_CONFIG cfg{};
    cfg.version        = NV_ENC_CONFIG_VER;
    cfg.gopLength      = 30;
    cfg.frameIntervalP = 1;   // IPP — no B-frames
    cfg.mvPrecision    = NV_ENC_MV_PRECISION_DEFAULT;
    cfg.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;

    NV_ENC_RC_PARAMS& rc = cfg.rcParams;
    rc.version         = NV_ENC_RC_PARAMS_VER;
    rc.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    rc.averageBitRate  = 1'000'000;
    rc.maxBitRate      = 1'500'000;
    rc.vbvBufferSize   = 500'000;
    rc.vbvInitialDelay = rc.vbvBufferSize;
    rc.minQP = NV_ENC_QP{0};
    rc.maxQP = NV_ENC_QP{51};
    rc.initialRCQP = NV_ENC_QP{26};
    rc.enableMinQP = 0;
    rc.enableMaxQP = 0;
    rc.enableInitialRCQP = 1;
    rc.enableAQ = 0;
    rc.enableLookahead = 0;

    cfg.profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
    NV_ENC_CONFIG_H264& h264 = cfg.encodeCodecConfig.h264Config;
    h264.level              = NV_ENC_LEVEL_AUTOSELECT;
    h264.idrPeriod          = cfg.gopLength;
    h264.repeatSPSPPS       = 1;
    h264.chromaFormatIDC    = 1;   // 4:2:0
    h264.maxNumRefFrames    = 1;
    h264.sliceMode          = 0;
    h264.sliceModeData      = 0;
    h264.outputBitDepth     = NV_ENC_BIT_DEPTH_8;
    h264.inputBitDepth      = NV_ENC_BIT_DEPTH_8;
    h264.numTemporalLayers  = 1;
    h264.spsId              = 0;
    h264.ppsId              = 0;
    h264.entropyCodingMode  = NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;
    return cfg;
}

int main() {
#if !defined(_WIN32)
    return 77;
#else
    HMODULE dll = LoadLibraryA("nvEncodeAPI64.dll");
    if (!dll) {
        std::fprintf(stderr, "nvenc_probe: LoadLibrary(nvEncodeAPI64.dll) failed\n");
        return 77;
    }
    auto createInstance = (PfnNvEncodeAPICreateInstance)
        GetProcAddress(dll, "NvEncodeAPICreateInstance");
    if (!createInstance) {
        std::fprintf(stderr, "nvenc_probe: NvEncodeAPICreateInstance export missing\n");
        FreeLibrary(dll);
        return 77;
    }

    NV_ENCODE_API_FUNCTION_LIST fnList{};
    fnList.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS s = createInstance(&fnList);
    if (s != NV_ENC_SUCCESS || !fnList.nvEncOpenEncodeSessionEx) {
        std::fprintf(stderr, "nvenc_probe: NvEncodeAPICreateInstance failed (status=%u)\n",
                     (unsigned)s);
        FreeLibrary(dll);
        return 77;
    }
    std::fprintf(stderr, "nvenc_probe: NvEncodeAPICreateInstance OK; function table populated\n");

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr) || !dev) {
        std::fprintf(stderr, "nvenc_probe: D3D11CreateDevice failed (hr=0x%08lX)\n",
                     (unsigned long)hr);
        FreeLibrary(dll);
        return 77;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
    open.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.device     = dev;
    open.apiVersion = NVENCAPI_VERSION;

    void* enc = nullptr;
    s = fnList.nvEncOpenEncodeSessionEx(&open, &enc);
    if (s != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "nvenc_probe: nvEncOpenEncodeSessionEx failed (status=%u)\n",
                     (unsigned)s);
        std::fprintf(stderr, "  -- see docs/hw_plugin_seam.md for diagnosis --\n");
        dev->Release();
        FreeLibrary(dll);
        return (s == NV_ENC_ERR_INVALID_VERSION) ? 1 : 77;
    }
    std::fprintf(stderr, "nvenc_probe: NVENC session opened\n");

    NV_ENC_CONFIG enc_cfg = build_config();

    NV_ENC_INITIALIZE_PARAMS init{};
    init.version      = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID   = NV_ENC_CODEC_H264_GUID;
    init.presetGUID   = NV_ENC_PRESET_P4_GUID;
    init.encodeWidth  = 320;
    init.encodeHeight = 240;
    init.darWidth     = 320;
    init.darHeight    = 240;
    init.frameRateNum = 30;
    init.frameRateDen = 1;
    init.enablePTD    = 1;
    init.tuningInfo   = NV_ENC_TUNING_INFO_LOW_LATENCY;
    init.encodeConfig = &enc_cfg;

    s = fnList.nvEncInitializeEncoder(enc, &init);
    if (s != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "nvenc_probe: nvEncInitializeEncoder failed (status=%u)\n",
                     (unsigned)s);
        fnList.nvEncDestroyEncoder(enc);
        dev->Release();
        FreeLibrary(dll);
        return 1;
    }
    std::fprintf(stderr, "nvenc_probe: encoder initialized 320x240 @ 30 fps (preset P4)\n");

    // The probe stops here on purpose: it verifies the *driver ABI*, not
    // the encode path.  The full NVENC -> NVDEC round trip (including the
    // bitstream and the NV12 output path) lives in
    // src/modules/nvcodec/tests/test_nvcodec_roundtrip.cpp.
    std::fprintf(stderr,
        "nvenc_probe: encode loop SKIPPED (this probe checks the driver ABI only)\n");

    fnList.nvEncDestroyEncoder(enc);
    dev->Release();
    if (ctx) ctx->Release();
    FreeLibrary(dll);
    return 0;
#endif
}
