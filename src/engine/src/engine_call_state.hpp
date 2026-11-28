/**
 * @file src/engine/src/engine_call_state.hpp
 * @brief PRIVATE engine collaborator (v1.4 D7): the call-state machine.
 *
 * A host that wants to know "how is this call doing" today has to assemble
 * the answer from `set_on_state_change` (a coarse two-point signal: "open" /
 * "closed"), `set_on_error` (error codes) and polling `Stats` — three shapes
 * owned by three places.  D7 adds the *confluence point*: one callback that
 * spans the whole stack — `connecting` → `connected` → `reconnecting` →
 * `failed` / `closed` — carrying the N3 (reconnection) / N4 (failure) / N8
 * (path-change) details.
 *
 * ## What lives here
 *
 *   - `CallStateMachine` — the "translate engine events into state
 *     transitions, and only report on an actual transition" machine.  It is
 *     pure state: it holds no transport handle, no clock and no plugin
 *     (C3 — a decision must be deterministic).
 *   - `failure_from_reason` — the one place where the N4 failure-vocabulary
 *     strings are mapped onto the `CallFailure` enum, so the two cannot
 *     drift apart (the risk the spec's 风险 table calls out).
 *
 * ## The judge
 *
 * The machine reports **transitions**, not states: an observation that maps
 * to the current state reports nothing (the host asked for *migrations*, not
 * a per-tick stream — spec §二), and the very first observation always
 * reports `kConnecting` (that is what `open()` fires — spec acceptance 7).
 *
 * ## Clock discipline
 *
 * The machine touches no clock.  The N3 / N8 details the *reports* carry are
 * read by the engine's `fire_call_state()` from the owning `SessionState`'s
 * `ice_recovery` / `path_change` collaborators at report time — the machine
 * only decides *when* a report is due.
 *
 * ## Privacy
 *
 * This header is PRIVATE: it is not installed and no public header includes
 * it.  It reaches the public `CallState` / `CallFailure` / `IceCandidatePairInfo`
 * types through the engine's own public header and reads ICE state through
 * `plugins::IceState` — never through a concrete `nimrtc::ice/` header
 * (Layout Invariant 4 / ADR-025).
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/ice_transport.hpp>

namespace nimrtc::engine::detail {

// The public types the machine reports in.  They are nested in
// `NimRTCEngine` (the same shape as the nested `IceRecoveryReport`), so the
// machine — a plain class in the `detail` namespace, not a member — spells
// them through these aliases instead of qualifying every declaration.
using CallState = nimrtc::engine::NimRTCEngine::CallState;
using CallFailure = nimrtc::engine::NimRTCEngine::CallFailure;

/** Map an N4 failure-vocabulary string onto the `CallFailure` enum.
 *
 *  The mapping is a single pure function so the string vocabulary (the N4
 *  `TransportStatsProjector::ice_failure_reason()` output) and the enum a
 *  host `switch`es on cannot drift apart (spec §风险):
 *
 *    "transport-unsupported"  → kTurnRefused
 *    "credentials-rejected"   → kCredentialsRejected
 *    "checks-failed"          → kIceFailed
 *    "no-pair"                → kIceFailed
 *    "no-candidates"          → kIceFailed
 *    "none" / "" / unknown    → kNone  (no failure is *not* a failure)
 *
 * @see TransportStatsProjector::ice_failure_reason (engine_transport_stats.hpp)
 */
CallFailure failure_from_reason(std::string_view reason) noexcept;

/** The call-state machine (v1.4 D7).
 *
 *  It does one thing: translate "what happened in the engine" into "one
 *  state transition", and fire at most once per real transition — the same
 *  state is never reported twice.
 *
 *  It is pure state: it does not touch a transport, a clock or a plugin
 *  model.  It is owned by `SessionState` (like the N3 `IceRecovery` and the
 *  N8 `PathChangeTracker`); the engine's tick drives it from its own
 *  observations.
 */
class CallStateMachine {
public:
    /** Record an observation and return whether a report is due.
     *
     *  The judge (pure function of the new state vs the current state):
     *  - state unchanged → **no report** (never repeat a state)
     *  - first observation → **report** `kConnecting` (that is what
     *    `open()` fires — the initial state)
     *  - state changed → **report**
     *
     *  `ice_state` is the ICE layer's raw state (`plugins::IceState`), which
     *  this observation translates:
     *  - `Disconnected` / `Gathering` / `Connecting` → `kConnecting`
     *  - `Connected` / `Completed` → `kConnected`
     *  - `Failed` → `kFailed`
     *
     *  `kReconnecting` is **not** decided by an ICE observation — it is
     *  decided by the N3 reconnection machine (the engine knows it is
     *  recovering; ICE only knows it is down), so it enters through
     *  `enter_reconnecting()`. */
    bool observe(plugins::IceState ice_state) noexcept;

    /** Enter reconnecting (called at the N3 restart point). */
    bool enter_reconnecting() noexcept;

    /** Enter terminal failure (N3 terminal / DTLS failure / media failure).
     *
     *  `kFailed` is absorbing: a later observation / `enter_reconnecting()`
     *  / `fail()` reports nothing — the migration diagram ends at `failed`,
     *  so a host hangs up and starts a new call rather than "recovering" a
     *  failed one.  Only `close()` may leave it. */
    bool fail(CallFailure failure, std::string_view reason) noexcept;

    /** Close the call (`stop()` / `hangup()` / destructor).
     *
     *  Idempotent (a repeated `close()` reports nothing), and the only
     *  migration that may leave `kFailed` — a closed call must still report
     *  `kClosed`. */
    bool close() noexcept;

    /** The current state. */
    CallState state() const noexcept { return state_; }

    /** The failure of the last `fail()` that entered `kFailed`. */
    CallFailure failure() const noexcept { return failure_; }

    /** The reason of the last `fail()` that entered `kFailed`. */
    const std::string& reason() const noexcept { return reason_; }

    /** Clear everything (`open()` calls it — every session starts from
     *  zero). */
    void reset() noexcept;

private:
    /** The transition judge: no report when the target is the current state,
     *  and the end states (`kFailed` / `kClosed`) are absorbing — no
     *  migration may leave them (spec's diagram ends at `failed` / `closed`).
     *  `close()` is the exception, so it does not use this judge. */
    bool transition(CallState target) noexcept;

    static CallState translate(plugins::IceState ice_state) noexcept;

    CallState state_ = CallState::kConnecting;
    CallFailure failure_ = CallFailure::kNone;
    std::string reason_;
    bool reported_ = false;
};

}  // namespace nimrtc::engine::detail
