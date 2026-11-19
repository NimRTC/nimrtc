/**
 * @file nimrtc/video_sync/render_clock.hpp
 * @brief The render clock — the video half of the A/V-sync correction
 *        (v1.3 plan **Q6**, remote-desktop plan **R3**).
 *
 * ## What this is
 *
 * The **audio** half of the A/V-sync contract shipped in `v1.2.0`
 * (audio plan WP-14, spec C-4).  This is the **video** half: the
 * measured skew becomes an explicit **present / drop / delay**
 * decision, so the viewer hears and sees the same instant.
 *
 * ## The skew — one model, not two
 *
 * The plan's hardest constraint is *"it must not grow a second skew
 * model"*.  So this class does **not** compute a skew; it **consumes**
 * one — the one the ADR-017 timeline already produces:
 *
 * ```
 *   skew_us = video_capture_us - audio_playout_capture_us + interclock_skew_us
 * ```
 *
 * The sign convention is fixed and documented:
 *
 *   | skew (video − audio)       | decision | why                          |
 *   |----------------------------|----------|------------------------------|
 *   | within ±`tolerance_us`    | kPresent | the two already agree        |
 *   | positive (video *ahead*)   | kDelay   | the picture must wait        |
 *   | negative (video *behind*)  | kDrop    | the moment is already gone   |
 *
 * "Ahead" means the picture is *later* than the sound it belongs to:
 * the viewer would see the image before the sound arrives.  "Behind" is
 * the opposite.
 *
 * @note The convention — **and the whole rule** — is the one the
 *       `video_sink` module's own `VideoSyncController` has used since
 *       `v1.2.0`.  The two are the same model seen from two sides, and
 *       Q6's "no second skew model" rule means they must not drift
 *       apart.  `tests/test_render_clock.cpp` compares the two directly.
 *
 * ## Why a hold that is too long becomes a drop
 *
 * The hold is the skew minus the tolerance band.  A hold that would be
 * longer than `max_delay_us` is **not** worth waiting for: the picture
 * would still be misaligned when it finally appeared, and the wait
 * itself is visible as a freeze.  Such a frame is dropped instead, and
 * the *next* frame — which is closer to the sound — takes its place.
 *
 * ## The promised hold is settled against reality
 *
 * `decide()` promises a hold; the presenter waits for it and then
 * reports back through `note_delay_applied()` how long it really
 * waited.  The reported `total_delay_us` is therefore the time the
 * presenter actually spent holding frames, not the time the clock
 * asked for.
 *
 * ## Determinism
 *
 * The clock is a pure function of its inputs plus its own counters: no
 * clock, no allocation, no thread.  `decide()` takes the skew and the
 * frame's capture timestamp and returns the decision.  That makes it
 * directly unit-testable on every platform — including the CI legs that
 * have no display at all.
 *
 * @see docs/plan/v1.3-plan.md §3 Q6.
 * @see docs/adr/ADR-017-cross-end-timeline.md — the skew's origin.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstddef>
#include <cstdint>

#include <nimrtc/plugins/video_sync.hpp>

namespace nimrtc::video_sync {

/**
 * @brief The render clock: turns a skew into a present / drop / delay
 *        decision.
 *
 * The clock is deliberately **stateless with respect to the clock**:
 * it never reads one.  The caller supplies the skew and the frame's
 * capture timestamp, and the clock returns the decision — which is what
 * makes it testable, and what keeps it free of a second skew model.
 */
class NIMRTC_API_STABLE RenderClock {
public:
    /** Construct a clock with the default configuration. */
    RenderClock() = default;

    /** Construct a clock with a specific configuration. */
    explicit RenderClock(const plugins::VideoSyncConfig& config) noexcept;

    // ---- Configuration -----------------------------------------------------

    /// Replace the configuration.  The counters are *not* reset.
    void configure(const plugins::VideoSyncConfig& config) noexcept;

    /// The configuration this clock was built with.
    [[nodiscard]] const plugins::VideoSyncConfig& config() const noexcept {
        return config_;
    }

    /// The tolerance band in microseconds.
    [[nodiscard]] std::int64_t tolerance_us() const noexcept;

    /// The cap on a single hold, in microseconds.
    [[nodiscard]] std::int64_t max_delay_us() const noexcept;

    /// Whether the clock corrects at all.
    [[nodiscard]] bool enabled() const noexcept { return config_.enabled; }

    /// Turn the correction on or off without touching the counters.
    void set_enabled(bool enabled) noexcept { config_.enabled = enabled; }

    // ---- The decision ------------------------------------------------------

    /**
     * @brief Decide what to do with one frame.
     *
     * @param skew_us  The skew between the picture and the sound, in
     *                 microseconds, on the ADR-017 timeline.  **Positive**
     *                 means the picture is *ahead* (hold); **negative**
     *                 means it is *behind* (drop).
     * @param capture_ts_us  The frame's capture timestamp, carried through
     *                       for the caller's benefit.
     * @return the decision.
     */
    [[nodiscard]] plugins::VideoSyncDecision decide(
        std::int64_t skew_us,
        std::int64_t capture_ts_us) noexcept;

    /**
     * @brief Account for a hold that actually happened.
     *
     * The caller waits `delay_us` before rendering the frame and then
     * reports back how long it really waited; the reported total is
     * settled against that reality.
     *
     * Calling this with `actual_delay_us == 0` is equivalent to
     * saying "the hold did not happen" — which is what a caller that
     * decided not to wait at all must do.
     *
     * @see plugins::IVideoSyncController::note_delay_applied()
     */
    void note_delay_applied(std::int64_t actual_delay_us) noexcept;

    // ---- Reporting ---------------------------------------------------------

    /// The counters so far.
    [[nodiscard]] plugins::VideoSyncCounters counters() const noexcept {
        return counters_;
    }

    /// Reset the counters and the pending promise.
    void reset_counters() noexcept;

    /**
     * @brief The decision the clock made at history slot `index`.
     *
     * 0 is the most recent.  The ring is fixed-size, so the clock never
     * allocates and the history cannot grow without bound.
     */
    [[nodiscard]] plugins::VideoSyncAction history_decision(
        std::size_t index) const noexcept;

    /**
     * @brief The skew the clock saw at history slot `index` (0 = most
     *        recent), in microseconds.
     */
    [[nodiscard]] std::int64_t history_skew_us(std::size_t index) const noexcept;

    /// The number of decisions the history ring holds.
    static constexpr std::size_t history_size() noexcept {
        return kHistorySize;
    }

private:
    /// One history entry.
    struct HistoryEntry {
        plugins::VideoSyncAction action = plugins::VideoSyncAction::kPresent;
        std::int64_t skew_us = 0;
        std::int64_t capture_ts_us = 0;
    };

    /// Clamp the configuration into its sane range.
    void normalize() noexcept;

    /// Take an outstanding hold off the books.
    ///
    /// A hold that was promised for a frame that is not going to be
    /// presented will never happen, so the reported total must not carry
    /// it.  This is what a **drop** does to the outstanding promise.
    void cancel_promised_hold() noexcept;

    /// Append a decision to the fixed-size history ring.
    void push_history(const plugins::VideoSyncDecision& out) noexcept;

    /// The size of the decision history ring.
    static constexpr std::size_t kHistorySize = 16;

    plugins::VideoSyncConfig  config_{};
    plugins::VideoSyncCounters counters_{};

    /// The hold the clock has promised but that has not been settled yet.
    ///
    /// `decide()` sets it when it asks for a hold; `note_delay_applied()`
    /// settles it against what the presenter really waited.  It is zero
    /// whenever there is nothing outstanding, so the reported total is
    /// always the realised one.
    std::int64_t promised_delay_us_ = 0;

    /// The fixed-size decision history (a ring, never reallocated).
    HistoryEntry history_[kHistorySize]{};
    std::size_t  head_ = 0;
};

} // namespace nimrtc::video_sync
