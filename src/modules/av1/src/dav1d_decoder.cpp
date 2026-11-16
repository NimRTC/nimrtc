/**
 * @file src/modules/av1/src/dav1d_decoder.cpp
 * @brief dav1d software AV1 decoder plugin.
 *
 * A *real* software AV1 decoder: it feeds the incoming OBU stream to
 * dav1d and converts the decoded pictures into I420 planes.  This is the
 * CPU bring-up baseline the remote-desktop plan asks for (WP-B).
 *
 * WHY THIS FILE EXISTS ALONGSIDE THE STUB
 * ---------------------------------------
 * The stub codec (`CodecPluginAdapter` in codec_plugin.cpp) recognises
 * the *shape* of an AV1 OBU stream but reconstructs nothing — it paints a
 * synthetic pattern.  It exists so the RTP / SRTP / ICE / OBU
 * reassembly pipeline can be exercised without a codec.  It is NOT a
 * video codec, and it reports `backend_id() == "software-stub"` so a host
 * can tell the two apart.
 *
 * The dav1d backend reports `backend_id() == "dav1d"`.
 *
 * BUILD
 * -----
 * The implementation is compiled only when `NIMRTC_PLUGINS_DAV1D_ON` is
 * defined, which cmake/NimRTHwPlugins.cmake sets when
 * `NIMRTC_PLUGINS_DAV1D=ON` AND the vendored dav1d source tree is
 * present under `src/third_party/dav1d/src`.  Without it the file
 * compiles to the "unavailable" stub and the registry falls through to
 * the software-stub backend.
 *
 * NOTE ON HONESTY
 * ---------------
 * The H.265 backend remains a stub: there is no vendored HEVC decoder
 * in the tree, and faking one would violate the "no pretending to decode"
 * rule the remote-desktop plan sets.  Likewise, AV1 *encoding* stays a
 * stub — dav1d is a decoder only.
 */

#include <nimrtc/av1/hw_backends.hpp>

#include <cstdint>
#include <cstring>
#include <memory>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/bytes.hpp>
#include <nimrtc/av1/codec_plugin.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/video_payload/av1.hpp>

#include <vector>

#if defined(NIMRTC_PLUGINS_DAV1D_ON)
  #include "dav1d/dav1d.h"
  #include "dav1d/data.h"
  #include "dav1d/picture.h"
#endif

namespace nimrtc::av1 {

#if defined(NIMRTC_PLUGINS_DAV1D_ON)

// ---------------------------------------------------------------------------
// Dav1dCodec — the real software AV1 decoder.
//
// dav1d is decode-only, so this codec reports `can_encode() == false`.
// The AV1 plugin factory pairs it with the stub encoder (or an NVENC
// encoder) through `nvcodec::HwCompositeCodec`.
// ---------------------------------------------------------------------------
class Dav1dCodec final : public plugins::IVideoCodec {
public:
    explicit Dav1dCodec(plugins::VideoCodecConfig cfg) noexcept
        : cfg_(std::move(cfg)) {}

    ~Dav1dCodec() override { close(); }

    Dav1dCodec(const Dav1dCodec&)            = delete;
    Dav1dCodec& operator=(const Dav1dCodec&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override {
        return "nimrtc::av1::Dav1dCodec (vendored dav1d, CPU)";
    }

    plugins::Status open() noexcept override {
        if (open_) return plugins::kOk;

        Dav1dSettings settings;
        dav1d_default_settings(&settings);
        // Single-threaded decode keeps the codec deterministic (the tests
        // compare decoded pixels) and avoids spawning a thread pool per
        // session.  A host that wants throughput can raise this.
        settings.n_threads = 1;
        settings.max_frame_delay = 1;
        // NimRTC routes codec diagnostics through its own logger; dav1d's
        // stderr callback would bypass it.
        settings.apply_grain = 0;

        if (dav1d_open(&decoder_, &settings) != 0 || !decoder_) {
            core::log::Logger::instance().error("dav1d: dav1d_open failed");
            decoder_ = nullptr;
            return plugins::kErrInternal;
        }
        open_ = true;
        core::log::Logger::instance().info("dav1d: opened software AV1 decoder");
        return plugins::kOk;
    }

    void close() noexcept override {
        if (decoder_) {
            dav1d_close(&decoder_);
            decoder_ = nullptr;
        }
        open_ = false;
    }

    // ---- plugins::IVideoCodec ----------------------------------------------

    // dav1d is decode-only: it cannot encode AV1.
    //
    // `can_decode()` answers the *capability* question, not "is the
    // session open yet" — the same contract the other backends follow
    // (see `plugins::IVideoCodec`).  The decode path itself checks the
    // decoder handle and reports kErrNotReady when it is not open yet.
    bool can_encode() const noexcept override { return false; }
    bool can_decode() const noexcept override { return true; }

    plugins::VideoCodecKind kind() const noexcept override {
        return plugins::VideoCodecKind::kAV1;
    }
    std::string_view codec_name() const noexcept override { return "av1"; }

    plugins::Status encode(const plugins::VideoFrame&,
                           std::uint8_t*, std::size_t,
                           plugins::EncodedVideoFrame&) noexcept override {
        // dav1d is a decoder only.  The registry pairs this codec with an
        // encoder through HwCompositeCodec, so reaching this method means
        // the composition went wrong.
        stats_.encode_errors++;
        return plugins::kErrUnsupported;
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

        // ---- Feed the OBU stream to dav1d ----------------------------------
        //
        // NimRTC carries AV1 OBUs in Annex-B framing (`00 00 00 01`
        // before each OBU — see `video_payload::av1::parse_annex_b_obus`),
        // while dav1d expects a low-overhead OBU stream (raw OBUs
        // concatenated, no start codes).  Strip the start codes here so
        // both framings decode: if the payload has no start code it is
        // already low-overhead and is fed as-is (this is also what the
        // unit test's embedded fixture uses).
        //
        // dav1d takes ownership of the Dav1dData on success, so the
        // payload is copied into a dav1d-owned buffer (dav1d_data_create)
        // and handed over.  The copy is unavoidable: dav1d keeps the
        // buffer alive across calls, while the caller's payload lives in
        // the engine's frame buffer.
        core::ByteSpan obu_stream{encoded.payload.data(), encoded.payload.size()};
        std::vector<std::uint8_t> stripped;
        {
            std::vector<video_payload::av1::Obu> obus;
            if (video_payload::av1::parse_annex_b_obus(
                    core::ByteSpan{encoded.payload.data(), encoded.payload.size()},
                    obus)) {
                std::size_t total = 0;
                for (const auto& o : obus) total += o.data.size();
                stripped.reserve(total);
                for (const auto& o : obus) {
                    stripped.insert(stripped.end(), o.data.data(),
                                    o.data.data() + o.data.size());
                }
                obu_stream = core::ByteSpan{stripped.data(), stripped.size()};
            }
        }
        Dav1dData data;
        memset(&data, 0, sizeof(data));
        std::uint8_t* buf = dav1d_data_create(&data, obu_stream.size());
        if (!buf) {
            stats_.decode_errors++;
            return plugins::kErrInternal;
        }
        std::memcpy(buf, obu_stream.data(), obu_stream.size());

        int res = dav1d_send_data(decoder_, &data);
        if (res < 0 && res != DAV1D_ERR(EAGAIN)) {
            dav1d_data_unref(&data);
            stats_.decode_errors++;
            return plugins::kErrCorrupt;
        }

        // ---- Pull the decoded picture ---------------------------------------
        Dav1dPicture picture;
        memset(&picture, 0, sizeof(picture));
        res = dav1d_get_picture(decoder_, &picture);
        if (res < 0) {
            dav1d_data_unref(&data);
            if (res == DAV1D_ERR(EAGAIN)) {
                // The frame was buffered — not an error, the picture will
                // come out on a later call.
                return plugins::kErrNotReady;
            }
            stats_.decode_errors++;
            return plugins::kErrCorrupt;
        }

        const plugins::Status status = emit_picture(picture, encoded, raw_out,
                                                     output_buffers);
        dav1d_picture_unref(&picture);
        dav1d_data_unref(&data);

        if (status == plugins::kOk) {
            stats_.frames_decoded++;
            stats_.bytes_decoded += encoded.payload.size();
        } else {
            stats_.decode_errors++;
        }
        return status;
    }

    plugins::Status force_keyframe() noexcept override {
        // Decoder-only: nothing to force.
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
     *  "dav1d" distinguishes this from the synthetic stub, which reports
     *  "software-stub". */
    std::string_view backend_id() const noexcept override { return "dav1d"; }

    bool is_hardware_accelerated() const noexcept override { return false; }

private:
    /** Copy one decoded I420 picture into the caller's buffers (or into a
     *  freshly allocated buffer owned by @p raw_out). */
    plugins::Status emit_picture(const Dav1dPicture& picture,
                                 const plugins::EncodedVideoFrame& encoded,
                                 plugins::VideoFrame& raw_out,
                                 std::uint8_t* const* output_buffers) noexcept {
        const int w = picture.p.w;
        const int h = picture.p.h;
        if (w <= 0 || h <= 0 || !picture.data[0]) return plugins::kErrCorrupt;

        // dav1d is configured for 8-bit output (the vendored build sets
        // CONFIG_8BPC), so the planes are byte-addressable.
        if (picture.p.bpc != 8) {
            core::log::Logger::instance().warn(
                "dav1d: expected an 8-bit picture, got " +
                std::to_string(picture.p.bpc) + " bpc");
            return plugins::kErrUnsupported;
        }

        const int stride_y = static_cast<int>(picture.stride[0]);
        const int stride_c = static_cast<int>(picture.stride[1]);
        const auto* src_y = static_cast<const std::uint8_t*>(picture.data[0]);
        const auto* src_u = static_cast<const std::uint8_t*>(picture.data[1]);
        const auto* src_v = static_cast<const std::uint8_t*>(picture.data[2]);

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
                                            static_cast<std::uint32_t>(w),
                                            static_cast<std::uint32_t>(h));
            cpu_buf = std::make_shared<video_frame::VideoFrameBuffer>(layout);
        }

        const std::uint32_t out_w = static_cast<std::uint32_t>(w);
        const std::uint32_t out_h = static_cast<std::uint32_t>(h);
        const std::uint32_t cw = out_w / 2;
        const std::uint32_t ch = out_h / 2;

        if (caller_supplied) {
            copy_plane(output_buffers[0], static_cast<std::int32_t>(out_w),
                       src_y, stride_y, out_w, out_h);
            copy_plane(output_buffers[1], static_cast<std::int32_t>(cw),
                       src_u, stride_c, cw, ch);
            copy_plane(output_buffers[2], static_cast<std::int32_t>(cw),
                       src_v, stride_c, cw, ch);
        } else {
            const video_frame::VideoFrameLayout& layout = cpu_buf->layout();
            copy_plane(cpu_buf->plane(0), layout.strides[0], src_y, stride_y, out_w, out_h);
            copy_plane(cpu_buf->plane(1), layout.strides[1], src_u, stride_c, cw, ch);
            copy_plane(cpu_buf->plane(2), layout.strides[2], src_v, stride_c, cw, ch);
        }

        raw_out.buffer = std::move(cpu_buf);
        raw_out.info.capture_ts_us = encoded.info.capture_ts_us;
        raw_out.info.frame_seq     = encoded.info.frame_seq;
        raw_out.info.rtp_timestamp = encoded.info.rtp_timestamp;
        return plugins::kOk;
    }

    /** Copy a `w`x`h` 8-bit plane from `src` (stride `src_stride`) to `dst`
     *  (stride `dst_stride`). */
    static void copy_plane(std::uint8_t* dst, std::int32_t dst_stride,
                           const std::uint8_t* src, int src_stride,
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
    bool open_ = false;
    Dav1dContext* decoder_ = nullptr;
};

// ---------------------------------------------------------------------------
// Backend availability probe.
//
// The vendored dav1d is statically linked into NimRTC, so the backend is
// available whenever the translation unit was compiled with
// NIMRTC_PLUGINS_DAV1D_ON.
// ---------------------------------------------------------------------------

bool dav1d_av1_available() noexcept {
    return true;
}

std::unique_ptr<plugins::IVideoCodec>
make_dav1d_av1_codec(plugins::VideoCodecConfig cfg) {
    return std::make_unique<Dav1dCodec>(std::move(cfg));
}

#else   // NIMRTC_PLUGINS_DAV1D_ON not defined

bool dav1d_av1_available() noexcept { return false; }

std::unique_ptr<plugins::IVideoCodec>
make_dav1d_av1_codec(plugins::VideoCodecConfig) { return nullptr; }

#endif

} // namespace nimrtc::av1
