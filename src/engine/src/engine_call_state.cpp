/**
 * @file src/engine/src/engine_call_state.cpp
 * @brief PRIVATE engine collaborator (v1.4 D7): the call-state machine.
 *
 * @see engine_call_state.hpp — the design and the transition judge.
 */

#include "engine_call_state.hpp"

namespace nimrtc::engine::detail {

CallFailure failure_from_reason(std::string_view reason) noexcept {
    // The N4 vocabulary's two non-ICE reasons are mapped explicitly; the
    // three ICE failure reasons all mean "the ICE layer failed".  "none"
    // (and anything unrecognised) is NOT a failure — the spec pins this
    // so a healthy session is never misreported as failed (acceptance 19).
    if (reason == "credentials-rejected") return CallFailure::kCredentialsRejected;
    if (reason == "transport-unsupported") return CallFailure::kTurnRefused;
    if (reason == "checks-failed" || reason == "no-pair" ||
        reason == "no-candidates") {
        return CallFailure::kIceFailed;
    }
    return CallFailure::kNone;
}

CallState CallStateMachine::translate(plugins::IceState ice_state) noexcept {
    switch (ice_state) {
        case plugins::IceState::Connected:
        case plugins::IceState::Completed:
            return CallState::kConnected;
        case plugins::IceState::Failed:
            return CallState::kFailed;
        case plugins::IceState::Disconnected:
        case plugins::IceState::Gathering:
        case plugins::IceState::Connecting:
            return CallState::kConnecting;
    }
    return CallState::kConnecting;
}

bool CallStateMachine::transition(CallState target) noexcept {
    // The end states are absorbing: once failed / closed, no migration may
    // leave them — the spec's migration diagram ends at `failed` / `closed`,
    // so a host hangs up and starts a new call rather than "recovering" a
    // failed one.  (This is what keeps a DTLS failure from oscillating the
    // report between kConnected and kFailed on every tick: the ICE layer
    // stays up, but the call is over.)
    if (state_ == CallState::kFailed || state_ == CallState::kClosed) return false;
    if (state_ == target) return false;
    state_ = target;
    return true;
}

bool CallStateMachine::observe(plugins::IceState ice_state) noexcept {
    // The first observation always reports `kConnecting` — that is the
    // "the call is being established" report `open()` fires (spec §二:
    // 首次观察 → 发).  `state_` starts at kConnecting, so no assignment is
    // needed; only the reported flag flips.  The terminal states gate first
    // though: after `fail()` / `close()` a later observation must report
    // nothing (the absorbing end states, ADR-035 §2).
    if (state_ == CallState::kFailed || state_ == CallState::kClosed) return false;
    if (!reported_) {
        reported_ = true;
        return true;
    }
    return transition(translate(ice_state));
}

bool CallStateMachine::enter_reconnecting() noexcept {
    // A repeat enter (already reconnecting) reports nothing; a close()
    // already happened reports nothing (kClosed is terminal).
    return transition(CallState::kReconnecting);
}

bool CallStateMachine::fail(CallFailure failure, std::string_view reason) noexcept {
    if (!transition(CallState::kFailed)) return false;
    failure_ = failure;
    reason_.assign(reason);
    return true;
}

bool CallStateMachine::close() noexcept {
    // Idempotent, and the only migration that may leave kFailed (it does not
    // use the `transition()` judge): a closed call must still report kClosed.
    if (state_ == CallState::kClosed) return false;
    state_ = CallState::kClosed;
    return true;
}

void CallStateMachine::reset() noexcept {
    state_ = CallState::kConnecting;
    failure_ = CallFailure::kNone;
    reason_.clear();
    reported_ = false;
}

}  // namespace nimrtc::engine::detail
