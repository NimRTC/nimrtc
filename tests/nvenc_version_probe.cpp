/**
 * @file tests/nvenc_version_probe.cpp
 *
 * Probes nvEncGetEncodePresetConfig with different preset-config struct versions.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
  #include <d3d11.h>
  #include <nvEncodeAPI.h>
#endif

typedef NVENCSTATUS(NVENCAPI* PfnNvEncodeAPICreateInstance)(
    NV_ENCODE_API_FUNCTION_LIST*);

int main() {
#if !defined(_WIN32)
    return 77;
#else
    using namespace std;

    // 1. Load DLL + get function table
    HMODULE dll = LoadLibraryA("nvEncodeAPI64.dll");
    if(!dll) {
        fprintf(stderr, "LoadLibrary failed\n");
        return 77;
    }
    auto createInstance = (PfnNvEncodeAPICreateInstance)
        GetProcAddress(dll, "NvEncodeAPICreateInstance");
    if(!createInstance) {
        fprintf(stderr, "NvEncodeAPICreateInstance not found\n");
        FreeLibrary(dll);
        return 77;
    }

    NV_ENCODE_API_FUNCTION_LIST fnList{};
    fnList.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS s = createInstance(&fnList);
    if(s != NV_ENC_SUCCESS) {
        fprintf(stderr, "NvEncodeAPICreateInstance failed (status=%u)\n", (unsigned)s);
        FreeLibrary(dll);
        return 77;
    }
    fprintf(stderr, "FnList populated. version=0x%08X\n", fnList.version);

    // 2. D3D11 device
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if(FAILED(hr) || !dev) {
        fprintf(stderr, "D3D11CreateDevice failed (hr=0x%08lX)\n", (unsigned long)hr);
        FreeLibrary(dll);
        return 77;
    }

    // 3. Open session with SDK 13.1 API version
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams{};
    openParams.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    openParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    openParams.device     = dev;
    openParams.apiVersion = NVENCAPI_VERSION;  // 0x0100000D

    void* enc = nullptr;
    s = fnList.nvEncOpenEncodeSessionEx(&openParams, &enc);
    if(s != NV_ENC_SUCCESS) {
        fprintf(stderr, "nvEncOpenEncodeSessionEx failed (status=%u)\n", (unsigned)s);
        dev->Release();
        FreeLibrary(dll);
        return 77;
    }
    fprintf(stderr, "Session opened. NVENCAPI_VERSION=0x%08X\n", NVENCAPI_VERSION);

    // 4. Print function table pointers
    fprintf(stderr, "  fnList.nvEncGetEncodePresetConfig = %p\n",
            (void*)fnList.nvEncGetEncodePresetConfig);
    fprintf(stderr, "  fnList.nvEncInitializeEncoder    = %p\n",
            (void*)fnList.nvEncInitializeEncoder);
    fprintf(stderr, "  fnList.nvEncGetEncodeCaps        = %p\n",
            (void*)fnList.nvEncGetEncodeCaps);
    fprintf(stderr, "  fnList.nvEncGetEncodePresetCount = %p\n",
            (void*)fnList.nvEncGetEncodePresetCount);
    fprintf(stderr, "  fnList.nvEncGetEncodePresetGUIDs = %p\n",
            (void*)fnList.nvEncGetEncodePresetGUIDs);
    fprintf(stderr, "  fnList.nvEncDestroyEncoder       = %p\n",
            (void*)fnList.nvEncDestroyEncoder);

    // 5. Try nvEncGetEncodeCaps (doesn't need preset config)
    {
        NV_ENC_CAPS_PARAM cp{};
        cp.version = NV_ENC_CAPS_PARAM_VER;
        int val = 0;
        s = fnList.nvEncGetEncodeCaps(enc, NV_ENC_CODEC_H264_GUID,
                                      &cp, &val);
        fprintf(stderr, "  nvEncGetEncodeCaps(WIDTH_MAX): status=%u val=%d\n",
                (unsigned)s, val);
    }

    // 6. Try nvEncGetEncodePresetCount
    {
        uint32_t count = 999;
        s = fnList.nvEncGetEncodePresetCount(enc, NV_ENC_CODEC_H264_GUID, &count);
        fprintf(stderr, "  nvEncGetEncodePresetCount: status=%u count=%u\n",
                (unsigned)s, count);
    }

    // 7. Try nvEncGetEncodePresetGUIDs
    {
        GUID guids[16] = {};
        uint32_t n = 0;
        s = fnList.nvEncGetEncodePresetGUIDs(enc, NV_ENC_CODEC_H264_GUID,
                                            guids, 16, &n);
        fprintf(stderr, "  nvEncGetEncodePresetGUIDs: status=%u n=%u\n",
                (unsigned)s, n);
        // Try P4 specifically
        for(uint32_t i = 0; i < n; ++i) {
            fprintf(stderr, "    preset[%u]: %08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X\n",
                    i,
                    guids[i].Data1, guids[i].Data2, guids[i].Data3,
                    guids[i].Data4[0], guids[i].Data4[1],
                    guids[i].Data4[2], guids[i].Data4[3],
                    guids[i].Data4[4], guids[i].Data4[5],
                    guids[i].Data4[6], guids[i].Data4[7]);
        }
    }

    // 8. Probe nvEncGetEncodePresetConfig with different struct versions
    struct ProbeEntry {
        uint32_t presetCfgVer;
        uint32_t presetInnerVer;
        const char* desc;
    };
    std::vector<ProbeEntry> candidates;

    // SDK 13.1 header default
    candidates.push_back({0x8705000Du, 0x8709000Du, "SDK13.1 sv5/9"});

    // Try struct versions 1..9 for major versions 11,12,13 with (0x7<<28) flag
    for(int major = 13; major >= 11; --major) {
        for(int ps = 1; ps <= 9; ++ps) {
            for(int cs = 1; cs <= 9; ++cs) {
                uint32_t pv = (0x7u<<28) | (ps<<16) | (major<<0);
                uint32_t iv = (0x7u<<28) | (cs<<16) | (major<<0);
                char buf[24];
                sprintf(buf, "f7_%d sv%d/%d", major, ps, cs);
                candidates.push_back({pv, iv, buf});
            }
        }
    }

    // Try (1<<31) flag style
    for(int major = 13; major >= 11; --major) {
        for(int ps = 1; ps <= 9; ++ps) {
            for(int cs = 1; cs <= 9; ++cs) {
                uint32_t pv = (1u<<31) | (ps<<16) | (major<<0);
                uint32_t iv = (1u<<31) | (cs<<16) | (major<<0);
                char buf[24];
                sprintf(buf, "f31_%d sv%d/%d", major, ps, cs);
                candidates.push_back({pv, iv, buf});
            }
        }
    }

    fprintf(stderr, "Trying %zu preset config version combinations...\n", candidates.size());

    bool found = false;
    for(const auto& cand : candidates) {
        NV_ENC_PRESET_CONFIG pc{};
        pc.version           = cand.presetCfgVer;
        pc.presetCfg.version = cand.presetInnerVer;

        s = fnList.nvEncGetEncodePresetConfig(enc, NV_ENC_CODEC_H264_GUID,
                                              NV_ENC_PRESET_P4_GUID, &pc);
        if(s == NV_ENC_SUCCESS) {
            fprintf(stdout,
                "SUCCESS: presetCfgVer=0x%08X presetInnerVer=0x%08X (%s)\n"
                "  Update nvenc_probe.cpp with these versions.\n",
                cand.presetCfgVer, cand.presetInnerVer, cand.desc);
            found = true;
            break;
        }
    }

    fnList.nvEncDestroyEncoder(enc);
    dev->Release();
    FreeLibrary(dll);

    if(!found) {
        fprintf(stderr, "\nNo preset config struct version worked.\n");
        return 77;
    }
    return 0;
#endif
}
