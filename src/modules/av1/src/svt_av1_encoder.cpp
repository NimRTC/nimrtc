/**
 * @file src/modules/av1/src/svt_av1_encoder.cpp
 * @brief SVT-AV1 software AV1 encoder — the optional B11 backend.
 *
 * ## What this is
 *
 * The remote-desktop plan's B11 asks for an *optional* software AV1
 * encoder: `svt-av1` (BSD-2-Clause Clear + AOM Patent License 1.0),
 * vendored behind `NIMRTC_VENDORED_SVT_AV1`, which **defaults OFF**.
 *
 * The plan's reason for choosing SVT-AV1 over libaom is its
 * **screen-content mode** (`screen_content_mode`, intra block copy) —
 * that is precisely the feature a remote control needs, and it is the
 * only cross-platform software AV1 encoder that carries it.  This
 * backend therefore turns the screen-content tools on by default and
 * exposes the plan's content-hint on the encoder configuration.
 *
 * ## Why this is the *encoder* half only
 *
 * SVT-AV1 is encode-only.  The decode half of the AV1 path is served by
 * dav1d (`src/modules/av1/src/dav1d_decoder.cpp`), which is vendored
 * separately.  The AV1 registry composes the two through
 * `nvcodec::HwCompositeCodec` when they differ.
 *
 * ## The bitstream framing
 *
 * SVT-AV1 emits the AV1 spec's **low-overhead** OBU stream: OBUs
 * concatenated, each with `obu_has_size_field = 1` (a LEB128 length
 * after the header).  NimRTC's AV1 RTP payload format carries AV1 in
 * **Annex-B** framing (`00 00 00 01` before each OBU — see
 * `video_payload::av1::parse_annex_b_obus`), which is what the NVENC
 * AV1 path produces via `outputAnnexBFormat = 1`.
 *
 * So the conversion happens here, on the encode path: the low-overhead
 * OBUs are rewritten into Annex-B before the bitstream leaves this file.
 * The conversion is pure byte manipulation (the same shape as the
 * VideoToolbox AV1 path's `av1_low_overhead_to_annex_b()`).
 *
 * ## BUILD
 *
 * The implementation is compiled only when `NIMRTC_PLUGINS_SVT_AV1_ON`
 * is defined, which cmake/NimRTHwPlugins.cmake sets when
 * `NIMRTC_VENDORED_SVT_AV1=ON` AND the vendored source tree is present
 * under `src/third_party/svt-av1/src`.  Without it the file compiles to
 * the "unavailable" stub and the registry never registers the row —
 * the plug-in is *absent*, not "present but broken" (the plan's B11
 * acceptance: "with the option OFF the build still configures and the
 * plug-in is absent").
 */

#include <nimrtc/av1/hw_backends.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/plugins/video_codec.hpp>

#if defined(NIMRTC_PLUGINS_SVT_AV1_ON)
  #include "EbSvtAv1.h"
  #include "EbSvtAv1Enc.h"
#endif

namespace nimrtc::av1 {
namespace svt_backend {

#if defined(NIMRTC_PLUGINS_SVT_AV1_ON)

// ---------------------------------------------------------------------------
// OBU framing helpers
// ---------------------------------------------------------------------------

/** The size of an OBU header (before the `obu_size` field).
 *
 *  One byte, plus one more when `obu_extension_flag` is set (AV1 spec
 *  §5.3.1). */
inline std::size_t obu_header_size(std::uint8_t header_byte) noexcept {
    return (header_byte & 0x04u) ? 2u : 1u;
}

/** Read a LEB128 value.  @see video_payload::av1::read_leb128 */
inline bool read_leb128(const std::uint8_t* data,
                        std::size_t size,
                        std::size_t& pos,
                        std::size_t& value) noexcept {
    value = 0;
    for (int i = 0; i < 8; ++i) {
        if (pos >= size) return false;
        const std::uint8_t byte = data[pos++];
        value |= static_cast<std::size_t>(byte & 0x7Fu) << (i * 7);
        if ((byte & 0x80u) == 0) return true;
    }
    return false;
}

/** Write a LEB128 encoding of @p value into @p out (≥ 10 bytes).
 *  @return the number of bytes written. */
inline std::size_t write_leb128(std::size_t value, std::uint8_t* out) noexcept {
    std::size_t n = 0;
    while (value >= 0x80u) {
        out[n++] = static_cast<std::uint8_t>(value) | 0x80u;
        value >>= 7;
    }
    out[n++] = static_cast<std::uint8_t>(value);
    return n;
}

/** Convert SVT-AV1's low-overhead OBU stream into Annex-B.
 *
 *  Each OBU's `obu_size` field is replaced by a `00 00 00 01` start
 *  code and the `obu_has_size_field` bit is cleared (the AV1 spec
 *  requires that bit to be 0 in Annex-B).
 *
 *  A stream that is *already* Annex-B is passed through unchanged.
 *
 *  @return true on success; false when the stream is not a well-formed
 *          low-overhead OBU stream. */
inline bool low_overhead_to_annex_b(const std::uint8_t* data,
                                    std::size_t size,
                                    std::vector<std::uint8_t>& out) noexcept {
    out.clear();
    if (!data || size == 0) return false;

    // Already Annex-B?
    if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        out.assign(data, data + size);
        return true;
    }

    constexpr std::uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};

    std::size_t pos = 0;
    while (pos < size) {
        const std::uint8_t header = data[pos];
        const std::size_t header_size = obu_header_size(header);
        if (pos + header_size > size) return false;

        // The size field must be present — without it the OBU's length
        // is not knowable, so the stream cannot be split.
        if ((header & 0x02u) == 0) return false;

        std::uint8_t extension = 0;
        if (header_size == 2) extension = data[pos + 1];

        std::size_t payload_size = 0;
        std::size_t size_pos = pos + header_size;
        if (!read_leb128(data, size, size_pos, payload_size)) return false;

        const std::size_t payload_pos = size_pos;
        if (payload_pos + payload_size > size) return false;

        out.insert(out.end(), kStartCode, kStartCode + 4);
        out.push_back(static_cast<std::uint8_t>(header & ~0x02u));
        if (header_size == 2) out.push_back(extension);
        out.insert(out.end(), data + payload_pos, data + payload_pos + payload_size);
        pos = payload_pos + payload_size;
    }
    return !out.empty();
}

// ---------------------------------------------------------------------------
// SvtAv1Codec — the real software AV1 encoder.
// ---------------------------------------------------------------------------
class SvtAv1Codec final : public plugins::IVideoCodec {
public:
    explicit SvtAv1Codec(plugins::VideoCodecConfig cfg) noexcept
        : cfg_(std::move(cfg)) {}

    ~SvtAv1Codec() override { close(); }

    SvtAv1Codec(const SvtAv1Codec&)            = delete;
    SvtAv1Codec& operator=(const SvtAv1Codec&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override {
        return "nimrtc::av1::SvtAv1Codec (vendored SVT-AV1, CPU)";
    }

    plugins::Status open() noexcept override {
        if (open_) return plugins::kOk;

        const std::uint32_t w = cfg_.width  ? cfg_.width  : 640;
        const std::uint32_t h = cfg_.height ? cfg_.height : 480;
        // 4:2:0 chroma needs even dimensions.
        width_  = w & ~1u;
        height_ = h & ~1u;
        if (width_ == 0 || height_ == 0) {
            core::log::Logger::instance().error(
                "svt-av1: refusing to open with an odd or zero frame size");
            return plugins::kErrInvalidParam;
        }

        EbComponentType* handle = nullptr;
        EbSvtAv1EncConfiguration config;
        memset(&config, 0, sizeof(config));

        // STEP 1: construct the handle.  This fills `config` with the
        // upstream defaults, which the code below then overrides.
        if (svt_av1_enc_init_handle(&handle, &config) != EB_ErrorNone || !handle) {
            core::log::Logger::instance().error(
                "svt-av1: svt_av1_enc_init_handle failed");
            return plugins::kErrInternal;
        }
        handle_ = handle;

        const int fps = cfg_.fps ? static_cast<int>(cfg_.fps) : 30;

        // STEP 2: configure.
        config.source_width           = width_;
        config.source_height          = height_;
        config.frame_rate_numerator   = static_cast<std::uint32_t>(fps);
        config.frame_rate_denominator = 1;
        config.intra_period_length   = cfg_.keyframe_interval
                                          ? static_cast<std::int32_t>(cfg_.keyframe_interval)
                                          : -2;   // auto
        // 2 = IDR, closed GOP.  (1 would be a CRA — an open GOP — whose
        // leading picture is not a random-access point the peer can start
        // decoding from without a key-frame request.)
        config.intra_refresh_type     = SVT_AV1_KF_REFRESH;

        // LOW DELAY prediction structure.
        //
        // This is what makes the codec seam's "one access unit per
        // encode() call" contract hold: in the low-delay structure the
        // encoder emits exactly one packet per submitted picture, and
        // `svt_av1_enc_get_packet()` blocks until it is there.  The
        // random-access structure would instead hold the picture back
        // for the look-ahead, and the seam would have to buffer several
        // pictures before it could hand one access unit back.
        config.pred_structure       = LOW_DELAY;
        config.hierarchical_levels  = 0;

        // Preset 8 is upstream's "good quality, real-time" default for
        // screen content; higher presets are faster but worse.
        config.enc_mode = 8;

        // Rate control: CBR at the requested bitrate, which is what the
        // engine's congestion control expects from a video encoder.
        config.rate_control_mode = SVT_AV1_RC_MODE_CBR;
        const std::uint32_t bitrate =
            cfg_.bitrate_bps ? cfg_.bitrate_bps
                             : (width_ * height_ * 4);
        config.target_bit_rate = bitrate;

        // SCREEN CONTENT MODE — the reason the plan names SVT-AV1.
        //
        // `screen_content_mode = 1` tells the encoder that the content is
        // screen content, which switches on the screen-content coding
        // tools (palette mode, intra block copy).  `enable_intrabc` turns
        // on intra block copy itself, which is the tool that makes a
        // mostly-static desktop cheap.  Both are on by default here
        // because the whole point of vendoring SVT-AV1 (over libaom) is
        // these two.
        config.screen_content_mode = 1;
        config.enable_intrabc      = true;

        if (svt_av1_enc_set_parameter(handle_, &config) != EB_ErrorNone) {
            core::log::Logger::instance().error(
                "svt-av1: svt_av1_enc_set_parameter failed");
            close();
            return plugins::kErrInternal;
        }

        // STEP 3: initialize (this allocates the encoder's buffers).
        if (svt_av1_enc_init(handle_) != EB_ErrorNone) {
            core::log::Logger::instance().error("svt-av1: svt_av1_enc_init failed");
            close();
            return plugins::kErrInternal;
        }
        initialized_ = true;

        // OPTIONAL: take the stream header (the sequence header OBU) out
        // of the encoder.  It is emitted once, before the first frame,
        // and NimRTC's Annex-B framing needs it in front of the frames
        // — so it is stored and prepended to the first encoded access
        // unit.
        EbBufferHeaderType* header = nullptr;
        if (svt_av1_enc_stream_header(handle_, &header) == EB_ErrorNone && header) {
            stream_header_.assign(header->p_buffer,
                                  header->p_buffer + header->n_filled_len);
            svt_av1_enc_stream_header_release(header);
        }

        open_ = true;
        core::log::Logger::instance().info(
            "svt-av1: opened " + std::to_string(width_) + "x" +
            std::to_string(height_) + " software AV1 encoder (screen-content "
            "mode on, intra block copy on)");
        return plugins::kOk;
    }

    void close() noexcept override {
        if (handle_) {
            if (initialized_) {
                svt_av1_enc_deinit(handle_);
                initialized_ = false;
            }
            svt_av1_enc_deinit_handle(handle_);
            handle_ = nullptr;
        }
        open_ = false;
        stream_header_.clear();
        header_emitted_ = false;
        force_keyframe_next_ = true;
    }

    // ---- plugins::IVideoCodec ----------------------------------------------

    // SVT-AV1 is encode-only.  `can_decode()` answers the *capability*
    // question, not "is the session open yet".
    bool can_encode() const noexcept override { return true; }
    bool can_decode() const noexcept override { return false; }

    plugins::VideoCodecKind kind() const noexcept override {
        return plugins::VideoCodecKind::kAV1;
    }
    std::string_view codec_name() const noexcept override { return "av1"; }

    plugins::Status encode(const plugins::VideoFrame& raw,
                           std::uint8_t* output, std::size_t output_capacity,
                           plugins::EncodedVideoFrame& encoded_out) noexcept override {
        if (!handle_ || !initialized_) {
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

        // ---- Submit the picture --------------------------------------------
        //
        // Upstream's contract (see `svt_av1_enc_send_picture()` in
        // `Source/Lib/Globals/enc_handle.c`):
        //
        //   - `p_buffer` must point at an `EbSvtIOFormat` that
        //     describes the *planes* of the picture.  Upstream reads
        //     it back in `copy_input_buffer()` and copies the planes
        //     into its own pool — so the frame's own planes are used
        //     directly and no second staging copy is made here.
        //   - `n_filled_len` must be at least the number of bytes the
        //     picture holds.  Upstream compares it against the
        //     expected read size and *zeroes the picture* when it is
        //     too small (a defensive path that would produce a black
        //     frame), so it has to be set correctly here.
        //   - `p_app_private` carries upstream's private-data node
        //     list (`EbPrivDataNode`), NOT the picture description.
        //     It must be null when no event is attached.
        const std::uint32_t cw = (width_ + 1u) / 2u;
        const std::uint32_t ch = (height_ + 1u) / 2u;
        const std::uint32_t read_size =
            width_ * height_ + 2u * cw * ch;

        EbSvtIOFormat picture;
        memset(&picture, 0, sizeof(picture));
        picture.luma      = const_cast<std::uint8_t*>(y_plane);
        picture.cb        = const_cast<std::uint8_t*>(u_plane);
        picture.cr        = const_cast<std::uint8_t*>(v_plane);
        picture.y_stride  = static_cast<std::uint32_t>(layout.strides[0]);
        picture.cb_stride = static_cast<std::uint32_t>(layout.strides[1]);
        picture.cr_stride = static_cast<std::uint32_t>(layout.strides[2]);

        EbBufferHeaderType pic;
        memset(&pic, 0, sizeof(pic));
        pic.size          = sizeof(pic);
        pic.p_buffer      = reinterpret_cast<std::uint8_t*>(&picture);
        pic.n_filled_len  = read_size;
        pic.n_alloc_len   = read_size;
        pic.p_app_private = nullptr;
        pic.pts           = static_cast<std::int64_t>(raw.info.capture_ts_us);
        pic.dts           = pic.pts;
        pic.pic_type      = force_keyframe_next_ ? EB_AV1_KEY_PICTURE
                                                 : EB_AV1_INVALID_PICTURE;

        const EbErrorType send_err = svt_av1_enc_send_picture(handle_, &pic);
        if (send_err == EB_NoErrorFrameSkipped) {
            // The encoder accepted the picture but deliberately skipped
            // it (the rate controller's frame-skip).  That is a skip,
            // not an error.
            stats_.frames_skipped++;
            return plugins::kErrUnsupported;
        }
        if (send_err != EB_ErrorNone) {
            stats_.encode_errors++;
            return plugins::kErrInternal;
        }
        if (force_keyframe_next_) force_keyframe_next_ = false;

        // ---- Collect the output --------------------------------------------
        //
        // In the LOW_DELAY prediction structure the encoder emits exactly
        // one packet per submitted picture, and `svt_av1_enc_get_packet()`
        // *blocks* until that packet is there (upstream documents this:
        // "this function will become blocking ... if we are in low-delay
        // (pred-struct=1)").  So a single call is the whole story; the
        // loop below only tolerates the (defensive) case where the queue
        // is momentarily empty, which upstream reports as
        // `EB_NoErrorEmptyQueue`.
        std::vector<std::uint8_t> payload;
        EbBufferHeaderType* packet = nullptr;
        bool got_packet = false;
        bool is_keyframe = false;

        while (true) {
            const EbErrorType err =
                svt_av1_enc_get_packet(handle_, &packet, /*pic_send_done=*/0);
            if (err == EB_NoErrorEmptyQueue) break;
            if (err != EB_ErrorNone || !packet) {
                stats_.encode_errors++;
                return plugins::kErrInternal;
            }
            got_packet = true;
            if (packet->n_filled_len > 0 && packet->p_buffer) {
                payload.insert(payload.end(), packet->p_buffer,
                               packet->p_buffer + packet->n_filled_len);
            }
            // The key-frame flag is upstream's own answer: it stamps the
            // picture type on the packet it hands back.
            if (packet->pic_type == EB_AV1_KEY_PICTURE ||
                packet->pic_type == EB_AV1_INTRA_ONLY_PICTURE) {
                is_keyframe = true;
            }
            svt_av1_enc_release_out_buffer(&packet);
            packet = nullptr;
            break;   // one access unit per encode() call
        }

        if (!got_packet) {
            // The encoder accepted the picture but produced nothing (it
            // may have skipped it).  That is a skip, not an error.
            stats_.frames_skipped++;
            return plugins::kErrUnsupported;
        }

        // ---- Annex-B conversion --------------------------------------------
        //
        // SVT-AV1 emits low-overhead OBUs; NimRTC carries AV1 in
        // Annex-B.  The conversion happens here so the rest of the tree
        // sees one framing.
        std::vector<std::uint8_t> annex_b;
        if (!low_overhead_to_annex_b(payload.data(), payload.size(), annex_b)) {
            stats_.encode_errors++;
            return plugins::kErrCorrupt;
        }

        // The sequence header OBU has to travel with the first access
        // unit (it is what tells the peer the geometry and the profile).
        if (!header_emitted_ && !stream_header_.empty()) {
            std::vector<std::uint8_t> header_annex_b;
            if (low_overhead_to_annex_b(stream_header_.data(),
                                        stream_header_.size(),
                                        header_annex_b)) {
                annex_b.insert(annex_b.begin(), header_annex_b.begin(),
                               header_annex_b.end());
                header_emitted_ = true;
                is_keyframe = true;
            }
        }

        if (annex_b.empty() || annex_b.size() > output_capacity) {
            stats_.encode_errors++;
            return plugins::kErrBufferTooSmall;
        }
        std::memcpy(output, annex_b.data(), annex_b.size());

        // ---- Fill the encoded frame descriptor -----------------------------
        encoded_out.codec        = plugins::VideoCodecKind::kAV1;
        encoded_out.payload      = core::ByteSpan{output, annex_b.size()};
        encoded_out.payload_type = cfg_.payload_type ? cfg_.payload_type
                                                     : std::uint8_t{45};
        encoded_out.is_keyframe  = is_keyframe;
        encoded_out.nalu_format  = plugins::NaluFormat::kAnnexBStartCode;
        encoded_out.info.capture_ts_us = raw.info.capture_ts_us;
        encoded_out.info.frame_seq     = raw.info.frame_seq;
        encoded_out.info.rtp_timestamp = raw.info.rtp_timestamp;

        stats_.frames_encoded++;
        stats_.bytes_encoded += annex_b.size();
        // SVT-AV1 consumes the CPU staging path — it has no zero-copy
        // route — so every frame it encodes is a staged one.
        stats_.frames_encoded_staged++;
        if (is_keyframe) stats_.keyframes_encoded++;
        return plugins::kOk;
    }

    plugins::Status decode(const plugins::EncodedVideoFrame&,
                           plugins::VideoFrame&,
                           std::uint8_t* const*) noexcept override {
        // SVT-AV1 is an encoder.  The AV1 registry pairs it with dav1d
        // (or another decoder) through `nvcodec::HwCompositeCodec`, so
        // reaching this method means the composition went wrong.
        stats_.decode_errors++;
        return plugins::kErrUnsupported;
    }

    plugins::Status force_keyframe() noexcept override {
        if (!handle_ || !initialized_) return plugins::kErrNotReady;
        // SVT-AV1 has no explicit "force IDR" API, but the picture type
        // is a property of the *submitted* picture: marking the next one
        // as `EB_AV1_KEY_PICTURE` makes the encoder code it as a key
        // picture.  That is the honest implementation of the request —
        // the next encoded access unit is a key frame.
        force_keyframe_next_ = true;
        return plugins::kOk;
    }

    plugins::VideoCodecConfig config() const noexcept override { return cfg_; }

    plugins::Status update_config(plugins::VideoCodecConfig cfg) noexcept override {
        cfg_ = std::move(cfg);
        return plugins::kOk;
    }

    plugins::VideoCodecStats stats() const noexcept override { return stats_; }

    std::uint8_t payload_type() const noexcept override {
        // AV1 has no statically-assigned payload type in RFC 3551; 45 is
        // the value the AV1 RTP payload specification uses.
        return cfg_.payload_type ? cfg_.payload_type : std::uint8_t{45};
    }

    // ---- Telemetry ---------------------------------------------------------

    /** The backend identity a host sees in `Stats::video_codec_backend`.
     *  "svt-av1" distinguishes this from the synthetic stub
     *  ("software-stub") and from the dav1d decoder ("dav1d"). */
    std::string_view backend_id() const noexcept override { return "svt-av1"; }

    bool is_hardware_accelerated() const noexcept override { return false; }

private:
    plugins::VideoCodecConfig cfg_{};
    plugins::VideoCodecStats stats_{};
    std::uint32_t width_  = 0;
    std::uint32_t height_ = 0;
    bool open_ = false;

    EbComponentType*    handle_      = nullptr;
    bool                initialized_ = false;
    std::vector<std::uint8_t> stream_header_;
    bool                header_emitted_ = false;
    /** True until the next submitted picture is marked as a key picture.
     *
     *  SVT-AV1 has no "force IDR now" API: the picture type is a
     *  *property of the submitted picture* (`EbBufferHeaderType::pic_type`),
     *  so the request is honoured by marking the next picture as
     *  `EB_AV1_KEY_PICTURE`. */
    bool                force_keyframe_next_ = true;
};

#endif  // NIMRTC_PLUGINS_SVT_AV1_ON

// ---------------------------------------------------------------------------
// Backend availability probe.
//
// The vendored SVT-AV1 is statically linked into NimRTC, so the backend is
// available whenever the translation unit was compiled with
// NIMRTC_PLUGINS_SVT_AV1_ON.  The probe still has to exist because the
// registry asks every backend whether it can run.
// ---------------------------------------------------------------------------

bool svt_av1_available() noexcept {
#if defined(NIMRTC_PLUGINS_SVT_AV1_ON)
    return true;
#else
    return false;
#endif
}

/** Why the SVT-AV1 encoder is unavailable.
 *
 *  The plan's B11 acceptance is "with the option OFF the build still
 *  configures and the plug-in is absent" — so when the option is off
 *  the row is not registered at all.  This reason is still here for the
 *  case where the option is ON but the vendored tree is missing. */
std::string svt_av1_unavailable_reason() {
#if defined(NIMRTC_PLUGINS_SVT_AV1_ON)
    return "svt-av1: the encoder is compiled in, so this reason should never "
           "be reported — please file a bug";
#else
    return "svt-av1: the software AV1 encoder is not built into this "
           "binary (NIMRTC_VENDORED_SVT_AV1 is off, or the vendored "
           "svt-av1 source tree is missing), so the software AV1 encoder is "
           "unavailable";
#endif
}

/** Create the SVT-AV1 encoder.
 *
 *  Returns nullptr when the encoder is not compiled in — which is what
 *  makes the registry fall through to the next backend (and eventually
 *  to the software stub). */
std::unique_ptr<plugins::IVideoCodec>
make_svt_av1_codec(plugins::VideoCodecConfig cfg) {
#if defined(NIMRTC_PLUGINS_SVT_AV1_ON)
    return std::make_unique<SvtAv1Codec>(std::move(cfg));
#else
    (void)cfg;
    return nullptr;
#endif
}

} // namespace svt_backend

bool svt_av1_available() noexcept {
    return svt_backend::svt_av1_available();
}

std::string svt_av1_unavailable_reason() {
    return svt_backend::svt_av1_unavailable_reason();
}

std::unique_ptr<plugins::IVideoCodec>
make_svt_av1_codec(plugins::VideoCodecConfig cfg) {
    return svt_backend::make_svt_av1_codec(std::move(cfg));
}

} // namespace nimrtc::av1
