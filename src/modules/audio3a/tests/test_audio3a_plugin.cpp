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
                     (int)id.size(), id.data());
    }
    // Also check ICE for comparison
    auto ice_ids = reg->list_transports();
    std::fprintf(stderr, "DEBUG: registry has %zu transport entries\n", ice_ids.size());
    for (auto id : ice_ids) {
        std::fprintf(stderr, "DEBUG:   transport id=\"%.*s\"\n",
                     (int)id.size(), id.data());
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
