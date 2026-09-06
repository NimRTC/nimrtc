/**
 * @file nimrtc/plugins/codec.hpp
 * @brief ICodec — pluggable audio/video codec interface.
 *
 * Per ARCHITECTURE.md §2.3: nimrtc_codec is the **codec injection seam** —
 * encoder/decoder implementations are provided by the host application or
 * a plugin, NOT bundled into NimRTC's core. §1.4 (non-goals) explicitly
 * states NimRTC does not bundle third-party codecs.
 *
 * The shipped `nimrtc_opus` library is one such plugin implementation
 * (libopus encoder/decoder wrapped behind this interface), registered
 * under id `"opus"`. Users can register their own codec by:
 *
 * @code
 * class MyCodec : public plugins::ICodec { ... };
 * class MyCodecFactory : public plugins::ICodecFactory { ... };
 * static nimrtc::core::PluginRegistry::instance()
 *     .register_codec("my_codec", &factory);
 * @endcode
 *
 * and selecting it at engine open:
 * @code
 * cfg.codec_name = "my_codec";
 * @endcode
 *
 * ## Type identity vs the concrete opus::Encoder / opus::Decoder
 *
 * The concrete opus module exposes `Encoder` and `Decoder` separately.
 * ICodec merges them into one factory that produces a single object
 * capable of encoding AND decoding — this matches how the engine
 * uses the codec (one peer-connection = one encoder + one decoder).
 *
 * If a user needs asymmetric encoding/decoding (e.g. send-only), they
 * can construct two separate `ICodec` instances (one per direction).
 *
 * @note P2 R2-Batch1. Interface is stable; binary layout TBD P3.
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_CODEC_HPP
#define NIMRTC_PLUGINS_CODEC_HPP

#include <cstdint>
#include <cstddef>
#include <string_view>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Codec configuration (mirrors concrete opus::EncoderConfig / DecoderConfig
// union; codec implementations translate as needed)
// ---------------------------------------------------------------------------

struct CodecConfig {
    /** Sample rate in Hz (48000 for Opus, 8000 for G.711, etc.). */
    std::uint32_t sample_rate_hz = 48000;

    /** Number of channels (1 = mono, 2 = stereo). */
    std::uint8_t  channels = 1;

    /** Target bitrate in bits per second (0 = codec default). */
    int           bitrate_bps = 0;

    /** Codec complexity 0-10, 10 = highest quality (Opus convention). */
    int           complexity = 10;

    /** Whether forward error correction is enabled (Opus FEC / Silk). */
    bool          fec_enabled = false;

    /** Whether discontinuous transmission is enabled (DTX). */
    bool          dtx_enabled = false;

    /** Whether voice activity detection is enabled. */
    bool          vad_enabled = false;

    /** RTP payload type for the codec (e.g. 111 for Opus, 0 for PCMU). */
    std::uint8_t  payload_type = 0;

    /** Human-readable codec name (e.g. "opus", "g711a"). */
    std::string_view name;
};

// ---------------------------------------------------------------------------
// Codec statistics
// ---------------------------------------------------------------------------

struct CodecStats {
    /** Total frames successfully encoded. */
    std::uint64_t frames_encoded = 0;
    /** Total bytes of encoded payload output. */
    std::uint64_t bytes_encoded = 0;
    /** DTX frames (silence suppression). */
    std::uint64_t dtx_frames = 0;
    /** PLC frames generated to conceal packet loss. */
    std::uint64_t plc_frames = 0;
    /** Total frames successfully decoded. */
    std::uint64_t frames_decoded = 0;
    /** Total samples output from decoder (per channel). */
    std::uint64_t samples_decoded = 0;
    /** Decode errors (malformed packet, codec error). */
    std::uint64_t decode_errors = 0;
};

// ---------------------------------------------------------------------------
// ICodec
// ---------------------------------------------------------------------------

/**
 * @brief Encoder + decoder for one codec implementation.
 *
 * One `ICodec` instance owns the encoder state and decoder state for one
 * peer-connection / one stream. Thread-safety: NOT thread-safe; caller
 * must serialize encode() and decode() calls (engine does this).
 */
class ICodec : public IPlugin {
public:
    /**
     * @brief Encode one frame of PCM to the codec's compressed format.
     *
     * @param pcm            Interleaved PCM samples (float, [-1.0, 1.0]).
     * @param num_samples    Samples per channel (NOT total samples).
     * @param output         Caller-owned buffer for compressed output.
     * @param output_capacity Capacity of @p output in bytes.
     * @return Number of bytes written to @p output; 0 on DTX or error.
     */
    virtual std::size_t encode(const float* pcm,
                               std::size_t  num_samples,
                               std::uint8_t* output,
                               std::size_t  output_capacity) noexcept = 0;

    /**
     * @brief Decode one compressed frame to PCM.
     *
     * @param encoded       Compressed payload.
     * @param encoded_len   Length of compressed payload in bytes.
     * @param output        Caller-owned PCM output buffer (float).
     * @param output_capacity Capacity of @p output in float samples (per channel).
     * @return Number of PCM samples per channel decoded; 0 on error.
     */
    virtual std::size_t decode(const std::uint8_t* encoded,
                               std::size_t  encoded_len,
                               float*      output,
                               std::size_t  output_capacity) noexcept = 0;

    /** Generate a PLC frame to conceal a missing packet (optional). */
    virtual std::size_t decode_plc(float*     output,
                                   std::size_t output_capacity) noexcept = 0;

    /** Get current configuration (for diagnostics / logs). */
    virtual CodecConfig config() const noexcept = 0;

    /** Update configuration live (e.g. adaptive bitrate change). */
    virtual Status update_config(CodecConfig cfg) noexcept = 0;

    /** Snapshot of codec statistics. */
    virtual CodecStats stats() const noexcept = 0;

    /** Human-readable codec name (e.g. "opus"). */
    virtual std::string_view codec_name() const noexcept = 0;

    /** RTP payload type this codec emits (mirrors config().payload_type). */
    virtual std::uint8_t    payload_type() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

class ICodecFactory {
public:
    virtual ~ICodecFactory() = default;
    virtual std::string_view id()           const noexcept = 0;
    virtual std::string_view display_name() const noexcept = 0;

    /**
     * @brief Create a codec instance with the given configuration.
     *
     * The returned ICodec is in kConstructed state; caller must call
     * `open()` before encode()/decode(). Engine's open() does this.
     */
    virtual ICodec* create(CodecConfig cfg) const = 0;
};

template<class T>
class SimpleCodecFactory : public ICodecFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleCodecFactory(std::string_view id,
                                std::string_view name) noexcept
        : id_(id), name_(name) {}

    std::string_view id()           const noexcept override { return id_; }
    std::string_view display_name() const noexcept override { return name_; }

    ICodec* create(CodecConfig cfg) const override {
        return new T(std::move(cfg));
    }
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_CODEC_HPP
