/**
 * @file nimrtc/video_sink/video_sync.hpp
 * @brief The video half of the A/V sync decision (remote-desktop plan E5).
 *
 * ## What this is
 *
 * The engine already corrects the **audio** playout against the capture
 * timeline (ADR-017's observe/handle path).  The video half has to make
 * its decision from the *same* skew — plan E5 is explicit about this:
 * "the video half must consume the same skew and must not grow a second
 * skew model (RD-9)".  So this class does not compute a skew; it
 * **consumes** one and turns it into an action.
 *
 * The decision is one of three:
 *
 *   - **present** — the frame is within the tolerance; show it now.
 *   - **delay**   — the frame is *ahead* of the audio (it would show
 *                  something the operator has not heard yet); hold it
 *                  back for a while, if the wait is short enough to be
 *                  worth it.
 *   - **drop**    — the frame is *behind* the audio by more than the
 *                  tolerance (the operator has already heard the sound of
 *                  a moment that is no longer on screen), or the required
 *                  delay is longer than the maximum worth waiting for.
 *
 * ## Why it is a module and not a seam
 *
 * The decision is pure arithmetic on two timestamps — no GPU handle, no
 * window-system type, no OS dependency.  That means it is **unit-tested
 * on every platform**, including the CI legs that have no display at all.
 * The engine wires it to the timeline; the class does not know the
 * engine exists.
 *
 * @note Remote-desktop plan E5 (module half).
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>

#include <cstdint>

namespace nimrtc::video_sink {

// ---------------------------------------------------------------------------
// SyncAction
// ---------------------------------------------------------------------------

/** What the presenter should do with a frame. */
enum class NIMRTC_API_STABLE SyncAction : std::uint8_t {
    /** Show the frame now. */
    kPresent = 0,
    /** Hold the frame back for `delay_us` microseconds. */
    kDelay   = 1,
    /** Do not show the frame at all. */
    kDrop    = 2,
};

/** Human-readable action name (for logs and diagnostics). */
NIMRTC_API_STABLE inline const char* sync_action_name(SyncAction a) noexcept {
    switch (a) {
        case SyncAction::kPresent: return "present";
        case SyncAction::kDelay:   return "delay";
        case SyncAction::kDrop:    return "drop";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// SyncConfig
// ---------------------------------------------------------------------------

/** The A/V sync configuration. */
struct NIMRTC_API_STABLE SyncConfig {
    /**
     * @brief How far the video may lead or lag the audio before a
     *        correction is applied, in microseconds.
     *
     * The default is 40 ms — about one frame at 25 fps, and comfortably
     * inside the ITU-R BT.1359 perceptual window for lip-sync (audio
     * leading video by more than 45 ms, or lagging by more than 125 ms,
     * is noticeable).
     */
    std::int64_t tolerance_us = 40'000;

    /**
     * @brief The longest delay worth applying, in microseconds.
     *
     * A frame that is ahead of the audio by more than this is dropped
     * instead of held: waiting longer than this would itself be visible
     * as a stutter.  The default is 100 ms.
     */
    std::int64_t max_delay_us = 100'000;

    /**
     * @brief Whether the controller is enabled.
     *
     * When false every frame is presented as-is.  The default is true.
     */
    bool enabled = true;
};

// ---------------------------------------------------------------------------
// SyncDecision
// ---------------------------------------------------------------------------

/** The controller's decision for one frame. */
struct NIMRTC_API_STABLE SyncDecision {
    /** What to do with the frame. */
    SyncAction action = SyncAction::kPresent;

    /** How long to hold the frame back, in microseconds.  Zero unless the
     *  action is `kDelay`. */
    std::int64_t delay_us = 0;

    /** The skew the decision was made from, in microseconds.  Positive
     *  means the video leads the audio. */
    std::int64_t skew_us = 0;
};

// ---------------------------------------------------------------------------
// VideoSyncController
// ---------------------------------------------------------------------------

/**
 * @brief Turns an observed A/V skew into a present / delay / drop
 *        decision (plan E5).
 *
 * ## The skew sign
 *
 * The skew is `video_position - audio_position`:
 *
 *   - **positive** — the video is *ahead* of the audio.  Showing it now
 *     would put something on screen the operator has not heard yet, so
 *     the frame is delayed.
 *   - **negative** — the video is *behind* the audio.  The operator has
 *     already heard the sound of a moment that is no longer on screen, so
 *     the frame is dropped when the lag exceeds the tolerance.
 *
 * ## The counters
 *
 * The controller counts the three outcomes.  The count is what proves the
 * correction is happening at all — an A/V sync controller that silently
 * does nothing looks exactly like one that is working.
 */
class NIMRTC_API_STABLE VideoSyncController {
public:
    /** Construct a controller. */
    explicit VideoSyncController(SyncConfig config = SyncConfig{});

    /**
     * @brief Decide what to do with a frame.
     *
     * @param skew_us the observed skew (video position minus audio
     *                position), in microseconds.
     * @return the decision.
     */
    SyncDecision decide(std::int64_t skew_us) noexcept;

    /** The counters. */
    struct NIMRTC_API_STABLE Stats {
        /** Frames presented without a correction. */
        std::uint64_t presented = 0;
        /** Frames held back. */
        std::uint64_t delayed = 0;
        /** Frames dropped. */
        std::uint64_t dropped = 0;
        /** The total number of frames the controller was asked about. */
        std::uint64_t total = 0;
        /** The most recent skew the controller saw, in microseconds. */
        std::int64_t last_skew_us = 0;
    };

    /** A snapshot of the counters. */
    Stats stats() const noexcept;

    /** The configuration this controller was built with. */
    const SyncConfig& config() const noexcept;

    /** Reset the counters. */
    void reset() noexcept;

private:
    SyncConfig config_;
    Stats stats_;
};

} // namespace nimrtc::video_sink
