/**
 * @file nimrtc/rd_input/deadman_watchdog.hpp
 * @brief The dead-man switch and the liveness heartbeat (remote-desktop plan G6).
 *
 * ## What this is
 *
 * A remote-desktop session gives one machine control of another machine's
 * keyboard and mouse.  Two things must therefore be true at all times:
 *
 *   1. the operator is **still there** — if the control path dies
 *      silently, the remote side must not sit in a state where it is
 *      waiting for an instruction that will never come, and
 *   2. the operator has **not walked away** — if the operator stops
 *      moving the mouse, the remote side must release whatever it is
 *      holding.
 *
 * The first is the **heartbeat**: the operator's side emits a periodic
 * liveness beat; the remote side notices when the beats stop.
 * The second is the **dead-man switch**: a modifier the operator holds
 * (usually a mouse button, or a key) while doing something that must stop
 * if the operator stops; the remote side acts as soon as it is released.
 *
 * Both are the same mechanism viewed from two ends, so they live in one
 * class: the **remote side** owns the `DeadManWatchdog`, the **operator
 * side** owns the `HeartbeatEmitter`.
 *
 * ## The safe state
 *
 * The watchdog does not know *what* the safe state is — that is the
 * application's business (release the mouse button, dismiss the menu,
 * close the overlay).  So it takes a **callback** and invokes it when the
 * transition happens.  The callback is invoked **once per transition**:
 * entering the safe state once, leaving it once.  A watchdog that fired
 * the callback on every tick would spam the application.
 *
 * ## Threading
 *
 * Not thread-safe.  The watchdog is driven from the session's control
 * thread — the same thread that receives the control events.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <functional>

#include "nimrtc/rd_input/input_event.hpp"

namespace nimrtc::rd_input {

// ---------------------------------------------------------------------------
// DeadManState
// ---------------------------------------------------------------------------

/** The watchdog's liveness state. */
enum class NIMRTC_API_STABLE DeadManState : std::uint8_t {
    /** The control path is alive and the operator is present. */
    kAlive = 0,
    /** The control path has gone quiet for longer than the timeout; the
     *  safe state has been requested. */
    kTimedOut = 1,
    /** The operator explicitly released the dead-man switch; the safe
     *  state has been requested. */
    kReleased = 2,
};

/** Human-readable state name (for logs and diagnostics). */
NIMRTC_API_STABLE inline std::string_view deadman_state_name(DeadManState s) noexcept {
    switch (s) {
        case DeadManState::kAlive:    return "alive";
        case DeadManState::kTimedOut: return "timed-out";
        case DeadManState::kReleased: return "released";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// DeadManConfig
// ---------------------------------------------------------------------------

/** The watchdog's configuration. */
struct NIMRTC_API_STABLE DeadManConfig {
    /**
     * @brief How long the control path may stay silent before the safe
     *        state is requested, in milliseconds.
     *
     * Zero disables the timeout (the heartbeat is then not enforced).
     * The default is 1000 ms — long enough to survive a scheduling hiccup
     * or one lost packet, short enough that an operator who walks away
     * does not leave the remote machine in a held state.
     */
    std::uint32_t heartbeat_timeout_ms = 1000;

    /**
     * @brief The interval at which the operator side emits a heartbeat.
     *
     * The operator side is responsible for honouring it; the watchdog
     * only enforces the timeout.  The default is 250 ms, a quarter of the
     * timeout, so two consecutive beats may be lost before the timeout
     * fires.
     */
    std::uint32_t heartbeat_interval_ms = 250;

    /**
     * @brief Whether the dead-man switch is armed.
     *
     * When true, the operator's side sends a "switch held" / "switch
     * released" pair of events (as `kMouseButton` down/up on the
     * configured button, or as a key down/up); the watchdog enters the
     * safe state on the release.
     */
    bool deadman_enabled = false;

    /** The mouse button that acts as the dead-man switch.  Only used when
     *  `deadman_enabled` is true and the operator's side sends button
     *  events. */
    MouseButton deadman_button = MouseButton::kLeft;
};

// ---------------------------------------------------------------------------
// DeadManWatchdog
// ---------------------------------------------------------------------------

/**
 * @brief The remote side's dead-man switch and heartbeat monitor.
 *
 * ## The state machine
 *
 * ```
 *            heartbeat / activity
 *   kTimedOut  ----------------->  kAlive
 *   kReleased  ----------------->  kAlive
 *   kAlive     --(timeout)------>  kTimedOut
 *   kAlive     --(release)------>  kReleased
 * ```
 *
 * The **safe-state callback** fires on the transition *into* `kTimedOut`
 * or `kReleased`, and the **recovery callback** fires on the transition
 * back into `kAlive`.  Each fires once per transition.
 *
 * ## What counts as activity
 *
 * `note_activity()` is called for every inbound control event, whether
 * or not it is a heartbeat — a session that is actively being controlled
 * is alive even if the heartbeat beat is late.  `note_heartbeat()` is the
 * explicit beat.
 */
class NIMRTC_API_STABLE DeadManWatchdog {
public:
    /**
     * @brief The callback invoked when the safe state must be entered.
     *
     * The argument is the state that was entered, so one callback can
     * serve both the timeout and the release paths.
     */
    using SafeStateCallback = std::function<void(DeadManState)>;

    /**
     * @brief The callback invoked when the session is alive again.
     */
    using RecoveryCallback = std::function<void()>;

    /**
     * @brief Construct a watchdog.
     *
     * @param config    the configuration.
     * @param on_safe   the safe-state callback.  May be empty, in which
     *                  case the transition is still recorded and
     *                  observable through `state()`.
     * @param on_recovery the recovery callback.  May be empty.
     */
    explicit DeadManWatchdog(DeadManConfig config,
                              SafeStateCallback on_safe = {},
                              RecoveryCallback on_recovery = {});

    /**
     * @brief Note that a heartbeat arrived.
     *
     * Resets the timeout.  A heartbeat received while the watchdog has
     * already timed out brings it back to `kAlive` — but only if the
     * dead-man switch is not currently held.
     *
     * @param now_us the current monotonic time in microseconds.
     */
    void note_heartbeat(std::uint64_t now_us) noexcept;

    /**
     * @brief Note that any control activity arrived.
     *
     * Equivalent to `note_heartbeat()` for liveness purposes: a session
     * that is being actively controlled is alive.
     *
     * @param now_us the current monotonic time in microseconds.
     */
    void note_activity(std::uint64_t now_us) noexcept;

    /**
     * @brief Note that the dead-man switch was released.
     *
     * Enters the safe state immediately, regardless of the heartbeat
     * timeout.  A release while already in the safe state does nothing.
     *
     * The released state is **sticky**: an arbitrary mouse move must not
     * silently resume the remote session.  Only `note_deadman_pressed()`
     * (the operator pressing the switch again) or `reset()` clears it.
     */
    void note_deadman_released() noexcept;

    /**
     * @brief Note that the dead-man switch was pressed.
     *
     * Re-arms the session: a watchdog in the `kReleased` state returns to
     * `kAlive`.  This is the way out of the sticky released state.
     */
    void note_deadman_pressed() noexcept;

    /**
     * @brief Feed one inbound control event to the watchdog.
     *
     * The event is inspected: any event counts as activity (so the
     * heartbeat timeout does not fire while the operator is typing), and
     * a **release** of the dead-man button enters the safe state.
     *
     * This is the convenience entry point — the host does not have to
     * classify the events itself.
     *
     * @param event  the inbound event.
     * @param now_us the current monotonic time in microseconds.
     */
    void observe(const InputEvent& event, std::uint64_t now_us) noexcept;

    /**
     * @brief Check whether the heartbeat timeout has expired.
     *
     * @param now_us the current monotonic time in microseconds.
     * @return true when the state changed as a result of this call.
     */
    bool check_timeout(std::uint64_t now_us) noexcept;

    /** The current state. */
    DeadManState state() const noexcept;

    /** True when the session is considered alive. */
    bool alive() const noexcept;

    /**
     * @brief How long since the last activity, in microseconds.
     *
     * Returns 0 when there has never been any activity.
     */
    std::uint64_t silence_us(std::uint64_t now_us) const noexcept;

    /** The number of times the watchdog entered the safe state. */
    std::uint64_t safe_state_entries() const noexcept;

    /** The number of times the watchdog returned to `kAlive`. */
    std::uint64_t recoveries() const noexcept;

    /** The configuration this watchdog was built with. */
    const DeadManConfig& config() const noexcept;

    /**
     * @brief Reset the watchdog to its initial state.
     *
     * Clears the timer and the counters.  The safe-state callback is
     * **not** invoked — a reset is not a transition.
     */
    void reset() noexcept;

private:
    void enter_safe_state(DeadManState next) noexcept;
    void enter_alive() noexcept;

    DeadManConfig config_;
    SafeStateCallback on_safe_;
    RecoveryCallback on_recovery_;

    DeadManState state_ = DeadManState::kAlive;
    std::uint64_t last_activity_us_ = 0;
    bool has_activity_ = false;
    std::uint64_t safe_entries_ = 0;
    std::uint64_t recoveries_ = 0;
};

// ---------------------------------------------------------------------------
// HeartbeatEmitter
// ---------------------------------------------------------------------------

/**
 * @brief The operator side's liveness heartbeat.
 *
 * The emitter is a **pure counter**: the host asks it whether a beat is
 * due and, when it is, sends whatever beat it wants over the control
 * path.  The emitter does not own a timer and does not own a thread —
 * the host's control loop already runs at a fixed cadence, and a second
 * timer would be a second thing to get wrong.
 *
 * The point of the counter is that it is **testable without a clock**:
 * `due()` answers the question "should I beat now?" from a monotonic
 * timestamp the host supplies.
 */
class NIMRTC_API_STABLE HeartbeatEmitter {
public:
    /**
     * @brief Construct an emitter.
     *
     * @param interval_ms the beat interval.  Zero disables the heartbeat.
     */
    explicit HeartbeatEmitter(std::uint32_t interval_ms);

    /**
     * @brief Whether a beat is due.
     *
     * @param now_us the current monotonic time in microseconds.
     * @return true when the host should send a beat.
     */
    bool due(std::uint64_t now_us) noexcept;

    /**
     * @brief Record that a beat was sent.
     *
     * Must be called after the host actually sends the beat, otherwise
     * `due()` keeps returning true.
     */
    void on_sent(std::uint64_t now_us) noexcept;

    /** The beat interval in milliseconds. */
    std::uint32_t interval_ms() const noexcept;

    /** The number of beats emitted so far. */
    std::uint64_t beats_sent() const noexcept;

    /** True when the heartbeat is disabled. */
    bool disabled() const noexcept;

    /** Reset the emitter's state. */
    void reset() noexcept;

private:
    std::uint32_t interval_ms_;
    std::uint64_t last_beat_us_ = 0;
    bool has_beat_ = false;
    std::uint64_t beats_ = 0;
};

} // namespace nimrtc::rd_input
