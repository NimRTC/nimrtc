// modules/rd_session/src/session_guard.cpp
// ----------------------------------------------------------------------------
// The portable dispatch.
//
// The *portable* half of the module is the operator-facing wording and
// the platform-neutral contract; the *platform* half is the probe and
// the recovery.  The split is what lets the wording — the part an
// operator actually reads — be unit-tested on every CI leg, including
// the legs where the Windows backend cannot be compiled at all.
// ----------------------------------------------------------------------------

#include <nimrtc/rd_session/session_guard.hpp>

#ifdef _WIN32
#include "platform/windows/windows_session_guard.hpp"
#endif

namespace nimrtc::rd_session {

SessionGuardState query_session_guard_state() noexcept {
#ifdef _WIN32
    return detail::windows_query_state();
#else
    // The concept of a Winlogon-owned input desktop does not exist on
    // the other platforms, and neither does *this* failure mode: a
    // macOS or Linux session that is locked is still a session with an
    // interactive desktop that the screen-capture APIs can read (the
    // picture is the lock screen, which is a perfectly good remote
    // desktop to share).  So the honest answer there is "usable", and
    // the platform-specific problems stay where they belong — in the
    // capture backend's own error reporting.
    return SessionGuardState::kUsable;
#endif
}

std::string session_guard_state_hint(SessionGuardState state) noexcept {
    switch (state) {
        case SessionGuardState::kUsable:
            return "the desktop session is unlocked and ready to share";
        case SessionGuardState::kLocked:
            return "the workstation is LOCKED - the capture cannot work "
                   "until it is unlocked (press Win+L and sign in, or run "
                   "rd_device --recover-session from an elevated prompt)";
        case SessionGuardState::kUnknown:
            return "the desktop session state could not be determined "
                   "(no interactive session?); the capture may not produce "
                   "frames";
    }
    return "the desktop session state could not be determined";
}

bool recover_console_session(std::string* error) noexcept {
#ifdef _WIN32
    return detail::windows_recover_console_session(error);
#else
    if (error != nullptr) {
        *error = "the console-session recovery is a Windows-only operation";
    }
    return false;
#endif
}

} // namespace nimrtc::rd_session
