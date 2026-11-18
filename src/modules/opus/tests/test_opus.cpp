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
 *   6. The int16 encode path (the TTS return path, slice v4-voice-agent)
 *      produces real Opus packets and survives a roundtrip.
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

/** The int16 twin of `make_sine` — what a TTS synthesiser emits. */
std::vector<std::int16_t> make_sine_int16(std::size_t n,
                                          float freq_hz,
                                          std::uint32_t rate) {
    std::vector<std::int16_t> pcm(n);
    const double phase_step =
        2.0 * 3.141592653589793 * static_cast<double>(freq_hz) / static_cast<double>(rate);
    for (std::size_t i = 0; i < n; ++i) {
        const double v = 0.5 * std::sin(static_cast<double>(i) * phase_step);
        pcm[i] = static_cast<std::int16_t>(v * 32767.0);
    }
    return pcm;
}

/** RMS energy of an int16 buffer, normalised to the float scale (-1..1). */
double rms_energy_int16(const std::int16_t* pcm, std::size_t n) {
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = static_cast<double>(pcm[i]) / 32768.0;
        sum += v * v;
    }
    return std::sqrt(sum / static_cast<double>(n));
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

// -----------------------------------------------------------------------------
// Bitrate defaults + in-place reconfiguration
// -----------------------------------------------------------------------------

TEST(OpusEncoder, DefaultBitrateFollowsTheChannelCount) {
    EXPECT_EQ(nimrtc::opus::default_bitrate_bps_for_channels(1), 32000);
    EXPECT_EQ(nimrtc::opus::default_bitrate_bps_for_channels(2), 64000);

    // An unset bitrate (0) resolves to the channel-count default.
    nimrtc::opus::EncoderConfig mono;
    mono.sample_rate_hz = kSampleRate;
    mono.channels       = 1;
    mono.bitrate_bps    = 0;
    nimrtc::opus::Encoder enc_mono(mono);
    EXPECT_EQ(enc_mono.bitrate_bps(), 32000);

    nimrtc::opus::EncoderConfig stereo;
    stereo.sample_rate_hz = kSampleRate;
    stereo.channels       = 2;
    stereo.bitrate_bps    = 0;
    nimrtc::opus::Encoder enc_stereo(stereo);
    EXPECT_EQ(enc_stereo.bitrate_bps(), 64000);
}

TEST(OpusEncoder, ReconfigureChangesTheBitrateInPlace) {
    nimrtc::opus::EncoderConfig ec;
    ec.sample_rate_hz = kSampleRate;
    ec.channels       = kChannels;
    ec.bitrate_bps    = 32000;
    nimrtc::opus::Encoder enc(ec);
    EXPECT_EQ(enc.bitrate_bps(), 32000);

    // Encode a frame so the encoder has some state to preserve.
    auto pcm = make_sine(kFrameSamples, 440.0f, kSampleRate);
    std::uint8_t buf[4000];
    ASSERT_GT(enc.encode(pcm.data(), kFrameSamples, buf, sizeof(buf)), 0u);
    ASSERT_EQ(enc.stats().frames_encoded, 1u);

    // A bitrate step reconfigures the running encoder: the statistics
    // survive it (this is what the engine's BWE adaptation relies on).
    ec.bitrate_bps = 16000;
    EXPECT_TRUE(enc.reconfigure(ec));
    EXPECT_EQ(enc.bitrate_bps(), 16000);
    EXPECT_EQ(enc.stats().frames_encoded, 1u)
        << "a bitrate step must not reset the encoder statistics";

    // The encoder still works afterwards.
    EXPECT_GT(enc.encode(pcm.data(), kFrameSamples, buf, sizeof(buf)), 0u);
    EXPECT_EQ(enc.stats().frames_encoded, 2u);
}

TEST(OpusEncoder, ReconfigureKeepsTheStatisticsAcrossAChannelChange) {
    nimrtc::opus::EncoderConfig ec;
    ec.sample_rate_hz = kSampleRate;
    ec.channels       = kChannels;
    ec.bitrate_bps    = 32000;
    nimrtc::opus::Encoder enc(ec);

    auto pcm = make_sine(kFrameSamples, 440.0f, kSampleRate);
    std::uint8_t buf[4000];
    ASSERT_GT(enc.encode(pcm.data(), kFrameSamples, buf, sizeof(buf)), 0u);

    // libopus cannot change the channel count in place, so the encoder
    // is re-created — but the statistics are carried over.
    ec.channels = 2;
    EXPECT_TRUE(enc.reconfigure(ec));
    EXPECT_EQ(enc.channels(), 2);
    EXPECT_EQ(enc.stats().frames_encoded, 1u);
}

// -----------------------------------------------------------------------------
// The int16 encode path (slice v4-voice-agent)
//
// `Encoder::encode_int16` is the TTS return path: a synthesiser emits signed
// 16-bit PCM and it goes straight into Opus, without a float round-trip.  The
// path only existed with the AI-game demo (PR #91), so it had no unit test of
// its own — the demo's self-test was its only coverage.  These tests pin the
// behaviour that path actually depends on.
// -----------------------------------------------------------------------------

TEST(OpusEncoder, EncodeInt16ProducesPacket) {
    nimrtc::opus::EncoderConfig cfg;
    cfg.sample_rate_hz = kSampleRate;
    cfg.channels       = kChannels;
    cfg.bitrate_bps    = 64000;
    nimrtc::opus::Encoder enc(cfg);

    auto pcm = make_sine_int16(kFrameSamples, 440.0f, kSampleRate);
    std::uint8_t buf[4000];
    const std::size_t n = enc.encode_int16(pcm.data(), pcm.size(), buf, sizeof(buf));

    EXPECT_GT(n, 0u) << "the int16 path returned 0 bytes";
    EXPECT_LE(n, sizeof(buf));

    // The statistics must move exactly like the float path's do — the TTS
    // return path counts packets with these counters.
    const auto stats = enc.stats();
    EXPECT_EQ(stats.frames_encoded, 1u);
    EXPECT_EQ(stats.bytes_encoded, n);
}

TEST(OpusCodec, EncodeInt16RoundtripPreservesEnergy) {
    nimrtc::opus::EncoderConfig ec;
    ec.sample_rate_hz = kSampleRate;
    ec.channels       = kChannels;
    ec.bitrate_bps    = 64000;
    nimrtc::opus::Encoder enc(ec);

    nimrtc::opus::DecoderConfig dc;
    dc.sample_rate_hz = kSampleRate;
    dc.channels       = kChannels;
    nimrtc::opus::Decoder dec(dc);

    auto pcm_in = make_sine_int16(kFrameSamples * 3, 440.0f, kSampleRate);
    const double energy_in = rms_energy_int16(pcm_in.data(), pcm_in.size());

    std::uint8_t buf[4000];
    float       out[kFrameSamples];

    std::size_t total_decoded = 0;
    for (std::size_t off = 0; off < pcm_in.size(); off += kFrameSamples) {
        const std::size_t got = enc.encode_int16(pcm_in.data() + off, kFrameSamples,
                                                 buf, sizeof(buf));
        ASSERT_GT(got, 0u) << "encode_int16 frame " << off / kFrameSamples;
        const std::size_t got_samples = dec.decode(buf, got, out, kFrameSamples);
        EXPECT_GE(got_samples, 1u);
        total_decoded += got_samples;
    }
    ASSERT_GE(total_decoded, kFrameSamples);

    const std::vector<float> pcm_out(out, out + kFrameSamples);
    const double energy_out = rms_energy(pcm_out.data(), pcm_out.size());

    // Opus is lossy, but the recovered signal must carry the input's energy
    // within an order of magnitude — otherwise the int16 path is not really
    // encoding the caller's samples.
    EXPECT_GT(energy_out, energy_in * 0.01)
        << "recovered energy " << energy_out << " << input energy " << energy_in;
    EXPECT_LT(energy_out, energy_in * 100.0)
        << "recovered energy " << energy_out << " >> input energy " << energy_in;

    EXPECT_EQ(enc.stats().frames_encoded, 3u);
    EXPECT_EQ(dec.stats().frames_decoded, 3u);
}

TEST(OpusEncoder, EncodeInt16AndFloatAgreeOnTheSameSignal) {
    // The two paths must be the *same* encoder: the int16 path is not a
    // different codec with different behaviour, it is the float path without
    // the conversion.  Encoding the same signal twice — once as int16, once
    // as float — must produce packets of the same size.
    //
    // (The bytes need not be identical: libopus's int16 and float entry
    // points round their input slightly differently.  The *size* is the
    // stable invariant, and the energy test above covers the content.)
    nimrtc::opus::EncoderConfig cfg;
    cfg.sample_rate_hz = kSampleRate;
    cfg.channels       = kChannels;
    cfg.bitrate_bps    = 64000;

    auto pcm_i16 = make_sine_int16(kFrameSamples, 440.0f, kSampleRate);
    std::vector<float> pcm_f32(kFrameSamples);
    for (std::size_t i = 0; i < kFrameSamples; ++i) {
        pcm_f32[i] = static_cast<float>(pcm_i16[i]) / 32768.0f;
    }

    nimrtc::opus::Encoder enc_i16(cfg);
    nimrtc::opus::Encoder enc_f32(cfg);

    std::uint8_t buf_i16[4000];
    std::uint8_t buf_f32[4000];
    const std::size_t n_i16 = enc_i16.encode_int16(pcm_i16.data(), pcm_i16.size(),
                                                   buf_i16, sizeof(buf_i16));
    const std::size_t n_f32 = enc_f32.encode(pcm_f32.data(), pcm_f32.size(),
                                             buf_f32, sizeof(buf_f32));

    ASSERT_GT(n_i16, 0u);
    ASSERT_GT(n_f32, 0u);
    EXPECT_EQ(n_i16, n_f32)
        << "the int16 and float paths encoded the same signal to "
           "different-sized packets (" << n_i16 << " vs " << n_f32 << ")";
}

TEST(OpusEncoder, EncodeInt16RejectsAnInvalidFrameSize) {
    nimrtc::opus::EncoderConfig cfg;
    cfg.sample_rate_hz = kSampleRate;
    cfg.channels       = kChannels;
    nimrtc::opus::Encoder enc(cfg);

    // 123 samples is not one of libopus's six permitted frame sizes.
    auto pcm = make_sine_int16(123, 440.0f, kSampleRate);
    std::uint8_t buf[4000];
    EXPECT_EQ(enc.encode_int16(pcm.data(), pcm.size(), buf, sizeof(buf)), 0u);
    EXPECT_EQ(enc.stats().frames_encoded, 0u)
        << "a rejected frame must not be counted as encoded";
}

TEST(OpusEncoder, EncodeInt16RejectsNullAndEmptyInput) {
    nimrtc::opus::EncoderConfig cfg;
    cfg.sample_rate_hz = kSampleRate;
    cfg.channels       = kChannels;
    nimrtc::opus::Encoder enc(cfg);

    auto pcm = make_sine_int16(kFrameSamples, 440.0f, kSampleRate);
    std::uint8_t buf[4000];

    EXPECT_EQ(enc.encode_int16(nullptr, kFrameSamples, buf, sizeof(buf)), 0u);
    EXPECT_EQ(enc.encode_int16(pcm.data(), kFrameSamples, nullptr, sizeof(buf)), 0u);
    EXPECT_EQ(enc.encode_int16(pcm.data(), 0, buf, sizeof(buf)), 0u);
    EXPECT_EQ(enc.encode_int16(pcm.data(), kFrameSamples, buf, 0), 0u);
    EXPECT_EQ(enc.stats().frames_encoded, 0u);
}

TEST(OpusEncoder, EncodeInt16AcceptsEveryPermittedFrameSize) {
    // The int16 path must accept the same six frame sizes the float path
    // does — the TTS synthesiser chunks its output at its own cadence, and
    // a frame size the float path accepts must not be rejected here.
    nimrtc::opus::EncoderConfig cfg;
    cfg.sample_rate_hz = kSampleRate;
    cfg.channels       = kChannels;
    nimrtc::opus::Encoder enc(cfg);

    for (std::size_t samples : {std::size_t{120}, std::size_t{240},
                                std::size_t{480}, std::size_t{960},
                                std::size_t{1920}, std::size_t{2880}}) {
        auto pcm = make_sine_int16(samples, 440.0f, kSampleRate);
        std::uint8_t buf[4000];
        EXPECT_GT(enc.encode_int16(pcm.data(), samples, buf, sizeof(buf)), 0u)
            << "the int16 path rejected the " << samples << "-sample frame";
    }
    EXPECT_EQ(enc.stats().frames_encoded, 6u);
}
