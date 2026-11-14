/**
 * @file nimrtc/rd_qoe/qoe_metrics.hpp
 * @brief The remote-desktop quality-of-experience metrics (plan Track J, J3 / J6).
 *
 * ## Why this exists
 *
 * The remote-desktop product is judged on *feel*, not on throughput:
 * latency, sharpness and responsiveness are what the operator notices.
 * The plan's item **J3** therefore asks for the metrics that describe the
 * experience, and item **J6** asks for the one that matters most for
 * remote control — the **input-to-photon** latency.
 *
 * This module is the *measurement* half of that work.  It defines:
 *
 *   - `PercentileEstimator` — a **bounded** latency percentile estimator
 *     (a fixed-size reservoir plus a running max), so a session that runs
 *     for hours cannot grow the process's memory;
 *   - `SharpnessEstimator` — a cheap, well-defined **sharpness proxy**
 *     computed from the luma plane of the presented picture;
 *   - `QoeCollector` — the observer that turns the pipeline's events
 *     (frames presented, frames dropped, freezes, input events) into a
 *     `QoeSnapshot`.
 *
 * ## The collector is *fed*, it does not pull
 *
 * `QoeCollector` never reaches into the engine.  The host (or the engine,
 * when the wiring lands) pushes events into it through the `on_*` methods.
 * That keeps the module:
 *
 *   - **decoupled** — the module depends on `nimrtc::core` and nothing
 *     else, so it can be unit-tested without a session;
 *   - **testable** — a test drives a synthetic frame timeline and asserts
 *     on the resulting metrics;
 *   - **engine-agnostic** — the same collector serves the remote-desktop
 *     client, a benchmark harness, or a test rig.
 *
 * ## The metrics
 *
 * | Metric | Meaning |
 * |---|---|
 * | `frames_presented` | frames the viewer actually saw |
 * | `frames_dropped`  | frames the pipeline dropped |
 * | `freeze_count` / `freeze_total_ms` / `freeze_longest_ms` | how long the picture stood still |
 * | `presented_fps` | the rate the **viewer** got (not the rate the source produced) |
 * | `latency_p50/p95/p99_us` | the **end-to-end** latency (render timestamp − capture timestamp) |
 * | `input_to_photon_p50/p95_us` | the operator's input event → the frame that reflects it, displayed |
 * | `sharpness_proxy` | how sharp the delivered picture is (higher = sharper) |
 * | `freeze_ratio` | the fraction of the session spent frozen |
 *
 * ## The bounded-percentile trade-off
 *
 * A remote-desktop session runs for *hours*.  An unbounded sample list is
 * a leak, so the latency percentiles come from a **fixed-capacity
 * reservoir** (`PercentileEstimator`, default capacity 1024 samples):
 * the most recent N samples are kept, and the percentile is computed over
 * them.  The bound is explicit (`capacity()`), the retained count is
 * explicit (`sample_count()`), and the **running max** is kept separately
 * so the worst case never disappears from the summary.
 *
 * The trade-off is stated plainly: the percentiles describe the **recent
 * window** of the session, not its whole history.  A latency excursion
 * that happened ten minutes ago is reflected in `max()` but not in the
 * percentiles.  That is the right trade for a live dashboard — a remote
 * desktop that recovers should *show* that it recovered — and the
 * alternative (an unbounded list) is not an option for a long-lived
 * process.
 *
 * ## The sharpness proxy
 *
 * `SharpnessEstimator::compute()` returns the **mean absolute Laplacian**
 * of the luma plane: the average, over the interior pixels, of
 * `|4·C − U − D − L − R|`.  It is a *proxy*, not a reference-based metric:
 *
 *   - it needs **no** reference picture, so it can be computed on the
 *     live decoded picture at the viewer;
 *   - it is **cheap** (one pass over the luma plane, no convolution
 *     kernel beyond the 3×3 cross);
 *   - it is **monotone in the right direction**: a hard edge scores high,
 *     the same content box-filtered scores low — so *higher means
 *     sharper*;
 *   - it is **scale-sensitive** (a larger picture of the same content
 *     scores differently) and it reacts to noise, so it compares pictures
 *     of the same resolution against each other, not across resolutions.
 *
 * @note Thread-safety: not thread-safe, like every other pipeline object
 *       in this codebase.  One collector per observing thread.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace nimrtc::rd_qoe {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/** The collector's configuration. */
struct NIMRTC_API_STABLE QoeConfig {
    /** How many latency samples the percentile estimator keeps.
     *
     *  This is the module's **memory bound** for the end-to-end latency
     *  distribution: the estimator retains at most this many samples
     *  (the most recent ones).  The default of 1024 samples is 8 KiB of
     *  `std::int64_t` — a rounding error next to a decoded frame. */
    std::size_t latency_reservoir = 1024;

    /** How many input-to-photon samples the percentile estimator keeps.
     *  Same bound, same rationale. */
    std::size_t input_reservoir = 1024;

    /** The width of the sliding window the presented frame rate is
     *  measured over, in microseconds.  1 s by default. */
    std::int64_t fps_window_us = 1000000;

    /** How many in-flight input events may be waiting for their
     *  photon frame before the oldest one is forgotten.
     *
     *  An input event whose frame never arrives (the session died, the
     *  frame was lost for good) would otherwise pin an entry forever.
     *  The bound is 4096 events. */
    std::size_t max_pending_inputs = 4096;
};

// ---------------------------------------------------------------------------
// The frame observation
// ---------------------------------------------------------------------------

/** One presented frame, as the collector sees it. */
struct NIMRTC_API_STABLE FrameObservation {
    /** The capture timestamp, on the host's monotonic clock, in
     *  microseconds.  The **end-to-end latency** is measured against
     *  this: `render_ts_us - capture_ts_us`. */
    std::int64_t capture_ts_us = 0;

    /** The timestamp at which the frame was displayed, on the same
     *  monotonic clock, in microseconds. */
    std::int64_t render_ts_us = 0;

    /** The frame's identifier (the pipeline's frame id / sequence). */
    std::uint64_t frame_id = 0;

    /** The id of the frame this one *references* — the frame whose
     *  content it carries (the "ref frame" of the encoder's dependency
     *  chain).  A frame whose reference is far in the past is a frame
     *  that waited for a keyframe. */
    std::uint64_t ref_frame_id = 0;

    /** The frame rate the source was producing, in frames per second.
     *  0 when unknown. */
    double source_fps = 0.0;
};

// ---------------------------------------------------------------------------
// The percentile estimator
// ---------------------------------------------------------------------------

/**
 * @brief A bounded latency percentile estimator.
 *
 * The estimator keeps the **most recent** `capacity()` samples in a
 * fixed-size ring buffer and answers percentile queries over them.  The
 * memory use is therefore **constant**: a session that runs for hours
 * costs exactly `capacity()` samples, whatever happens on the wire.
 *
 * Alongside the reservoir the estimator tracks the running **max** and
 * **min**, so the worst case of the whole session is never lost when the
 * reservoir rolls over.
 *
 * ## The percentile definition
 *
 * `percentile(q)` uses the **nearest-rank** method on the sorted
 * reservoir: the returned value is the sample at index
 * `ceil(q · n) − 1` (clamped into range).  With no samples it returns 0.
 *
 * ## The trade-off
 *
 * The percentiles describe the recent window, not the whole session —
 * see the file-level comment.  The bound is public (`capacity()`) and the
 * retained count is public (`sample_count()`), so a host can assert on
 * them.
 */
class NIMRTC_API_STABLE PercentileEstimator {
public:
    /** The default reservoir capacity, in samples. */
    static constexpr std::size_t kDefaultCapacity = 1024;

    /**
     * @param capacity  how many samples to retain.  0 is treated as 1
     *                  (an estimator with no room could not answer
     *                  anything).
     */
    explicit PercentileEstimator(std::size_t capacity = kDefaultCapacity);

    /** Add one sample.  Older samples are dropped once the reservoir is
     *  full. */
    void add(std::int64_t value_us);

    /** Forget every sample. */
    void reset() noexcept;

    /**
     * @brief The q-th percentile of the retained samples (nearest-rank).
     *
     * @param q  the quantile, in [0, 1].  Values outside the range are
     *           clamped.
     * @return the percentile in microseconds, or 0 when no sample was
     *         ever added.
     */
    [[nodiscard]] std::int64_t percentile(double q) const;

    /** The largest sample ever added (not just the retained ones). */
    [[nodiscard]] std::int64_t max() const noexcept { return max_; }

    /** The smallest sample ever added (not just the retained ones). */
    [[nodiscard]] std::int64_t min() const noexcept { return min_; }

    /** The mean of **every** sample ever added, in microseconds. */
    [[nodiscard]] double mean() const noexcept;

    /** How many samples are currently retained.  Never exceeds
     *  `capacity()`. */
    [[nodiscard]] std::size_t sample_count() const noexcept { return samples_.size(); }

    /** The reservoir's capacity — the module's memory bound. */
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    /** How many samples were ever added, including the ones the
     *  reservoir has already dropped. */
    [[nodiscard]] std::uint64_t total_count() const noexcept { return total_; }

private:
    std::size_t   capacity_;
    std::vector<std::int64_t> samples_;  ///< the ring buffer
    std::size_t   next_ = 0;              ///< the ring's write cursor
    std::uint64_t total_ = 0;
    std::int64_t  max_ = 0;
    std::int64_t  min_ = 0;
    double        sum_ = 0.0;
    bool          seen_ = false;
};

// ---------------------------------------------------------------------------
// The sharpness estimator
// ---------------------------------------------------------------------------

/**
 * @brief A cheap sharpness proxy computed from the luma plane.
 *
 * The proxy is the **mean absolute Laplacian**:
 *
 * ```
 * value = (1 / N) · Σ |4·C(x,y) − U(x,y) − D(x,y) − L(x,y) − R(x,y)|
 * ```
 *
 * over the interior pixels of the plane (the border pixels are skipped
 * because the 3×3 cross would read outside the picture).
 *
 * **Higher means sharper.**  A hard black/white edge produces a large
 * second derivative and scores high; the same content box-filtered
 * spreads the edge over several pixels, the second derivative drops, and
 * the score drops with it.
 *
 * It is a **proxy**, not a reference-based metric: there is no
 * ground-truth picture, so the number is meaningful only *relative* to
 * another picture of the same resolution, and it is sensitive to noise
 * and to the picture's scale.
 */
class NIMRTC_API_STABLE SharpnessEstimator {
public:
    /**
     * @brief Compute the mean absolute Laplacian of a luma plane.
     *
     * @param luma    the first pixel of the plane (8-bit luma).  May be
     *                null, in which case the result is 0.
     * @param stride  the row stride in bytes (>= width).
     * @param width   the picture width in pixels.
     * @param height  the picture height in pixels.
     * @return the sharpness proxy; 0 for a degenerate picture
     *         (null plane, or smaller than 3×3 — there is no interior
     *         pixel to measure).
     */
    [[nodiscard]] static double compute(const std::uint8_t* luma,
                                        std::size_t stride,
                                        std::size_t width,
                                        std::size_t height);

    /**
     * @brief Compute the proxy and fold it into the running statistics.
     * @return the value that was computed.
     */
    double observe(const std::uint8_t* luma,
                    std::size_t stride,
                    std::size_t width,
                    std::size_t height);

    /**
     * @brief Fold an externally computed value into the running
     *        statistics.
     *
     * The host may already have a sharpness value from another source
     * (e.g. a GPU-side metric); this folds it in without pretending it
     * came from a luma plane.
     *
     * @return the value that was folded in.
     */
    double observe_value(double value);

    /** Forget every observed value. */
    void reset() noexcept;

    /** The most recently observed value (0 before the first one). */
    [[nodiscard]] double last() const noexcept { return last_; }

    /** The mean of every observed value. */
    [[nodiscard]] double mean() const noexcept;

    /** How many values were observed. */
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

private:
    double        last_ = 0.0;
    double        sum_ = 0.0;
    std::uint64_t count_ = 0;
};

// ---------------------------------------------------------------------------
// The snapshot
// ---------------------------------------------------------------------------

/** The collector's metrics, as of the moment the snapshot was taken. */
struct NIMRTC_API_STABLE QoeSnapshot {
    /** Frames the viewer actually saw. */
    std::uint64_t frames_presented = 0;

    /** Frames the pipeline dropped. */
    std::uint64_t frames_dropped = 0;

    /** How many freezes were detected. */
    std::uint64_t freeze_count = 0;

    /** The total time the picture stood still, in milliseconds. */
    std::int64_t freeze_total_ms = 0;

    /** The longest single freeze, in milliseconds. */
    std::int64_t freeze_longest_ms = 0;

    /** The frame rate the **viewer** received, in frames per second,
     *  measured over the sliding window.  0 before two frames have
     *  been presented. */
    double presented_fps = 0.0;

    /** The end-to-end latency (render − capture) at the 50th
     *  percentile, in microseconds. */
    std::int64_t latency_p50_us = 0;

    /** The end-to-end latency at the 95th percentile, in microseconds. */
    std::int64_t latency_p95_us = 0;

    /** The end-to-end latency at the 99th percentile, in microseconds. */
    std::int64_t latency_p99_us = 0;

    /** The input-to-photon latency at the 50th percentile, in
     *  microseconds (plan item J6). */
    std::int64_t input_to_photon_p50_us = 0;

    /** The input-to-photon latency at the 95th percentile, in
     *  microseconds. */
    std::int64_t input_to_photon_p95_us = 0;

    /** The sharpness proxy of the presented picture.  Higher = sharper.
     *  0 until the host observes a picture. */
    double sharpness_proxy = 0.0;

    /** The fraction of the session that was spent frozen, in [0, 1]. */
    double freeze_ratio = 0.0;

    // ---- context ----------------------------------------------------------

    /** The session's duration as far as the collector can tell, in
     *  milliseconds (from the first to the last presented frame). */
    std::int64_t session_duration_ms = 0;

    /** The frame rate the source was producing, in frames per second.
     *  0 when the host did not report one. */
    double source_fps = 0.0;

    /** The worst end-to-end latency ever observed, in microseconds. */
    std::int64_t latency_max_us = 0;

    /** The worst input-to-photon latency ever observed, in
     *  microseconds. */
    std::int64_t input_to_photon_max_us = 0;
};

// ---------------------------------------------------------------------------
// The collector
// ---------------------------------------------------------------------------

/**
 * @brief Collects the pipeline's events and computes the QoE metrics.
 *
 * The collector is **fed** — it never pulls from the engine.  The host
 * pushes:
 *
 *   - `on_frame_presented()` for every frame the viewer saw;
 *   - `on_frame_dropped()` for every frame the pipeline dropped;
 *   - `on_freeze_detected()` for every freeze (or the host can let the
 *     collector infer them from the frame gaps — see
 *     `note_freeze_threshold_ms()`);
 *   - `on_input_event_sent()` for every input event that went to the
 *     remote side;
 *   - `on_frame_presented_for_input()` for the frame that *reflects* one
 *     of those input events (the input-to-photon pairing, J6).
 *
 * Thread-safety: not thread-safe.  One collector per observing thread.
 */
class NIMRTC_API_STABLE QoeCollector {
public:
    explicit QoeCollector(const QoeConfig& config = QoeConfig{});

    /** The configuration this collector was built with. */
    [[nodiscard]] const QoeConfig& config() const noexcept { return config_; }

    /**
     * @brief Report a frame the viewer saw.
     *
     * The frame's end-to-end latency (`render_ts_us - capture_ts_us`) is
     * fed to the latency percentile estimator, and the render timestamp
     * feeds the sliding window the presented frame rate is measured over.
     */
    void on_frame_presented(const FrameObservation& observation);

    /**
     * @brief Report frames the pipeline dropped.
     * @param count  how many frames were dropped (0 is a no-op).
     */
    void on_frame_dropped(std::uint64_t count = 1);

    /**
     * @brief Report a freeze.
     *
     * @param duration_ms  how long the picture stood still, in
     *                      milliseconds.  Negative values are ignored.
     */
    void on_freeze_detected(std::int64_t duration_ms);

    /**
     * @brief Report an input event that was sent to the remote side.
     *
     * The event is remembered until the frame that reflects it is
     * presented; the pair is what the input-to-photon latency is
     * measured from (plan item J6).
     *
     * @param ts_us     when the event went out, on the host's monotonic
     *                  clock, in microseconds.
     * @param event_id  the event's identifier — the id the frame that
     *                  reflects it will carry.
     */
    void on_input_event_sent(std::int64_t ts_us, std::uint64_t event_id);

    /**
     * @brief Report the frame that reflects a previously sent input
     *        event.
     *
     * The pairing consumes the pending event: the latency is
     * `ts_us - sent_ts_us`, and it is fed to the input-to-photon
     * percentile estimator.  An `event_id` that was never sent is
     * ignored (there is no honest latency to report for it).
     *
     * This method does **not** count as a presented frame — the frame it
     * refers to is reported through `on_frame_presented()` as well, and
     * counting it twice would inflate the frame rate.
     *
     * @param ts_us     when the frame was displayed, in microseconds.
     * @param event_id  the input event the frame reflects.
     */
    void on_frame_presented_for_input(std::int64_t ts_us, std::uint64_t event_id);

    /**
     * @brief Compute the sharpness proxy of a presented picture and fold
     *        it into the snapshot.
     *
     * @param luma    the luma plane's first pixel.
     * @param stride  the row stride in bytes.
     * @param width   the picture width.
     * @param height  the picture height.
     * @return the value that was computed.
     */
    double observe_sharpness(const std::uint8_t* luma,
                             std::size_t stride,
                             std::size_t width,
                             std::size_t height);

    /** Fold an externally computed sharpness value into the snapshot. */
    void on_sharpness_observed(double value);

    /** The metrics so far. */
    [[nodiscard]] QoeSnapshot snapshot() const;

    /** Forget everything and start over (a session reset). */
    void reset() noexcept;

private:
    /** The presented frame rate over the sliding window. */
    [[nodiscard]] double compute_presented_fps() const;

    QoeConfig config_;

    PercentileEstimator latency_;
    PercentileEstimator input_to_photon_;
    SharpnessEstimator  sharpness_;

    std::uint64_t frames_presented_ = 0;
    std::uint64_t frames_dropped_ = 0;
    std::uint64_t freeze_count_ = 0;
    std::int64_t  freeze_total_ms_ = 0;
    std::int64_t  freeze_longest_ms_ = 0;

    /// The render timestamps inside the sliding window.
    std::deque<std::int64_t> fps_window_;

    /// The first / last presented frame's render timestamp.
    std::int64_t first_render_ts_us_ = 0;
    std::int64_t last_render_ts_us_ = 0;
    bool          have_frame_ = false;

    /// The most recently reported source frame rate.
    double source_fps_ = 0.0;

    /// The input events waiting for their photon frame.
    std::unordered_map<std::uint64_t, std::int64_t> pending_inputs_;

    /// The pending events in send order (for the FIFO eviction).
    std::deque<std::uint64_t> pending_order_;
};

} // namespace nimrtc::rd_qoe
