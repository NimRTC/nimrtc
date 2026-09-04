/**
 * @file src/modules/opus/src/opus.cpp
 * @brief Opus codec stub — PCM passthrough (no real encoding).
 *
 * P1 stub: all encode/decode methods are no-ops or return error.
 * Real Opus support lands when libopus is vendored.
 */

#include <nimrtc/opus/opus.hpp>

#include <algorithm>
#include <cstring>

namespace nimrtc::opus {

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

constexpr std::size_t kSamplesPer10Ms = 480;   // 48 kHz × 10 ms
constexpr std::size_t kMaxPcmBytes    = kSamplesPer10Ms * 2 * 2; // stereo float

// -----------------------------------------------------------------------------
// Encoder::Impl
// -----------------------------------------------------------------------------

struct Encoder::Impl {
    EncoderConfig              config;
    Encoder::Stats            stats{};
    bool                     dtx_active = false;

    explicit Impl(EncoderConfig c) : config(std::move(c)) {}
};

Encoder::Encoder(EncoderConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
Encoder::~Encoder() = default;
Encoder::Encoder(Encoder&&) noexcept = default;
Encoder& Encoder::operator=(Encoder&&) noexcept = default;

std::size_t Encoder::encode(const float* pcm,
                             std::size_t num_samples,
                             std::uint8_t* output,
                             std::size_t output_capacity) noexcept {
    if (!pcm || !output || num_samples == 0 ||
        output_capacity < num_samples * sizeof(float)) {
        return 0;
    }
    // Stub: copy raw PCM as-is (no encoding).  The demo pipeline exercises
    // the RTP/ICE/SDP path; the far end will hear silence since our stub
    // decoder produces silence.
    std::memcpy(output, pcm, num_samples * sizeof(float));
    impl_->stats.frames_encoded++;
    impl_->stats.bytes_encoded += num_samples * sizeof(float);
    return num_samples * sizeof(float);
}

std::size_t Encoder::encode_frame(const float* pcm,
                                   std::size_t num_samples,
                                   std::uint8_t* output,
                                   std::size_t output_capacity,
                                   bool force_dtx) noexcept {
    if (force_dtx || impl_->dtx_active) {
        impl_->stats.dtx_frames++;
        return 0;
    }
    if (output_capacity < num_samples * sizeof(float)) return 0;
    return encode(pcm, num_samples, output, output_capacity);
}

std::size_t Encoder::encode_plc(std::uint8_t* output,
                                 std::size_t output_capacity) noexcept {
    impl_->stats.plc_frames++;
    if (output_capacity < kSamplesPer10Ms * sizeof(float)) return 0;
    std::memset(output, 0, kSamplesPer10Ms * sizeof(float));
    return kSamplesPer10Ms * sizeof(float);
}

void Encoder::reset() noexcept {
    impl_->stats = Encoder::Stats{};
    impl_->dtx_active = false;
}

bool Encoder::is_dtx() const noexcept { return impl_->dtx_active; }
Encoder::Stats Encoder::stats() const noexcept { return impl_->stats; }

// -----------------------------------------------------------------------------
// Decoder::Impl
// -----------------------------------------------------------------------------

struct Decoder::Impl {
    DecoderConfig            config;
    Decoder::Stats          stats{};

    explicit Impl(DecoderConfig c) : config(std::move(c)) {}
};

Decoder::Decoder(DecoderConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}
Decoder::~Decoder() = default;
Decoder::Decoder(Decoder&&) noexcept = default;
Decoder& Decoder::operator=(Decoder&&) noexcept = default;

std::size_t Decoder::decode(const std::uint8_t* encoded,
                             std::size_t encoded_len,
                             float* output,
                             std::size_t output_capacity) noexcept {
    (void)encoded;
    if (!output || output_capacity < kSamplesPer10Ms * sizeof(float)) return 0;
    // Stub: produce silence.  Real Opus decoder (libopus) fills in actual PCM.
    std::memset(output, 0,
                 std::min(output_capacity, kSamplesPer10Ms * sizeof(float)));
    impl_->stats.frames_decoded++;
    impl_->stats.bytes_decoded += encoded_len;
    return kSamplesPer10Ms;
}

std::size_t Decoder::decode_plc(float* output,
                                 std::size_t output_capacity) noexcept {
    impl_->stats.plc_frames++;
    if (output_capacity < kSamplesPer10Ms * sizeof(float)) return 0;
    std::memset(output, 0, kSamplesPer10Ms * sizeof(float));
    return kSamplesPer10Ms;
}

void Decoder::reset() noexcept {
    impl_->stats = Decoder::Stats{};
}
Decoder::Stats Decoder::stats() const noexcept { return impl_->stats; }

// -----------------------------------------------------------------------------
// RTP packetisation (RFC 7587)
// -----------------------------------------------------------------------------

std::size_t packetise(const std::uint8_t* /*frames*/,
                     std::size_t /*num_frames*/,
                     std::uint8_t* output,
                     std::size_t output_capacity,
                     std::uint8_t channels,
                     bool /*cbr*/) noexcept {
    if (!output || output_capacity < 1) return 0;
    // RFC 7587 TOC byte for single-frame Opus:
    //   For mono: 0x80 (config=1, mono, no FEC)
    //   For stereo: 0x78 (config=0, stereo, no FEC)
    output[0] = static_cast<std::uint8_t>(
        (channels == 1) ? 0x80 : 0x78);
    return 1;  // stub: only the TOC byte
}

} // namespace nimrtc::opus
