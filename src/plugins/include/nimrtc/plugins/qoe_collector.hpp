/**
 * @file nimrtc/plugins/qoe_collector.hpp
 * @brief IQoeCollector — the quality-of-experience collector seam
 *        (remote-desktop plan **J3** / **J6**).
 *
 * ## What this seam is for
 *
 * A remote-desktop session is judged on **feel**, not on throughput.
 * The operator notices three things and nothing else:
 *
 *   - the picture **freezing** (the cursor moves, the screen does not);
 *   - the **latency** between a click and the picture reacting;
 *   - the picture being **blurry**.
 *
 * Plan item **J3** asks for the metrics that describe the experience,
 * and item **J6** asks for the one that matters most for remote
 * control — the **input-to-photon** latency: from the operator's input
 * event to the frame that reflects it being displayed.
 *
 * ## Why a seam and not a direct call
 *
 * The metric *implementation* — the bounded percentile estimator, the
 * sharpness proxy, the freeze accounting — is a **module**
 * (`src/modules/rd_qoe/`), and the engine may not include a concrete
 * module's header (Layout Invariant 4 / ADR-025).  So the engine
 * reaches it through this seam, exactly as it reaches the bitrate
 * controller and the control channel.
 *
 * The seam is deliberately **narrow**: it is an *event sink* with a
 * *snapshot* out.  The engine pushes the events it already knows about
 * (a frame reached the viewer, a frame was dropped, an input event went
 * out) and reads the aggregate back.  It does not compute anything
 * itself, so there is exactly one implementation of "what a freeze is".
 *
 * ## The clock
 *
 * Every timestamp on this seam is on the **host's monotonic clock**, in
 * microseconds — the same clock the engine's `now_us()` uses.  The
 * capture timestamp and the render timestamp must be on the *same*
 * clock for the end-to-end latency to mean anything, and the engine is
 * the party that knows both.
 *
 * The **peer's** clock is a different matter: the capture timestamp of
 * an inbound frame is reconstructed from the peer's Sender Report
 * (the ADR-017 timeline), so it is already normalised onto the local
 * timeline before it reaches this seam.
 *
 * ## Threading
 *
 * Not thread-safe, like every other pipeline object in this codebase.
 * The engine's tick thread is the only driver.
 *
 * @see docs/plan/remote-desktop-plan.md §Track J (J3, J6).
 */

#ifndef NIMRTC_PLUGINS_QOE_COLLECTOR_HPP
#define NIMRTC_PLUGINS_QOE_COLLECTOR_HPP

// Stability classification: NIMRTC_API_STABLE (file-level marker from the
// v0.13.0 sweep; see ADR-015 / docs/plan/v0.13-plan.md).
//
// `nimrtc_plugins` links `nimrtc::core` PUBLIC (see src/plugins/CMakeLists.txt),
// so <nimrtc/core/api_version.hpp> is reachable from this header.
#include <nimrtc/core/api_version.hpp>   // NIMRTC_API_STABLE

#include <cstddef>
#include <cstdint>
#include <memory>

#include "nimrtc/plugins/base.hpp"

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// The configuration
// ---------------------------------------------------------------------------

/**
 * @brief The collector's configuration.
 *
 * The reservoir sizes are the collector's **memory bound**: a session
 * that runs for hours must not grow the process's memory, so the
 * latency percentiles are computed over the most recent N samples and
 * the running maximum is kept separately.
 */
struct NIMRTC_API_STABLE QoeConfig {
    /** How many latency samples the percentile estimator retains. */
    std::size_t latency_reservoir = 1024;

    /** How many input-to-photon samples the estimator retains. */
    std::size_t input_reservoir = 1024;

    /** The width of the sliding window the presented frame rate is
     *  measured over, in microseconds.  1 s by default. */
    std::int64_t fps_window_us = 1000000;

    /** How many in-flight input events may wait for their photon frame
     *  before the oldest one is forgotten. */
    std::size_t max_pending_inputs = 4096;
};

// ---------------------------------------------------------------------------
// The snapshot
// ---------------------------------------------------------------------------

/**
 * @brief The collector's metrics, as of the moment the snapshot was
 *        taken.
 *
 * This is the *seam's* shape — a flat value type the engine can publish
 * without knowing the module behind it.  The fields are the ones plan
 * item J3 names.
 */
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

    /** The worst end-to-end latency ever observed, in microseconds. */
    std::int64_t latency_max_us = 0;

    /** The input-to-photon latency at the 50th percentile, in
     *  microseconds (plan item J6). */
    std::int64_t input_to_photon_p50_us = 0;

    /** The input-to-photon latency at the 95th percentile, in
     *  microseconds. */
    std::int64_t input_to_photon_p95_us = 0;

    /** The worst input-to-photon latency ever observed, in
     *  microseconds. */
    std::int64_t input_to_photon_max_us = 0;

    /** The sharpness proxy of the presented picture.  Higher = sharper.
     *  0 until the host observes a picture. */
    double sharpness_proxy = 0.0;

    /** The fraction of the session that was spent frozen, in [0, 1]. */
    double freeze_ratio = 0.0;

    /** The session's duration as far as the collector can tell, in
     *  milliseconds (from the first to the last presented frame). */
    std::int64_t session_duration_ms = 0;

    /** The frame rate the source was producing, in frames per second.
     *  0 when the engine did not report one. */
    double source_fps = 0.0;
};

// ---------------------------------------------------------------------------
// The observation
// ---------------------------------------------------------------------------

/**
 * @brief One presented frame, as the collector sees it.
 */
struct NIMRTC_API_STABLE FrameObservation {
    /** The capture timestamp, on the host's monotonic clock, in
     *  microseconds. */
    std::int64_t capture_ts_us = 0;

    /** The instant the frame was displayed, on the same clock, in
     *  microseconds. */
    std::int64_t render_ts_us = 0;

    /** The frame's identifier (the pipeline's frame sequence). */
    std::uint64_t frame_id = 0;

    /** The frame rate the source was producing, in frames per second.
     *  0 when unknown. */
    double source_fps = 0.0;
};

// ---------------------------------------------------------------------------
// IQoeCollector
// ---------------------------------------------------------------------------

/**
 * @brief Collects the pipeline's events and computes the QoE metrics.
 *
 * The collector is **fed** — it never pulls from the engine.  The host
 * (or the engine, when the wiring lands) pushes:
 *
 *   - `on_frame_presented()` for every frame the viewer saw;
 *   - `on_frame_dropped()` for every frame the pipeline dropped;
 *   - `on_freeze_detected()` for every freeze;
 *   - `on_input_event_sent()` for every input event that went to the
 *     remote side;
 *   - `on_frame_presented_for_input()` for the frame that *reflects*
 *     one of those input events (the input-to-photon pairing, J6).
 */
class NIMRTC_API_STABLE IQoeCollector : public IPlugin {
public:
    ~IQoeCollector() override = default;

    // ---- IPlugin overrides -------------------------------------------------

    NIMRTC_API_STABLE const char* name() const noexcept override = 0;
    NIMRTC_API_STABLE Status      open() noexcept override = 0;
    NIMRTC_API_STABLE void        close() noexcept override = 0;

    // ---- The event surface -------------------------------------------------

    /**
     * @brief Report a frame the viewer saw.
     *
     * The frame's end-to-end latency (`render_ts_us - capture_ts_us`) is
     * fed to the latency percentile estimator, and the render timestamp
     * feeds the sliding window the presented frame rate is measured
     * over.
     */
    NIMRTC_API_STABLE virtual void on_frame_presented(
        const FrameObservation& observation) = 0;

    /**
     * @brief Report frames the pipeline dropped.
     * @param count  how many frames were dropped (0 is a no-op).
     */
    NIMRTC_API_STABLE virtual void on_frame_dropped(
        std::uint64_t count = 1) = 0;

    /**
     * @brief Report a freeze.
     *
     * @param duration_ms  how long the picture stood still, in
     *                     milliseconds.  Negative values are ignored.
     */
    NIMRTC_API_STABLE virtual void on_freeze_detected(
        std::int64_t duration_ms) = 0;

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
    NIMRTC_API_STABLE virtual void on_input_event_sent(
        std::int64_t ts_us, std::uint64_t event_id) = 0;

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
    NIMRTC_API_STABLE virtual void on_frame_presented_for_input(
        std::int64_t ts_us, std::uint64_t event_id) = 0;

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
    NIMRTC_API_STABLE virtual double observe_sharpness(
        const std::uint8_t* luma,
        std::size_t stride,
        std::size_t width,
        std::size_t height) = 0;

    /** Fold an externally computed sharpness value into the snapshot. */
    NIMRTC_API_STABLE virtual void on_sharpness_observed(double value) = 0;

    /** The metrics so far. */
    NIMRTC_API_STABLE virtual QoeSnapshot snapshot() const = 0;

    /** Forget everything and start over (a session reset). */
    NIMRTC_API_STABLE virtual void reset() noexcept = 0;
};

// ---------------------------------------------------------------------------
// The factory
// ---------------------------------------------------------------------------

/**
 * @brief The factory that produces QoE collectors.
 *
 * The engine resolves a factory through the registry's qoe-collector
 * slot and creates one collector from it.  The concrete implementation
 * lives in the `rd_qoe` module.
 */
class NIMRTC_API_STABLE IQoeCollectorFactory : public IPluginFactory {
public:
    ~IQoeCollectorFactory() override = default;

    NIMRTC_API_STABLE std::string_view id()          const noexcept override = 0;
    NIMRTC_API_STABLE std::string_view display_name() const noexcept override = 0;

    /** Create one collector.  Ownership passes to the caller. */
    NIMRTC_API_STABLE virtual std::unique_ptr<IQoeCollector> create(
        const QoeConfig& config) const = 0;
};

} // namespace nimrtc::plugins

#endif // NIMRTC_PLUGINS_QOE_COLLECTOR_HPP
