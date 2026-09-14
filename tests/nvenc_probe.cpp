/**
 * @file tests/nvenc_probe.cpp
 * @brief Standalone NVENC H.264 encoder smoke test for NVIDIA GPUs.
 *
 * Talks to nvEncodeAPI64.dll via LoadLibrary + GetProcAddress (no .lib
 * needed).  Opens a D3D11 device, opens an NVENC session, calls
 * nvEncInitializeEncoder, and exits.
 *
 * Driver 616.92 (Game Ready / Studio 616.92, RTX 3060) has a hybrid ABI:
 * it accepts SDK 11.1.5.4 NVENCAPI_VERSION + init.encodeConfig = nullptr
 * but rejects every NV_ENC_PIC_PARAMS layout (sub-ver 4-7 with type 0x0B
 * return INVALID_PARAM + corrupt the heap; type 0x0C/0x0D return
 * INVALID_VERSION).  See docs/hw_plugin_seam.md ?2.1.
 *
 * So this probe stops right after nvEncInitializeEncoder succeeds ?
 * it's the most we can verify with the SDK 11.1.5.4 headers without
 * triggering the encode-path crash.
 *
 * Compile: cl /std:c++17 /EHsc nvenc_probe.cpp /link d3d11.lib
 *
 * Exit codes:
 *   0 = success (DLL loaded, function table populated, session opened,
 *       encoder initialized)
 *   1 = encode path attempted but failed (skipped on this build)
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
  // SDK 11.1.5 type ID 0x0B + (1u<<31) for the init-time structs.
  #undef  NV_ENC_PRESET_CONFIG_VER
  #define NV_ENC_PRESET_CONFIG_VER    (0x0100000Bu | (4u << 16) | (0x7u << 28) | (1u << 31))
  #undef  NV_ENC_CONFIG_VER
  #define NV_ENC_CONFIG_VER           (0x0100000Bu | (7u << 16) | (0x7u << 28) | (1u << 31))
  #undef  NV_ENC_INITIALIZE_PARAMS_VER
  #define NV_ENC_INITIALIZE_PARAMS_VER (0x0100000Bu | (5u << 16) | (0x7u << 28) | (1u << 31))
  #undef  NV_ENC_PIC_PARAMS_VER
  #define NV_ENC_PIC_PARAMS_VER       (0x0100000Bu | (4u << 16) | (0x7u << 28) | (1u << 31))
#endif

typedef NVENCSTATUS(NVENCAPI* PfnNvEncodeAPICreateInstance)(
    NV_ENCODE_API_FUNCTION_LIST*);

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

    // Driver 616.92 quirk: any hand-built NV_ENC_CONFIG passed via
    // init.encodeConfig causes nvEncInitializeEncoder to return
    // NV_ENC_ERR_INVALID_PARAM no matter what struct version we use.
    // Only init.encodeConfig = nullptr works ? the driver derives
    // everything from presetGUID + tuningInfo.
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
    init.encodeConfig = nullptr;

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

    // Driver 616.92 rejects every NV_ENC_PIC_PARAMS layout (SDK 11.x
    // returns INVALID_PARAM + corrupts heap; SDK 12.x/13.x returns
    // INVALID_VERSION).  We deliberately stop here instead of trying
    // to encode.  See docs/hw_plugin_seam.md ?2.1.
    std::fprintf(stderr,
        "nvenc_probe: encode loop SKIPPED (hybrid ABI: init OK, encode rejects)\n");

    fnList.nvEncDestroyEncoder(enc);
    dev->Release();
    if (ctx) ctx->Release();
    FreeLibrary(dll);
    return 0;
#endif
}


