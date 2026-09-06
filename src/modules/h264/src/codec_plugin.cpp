/**
 * @file src/modules/h264/src/codec_plugin.cpp
 * @brief CodecPluginAdapter + H264PluginFactory implementation.
 *
 * Wraps concrete h264::Decoder behind plugins::IVideoCodec.
 */

#include <nimrtc/h264/codec_plugin.hpp>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>

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

plugins::Status CodecPluginAdapter::encode(const plugins::VideoFrame& /*raw*/,
                                           std::uint8_t* /*output*/,
                                           std::size_t /*output_capacity*/,
                                           plugins::EncodedVideoFrame& /*encoded_out*/) noexcept {
    stats_.encode_errors++;
    return plugins::kErrUnsupported;   // stub is decoder-only
}

plugins::Status CodecPluginAdapter::decode(const plugins::EncodedVideoFrame& encoded,
                                           plugins::VideoFrame& raw_out,
                                           std::uint8_t* const* /*output_buffers*/) noexcept {
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

    // Propagate metadata into the caller-provided VideoFrame.
    raw_out.width         = decoded->width;
    raw_out.height        = decoded->height;
    raw_out.format        = encoded.codec == plugins::VideoCodecKind::kH264
                                ? plugins::VideoPixelFormat::kI420
                                : plugins::VideoPixelFormat::kUnknown;
    raw_out.capture_ts_us = decoded->capture_ts_us;
    raw_out.frame_seq     = decoded->frame_seq;
    raw_out.rtp_timestamp = decoded->rtp_timestamp;
    // Note: the stub produces an empty pixel buffer.  Real decoders would
    // populate raw_out.plane_y/u/v from `output_buffers`.
    return plugins::kOk;
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
    return new CodecPluginAdapter(std::move(cfg));
}

// ---------------------------------------------------------------------------
// Public registration entry point
// ---------------------------------------------------------------------------

namespace detail {

void do_register_default_plugins() noexcept {
    static const struct Registrar {
        Registrar() {
            static nimrtc::h264::H264PluginFactory s_factory{};
            nimrtc::core::PluginRegistry::instance().register_video_codec(
                std::string_view{s_factory.id()}, &s_factory);
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
