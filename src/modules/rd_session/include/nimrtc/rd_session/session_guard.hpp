/**
 * @file nimrtc/rd_session/session_guard.hpp
 * @brief The remote-desktop **session guard** — is the operator's machine
 *        usable as a remote-desktop *device* right now?
 *
 * ## The problem this solves
 *
 * A remote-desktop device shares the screen of a machine that a *person*
 * is sitting at.  On Windows that machine has a **lock screen**, and the
 * lock screen is not merely a picture over the desktop: it **replaces**
 * the interactive desktop.
 *
 * The consequences for the capture path are severe and, without this
 * module, silent:
 *
 *   - **DXGI Desktop Duplication** fails outright with
 *     `E_ACCESSDENIED` (`0x80070005`) — the duplication API cannot
 *     touch the Winlogon desktop;
 *   - **Windows Graphics Capture** does *not* fail: it opens happily
 *     and then delivers **exactly one frame**, and no more.  The
 *     operator sees a frozen picture and no error anywhere.
 *
 * The second case is the expensive one.  A frozen picture looks like a
 * broken encoder, a broken network, or a broken peer — and the
 * operator burns an hour bisecting the *wrong* layer.
 *
 * ## What this module does
 *
 * It answers one question, portably, and it answers it the same way on
 * every platform:
 *
 * > Is the interactive desktop session currently available to a screen
 * > capture?
 *
 * The answer is a three-state value, not a boolean, because the third
 * state matters:
 *
 * | state      | meaning                                                    |
 * |------------|------------------------------------------------------------|
 * | `kUsable`  | the desktop is available; a capture will produce frames   |
 * | `kLocked`  | the session is locked; a capture will fail or freeze      |
 * | `kUnknown` | the platform cannot tell (headless, a service session, …)  |
 *
 * `kUnknown` is a *first-class* answer, not a failure.  A capture on a
 * machine with no interactive session at all (a CI runner, a container)
 * is not a *locked* session — it is a machine that never had one.  The
 * two need different operator responses, so the two are different
 * values.
 *
 * ## How the detection works (Windows)
 *
 * The check is a single Win32 call: `OpenInputDesktop()`.  It opens the
 * **input desktop** of the calling session — the desktop the user
 * actually interacts with.
 *
 *   - The session is **unlocked**: the input desktop is the user's own
 *     (`Default`), the call succeeds, and its name reads back as
 *     `"Default"`.
 *   - The session is **locked**: the input desktop has been switched to
 *     the **Winlogon** one, and the call fails with
 *     `ERROR_ACCESS_DENIED` (`5`) because the Winlogon desktop denies
 *     access to an ordinary process.
 *
 * Both outcomes were verified against a real locked and a real
 * unlocked session on Windows 11 (§ "Verified" in the module's
 * `src/platform/windows/` backend).
 *
 * ## The recovery
 *
 * A locked session can be *unlocked* without a human: the console
 * session is disconnected and reconnected (`tsdiscon` / `tscon`), which
 * leaves the session logged in but **not locked**.  That is a
 * privileged operation, so it is exposed as an explicit, opt-in call
 * (`recover_console_session()`) rather than something the module does
 * behind the operator's back.
 *
 * ## Why a module and not a seam
 *
 * The same argument as the `rd_input_inject` module: this is a
 * **product** concern driven by the example application, not an engine
 * capability.  The engine does not resolve it through the registry and
 * never names it; the device-side example does.
 *
 * @see docs/guides/lan-test-runbook.md §"The locked-workstation trap"
 * @see src/modules/rd_input_inject/include/nimrtc/rd_input_inject/input_injector.hpp
 */

#ifndef NIMRTC_RD_SESSION_SESSION_GUARD_HPP
#define NIMRTC_RD_SESSION_SESSION_GUARD_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace nimrtc::rd_session {

// ---------------------------------------------------------------------------
// The state
// ---------------------------------------------------------------------------

/**
 * @brief Whether the interactive desktop session can be captured.
 *
 * The three values are distinguished by *what the operator should do*,
 * not by how the platform reports them:
 *
 *   - `kUsable`  — nothing to do; the capture will work.
 *   - `kLocked`  — **unlock the workstation**; a capture will fail
 *     (DXGI) or freeze after one frame (WGC).
 *   - `kUnknown` — the platform cannot tell; a capture may or may not
 *     work, and the operator has to look at the picture.
 */
enum class NIMRTC_API_STABLE SessionGuardState : std::uint8_t {
    /** The interactive desktop is available to a screen capture. */
    kUsable = 0,

    /** The session is locked — the Winlogon desktop owns the input. */
    kLocked = 1,

    /** The platform cannot tell (no interactive session, or the probe
     *  failed for a reason unrelated to a lock). */
    kUnknown = 2,
};

/** Human-readable state name (for logs and diagnostics). */
NIMRTC_API_STABLE inline std::string_view session_guard_state_name(
    SessionGuardState state) noexcept {
    switch (state) {
        case SessionGuardState::kUsable:  return "usable";
        case SessionGuardState::kLocked:  return "locked";
        case SessionGuardState::kUnknown: return "unknown";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// The probe
// ---------------------------------------------------------------------------

/**
 * @brief Query whether the interactive desktop session can be captured.
 *
 * @return the state; see `SessionGuardState`.
 *
 * ## Threading
 *
 * Safe to call from any thread.  The Windows implementation makes a
 * handful of Win32 calls and allocates nothing; it takes no locks.
 *
 * ## Cost
 *
 * One syscall on Windows (a desktop handle open plus a name read-back).
 * It is cheap enough to call on a timer — the device-side example calls
 * it once at start-up and then periodically, because a session can be
 * locked *while* a session is running (the idle timeout, or a human
 * pressing `Win+L`).
 */
NIMRTC_API_STABLE SessionGuardState query_session_guard_state() noexcept;

/**
 * @brief A one-line explanation of the state, aimed at the operator.
 *
 * The string names the *remedy*, not just the fact: a log line that
 * says "locked" saves the reader nothing, while one that says "locked —
 * press Win+L and sign in, or run `rd_device --recover-session`" tells
 * them exactly what to do.
 *
 * @param state the state to explain.
 * @return a human-readable sentence (never empty).
 */
NIMRTC_API_STABLE std::string session_guard_state_hint(
    SessionGuardState state) noexcept;

// ---------------------------------------------------------------------------
// The recovery
// ---------------------------------------------------------------------------

/**
 * @brief Try to bring a locked console session back to an unlocked one.
 *
 * On Windows the console session is disconnected and immediately
 * reconnected, which leaves it **logged in but not locked** — the
 * running programs survive, and the input desktop becomes the user's
 * own again.  This is the same operation the `tsdiscon` / `tscon` pair
 * performs, done in-process.
 *
 * @param[out] error  the reason the recovery failed (empty on success).
 * @return `true` when the session is usable again.
 *
 * ## Privileges
 *
 * The operation is **privileged** on Windows (it manipulates the
 * terminal-services session state), so it fails on an unelevated
 * process.  The failure is reported through `error`, never as a silent
 * no-op.
 *
 * ## Non-Windows
 *
 * Every other platform reports `false` with an explanatory `error`: the
 * concept of a Winlogon-owned input desktop does not exist there, and
 * the platforms that have a screen lock do not have *this* failure
 * mode.
 */
NIMRTC_API_STABLE bool recover_console_session(std::string* error) noexcept;

} // namespace nimrtc::rd_session

#endif // NIMRTC_RD_SESSION_SESSION_GUARD_HPP
