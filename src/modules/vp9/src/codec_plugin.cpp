/**
 * @file src/modules/vp9/src/codec_plugin.cpp
 * @brief CodecPluginAdapter + Vp9PluginFactory implementation.
 *
 * Wraps a concrete vp9::Decoder behind plugins::IVideoCodec.
 *
 * ## The synthetic bitstream
 *
 * The stub's `encode()` emits a *synthetic* VP9-shaped frame.  VP9 has
 * no start-code framing (one frame per access unit), so the synthetic
 * payload is simply a short byte sequence that carries the frame
 * sequence number — enough for the pipeline to prove that a frame
 * travelled from the sender to the receiver in order.
 *
 * The real bitstream comes from the vendored libvpx backend (the
 * `libvpx_vp9` row), which is compiled in only when
 * `NIMRTC_VENDORED_LIBVPX=ON`.
 */

#include <nimrtc/vp9/codec_plugin.hpp>

#include <cstring>   // memcpy, memset

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/vp9/hw_backends.hpp>

// The combined `IVideoCodec` view of a split encoder/decoder pair.
#include <nimrtc/plugins/hw_composite_codec.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::vp9 {

// ---------------------------------------------------------------------------
// CodecPluginAdapter
// ---------------------------------------------------------------------------

CodecPluginAdapter::CodecPluginAdapter(plugins::VideoCodecConfig cfg) noexcept
    : cfg_(std::move(cfg)) {
    core::log::Logger::instance().debug(
        "vp9::CodecPluginAdapter created (stub VP9 decoder behind plugins::IVideoCodec)");
}

CodecPluginAdapter::~CodecPluginAdapter() = default;

const char* CodecPluginAdapter::name() const noexcept {
    return "nimrtc::vp9::CodecPluginAdapter (stub VP9 decoder behind plugins::IVideoCodec)";
}

plugins::Status CodecPluginAdapter::open() noexcept {
    decoder_ = create_stub_decoder();
    if (!decoder_) return plugins::kErrInternal;
    const plugins::Status s = decoder_->open();
    if (s != plugins::kOk) {
        decoder_.reset();
        return s;
    }
    return plugins::kOk;
}

void CodecPluginAdapter::close() noexcept {
    if (decoder_) decoder_->close();
    decoder_.reset();
}

plugins::Status CodecPluginAdapter::encode(const plugins::VideoFrame& raw,
                                           std::uint8_t* output,
                                           std::size_t output_capacity,
                                           plugins::EncodedVideoFrame& encoded_out) noexcept {
    // ---- Stub VP9 encoder --------------------------------------------------
    //
    // Emits a *synthetic* VP9-shaped frame.  The frame sequence number
    // rides in the first four bytes so the receiver can confirm that
    // round-tripped frames arrive in order; the rest is a pseudo-random
    // fill so each frame looks distinct on the wire.
    if (output == nullptr || output_capacity == 0) {
        stats_.encode_errors++;
        return plugins::kErrInvalidParam;
    }
    const std::uint32_t w = raw.width()  ? raw.width()  : cfg_.width;
    const std::uint32_t h = raw.height() ? raw.height() : cfg_.height;
    if (w == 0 || h == 0) {
        stats_.encode_errors++;
        return plugins::kErrInvalidParam;
    }

    constexpr std::size_t kPayloadBytes = 16;
    std::uint8_t payload[kPayloadBytes];
    const std::uint32_t seq = raw.info.frame_seq;
    payload[0] = static_cast<std::uint8_t>(seq         & 0xFF);
    payload[1] = static_cast<std::uint8_t>((seq >> 8)  & 0xFF);
    payload[2] = static_cast<std::uint8_t>((seq >> 16) & 0xFF);
    payload[3] = static_cast<std::uint8_t>((seq >> 24) & 0xFF);
    std::uint32_t seed = seq ^ 0xA5A5A5A5u;
    for (std::size_t i = 4; i < kPayloadBytes; ++i) {
        seed = seed * 1664525u + 1013904223u;     // Numerical Recipes LCG
        payload[i] = static_cast<std::uint8_t>(seed >> 24);
    }

    if (kPayloadBytes > output_capacity) {
        stats_.encode_errors++;
        return plugins::kErrBufferTooSmall;
    }
    std::memcpy(output, payload, kPayloadBytes);

    encoded_out.codec         = plugins::VideoCodecKind::kVP9;
    encoded_out.payload       = core::ByteSpan{output, kPayloadBytes};
    encoded_out.payload_type  = cfg_.payload_type;
    encoded_out.is_keyframe   = true;     // the stub always emits a
                                          // random-access point
    encoded_out.nalu_format   = plugins::NaluFormat::kUnknown;
    encoded_out.info.capture_ts_us = raw.info.capture_ts_us;
    encoded_out.info.frame_seq     = raw.info.frame_seq;
    encoded_out.info.rtp_timestamp = raw.info.rtp_timestamp;

    stats_.frames_encoded++;
    stats_.bytes_encoded += kPayloadBytes;
    return plugins::kOk;
}

plugins::Status CodecPluginAdapter::decode(const plugins::EncodedVideoFrame& encoded,
                                           plugins::VideoFrame& raw_out,
                                           std::uint8_t* const* output_buffers) noexcept {
    if (!decoder_) return plugins::kErrNotReady;
    if (encoded.payload.empty()) {
        stats_.decode_errors++;
        return plugins::kErrInvalidParam;
    }

    // Convert plugins::EncodedVideoFrame::Info → video_frame::VideoFrameInfo.
    video_frame::VideoFrameInfo info{};
    info.capture_ts_us = encoded.info.capture_ts_us;
    info.frame_seq     = encoded.info.frame_seq;
    info.rtp_timestamp = encoded.info.rtp_timestamp;

    auto decoded = decoder_->decode(encoded.payload, info);
    if (!decoded) {
        stats_.decode_errors++;
        return plugins::kErrCorrupt;
    }

    stats_.frames_decoded++;
    stats_.bytes_decoded += encoded.payload.size();

    // The stub does not parse the frame header, so the geometry comes
    // from the codec config (which the encoder stamped from the source's
    // actual dimensions).
    const std::uint32_t w = decoded->width  ? decoded->width  : cfg_.width;
    const std::uint32_t h = decoded->height ? decoded->height : cfg_.height;
    if (w == 0 || h == 0) {
        stats_.decode_errors++;
        return plugins::kErrCorrupt;
    }

    raw_out.info.capture_ts_us = decoded->capture_ts_us;
    raw_out.info.frame_seq     = decoded->frame_seq;
    raw_out.info.rtp_timestamp = decoded->rtp_timestamp;

    // ---- Resolve the pixel buffers ----------------------------------------
    //   1. The caller supplied output_buffers and raw_out already owns a
    //      buffer matching output_buffers[0] → reuse it (zero-copy).
    //   2. The caller supplied output_buffers but raw_out has no matching
    //      buffer → allocate a fresh one for raw_out and paint into the
    //      caller's memory.
    //   3. No caller buffers → allocate a fresh one and paint into it;
    //      raw_out takes ownership.
    std::shared_ptr<video_frame::VideoFrameBuffer> cpu_buf;
    std::uint8_t* y_plane = nullptr;
    std::uint8_t* u_plane = nullptr;
    std::uint8_t* v_plane = nullptr;
    const bool caller_supplied_buffers = output_buffers && output_buffers[0];

    if (caller_supplied_buffers) {
        y_plane = output_buffers[0];
        u_plane = output_buffers[1];
        v_plane = (cfg_.pixel_format == plugins::VideoPixelFormat::kNV12)
                      ? nullptr          : output_buffers[2];
        if (auto existing = raw_out.cpu_buffer();
            existing && existing->data() == output_buffers[0]) {
            cpu_buf = std::move(existing);
        }
    }
    if (!cpu_buf) {
        const video_frame::VideoFrameLayout layout =
            video_frame::compute_layout(video_frame::PixelFormat::kI420, w, h);
        cpu_buf = std::make_shared<video_frame::VideoFrameBuffer>(layout);
        if (!caller_supplied_buffers) {
            // The plane pointers must come from the layout (which knows
            // the 16-byte-aligned strides and the plane offsets), not from
            // a hand-computed w*h offset.
            y_plane = cpu_buf->plane(0);
            u_plane = cpu_buf->plane(1);
            v_plane = cpu_buf->plane(2);
        }
    }

    // ---- Paint the I420 planes with a recognisable synthetic pattern ------
    if (caller_supplied_buffers) {
        paint_i420(y_plane, u_plane, v_plane, w, h, decoded->frame_seq,
                   w, w / 2, w / 2);
    } else {
        const video_frame::VideoFrameLayout layout =
            video_frame::compute_layout(video_frame::PixelFormat::kI420, w, h);
        // The layout's strides are signed (a negative stride would mean a
        // bottom-up plane), while paint_i420 takes them unsigned.  A
        // negative stride is not something this path can produce --
        // compute_layout always returns forward, positive strides -- so
        // the cast is a narrowing of a value the caller has already
        // validated.
        paint_i420(y_plane, u_plane, v_plane, w, h, decoded->frame_seq,
                   static_cast<std::uint32_t>(layout.strides[0]),
                   static_cast<std::uint32_t>(layout.strides[1]),
                   static_cast<std::uint32_t>(layout.strides[2]));
    }

    // ---- Wire up the caller-visible VideoFrame ----------------------------
    raw_out.buffer = std::move(cpu_buf);
    raw_out.info.capture_ts_us = decoded->capture_ts_us;
    raw_out.info.frame_seq     = decoded->frame_seq;
    raw_out.info.rtp_timestamp = decoded->rtp_timestamp;

    return plugins::kOk;
}

void CodecPluginAdapter::paint_i420(std::uint8_t* y,
                                    std::uint8_t* u,
                                    std::uint8_t* v,
                                    std::uint32_t w, std::uint32_t h,
                                    std::uint32_t frame_seq,
                                    std::uint32_t stride_y,
                                    std::uint32_t stride_u,
                                    std::uint32_t stride_v) noexcept {
    video_frame::paint_test_pattern_i420(y, u, v, w, h, frame_seq,
                                         static_cast<std::int32_t>(stride_y),
                                         static_cast<std::int32_t>(stride_u),
                                         static_cast<std::int32_t>(stride_v));
}

plugins::Status CodecPluginAdapter::update_config(plugins::VideoCodecConfig cfg) noexcept {
    cfg_ = std::move(cfg);
    return plugins::kOk;
}

plugins::VideoCodecStats CodecPluginAdapter::stats() const noexcept {
    return stats_;
}

std::uint8_t CodecPluginAdapter::payload_type() const noexcept {
    // VP9 has no statically-assigned payload type in RFC 3551; 98 is
    // the value RFC 9559 recommends and the one Chrome's Unified Plan
    // uses for VP9.
    return cfg_.payload_type != 0 ? cfg_.payload_type : std::uint8_t{98};
}

// ---------------------------------------------------------------------------
// Vp9PluginFactory
// ---------------------------------------------------------------------------

plugins::IVideoEncoder* Vp9PluginFactory::create_encoder(
        plugins::VideoCodecConfig cfg) const {
    // The encoder table is consulted on its own: NVENC is an encoder,
    // NVDEC is a decoder, and neither can serve the other's direction.
    // That separation is the whole point of the split seam.
    std::unique_ptr<plugins::IVideoEncoder> encoder;
    const plugins::VideoEncoderBackend* enc_be = select_encoder_backend(cfg);
    if (enc_be && enc_be->create) {
        encoder = enc_be->create(cfg);
        if (encoder) {
            const std::string hw_str = enc_be->is_hw ? "yes" : "no";
            const std::string zc_str = enc_be->zero_copy_supported ? "yes" : "no";
            const std::string id_str{enc_be->id};
            const std::string be_str{plugins::hw_backend_name(enc_be->backend)};
            core::log::Logger::instance().info(
                std::string{"vp9: selected encoder backend '"} + id_str +
                "' (" + be_str + ", hw=" + hw_str +
                ", zero_copy=" + zc_str + ")");
        }
    }

    if (!encoder) {
        // Last-resort fallback (the selector normally always returns at
        // least the stub row).
        core::log::Logger::instance().warn(
            "vp9: no encoder backend available — falling back to the "
            "software stub encoder");
        return new CodecPluginAdapter(std::move(cfg));
    }
    return encoder.release();
}

plugins::IVideoDecoder* Vp9PluginFactory::create_decoder(
        plugins::VideoCodecConfig cfg) const {
    // The decoder table is consulted on its own.
    std::unique_ptr<plugins::IVideoDecoder> decoder;
    const plugins::VideoDecoderBackend* dec_be = select_decoder_backend(cfg);
    if (dec_be && dec_be->create) {
        decoder = dec_be->create(cfg);
        if (decoder) {
            const std::string id_str{dec_be->id};
            const std::string be_str{plugins::hw_backend_name(dec_be->backend)};
            core::log::Logger::instance().info(
                std::string{"vp9: selected decoder backend '"} + id_str +
                "' (" + be_str + ")");
        }
    }

    if (!decoder) {
        core::log::Logger::instance().warn(
            "vp9: no decoder backend available — falling back to the "
            "software stub decoder");
        return new CodecPluginAdapter(std::move(cfg));
    }
    return decoder.release();
}

plugins::IVideoCodec* Vp9PluginFactory::create(plugins::VideoCodecConfig cfg) const {
    // The combined seam, kept for source compatibility with a host that
    // predates the split.
    //
    // The two halves are created *independently* — the encoder table and
    // the decoder table are separate, and a hardware encoder (NVENC) is
    // not a hardware decoder (NVDEC).
    std::unique_ptr<plugins::IVideoEncoder> encoder;
    std::unique_ptr<plugins::IVideoDecoder> decoder;

    const plugins::VideoEncoderBackend* enc_be = select_encoder_backend(cfg);
    if (enc_be && enc_be->create) {
        encoder = enc_be->create(cfg);
        if (encoder) {
            const std::string id_str{enc_be->id};
            const std::string be_str{plugins::hw_backend_name(enc_be->backend)};
            core::log::Logger::instance().info(
                std::string{"vp9: selected encoder backend '"} + id_str +
                "' (" + be_str + ", hw=" +
                (enc_be->is_hw ? "yes" : "no") + ")");
        }
    }

    const plugins::VideoDecoderBackend* dec_be = select_decoder_backend(cfg);
    if (dec_be && dec_be->create) {
        decoder = dec_be->create(cfg);
        if (decoder) {
            const std::string id_str{dec_be->id};
            const std::string be_str{plugins::hw_backend_name(dec_be->backend)};
            core::log::Logger::instance().info(
                std::string{"vp9: selected decoder backend '"} + id_str +
                "' (" + be_str + ")");
        }
    }

    // The backend preference could not be satisfied (plan B2).
    if (!encoder && !decoder &&
        cfg.backend_preference != plugins::VideoCodecBackendPreference::kAuto) {
        core::log::Logger::instance().error(
            "vp9: no codec backend is available on this machine — the "
            "requested backend class cannot be satisfied");
        return nullptr;
    }

    if (!encoder && !decoder) {
        // Last-resort fallback (the selector should always return at least
        // the stub).
        return new CodecPluginAdapter(std::move(cfg));
    }

    // The two halves are paired.  `HwCompositeCodec` is the combined
    // `IVideoCodec` view of a split pair, and it accepts a null half.
    auto* composite = new plugins::HwCompositeCodec(std::move(cfg),
                                                    std::move(encoder),
                                                    std::move(decoder));
    core::log::Logger::instance().info(
        "vp9: combined the encoder and the decoder into a composite codec");
    return composite;
}

// ---------------------------------------------------------------------------
// Public registration entry point
// ---------------------------------------------------------------------------

namespace detail {

void do_register_default_plugins() noexcept {
    static const struct Registrar {
        Registrar() {
            // 1. Register the VP9 plugin factory under id "vp9" so
            // EngineConfig::video_codec_name == "vp9" picks it up.
            static nimrtc::vp9::Vp9PluginFactory s_factory{};
            nimrtc::core::PluginRegistry::instance().register_video_codec(
                std::string_view{s_factory.id()}, &s_factory);
            nimrtc::core::PluginRegistry::instance().register_video_encoder(
                std::string_view{s_factory.id()}, &s_factory);
            nimrtc::core::PluginRegistry::instance().register_video_decoder(
                std::string_view{s_factory.id()}, &s_factory);
            // 2. Populate the per-platform HW backend table.
            nimrtc::vp9::register_default_video_backends();
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

void register_default_plugins() noexcept {
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::vp9
