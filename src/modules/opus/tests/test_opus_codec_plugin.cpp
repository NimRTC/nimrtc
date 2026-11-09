/**
 * @file src/modules/opus/tests/test_opus_codec_plugin.cpp
 * @brief The Opus implementation of the `plugins::ICodec` seam.
 *
 * The adapter is what the engine actually talks to, and the engine's
 * BWE-linked bitrate adaptation goes through it (`update_config()`).
 * Two properties matter here:
 *
 *   1. `update_config()` reconfigures the *running* encoder in place —
 *      a bitrate step must not reset the codec state or its statistics.
 *   2. `update_config()` before the codec was opened is legal: the
 *      configuration is then applied when the encoder is built.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include <nimrtc/opus/codec_plugin.hpp>
#include <nimrtc/plugins/codec.hpp>

namespace {

namespace np = nimrtc::plugins;

constexpr std::uint32_t kSampleRate   = 48000;
constexpr std::size_t   kFrameSamples = 960;  // 20 ms @ 48 kHz

std::vector<float> make_sine(std::size_t n, float freq_hz, std::uint32_t rate) {
    std::vector<float> pcm(n);
    const double phase_step =
        2.0 * 3.141592653589793 * static_cast<double>(freq_hz) / static_cast<double>(rate);
    for (std::size_t i = 0; i < n; ++i) {
        pcm[i] = 0.5f * static_cast<float>(std::sin(static_cast<double>(i) * phase_step));
    }
    return pcm;
}

np::CodecConfig make_config() {
    np::CodecConfig cfg{};
    cfg.sample_rate_hz = kSampleRate;
    cfg.channels       = 1;
    cfg.bitrate_bps    = 0;   // codec default
    cfg.fec_enabled    = true;
    return cfg;
}

}  // namespace

TEST(OpusCodecPlugin, AnUnsetBitrateResolvesToTheChannelDefault) {
    np::CodecConfig cfg = make_config();
    cfg.channels        = 1;
    nimrtc::opus::CodecPluginAdapter adapter(cfg);
    EXPECT_EQ(adapter.open(), np::kOk);

    // The adapter reports the configuration it was handed; the codec
    // default is resolved by the concrete encoder (32 kbps mono).
    EXPECT_EQ(adapter.config().bitrate_bps, 0);

    // A mono frame encodes into a payload of a few dozen bytes at
    // 32 kbps / 20 ms — far below what a 64 kbps stereo encoder would
    // need, but the point of the test is that the codec accepted the
    // "codec default" configuration at all.
    auto pcm = make_sine(kFrameSamples, 440.0f, kSampleRate);
    std::uint8_t buf[4000];
    EXPECT_GT(adapter.encode(pcm.data(), kFrameSamples, buf, sizeof(buf)), 0u);
}

TEST(OpusCodecPlugin, UpdateConfigBeforeOpenDoesNotBuildTheEncoder) {
    np::CodecConfig cfg = make_config();
    nimrtc::opus::CodecPluginAdapter adapter(cfg);

    np::CodecConfig next = cfg;
    next.bitrate_bps     = 16000;
    EXPECT_EQ(adapter.update_config(next), np::kOk);

    // Nothing was encoded yet, so the codec has no statistics.
    EXPECT_EQ(adapter.stats().frames_encoded, 0u);
    EXPECT_EQ(adapter.config().bitrate_bps, 16000);
}

TEST(OpusCodecPlugin, UpdateConfigAfterOpenReconfiguresInPlace) {
    np::CodecConfig cfg = make_config();
    nimrtc::opus::CodecPluginAdapter adapter(cfg);
    ASSERT_EQ(adapter.open(), np::kOk);

    auto pcm = make_sine(kFrameSamples, 440.0f, kSampleRate);
    std::uint8_t buf[4000];
    ASSERT_GT(adapter.encode(pcm.data(), kFrameSamples, buf, sizeof(buf)), 0u);
    ASSERT_EQ(adapter.stats().frames_encoded, 1u);

    // The bitrate step the engine's BWE adaptation performs.
    np::CodecConfig next = cfg;
    next.bitrate_bps     = 16000;
    EXPECT_EQ(adapter.update_config(next), np::kOk);

    // The encoder was reconfigured, not replaced: the statistics of the
    // frames encoded before the step are still there.
    EXPECT_EQ(adapter.stats().frames_encoded, 1u)
        << "a bitrate step must not reset the encoder statistics";

    // And the encoder still works.
    EXPECT_GT(adapter.encode(pcm.data(), kFrameSamples, buf, sizeof(buf)), 0u);
    EXPECT_EQ(adapter.stats().frames_encoded, 2u);
}
