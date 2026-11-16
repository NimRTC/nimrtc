// modules/rd_session/src/platform/windows/windows_session_guard.hpp
// ----------------------------------------------------------------------------
// The Windows half of the session guard.
//
// Split out of `session_guard.cpp` so that the *portable* half (the
// dispatch and the operator-facing wording) is compiled — and unit
// tested — on every CI leg, including the legs where the Windows
// backend cannot be compiled at all.
// ----------------------------------------------------------------------------
#ifndef NIMRTC_RD_SESSION_WINDOWS_SESSION_GUARD_HPP
#define NIMRTC_RD_SESSION_WINDOWS_SESSION_GUARD_HPP

#include <cstdint>
#include <string>

#include <nimrtc/rd_session/session_guard.hpp>

namespace nimrtc::rd_session::detail {

/** The Windows session id of the calling process (0 when it cannot be
 *  determined). */
std::uint32_t current_session_id() noexcept;

/** Whether the Winlogon UI process is running *in this session*.
 *
 * This is the secondary signal: the primary one is the input
 * desktop's name, but `OpenInputDesktop()` fails with
 * `ERROR_ACCESS_DENIED` when the session is locked, so the failure has
 * to be *attributed* to something.  A Winlogon UI in our own session is
 * that something.
 */
bool winlogon_in_this_session() noexcept;

/** The name of the session's input desktop ("" when it cannot be read). */
std::string input_desktop_name() noexcept;

/** The Windows implementation of `query_session_guard_state()`. */
SessionGuardState windows_query_state() noexcept;

/** The Windows implementation of `recover_console_session()`. */
bool windows_recover_console_session(std::string* error) noexcept;

} // namespace nimrtc::rd_session::detail

#endif // NIMRTC_RD_SESSION_WINDOWS_SESSION_GUARD_HPP
