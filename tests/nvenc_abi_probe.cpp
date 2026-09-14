/**
 * @file tests/nvenc_abi_probe.cpp
 *
 * Systematic ABI probe: for every combination of (session apiVersion,
 * NV_ENC_INITIALIZE_PARAMS_VER, NV_ENC_CONFIG_VER) — picking the type ID
 * from {0x0B (SDK 11.x), 0x0C (SDK 12.x), 0x0D (SDK 13.x)} and the
 * struct sub-version that maps to "minimum" vs "current" — open a
 * session and try to call nvEncInitializeEncoder with a minimal valid
 * config.  Reports the first combo that succeeds, plus a per-combo log.
 *
 * Exit codes:
 *   0 = some combination succeeded
 *   1 = every combination failed with INVALID_PARAM
 *  77 = DLL/device missing — skip
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
  #ifndef _WIN32_WINNT
  #define _WIN32_WINNT 0x0A00
  #endif
  #include <windows.h>
  #include <d3d11.h>
  #include <nvEncodeAPI.h>
#endif

typedef NVENCSTATUS(NVENCAPI* PfnNvEncodeAPICreateInstance)(
    NV_ENCODE_API_FUNCTION_LIST*);

static const char* status_name(NVENCSTATUS s) {
    switch(s) {
        case NV_ENC_SUCCESS:                       return "OK";
        case NV_ENC_ERR_NO_ENCODE_DEVICE:          return "NO_ENCODE_DEVICE";
        case NV_ENC_ERR_INVALID_PARAM:             return "INVALID_PARAM";
        case NV_ENC_ERR_INVALID_VERSION:           return "INVALID_VERSION";
        case NV_ENC_ERR_UNSUPPORTED_PARAM:         return "UNSUPPORTED_PARAM";
        default:                                   return "OTHER";
    }
}

// Make a struct version: type ID in bits 0-15, struct sub-ver in bits 16-23,
// SDK minor version in bits 24-27, (0x7<<28) high nibble, optional (1u<<31)
// flag.  Layout mirrors NVENCAPI_STRUCT_VERSION(ver) | (1u<<31) from the SDK.
static uint32_t make_ver(uint16_t type_id, uint8_t sub_ver, bool high_bit) {
    uint32_t v = type_id;
    v |= (uint32_t)sub_ver << 16;
    v |= (uint32_t)0x1u << 24;          // NVENCAPI_MINOR_VERSION = 1
    v |= 0x7u << 28;
    if (high_bit) v |= 1u << 31;
    return v;
}

int main() {
#if !defined(_WIN32)
    return 77;
#else
    HMODULE dll = LoadLibraryA("nvEncodeAPI64.dll");
    if (!dll) {
        fprintf(stderr, "abi_probe: LoadLibrary failed\n");
        return 77;
    }
    auto createInstance = (PfnNvEncodeAPICreateInstance)
        GetProcAddress(dll, "NvEncodeAPICreateInstance");
    if (!createInstance) {
        fprintf(stderr, "abi_probe: NvEncodeAPICreateInstance missing\n");
        FreeLibrary(dll);
        return 77;
    }

    NV_ENCODE_API_FUNCTION_LIST fnList{};
    fnList.version = NVENCAPI_STRUCT_VERSION(2);
    NVENCSTATUS s = createInstance(&fnList);
    if (s != NV_ENC_SUCCESS || !fnList.nvEncOpenEncodeSessionEx ||
        !fnList.nvEncInitializeEncoder || !fnList.nvEncDestroyEncoder) {
        fprintf(stderr, "abi_probe: createInstance failed (%s)\n", status_name(s));
        FreeLibrary(dll);
        return 77;
    }
    fprintf(stderr, "abi_probe: FnList.version=0x%08X (type=0x%02X subVer=%u)\n",
            fnList.version, fnList.version & 0xFFFF, (fnList.version >> 16) & 0xFF);

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr) || !dev) {
        fprintf(stderr, "abi_probe: D3D11CreateDevice hr=0x%08lX\n", (unsigned long)hr);
        FreeLibrary(dll);
        return 77;
    }

    // Try multiple apiVersion × initVersion × cfgVersion combos.
    // (apiVersion, initVersion, cfgVersion) — listed as type IDs.
    struct Combo {
        std::string label;
        uint32_t api_version;
        uint32_t open_session_ver;
        uint32_t init_ver;
        uint32_t cfg_ver;
    };
    std::vector<Combo> combos;
    for (uint16_t api_type : { (uint16_t)0x0Bu, (uint16_t)0x0Cu, (uint16_t)0x0Du }) {
        for (uint16_t init_type : { (uint16_t)0x0Bu, (uint16_t)0x0Cu, (uint16_t)0x0Du }) {
            for (uint16_t cfg_type : { (uint16_t)0x0Bu, (uint16_t)0x0Cu, (uint16_t)0x0Du }) {
                for (uint8_t init_sub : { (uint8_t)5, (uint8_t)7 }) {
                    for (uint8_t cfg_sub : { (uint8_t)7, (uint8_t)9 }) {
                        char buf[64];
                        std::snprintf(buf, sizeof(buf),
                            "api=0x%X init[t%X s%u] cfg[t%X s%u]",
                            api_type, init_type, init_sub, cfg_type, cfg_sub);
                        Combo c;
                        // NVENCAPI_VERSION layout:
                        //   bits  0-15 : SDK major version (== NVENCAPI_MAJOR_VERSION == type ID)
                        //   bits 16-23 : NVENCAPI_STRUCT_VERSION compatibility marker (0x7)
                        //   bits 24-31 : SDK minor version (typically 1)
                        // So full apiVersion for SDK 11.x = 0x01000000 | type.
                        c.label            = buf;
                        c.api_version      = ((uint32_t)api_type) | 0x01000000u;
                        c.open_session_ver = NVENCAPI_STRUCT_VERSION(1);
                        c.init_ver         = make_ver(init_type, init_sub, true);
                        c.cfg_ver          = make_ver(cfg_type, cfg_sub, true);
                        combos.push_back(c);
                    }
                }
            }
        }
    }

    int total = (int)combos.size();
    int success_count = 0;
    fprintf(stderr, "abi_probe: trying %d combinations\n", total);
    fprintf(stderr, "  FnList.version=0x%08X NVENCAPI_VERSION=0x%08X NVENCAPI_STRUCT_VERSION(1)=0x%08X\n",
            fnList.version, NVENCAPI_VERSION, NVENCAPI_STRUCT_VERSION(1));

    for (auto& c : combos) {
        // Open a fresh session for each attempt (so we don't carry state).
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
        open.version    = c.open_session_ver;
        open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        open.device     = dev;
        open.apiVersion = c.api_version;
        void* enc = nullptr;
        s = fnList.nvEncOpenEncodeSessionEx(&open, &enc);
        if (s != NV_ENC_SUCCESS) {
            fprintf(stderr, "  [session fail %s] apiVer=0x%08X status=%u(%s)\n",
                    c.label.c_str(), c.api_version, (unsigned)s, status_name(s));
            continue;
        }

        NV_ENC_CONFIG cfg{};
        cfg.version = c.cfg_ver;
        cfg.gopLength = 30;
        cfg.frameIntervalP = 1;
        cfg.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
        cfg.encodeCodecConfig.h264Config.idrPeriod = 30;

        NV_ENC_INITIALIZE_PARAMS init{};
        init.version      = c.init_ver;
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
        init.encodeConfig = &cfg;

        s = fnList.nvEncInitializeEncoder(enc, &init);
        if (s == NV_ENC_SUCCESS) {
            fprintf(stderr, "  [SUCCESS %s] init=0x%08X cfg=0x%08X\n",
                    c.label.c_str(), (unsigned)init.version, (unsigned)cfg.version);
            success_count++;
            fnList.nvEncDestroyEncoder(enc);
            enc = nullptr;
        } else {
            fprintf(stderr, "  [%s %s] init=0x%08X cfg=0x%08X\n",
                    status_name(s), c.label.c_str(),
                    (unsigned)init.version, (unsigned)cfg.version);
            // Always destroy the (failed) encoder so the next combo gets a
            // fresh handle.  Failure on nvEncInitializeEncoder still
            // leaves the handle in an undefined state.
            fnList.nvEncDestroyEncoder(enc);
            enc = nullptr;
        }
    }

    fprintf(stderr, "\nabi_probe: %d / %d combinations succeeded\n",
            success_count, total);

    if (dev) dev->Release();
    if (ctx) ctx->Release();
    FreeLibrary(dll);
    return success_count > 0 ? 0 : 1;
#endif
}
