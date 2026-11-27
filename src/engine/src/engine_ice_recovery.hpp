/**
 * @file src/engine/src/engine_ice_recovery.hpp
 * @brief PRIVATE engine collaborator (v1.4 N3): the ICE-recovery state machine.
 *
 * The engine's reconnection policy (ADR-031, accepted): when ICE goes down,
 * the engine restarts ICE on its own, bounded by an exponential backoff and
 * a sliding restart budget, instead of leaving the session to die.
 *
 * ## What lives here
 *
 *   - `IceRecoveryPolicy` / `resolve_ice_recovery_policy` — the host-facing
 *     switch ("auto" = the engine manages reconnection, "off" = the host
 *     does).  Unknown names resolve to `kOff` (deliberately the *opposite*
 *     fallback to `connectivity_profile`: a mistyped key must not arm the
 *     engine to restart ICE at a moment the host did not expect — the
 *     engine's failure mode is "restarts too often", not "fails to
 *     connect", ADR-031 §2.2).
 *
 *   - `IceRecovery` — the state machine.  `kIdle` → `kGrace` (a
 *     `Disconnected` blip gets a grace window to self-heal) → `kBackoff`
 *     (grace expired: wait 1 / 2 / 4 / 8 / 8… s, capped at 8 s, then the
 *     engine restarts ICE) → `kTerminal` (the budget — 5 restarts per
 *     sliding 10-minute window — is spent: reported, not looped).
 *
 * ## The three pinned semantics (spec §方案)
 *
 *   1. **A grace-window recovery clears the count.**  ICE came back by
 *      itself, so no restart was executed and no budget was spent — the
 *      count only increments on an actual restart.
 *   2. **The budget window is sliding.**  Every restart pushes the window's
 *      start to `now`; a restart outside the window is a fresh start.  A
 *      2-hour-old session is never blocked by its own history.
 *   3. **A failed restart still consumes budget and doubles the backoff.**
 *      Rebuilding the libjuice agent costs the same whether the restart
 *      lands or not.
 *
 * ## Clock discipline
 *
 * Every entry point takes `now_us` explicitly (the same rule as
 * `engine_capture_reconnect.hpp`, D4) so the tests drive a virtual clock and
 * never sleep: `test_engine_ice_recovery.cpp` pins grace, backoff, cap and
 * the terminal state deterministically.
 *
 * ## Privacy
 *
 * This header is PRIVATE: it is not installed and no public header includes
 * it.  It includes only `core` / `plugins` types — never a concrete
 * `nimrtc::ice/` header (Layout Invariant 4 / ADR-025: the engine reads ICE
 * state through `plugins::IceState` and restarts through
 * `plugins::IICETransport::restart()`).
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace nimrtc::engine::detail {

/** The reconnection-policy value the engine applies (ADR-031 §2.2). */
enum class IceRecoveryPolicy : std::uint8_t {
    kOff = 0,   ///< the host owns reconnection — nothing happens automatically
    kAuto = 1,  ///< the engine restarts ICE on its own (default)
};

/** Resolve a policy name.
 *
 *  `"auto"` → kAuto; everything else (`""`, `"off"`, unknown) → kOff.
 *  The fallback direction is the opposite of `connectivity_profile`'s and
 *  is deliberate: a mistyped key must not silently arm the engine. */
IceRecoveryPolicy resolve_ice_recovery_policy(std::string_view name) noexcept;

/** The reconnection state machine.
 *
 *  All entry points take the clock explicitly (`now_us`, microseconds
 *  since boot).  The machine is owned by `SessionState`; the engine's tick
 *  drives it from the `plugins::IceState` observations and executes the
 *  restarts (`plugins::IICETransport::restart()`), so the machine itself
 *  never touches a transport.
 */
class IceRecovery {
public:
    struct Config {
        /** Grace window on `Disconnected` (µs).  Default 5 s. */
        std::int64_t grace_us = 5'000'000;
        /** First backoff (µs).  Default 1 s. */
        std::int64_t initial_backoff_us = 1'000'000;
        /** Backoff cap (µs).  Default 8 s. */
        std::int64_t max_backoff_us = 8'000'000;
        /** Budget window (µs).  Default 10 minutes. */
        std::int64_t budget_window_us = 600'000'000;
        /** Restarts allowed per window.  Default 5. */
        std::uint32_t max_restarts = 5;
    };

    enum class State : std::uint8_t {
        kIdle = 0,      ///< healthy
        kGrace = 1,     ///< down, inside the grace window
        kBackoff = 2,   ///< grace expired, waiting out the backoff
        kTerminal = 3,  ///< budget spent, giving up
    };

    explicit IceRecovery(Config config = {}) noexcept;

    const Config& config() const noexcept { return config_; }
    State state() const noexcept { return state_; }
    bool terminal() const noexcept { return state_ == State::kTerminal; }

    /** The stored terminal reason: "budget-exhausted" / "manual-blocked".
     *  Empty before the terminal state is entered. */
    std::string_view terminal_reason() const noexcept { return terminal_reason_; }

    /** The reason the *next* restart would be executed for:
     *  "local-address-changed" / "ice-failed".  Empty when no outage is
     *  pending. */
    std::string_view pending_reason() const noexcept { return pending_reason_; }

    /** The backoff the next restart will wait out, µs. */
    std::int64_t current_backoff_us() const noexcept { return backoff_us_; }

    // ---- Entry points ----------------------------------------------------

    /** ICE went `Disconnected` (first drop): kIdle → kGrace.  Already in
     *  kGrace → stays (the grace window does not extend); kBackoff /
     *  kTerminal → no-op. */
    void note_disconnected(std::int64_t now_us) noexcept;

    /** ICE went `Failed`: kIdle / kGrace → kBackoff directly, skipping the
     *  grace window (explicit failure is not a blip).  kBackoff / kTerminal
     *  → no-op. */
    void note_failed(std::int64_t now_us) noexcept;

    /** ICE came back up: kGrace → kIdle with the count cleared (semantics
     *  #1); kBackoff → kIdle with the count kept in the window; kIdle /
     *  kTerminal → no-op. */
    void note_up(std::int64_t now_us) noexcept;

    /** The grace window elapsed: kGrace → kBackoff (first backoff is 1 s). */
    void note_grace_expired(std::int64_t now_us) noexcept;

    /** True when kGrace and the grace window has elapsed. */
    bool grace_expired(std::int64_t now_us) const noexcept;

    /** True when the engine should execute a restart: kBackoff, not
     *  terminal, and `next_retry_us_` reached. */
    bool due(std::int64_t now_us) const noexcept;

    /** Consume one budget slot for a restart that is about to execute
     *  (auto or manual — one shared budget, ADR-031 §2.3).
     *
     *  The window is slid to `now` and the backoff doubled for the *next*
     *  attempt (semantics #2 / #3).  When the budget is already spent the
     *  machine enters kTerminal with `blocked_reason` and returns false. */
    bool note_restart_attempted(std::int64_t now_us,
                                std::string_view blocked_reason) noexcept;

    /** Record that the executed restart failed.  The budget slot and the
     *  backoff doubling were already accounted for at
     *  `note_restart_attempted` time (semantics #3); the machine keeps
     *  waiting in kBackoff. */
    void note_restart_failed(std::int64_t now_us) noexcept;

    // ---- Projections ------------------------------------------------------

    /** Restarts spent in the current window, 0 once the window lapsed. */
    std::uint32_t restarts_used(std::int64_t now_us) const noexcept;

    /** Budget slots left in the current window (0 = spent). */
    std::uint32_t remaining_budget(std::int64_t now_us) const noexcept;

    // ---- Reset ------------------------------------------------------------

    /** Full reset: idle, counts and window origin cleared. */
    void reset() noexcept;

    /** Clear the count and the terminal flag only — the sliding window's
     *  origin is deliberately kept (spec §四 20): the host's "retry"
     *  button stays bounded by the same 5-per-10-minutes rule. */
    void reset_budget() noexcept;

private:
    void enter_backoff(std::int64_t now_us) noexcept;
    void enter_terminal(std::string_view reason) noexcept;
    bool window_expired(std::int64_t now_us) const noexcept {
        return now_us - window_start_us_ >= config_.budget_window_us;
    }

    Config config_;
    State state_ = State::kIdle;
    std::uint32_t restarts_used_ = 0;
    std::int64_t window_start_us_ = 0;
    std::int64_t grace_started_us_ = 0;
    std::int64_t backoff_us_ = 0;
    std::int64_t next_retry_us_ = 0;
    std::string terminal_reason_;
    std::string pending_reason_;
};

}  // namespace nimrtc::engine::detail
