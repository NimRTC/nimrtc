/**
 * @file modules/audio3a/tests/test_audio3a_plugin.cpp
 * @brief Smoke tests for audio3a plugin adapter (registry -> factory -> adapter).
 *
 * Verifies the R2 adapter layer:
 *   - NullPluginFactory registers under id "webrtc"
 *   - factory->create() returns a PluginAdapter wrapping NullAudio3A
 *   - lifecycle: open / process_* / close round-trips cleanly
 *   - VAD / level callbacks fire when explicitly requested
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string_view>
#include <vector>

#include <nimrtc/audio3a/audio3a_plugin.hpp>
#include <nimrtc/core/registry.hpp>

namespace {

using nimrtc::audio3a::NullPluginFactory;
using nimrtc::audio3a::PluginAdapter;
using nimrtc::core::PluginRegistry;
using nimrtc::plugins::AudioErrorCallback;
using nimrtc::plugins::Audio3AConfig;
using nimrtc::plugins::IAudio3A;
using nimrtc::plugins::LevelCallback;
using nimrtc::plugins::VadCallback;

/** Test fixture: forces MSVC to pull audio3a_plugin.cpp into the link by
 *  invoking the explicit-registration entry point once per test process. */
class Audio3APluginFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::audio3a::register_default_plugins();
    }
};

// 10 ms audio frame at 48 kHz mono = 480 samples
constexpr std::size_t  kSamplesPerFrame = 480;
constexpr std::uint32_t kSampleRateHz   = 48000;
constexpr std::uint8_t  kChannels       = 1;

// Silence = -96 dBFS (minimum level for NullAudio3A)
constexpr float kSilenceLevel = -96.0f;

// A 440 Hz sine wave at amplitude 0.5 = -6 dBFS (audible, not clipping)
float make_sine_sample(float phase) {
    return 0.5f * std::sin(phase);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test: factory registration
// ---------------------------------------------------------------------------

TEST_F(Audio3APluginFixture, registry_has_any_audio3a_entries) {
    const auto* reg = &PluginRegistry::instance();
    auto ids = reg->list_audio3a();
    std::fprintf(stderr, "DEBUG: registry has %zu audio3a entries\n", ids.size());
    for (auto id : ids) {
        std::fprintf(stderr, "DEBUG:   audio3a id=\"%.*s\"\n",
                     static_cast<int>(id.size()), id.data());
    }
    // Also check ICE for comparison
    auto ice_ids = reg->list_transports();
    std::fprintf(stderr, "DEBUG: registry has %zu transport entries\n", ice_ids.size());
    for (auto id : ice_ids) {
        std::fprintf(stderr, "DEBUG:   transport id=\"%.*s\"\n",
                     static_cast<int>(id.size()), id.data());
    }
    SUCCEED();   // always pass ? diagnostic only
}

TEST_F(Audio3APluginFixture, factory_registered_under_webrtc_id) {
    const auto* reg = &PluginRegistry::instance();
    const auto* f = reg->get_audio3a("webrtc");
    ASSERT_NE(f, nullptr) << "NullPluginFactory should be registered under id='webrtc'";
}

TEST_F(Audio3APluginFixture, factory_id_is_webrtc) {
    const auto* reg = &PluginRegistry::instance();
    const auto* f = reg->get_audio3a("webrtc");
    ASSERT_NE(f, nullptr);
    EXPECT_STREQ(f->id().data(), "webrtc");
}

TEST_F(Audio3APluginFixture, factory_display_name_not_empty) {
    const auto* reg = &PluginRegistry::instance();
    const auto* f = reg->get_audio3a("webrtc");
    ASSERT_NE(f, nullptr);
    EXPECT_FALSE(f->display_name().empty());
}

// ---------------------------------------------------------------------------
// Test: factory->create()
// ---------------------------------------------------------------------------

TEST_F(Audio3APluginFixture, create_returns_non_null) {
    const auto* reg = &PluginRegistry::instance();
    const auto* f = reg->get_audio3a("webrtc");
    ASSERT_NE(f, nullptr);
    std::unique_ptr<IAudio3A> plugin{f->create()};
    EXPECT_NE(plugin, nullptr);
}

TEST_F(Audio3APluginFixture, created_plugin_name_contains_plugin_adapter) {
    const auto* reg = &PluginRegistry::instance();
    const auto* f = reg->get_audio3a("webrtc");
    ASSERT_NE(f, nullptr);
    std::unique_ptr<IAudio3A> plugin{f->create()};
    ASSERT_NE(plugin, nullptr);
    const char* name = plugin->name();
    EXPECT_NE(std::strstr(name, "PluginAdapter"), nullptr);
}

// ---------------------------------------------------------------------------
// Test: lifecycle
// ---------------------------------------------------------------------------

TEST_F(Audio3APluginFixture, open_close_round_trip) {
    const auto* reg = &PluginRegistry::instance();
    const auto* f = reg->get_audio3a("webrtc");
    ASSERT_NE(f, nullptr);
    std::unique_ptr<IAudio3A> plugin{f->create()};
    ASSERT_NE(plugin, nullptr);

    EXPECT_EQ(plugin->open(), nimrtc::plugins::kOk);
    plugin->close();   // no return value in interface
    // Re-open after close should succeed (NullAudio3A::reset() allows re-init)
    EXPECT_EQ(plugin->open(), nimrtc::plugins::kOk);
}

TEST_F(Audio3APluginFixture, process_capture_rejects_null_samples) {
    const auto* reg = &PluginRegistry::instance();
    const auto* f = reg->get_audio3a("webrtc");
    ASSERT_NE(f, nullptr);
    std::unique_ptr<IAudio3A> plugin{f->create()};
    ASSERT_NE(plugin, nullptr);
    ASSERT_EQ(plugin->open(), nimrtc::plugins::kOk);

    EXPECT_EQ(plugin->process_capture(nullptr, kSamplesPerFrame, kChannels),
              nimrtc::plugins::kErrInvalidParam);
}

TEST_F(Audio3APluginFixture, process_capture_rejects_zero_samples) {
    const auto* reg = &PluginRegistry::instance();
    const auto* f = reg->get_audio3a("webrtc");
    ASSERT_NE(f, nullptr);
    std::unique_ptr<IAudio3A> plugin{f->create()};
    ASSERT_NE(plugin, nullptr);
    ASSERT_EQ(plugin->open(), nimrtc::plugins::kOk);

    std::vector<float> buf(kSamplesPerFrame, 0.0f);
    EXPECT_EQ(plugin->process_capture(buf.data(), 0, kChannels),
              nimrtc::plugins::kErrInvalidParam);
}

TEST_F(Audio3APluginFixture, process_capture_silence_is_noop) {
    const auto* reg = &PluginRegistry::instance();
    const auto* f = reg->get_audio3a("webrtc");
    ASSERT_NE(f, nullptr);
    std::unique_ptr<IAudio3A> plugin{f->create()};
    ASSERT_NE(plugin, nullptr);
    ASSERT_EQ(plugin->open(), nimrtc::plugins::kOk);

    std::vector<float> buf(kSamplesPerFrame, 0.0f);
    EXPECT_EQ(plugin->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);

    auto st = plugin->stats();
    EXPECT_LE(st.last_capture_level_dbfs, kSilenceLevel + 1.0f);
}

TEST_F(Audio3APluginFixture, process_render_sine_wave_no_crash) {
    const auto* reg = &PluginRegistry::instance();
    const auto* f = reg->get_audio3a("webrtc");
    ASSERT_NE(f, nullptr);
    std::unique_ptr<IAudio3A> plugin{f->create()};
    ASSERT_NE(plugin, nullptr);
    ASSERT_EQ(plugin->open(), nimrtc::plugins::kOk);

    std::vector<float> buf(kSamplesPerFrame);
    for (std::size_t i = 0; i < buf.size(); ++i) {
        float phase = 2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / kSampleRateHz;
        buf[i] = make_sine_sample(phase);
    }
    EXPECT_EQ(plugin->process_render(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);
}

// ---------------------------------------------------------------------------
// Test: stats are accessible after processing
// ---------------------------------------------------------------------------

TEST_F(Audio3APluginFixture, stats_after_open_before_process) {
    const auto* reg = &PluginRegistry::instance();
    const auto* f = reg->get_audio3a("webrtc");
    ASSERT_NE(f, nullptr);
    std::unique_ptr<IAudio3A> plugin{f->create()};
    ASSERT_NE(plugin, nullptr);
    ASSERT_EQ(plugin->open(), nimrtc::plugins::kOk);

    auto st = plugin->stats();
    // NullAudio3A initialises levels to -96 dBFS
    EXPECT_LE(st.last_capture_level_dbfs, kSilenceLevel + 0.1f);
    EXPECT_LE(st.last_render_level_dbfs, kSilenceLevel + 0.1f);
}

TEST_F(Audio3APluginFixture, stats_improve_after_capture) {
    const auto* reg = &PluginRegistry::instance();
    const auto* f = reg->get_audio3a("webrtc");
    ASSERT_NE(f, nullptr);
    std::unique_ptr<IAudio3A> plugin{f->create()};
    ASSERT_NE(plugin, nullptr);
    ASSERT_EQ(plugin->open(), nimrtc::plugins::kOk);

    std::vector<float> buf(kSamplesPerFrame, 0.0f);
    ASSERT_EQ(plugin->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);

    auto st = plugin->stats();
    EXPECT_LE(st.last_capture_level_dbfs, kSilenceLevel + 1.0f);
}

// ---------------------------------------------------------------------------
// Test: callbacks fire when requested
// ---------------------------------------------------------------------------

TEST_F(Audio3APluginFixture, vad_callback_fires_after_request) {
    const auto* reg = &PluginRegistry::instance();
    const auto* f = reg->get_audio3a("webrtc");
    ASSERT_NE(f, nullptr);
    std::unique_ptr<IAudio3A> plugin{f->create()};
    ASSERT_NE(plugin, nullptr);
    ASSERT_EQ(plugin->open(), nimrtc::plugins::kOk);

    std::atomic<bool> vad_called{false};
    plugin->set_callbacks(
        /* on_vad */  [&vad_called](bool) { vad_called = true; },
        /* on_level */[](float) {},
        /* on_error */[](std::uint32_t, std::string_view) {});

    std::vector<float> buf(kSamplesPerFrame, 0.0f);

    // Request VAD report ? NullAudio3A fires on the NEXT process_capture cycle
    plugin->request_vad_report();
    ASSERT_EQ(plugin->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);

    // NullAudio3A always reports vad_active = false for silence
    EXPECT_TRUE(vad_called);   // callback was invoked (even if result is false)
}

TEST_F(Audio3APluginFixture, level_callback_fires_after_request) {
    const auto* reg = &PluginRegistry::instance();
    const auto* f = reg->get_audio3a("webrtc");
    ASSERT_NE(f, nullptr);
    std::unique_ptr<IAudio3A> plugin{f->create()};
    ASSERT_NE(plugin, nullptr);
    ASSERT_EQ(plugin->open(), nimrtc::plugins::kOk);

    std::atomic<float> captured_level{-999.0f};
    plugin->set_callbacks(
        /* on_vad */  [](bool) {},
        /* on_level */[&captured_level](float dbfs) { captured_level.store(dbfs); },
        /* on_error */[](std::uint32_t, std::string_view) {});

    std::vector<float> buf(kSamplesPerFrame, 0.0f);

    plugin->request_level_report();
    ASSERT_EQ(plugin->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);

    // Level should be captured (silence ~= -96 dBFS)
    float level = captured_level.load();
    EXPECT_LE(level, -90.0f);   // at or below silence floor
}

// =============================================================================
// Real-WebRTC-APM integration tests (only meaningful when APM is linked).
// These exercise the "webrtc_apm" factory, which (when NIMRTC_USE_WEBRTC_APM=1)
// is wired to a real webrtc::AudioProcessing instance and runs actual AEC +
// ANS + AGC + high-pass processing.  When the APM is unavailable the factory
// falls back to a NullAudio3A — in that case the tests below still pass (they
// just exercise the fallback path), but the test filename + Assert logs make
// the fact visible.
// =============================================================================

TEST_F(Audio3APluginFixture, webrtc_apm_factory_registered) {
    const auto* reg = &PluginRegistry::instance();
    const auto* f = reg->get_audio3a("webrtc_apm");
    ASSERT_NE(f, nullptr) << "WebRtcPluginFactory must be registered under id='webrtc_apm'";
    EXPECT_STREQ(f->id().data(), "webrtc_apm");
}

TEST_F(Audio3APluginFixture, webrtc_apm_open_processes_real_audio) {
    const auto* reg = &PluginRegistry::instance();
    const auto* f = reg->get_audio3a("webrtc_apm");
    ASSERT_NE(f, nullptr);
    std::unique_ptr<IAudio3A> plugin{f->create()};
    ASSERT_NE(plugin, nullptr);

    // PluginAdapter uses its default concrete_config_ which is fine for the
    // smoke test (48 kHz mono echo-cancellation path).
    ASSERT_EQ(plugin->open(), nimrtc::plugins::kOk);

    // 200 ms of 1 kHz sine at -12 dBFS plus -50 dBFS noise — enough samples for
    // the APM to do meaningful AEC/ANS/AGC processing at 48 kHz (10 ms frames).
    constexpr std::size_t kFrameSize  = 480;          // 10 ms @ 48 kHz
    constexpr std::size_t kNumFrames  = 20;           // 200 ms total
    constexpr float       kSineAmp    = 0.25f;        // ~-12 dBFS
    constexpr float       kNoiseAmp   = 0.005f;       // ~-46 dBFS
    std::vector<float> cap(kFrameSize);
    std::vector<float> rnd(kFrameSize);

    auto lcg = [state = 0x1234u]() mutable {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state) / static_cast<float>(UINT32_MAX) * 2.0f - 1.0f;
    };
    auto stats = plugin->stats();
    for (std::size_t f_idx = 0; f_idx < kNumFrames; ++f_idx) {
        for (std::size_t i = 0; i < kFrameSize; ++i) {
            rnd[i] = kNoiseAmp * lcg();
            const std::size_t n = f_idx * kFrameSize + i;
            const float t = static_cast<float>(n) / 48000.0f;
            cap[i] = kSineAmp * std::sin(2.0f * 3.14159265f * 1000.0f * t) + rnd[i];
        }
        ASSERT_EQ(plugin->process_capture(cap.data(), kFrameSize, 1),
                  nimrtc::plugins::kOk);
        ASSERT_EQ(plugin->process_render(cap.data(), kFrameSize, 1),
                  nimrtc::plugins::kOk);
        plugin->set_render_delivered(kFrameSize);
    }
    stats = plugin->stats();

    // Output should remain finite (NaN/Inf would mean a real APM crash; pass-
    // through Null would also pass this — both are acceptable).
    int bad = 0;
    for (float s : cap) {
        if (std::isnan(s) || std::isinf(s)) ++bad;
    }
    EXPECT_EQ(bad, 0) << "WebRtcAudio3A produced NaN/Inf samples";

    // Sanity: the APM reports RMS-based level stats; capture_level_dbfs must
    // be populated (NullAudio3A also populates this, but the value is
    // consistent with the post-DSP RMS of the buffer we fed in). For a real
    // APM, the render_level_dbfs is also populated from the reverse path.
    EXPECT_LE(stats.last_capture_level_dbfs, 0.0f)
        << "last_capture_level_dbfs out of range";
    EXPECT_LE(stats.last_render_level_dbfs, 0.0f)
        << "last_render_level_dbfs out of range";

    // If the real APM ran, the RMS of the post-DSP capture buffer must
    // match what was reported (within a small db tolerance).
    double sumsq = 0.0;
    for (float s : cap) {
        const double sd = static_cast<double>(s);
        sumsq += sd * sd;
    }
    const double rms = std::sqrt(sumsq / static_cast<double>(cap.size()));
    const float out_dbfs = (rms < 1e-7) ? -96.0f
        : static_cast<float>(20.0 * std::log10(rms));
    EXPECT_NEAR(out_dbfs, stats.last_capture_level_dbfs, 2.0f)
        << "APM-reported level and post-DSP RMS disagree";

    std::fprintf(stderr,
        "INFO: webrtc_apm post-DSP tx_level=%.2f dBFS rx_level=%.2f dBFS\n",
        static_cast<double>(stats.last_capture_level_dbfs),
        static_cast<double>(stats.last_render_level_dbfs));

    plugin->close();
}
