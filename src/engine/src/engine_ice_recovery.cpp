/**
 * @file src/engine/src/engine_ice_recovery.cpp
 * @brief PRIVATE engine collaborator (v1.4 N3): the ICE-recovery state machine.
 *
 * The implementation half of `engine_ice_recovery.hpp`.  The machine owns
 * no clock and no transport: every entry point receives `now_us` from the
 * caller (the engine's tick thread), and the restarts themselves are
 * executed by the engine through the `plugins::IICETransport::restart()`
 * seam — this TU only books the budget and the backoff.
 */
#include "engine_ice_recovery.hpp"

#include <algorithm>

namespace nimrtc::engine::detail {

IceRecoveryPolicy resolve_ice_recovery_policy(std::string_view name) noexcept {
    // The fallback is "off" on purpose (ADR-031 §2.2): the engine's failure
    // mode is "restarts too often", so a mistyped key must disarm the
    // engine, never arm it.  This is the opposite of connectivity_profile's
    // "unknown → auto" fallback, and the asymmetry is deliberate.
    if (name == "auto") return IceRecoveryPolicy::kAuto;
    return IceRecoveryPolicy::kOff;
}

IceRecovery::IceRecovery(Config config) noexcept : config_(config) {}

void IceRecovery::note_disconnected(std::int64_t now_us) noexcept {
    switch (state_) {
        case State::kIdle:
            state_ = State::kGrace;
            grace_started_us_ = now_us;
            pending_reason_ = "local-address-changed";
            break;
        case State::kGrace:
        case State::kBackoff:
        case State::kTerminal:
            // Already in grace / already backing off / given up: a repeat
            // `Disconnected` observation changes nothing.
            break;
    }
}

void IceRecovery::note_failed(std::int64_t now_us) noexcept {
    if (state_ == State::kBackoff || state_ == State::kTerminal) return;
    // ICE explicitly failed — not a blip, so the grace window is skipped.
    enter_backoff(now_us);
    pending_reason_ = "ice-failed";
}

void IceRecovery::note_up(std::int64_t now_us) noexcept {
    (void)now_us;
    switch (state_) {
        case State::kGrace:
            // The blip self-healed inside the grace window: no restart was
            // executed, so no budget was spent (semantics #1 — the count
            // clears, the same family as D4's note_succeeded()).
            state_ = State::kIdle;
            restarts_used_ = 0;
            pending_reason_.clear();
            break;
        case State::kBackoff:
            // A restart (or the peer's own recovery) brought ICE up: back
            // to idle.  The restart that was executed keeps its budget slot
            // in the sliding window.
            state_ = State::kIdle;
            backoff_us_ = 0;
            next_retry_us_ = 0;
            pending_reason_.clear();
            break;
        case State::kIdle:
        case State::kTerminal:
            break;
    }
}

void IceRecovery::note_grace_expired(std::int64_t now_us) noexcept {
    if (state_ != State::kGrace) return;
    enter_backoff(now_us);
    pending_reason_ = "local-address-changed";
}

bool IceRecovery::grace_expired(std::int64_t now_us) const noexcept {
    return state_ == State::kGrace &&
           now_us - grace_started_us_ >= config_.grace_us;
}

bool IceRecovery::due(std::int64_t now_us) const noexcept {
    return state_ == State::kBackoff && !terminal() &&
           now_us >= next_retry_us_;
}

bool IceRecovery::note_restart_attempted(std::int64_t now_us,
                                         std::string_view blocked_reason) noexcept {
    if (terminal()) return false;

    // The budget window is sliding (semantics #2): once a whole window has
    // lapsed since the last restart, old restarts fall out of the budget.
    if (window_expired(now_us)) {
        window_start_us_ = now_us;
        restarts_used_ = 0;
    }
    if (restarts_used_ >= config_.max_restarts) {
        enter_terminal(blocked_reason);
        return false;
    }

    ++restarts_used_;
    // Every restart pushes the window's origin to now (ADR-031 §2.5), so a
    // burst of restarts shares one window and the oldest ones fall off.
    window_start_us_ = now_us;

    // The backoff doubles *at attempt time* (semantics #3): a failed
    // restart must still make the next attempt wait twice as long, and the
    // engine cannot rely on observing the failure.  A manual restart from a
    // non-backoff state (backoff_us_ == 0) starts the doubling from the
    // initial backoff; the schedule stays inert until kBackoff is entered.
    backoff_us_ = std::min((backoff_us_ == 0 ? config_.initial_backoff_us
                                             : backoff_us_) * 2,
                           config_.max_backoff_us);
    next_retry_us_ = now_us + backoff_us_;
    return true;
}

void IceRecovery::note_restart_failed(std::int64_t now_us) noexcept {
    // The attempt already consumed its budget slot and doubled the backoff
    // when `note_restart_attempted` ran (semantics #3).  Nothing further to
    // schedule: the machine keeps waiting in kBackoff until the already
    // doubled `next_retry_us_` comes due.
    (void)now_us;
}

std::uint32_t IceRecovery::restarts_used(std::int64_t now_us) const noexcept {
    if (terminal()) return restarts_used_;
    return window_expired(now_us) ? 0u : restarts_used_;
}

std::uint32_t IceRecovery::remaining_budget(std::int64_t now_us) const noexcept {
    if (terminal()) return 0u;
    const std::uint32_t used = restarts_used(now_us);
    return used >= config_.max_restarts ? 0u : config_.max_restarts - used;
}

void IceRecovery::reset() noexcept {
    state_ = State::kIdle;
    restarts_used_ = 0;
    window_start_us_ = 0;
    grace_started_us_ = 0;
    backoff_us_ = 0;
    next_retry_us_ = 0;
    terminal_reason_.clear();
    pending_reason_.clear();
}

void IceRecovery::reset_budget() noexcept {
    state_ = State::kIdle;
    restarts_used_ = 0;
    grace_started_us_ = 0;
    backoff_us_ = 0;
    next_retry_us_ = 0;
    terminal_reason_.clear();
    pending_reason_.clear();
    // window_start_us_ is deliberately NOT reset (spec §四 20): the
    // sliding window's origin stays, so the host's "retry" button remains
    // bounded by the same 5-per-10-minutes rule.
}

void IceRecovery::enter_backoff(std::int64_t now_us) noexcept {
    state_ = State::kBackoff;
    backoff_us_ = config_.initial_backoff_us;
    next_retry_us_ = now_us + backoff_us_;
}

void IceRecovery::enter_terminal(std::string_view reason) noexcept {
    state_ = State::kTerminal;
    terminal_reason_ = std::string(reason);
}

}  // namespace nimrtc::engine::detail
