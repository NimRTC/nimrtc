/**
 * @file src/engine/src/video_sync_controller.hpp
 * @brief PRIVATE engine collaborator: the render clock / A-V sync
 *        decision (remote-desktop plan E5).
 *
 * ## What this is
 *
 * Plan E5 asks for the *video half* of the A/V-sync contract: the
 * observed skew — the difference between the audio playout clock and
 * the video clock — has to become an explicit **align / drop / delay**
 * decision, so the viewer hears and sees the same instant.
 *
 * The engine already corrects the **audio** playout against the same
 * capture timeline (the audio receive pipeline's own clock).  This
 * controller does the video half and **does not grow a second skew
 * model** (RD-9): the skew it consumes is the one the ADR-017 timeline
 * already produces (`nimrtc::rtp::SenderReportTracker`'s
 * `sender_recv_skew_us`, published by the engine as
 * `last_sender_recv_skew_us()`), and the frame's capture timestamp is
 * the one the same timeline reconstructed.
 *
 * ## The rule
 *
 * A tolerance band around zero is the whole policy:
 *
 *   | skew (video − audio)          | decision  | why                       |
 *   |--------------------------------|-----------|--------------------------|
 *   | within ±`tolerance_ms`        | kPresent  | the two already agree    |
 *   | video *ahead* of the audio    | kDrop     | the picture would run early |
 *   | video *behind* the audio      | kDelay    | the picture must wait     |
 *
 * "Ahead" means the video frame is *later* than the audio playout
 * instant it is supposed to line up with: the viewer would see the
 * picture before the sound arrives, so the frame is dropped rather than
 * shown out of order.  "Behind" is the opposite: the picture would lag
 * the sound, so it is held for the duration of the lag (bounded — see
 * below).
 *
 * ## Why the delay is bounded
 *
 * An unbounded delay is a latency bomb: a single clock glitch would
 * push the picture half a second into the future and every later frame
 * would inherit the debt.  The controller therefore:
 *
 *   - caps a single delay at `max_delay_ms` (`kDefaultMaxDelayCapMs` by
 *     default), and
 *   - caps the *accumulated* delay: a frame that is already late
 *     because of an earlier delay does not get the full skew added on
 *     top of it.
 *
 * The accumulated debt is tracked explicitly (`pending_delay_us_`) and
 * is *reduced* by the amount actually waited, so it cannot grow without
 * bound.
 *
 * ## Determinism
 *
 * The controller is a pure function of its inputs plus its own counters:
 * no clock, no allocation, no thread.  `decide()` takes the skew and
 * the frame's capture timestamp and returns the decision.  That makes
 * it directly unit-testable (see `tests/test_engine_video_sync.cpp`).
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * @see docs/plan/remote-desktop-plan.md §Track E (E5 — render clock).
 * @see docs/adr/ADR-017-cross-end-timeline.md — the skew's origin.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nimrtc::engine::detail {

/** The decision the render clock makes about one decoded video frame. */
enum class VideoSyncDecision : std::uint8_t {
    /** The skew is inside the tolerance band: render the frame now. */
    kPresent = 0,
    /** The video is ahead of the audio: drop the frame (it would show
     *  the picture before the sound it belongs to). */
    kDrop    = 1,
    /** The video is behind the audio: hold the frame for `delay_us`
     *  before rendering it. */
    kDelay   = 2,
};

/** Human-readable name of a sync decision (for logs / diagnostics). */
[[nodiscard]] inline constexpr std::string_view
video_sync_decision_name(VideoSyncDecision d) noexcept {
    switch (d) {
        case VideoSyncDecision::kPresent: return "present";
        case VideoSyncDecision::kDrop:    return "drop";
        case VideoSyncDecision::kDelay:   return "delay";
    }
    return "unknown";
}

/** The outcome of one `decide()` call. */
struct VideoSyncOutcome {
    /** What the engine should do with the frame. */
    VideoSyncDecision decision = VideoSyncDecision::kPresent;

    /** How long to hold the frame before rendering it (microseconds).
     *  Zero for `kPresent` and `kDrop`.  Bounded by the controller's
     *  `max_delay_us`. */
    std::int64_t delay_us = 0;

    /** The skew the decision was based on (microseconds).  Positive
     *  means the video is *behind* the audio (a delay), negative means
     *  the video is *ahead* (a drop). */
    std::int64_t skew_us = 0;

    /** The frame's capture timestamp the decision was made for. */
    std::int64_t capture_ts_us = 0;
};

/** The controller's configuration. */
struct VideoSyncConfig {
    /** The tolerance band, in milliseconds.
     *
     *  A skew whose magnitude is at or below this band is *inside* the
     *  band: the frame is presented unchanged.  The default (40 ms) is
     *  roughly the threshold below which a lip-sync error is
     *  imperceptible for speech. */
    std::int32_t tolerance_ms = 40;

    /** The maximum delay a single frame may be held for, in
     *  milliseconds.  0 = use `kDefaultMaxDelayCapMs`. */
    std::int32_t max_delay_ms = 0;

    /** The default cap on a single delay: 200 ms.
     *
     *  A frame held longer than this is not "aligned" any more — it is
     *  simply late, and the caller is better served by a late frame
     *  than by a frame that never arrives. */
    static constexpr std::int32_t kDefaultMaxDelayCapMs = 200;

    /** The maximum number of decisions the controller keeps for
     *  reporting.  The ring is fixed-size, so the controller never
     *  allocates and the history cannot grow without bound. */
    static constexpr std::size_t kHistorySize = 16;
};

/** The counters the controller keeps. */
struct VideoSyncCounters {
    /** Frames the controller said to present. */
    std::uint64_t frames_presented = 0;
    /** Frames the controller said to drop (the video was ahead). */
    std::uint64_t frames_dropped = 0;
    /** Frames the controller said to delay (the video was behind). */
    std::uint64_t frames_delayed = 0;
    /** The summed hold time, in microseconds. */
    std::int64_t total_delay_us = 0;
    /** The most recent skew the controller saw, in microseconds. */
    std::int64_t last_skew_us = 0;
};

/**
 * @brief The render clock: turns a skew into an align / drop / delay
 *        decision.
 *
 * The controller is deliberately **stateless with respect to the
 * clock**: it never reads one.  The caller supplies the skew and the
 * frame's capture timestamp, and the controller returns the decision —
 * which is what makes it testable and what keeps it free of a second
 * skew model (RD-9).
 */
class VideoSyncController {
public:
    VideoSyncController() = default;

    explicit VideoSyncController(const VideoSyncConfig& cfg) noexcept;

    // ---- Configuration ---------------------------------------------------

    /// Replace the configuration.  The counters are *not* reset.
    void configure(const VideoSyncConfig& cfg) noexcept;

    [[nodiscard]] const VideoSyncConfig& config() const noexcept { return cfg_; }

    /// The tolerance band in microseconds.
    [[nodiscard]] std::int64_t tolerance_us() const noexcept;

    /// The cap on a single delay, in microseconds.
    [[nodiscard]] std::int64_t max_delay_us() const noexcept;

    // ---- The decision ----------------------------------------------------

    /**
     * @brief Decide what to do with one decoded frame.
     *
     * @param skew_us  The skew between the audio playout clock and the
     *                 video clock, in microseconds.  **Positive** means
     *                 the video is *behind* the audio (delay);
     *                 **negative** means the video is *ahead* (drop).
     *                 This is the skew the ADR-017 timeline already
     *                 computes — the controller does not derive it.
     * @param capture_ts_us  The frame's capture timestamp.  It is
     *                       carried through for the caller's benefit
     *                       (and for the history entry); the decision
     *                       itself depends only on the skew.
     *
     * @return The decision, the hold time (for `kDelay`) and the skew
     *         the decision was based on.
     */
    [[nodiscard]] VideoSyncOutcome decide(std::int64_t skew_us,
                                          std::int64_t capture_ts_us) noexcept;

    /**
     * @brief Account for a hold that actually happened.
     *
     * The caller waits `delay_us` before rendering the frame and then
     * reports back how long it really waited.  The difference between
     * the promised and the actual hold is the debt that was *not*
     * consumed, and it stays on the books (bounded by the cap).
     *
     * Calling this with `actual_delay_us == 0` is equivalent to
     * cancelling the debt — which is what a caller that decided not to
     * wait at all must do.
     */
    void note_delay_applied(std::int64_t actual_delay_us) noexcept;

    // ---- Reporting --------------------------------------------------------

    [[nodiscard]] const VideoSyncCounters& counters() const noexcept {
        return counters_;
    }

    /// Reset the counters and the pending debt.  The configuration and
    /// the history survive.
    void reset_counters() noexcept;

    /// The number of decisions the history ring holds.
    static constexpr std::size_t history_size() noexcept {
        return VideoSyncConfig::kHistorySize;
    }

    /// The decision at history slot `index` (0 = the most recent).
    [[nodiscard]] VideoSyncDecision history_decision(std::size_t index) const noexcept;

    /// The skew at history slot `index` (0 = the most recent).
    [[nodiscard]] std::int64_t history_skew_us(std::size_t index) const noexcept;

private:
    /// One history entry.
    struct HistoryEntry {
        VideoSyncDecision decision = VideoSyncDecision::kPresent;
        std::int64_t skew_us = 0;
        std::int64_t capture_ts_us = 0;
    };

    /// Clamp the configuration into its sane range.
    void normalize() noexcept;

    /// Append a decision to the fixed-size history ring.
    void push_history(const VideoSyncOutcome& out) noexcept;

    VideoSyncConfig  cfg_{};
    VideoSyncCounters counters_{};

    /// The hold the controller still owes the frames it is presenting.
    ///
    ///  Bounded by `max_delay_us()`: `decide()` never grows it past the
    ///  cap, and `note_delay_applied()` only shrinks it.
    std::int64_t pending_delay_us_ = 0;

    /// The fixed-size decision history (a ring, never reallocated).
    HistoryEntry history_[VideoSyncConfig::kHistorySize]{};
    std::size_t  head_ = 0;
};

}  // namespace nimrtc::engine::detail
