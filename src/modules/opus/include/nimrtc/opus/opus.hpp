/**
 * @file nimrtc/opus/opus.hpp
 * @brief Opus audio codec interface (RFC 6716).
 *
 * P1 stub: implements a no-op PCM passthrough (encode = copy raw PCM as-is,
 * decode = return silence).  Real Opus encoding/decoding lands when
 * libopus is vendored (see NimRTCVendored.cmake §11.2).
 *
 * Interface is stable and binary layout is P1-complete even for the stub.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>

namespace nimrtc::opus {

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

constexpr std::uint32_t kDefaultSampleRate = 48000;
constexpr std::uint8_t  kDefaultChannels   = 1;
constexpr std::uint8_t  kDefaultPayloadType = 111;  // RFC 7587

// -----------------------------------------------------------------------------
// Config
// -----------------------------------------------------------------------------

struct EncoderConfig {
    std::uint32_t sample_rate_hz = kDefaultSampleRate;
    std::uint8_t  channels     = kDefaultChannels;
    int            bitrate_bps   = 64000;
    int            complexity     = 10;     // 0-10, 10 = best quality
    bool           vad_enabled    = false;
    bool           fec_enabled   = true;    // forward error correction
    bool           dtx_enabled   = false;   // discontinuous transmission
};

struct DecoderConfig {
    std::uint32_t sample_rate_hz = kDefaultSampleRate;
    std::uint8_t  channels     = kDefaultChannels;
    int            complexity     = 10;
};

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------

class Encoder {
public:
    explicit Encoder(EncoderConfig config);
    ~Encoder();
    Encoder(const Encoder&)            = delete;
    Encoder& operator=(const Encoder&) = delete;
    Encoder(Encoder&&) noexcept;
    Encoder& operator=(Encoder&&) noexcept;

    /** Encode PCM samples to Opus. Returns the number of output bytes.
     *  @param pcm  Interleaved PCM samples in float [-1.0, 1.0].
     *  @param num_samples  Number of samples per channel.
     *  @param output  Output buffer for encoded Opus packets.
     *  @return Number of encoded bytes, or 0 on error / DTX silence. */
    std::size_t encode(const float* pcm,
                       std::size_t num_samples,
                       std::uint8_t* output,
                       std::size_t output_capacity) noexcept;

    /** Encode a 10ms frame (480 samples at 48kHz).
     *  Returns opus encoded length in bytes, or 0 if DTX / error. */
    std::size_t encode_frame(const float* pcm,
                             std::size_t num_samples,
                             std::uint8_t* output,
                             std::size_t output_capacity,
                             bool force_dtx = false) noexcept;

    /** Packet loss concealment: generate a PLC frame.
     *  Fills output with a synthetic frame derived from the last decoded audio. */
    std::size_t encode_plc(std::uint8_t* output,
                            std::size_t output_capacity) noexcept;

    /** Reset encoder state (on stream start / keyframe). */
    void reset() noexcept;

    /** Whether the encoder is currently in DTX (silence) mode. */
    bool is_dtx() const noexcept;

    struct Stats {
        std::uint64_t frames_encoded = 0;
        std::uint64_t bytes_encoded  = 0;
        std::uint64_t dtx_frames    = 0;
        std::uint64_t plc_frames     = 0;
    };
    Stats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// -----------------------------------------------------------------------------
// Decoder
// -----------------------------------------------------------------------------

class Decoder {
public:
    explicit Decoder(DecoderConfig config);
    ~Decoder();
    Decoder(const Decoder&)            = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) noexcept;
    Decoder& operator=(Decoder&&) noexcept;

    /** Decode Opus to PCM.
     *  @param opus  Opus-encoded packet.
     *  @param opus_len  Size of the Opus packet.
     *  @param output  Output buffer for decoded PCM.
     *  @param output_capacity  Capacity of the output buffer (in float samples).
     *  @return Number of output PCM samples per channel, or 0 on error. */
    std::size_t decode(const std::uint8_t* opus,
                       std::size_t opus_len,
                       float* output,
                       std::size_t output_capacity) noexcept;

    /** Decode a PLC frame (packet loss). */
    std::size_t decode_plc(float* output,
                            std::size_t output_capacity) noexcept;

    /** Reset decoder state. */
    void reset() noexcept;

    struct Stats {
        std::uint64_t frames_decoded = 0;
        std::uint64_t bytes_decoded  = 0;
        std::uint64_t plc_frames     = 0;
        std::uint64_t fec_frames     = 0;
        std::uint64_t errors         = 0;
    };
    Stats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// -----------------------------------------------------------------------------
// RTP packetisation helpers (RFC 7587)
// -----------------------------------------------------------------------------

/** Packetise an Opus frame for RTP.
 *  For payloads < 1275 bytes: single byte payload header (TOC).
 *  For larger payloads: multiple frames with TOC per frame.
 *
 *  Output format: [TOC byte][payload][payload]...
 *  TOC byte: 0x80 = mono, CBR, no FEC.
 */
std::size_t packetise(const std::uint8_t* frames,
                     std::size_t num_frames,
                     std::uint8_t* output,
                     std::size_t output_capacity,
                     std::uint8_t channels,
                     bool cbr) noexcept;

} // namespace nimrtc::opus
