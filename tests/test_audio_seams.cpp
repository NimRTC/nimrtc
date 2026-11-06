// tests/test_audio_seams.cpp
// ----------------------------------------------------------------------------
// v1.2.0 (audio pipeline) -- the two new plugin seams.
//
// The audio pipeline introduces two seams (spec B-1 / B-2):
//   - plugins::IAudioDecoder   -- the codec side
//   - plugins::IAudioReceiver  -- the pipeline side
//
// This test is deliberately dependency-free: it uses fake
// implementations, so it needs neither the Opus module nor the audio_rx
// module.  It pins down the parts of the contract that everything else
// builds on:
//
//   - the seam headers compile standalone and expose the documented
//     defaults (the audio pipeline's negotiation defaults live here),
//   - the registry has a distinct category for each seam, and ids are
//     per-category (so the Opus decoder can use the id "opus" even
//     though the Opus codec plugin already uses it in the codec
//     category),
//   - the static registration macros work,
//   - the engine-side resolvers find what the registry holds.
// ----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include <nimrtc/core/api_version.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/pal/engine_plugin_resolver.hpp>
#include <nimrtc/plugins/audio_decoder.hpp>
#include <nimrtc/plugins/audio_receiver.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace {

using nimrtc::core::PluginRegistry;
using nimrtc::plugins::AudioDecodeFormat;
using nimrtc::plugins::AudioDecodedFrame;
using nimrtc::plugins::AudioReceiverConfig;
using nimrtc::plugins::IAudioDecoder;
using nimrtc::plugins::IAudioDecoderFactory;
using nimrtc::plugins::IAudioReceiver;
using nimrtc::plugins::IAudioReceiverFactory;

// ---------------------------------------------------------------------------
// Fakes
// ---------------------------------------------------------------------------

class FakeDecoder final : public IAudioDecoder {
public:
    const char* name() const noexcept override { return "fake_decoder"; }
    nimrtc::plugins::Status open() noexcept override { return 0; }
    void close() noexcept override {}

    AudioDecodeFormat format() const noexcept override { return fmt_; }
    nimrtc::plugins::Status update_format(AudioDecodeFormat f) noexcept override {
        fmt_ = f;
        return 0;
    }
    std::size_t decode_packet(const std::uint8_t* payload, std::size_t payload_len,
                              std::uint32_t rtp_timestamp, AudioDecodedFrame* out,
                              std::size_t max_frames) noexcept override {
        if (!out || max_frames == 0 || (!payload && payload_len != 0)) return 0;
        out[0].rtp_timestamp = rtp_timestamp;
        out[0].samples_per_channel = 960;
        out[0].sample_rate_hz = fmt_.sample_rate_hz;
        out[0].channels = fmt_.channels;
        out[0].duration_ms = 20;
        ++stats_.frames_decoded;
        stats_.bytes_decoded += payload_len;
        return 1;
    }
    std::size_t decode_plc(std::uint32_t rtp_timestamp, std::uint16_t duration_ms,
                           AudioDecodedFrame* out, std::size_t max_frames) noexcept override {
        if (!out || max_frames == 0) return 0;
        out[0].rtp_timestamp = rtp_timestamp;
        out[0].samples_per_channel = 960;
        out[0].duration_ms = duration_ms;
        out[0].concealed = true;
        ++stats_.plc_frames;
        return 1;
    }
    void reset() noexcept override {}
    Stats stats() const noexcept override { return stats_; }

private:
    AudioDecodeFormat fmt_{};
    Stats stats_{};
};

class FakeDecoderFactory final : public IAudioDecoderFactory {
public:
    std::string_view id() const noexcept override { return "fake_decoder"; }
    std::string_view display_name() const noexcept override { return "Fake decoder"; }
    IAudioDecoder* create() const override { return new FakeDecoder(); }
};

class FakeReceiver final : public IAudioReceiver {
public:
    const char* name() const noexcept override { return "fake_receiver"; }
    nimrtc::plugins::Status open() noexcept override { return 0; }
    void close() noexcept override {}

    void set_callbacks(FrameCallback on_frame, ErrorCallback on_error) noexcept override {
        on_frame_ = std::move(on_frame);
        on_error_ = std::move(on_error);
    }
    void push(const std::uint8_t*, std::size_t payload_len, std::uint32_t,
              std::uint16_t, std::uint32_t, nimrtc::plugins::TimestampUs) noexcept override {
        ++stats_.packets_received;
        stats_.bytes_received += payload_len;
    }
    void tick(nimrtc::plugins::TimestampUs) noexcept override {}
    void reset_stream(std::uint32_t) noexcept override {}
    Stats stats() const noexcept override { return stats_; }

private:
    FrameCallback on_frame_;
    ErrorCallback on_error_;
    Stats stats_{};
};

class FakeReceiverFactory final : public IAudioReceiverFactory {
public:
    std::string_view id() const noexcept override { return "fake_receiver"; }
    std::string_view display_name() const noexcept override { return "Fake receiver"; }
    IAudioReceiver* create(const AudioReceiverConfig&) const override {
        return new FakeReceiver();
    }
};

// A statically registered decoder -- exercises NIMRTC_REGISTER_AUDIO_DECODER.
const FakeDecoderFactory kStaticDecoderFactory;
NIMRTC_REGISTER_AUDIO_DECODER("static_audio_decoder", &kStaticDecoderFactory);

// A statically registered receiver -- exercises NIMRTC_REGISTER_AUDIO_RECEIVER.
const FakeReceiverFactory kStaticReceiverFactory;
NIMRTC_REGISTER_AUDIO_RECEIVER("static_audio_receiver", &kStaticReceiverFactory);

// ---------------------------------------------------------------------------
// Contract defaults
// ---------------------------------------------------------------------------

TEST(AudioSeams, DecodeFormatDefaults) {
    const AudioDecodeFormat f;
    EXPECT_EQ(f.sample_rate_hz, 48000u);
    EXPECT_EQ(f.channels, 1u);
    EXPECT_EQ(f.payload_type, 111u);   // RFC 7587 Opus payload type
    EXPECT_TRUE(f.use_inband_fec);
    EXPECT_FALSE(f.use_dtx);
}

TEST(AudioSeams, ReceiverConfigDefaults) {
    const AudioReceiverConfig c;
    EXPECT_EQ(c.clock_rate_hz, 48000u);
    EXPECT_EQ(c.decoder_sample_rate_hz, 48000u);
    EXPECT_EQ(c.decoder_channels, 2u);
    EXPECT_EQ(c.output_channels, 1u);
    EXPECT_EQ(c.frame_duration_ms, 20u);
    EXPECT_EQ(c.target_delay_ms, 40u);
    EXPECT_EQ(c.min_delay_ms, 10u);
    EXPECT_EQ(c.max_delay_ms, 200u);
    EXPECT_EQ(c.max_concealed_frames, 10u);
    EXPECT_TRUE(c.enable_fec_recovery);
    EXPECT_TRUE(c.enable_downmix);
}

// ---------------------------------------------------------------------------
// Registry categories
// ---------------------------------------------------------------------------

TEST(AudioSeams, DecoderCategoryIsDistinctFromTheCodecCategory) {
    auto& reg = PluginRegistry::instance();
    // The id "opus" is free in the decoder category even though the Opus
    // codec plugin registers "opus" in the codec category: ids are
    // per-category.
    EXPECT_EQ(reg.get_codec("opus"), nullptr);
    EXPECT_EQ(reg.get_audio_decoder("opus"), nullptr);

    static const FakeDecoderFactory factory;
    reg.register_audio_decoder("opus", &factory);
    EXPECT_EQ(reg.get_audio_decoder("opus"), &factory);
    // The codec category is untouched by the decoder registration.
    EXPECT_EQ(reg.get_codec("opus"), nullptr);
}

TEST(AudioSeams, ReceiverCategoryResolves) {
    auto& reg = PluginRegistry::instance();
    static const FakeReceiverFactory factory;
    reg.register_audio_receiver("fake_receiver", &factory);
    EXPECT_EQ(reg.get_audio_receiver("fake_receiver"), &factory);

    const auto ids = reg.list_audio_receivers();
    EXPECT_NE(std::find(ids.begin(), ids.end(), std::string_view("fake_receiver")), ids.end());
}

TEST(AudioSeams, StaticRegistrationMacroWorks) {
    auto& reg = PluginRegistry::instance();
    EXPECT_NE(reg.get_audio_decoder("static_audio_decoder"), nullptr);
    const auto ids = reg.list_audio_decoders();
    EXPECT_NE(std::find(ids.begin(), ids.end(), std::string_view("static_audio_decoder")),
              ids.end());

    EXPECT_NE(reg.get_audio_receiver("static_audio_receiver"), nullptr);
    const auto rids = reg.list_audio_receivers();
    EXPECT_NE(std::find(rids.begin(), rids.end(), std::string_view("static_audio_receiver")),
              rids.end());
}

// ---------------------------------------------------------------------------
// Engine-side resolvers
// ---------------------------------------------------------------------------

TEST(AudioSeams, ResolversFindTheRegisteredFactories) {
    auto& reg = PluginRegistry::instance();
    static const FakeDecoderFactory decoder_factory;
    static const FakeReceiverFactory receiver_factory;
    reg.register_audio_decoder("resolved_decoder", &decoder_factory);
    reg.register_audio_receiver("resolved_receiver", &receiver_factory);

    EXPECT_EQ(nimrtc::engine::pal::resolve_audio_decoder("resolved_decoder"),
              &decoder_factory);
    EXPECT_EQ(nimrtc::engine::pal::resolve_audio_receiver("resolved_receiver"),
              &receiver_factory);

    // An unknown id resolves to nullptr rather than throwing or
    // defaulting to some other plugin.
    EXPECT_EQ(nimrtc::engine::pal::resolve_audio_decoder("no_such_decoder"), nullptr);
    EXPECT_EQ(nimrtc::engine::pal::resolve_audio_receiver("no_such_receiver"), nullptr);
}

} // namespace
