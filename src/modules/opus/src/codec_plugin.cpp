/**
 * @file src/modules/opus/src/codec_plugin.cpp
 * @brief CodecPluginAdapter + OpusPluginFactory implementation.
 *
 * Wraps concrete opus::Encoder / opus::Decoder behind plugins::ICodec.
 *
 * Per ADR-001 + MSVC static-link workaround: non-inline
 * `register_default_plugins()` forces the .obj into the consumer's link.
 */

#include <nimrtc/opus/codec_plugin.hpp>

#include <atomic>
#include <utility>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>

namespace nimrtc::opus {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

EncoderConfig
CodecPluginAdapter::to_encoder_config(const ::nimrtc::plugins::CodecConfig& p) noexcept {
    EncoderConfig c;
    c.sample_rate_hz = p.sample_rate_hz;
    c.channels       = p.channels;
    c.bitrate_bps    = p.bitrate_bps != 0 ? p.bitrate_bps : 64000;
    c.complexity     = p.complexity;
    c.fec_enabled    = p.fec_enabled;
    c.dtx_enabled    = p.dtx_enabled;
    c.vad_enabled    = p.vad_enabled;
    return c;
}

DecoderConfig
CodecPluginAdapter::to_decoder_config(const ::nimrtc::plugins::CodecConfig& p) noexcept {
    DecoderConfig c;
    c.sample_rate_hz = p.sample_rate_hz;
    c.channels       = p.channels;
    c.complexity     = p.complexity;
    return c;
}

// ---------------------------------------------------------------------------
// CodecPluginAdapter
// ---------------------------------------------------------------------------

CodecPluginAdapter::CodecPluginAdapter(::nimrtc::plugins::CodecConfig cfg)
    : enc_cfg_(to_encoder_config(cfg)),
      dec_cfg_(to_decoder_config(cfg)),
      plugin_cfg_(std::move(cfg)) {
    core::log::Logger::instance().debug(
        "opus::CodecPluginAdapter created");
}

CodecPluginAdapter::~CodecPluginAdapter() {
    // unique_ptr destruction handles encoder/decoder cleanup.
}

const char* CodecPluginAdapter::name() const noexcept {
    return "nimrtc::opus::CodecPluginAdapter (Opus encoder + decoder behind plugins::ICodec)";
}

plugins::Status CodecPluginAdapter::open() noexcept {
    if (configured_) return plugins::kOk;
    return ensure_configured();
}

plugins::Status CodecPluginAdapter::ensure_configured() noexcept {
    try {
        encoder_ = std::make_unique<Encoder>(enc_cfg_);
        decoder_ = std::make_unique<Decoder>(dec_cfg_);
        configured_ = true;
        return plugins::kOk;
    } catch (...) {
        encoder_.reset();
        decoder_.reset();
        return plugins::kErrInternal;
    }
}

void CodecPluginAdapter::close() noexcept {
    encoder_.reset();
    decoder_.reset();
    configured_ = false;
}

std::size_t CodecPluginAdapter::encode(const float* pcm,
                                       std::size_t  num_samples,
                                       std::uint8_t* output,
                                       std::size_t  output_capacity) noexcept {
    if (!pcm || !output) return 0;
    if (num_samples == 0 || output_capacity == 0) return 0;
    if (!configured_) {
        if (ensure_configured() != plugins::kOk) return 0;
    }
    return encoder_->encode(pcm, num_samples, output, output_capacity);
}

std::size_t CodecPluginAdapter::decode(const std::uint8_t* encoded,
                                       std::size_t  encoded_len,
                                       float*      output,
                                       std::size_t  output_capacity) noexcept {
    if (!encoded || !output) return 0;
    if (encoded_len == 0 || output_capacity == 0) return 0;
    if (!configured_) {
        if (ensure_configured() != plugins::kOk) return 0;
    }
    return decoder_->decode(encoded, encoded_len, output, output_capacity);
}

std::size_t CodecPluginAdapter::decode_plc(float*      output,
                                            std::size_t output_capacity) noexcept {
    if (!output || output_capacity == 0) return 0;
    if (!configured_ || !decoder_) return 0;
    return decoder_->decode_plc(output, output_capacity);
}

plugins::CodecConfig CodecPluginAdapter::config() const noexcept {
    return plugin_cfg_;
}

plugins::Status CodecPluginAdapter::update_config(plugins::CodecConfig cfg) noexcept {
    plugin_cfg_ = std::move(cfg);
    enc_cfg_ = to_encoder_config(plugin_cfg_);
    dec_cfg_ = to_decoder_config(plugin_cfg_);
    // Reconstruct encoder/decoder with new config.
    encoder_ = std::make_unique<Encoder>(enc_cfg_);
    decoder_ = std::make_unique<Decoder>(dec_cfg_);
    return plugins::kOk;
}

plugins::CodecStats CodecPluginAdapter::snapshot_stats() const noexcept {
    plugins::CodecStats s;
    if (encoder_) {
        auto e = encoder_->stats();
        s.frames_encoded = e.frames_encoded;
        s.bytes_encoded  = e.bytes_encoded;
        s.dtx_frames     = e.dtx_frames;
        s.plc_frames     = e.plc_frames;
    }
    // Decoder stats are not exposed in opus::Decoder::Stats (encoder-only);
    // we leave decode side at 0 for the plugin view, matching what
    // the concrete interface provides. Future: plumb Decoder stats
    // through when opus::Decoder exposes them.
    return s;
}

plugins::CodecStats CodecPluginAdapter::stats() const noexcept {
    return snapshot_stats();
}

std::string_view CodecPluginAdapter::codec_name() const noexcept {
    return "opus";
}

std::uint8_t CodecPluginAdapter::payload_type() const noexcept {
    return plugin_cfg_.payload_type != 0 ? plugin_cfg_.payload_type
                                          : std::uint8_t{111};  // RFC 7587
}

// ---------------------------------------------------------------------------
// OpusPluginFactory
// ---------------------------------------------------------------------------

std::string_view OpusPluginFactory::id() const noexcept {
    return "opus";   // matches EngineConfig::codec_name default
}

std::string_view OpusPluginFactory::display_name() const noexcept {
    return "Opus codec (RFC 6716) — encoder + decoder, 48 kHz mono default";
}

plugins::ICodec* OpusPluginFactory::create(::nimrtc::plugins::CodecConfig cfg) const {
    return new CodecPluginAdapter(std::move(cfg));
}

// ---------------------------------------------------------------------------
// Public registration entry point
// ---------------------------------------------------------------------------

namespace detail {

void do_register_default_plugins() noexcept {
    // static ⇒ address stable for process lifetime; runs once at first call.
    static const struct Registrar {
        Registrar() {
            static nimrtc::opus::OpusPluginFactory s_factory{};
            nimrtc::core::PluginRegistry::instance().register_codec(
                std::string_view{s_factory.id()}, &s_factory);
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

// Non-inline (declared in codec_plugin.hpp) so the symbol is guaranteed
// in nimrtc_opus.lib for consumers that link via static lib + PluginRegistry.
void register_default_plugins() noexcept {
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::opus
