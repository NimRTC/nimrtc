/**
 * @file src/modules/nvcodec/src/nvenc_encoder.cpp
 * @brief NVIDIA NVENC encoder — real adapter (H.264 / H.265 / AV1).
 *
 * Talks to nvEncodeAPI64.dll via LoadLibrary + GetProcAddress (no .lib
 * dependency) using the modern NvEncodeAPICreateInstance() entry point.
 *
 * Entry points (wired into the codec modules' backend tables):
 *   bool    nvenc_codec_available(codec)  → runtime probe (see below)
 *   make_nvenc_codec(codec, cfg)          → real NvEncEncoder or nullptr
 *
 * The encoder is *codec-parameterised*: the same session code serves
 * H.264, HEVC and AV1, and only the `encodeGUID`, the `profileGUID`
 * and the per-codec config branch differ (see `build_encode_config`).
 * The H.264 / H.265 / AV1 codec modules all call the generic entry
 * points above — they do not re-implement the driver integration.
 *
 * ## ABI: the struct version macros are NOT overridden
 *
 * The vendored headers under `src/third_party/nv_codec_headers/` are the
 * NVIDIA Video Codec SDK 13.1 headers (MIT-licensed, redistributed by the
 * FFmpeg project).  They define the struct version words for exactly
 * one SDK version, and the struct *layouts* and the version *words* must
 * come from the same header set.
 *
 * Overriding a version macro with a different SDK's word (as an earlier
 * revision of this file did — pinning the SDK 11.x type ID `0x0B` while
 * the struct in memory still had the SDK 13.1 layout) makes the driver
 * interpret the 13.1 layout with 11.x offsets: the field offsets
 * disagree, the driver reads past the end of the struct it expects, and
 * the process corrupts its heap.  That is exactly the failure the
 * previous revision documented as "hybrid ABI mismatch on driver
 * 616.92" — the mismatch was in this file, not in the driver.
 *
 * So: the version macros are used **as-is**.  When the driver rejects a
 * struct version, that is reported honestly (`nvenc_codec_available()`
 * returns false) rather than papered over with a mismatched word.
 *
 * ## Runtime availability probe
 *
 * nvenc_codec_available() probes in order:
 *   1. LoadLibrary(nvEncodeAPI64.dll)
 *   2. NvEncodeAPICreateInstance → get the function table
 *   3. D3D11CreateDevice (D3D_DRIVER_TYPE_HARDWARE)
 *   4. nvEncOpenEncodeSessionEx with NVENCAPI_VERSION
 *   5. nvEncInitializeEncoder with a real NV_ENC_CONFIG for the codec
 *
 * Steps 1-3 succeed on any machine with the driver installed.  Steps
 * 4-5 succeed only when the driver accepts the SDK 13.1 struct ABI.
 * Step 5 is *per codec*: a GPU can have a working NVENC session and
 * still have no HEVC or no AV1 encoder, and `nvEncInitializeEncoder`
 * is the only call that reports that honestly.
 *
 * ## Dynamic resolution and bitrate
 *
 * An NVENC session is bound to a fixed `encodeWidth` / `encodeHeight`.
 * When the source resolution changes the session has to be torn down and
 * re-created (see `reinit_at()`).  A bitrate change does *not* require
 * that: `nvEncReconfigureEncoder` applies it to the live session (see
 * `apply_bitrate()`).
 */

#include <nimrtc/nvcodec/nv_backends.hpp>

#if defined(NIMRTC_PLUGINS_NVENC_ON)

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <nimrtc/nvcodec/nv_codec_kind.hpp>

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
#endif

#include <nimrtc/core/log.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/video_frame/frame.hpp>   // VideoFrame / EncodedVideoFrame

namespace nimrtc::nvcodec::nvenc_backend {
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
                        "function table populated (SDK " << NVENCAPI_MAJOR_VERSION
                        << "." << NVENCAPI_MINOR_VERSION << ")");
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
constexpr uint32_t kDefaultBitrate    = 1'000'000;
constexpr uint32_t kDefaultGopLength  = 60;
constexpr size_t   kBsBufBytes       = 2 * 1024 * 1024;  // 2 MiB

// ---------------------------------------------------------------------------
// The driver's minimum frame geometry.
//
// NVENC refuses to initialize a session whose frame size is below the
// silicon's minimum: `nvEncInitializeEncoder` then fails with
// NV_ENC_ERR_INVALID_PARAM (status 8) and the driver error string is
// "Frame Dimension less than the minimum supported value."  A source
// frame that is smaller than that cannot be encoded as-is, so the
// encoder has to *pad* the picture up to the legal size instead of
// failing the whole frame.
//
// The limits below were measured on the reference host (RTX 3060,
// driver 5xx, Video Codec SDK 13.1) by probing
// `nvEncInitializeEncoder` over a width x height matrix:
//
//   width  < 145            -> rejected
//   height <  49            -> rejected
//   width  >= 145 && height >= 49 -> accepted
//
// They are *conservative* on purpose: a driver with a stricter minimum
// rejects the padded picture and the frame is dropped with an error,
// which is honest; a driver with a laxer minimum simply gets a picture
// with a small black border, which is invisible in practice.
constexpr uint32_t kMinEncodeWidth  = 160;
constexpr uint32_t kMinEncodeHeight =  64;

/** Clamp a requested geometry to the driver's minimum frame size.
 *
 *  Returns the geometry the encode session is actually created for.  The
 *  caller is responsible for padding the picture up to that size (see
 *  `pad_to_min_geometry`). */
struct Geometry {
    uint32_t width  = 0;
    uint32_t height = 0;
    /** True iff the requested geometry had to be grown. */
    bool padded = false;
};

Geometry clamp_to_min_geometry(uint32_t w, uint32_t h) noexcept {
    Geometry g;
    g.width  = w < kMinEncodeWidth  ? kMinEncodeWidth  : w;
    g.height = h < kMinEncodeHeight ? kMinEncodeHeight : h;
    g.padded = (g.width != w) || (g.height != h);
    return g;
}

// ---------------------------------------------------------------------------
// Pixel conversion helpers
// ---------------------------------------------------------------------------

/** Convert I420 (y-plane, u-plane, v-plane) to NV12 in-place layout.
 *
 *  dst_y = first w*h bytes; dst_uv = next w*h/2 bytes (interleaved U/V).
 *
 *  The source planes are *stride-aware*: the frame layout pads every
 *  stride to a 16-byte boundary, so a row of the source starts at
 *  `row * stride` and not at `row * width`.  The destination is tightly
 *  packed, because NVENC's input buffer is a flat NV12 image. */
void convert_i420_to_nv12(const uint8_t* y_in, const uint8_t* u_in,
                          const uint8_t* v_in,
                          uint32_t stride_y, uint32_t stride_u,
                          uint8_t* dst, uint32_t w, uint32_t h) {
    const uint32_t y_size = w * h;
    const uint32_t uv_size = y_size / 2;

    // Luma: copy row by row so the padded source stride is honoured.
    for (uint32_t row = 0; row < h; ++row) {
        std::memcpy(dst + static_cast<size_t>(row) * w,
                    y_in + static_cast<size_t>(row) * stride_y, w);
    }

    // Interleave U and V into the UV plane.
    uint8_t* uv_dst = dst + y_size;
    const uint32_t cw = w / 2;
    const uint32_t ch = h / 2;
    for (uint32_t row = 0; row < ch; ++row) {
        const uint8_t* u_src = u_in + static_cast<size_t>(row) * stride_u;
        const uint8_t* v_src = v_in + static_cast<size_t>(row) * stride_u;
        uint8_t* out = uv_dst + static_cast<size_t>(row) * w;
        for (uint32_t col = 0; col < cw; ++col) {
            out[col * 2]     = u_src[col];
            out[col * 2 + 1] = v_src[col];
        }
    }
    (void)uv_size;
}

// ---------------------------------------------------------------------------
// The encoder configuration.
//
// A hand-built NV_ENC_CONFIG is used (rather than
// nvEncGetEncodePresetConfig) for two reasons:
//
//   1. The preset-config query is not available on every driver, and its
//      absence must not disable the encoder.
//   2. The bitrate and the GOP length have to come from the caller
//      (`VideoCodecConfig`), and the preset config would have to be
//      fetched *and* patched — which is exactly as much code as building
//      the struct, but with an extra driver round-trip.
//
// The struct version comes from the vendored SDK 13.1 header and must
// not be overridden — see the file header.
// ---------------------------------------------------------------------------

/** The NVENC `encodeGUID` for a codec family.
 *
 *  The driver dispatches on this GUID: the same session code path
 *  serves H.264, HEVC and AV1, and the per-codec configuration
 *  struct that the driver reads is selected by the GUID. */
GUID nv_encode_guid(NvCodec codec) noexcept {
    switch (codec) {
        case NvCodec::kH265: return NV_ENC_CODEC_HEVC_GUID;
        case NvCodec::kAv1:  return NV_ENC_CODEC_AV1_GUID;
        case NvCodec::kH264:
        case NvCodec::kUnsupported:
        default:                 return NV_ENC_CODEC_H264_GUID;
    }
}

/** Build the NV_ENC_CONFIG for the given geometry and rate settings.
 *
 *  The per-codec fields live in a *union* inside NV_ENC_CONFIG
 *  (`encodeCodecConfig`), so exactly one branch may be filled.  The
 *  branch is selected by @p codec, and the union member that is *not*
 *  used stays zero-initialised — the driver only reads the member that
 *  matches the `encodeGUID` it was given.
 *
 *  @param codec  Which codec family the session encodes. */
NV_ENC_CONFIG build_encode_config(NvCodec codec,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t fps,
                                  uint32_t bitrate_bps,
                                  uint32_t gop_length) noexcept {
    NV_ENC_CONFIG cfg{};
    cfg.version      = NV_ENC_CONFIG_VER;
    cfg.gopLength    = gop_length > 0 ? gop_length : kDefaultGopLength;
    cfg.frameIntervalP = 1;   // IPP — no B-frames, so decode order == display order
    cfg.mvPrecision  = NV_ENC_MV_PRECISION_DEFAULT;
    cfg.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;

    // ---- Rate control ----------------------------------------------------
    NV_ENC_RC_PARAMS& rc = cfg.rcParams;
    rc.version        = NV_ENC_RC_PARAMS_VER;
    rc.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    rc.averageBitRate = bitrate_bps > 0 ? bitrate_bps : kDefaultBitrate;
    rc.maxBitRate     = rc.averageBitRate * 3 / 2;   // 1.5x headroom
    rc.vbvBufferSize  = bitrate_bps > 0 ? bitrate_bps / 2 : kDefaultBitrate / 2;
    rc.vbvInitialDelay = rc.vbvBufferSize;
    rc.minQP = NV_ENC_QP{0};
    rc.maxQP = NV_ENC_QP{51};
    rc.initialRCQP = NV_ENC_QP{26};
    rc.enableMinQP = 0;
    rc.enableMaxQP = 0;
    rc.enableInitialRCQP = 1;
    rc.enableAQ = 0;
    rc.enableLookahead = 0;   // low-latency: no lookahead buffering

    // ---- Codec specifics --------------------------------------------------
    switch (codec) {
        case NvCodec::kH264: {
            cfg.profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
            NV_ENC_CONFIG_H264& h264 = cfg.encodeCodecConfig.h264Config;
            h264.level              = NV_ENC_LEVEL_AUTOSELECT;
            h264.idrPeriod          = cfg.gopLength;
            h264.repeatSPSPPS       = 1;   // write SPS/PPS with every IDR
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
            break;
        }
        case NvCodec::kH265: {
            cfg.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
            NV_ENC_CONFIG_HEVC& hevc = cfg.encodeCodecConfig.hevcConfig;
            hevc.level              = NV_ENC_LEVEL_AUTOSELECT;
            hevc.idrPeriod          = cfg.gopLength;
            hevc.repeatSPSPPS       = 1;   // write VPS/SPS/PPS with every IDR
            hevc.chromaFormatIDC    = 1;   // 4:2:0
            hevc.outputBitDepth     = NV_ENC_BIT_DEPTH_8;
            hevc.inputBitDepth      = NV_ENC_BIT_DEPTH_8;
            hevc.numTemporalLayers  = 1;
            hevc.vpsId              = 0;
            hevc.spsId              = 0;
            hevc.ppsId              = 0;
            hevc.sliceMode          = 0;
            hevc.sliceModeData      = 0;
            hevc.maxNumRefFramesInDPB = 1;
            hevc.numRefL0          = NV_ENC_NUM_REF_FRAMES_1;
            hevc.numRefL1          = NV_ENC_NUM_REF_FRAMES_1;
            hevc.tfLevel           = NV_ENC_TEMPORAL_FILTER_LEVEL_0;
            break;
        }
        case NvCodec::kAv1: {
            cfg.profileGUID = NV_ENC_AV1_PROFILE_MAIN_GUID;
            NV_ENC_CONFIG_AV1& av1 = cfg.encodeCodecConfig.av1Config;
            av1.level              = NV_ENC_LEVEL_AUTOSELECT;
            av1.idrPeriod          = cfg.gopLength;
            // The AV1 bitstream is emitted in Annex-B form so the
            // RTP packetizer can split it on OBU boundaries like it
            // does for H.264 / H.265.
            av1.outputAnnexBFormat = 1;
            av1.repeatSeqHdr       = 1;   // write the sequence header with every key frame
            av1.chromaFormatIDC    = 1;   // 4:2:0
            av1.outputBitDepth     = NV_ENC_BIT_DEPTH_8;
            av1.inputBitDepth      = NV_ENC_BIT_DEPTH_8;
            av1.numTemporalLayers  = 1;
            av1.maxNumRefFramesInDPB = 1;
            av1.numFwdRefs         = NV_ENC_NUM_REF_FRAMES_1;
            av1.numBwdRefs         = NV_ENC_NUM_REF_FRAMES_1;
            av1.tfLevel            = NV_ENC_TEMPORAL_FILTER_LEVEL_0;
            av1.minPartSize        = NV_ENC_AV1_PART_SIZE_AUTOSELECT;
            av1.maxPartSize        = NV_ENC_AV1_PART_SIZE_AUTOSELECT;
            // BT.709 primaries / transfer / matrix — the default for
            // the 8-bit 4:2:0 content NimRTC carries.
            av1.colorPrimaries       = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
            av1.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
            av1.matrixCoefficients   = NV_ENC_VUI_MATRIX_COEFFS_BT709;
            break;
        }
        case NvCodec::kUnsupported:
        default:
            // The caller checks the codec before it gets here; reaching
            // this branch means a programming error, and the driver will
            // reject the resulting config.
            break;
    }

    (void)width;
    (void)height;
    (void)fps;
    return cfg;
}

/** Fill the NV_ENC_INITIALIZE_PARAMS for the given geometry.
 *
 *  `encode_config` is non-const because NV_ENC_INITIALIZE_PARAMS::encodeConfig
 *  is a `NV_ENC_CONFIG*` in the pinned SDK-13.1 header.  Passing a const
 *  pointer would be a compile error there; passing a non-const one is also
 *  accepted by the older SDK headers, where the field is declared const.
 *
 *  @param codec  Which codec family the session encodes — this selects
 *                the `encodeGUID` the driver dispatches on. */
NV_ENC_INITIALIZE_PARAMS build_init_params(NvCodec codec,
                                            uint32_t width,
                                            uint32_t height,
                                            uint32_t fps,
                                            NV_ENC_CONFIG* encode_config) noexcept {
    NV_ENC_INITIALIZE_PARAMS init{};
    init.version      = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID   = nv_encode_guid(codec);
    init.presetGUID   = NV_ENC_PRESET_P4_GUID;
    init.encodeWidth  = width;
    init.encodeHeight = height;
    init.darWidth     = width;
    init.darHeight    = height;
    init.frameRateNum = fps > 0 ? fps : kDefaultFps;
    init.frameRateDen = 1;
    init.enablePTD    = 1;   // the encoder picks the picture type
    init.tuningInfo   = NV_ENC_TUNING_INFO_LOW_LATENCY;
    init.encodeConfig = encode_config;
    return init;
}

// ---------------------------------------------------------------------------
// NvEncEncoder — implements plugins::IVideoCodec
// ---------------------------------------------------------------------------

class NvEncEncoder final : public plugins::IVideoCodec {
public:
    /** @param cfg    The caller's configuration.
     *  @param codec  Which codec family this encoder produces. */
    explicit NvEncEncoder(plugins::VideoCodecConfig cfg,
                          NvCodec codec) noexcept
        : cfg_(std::move(cfg)), codec_(codec),
          desc_(codec_desc(codec)) {}

    ~NvEncEncoder() override { close(); }

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override {
        return "nimrtc::nvcodec::nvenc_backend::NvEncEncoder";
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

        const uint32_t w = cfg_.width  > 0 ? cfg_.width  : kDefaultWidth;
        const uint32_t h = cfg_.height > 0 ? cfg_.height : kDefaultHeight;

        // The driver refuses a frame size below the silicon's minimum, so
        // the session is created for the clamped geometry and the picture
        // is padded to it (see `encode()`).
        const Geometry geo = clamp_to_min_geometry(w, h);
        if (geo.padded) {
            NIMRTC_LOG_INFO("nvenc: the requested geometry " << w << "x" << h
                            << " is below the hardware minimum — encoding at "
                            << geo.width << "x" << geo.height
                            << " and padding the picture");
        }

        if (!init_session(geo.width, geo.height)) {
            destroy_session();
            return plugins::kErrHardwareError;
        }

        opened_ = true;
        return plugins::kOk;
    }

    void close() noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!opened_ && !session_) return;
        destroy_session();
        opened_ = false;
        NIMRTC_LOG_INFO("nvenc: encoder closed");
    }

    // ---- plugins::IVideoCodec ----------------------------------------------

    bool can_encode()   const noexcept override { return true; }
    bool can_decode()   const noexcept override { return false; }

    plugins::VideoCodecKind kind() const noexcept override {
        return desc_.kind;
    }
    std::string_view codec_name() const noexcept override {
        return desc_.plugin_id;
    }

    plugins::Status encode(const plugins::VideoFrame& raw,
                         std::uint8_t* output,
                         std::size_t   output_capacity,
                         plugins::EncodedVideoFrame& encoded_out) noexcept override {
        if (broken_) { NIMRTC_LOG_ERROR("nvenc: encode — the encoder is broken"); return plugins::kErrHardwareError; }
        if (!opened_) { NIMRTC_LOG_ERROR("nvenc: encode — the encoder is not open"); return plugins::kErrNotReady; }
        if (!output || output_capacity == 0) {
            stats_.encode_errors++;
            NIMRTC_LOG_ERROR("nvenc: encode — no output buffer");
            return plugins::kErrInvalidParam;
        }
        if (!session_) {
            stats_.encode_errors++;
            NIMRTC_LOG_ERROR("nvenc: encode — no session");
            return plugins::kErrNotReady;
        }

        // Resolve dimensions
        const uint32_t w = raw.width()  ? raw.width()  : width_;
        const uint32_t h = raw.height() ? raw.height() : height_;
        if (w == 0 || h == 0) {
            stats_.encode_errors++;
            NIMRTC_LOG_ERROR("nvenc: encode — the source frame has no "
                             "dimensions (raw=" << raw.width() << "x"
                             << raw.height() << ")");
            return plugins::kErrInvalidParam;
        }

        // ---- The source resolution changed ---------------------------------
        //
        // An NVENC session is bound to a fixed frame size, so a different
        // source size needs a fresh session.  Re-initialising resets the
        // frame counter, which makes the next picture an IDR with inline
        // SPS/PPS — exactly what a receiver needs after a resolution
        // change.
        //
        // The requested size is *clamped up* to the driver's minimum
        // frame geometry first: NVENC rejects a session whose frame size
        // is below the silicon's minimum, and a source that small would
        // otherwise take the whole encoder down.  The picture is padded
        // to the clamped size below.
        const Geometry want = clamp_to_min_geometry(w, h);
        if (want.width != width_ || want.height != height_) {
            NIMRTC_LOG_INFO("nvenc: the source resolution changed from "
                            << width_ << "x" << height_ << " to " << want.width
                            << "x" << want.height
                            << " — re-initializing the encode session");
            if (!reinit_at(want.width, want.height)) {
                stats_.encode_errors++;
                return plugins::kErrHardwareError;
            }
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

        // The source plane strides are padded (16-byte aligned), so the
        // I420 → NV12 conversion has to be stride-aware.
        const auto& in_layout = vbuf->layout();
        const uint32_t sy = in_layout.strides[0];
        const uint32_t su = in_layout.strides[1];

        // ---- Lock input buffer --------------------------------------------
        //
        // The lock has to happen *before* the pixels are staged: the
        // driver reports the buffer's row pitch, and the NV12 planes must
        // be written at that pitch.  A tightly-packed copy is only correct
        // when the pitch happens to equal the frame width — which is the
        // case for a width that is a multiple of the driver's alignment
        // (320 is), but *not* for e.g. 322, where every row after the
        // first would land at the wrong offset and the encoder would
        // encode a sheared picture.
        NV_ENC_LOCK_INPUT_BUFFER lock_in{};
        lock_in.version     = NV_ENC_LOCK_INPUT_BUFFER_VER;
        lock_in.inputBuffer = in_buf_.inputBuffer;
        auto& lib = NvEncLib::instance();
        NVENCSTATUS s = lib.fn.nvEncLockInputBuffer(session_, &lock_in);
        if (s != NV_ENC_SUCCESS) {
            stats_.encode_errors++;
            return plugins::kErrInternal;
        }

        auto* dst = static_cast<uint8_t*>(lock_in.bufferDataPtr);
        const uint32_t pitch = lock_in.pitch ? lock_in.pitch : width_;
        if (!dst) {
            lib.fn.nvEncUnlockInputBuffer(session_, in_buf_.inputBuffer);
            stats_.encode_errors++;
            return plugins::kErrInternal;
        }

        // The NV12 input buffer holds the luma plane first (pitch bytes per
        // row) and the interleaved chroma plane right below it, also with
        // `pitch` bytes per row.
        //
        // The *session* geometry is `width_` x `height_`, which may be
        // larger than the source picture (`w` x `h`) when the source was
        // below the driver's minimum frame size.  The picture is then
        // copied into the top-left corner and the rest is filled with
        // black luma / neutral chroma, so the receiver sees a small
        // picture with a border instead of a failed frame.
        const uint32_t enc_w = width_;
        const uint32_t enc_h = height_;
        const uint32_t half_w = (enc_w + 1u) / 2u;
        const uint32_t half_h = (enc_h + 1u) / 2u;

        // Luma — byte-for-byte, but at the driver's pitch.  The rows past
        // the source picture are black (16 = the BT.601 limited-range
        // floor).
        for (uint32_t row = 0; row < enc_h; ++row) {
            uint8_t* dst_row = dst + static_cast<size_t>(row) * pitch;
            if (row < h) {
                std::memcpy(dst_row, y_in + static_cast<size_t>(row) * sy, w);
                if (enc_w > w) {
                    std::memset(dst_row + w, 16, enc_w - w);
                }
            } else {
                std::memset(dst_row, 16, enc_w);
            }
        }

        // Chroma — U and V interleaved, at the driver's pitch, starting
        // below the luma plane.  The padding is neutral grey (128), which
        // is the chroma centre of the BT.601 limited range.
        uint8_t* uv_dst = dst + static_cast<size_t>(pitch) * enc_h;
        const uint32_t src_half_w = (w + 1u) / 2u;
        const uint32_t src_half_h = (h + 1u) / 2u;
        for (uint32_t row = 0; row < half_h; ++row) {
            uint8_t* uv_row = uv_dst + static_cast<size_t>(row) * pitch;
            if (row < src_half_h) {
                const uint8_t* u_row = u_in + static_cast<size_t>(row) * su;
                const uint8_t* v_row = v_in + static_cast<size_t>(row) * su;
                for (uint32_t col = 0; col < src_half_w && col < half_w; ++col) {
                    uv_row[col * 2 + 0] = u_row[col];
                    uv_row[col * 2 + 1] = v_row[col];
                }
            }
            // Fill the rest of the row (the padding) with neutral chroma.
            const uint32_t filled = (row < src_half_h ? src_half_w : 0u);
            for (uint32_t col = filled; col < half_w; ++col) {
                uv_row[col * 2 + 0] = 128;
                uv_row[col * 2 + 1] = 128;
            }
        }

        lib.fn.nvEncUnlockInputBuffer(session_, in_buf_.inputBuffer);

        // ---- Encode ----
        NV_ENC_PIC_PARAMS pic{};
        pic.version         = NV_ENC_PIC_PARAMS_VER;
        pic.inputBuffer     = in_buf_.inputBuffer;
        pic.bufferFmt       = NV_ENC_BUFFER_FORMAT_NV12;
        pic.pictureStruct   = NV_ENC_PIC_STRUCT_FRAME;
        pic.outputBitstream = out_buf_.bitstreamBuffer;

        if (force_keyframe_next_ || frame_idx_ == 0) {
            pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR |
                                NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
            force_keyframe_next_ = false;
        }

        s = lib.fn.nvEncEncodePicture(session_, &pic);
        if (s != NV_ENC_SUCCESS && s != NV_ENC_ERR_NEED_MORE_INPUT) {
            stats_.encode_errors++;
            NIMRTC_LOG_ERROR("nvenc: nvEncEncodePicture failed (status="
                            << unsigned(s) << ")");
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

        // NVENC already outputs Annex B (00 00 00 01 start codes) for
        // H.264 / H.265, and Annex-B OBU framing for AV1 (the
        // `outputAnnexBFormat` flag is set in the AV1 config).
        encoded_out.codec          = desc_.kind;
        encoded_out.payload        = core::ByteSpan{output, n_copy};
        encoded_out.payload_type   = cfg_.payload_type != 0 ? cfg_.payload_type
                                                          : desc_.default_payload_type;
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
        std::lock_guard<std::mutex> lock(mtx_);

        // The resize target is clamped to the driver's minimum geometry,
        // exactly like `open()` and `encode()` do — otherwise a resize
        // below the minimum would tear the session down and fail to
        // bring it back up.
        const Geometry want = clamp_to_min_geometry(
            cfg.width  > 0 ? cfg.width  : cfg_.width,
            cfg.height > 0 ? cfg.height : cfg_.height);

        const bool want_resize = cfg.width > 0 && cfg.height > 0 &&
                                 opened_ &&
                                 (want.width != width_ || want.height != height_);

        // Remember the requested bitrate / GOP before the (possibly
        // failing) reconfigure, so a later retry uses the same values.
        const uint32_t bitrate = cfg.bitrate_bps;
        const uint32_t gop     = cfg.keyframe_interval;

        cfg_ = std::move(cfg);

        if (!opened_) return plugins::kOk;

        if (want_resize) {
            // A resolution change needs a fresh session — the bitrate
            // rides along with it.
            if (!reinit_at(want.width, want.height)) {
                return plugins::kErrHardwareError;
            }
            return plugins::kOk;
        }

        // A bitrate (or GOP) change applies to the live session.
        return apply_bitrate(bitrate, gop);
    }

    plugins::VideoCodecStats stats() const noexcept override { return stats_; }
    std::uint8_t payload_type() const noexcept override {
        return cfg_.payload_type != 0 ? cfg_.payload_type
                                      : desc_.default_payload_type;
    }

private:
    // ---- Session lifecycle -------------------------------------------------

    /** Destroy the encode session and its buffers.  Safe to call twice. */
    void destroy_session() noexcept {
        auto& lib = NvEncLib::instance();
        if (out_buf_.bitstreamBuffer && session_) {
            lib.fn.nvEncDestroyBitstreamBuffer(session_, out_buf_.bitstreamBuffer);
        }
        if (in_buf_.inputBuffer && session_) {
            lib.fn.nvEncDestroyInputBuffer(session_, in_buf_.inputBuffer);
        }
        if (session_) {
            lib.fn.nvEncDestroyEncoder(session_);
        }
        session_ = nullptr;
        in_buf_  = {};
        out_buf_ = {};
        width_   = 0;
        height_  = 0;
        frame_idx_ = 0;
    }

    /** Open a session and initialize the encoder for @p w x @p h. */
    bool init_session(uint32_t w, uint32_t h) noexcept {
        auto& lib = NvEncLib::instance();

        void* sess = nullptr;
        NVENCSTATUS s = lib.try_open_session(d3d11().dev, &sess);
        if (s != NV_ENC_SUCCESS) {
            if (s == NV_ENC_ERR_INVALID_VERSION) {
                NIMRTC_LOG_ERROR("nvenc: nvEncOpenEncodeSessionEx returned "
                                "NV_ENC_ERR_INVALID_VERSION (15) — the driver "
                                "does not accept the SDK "
                                << NVENCAPI_MAJOR_VERSION << "."
                                << NVENCAPI_MINOR_VERSION << " ABI");
            } else {
                NIMRTC_LOG_ERROR("nvenc: nvEncOpenEncodeSessionEx failed (status="
                                << unsigned(s) << ")");
            }
            return false;
        }
        session_ = sess;

        const uint32_t fps = cfg_.fps > 0 ? cfg_.fps : kDefaultFps;

        NV_ENC_CONFIG encode_cfg = build_encode_config(
            codec_, w, h, fps,
            cfg_.bitrate_bps > 0 ? cfg_.bitrate_bps : kDefaultBitrate,
            cfg_.keyframe_interval > 0 ? cfg_.keyframe_interval : kDefaultGopLength);

        NV_ENC_INITIALIZE_PARAMS init = build_init_params(codec_, w, h, fps,
                                                          &encode_cfg);

        s = lib.fn.nvEncInitializeEncoder(session_, &init);
        if (s != NV_ENC_SUCCESS) {
            NIMRTC_LOG_ERROR("nvenc: nvEncInitializeEncoder failed (status="
                            << unsigned(s) << ")");
            return false;
        }
        NIMRTC_LOG_INFO("nvenc: " << desc_.display_name << " encoder initialized "
                        << w << "x" << h
                        << " @" << init.frameRateNum << " fps, "
                        << (cfg_.bitrate_bps ? cfg_.bitrate_bps : kDefaultBitrate)
                        << " bps, preset P4");

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
            return false;
        }

        NV_ENC_CREATE_BITSTREAM_BUFFER out_buf{};
        out_buf.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        out_buf.size    = (uint32_t)kBsBufBytes;
        s = lib.fn.nvEncCreateBitstreamBuffer(session_, &out_buf);
        if (s != NV_ENC_SUCCESS) {
            NIMRTC_LOG_ERROR("nvenc: nvEncCreateBitstreamBuffer failed ("
                            << unsigned(s) << ")");
            lib.fn.nvEncDestroyInputBuffer(session_, in_buf.inputBuffer);
            return false;
        }

        in_buf_   = in_buf;
        out_buf_  = out_buf;
        width_     = w;
        height_    = h;
        frame_idx_ = 0;
        // The first picture after a (re-)initialization must be an IDR
        // with inline SPS/PPS so the receiver can start decoding.
        force_keyframe_next_ = true;
        return true;
    }

    /** Tear the session down and re-create it for @p w x @p h. */
    bool reinit_at(uint32_t w, uint32_t h) noexcept {
        destroy_session();
        if (!init_session(w, h)) {
            destroy_session();
            broken_ = true;
            return false;
        }
        return true;
    }

    /** Apply a new bitrate / GOP length to the live session. */
    plugins::Status apply_bitrate(uint32_t bitrate_bps, uint32_t gop_length) noexcept {
        if (!session_ || !NvEncLib::instance().fn.nvEncReconfigureEncoder) {
            // No reconfigure available — the new values still take effect
            // on the next re-initialization.
            return plugins::kOk;
        }
        if (bitrate_bps == 0 && gop_length == 0) return plugins::kOk;

        auto& lib = NvEncLib::instance();

        NV_ENC_CONFIG encode_cfg = build_encode_config(
            codec_, width_, height_,
            cfg_.fps > 0 ? cfg_.fps : kDefaultFps,
            bitrate_bps > 0 ? bitrate_bps
                            : (cfg_.bitrate_bps ? cfg_.bitrate_bps : kDefaultBitrate),
            gop_length > 0 ? gop_length
                           : (cfg_.keyframe_interval ? cfg_.keyframe_interval
                                                     : kDefaultGopLength));

        NV_ENC_INITIALIZE_PARAMS init = build_init_params(
            codec_, width_, height_,
            cfg_.fps > 0 ? cfg_.fps : kDefaultFps, &encode_cfg);

        NV_ENC_RECONFIGURE_PARAMS rp{};
        rp.version                = NV_ENC_RECONFIGURE_PARAMS_VER;
        rp.reInitEncodeParams     = init;
        rp.resetEncoder           = 1;
        rp.forceIDR               = 1;

        const NVENCSTATUS s = lib.fn.nvEncReconfigureEncoder(session_, &rp);
        if (s != NV_ENC_SUCCESS) {
            NIMRTC_LOG_WARN("nvenc: nvEncReconfigureEncoder failed (status="
                            << unsigned(s) << ") — the new bitrate applies at "
                            "the next session re-initialization");
            return plugins::kErrHardwareError;
        }

        // The rate-control state was reset, so the next picture has to be
        // an IDR.
        force_keyframe_next_ = true;
        frame_idx_ = 0;
        NIMRTC_LOG_INFO("nvenc: reconfigured the encoder (bitrate="
                        << bitrate_bps << " bps, GOP=" << gop_length << ")");
        return plugins::kOk;
    }

    // ---- State -------------------------------------------------------------

    plugins::VideoCodecConfig cfg_;
    NvCodec             codec_      = NvCodec::kH264;
    NvCodecDesc         desc_{};
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

enum class ProbeResult { Available, NoDriver, NoGpuSession, UnsupportedCodec };

/** Probe the NVENC backend for one codec family.
 *
 *  The probe is *two* steps, and both matter:
 *
 *   1. Open a GPU encode session.  This fails when the driver is
 *      missing or the D3D11 device cannot be created.
 *   2. Actually initialize an encoder for @p codec.  A GPU can have a
 *      working NVENC session and still have no HEVC or no AV1 encoder
 *      (the hardware encoder blocks differ per silicon generation),
 *      and `nvEncInitializeEncoder` is the only call that reports
 *      that honestly.
 *
 *  Step 2 also proves that the driver accepts the SDK-13.1 struct ABI,
 *  which is what the INVALID_VERSION check used to cover. */
ProbeResult probe_nvenc(NvCodec codec) noexcept {
    auto& lib = NvEncLib::instance();
    if (!lib.loaded()) return ProbeResult::NoDriver;
    if (!d3d11().create()) return ProbeResult::NoDriver;

    void* sess = nullptr;
    NVENCSTATUS s = lib.try_open_session(d3d11().dev, &sess);
    if (s != NV_ENC_SUCCESS) {
        if (s == NV_ENC_ERR_INVALID_VERSION) {
            NIMRTC_LOG_WARN("nvenc: nvEncOpenEncodeSessionEx = INVALID_VERSION "
                            "(the driver does not accept the SDK "
                            << NVENCAPI_MAJOR_VERSION << "."
                            << NVENCAPI_MINOR_VERSION << " ABI)");
        } else {
            NIMRTC_LOG_WARN("nvenc: nvEncOpenEncodeSessionEx failed (status="
                            << unsigned(s) << ")");
        }
        return ProbeResult::NoGpuSession;
    }

    // The session is open — now ask the driver whether it can actually
    // encode this codec family.
    constexpr uint32_t kProbeWidth  = 320;
    constexpr uint32_t kProbeHeight = 240;
    NV_ENC_CONFIG enc_cfg = build_encode_config(
        codec, kProbeWidth, kProbeHeight, kDefaultFps,
        kDefaultBitrate, kDefaultGopLength);
    NV_ENC_INITIALIZE_PARAMS init = build_init_params(
        codec, kProbeWidth, kProbeHeight, kDefaultFps, &enc_cfg);

    s = lib.fn.nvEncInitializeEncoder(sess, &init);
    lib.fn.nvEncDestroyEncoder(sess);

    if (s != NV_ENC_SUCCESS) {
        const auto desc = codec_desc(codec);
        NIMRTC_LOG_WARN("nvenc: " << desc.display_name
                        << " encoding is not supported by this GPU "
                        "(nvEncInitializeEncoder status=" << unsigned(s) << ")");
        return ProbeResult::UnsupportedCodec;
    }
    return ProbeResult::Available;
}

} // namespace

} // namespace nvenc_backend

namespace nimrtc::nvcodec {

// ---------------------------------------------------------------------------
// Codec-generic entry points.
//
// The H.264, H.265 and AV1 codec modules do not re-implement the NVENC
// session code — they call these, which is the whole point of
// parameterising the encoder by codec.
// ---------------------------------------------------------------------------

bool nvenc_codec_available(NvCodec codec) noexcept {
#if defined(NIMRTC_PLUGINS_NVENC_ON) && defined(_WIN32)
    return nvenc_backend::probe_nvenc(codec) ==
           nvenc_backend::ProbeResult::Available;
#else
    (void)codec;
    return false;
#endif
}

std::unique_ptr<plugins::IVideoCodec>
make_nvenc_codec(NvCodec codec, plugins::VideoCodecConfig cfg) {
#if defined(NIMRTC_PLUGINS_NVENC_ON) && defined(_WIN32)
    if (!nvenc_codec_available(codec)) {
        NIMRTC_LOG_INFO("nvenc: the " << codec_desc(codec).display_name
                        << " encoder is not available — returning nullptr "
                           "(the engine will use the next backend)");
        return nullptr;
    }
    auto enc = std::make_unique<nvenc_backend::NvEncEncoder>(
        std::move(cfg), codec);
    if (enc->open() != plugins::kOk) {
        NIMRTC_LOG_WARN("nvenc: encoder open() failed — returning nullptr");
        return nullptr;
    }
    return enc;
#else
    (void)codec;
    (void)cfg;
    return nullptr;
#endif
}

} // namespace nimrtc::nvcodec

#else  // NIMRTC_PLUGINS_NVENC_ON

// Stub: SDK not configured at compile time.
//
// The generic entry points still exist so that the H.264 / H.265 / AV1
// codec modules need no `#if` guards of their own: a build without the
// NVIDIA SDK behaves exactly like a build on a host whose GPU cannot
// do the codec.
namespace nimrtc::nvcodec {

bool nvenc_codec_available(NvCodec) noexcept { return false; }
std::unique_ptr<plugins::IVideoCodec>
make_nvenc_codec(NvCodec, plugins::VideoCodecConfig) { return nullptr; }

} // namespace nimrtc::nvcodec

#endif  // NIMRTC_PLUGINS_NVENC_ON
