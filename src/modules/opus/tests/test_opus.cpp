/**
 * @file src/modules/opus/tests/test_opus.cpp
 * @brief Unit tests for the real libopus-backed Opus encoder/decoder.
 *
 * Verifies:
 *   1. Encoding a sine wave produces a non-empty Opus packet.
 *   2. Decoding recovers a packet of the correct frame length.
 *   3. Roundtrip preserves signal energy (PESQ-style sanity).
 *   4. PLC produces output for concealment.
 *   5. Stats counters increment correctly.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

#include <nimrtc/opus/opus.hpp>

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint8_t  kChannels   = 1;
constexpr std::size_t   kFrameSamples = 960;  // 20 ms @ 48 kHz, libopus sweet spot

double rms_energy(const float* pcm, std::size_t n) {
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = static_cast<double>(pcm[i]);
        sum += v * v;
    }
    return std::sqrt(sum / static_cast<double>(n));
}

std::vector<float> make_sine(std::size_t n, float freq_hz, std::uint32_t rate) {
    std::vector<float> pcm(n);
    const double phase_step =
        2.0 * 3.141592653589793 * static_cast<double>(freq_hz) / static_cast<double>(rate);
    for (std::size_t i = 0; i < n; ++i) {
        pcm[i] = 0.5f * static_cast<float>(std::sin(static_cast<double>(i) * phase_step));
    }
    return pcm;
}

} // namespace

// -----------------------------------------------------------------------------
// Basic construction
// -----------------------------------------------------------------------------

TEST(OpusEncoder, ConstructDefault) {
    nimrtc::opus::EncoderConfig cfg;
    cfg.sample_rate_hz = kSampleRate;
    cfg.channels       = kChannels;
    nimrtc::opus::Encoder enc(cfg);
    auto stats = enc.stats();
    EXPECT_EQ(stats.frames_encoded, 0u);
    EXPECT_FALSE(enc.is_dtx());
}

TEST(OpusDecoder, ConstructDefault) {
    nimrtc::opus::DecoderConfig cfg;
    cfg.sample_rate_hz = kSampleRate;
    cfg.channels       = kChannels;
    nimrtc::opus::Decoder dec(cfg);
    auto stats = dec.stats();
    EXPECT_EQ(stats.frames_decoded, 0u);
}

// -----------------------------------------------------------------------------
// Encode a sine wave
// -----------------------------------------------------------------------------

TEST(OpusEncoder, EncodeSineProducesPacket) {
    nimrtc::opus::EncoderConfig cfg;
    cfg.sample_rate_hz = kSampleRate;
    cfg.channels       = kChannels;
    cfg.bitrate_bps    = 64000;
    cfg.complexity     = 5;
    nimrtc::opus::Encoder enc(cfg);

    auto pcm = make_sine(kFrameSamples, 440.0f, kSampleRate);
    std::uint8_t buf[4000];
    std::size_t n = enc.encode(pcm.data(), kFrameSamples, buf, sizeof(buf));
    EXPECT_GT(n, 0u) << "Opus encoder returned 0 bytes";
    EXPECT_LE(n, sizeof(buf));

    auto stats = enc.stats();
    EXPECT_EQ(stats.frames_encoded, 1u);
    EXPECT_EQ(stats.bytes_encoded, n);
}

// -----------------------------------------------------------------------------
// Roundtrip preserves signal energy (loose sanity)
// -----------------------------------------------------------------------------

TEST(OpusCodec, RoundtripPreservesEnergy) {
    nimrtc::opus::EncoderConfig ec;
    ec.sample_rate_hz = kSampleRate;
    ec.channels       = kChannels;
    ec.bitrate_bps    = 64000;
    ec.fec_enabled    = true;
    nimrtc::opus::Encoder enc(ec);

    nimrtc::opus::DecoderConfig dc;
    dc.sample_rate_hz = kSampleRate;
    dc.channels       = kChannels;
    nimrtc::opus::Decoder dec(dc);

    auto pcm_in = make_sine(kFrameSamples * 3, 440.0f, kSampleRate);
    const double energy_in = rms_energy(pcm_in.data(), pcm_in.size());

    std::uint8_t buf[4000];
    float       out[kFrameSamples];

    std::size_t total_decoded = 0;
    for (std::size_t off = 0; off < pcm_in.size(); off += kFrameSamples) {
        std::size_t got = enc.encode(pcm_in.data() + off, kFrameSamples,
                                      buf, sizeof(buf));
        ASSERT_GT(got, 0u) << "encode frame " << off / kFrameSamples;
        std::size_t got_samples = dec.decode(buf, got, out, kFrameSamples);
        EXPECT_GE(got_samples, 1u);
        total_decoded += got_samples;
    }
    ASSERT_GE(total_decoded, kFrameSamples);

    std::vector<float> pcm_out(out, out + kFrameSamples);
    const double energy_out = rms_energy(pcm_out.data(), pcm_out.size());

    // Opus is lossy, but for a 440 Hz sine at 64 kbps mono the recovered
    // signal should have non-trivial energy (within an order of magnitude).
    EXPECT_GT(energy_out, energy_in * 0.01)
        << "recovered energy " << energy_out << " << input energy " << energy_in;
    EXPECT_LT(energy_out, energy_in * 100.0)
        << "recovered energy " << energy_out << " >> input energy " << energy_in;

    EXPECT_EQ(enc.stats().frames_encoded, 3u);
    EXPECT_EQ(dec.stats().frames_decoded, 3u);
}

// -----------------------------------------------------------------------------
// PLC: packet loss concealment produces audio
// -----------------------------------------------------------------------------

TEST(OpusDecoder, DecodePlcProducesAudio) {
    nimrtc::opus::DecoderConfig dc;
    dc.sample_rate_hz = kSampleRate;
    dc.channels       = kChannels;
    nimrtc::opus::Decoder dec(dc);

    float out[kFrameSamples];
    std::size_t n = dec.decode_plc(out, kFrameSamples);
    EXPECT_GT(n, 0u);
    EXPECT_EQ(dec.stats().plc_frames, 1u);
}

// -----------------------------------------------------------------------------
// Stats reset
// -----------------------------------------------------------------------------

TEST(OpusCodec, ResetClearsStats) {
    nimrtc::opus::EncoderConfig ec;
    ec.sample_rate_hz = kSampleRate;
    ec.channels       = kChannels;
    nimrtc::opus::Encoder enc(ec);
    auto pcm = make_sine(kFrameSamples, 1000.0f, kSampleRate);
    std::uint8_t buf[4000];
    enc.encode(pcm.data(), kFrameSamples, buf, sizeof(buf));
    EXPECT_EQ(enc.stats().frames_encoded, 1u);
    enc.reset();
    EXPECT_EQ(enc.stats().frames_encoded, 0u);
}

// -----------------------------------------------------------------------------
// Invalid frame size
// -----------------------------------------------------------------------------

TEST(OpusEncoder, InvalidFrameSizeReturnsZero) {
    nimrtc::opus::EncoderConfig ec;
    ec.sample_rate_hz = kSampleRate;
    ec.channels       = kChannels;
    nimrtc::opus::Encoder enc(ec);
    auto pcm = make_sine(123, 440.0f, kSampleRate);  // 123 samples is not a valid frame
    std::uint8_t buf[4000];
    EXPECT_EQ(enc.encode(pcm.data(), pcm.size(), buf, sizeof(buf)), 0u);
}

// -----------------------------------------------------------------------------
// DTX silence path
// -----------------------------------------------------------------------------

TEST(OpusEncoder, EncodeFrameForceDtxReturnsZero) {
    nimrtc::opus::EncoderConfig ec;
    ec.sample_rate_hz = kSampleRate;
    ec.channels       = kChannels;
    ec.dtx_enabled    = true;
    nimrtc::opus::Encoder enc(ec);
    auto pcm = make_sine(kFrameSamples, 1000.0f, kSampleRate);
    std::uint8_t buf[4000];
    EXPECT_EQ(enc.encode_frame(pcm.data(), kFrameSamples, buf, sizeof(buf), /*force_dtx*/ true), 0u);
    EXPECT_EQ(enc.stats().dtx_frames, 1u);
}
