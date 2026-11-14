/**
 * @file src/modules/av1/src/codec_plugin.cpp
 * @brief CodecPluginAdapter + Av1PluginFactory implementation.
 *
 * Wraps a concrete av1::Decoder behind plugins::IVideoCodec.
 */

#include <nimrtc/av1/codec_plugin.hpp>

#include <cstring>   // memcpy, memset

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/av1/hw_backends.hpp>

#include <nimrtc/nvcodec/hw_composite_codec.hpp>
#include <nimrtc/plugins/hw_video_backend.hpp>

namespace nimrtc::av1 {

namespace {

// ---------------------------------------------------------------------------
// The AV1 OBU header (AV1 spec §5.3.1).
//
//    0 1 2 3 4 5 6 7
//   +-+-+-+-+-+-+-+-+
//   |1| type  |X|S|-|
//   +-+-+-+-+-+-+-+-+
//
//   bit 7     : obu_forbidden_bit (always 0)
//   bits 6..3 : obu_type
//   bit 2     : obu_extension_flag
//   bit 1     : obu_has_size_field
//   bit 0     : obu_reserved_1bit
// ---------------------------------------------------------------------------

constexpr std::uint8_t obu_header(std::uint8_t type, bool has_size) noexcept {
    return static_cast<std::uint8_t>(((type & 0x0F) << 3) |
                                     (has_size ? 0x02 : 0x00));
}

} // namespace

// ---------------------------------------------------------------------------
// CodecPluginAdapter
// ---------------------------------------------------------------------------

CodecPluginAdapter::CodecPluginAdapter(plugins::VideoCodecConfig cfg) noexcept
    : cfg_(std::move(cfg)) {
    core::log::Logger::instance().debug(
        "av1::CodecPluginAdapter created (decoder-only stub)");
}

CodecPluginAdapter::~CodecPluginAdapter() = default;

const char* CodecPluginAdapter::name() const noexcept {
    return "nimrtc::av1::CodecPluginAdapter (stub AV1 decoder behind plugins::IVideoCodec)";
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
    // ---- Stub AV1 encoder --------------------------------------------------
    //
    // Emits a *synthetic* Annex B OBU stream shaped like:
    //
    //   00 00 00 01 [TD OBU]        type 2 (temporal delimiter)
    //   00 00 00 01 [SEQ HDR OBU]   type 1 (sequence header)
    //   00 00 00 01 [FRAME OBU]     type 6 (frame), payload = frame_seq
    //
    // The sequence header is the AV1 random-access signal, and the stub
    // decoder on the far side recognises it and reports the frame as a
    // key frame.  The receiver then paints the matching synthetic
    // pattern.  This exercises the whole send → RTP → SRTP → ICE → recv
    // → OBU reassembly → decode → render pipeline for AV1 without
    // dragging a proprietary codec into the tree.
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

    // ---- Temporal delimiter OBU (type 2) — header only, no payload -------
    const std::uint8_t td_obu[1] = {obu_header(2, /*has_size=*/false)};

    // ---- Sequence header OBU (type 1) — header + size + payload ---------
    // The payload is opaque to NimRTC (a real decoder parses the
    // sequence header OBU's payload); the stub only looks at the type.
    constexpr std::size_t kSeqHdrPayload = 12;
    std::uint8_t seq_hdr_obu[1 + 1 + kSeqHdrPayload];
    seq_hdr_obu[0] = obu_header(1, /*has_size=*/true);
    seq_hdr_obu[1] = static_cast<std::uint8_t>(kSeqHdrPayload);
    std::memset(seq_hdr_obu + 2, 0x00, kSeqHdrPayload);

    // ---- Frame OBU (type 6) — header + size + payload -------------------
    // The payload carries the frame_seq (LE 4 bytes) so the receiver can
    // confirm that round-tripped frames arrive in order, followed by a
    // pseudo-random fill so each frame looks distinct on the wire.
    constexpr std::size_t kFramePayload = 16;
    std::uint8_t frame_obu[1 + 1 + kFramePayload];
    frame_obu[0] = obu_header(6, /*has_size=*/true);
    frame_obu[1] = static_cast<std::uint8_t>(kFramePayload);
    const std::uint32_t seq = raw.info.frame_seq;
    frame_obu[2] = static_cast<std::uint8_t>(seq         & 0xFF);
    frame_obu[3] = static_cast<std::uint8_t>((seq >> 8)  & 0xFF);
    frame_obu[4] = static_cast<std::uint8_t>((seq >> 16) & 0xFF);
    frame_obu[5] = static_cast<std::uint8_t>((seq >> 24) & 0xFF);
    std::uint32_t seed = seq ^ 0xA5A5A5A5u;
    for (std::size_t i = 6; i < 2 + kFramePayload; ++i) {
        seed = seed * 1664525u + 1013904223u;     // Numerical Recipes LCG
        frame_obu[i] = static_cast<std::uint8_t>(seed >> 24);
    }

    // ---- Assemble the Annex B bitstream ------------------------------------
    constexpr std::uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};
    std::size_t pos = 0;
    auto append = [&](const std::uint8_t* data, std::size_t n) -> bool {
        if (pos + n > output_capacity) return false;
        std::memcpy(output + pos, data, n);
        pos += n;
        return true;
    };
    if (!append(kStartCode, 4))                    { stats_.encode_errors++; return plugins::kErrInternal; }
    if (!append(td_obu, sizeof(td_obu)))           { stats_.encode_errors++; return plugins::kErrInternal; }
    if (!append(kStartCode, 4))                    { stats_.encode_errors++; return plugins::kErrInternal; }
    if (!append(seq_hdr_obu, sizeof(seq_hdr_obu))) { stats_.encode_errors++; return plugins::kErrInternal; }
    if (!append(kStartCode, 4))                    { stats_.encode_errors++; return plugins::kErrInternal; }
    if (!append(frame_obu, sizeof(frame_obu)))     { stats_.encode_errors++; return plugins::kErrInternal; }

    encoded_out.codec         = plugins::VideoCodecKind::kAV1;
    encoded_out.payload       = core::ByteSpan{output, pos};
    encoded_out.payload_type  = cfg_.payload_type;
    encoded_out.is_keyframe   = true;     // the stub always repeats the
                                          // sequence header → key frame
    encoded_out.nalu_format   = plugins::NaluFormat::kAnnexBStartCode;
    encoded_out.info.capture_ts_us = raw.info.capture_ts_us;
    encoded_out.info.frame_seq     = raw.info.frame_seq;
    encoded_out.info.rtp_timestamp = raw.info.rtp_timestamp;

    stats_.frames_encoded++;
    stats_.bytes_encoded += pos;
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

    // The stub does not parse the sequence header, so the geometry comes
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
    // AV1 has no statically-assigned payload type in RFC 3551; 45 is the
    // value the AV1 RTP payload specification (draft-ietf-payload-av1
    // §10) and Chrome's Unified Plan use for AV1.
    return cfg_.payload_type != 0 ? cfg_.payload_type : std::uint8_t{45};
}

// ---------------------------------------------------------------------------
// Av1PluginFactory
// ---------------------------------------------------------------------------

plugins::IVideoCodec* Av1PluginFactory::create(plugins::VideoCodecConfig cfg) const {
    // Delegate to the HW-first backend selector.  On hosts where no HW
    // backend is available the selector returns the stub backend (which
    // this very file implements).
    //
    // The engine holds ONE IVideoCodec per session and drives both
    // directions through it, so the factory has to hand back something
    // that can encode *and* decode.  The hardware backends are
    // one-directional (NVENC encodes, NVDEC decodes), so when the two
    // directions resolve to different backends they are wrapped in a
    // HwCompositeCodec.
    std::unique_ptr<plugins::IVideoCodec> encoder;
    const plugins::VideoEncoderBackend* enc_be = select_encoder_backend(cfg);
    if (enc_be && enc_be->create) {
        encoder = enc_be->create(cfg);
        if (encoder) {
            const std::string hw_str = enc_be->is_hw ? "yes" : "no";
            const std::string zc_str = enc_be->zero_copy_supported ? "yes" : "no";
            const std::string id_str{enc_be->id};
            const std::string be_str{plugins::hw_backend_name(enc_be->backend)};
            core::log::Logger::instance().info(
                std::string{"av1: selected encoder backend '"} + id_str +
                "' (" + be_str + ", hw=" + hw_str +
                ", zero_copy=" + zc_str + ")");
        }
    }

    // The encoder already serves both directions (the software stub) —
    // no composite needed.
    if (encoder && encoder->can_decode()) {
        return encoder.release();
    }

    // Pick the decoder backend for the receive direction.
    std::unique_ptr<plugins::IVideoCodec> decoder;
    const plugins::VideoDecoderBackend* dec_be = select_decoder_backend(cfg);
    if (dec_be && dec_be->create) {
        decoder = dec_be->create(cfg);
        if (decoder) {
            const std::string id_str{dec_be->id};
            const std::string be_str{plugins::hw_backend_name(dec_be->backend)};
            core::log::Logger::instance().info(
                std::string{"av1: selected decoder backend '"} + id_str +
                "' (" + be_str + ")");
        }
    }

    if (!encoder && !decoder) {
        // The backend preference could not be satisfied (plan B2).
        if (cfg.backend_preference != plugins::VideoCodecBackendPreference::kAuto) {
            core::log::Logger::instance().error(
                std::string{"av1: no "} +
                std::string{plugins::video_codec_backend_preference_name(
                    cfg.backend_preference)} +
                " codec backend is available on this machine — the "
                "requested backend class cannot be satisfied");
            return nullptr;
        }
        // Last-resort fallback (the selector should always return at
        // least the stub).
        return new CodecPluginAdapter(std::move(cfg));
    }

    if (!decoder) {
        // An encoder without a decoder: the receive path is unusable, but
        // the send path still works.
        if (cfg.backend_preference != plugins::VideoCodecBackendPreference::kAuto) {
            core::log::Logger::instance().error(
                std::string{"av1: no "} +
                std::string{plugins::video_codec_backend_preference_name(
                    cfg.backend_preference)} +
                " decoder backend is available on this machine — the "
                "requested backend class cannot be satisfied");
            return nullptr;
        }
        core::log::Logger::instance().warn(
            "av1: no decoder backend available — the codec can encode only");
        return encoder.release();
    }

    if (!encoder) {
        // A decoder without an encoder: the send path is unusable.
        if (cfg.backend_preference != plugins::VideoCodecBackendPreference::kAuto) {
            core::log::Logger::instance().error(
                std::string{"av1: no "} +
                std::string{plugins::video_codec_backend_preference_name(
                    cfg.backend_preference)} +
                " encoder backend is available on this machine — the "
                "requested backend class cannot be satisfied");
            return nullptr;
        }
        core::log::Logger::instance().warn(
            "av1: no encoder backend available — the codec can decode only");
        return decoder.release();
    }

    // Two one-directional backends: pair them.
    auto* composite = new nvcodec::HwCompositeCodec(std::move(cfg),
                                                    std::move(encoder),
                                                    std::move(decoder));
    core::log::Logger::instance().info(
        "av1: combined the encoder and the decoder into a composite codec");
    return composite;
}

// ---------------------------------------------------------------------------
// Public registration entry point
// ---------------------------------------------------------------------------

namespace detail {

void do_register_default_plugins() noexcept {
    static const struct Registrar {
        Registrar() {
            // 1. Register the AV1 plugin factory under id "av1" so
            //    EngineConfig::video_codec_name == "av1" picks it up.
            static nimrtc::av1::Av1PluginFactory s_factory{};
            nimrtc::core::PluginRegistry::instance().register_video_codec(
                std::string_view{s_factory.id()}, &s_factory);
            // 2. Populate the per-platform HW backend table.
            nimrtc::av1::register_default_video_backends();
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

} // namespace nimrtc::av1
