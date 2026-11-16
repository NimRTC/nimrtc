// src/modules/rd_input/src/deadman_watchdog.cpp
// ----------------------------------------------------------------------------
// The dead-man switch and the liveness heartbeat (remote-desktop plan G6).
// ----------------------------------------------------------------------------

#include "nimrtc/rd_input/deadman_watchdog.hpp"

namespace nimrtc::rd_input {
namespace {

/// Microseconds per millisecond.
constexpr std::uint64_t kUsPerMs = 1000ull;

} // namespace

// ---------------------------------------------------------------------------
// DeadManWatchdog
// ---------------------------------------------------------------------------

DeadManWatchdog::DeadManWatchdog(DeadManConfig config,
                                  SafeStateCallback on_safe,
                                  RecoveryCallback on_recovery)
    : config_{config},
      on_safe_{std::move(on_safe)},
      on_recovery_{std::move(on_recovery)} {}

void DeadManWatchdog::enter_safe_state(DeadManState next) noexcept {
    if (state_ == next) {
        return;
    }
    state_ = next;
    ++safe_entries_;
    if (on_safe_) {
        on_safe_(next);
    }
}

void DeadManWatchdog::enter_alive() noexcept {
    if (state_ == DeadManState::kAlive) {
        return;
    }
    state_ = DeadManState::kAlive;
    ++recoveries_;
    if (on_recovery_) {
        on_recovery_();
    }
}

void DeadManWatchdog::note_heartbeat(std::uint64_t now_us) noexcept {
    last_activity_us_ = now_us;
    has_activity_ = true;

    // The heartbeat never resurrects a watchdog that was released by the
    // dead-man switch: a release is a deliberate operator action and only
    // a new explicit activity may clear it.
    if (state_ == DeadManState::kTimedOut) {
        enter_alive();
    }
}

void DeadManWatchdog::note_activity(std::uint64_t now_us) noexcept {
    last_activity_us_ = now_us;
    has_activity_ = true;

    if (state_ == DeadManState::kReleased) {
        // A release is deliberate and the safe state is **sticky**: an
        // arbitrary mouse move must not silently resume the remote
        // session.  Only an explicit re-arm (pressing the switch again)
        // or `reset()` clears it.
        return;
    }
    if (state_ == DeadManState::kTimedOut) {
        enter_alive();
    }
}

void DeadManWatchdog::note_deadman_released() noexcept {
    enter_safe_state(DeadManState::kReleased);
}

void DeadManWatchdog::note_deadman_pressed() noexcept {
    // The operator pressed the switch again: the session is armed.  This
    // is the only way out of the sticky released state (besides `reset`).
    enter_alive();
}

void DeadManWatchdog::observe(const InputEvent& event, std::uint64_t now_us) noexcept {
    if (config_.deadman_enabled
        && event.type == InputEventType::kMouseButton
        && event.button == config_.deadman_button) {
        note_activity(now_us);
        if (event.button_action == ButtonAction::kUp) {
            note_deadman_released();
        } else {
            note_deadman_pressed();
        }
        return;
    }
    note_activity(now_us);
}

bool DeadManWatchdog::check_timeout(std::uint64_t now_us) noexcept {
    if (config_.heartbeat_timeout_ms == 0) {
        return false;
    }
    if (!has_activity_) {
        return false;
    }
    if (state_ != DeadManState::kAlive) {
        return false;
    }
    const std::uint64_t timeout_us =
        static_cast<std::uint64_t>(config_.heartbeat_timeout_ms) * kUsPerMs;
    if (now_us < last_activity_us_) {
        return false;
    }
    if (now_us - last_activity_us_ < timeout_us) {
        return false;
    }
    enter_safe_state(DeadManState::kTimedOut);
    return true;
}

DeadManState DeadManWatchdog::state() const noexcept {
    return state_;
}

bool DeadManWatchdog::alive() const noexcept {
    return state_ == DeadManState::kAlive;
}

std::uint64_t DeadManWatchdog::silence_us(std::uint64_t now_us) const noexcept {
    if (!has_activity_) {
        return 0;
    }
    if (now_us < last_activity_us_) {
        return 0;
    }
    return now_us - last_activity_us_;
}

std::uint64_t DeadManWatchdog::safe_state_entries() const noexcept {
    return safe_entries_;
}

std::uint64_t DeadManWatchdog::recoveries() const noexcept {
    return recoveries_;
}

const DeadManConfig& DeadManWatchdog::config() const noexcept {
    return config_;
}

void DeadManWatchdog::reset() noexcept {
    state_ = DeadManState::kAlive;
    last_activity_us_ = 0;
    has_activity_ = false;
    safe_entries_ = 0;
    recoveries_ = 0;
}

// ---------------------------------------------------------------------------
// HeartbeatEmitter
// ---------------------------------------------------------------------------

HeartbeatEmitter::HeartbeatEmitter(std::uint32_t interval_ms)
    : interval_ms_{interval_ms} {}

bool HeartbeatEmitter::due(std::uint64_t now_us) noexcept {
    if (interval_ms_ == 0) {
        return false;
    }
    if (!has_beat_) {
        return true;
    }
    if (now_us < last_beat_us_) {
        // The clock went backwards; treat it as "a beat is due" rather
        // than as an arithmetic accident.
        return true;
    }
    const std::uint64_t interval_us =
        static_cast<std::uint64_t>(interval_ms_) * kUsPerMs;
    return now_us - last_beat_us_ >= interval_us;
}

void HeartbeatEmitter::on_sent(std::uint64_t now_us) noexcept {
    last_beat_us_ = now_us;
    has_beat_ = true;
    ++beats_;
}

std::uint32_t HeartbeatEmitter::interval_ms() const noexcept {
    return interval_ms_;
}

std::uint64_t HeartbeatEmitter::beats_sent() const noexcept {
    return beats_;
}

bool HeartbeatEmitter::disabled() const noexcept {
    return interval_ms_ == 0;
}

void HeartbeatEmitter::reset() noexcept {
    last_beat_us_ = 0;
    has_beat_ = false;
    beats_ = 0;
}

} // namespace nimrtc::rd_input
