/**
 * @file src/modules/h264/src/codec_plugin.cpp
 * @brief CodecPluginAdapter + H264PluginFactory implementation.
 *
 * Wraps concrete h264::Decoder behind plugins::IVideoCodec.
 */

#include <nimrtc/h264/codec_plugin.hpp>

#include <cstring>   // memcpy, memset

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/h264/hw_backends.hpp>
#include <nimrtc/plugins/hw_video_backend.hpp>

namespace nimrtc::h264 {

// ---------------------------------------------------------------------------
// CodecPluginAdapter
// ---------------------------------------------------------------------------

CodecPluginAdapter::CodecPluginAdapter(plugins::VideoCodecConfig cfg) noexcept
    : cfg_(std::move(cfg)) {
    core::log::Logger::instance().debug(
        "h264::CodecPluginAdapter created (decoder-only stub)");
}

CodecPluginAdapter::~CodecPluginAdapter() = default;

const char* CodecPluginAdapter::name() const noexcept {
    return "nimrtc::h264::CodecPluginAdapter (stub H.264 decoder behind plugins::IVideoCodec)";
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
    // ---- Stub H.264 encoder ------------------------------------------------
    //
    // Emits a *synthetic* Annex B bitstream shaped like:
    //
    //   00 00 00 01 [SPS NAL]   profile=Baseline(66) level=30, w/h from cfg
    //   00 00 00 01 [PPS NAL]   1 byte RBSP
    //   00 00 00 01 [IDR slice] payload = [frame_seq_lo, frame_seq_hi,
    //                                     ...pattern bytes...]
    //
    // The bitstream is recognised by the matching StubDecoder below (which
    // sets width/height from the codec config and paints an I420 frame with
    // a recognisable pattern).  This is enough to validate the full
    // send_video() → RTP → SRTP → ICE → recv → FU-A reassembly → decode
    // → render pipeline without dragging in libopenh264 / libx264.
    //
    // A real codec plugin should replace this by linking x264 / OpenH264
    // and registering its own IVideoCodecFactory under a different id
    // (see EngineConfig::video_codec_name).
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

    // ---- SPS NAL (type 7) — Baseline profile, level 30 -------------------
    // 67 42 C0 1E   NAL header (67=F1,NRI=11,Type=7) + profile_idc=66 + constraints=C0 + level=30.
    // The remaining RBSP bytes (seq_parameter_set_id=0, log2_max_frame_num_minus4=0,
    // pic_order_cnt_type=0, etc.) are condensed into a fixed 1-byte marker
    // (0x01 = seq_parameter_set_id ue(0)) that the decoder stub recognises.
    // The downstream decoder must fall back on cfg.width/cfg.height — see
    // StubDecoder below.
    static constexpr std::uint8_t kSpsNal[] = {0x67, 0x42, 0xC0, 0x1E, 0x01};

    // ---- PPS NAL (type 8) — minimal 1-byte RBSP ---------------------------
    static constexpr std::uint8_t kPpsNal[] = {0x68, 0xCE, 0x38, 0x80};

    // ---- IDR slice NAL (type 5) — payload = frame_seq (LE 4 bytes) + fill -
    constexpr std::size_t kIdrPayload = 16;
    std::uint8_t idr_nal[1 + kIdrPayload];
    idr_nal[0] = 0x65;     // F=1, NRI=11, Type=5 (IDR slice)
    idr_nal[1] = 0x88;     // slice header marker (first_mb_in_slice=0)
    idr_nal[2] = 0x00;
    // Bytes 3..6: frame_seq as little-endian uint32 — lets the receiver
    // confirm that round-tripped frames arrive in order.
    const std::uint32_t seq = raw.info.frame_seq;
    idr_nal[3] = static_cast<std::uint8_t>(seq        & 0xFF);
    idr_nal[4] = static_cast<std::uint8_t>((seq >> 8) & 0xFF);
    idr_nal[5] = static_cast<std::uint8_t>((seq >> 16) & 0xFF);
    idr_nal[6] = static_cast<std::uint8_t>((seq >> 24) & 0xFF);
    // Bytes 7..15: payload fill — a pseudo-random pattern derived from the
    // frame_seq so each frame looks distinct on the wire.
    std::uint32_t seed = seq ^ 0xA5A5A5A5u;
    for (std::size_t i = 7; i < 1 + kIdrPayload; ++i) {
        seed = seed * 1664525u + 1013904223u;     // Numerical Recipes LCG
        idr_nal[i] = static_cast<std::uint8_t>(seed >> 24);
    }

    // ---- Assemble Annex B bitstream ---------------------------------------
    constexpr std::uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};
    std::size_t pos = 0;
    auto append = [&](const std::uint8_t* data, std::size_t n) -> bool {
        if (pos + n > output_capacity) return false;
        std::memcpy(output + pos, data, n);
        pos += n;
        return true;
    };
    if (!append(kStartCode, 4))               { stats_.encode_errors++; return plugins::kErrInternal; }
    if (!append(kSpsNal, sizeof(kSpsNal)))    { stats_.encode_errors++; return plugins::kErrInternal; }
    if (!append(kStartCode, 4))               { stats_.encode_errors++; return plugins::kErrInternal; }
    if (!append(kPpsNal, sizeof(kPpsNal)))    { stats_.encode_errors++; return plugins::kErrInternal; }
    if (!append(kStartCode, 4))               { stats_.encode_errors++; return plugins::kErrInternal; }
    if (!append(idr_nal, sizeof(idr_nal)))    { stats_.encode_errors++; return plugins::kErrInternal; }

    encoded_out.codec         = plugins::VideoCodecKind::kH264;
    encoded_out.payload       = core::ByteSpan{output, pos};
    encoded_out.payload_type  = cfg_.payload_type;
    encoded_out.is_keyframe   = true;     // stub always emits IDR
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
    if (output_buffers == nullptr) {
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

    // ---- Resolve output dimensions ---------------------------------------
    // The stub SPS doesn't carry a fully-decoded width/height (see
    // parse_sps() returning valid=false for our synthetic bitstream).
    // We therefore fall back on the codec config, which the encoder
    // stamped from the source's actual dimensions.
    const std::uint32_t w = decoded->width  ? decoded->width  : cfg_.width;
    const std::uint32_t h = decoded->height ? decoded->height : cfg_.height;
    if (w == 0 || h == 0) {
        // No SPS width/height and no codec-config fallback — bail.
        stats_.decode_errors++;
        return plugins::kErrCorrupt;
    }

    // ---- Propagate metadata into the caller-provided VideoFrame -----------
    raw_out.info.capture_ts_us = decoded->capture_ts_us;
    raw_out.info.frame_seq     = decoded->frame_seq;
    raw_out.info.rtp_timestamp = decoded->rtp_timestamp;

    // ---- Resolve pixel buffers --------------------------------------------
    //
    // The unified VideoFrame holds either a GpuBuffer (HW path) or a
    // VideoFrameBuffer (CPU path). For the CPU fallback we support three
    // scenarios:
    //   1. Caller supplies output_buffers AND raw_out already owns a
    //      VideoFrameBuffer whose data() matches output_buffers[0]
    //      → reuse that buffer (zero-copy), paint into the caller's memory.
    //   2. Caller supplies output_buffers but raw_out has no matching
    //      buffer → allocate a fresh VideoFrameBuffer for raw_out so its
    //      `plane()` / `format()` can be inspected, and paint into the
    //      caller's output_buffers (the caller's working memory).
    //   3. No caller buffers → allocate a fresh VideoFrameBuffer and paint
    //      into it; raw_out takes ownership.
    //
    // Zero-copy note: a real HW decoder would set `raw_out.gpu_buffer()`
    // instead and skip this CPU paint entirely.

    std::shared_ptr<video_frame::VideoFrameBuffer> cpu_buf;
    std::uint8_t* y_plane = nullptr;
    std::uint8_t* u_plane = nullptr;
    std::uint8_t* v_plane = nullptr;
    const bool caller_supplied_buffers = output_buffers && output_buffers[0];

    if (caller_supplied_buffers) {
        y_plane = output_buffers[0];
        u_plane = (cfg_.pixel_format == plugins::VideoPixelFormat::kNV12)
                      ? output_buffers[1] : output_buffers[1];
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
            // No caller buffers — reuse our own allocation for painting.
            const std::size_t y_bytes = static_cast<std::size_t>(w) * h;
            const std::size_t u_bytes = y_bytes / 4;
            y_plane = cpu_buf->data();
            u_plane = cpu_buf->data() + y_bytes;
            v_plane = cpu_buf->data() + y_bytes + u_bytes;
        }
        // else: caller_supplied_buffers == true; y/u/v_plane already point
        // at the caller's output_buffers. cpu_buf is a fresh allocation
        // owned by raw_out so its plane() returns valid pointers for
        // introspection / tests.
    }

    // ---- Paint the I420 planes with a recognisable synthetic pattern ------
    //
    // We synthesise a 3-band colour pattern (red→green→blue horizontally,
    // modulated by the frame_seq so successive frames look different on the
    // wire / on the screen).  This is enough for a human-eye demo and for
    // tests that verify "frame N painted with expected colours".
    paint_i420(y_plane, u_plane, v_plane, w, h, decoded->frame_seq);

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
                                    std::uint32_t frame_seq) noexcept {
    if (!y) return;
    // ---- Y plane: 3 horizontal bands whose brightness ramps with frame_seq
    const std::uint32_t band_h = h / 3;
    const std::uint8_t base_y  = static_cast<std::uint8_t>(16 +
        ((frame_seq * 7u) % 200u));   // BT.601 limited range
    for (std::uint32_t row = 0; row < h; ++row) {
        const std::uint32_t band = std::min<std::uint32_t>(2, row / std::max<std::uint32_t>(1u, band_h));
        const std::uint8_t yv = static_cast<std::uint8_t>(base_y + band * 60u);
        std::memset(y + row * w, yv, w);
    }
    // ---- U/V planes (BT.601 limited-range neutral grey chroma)
    const std::uint32_t half_w = w / 2;
    const std::uint32_t half_h = h / 2;
    if (u) std::memset(u, 128, half_w * half_h);
    if (v) std::memset(v, 128, half_w * half_h);
}

plugins::Status CodecPluginAdapter::update_config(plugins::VideoCodecConfig cfg) noexcept {
    cfg_ = std::move(cfg);
    return plugins::kOk;
}

plugins::VideoCodecStats CodecPluginAdapter::stats() const noexcept {
    return stats_;
}

std::uint8_t CodecPluginAdapter::payload_type() const noexcept {
    return cfg_.payload_type != 0 ? cfg_.payload_type : std::uint8_t{102};
}

// ---------------------------------------------------------------------------
// H264PluginFactory
// ---------------------------------------------------------------------------

plugins::IVideoCodec* H264PluginFactory::create(plugins::VideoCodecConfig cfg) const {
    // Delegate to the HW-first backend selector. On hosts where no HW
    // backend is available the selector returns the stub backend (which
    // this very file implements). On hosts with NVENC / VideoToolbox /
    // MediaCodec / VAAPI / etc. the out-of-tree plugin's `create` runs
    // instead, returning a real HW codec instance.
    if (auto* be = select_encoder_backend(cfg); be && be->create) {
        auto c = be->create(cfg);
        if (c) {
            const std::string hw_str = be->is_hw ? "yes" : "no";
            const std::string zc_str = be->zero_copy_supported ? "yes" : "no";
            const std::string id_str{be->id};
            const std::string be_str{plugins::hw_backend_name(be->backend)};
            core::log::Logger::instance().info(
                std::string{"h264: selected backend '"} + id_str +
                "' (" + be_str + ", hw=" + hw_str +
                ", zero_copy=" + zc_str + ")");
            return c.release();
        }
    }
    // Last-resort fallback (selector should always return at least the stub).
    return new CodecPluginAdapter(std::move(cfg));
}

// ---------------------------------------------------------------------------
// Public registration entry point
// ---------------------------------------------------------------------------

namespace detail {

void do_register_default_plugins() noexcept {
    static const struct Registrar {
        Registrar() {
            // 1. Register the H.264 plugin factory under id "h264" so
            //    EngineConfig::video_codec_name == "h264" picks it up.
            static nimrtc::h264::H264PluginFactory s_factory{};
            nimrtc::core::PluginRegistry::instance().register_video_codec(
                std::string_view{s_factory.id()}, &s_factory);
            // 2. Populate the per-platform HW backend table. The table
            //    decides (per CodecConfig) whether to use the stub, a
            //    software fallback, or a real HW encoder.
            nimrtc::h264::register_default_video_backends();
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

} // namespace nimrtc::h264
