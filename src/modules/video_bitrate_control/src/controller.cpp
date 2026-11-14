/**
 * @file src/controller.cpp
 * @brief The built-in video rate controller.
 *
 * Remote-desktop plan D1 / D2 / D3 / D4; video plan B2 / B3 / B4 / B9.
 */

#include <nimrtc/video_bitrate_control/controller.hpp>

#include <algorithm>
#include <cmath>

#include <nimrtc/core/log.hpp>

namespace nimrtc::video_bitrate_control {

using plugins::VideoBitrateControllerConfig;
using plugins::VideoBitrateControllerStats;
using plugins::VideoLadderLevel;
using plugins::VideoLadderMode;

namespace {

/** The ladder the controller falls back on when the host gives none. */
VideoBitrateControllerConfig with_default_ladder(
        VideoBitrateControllerConfig cfg) {
    if (cfg.ladder.empty()) {
        const Ladder def = Ladder::make_default(cfg.source_width,
                                                 cfg.source_height,
                                                 cfg.source_fps,
                                                 cfg.source_bitrate_bps,
                                                 cfg.mode);
        cfg.ladder = def.levels();
    }
    return cfg;
}

} // namespace

BitrateController::BitrateController(VideoBitrateControllerConfig cfg) noexcept
    : cfg_(with_default_ladder(std::move(cfg))) {
    ladder_.set_levels(cfg_.ladder);
    ladder_.set_bitrate_bounds(cfg_.min_bitrate_bps, cfg_.max_bitrate_bps);
    clock_ = [] { return Clock::now(); };
}

BitrateController::~BitrateController() = default;

const char* BitrateController::name() const noexcept {
    return "nimrtc-video-bitrate-control";
}

plugins::Status BitrateController::open() noexcept {
    last_step_at_ = now();
    have_step_time_ = true;
    return plugins::kOk;
}

void BitrateController::close() noexcept {}

plugins::Status BitrateController::set_config(
    VideoBitrateControllerConfig cfg) noexcept {
    if (cfg.min_bitrate_bps > 0 && cfg.max_bitrate_bps > 0 &&
        cfg.min_bitrate_bps > cfg.max_bitrate_bps) {
        return plugins::kErrInvalidParam;
    }
    cfg_ = with_default_ladder(std::move(cfg));
    ladder_.set_levels(cfg_.ladder);
    ladder_.set_bitrate_bounds(cfg_.min_bitrate_bps, cfg_.max_bitrate_bps);

    // The level may now be out of range (a shorter ladder).
    if (!ladder_.empty() && level_index_ >= ladder_.size()) {
        level_index_ = ladder_.size() - 1;
    }
    update();
    return plugins::kOk;
}

VideoBitrateControllerConfig BitrateController::config() const noexcept {
    return cfg_;
}

void BitrateController::set_clock(std::function<TimePoint()> clock) {
    if (clock) clock_ = std::move(clock);
}

BitrateController::TimePoint BitrateController::now() const {
    return clock_ ? clock_() : Clock::now();
}

void BitrateController::on_bandwidth_estimate(std::uint32_t bps) noexcept {
    estimate_bps_ = bps;
    update();
}

void BitrateController::on_measured_bitrate(std::uint32_t bps) noexcept {
    measured_bps_ = bps;
    update();
}

void BitrateController::on_keyframe_requested() noexcept {
    ++keyframe_requests_;
    // The peer cannot decode what it is getting.  The usual cause is a
    // geometry change whose parameter sets did not travel with the
    // frame; the remedy is to give the peer a smaller picture.
    //
    // The step is *urgent* (the peer is asking), so the keyframe-cost
    // budget does not apply to it: the cost of not answering is higher
    // than the cost of the IDR.
    if (ladder_.empty()) return;

    std::size_t want = level_index_;
    if (want + 1 < ladder_.size()) ++want;

    // The floor still holds — the peer asking does not change what the
    // product can survive (plan D2).
    const std::size_t floor = floor_index();
    if (want > floor) want = floor;
    if (want == level_index_) return;

    level_index_ = want;
    ++stats_.level_changes;
    ++stats_.level_drops;
    last_step_at_ = now();
    have_step_time_ = true;

    core::log::Logger::instance().warn(
        "engine[bitrate]: the peer asked for a keyframe — the ladder "
        "steps down to level " + std::to_string(level_index_));
}

VideoLadderLevel BitrateController::current_level() const noexcept {
    VideoLadderLevel lvl = ladder_.at(level_index_);
    // The *effective* target is what the encoder must be told: the
    // rung's nominal bitrate, capped by the link's budget.  The engine
    // pushes this value into the encoder's parameter set (plan D4).
    lvl.bitrate_bps = effective_target(level_index_);
    return lvl;
}

VideoBitrateControllerStats BitrateController::stats() const noexcept {
    VideoBitrateControllerStats out = stats_;
    out.target_bitrate_bps = effective_target(level_index_);
    out.level_index = static_cast<std::uint32_t>(level_index_);
    if (cfg_.floor_level_index != VideoBitrateControllerConfig::kNoResolutionFloor ||
        cfg_.min_height > 0) {
        out.resolution_floor_index = static_cast<std::uint32_t>(floor_index());
        out.resolution_floor_height = ladder_.at(floor_index()).height;
    }
    return out;
}

plugins::Status BitrateController::set_level(std::uint32_t level_index) noexcept {
    if (ladder_.empty()) return plugins::kErrNotReady;
    if (level_index >= ladder_.size()) return plugins::kErrInvalidParam;

    // The floor is a *product* constraint, not a hint: the host cannot
    // override it through this path either, because the ladder's lower
    // rungs may not exist in a screen-content configuration at all.
    const std::size_t floor = floor_index();
    if (level_index > floor) return plugins::kErrInvalidParam;

    const std::size_t old = level_index_;
    level_index_ = level_index;
    if (old != level_index_) {
        ++stats_.level_changes;
        if (level_index_ > old) ++stats_.level_drops;
        else ++stats_.level_rises;
        last_step_at_ = now();
        have_step_time_ = true;
    }
    return plugins::kOk;
}

// ---------------------------------------------------------------------------
// The policy
// ---------------------------------------------------------------------------

std::size_t BitrateController::floor_index() const noexcept {
    if (ladder_.empty()) return 0;

    // The floor stated as a picture height wins: a profile says
    // "720p", and the rung index is derived from it.
    if (cfg_.min_height > 0) {
        return ladder_.index_for_height(cfg_.min_height);
    }

    if (cfg_.floor_level_index == VideoBitrateControllerConfig::kNoResolutionFloor) {
        // No floor: the ladder may use every rung.
        return ladder_.size() - 1;
    }
    return std::min<std::size_t>(cfg_.floor_level_index, ladder_.size() - 1);
}

std::uint32_t BitrateController::budget() const noexcept {
    // The link's opinion is the authority: the estimate is what the
    // link says it can carry.
    std::uint32_t b = estimate_bps_;

    // The measurement is the *encoder's* opinion, and it is live where
    // the estimate is stale.  When the encoder puts more on the wire
    // than the link says it can carry, the budget is scaled down in
    // proportion — the classic multiplicative decrease.  Without this
    // the controller would keep the estimate's optimism and the link
    // would keep losing packets.
    if (measured_bps_ > 0 && measured_bps_ > b && b > 0) {
        b = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(b) * b) / measured_bps_);
    }
    return b;
}

bool BitrateController::is_congested() const noexcept {
    // The encoder is putting more on the wire than the link says it
    // can carry.  The estimate may be stale — the measurement is the
    // live signal — so this case bypasses the hysteresis band.
    if (measured_bps_ == 0 || estimate_bps_ == 0) return false;
    return measured_bps_ > estimate_bps_;
}

std::uint32_t BitrateController::effective_target(std::size_t index) const noexcept {
    const std::uint32_t nominal = ladder_.bitrate_for(index);
    const std::uint32_t b = budget();
    if (b == 0) return nominal;
    // The rung says what the picture costs at its quality; the budget
    // says what the link can carry.  The encoder gets the smaller of
    // the two, so a link that collapses shrinks the bitrate even
    // before the ladder moves a rung.
    return ladder_.clamp_bitrate(std::min(nominal, b));
}

std::uint32_t BitrateController::current_usage() const noexcept {
    // What the current rung *asks for*.  This is the number the
    // headroom and the affordability of a step are measured against.
    return ladder_.bitrate_for(level_index_);
}

bool BitrateController::step_is_urgent() const noexcept {
    // Two ways a step is urgent:
    //
    //   1. the measurement says the link is *already* losing packets
    //      (the encoder is overrunning) — the burst is accepted
    //      knowingly, because not acting is worse;
    //   2. the link cannot even afford the *cheapest* rung the product
    //      allows — the ladder is heading for the floor anyway, so the
    //      IDR is paid once instead of the picture being stuck at a
    //      resolution the link cannot carry.
    if (is_congested()) return true;
    if (estimate_bps_ == 0) return false;
    return estimate_bps_ <= ladder_.bitrate_for(floor_index());
}

bool BitrateController::step_is_affordable(std::size_t target_index) const noexcept {
    if (cfg_.keyframe_cost_bps == 0) return true;
    if (estimate_bps_ == 0) return false;

    // The IDR is a burst *on top of* what the link is already
    // carrying, so the link needs that much headroom for the step to
    // be a *quiet* one ("step down on a quiet interval" — plan D3).
    // The headroom is measured above the new target, because that is
    // the rate the IDR is coded at.
    const std::uint32_t target = effective_target(target_index);
    const std::uint64_t needed =
        static_cast<std::uint64_t>(target) + cfg_.keyframe_cost_bps;
    return estimate_bps_ >= needed;
}

std::size_t BitrateController::choose_level() const noexcept {
    if (ladder_.empty()) return 0;

    const std::uint32_t b = budget();
    if (b == 0) {
        // No estimate yet: keep the current level rather than reading
        // "0" as "no bandwidth at all" and collapsing the stream on the
        // first frame.
        return level_index_;
    }

    // ---- The hysteresis band --------------------------------------------
    //
    // The budget has to move away from the current rung's nominal
    // target by more than the band before the ladder moves.  This is
    // what keeps a noisy estimate — a sawtooth trace, a jittery
    // measurement — from producing a staircase that flips level every
    // interval (plan D3).
    //
    // The band is relative to the *current target*, which is what makes
    // it a band rather than a threshold: the same 15 % means different
    // things at 2 Mbps and at 200 kbps, and the band follows.
    //
    // A link the measurement says is congested right now bypasses the
    // band: the estimate is stale and the live signal wins.
    const std::uint32_t current_nominal = current_usage();
    const double band = std::max(0.0, cfg_.hysteresis);
    if (current_nominal > 0 && band > 0.0 && !is_congested()) {
        const double delta =
            (static_cast<double>(b) - static_cast<double>(current_nominal)) /
            static_cast<double>(current_nominal);
        if (std::fabs(delta) < band) return level_index_;
    }

    // ---- The best rung that fits the budget ------------------------------
    //
    // The rung with the highest nominal bitrate that still fits the
    // budget is the best the link can afford.  The ladder is ordered
    // best-first for the configured mode, so the first rung that fits
    // is the answer.
    std::size_t best = ladder_.size() - 1;
    for (std::size_t i = 0; i < ladder_.size(); ++i) {
        if (ladder_.bitrate_for(i) <= b) {
            best = i;
            break;
        }
    }

    // ---- Prefer a rung that keeps the geometry ----------------------------
    //
    // A step that changes the geometry forces an IDR, and an IDR is a
    // bandwidth burst (plan D3).  So when the ladder is going *down*,
    // it first looks for a rung with the *same* picture size that still
    // fits the budget: the encoder simply gets less to spend and no
    // IDR is needed.  Only when no such rung exists does the picture
    // size have to change — and then the keyframe-cost budget has a
    // say (see `update()`).
    if (best > level_index_) {
        const VideoLadderLevel cur = ladder_.at(level_index_);
        for (std::size_t i = level_index_; i < ladder_.size(); ++i) {
            const VideoLadderLevel lvl = ladder_.at(i);
            if (lvl.width != cur.width || lvl.height != cur.height) continue;
            if (ladder_.bitrate_for(i) <= b) return i;
        }
    }

    return best;
}

void BitrateController::update() noexcept {
    if (ladder_.empty()) return;

    std::size_t wanted = choose_level();

    // ---- The resolution floor (remote-desktop plan D2) ------------------
    //
    // The floor is the worst rung the ladder may use.  A screen-content
    // stream that drops below it is no longer legible, and no amount of
    // bandwidth pressure makes that acceptable.
    const std::size_t floor = floor_index();
    if (wanted > floor) {
        ++stats_.floor_blocks;
        wanted = floor;
    }

    if (wanted == level_index_) {
        stats_.target_bitrate_bps = effective_target(level_index_);
        stats_.level_index = static_cast<std::uint32_t>(level_index_);
        return;
    }

    // ---- The cooldown (video plan B9) ------------------------------------
    //
    // The ladder may only move once per step interval.  Between two
    // steps the decision is *suppressed* — counted, so the behaviour is
    // observable rather than mysterious.
    const TimePoint t = now();
    if (have_step_time_ && cfg_.step_interval_ms > 0) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(t - last_step_at_);
        if (elapsed.count() < static_cast<std::int64_t>(cfg_.step_interval_ms)) {
            ++stats_.level_changes_suppressed;
            return;
        }
    }

    // ---- The maximum step per interval ----------------------------------
    const std::size_t max_step =
        cfg_.max_steps_per_interval > 0 ? cfg_.max_steps_per_interval : 1;
    if (wanted > level_index_) {
        wanted = std::min(wanted, level_index_ + max_step);
    } else if (level_index_ > wanted) {
        wanted = (level_index_ >= max_step) ? (level_index_ - max_step) : 0;
    }

    // The floor holds after the step is bounded as well: the ladder may
    // only ever move *towards* the floor, never past it.
    if (wanted > floor) wanted = floor;

    // ---- The keyframe-cost budget (remote-desktop plan D3) --------------
    //
    // A geometry change costs an IDR, and an IDR is a bandwidth burst.
    // The controller budgets for it: the step is deferred until the
    // link has the headroom to carry the burst — unless the step is
    // *urgent*, in which case the burst is accepted knowingly because
    // staying put is worse ("or accept the burst knowingly").
    const bool geometry_changes =
        wanted != level_index_ &&
        (ladder_.at(wanted).width != ladder_.at(level_index_).width ||
         ladder_.at(wanted).height != ladder_.at(level_index_).height);

    if (geometry_changes && !step_is_urgent() && !step_is_affordable(wanted)) {
        ++stats_.keyframe_cost_blocks;
        core::log::Logger::instance().debug(
            "engine[bitrate]: the keyframe-cost budget refused the "
            "geometry change to level " + std::to_string(wanted));
        return;
    }

    const std::size_t old = level_index_;
    level_index_ = wanted;
    last_step_at_ = t;
    have_step_time_ = true;

    ++stats_.level_changes;
    if (level_index_ > old) ++stats_.level_drops;
    else ++stats_.level_rises;

    stats_.target_bitrate_bps = effective_target(level_index_);
    stats_.level_index = static_cast<std::uint32_t>(level_index_);

    core::log::Logger::instance().info(
        "engine[bitrate]: the ladder moved " +
        std::string(level_index_ > old ? "down" : "up") + " to level " +
        std::to_string(level_index_) + " (" +
        std::to_string(ladder_.at(level_index_).width) + "x" +
        std::to_string(ladder_.at(level_index_).height) + "@" +
        std::to_string(ladder_.at(level_index_).fps) + ", " +
        std::to_string(stats_.target_bitrate_bps) + " bps, mode=" +
        std::string(plugins::video_ladder_mode_name(cfg_.mode)) + ")");
}

} // namespace nimrtc::video_bitrate_control
