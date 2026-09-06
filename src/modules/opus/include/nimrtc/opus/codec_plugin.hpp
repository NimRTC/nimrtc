/**
 * @file nimrtc/opus/codec_plugin.hpp
 * @brief Codec plugin adapter: wraps nimrtc::opus::Encoder / Decoder behind plugins::ICodec.
 *
 * Implements the codec plugin interface defined in <nimrtc/plugins/codec.hpp>
 * by delegating to the concrete opus module's `Encoder` + `Decoder` classes.
 *
 * ## Type conversion
 *
 * `plugins::CodecConfig` is split into the concrete module's
 * `opus::EncoderConfig` + `opus::DecoderConfig` (opus has separate structs).
 * Adapter stores both, keeps them in sync via `update_config()`.
 *
 * ## Lifetime
 *
 * The adapter owns the concrete `Encoder` and `Decoder`. Both are
 * constructed in `open()`, destroyed in `close()` and the adapter's
 * destructor.
 *
 * ## Registration
 *
 * Registered as id "opus" (matches EngineConfig::codec_name default).
 * Per ADR-001; consumer MUST call `nimrtc::opus::register_default_plugins()`
 * once at startup (same MSVC static-link workaround as the other modules).
 *
 * @note P2 R2-Batch1. Replaces the engine's direct `make_unique<opus::Encoder>`
 *       instantiation with registry lookup.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include <nimrtc/opus/opus.hpp>          // concrete opus::Encoder + Decoder
#include <nimrtc/plugins/codec.hpp>      // plugins::ICodec / ICodecFactory / CodecConfig

namespace nimrtc::opus {

// ---------------------------------------------------------------------------
// CodecPluginAdapter
// ---------------------------------------------------------------------------

/**
 * @brief Wraps a concrete opus::Encoder + opus::Decoder behind plugins::ICodec.
 *
 * One adapter per peer-connection / stream. Caller must call open() with
 * a valid CodecConfig (or with sample_rate/channels defaulted) before
 * encode()/decode().
 */
class CodecPluginAdapter : public plugins::ICodec {
public:
    explicit CodecPluginAdapter(plugins::CodecConfig cfg);
    ~CodecPluginAdapter() override;

    CodecPluginAdapter(const CodecPluginAdapter&)            = delete;
    CodecPluginAdapter& operator=(const CodecPluginAdapter&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------

    const char* name() const noexcept override;
    plugins::Status open() noexcept override;
    void          close() noexcept override;

    // ---- plugins::ICodec ---------------------------------------------------

    std::size_t encode(const float* pcm,
                       std::size_t  num_samples,
                       std::uint8_t* output,
                       std::size_t  output_capacity) noexcept override;

    std::size_t decode(const std::uint8_t* encoded,
                       std::size_t  encoded_len,
                       float*      output,
                       std::size_t  output_capacity) noexcept override;

    std::size_t decode_plc(float*      output,
                            std::size_t output_capacity) noexcept override;

    plugins::CodecConfig config() const noexcept override;
    plugins::Status update_config(plugins::CodecConfig cfg) noexcept override;
    plugins::CodecStats stats() const noexcept override;
    std::string_view codec_name() const noexcept override;
    std::uint8_t    payload_type() const noexcept override;

private:
    /** Concrete-side configuration (split because opus has separate structs). */
    EncoderConfig enc_cfg_{};
    DecoderConfig dec_cfg_{};

    /** Plugin-side config (mirrors enc_cfg_/dec_cfg_, retained for diagnostics). */
    plugins::CodecConfig plugin_cfg_{};

    /** Concrete encoder / decoder, constructed lazily on first encode/decode. */
    std::unique_ptr<Encoder> encoder_;
    std::unique_ptr<Decoder> decoder_;

    /** Whether open() has been called and encoder/decoder were constructed. */
    bool configured_ = false;

    /** Translate plugins::CodecConfig → opus::EncoderConfig. */
    static EncoderConfig to_encoder_config(const plugins::CodecConfig& p) noexcept;
    /** Translate plugins::CodecConfig → opus::DecoderConfig. */
    static DecoderConfig to_decoder_config(const plugins::CodecConfig& p) noexcept;

    /** Lazy construction: builds encoder/decoder if not yet done. */
    plugins::Status ensure_configured() noexcept;

    /** Snapshot concrete stats into plugin-side CodecStats. */
    plugins::CodecStats snapshot_stats() const noexcept;
};

// ---------------------------------------------------------------------------
// OpusPluginFactory
// ---------------------------------------------------------------------------

/**
 * @brief Factory producing CodecPluginAdapter instances.
 *
 * Registered as id "opus" (matches EngineConfig::codec_name default).
 * The codec implementation is the bundled libopus wrapper; users wanting
 * a different codec register their own ICodecFactory and set
 * cfg.codec_name accordingly.
 */
class OpusPluginFactory : public plugins::ICodecFactory {
public:
    std::string_view id()           const noexcept override;
    std::string_view display_name() const noexcept override;
    plugins::ICodec* create(::nimrtc::plugins::CodecConfig cfg) const override;
};

// ---------------------------------------------------------------------------
// Public registration entry point (MSVC static-link workaround)
// ---------------------------------------------------------------------------

namespace detail {
/** Defined in codec_plugin.cpp. Forces .obj linkage on consumer call. */
void do_register_default_plugins() noexcept;
} // namespace detail

/**
 * @brief Register all built-in opus codec plugins with core::PluginRegistry.
 *
 * @note Not `inline` because the static-local latch would otherwise be
 *       emitted as a weak external symbol that the static lib doesn't
 *       carry; non-inline ensures the symbol is in `nimrtc_opus.lib`.
 */
void register_default_plugins() noexcept;

} // namespace nimrtc::opus
