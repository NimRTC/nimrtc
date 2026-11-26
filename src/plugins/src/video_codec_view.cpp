/**
 * @file src/plugins/src/video_codec_view.cpp
 * @brief VideoCodecView — implementation.
 *
 * See nimrtc/plugins/video_codec_view.hpp for the rationale.
 */

#include <nimrtc/plugins/video_codec_view.hpp>

namespace nimrtc::plugins {

NIMRTC_SUPPRESS_DEPRECATION_BEGIN

VideoCodecView::VideoCodecView(IVideoEncoder* encoder,
                               IVideoDecoder* decoder,
                               VideoCodecConfig cfg) noexcept
    : encoder_(encoder), decoder_(decoder), cfg_(std::move(cfg)) {}

VideoCodecView::~VideoCodecView() = default;

const char* VideoCodecView::name() const noexcept {
    return "nimrtc::plugins::VideoCodecView (borrowed encoder + decoder view)";
}

Status VideoCodecView::open() noexcept {
    // The view owns nothing, so it opens nothing: the halves were
    // already opened by whoever created them.  Opening them *again*
    // would be wrong — `IVideoEncoder::open()` is not required to be
    // idempotent — and the view is not in a position to report a
    // half-open state.
    return kOk;
}

void VideoCodecView::close() noexcept {
    // Symmetrically: the view closes nothing.  The owner of the halves
    // closes them.
}

bool VideoCodecView::can_encode() const noexcept {
    return encoder_ != nullptr;
}

bool VideoCodecView::can_decode() const noexcept {
    return decoder_ != nullptr;
}

VideoCodecKind VideoCodecView::kind() const noexcept {
    if (encoder_) return encoder_->kind();
    if (decoder_) return decoder_->kind();
    return cfg_.codec;
}

std::string_view VideoCodecView::codec_name() const noexcept {
    if (encoder_) return encoder_->codec_name();
    if (decoder_) return decoder_->codec_name();
    return cfg_.name;
}

Status VideoCodecView::encode(const VideoFrame& raw,
                              std::uint8_t* output,
                              std::size_t output_capacity,
                              EncodedVideoFrame& encoded_out) noexcept {
    if (!encoder_) return kErrUnsupported;
    return encoder_->encode(raw, output, output_capacity, encoded_out);
}

Status VideoCodecView::decode(const EncodedVideoFrame& encoded,
                              VideoFrame& raw_out,
                              std::uint8_t* const* output_buffers) noexcept {
    if (!decoder_) return kErrUnsupported;
    return decoder_->decode(encoded, raw_out, output_buffers);
}

Status VideoCodecView::force_keyframe() noexcept {
    if (!encoder_) return kErrUnsupported;
    return encoder_->force_keyframe();
}

Status VideoCodecView::set_params(const VideoEncoderParams& params) noexcept {
    if (!encoder_) return kErrUnsupported;
    return encoder_->set_params(params);
}

bool VideoCodecView::supports_zero_copy_encode() const noexcept {
    return encoder_ && encoder_->supports_zero_copy_encode();
}

bool VideoCodecView::supports_zero_copy_decode() const noexcept {
    return decoder_ && decoder_->supports_zero_copy_decode();
}

Status VideoCodecView::encode_zero_copy(const GpuBuffer& gpu,
                                        EncodedVideoFrame& encoded_out) noexcept {
    if (!encoder_) return kErrUnsupported;
    return encoder_->encode_zero_copy(gpu, encoded_out);
}

Status VideoCodecView::decode_zero_copy(const EncodedVideoFrame& encoded,
                                        GpuBuffer& out_surface) noexcept {
    if (!decoder_) return kErrUnsupported;
    return decoder_->decode_zero_copy(encoded, out_surface);
}

VideoCodecConfig VideoCodecView::config() const noexcept {
    if (encoder_) return encoder_->config();
    if (decoder_) return decoder_->config();
    return cfg_;
}

Status VideoCodecView::update_config(VideoCodecConfig cfg) noexcept {
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

VideoCodecStats VideoCodecView::stats() const noexcept {
    VideoCodecStats out{};
    if (encoder_) {
        const auto s = encoder_->stats();
        out.frames_encoded = s.frames_encoded;
        out.bytes_encoded  = s.bytes_encoded;
        out.encode_errors  = s.encode_errors;
        out.frames_skipped += s.frames_skipped;
        out.frames_encoded_staged    = s.frames_encoded_staged;
        out.frames_encoded_zero_copy = s.frames_encoded_zero_copy;
        out.keyframes_encoded       = s.keyframes_encoded;
        out.bytes_copied             = s.bytes_copied;
        out.encode_latency_us_total  = s.encode_latency_us_total;
        out.encode_latency_us_max    = s.encode_latency_us_max;

        // The telemetry fields describe the *outbound* stream, so they
        // travel with the encoder's snapshot.
        out.backend_id             = s.backend_id;
        out.hardware               = s.hardware;
        out.zero_copy              = s.zero_copy;
        out.configured_bitrate_bps = s.configured_bitrate_bps;
        out.actual_bitrate_bps     = s.actual_bitrate_bps;
        out.avg_encode_latency_us  = s.avg_encode_latency_us;
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

std::uint8_t VideoCodecView::payload_type() const noexcept {
    // The payload type describes the *outbound* stream, so the encoder
    // half owns it.
    if (encoder_) return encoder_->payload_type();
    return cfg_.payload_type != 0 ? cfg_.payload_type : std::uint8_t{102};
}

std::string_view VideoCodecView::backend_id() const noexcept {
    if (encoder_) return encoder_->backend_id();
    if (decoder_) return decoder_->backend_id();
    return "unknown";
}

bool VideoCodecView::is_hardware_accelerated() const noexcept {
    if (encoder_) return encoder_->is_hardware_accelerated();
    if (decoder_) return decoder_->is_hardware_accelerated();
    return false;
}

NIMRTC_SUPPRESS_DEPRECATION_END

} // namespace nimrtc::plugins
