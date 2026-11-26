/**
 * @file src/plugins/src/hw_composite_codec.cpp
 * @brief HwCompositeCodec — implementation.
 *
 * See nimrtc/plugins/hw_composite_codec.hpp for the rationale.
 */

#include <nimrtc/plugins/hw_composite_codec.hpp>

#include <nimrtc/core/log.hpp>

namespace nimrtc::plugins {

NIMRTC_SUPPRESS_DEPRECATION_BEGIN

HwCompositeCodec::HwCompositeCodec(VideoCodecConfig cfg,
                                   std::unique_ptr<IVideoEncoder> encoder,
                                   std::unique_ptr<IVideoDecoder> decoder) noexcept
    : cfg_(std::move(cfg)),
      encoder_(std::move(encoder)),
      decoder_(std::move(decoder)) {}

HwCompositeCodec::~HwCompositeCodec() {
    close();
}

const char* HwCompositeCodec::name() const noexcept {
    return "nimrtc::plugins::HwCompositeCodec (hardware encode + hardware decode)";
}

Status HwCompositeCodec::open() noexcept {
    if (opened_) return kOk;

    // The encoder goes first: it owns the GPU encode session, and
    // failing fast on it keeps the error message about the encoder.
    if (encoder_) {
        const Status s = encoder_->open();
        if (s != kOk) {
            NIMRTC_LOG_ERROR("plugins: composite encoder open() failed (status="
                             << unsigned(s) << ")");
            return s;
        }
    }

    if (decoder_) {
        const Status s = decoder_->open();
        if (s != kOk) {
            NIMRTC_LOG_ERROR("plugins: composite decoder open() failed (status="
                             << unsigned(s) << ")");
            // Do not leave the encoder's hardware session dangling.
            if (encoder_) encoder_->close();
            return s;
        }
    }

    opened_ = true;
    return kOk;
}

void HwCompositeCodec::close() noexcept {
    if (decoder_) decoder_->close();
    if (encoder_) encoder_->close();
    opened_ = false;
}

bool HwCompositeCodec::can_encode() const noexcept {
    // The encoder half is present *and* usable.  `IVideoEncoder` has no
    // `can_encode()` of its own -- the direction is in the type -- so
    // the question reduces to "is there an encoder half at all?".
    return encoder_ != nullptr;
}

bool HwCompositeCodec::can_decode() const noexcept {
    // Same for the decode half.
    return decoder_ != nullptr;
}

VideoCodecKind HwCompositeCodec::kind() const noexcept {
    // The encoder describes the outbound stream, which is the direction
    // the composite is nominally "for"; the decoder is the fallback for
    // a receive-only composite.
    if (encoder_) return encoder_->kind();
    if (decoder_) return decoder_->kind();
    return cfg_.codec;
}

std::string_view HwCompositeCodec::codec_name() const noexcept {
    if (encoder_) return encoder_->codec_name();
    if (decoder_) return decoder_->codec_name();
    return cfg_.name;
}

Status HwCompositeCodec::encode(const VideoFrame& raw,
                                std::uint8_t* output,
                                std::size_t output_capacity,
                                EncodedVideoFrame& encoded_out) noexcept {
    if (!encoder_) return kErrUnsupported;
    return encoder_->encode(raw, output, output_capacity, encoded_out);
}

Status HwCompositeCodec::decode(const EncodedVideoFrame& encoded,
                                VideoFrame& raw_out,
                                std::uint8_t* const* output_buffers) noexcept {
    if (!decoder_) return kErrUnsupported;
    return decoder_->decode(encoded, raw_out, output_buffers);
}

Status HwCompositeCodec::force_keyframe() noexcept {
    if (!encoder_) return kErrUnsupported;
    return encoder_->force_keyframe();
}

bool HwCompositeCodec::supports_zero_copy_encode() const noexcept {
    return encoder_ && encoder_->supports_zero_copy_encode();
}

bool HwCompositeCodec::supports_zero_copy_decode() const noexcept {
    return decoder_ && decoder_->supports_zero_copy_decode();
}

Status HwCompositeCodec::encode_zero_copy(
        const GpuBuffer& gpu,
        EncodedVideoFrame& encoded_out) noexcept {
    if (!encoder_) return kErrUnsupported;
    return encoder_->encode_zero_copy(gpu, encoded_out);
}

Status HwCompositeCodec::decode_zero_copy(
        const EncodedVideoFrame& encoded,
        GpuBuffer& out_surface) noexcept {
    if (!decoder_) return kErrUnsupported;
    return decoder_->decode_zero_copy(encoded, out_surface);
}

VideoCodecConfig HwCompositeCodec::config() const noexcept {
    // The encoder's configuration is the one that describes the outbound
    // stream, which is what a caller inspecting `config()` wants.
    if (encoder_) return encoder_->config();
    if (decoder_) return decoder_->config();
    return cfg_;
}

Status HwCompositeCodec::update_config(VideoCodecConfig cfg) noexcept {
    cfg_ = cfg;

    Status status = kOk;

    if (encoder_) {
        status = encoder_->update_config(cfg);
        if (status != kOk) return status;
    }

    // The decoder's width/height are an *upper bound* on the picture it
    // may receive — the real geometry comes from the bitstream's
    // parameter sets.  Forwarding the encoder's frame size would
    // therefore narrow the decoder's bound every time the local capture
    // resolution changed, and a remote peer sending a larger picture
    // would be rejected.  Only the non-geometric knobs are forwarded.
    if (decoder_) {
        VideoCodecConfig dcfg = decoder_->config();
        dcfg.fps               = cfg.fps;
        dcfg.bitrate_bps       = cfg.bitrate_bps;
        dcfg.keyframe_interval = cfg.keyframe_interval;
        dcfg.payload_type      = cfg.payload_type;
        dcfg.codec             = cfg.codec;
        dcfg.pixel_format      = cfg.pixel_format;
        const Status s = decoder_->update_config(dcfg);
        // A decoder that refuses the change is **normal**, not a
        // failure: its geometry comes from the bitstream, so the seam's
        // default answer is `kErrUnsupported` (a DXVA decoder, for
        // instance, has no live-configuration hook at all).  The
        // *encoder* half is the one the caller asked to change, and it
        // already answered — so the decoder's refusal must not turn a
        // successful re-configuration into an error.  Only a decoder
        // that fails for a *different* reason (a hardware error, say)
        // is worth reporting.
        if (s != kOk && s != kErrUnsupported && status == kOk) status = s;
    }

    return status;
}

VideoCodecStats HwCompositeCodec::stats() const noexcept {
    VideoCodecStats out{};
    if (encoder_) {
        const auto s = encoder_->stats();
        out.frames_encoded = s.frames_encoded;
        out.bytes_encoded  = s.bytes_encoded;
        out.encode_errors  = s.encode_errors;
        out.frames_skipped += s.frames_skipped;
        // The staged / zero-copy split is what tells a host *how* the
        // picture reached the encoder.  A composite that dropped it
        // would report "0 frames staged, 0 frames zero-copy" for every
        // hardware session — which reads as "the encoder never ran".
        out.frames_encoded_staged    = s.frames_encoded_staged;
        out.frames_encoded_zero_copy = s.frames_encoded_zero_copy;
        out.keyframes_encoded       = s.keyframes_encoded;
        out.bytes_copied             = s.bytes_copied;
        out.encode_latency_us_total  = s.encode_latency_us_total;
        out.encode_latency_us_max    = s.encode_latency_us_max;

        // Remote-desktop plan B15: the telemetry fields travel with the
        // encoder's snapshot — they describe the *outbound* stream.
        out.backend_id             = s.backend_id;
        out.hardware               = s.hardware;
        out.zero_copy              = s.zero_copy;
        out.configured_bitrate_bps = s.configured_bitrate_bps;
        out.actual_bitrate_bps     = s.actual_bitrate_bps;
        out.avg_encode_latency_us  = s.avg_encode_latency_us;
        out.keyframes_encoded      = s.keyframes_encoded;
        out.zero_copy_frames       = s.zero_copy_frames;
        out.staged_frames          = s.staged_frames;
        out.queue_drops            = s.queue_drops;
    }
    if (decoder_) {
        const auto s = decoder_->stats();
        out.frames_decoded = s.frames_decoded;
        out.bytes_decoded  = s.bytes_decoded;
        out.decode_errors  = s.decode_errors;
    }
    return out;
}

std::uint8_t HwCompositeCodec::payload_type() const noexcept {
    // The payload type describes the *outbound* stream, so the encoder
    // half owns it.  A receive-only composite has no encoder half and
    // falls back to the configured value.
    if (encoder_) return encoder_->payload_type();
    return cfg_.payload_type != 0 ? cfg_.payload_type : std::uint8_t{102};
}

std::string_view HwCompositeCodec::backend_id() const noexcept {
    // The encoder first, for the same reason `kind()` and `codec_name()`
    // prefer it: the engine's codec telemetry describes the outbound
    // stream.  A receive-only composite (encoder == nullptr) falls back
    // to the decoder.
    if (encoder_) return encoder_->backend_id();
    if (decoder_) return decoder_->backend_id();
    return "unknown";
}

bool HwCompositeCodec::is_hardware_accelerated() const noexcept {
    // The *encoder* is the direction the engine's stats describe (and
    // the direction §B.6's send-side budget is written against).  A
    // composite with a hardware decoder and a software encoder is a
    // software *send* path, and reporting it as accelerated would
    // overstate what the machine is doing.
    if (encoder_) return encoder_->is_hardware_accelerated();
    if (decoder_) return decoder_->is_hardware_accelerated();
    return false;
}

NIMRTC_SUPPRESS_DEPRECATION_END

} // namespace nimrtc::plugins
