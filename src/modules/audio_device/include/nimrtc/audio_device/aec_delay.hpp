/**
 * @file nimrtc/audio_device/aec_delay.hpp
 * @brief Measuring the loudspeaker -> microphone loop delay (remote-desktop
 *        plan F4).
 *
 * ## The problem
 *
 * A remote-desktop agent that plays the peer's voice on the same machine
 * that holds the microphone will hear itself.  The acoustic echo canceller
 * removes that echo — but only if it knows how long the echo took to come
 * back.
 *
 * The loop has three parts:
 *
 *   1. the **playout** latency — the frames the sink queued but the
 *      loudspeaker has not played yet (the F3 backlog);
 *   2. the **acoustic** path — the speaker diaphragm, the air and the
 *      microphone diaphragm (a few milliseconds to tens of milliseconds);
 *   3. the **capture** latency — the frames the microphone's buffer still
 *      holds.
 *
 * The estimate this class produces is (2) — the acoustic part — which is
 * the part that *depends on the room* and therefore the part that has to
 * be *measured*.  The host adds (1) and (3) as headroom.
 *
 * ## How it is measured
 *
 * The loudspeaker signal is *known*; the microphone signal *contains* it,
 * delayed and attenuated.  So the delay is found by **cross-correlation**:
 * the estimator keeps a sliding window of the played-out signal and a
 * sliding window of the captured signal, and searches for the lag that
 * maximises the normalised correlation.
 *
 * The work is bounded and allocation-free in steady state:
 *
 *   - both signals live in fixed-size ring buffers;
 *   - the correlation is computed over a *short* window (a few tens of
 *     milliseconds) at a *coarse* cadence (a few tens of milliseconds),
 *     not on every 10 ms frame;
 *   - the search is over a bounded lag range.
 *
 * ## Honesty
 *
 * The estimator never guesses:
 *
 *   - it reports `nullopt` until it has enough data to correlate at all;
 *   - it reports a **confidence** (the normalised peak correlation), so a
 *     host can refuse a low-confidence estimate;
 *   - the result is **smoothed** (median of the recent estimates), so one
 *     bad correlation cannot move the AEC delay;
 *   - a **configured** delay (the host knows the loop, e.g. a fixed
 *     hardware path) bypasses the correlation entirely.
 *
 * ## The timeline
 *
 * The estimator correlates by *content*, so the caller has to give the two
 * signals a common time base: the absolute sample index of the first
 * sample of each block, on a timeline both streams share.  A host that
 * knows the playout instant of a block and the capture instant of a
 * block can derive it; a host that does not can use the convenience
 * overload, which assumes the two streams advance in lockstep.
 *
 * @note Remote-desktop plan F4 (M2-b).
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace nimrtc::audio_device {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/**
 * @brief How the AEC-delay estimator is configured.
 */
struct NIMRTC_API_STABLE AecDelayConfig {
    /** The rate both signals are sampled at (Hz).  Only used to convert
     *  the lag to milliseconds. */
    std::uint32_t sample_rate_hz = 48000;

    /** The correlation window, in frames (samples per channel).
     *
     *  This is the number of *samples* the correlation is summed over.
     *  It must be large enough to contain a few periods of speech (so
     *  the correlation is meaningful) and small enough to be cheap:
     *  1024 frames is ~21 ms at 48 kHz.
     */
    std::size_t correlation_frames = 1024;

    /** The largest lag the estimator searches, in frames.
     *
     *  960 frames is 20 ms at 48 kHz — comfortably above the acoustic
     *  delay of a desktop (speaker to microphone).  A larger value costs
     *  proportionally more.
     */
    std::size_t max_delay_frames = 960;

    /** The margin (ms) added to the measured delay.
     *
     *  The headroom covers what the estimator does *not* measure: the
     *  playout backlog (F3) and the capture-side buffering.  It is the
     *  value the host should hand the 3A as `aec_delay_headroom_ms`.
     */
    std::int32_t headroom_ms = 10;

    /** The configured delay (ms), when the host knows it.
     *
     *  When this is set, the estimator does not correlate at all: it
     *  reports this value.  A configured delay of 0 ms is a legitimate
     *  configuration (a wired loop), so the field is an `optional` and
     *  not a sentinel.
     */
    std::optional<std::int32_t> configured_delay_ms;

    /** The minimum normalised peak correlation an estimate must reach
     *  to be trusted, in [0, 1]. */
    float confidence_threshold = 0.35f;

    /** How many recent estimates the median smoother keeps.
     *
     *  1 disables the smoothing (the raw estimate is used).  5 is the
     *  default: enough to ride out one bad correlation, small enough
     *  that the estimate still follows a real change.
     */
    std::size_t smoothing_window = 5;

    /** How many frames must pass between two correlation runs.
     *
     *  The correlation is the expensive part; running it on every 10 ms
     *  frame would be wasteful.  2048 frames is ~43 ms at 48 kHz.
     */
    std::size_t estimate_interval_frames = 2048;
};

// ---------------------------------------------------------------------------
// AecDelayEstimator
// ---------------------------------------------------------------------------

/**
 * @brief Estimates the acoustic loop delay by cross-correlating the
 *        played-out signal with the captured signal.
 */
class NIMRTC_API_STABLE AecDelayEstimator {
public:
    explicit AecDelayEstimator(AecDelayConfig cfg = {}) noexcept;

    AecDelayEstimator(const AecDelayEstimator&)            = delete;
    AecDelayEstimator& operator=(const AecDelayEstimator&) = delete;

    /** Reconfigure the estimator (and drop the history). */
    void set_config(AecDelayConfig cfg) noexcept;

    /** The configuration in force. */
    [[nodiscard]] const AecDelayConfig& config() const noexcept { return cfg_; }

    /**
     * Feed the signal that was played out.
     *
     * @param samples     The interleaved float PCM that reached the
     *                    loudspeaker (mono is the normal case).
     * @param num_frames  Samples per channel.
     * @param frame_index The absolute sample index of the first sample,
     *                    on the timeline shared with `on_capture()`.
     */
    void on_render_played(const float* samples,
                           std::size_t num_frames,
                           std::uint64_t frame_index) noexcept;

    /**
     * Feed the signal that was captured.
     *
     * @param samples     The interleaved float PCM from the microphone.
     * @param num_frames  Samples per channel.
     * @param frame_index The absolute sample index of the first sample,
     *                    on the timeline shared with
     *                    `on_render_played()`.
     */
    void on_capture(const float* samples,
                    std::size_t num_frames,
                    std::uint64_t frame_index) noexcept;

    /**
     * Feed both signals in lockstep.
     *
     * The convenience path for a host whose two streams advance together
     * (the render reference and the microphone capture, both at the
     * session's sample rate).  The shared timeline is the estimator's
     * own counter, so the caller must feed the *same* number of frames
     * to both calls.
     */
    void on_render_played(const float* samples, std::size_t num_frames) noexcept;

    /** The lockstep counterpart of the two-argument `on_render_played()`. */
    void on_capture(const float* samples, std::size_t num_frames) noexcept;

    /**
     * The measured acoustic delay, in milliseconds.
     *
     * `nullopt` when the estimator has not yet produced a *trusted*
     * estimate: too little data, or a peak correlation below the
     * confidence threshold.  When a delay is configured, the configured
     * value is reported (and the correlation is not used at all).
     */
    [[nodiscard]] std::optional<std::int32_t> delay_ms() const noexcept;

    /**
     * The normalised peak correlation of the last correlation run, in
     * [0, 1].  1.0 means a perfect match.  0 means "no correlation
     * found" (or "no run yet").
     */
    [[nodiscard]] float confidence() const noexcept;

    /**
     * The value the host should hand the 3A as
     * `aec_delay_headroom_ms`: the measured delay plus the configured
     * headroom.
     *
     * `nullopt` when there is no trusted estimate yet.
     */
    [[nodiscard]] std::optional<std::int32_t> aec_delay_headroom_ms()
        const noexcept;

    /** True when the estimator is using a configured delay. */
    [[nodiscard]] bool uses_configured_delay() const noexcept;

    /** Drop the history and the smoothed estimate. */
    void reset() noexcept;

private:
    /** One correlation run: find the lag that maximises the correlation. */
    void run_correlation() noexcept;

    /** Push a raw estimate into the median smoother. */
    void push_estimate(std::int32_t delay_ms) noexcept;

    /** The smoothed estimate (median of the recent estimates). */
    [[nodiscard]] std::optional<std::int32_t> smoothed_delay_ms() const noexcept;

    AecDelayConfig cfg_;

    /** Guards the rings, the counters and the history.  The sink and
     *  the source are two different devices on two different threads,
     *  so the estimator is internally synchronised. */
    mutable std::mutex mutex_;

    /** The played-out history, in a ring indexed by absolute frame. */
    std::vector<float> played_;
    std::uint64_t played_write_ = 0;   // total frames fed
    std::uint64_t played_origin_ = 0;  // index of the first frame ever fed
    std::uint64_t played_next_ = 0;    // index the next block must start at
    bool played_have_origin_ = false;

    /** The captured history, in a ring indexed by absolute frame. */
    std::vector<float> captured_;
    std::uint64_t captured_write_ = 0;
    std::uint64_t captured_origin_ = 0;
    std::uint64_t captured_next_ = 0;
    bool captured_have_origin_ = false;

    /** Scratch space for the mono view of a block (one per side). */
    std::vector<float> played_scratch_;
    std::vector<float> captured_scratch_;

    /** The lockstep timeline (one counter per side). */
    std::uint64_t lockstep_play_index_ = 0;
    std::uint64_t lockstep_capture_index_ = 0;

    /** The frames consumed since the last correlation run. */
    std::uint64_t frames_since_estimate_ = 0;

    /** The recent raw estimates (for the median smoother). */
    std::vector<std::int32_t> history_;

    /** The last measured peak confidence. */
    float confidence_ = 0.0f;

    /** The last measured delay (before smoothing), in ms. */
    std::int32_t last_raw_delay_ms_ = 0;

    /** True when the last correlation run produced a trusted peak. */
    bool have_estimate_ = false;

    /** The number of correlation runs. */
    std::uint64_t correlation_runs_ = 0;
};

// ---------------------------------------------------------------------------
// AecDelayMonitor
// ---------------------------------------------------------------------------

/**
 * @brief The host-facing wrapper around the estimator.
 *
 * The estimator wants a *shared* timeline: the played-out block and the
 * captured block must be indexed on the same axis, or the lag the
 * correlation finds is meaningless.  A host that has two independent
 * devices (a loudspeaker and a microphone) does not have such an axis —
 * but it does have a **clock**: both devices are driven by the same
 * monotonic wall clock, so the instant a block was played and the instant
 * a block was captured are directly comparable.
 *
 * The monitor therefore indexes blocks by their steady-clock instant:
 * `index = (now_us - origin_us) * sample_rate / 1e6`.  The host passes
 * the instant; the monitor does the arithmetic.  A host that already
 * has a sample-accurate shared timeline should use the estimator
 * directly.
 */
class NIMRTC_API_STABLE AecDelayMonitor {
public:
    explicit AecDelayMonitor(AecDelayConfig cfg = {}) noexcept;

    /** The underlying estimator (for the sample-accurate path). */
    [[nodiscard]] AecDelayEstimator& estimator() noexcept { return estimator_; }

    /** The underlying estimator (read-only). */
    [[nodiscard]] const AecDelayEstimator& estimator() const noexcept {
        return estimator_;
    }

    /**
     * Feed the block that was played out.
     *
     * @param samples    The interleaved float PCM that reached the
     *                   loudspeaker.
     * @param num_frames Samples per channel.
     * @param now_us     The monotonic instant (microseconds) at which
     *                   the block was played.
     */
    void on_render_played(const float* samples,
                           std::size_t num_frames,
                           std::int64_t now_us) noexcept;

    /**
     * Feed the block that was captured.
     *
     * @param samples    The interleaved float PCM from the microphone.
     * @param num_frames Samples per channel.
     * @param now_us     The monotonic instant (microseconds) at which
     *                   the block was captured.
     */
    void on_capture(const float* samples,
                    std::size_t num_frames,
                    std::int64_t now_us) noexcept;

    /** The measured acoustic delay, in milliseconds (see the estimator). */
    [[nodiscard]] std::optional<std::int32_t> delay_ms() const noexcept;

    /** The value to hand the 3A as `aec_delay_headroom_ms`. */
    [[nodiscard]] std::optional<std::int32_t> aec_delay_headroom_ms()
        const noexcept;

    /** The normalised peak correlation of the last correlation run. */
    [[nodiscard]] float confidence() const noexcept;

    /** Drop the history and the smoothed estimate. */
    void reset() noexcept;

private:
    /** The shared-clock sample index for an instant. */
    std::uint64_t index_at(std::int64_t now_us) noexcept;

    AecDelayEstimator estimator_;

    /** Guards the origin bookkeeping below: the monitor is fed from
     *  the device thread (capture) and the render thread (playout),
     *  so the origin can be set concurrently. */
    mutable std::mutex mutex_;

    std::int64_t origin_us_ = 0;
    bool have_origin_ = false;
};

// ---------------------------------------------------------------------------
// The attach seam
// ---------------------------------------------------------------------------

/**
 * @brief Implemented by the module's own sinks so a host can attach an
 *        `AecDelayMonitor` to them.
 *
 * The device layer owns the *played-out* signal (it is the only party
 * that sees what actually came out of the loudspeaker), so the monitor
 * has to be attached to the sink rather than the other way round.
 *
 * A host uses the module's `attach_aec_delay_monitor()` helper, which
 * dynamic-casts to this interface; a sink that is not one of the
 * module's own (a third-party sink) simply does not get a monitor, and
 * the host can feed the estimator by hand instead.
 */
class NIMRTC_API_STABLE IAecDelaySink {
public:
    virtual ~IAecDelaySink() = default;

    /** Attach (or detach, with nullptr) the monitor. */
    virtual void set_aec_delay_monitor(AecDelayMonitor* monitor) noexcept = 0;

    /**
     * The attached monitor, or nullptr when none is attached.
     *
     * A host that wants the *current* estimate reads it from here
     * instead of keeping its own pointer: the module's accessors
     * (`aec_delay_ms()` / `aec_delay_headroom_ms()`) reach the monitor
     * through this seam, so a sink that is not one of the module's own
     * simply reports "no estimate" rather than a wrong one.
     */
    [[nodiscard]] virtual AecDelayMonitor* aec_delay_monitor() const noexcept = 0;
};

/**
 * @brief The capture-side counterpart of `IAecDelaySink`.
 *
 * The module's own capture devices feed the monitor with the block they
 * captured, so that the estimator sees the microphone side of the loop.
 */
class NIMRTC_API_STABLE IAecDelaySource {
public:
    virtual ~IAecDelaySource() = default;

    /** Attach (or detach, with nullptr) the monitor. */
    virtual void set_aec_delay_monitor(AecDelayMonitor* monitor) noexcept = 0;

    /** The attached monitor, or nullptr when none is attached. */
    [[nodiscard]] virtual AecDelayMonitor* aec_delay_monitor() const noexcept = 0;
};

} // namespace nimrtc::audio_device
