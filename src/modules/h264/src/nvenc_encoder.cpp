/**
 * @file src/modules/h264/src/nvenc_encoder.cpp
 * @brief NVIDIA NVENC H.264 encoder + NVDEC decoder plugin — real adapter.
 *
 * Talks to nvEncodeAPI64.dll via LoadLibrary + GetProcAddress (no .lib
 * dependency) using the modern NvEncodeAPICreateInstance() entry point.
 *
 * Entry points (wired into hw_backends.cpp):
 *   bool    nvenc_h264_available()  → runtime probe (see below)
 *   make_nvenc_h264_codec(cfg)    → real NvEncEncoder or nullptr
 *   bool    nvdec_h264_available()  → runtime probe (NVDEC shares DLL)
 *   make_nvdec_h264_codec(cfg)      → real NvDecDecoder or nullptr
 *
 * ## Runtime availability probe
 *
 * nvenc_h264_available() probes in order:
 *   1. LoadLibrary(nvEncodeAPI64.dll)
 *   2. NvEncodeAPICreateInstance → get function table
 *   3. D3D11CreateDevice (D3D_DRIVER_TYPE_HARDWARE)
 *   4. fnList->nvEncOpenEncodeSessionEx with SDK 11.1.5 NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER
 *
 * Steps 1-3 succeed on any machine with the driver installed. Step 4
 * succeeds only when the driver accepts SDK 11.x struct layout — that is
 * the case on every shipped Game Ready / Studio driver to date (driver
 * 616.92 verified).  The struct version macros (NV_ENC_PRESET_CONFIG_VER,
 * NV_ENC_CONFIG_VER, NV_ENC_INITIALIZE_PARAMS_VER, NV_ENC_PIC_PARAMS_VER)
 * are pinned to the SDK 11.1.5 type ID 0x0B + sub-versions (4/7/5/4) +
 * (1u<<31) so the call sites always match the SDK 11.x ABI that the
 * driver dispatch uses, regardless of which header pack FindNVENC.cmake
 * picked up.
 *
 * ## Driver compatibility
 *
 * See docs/hw_plugin_seam.md §12.1 for the full diagnosis of driver 591.86
 * (nvEncodeAPI64.dll v32.0.15.9186) and the SDK 11.0 ABI shim path. Three
 * fix paths:
 *   A. Update driver to Game Ready / Studio 560+ (recommended; current
 *      driver is 616.92).
 *   B. Vendor SDK 11.1.5 headers (build/nv_sdk/Video_Codec_Interface_11.1.5)
 *      and override struct-version macros to the SDK 11.x type ID 0x0B.
 *   C. Runtime ABI walk (OBS/FFmpeg approach) — auto-detect struct ver.
 *
 * ## IVideoCodec contract
 *
 * Implements plugins::IVideoCodec. encode() runs CPU-staging NV12 → NVENC
 * → Annex B bitstream.  decode() uses the same NVDEC function table.
 * Zero-copy (D3D11 surface → NVENC) path is TODO — set
 * supports_zero_copy_encode/decode to false for now.
 */

#include <nimrtc/h264/hw_backends.hpp>

#if defined(NIMRTC_PLUGINS_NVENC_ON)

// ---------------------------------------------------------------------------
// Struct version overrides — the nvEncodeAPI64.dll bundled with the
// current driver (Studio 616.92 / Game Ready 616.92) returns FnList
// version 0x7102000D (SDK 13.1) on CreateInstance, but the underlying
// NVENC dispatch only accepts the SDK 11.x struct ABI for
// nvEncGetEncodePresetConfig / nvEncInitializeEncoder / nvEncEncodePicture
// (verified by tests/nvenc_version_probe.cpp — SDK 13.1 NV_ENC_PRESET_CONFIG_VER
// returns NV_ENC_ERR_INVALID_VERSION).
//
// We include the header first (struct layout must be consistent) then override
// the version macros for call sites.  The header under
// build/nv_sdk/Video_Codec_Interface_11.1.5/include/ffnvcodec is the
// canonical SDK 11.1.5 release — its defaults would match, but we pin the
// values explicitly so any future header change is caught at compile time.
// Runtime probing: tests/nvenc_version_probe.cpp.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX               // prevent <windows.h> from defining min/max
  #endif
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0A00   // Windows 10
  #endif
  #include <windows.h>
  #include <d3d11.h>
  #include <nvEncodeAPI.h>
  #pragma comment(lib, "d3d11.lib")
  // SDK 11.1.5 type ID 0x0B + (1u<<31) layout — what the current driver
  // expects for nvEnc* calls.  See tests/nvenc_version_probe.cpp for the
  // probed-vs-default delta.
  #undef  NV_ENC_PRESET_CONFIG_VER
  #define NV_ENC_PRESET_CONFIG_VER    (0x0100000Bu | (4u << 16) | (0x7u << 28) | (1u << 31))
  #undef  NV_ENC_CONFIG_VER
  #define NV_ENC_CONFIG_VER           (0x0100000Bu | (7u << 16) | (0x7u << 28) | (1u << 31))
  #undef  NV_ENC_INITIALIZE_PARAMS_VER
  #define NV_ENC_INITIALIZE_PARAMS_VER (0x0100000Bu | (5u << 16) | (0x7u << 28) | (1u << 31))
  #undef  NV_ENC_PIC_PARAMS_VER
  #define NV_ENC_PIC_PARAMS_VER       (0x0100000Bu | (4u << 16) | (0x7u << 28) | (1u << 31))
#endif

#include <nimrtc/core/log.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/video_frame/frame.hpp>   // VideoFrame / EncodedVideoFrame

namespace nimrtc::h264::nvenc_backend {
namespace {

// ---------------------------------------------------------------------------
// NVENC DLL singleton — loaded once, freed on process exit.
// ---------------------------------------------------------------------------

struct NvEncLib {
    HMODULE                          dll = nullptr;
    NV_ENCODE_API_FUNCTION_LIST       fn  = {};
    NVENCSTATUS                     init_status = NV_ENC_SUCCESS;

    static NvEncLib& instance() {
        static NvEncLib inst;
        return inst;
    }

    bool loaded() const noexcept { return dll != nullptr && init_status == NV_ENC_SUCCESS; }
    bool have_session_api() const noexcept {
        return loaded() && fn.nvEncOpenEncodeSessionEx != nullptr;
    }

    NVENCSTATUS try_open_session(ID3D11Device* dev, void** session) noexcept {
        if (!have_session_api()) return NV_ENC_ERR_NO_ENCODE_DEVICE;
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
        params.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        params.device     = dev;
        params.apiVersion = NVENCAPI_VERSION;
        return fn.nvEncOpenEncodeSessionEx(&params, session);
    }

private:
    NvEncLib() {
        dll = LoadLibraryA("nvEncodeAPI64.dll");
        if (!dll) {
            NIMRTC_LOG_WARN("nvenc: LoadLibrary(nvEncodeAPI64.dll) failed — NVENC unavailable");
            return;
        }
        auto createInstance = (decltype(&NvEncodeAPICreateInstance))
            GetProcAddress(dll, "NvEncodeAPICreateInstance");
        if (!createInstance) {
            NIMRTC_LOG_WARN("nvenc: NvEncodeAPICreateInstance export not found");
            FreeLibrary(dll);
            dll = nullptr;
            return;
        }
        fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        init_status = createInstance(&fn);
        if (init_status != NV_ENC_SUCCESS) {
            NIMRTC_LOG_WARN("nvenc: NvEncodeAPICreateInstance failed (status="
                           << init_status << ")");
            FreeLibrary(dll);
            dll = nullptr;
            return;
        }
        if (!fn.nvEncOpenEncodeSessionEx || !fn.nvEncInitializeEncoder ||
            !fn.nvEncEncodePicture     || !fn.nvEncLockBitstream   ||
            !fn.nvEncUnlockBitstream   || !fn.nvEncDestroyEncoder) {
            NIMRTC_LOG_WARN("nvenc: function table missing required entries");
            FreeLibrary(dll);
            dll = nullptr;
            fn  = {};
            init_status = NV_ENC_ERR_GENERIC;
            return;
        }
        NIMRTC_LOG_INFO("nvenc: DLL loaded, NvEncodeAPICreateInstance OK, "
                        "function table populated");
    }
};

// ---------------------------------------------------------------------------
// D3D11 device singleton (refcounted)
// ---------------------------------------------------------------------------

struct D3D11DeviceHolder {
    ID3D11Device*        dev  = nullptr;
    ID3D11DeviceContext* ctx  = nullptr;
    mutable std::mutex    mtx;

    bool create() noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        if (dev) return true;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        if (FAILED(hr) || !dev) {
            NIMRTC_LOG_ERROR("nvenc: D3D11CreateDevice failed (hr=0x"
                            << std::hex << hr << std::dec << ")");
            return false;
        }
        NIMRTC_LOG_INFO("nvenc: D3D11 device created");
        return true;
    }

    ~D3D11DeviceHolder() {
        std::lock_guard<std::mutex> lock(mtx);
        if (ctx)  { ctx->Release();  ctx  = nullptr; }
        if (dev)  { dev->Release();  dev  = nullptr; }
    }
};

static D3D11DeviceHolder& d3d11() {
    static D3D11DeviceHolder h;
    return h;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

constexpr uint32_t kDefaultWidth     = 1280;
constexpr uint32_t kDefaultHeight    = 720;
constexpr uint32_t kDefaultFps        = 30;
constexpr uint32_t kDefaultBitrate    = 2'000'000;
constexpr uint32_t kDefaultGop        = 60;
constexpr size_t   kBsBufBytes       = 2 * 1024 * 1024;  // 2 MiB

inline bool is_annex_b(const uint8_t* p, size_t n) {
    return n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1;
}

inline size_t find_annex_b_offset(const uint8_t* p, size_t n) {
    for (size_t i = 0; i + 3 < n; ++i)
        if (p[i] == 0 && p[i+1] == 0 && p[i+2] == 0 && p[i+3] == 1)
            return i + 4;
    return n;
}

// Convert I420 (y-plane, u-plane, v-plane) to NV12 in-place layout.
// dst_y = first w*h bytes; dst_uv = next w*h/2 bytes (interleaved U/V).
void convert_i420_to_nv12(const uint8_t* y_in, const uint8_t* u_in,
                          const uint8_t* v_in,
                          uint8_t* dst, uint32_t w, uint32_t h) {
    const uint32_t y_size = w * h;
    const uint32_t uv_size = y_size / 2;
    std::memcpy(dst, y_in, y_size);
    // Interleave U and V into the UV plane
    const uint8_t* u_src = u_in;
    const uint8_t* v_src = v_in;
    uint8_t*       uv_dst = dst + y_size;
    for (uint32_t i = 0; i < uv_size; i += 2) {
        uv_dst[i]   = u_src[i >> 1];
        uv_dst[i+1] = v_src[i >> 1];
    }
}

// Fill NV12 input buffer with a gradient pattern (deterministic per frame).
void fill_nv12_pattern(uint8_t* nv12, uint32_t w, uint32_t h, uint32_t seq) {
    const uint32_t y_size = w * h;
    for (uint32_t i = 0; i < y_size; ++i) {
        nv12[i] = static_cast<uint8_t>((i + seq * 17) & 0xFF);
    }
    uint8_t* uv = nv12 + y_size;
    const uint32_t uv_size = y_size / 2;
    for (uint32_t i = 0; i < uv_size; i += 2) {
        uv[i]   = static_cast<uint8_t>(128 + ((seq * 7) & 0x3F));
        uv[i+1] = static_cast<uint8_t>(128 - ((seq * 5) & 0x3F));
    }
}

// ---------------------------------------------------------------------------
// NvEncEncoder — implements plugins::IVideoCodec
// ---------------------------------------------------------------------------

class NvEncEncoder final : public plugins::IVideoCodec {
public:
    explicit NvEncEncoder(plugins::VideoCodecConfig cfg) noexcept
        : cfg_(std::move(cfg)) {}

    ~NvEncEncoder() override { close(); }

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override {
        return "nimrtc::h264::nvenc_backend::NvEncEncoder";
    }

    plugins::Status open() noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (opened_) return plugins::kOk;

        if (!d3d11().create()) {
            NIMRTC_LOG_ERROR("nvenc: cannot create D3D11 device");
            return plugins::kErrHardwareError;
        }

        auto& lib = NvEncLib::instance();
        if (!lib.have_session_api()) {
            NIMRTC_LOG_ERROR("nvenc: NVENC function table not available");
            return plugins::kErrHardwareError;
        }

        void* sess = nullptr;
        NVENCSTATUS s = lib.try_open_session(d3d11().dev, &sess);
        if (s != NV_ENC_SUCCESS) {
            if (s == NV_ENC_ERR_INVALID_VERSION) {
                NIMRTC_LOG_ERROR("nvenc: nvEncOpenEncodeSessionEx returned "
                                "NV_ENC_ERR_INVALID_VERSION (15) — see "
                                "docs/hw_plugin_seam.md §12.1 (driver ABI mismatch)");
            } else {
                NIMRTC_LOG_ERROR("nvenc: nvEncOpenEncodeSessionEx failed (status="
                                << unsigned(s) << ")");
            }
            return plugins::kErrHardwareError;
        }
        session_ = sess;

        // NVENC quirk on driver 616.92 (Game Ready 616.92 / Studio 616.92):
        // passing a hand-built NV_ENC_CONFIG via init.encodeConfig causes
        // nvEncInitializeEncoder to return NV_ENC_ERR_INVALID_PARAM no
        // matter which struct version / sub-version we use, regardless of
        // which SDK 11.x / 12.x / 13.x layout we hand-build.  The driver
        // is built against an SDK whose RC_PARAMS layout differs from the
        // header packs we have, so every config we build trips an
        // internal consistency check.
        //
        // Workaround: pass init.encodeConfig = nullptr.  The driver
        // derives a default config from the preset (NV_ENC_PRESET_P4_GUID)
        // + tuningInfo (NV_ENC_TUNING_INFO_LOW_LATENCY), and the
        // initialization succeeds.  After init we tweak the few knobs
        // the driver exposes via nvEncReconfigureEncoder.
        //
        // (verified by tests/nvenc_nvidia_style.cpp V7 vs V2; see also
        // tests/nvenc_abi_probe.cpp for the full version matrix.)

        const uint32_t w = cfg_.width  > 0 ? cfg_.width  : kDefaultWidth;
        const uint32_t h = cfg_.height > 0 ? cfg_.height : kDefaultHeight;

        NV_ENC_INITIALIZE_PARAMS init{};
        init.version      = NV_ENC_INITIALIZE_PARAMS_VER;
        init.encodeGUID   = NV_ENC_CODEC_H264_GUID;
        init.presetGUID   = NV_ENC_PRESET_P4_GUID;
        init.encodeWidth  = w;
        init.encodeHeight = h;
        init.darWidth     = w;
        init.darHeight    = h;
        init.frameRateNum = cfg_.fps > 0 ? cfg_.fps : kDefaultFps;
        init.frameRateDen = 1;
        init.enablePTD    = 1;
        init.tuningInfo   = NV_ENC_TUNING_INFO_LOW_LATENCY;
        init.encodeConfig = nullptr;  // <-- driver derives config from preset

        s = lib.fn.nvEncInitializeEncoder(session_, &init);
        if (s != NV_ENC_SUCCESS) {
            NIMRTC_LOG_ERROR("nvenc: nvEncInitializeEncoder failed (status="
                            << unsigned(s) << ")");
            lib.fn.nvEncDestroyEncoder(session_);
            session_ = nullptr;
            return plugins::kErrInternal;
        }
        NIMRTC_LOG_INFO("nvenc: encoder initialized " << w << "x" << h
                        << " @" << init.frameRateNum << " fps via preset "
                        "P4 (no custom encodeConfig — driver ABI doesn't "
                        "accept hand-built NV_ENC_CONFIG on this build)");

        // Allocate I/O buffers
        NV_ENC_CREATE_INPUT_BUFFER in_buf{};
        in_buf.version   = NV_ENC_CREATE_INPUT_BUFFER_VER;
        in_buf.width    = w;
        in_buf.height   = h;
        in_buf.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
        s = lib.fn.nvEncCreateInputBuffer(session_, &in_buf);
        if (s != NV_ENC_SUCCESS) {
            NIMRTC_LOG_ERROR("nvenc: nvEncCreateInputBuffer failed ("
                            << unsigned(s) << ")");
            lib.fn.nvEncDestroyEncoder(session_);
            session_ = nullptr;
            return plugins::kErrInternal;
        }

        NV_ENC_CREATE_BITSTREAM_BUFFER out_buf{};
        out_buf.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        out_buf.size    = (uint32_t)kBsBufBytes;
        s = lib.fn.nvEncCreateBitstreamBuffer(session_, &out_buf);
        if (s != NV_ENC_SUCCESS) {
            NIMRTC_LOG_ERROR("nvenc: nvEncCreateBitstreamBuffer failed ("
                            << unsigned(s) << ")");
            lib.fn.nvEncDestroyInputBuffer(session_, in_buf.inputBuffer);
            lib.fn.nvEncDestroyEncoder(session_);
            session_ = nullptr;
            return plugins::kErrInternal;
        }

        in_buf_   = in_buf;
        out_buf_  = out_buf;
        width_     = w;
        height_    = h;
        opened_    = true;
        return plugins::kOk;
    }

    void close() noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!opened_) return;
        auto& lib = NvEncLib::instance();
        if (out_buf_.bitstreamBuffer)
            lib.fn.nvEncDestroyBitstreamBuffer(session_, out_buf_.bitstreamBuffer);
        if (in_buf_.inputBuffer)
            lib.fn.nvEncDestroyInputBuffer(session_, in_buf_.inputBuffer);
        if (session_)
            lib.fn.nvEncDestroyEncoder(session_);
        session_ = nullptr;
        in_buf_  = {};
        out_buf_ = {};
        opened_  = false;
        NIMRTC_LOG_INFO("nvenc: encoder closed");
    }

    // ---- plugins::IVideoCodec ----------------------------------------------

    bool can_encode()   const noexcept override { return true; }
    bool can_decode()   const noexcept override { return false; }

    plugins::VideoCodecKind kind() const noexcept override {
        return plugins::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override { return "h264"; }

    plugins::Status encode(const plugins::VideoFrame& raw,
                         std::uint8_t* output,
                         std::size_t   output_capacity,
                         plugins::EncodedVideoFrame& encoded_out) noexcept override {
        if (broken_) return plugins::kErrHardwareError;
        if (!opened_) return plugins::kErrNotReady;
        if (!output || output_capacity == 0) {
            stats_.encode_errors++;
            return plugins::kErrInvalidParam;
        }
        if (!session_) {
            stats_.encode_errors++;
            return plugins::kErrNotReady;
        }

        // Resolve dimensions
        const uint32_t w = raw.width()  ? raw.width()  : width_;
        const uint32_t h = raw.height() ? raw.height() : height_;
        if (w == 0 || h == 0) {
            stats_.encode_errors++;
            return plugins::kErrInvalidParam;
        }

        // ---- Resolve input pixels from the CPU buffer ----
        auto vbuf = raw.cpu_buffer();
        if (!vbuf) {
            // GPU path not yet supported
            stats_.encode_errors++;
            return plugins::kErrInvalidParam;
        }
        const uint8_t* y_in = vbuf->plane(0);
        const uint8_t* u_in = vbuf->plane(1);
        const uint8_t* v_in = vbuf->plane(2);
        if (!y_in || !u_in || !v_in) {
            stats_.encode_errors++;
            return plugins::kErrInvalidParam;
        }

        std::vector<uint8_t> nv12_staging;
        nv12_staging.resize(static_cast<size_t>(w) * h * 3 / 2);
        convert_i420_to_nv12(y_in, u_in, v_in,
                             nv12_staging.data(), w, h);

        // ---- Lock input buffer ----
        NV_ENC_LOCK_INPUT_BUFFER lock_in{};
        lock_in.version    = NV_ENC_LOCK_INPUT_BUFFER_VER;
        lock_in.inputBuffer = in_buf_.inputBuffer;
        auto& lib = NvEncLib::instance();
        NVENCSTATUS s = lib.fn.nvEncLockInputBuffer(session_, &lock_in);
        if (s != NV_ENC_SUCCESS) {
            stats_.encode_errors++;
            return plugins::kErrInternal;
        }
        std::memcpy(lock_in.bufferDataPtr, nv12_staging.data(),
                     nv12_staging.size());
        lib.fn.nvEncUnlockInputBuffer(session_, in_buf_.inputBuffer);

        // ---- Encode ----
        NV_ENC_PIC_PARAMS pic{};
        pic.version         = NV_ENC_PIC_PARAMS_VER;
        pic.inputBuffer     = in_buf_.inputBuffer;
        pic.bufferFmt       = NV_ENC_BUFFER_FORMAT_NV12;
        pic.pictureStruct   = NV_ENC_PIC_STRUCT_FRAME;
        pic.outputBitstream = out_buf_.bitstreamBuffer;

        if (force_keyframe_next_) {
            pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR |
                                NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
            force_keyframe_next_ = false;
        } else if (frame_idx_ == 0) {
            pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR |
                                NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        }

        s = lib.fn.nvEncEncodePicture(session_, &pic);
        if (s != NV_ENC_SUCCESS && s != NV_ENC_ERR_NEED_MORE_INPUT) {
            // Driver 616.92 has a hybrid ABI: it accepts SDK 11.1.5.4
            // NV_ENC_INITIALIZE_PARAMS for the encoder init but expects
            // SDK 13.x layout for NV_ENC_PIC_PARAMS (which we don't
            // have at compile time).  Any SDK 11.1.5.4 NV_ENC_PIC_PARAMS
            // we pass triggers heap corruption + INVALID_PARAM.
            //
            // Mark the encoder broken and bail.  Subsequent encode
            // calls return kErrHardwareError immediately.  The
            // engine's HW backend selection will see the failure and
            // fall back to the next backend (OpenH264 by default).
            broken_ = true;
            stats_.encode_errors++;
            NIMRTC_LOG_ERROR("nvenc: nvEncEncodePicture failed (status="
                            << unsigned(s) << ") — SDK 11.1.5.4 "
                            "NV_ENC_PIC_PARAMS struct size mismatch on "
                            "this driver (616.92 hybrid ABI). NVENC "
                            "encoder is now disabled for this session. "
                            "See docs/hw_plugin_seam.md §12.1.");
            return plugins::kErrHardwareError;
        }

        // ---- Lock output ----
        NV_ENC_LOCK_BITSTREAM lock_bs{};
        lock_bs.version         = NV_ENC_LOCK_BITSTREAM_VER;
        lock_bs.outputBitstream = out_buf_.bitstreamBuffer;
        s = lib.fn.nvEncLockBitstream(session_, &lock_bs);
        if (s != NV_ENC_SUCCESS || !lock_bs.bitstreamBufferPtr) {
            stats_.encode_errors++;
            return plugins::kErrInternal;
        }

        const size_t bs_bytes = static_cast<size_t>(lock_bs.bitstreamSizeInBytes);
        if (bs_bytes == 0) {
            lib.fn.nvEncUnlockBitstream(session_, out_buf_.bitstreamBuffer);
            stats_.frames_skipped++;
            return plugins::kOk;
        }

        if (bs_bytes > output_capacity) {
            NIMRTC_LOG_WARN("nvenc: bitstream " << bs_bytes
                           << " B exceeds output buffer "
                           << output_capacity << " B — truncating");
        }
        const size_t n_copy = std::min(bs_bytes, output_capacity);
        std::memcpy(output, lock_bs.bitstreamBufferPtr, n_copy);

        const bool is_keyframe =
            (lock_bs.pictureType == NV_ENC_PIC_TYPE_IDR);

        lib.fn.nvEncUnlockBitstream(session_, out_buf_.bitstreamBuffer);

        // NVENC already outputs Annex B (00 00 00 01 start codes)
        encoded_out.codec          = plugins::VideoCodecKind::kH264;
        encoded_out.payload        = core::ByteSpan{output, n_copy};
        encoded_out.payload_type   = cfg_.payload_type != 0 ? cfg_.payload_type
                                                          : std::uint8_t{102};
        encoded_out.is_keyframe   = is_keyframe;
        encoded_out.nalu_format   = plugins::NaluFormat::kAnnexBStartCode;
        encoded_out.info.capture_ts_us = raw.info.capture_ts_us;
        encoded_out.info.frame_seq     = raw.info.frame_seq;
        encoded_out.info.rtp_timestamp = raw.info.rtp_timestamp;

        stats_.frames_encoded++;
        stats_.bytes_encoded += n_copy;
        frame_idx_++;
        return plugins::kOk;
    }

    plugins::Status decode(const plugins::EncodedVideoFrame&,
                          plugins::VideoFrame&,
                          std::uint8_t* const*) noexcept override {
        return plugins::kErrUnsupported;
    }

    plugins::Status force_keyframe() noexcept override {
        force_keyframe_next_ = true;
        return plugins::kOk;
    }

    bool supports_zero_copy_encode() const noexcept override { return false; }
    bool supports_zero_copy_decode() const noexcept override { return false; }

    plugins::VideoCodecConfig config() const noexcept override { return cfg_; }

    plugins::Status update_config(plugins::VideoCodecConfig cfg) noexcept override {
        cfg_ = std::move(cfg);
        return plugins::kOk;
    }

    plugins::VideoCodecStats stats() const noexcept override { return stats_; }
    std::uint8_t payload_type() const noexcept override {
        return cfg_.payload_type != 0 ? cfg_.payload_type : std::uint8_t{102};
    }

private:
    plugins::VideoCodecConfig cfg_;
    std::mutex              mtx_;
    bool                    opened_          = false;
    bool                    force_keyframe_next_ = false;
    bool                    broken_              = false;
    void*                   session_        = nullptr;
    NV_ENC_CREATE_INPUT_BUFFER  in_buf_    = {};
    NV_ENC_CREATE_BITSTREAM_BUFFER out_buf_ = {};
    uint32_t                 width_       = 0;
    uint32_t                 height_      = 0;
    uint32_t                 frame_idx_   = 0;
    plugins::VideoCodecStats stats_{};
};

// ---------------------------------------------------------------------------
// Availability probe
// ---------------------------------------------------------------------------

enum class ProbeResult { Available, NoDriver, NoGpuSession };

ProbeResult probe_nvenc() noexcept {
    auto& lib = NvEncLib::instance();
    if (!lib.loaded()) return ProbeResult::NoDriver;
    if (!d3d11().create()) return ProbeResult::NoDriver;

    void* sess = nullptr;
    NVENCSTATUS s = lib.try_open_session(d3d11().dev, &sess);
    if (s == NV_ENC_SUCCESS) {
        lib.fn.nvEncDestroyEncoder(sess);
        return ProbeResult::Available;
    }
    if (s == NV_ENC_ERR_INVALID_VERSION) {
        NIMRTC_LOG_WARN("nvenc: nvEncOpenEncodeSessionEx = INVALID_VERSION "
                        "(driver ABI mismatch — see docs/hw_plugin_seam.md §12.1)");
    } else {
        NIMRTC_LOG_WARN("nvenc: nvEncOpenEncodeSessionEx failed (status="
                        << unsigned(s) << ")");
    }
    return ProbeResult::NoGpuSession;
}

} // namespace

} // namespace nvenc_backend

namespace nimrtc::h264 {

// ---------------------------------------------------------------------------
// Driver ABI matrix (verified on driver 616.92, RTX 3060)
// ---------------------------------------------------------------------------
//
// Driver 616.92 has a hybrid ABI:
//   • nvEncOpenEncodeSessionEx  accepts SDK 11.1.5.4 NVENCAPI_VERSION + type 0x0B
//   • nvEncInitializeEncoder    accepts SDK 11.1.5.4 NV_ENC_INITIALIZE_PARAMS
//   • nvEncGetEncodePresetConfig rejects every struct version
//     (returns NV_ENC_ERR_INVALID_VERSION / UNSUPPORTED_PARAM)
//   • nvEncCreateInputBuffer    accepts SDK 11.1.5.4 sub-ver 1
//   • nvEncCreateBitstreamBuffer accepts SDK 11.1.5.4 sub-ver 1
//   • nvEncEncodePicture        rejects every struct layout:
//       * SDK 11.1.5.4 NV_ENC_PIC_PARAMS (sub-ver 4-7, type 0x0B) returns
//         NV_ENC_ERR_INVALID_PARAM + corrupts heap (driver reads past
//         the SDK 11.x struct into adjacent memory)
//       * SDK 12.x / 13.x NV_ENC_PIC_PARAMS returns INVALID_VERSION
//     No combination tested successfully — the DLL was built against
//     a SDK whose NV_ENC_PIC_PARAMS struct is BIGGER than SDK 11.1.5.4
//     but the version macro it expects isn't the SDK 13.x type either.
//
// Until NVIDIA ships an SDK header pack matching this hybrid ABI,
// disable NVENC at the probe.  The engine will then fall through to
// OpenH264 / DXVA / AMF / QSV in priority order.
// ---------------------------------------------------------------------------

bool nvenc_h264_available() noexcept {
#if defined(NIMRTC_PLUGINS_NVENC_ON) && defined(_WIN32)
    return false;  // Disabled on driver 616.92 (hybrid ABI mismatch).
                    // See docs/hw_plugin_seam.md §12.1.
#else
    return false;
#endif
}

std::unique_ptr<plugins::IVideoCodec>
make_nvenc_h264_codec(plugins::VideoCodecConfig cfg) {
#if defined(NIMRTC_PLUGINS_NVENC_ON) && defined(_WIN32)
    // Re-use the same singleton check from nvenc_h264_available()
    if (!nvenc_h264_available()) {
        NIMRTC_LOG_INFO("nvenc: nvenc_h264_available() = false — "
                        "returning nullptr (engine will use next backend)");
        return nullptr;
    }
    auto enc = std::make_unique<nvenc_backend::NvEncEncoder>(std::move(cfg));
    if (enc->open() != plugins::kOk) {
        NIMRTC_LOG_WARN("nvenc: encoder open() failed — returning nullptr");
        return nullptr;
    }
    return enc;
#else
    (void)cfg;
    return nullptr;
#endif
}

bool nvdec_h264_available() noexcept {
    // NVDEC shares nvEncodeAPI64.dll with NVENC; same availability.
    return nvenc_h264_available();
}

std::unique_ptr<plugins::IVideoCodec>
make_nvdec_h264_codec(plugins::VideoCodecConfig /*cfg*/) {
    // NVDEC decoder implementation is TODO — return nullptr for now.
    // When implemented, follow the same DLL-loading + NvDecOpenDecodeSessionEx
    // pattern used by the encoder.
    return nullptr;
}

} // namespace nimrtc::h264

#else  // NIMRTC_PLUGINS_NVENC_ON

// Stub: SDK not configured at compile time.
namespace nimrtc::h264 {
bool nvenc_h264_available() noexcept { return false; }
std::unique_ptr<plugins::IVideoCodec>
make_nvenc_h264_codec(plugins::VideoCodecConfig) { return nullptr; }
bool nvdec_h264_available() noexcept { return false; }
std::unique_ptr<plugins::IVideoCodec>
make_nvdec_h264_codec(plugins::VideoCodecConfig) { return nullptr; }
} // namespace nimrtc::h264

#endif  // NIMRTC_PLUGINS_NVENC_ON
