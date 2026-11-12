/**
 * @file src/modules/nvcodec/src/nvdec_decoder.cpp
 * @brief NVIDIA NVDEC decoder — hardware decode via nvcuvid.dll
 *        (H.264 / H.265 / AV1).
 *
 * Entry points (wired into the codec modules' backend tables):
 *   bool    nvdec_codec_available(codec)  → runtime probe
 *   make_nvdec_codec(codec, cfg)          → real NvDecDecoder or nullptr
 *
 * ## Why a separate file from nvenc_encoder.cpp
 *
 * NVENC and NVDEC live in *different* runtime DLLs:
 *
 *   - nvEncodeAPI64.dll  → NVENC (encoder)
 *   - nvcuvid.dll        → NVDEC (decoder)
 *   - nvcuda.dll         → CUDA driver API (both need a current context)
 *
 * Keeping them apart means the decoder can be enabled (or disabled)
 * independently of the encoder, and each file only pulls in the headers
 * it actually needs.
 *
 * ## Codec parameterisation
 *
 * The decoder is *codec-parameterised*: the same parser / decoder code
 * serves H.264, HEVC and AV1, and only the CUVID `CodecType` differs
 * (see `nv_cuvid_codec`).  The availability probe asks
 * `cuvidGetDecoderCaps` about the *specific* codec, so a GPU that
 * decodes H.264 and HEVC but has no AV1 unit reports `false` for AV1
 * rather than "yes, because NVDEC exists".
 *
 * ## Decode pipeline
 *
 * NVDEC is a *parser + decoder* pair.  The client never hands raw NAL
 * units to the decoder directly:
 *
 *   1. cuvidCreateVideoParser()  — created with a CUDA context current.
 *   2. cuvidParseVideoData()     — fed the Annex-B access unit.  The
 *      parser synchronously invokes three callbacks:
 *
 *        pfnSequenceCallback  — a new SPS arrived (or the resolution
 *                               changed).  We create / recreate the
 *                               hardware decoder here.
 *        pfnDecodePicture     — one picture is ready to decode *in
 *                               decode order*.  We call
 *                               cuvidDecodePicture(), map the frame with
 *                               cuvidMapVideoFrame64(), copy it to host
 *                               memory and unmap it.
 *        pfnDisplayPicture    — display-order notification.  We do not
 *                               use it: the engine consumes frames in
 *                               decode order and the NVENC encoder is
 *                               configured for low latency (no B-frames),
 *                               so decode order == display order.
 *
 *   3. cuvidDestroyVideoParser() on close.
 *
 * ## Surface layout
 *
 * The decoder is configured with
 * `OutputFormat = cudaVideoSurfaceFormat_NV12`, so the mapped surface is
 * a semi-planar 4:2:0 buffer in *device* memory:
 *
 *   Y plane  : coded_height rows of `pitch` bytes
 *   UV plane : coded_height/2 rows of `pitch` bytes, interleaved U,V
 *
 * The UV plane starts at byte offset `pitch * coded_height`.  We copy
 * both planes to host memory with cuMemcpy2D() and de-interleave the
 * chroma into separate U / V planes so the engine's I420 contract is
 * honoured.
 *
 * `CUVIDEOFORMAT::display_area` is honoured: NVDEC frequently decodes
 * into a macroblock-aligned frame (e.g. 1280x736 for 720p) and the
 * *visible* picture is a sub-rectangle of it.  We crop on the way out.
 *
 * ## Availability probe
 *
 * nvdec_codec_available() loads nvcuvid.dll + nvcuda.dll, initialises
 * the CUDA driver, selects the first GPU and asks cuvidGetDecoderCaps()
 * whether the requested codec / 4:2:0 / 8-bit is supported.  It
 * performs no allocation, so it is cheap enough to call at engine
 * start-up.
 */

#include <nimrtc/nvcodec/nv_backends.hpp>

#if defined(NIMRTC_PLUGINS_NVDEC_ON)

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
    #define NOMINMAX
  #endif
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0A00
  #endif
  #include <windows.h>
#endif

// The vendored dynamic-loader headers.  We silence the loader's own
// fprintf-based error reporting and route failures through the NimRTC
// logger instead.
#define FFNV_LOG_FUNC(logctx, ...) ((void)0)
#define FFNV_DEBUG_LOG_FUNC(logctx, ...) ((void)0)
#include "dynlink_loader.h"

#include <nimrtc/core/log.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/video_frame/frame.hpp>

namespace nimrtc::nvcodec::nvdec_backend {
namespace {

// ---------------------------------------------------------------------------
// CUDA + CUVID library singletons
// ---------------------------------------------------------------------------

struct NvDecLib {
    CudaFunctions* cuda   = nullptr;
    CuvidFunctions* cuvid = nullptr;

    static NvDecLib& instance() {
        static NvDecLib inst;
        return inst;
    }

    bool loaded() const noexcept { return cuda != nullptr && cuvid != nullptr; }

private:
    NvDecLib() {
        if (cuda_load_functions(&cuda, nullptr) != 0) {
            cuda = nullptr;
            NIMRTC_LOG_WARN("nvdec: cannot load nvcuda.dll — NVDEC unavailable");
            return;
        }
        if (cuvid_load_functions(&cuvid, nullptr) != 0) {
            cuvid = nullptr;
            cuda_free_functions(&cuda);
            cuda = nullptr;
            NIMRTC_LOG_WARN("nvdec: cannot load nvcuvid.dll — NVDEC unavailable");
            return;
        }
        NIMRTC_LOG_INFO("nvdec: nvcuda.dll + nvcuvid.dll loaded");
    }

    ~NvDecLib() {
        if (cuvid) cuvid_free_functions(&cuvid);
        if (cuda)  cuda_free_functions(&cuda);
    }
};

// ---------------------------------------------------------------------------
// CUDA context holder
// ---------------------------------------------------------------------------
//
// NVDEC's cuvid* entry points require a current CUDA context.  We retain
// the *primary* context of the first GPU (the same context every other
// CUDA consumer in the process would use) and push / pop it around every
// driver call.
//
// The context is retained once and shared by all decoder instances; the
// push / pop pair makes the "current context" per-thread.
//
// Note: CUDA's primary context is reference counted, so two decoder
// instances (or a decoder plus another CUDA consumer) coexist safely.
// ---------------------------------------------------------------------------

class CudaContext {
public:
    static CudaContext& instance() {
        static CudaContext ctx;
        return ctx;
    }

    bool ready() const noexcept { return ctx_ != nullptr; }

    /** Push the retained primary context onto the calling thread's stack. */
    bool push() noexcept {
        if (!ctx_) return false;
        auto* fn = NvDecLib::instance().cuda;
        if (!fn || !fn->cuCtxPushCurrent) return false;
        return fn->cuCtxPushCurrent(ctx_) == CUDA_SUCCESS;
    }

    /** Pop the context pushed by push(). */
    void pop() noexcept {
        auto* fn = NvDecLib::instance().cuda;
        if (!fn || !fn->cuCtxPopCurrent) return;
        CUcontext popped = nullptr;
        fn->cuCtxPopCurrent(&popped);
    }

    /** Initialise the driver and retain the primary context of device 0. */
    bool init() noexcept {
        if (ctx_) return true;
        auto& lib = NvDecLib::instance();
        if (!lib.loaded()) return false;
        auto* fn = lib.cuda;

        if (fn->cuInit(0) != CUDA_SUCCESS) {
            NIMRTC_LOG_ERROR("nvdec: cuInit failed");
            return false;
        }
        int count = 0;
        if (!fn->cuDeviceGetCount || fn->cuDeviceGetCount(&count) != CUDA_SUCCESS ||
            count <= 0) {
            NIMRTC_LOG_ERROR("nvdec: no CUDA device found");
            return false;
        }
        if (!fn->cuDeviceGet || fn->cuDeviceGet(&dev_, 0) != CUDA_SUCCESS) {
            NIMRTC_LOG_ERROR("nvdec: cuDeviceGet(0) failed");
            return false;
        }
        if (!fn->cuDevicePrimaryCtxRetain ||
            fn->cuDevicePrimaryCtxRetain(&ctx_, dev_) != CUDA_SUCCESS) {
            NIMRTC_LOG_ERROR("nvdec: cuDevicePrimaryCtxRetain failed");
            ctx_ = nullptr;
            return false;
        }

        // NVDEC requires compute capability 3.0 or newer.
        int major = 0;
        if (fn->cuDeviceGetAttribute &&
            fn->cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR,
                                     dev_) == CUDA_SUCCESS && major < 3) {
            NIMRTC_LOG_WARN("nvdec: device compute capability " << major
                            << " is too old for NVDEC");
            fn->cuDevicePrimaryCtxRelease(dev_);
            ctx_ = nullptr;
            return false;
        }

        NIMRTC_LOG_INFO("nvdec: CUDA context ready (device 0, CC " << major << ".x)");
        return true;
    }

private:
    CudaContext() = default;

    CUcontext ctx_ = nullptr;
    CUdevice  dev_  = 0;
};

// ---------------------------------------------------------------------------
// NvDecDecoder — implements plugins::IVideoCodec (decode direction)
// ---------------------------------------------------------------------------

/** Map an internal codec kind onto the CUVID codec type.
 *
 *  This mapping is *driver-specific* and therefore deliberately lives in
 *  this translation unit rather than in the public `nv_codec_kind.hpp`
 *  header: the CUVID enums are only visible when the NVDEC headers are
 *  on the include path, and the public header must stay usable in
 *  builds without the NVIDIA SDK. */
cudaVideoCodec nv_cuvid_codec(NvCodec codec) noexcept {
    switch (codec) {
    case NvCodec::kH264: return cudaVideoCodec_H264;
    case NvCodec::kH265: return cudaVideoCodec_HEVC;
    case NvCodec::kAv1:  return cudaVideoCodec_AV1;
    case NvCodec::kUnsupported: break;
    }
    return cudaVideoCodec_H264;
}

class NvDecDecoder final : public plugins::IVideoCodec {
public:
    /** @param cfg    The caller's configuration.
     *  @param codec  Which codec family this decoder consumes. */
    explicit NvDecDecoder(plugins::VideoCodecConfig cfg,
                          NvCodec codec) noexcept
        : cfg_(std::move(cfg)), codec_(codec),
          desc_(codec_desc(codec)) {}

    ~NvDecDecoder() override { close(); }

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override {
        return "nimrtc::nvcodec::nvdec_backend::NvDecDecoder";
    }

    plugins::Status open() noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (opened_) return plugins::kOk;

        auto& lib = NvDecLib::instance();
        if (!lib.loaded()) {
            NIMRTC_LOG_ERROR("nvdec: nvcuvid.dll / nvcuda.dll not available");
            return plugins::kErrHardwareError;
        }
        if (!CudaContext::instance().init()) {
            NIMRTC_LOG_ERROR("nvdec: cannot initialise the CUDA context");
            return plugins::kErrHardwareError;
        }

        // Create the video parser.  The parser callbacks are static
        // functions that receive `this` as pUserData.
        CUVIDPARSERPARAMS pp{};
        pp.CodecType              = nv_cuvid_codec(codec_);
        // The parser's DPB size.  It must be large enough to hold the
        // reference frames of a P-frame stream: with a DPB of 1 the parser
        // has no surface to reference, so every non-IDR picture fails to
        // decode.  16 is the H.264 maximum number of reference frames
        // (the CUVIDH264DPBENTRY array is 16 entries wide), so it is a
        // safe upper bound for any conforming stream.
        //
        // The exact value is refined per sequence: the sequence callback
        // returns the driver's own `min_num_decode_surfaces` to override
        // this (see handle_sequence()).
        pp.ulMaxNumDecodeSurfaces = 16;
        pp.ulMaxDisplayDelay      = 1;
        pp.ulClockRate            = 0;      // default 10 MHz
        pp.ulErrorThreshold       = 0;
        pp.pUserData              = this;
        pp.pfnSequenceCallback    = &NvDecDecoder::on_sequence;
        pp.pfnDecodePicture       = &NvDecDecoder::on_decode_picture;
        pp.pfnDisplayPicture      = &NvDecDecoder::on_display_picture;

        if (!CudaContext::instance().push()) {
            NIMRTC_LOG_ERROR("nvdec: cannot push the CUDA context");
            return plugins::kErrHardwareError;
        }
        CUresult r = lib.cuvid->cuvidCreateVideoParser(&parser_, &pp);
        CudaContext::instance().pop();

        if (r != CUDA_SUCCESS || !parser_) {
            NIMRTC_LOG_ERROR("nvdec: cuvidCreateVideoParser failed (" << r << ")");
            parser_ = nullptr;
            return plugins::kErrHardwareError;
        }

        opened_ = true;
        NIMRTC_LOG_INFO("nvdec: video parser created");
        return plugins::kOk;
    }

    void close() noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!opened_) return;
        auto& lib = NvDecLib::instance();

        if (parser_) {
            CudaContext::instance().push();
            if (lib.cuvid) lib.cuvid->cuvidDestroyVideoParser(parser_);
            CudaContext::instance().pop();
            parser_ = nullptr;
        }
        destroy_decoder();

        opened_ = false;
        NIMRTC_LOG_INFO("nvdec: decoder closed");
    }

    // ---- plugins::IVideoCodec ----------------------------------------------

    bool can_encode() const noexcept override { return false; }
    bool can_decode() const noexcept override { return true; }

    plugins::VideoCodecKind kind() const noexcept override {
        return desc_.kind;
    }
    std::string_view codec_name() const noexcept override {
        return desc_.rtpmap_encoding;
    }

    plugins::Status encode(const plugins::VideoFrame&, std::uint8_t*,
                           std::size_t, plugins::EncodedVideoFrame&) noexcept override {
        return plugins::kErrUnsupported;
    }

    plugins::Status decode(const plugins::EncodedVideoFrame& encoded,
                           plugins::VideoFrame& raw_out,
                           std::uint8_t* const* output_buffers) noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!opened_ || !parser_) return plugins::kErrNotReady;
        if (encoded.payload.empty() || encoded.payload.data() == nullptr) {
            stats_.decode_errors++;
            return plugins::kErrInvalidParam;
        }

        // The caller may either supply the output planes or leave the
        // allocation to the decoder.  The decoder-owned mode is the one a
        // hardware decoder needs: only it knows the native decoded
        // geometry (the bitstream's SPS), so only it can size the buffer
        // correctly.  The caller-supplied mode stays supported for
        // callers that already know the geometry.
        const bool caller_supplied = output_buffers && output_buffers[0] != nullptr;

        // Reset the per-frame latch.  The parser fires the decode
        // callback synchronously from inside cuvidParseVideoData().
        have_frame_ = false;

        CUVIDSOURCEDATAPACKET pkt{};
        pkt.flags        = CUVID_PKT_ENDOFPICTURE;
        pkt.payload_size = static_cast<tcu_ulong>(encoded.payload.size());
        pkt.payload      = encoded.payload.data();

        auto& lib = NvDecLib::instance();
        CudaContext::instance().push();
        CUresult r = lib.cuvid->cuvidParseVideoData(parser_, &pkt);
        CudaContext::instance().pop();

        if (r != CUDA_SUCCESS) {
            stats_.decode_errors++;
            NIMRTC_LOG_WARN("nvdec: cuvidParseVideoData failed (" << r << ")");
            return plugins::kErrCorrupt;
        }

        if (!have_frame_) {
            // The parser buffered the data without completing a picture
            // (e.g. a non-IDR frame arriving before its reference).  Not
            // an error — just nothing to show yet.
            stats_.frames_skipped++;
            return plugins::kErrNotReady;
        }

        // ---- the decoded geometry ----
        const std::uint32_t ow = out_w_;
        const std::uint32_t oh = out_h_;
        if (ow == 0 || oh == 0) {
            stats_.decode_errors++;
            return plugins::kErrCorrupt;
        }

        // The output strides must match the frame layout the caller used to
        // size its buffers, so they are derived from the layout rather than
        // hand-computed here.
        //
        //   I420 : stride_y = align16(w), stride_u = stride_v = align16(w/2)
        //   NV12 : stride_y = align16(w), stride_uv = align16(w)   <- note:
        //          the UV plane is *interleaved*, so one row of it holds
        //          w bytes (w/2 chroma samples), not w/2 bytes.
        const bool nv12_out = (cfg_.pixel_format == plugins::VideoPixelFormat::kNV12);
        const auto out_layout = video_frame::compute_layout(
            nv12_out ? video_frame::PixelFormat::kNV12
                     : video_frame::PixelFormat::kI420, ow, oh);
        const std::uint32_t sy = out_layout.strides[0];
        const std::uint32_t su = out_layout.strides[1];

        if (caller_supplied) {
            // The caller sized its buffers from cfg_.width / cfg_.height.
            // If the stream carries a different resolution the copy below
            // would run past the end of those buffers, so refuse instead of
            // corrupting memory.
            if (cfg_.width && cfg_.height && (ow > cfg_.width || oh > cfg_.height)) {
                NIMRTC_LOG_ERROR("nvdec: decoded frame " << ow << "x" << oh
                                << " does not fit the configured "
                                << cfg_.width << "x" << cfg_.height
                                << " output buffers");
                stats_.decode_errors++;
                return plugins::kErrInvalidParam;
            }
        }

        // ---- the destination planes ----
        //
        // In the decoder-owned mode the buffer is allocated here with the
        // native layout, and the staged pixels are written straight into
        // it.  In the caller-supplied mode the pixels go into the
        // caller's memory.
        std::shared_ptr<video_frame::VideoFrameBuffer> owned_buf;
        std::uint8_t* y_dst = nullptr;
        std::uint8_t* u_dst = nullptr;
        std::uint8_t* v_dst = nullptr;

        if (caller_supplied) {
            y_dst = output_buffers[0];
            u_dst = output_buffers[1];
            v_dst = output_buffers[2];
        } else {
            owned_buf = std::make_shared<video_frame::VideoFrameBuffer>(out_layout);
            if (!owned_buf || !owned_buf->data()) {
                stats_.decode_errors++;
                return plugins::kErrInternal;
            }
            y_dst = owned_buf->plane(0);
            u_dst = owned_buf->plane(1);
            v_dst = owned_buf->plane(2);
        }

        // Luma.
        for (std::uint32_t row = 0; row < oh; ++row) {
            std::memcpy(y_dst + static_cast<std::size_t>(row) * sy,
                        stage_y_.data() + static_cast<std::size_t>(row) * ow, ow);
        }

        if (nv12_out) {
            // Interleaved UV straight out of the device surface.
            for (std::uint32_t row = 0; row < oh / 2; ++row) {
                std::memcpy(u_dst + static_cast<std::size_t>(row) * su,
                            stage_uv_.data() + static_cast<std::size_t>(row) * ow, ow);
            }
        } else {
            // De-interleave the chroma into separate U / V planes.
            const std::uint32_t cw = ow / 2;
            for (std::uint32_t row = 0; row < oh / 2; ++row) {
                const std::uint8_t* src = stage_uv_.data()
                                        + static_cast<std::size_t>(row) * ow;
                std::uint8_t* u = u_dst + static_cast<std::size_t>(row) * su;
                std::uint8_t* v = v_dst + static_cast<std::size_t>(row) * su;
                for (std::uint32_t col = 0; col < cw; ++col) {
                    u[col] = src[col * 2];
                    v[col] = src[col * 2 + 1];
                }
            }
        }

        // ---- publish the frame metadata -------------------------------------
        raw_out.info.capture_ts_us = encoded.info.capture_ts_us;
        raw_out.info.frame_seq     = encoded.info.frame_seq;
        raw_out.info.rtp_timestamp = encoded.info.rtp_timestamp;
        // The dimensions and the pixel format are carried by the buffer's
        // layout (VideoFrame::width() / height() / format() read them from
        // there), so publishing the buffer below is enough.

        if (owned_buf) {
            // The decoder owns the pixels: hand the buffer to the caller.
            raw_out.buffer = std::move(owned_buf);
        } else {
            // The caller owns the pixels.  Expose them through the buffer
            // too, so a consumer that inspects `raw_out.buffer` sees a
            // valid frame.  The layout matches the strides we just used,
            // and the copy is done plane by plane — a flat memcpy of
            // buffer_size bytes would read past the end of the caller's Y
            // plane whenever the chroma planes are not contiguous with it.
            auto buf = std::make_shared<video_frame::VideoFrameBuffer>(out_layout);
            if (buf && buf->data()) {
                for (std::uint32_t row = 0; row < oh; ++row) {
                    std::memcpy(buf->plane(0) + static_cast<std::size_t>(row) * out_layout.strides[0],
                                y_dst + static_cast<std::size_t>(row) * sy, ow);
                }
                if (nv12_out) {
                    for (std::uint32_t row = 0; row < oh / 2; ++row) {
                        std::memcpy(buf->plane(1) + static_cast<std::size_t>(row) * out_layout.strides[1],
                                    u_dst + static_cast<std::size_t>(row) * su, ow);
                    }
                } else {
                    for (std::uint32_t row = 0; row < oh / 2; ++row) {
                        std::memcpy(buf->plane(1) + static_cast<std::size_t>(row) * out_layout.strides[1],
                                    u_dst + static_cast<std::size_t>(row) * su, ow / 2);
                        std::memcpy(buf->plane(2) + static_cast<std::size_t>(row) * out_layout.strides[2],
                                    v_dst + static_cast<std::size_t>(row) * su, ow / 2);
                    }
                }
            }
            raw_out.buffer = std::move(buf);
        }

        stats_.frames_decoded++;
        stats_.bytes_decoded += encoded.payload.size();
        return plugins::kOk;
    }

    plugins::Status force_keyframe() noexcept override {
        // A decoder never emits keyframes; the request is a no-op.
        return plugins::kOk;
    }

    bool supports_zero_copy_encode() const noexcept override { return false; }
    bool supports_zero_copy_decode() const noexcept override { return false; }

    plugins::VideoCodecConfig config() const noexcept override { return cfg_; }

    plugins::Status update_config(plugins::VideoCodecConfig cfg) noexcept override {
        // The decoder's width/height are an *upper bound* on the picture
        // it may receive — the real geometry comes from the bitstream's
        // SPS.  A caller that forwards an encoder's frame size (a local
        // capture resolution) would therefore narrow the bound and make
        // the decoder reject a larger remote picture.  The bound is
        // therefore kept at the larger of the two.
        const std::uint32_t w = std::max(cfg.width, cfg_.width);
        const std::uint32_t h = std::max(cfg.height, cfg_.height);
        cfg_                 = std::move(cfg);
        cfg_.width           = w;
        cfg_.height          = h;
        return plugins::kOk;
    }

    plugins::VideoCodecStats stats() const noexcept override { return stats_; }

    std::uint8_t payload_type() const noexcept override {
        return cfg_.payload_type != 0 ? cfg_.payload_type : std::uint8_t{102};
    }

private:
    // ---- parser callbacks (invoked synchronously from cuvidParseVideoData)

    static int CUDAAPI on_sequence(void* user_data, CUVIDEOFORMAT* fmt) {
        auto* self = static_cast<NvDecDecoder*>(user_data);
        if (!self || !fmt) return 0;
        // A return value > 1 overrides the parser's DPB size with the
        // driver's own `min_num_decode_surfaces` for this sequence, which
        // is the number the driver guarantees is enough for correct
        // decoding.  Returning 0 would abort the parse.
        return self->handle_sequence(fmt);
    }

    static int CUDAAPI on_decode_picture(void* user_data, CUVIDPICPARAMS* pic) {
        auto* self = static_cast<NvDecDecoder*>(user_data);
        if (!self || !pic) return 0;
        return self->handle_picture(pic) ? 1 : 0;
    }

    static int CUDAAPI on_display_picture(void*, CUVIDPARSERDISPINFO*) {
        // Display order is not used — the engine consumes frames in decode
        // order and the encoder is configured without B-frames.
        return 1;
    }

    // ---- sequence handling -------------------------------------------------

    /** Handle a new sequence.  Returns the DPB size the parser should use
     *  for this sequence (0 = failure, aborting the parse). */
    int handle_sequence(CUVIDEOFORMAT* fmt) {
        const std::uint32_t cw = fmt->coded_width  ? fmt->coded_width  : cfg_.width;
        const std::uint32_t ch = fmt->coded_height ? fmt->coded_height : cfg_.height;
        if (cw == 0 || ch == 0) return 0;

        // The visible picture is display_area; fall back to the coded size.
        std::uint32_t left = 0, top = 0, right = cw, bottom = ch;
        if (fmt->display_area.right > fmt->display_area.left &&
            fmt->display_area.bottom > fmt->display_area.top) {
            left  = static_cast<std::uint32_t>(fmt->display_area.left);
            top   = static_cast<std::uint32_t>(fmt->display_area.top);
            right = static_cast<std::uint32_t>(fmt->display_area.right);
            bottom = static_cast<std::uint32_t>(fmt->display_area.bottom);
        }
        // Chroma coordinates must be even.
        left  &= ~1u;
        top   &= ~1u;
        right &= ~1u;
        bottom &= ~1u;
        if (right <= left || bottom <= top) { left = 0; top = 0; right = cw; bottom = ch; }

        out_w_ = right - left;
        out_h_ = bottom - top;
        // The driver performs the display-area crop for us (see
        // CUVIDDECODECREATEINFO::display_area below), so the mapped surface
        // is already the *visible* size and no host-side crop is needed.

        // (Re)create the hardware decoder for this sequence.
        destroy_decoder();

        auto& lib = NvDecLib::instance();
        if (!lib.cuvid || !lib.cuvid->cuvidCreateDecoder) return 0;

        CUVIDDECODECREATEINFO dci{};
        dci.CodecType          = nv_cuvid_codec(codec_);
        dci.ChromaFormat       = cudaVideoChromaFormat_420;
        dci.OutputFormat       = cudaVideoSurfaceFormat_NV12;
        dci.bitDepthMinus8     = 0;
        dci.ulNumDecodeSurfaces = std::max(1u,
            static_cast<std::uint32_t>(fmt->min_num_decode_surfaces));
        // ulNumOutputSurfaces is the number of output surfaces that may be
        // *simultaneously mapped*.  It must be non-zero: with 0 the driver
        // allocates no mappable output surface and cuvidMapVideoFrame()
        // fails with CUDA_ERROR_INVALID_VALUE.  One is enough here because
        // the decoder maps and copies one picture at a time.
        //
        // Note: the header's comment ("must be set to 0 when using
        // externally allocated memory for output") only applies to the
        // externally-allocated path, which this backend does not use.
        dci.ulNumOutputSurfaces = 1;
        dci.ulIntraDecodeOnly  = 0;
        dci.DeinterlaceMode    = cudaVideoDeinterlaceMode_Weave;
        dci.ulWidth            = cw;
        dci.ulHeight           = ch;
        dci.ulMaxWidth         = cw;
        dci.ulMaxHeight        = ch;

        // Post-processing geometry.  Both must be non-zero: with
        // ulTargetWidth/Height == 0 the driver has no output geometry and
        // cuvidMapVideoFrame() fails with CUDA_ERROR_INVALID_VALUE.
        //
        // Setting the target to the *display* size (rather than the coded
        // size) together with display_area makes the driver crop the
        // macroblock-aligned coded frame down to the visible picture in
        // one hardware step — NVDEC decodes 1280x736 for a 720p stream but
        // only 1280x720 is visible.
        dci.ulTargetWidth  = out_w_;
        dci.ulTargetHeight = out_h_;
        dci.display_area.left   = static_cast<short>(left);
        dci.display_area.top    = static_cast<short>(top);
        dci.display_area.right  = static_cast<short>(right);
        dci.display_area.bottom = static_cast<short>(bottom);

        CUresult r = lib.cuvid->cuvidCreateDecoder(&decoder_, &dci);
        if (r != CUDA_SUCCESS || !decoder_) {
            NIMRTC_LOG_ERROR("nvdec: cuvidCreateDecoder failed (" << r << ") for "
                             << cw << "x" << ch);
            decoder_ = nullptr;
            return 0;
        }
        coded_w_ = cw;
        coded_h_ = ch;
        NIMRTC_LOG_INFO("nvdec: decoder created for " << cw << "x" << ch
                        << " (display " << out_w_ << "x" << out_h_ << ")");

        // Override the parser's DPB size with the driver's own minimum for
        // this sequence.  A return value > 1 tells the parser to use it.
        //
        // The driver reports min_num_decode_surfaces = 8 for a 320x240
        // H.264 stream (it is sized for the worst-case reference chain),
        // and the parser's DPB has to be at least that large or P-frames
        // have no reference surface to decode against.  Clamp to the
        // parser's maximum (16) in case the driver asks for more.
        const int dpb = static_cast<int>(fmt->min_num_decode_surfaces);
        return dpb > 1 ? dpb : 2;
    }

    void destroy_decoder() noexcept {
        auto& lib = NvDecLib::instance();
        if (decoder_ && lib.cuvid && lib.cuvid->cuvidDestroyDecoder) {
            lib.cuvid->cuvidDestroyDecoder(decoder_);
        }
        decoder_ = nullptr;
    }

    // ---- picture handling --------------------------------------------------

    bool handle_picture(CUVIDPICPARAMS* pic) {
        auto& lib = NvDecLib::instance();
        if (!decoder_ || !lib.cuvid) return false;

        CUresult r = lib.cuvid->cuvidDecodePicture(decoder_, pic);
        if (r != CUDA_SUCCESS) {
            NIMRTC_LOG_WARN("nvdec: cuvidDecodePicture failed (" << r << ")");
            return false;
        }

        // Map the decoded surface.
        //
        // The CUVIDPROCPARAMS argument must NOT be NULL on this driver:
        // cuvidMapVideoFrame() rejects a NULL VPP descriptor with
        // CUDA_ERROR_INVALID_VALUE.  The struct enables the driver-side
        // post-processing that was configured at cuvidCreateDecoder() time
        // (display-area crop + target size), which is why the zeroed
        // `progressive_frame` field is correct here — the source is a
        // progressive frame and the deinterlace mode was already fixed to
        // Weave at creation time.
        CUVIDPROCPARAMS proc{};
        proc.progressive_frame = 1;

        unsigned long long dev_ptr = 0;
        unsigned int pitch = 0;
        r = lib.cuvid->cuvidMapVideoFrame(decoder_, pic->CurrPicIdx,
                                          &dev_ptr, &pitch, &proc);
        if (r != CUDA_SUCCESS || dev_ptr == 0) {
            NIMRTC_LOG_WARN("nvdec: cuvidMapVideoFrame failed (" << r << ")");
            return false;
        }

        const bool ok = copy_surface_to_host(static_cast<CUdeviceptr>(dev_ptr), pitch);

        lib.cuvid->cuvidUnmapVideoFrame(decoder_, dev_ptr);
        if (ok) have_frame_ = true;
        return ok;
    }

    /** Copy the mapped NV12 device surface into the host staging buffers.
     *
     *  The surface geometry is the *target* geometry that was requested at
     *  cuvidCreateDecoder() time (the display area, already cropped by the
     *  driver), so the surface is out_w_ x out_h_ pixels with the driver
     *  reported `pitch` stride. */
    bool copy_surface_to_host(CUdeviceptr dev_ptr, unsigned int pitch) {
        auto& lib = NvDecLib::instance();
        auto* fn = lib.cuda;
        if (!fn || !fn->cuMemcpy2D) return false;
        if (pitch == 0) pitch = out_w_;

        const std::uint32_t sh = out_h_;   // surface height (== target height)
        const std::uint32_t ow = out_w_;
        const std::uint32_t oh = out_h_;

        stage_y_.resize(static_cast<std::size_t>(ow) * oh);
        stage_uv_.resize(static_cast<std::size_t>(ow) * (oh / 2));
        if (stage_y_.empty() || stage_uv_.empty()) return false;

        // ---- luma ----
        {
            CUDA_MEMCPY2D c{};
            c.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            c.srcDevice     = dev_ptr;
            c.srcPitch      = pitch;
            c.dstMemoryType = CU_MEMORYTYPE_HOST;
            c.dstHost       = stage_y_.data();
            c.dstPitch      = ow;
            c.WidthInBytes  = ow;
            c.Height        = oh;
            if (fn->cuMemcpy2D(&c) != CUDA_SUCCESS) {
                NIMRTC_LOG_WARN("nvdec: luma copy failed");
                return false;
            }
        }

        // ---- chroma (NV12: U,V interleaved) ----
        {
            // In an NV12 surface the chroma plane is interleaved, so one
            // byte-pair covers two luma columns.  The chroma plane starts
            // right below the luma plane, at byte offset pitch * height.
            CUDA_MEMCPY2D c{};
            c.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            c.srcDevice     = dev_ptr + static_cast<CUdeviceptr>(pitch) * sh;
            c.srcPitch      = pitch;
            c.dstMemoryType = CU_MEMORYTYPE_HOST;
            c.dstHost       = stage_uv_.data();
            c.dstPitch      = ow;
            c.WidthInBytes  = ow;
            c.Height        = oh / 2u;
            if (fn->cuMemcpy2D(&c) != CUDA_SUCCESS) {
                NIMRTC_LOG_WARN("nvdec: chroma copy failed");
                return false;
            }
        }
        return true;
    }

    // ---- state -------------------------------------------------------------

    plugins::VideoCodecConfig cfg_;
    NvCodec               codec_ = NvCodec::kUnsupported;
    NvCodecDesc           desc_;
    std::mutex              mtx_;
    bool                    opened_      = false;
    bool                    have_frame_  = false;

    CUvideoparser           parser_      = nullptr;
    CUvideodecoder          decoder_     = nullptr;

    std::uint32_t coded_w_   = 0;
    std::uint32_t coded_h_   = 0;
    std::uint32_t out_w_     = 0;
    std::uint32_t out_h_     = 0;

    std::vector<std::uint8_t> stage_y_;
    std::vector<std::uint8_t> stage_uv_;

    plugins::VideoCodecStats stats_{};
};

// ---------------------------------------------------------------------------
// Availability probe
// ---------------------------------------------------------------------------

/** Ask the driver whether it can decode @p codec in 4:2:0 8-bit.
 *
 *  This is the *honest* probe: it asks `cuvidGetDecoderCaps` about the
 *  specific codec family, so a GPU that can decode H.264 and HEVC but
 *  has no AV1 unit reports `false` for AV1 rather than "yes, because
 *  NVDEC exists".
 *
 *  The chroma format and bit depth are pinned to 4:2:0 / 8-bit because
 *  that is the only configuration this decoder implements — asking for
 *  a capability the backend cannot actually use would be a lie. */
bool probe_nvdec(NvCodec codec) noexcept {
    auto& lib = NvDecLib::instance();
    if (!lib.loaded()) return false;
    if (!lib.cuvid->cuvidGetDecoderCaps) return false;
    if (!CudaContext::instance().init()) return false;

    const NvCodecDesc desc = codec_desc(codec);
    if (desc.codec == NvCodec::kUnsupported) return false;

    CUVIDDECODECAPS caps{};
    caps.eCodecType      = nv_cuvid_codec(codec);
    caps.eChromaFormat   = cudaVideoChromaFormat_420;
    caps.nBitDepthMinus8 = 0;

    CudaContext::instance().push();
    CUresult r = lib.cuvid->cuvidGetDecoderCaps(&caps);
    CudaContext::instance().pop();

    if (r != CUDA_SUCCESS || !caps.bIsSupported) {
        NIMRTC_LOG_WARN("nvdec: " << desc.display_name
                        << " 4:2:0 8-bit decode is not supported by this GPU");
        return false;
    }
    return true;
}

} // namespace

} // namespace nimrtc::nvcodec::nvdec_backend

namespace nimrtc::nvcodec {

// ---------------------------------------------------------------------------
// Codec-generic entry points (see nimrtc/nvcodec/nv_backends.hpp)
//
// The H.264 / H.265 / AV1 codec modules all call these — they do not
// re-implement the driver integration.
// ---------------------------------------------------------------------------

bool nvdec_codec_available(NvCodec codec) noexcept {
#if defined(NIMRTC_PLUGINS_NVDEC_ON) && defined(_WIN32)
    return nvdec_backend::probe_nvdec(codec);
#else
    (void)codec;
    return false;
#endif
}

std::unique_ptr<plugins::IVideoCodec>
make_nvdec_codec(NvCodec codec, plugins::VideoCodecConfig cfg) {
#if defined(NIMRTC_PLUGINS_NVDEC_ON) && defined(_WIN32)
    if (codec == NvCodec::kUnsupported) return nullptr;
    if (!nvdec_codec_available(codec)) {
        NIMRTC_LOG_INFO("nvdec: no hardware decode for "
                        << codec_desc(codec).display_name
                        << " — returning nullptr (engine will use the next backend)");
        return nullptr;
    }
    auto dec = std::make_unique<nvdec_backend::NvDecDecoder>(std::move(cfg), codec);
    if (dec->open() != plugins::kOk) {
        NIMRTC_LOG_WARN("nvdec: decoder open() failed — returning nullptr");
        return nullptr;
    }
    return dec;
#else
    (void)codec;
    (void)cfg;
    return nullptr;
#endif
}

} // namespace nimrtc::nvcodec

#else  // NIMRTC_PLUGINS_NVDEC_ON

// Stub: SDK not configured at compile time.
//
// The generic entry points still exist so that the H.264 / H.265 / AV1
// codec modules need no `#if` guards of their own: a build without the
// NVIDIA SDK behaves exactly like a build on a host whose GPU cannot
// do the codec.
namespace nimrtc::nvcodec {

bool nvdec_codec_available(NvCodec) noexcept { return false; }
std::unique_ptr<plugins::IVideoCodec>
make_nvdec_codec(NvCodec, plugins::VideoCodecConfig) { return nullptr; }

} // namespace nimrtc::nvcodec

#endif  // NIMRTC_PLUGINS_NVDEC_ON
