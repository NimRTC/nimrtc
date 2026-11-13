/**
 * @file src/modules/h264/src/openh264_encoder.cpp
 * @brief OpenH264 software H.264 encoder + decoder plugin.
 *
 * A *real* software H.264 codec: it encodes raw I420 frames with
 * OpenH264's `ISVCEncoder` and decodes the resulting Annex B bitstream
 * with OpenH264's `ISVCDecoder`.  This is the CPU bring-up baseline the
 * remote-desktop plan asks for (WP-B): the pipeline carries genuine
 * H.264, not a synthetic bitstream.
 *
 * WHY THIS FILE EXISTS ALONGSIDE THE STUB
 * ---------------------------------------
 * The stub codec (`CodecPluginAdapter` in codec_plugin.cpp) emits a
 * synthetic Annex B bitstream shaped like H.264 but carrying no real
 * macroblocks.  It exists so the RTP / SRTP / ICE / FU-A pipeline can be
 * exercised without a codec.  It is NOT a video codec, and it reports
 * `backend_id() == "software-stub"` so a host can tell the two apart.
 *
 * The OpenH264 backend reports `backend_id() == "openh264"`.
 *
 * BUILD
 * -----
 * The implementation is compiled only when `NIMRTC_PLUGINS_OPENH264_ON`
 * is defined, which cmake/NimRTHwPlugins.cmake sets when
 * `NIMRTC_PLUGINS_OPENH264=ON` AND the vendored OpenH264 source tree is
 * present under `src/third_party/openh264/src`.  Without it the file
 * compiles to the "unavailable" stub and the registry falls through to
 * the software-stub backend.
 */

#include <nimrtc/h264/hw_backends.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/h264/codec_plugin.hpp>
#include <nimrtc/plugins/video_codec.hpp>

#if defined(NIMRTC_PLUGINS_OPENH264_ON)
  #include "codec_api.h"
#endif

namespace nimrtc::h264 {

#if defined(NIMRTC_PLUGINS_OPENH264_ON)

// ---------------------------------------------------------------------------
// OpenH264Codec — the real software H.264 codec.
//
// One instance owns ONE encoder and ONE decoder.  The engine drives both
// directions through the same IVideoCodec, so a single instance must be
// able to do both.
// ---------------------------------------------------------------------------
class Openh264Codec final : public plugins::IVideoCodec {
public:
    explicit Openh264Codec(plugins::VideoCodecConfig cfg) noexcept
        : cfg_(std::move(cfg)) {}

    ~Openh264Codec() override { close(); }

    Openh264Codec(const Openh264Codec&)            = delete;
    Openh264Codec& operator=(const Openh264Codec&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override {
        return "nimrtc::h264::Openh264Codec (vendored OpenH264, CPU)";
    }

    plugins::Status open() noexcept override {
        if (open_) return plugins::kOk;

        const std::uint32_t w = cfg_.width  ? cfg_.width  : 640;
        const std::uint32_t h = cfg_.height ? cfg_.height : 480;
        if (w == 0 || h == 0 || (w % 2) != 0 || (h % 2) != 0) {
            core::log::Logger::instance().error(
                "openh264: refusing to open with an odd or zero frame size");
            return plugins::kErrInvalidParam;
        }
        // OpenH264 requires even dimensions for 4:2:0 chroma.
        width_  = w & ~1u;
        height_ = h & ~1u;

        const plugins::Status enc_status = open_encoder();
        if (enc_status != plugins::kOk) return enc_status;

        const plugins::Status dec_status = open_decoder();
        if (dec_status != plugins::kOk) {
            close_encoder();
            return dec_status;
        }

        open_ = true;
        core::log::Logger::instance().info(
            "openh264: opened " + std::to_string(width_) + "x" +
            std::to_string(height_) + " software H.264 codec");
        return plugins::kOk;
    }

    void close() noexcept override {
        close_encoder();
        close_decoder();
        open_ = false;
    }

    // ---- plugins::IVideoCodec ----------------------------------------------

    // Both directions are supported by this backend: it owns an
    // OpenH264 encoder *and* an OpenH264 decoder.  These report
    // capability, not "is the session open yet" — every other backend
    // (stub, NVENC, NVDEC, …) answers the same way, and the engine
    // inspects the flags before open().
    bool can_encode() const noexcept override { return true; }
    bool can_decode() const noexcept override { return true; }

    plugins::VideoCodecKind kind() const noexcept override {
        return plugins::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override { return "h264"; }

    plugins::Status encode(const plugins::VideoFrame& raw,
                           std::uint8_t* output, std::size_t output_capacity,
                           plugins::EncodedVideoFrame& encoded_out) noexcept override {
        if (!encoder_) {
            stats_.encode_errors++;
            return plugins::kErrNotReady;
        }
        if (!output || output_capacity == 0) {
            stats_.encode_errors++;
            return plugins::kErrInvalidParam;
        }

        const std::uint32_t w = raw.width()  ? raw.width()  : width_;
        const std::uint32_t h = raw.height() ? raw.height() : height_;
        if (w != width_ || h != height_) {
            // The encoder is configured for a fixed geometry.  Rather than
            // rejecting the frame (which would break every host whose
            // capture resolution differs from the engine's configured
            // default), re-create the encoder for the new geometry.
            //
            // The *decoder* needs no re-init: it takes its geometry from
            // the bitstream's own parameter sets, so it must survive.
            const std::uint32_t new_w = w & ~1u;
            const std::uint32_t new_h = h & ~1u;
            if (new_w == 0 || new_h == 0) {
                stats_.encode_errors++;
                return plugins::kErrInvalidParam;
            }
            close_encoder();
            width_  = new_w;
            height_ = new_h;
            cfg_.width  = new_w;
            cfg_.height = new_h;
            const plugins::Status s = open_encoder();
            if (s != plugins::kOk) {
                stats_.encode_errors++;
                return s;
            }
            core::log::Logger::instance().info(
                "openh264: re-configured the encoder for " +
                std::to_string(width_) + "x" + std::to_string(height_));
        }

        // ---- Gather the source planes -------------------------------------
        std::shared_ptr<video_frame::VideoFrameBuffer> buf = raw.cpu_buffer();
        if (!buf) {
            stats_.encode_errors++;
            return plugins::kErrInvalidParam;
        }
        const std::uint8_t* y_plane = buf->plane(0);
        const std::uint8_t* u_plane = buf->plane(1);
        const std::uint8_t* v_plane = buf->plane(2);
        if (!y_plane || !u_plane || !v_plane) {
            stats_.encode_errors++;
            return plugins::kErrInvalidParam;
        }
        const video_frame::VideoFrameLayout& layout = buf->layout();

        // ---- Feed OpenH264 ------------------------------------------------
        SSourcePicture pic{};
        memset(&pic, 0, sizeof(pic));
        pic.iColorFormat = videoFormatI420;
        pic.iPicWidth    = static_cast<int>(width_);
        pic.iPicHeight   = static_cast<int>(height_);
        pic.iStride[0]   = layout.strides[0];
        pic.iStride[1]   = layout.strides[1];
        pic.iStride[2]   = layout.strides[2];
        pic.pData[0]     = const_cast<std::uint8_t*>(y_plane);
        pic.pData[1]     = const_cast<std::uint8_t*>(u_plane);
        pic.pData[2]     = const_cast<std::uint8_t*>(v_plane);
        pic.uiTimeStamp  = raw.info.capture_ts_us / 1000;   // ms

        SFrameBSInfo info{};
        memset(&info, 0, sizeof(info));

        const int rv = encoder_->EncodeFrame(&pic, &info);
        if (rv != cmResultSuccess) {
            stats_.encode_errors++;
            return plugins::kErrInternal;
        }
        if (info.eFrameType == videoFrameTypeSkip) {
            // The rate controller dropped the frame.  Report it as a skip
            // rather than an error — the caller may retry or drop it.
            stats_.frames_skipped++;
            return plugins::kErrUnsupported;
        }

        // ---- Flatten the layer bitstreams into one Annex B buffer ---------
        //
        // OpenH264 hands back one SLayerBSInfo per spatial layer; each
        // carries `iNalCount` NAL units of `pNalLengthInByte[i]` bytes.
        // With `bSimulcastAVC = true` and a single spatial layer there is
        // exactly one layer, and the NALs already carry their 4-byte Annex
        // B start codes (added unconditionally by upstream's
        // `WelsEncodeNal`).
        //
        // IMPORTANT: each layer's `pBsBuf` points at its *own* offset
        // inside upstream's shared frame buffer, so the NAL bytes of a
        // layer start at `pBsBuf`, not at the previous layer's end.
        std::size_t total = 0;
        for (int layer = 0; layer < info.iLayerNum; ++layer) {
            const SLayerBSInfo& lbi = info.sLayerInfo[layer];
            for (int nal = 0; nal < lbi.iNalCount; ++nal) {
                total += static_cast<std::size_t>(lbi.pNalLengthInByte[nal]);
            }
        }
        if (total == 0 || total > output_capacity) {
            stats_.encode_errors++;
            return plugins::kErrBufferTooSmall;
        }

        std::size_t pos = 0;
        for (int layer = 0; layer < info.iLayerNum; ++layer) {
            const SLayerBSInfo& lbi = info.sLayerInfo[layer];
            std::size_t layer_pos = 0;
            for (int nal = 0; nal < lbi.iNalCount; ++nal) {
                const std::size_t len = static_cast<std::size_t>(lbi.pNalLengthInByte[nal]);
                if (pos + len > output_capacity) {
                    stats_.encode_errors++;
                    return plugins::kErrBufferTooSmall;
                }
                std::memcpy(output + pos, lbi.pBsBuf + layer_pos, len);
                pos       += len;
                layer_pos += len;
            }
        }

        // ---- Fill the encoded frame descriptor ----------------------------
        encoded_out.codec        = plugins::VideoCodecKind::kH264;
        encoded_out.payload      = core::ByteSpan{output, pos};
        encoded_out.payload_type = cfg_.payload_type ? cfg_.payload_type
                                                     : std::uint8_t{102};
        encoded_out.is_keyframe  = (info.eFrameType == videoFrameTypeIDR);
        encoded_out.nalu_format  = plugins::NaluFormat::kAnnexBStartCode;
        encoded_out.info.capture_ts_us = raw.info.capture_ts_us;
        encoded_out.info.frame_seq     = raw.info.frame_seq;
        encoded_out.info.rtp_timestamp = raw.info.rtp_timestamp;

        stats_.frames_encoded++;
        stats_.bytes_encoded += pos;
        if (encoded_out.is_keyframe) stats_.keyframes_encoded++;
        return plugins::kOk;
    }

    plugins::Status decode(const plugins::EncodedVideoFrame& encoded,
                           plugins::VideoFrame& raw_out,
                           std::uint8_t* const* output_buffers) noexcept override {
        if (!decoder_) {
            stats_.decode_errors++;
            return plugins::kErrNotReady;
        }
        if (encoded.payload.empty() || encoded.payload.data() == nullptr) {
            stats_.decode_errors++;
            return plugins::kErrInvalidParam;
        }

        unsigned char* planes[3] = {nullptr, nullptr, nullptr};
        SBufferInfo buf_info{};
        memset(&buf_info, 0, sizeof(buf_info));

        // NOTE: `DecodeFrameNoDelay()` is *not* used here.  In its
        // single-threaded path upstream calls `DecodeFrame2()` twice —
        // once with the payload and once with a NULL payload to flush —
        // and the second call clears the `SBufferInfo` that the first
        // call filled, so the decoded picture is lost.  (Upstream's own
        // console decoder works around this by calling `DecodeFrame2()`
        // directly and inspecting the result after each call; see
        // src/codec/console/dec/src/h264dec.cpp.)  We do the same.
        const int rv = decoder_->DecodeFrame2(
            reinterpret_cast<const unsigned char*>(encoded.payload.data()),
            static_cast<int>(encoded.payload.size()),
            planes, &buf_info);

        if (rv != 0) {
            stats_.decode_errors++;
            return plugins::kErrCorrupt;
        }

        if (buf_info.iBufferStatus != 1 || !planes[0]) {
            // No picture came out of the access unit yet.  Two distinct
            // upstream mechanisms are at work here, and both have to be
            // driven explicitly:
            //
            // 1. **Access-unit completion.**  OpenH264's NAL parser only
            //    closes an access unit when it sees the *next* AU's start
            //    (or at end-of-stream).  A payload carrying exactly one
            //    picture therefore stays pending until something tells
            //    the decoder "there is no more data in this chunk".
            //    `DecodeFrame2(NULL, 0)` is that signal.
            //
            // 2. **POC reordering.**  For a Main/High profile stream
            //    (OpenH264's default when CABAC is on) upstream does not
            //    hand the picture to the caller straight away: it parks
            //    it in a reorder queue (`BufferingReadyPicture()`) and
            //    releases it only once the reordering logic decides the
            //    picture is next in display order.  The queue is drained
            //    by `FlushFrame()`, which — per upstream's own console
            //    decoder — only runs once the end-of-stream flag is set.
            //
            // So the sequence is: flush the AU, then drain the reorder
            // queue.  Doing it after *every* access unit keeps the
            // codec delay-free: otherwise the picture for frame N would
            // only surface when frame N+1 arrived.
            unsigned char* flush_planes[3] = {nullptr, nullptr, nullptr};
            SBufferInfo flush_info{};
            memset(&flush_info, 0, sizeof(flush_info));
            decoder_->DecodeFrame2(nullptr, 0, flush_planes, &flush_info);

            if (flush_info.iBufferStatus == 1 && flush_planes[0]) {
                // The AU completed and upstream handed the picture over
                // directly (the baseline path).
                planes[0] = flush_planes[0];
                planes[1] = flush_planes[1];
                planes[2] = flush_planes[2];
                buf_info = flush_info;
            } else {
                // The picture is parked in the reorder queue.  Drain it.
                int end_of_stream = 1;
                decoder_->SetOption(DECODER_OPTION_END_OF_STREAM, &end_of_stream);

                unsigned char* drained_planes[3] = {nullptr, nullptr, nullptr};
                SBufferInfo drained_info{};
                memset(&drained_info, 0, sizeof(drained_info));
                decoder_->FlushFrame(drained_planes, &drained_info);

                // The end-of-stream flag only matters for the drain
                // above; the next real access unit clears it inside
                // DecodeFrame2().
                if (drained_info.iBufferStatus != 1 || !drained_planes[0]) {
                    // Not an error: the frame was buffered and will come
                    // out on a later call.
                    return plugins::kErrNotReady;
                }
                planes[0] = drained_planes[0];
                planes[1] = drained_planes[1];
                planes[2] = drained_planes[2];
                buf_info = drained_info;
            }
        }

        const int dec_w = buf_info.UsrData.sSystemBuffer.iWidth;
        const int dec_h = buf_info.UsrData.sSystemBuffer.iHeight;
        if (dec_w <= 0 || dec_h <= 0) {
            stats_.decode_errors++;
            return plugins::kErrCorrupt;
        }
        const int dec_stride_y = buf_info.UsrData.sSystemBuffer.iStride[0];
        const int dec_stride_c = buf_info.UsrData.sSystemBuffer.iStride[1];

        // ---- Resolve the destination buffers --------------------------------
        //
        // Same three-way contract as the stub adapter:
        //   1. The caller supplied buffers and raw_out already owns a
        //      matching VideoFrameBuffer → reuse it.
        //   2. The caller supplied buffers but raw_out owns no matching
        //      buffer → allocate a fresh one for raw_out, paint into the
        //      caller's memory.
        //   3. No caller buffers → allocate a fresh one and let raw_out
        //      own it; the decoder's own planes are the source.
        const bool caller_supplied = (output_buffers && output_buffers[0]);
        std::shared_ptr<video_frame::VideoFrameBuffer> cpu_buf;

        if (caller_supplied) {
            if (auto existing = raw_out.cpu_buffer();
                existing && existing->data() == output_buffers[0]) {
                cpu_buf = std::move(existing);
            }
        }
        if (!cpu_buf) {
            const video_frame::VideoFrameLayout layout =
                video_frame::compute_layout(video_frame::PixelFormat::kI420,
                                            static_cast<std::uint32_t>(dec_w),
                                            static_cast<std::uint32_t>(dec_h));
            cpu_buf = std::make_shared<video_frame::VideoFrameBuffer>(layout);
        }

        // ---- Copy the decoded planes into the destination --------------------
        //
        // The decoder's planes are owned by OpenH264 and stay valid only
        // until the next DecodeFrameNoDelay() call, so they always have to
        // be copied.  The *destination* is either the caller's memory
        // (tightly packed — the interface carries no stride information,
        // so the planes are assumed to be stride == width, which is what
        // the engine's decode path and the module tests use) or our own
        // layout (whose strides are padded to a 16-byte boundary).
        const std::uint32_t out_w = static_cast<std::uint32_t>(dec_w);
        const std::uint32_t out_h = static_cast<std::uint32_t>(dec_h);
        if (caller_supplied) {
            copy_plane(output_buffers[0], static_cast<std::int32_t>(out_w),
                       planes[0], dec_stride_y, out_w, out_h);
            copy_plane(output_buffers[1], static_cast<std::int32_t>(out_w / 2),
                       planes[1], dec_stride_c, out_w / 2, out_h / 2);
            copy_plane(output_buffers[2], static_cast<std::int32_t>(out_w / 2),
                       planes[2], dec_stride_c, out_w / 2, out_h / 2);
        } else {
            const video_frame::VideoFrameLayout& layout = cpu_buf->layout();
            copy_plane(cpu_buf->plane(0), layout.strides[0],
                       planes[0], dec_stride_y, out_w, out_h);
            copy_plane(cpu_buf->plane(1), layout.strides[1],
                       planes[1], dec_stride_c, out_w / 2, out_h / 2);
            copy_plane(cpu_buf->plane(2), layout.strides[2],
                       planes[2], dec_stride_c, out_w / 2, out_h / 2);
        }

        raw_out.buffer = std::move(cpu_buf);
        raw_out.info.capture_ts_us = encoded.info.capture_ts_us;
        raw_out.info.frame_seq     = encoded.info.frame_seq;
        raw_out.info.rtp_timestamp = encoded.info.rtp_timestamp;

        stats_.frames_decoded++;
        stats_.bytes_decoded += encoded.payload.size();
        return plugins::kOk;
    }

    plugins::Status force_keyframe() noexcept override {
        if (!encoder_) return plugins::kErrNotReady;
        const int rv = encoder_->ForceIntraFrame(true);
        return (rv == cmResultSuccess) ? plugins::kOk : plugins::kErrInternal;
    }

    plugins::VideoCodecConfig config() const noexcept override { return cfg_; }

    plugins::Status update_config(plugins::VideoCodecConfig cfg) noexcept override {
        // The bitrate is the only parameter OpenH264 can change live
        // without a full re-init; a geometry change needs a re-init, which
        // the caller has to request by closing and reopening the codec.
        cfg_ = std::move(cfg);
        if (encoder_ && cfg_.bitrate_bps > 0) {
            SBitrateInfo info{};
            info.iLayer   = SPATIAL_LAYER_ALL;
            info.iBitrate = static_cast<int>(cfg_.bitrate_bps);
            const int rv = encoder_->SetOption(ENCODER_OPTION_BITRATE, &info);
            if (rv != cmResultSuccess) {
                core::log::Logger::instance().warn(
                    "openh264: SetOption(ENCODER_OPTION_BITRATE) failed");
            }
        }
        return plugins::kOk;
    }

    // ---- The parameter set (remote-desktop plan B12 / B13) -----------------

    plugins::Status set_params(
            const plugins::VideoEncoderParams& params) noexcept override {
        // A parameter set may leave the geometry out entirely (the
        // caller only wants to change, say, the bitrate).  But it may
        // not state *half* a geometry: "480 rows, no width" is a
        // contradiction, and quietly ignoring it would leave the
        // encoder coding at a size the caller did not ask for.
        if ((params.width == 0) != (params.height == 0)) {
            return plugins::kErrInvalidParam;
        }

        // The live knobs first: they survive a restart, and applying
        // them before the restart means the re-created encoder is
        // configured with the new values from the start.
        plugins::VideoCodecConfig cfg = cfg_;
        if (params.fps != 0)            cfg.fps = params.fps;
        if (params.bitrate_bps != 0)    cfg.bitrate_bps = params.bitrate_bps;
        if (params.keyframe_interval != 0) {
            cfg.keyframe_interval = params.keyframe_interval;
        }
        cfg.pixel_format = params.pixel_format;

        if (!params.has_geometry()) {
            // A partial update: the geometry stays, the live knobs move.
            return update_config(cfg);
        }

        // OpenH264 needs *even* dimensions for 4:2:0 chroma.
        const std::uint32_t new_w = params.width  & ~1u;
        const std::uint32_t new_h = params.height & ~1u;
        if (new_w == 0 || new_h == 0) {
            return plugins::kErrInvalidParam;
        }
        cfg.width  = new_w;
        cfg.height = new_h;

        if (new_w != width_ || new_h != height_) {
            // ---- A geometry change is a parameter-set change (B13) -------
            //
            // The peer's decoder is holding parameter sets that describe
            // the *old* geometry.  The new ones have to travel with the
            // new frame, or the peer cannot decode it until it asks for a
            // keyframe (and, on a plain H.264 stream, may not even then).
            //
            // OpenH264 emits the SPS / PPS with every IDR, and a
            // freshly initialised encoder starts with an IDR — so
            // re-creating the encoder is exactly the mechanism that
            // puts the new parameter set on the wire with the next
            // frame.
            //
            // The *decoder* needs no re-init: it takes its geometry from
            // the bitstream's own parameter sets, so it must survive.
            close_encoder();
            width_  = new_w;
            height_ = new_h;
            cfg_    = cfg;
            const plugins::Status s = open_encoder();
            if (s != plugins::kOk) {
                stats_.encode_errors++;
                return s;
            }
            core::log::Logger::instance().info(
                "openh264: re-configured the encoder for " +
                std::to_string(width_) + "x" + std::to_string(height_) +
                " — the new SPS / PPS travel with the next frame");
            return plugins::kOk;
        }

        // The geometry did not change — apply the live knobs only.
        cfg_.bitrate_bps      = cfg.bitrate_bps;
        cfg_.fps              = cfg.fps;
        cfg_.keyframe_interval = cfg.keyframe_interval;
        cfg_.pixel_format     = cfg.pixel_format;
        return update_config(cfg_);
    }

    plugins::VideoEncoderParams params() const noexcept override {
        plugins::VideoEncoderParams p;
        p.width             = width_;
        p.height            = height_;
        p.fps               = cfg_.fps;
        p.bitrate_bps       = cfg_.bitrate_bps;
        p.keyframe_interval = cfg_.keyframe_interval;
        p.pixel_format      = cfg_.pixel_format;
        return p;
    }

    plugins::VideoCodecStats stats() const noexcept override { return stats_; }

    std::uint8_t payload_type() const noexcept override {
        return cfg_.payload_type ? cfg_.payload_type : std::uint8_t{102};
    }

    // ---- Telemetry ---------------------------------------------------------

    /** The backend identity a host sees in `Stats::video_codec_backend`.
     *  "openh264" distinguishes this from the synthetic stub, which
     *  reports "software-stub". */
    std::string_view backend_id() const noexcept override { return "openh264"; }

    bool is_hardware_accelerated() const noexcept override { return false; }

private:
    // ---- Setup -------------------------------------------------------------

    plugins::Status open_encoder() noexcept {
        if (WelsCreateSVCEncoder(&encoder_) != 0 || !encoder_) {
            core::log::Logger::instance().error("openh264: WelsCreateSVCEncoder failed");
            encoder_ = nullptr;
            return plugins::kErrInternal;
        }

        SEncParamExt param;
        memset(&param, 0, sizeof(param));
        if (encoder_->GetDefaultParams(&param) != 0) {
            core::log::Logger::instance().error("openh264: GetDefaultParams failed");
            WelsDestroySVCEncoder(encoder_);
            encoder_ = nullptr;
            return plugins::kErrInternal;
        }

        const int   fps     = cfg_.fps ? static_cast<int>(cfg_.fps) : 30;
        const int   bitrate = cfg_.bitrate_bps ? static_cast<int>(cfg_.bitrate_bps)
                                               : (static_cast<int>(width_) * height_ * 4);

        param.iUsageType      = CAMERA_VIDEO_REAL_TIME;
        param.iPicWidth       = static_cast<int>(width_);
        param.iPicHeight      = static_cast<int>(height_);
        param.fMaxFrameRate   = static_cast<float>(fps);
        param.iTargetBitrate  = bitrate;
        param.iRCMode         = RC_BITRATE_MODE;
        param.iTemporalLayerNum = 1;
        param.iSpatialLayerNum  = 1;
        param.uiIntraPeriod   = cfg_.keyframe_interval ? cfg_.keyframe_interval : 60;
        param.iNumRefFrame    = 1;
        param.eSpsPpsIdStrategy = CONSTANT_ID;
        // Annex B start codes on every NAL — the format NimRTC's H.264
        // packetizer and FU-A depacketizer expect.  (Upstream adds the
        // 4-byte start code unconditionally in `WelsEncodeNal`; the
        // `bPrefixNalAddingCtrl` flag controls the *SVC prefix NAL*,
        // which is incompatible with `bSimulcastAVC` and is therefore
        // kept off.)
        param.bPrefixNalAddingCtrl = false;
        param.bEnableSSEI         = false;
        // Single-layer AVC (not SVC) — NimRTC negotiates plain H.264.
        param.bSimulcastAVC        = true;
        param.bEnableFrameSkip     = false;
        param.bEnableDenoise       = false;
        param.bEnableBackgroundDetection = false;
        param.bEnableAdaptiveQuant = true;
        param.bEnableSceneChangeDetect = true;
        param.iMultipleThreadIdc   = 1;
        param.iEntropyCodingModeFlag = 1;   // CABAC

        SSpatialLayerConfig& layer = param.sSpatialLayers[0];
        memset(&layer, 0, sizeof(layer));
        layer.iVideoWidth   = static_cast<int>(width_);
        layer.iVideoHeight  = static_cast<int>(height_);
        layer.fFrameRate    = static_cast<float>(fps);
        layer.iSpatialBitrate = bitrate;
        // Upstream's SSpatialLayerConfig carries the slice configuration as
        // `sSliceArgument` (an SSliceArgument), not `sSliceCfg`.
        layer.sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;

        if (encoder_->InitializeExt(&param) != 0) {
            core::log::Logger::instance().error("openh264: InitializeExt failed");
            WelsDestroySVCEncoder(encoder_);
            encoder_ = nullptr;
            return plugins::kErrInternal;
        }

        const int video_format = videoFormatI420;
        encoder_->SetOption(ENCODER_OPTION_DATAFORMAT,
                            const_cast<int*>(&video_format));
        return plugins::kOk;
    }

    void close_encoder() noexcept {
        if (!encoder_) return;
        encoder_->Uninitialize();
        WelsDestroySVCEncoder(encoder_);
        encoder_ = nullptr;
    }

    plugins::Status open_decoder() noexcept {
        if (WelsCreateDecoder(&decoder_) != 0 || !decoder_) {
            core::log::Logger::instance().error("openh264: WelsCreateDecoder failed");
            decoder_ = nullptr;
            return plugins::kErrInternal;
        }

        SDecodingParam param;
        memset(&param, 0, sizeof(param));
        param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
        param.eEcActiveIdc                 = ERROR_CON_DISABLE;
        param.bParseOnly                    = false;

        if (decoder_->Initialize(&param) != 0) {
            core::log::Logger::instance().error("openh264: decoder Initialize failed");
            WelsDestroyDecoder(decoder_);
            decoder_ = nullptr;
            return plugins::kErrInternal;
        }
        return plugins::kOk;
    }

    void close_decoder() noexcept {
        if (!decoder_) return;
        decoder_->Uninitialize();
        WelsDestroyDecoder(decoder_);
        decoder_ = nullptr;
    }

    // ---- Helpers -----------------------------------------------------------

    /** Copy a `w`x`h` plane from `src` (stride `src_stride`) to `dst`
     *  (stride `dst_stride`).  Both planes are 8-bit. */
    static void copy_plane(std::uint8_t* dst, std::int32_t dst_stride,
                           const unsigned char* src, int src_stride,
                           std::uint32_t w, std::uint32_t h) noexcept {
        if (!dst || !src || w == 0 || h == 0) return;
        for (std::uint32_t row = 0; row < h; ++row) {
            std::memcpy(dst + static_cast<std::size_t>(row) * dst_stride,
                        src + static_cast<std::size_t>(row) * src_stride,
                        static_cast<std::size_t>(w));
        }
    }

    plugins::VideoCodecConfig cfg_{};
    plugins::VideoCodecStats stats_{};
    std::uint32_t width_  = 0;
    std::uint32_t height_ = 0;
    bool open_ = false;

    ISVCEncoder* encoder_ = nullptr;
    ISVCDecoder* decoder_ = nullptr;
};

// ---------------------------------------------------------------------------
// Backend availability probe.
//
// The vendored OpenH264 is statically linked into NimRTC, so the backend
// is available whenever the translation unit was compiled with
// NIMRTC_PLUGINS_OPENH264_ON.  The probe still has to exist because the
// registry asks every backend whether it can run.
// ---------------------------------------------------------------------------

bool openh264_h264_available() noexcept {
    return true;
}

std::unique_ptr<plugins::IVideoCodec>
make_openh264_h264_codec(plugins::VideoCodecConfig cfg) {
    return std::make_unique<Openh264Codec>(std::move(cfg));
}

#else   // NIMRTC_PLUGINS_OPENH264_ON not defined

bool openh264_h264_available() noexcept { return false; }

std::unique_ptr<plugins::IVideoCodec>
make_openh264_h264_codec(plugins::VideoCodecConfig) { return nullptr; }

#endif

} // namespace nimrtc::h264
