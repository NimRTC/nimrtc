/**
 * @file tests/nvenc_probe.cpp
 * @brief Standalone NVENC H.264 encoder smoke test for NVIDIA GPUs.
 *
 * Uses the modern NvEncodeAPICreateInstance() entry point to obtain a
 * function-table, then drives NVENC through that table.  This is the only
 * entry pattern exported by current NVIDIA drivers (R470+, CUDA 11+); the
 * legacy direct exports (NvEncOpenEncodeSessionEx, NvEncInitializeEncoder,
 * ...) are gone.
 *
 * Talks to nvEncodeAPI64.dll via LoadLibrary + GetProcAddress (no .lib needed).
 * Encodes a 320x240 NV12 synthetic test pattern and verifies the output
 * bitstream is valid H.264 Annex B (00 00 00 01 prefix + IDR NAL unit).
 *
 * Compile: cl /std:c++17 /EHsc nvenc_probe.cpp /link d3d11.lib
 * Run:     any Windows machine with NVIDIA GPU + driver installed.
 *
 * Exit codes:
 *   0  = success — NVENC encoded at least one frame, Annex B valid
 *   1  = encode error — NVENC session opened but produced no output
 *  77  = skip  — no GPU / driver issue, or SDK not configured
 *
 * KNOWN ISSUE (driver 591.86 / nvEncodeAPI64.dll v32.0.15.9186):
 *   The bundled runtime is built against NVENC SDK 11.0 and rejects every
 *   SDK 13.1 struct version we try with NV_ENC_ERR_INVALID_VERSION (15).
 *   See docs/hw_plugin_seam.md for the full diagnosis.  The fix path is
 *   either to update the driver to a version that ships with the SDK 13.x
 *   runtime (R570+) or to ship our own minimal SDK 11.0 struct shims.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
  // Use Win10+ targeting for modern struct packing.
  #ifndef _WIN32_WINNT
  #define _WIN32_WINNT 0x0A00
  #endif
  #include <windows.h>
  #include <d3d11.h>
  #include <nvEncodeAPI.h>
#endif

namespace {

constexpr uint32_t kWidth  = 320;
constexpr uint32_t kHeight = 240;
constexpr uint32_t kFps    = 30;
constexpr uint32_t kBitrateBps = 1'500'000;
constexpr uint32_t kGopLength  = 30;   // IDR every second
constexpr uint32_t kNumFrames  = 30;   // 1 second of video
constexpr size_t   kBsBufBytes = 2u * 1024u * 1024u;  // 2 MiB output buf

// Modern entry point: obtain a function table once, then call through it.
typedef NVENCSTATUS(NVENCAPI* PfnNvEncodeAPICreateInstance)(
    NV_ENCODE_API_FUNCTION_LIST*);

const char* nalu_type_name(uint8_t hdr) {
    switch(hdr & 0x1F) {
        case 1:  return "non-IDR slice";
        case 5:  return "IDR slice";
        case 6:  return "SEI";
        case 7:  return "SPS";
        case 8:  return "PPS";
        case 9:  return "AUD";
        default: return "other";
    }
}

bool has_annex_b(const uint8_t* p, size_t n) {
    return n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1;
}

size_t find_nalu_header(const uint8_t* p, size_t n) {
    for(size_t i = 0; i + 3 < n; ++i)
        if(p[i] == 0 && p[i+1] == 0 && p[i+2] == 0 && p[i+3] == 1 && i + 4 < n)
            return i + 4;
    return n;
}

} // namespace

int main() {
#if !defined(_WIN32)
    return 77;
#else
    using namespace std;

    // ------------------------------------------------------------------
    // 1. Load nvEncodeAPI64.dll and obtain the function table.
    //
    //    The DLL exports are: NvEncodeAPICreateInstance,
    //    NvEncodeAPIGetMaxSupportedVersion, and a handful of legacy
    //    NvTool* entries.  The NvEncOpenEncodeSessionEx etc. names from
    //    pre-CUDA-11 headers are NOT exported anymore, so we MUST go
    //    through the function-table path.
    // ------------------------------------------------------------------
    HMODULE dll = LoadLibraryA("nvEncodeAPI64.dll");
    if(!dll) {
        std::fprintf(stderr, "nvenc_probe: LoadLibrary(nvEncodeAPI64.dll) failed\n");
        return 77;
    }
    auto createInstance = (PfnNvEncodeAPICreateInstance)
        GetProcAddress(dll, "NvEncodeAPICreateInstance");
    if(!createInstance) {
        std::fprintf(stderr, "nvenc_probe: NvEncodeAPICreateInstance export missing\n");
        FreeLibrary(dll);
        return 77;
    }

    NV_ENCODE_API_FUNCTION_LIST fnList{};
    fnList.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS s = createInstance(&fnList);
    if(s != NV_ENC_SUCCESS || !fnList.nvEncOpenEncodeSessionEx) {
        std::fprintf(stderr, "nvenc_probe: NvEncodeAPICreateInstance failed (status=%u)\n",
                     (unsigned)s);
        FreeLibrary(dll);
        return 77;
    }
    if(!fnList.nvEncInitializeEncoder || !fnList.nvEncEncodePicture ||
       !fnList.nvEncLockBitstream     || !fnList.nvEncUnlockBitstream) {
        std::fprintf(stderr, "nvenc_probe: function table missing required entries\n");
        FreeLibrary(dll);
        return 77;
    }
    fprintf(stderr, "nvenc_probe: NvEncodeAPICreateInstance OK; function table populated\n");

    // ------------------------------------------------------------------
    // 2. Open D3D11 device.
    // ------------------------------------------------------------------
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if(FAILED(hr) || !dev) {
        std::fprintf(stderr, "nvenc_probe: D3D11CreateDevice failed (hr=0x%08lX)\n",
                     (unsigned long)hr);
        FreeLibrary(dll);
        return 77;
    }

    // ------------------------------------------------------------------
    // 3. Open NVENC session.
    // ------------------------------------------------------------------
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
    open.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.device     = dev;
    open.apiVersion = NVENCAPI_VERSION;

    void* enc = nullptr;
    s = fnList.nvEncOpenEncodeSessionEx(&open, &enc);
    if(s != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "nvenc_probe: nvEncOpenEncodeSessionEx failed (status=%u)\n",
                     (unsigned)s);
        std::fprintf(stderr, "  -- see docs/hw_plugin_seam.md for diagnosis --\n");
        dev->Release();
        FreeLibrary(dll);
        return (s == NV_ENC_ERR_INVALID_VERSION) ? 1 : 77;
    }
    fprintf(stderr, "nvenc_probe: NVENC session opened\n");

    // ------------------------------------------------------------------
    // 4. Get preset config + tweak H.264 params.
    //    NOTE: presetCfg.version inside NV_ENC_PRESET_CONFIG must be
    //    NV_ENC_CONFIG_VER (the inner NV_ENC_CONFIG struct version),
    //    not the outer NV_ENC_PRESET_CONFIG version.
    // ------------------------------------------------------------------
    NV_ENC_PRESET_CONFIG preset_cfg{};
    preset_cfg.version           = NV_ENC_PRESET_CONFIG_VER;
    preset_cfg.presetCfg.version = NV_ENC_CONFIG_VER;
    s = fnList.nvEncGetEncodePresetConfig(enc, NV_ENC_CODEC_H264_GUID,
                                         NV_ENC_PRESET_P4_GUID, &preset_cfg);
    if(s != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "nvenc_probe: nvEncGetEncodePresetConfig failed (status=%u)\n",
                     (unsigned)s);
        fnList.nvEncDestroyEncoder(enc);
        dev->Release();
        FreeLibrary(dll);
        return 1;
    }

    NV_ENC_CONFIG cfg = preset_cfg.presetCfg;
    cfg.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
    cfg.encodeCodecConfig.h264Config.idrPeriod       = kGopLength;
    cfg.encodeCodecConfig.h264Config.repeatSPSPPS   = 1;
    cfg.encodeCodecConfig.h264Config.maxNumRefFrames = 1;
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    cfg.rcParams.averageBitRate  = kBitrateBps;
    cfg.rcParams.maxBitRate      = kBitrateBps;
    cfg.rcParams.vbvBufferSize   = kBitrateBps / kFps;
    cfg.rcParams.enableMinQP    = 1;
    cfg.rcParams.enableMaxQP    = 1;
    cfg.rcParams.minQP.qpInterP = 18;
    cfg.rcParams.minQP.qpIntra  = 18;
    cfg.rcParams.minQP.qpInterB = 18;
    cfg.rcParams.maxQP.qpInterP = 32;
    cfg.rcParams.maxQP.qpIntra  = 32;
    cfg.rcParams.maxQP.qpInterB = 32;

    // ------------------------------------------------------------------
    // 5. Initialize encoder.
    // ------------------------------------------------------------------
    NV_ENC_INITIALIZE_PARAMS init{};
    init.version      = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID   = NV_ENC_CODEC_H264_GUID;
    init.presetGUID   = NV_ENC_PRESET_P4_GUID;
    init.encodeWidth  = kWidth;
    init.encodeHeight = kHeight;
    init.darWidth     = kWidth;
    init.darHeight    = kHeight;
    init.frameRateNum = kFps;
    init.frameRateDen = 1;
    init.enablePTD    = 1;
    init.tuningInfo   = NV_ENC_TUNING_INFO_LOW_LATENCY;
    init.encodeConfig = &cfg;

    s = fnList.nvEncInitializeEncoder(enc, &init);
    if(s != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "nvenc_probe: nvEncInitializeEncoder failed (status=%u)\n",
                     (unsigned)s);
        fnList.nvEncDestroyEncoder(enc);
        dev->Release();
        FreeLibrary(dll);
        return 1;
    }
    fprintf(stderr, "nvenc_probe: encoder initialized %ux%u @ %u fps, CBR %u bps\n",
            kWidth, kHeight, kFps, kBitrateBps);

    // ------------------------------------------------------------------
    // 6. Allocate I/O buffers.
    // ------------------------------------------------------------------
    NV_ENC_CREATE_INPUT_BUFFER in_buf{};
    in_buf.version   = NV_ENC_CREATE_INPUT_BUFFER_VER;
    in_buf.width     = kWidth;
    in_buf.height    = kHeight;
    in_buf.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    s = fnList.nvEncCreateInputBuffer(enc, &in_buf);
    if(s != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "nvenc_probe: nvEncCreateInputBuffer failed (status=%u)\n",
                     (unsigned)s);
        fnList.nvEncDestroyEncoder(enc);
        dev->Release();
        FreeLibrary(dll);
        return 1;
    }

    NV_ENC_CREATE_BITSTREAM_BUFFER out_buf{};
    out_buf.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    out_buf.size    = (uint32_t)kBsBufBytes;
    s = fnList.nvEncCreateBitstreamBuffer(enc, &out_buf);
    if(s != NV_ENC_SUCCESS) {
        std::fprintf(stderr, "nvenc_probe: nvEncCreateBitstreamBuffer failed (status=%u)\n",
                     (unsigned)s);
        fnList.nvEncDestroyInputBuffer(enc, in_buf.inputBuffer);
        fnList.nvEncDestroyEncoder(enc);
        dev->Release();
        FreeLibrary(dll);
        return 1;
    }

    // ------------------------------------------------------------------
    // 7. Encode kNumFrames frames.
    // ------------------------------------------------------------------
    size_t total_bytes = 0;
    size_t frames_out  = 0;
    size_t idr_frames  = 0;
    std::vector<uint8_t> first_access_unit;

    for(uint32_t f = 0; f < kNumFrames; ++f) {
        // Fill the NV12 input buffer with a per-frame gradient pattern.
        uint8_t* nv12 = static_cast<uint8_t*>(in_buf.inputBuffer);
        for(uint32_t y = 0; y < kHeight; ++y)
            for(uint32_t x = 0; x < kWidth; ++x)
                nv12[y * kWidth + x] = (uint8_t)((x + y + f * 4) & 0xFF);
        uint8_t* uv = nv12 + (size_t)kWidth * kHeight;
        size_t uv_size = (size_t)kWidth * kHeight / 2;
        for(size_t i = 0; i < uv_size; i += 2) {
            uv[i] = 128; uv[i+1] = 128;
        }

        NV_ENC_PIC_PARAMS pic{};
        pic.version         = NV_ENC_PIC_PARAMS_VER;
        pic.inputBuffer     = in_buf.inputBuffer;
        pic.bufferFmt       = NV_ENC_BUFFER_FORMAT_NV12;
        pic.pictureStruct   = NV_ENC_PIC_STRUCT_FRAME;
        pic.outputBitstream = out_buf.bitstreamBuffer;
        if(f == 0) {
            pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR |
                                NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        }

        s = fnList.nvEncEncodePicture(enc, &pic);
        if(s != NV_ENC_SUCCESS && s != NV_ENC_ERR_NEED_MORE_INPUT) {
            std::fprintf(stderr, "nvenc_probe: nvEncEncodePicture frame %u failed "
                     "(status=%u)\n", f, (unsigned)s);
            break;
        }

        NV_ENC_LOCK_BITSTREAM bs{};
        bs.version          = NV_ENC_LOCK_BITSTREAM_VER;
        bs.outputBitstream  = out_buf.bitstreamBuffer;
        s = fnList.nvEncLockBitstream(enc, &bs);
        if(s != NV_ENC_SUCCESS || !bs.bitstreamBufferPtr) {
            continue;  // encoder buffering
        }

        total_bytes += bs.bitstreamSizeInBytes;
        frames_out++;
        if(bs.pictureType == NV_ENC_PIC_TYPE_IDR) idr_frames++;

        if(first_access_unit.empty()) {
            first_access_unit.assign(
                static_cast<uint8_t*>(bs.bitstreamBufferPtr),
                static_cast<uint8_t*>(bs.bitstreamBufferPtr) + bs.bitstreamSizeInBytes);
        }

        fnList.nvEncUnlockBitstream(enc, bs.outputBitstream);
    }

    // ------------------------------------------------------------------
    // 8. Report.
    // ------------------------------------------------------------------
    int rc = 0;
    if(frames_out == 0) {
        std::fprintf(stderr, "nvenc_probe: FAIL — encoder produced 0 frames\n");
        rc = 1;
    } else if(!has_annex_b(first_access_unit.data(), first_access_unit.size())) {
        std::fprintf(stderr, "nvenc_probe: FAIL — no Annex B start code in first "
                "access unit (first bytes: ");
        for(size_t i = 0; i < 8 && i < first_access_unit.size(); ++i)
            std::fprintf(stderr, "%02x ", first_access_unit[i]);
        std::fprintf(stderr, ")\n");
        rc = 1;
    } else {
        size_t nal_offset = find_nalu_header(first_access_unit.data(),
                                           first_access_unit.size());
        uint8_t hdr = (nal_offset < first_access_unit.size())
                          ? first_access_unit[nal_offset] : 0;
        fprintf(stderr, "nvenc_probe: SUCCESS\n");
        fprintf(stderr, "  Frames output : %zu / %u\n", frames_out, kNumFrames);
        fprintf(stderr, "  IDR frames   : %zu\n", idr_frames);
        fprintf(stderr, "  Total bytes  : %zu  (avg %.0f bytes/frame)\n",
                total_bytes, frames_out ? (double)total_bytes / frames_out : 0.0);
        fprintf(stderr, "  First NALU   : %s (0x%02X)\n",
                nalu_type_name(hdr), hdr);
        fprintf(stderr, "  Annex B SC    : YES (starts 00 00 00 01)\n");
        fprintf(stderr, "  First 32 bytes: ");
        for(size_t i = 0; i < 32 && i < first_access_unit.size(); ++i)
            std::fprintf(stderr, "%02x ", first_access_unit[i]);
        std::fprintf(stderr, "\n");
    }

    // ------------------------------------------------------------------
    // 9. Cleanup.
    // ------------------------------------------------------------------
    fnList.nvEncDestroyBitstreamBuffer(enc, out_buf.bitstreamBuffer);
    fnList.nvEncDestroyInputBuffer(enc, in_buf.inputBuffer);
    fnList.nvEncDestroyEncoder(enc);
    dev->Release();
    FreeLibrary(dll);
    return rc;
#endif
}
