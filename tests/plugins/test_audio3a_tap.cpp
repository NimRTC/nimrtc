/**
 * @file tests/plugins/test_audio3a_tap.cpp
 * @brief PCM tap round-trip test for IAudio3A (section 8.7).
 *
 * Verifies:
 *   - Pre-tap receives raw mic PCM before 3A processing
 *   - Post-tap receives 3A-cleaned PCM after processing
 *   - int16_t post-tap converts float to int16 correctly
 *   - Tap installation/uninstallation is idempotent
 *   - Taps are invoked the correct number of times (frame count)
 *
 * @note P2 - requires IAudio3A::set_pre_process_tap / set_post_process_tap.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <nimrtc/audio3a/audio3a_plugin.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/plugins/audio3a.hpp>

namespace {

using nimrtc::audio3a::PluginAdapter;
using nimrtc::core::PluginRegistry;
using nimrtc::plugins::Audio3AConfig;
using nimrtc::plugins::IAudio3A;
using nimrtc::plugins::PcmFrameMetadata;
using nimrtc::plugins::PcmTapCallback;
using nimrtc::plugins::PcmTapCallbackI16;

class Audio3ATapFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::audio3a::register_default_plugins();
    }

    void SetUp() override {
        const auto* reg = &PluginRegistry::instance();
        const auto* f = reg->get_audio3a("webrtc");
        ASSERT_NE(f, nullptr);
        plugin_.reset(f->create());
        ASSERT_NE(plugin_, nullptr);
        ASSERT_EQ(plugin_->open(), nimrtc::plugins::kOk);
    }

    void TearDown() override {
        if (plugin_) plugin_->close();
    }

    std::unique_ptr<IAudio3A> plugin_;

    // 10 ms at 48 kHz mono = 480 samples
    static constexpr std::size_t  kSamplesPerFrame = 480;
    static constexpr std::uint32_t kSampleRateHz   = 48000;
    static constexpr std::uint8_t  kChannels       = 1;
};

// ---------------------------------------------------------------------------
// Test: pre-tap invoked before 3A
// ---------------------------------------------------------------------------

TEST_F(Audio3ATapFixture, pre_tap_invoked_on_process_capture) {
    std::atomic<int> pre_count{0};
    plugin_->set_pre_process_tap(
        [&pre_count](const float*, const PcmFrameMetadata&) {
            pre_count.fetch_add(1);
        });

    std::vector<float> buf(kSamplesPerFrame, 0.0f);
    ASSERT_EQ(plugin_->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);
    EXPECT_EQ(pre_count.load(), 1);

    // Second call should also trigger tap
    ASSERT_EQ(plugin_->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);
    EXPECT_EQ(pre_count.load(), 2);
}

TEST_F(Audio3ATapFixture, pre_tap_receives_correct_metadata) {
    std::atomic<PcmFrameMetadata> captured_meta{};
    plugin_->set_pre_process_tap(
        [&captured_meta](const float*, const PcmFrameMetadata& meta) {
            captured_meta.store(meta);
        });

    std::vector<float> buf(kSamplesPerFrame, 0.0f);
    ASSERT_EQ(plugin_->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);

    PcmFrameMetadata m = captured_meta.load();
    EXPECT_EQ(m.num_samples, kSamplesPerFrame);
    EXPECT_EQ(m.num_channels, kChannels);
    EXPECT_EQ(m.sample_rate_hz, kSampleRateHz);
}

TEST_F(Audio3ATapFixture, pre_tap_receives_raw_samples_before_3a) {
    // NullAudio3A is a passthrough — pre and post tap see identical PCM.
    // This test proves the tap receives the unmodified input signal.
    std::vector<float> buf(kSamplesPerFrame);
    for (std::size_t i = 0; i < buf.size(); ++i) {
        float phase = 2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / kSampleRateHz;
        buf[i] = 0.5f * std::sin(phase);
    }

    std::vector<float> captured_samples;
    std::mutex captured_samples_mu;
    plugin_->set_pre_process_tap(
        [&captured_samples, &captured_samples_mu](const float* samples, const PcmFrameMetadata& meta) {
            std::vector<float> copy(samples, samples + meta.num_samples * meta.num_channels);
            std::lock_guard<std::mutex> lk(captured_samples_mu);
            captured_samples = std::move(copy);
        });

    ASSERT_EQ(plugin_->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);

    std::vector<float> captured;
    {
        std::lock_guard<std::mutex> lk(captured_samples_mu);
        captured = captured_samples;
    }
    ASSERT_FALSE(captured.empty());
    // Verify the first 10 samples match the sine input exactly (passthrough).
    for (std::size_t i = 0; i < 10 && i < captured.size(); ++i) {
        EXPECT_FLOAT_EQ(captured[i], buf[i])
            << "pre-tap must see unmodified input signal";
    }
}

TEST_F(Audio3ATapFixture, pre_tap_nullptr_uninstalls) {
    std::atomic<int> count{0};
    plugin_->set_pre_process_tap(
        [&count](const float*, const PcmFrameMetadata&) { count.fetch_add(1); });

    std::vector<float> buf(kSamplesPerFrame, 0.0f);
    ASSERT_EQ(plugin_->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);
    EXPECT_EQ(count.load(), 1);

    // Uninstall
    plugin_->set_pre_process_tap(nullptr);

    ASSERT_EQ(plugin_->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);
    EXPECT_EQ(count.load(), 1) << "tap should not fire after uninstall";
}

// ---------------------------------------------------------------------------
// Test: post-tap invoked after 3A
// ---------------------------------------------------------------------------

TEST_F(Audio3ATapFixture, post_tap_invoked_on_process_capture) {
    std::atomic<int> post_count{0};
    plugin_->set_post_process_tap(
        [&post_count](const float*, const PcmFrameMetadata&) { post_count.fetch_add(1); },
        nullptr
    );

    std::vector<float> buf(kSamplesPerFrame, 0.0f);
    ASSERT_EQ(plugin_->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);
    EXPECT_EQ(post_count.load(), 1);
}

TEST_F(Audio3ATapFixture, post_tap_receives_correct_metadata) {
    std::atomic<PcmFrameMetadata> captured_meta{};
    plugin_->set_post_process_tap(
        [&captured_meta](const float*, const PcmFrameMetadata& meta) {
            captured_meta.store(meta);
        },
        nullptr
    );

    std::vector<float> buf(kSamplesPerFrame, 0.0f);
    ASSERT_EQ(plugin_->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);

    PcmFrameMetadata m = captured_meta.load();
    EXPECT_EQ(m.num_samples, kSamplesPerFrame);
    EXPECT_EQ(m.num_channels, kChannels);
    EXPECT_EQ(m.sample_rate_hz, kSampleRateHz);
}

TEST_F(Audio3ATapFixture, post_tap_nullptr_uninstalls) {
    std::atomic<int> count{0};
    plugin_->set_post_process_tap(
        [&count](const float*, const PcmFrameMetadata&) { count.fetch_add(1); },
        nullptr
    );

    std::vector<float> buf(kSamplesPerFrame, 0.0f);
    ASSERT_EQ(plugin_->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);
    EXPECT_EQ(count.load(), 1);

    // Uninstall: nullptr for both taps
    plugin_->set_post_process_tap(nullptr, nullptr);

    ASSERT_EQ(plugin_->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);
    EXPECT_EQ(count.load(), 1) << "post-tap should not fire after uninstall";
}

// ---------------------------------------------------------------------------
// Test: int16_t post-tap
// ---------------------------------------------------------------------------

TEST_F(Audio3ATapFixture, post_tap_i16_converts_float_to_int16) {
    // Verify: round(s * 32767.0f) saturating to INT16_MIN / INT16_MAX.
    // Use a ramp so every integer value in [0, 32767] maps to a predictable int16.
    std::vector<float> buf(kSamplesPerFrame);
    for (std::size_t i = 0; i < buf.size(); ++i) {
        // Values strictly in [0, 1] — maps 1:1 with int16_t.
        buf[i] = static_cast<float>(i) / static_cast<float>(kSamplesPerFrame - 1);
    }

    std::vector<std::int16_t> captured_i16;
    std::mutex captured_i16_mu;
    plugin_->set_post_process_tap(
        nullptr,
        [&captured_i16, &captured_i16_mu](const std::int16_t* samples, const PcmFrameMetadata& meta) {
            std::vector<std::int16_t> copy(samples, samples + meta.num_samples * meta.num_channels);
            std::lock_guard<std::mutex> lk(captured_i16_mu);
            captured_i16 = std::move(copy);
        }
    );

    ASSERT_EQ(plugin_->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);

    std::vector<std::int16_t> captured;
    {
        std::lock_guard<std::mutex> lk(captured_i16_mu);
        captured = captured_i16;
    }
    ASSERT_FALSE(captured.empty());

    // Spot-check: i=240 → buf[240] ≈ 0.5 → expected_i16 ≈ 16384.
    EXPECT_NEAR(captured[240], 16384, 2)
        << "float-to-int16 conversion at 0.5 amplitude";

    // i=0 → buf[0] = 0 → expected_i16 = 0.
    EXPECT_EQ(captured[0], 0) << "zero float maps to zero int16";

    // i=479 → buf[479] ≈ 1.0 → expected_i16 = 32767.
    EXPECT_EQ(captured[479], 32767) << "1.0 float maps to INT16_MAX";
}

TEST_F(Audio3ATapFixture, post_tap_i16_clipping_at_boundaries) {
    std::vector<float> buf(kSamplesPerFrame);
    for (std::size_t i = 0; i < buf.size(); ++i) {
        float phase = 2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / kSampleRateHz;
        // Scale by 1.5 so peaks exceed ±1.0 and the int16 clamp fires
        // (sin() alone stays strictly inside [-1, +1) and never clips).
        buf[i] = std::sin(phase) * 1.5f;
    }

    std::atomic<bool> clipping_seen{false};
    plugin_->set_post_process_tap(
        nullptr,
        [&clipping_seen](const std::int16_t* samples, const PcmFrameMetadata& meta) {
            for (std::size_t i = 0; i < meta.num_samples * meta.num_channels; ++i) {
                if (samples[i] == 32767 || samples[i] == -32768) {
                    clipping_seen = true;
                    return;
                }
            }
        }
    );

    ASSERT_EQ(plugin_->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);
    EXPECT_TRUE(clipping_seen.load()) << "amplitude=1.0 should trigger clipping";
}

// ---------------------------------------------------------------------------
// Test: round-trip count verification
// ---------------------------------------------------------------------------

TEST_F(Audio3ATapFixture, roundtrip_100_frames_tap_invocation_count) {
    std::atomic<int> pre_count{0};
    std::atomic<int> post_count{0};
    plugin_->set_pre_process_tap(
        [&pre_count](const float*, const PcmFrameMetadata&) { pre_count.fetch_add(1); });
    plugin_->set_post_process_tap(
        [&post_count](const float*, const PcmFrameMetadata&) { post_count.fetch_add(1); },
        nullptr
    );

    std::vector<float> buf(kSamplesPerFrame, 0.0f);
    for (int i = 0; i < 100; ++i) {
        ASSERT_EQ(plugin_->process_capture(buf.data(), buf.size(), kChannels),
                  nimrtc::plugins::kOk);
    }

    EXPECT_EQ(pre_count.load(), 100) << "pre-tap should be invoked exactly 100 times";
    EXPECT_EQ(post_count.load(), 100) << "post-tap should be invoked exactly 100 times";
}

TEST_F(Audio3ATapFixture, both_taps_fire_together) {
    std::atomic<int> pre_seen{0};
    std::atomic<int> post_seen{0};

    plugin_->set_pre_process_tap(
        [&pre_seen](const float*, const PcmFrameMetadata&) { pre_seen.fetch_add(1); });
    plugin_->set_post_process_tap(
        [&post_seen](const float*, const PcmFrameMetadata&) {
            post_seen.fetch_add(1);
        },
        nullptr
    );

    std::vector<float> buf(kSamplesPerFrame, 0.0f);
    ASSERT_EQ(plugin_->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);

    EXPECT_EQ(pre_seen.load(), 1);
    EXPECT_EQ(post_seen.load(), 1);
    EXPECT_EQ(pre_seen.load(), post_seen.load());
}

// ---------------------------------------------------------------------------
// Test: pre tap is invoked before post tap (call order per RFC-001 §2.4)
// ---------------------------------------------------------------------------

TEST_F(Audio3ATapFixture, pre_tap_before_post_tap_per_frame) {
    std::vector<std::string> call_order;
    std::mutex mu;

    plugin_->set_pre_process_tap(
        [&call_order, &mu](const float*, const PcmFrameMetadata&) {
            std::lock_guard<std::mutex> lk(mu);
            call_order.push_back("pre");
        });
    plugin_->set_post_process_tap(
        [&call_order, &mu](const float*, const PcmFrameMetadata&) {
            std::lock_guard<std::mutex> lk(mu);
            call_order.push_back("post-f32");
        },
        [&call_order, &mu](const std::int16_t*, const PcmFrameMetadata&) {
            std::lock_guard<std::mutex> lk(mu);
            call_order.push_back("post-i16");
        });

    std::vector<float> buf(kSamplesPerFrame, 0.0f);
    ASSERT_EQ(plugin_->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);

    ASSERT_EQ(call_order.size(), 3u);
    EXPECT_EQ(call_order[0], "pre")      << "pre tap must be first";
    EXPECT_EQ(call_order[1], "post-f32") << "post float tap must be second";
    EXPECT_EQ(call_order[2], "post-i16") << "post int16 tap must be third";
}

// ---------------------------------------------------------------------------
// Test: int16-only tap (no float tap installed)
// ---------------------------------------------------------------------------

TEST_F(Audio3ATapFixture, int16_tap_without_float_tap) {
    std::atomic<bool> called{false};
    plugin_->set_post_process_tap(
        nullptr,  // no float tap
        [&called](const std::int16_t* samples, const PcmFrameMetadata& meta) {
            called = true;
            EXPECT_NE(samples, nullptr);
            EXPECT_EQ(meta.num_samples, kSamplesPerFrame);
        });

    std::vector<float> buf(kSamplesPerFrame, 0.0f);
    ASSERT_EQ(plugin_->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);
    EXPECT_TRUE(called.load()) << "int16 tap must fire even without float tap";
}

// ---------------------------------------------------------------------------
// Test: re-install after uninstall
// ---------------------------------------------------------------------------

TEST_F(Audio3ATapFixture, reinstall_after_uninstall) {
    std::atomic<int> count{0};
    plugin_->set_pre_process_tap(
        [&count](const float*, const PcmFrameMetadata&) { count.fetch_add(1); });

    std::vector<float> buf(kSamplesPerFrame, 0.0f);
    ASSERT_EQ(plugin_->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);
    ASSERT_EQ(count.load(), 1);

    // Uninstall.
    plugin_->set_pre_process_tap(nullptr);
    ASSERT_EQ(plugin_->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);
    ASSERT_EQ(count.load(), 1);

    // Re-install.
    plugin_->set_pre_process_tap(
        [&count](const float*, const PcmFrameMetadata&) { count.fetch_add(1); });
    ASSERT_EQ(plugin_->process_capture(buf.data(), buf.size(), kChannels),
              nimrtc::plugins::kOk);
    EXPECT_EQ(count.load(), 2) << "re-installed tap must fire again";
}

// ---------------------------------------------------------------------------
// Test: thread-safety — concurrent install/uninstall vs process calls
// ---------------------------------------------------------------------------

TEST_F(Audio3ATapFixture, concurrent_install_uninstall_no_crash) {
    std::atomic<bool> running{true};
    std::atomic<int>  tap_fired{0};

    std::thread bg([&]() {
        while (running.load()) {
            plugin_->set_pre_process_tap(nullptr);
            plugin_->set_pre_process_tap(
                [&tap_fired](const float*, const PcmFrameMetadata&) {
                    tap_fired.fetch_add(1);
                });
        }
    });

    // Main thread: process many frames while bg thread races on tap registration.
    std::vector<float> buf(kSamplesPerFrame, 0.0f);
    for (int i = 0; i < 50; ++i) {
        ASSERT_EQ(plugin_->process_capture(buf.data(), buf.size(), kChannels),
                  nimrtc::plugins::kOk);
    }

    running = false;
    bg.join();

    // No crash, no hang. tap_fired is approximate (race allowed) but > 0
    // if the install hit at least once before the join.
    std::fprintf(stderr, "INFO: tap fired %d times during concurrent install/uninstall\n",
                 tap_fired.load());
}

}  // namespace
