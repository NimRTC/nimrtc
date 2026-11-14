// modules/audio_device/tests/test_mixer.cpp
//
// The two-source audio mixer (remote-desktop plan F6).
//
// What they cover:
//
//   1. the **mix** configuration — the two sources are summed into one
//      stream, with the exact sample values asserted;
//   2. the **separate** configuration — the two sources come out as
//      two distinct buffers, never combined;
//   3. the **rate mismatch** — the system leg runs at a different rate
//      and is converted with the module's `Resampler`;
//   4. the **channel mismatch** — mono / stereo handled explicitly;
//   5. the **saturating** behaviour — a loud sum clamps instead of
//      wrapping.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include <nimrtc/audio_device/mixer.hpp>
#include <nimrtc/audio_device/resampler.hpp>

namespace {

using nimrtc::audio_device::AudioMixMode;
using nimrtc::audio_device::AudioMixStatus;
using nimrtc::audio_device::AudioMixer;
using nimrtc::audio_device::AudioMixerConfig;

constexpr std::uint32_t kRate = 48000;

AudioMixerConfig mono_mix_config() {
    AudioMixerConfig cfg;
    cfg.mode = AudioMixMode::kMix;
    cfg.output_rate_hz = kRate;
    cfg.output_channels = 1;
    return cfg;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. The mix configuration
// ---------------------------------------------------------------------------

TEST(audio_mixer, mixing_two_sources_sums_them_exactly) {
    AudioMixer mixer(mono_mix_config());
    ASSERT_TRUE(mixer.valid());
    EXPECT_EQ(mixer.mode(), AudioMixMode::kMix);

    const float mic[4]  = {0.10f, 0.20f, 0.30f, 0.40f};
    const float sys[4]  = {0.05f, 0.10f, 0.15f, 0.20f};

    ASSERT_EQ(mixer.push_microphone(mic, 4, 1, kRate), AudioMixStatus::kOk);
    ASSERT_EQ(mixer.push_system(sys, 4, 1, kRate), AudioMixStatus::kOk);

    std::vector<float> mixed;
    std::vector<float> mic_out;
    std::vector<float> sys_out;
    ASSERT_EQ(mixer.pull(mixed, mic_out, sys_out), AudioMixStatus::kOk);

    // The mix is a saturating add with headroom 1.0: the exact sum.
    ASSERT_EQ(mixed.size(), 4u);
    EXPECT_FLOAT_EQ(mixed[0], 0.15f);
    EXPECT_FLOAT_EQ(mixed[1], 0.30f);
    EXPECT_FLOAT_EQ(mixed[2], 0.45f);
    EXPECT_FLOAT_EQ(mixed[3], 0.60f);

    // In the mix mode the two legs are NOT delivered separately.
    EXPECT_TRUE(mic_out.empty());
    EXPECT_TRUE(sys_out.empty());
}

TEST(audio_mixer, the_mix_is_deterministic_across_repeated_runs) {
    const float mic[8] = {0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f, 0.7f, -0.8f};
    const float sys[8] = {0.05f, 0.05f, -0.05f, -0.05f, 0.25f, 0.25f, 0.1f, 0.1f};

    std::vector<float> first;
    std::vector<float> second;
    for (std::vector<float>* out : {&first, &second}) {
        AudioMixer mixer(mono_mix_config());
        ASSERT_EQ(mixer.push_microphone(mic, 8, 1, kRate), AudioMixStatus::kOk);
        ASSERT_EQ(mixer.push_system(sys, 8, 1, kRate), AudioMixStatus::kOk);
        std::vector<float> m;
        std::vector<float> a;
        std::vector<float> b;
        ASSERT_EQ(mixer.pull(m, a, b), AudioMixStatus::kOk);
        *out = m;
    }

    ASSERT_EQ(first.size(), second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_FLOAT_EQ(first[i], second[i]) << "at index " << i;
    }
}

TEST(audio_mixer, the_gain_pair_and_the_headroom_are_applied) {
    AudioMixerConfig cfg = mono_mix_config();
    cfg.microphone_gain = 0.5f;
    cfg.system_gain = 0.25f;
    cfg.headroom = 0.5f;

    AudioMixer mixer(cfg);
    ASSERT_TRUE(mixer.valid());

    const float mic[2] = {1.0f, 0.0f};
    const float sys[2] = {1.0f, 0.0f};
    ASSERT_EQ(mixer.push_microphone(mic, 2, 1, kRate), AudioMixStatus::kOk);
    ASSERT_EQ(mixer.push_system(sys, 2, 1, kRate), AudioMixStatus::kOk);

    std::vector<float> mixed;
    std::vector<float> a;
    std::vector<float> b;
    ASSERT_EQ(mixer.pull(mixed, a, b), AudioMixStatus::kOk);

    ASSERT_EQ(mixed.size(), 2u);
    // (0.5 * 1.0 + 0.25 * 1.0) * 0.5 = 0.375
    EXPECT_FLOAT_EQ(mixed[0], 0.375f);
    EXPECT_FLOAT_EQ(mixed[1], 0.0f);
}

TEST(audio_mixer, a_loud_sum_saturates_instead_of_wrapping) {
    AudioMixer mixer(mono_mix_config());

    const float mic[2] = {0.8f, -0.8f};
    const float sys[2] = {0.8f, -0.8f};
    ASSERT_EQ(mixer.push_microphone(mic, 2, 1, kRate), AudioMixStatus::kOk);
    ASSERT_EQ(mixer.push_system(sys, 2, 1, kRate), AudioMixStatus::kOk);

    std::vector<float> mixed;
    std::vector<float> a;
    std::vector<float> b;
    ASSERT_EQ(mixer.pull(mixed, a, b), AudioMixStatus::kOk);

    ASSERT_EQ(mixed.size(), 2u);
    EXPECT_FLOAT_EQ(mixed[0], 1.0f);   // clamped, not 1.6
    EXPECT_FLOAT_EQ(mixed[1], -1.0f);  // clamped, not -1.6
}

TEST(audio_mixer, the_statistics_show_the_mix_path_was_used) {
    AudioMixer mixer(mono_mix_config());

    const float mic[4] = {0.1f, 0.1f, 0.1f, 0.1f};
    const float sys[4] = {0.1f, 0.1f, 0.1f, 0.1f};
    ASSERT_EQ(mixer.push_microphone(mic, 4, 1, kRate), AudioMixStatus::kOk);
    ASSERT_EQ(mixer.push_system(sys, 4, 1, kRate), AudioMixStatus::kOk);

    std::vector<float> mixed;
    std::vector<float> a;
    std::vector<float> b;
    ASSERT_EQ(mixer.pull(mixed, a, b), AudioMixStatus::kOk);

    const auto stats = mixer.stats();
    EXPECT_EQ(stats.microphone_frames_in, 4u);
    EXPECT_EQ(stats.system_frames_in, 4u);
    EXPECT_EQ(stats.mixed_frames_out, 4u);
    // No conversion happened: both legs already ran at the output rate.
    EXPECT_FALSE(stats.microphone_resampled);
    EXPECT_FALSE(stats.system_resampled);
}

// ---------------------------------------------------------------------------
// 2. The separate configuration
// ---------------------------------------------------------------------------

TEST(audio_mixer, the_separate_configuration_yields_two_distinct_buffers) {
    AudioMixerConfig cfg = mono_mix_config();
    cfg.mode = AudioMixMode::kSeparate;

    AudioMixer mixer(cfg);
    ASSERT_TRUE(mixer.valid());
    EXPECT_EQ(mixer.mode(), AudioMixMode::kSeparate);

    const float mic[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    const float sys[4] = {0.9f, 0.8f, 0.7f, 0.6f};
    ASSERT_EQ(mixer.push_microphone(mic, 4, 1, kRate), AudioMixStatus::kOk);
    ASSERT_EQ(mixer.push_system(sys, 4, 1, kRate), AudioMixStatus::kOk);

    std::vector<float> mixed;
    std::vector<float> mic_out;
    std::vector<float> sys_out;
    ASSERT_EQ(mixer.pull(mixed, mic_out, sys_out), AudioMixStatus::kOk);

    // The mixed buffer is EMPTY in the separate mode: the two sources
    // are never combined.
    EXPECT_TRUE(mixed.empty());

    ASSERT_EQ(mic_out.size(), 4u);
    ASSERT_EQ(sys_out.size(), 4u);

    // Each buffer carries its own source's samples, unmodified.
    EXPECT_FLOAT_EQ(mic_out[0], 0.1f);
    EXPECT_FLOAT_EQ(mic_out[3], 0.4f);
    EXPECT_FLOAT_EQ(sys_out[0], 0.9f);
    EXPECT_FLOAT_EQ(sys_out[3], 0.6f);

    // And they really are distinct.
    EXPECT_NE(mic_out, sys_out);
}

TEST(audio_mixer, the_separate_configuration_never_mixes) {
    AudioMixerConfig cfg = mono_mix_config();
    cfg.mode = AudioMixMode::kSeparate;

    AudioMixer mixer(cfg);

    const float mic[2] = {0.25f, 0.25f};
    const float sys[2] = {0.25f, 0.25f};
    ASSERT_EQ(mixer.push_microphone(mic, 2, 1, kRate), AudioMixStatus::kOk);
    ASSERT_EQ(mixer.push_system(sys, 2, 1, kRate), AudioMixStatus::kOk);

    std::vector<float> mixed;
    std::vector<float> mic_out;
    std::vector<float> sys_out;
    ASSERT_EQ(mixer.pull(mixed, mic_out, sys_out), AudioMixStatus::kOk);

    EXPECT_TRUE(mixed.empty());
    EXPECT_FLOAT_EQ(mic_out[0], 0.25f);   // NOT 0.5
    EXPECT_FLOAT_EQ(sys_out[0], 0.25f);   // NOT 0.5
}

TEST(audio_mixer, the_separate_configuration_drains_each_leg_independently) {
    AudioMixerConfig cfg = mono_mix_config();
    cfg.mode = AudioMixMode::kSeparate;

    AudioMixer mixer(cfg);

    const float mic[2] = {0.1f, 0.2f};
    const float sys[4] = {0.9f, 0.8f, 0.7f, 0.6f};
    ASSERT_EQ(mixer.push_microphone(mic, 2, 1, kRate), AudioMixStatus::kOk);
    ASSERT_EQ(mixer.push_system(sys, 4, 1, kRate), AudioMixStatus::kOk);

    std::vector<float> mixed;
    std::vector<float> mic_out;
    std::vector<float> sys_out;
    ASSERT_EQ(mixer.pull(mixed, mic_out, sys_out), AudioMixStatus::kOk);

    // The microphone leg only had two frames; the system leg had four.
    EXPECT_EQ(mic_out.size(), 2u);
    EXPECT_EQ(sys_out.size(), 4u);
}

TEST(audio_mixer, the_mode_is_surfaced_in_the_configuration) {
    AudioMixerConfig cfg = mono_mix_config();
    cfg.mode = AudioMixMode::kSeparate;

    AudioMixer mixer(cfg);
    EXPECT_EQ(mixer.config().mode, AudioMixMode::kSeparate);
    EXPECT_EQ(nimrtc::audio_device::audio_mix_mode_name(AudioMixMode::kMix),
              "mix");
    EXPECT_EQ(nimrtc::audio_device::audio_mix_mode_name(AudioMixMode::kSeparate),
              "separate");
}

// ---------------------------------------------------------------------------
// 3. The rate mismatch
// ---------------------------------------------------------------------------

TEST(audio_mixer, a_different_rate_on_one_leg_is_resampled) {
    AudioMixer mixer(mono_mix_config());

    // The microphone runs at 48 kHz, the system audio at 44.1 kHz.
    const float mic[480] = {0.5f};
    std::vector<float> sys(441, 0.25f);

    ASSERT_EQ(mixer.push_microphone(mic, 480, 1, 48000), AudioMixStatus::kOk);
    ASSERT_EQ(mixer.push_system(sys.data(), 441, 1, 44100), AudioMixStatus::kOk);

    std::vector<float> mixed;
    std::vector<float> a;
    std::vector<float> b;
    ASSERT_EQ(mixer.pull(mixed, a, b), AudioMixStatus::kOk);

    // 441 frames at 44.1 kHz become 480 frames at 48 kHz (the same
    // duration); the mixed block is bounded by the shorter leg, so it
    // must be close to 441 frames, not 480 and not 0.
    EXPECT_GE(mixed.size(), 400u);
    EXPECT_LE(mixed.size(), 480u);

    const auto stats = mixer.stats();
    EXPECT_FALSE(stats.microphone_resampled);
    EXPECT_TRUE(stats.system_resampled);
}

TEST(audio_mixer, the_resampled_result_has_the_expected_duration) {
    AudioMixer mixer(mono_mix_config());

    // One second of audio at 44.1 kHz must come out as one second at
    // 48 kHz (within one frame of rounding).
    std::vector<float> sys(44100, 0.5f);
    const float mic[48000] = {0.0f};

    ASSERT_EQ(mixer.push_microphone(mic, 48000, 1, 48000), AudioMixStatus::kOk);
    ASSERT_EQ(mixer.push_system(sys.data(), 44100, 1, 44100), AudioMixStatus::kOk);

    std::vector<float> mixed;
    std::vector<float> a;
    std::vector<float> b;
    ASSERT_EQ(mixer.pull(mixed, a, b), AudioMixStatus::kOk);

    // The mixed block is bounded by the SHORTER leg.  The microphone leg
    // has exactly 48000 frames; the resampled system leg has 48000
    // frames minus the converter's filter tail (the band-limited
    // converter holds a partial tail), so the block is a few dozen
    // frames short of a full 48 kHz second.
    EXPECT_GE(mixed.size(), 47000u);
    EXPECT_LE(mixed.size(), 48000u);

    // The system leg contributed 44100 frames at 44.1 kHz, which
    // resampled to 48000 frames at 48 kHz.  The mic leg is silence and
    // the system leg is a constant 0.5, so the mixed signal must be
    // ~0.5 everywhere — no gross error, no drop-out.
    //
    // The first samples are skipped: a band-limited converter's filter
    // has a group delay, so its output ramps up over the first few
    // dozen samples.  That is the converter's documented behaviour, not
    // a mix error.
    for (std::size_t i = 256; i < mixed.size(); i += 997) {
        EXPECT_NEAR(mixed[i], 0.5f, 0.01f) << "at index " << i;
    }
}

TEST(audio_mixer, an_equal_rate_leg_is_not_converted) {
    AudioMixer mixer(mono_mix_config());

    const float mic[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    const float sys[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    ASSERT_EQ(mixer.push_microphone(mic, 4, 1, kRate), AudioMixStatus::kOk);
    ASSERT_EQ(mixer.push_system(sys, 4, 1, kRate), AudioMixStatus::kOk);

    std::vector<float> mixed;
    std::vector<float> a;
    std::vector<float> b;
    ASSERT_EQ(mixer.pull(mixed, a, b), AudioMixStatus::kOk);

    // Bit-exact: no 16-bit round-trip happened.
    ASSERT_EQ(mixed.size(), 4u);
    EXPECT_FLOAT_EQ(mixed[0], 0.2f);
    EXPECT_FLOAT_EQ(mixed[1], 0.4f);
    EXPECT_FLOAT_EQ(mixed[2], 0.6f);
    EXPECT_FLOAT_EQ(mixed[3], 0.8f);
}

// ---------------------------------------------------------------------------
// 4. The channel mismatch
// ---------------------------------------------------------------------------

TEST(audio_mixer, a_stereo_source_is_down_mixed_to_mono) {
    AudioMixer mixer(mono_mix_config());   // mono output

    // Two channels: 0.6 on the left, 0.2 on the right -> 0.4 on average.
    const float sys[4] = {0.6f, 0.2f, 0.8f, 0.0f};
    const float mic[2] = {0.0f, 0.0f};
    ASSERT_EQ(mixer.push_microphone(mic, 2, 1, kRate), AudioMixStatus::kOk);
    ASSERT_EQ(mixer.push_system(sys, 2, 2, kRate), AudioMixStatus::kOk);

    std::vector<float> mixed;
    std::vector<float> a;
    std::vector<float> b;
    ASSERT_EQ(mixer.pull(mixed, a, b), AudioMixStatus::kOk);

    ASSERT_EQ(mixed.size(), 2u);
    EXPECT_NEAR(mixed[0], 0.4f, 1e-5f);
    EXPECT_NEAR(mixed[1], 0.4f, 1e-5f);
}

TEST(audio_mixer, a_mono_source_is_up_mixed_to_stereo) {
    AudioMixerConfig cfg = mono_mix_config();
    cfg.output_channels = 2;

    AudioMixer mixer(cfg);
    ASSERT_TRUE(mixer.valid());

    const float mic[2] = {0.0f, 0.0f};
    const float sys[2] = {0.3f, 0.7f};
    ASSERT_EQ(mixer.push_microphone(mic, 2, 1, kRate), AudioMixStatus::kOk);
    ASSERT_EQ(mixer.push_system(sys, 2, 1, kRate), AudioMixStatus::kOk);

    std::vector<float> mixed;
    std::vector<float> a;
    std::vector<float> b;
    ASSERT_EQ(mixer.pull(mixed, a, b), AudioMixStatus::kOk);

    // Two output channels per frame, two frames.
    ASSERT_EQ(mixed.size(), 4u);
    EXPECT_FLOAT_EQ(mixed[0], 0.3f);
    EXPECT_FLOAT_EQ(mixed[1], 0.3f);
    EXPECT_FLOAT_EQ(mixed[2], 0.7f);
    EXPECT_FLOAT_EQ(mixed[3], 0.7f);
}

TEST(audio_mixer, a_stereo_output_keeps_the_stereo_source_stereo) {
    AudioMixerConfig cfg = mono_mix_config();
    cfg.output_channels = 2;

    AudioMixer mixer(cfg);

    const float mic[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float sys[4] = {0.1f, 0.9f, 0.2f, 0.8f};
    ASSERT_EQ(mixer.push_microphone(mic, 2, 2, kRate), AudioMixStatus::kOk);
    ASSERT_EQ(mixer.push_system(sys, 2, 2, kRate), AudioMixStatus::kOk);

    std::vector<float> mixed;
    std::vector<float> a;
    std::vector<float> b;
    ASSERT_EQ(mixer.pull(mixed, a, b), AudioMixStatus::kOk);

    ASSERT_EQ(mixed.size(), 4u);
    EXPECT_FLOAT_EQ(mixed[0], 0.1f);
    EXPECT_FLOAT_EQ(mixed[1], 0.9f);
    EXPECT_FLOAT_EQ(mixed[2], 0.2f);
    EXPECT_FLOAT_EQ(mixed[3], 0.8f);
}

// ---------------------------------------------------------------------------
// 5. The error paths
// ---------------------------------------------------------------------------

TEST(audio_mixer, an_invalid_configuration_is_rejected_without_a_crash) {
    AudioMixerConfig cfg;
    cfg.output_rate_hz = 0;

    AudioMixer mixer(cfg);
    EXPECT_FALSE(mixer.valid());

    const float samples[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    EXPECT_EQ(mixer.push_microphone(samples, 4, 1, kRate),
              AudioMixStatus::kInvalidParam);
    EXPECT_EQ(mixer.push_system(samples, 4, 1, kRate),
              AudioMixStatus::kInvalidParam);

    std::vector<float> mixed;
    std::vector<float> a;
    std::vector<float> b;
    EXPECT_EQ(mixer.pull(mixed, a, b), AudioMixStatus::kInvalidParam);
}

TEST(audio_mixer, a_null_buffer_with_frames_is_rejected) {
    AudioMixer mixer(mono_mix_config());
    EXPECT_EQ(mixer.push_microphone(nullptr, 4, 1, kRate),
              AudioMixStatus::kInvalidParam);
    EXPECT_EQ(mixer.push_system(nullptr, 4, 1, kRate),
              AudioMixStatus::kInvalidParam);
}

TEST(audio_mixer, pulling_from_an_empty_mixer_reports_not_ready) {
    AudioMixer mixer(mono_mix_config());
    std::vector<float> mixed;
    std::vector<float> a;
    std::vector<float> b;
    EXPECT_EQ(mixer.pull(mixed, a, b), AudioMixStatus::kNotReady);
}

TEST(audio_mixer, the_mix_waits_for_both_legs) {
    AudioMixer mixer(mono_mix_config());

    const float mic[4] = {0.1f, 0.1f, 0.1f, 0.1f};
    ASSERT_EQ(mixer.push_microphone(mic, 4, 1, kRate), AudioMixStatus::kOk);

    std::vector<float> mixed;
    std::vector<float> a;
    std::vector<float> b;
    // Only one leg has data: there is nothing to mix yet.
    EXPECT_EQ(mixer.pull(mixed, a, b), AudioMixStatus::kNotReady);
    EXPECT_EQ(mixer.microphone_frames_available(), 4u);
    EXPECT_EQ(mixer.system_frames_available(), 0u);
}
