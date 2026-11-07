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

#include "audio_decoder_plugin.hpp"

#include <atomic>
#include <cstring>
#include <utility>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/plugin_id.hpp>
#include <nimrtc/core/registry.hpp>

namespace nimrtc::opus {

namespace {

/** Maximum number of Opus frames one RFC 7587 payload may carry
 *  (RFC 6716 §3.2.5 [R5]: 120 ms at the shortest 2.5 ms frame). */
constexpr std::size_t kMaxFramesPerPacket = 48;

} // anonymous namespace

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
    // The in-band redundancy (spec B-8).  Without it the encoder never
    // embeds the low-bitrate copy of the previous frame, and the
    // receiver has nothing to reconstruct a lost frame from.
    c.packet_loss_perc = p.packet_loss_perc < 0 ? 0
                             : (p.packet_loss_perc > 100 ? 100 : p.packet_loss_perc);
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

    // ---- Step 1: encode into the module's own buffer ------------------
    //
    // The buffer has to hold the TOC byte libopus writes plus the
    // frame's body, so it is one byte larger than the largest frame
    // libopus can produce (1275 bytes, RFC 6716 §3.2.1).
    enc_scratch_.resize(kMaxOpusFrameBytes + 1);
    const std::size_t encoded = encoder_->encode(pcm, num_samples,
                                                  enc_scratch_.data(),
                                                  enc_scratch_.size());
    if (encoded == 0) return 0;

    // ---- Step 2: frame it as RFC 7587 ----------------------------------
    //
    // The engine must not have to know how Opus frames its output, and
    // the wire format must be RFC 7587 by construction rather than by
    // luck (spec B-5 / defect F-14).  `packetise()` reads the frame's
    // own TOC — so the config field on the wire is the one the encoder
    // actually used, never one the packetiser invented.
    CodecFrame frame{enc_scratch_.data(), encoded};
    return packetise(&frame, 1, enc_cfg_.sample_rate_hz, /*cbr=*/false,
                     output, output_capacity);
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

    // The payload is in the RFC 7587 framing, so it is depacketised
    // first; every frame is then decoded on its own.
    PacketView views[kMaxFramesPerPacket];
    std::size_t num_views = 0;
    if (depacketise(encoded, encoded_len, views, kMaxFramesPerPacket,
                    &num_views) == 0 || num_views == 0) {
        return 0;
    }

    const std::size_t channels = dec_cfg_.channels ? dec_cfg_.channels : 1;
    std::size_t written = 0;
    for (std::size_t i = 0; i < num_views; ++i) {
        if (written >= output_capacity) break;

        // Rebuild the complete Opus packet: the depacketiser strips the
        // TOC, libopus needs it (RFC 6716 §3.1).
        dec_packet_scratch_.resize(views[i].size + 1);
        dec_packet_scratch_[0] = make_toc_byte(views[i].config, views[i].s, 0);
        if (views[i].size > 0) {
            std::memcpy(dec_packet_scratch_.data() + 1, views[i].data, views[i].size);
        }

        const std::size_t n = decoder_->decode(
            dec_packet_scratch_.data(), dec_packet_scratch_.size(),
            output + written * channels, output_capacity - written);
        if (n == 0) break;
        written += n;
    }
    return written;
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
    return NIMRTC_PLUGIN_ID("opus");   // matches EngineConfig::codec_name default
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

    // The audio pipeline's decoder seam (v1.2.0, WP-2): the same "opus"
    // id, but in the registry's audio-decoder category.
    do_register_audio_decoder_plugin();
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
