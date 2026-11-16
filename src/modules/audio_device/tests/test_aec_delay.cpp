// modules/audio_device/tests/test_aec_delay.cpp
//
// The AEC-delay estimator's unit tests (remote-desktop plan F4).
//
// What they cover, and why:
//
//   1. the **positive control** — the same signal fed into the played
//      path and, delayed by exactly 20 ms, into the captured path must
//      make the estimator report ~20 ms;
//   2. the **negative control** — two *uncorrelated* signals must NOT
//      make the estimator confidently report a random delay.  This is
//      the test that matters most: an estimator that guesses is worse
//      than no estimator at all, because the guess misaligns the AEC
//      reference;
//   3. the **override** path — a configured delay bypasses the
//      correlation entirely;
//   4. the **headroom** — the reported headroom is `measured +
//      headroom_ms`;
//   5. the honesty contract — no estimate before enough data has
//      arrived.
//
// The test uses a deterministic PRNG (a fixed-seed LCG) so the result
// is reproducible on every machine and every CI leg.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <nimrtc/audio_device/aec_delay.hpp>

namespace {

using nimrtc::audio_device::AecDelayConfig;
using nimrtc::audio_device::AecDelayEstimator;
using nimrtc::audio_device::AecDelayMonitor;

constexpr std::uint32_t kRate = 48000;

// ---------------------------------------------------------------------------
// A deterministic PRNG
// ---------------------------------------------------------------------------

/**
 * @brief A fixed-seed linear congruential generator.
 *
 * The estimator's negative control needs two signals that are
 * *statistically* uncorrelated but *exactly* reproducible: a fixed
 * seed gives the same sequence on every machine, every run.
 */
class Lcg {
public:
    explicit Lcg(std::uint64_t seed) noexcept : state_(seed) {}

    /** The next value in [-1.0, 1.0). */
    float next() noexcept {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto bits = static_cast<std::uint32_t>(state_ >> 33);
        return (static_cast<float>(bits) / 2147483648.0f) - 1.0f;
    }

private:
    std::uint64_t state_;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/**
 * @brief A configuration for a 20 ms measurement at 48 kHz.
 *
 * The correlation window is 1024 frames (~21 ms) and the lag search
 * reaches 960 frames (20 ms), which is exactly the delay the positive
 * control injects.  The estimate interval is short enough that the
 * test does not have to feed a second's worth of audio.
 */
AecDelayConfig test_config() {
    AecDelayConfig cfg;
    cfg.sample_rate_hz = kRate;
    cfg.correlation_frames = 1024;
    cfg.max_delay_frames = 960;
    cfg.headroom_ms = 10;
    cfg.confidence_threshold = 0.35f;
    cfg.smoothing_window = 1;          // no smoothing: the raw estimate
    cfg.estimate_interval_frames = 1024;
    return cfg;
}

/**
 * @brief Feed the estimator a block and its delayed twin.
 *
 * @param estimator The estimator to drive.
 * @param played    The signal that reached the loudspeaker.
 * @param delay_frames How far the captured signal lags the played one.
 * @param index     The absolute frame index of the first sample of
 *                  @p played (the captured signal is indexed on the
 *                  SAME axis — the microphone hears the loudspeaker's
 *                  signal delayed, not a different point in time).
 */
void feed_with_delay(AecDelayEstimator& estimator,
                     const std::vector<float>& played,
                     std::size_t delay_frames,
                     std::uint64_t index) {
    estimator.on_render_played(played.data(), played.size(), index);

    // The captured signal is the played signal, delayed.  The delay is
    // expressed as a *shift of the content*: the captured block at
    // index `index` contains what was played at `index - delay`.
    std::vector<float> captured(played.size(), 0.0f);
    const std::size_t usable =
        played.size() > delay_frames ? played.size() - delay_frames : 0;
    for (std::size_t i = 0; i < usable; ++i) {
        captured[i + delay_frames] = played[i];
    }
    estimator.on_capture(captured.data(), captured.size(), index);
}

/**
 * @brief Feed the estimator a long enough stream for the correlation
 *        to run at least once.
 *
 * The delay is a shift of the *stream*, not of a single block: the
 * captured stream is the played stream, shifted right by
 * `delay_frames` (and padded with silence at the front).  Shifting
 * inside a block instead would be wrong -- the microphone hears the
 * loudspeaker's signal from the *previous* blocks, and a block-local
 * shift produces a captured stream that is uncorrelated with the
 * played one (for a delay longer than one block, it produces pure
 * silence).
 */
void feed_stream(AecDelayEstimator& estimator,
                 const std::vector<float>& played,
                 std::size_t delay_frames) {
    constexpr std::size_t kBlock = 480;   // 10 ms at 48 kHz

    // The captured stream: the played stream shifted right by the delay.
    std::vector<float> captured(played.size(), 0.0f);
    for (std::size_t i = delay_frames; i < played.size(); ++i) {
        captured[i] = played[i - delay_frames];
    }

    std::uint64_t index = 0;
    for (std::size_t offset = 0; offset + kBlock <= played.size();
         offset += kBlock) {
        estimator.on_render_played(played.data() + offset, kBlock, index);
        estimator.on_capture(captured.data() + offset, kBlock, index);
        index += kBlock;
    }
}

// ---------------------------------------------------------------------------
// 1. The positive control
// ---------------------------------------------------------------------------

TEST(aec_delay, a_known_delay_is_measured) {
    AecDelayEstimator estimator(test_config());

    // A pseudo-random signal has a flat spectrum, so the correlation
    // has ONE clear peak (unlike a sine, whose periodic peaks make the
    // lag ambiguous).
    Lcg rng(0xC0FFEEULL);
    std::vector<float> signal(48000);   // one second
    for (std::size_t i = 0; i < signal.size(); ++i) {
        signal[i] = rng.next();
    }

    constexpr std::size_t kDelayFrames = 960;   // 20 ms at 48 kHz
    feed_stream(estimator, signal, kDelayFrames);

    const std::optional<std::int32_t> delay = estimator.delay_ms();
    ASSERT_TRUE(delay.has_value())
        << "the estimator produced no estimate for a signal it could "
           "correlate (confidence " << estimator.confidence() << ")";

    // A few milliseconds of tolerance: the estimate is a *sample* count
    // converted to milliseconds, so it lands on a frame boundary.
    EXPECT_NEAR(*delay, 20, 3) << "the measured delay is wrong";
    EXPECT_GT(estimator.confidence(), 0.9f)
        << "a perfectly correlated signal must give a high confidence";
}

TEST(aec_delay, a_zero_delay_is_measured_as_zero) {
    AecDelayEstimator estimator(test_config());

    Lcg rng(0xBEEFULL);
    std::vector<float> signal(48000);
    for (std::size_t i = 0; i < signal.size(); ++i) {
        signal[i] = rng.next();
    }

    feed_stream(estimator, signal, 0);

    const std::optional<std::int32_t> delay = estimator.delay_ms();
    ASSERT_TRUE(delay.has_value());
    EXPECT_NEAR(*delay, 0, 2)
        << "an undelayed signal must be measured as (almost) zero";
}

// ---------------------------------------------------------------------------
// 2. The negative control — the estimator must NOT guess
// ---------------------------------------------------------------------------

TEST(aec_delay, uncorrelated_signals_do_not_produce_a_confident_estimate) {
    AecDelayEstimator estimator(test_config());

    // Two *different* pseudo-random sequences: statistically
    // uncorrelated, but exactly reproducible.
    Lcg rng_a(1);
    Lcg rng_b(2);
    std::vector<float> played(48000);
    std::vector<float> captured(48000);
    for (std::size_t i = 0; i < played.size(); ++i) {
        played[i] = rng_a.next();
        captured[i] = rng_b.next();
    }

    // Feed them on the same timeline, block by block.
    constexpr std::size_t kBlock = 480;
    std::uint64_t index = 0;
    for (std::size_t offset = 0; offset + kBlock <= played.size();
         offset += kBlock) {
        estimator.on_render_played(
            played.data() + offset, kBlock, index);
        estimator.on_capture(
            captured.data() + offset, kBlock, index);
        index += kBlock;
    }

    const std::optional<std::int32_t> delay = estimator.delay_ms();
    if (delay.has_value()) {
        // A weak peak *may* squeak past the threshold on some data; what
        // must not happen is a *confident* claim of a specific delay.
        EXPECT_LT(estimator.confidence(), 0.5f)
            << "the estimator confidently reported " << *delay
            << " ms for two uncorrelated signals";
    } else {
        EXPECT_LT(estimator.confidence(), 0.35f);
    }
}

TEST(aec_delay, silence_produces_no_estimate) {
    AecDelayEstimator estimator(test_config());

    const std::vector<float> silence(48000, 0.0f);
    feed_stream(estimator, silence, 0);

    EXPECT_FALSE(estimator.delay_ms().has_value())
        << "silence carries no delay information";
    EXPECT_EQ(estimator.confidence(), 0.0f);
}

TEST(aec_delay, no_estimate_before_enough_data_has_arrived) {
    AecDelayEstimator estimator(test_config());

    Lcg rng(7);
    std::vector<float> signal(480, 0.0f);
    for (std::size_t i = 0; i < signal.size(); ++i) {
        signal[i] = rng.next();
    }

    // A single block is far too little: the correlation window alone is
    // 1024 frames.
    feed_with_delay(estimator, signal, 960, 0);

    EXPECT_FALSE(estimator.delay_ms().has_value())
        << "the estimator must not report an estimate before it has "
           "enough data to correlate";
}

// ---------------------------------------------------------------------------
// 3. The override path
// ---------------------------------------------------------------------------

TEST(aec_delay, a_configured_delay_bypasses_the_correlation) {
    AecDelayConfig cfg = test_config();
    cfg.configured_delay_ms = 35;

    AecDelayEstimator estimator(cfg);

    // Even with plenty of (uncorrelated) data, the configured value is
    // what is reported.
    Lcg rng(11);
    std::vector<float> signal(48000);
    for (std::size_t i = 0; i < signal.size(); ++i) {
        signal[i] = rng.next();
    }
    feed_stream(estimator, signal, 0);

    ASSERT_TRUE(estimator.uses_configured_delay());
    const std::optional<std::int32_t> delay = estimator.delay_ms();
    ASSERT_TRUE(delay.has_value());
    EXPECT_EQ(*delay, 35);
    EXPECT_EQ(estimator.confidence(), 1.0f)
        << "a configured delay is trusted by definition";
}

TEST(aec_delay, a_configured_delay_of_zero_is_a_real_configuration) {
    AecDelayConfig cfg = test_config();
    cfg.configured_delay_ms = 0;

    AecDelayEstimator estimator(cfg);

    EXPECT_TRUE(estimator.uses_configured_delay());
    const std::optional<std::int32_t> delay = estimator.delay_ms();
    ASSERT_TRUE(delay.has_value());
    EXPECT_EQ(*delay, 0);
}

// ---------------------------------------------------------------------------
// 4. The headroom
// ---------------------------------------------------------------------------

TEST(aec_delay, the_headroom_is_the_measured_delay_plus_the_configured_one) {
    AecDelayConfig cfg = test_config();
    cfg.headroom_ms = 10;

    AecDelayEstimator estimator(cfg);

    Lcg rng(0xABCDEFULL);
    std::vector<float> signal(48000);
    for (std::size_t i = 0; i < signal.size(); ++i) {
        signal[i] = rng.next();
    }

    constexpr std::size_t kDelayFrames = 960;   // 20 ms
    feed_stream(estimator, signal, kDelayFrames);

    const std::optional<std::int32_t> measured = estimator.delay_ms();
    const std::optional<std::int32_t> headroom =
        estimator.aec_delay_headroom_ms();
    ASSERT_TRUE(measured.has_value());
    ASSERT_TRUE(headroom.has_value());
    EXPECT_EQ(*headroom, *measured + cfg.headroom_ms);
    EXPECT_NEAR(*headroom, 30, 3);
}

TEST(aec_delay, there_is_no_headroom_without_an_estimate) {
    AecDelayEstimator estimator(test_config());

    EXPECT_FALSE(estimator.aec_delay_headroom_ms().has_value())
        << "the headroom must not be reported without a measurement";
}

TEST(aec_delay, the_configured_delay_gets_the_headroom_too) {
    AecDelayConfig cfg = test_config();
    cfg.configured_delay_ms = 20;
    cfg.headroom_ms = 15;

    AecDelayEstimator estimator(cfg);

    const std::optional<std::int32_t> headroom =
        estimator.aec_delay_headroom_ms();
    ASSERT_TRUE(headroom.has_value());
    EXPECT_EQ(*headroom, 35);
}

// ---------------------------------------------------------------------------
// 5. The monitor — the clock-indexed path
// ---------------------------------------------------------------------------

TEST(aec_delay_monitor, the_monitor_indexes_by_the_shared_clock) {
    AecDelayMonitor monitor(test_config());

    Lcg rng(3);
    const std::size_t block = 480;   // 10 ms at 48 kHz
    std::vector<float> signal(block);
    for (std::size_t i = 0; i < signal.size(); ++i) {
        signal[i] = rng.next();
    }

    // The captured signal lags the played one by 20 ms: at 48 kHz that
    // is 960 frames, i.e. 20 000 microseconds of wall clock.
    constexpr std::int64_t kDelayUs = 20000;

    std::int64_t now = 1000000;
    std::int64_t captured_at = 1000000 + kDelayUs;
    for (int i = 0; i < 100; ++i) {
        monitor.on_render_played(signal.data(), block, now);
        monitor.on_capture(signal.data(), block, captured_at);
        now += 10000;              // 10 ms
        captured_at += 10000;
    }

    const std::optional<std::int32_t> delay = monitor.delay_ms();
    ASSERT_TRUE(delay.has_value())
        << "the monitor produced no estimate (confidence "
           << monitor.confidence() << ")";
    EXPECT_NEAR(*delay, 20, 3);
}

TEST(aec_delay_monitor, a_reset_clears_the_estimate) {
    AecDelayMonitor monitor(test_config());

    Lcg rng(5);
    std::vector<float> signal(480);
    for (std::size_t i = 0; i < signal.size(); ++i) {
        signal[i] = rng.next();
    }
    for (int i = 0; i < 100; ++i) {
        monitor.on_render_played(signal.data(), signal.size(),
                                 static_cast<std::int64_t>(i) * 10000);
        monitor.on_capture(signal.data(), signal.size(),
                           static_cast<std::int64_t>(i) * 10000);
    }

    monitor.reset();
    EXPECT_FALSE(monitor.delay_ms().has_value());
    EXPECT_EQ(monitor.confidence(), 0.0f);
}

} // namespace
