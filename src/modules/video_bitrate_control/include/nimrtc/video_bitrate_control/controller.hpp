/**
 * @file nimrtc/video_bitrate_control/controller.hpp
 * @brief The built-in video rate controller.
 *
 * This is the implementation of `plugins::IVideoBitrateController`
 * that ships with NimRTC (remote-desktop plan D1 / D2 / D3 / D4; video
 * plan B2 / B3 / B4 / B9).
 *
 * ## What it decides
 *
 * Given
 *
 *   - the bandwidth estimate (the link's opinion),
 *   - the measured outbound bitrate (the encoder's opinion),
 *   - the keyframe-request feedback (the peer's opinion),
 *
 * it picks a rung of the ladder.  Three things shape the decision:
 *
 *   1. the **mode** (quality-first / smoothness-first) — which end of
 *      the ladder gives way first;
 *   2. the **hysteresis band** — a target that moved by less than the
 *      band does not move the ladder, which is what stops the
 *      staircase from oscillating;
 *   3. the **safety rails** — the resolution floor (plan D2), the
 *      bitrate bounds, the step cooldown and the keyframe-cost budget
 *      (plan D3).
 *
 * ## The keyframe-cost budget (plan D3)
 *
 * A rung that changes the geometry forces an IDR, and an IDR is a
 * bandwidth spike at the exact moment the link is congested.  The
 * controller therefore refuses a *geometry* change while the estimate
 * is below the current target plus the keyframe cost — unless the step
 * is *urgent* (the link has already collapsed below the current
 * target, in which case staying put is worse).
 *
 * ## Thread safety
 *
 * The engine drives the controller from its tick thread only.  The
 * class is therefore not internally synchronised; it relies on the
 * same single-threaded contract the seam documents.
 *
 * @note Remote-desktop plan D1 / D2 / D3 / D4.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#include <nimrtc/plugins/video_bitrate_controller.hpp>

#include <nimrtc/video_bitrate_control/ladder.hpp>

namespace nimrtc::video_bitrate_control {

/**
 * @brief The built-in video rate controller.
 *
 * Register it through the factory below:
 *
 * ```cpp
 * static const nimrtc::plugins::VideoBitrateControllerFactory kFactory;
 * NIMRTC_REGISTER_VIDEO_BITRATE_CONTROLLER(video_bitrate_control, &kFactory);
 * ```
 */
class NIMRTC_API_STABLE BitrateController final
    : public plugins::IVideoBitrateController {
public:
    explicit BitrateController(plugins::VideoBitrateControllerConfig cfg) noexcept;
    ~BitrateController() override;

    BitrateController(const BitrateController&)            = delete;
    BitrateController& operator=(const BitrateController&) = delete;

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override;
    plugins::Status open() noexcept override;
    void close() noexcept override;

    // ---- Configuration ----------------------------------------------------
    plugins::Status set_config(
        plugins::VideoBitrateControllerConfig cfg) noexcept override;
    plugins::VideoBitrateControllerConfig config() const noexcept override;

    // ---- Feedback ---------------------------------------------------------
    void on_bandwidth_estimate(std::uint32_t bps) noexcept override;
    void on_measured_bitrate(std::uint32_t bps) noexcept override;
    void on_keyframe_requested() noexcept override;

    // ---- Output -----------------------------------------------------------
    plugins::VideoLadderLevel current_level() const noexcept override;
    plugins::VideoBitrateControllerStats stats() const noexcept override;
    plugins::Status set_level(std::uint32_t level_index) noexcept override;

    // ---- The clock --------------------------------------------------------
    //
    // The controller's cooldown is measured against a monotonic clock.
    // The engine does not pass the time in (the seam stays free of the
    // engine's clock), so the controller reads its own.  A test needs
    // to control that clock, so the clock is injectable.

    /** The type of the monotonic clock the controller reads. */
    using Clock = std::chrono::steady_clock;

    /** Point on the controller's clock. */
    using TimePoint = Clock::time_point;

    /** Replace the clock (test-only).  The new clock's `now()` is
     *  read from the next decision onwards. */
    void set_clock(std::function<TimePoint()> clock);

    /** The current time on the controller's clock. */
    [[nodiscard]] TimePoint now() const;

private:
    /** Recompute the target level from the current inputs. */
    void update() noexcept;

    /** The rung the controller would pick, without applying it. */
    [[nodiscard]] std::size_t choose_level() const noexcept;

    /** The lowest rung the ladder may cross to (the resolution floor,
     *  plan D2).  `0` when there is no floor. */
    [[nodiscard]] std::size_t floor_index() const noexcept;

    /** The bandwidth budget the ladder must fit into (the link's
     *  estimate). */
    [[nodiscard]] std::uint32_t budget() const noexcept;

    /** True when the measurement says the link is congested even
     *  though the estimate has not caught up with it. */
    [[nodiscard]] bool is_congested() const noexcept;

    /** The bitrate the encoder should actually be told for @p index:
     *  the rung's nominal bitrate, capped by the budget. */
    [[nodiscard]] std::uint32_t effective_target(std::size_t index) const noexcept;

    /** What the encoder is putting on the wire right now. */
    [[nodiscard]] std::uint32_t current_usage() const noexcept;

    /** True when the link is congested enough to justify the IDR a
     *  geometry change costs (plan D3). */
    [[nodiscard]] bool step_is_urgent() const noexcept;

    /** True when the estimate can afford the IDR a geometry change
     *  costs (plan D3). */
    [[nodiscard]] bool step_is_affordable(std::size_t target_index) const noexcept;

    plugins::VideoBitrateControllerConfig cfg_;
    Ladder ladder_;

    std::size_t level_index_ = 0;

    std::uint32_t estimate_bps_ = 0;
    std::uint32_t measured_bps_ = 0;

    std::uint64_t keyframe_requests_ = 0;

    plugins::VideoBitrateControllerStats stats_;

    TimePoint last_step_at_{};
    bool have_step_time_ = false;

    std::function<TimePoint()> clock_;
};

} // namespace nimrtc::video_bitrate_control
