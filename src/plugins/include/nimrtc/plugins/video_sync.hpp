/**
 * @file nimrtc/plugins/video_sync.hpp
 * @brief IVideoSyncController — the render-clock / A-V sync decision seam
 *        (v1.3 plan **Q6**, remote-desktop plan **R3**).
 *
 * ## What this seam is for
 *
 * The **audio** half of the A/V-sync contract shipped in `v1.2.0`
 * (audio plan WP-14, spec C-4): the engine aligns the *audio playout*
 * against the capture timeline that ADR-017 reconstructs.  The **video**
 * half is what Q6 lands: the picture is either presented at the clock's
 * instant, held back for a bounded while, or dropped.
 *
 * The correction policy is a **module** (`src/modules/video_sync/`), and
 * the engine may not include a concrete module's header (Layout
 * Invariant 4 / ADR-025).  So the engine reaches the policy through
 * this seam, exactly as it reaches the QoE collector and the bitrate
 * controller.
 *
 * ## The skew — one model, not two
 *
 * The plan's hardest constraint is *"it must not grow a second skew
 * model"*.  Both halves read the **ADR-017** mapping: the peer's
 * Sender Report reconstructs a frame's capture instant, and the
 * inter-clock skew between the peer's clock and ours is the same
 * `sender_recv_skew_us` the timeline already publishes.  This seam
 * **consumes** a skew; it never derives one.
 *
 * The skew this seam consumes is:
 *
 * ```
 *   skew_us = video_capture_us - audio_playout_capture_us + interclock_skew_us
 * ```
 *
 * and the sign convention is fixed:
 *
 *   - **positive** — the picture is *ahead* of the sound.  The frame was
 *     captured *later* than the audio that is at the speaker now, so
 *     showing it now would put an image on the screen before the
 *     operator has heard the moment it belongs to.  The frame is
 *     **held** for the difference (bounded — see
 *     `VideoSyncConfig::max_delay_us`) so the sound can catch up;
 *   - **negative** — the picture is *behind* of the sound.  The operator
 *     has already heard a moment that is no longer on screen, so holding
 *     the frame would only make it later.  The frame is **dropped**.
 *
 * The two are mirror images, and the mirror is the point: a frame that
 * arrived *early* is recoverable by waiting, a frame that arrived *late*
 * is not recoverable at all.
 *
 * @note The convention matches the one the `video_sink` module's own
 *       `VideoSyncController` has used since `v1.2.0` — the two are the
 *       same model seen from two sides, and Q6's "no second skew model"
 *       rule means they must not drift apart.
 *
 * ## The decision is a *value*, not a callback
 *
 * The decision is a pure function of the skew and the controller's own
 * counters: no clock, no allocation, no thread.  That is what makes it
 * directly unit-testable on every platform — including the CI legs that
 * have no display at all — and what keeps the policy honest.
 *
 * ## Threading
 *
 * Not thread-safe, like every other pipeline object in this codebase.
 * The engine's receive path is the only driver.
 *
 * @see docs/plan/v1.3-plan.md §3 Q6.
 * @see docs/adr/ADR-017-cross-end-timeline.md — the skew's origin.
 */

#ifndef NIMRTC_PLUGINS_VIDEO_SYNC_HPP
#define NIMRTC_PLUGINS_VIDEO_SYNC_HPP

// Stability classification: NIMRTC_API_STABLE (file-level marker from the
// v0.13.0 sweep; see ADR-015 / docs/plan/v0.13-plan.md).
//
// `nimrtc_plugins` links `nimrtc::core` PUBLIC (see src/plugins/CMakeLists.txt),
// so <nimrtc/core/api_version.hpp> is reachable from this header.
#include <nimrtc/core/api_version.hpp>   // NIMRTC_API_STABLE

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "nimrtc/plugins/base.hpp"

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// The action
// ---------------------------------------------------------------------------

/** What the renderer should do with a frame. */
enum class NIMRTC_API_STABLE VideoSyncAction : std::uint8_t {
    /** The skew is inside the tolerance band: present the frame now. */
    kPresent = 0,
    /** The picture is behind the sound: drop the frame. */
    kDrop    = 1,
    /** The picture is ahead of the sound: hold it for `delay_us`. */
    kDelay   = 2,
};

/** Human-readable name of an action (for logs and diagnostics). */
NIMRTC_API_STABLE inline const char*
video_sync_action_name(VideoSyncAction action) noexcept {
    switch (action) {
        case VideoSyncAction::kPresent: return "present";
        case VideoSyncAction::kDrop:    return "drop";
        case VideoSyncAction::kDelay:   return "delay";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// The configuration
// ---------------------------------------------------------------------------

/**
 * @brief The render clock's configuration.
 *
 * The defaults are the ITU-R BT.1359 perceptual window: a lip-sync
 * error below ~40 ms is imperceptible for speech.
 */
struct NIMRTC_API_STABLE VideoSyncConfig {
    /**
     * @brief The tolerance band, in microseconds.
     *
     * A skew whose magnitude is at or below this band means the picture
     * and the sound already agree, so the frame is presented unchanged.
     * The default is 40 ms — about one frame at 25 fps.
     */
    std::int64_t tolerance_us = 40'000;

    /**
     * @brief The cap on a single hold, in microseconds.
     *
     * A frame held longer than this is not "aligned" any more — it is
     * simply late, and a late frame is better served than a frame that
     * never arrives.  The default is 200 ms.
     */
    std::int64_t max_delay_us = 200'000;

    /**
     * @brief Whether the controller corrects at all.
     *
     * When false every frame is presented as-is.  The default is true,
     * matching the audio half's switchable correction (WP-14 / spec
     * C-4): a caller that turned the audio correction off should not
     * get a video correction it cannot also turn off.
     */
    bool enabled = true;
};

// ---------------------------------------------------------------------------
// The decision
// ---------------------------------------------------------------------------

/** The controller's decision for one frame. */
struct NIMRTC_API_STABLE VideoSyncDecision {
    /** What to do with the frame. */
    VideoSyncAction action = VideoSyncAction::kPresent;

    /** How long to hold the frame back, in microseconds.  Zero unless the
     *  action is `kDelay`. */
    std::int64_t delay_us = 0;

    /** The skew the decision was made from, in microseconds.  Positive
     *  means the picture is *ahead* of the sound; negative means it is
     *  *behind*. */
    std::int64_t skew_us = 0;
    /** The frame's capture timestamp the decision was made for.  Carried
     *  through for the caller's benefit — the decision itself depends
     *  only on the skew. */
    std::int64_t capture_ts_us = 0;
};

// ---------------------------------------------------------------------------
// The counters
// ---------------------------------------------------------------------------

/**
 * @brief The render clock's counters.
 *
 * The count is what proves the correction is happening at all — a
 * sync controller that silently does nothing looks exactly like one that
 * is working.
 */
struct NIMRTC_API_STABLE VideoSyncCounters {
    /** Frames presented unchanged (the skew was inside the band). */
    std::uint64_t frames_presented = 0;

    /** Frames dropped (the picture was behind the sound). */
    std::uint64_t frames_dropped = 0;

    /** Frames held back (the picture was ahead of the sound). */
    std::uint64_t frames_delayed = 0;

    /** The summed hold time, in microseconds. */
    std::int64_t total_delay_us = 0;

    /** The most recent skew the controller saw, in microseconds. */
    std::int64_t last_skew_us = 0;
};

// ---------------------------------------------------------------------------
// IVideoSyncController
// ---------------------------------------------------------------------------

/**
 * @brief Turns an observed A/V skew into a present / drop / delay
 *        decision (v1.3 Q6, remote-desktop plan R3).
 */
class NIMRTC_API_STABLE IVideoSyncController : public IPlugin {
public:
    ~IVideoSyncController() override = default;

    // ---- IPlugin overrides -------------------------------------------------

    NIMRTC_API_STABLE const char* name() const noexcept override = 0;
    NIMRTC_API_STABLE Status      open() noexcept override = 0;
    NIMRTC_API_STABLE void        close() noexcept override = 0;

    // ---- The decision ------------------------------------------------------

    /**
     * @brief Decide what to do with one frame.
     *
     * @param skew_us  The skew between the picture and the sound, in
     *                 microseconds, on the ADR-017 timeline:
     *                 `video_capture_us - audio_playout_capture_us +
     *                 interclock_skew_us`.  **Positive** means the picture
     *                 is *ahead* of the sound (hold it); **negative** means
     *                 it is *behind* (drop it).  The controller does not
     *                 derive the skew — it consumes the one the timeline
     *                 already produced.
     * @param capture_ts_us  The frame's capture timestamp, carried
     *                       through for the caller's benefit.
     * @return the decision.
     */
    NIMRTC_API_STABLE virtual VideoSyncDecision decide(
        std::int64_t skew_us,
        std::int64_t capture_ts_us) noexcept = 0;

    /**
     * @brief Account for a hold that actually happened.
     *
     * The caller waits `delay_us` before rendering the frame and then
     * reports back how long it really waited.  The difference between
     * the promised and the actual hold is the debt that was *not*
     * consumed, and it stays on the books (bounded by
     * `VideoSyncConfig::max_delay_us`).
     *
     * Calling this with `actual_delay_us == 0` is equivalent to
     * cancelling the debt — which is what a caller that decided not to
     * wait at all must do.
     */
    NIMRTC_API_STABLE virtual void note_delay_applied(
        std::int64_t actual_delay_us) noexcept = 0;

    // ---- Configuration -----------------------------------------------------

    /** Replace the configuration.  The counters are *not* reset. */
    NIMRTC_API_STABLE virtual void configure(
        const VideoSyncConfig& config) noexcept = 0;

    /** The tolerance band in microseconds. */
    NIMRTC_API_STABLE virtual std::int64_t tolerance_us() const noexcept = 0;

    /** The cap on a single hold, in microseconds. */
    NIMRTC_API_STABLE virtual std::int64_t max_delay_us() const noexcept = 0;

    /** Whether the controller corrects at all. */
    NIMRTC_API_STABLE virtual bool enabled() const noexcept = 0;

    /** Turn the correction on or off without touching the counters. */
    NIMRTC_API_STABLE virtual void set_enabled(bool enabled) noexcept = 0;

    // ---- Reporting ---------------------------------------------------------

    /** The counters so far. */
    NIMRTC_API_STABLE virtual VideoSyncCounters counters() const noexcept = 0;

    /** Reset the counters and the pending debt. */
    NIMRTC_API_STABLE virtual void reset_counters() noexcept = 0;
};

// ---------------------------------------------------------------------------
// The factory
// ---------------------------------------------------------------------------

/**
 * @brief The factory that produces render clocks.
 *
 * The engine resolves a factory through the registry's video-sync slot
 * and creates one controller from it.  The concrete implementation lives
 * in the `video_sync` module.
 */
class NIMRTC_API_STABLE IVideoSyncControllerFactory : public IPluginFactory {
public:
    ~IVideoSyncControllerFactory() override = default;

    NIMRTC_API_STABLE std::string_view id()          const noexcept override = 0;
    NIMRTC_API_STABLE std::string_view display_name() const noexcept override = 0;

    /** Create one controller.  Ownership passes to the caller. */
    NIMRTC_API_STABLE virtual std::unique_ptr<IVideoSyncController> create(
        const VideoSyncConfig& config) const = 0;
};

} // namespace nimrtc::plugins

#endif // NIMRTC_PLUGINS_VIDEO_SYNC_HPP
