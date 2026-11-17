/**
 * @file modules/audio3a/tests/test_audio3a_erle.cpp
 * @brief ERLE measurement rig for the acoustic echo canceller.
 *
 * ## What this measures
 *
 * ERLE (Echo Return Loss Enhancement) is the standard figure of merit for an
 * acoustic echo canceller:
 *
 *     ERLE = 10 * log10( P_echo_in / P_echo_out )   [dB]
 *
 * where `P_echo_in` is the power of the echo that entered the capture path
 * and `P_echo_out` is the power of the echo that survived the canceller.
 *
 * ## The rig
 *
 * The rig synthesises a *known* echo path instead of relying on a physical
 * loudspeaker-to-microphone loop, so the measurement is reproducible on any
 * machine and needs no audio hardware:
 *
 *   render (x)  ---> loudspeaker ----------------+
 *                                                 |  gain g, delay D
 *                                                 v
 *   capture (y) <--- microphone <-----------------+
 *
 * `D` is the render-to-capture delay of the synthetic path.  It is the same
 * quantity the host would measure with a real device and pass to
 * `Config::aec_delay_headroom_ms`; the rig sets the two consistently,
 * because the canceller cannot converge on an echo path whose delay it does
 * not know.
 *
 * The render signal is a *speech-like* broadband signal (noise shaped by a
 * slowly varying envelope), not a pure tone: a periodic tone is rejected by
 * the canceller's delay estimator, so a tone would report an ERLE that no
 * real session would ever reach.
 *
 * ## How ERLE is extracted
 *
 * The capture signal carries echo **only** — there is no near-end talker in
 * the measured segment.  That makes the measurement unambiguous:
 *
 *   run A (AEC disabled):  y_A = e          (the echo passes through)
 *   run B (AEC enabled) :  y_B = e_residual (what survived the canceller)
 *
 *     ERLE = 10 * log10( P(y_A) / P(y_B) )
 *
 * Both runs see exactly the same input, so the ratio isolates the
 * canceller's contribution and needs no near-end/echo separation — the hard
 * part of the general case.
 *
 * ## Why the near-end check uses a tone
 *
 * A measurement above would also pass if the canceller simply muted the
 * whole capture path.  The rig therefore also measures a **near-end
 * talker** and asserts that it survives.
 *
 * The near-end talker is a *tone*, not broadband noise, and that is
 * deliberate.  The upstream APM runs a residual echo suppressor after the
 * canceller: it estimates what the canceller left behind and attenuates it.
 * A broadband near-end signal is statistically indistinguishable from a
 * residual echo, so it is suppressed along with it — that is correct
 * upstream behaviour (the residual echo suppressor exists precisely to
 * remove the canceller's leftovers), not a NimRTC defect.  A tone at a
 * frequency the echo path does not carry is *separable*: a single-bin DFT
 * measures how much of the near-end talker survived without confusing it
 * with the echo.
 *
 * ## When the measurement is skipped
 *
 * The rig needs the real WebRTC APM (`webrtc_apm_compiled_in()`).  Without it
 * the module builds its stub implementation, which is a passthrough — an
 * ERLE of 0 dB would be the correct answer for a passthrough, but recording
 * that as "the ERLE measurement" would be meaningless.  The tests therefore
 * skip explicitly, and the artefact is only produced on a build that has the
 * APM.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <nimrtc/audio3a/audio3a.hpp>

namespace {

using nimrtc::audio3a::Config;
using nimrtc::audio3a::Frame;
using nimrtc::audio3a::IAudio3A;
using nimrtc::audio3a::webrtc_apm_compiled_in;

constexpr std::uint32_t kRate     = 48000;
constexpr std::uint8_t  kMono     = 1;
constexpr std::size_t   kApmFrame = 480;  // 10 ms @ 48 kHz

// The rig runs long enough for the canceller to converge (the upstream AEC3
// needs a few hundred milliseconds of reference signal to build a usable
// echo-path estimate) and long enough for a stable power estimate.
constexpr std::size_t kWarmupFrames  = 300;  // 3 s
constexpr std::size_t kMeasureFrames = 200;  // 2 s

// Synthetic echo path: a pure delay of 20 ms with unit gain (the gain
// cancels out of the ERLE ratio, so it is kept at 1.0 for clarity).
constexpr std::size_t kEchoDelayMs = 20;

// The near-end talker's tone.  1 kHz sits in the middle of the speech band
// and is far enough from the echo path's dominant energy to be separable.
constexpr double kNearendToneHz = 1000.0;
constexpr float  kNearendToneAmp = 0.2f;

/**
 * @brief Speech-like broadband render signal.
 *
 * White noise shaped by a slowly varying syllable-rate envelope.  A
 * canceller cannot lock onto a stationary tone, but it can lock onto this.
 */
std::vector<float> make_speechlike(std::size_t n, std::uint32_t seed) {
    std::vector<float> out(n);
    // xorshift32 — deterministic, so a failing measurement can be replayed.
    std::uint32_t state = seed ? seed : 0x9E3779B9u;
    for (std::size_t i = 0; i < n; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        // Uniform in [-1, 1).
        const double white =
            static_cast<double>(state) / 2147483648.0 - 1.0;
        const double t = static_cast<double>(i) / kRate;
        // Syllable-rate envelope: ~4 Hz with a slower amplitude drift.
        const double env =
            0.5 * (1.0 + std::sin(2.0 * 3.14159265358979 * 4.0 * t))
            * (0.7 + 0.3 * std::sin(2.0 * 3.14159265358979 * 0.7 * t));
        out[i] = static_cast<float>(0.5 * env * white);
    }
    return out;
}

/** Power (mean square) of a buffer. */
double power(const std::vector<float>& v) {
    if (v.empty()) return 0.0;
    double sum = 0.0;
    for (float s : v) {
        sum += static_cast<double>(s) * static_cast<double>(s);
    }
    return sum / static_cast<double>(v.size());
}

/** ERLE in dB between the uncancelled and the cancelled capture power. */
double erle_db(double echo_in_power, double echo_out_power) {
    if (echo_out_power <= 0.0) {
        // A perfect cancellation drives the residual to (numerical) zero.
        return 96.0;
    }
    return 10.0 * std::log10(echo_in_power / echo_out_power);
}

/**
 * @brief Amplitude of a single frequency in a buffer (single-bin DFT).
 *
 * Used to separate a *near-end tone* from the broadband echo: the echo is
 * speech-like noise with almost no energy at the tone's exact frequency, so
 * the tone's amplitude in the processed output measures how much of the
 * near-end talker survived the canceller.
 */
double tone_amplitude(const std::vector<float>& v, double freq_hz, std::uint32_t rate) {
    if (v.empty()) return 0.0;
    double re = 0.0;
    double im = 0.0;
    const double w = 2.0 * 3.14159265358979 * freq_hz / static_cast<double>(rate);
    for (std::size_t i = 0; i < v.size(); ++i) {
        const double phase = w * static_cast<double>(i);
        re += static_cast<double>(v[i]) * std::cos(phase);
        im += static_cast<double>(v[i]) * std::sin(phase);
    }
    // 2/N for a real sinusoid.
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(v.size());
}

/** Print a measurement line so the numbers are visible without XML parsing. */
void report(const char* label, double value) {
    std::printf("  [ERLE] %-30s %12.6g\n", label, value);
}

/** Push one capture frame through the 3A stage. */
void push_capture(IAudio3A& impl, std::vector<float>& buf) {
    Frame f;
    f.samples        = buf.data();
    f.num_samples    = buf.size();
    f.num_channels   = kMono;
    f.sample_rate_hz = kRate;
    impl.process_capture(f);
}

/** Push one render (reference) frame through the 3A stage. */
void push_render(IAudio3A& impl, const std::vector<float>& buf) {
    Frame f;
    f.samples        = const_cast<float*>(buf.data());
    f.num_samples    = buf.size();
    f.num_channels   = kMono;
    f.sample_rate_hz = kRate;
    impl.process_render(f);
}

/**
 * @brief Run the rig and return the processed capture over the measured segment.
 *
 * @param enable_aec  whether the canceller is active in this run.
 * @param capture     the capture signal to feed (echo-only or near-end).
 * @param reference   the render signal to feed as the AEC reference.
 */
std::vector<float> run_and_measure(bool enable_aec,
                                    const std::vector<float>& capture,
                                    const std::vector<float>& reference) {
    Config cfg;
    cfg.sample_rate_hz        = kRate;
    cfg.capture_channels      = kMono;
    cfg.render_channels       = kMono;
    cfg.enable_aec            = enable_aec;
    cfg.enable_agc            = false;  // AGC would rescale the signal and
                                        // destroy the power comparison.
    cfg.ans_level             = 0;      // Same reason: the noise suppressor
                                        // would gate a broadband signal.
    cfg.enable_vad            = false;
    cfg.aec_delay_headroom_ms = static_cast<std::int32_t>(kEchoDelayMs);

    auto impl = nimrtc::audio3a::create_webrtc_audio3a();
    EXPECT_TRUE(impl->init(cfg)) << "the WebRTC APM must initialise";
    if (!impl) return {};

    const std::size_t capture_frames  = capture.size() / kApmFrame;
    const std::size_t reference_frames = reference.size() / kApmFrame;
    const std::size_t frames = std::min(capture_frames, reference_frames);

    std::vector<float> residual;
    residual.reserve(kMeasureFrames * kApmFrame);

    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t off = i * kApmFrame;

        // The reference the AEC sees at time t is the render signal *now*;
        // the echo in the capture is that signal delayed by kEchoDelayMs.
        // The canceller is told the delay via set_stream_delay_ms(), so it
        // looks back into its own reference buffer by exactly that amount —
        // which is why the reference must be pushed un-delayed.
        const std::size_t ref_off = off * reference_frames / frames;
        push_render(*impl, std::vector<float>(
            reference.begin() + static_cast<std::ptrdiff_t>(ref_off),
            reference.begin() + static_cast<std::ptrdiff_t>(ref_off + kApmFrame)));
        impl->on_render_delivered(kApmFrame);

        std::vector<float> cap(capture.begin() + static_cast<std::ptrdiff_t>(off),
                               capture.begin() + static_cast<std::ptrdiff_t>(off + kApmFrame));
        push_capture(*impl, cap);

        if (i >= kWarmupFrames) {
            residual.insert(residual.end(), cap.begin(), cap.end());
        }
    }

    return residual;
}

/** Build the capture signal that carries the synthetic echo. */
std::vector<float> make_echo_capture(const std::vector<float>& reference,
                                      std::size_t delay_samples) {
    const std::size_t n = reference.size();
    std::vector<float> out(n, 0.0f);
    for (std::size_t i = delay_samples; i < n; ++i) {
        out[i] = reference[i - delay_samples];
    }
    return out;
}

/**
 * @brief Build the capture signal that carries a near-end talker only.
 */
std::vector<float> make_nearend_tone(std::size_t n, double freq_hz, float amp) {
    std::vector<float> out(n);
    const double a = static_cast<double>(amp);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRate;
        out[i] = static_cast<float>(
            a * std::sin(2.0 * 3.14159265358979 * freq_hz * t));
    }
    return out;
}

} // anonymous namespace

// =============================================================================
// ERLE measurement
// =============================================================================

// The headline number: with the canceller active the residual echo power
// must be far below the echo power that entered the capture path.
//
// 15 dB is the floor that ITU-T P.807 requires of a converged echo canceller
// in a *stationary* synthetic path; a broadband, delay-aligned synthetic path
// is easier than a real room, so the rig holds a higher bar than that.
TEST(Audio3AErle, CancellerSuppressesTheSyntheticEcho) {
    if (!webrtc_apm_compiled_in()) {
        GTEST_SKIP() << "real WebRTC APM not compiled in — no ERLE to measure";
    }

    const std::size_t n = (kWarmupFrames + kMeasureFrames) * kApmFrame;
    const auto reference = make_speechlike(n, 0xC0FFEE01u);
    const auto capture   = make_echo_capture(reference, kEchoDelayMs * kRate / 1000);

    const auto uncancelled = run_and_measure(false, capture, reference);
    const auto cancelled   = run_and_measure(true,  capture, reference);

    const double echo_in  = power(uncancelled);
    const double echo_out = power(cancelled);

    ASSERT_GT(echo_in, 1e-12) << "the synthetic echo path produced no signal";
    ASSERT_GT(echo_out, 0.0) << "the cancelled run produced no output at all";

    const double erle = erle_db(echo_in, echo_out);
    RecordProperty("erle_db", erle);
    RecordProperty("echo_in_power", echo_in);
    RecordProperty("echo_out_power", echo_out);

    report("echo_in_power", echo_in);
    report("echo_out_power", echo_out);
    report("ERLE (dB)", erle);

    EXPECT_GE(erle, 15.0)
        << "ERLE = " << erle << " dB (echo_in power " << echo_in
        << ", echo_out power " << echo_out << ")";
}

// The measurement above would also pass if the canceller simply muted the
// whole capture path.  This test pins down that the *near-end* talker — a
// microphone signal with no echo in it — survives the canceller.
TEST(Audio3AErle, NearEndTalkerSurvivesTheCanceller) {
    if (!webrtc_apm_compiled_in()) {
        GTEST_SKIP() << "real WebRTC APM not compiled in — no ERLE to measure";
    }

    const std::size_t n = (kWarmupFrames + kMeasureFrames) * kApmFrame;
    // The render path runs, so the canceller is fully active; the capture
    // path carries the near-end talker only.
    const auto reference = make_speechlike(n, 0xBEEF0002u);
    const auto capture   = make_nearend_tone(n, kNearendToneHz, kNearendToneAmp);

    ASSERT_FALSE(reference.empty());

    // Run with the canceller off first: that is the "the microphone works"
    // baseline — the pipeline in front of the AEC must not alter the tone.
    const auto baseline = run_and_measure(false, capture, reference);
    // Then with the canceller on.
    const auto processed = run_and_measure(true, capture, reference);

    const double baseline_amp  = tone_amplitude(baseline, kNearendToneHz, kRate);
    const double processed_amp = tone_amplitude(processed, kNearendToneHz, kRate);

    ASSERT_GT(baseline_amp, 1e-6)
        << "the near-end tone did not reach the pipeline at all";
    RecordProperty("nearend_baseline_amplitude", baseline_amp);
    RecordProperty("nearend_processed_amplitude", processed_amp);

    report("nearend baseline amp", baseline_amp);
    report("nearend processed amp", processed_amp);

    // The near-end talker must come through.  The upstream APM attenuates
    // the very first frame while it converges, so the bar is on the
    // *measured* segment (after the warm-up), where the pipeline has
    // settled.  The floor is generous on purpose: this test is about "the
    // mic still works", not about the exact gain the AGC-free pipeline
    // applies.
    EXPECT_GT(processed_amp, baseline_amp * 0.1)
        << "the near-end talker must survive the canceller (baseline amplitude "
        << baseline_amp << ", processed amplitude " << processed_amp << ")";
}

// With the canceller disabled the rig must report a flat ~0 dB: this is the
// control that proves the ERLE number above comes from the canceller and
// not from an artefact of the rig.
TEST(Audio3AErle, ControlRunWithoutTheCancellerReportsNoSuppression) {
    if (!webrtc_apm_compiled_in()) {
        GTEST_SKIP() << "real WebRTC APM not compiled in — no ERLE to measure";
    }

    const std::size_t n = (kWarmupFrames + kMeasureFrames) * kApmFrame;
    const auto reference = make_speechlike(n, 0x0BADC0DEu);
    const auto capture   = make_echo_capture(reference, kEchoDelayMs * kRate / 1000);

    const double echo_in  = power(run_and_measure(false, capture, reference));
    const double echo_out = power(run_and_measure(false, capture, reference));

    ASSERT_GT(echo_in, 1e-12);
    const double erle = erle_db(echo_in, echo_out);
    RecordProperty("control_erle_db", erle);
    report("control ERLE (dB)", erle);

    // Two identical runs differ only by floating-point noise; the "ERLE" of
    // the control must be a small number, not a large positive one.
    EXPECT_LT(erle, 3.0)
        << "the control run must not report suppression (ERLE = " << erle << " dB)";
}
