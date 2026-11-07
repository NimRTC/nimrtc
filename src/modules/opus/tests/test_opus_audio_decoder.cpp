/**
 * @file src/modules/opus/tests/test_opus_audio_decoder.cpp
 * @brief Unit tests for the Opus implementation of nimrtc::plugins::IAudioDecoder.
 *
 * Covers the audio pipeline WP-2 rows of the verification matrix
 * (`docs/plan/audio-pipeline-dev-plan.md` Â§5):
 *
 *   - A-2  "Opus decoder plugin round-trip" â€?an RFC 7587 payload built by
 *          the module's own packetiser comes out of the decoder plugin as
 *          PCM frames of the expected length, rate and channel count.
 *   - B-8  "FEC recovery" â€?after a loss the next packet's in-band
 *          redundancy reconstructs the lost frame, flagged `fec_recovered`.
 *   - B-7  "PLC frame properties" â€?the concealment frame is flagged
 *          `concealed` and has the requested duration.
 *
 * The plugin is reached the way the engine reaches it: through the
 * registry's audio-decoder slot (`core::PluginRegistry`), never through a
 * concrete class.  That is the property WP-7 depends on.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/opus/codec_plugin.hpp>
#include <nimrtc/opus/opus.hpp>
#include <nimrtc/plugins/audio_decoder.hpp>

namespace {

constexpr std::uint32_t kRate         = 48000;
constexpr std::size_t   kFrameSamples = 960;   // 20 ms @ 48 kHz
constexpr std::uint8_t  kOpusPayloadType = 111; // RFC 7587

/** Register the module's plugins and fetch the Opus audio decoder factory.
 *  Returns nullptr when the module did not register (a hard error for the
 *  engine â€?see the "missing receiver failure" row of the matrix). */
const nimrtc::plugins::IAudioDecoderFactory* opus_decoder_factory() {
    nimrtc::opus::register_default_plugins();
    return nimrtc::core::PluginRegistry::instance().get_audio_decoder("opus");
}

/** A decoder instance created through the factory, ready to use. */
std::unique_ptr<nimrtc::plugins::IAudioDecoder> make_decoder() {
    const auto* factory = opus_decoder_factory();
    if (!factory) return nullptr;
    std::unique_ptr<nimrtc::plugins::IAudioDecoder> dec(factory->create());
    if (dec) dec->open();
    return dec;
}

/** A sine wave of @p n samples per channel. */
std::vector<float> make_sine(std::size_t n, float freq_hz, std::uint32_t rate) {
    std::vector<float> pcm(n);
    const double step = 2.0 * 3.141592653589793 * static_cast<double>(freq_hz)
                        / static_cast<double>(rate);
    for (std::size_t i = 0; i < n; ++i) {
        pcm[i] = 0.5f * static_cast<float>(std::sin(static_cast<double>(i) * step));
    }
    return pcm;
}

/** RMS of the first @p n interleaved samples. */
double rms(const float* pcm, std::size_t n) {
    if (!pcm || n == 0) return 0.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = static_cast<double>(pcm[i]);
        sum += v * v;
    }
    return std::sqrt(sum / static_cast<double>(n));
}

/** Encode one 20 ms frame and wrap it in an RFC 7587 payload (Code 0). */
std::vector<std::uint8_t> encode_one_payload(std::uint8_t channels) {
    nimrtc::opus::EncoderConfig cfg;
    cfg.sample_rate_hz = kRate;
    cfg.channels       = channels;
    cfg.bitrate_bps    = 24000;
    nimrtc::opus::Encoder enc(cfg);

    const auto pcm = make_sine(kFrameSamples, 300.0f, kRate);
    std::vector<std::uint8_t> opus_bytes(512);
    const std::size_t n = enc.encode(pcm.data(), kFrameSamples,
                                      opus_bytes.data(), opus_bytes.size());
    opus_bytes.resize(n);

    nimrtc::opus::CodecFrame frame{opus_bytes.data(), opus_bytes.size(), 20,
                                   static_cast<std::uint8_t>(channels - 1)};
    std::vector<std::uint8_t> payload(1024);
    const std::size_t written = nimrtc::opus::packetise(&frame, 1, kRate, false,
                                                         payload.data(), payload.size());
    payload.resize(written);
    return payload;
}

} // namespace

// ---------------------------------------------------------------------------
// Registry wiring
// ---------------------------------------------------------------------------

TEST(OpusAudioDecoder, RegistryCarriesTheOpusDecoder) {
    const auto* factory = opus_decoder_factory();
    ASSERT_NE(factory, nullptr);
    EXPECT_EQ(factory->id(), "opus");
    EXPECT_FALSE(factory->display_name().empty());

    std::unique_ptr<nimrtc::plugins::IAudioDecoder> dec(factory->create());
    ASSERT_NE(dec, nullptr);
    EXPECT_EQ(dec->open(), nimrtc::plugins::kOk);

    const nimrtc::plugins::AudioDecodeFormat fmt = dec->format();
    EXPECT_EQ(fmt.sample_rate_hz, kRate);
    EXPECT_EQ(fmt.channels, 1u);
    EXPECT_EQ(fmt.payload_type, kOpusPayloadType);
    EXPECT_TRUE(fmt.use_inband_fec);
}

TEST(OpusAudioDecoder, RegistryListsTheOpusDecoderId) {
    opus_decoder_factory();
    const auto ids = nimrtc::core::PluginRegistry::instance().list_audio_decoders();
    EXPECT_NE(std::find(ids.begin(), ids.end(), std::string_view("opus")), ids.end());
}

// ---------------------------------------------------------------------------
// A-2: round-trip
// ---------------------------------------------------------------------------

TEST(OpusAudioDecoder, DecodesASingleFramePayload) {
    auto dec = make_decoder();
    ASSERT_NE(dec, nullptr);

    const auto payload = encode_one_payload(1);
    ASSERT_FALSE(payload.empty());

    nimrtc::plugins::AudioDecodedFrame frames[4];
    const std::size_t n = dec->decode_packet(payload.data(), payload.size(),
                                              /*rtp_timestamp=*/1000, frames, 4);
    ASSERT_EQ(n, 1u);

    EXPECT_EQ(frames[0].samples_per_channel, kFrameSamples);
    EXPECT_EQ(frames[0].sample_rate_hz, kRate);
    EXPECT_EQ(frames[0].channels, 1u);
    EXPECT_EQ(frames[0].rtp_timestamp, 1000u);
    EXPECT_EQ(frames[0].duration_ms, 20);
    EXPECT_FALSE(frames[0].concealed);
    EXPECT_FALSE(frames[0].fec_recovered);
    EXPECT_GT(rms(frames[0].pcm, frames[0].samples_per_channel), 0.001);

    const auto stats = dec->stats();
    EXPECT_EQ(stats.frames_decoded, 1u);
    EXPECT_EQ(stats.bytes_decoded, payload.size());
    EXPECT_EQ(stats.decode_errors, 0u);
}

TEST(OpusAudioDecoder, DecodesAMultiFramePayload) {
    auto dec = make_decoder();
    ASSERT_NE(dec, nullptr);

    // Three 20 ms frames in one RFC 7587 payload (Code 3, VBR).
    nimrtc::opus::EncoderConfig cfg;
    cfg.sample_rate_hz = kRate;
    cfg.channels       = 1;
    cfg.bitrate_bps    = 24000;
    nimrtc::opus::Encoder enc(cfg);

    std::vector<std::vector<std::uint8_t>> opus_frames;
    for (int i = 0; i < 3; ++i) {
        const auto pcm = make_sine(kFrameSamples, 200.0f + 100.0f * i, kRate);
        std::vector<std::uint8_t> bytes(512);
        const std::size_t n = enc.encode(pcm.data(), kFrameSamples,
                                          bytes.data(), bytes.size());
        bytes.resize(n);
        opus_frames.push_back(std::move(bytes));
    }

    std::vector<nimrtc::opus::CodecFrame> frames;
    for (const auto& f : opus_frames) {
        frames.push_back(nimrtc::opus::CodecFrame{f.data(), f.size(), 20, 0});
    }
    std::vector<std::uint8_t> payload(4096);
    const std::size_t written = nimrtc::opus::packetise(frames.data(),
                                                         frames.size(), kRate, false,
                                                         payload.data(), payload.size());
    ASSERT_GT(written, 0u);

    nimrtc::plugins::AudioDecodedFrame out[8];
    const std::size_t n = dec->decode_packet(payload.data(), written,
                                              /*rtp_timestamp=*/0, out, 8);
    ASSERT_EQ(n, 3u);
    for (std::size_t i = 0; i < n; ++i) {
        EXPECT_EQ(out[i].samples_per_channel, kFrameSamples);
        EXPECT_EQ(out[i].duration_ms, 20);
        EXPECT_EQ(out[i].rtp_timestamp, static_cast<std::uint32_t>(i * kFrameSamples));
        EXPECT_FALSE(out[i].concealed);
        EXPECT_GT(rms(out[i].pcm, out[i].samples_per_channel), 0.001);
    }
}

TEST(OpusAudioDecoder, RejectsAMalformedPayload) {
    auto dec = make_decoder();
    ASSERT_NE(dec, nullptr);

    const std::uint8_t garbage[3] = {0xFF, 0xFF, 0xFF};
    nimrtc::plugins::AudioDecodedFrame frames[4];
    EXPECT_EQ(dec->decode_packet(garbage, sizeof(garbage), 0, frames, 4), 0u);
    EXPECT_EQ(dec->stats().decode_errors, 1u);
}

TEST(OpusAudioDecoder, RejectsAnEmptyPayload) {
    auto dec = make_decoder();
    ASSERT_NE(dec, nullptr);
    nimrtc::plugins::AudioDecodedFrame frames[4];
    EXPECT_EQ(dec->decode_packet(nullptr, 0, 0, frames, 4), 0u);
    EXPECT_EQ(dec->decode_packet(nullptr, 0, 0, nullptr, 4), 0u);
}

// ---------------------------------------------------------------------------
// B-7: concealment (PLC)
// ---------------------------------------------------------------------------

TEST(OpusAudioDecoder, ConcealmentFrameIsFlagged) {
    auto dec = make_decoder();
    ASSERT_NE(dec, nullptr);

    nimrtc::plugins::AudioDecodedFrame frames[2];
    const std::size_t n = dec->decode_plc(/*rtp_timestamp=*/5000,
                                           /*duration_ms=*/20, frames, 2);
    ASSERT_EQ(n, 1u);
    EXPECT_TRUE(frames[0].concealed);
    EXPECT_FALSE(frames[0].fec_recovered);
    EXPECT_EQ(frames[0].rtp_timestamp, 5000u);
    EXPECT_EQ(frames[0].samples_per_channel, kFrameSamples);
    EXPECT_EQ(frames[0].duration_ms, 20);
    EXPECT_EQ(dec->stats().plc_frames, 1u);
}

TEST(OpusAudioDecoder, ConcealmentOfZeroDurationIsRefused) {
    auto dec = make_decoder();
    ASSERT_NE(dec, nullptr);
    nimrtc::plugins::AudioDecodedFrame frames[2];
    EXPECT_EQ(dec->decode_plc(0, 0, frames, 2), 0u);
}

// ---------------------------------------------------------------------------
// B-8: in-band FEC recovery
// ---------------------------------------------------------------------------

TEST(OpusAudioDecoder, RecoversTheLostFrameFromTheNextPacket) {
    // libopus only embeds the in-band redundancy in the modes that support
    // it, and only when the loss-percentage hint makes it worth the bits.
    // At 12 kbit/s the packet size is identical with and without FEC (no
    // redundancy at all), so a recovery cannot work there; at 24 kbit/s
    // with a 20 % loss hint the redundancy is present.
    nimrtc::opus::EncoderConfig cfg;
    cfg.sample_rate_hz      = kRate;
    cfg.channels            = 1;
    cfg.bitrate_bps         = 24000;
    cfg.fec_enabled         = true;
    cfg.packet_loss_perc    = 20;
    cfg.application         = nimrtc::opus::kOpusApplicationVoip;
    nimrtc::opus::Encoder enc(cfg);

    std::vector<std::vector<std::uint8_t>> packets;
    for (int i = 0; i < 3; ++i) {
        const auto pcm = make_sine(kFrameSamples, 180.0f + 20.0f * i, kRate);
        std::vector<std::uint8_t> bytes(512);
        const std::size_t n = enc.encode(pcm.data(), kFrameSamples,
                                          bytes.data(), bytes.size());
        ASSERT_GT(n, 0u);
        bytes.resize(n);
        packets.push_back(std::move(bytes));
    }

    // Wrap one Opus frame into an RFC 7587 payload (Code 0).
    auto wrap = [](const std::vector<std::uint8_t>& frame) {
        nimrtc::opus::CodecFrame f{frame.data(), frame.size(), 20, 0};
        std::vector<std::uint8_t> payload(1024);
        const std::size_t written = nimrtc::opus::packetise(&f, 1, kRate, false,
                                                             payload.data(),
                                                             payload.size());
        payload.resize(written);
        return payload;
    };

    auto dec = make_decoder();
    ASSERT_NE(dec, nullptr);

    nimrtc::plugins::AudioDecodedFrame frames[4];

    // Packet 0 arrives and is decoded normally -- this is what gives the
    // plugin its timeline, so it can notice that packet 1 never came.
    const auto payload0 = wrap(packets[0]);
    ASSERT_EQ(dec->decode_packet(payload0.data(), payload0.size(),
                                  /*rtp_timestamp=*/0, frames, 4), 1u);
    ASSERT_FALSE(frames[0].fec_recovered);

    // Packet 1 is lost.  Packet 2 carries its redundancy: the plugin
    // notices the timestamp jump and asks the decoder to reconstruct the
    // lost frame from that redundancy before decoding packet 2 itself.
    const auto payload2 = wrap(packets[2]);
    const std::size_t n = dec->decode_packet(payload2.data(), payload2.size(),
                                              /*rtp_timestamp=*/2 * kFrameSamples,
                                              frames, 4);
    ASSERT_GE(n, 2u);

    // The first frame is the reconstruction of the lost frame (timestamp
    // 1 * 960), the second is the frame the packet actually carries.
    bool recovered = false;
    for (std::size_t i = 0; i < n; ++i) {
        if (frames[i].fec_recovered) {
            recovered = true;
            EXPECT_EQ(frames[i].rtp_timestamp, kFrameSamples);
            EXPECT_FALSE(frames[i].concealed);
            EXPECT_EQ(frames[i].samples_per_channel, kFrameSamples);
            EXPECT_GT(rms(frames[i].pcm, frames[i].samples_per_channel), 0.001);
        }
    }
    EXPECT_TRUE(recovered);
    EXPECT_EQ(dec->stats().fec_recovered_frames, 1u);
}

// ---------------------------------------------------------------------------
// Format negotiation
// ---------------------------------------------------------------------------

TEST(OpusAudioDecoder, UpdateFormatSwitchesTheChannelCount) {
    auto dec = make_decoder();
    ASSERT_NE(dec, nullptr);

    nimrtc::plugins::AudioDecodeFormat fmt = dec->format();
    fmt.channels = 2;
    EXPECT_EQ(dec->update_format(fmt), nimrtc::plugins::kOk);
    EXPECT_EQ(dec->format().channels, 2u);
}

TEST(OpusAudioDecoder, UpdateFormatRejectsInvalidValues) {
    auto dec = make_decoder();
    ASSERT_NE(dec, nullptr);

    nimrtc::plugins::AudioDecodeFormat fmt = dec->format();
    fmt.channels = 3;
    EXPECT_EQ(dec->update_format(fmt), nimrtc::plugins::kErrInvalidParam);

    fmt = dec->format();
    fmt.sample_rate_hz = 0;
    EXPECT_EQ(dec->update_format(fmt), nimrtc::plugins::kErrInvalidParam);
}

TEST(OpusAudioDecoder, ResetDropsTheDecoderState) {
    auto dec = make_decoder();
    ASSERT_NE(dec, nullptr);

    const auto payload = encode_one_payload(1);
    nimrtc::plugins::AudioDecodedFrame frames[4];
    ASSERT_EQ(dec->decode_packet(payload.data(), payload.size(), 0, frames, 4), 1u);

    dec->reset();
    EXPECT_EQ(dec->stats().frames_decoded, 0u);
}
