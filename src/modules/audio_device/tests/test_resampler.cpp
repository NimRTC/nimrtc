// modules/audio_device/tests/test_resampler.cpp
//
// The resampler's unit tests (remote-desktop plan F5).
//
// What they cover, and why:
//
//   1. the round-trip fidelity �?a 48 kHz sine down to 44.1 kHz and
//      back must track the input: the fundamental frequency (measured
//      by a zero-crossing count) and the peak amplitude;
//   2. the rate-ratio accuracy �?over N frames the output frame count
//      must match `input_frames * ratio` (the *cumulative* count is
//      what matters, because a converter buffers internally);
//   3. the channel interleaving �?a 2-channel input stays 2-channel and
//      the channels are not mixed;
//   4. the invalid-configuration rejection �?a zero rate, a zero
//      channel count: a clean error, not an exception and not a crash;
//   5. the fallback path �?when the vendored library is not compiled
//      in, `backend()` reports "linear" and the same tests still pass
//      with a looser tolerance.  The tolerance is picked from
//      `resampler_backend()`, so the file works with either backend;
//   6. the continuity across calls �?resampling in 10 small blocks must
//      produce the same output as resampling in one large block, which
//      proves the internal state is kept.
//
// The tests never require an audio device, so they run on a CI machine
// with no sound card.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <nimrtc/audio_device/audio_device.hpp>
#include <nimrtc/audio_device/resampler.hpp>

namespace {

using nimrtc::audio_device::Resampler;
using nimrtc::audio_device::ResamplerQuality;
using nimrtc::audio_device::ResampleStatus;

constexpr double kTwoPi = 6.28318530717958647692;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// True when the module was built against the vendored libsamplerate.
bool has_libsamplerate() {
    return nimrtc::audio_device::resampler_backend() == "libsamplerate";
}

// The peak amplitude tolerance.  The libsamplerate path is a band-
// limited sinc interpolator: the amplitude is preserved to well under
// 1 %.  The linear fallback is not band-limited and loses a little
// energy at the top of the band, so it gets a looser bound.
double peak_tolerance() {
    return has_libsamplerate() ? 0.05 : 0.15;
}

// The frequency tolerance, as a fraction of the nominal frequency.  A
// zero-crossing count over a whole number of cycles is exact for an
// ideal sine; the interpolation error shows up as a small drift, and
// the linear fallback drifts a little more.
double frequency_tolerance() {
    return has_libsamplerate() ? 0.02 : 0.05;
}

// Generate `frames` frames of a sine at `hz`, interleaved over
// `channels`, at the given rate.  The phase is continuous across the
// whole buffer.
std::vector<std::int16_t> make_sine(std::uint32_t rate,
                                     std::uint16_t channels,
                                     std::size_t frames,
                                     double hz,
                                     double amplitude = 0.5) {
    std::vector<std::int16_t> out(frames * channels);
    const double step = kTwoPi * hz / static_cast<double>(rate);
    for (std::size_t i = 0; i < frames; ++i) {
        const double v = amplitude * std::sin(step * static_cast<double>(i));
        const auto s = static_cast<std::int16_t>(std::lrint(v * 32767.0));
        for (std::uint16_t ch = 0; ch < channels; ++ch) {
            out[i * channels + ch] = s;
        }
    }
    return out;
}

// The peak absolute sample of a mono buffer.
double peak_of(const std::vector<std::int16_t>& buf) {
    double peak = 0.0;
    for (std::int16_t s : buf) {
        peak = std::max(peak, std::fabs(static_cast<double>(s)));
    }
    return peak;
}

// The fundamental frequency of a mono buffer, measured by counting the
// zero crossings (rising edges) and dividing by the buffer's duration.
//
// The buffer must hold a whole number of cycles for the count to be
// exact, so the caller picks the frame count accordingly.
double measure_frequency(const std::vector<std::int16_t>& buf,
                         std::uint32_t rate) {
    if (buf.size() < 2) {
        return 0.0;
    }
    std::size_t crossings = 0;
    for (std::size_t i = 1; i < buf.size(); ++i) {
        if (buf[i - 1] <= 0 && buf[i] > 0) {
            ++crossings;
        }
    }
    const double seconds = static_cast<double>(buf.size()) / static_cast<double>(rate);
    if (seconds <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(crossings) / seconds;
}

// Extract one channel of an interleaved buffer.
std::vector<std::int16_t> extract_channel(const std::vector<std::int16_t>& buf,
                                           std::uint16_t channels,
                                           std::uint16_t channel) {
    std::vector<std::int16_t> out;
    out.reserve(buf.size() / channels);
    for (std::size_t i = channel; i < buf.size(); i += channels) {
        out.push_back(buf[i]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 1. The module surface
// ---------------------------------------------------------------------------

TEST(resampler_module, the_backend_is_reported_honestly) {
    const std::string backend = nimrtc::audio_device::resampler_backend();
    EXPECT_FALSE(backend.empty());
    EXPECT_TRUE(backend == "libsamplerate" || backend == "linear");

    // The module-level function and the class's own answer must agree.
    Resampler r(48000, 44100, 1);
    EXPECT_EQ(r.backend(), backend);
    EXPECT_EQ(r.backend(), nimrtc::audio_device::resampler_backend());

    // The availability flag and the backend string must agree too.
    EXPECT_EQ(nimrtc::audio_device::resampler_available(), has_libsamplerate());
}

TEST(resampler_module, the_free_helpers_are_consistent) {
    EXPECT_DOUBLE_EQ(nimrtc::audio_device::resample_ratio(48000, 44100),
                     44100.0 / 48000.0);
    EXPECT_DOUBLE_EQ(nimrtc::audio_device::resample_ratio(44100, 48000),
                     48000.0 / 44100.0);
    EXPECT_DOUBLE_EQ(nimrtc::audio_device::resample_ratio(48000, 48000), 1.0);

    // A zero rate has no ratio.
    EXPECT_DOUBLE_EQ(nimrtc::audio_device::resample_ratio(0, 48000), 0.0);
    EXPECT_DOUBLE_EQ(nimrtc::audio_device::resample_ratio(48000, 0), 0.0);

    EXPECT_EQ(nimrtc::audio_device::default_resampler_quality(),
              ResamplerQuality::kMedium);
}

TEST(resampler_module, the_status_names_are_distinct) {
    EXPECT_EQ(nimrtc::audio_device::resample_status_name(ResampleStatus::kOk),
              "ok");
    EXPECT_NE(nimrtc::audio_device::resample_status_name(
                  ResampleStatus::kInvalidConfig),
              nimrtc::audio_device::resample_status_name(
                  ResampleStatus::kInvalidInput));
    EXPECT_NE(nimrtc::audio_device::resample_status_name(
                  ResampleStatus::kInvalidInput),
              nimrtc::audio_device::resample_status_name(
                  ResampleStatus::kInternalError));
}

// ---------------------------------------------------------------------------
// 2. The invalid-configuration rejection
// ---------------------------------------------------------------------------

TEST(resampler_config, a_zero_input_rate_is_rejected) {
    Resampler r(0, 48000, 1);
    EXPECT_FALSE(r.valid());

    std::vector<std::int16_t> in(128, 0);
    std::vector<std::int16_t> out;
    EXPECT_EQ(r.resample(in.data(), in.size(), out),
              ResampleStatus::kInvalidConfig);
    EXPECT_TRUE(out.empty());
}

TEST(resampler_config, a_zero_output_rate_is_rejected) {
    Resampler r(48000, 0, 1);
    EXPECT_FALSE(r.valid());

    std::vector<std::int16_t> in(128, 0);
    std::vector<std::int16_t> out;
    EXPECT_EQ(r.resample(in.data(), in.size(), out),
              ResampleStatus::kInvalidConfig);
}

TEST(resampler_config, a_zero_channel_count_is_rejected) {
    Resampler r(48000, 44100, 0);
    EXPECT_FALSE(r.valid());

    std::vector<std::int16_t> in(128, 0);
    std::vector<std::int16_t> out;
    EXPECT_EQ(r.resample(in.data(), in.size(), out),
              ResampleStatus::kInvalidConfig);
}

TEST(resampler_config, a_rate_ratio_out_of_range_is_rejected) {
    // libsamplerate supports ratios in [1/256, 256].  A ratio of 1000
    // is outside it.
    Resampler r(100, 100000, 1);
    EXPECT_FALSE(r.valid());

    std::vector<std::int16_t> in(128, 0);
    std::vector<std::int16_t> out;
    EXPECT_EQ(r.resample(in.data(), in.size(), out),
              ResampleStatus::kInvalidConfig);
}

TEST(resampler_config, a_null_buffer_with_frames_is_rejected) {
    Resampler r(48000, 44100, 1);
    ASSERT_TRUE(r.valid());

    std::vector<std::int16_t> out;
    EXPECT_EQ(r.resample(nullptr, 128, out), ResampleStatus::kInvalidInput);
    EXPECT_TRUE(out.empty());
}

TEST(resampler_config, an_equal_rate_pair_is_valid) {
    Resampler r(48000, 48000, 1);
    EXPECT_TRUE(r.valid());
    EXPECT_DOUBLE_EQ(r.ratio(), 1.0);
    EXPECT_EQ(r.input_rate_hz(), 48000u);
    EXPECT_EQ(r.output_rate_hz(), 48000u);
    EXPECT_EQ(r.channels(), 1u);
}

// ---------------------------------------------------------------------------
// 3. The rate-ratio accuracy
// ---------------------------------------------------------------------------

TEST(resampler_ratio, the_cumulative_output_count_matches_the_ratio) {
    constexpr std::uint32_t kIn = 44100;
    constexpr std::uint32_t kOut = 48000;
    constexpr std::size_t kFrames = 44100;   // one second

    Resampler r(kIn, kOut, 1);
    ASSERT_TRUE(r.valid());

    const std::vector<std::int16_t> in = make_sine(kIn, 1, kFrames, 440.0);

    // Feed the stream in ten blocks: the converter buffers internally,
    // so the *cumulative* count is what has to be right, not any
    // single block's count.
    std::vector<std::int16_t> out;
    std::size_t total = 0;
    const std::size_t block = kFrames / 10;
    for (std::size_t offset = 0; offset < kFrames; offset += block) {
        std::vector<std::int16_t> block_out;
        ASSERT_EQ(r.resample(in.data() + offset, block, block_out),
                  ResampleStatus::kOk);
        total += block_out.size();
    }

    // One second at 48 kHz is 48000 frames.  The converter's own
    // group delay accounts for a handful of frames, so the tolerance
    // is a small percentage.
    const double expected = static_cast<double>(kFrames) * (48000.0 / 44100.0);
    const double error = std::fabs(static_cast<double>(total) - expected);
    EXPECT_LT(error, expected * 0.01)
        << "expected ~" << expected << " output frames, got " << total;
}

TEST(resampler_ratio, a_downsampling_pair_produces_fewer_frames) {
    constexpr std::uint32_t kIn = 48000;
    constexpr std::uint32_t kOut = 44100;
    constexpr std::size_t kFrames = 48000;

    Resampler r(kIn, kOut, 1);
    ASSERT_TRUE(r.valid());

    const std::vector<std::int16_t> in = make_sine(kIn, 1, kFrames, 440.0);

    std::vector<std::int16_t> out;
    ASSERT_EQ(r.resample(in.data(), in.size(), out), ResampleStatus::kOk);

    const double expected = static_cast<double>(kFrames) * (44100.0 / 48000.0);
    EXPECT_LT(std::fabs(static_cast<double>(out.size()) - expected),
              expected * 0.01);
}

// ---------------------------------------------------------------------------
// 4. The round-trip fidelity
// ---------------------------------------------------------------------------

TEST(resampler_fidelity, a_round_trip_preserves_the_frequency_and_the_amplitude) {
    constexpr std::uint32_t kRate48 = 48000;
    constexpr std::uint32_t kRate44 = 44100;
    constexpr double kHz = 1000.0;      // 1 kHz: a whole number of cycles
    constexpr std::size_t kFrames = 48000;   // one second at 48 kHz

    // 48 kHz -> 44.1 kHz -> 48 kHz.
    const std::vector<std::int16_t> original =
        make_sine(kRate48, 1, kFrames, kHz);

    Resampler down(kRate48, kRate44, 1);
    ASSERT_TRUE(down.valid());
    std::vector<std::int16_t> mid;
    ASSERT_EQ(down.resample(original.data(), original.size(), mid),
              ResampleStatus::kOk);

    Resampler up(kRate44, kRate48, 1);
    ASSERT_TRUE(up.valid());
    std::vector<std::int16_t> result;
    ASSERT_EQ(up.resample(mid.data(), mid.size(), result),
              ResampleStatus::kOk);

    ASSERT_GT(result.size(), 1000u);

    // The frequency must survive the round trip.  The measured value is
    // slightly off because the round trip does not return exactly one
    // second of audio, so the count is divided by the *actual* length.
    const double measured = measure_frequency(result, kRate48);
    const double actual_seconds =
        static_cast<double>(result.size()) / static_cast<double>(kRate48);
    const double crossings =
        measured * actual_seconds;
    const double expected_crossings =
        kHz * static_cast<double>(original.size()) / static_cast<double>(kRate48);

    EXPECT_NEAR(crossings, expected_crossings,
                expected_crossings * frequency_tolerance() + 1.0)
        << "the fundamental drifted through the round trip";

    // The peak amplitude must be preserved (within the tolerance).
    const double in_peak = peak_of(original);
    const double out_peak = peak_of(result);
    EXPECT_NEAR(out_peak, in_peak, in_peak * peak_tolerance())
        << "the peak amplitude changed through the round trip";
}

TEST(resampler_fidelity, the_pitch_does_not_shift_on_a_downsample) {
    // The plan's acceptance criterion for F5 is "no audible pitch
    // error": a 1 kHz tone resampled from 48 kHz to 44.1 kHz must
    // still be a 1 kHz tone at the new rate.
    constexpr std::uint32_t kIn = 48000;
    constexpr std::uint32_t kOut = 44100;
    constexpr double kHz = 1000.0;
    constexpr std::size_t kFrames = 48000;

    const std::vector<std::int16_t> in = make_sine(kIn, 1, kFrames, kHz);

    Resampler r(kIn, kOut, 1);
    ASSERT_TRUE(r.valid());
    std::vector<std::int16_t> out;
    ASSERT_EQ(r.resample(in.data(), in.size(), out), ResampleStatus::kOk);
    ASSERT_GT(out.size(), 100u);

    // Measure over the middle of the buffer: the converter's start-up
    // transient (the filter filling up) is at the head.
    const std::size_t skip = out.size() / 10;
    const std::vector<std::int16_t> middle(out.begin() + static_cast<long>(skip),
                                           out.end());

    const double measured = measure_frequency(middle, kOut);
    EXPECT_NEAR(measured, kHz, kHz * frequency_tolerance())
        << "the pitch shifted when resampling 48 kHz -> 44.1 kHz";
}

// ---------------------------------------------------------------------------
// 5. The channel interleaving
// ---------------------------------------------------------------------------

TEST(resampler_channels, a_two_channel_input_stays_two_channel) {
    constexpr std::uint32_t kIn = 48000;
    constexpr std::uint32_t kOut = 44100;
    constexpr std::size_t kFrames = 4800;

    // Left is a 1 kHz tone, right is a 3 kHz tone �?clearly different
    // signals, so a mix-up is unmistakable.
    const std::vector<std::int16_t> in = make_sine(kIn, 2, kFrames, 1000.0);
    std::vector<std::int16_t> two = in;
    const std::vector<std::int16_t> right = make_sine(kIn, 2, kFrames, 3000.0);
    for (std::size_t i = 0; i < kFrames; ++i) {
        two[i * 2 + 1] = right[i * 2];
    }

    Resampler r(kIn, kOut, 2);
    ASSERT_TRUE(r.valid());
    std::vector<std::int16_t> out;
    ASSERT_EQ(r.resample(two.data(), kFrames, out), ResampleStatus::kOk);

    ASSERT_GT(out.size(), 100u);
    // Every output frame occupies exactly two samples.
    EXPECT_EQ(out.size() % 2u, 0u);

    // The channels must not be mixed: the left channel still carries
    // the 1 kHz tone and the right one the 3 kHz tone.
    const std::vector<std::int16_t> left = extract_channel(out, 2, 0);
    const std::vector<std::int16_t> right_ch = extract_channel(out, 2, 1);

    const double left_hz = measure_frequency(left, kOut);
    const double right_hz = measure_frequency(right_ch, kOut);

    EXPECT_NEAR(left_hz, 1000.0, 1000.0 * frequency_tolerance())
        << "the left channel changed";
    EXPECT_NEAR(right_hz, 3000.0, 3000.0 * frequency_tolerance())
        << "the right channel changed";

    // The two channels must still differ from each other.
    EXPECT_GT(peak_of(right_ch), 0.0);
    EXPECT_FALSE(left == right_ch);
}

// ---------------------------------------------------------------------------
// 6. The continuity across calls
// ---------------------------------------------------------------------------

TEST(resampler_continuity, splitting_the_stream_does_not_change_the_output) {
    constexpr std::uint32_t kIn = 48000;
    constexpr std::uint32_t kOut = 44100;
    constexpr std::size_t kFrames = 48000;
    constexpr std::size_t kBlocks = 10;

    const std::vector<std::int16_t> in = make_sine(kIn, 1, kFrames, 440.0);

    // One big block.
    Resampler whole(kIn, kOut, 1);
    ASSERT_TRUE(whole.valid());
    std::vector<std::int16_t> whole_out;
    ASSERT_EQ(whole.resample(in.data(), in.size(), whole_out),
              ResampleStatus::kOk);

    // Ten small blocks.
    Resampler split(kIn, kOut, 1);
    ASSERT_TRUE(split.valid());
    std::vector<std::int16_t> split_out;
    const std::size_t block = kFrames / kBlocks;
    for (std::size_t offset = 0; offset + block <= kFrames; offset += block) {
        std::vector<std::int16_t> block_out;
        ASSERT_EQ(split.resample(in.data() + offset, block, block_out),
                  ResampleStatus::kOk);
        split_out.insert(split_out.end(), block_out.begin(), block_out.end());
    }

    ASSERT_FALSE(whole_out.empty());
    ASSERT_FALSE(split_out.empty());

    // The two must be the same length (the block boundaries do not
    // change the frame count) and the same signal.
    EXPECT_EQ(whole_out.size(), split_out.size());

    const std::size_t compare =
        std::min(whole_out.size(), split_out.size());
    ASSERT_GT(compare, 1000u);
    for (std::size_t i = 0; i < compare; ++i) {
        ASSERT_NEAR(whole_out[i], split_out[i], 2)
            << "the output differs at sample " << i;
    }
}

// ---------------------------------------------------------------------------
// 7. The statistics �?the DEC-3 "make the conversion visible" surface
// ---------------------------------------------------------------------------

TEST(resampler_stats, the_conversion_is_reported) {
    constexpr std::uint32_t kIn = 44100;
    constexpr std::uint32_t kOut = 48000;
    constexpr std::size_t kFrames = 4410;

    Resampler r(kIn, kOut, 1);
    ASSERT_TRUE(r.valid());

    const std::vector<std::int16_t> in = make_sine(kIn, 1, kFrames, 440.0);
    std::vector<std::int16_t> out;
    ASSERT_EQ(r.resample(in.data(), in.size(), out), ResampleStatus::kOk);

    const auto stats = r.stats();
    EXPECT_EQ(stats.input_rate_hz, kIn);
    EXPECT_EQ(stats.output_rate_hz, kOut);
    EXPECT_EQ(stats.channels, 1u);
    EXPECT_DOUBLE_EQ(stats.ratio, 48000.0 / 44100.0);
    EXPECT_TRUE(stats.conversion_performed);
    EXPECT_EQ(stats.input_frames, kFrames);
    EXPECT_GT(stats.output_frames, 0u);
    EXPECT_EQ(stats.backend, r.backend());
    EXPECT_FALSE(stats.converter.empty());
}

TEST(resampler_stats, an_equal_rate_pair_reports_no_conversion) {
    Resampler r(48000, 48000, 2);
    ASSERT_TRUE(r.valid());

    constexpr std::size_t kFrames = 480;
    const std::vector<std::int16_t> in = make_sine(48000, 2, kFrames, 440.0);
    std::vector<std::int16_t> out;
    ASSERT_EQ(r.resample(in.data(), kFrames, out), ResampleStatus::kOk);

    // The 1:1 case is a bit-exact pass-through: no filter, no delay.
    ASSERT_EQ(out.size(), in.size());
    EXPECT_EQ(out, in);

    const auto stats = r.stats();
    EXPECT_FALSE(stats.conversion_performed);
    EXPECT_EQ(stats.input_frames, kFrames);
    EXPECT_EQ(stats.output_frames, kFrames);
    EXPECT_EQ(stats.channels, 2u);
}

// ---------------------------------------------------------------------------
// 8. The flush and the reset
// ---------------------------------------------------------------------------

TEST(resampler_flush, the_tail_is_flushed_at_end_of_stream) {
    constexpr std::uint32_t kIn = 48000;
    constexpr std::uint32_t kOut = 44100;
    constexpr std::size_t kFrames = 4800;

    Resampler r(kIn, kOut, 1);
    ASSERT_TRUE(r.valid());

    const std::vector<std::int16_t> in = make_sine(kIn, 1, kFrames, 440.0);
    std::vector<std::int16_t> out;
    ASSERT_EQ(r.resample(in.data(), in.size(), out), ResampleStatus::kOk);

    // What the converter still owes *before* the tail is drained.
    const std::size_t owed_before = r.output_frames_available();

    std::vector<std::int16_t> tail;
    EXPECT_EQ(r.flush(tail), ResampleStatus::kOk);
    // The flush must not lose the tail: whatever was still owed comes
    // out now.
    //
    // The libsamplerate backend holds back the sinc filter's group
    // delay, so its flush produces a real tail.  The linear fallback
    // emits every frame it can interpolate during the conversion
    // itself, so its tail is (at most) the last frame.
    const std::size_t total = r.stats().output_frames;
    if (owed_before > 1u) {
        // A filter-backed converter owed more than the final frame:
        // the flush must have paid that debt.
        EXPECT_GT(total, out.size());
        EXPECT_EQ(tail.size(), owed_before);
    } else {
        EXPECT_LE(total, out.size() + 1u);
    }
}

TEST(resampler_flush, a_flush_is_idempotent) {
    Resampler r(48000, 44100, 1);
    ASSERT_TRUE(r.valid());

    const std::vector<std::int16_t> in = make_sine(48000, 1, 480, 440.0);
    std::vector<std::int16_t> out;
    ASSERT_EQ(r.resample(in.data(), in.size(), out), ResampleStatus::kOk);

    std::vector<std::int16_t> tail;
    ASSERT_EQ(r.flush(tail), ResampleStatus::kOk);
    const std::size_t first = tail.size();

    std::vector<std::int16_t> again;
    EXPECT_EQ(r.flush(again), ResampleStatus::kOk);
    EXPECT_EQ(again.size(), 0u);
    EXPECT_EQ(first, tail.size());
}

TEST(resampler_flush, a_reset_makes_the_resampler_usable_again) {
    Resampler r(48000, 44100, 1);
    ASSERT_TRUE(r.valid());

    const std::vector<std::int16_t> in = make_sine(48000, 1, 480, 440.0);
    std::vector<std::int16_t> out;
    ASSERT_EQ(r.resample(in.data(), in.size(), out), ResampleStatus::kOk);

    std::vector<std::int16_t> tail;
    ASSERT_EQ(r.flush(tail), ResampleStatus::kOk);

    // After a flush the converter refuses more input ...
    std::vector<std::int16_t> more;
    EXPECT_EQ(r.resample(in.data(), in.size(), more),
              ResampleStatus::kInternalError);

    // ... until it is reset.
    r.reset();
    std::vector<std::int16_t> after_reset;
    EXPECT_EQ(r.resample(in.data(), in.size(), after_reset),
              ResampleStatus::kOk);
}

} // namespace
