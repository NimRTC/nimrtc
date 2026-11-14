/**
 * @file nimrtc/plugins/video_bitrate_controller.hpp
 * @brief IVideoBitrateController — the pluggable video rate controller.
 *
 * ## Why this seam exists
 *
 * The video path has two halves that must never meet:
 *
 *   - the **encoder**, which is *passive*: it codes the frame it is
 *     handed at the bitrate it is handed, and it knows nothing about
 *     the network, the bandwidth estimator or the resolution ladder
 *     (remote-desktop plan B12);
 *   - the **rate controller**, which is *active*: it watches the
 *     bandwidth estimate, the measured outbound bitrate and the
 *     keyframe-request feedback, and decides what the encoder should
 *     be producing.
 *
 * The contract between them is one value: the encoder's parameter set
 * (`plugins::VideoEncoderParams`).  The engine computes that set in one
 * place and pushes it; the controller never touches the encoder.
 *
 * This header is the **interface only** (AGENTS.md L2).  The built-in
 * implementation lives in `src/modules/video_bitrate_control/` and is
 * resolved through the registry's video-bitrate-controller slot.
 *
 * ## The ladder
 *
 * The controller owns a *ladder* — an ordered list of levels, each one
 * a (width, height, fps, bitrate) tuple.  Two policies decide which
 * end of the ladder a given situation wants (remote-desktop plan D2):
 *
 *   - **quality-first**: hold the resolution and the frame rate, spend
 *     the budget on quality (a lower quantiser); drop the frame rate
 *     only when the floor of the ladder is reached.
 *   - **smoothness-first**: hold the frame rate and the motion
 *     cadence, give up resolution first and quality second.
 *
 * The mode is a **configuration** value (plan D4), not a hard-coded
 * constant: the scene profile selects it.
 *
 * ## Implementing a custom controller
 *
 * ```cpp
 * class MyController : public nimrtc::plugins::IVideoBitrateController {
 * public:
 *     const char* name() const noexcept override { return "my-controller"; }
 *     nimrtc::plugins::Status open() noexcept override { return kOk; }
 *     void close() noexcept override {}
 *     void set_config(nimrtc::plugins::VideoBitrateControllerConfig cfg) noexcept override {
 *         cfg_ = cfg;
 *     }
 *     void on_bandwidth_estimate(std::uint32_t bps) noexcept override { ... }
 *     nimrtc::plugins::VideoLadderLevel current_level() const noexcept override { ... }
 *     ...
 * };
 * ```
 *
 * ## Thread safety
 *
 * The engine drives the controller from its **tick thread** only.  The
 * methods are therefore NOT required to be reentrant across threads;
 * they must tolerate being called from a single thread at a time.
 * (The engine reads the controller's *current level* from the same
 * thread, so no locking is needed on the hot path.)
 *
 * @note Remote-desktop plan D1 / D2 / D3 / D4, video plan B1 / B2 / B3 /
 *       B4 / B9.  Interface is stable.
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include <nimrtc/core/api_version.hpp>
#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_VIDEO_BITRATE_CONTROLLER_HPP
#define NIMRTC_PLUGINS_VIDEO_BITRATE_CONTROLLER_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.

#include <cstdint>
#include <string_view>
#include <vector>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Ladder mode
// ---------------------------------------------------------------------------

/**
 * @brief Which end of the ladder the controller gives up first.
 *
 * Remote-desktop plan D2 names the two modes; the video plan's B3
 * makes them a pure function of (bandwidth estimate, current level,
 * scene configuration).
 */
enum class NIMRTC_API_STABLE VideoLadderMode : std::uint8_t {
    /** Hold the resolution and the frame rate; reduce the bitrate /
     *  quality first and drop the frame rate only at the floor.
     *
     *  The default for a camera stream and for a general call. */
    kQuality = 0,

    /** Hold the frame rate and the motion cadence; give up the
     *  resolution first and the bitrate second.
     *
     *  The mode a remote-desktop operator wants: a picture that keeps
     *  moving feels more responsive than a sharp picture that lags. */
    kSmoothness = 1,
};

/** Human-readable ladder-mode name (for logs / stats). */
inline NIMRTC_API_STABLE std::string_view video_ladder_mode_name(
        VideoLadderMode mode) noexcept {
    switch (mode) {
        case VideoLadderMode::kQuality:    return "quality";
        case VideoLadderMode::kSmoothness: return "smoothness";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Ladder level
// ---------------------------------------------------------------------------

/**
 * @brief One rung of the resolution / frame-rate / bitrate ladder.
 *
 * The levels are ordered from the *best* (index 0) to the *worst*
 * (the last index).  A higher index means a smaller picture or a
 * slower one or a thinner one.
 */
struct NIMRTC_API_STABLE VideoLadderLevel {
    /** Picture width in pixels. */
    std::uint32_t width = 0;

    /** Picture height in pixels. */
    std::uint32_t height = 0;

    /** Frame rate in frames per second. */
    std::uint32_t fps = 30;

    /** Target bitrate in bits per second. */
    std::uint32_t bitrate_bps = 0;

    /** True when this level changes the geometry relative to the
     *  previous one — a parameter-set change (plan B13), so the
     *  encoder must restart and emit the new parameter set. */
    bool changes_geometry = false;
};

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/**
 * @brief The controller's configuration.
 *
 * Every field has a default that makes the controller usable with no
 * configuration at all; the scene profile overrides what it needs
 * (remote-desktop plan D4).
 */
struct NIMRTC_API_STABLE VideoBitrateControllerConfig {
    /** The ladder, ordered best-first.  An empty ladder means "the
     *  controller builds a default one from the fields below". */
    std::vector<VideoLadderLevel> ladder;

    /** Which end of the ladder to give up first. */
    VideoLadderMode mode = VideoLadderMode::kQuality;

    /** The capture geometry the ladder starts from (level 0's size). */
    std::uint32_t source_width = 1920;
    std::uint32_t source_height = 1080;

    /** The frame rate the ladder starts from. */
    std::uint32_t source_fps = 30;

    /** The bitrate the ladder starts from (level 0's bitrate). */
    std::uint32_t source_bitrate_bps = 2'000'000;

    // ---- The resolution floor (remote-desktop plan D2) ------------------
    //
    // A screen-content stream has a *legibility* floor: dropping to
    // 480p destroys the product, because the operator is reading text.
    // The floor is expressed as the **worst rung the ladder may use**,
    // so the controller never crosses it — not even under a synthetic
    // bandwidth collapse.
    //
    // The floor is **configurable per machine class**: 1080p on a
    // hardware-encoding machine, 720p on a software-only one.  A floor
    // the machine cannot sustain is a floor the ladder breaks anyway,
    // so it must not be hard-coded.
    //
    // Two ways to state it:
    //
    //   - `floor_level_index` — the worst rung the ladder may use;
    //   - `min_height` — the floor as a picture height ("720p"), which
    //     the controller resolves to the worst rung that is still tall
    //     enough.  `min_height` wins when it is non-zero.

    /** The worst rung the ladder may use.  A value of
     *  `kNoResolutionFloor` disables the floor. */
    std::uint32_t floor_level_index = kNoResolutionFloor;

    /** The floor stated as a picture height, in pixels ("720p" → 720).
     *
     *  When this is non-zero it takes precedence over
     *  `floor_level_index`: the controller resolves it to the cheapest
     *  rung that is still tall enough.  A remote-desktop profile
     *  states the floor this way, because "720p" is what a machine
     *  class means; the rung index is derived from it. */
    std::uint32_t min_height = 0;

    /** The sentinel for "no resolution floor". */
    static constexpr std::uint32_t kNoResolutionFloor = 0xFFFFFFFFu;

    // ---- The safety rails (video plan B9) -------------------------------

    /** The absolute floor of the target bitrate, in bits per second.
     *  The controller never asks for less. */
    std::uint32_t min_bitrate_bps = 150'000;

    /** The absolute ceiling of the target bitrate, in bits per second. */
    std::uint32_t max_bitrate_bps = 8'000'000;

    /** The hysteresis band, as a fraction of the current target
     *  (0.15 = the target must move by 15 % before the controller
     *  acts).  This is what stops the ladder from oscillating around
     *  one rung. */
    double hysteresis = 0.15;

    /** The minimum interval between two level changes, in
     *  milliseconds.  Together with the hysteresis this is the
     *  "cooldown" the plan's B9 asks for. */
    std::uint32_t step_interval_ms = 500;

    /** The maximum number of rungs the controller may cross in one
     *  step.  1 = one rung at a time (the conservative default). */
    std::uint32_t max_steps_per_interval = 1;

    /** The extra bandwidth the controller reserves for the IDR burst a
     *  geometry change costs (remote-desktop plan D3).
     *
     *  A resolution step forces a keyframe, and a keyframe is a
     *  bandwidth spike at the exact moment the link is congested.  The
     *  controller budgets for it: it will not step down while the
     *  estimate is below the current target plus this reserve, unless
     *  the step is urgent (the link has already collapsed). */
    std::uint32_t keyframe_cost_bps = 400'000;

    /** Whether the controller may change the *geometry* at all.
     *
     *  A host that wants a fixed resolution (a camera whose capture
     *  geometry is negotiated once) turns this off; the controller
     *  then only moves the bitrate. */
    bool allow_resolution_change = true;

    /** Whether the controller may change the frame rate. */
    bool allow_fps_change = true;
};

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

/** The controller's own counters, surfaced through the engine stats. */
struct NIMRTC_API_STABLE VideoBitrateControllerStats {
    /** How many times the controller changed the level. */
    std::uint64_t level_changes = 0;

    /** How many of those changes moved *down* the ladder. */
    std::uint64_t level_drops = 0;

    /** How many moved *up*. */
    std::uint64_t level_rises = 0;

    /** How many times a level change was *refused* because the
     *  cooldown had not elapsed. */
    std::uint64_t level_changes_suppressed = 0;

    /** How many times the resolution floor stopped a step down. */
    std::uint64_t floor_blocks = 0;

    /** How many times the keyframe-cost budget stopped a geometry
     *  change. */
    std::uint64_t keyframe_cost_blocks = 0;

    /** The rung of the resolution floor the controller honours
     *  (remote-desktop plan D2).  `0xFFFFFFFF` means "no floor". */
    std::uint32_t resolution_floor_index = 0xFFFFFFFFu;

    /** The picture height of the resolution floor, in pixels.  Zero
     *  when there is no floor. */
    std::uint32_t resolution_floor_height = 0;

    /** The current target bitrate, in bits per second. */
    std::uint32_t target_bitrate_bps = 0;

    /** The current rung of the ladder (0 = the best). */
    std::uint32_t level_index = 0;
};

// ---------------------------------------------------------------------------
// The controller
// ---------------------------------------------------------------------------

/**
 * @brief The pluggable video rate controller.
 *
 * The engine drives the controller from its tick thread:
 *
 * ```text
 *   tick():
 *     bwe_estimate  ──► controller.on_bandwidth_estimate(bps)
 *     measured rate ──► controller.on_measured_bitrate(bps)
 *     keyframe reqs ──► controller.on_keyframe_requested()
 *     controller.current_level() ──► apply_video_encoder_params()
 * ```
 *
 * The controller **never** touches the encoder: it returns a level,
 * and the engine turns that level into the encoder's parameter set.
 */
class NIMRTC_API_STABLE IVideoBitrateController : public IPlugin {
public:
    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override = 0;

    plugins::Status open() noexcept override = 0;
    void close() noexcept override = 0;

    // ---- Configuration ----------------------------------------------------

    /**
     * @brief Apply the configuration.
     *
     * Called before `open()` and again whenever the host changes the
     * scene profile.  A controller that cannot honour the
     * configuration keeps the previous one and returns
     * `kErrUnsupported`.
     */
    virtual Status set_config(VideoBitrateControllerConfig cfg) noexcept = 0;

    /** The configuration currently in force. */
    virtual VideoBitrateControllerConfig config() const noexcept = 0;

    // ---- The feedback the controller consumes ----------------------------

    /**
     * @brief The bandwidth estimate, in bits per second.
     *
     * This is the *session* estimate; the engine has already applied
     * the per-stream share rule (video plan F8 /
     * `bandwidth_allocation.hpp`) before calling this.
     */
    virtual void on_bandwidth_estimate(std::uint32_t bps) noexcept = 0;

    /**
     * @brief The bitrate actually measured on the wire, in bits per
     *  second.
     *
     *  The estimate says what the link *can* carry; the measurement
     *  says what the encoder *is* producing.  The controller uses the
     *  measurement to notice that the encoder is overrunning its
     *  target even though the link looks healthy.
     */
    virtual void on_measured_bitrate(std::uint32_t bps) noexcept = 0;

    /**
     * @brief The peer asked for a keyframe.
     *
     *  A peer that repeatedly asks for keyframes is a peer that cannot
     *  decode what it is getting — usually because the resolution
     *  changed without the parameter sets travelling with the frame.
     *  The controller lowers the level (video plan B7).
     */
    virtual void on_keyframe_requested() noexcept = 0;

    // ---- The output -------------------------------------------------------

    /**
     * @brief The rung of the ladder the controller currently wants.
     *
     *  The engine reads this on every frame and turns it into the
     *  encoder's parameter set (remote-desktop plan B12 / D4).
     */
    virtual VideoLadderLevel current_level() const noexcept = 0;

    /** The controller's counters. */
    virtual VideoBitrateControllerStats stats() const noexcept = 0;

    /**
     * @brief Force a level change (the host's override).
     *
     *  @param level_index  The rung to move to.  Values outside the
     *                      ladder are refused with `kErrInvalidParam`.
     */
    virtual Status set_level(std::uint32_t level_index) noexcept = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/**
 * @brief The factory for the video rate controller.
 *
 * The built-in implementation registers under the id
 * `"video-bitrate-control"`.
 */
class NIMRTC_API_STABLE IVideoBitrateControllerFactory {
public:
    virtual ~IVideoBitrateControllerFactory() = default;

    /** Unique identifier, e.g. "video-bitrate-control". */
    virtual std::string_view id() const noexcept = 0;

    /** Short human-readable name. */
    virtual std::string_view display_name() const noexcept = 0;

    /** Create a new controller instance.  The caller owns it. */
    virtual IVideoBitrateController* create(
        VideoBitrateControllerConfig cfg) const = 0;
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_VIDEO_BITRATE_CONTROLLER_HPP
