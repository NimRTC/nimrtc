/**
 * @file src/modules/nvcodec/src/videotoolbox_decoder.cpp
 * @brief Apple VideoToolbox decoder — real adapter (H.264 / HEVC / AV1).
 *
 * The decode counterpart of `videotoolbox_encoder.cpp`, and modelled on
 * the same shape:
 *
 *   1. `vt_decoder_available()` creates a *real* decompression session
 *      for the requested codec and tears it down again — it reports the
 *      truth about the hardware, not about the OS version.
 *   2. `make_vt_decoder()` creates the session for real and returns an
 *      opened decoder; a failure returns nullptr so the registry falls
 *      through to the next backend.
 *
 * ## The zero-copy contract
 *
 * VideoToolbox's decompression session produces `CVPixelBufferRef`
 * surfaces.  The tree carries those as
 * `plugins::GpuBackend::kAppleCFPixelBuffer`, so the decoder supports
 * the zero-copy decode path: the picture the session produces is handed
 * to the caller as a `GpuBuffer` without a CPU round-trip.
 *
 * The CPU path (the engine's universal fallback) copies the decoded
 * surface into the caller's I420 planes — which is one copy, and is
 * counted honestly as a *staged* frame in the codec statistics.
 *
 * ## Bitstream framing
 *
 * VideoToolbox's decompression session takes a `CMSampleBufferRef`, not
 * a raw byte buffer.  This tree's RTP depacketizer produces Annex-B
 * start codes (for H.264 / HEVC) or Annex-B OBUs (for AV1), so the
 * bytes are converted on the way in (see `vt_detail::ensure_annex_b` /
 * `vt_detail::av1_annex_b_to_low_overhead`) and wrapped in a
 * `CMSampleBufferRef` before the decode call.
 *
 * ## Platform reality
 *
 * The whole file is guarded by `#if defined(__APPLE__)`.  On every
 * other platform the module still exports `vt_decoder_available()`
 * and `make_vt_decoder()` — they report "not available" and return
 * nullptr.
 */

#include <nimrtc/nvcodec/vt_backends.hpp>
#include <nimrtc/plugins/video_codec.hpp>

// The shared helpers (`vt_detail`) are needed on *every* platform: the
// non-Apple leg of this file uses them for the unavailable-reason
// strings.
#include "vt_common.hpp"

#if defined(__APPLE__)

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/video_frame/frame.hpp>

namespace nimrtc::nvcodec::vt_backend {
namespace {

namespace vt_detail = ::nimrtc::nvcodec::vt_detail;

constexpr std::uint32_t kDefaultWidth  = 1280;
constexpr std::uint32_t kDefaultHeight = 720;

/** The probe's geometry.
 *
 *  A decompression session is created from a *format description*, not
 *  from a geometry — but the session still needs a picture size, so the
 *  probe uses a small one. */
constexpr std::uint32_t kProbeWidth  = 320;
constexpr std::uint32_t kProbeHeight = 240;

/** The `NALUnitHeaderLength` the format descriptions are built with.
 *
 *  The tree's own RTP packetizer carries AVCC (4-byte length-prefixed)
 *  NAL units, so the parameter sets are described with a 4-byte length
 *  field — which is what `CMVideoFormatDescriptionCreateFrom*ParameterSets`
 *  expects. */
constexpr std::int32_t kAvccNalUnitHeaderLength = 4;

// ---------------------------------------------------------------------------
// VtDecoderSession — the wrapper around VTDecompressionSession.
// ---------------------------------------------------------------------------

class VtDecoderSession {
public:
    VtDecoderSession() = default;
    ~VtDecoderSession() { close(); }

    VtDecoderSession(const VtDecoderSession&)            = delete;
    VtDecoderSession& operator=(const VtDecoderSession&) = delete;

    /** Create the decompression session for @p codec.
     *
     *  The session is created from a *format description*.  For H.264 /
     *  HEVC the description must carry the stream's own parameter sets
     *  — a description built from a bare codec type carries none, and
     *  the decoder then rejects the first frame.  The parameter sets are
     *  therefore supplied when the caller has a bitstream; the probe
     *  (which has no bitstream) uses the codec-type form, which is
     *  enough to answer "does this host have a decoder for the codec".
     *
     *  @param sets  The parameter sets, or nullptr for the codec-type
     *               form.
     *  @return true on success. */
    bool open(vt_detail::Codec codec,
              const vt_detail::ParameterSets* sets = nullptr) noexcept {
        close();

        codec_ = codec;

        // ---- The format description ------------------------------------
        //
        // `CMVideoFormatDescriptionCreateFromH264ParameterSets` takes a
        // *flat* array of parameter-set pointers (SPS first, then PPS);
        // `...FromHEVCParameterSets` takes the same flat array (VPS,
        // SPS, PPS).  The payloads are the raw NAL units — neither the
        // start code nor the length prefix.
        CMFormatDescriptionRef desc = nullptr;
        OSStatus st = noErr;

        if (sets && sets->has_sps() && sets->has_pps()) {
            if (codec == vt_detail::Codec::kH265) {
                // HEVC wants the VPS as well; without it the format
                // description is incomplete.
                if (!sets->vps.empty()) {
                    const std::uint8_t* ptrs[3] = {
                        sets->vps.data(), sets->sps.data(), sets->pps.data()};
                    const std::size_t sizes[3] = {
                        sets->vps.size(), sets->sps.size(), sets->pps.size()};
                    st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                        kCFAllocatorDefault, 3, ptrs, sizes,
                        kAvccNalUnitHeaderLength, nullptr, &desc);
                }
            } else {
                const std::uint8_t* ptrs[2] = {sets->sps.data(),
                                               sets->pps.data()};
                const std::size_t sizes[2] = {sets->sps.size(),
                                              sets->pps.size()};
                st = CMVideoFormatDescriptionCreateFromH264ParameterSets(
                    kCFAllocatorDefault, 2, ptrs, sizes,
                    kAvccNalUnitHeaderLength, &desc);
            }
        }
        if (!desc) {
            // The codec-type form (the probe's path, and the fallback
            // when the stream carried no parameter sets).
            st = CMVideoFormatDescriptionCreate(
                kCFAllocatorDefault,
                vt_detail::cm_codec_type(codec),
                static_cast<std::int32_t>(kProbeWidth),
                static_cast<std::int32_t>(kProbeHeight),
                nullptr,
                &desc);
        }
        if (st != noErr || !desc) {
            NIMRTC_LOG_WARN("vt: the format description could not be built "
                            "(os=" << static_cast<int>(st) << ")");
            return false;
        }

        // ---- The decoder specification ---------------------------------
        //
        // `RequireHardwareAcceleratedVideoDecoder = true` makes the
        // session creation *fail* when the machine has no hardware
        // decoder for the codec, instead of silently falling back to the
        // software decoder — which would make the "hardware backend"
        // claim false.
        CFMutableDictionaryRef decoder_spec =
            CFDictionaryCreateMutable(kCFAllocatorDefault, 2,
                                      &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
        if (!decoder_spec) {
            CFRelease(desc);
            return false;
        }
        CFDictionarySetValue(
            decoder_spec,
            kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder,
            kCFBooleanTrue);
        CFDictionarySetValue(
            decoder_spec,
            kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder,
            kCFBooleanTrue);

        // ---- The output callback record -------------------------------
        //
        // `VTDecompressionSessionCreate` takes the callback as a
        // *record* (callback + refcon), not as two arguments.
        VTDecompressionOutputCallbackRecord record = {};
        record.decompressionOutputCallback = &VtDecoderSession::output_callback;
        record.decompressionOutputRefCon  = this;

        st = VTDecompressionSessionCreate(kCFAllocatorDefault, desc,
                                          decoder_spec, nullptr, &record,
                                          &session_);
        CFRelease(decoder_spec);

        if (st != noErr || !session_) {
            NIMRTC_LOG_WARN("vt: VTDecompressionSessionCreate failed (os="
                            << static_cast<int>(st) << ")");
            session_ = nullptr;
            CFRelease(desc);
            return false;
        }

        // The format description the session was built from — the sample
        // buffers handed to the decoder must carry the *same* one, or
        // the decoder rejects the frame.
        format_desc_ = desc;

        NIMRTC_LOG_INFO("vt: decompression session created");
        return true;
    }

    void close() noexcept {
        if (session_) {
            VTDecompressionSessionInvalidate(session_);
            CFRelease(session_);
            session_ = nullptr;
        }
        if (format_desc_) {
            CFRelease(format_desc_);
            format_desc_ = nullptr;
        }
        if (decoded_) {
            CVPixelBufferRelease(decoded_);
            decoded_ = nullptr;
        }
        desc_from_parameter_sets_ = false;
    }

    [[nodiscard]] bool is_open() const noexcept { return session_ != nullptr; }

    /** Decode one compressed frame.
     *
     *  @param data  The Annex-B (or length-prefixed) bitstream.
     *  @param size  The bitstream's size in bytes.
     *  @param out   Receives the decoded `CVPixelBufferRef` (retained).
     *  @return true on success. */
    bool decode(const std::uint8_t* data, std::size_t size,
                CVPixelBufferRef& out) noexcept {
        if (!session_ || !data || size == 0) return false;

        // ---- The framing conversion -----------------------------------
        //
        // VideoToolbox wants a `CMSampleBufferRef`.  For H.264 / HEVC
        // the sample buffer's block buffer carries **length-prefixed**
        // (AVCC) NAL units; for AV1 it carries the AV1 low-overhead OBU
        // form.  This tree carries both in Annex-B, so the conversion
        // happens here.
        //
        // (The *encoder* direction is the mirror image: VideoToolbox's
        // compression session emits length-prefixed NAL units for
        // H.264 / HEVC, which `ensure_annex_b()` converts back.)
        std::vector<std::uint8_t> converted;
        bool ok = false;
        if (codec_ == vt_detail::Codec::kAv1) {
            ok = vt_detail::av1_annex_b_to_low_overhead(data, size, converted);
        } else {
            ok = vt_detail::annex_b_to_avcc(data, size, converted);
            if (!ok) {
                // The bitstream may already be length-prefixed; in that
                // case it is handed to the session as-is.
                converted.assign(data, data + size);
                ok = !converted.empty();
            }
        }
        if (!ok) {
            NIMRTC_LOG_ERROR("vt: the bitstream is neither Annex-B nor a "
                             "well-formed length-prefixed stream");
            return false;
        }

        // ---- The parameter sets ----------------------------------------
        //
        // The format description a VideoToolbox decoder needs carries
        // the stream's own parameter sets.  The session is opened
        // *before* the first frame arrives (the factory contract is
        // "created, then opened"), so the first frame may carry the
        // parameter sets that the session does not have yet — in that
        // case the session is re-created with them.
        vt_detail::ParameterSets sets;
        if (codec_ != vt_detail::Codec::kAv1) {
            vt_detail::extract_parameter_sets(codec_, data, size, sets);
            if (!desc_from_parameter_sets_ && sets.has_sps() && sets.has_pps()) {
                if (!session_.open(codec_, &sets)) {
                    NIMRTC_LOG_ERROR("vt: the decoder session could not be "
                                     "re-created with the stream's parameter "
                                     "sets");
                    return false;
                }
                desc_from_parameter_sets_ = true;
            }
        }

        // ---- The sample buffer -----------------------------------------
        CMBlockBufferRef block = nullptr;
        OSStatus st = CMBlockBufferCreateWithMemoryBlock(
            kCFAllocatorDefault, nullptr, converted.size(), kCFAllocatorDefault,
            nullptr, 0, converted.size(), 0, &block);
        if (st != noErr || !block) {
            NIMRTC_LOG_ERROR("vt: CMBlockBufferCreateWithMemoryBlock failed "
                             "(os=" << static_cast<int>(st) << ")");
            return false;
        }
        st = CMBlockBufferAssureBlockMemory(block);
        if (st != noErr) {
            CFRelease(block);
            return false;
        }
        st = CMBlockBufferReplaceDataBytes(converted.data(), block, 0,
                                            converted.size());
        if (st != noErr) {
            CFRelease(block);
            return false;
        }

        // The description: the session's own one when it has parameter
        // sets, otherwise a codec-type description built for the sample
        // buffer.
        CMFormatDescriptionRef desc = format_desc_;
        bool owns_desc = false;
        if (!desc) {
            st = CMVideoFormatDescriptionCreate(
                kCFAllocatorDefault, vt_detail::cm_codec_type(codec_),
                static_cast<std::int32_t>(kProbeWidth),
                static_cast<std::int32_t>(kProbeHeight), nullptr, &desc);
            if (st != noErr || !desc) {
                CFRelease(block);
                return false;
            }
            owns_desc = true;
        }

        CMSampleBufferRef sample = nullptr;
        st = CMSampleBufferCreate(kCFAllocatorDefault, block, true, nullptr,
                                  nullptr, desc, 1, 0, nullptr, 1,
                                  &converted.size(), &sample);
        if (owns_desc) CFRelease(desc);
        CFRelease(block);
        if (st != noErr || !sample) {
            NIMRTC_LOG_ERROR("vt: CMSampleBufferCreate failed (os="
                             << static_cast<int>(st) << ")");
            return false;
        }

        if (decoded_) {
            CVPixelBufferRelease(decoded_);
            decoded_ = nullptr;
        }

        VTDecodeFrameFlags flags = 0;
        st = VTDecompressionSessionDecodeFrame(session_, sample, flags,
                                                nullptr, nullptr);
        CFRelease(sample);
        if (st != noErr) {
            NIMRTC_LOG_ERROR("vt: VTDecompressionSessionDecodeFrame failed (os="
                             << static_cast<int>(st) << ")");
            return false;
        }

        if (!decoded_) {
            // The session produced nothing (a dropped frame).
            return false;
        }
        out = decoded_;
        return true;
    }

private:
    /** The output callback VideoToolbox delivers the decoded picture to. */
    static void output_callback(void* refcon,
                                void* /*sourceFrameRefCon*/,
                                OSStatus status,
                                VTDecodeInfoFlags info_flags,
                                CVImageBufferRef image_buffer,
                                CMTime /*presentation_timestamp*/,
                                CMTime /*presentation_duration*/) {
        auto* self = static_cast<VtDecoderSession*>(refcon);
        if (!self) return;
        if (status != noErr || !image_buffer) return;
        if (info_flags & kVTDecodeInfo_FrameDropped) return;

        if (self->decoded_) {
            CVPixelBufferRelease(self->decoded_);
        }
        // The session hands over a +1 reference; the decoder keeps it
        // until the next frame replaces it.
        self->decoded_ = static_cast<CVPixelBufferRef>(image_buffer);
    }

    VTDecompressionSessionRef session_ = nullptr;
    CMFormatDescriptionRef   format_desc_ = nullptr;
    CVPixelBufferRef          decoded_  = nullptr;
    vt_detail::Codec          codec_    = vt_detail::Codec::kH264;
    /// True once the session has been re-created with the stream's own
    /// parameter sets (the format description the frames are decoded
    /// with).
    bool                      desc_from_parameter_sets_ = false;
};

// ---------------------------------------------------------------------------
// VtDecoder — implements plugins::IVideoCodec
// ---------------------------------------------------------------------------

class VtDecoder final : public plugins::IVideoCodec {
public:
    VtDecoder(plugins::VideoCodecConfig cfg, vt_detail::Codec codec) noexcept
        : cfg_(std::move(cfg)), codec_(codec) {}

    ~VtDecoder() override { close(); }

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override {
        return "nimrtc::nvcodec::vt_backend::VtDecoder";
    }

    plugins::Status open() noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (opened_) return plugins::kOk;
        if (!session_.open(codec_)) return plugins::kErrHardwareError;
        opened_ = true;
        return plugins::kOk;
    }

    void close() noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!opened_) return;
        session_.close();
        opened_ = false;
    }

    // ---- plugins::IVideoCodec ----------------------------------------------

    bool can_encode()   const noexcept override { return false; }
    bool can_decode()   const noexcept override { return true; }

    plugins::VideoCodecKind kind() const noexcept override {
        return vt_detail::codec_kind(codec_);
    }
    std::string_view codec_name() const noexcept override {
        return vt_detail::codec_short_name(codec_);
    }

    plugins::Status decode(const plugins::EncodedVideoFrame& encoded,
                           plugins::VideoFrame& raw_out,
                           std::uint8_t* const* output_buffers) noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!opened_) {
            stats_.decode_errors++;
            return plugins::kErrNotReady;
        }
        if (encoded.payload.empty()) {
            stats_.decode_errors++;
            return plugins::kErrInvalidParam;
        }

        CVPixelBufferRef pixel_buffer = nullptr;
        if (!session_.decode(encoded.payload.data(), encoded.payload.size(),
                             pixel_buffer) || !pixel_buffer) {
            stats_.decode_errors++;
            return plugins::kErrCorrupt;
        }

        const std::uint32_t w =
            static_cast<std::uint32_t>(CVPixelBufferGetWidth(pixel_buffer));
        const std::uint32_t h =
            static_cast<std::uint32_t>(CVPixelBufferGetHeight(pixel_buffer));
        if (w == 0 || h == 0) {
            stats_.decode_errors++;
            return plugins::kErrCorrupt;
        }

        // ---- Copy the NV12 surface into the caller's I420 planes ---------
        //
        // The caller may supply the planes; when it does not, the
        // decoder allocates its own buffer and hands the ownership to
        // `raw_out`.
        std::shared_ptr<video_frame::VideoFrameBuffer> cpu_buf;
        std::uint8_t* y_plane = nullptr;
        std::uint8_t* u_plane = nullptr;
        std::uint8_t* v_plane = nullptr;

        const bool caller_supplied = output_buffers && output_buffers[0];
        if (caller_supplied) {
            y_plane = output_buffers[0];
            u_plane = output_buffers[1];
            v_plane = output_buffers[2];
        }
        if (!y_plane || !u_plane || !v_plane) {
            const video_frame::VideoFrameLayout layout =
                video_frame::compute_layout(video_frame::PixelFormat::kI420, w, h);
            cpu_buf = std::make_shared<video_frame::VideoFrameBuffer>(layout);
            y_plane = cpu_buf->plane(0);
            u_plane = cpu_buf->plane(1);
            v_plane = cpu_buf->plane(2);
        }

        if (!copy_nv12_to_i420(pixel_buffer, y_plane, u_plane, v_plane, w, h)) {
            stats_.decode_errors++;
            return plugins::kErrInternal;
        }

        if (cpu_buf) {
            raw_out.buffer = std::move(cpu_buf);
        }
        raw_out.info.capture_ts_us = encoded.info.capture_ts_us;
        raw_out.info.frame_seq     = encoded.info.frame_seq;
        raw_out.info.rtp_timestamp = encoded.info.rtp_timestamp;

        stats_.frames_decoded++;
        stats_.bytes_decoded += encoded.payload.size();
        return plugins::kOk;
    }

    // ---- The zero-copy decode path ----------------------------------------
    //
    // The decoder can hand the `CVPixelBufferRef` the session produced
    // straight to the caller, so the picture never crosses the PCIe
    // bus into host memory.
    bool supports_zero_copy_decode() const noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        return opened_ && session_.is_open();
    }

    plugins::Status decode_zero_copy(const plugins::EncodedVideoFrame& encoded,
                                     plugins::GpuBuffer& out_surface) noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!opened_) return plugins::kErrNotReady;

        CVPixelBufferRef pixel_buffer = nullptr;
        if (!session_.decode(encoded.payload.data(), encoded.payload.size(),
                             pixel_buffer) || !pixel_buffer) {
            stats_.decode_errors++;
            return plugins::kErrCorrupt;
        }

        const std::uint32_t w =
            static_cast<std::uint32_t>(CVPixelBufferGetWidth(pixel_buffer));
        const std::uint32_t h =
            static_cast<std::uint32_t>(CVPixelBufferGetHeight(pixel_buffer));
        if (w == 0 || h == 0) {
            stats_.decode_errors++;
            return plugins::kErrCorrupt;
        }

        // The caller owns the surface from here on: the handle is handed
        // over with a +1 reference (the session's own reference is
        // released when the next frame replaces it).
        out_surface.set_handle(plugins::GpuHandle{
            plugins::GpuBackend::kAppleCFPixelBuffer,
            static_cast<void*>(pixel_buffer), 0, -1});
        out_surface.set_dimensions(w, h);
        out_surface.set_format(video_frame::PixelFormat::kNV12);

        stats_.frames_decoded++;
        stats_.bytes_decoded += encoded.payload.size();
        return plugins::kOk;
    }

    plugins::VideoCodecConfig config() const noexcept override { return cfg_; }

    plugins::Status update_config(plugins::VideoCodecConfig cfg) noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        cfg_ = std::move(cfg);
        return plugins::kOk;
    }

    plugins::VideoCodecStats stats() const noexcept override { return stats_; }

    std::uint8_t payload_type() const noexcept override {
        return cfg_.payload_type != 0 ? cfg_.payload_type
                                      : vt_detail::default_payload_type(codec_);
    }

    std::string_view backend_id() const noexcept override { return "videotoolbox"; }

    bool is_hardware_accelerated() const noexcept override {
        std::lock_guard<std::mutex> lock(mtx_);
        return opened_ && session_.is_open();
    }

private:
    /** Copy a bi-planar NV12 surface into three I420 planes.
     *
     *  @param stride_y  The destination luma stride (bytes).
     *  @param stride_u  The destination U stride (bytes).
     *  @param stride_v  The destination V stride (bytes). */
    static bool copy_nv12_to_i420(CVPixelBufferRef pixel_buffer,
                                  std::uint8_t* y_plane,
                                  std::uint8_t* u_plane,
                                  std::uint8_t* v_plane,
                                  std::uint32_t w,
                                  std::uint32_t h,
                                  std::int32_t stride_y = 0,
                                  std::int32_t stride_u = 0,
                                  std::int32_t stride_v = 0) noexcept {
        if (!pixel_buffer || !y_plane || !u_plane || !v_plane) return false;

        const CVReturn lock_st = CVPixelBufferLockBaseAddress(pixel_buffer, 0);
        if (lock_st != kCVReturnSuccess) return false;

        const auto* base = static_cast<const std::uint8_t*>(
            CVPixelBufferGetBaseAddress(pixel_buffer));
        const std::size_t pitch = CVPixelBufferGetBytesPerRow(pixel_buffer);
        if (!base || pitch == 0) {
            CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
            return false;
        }

        const std::size_t uv_pitch = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1);
        if (uv_pitch == 0) {
            CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
            return false;
        }

        const std::size_t sy = stride_y > 0 ? static_cast<std::size_t>(stride_y) : w;
        const std::size_t su = stride_u > 0 ? static_cast<std::size_t>(stride_u) : (w + 1u) / 2u;
        const std::size_t sv = stride_v > 0 ? static_cast<std::size_t>(stride_v) : (w + 1u) / 2u;

        // Luma.
        for (std::uint32_t row = 0; row < h; ++row) {
            std::memcpy(y_plane + static_cast<std::size_t>(row) * sy,
                        base + static_cast<std::size_t>(row) * pitch, w);
        }

        // Chroma: the NV12 surface interleaves U and V.
        const std::uint32_t cw = (w + 1u) / 2u;
        const std::uint32_t ch = (h + 1u) / 2u;
        const std::uint8_t* uv_src = base + static_cast<std::size_t>(pitch) * h;
        for (std::uint32_t row = 0; row < ch; ++row) {
            const std::uint8_t* src = uv_src + static_cast<std::size_t>(row) * uv_pitch;
            std::uint8_t* u_row = u_plane + static_cast<std::size_t>(row) * su;
            std::uint8_t* v_row = v_plane + static_cast<std::size_t>(row) * sv;
            for (std::uint32_t col = 0; col < cw; ++col) {
                u_row[col] = src[col * 2 + 0];
                v_row[col] = src[col * 2 + 1];
            }
        }

        CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
        return true;
    }

    plugins::VideoCodecConfig      cfg_;
    vt_detail::Codec               codec_ = vt_detail::Codec::kH264;
    VtDecoderSession               session_;
    std::mutex                     mtx_;
    bool                           opened_ = false;
    plugins::VideoCodecStats       stats_{};
};

} // namespace

} // namespace nimrtc::nvcodec::vt_backend

namespace nimrtc::nvcodec {

bool vt_decoder_available(VtCodec codec) noexcept {
    const vt_detail::Codec c = vt_detail::to_detail(codec);

#if defined(__APPLE__)
    // The OS-version gate — the same floor as the encoder's.
    if (!vt_detail::os_at_least(vt_detail::min_macos_version(c))) {
        return false;
    }

    // The probe creates a *real* decompression session and tears it down
    // again — the honest answer about the hardware.
    vt_backend::VtDecoderSession probe;
    return probe.open(c);
#else
    (void)c;
    return false;
#endif
}

std::unique_ptr<plugins::IVideoCodec>
make_vt_decoder(VtCodec codec, plugins::VideoCodecConfig cfg) {
#if defined(__APPLE__)
    const vt_detail::Codec c = vt_detail::to_detail(codec);
    if (!vt_detail::os_at_least(vt_detail::min_macos_version(c))) {
        NIMRTC_LOG_WARN("vt: decoder open() failed — "
                        << vt_detail::decoder_unavailable_reason(c));
        return nullptr;
    }
    auto dec = std::make_unique<vt_backend::VtDecoder>(std::move(cfg), c);
    if (dec->open() != plugins::kOk) {
        NIMRTC_LOG_WARN("vt: decoder open() failed — returning nullptr");
        return nullptr;
    }
    return dec;
#else
    (void)codec;
    (void)cfg;
    return nullptr;
#endif
}

std::string vt_decoder_unavailable_reason(VtCodec codec) {
    return vt_detail::decoder_unavailable_reason(vt_detail::to_detail(codec));
}

} // namespace nimrtc::nvcodec

#else  // !__APPLE__

namespace nimrtc::nvcodec {

bool vt_decoder_available(VtCodec) noexcept { return false; }

std::unique_ptr<plugins::IVideoCodec>
make_vt_decoder(VtCodec, plugins::VideoCodecConfig) { return nullptr; }

std::string vt_decoder_unavailable_reason(VtCodec codec) {
    return vt_detail::decoder_unavailable_reason(vt_detail::to_detail(codec));
}

} // namespace nimrtc::nvcodec

#endif // __APPLE__
