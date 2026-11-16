/**
 * @file nimrtc/audio_device/playback_progress.hpp
 * @brief Playback-progress accounting — how much audio the loudspeaker
 *        actually played out.
 *
 * ## Why the device layer needs this
 *
 * The acoustic echo canceller subtracts the *loudspeaker* signal from the
 * *microphone* signal.  For that subtraction to be correct, the AEC
 * reference must be aligned with the capture: the canceller has to know
 * which part of the played-out signal the microphone is currently
 * hearing.
 *
 * That alignment has two parts:
 *
 *   - **the count** — how many frames the device has consumed.  The
 *     engine's `set_render_delivered()` path wants exactly this: a
 *     monotonic count of the frames that reached the speaker.
 *   - **the lag** — how far behind the loudspeaker still is.  A sink
 *     that queues 200 ms of audio plays the signal 200 ms late, and
 *     the measured loop delay has to include that.
 *
 * The seam (`plugins::IAudioSink`) already carries the *count*
 * (`played_samples()`); this class is the portable bookkeeping that
 * hangs the *queue/pending* half off it, so that a host can wire the
 * two together without every backend re-implementing the arithmetic.
 *
 * ## The monotonicity contract
 *
 * The tracker maintains two counters:
 *
 *   - `frames_queued` — frames the sink accepted for playback;
 *   - `frames_played` — frames the device actually consumed.
 *
 * The played counter is **monotonic** (it never decreases) and it
 * **never exceeds** the queued counter.  A device that reports more
 * played frames than were ever queued is a bug in the device, and the
 * tracker clamps the over-report rather than letting the AEC reference
 * run ahead of reality.  The number of clamped over-reports is itself
 * reported (`stats().over_reports_clamped`), so the bug is visible.
 *
 * ## Thread safety
 *
 * `on_samples_queued()` is called on the producer side (the host's
 * thread) and `on_samples_played()` on the device thread.  The
 * counters are atomic, so both may be called concurrently; the
 * counters may be momentarily inconsistent with each other (the
 * producer can be a few frames ahead), which is exactly the real
 * situation and not an error.
 *
 * @note Remote-desktop plan F3 (M2-a).
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nimrtc::audio_device {

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

/**
 * @brief A snapshot of the playback-progress counters.
 *
 * The snapshot is not atomic as a whole: the counters are read one by
 * one, so a snapshot taken while the device is playing may show the
 * queued count a few frames ahead of the played count.  That is the
 * truth, not an error.
 */
struct NIMRTC_API_STABLE PlaybackProgressStats {
    /** Frames the sink accepted for playback (cumulative). */
    std::uint64_t frames_queued = 0;

    /** Frames the device actually consumed (cumulative, monotonic). */
    std::uint64_t frames_played = 0;

    /** Frames queued but not yet played (the backlog). */
    std::size_t frames_pending = 0;

    /** The monotonic instant (microseconds) of the last played block.
     *  0 when nothing has been played yet. */
    std::int64_t last_playback_us = 0;

    /** How many times the backlog exceeded the configured limit. */
    std::uint64_t backlog_limit_exceeded = 0;

    /** How many over-reports (played > queued) were clamped away. */
    std::uint64_t over_reports_clamped = 0;
};

// ---------------------------------------------------------------------------
// PlaybackProgressTracker
// ---------------------------------------------------------------------------

/**
 * @brief The portable bookkeeping behind the F3 playback-progress path.
 *
 * A sink calls `on_samples_queued()` when it accepts frames and
 * `on_samples_played()` when the device consumes them; the host polls
 * (or is pushed) the resulting counters.
 *
 * The tracker owns no audio buffers — it only counts.
 */
class NIMRTC_API_STABLE PlaybackProgressTracker {
public:
    /**
     * Construct a tracker.
     *
     * @param backlog_limit_frames The backlog (queued-but-not-played)
     *        above which the tracker counts an overflow.  0 = no limit
     *        (the backlog is still reported, it is just not flagged).
     */
    explicit PlaybackProgressTracker(std::size_t backlog_limit_frames = 0) noexcept;

    /**
     * The sink accepted @p frames for playback.
     *
     * @param frames The number of frames (samples per channel).
     */
    void on_samples_queued(std::size_t frames) noexcept;

    /**
     * The device consumed @p frames.
     *
     * The counter is clamped to the queued count: a device that claims
     * to have played more than it was ever given is clamped away and
     * counted in `stats().over_reports_clamped`.
     *
     * @param frames The number of frames (samples per channel).
     */
    void on_samples_played(std::size_t frames) noexcept;

    /** The cumulative number of frames the device consumed. */
    [[nodiscard]] std::uint64_t frames_played() const noexcept;

    /** The cumulative number of frames accepted for playback. */
    [[nodiscard]] std::uint64_t frames_queued() const noexcept;

    /**
     * The queued-but-not-yet-played backlog.
     *
     * Never negative, and never larger than the queued count.
     */
    [[nodiscard]] std::size_t frames_pending() const noexcept;

    /**
     * The monotonic instant (microseconds) at which the most recent
     * batch was played.  0 when nothing has been played yet.
     */
    [[nodiscard]] std::int64_t last_playback_us() const noexcept;

    /** True when the backlog is currently above the configured limit. */
    [[nodiscard]] bool backlog_exceeded() const noexcept;

    /** The counters, as a snapshot. */
    [[nodiscard]] PlaybackProgressStats stats() const noexcept;

    /** Reset every counter to zero. */
    void reset() noexcept;

private:
    std::atomic<std::uint64_t> queued_ {0};
    std::atomic<std::uint64_t> played_ {0};
    std::atomic<std::int64_t> last_playback_us_ {0};
    std::atomic<std::uint64_t> backlog_limit_exceeded_ {0};
    std::atomic<std::uint64_t> over_reports_clamped_ {0};
    std::size_t backlog_limit_frames_ = 0;
};

} // namespace nimrtc::audio_device
