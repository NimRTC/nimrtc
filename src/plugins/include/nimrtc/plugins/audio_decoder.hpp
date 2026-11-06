/**
 * @file nimrtc/plugins/audio_decoder.hpp
 * @brief IAudioDecoder — the pluggable audio *decoder* seam.
 *
 * `ICodec` (nimrtc/plugins/codec.hpp) is a published STABLE interface
 * whose `decode()` shape is frozen; the audio pipeline therefore gets
 * its own decoder interface instead of extending it (spec DEC-1,
 * `docs/plan/audio-pipeline-spec.md` B-1).
 *
 * The seam is **payload-level**: one call carries one complete RTP
 * payload and may yield several decoded frames. The codec-specific
 * framing (RFC 7587 for Opus) lives inside the implementation, so a
 * future G.711 decoder can reuse the interface without inheriting Opus
 * framing.
 *
 * ## Implementing a custom audio decoder
 *
 * 1. Implement `IAudioDecoder`.
 * 2. Register: `PluginRegistry::instance().register_audio_decoder("my_decoder", &factory);`
 * 3. Select it at engine open: `cfg.audio_decoder_name = "my_decoder";`
 *
 * @note v1.2.0 (audio pipeline, spec B-1). Not thread-safe — the engine
 * serialises decoder calls on the engine thread.
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include <nimrtc/core/api_version.hpp>
#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_AUDIO_DECODER_HPP
#define NIMRTC_PLUGINS_AUDIO_DECODER_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Format
// ---------------------------------------------------------------------------

/** Format the decoder is asked to produce. */
struct NIMRTC_API_STABLE AudioDecodeFormat {
    /** Decoder output rate in Hz (Opus decodes at 48000). */
    std::uint32_t sample_rate_hz = 48000;
    /** 1 = mono, 2 = stereo. */
    std::uint8_t  channels = 1;
    /** RTP payload type of the stream (RFC 7587 Opus PT = 111). */
    std::uint8_t  payload_type = 111;
    /** Honour Opus in-band FEC. */
    bool          use_inband_fec = true;
    /** The peer signalled `usedtx=1`. */
    bool          use_dtx = false;
    /** In-band FEC redundancy percentage, 0..100. */
    int           packet_loss_perc = 5;
};

// ---------------------------------------------------------------------------
// Decoded frame
// ---------------------------------------------------------------------------

/** One decoded frame. PCM is interleaved, `samples_per_channel` per channel. */
struct NIMRTC_API_STABLE AudioDecodedFrame {
    /** Interleaved float PCM; owned by the decoder, valid until the next call. */
    const float*  pcm = nullptr;
    /** Number of samples per channel. */
    std::size_t   samples_per_channel = 0;
    /** Sample rate of `pcm`. */
    std::uint32_t sample_rate_hz = 48000;
    /** Channel count of `pcm`. */
    std::uint8_t  channels = 1;
    /** RTP timestamp of the first sample. */
    std::uint32_t rtp_timestamp = 0;
    /** Media duration of this frame in milliseconds. */
    std::uint16_t duration_ms = 0;
    /** The frame was produced by packet-loss concealment. */
    bool          concealed = false;
    /** The frame was reconstructed from in-band FEC. */
    bool          fec_recovered = false;
};

// ---------------------------------------------------------------------------
// IAudioDecoder
// ---------------------------------------------------------------------------

/**
 * @brief Decoder for one audio codec.
 *
 * One instance owns the decoder state for one stream. `decode_packet()`
 * never conceals — concealment is the pipeline's job.
 */
class NIMRTC_API_STABLE IAudioDecoder : public IPlugin {
public:
    /** The format the decoder currently produces. */
    virtual AudioDecodeFormat format() const noexcept = 0;

    /**
     * @brief Update the negotiated parameters (channels, FEC behaviour).
     *
     * Called when the remote SDP changes the negotiated parameters; a
     * decoder must be able to switch without being recreated.
     */
    virtual Status update_format(AudioDecodeFormat f) noexcept = 0;

    /**
     * @brief Decode one complete RTP payload.
     *
     * @param payload       RTP payload bytes (one packet).
     * @param payload_len   Length of @p payload in bytes.
     * @param rtp_timestamp RTP timestamp of the first sample.
     * @param out           Caller-owned array of at least @p max_frames entries.
     * @param max_frames    Capacity of @p out.
     * @return Number of frames written (0 on error / malformed payload).
     */
    virtual std::size_t decode_packet(const std::uint8_t* payload,
                                      std::size_t  payload_len,
                                      std::uint32_t rtp_timestamp,
                                      AudioDecodedFrame* out,
                                      std::size_t  max_frames) noexcept = 0;

    /**
     * @brief Generate one concealment frame of @p duration_ms.
     *
     * @param duration_ms Length of the concealment frame in milliseconds.
     * @return Number of frames written (0 on error).
     */
    virtual std::size_t decode_plc(std::uint32_t rtp_timestamp,
                                   std::uint16_t duration_ms,
                                   AudioDecodedFrame* out,
                                   std::size_t  max_frames) noexcept = 0;

    /** Drop all decoder state (stream reset / renegotiation). */
    virtual void reset() noexcept = 0;

    /** Decoder statistics. */
    struct Stats {
        std::uint64_t frames_decoded = 0;
        std::uint64_t bytes_decoded = 0;
        std::uint64_t plc_frames = 0;
        std::uint64_t fec_recovered_frames = 0;
        std::uint64_t decode_errors = 0;
    };

    /** Snapshot of the decoder statistics. */
    virtual Stats stats() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/** Factory for audio decoders. */
class NIMRTC_API_STABLE IAudioDecoderFactory {
public:
    virtual ~IAudioDecoderFactory() = default;
    /** Unique plugin id, e.g. "opus". */
    virtual std::string_view id() const noexcept = 0;
    /** Short human-readable name. */
    virtual std::string_view display_name() const noexcept = 0;
    /** Create a decoder instance in the kConstructed state. */
    virtual IAudioDecoder* create() const = 0;
};

template<class T>
class NIMRTC_API_STABLE SimpleAudioDecoderFactory : public IAudioDecoderFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleAudioDecoderFactory(std::string_view id,
                                       std::string_view name) noexcept
        : id_(id), name_(name) {}

    std::string_view id() const noexcept override { return id_; }
    std::string_view display_name() const noexcept override { return name_; }
    IAudioDecoder* create() const override { return new T(); }
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_AUDIO_DECODER_HPP
