/**
 * @file src/modules/vp9/src/libvpx_vp9_codec.cpp
 * @brief libvpx software VP9 encoder — the optional B11 backend.
 *
 * ## What this is
 *
 * The remote-desktop plan's B11 asks for an *optional* software VP9
 * encoder: `libvpx` (BSD-3-Clause), vendored behind
 * `NIMRTC_VENDORED_LIBVPX`, which **defaults OFF**.
 *
 * libvpx is the reference VP9 implementation, and it is the only
 * permissively-licensed VP9 encoder that clears the plan's RD-2 licence
 * filter (the alternative, x265-style proprietary stacks, are not
 * redistributable).
 *
 * ## The seam contract
 *
 * The entry points exist on *every* platform, with the right
 * signatures.  The backend body is compiled only when
 * `NIMRTC_PLUGINS_LIBVPX_ON` is defined — which
 * `cmake/NimRTHwPlugins.cmake` sets when `NIMRTC_VENDORED_LIBVPX=ON`
 * AND the vendored source tree is present AND the build host carries a
 * `perl` interpreter (libvpx's `rtcd.pl` code generators need one; see
 * the R-11 note in `src/third_party/libvpx/CMakeLists.txt`).
 *
 * With the option OFF the row is *absent* from the backend registry —
 * the plan's B11 acceptance ("with the option OFF the build still
 * configures and the plug-in is absent"), not "the plug-in is present
 * but broken".
 *
 * ## The libvpx API shape
 *
 * libvpx is a *two-call* encoder: the caller hands it an input image
 * (`vpx_codec_encode`) and then drains the compressed packets
 * (`vpx_codec_get_cx_data`) until the "no more packets" sentinel.
 * The VP9 *frame* framing is what lands on the wire — one access unit
 * per encoded picture.
 *
 * The input image is a `vpx_image_t` whose planes are filled from the
 * source frame's I420 planes.  libvpx wants the planes in its own
 * stride layout, so the copy is explicit here (one staging copy, and
 * the codec statistics count it honestly as a staged frame).
 */

#include <nimrtc/vp9/hw_backends.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/plugins/video_codec.hpp>

#if defined(NIMRTC_PLUGINS_LIBVPX_ON)
  #include "vpx/vpx_encoder.h"
  #include "vpx/vp8cx.h"
#endif

namespace nimrtc::vp9 {
namespace libvpx_backend {

#if defined(NIMRTC_PLUGINS_LIBVPX_ON)

// ---------------------------------------------------------------------------
// LibvpxVp9Codec — the real software VP9 encoder.
// ---------------------------------------------------------------------------
class LibvpxVp9Codec final : public plugins::IVideoCodec {
public:
    explicit LibvpxVp9Codec(plugins::VideoCodecConfig cfg) noexcept
        : cfg_(std::move(cfg)) {}

    ~LibvpxVp9Codec() override { close(); }

    LibvpxVp9Codec(const LibvpxVp9Codec&)            = delete;
    LibvpxVp9Codec& operator=(const LibvpxVp9Codec&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override {
        return "nimrtc::vp9::LibvpxVp9Codec (vendored libvpx, CPU)";
    }

    plugins::Status open() noexcept override {
        if (open_) return plugins::kOk;

        const std::uint32_t w = cfg_.width  ? cfg_.width  : 640;
        const std::uint32_t h = cfg_.height ? cfg_.height : 480;
        width_  = w & ~1u;
        height_ = h & ~1u;
        if (width_ == 0 || height_ == 0) {
            core::log::Logger::instance().error(
                "libvpx: refusing to open with an odd or zero frame size");
            return plugins::kErrInvalidParam;
        }

        vpx_codec_ctx_t ctx;
        vpx_codec_err_t err = vpx_codec_enc_init(&ctx, vpx_codec_vp9_cx(), nullptr, 0);
        if (err != VPX_CODEC_OK) {
            core::log::Logger::instance().error(
                "libvpx: vpx_codec_enc_init failed: " +
                std::string(vpx_codec_error_detail(&ctx)));
            return plugins::kErrInternal;
        }
        codec_ = ctx;

        // The configuration is applied *after* the init and *before* the
        // first `vpx_codec_enc_config_default()` pass — libvpx's contract
        // is: init -> set defaults -> apply the overrides -> configure.
        vpx_codec_enc_cfg_t cfg;
        err = vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &cfg, 0);
        if (err != VPX_CODEC_OK) {
            core::log::Logger::instance().error(
                "libvpx: vpx_codec_enc_config_default failed");
            close();
            return plugins::kErrInternal;
        }

        cfg.g_w = width_;
        cfg.g_h = height_;
        cfg.g_timebase.num = 1;
        cfg.g_timebase.den = cfg_.fps ? static_cast<int>(cfg_.fps) : 30;
        cfg.g_lag_in_frames = 0;   // no lookahead: one frame in, one out
        cfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;
        cfg.g_threads = 1;
        cfg.rc_end_usage = VPX_VBR;
        cfg.rc_target_bitrate = cfg_.bitrate_bps ? cfg_.bitrate_bps
                                                 : (width_ * height_ * 4 / 5);
        cfg.kf_max_dist = cfg_.keyframe_interval
                              ? static_cast<unsigned int>(cfg_.keyframe_interval)
                              : 60;
        // Screen content: libvpx's "tune_content = screen" switches on
        // the screen-content coding tools (palette mode, intra block
        // copy) — the same feature the plan names SVT-AV1 for.
        cfg.tune_content = VPX_CONTENT_SCREEN;
        cfg.g_lag_in_frames = 0;

        err = vpx_codec_enc_init(&codec_, vpx_codec_vp9_cx(), &cfg, 0);
        if (err != VPX_CODEC_OK) {
            core::log::Logger::instance().error(
                "libvpx: vpx_codec_enc_init failed: " +
                std::string(vpx_codec_error_detail(&codec_)));
            close();
            return plugins::kErrInternal;
        }

        // The input image.
        if (!vpx_img_alloc(&image_, VPX_IMG_FMT_I420, width_, height_, 1)) {
            core::log::Logger::instance().error("libvpx: vpx_img_alloc failed");
            close();
            return plugins::kErrInternal;
        }
        image_allocated_ = true;

        open_ = true;
        force_keyframe_next_ = true;
        core::log::Logger::instance().info(
            "libvpx: opened " + std::to_string(width_) + "x" +
            std::to_string(height_) + " software VP9 encoder (screen-content "
            "tuning on)");
        return plugins::kOk;
    }

    void close() noexcept override {
        if (image_allocated_) {
            vpx_img_free(&image_);
            image_allocated_ = false;
        }
        if (codec_) {
            vpx_codec_destroy(&codec_);
            codec_ = nullptr;
        }
        open_ = false;
        force_keyframe_next_ = true;
    }

    // ---- plugins::IVideoCodec ----------------------------------------------

    bool can_encode() const noexcept override { return true; }
    bool can_decode() const noexcept override { return false; }

    plugins::VideoCodecKind kind() const noexcept override {
        return plugins::VideoCodecKind::kVP9;
    }
    std::string_view codec_name() const noexcept override { return "vp9"; }

    plugins::Status encode(const plugins::VideoFrame& raw,
                           std::uint8_t* output, std::size_t output_capacity,
                           plugins::EncodedVideoFrame& encoded_out) noexcept override {
        if (!open_ || !codec_) {
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
            stats_.encode_errors++;
            return plugins::kErrInvalidParam;
        }

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

        // ---- Stage the source picture into the libvpx image ---------------
        //
        // libvpx's planes have their own strides, so the copy is
        // row-by-row rather than a single memcpy.
        {
            unsigned char* dst_y = image_.planes[VPX_PLANE_Y];
            unsigned char* dst_u = image_.planes[VPX_PLANE_U];
            unsigned char* dst_v = image_.planes[VPX_PLANE_V];
            const std::uint32_t cw = (width_ + 1u) / 2u;
            const std::uint32_t ch = (height_ + 1u) / 2u;
            for (std::uint32_t row = 0; row < height_; ++row) {
                std::memcpy(dst_y + static_cast<std::size_t>(row) * image_.stride[VPX_PLANE_Y],
                            y_plane + static_cast<std::size_t>(row) * layout.strides[0],
                            width_);
            }
            for (std::uint32_t row = 0; row < ch; ++row) {
                std::memcpy(dst_u + static_cast<std::size_t>(row) * image_.stride[VPX_PLANE_U],
                            u_plane + static_cast<std::size_t>(row) * layout.strides[1],
                            cw);
                std::memcpy(dst_v + static_cast<std::size_t>(row) * image_.stride[VPX_PLANE_V],
                            v_plane + static_cast<std::size_t>(row) * layout.strides[2],
                            cw);
            }
        }

        // ---- Submit the picture --------------------------------------------
        const vpx_codec_err_t err = vpx_codec_encode(&codec_, &image_,
                                                      force_keyframe_next_ ? VPX_EFLAG_FORCE_KF : 0,
                                                      /*deadline=*/0,
                                                      VPX_DL_GOOD_QUALITY);
        if (err != VPX_CODEC_OK) {
            core::log::Logger::instance().error(
                "libvpx: vpx_codec_encode failed: " +
                std::string(vpx_codec_error(&codec_)));
            stats_.encode_errors++;
            return plugins::kErrInternal;
        }
        if (force_keyframe_next_) force_keyframe_next_ = false;

        // ---- Collect the output --------------------------------------------
        //
        // libvpx hands the compressed data back in *chunks*; the whole
        // frame is the concatenation of the chunks that carry the same
        // "pts" until the `VPX_CODEC_CX_FRAME_PKT` sentinel.
        vpx_codec_iter_t iter = nullptr;
        const vpx_codec_err_t iter_err = vpx_codec_get_cx_data(&codec_, &iter);
        if (iter_err != VPX_CODEC_OK || !iter) {
            stats_.encode_errors++;
            return plugins::kErrInternal;
        }

        std::vector<std::uint8_t> payload;
        bool is_keyframe = false;
        while (iter) {
            const vpx_codec_iter_t next = vpx_codec_iter_next(iter);
            if (iter->kind == VPX_CODEC_CX_FRAME_PKT) {
                if (iter->data.frame.sz > 0 && iter->data.frame.buf) {
                    const std::uint8_t* p =
                        static_cast<const std::uint8_t*>(iter->data.frame.buf);
                    payload.insert(payload.end(), p, p + iter->data.frame.sz);
                }
                if (iter->data.frame.flags & VPX_FRAME_IS_KEY) {
                    is_keyframe = true;
                }
            }
            iter = next;
        }

        if (payload.empty()) {
            // libvpx produced nothing for this picture (it can drop a
            // frame under rate control).  That is a skip, not an error.
            stats_.frames_skipped++;
            return plugins::kErrUnsupported;
        }

        if (payload.size() > output_capacity) {
            stats_.encode_errors++;
            return plugins::kErrBufferTooSmall;
        }
        std::memcpy(output, payload.data(), payload.size());

        encoded_out.codec        = plugins::VideoCodecKind::kVP9;
        encoded_out.payload      = core::ByteSpan{output, payload.size()};
        encoded_out.payload_type = payload_type();
        encoded_out.is_keyframe  = is_keyframe;
        // VP9 has no start-code framing: one frame per access unit.
        encoded_out.nalu_format  = plugins::NaluFormat::kUnknown;
        encoded_out.info.capture_ts_us = raw.info.capture_ts_us;
        encoded_out.info.frame_seq     = raw.info.frame_seq;
        encoded_out.info.rtp_timestamp = raw.info.rtp_timestamp;

        stats_.frames_encoded++;
        stats_.bytes_encoded += payload.size();
        // libvpx consumes the CPU staging path — it has no zero-copy
        // route — so every frame it encodes is a staged one.
        stats_.frames_encoded_staged++;
        if (is_keyframe) stats_.keyframes_encoded++;
        return plugins::kOk;
    }

    plugins::Status decode(const plugins::EncodedVideoFrame&,
                           plugins::VideoFrame&,
                           std::uint8_t* const*) noexcept override {
        // This backend is the encoder half.  The VP9 registry pairs it
        // with a decoder through `nvcodec::HwCompositeCodec` when one
        // exists; reaching this method means the composition went wrong.
        stats_.decode_errors++;
        return plugins::kErrUnsupported;
    }

    plugins::Status force_keyframe() noexcept override {
        if (!open_) return plugins::kErrNotReady;
        // libvpx's "force a key frame" request is a *frame property*, not
        // a flag on the encode call, so the request is honoured by
        // marking the next submitted frame.
        force_keyframe_next_ = true;
        return plugins::kOk;
    }

    plugins::VideoCodecConfig config() const noexcept override { return cfg_; }

    plugins::Status update_config(plugins::VideoCodecConfig cfg) noexcept override {
        cfg_ = std::move(cfg);
        return plugins::kOk;
    }

    plugins::VideoCodecStats stats() const noexcept override { return stats_; }

    std::uint8_t payload_type() noexcept override {
        // VP9 has no statically-assigned payload type in RFC 3551; 98 is
        // the value RFC 9559 recommends.
        return cfg_.payload_type ? cfg_.payload_type : std::uint8_t{98};
    }

    // ---- Telemetry ---------------------------------------------------------

    /** The backend identity a host sees in `Stats::video_codec_backend`.
     *  "libvpx" distinguishes this from the synthetic stub
     *  ("software-stub"). */
    std::string_view backend_id() const noexcept override { return "libvpx"; }

    bool is_hardware_accelerated() const noexcept override { return false; }

private:
    plugins::VideoCodecConfig cfg_{};
    plugins::VideoCodecStats stats_{};
    std::uint32_t width_  = 0;
    std::uint32_t height_ = 0;
    bool open_ = false;

    vpx_codec_ctx_t codec_ = nullptr;
    vpx_image_t     image_{};
    bool            image_allocated_ = false;
    /** True until the next submitted picture is marked as a key frame. */
    bool            force_keyframe_next_ = true;
};

#endif  // NIMRTC_PLUGINS_LIBVPX_ON

// ---------------------------------------------------------------------------
// Backend availability probe.
//
// The vendored libvpx is statically linked into NimRTC, so the backend is
// available whenever the translation unit was compiled with
// NIMRTC_PLUGINS_LIBVPX_ON.  The probe still has to exist because the
// registry asks every backend whether it can run.
// ---------------------------------------------------------------------------

bool libvpx_vp9_available() noexcept {
#if defined(NIMRTC_PLUGINS_LIBVPX_ON)
    return true;
#else
    return false;
#endif
}

/** Why the libvpx VP9 encoder is unavailable.
 *
 *  The plan's B11 acceptance is "with the option OFF the build still
 *  configures and the plug-in is absent" — so when the option is off
 *  the row is not registered at all.  This reason is still here for the
 *  case where the option is ON but the vendored tree is missing. */
std::string libvpx_vp9_unavailable_reason() {
#if defined(NIMRTC_PLUGINS_LIBVPX_ON)
    return "libvpx: the encoder is compiled in, so this reason should never "
           "be reported — please file a bug";
#else
    return "libvpx: the software VP9 encoder is not built into this "
           "binary (NIMRTC_VENDORED_LIBVPX is off, the vendored libvpx "
           "source tree is missing, or the build host has no perl "
           "interpreter for libvpx's code generators), so the software VP9 "
           "encoder is unavailable";
#endif
}

/** Create the libvpx VP9 encoder.
 *
 *  Returns nullptr when the encoder is not compiled in — which is what
 *  makes the registry fall through to the next backend (and eventually
 *  to the software stub). */
std::unique_ptr<plugins::IVideoCodec>
make_libvpx_vp9_codec(plugins::VideoCodecConfig cfg) {
#if defined(NIMRTC_PLUGINS_LIBVPX_ON)
    return std::make_unique<LibvpxVp9Codec>(std::move(cfg));
#else
    (void)cfg;
    return nullptr;
#endif
}

} // namespace libvpx_backend

bool libvpx_vp9_available() noexcept {
    return libvpx_backend::libvpx_vp9_available();
}

std::string libvpx_vp9_unavailable_reason() {
    return libvpx_backend::libvpx_vp9_unavailable_reason();
}

std::unique_ptr<plugins::IVideoCodec>
make_libvpx_vp9_codec(plugins::VideoCodecConfig cfg) {
    return libvpx_backend::make_libvpx_vp9_codec(std::move(cfg));
}

} // namespace nimrtc::vp9
