/**
 * @file nimrtc/opus/opus.hpp
 * @brief Opus audio codec interface (RFC 6716) + RFC 7587 RTP packetisation.
 *
 * Opus encode / decode wraps libopus (vendored at src/third_party/libopus).
 * RTP packetisation implements the full RFC 7587 §4 framing:
 *
 *   - Code 0 (1 frame per packet):  TOC byte + frame data
 *   - Code 1 (2 frames, equal size): TOC byte + 2 frames of (N-1)/2 bytes
 *   - Code 2 (2 frames, unequal):   TOC byte + 1- or 2-byte len(N1) + frame1 + frame2
 *   - Code 3 (signalled M frames):  TOC byte + frame-count byte [+ padding len] [+ length table] + frames
 *
 * Self-delimiting length coding (RFC 6716 §3.2.1) — values 252..255 trigger
 * a 2-byte sequence with the actual length computed as `b0 + b1*4`.
 *
 * The TOC byte's config field selects mode / bandwidth / frame-size per
 * RFC 6716 Table 2; the stereo bit `s` (TOC[1]) is 0=mono / 1=stereo.
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
// RFC 7587 RTP packetisation
// -----------------------------------------------------------------------------

/** Per-frame input to packetise(). The caller owns the storage; packetise()
 *  copies the bytes into the output payload. `frame_size_ms` must match the
 *  Opus frame size used to encode the frame (2.5 / 5 / 10 / 20 / 40 / 60 ms)
 *  and is what RFC 7587 / RFC 6716 §3.1 uses to choose the TOC config field.
 *
 *  `is_stereo` is the TOC `s` bit (RFC 6716 §3.1) — 0 = mono, 1 = stereo
 *  (interleaved L/R). Higher values are clamped to 0 or 1 by the
 *  packetiser.
 */
struct CodecFrame {
    const std::uint8_t* data     = nullptr;
    std::size_t         size     = 0;
    std::uint16_t       frame_size_ms = 20;  // 2.5, 5, 10, 20, 40, 60
    std::uint8_t        is_stereo    = 0;    // 0 = mono, 1 = stereo (LR)
};

/** SDP-style codec configuration used by callers to choose packetisation
 *  granularity (RFC 7587 §6.1). `ptime_ms` / `maxptime_ms` together with
 *  `sample_rate_hz` determine how many frames packetise() packs per RTP
 *  payload.
 */
struct CodecConfig {
    std::uint32_t sample_rate_hz = kDefaultSampleRate;
    std::uint8_t  channels       = kDefaultChannels;   // 1 or 2 (stereo LR)
    std::uint16_t ptime_ms       = 20;                // preferred packet dur
    std::uint16_t maxptime_ms    = 120;               // max packet dur
    bool          cbr            = false;             // const vs VBR
};

/** Output of depacketise() — describes one Opus sub-frame inside an RTP
 *  payload. `code` is the TOC[1:0] c-field (0..3) and `s` is the stereo
 *  bit at TOC[2]. `data` points into the original payload buffer (no copy).
 */
struct PacketView {
    const std::uint8_t* data          = nullptr;
    std::size_t         size          = 0;
    std::uint8_t        config        = 0;   // TOC[7:3] (RFC 6716 Table 2)
    std::uint8_t        s             = 0;   // TOC[2] — stereo flag
    std::uint8_t        code          = 0;   // TOC[1:0] — frame count code
    std::uint16_t       frame_size_ms = 0;
};

/** Maximum representable Opus frame length per RFC 6716 §3.2.1 (R2):
 *  255 * 4 + 255 = 1275 bytes. */
inline constexpr std::size_t kMaxOpusFrameBytes = 1275;

/** Compute the TOC config field for a given frame size in ms. Returns 0..31
 *  per RFC 6716 Table 2 (config bits TOC[7:3]), or `std::nullopt` if the
 *  frame size is not a supported Opus duration.
 *
 *  NB / MB / WB / SWB / FB map to the lowest config in the corresponding
 *  range (config 0..3 = SILK-only NB, 28..31 = CELT-only FB). The caller
 *  chooses the bandwidth via libopus CTL (OPUS_SET_MAX_BANDWIDTH) — this
 *  helper only encodes the duration.
 */
std::optional<std::uint8_t> toc_config_for_frame_size_ms(std::uint16_t frame_size_ms) noexcept;

/** Build the TOC byte from a config field and an `s` field (TOC[1]).
 *  Bits: TOC = (config << 3) | (s << 2) | code.
 *  - config (5 bits) sits in bits 7..3 (RFC 6716 §3.1)
 *  - s       (1 bit)  sits in bit  2
 *  - code    (2 bits) sit in bits 1..0
 *  See RFC 6716 §3.1 Figure 1.
 */
inline std::uint8_t make_toc_byte(std::uint8_t config,
                                    std::uint8_t s,
                                    std::uint8_t code) noexcept {
    return static_cast<std::uint8_t>(
        ((config & 0x1F) << 3) | ((s & 0x01) << 2) | (code & 0x03));
}

/** Encode a list of Opus frames into a single RFC 7587 RTP payload.
 *
 *  - 1 frame  → Code 0 (TOC + frame data)
 *  - 2 frames with equal size → Code 1 (TOC + 2×equal-size chunks)
 *  - 2 frames with different sizes → Code 2 (TOC + 1-2 byte len + frame1 + frame2)
 *  - M ≥ 3 frames (or M = 2 frames and caller forces VBR layout, etc.)
 *                → Code 3 (TOC + frame-count byte [+ padding len bytes]
 *                          + M-1 length entries + frame data)
 *
 *  The TOC config field is computed per-frame from `frame.frame_size_ms`
 *  using `toc_config_for_frame_size_ms()`.  All frames in a packet must
 *  share the same config — packetise() returns 0 if the inputs disagree.
 *
 *  @param frames        Array of `CodecFrame` describing the input frames.
 *  @param num_frames    Number of frames (must be ≥ 1 and ≤ 48).
 *  @param sample_rate_hz  RTP clock rate (always 48000 for Opus).
 *  @param cbr            If true and num_frames == 1, emit Code 0; if true
 *                        and num_frames ≥ 2, emit Code 3 (CBR variant).
 *  @param output        Caller-owned output buffer.
 *  @param output_capacity  Capacity of the output buffer in bytes.
 *  @return Number of bytes written to `output`, or 0 on error (insufficient
 *          capacity, mismatched configs, too many frames, etc.).
 */
std::size_t packetise(const CodecFrame* frames,
                      std::size_t num_frames,
                      std::uint32_t sample_rate_hz,
                      bool cbr,
                      std::uint8_t* output,
                      std::size_t output_capacity) noexcept;

/** Decode an RFC 7587 / RFC 6716 RTP payload into per-frame `PacketView`s.
 *
 *  Inspects the TOC byte, picks the appropriate Code path (0/1/2/3), and
 *  parses the length table if needed. Writes up to `views_capacity`
 *  `PacketView` entries and updates `*num_views`.
 *
 *  `views` entries point into the original `payload` buffer; no bytes are
 *  copied. Frame size in ms is reconstructed from the TOC config field
 *  (RFC 6716 Table 2 — within each range the lowest index = smallest frame).
 *
 *  @return Number of frames parsed on success (same as *num_views), or 0
 *          on malformed input (truncated length table, Code 1 with odd
 *          payload, Code 2 len overflow, etc.).
 */
std::size_t depacketise(const std::uint8_t* payload,
                        std::size_t payload_len,
                        PacketView* views,
                        std::size_t views_capacity,
                        std::size_t* num_views) noexcept;

} // namespace nimrtc::opus
