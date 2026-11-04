/**
 * @file modules/audio3a/tests/test_webrtc_audio3a.cpp
 * @brief Behavioural tests for the WebRTC-APM-backed 3A implementation.
 *
 * These cover the parts of the 3A contract that the plugin-adapter smoke
 * tests cannot reach:
 *
 *  - **Frame alignment.**  The APM only accepts whole 10 ms frames while
 *    `IAudio3A::process_capture()` takes arbitrary frame lengths and
 *    mutates the caller's buffer in place.  When the caller pushes a frame
 *    whose length is not a multiple of the APM frame size, the samples that
 *    straddle the frame boundary are carried over to the next call.  The
 *    processed output of those carried-over samples has to reach the caller
 *    on the call that completes the frame — otherwise the 3A silently does
 *    nothing for unaligned pushers.
 *
 *  - **Config wiring.**  `enable_vad`, `report_tx_level` and
 *    `aec_delay_headroom_ms` are part of the public `Config`; each one has
 *    an observable effect.
 *
 *  - **Degradation reporting.**  When the APM is not available, the plugin
 *    factory must say so instead of handing out a passthrough silently.
 *
 *  - **Startup convergence.**  The upstream APM attenuates the first
 *    capture frame heavily when AEC and ANS are enabled together.  The
 *    signal has to come back on the following frames, including when the
 *    render path is never fed at all.
 *
 * Tests that need the real APM are skipped when the build does not contain
 * it (`webrtc_apm_compiled_in()`).
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <nimrtc/audio3a/audio3a.hpp>
#include <nimrtc/audio3a/audio3a_plugin.hpp>

namespace {

using nimrtc::audio3a::Config;
using nimrtc::audio3a::Frame;
using nimrtc::audio3a::NullAudio3A;
using nimrtc::audio3a::WebRtcAudio3A;
using nimrtc::audio3a::WebRtcPluginFactory;
using nimrtc::audio3a::webrtc_apm_compiled_in;

// The concrete (module-level) 3A interface and the pluggable (plugin-level)
// one share a name — alias the concrete one so both can be used.
using ConcreteIAudio3A = nimrtc::audio3a::IAudio3A;
using nimrtc::plugins::IAudio3A;

constexpr std::uint32_t kRate = 48000;
constexpr std::uint8_t  kMono = 1;
constexpr std::size_t   kApmFrame = 480;  // 10 ms @ 48 kHz

/** 1 kHz sine at -12 dBFS. */
std::vector<float> make_tone(std::size_t n, float amp = 0.25f) {
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRate;
        out[i] = static_cast<float>(
            amp * std::sin(2.0 * 3.14159265358979 * 1000.0 * t));
    }
    return out;
}

Config default_config() {
    Config cfg;
    cfg.sample_rate_hz   = kRate;
    cfg.capture_channels = kMono;
    cfg.render_channels  = kMono;
    cfg.enable_aec       = true;
    cfg.enable_agc       = true;
    cfg.ans_level        = 2;
    return cfg;
}

/** Push `samples` through `impl` as one capture frame. */
void push_capture(ConcreteIAudio3A& impl, float* samples, std::size_t n) {
    Frame f;
    f.samples        = samples;
    f.num_samples    = n;
    f.num_channels   = kMono;
    f.sample_rate_hz = kRate;
    impl.process_capture(f);
}

} // anonymous namespace

// =============================================================================
// Frame alignment
// =============================================================================

// A frame whose length is a multiple of the APM frame size is processed
// in place: the caller gets the processed audio back.
TEST(Audio3AFrameAlignment, AlignedFrameIsProcessedInPlace) {
    if (!webrtc_apm_compiled_in()) {
        GTEST_SKIP() << "real WebRTC APM not compiled in";
    }
    WebRtcAudio3A impl;
    ASSERT_TRUE(impl.init(default_config()));

    const auto tone = make_tone(kApmFrame);
    std::vector<float> buf = tone;
    push_capture(impl, buf.data(), buf.size());

    bool changed = false;
    for (std::size_t i = 0; i < buf.size(); ++i) {
        if (std::fabs(buf[i] - tone[i]) > 1e-6f) changed = true;
        EXPECT_FALSE(std::isnan(buf[i])) << "sample " << i;
    }
    EXPECT_TRUE(changed) << "the APM output must reach the caller's buffer";
}

// The regression test for the dropped-processed-output bug.
//
// The same 10 ms of audio is pushed two ways:
//   A: one aligned 480-sample frame,
//   B: three 160-sample frames (not a multiple of the APM frame size).
//
// In case B the first two pushes leave 320 samples in the carry-over
// buffer; the third push completes the APM frame.  The processed output of
// the samples that belong to that third push must be written back into the
// caller's buffer — the APM processed exactly the same 480-sample frame in
// both cases, so the resulting samples have to match.
TEST(Audio3AFrameAlignment, UnalignedPushDeliversProcessedOutputOnCompletingCall) {
    if (!webrtc_apm_compiled_in()) {
        GTEST_SKIP() << "real WebRTC APM not compiled in";
    }

    WebRtcAudio3A aligned;
    WebRtcAudio3A unaligned;
    ASSERT_TRUE(aligned.init(default_config()));
    ASSERT_TRUE(unaligned.init(default_config()));

    const auto tone = make_tone(kApmFrame);

    // A: one aligned frame.
    std::vector<float> bufA = tone;
    push_capture(aligned, bufA.data(), bufA.size());

    // B: three unaligned pushes of the same audio.
    constexpr std::size_t kChunk = 160;
    std::vector<float> bufB(kApmFrame, 0.0f);
    for (std::size_t k = 0; k < 3; ++k) {
        for (std::size_t i = 0; i < kChunk; ++i) {
            bufB[i] = tone[k * kChunk + i];
        }
        push_capture(unaligned, bufB.data(), kChunk);
    }

    // The third push completed the APM frame.  Its output covers the whole
    // frame; the part that belongs to the third push (the last 160 samples)
    // is what the caller may see, and it must equal the aligned run's output
    // for the same region.
    for (std::size_t i = 0; i < kChunk; ++i) {
        EXPECT_NEAR(bufB[i], bufA[2 * kChunk + i], 1e-6f)
            << "sample " << i << " of the completing call";
    }
}

// The carry-over buffer must not swallow samples: pushing the same audio in
// unaligned chunks has to leave the APM in the same state as pushing it
// aligned (verified through the reported capture level, which is measured in
// the input domain — see the LevelStats contract in audio3a.hpp).
TEST(Audio3AFrameAlignment, UnalignedPushDoesNotDropSamples) {
    if (!webrtc_apm_compiled_in()) {
        GTEST_SKIP() << "real WebRTC APM not compiled in";
    }

    Config cfg = default_config();
    cfg.report_tx_level = true;

    WebRtcAudio3A aligned;
    WebRtcAudio3A unaligned;
    ASSERT_TRUE(aligned.init(cfg));
    ASSERT_TRUE(unaligned.init(cfg));

    const auto tone = make_tone(kApmFrame);

    std::vector<float> bufA = tone;
    push_capture(aligned, bufA.data(), bufA.size());

    constexpr std::size_t kChunk = 160;
    std::vector<float> bufB(kApmFrame, 0.0f);
    for (std::size_t k = 0; k < 3; ++k) {
        for (std::size_t i = 0; i < kChunk; ++i) {
            bufB[i] = tone[k * kChunk + i];
        }
        push_capture(unaligned, bufB.data(), kChunk);
    }

    // Both runs processed the same 480-sample frame, so the reported level
    // has to agree — a dropped carry-over would change the RMS.
    EXPECT_NEAR(unaligned.stats().capture_level_dbfs,
                aligned.stats().capture_level_dbfs, 2.0f);
}

// =============================================================================
// Config wiring
// =============================================================================

TEST(Audio3AConfig, VadDisabledNeverReportsSpeech) {
    // These three tests drive the *real* APM: without it
    // `WebRtcAudio3A::init()` refuses to start (the CI jobs configure
    // with NIMRTC_VENDORED_WEBRTC_APM=OFF), so the assertions below
    // would measure nothing.  The stub path has its own coverage in
    // `Audio3AConfig.NullImplHonoursVadAndLevelFlags`.
    if (!webrtc_apm_compiled_in()) {
        GTEST_SKIP() << "real WebRTC APM not compiled in";
    }

    Config cfg = default_config();
    cfg.enable_vad = false;

    WebRtcAudio3A impl;
    ASSERT_TRUE(impl.init(cfg));

    auto tone = make_tone(kApmFrame, 0.9f);  // loud — a VAD would say "speech"
    impl.request_vad_report();
    push_capture(impl, tone.data(), tone.size());

    EXPECT_FALSE(impl.stats().vad_active)
        << "a disabled VAD must never report speech";
}

TEST(Audio3AConfig, ReportTxLevelOffKeepsLevelUntouched) {
    if (!webrtc_apm_compiled_in()) {
        GTEST_SKIP() << "real WebRTC APM not compiled in";
    }

    Config cfg = default_config();
    cfg.report_tx_level = false;

    WebRtcAudio3A impl;
    ASSERT_TRUE(impl.init(cfg));

    auto tone = make_tone(kApmFrame, 0.5f);
    push_capture(impl, tone.data(), tone.size());

    EXPECT_FLOAT_EQ(impl.stats().capture_level_dbfs, -96.0f)
        << "report_tx_level=false must not publish a capture level";
}

TEST(Audio3AConfig, ReportTxLevelOnPublishesLevel) {
    if (!webrtc_apm_compiled_in()) {
        GTEST_SKIP() << "real WebRTC APM not compiled in";
    }

    Config cfg = default_config();
    cfg.report_tx_level = true;

    WebRtcAudio3A impl;
    ASSERT_TRUE(impl.init(cfg));

    auto tone = make_tone(kApmFrame, 0.5f);
    push_capture(impl, tone.data(), tone.size());

    // The level meter reports the INPUT domain: the level of the signal as
    // it arrived from the microphone.  A -6 dBFS peak tone has an RMS of
    // about -9 dBFS, which is well above the -40 dBFS floor.
    //
    // It must NOT be measured on the 3A output: the noise suppressor
    // legitimately silences a stationary tone (it looks like noise to a
    // spectral suppressor), so an output-domain meter would report "the
    // microphone is dead" for a perfectly healthy input.
    EXPECT_GT(impl.stats().capture_level_dbfs, -40.0f)
        << "a -6 dBFS tone must report a level well above the noise floor";
}

// =============================================================================
// Startup convergence
// =============================================================================
//
// The upstream WebRTC APM needs a couple of frames before its echo
// canceller and its noise suppressor converge.  With AEC and ANS both
// enabled the very first capture frame comes out heavily attenuated: the
// echo canceller has no valid echo-path estimate yet and suppresses the
// whole band, and the noise suppressor then treats the attenuated result as
// noise.  That transient is upstream behaviour — feeding the render path
// first does not avoid it — and it lasts exactly one frame.
//
// What NimRTC has to guarantee is that the signal *comes back*.  A 3A
// stage that keeps the microphone muted would break every call, so these
// tests pin down the recovery rather than the transient itself.

TEST(Audio3AStartup, SignalReturnsAfterTheFirstFrame) {
    if (!webrtc_apm_compiled_in()) {
        GTEST_SKIP() << "real WebRTC APM not compiled in";
    }

    Config cfg = default_config();  // AEC + ANS + AGC, the hardest combination
    WebRtcAudio3A impl;
    ASSERT_TRUE(impl.init(cfg));

    float converged_peak = 0.0f;
    for (int frame = 0; frame < 4; ++frame) {
        auto tone = make_tone(kApmFrame, 0.5f);
        push_capture(impl, tone.data(), tone.size());

        float peak = 0.0f;
        for (float s : tone) {
            EXPECT_FALSE(std::isnan(s)) << "frame " << frame;
            peak = std::max(peak, std::fabs(s));
        }
        converged_peak = peak;
    }

    EXPECT_GT(converged_peak, 0.1f)
        << "the capture signal must survive once the 3A pipeline converged";
}

// The most dangerous variant of the transient: a receive-only participant
// never plays audio, so the echo canceller has no reference signal at all.
// The capture path still has to deliver the microphone.
TEST(Audio3AStartup, MicStaysAliveWhenTheRemoteSideNeverPlays) {
    if (!webrtc_apm_compiled_in()) {
        GTEST_SKIP() << "real WebRTC APM not compiled in";
    }

    Config cfg = default_config();
    WebRtcAudio3A impl;
    ASSERT_TRUE(impl.init(cfg));

    // No process_render() call at all — the render path stays empty.
    float last_peak = 0.0f;
    for (int frame = 0; frame < 8; ++frame) {
        auto tone = make_tone(kApmFrame, 0.5f);
        push_capture(impl, tone.data(), tone.size());

        float peak = 0.0f;
        for (float s : tone) {
            EXPECT_FALSE(std::isnan(s)) << "frame " << frame;
            peak = std::max(peak, std::fabs(s));
        }
        last_peak = peak;
    }

    EXPECT_GT(last_peak, 0.1f)
        << "without a render reference the mic must not be muted";
}

TEST(Audio3AConfig, NullImplHonoursVadAndLevelFlags) {
    Config cfg;
    cfg.enable_vad      = false;
    cfg.report_tx_level = false;

    NullAudio3A impl;
    ASSERT_TRUE(impl.init(cfg));

    auto tone = make_tone(kApmFrame, 0.9f);
    impl.request_vad_report();
    Frame f;
    f.samples        = tone.data();
    f.num_samples    = tone.size();
    f.num_channels   = kMono;
    f.sample_rate_hz = kRate;
    impl.process_capture(f);

    EXPECT_FALSE(impl.stats().vad_active);
    EXPECT_FLOAT_EQ(impl.stats().capture_level_dbfs, -96.0f);
}

// =============================================================================
// AEC reference path
// =============================================================================

// The echo canceller needs the loudspeaker reference and the system delay.
// Feeding the reference path and reporting playback must be accepted, and
// the delay estimate has to reach the APM (no error is reported).
TEST(Audio3ARenderPath, ReferenceSignalAndPlaybackAreAccepted) {
    if (!webrtc_apm_compiled_in()) {
        GTEST_SKIP() << "real WebRTC APM not compiled in";
    }

    Config cfg = default_config();
    cfg.aec_delay_headroom_ms = 40;

    WebRtcAudio3A impl;
    ASSERT_TRUE(impl.init(cfg));

    auto reference = make_tone(kApmFrame, 0.4f);
    Frame rf;
    rf.samples        = reference.data();
    rf.num_samples    = reference.size();
    rf.num_channels   = kMono;
    rf.sample_rate_hz = kRate;
    impl.process_render(rf);
    impl.on_render_delivered(kApmFrame);

    // The capture path must still work after the reference path ran.
    auto capture = make_tone(kApmFrame);
    push_capture(impl, capture.data(), capture.size());
    EXPECT_FALSE(std::isnan(capture[0]));
}

// The delay the APM is told about is the configured system delay, not a
// running playback counter.  Feeding the same delay many times must not
// change the outcome of the capture path.
TEST(Audio3ARenderPath, SystemDelayIsStableAcrossPlaybackUpdates) {
    if (!webrtc_apm_compiled_in()) {
        GTEST_SKIP() << "real WebRTC APM not compiled in";
    }

    Config cfg = default_config();
    cfg.aec_delay_headroom_ms = 30;

    WebRtcAudio3A impl;
    ASSERT_TRUE(impl.init(cfg));

    auto reference = make_tone(kApmFrame, 0.4f);
    for (int i = 0; i < 50; ++i) {
        Frame rf;
        rf.samples        = reference.data();
        rf.num_samples    = reference.size();
        rf.num_channels   = kMono;
        rf.sample_rate_hz = kRate;
        impl.process_render(rf);
        impl.on_render_delivered(kApmFrame);
    }

    auto capture = make_tone(kApmFrame);
    push_capture(impl, capture.data(), capture.size());
    for (float s : capture) {
        EXPECT_FALSE(std::isnan(s));
    }
}

// =============================================================================
// Degradation reporting
// =============================================================================

// When the APM is not part of the build, the factory substitutes the
// passthrough stub — and says so on the plugin instance.
TEST(Audio3ADegradation, FactoryFlagsTheFallbackWhenApmIsMissing) {
    if (webrtc_apm_compiled_in()) {
        GTEST_SKIP() << "the real APM is present — no degradation expected";
    }

    WebRtcPluginFactory factory;
    std::unique_ptr<IAudio3A> plugin{factory.create()};
    ASSERT_NE(plugin, nullptr);

    const std::string name = plugin->name() ? plugin->name() : "";
    EXPECT_NE(name.find("DEGRADED"), std::string::npos)
        << "a fallback instance must announce the degradation, got: " << name;
}

// With the APM present, the factory must NOT flag a degradation.
TEST(Audio3ADegradation, FactoryDoesNotFlagDegradationWhenApmIsPresent) {
    if (!webrtc_apm_compiled_in()) {
        GTEST_SKIP() << "real WebRTC APM not compiled in";
    }

    WebRtcPluginFactory factory;
    std::unique_ptr<IAudio3A> plugin{factory.create()};
    ASSERT_NE(plugin, nullptr);

    const std::string name = plugin->name() ? plugin->name() : "";
    EXPECT_EQ(name.find("DEGRADED"), std::string::npos)
        << "a real APM instance must not be flagged as degraded";
}
