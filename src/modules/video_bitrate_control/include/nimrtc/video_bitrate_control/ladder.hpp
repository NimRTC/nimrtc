/**
 * @file nimrtc/video_bitrate_control/ladder.hpp
 * @brief The resolution / frame-rate / bitrate ladder.
 *
 * The ladder is the controller's *state*: an ordered list of rungs,
 * best first.  The controller decides which rung to move to; this file
 * owns the rungs and the pure geometry of moving between them.
 *
 * It is a separate file from the controller itself because the ladder
 * is **data**, and the controller is **policy**.  Keeping them apart
 * means the ladder can be unit-tested on its own (a golden-trace test
 * over a synthetic bandwidth trace) without instantiating a plugin.
 *
 * ## The two default ladders
 *
 * The *mode* (quality-first / smoothness-first) decides which end of
 * the ladder gives way first, and it is expressed **in the ladder
 * itself** — the two modes build two different rung lists:
 *
 *   | rung | quality-first                    | smoothness-first              |
 *   |------|----------------------------------|--------------------------------|
 *   | 0    | full size, full bitrate           | full size, full bitrate        |
 *   | 1    | full size, 70 % bitrate           | 3/4 size, ~56 % bitrate        |
 *   | 2    | full size, 50 % bitrate           | 1/2 size, ~25 % bitrate        |
 *   | 3    | full size, 35 % bitrate           | 1/4 size, ~6 % bitrate         |
 *   | 4    | 3/4 size                         | 1/4 size, less bitrate         |
 *   | ...  | ... then the frame rate           | ... then the frame rate        |
 *
 * A quality-first ladder keeps the *picture size* for as long as it
 * can and spends the budget on quality; a smoothness-first ladder
 * gives up the picture size first and holds the frame rate, because an
 * operator reads a moving picture as more responsive than a sharp
 * picture that lags.
 *
 * ## Monotonicity
 *
 * The rungs of a default ladder are strictly decreasing in bitrate, so
 * "the best rung that fits the budget" is well defined.  A host-supplied
 * ladder need not be monotone; the controller therefore ranks the
 * feasible rungs explicitly (by area / frame rate / bitrate, depending
 * on the mode) rather than assuming monotonicity.
 *
 * @note Remote-desktop plan D1 / D2 / D3, video plan B2 / B3 / B4 / B9.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <vector>

#include <nimrtc/plugins/video_bitrate_controller.hpp>

namespace nimrtc::video_bitrate_control {

using plugins::VideoLadderLevel;
using plugins::VideoLadderMode;

/**
 * @brief The ladder: an ordered list of rungs, best first.
 *
 * The class owns the rungs and the two "soft" constraints that live on
 * the rungs themselves:
 *
 *   - the **resolution floor** (remote-desktop plan D2) — the lowest
 *     rung the controller may cross to;
 *   - the **bitrate bounds** (video plan B9) — the absolute floor and
 *     ceiling the target may take.
 *
 * It does **not** own the policy: which rung to move to is the
 * controller's decision, not the ladder's.
 */
class NIMRTC_API_STABLE Ladder {
public:
    /** Construct an empty ladder. */
    Ladder() = default;

    /**
     * @brief Build the default ladder for a given capture geometry.
     *
     * The default ladder is the one a 1080p30 capture wants.  The
     * *mode* selects between the two shapes described in the file
     * comment: a quality-first ladder (the picture keeps its size) and
     * a smoothness-first ladder (the picture gives up its size first
     * and the frame rate is held).
     *
     * @param width    The capture width in pixels.
     * @param height   The capture height in pixels.
     * @param fps      The capture frame rate.
     * @param bitrate  The bitrate of the top rung.
     * @param mode     Which end of the ladder gives way first.
     */
    static Ladder make_default(std::uint32_t width,
                                std::uint32_t height,
                                std::uint32_t fps,
                                std::uint32_t bitrate,
                                VideoLadderMode mode = VideoLadderMode::kQuality);

    /** Replace the rungs.  An empty vector clears the ladder. */
    void set_levels(std::vector<VideoLadderLevel> levels);

    /** The rungs, best first. */
    [[nodiscard]] const std::vector<VideoLadderLevel>& levels() const noexcept {
        return levels_;
    }

    /** The number of rungs. */
    [[nodiscard]] std::size_t size() const noexcept { return levels_.size(); }

    /** True when the ladder has no rungs. */
    [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }

    /**
     * @brief The rung at @p index, clamped into range.
     *
     * An out-of-range index clamps rather than throwing: the ladder
     * is consulted on the per-frame path, where an exception would be
     * a hazard.
     */
    [[nodiscard]] VideoLadderLevel at(std::size_t index) const noexcept;

    /**
     * @brief The lowest rung the ladder may use for a resolution floor.
     *
     * Used to resolve the **resolution floor** (remote-desktop plan
     * D2): the floor is stated as a picture height ("720p"), and the
     * ladder is indexed by position, so the floor has to be resolved
     * to a position.
     *
     * The answer is the *last* rung whose height is still at least
     * @p height — that is, the cheapest rung that still satisfies the
     * floor.  The rung below it is the first one that would violate
     * it.
     *
     * @return The index of the lowest rung that is still tall enough,
     *         or `levels_.size() - 1` when every rung is smaller
     *         (the floor is then unreachable and the ladder simply
     *         cannot go lower — the honest answer).
     */
    [[nodiscard]] std::size_t index_for_height(std::uint32_t height) const noexcept;

    /**
     * @brief The bitrate of the rung at @p index, clamped into the
     *  configured bounds.
     */
    [[nodiscard]] std::uint32_t clamped_bitrate(std::size_t index) const noexcept;

    /** Set the absolute bitrate floor and ceiling. */
    void set_bitrate_bounds(std::uint32_t min_bps, std::uint32_t max_bps) noexcept;

    /** The absolute bitrate floor. */
    [[nodiscard]] std::uint32_t min_bitrate_bps() const noexcept { return min_bps_; }

    /** The absolute bitrate ceiling. */
    [[nodiscard]] std::uint32_t max_bitrate_bps() const noexcept { return max_bps_; }

    /**
     * @brief Clamp a target bitrate into the configured bounds.
     *
     * The bounds are the controller's *hard* rails: whatever the
     * policy decides, the target stays inside them.
     */
    [[nodiscard]] std::uint32_t clamp_bitrate(std::uint32_t bps) const noexcept;

    /**
     * @brief The target bitrate the controller asks for at @p index.
     *
     * The rung's own nominal bitrate, clamped into the configured
     * bounds.  The *mode* is not consulted here: the mode is baked
     * into the ladder's shape (see `make_default`), so a rung's
     * nominal bitrate already says what that rung costs.
     */
    [[nodiscard]] std::uint32_t bitrate_for(std::size_t index) const noexcept;

    /**
     * @brief The picture area of the rung at @p index, in pixels.
     *
     * The controller ranks the feasible rungs by area (quality-first)
     * or by frame rate first (smoothness-first); this is the area it
     * ranks on.
     */
    [[nodiscard]] std::uint64_t area_for(std::size_t index) const noexcept;

private:
    std::vector<VideoLadderLevel> levels_;
    std::uint32_t min_bps_ = 150'000;
    std::uint32_t max_bps_ = 8'000'000;
};

} // namespace nimrtc::video_bitrate_control
