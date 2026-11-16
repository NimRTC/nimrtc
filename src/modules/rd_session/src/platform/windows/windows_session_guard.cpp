// modules/rd_session/src/platform/windows/windows_session_guard.cpp
// ----------------------------------------------------------------------------
// The Windows session guard.
//
// ## The detection
//
// One Win32 call carries the whole answer: `OpenInputDesktop()`.
//
// The *input desktop* is the desktop the user interacts with.  When the
// session is unlocked it is the user's own (`Default`); when the
// session is locked, Winlogon switches it to the Winlogon desktop,
// which denies `ERROR_ACCESS_DENIED` to an ordinary process.
//
// Verified against a real Windows 11 session:
//
//   unlocked: OpenInputDesktop(DESKTOP_READOBJECTS) succeeds and the
//             desktop name reads back as "Default";
//   locked:   OpenInputDesktop() fails with error 5
//             (ERROR_ACCESS_DENIED) and the Winlogon UI process is
//             running in our session.
//
// The `ERROR_ACCESS_DENIED` outcome is *not* on its own proof of a
// lock: the same error comes from a service session (session 0) and
// from a process whose window station is not `WinSta0`.  So the
// failure is attributed to a lock only when the Winlogon UI is
// running in our own session; otherwise the answer is `kUnknown`,
// which is the honest one.
//
// ## The recovery
//
// The console session is disconnected and reconnected.  The two
// Win32 calls that do it are `WTSDisconnectSession()` and
// `WTSConnectSession()`, and there is a catch: **the disconnect kills
// the calling process** when the caller lives in the session it
// disconnects — which is exactly the case here (the device runs in the
// console session it would disconnect).  A P/Invoke of
// `WTSDisconnectSession()` on *your own* session terminates the caller
// before the call returns; that was observed directly.
//
// So the recovery is performed by a **helper process**: the device
// spawns `tsdiscon.exe` / `tscon.exe`, which run in a session of
// their own and therefore survive.  `CreateProcess()` is used rather
// than `system()` so that the helper's console window never flashes
// up on the operator's screen.
//
// The `tsdiscon` / `tscon` pair is the operation Microsoft documents
// for exactly this ("to unlock a console session that was locked by
// the screen saver"), and it is what the runbook already instructs an
// operator to run by hand.
// ----------------------------------------------------------------------------

#include "platform/windows/windows_session_guard.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// `CreateToolhelp32Snapshot` / `Process32FirstW` / `Process32NextW` live
// here, not in <windows.h>.
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace nimrtc::rd_session::detail {

namespace {

/** The Winlogon UI's executable name.  Its presence in *our* session is
 *  the tell-tale of a locked workstation. */
constexpr wchar_t kLogonUi[] = L"LogonUI.exe";

/** Read the Winlogon UI's session id out of a `PROCESSENTRY32` walk. */
std::uint32_t process_session_id(HANDLE process) {
    // `ProcessIdToSessionId` is the direct route and needs no snapshot.
    // The out-parameter is a `DWORD*`, which is *not* the same type as
    // `std::uint32_t*` on Windows (`DWORD` is `unsigned long`), so the
    // value is read out through a `DWORD` and then narrowed.
    DWORD session = 0;
    if (!::ProcessIdToSessionId(::GetProcessId(process), &session)) return 0;
    return static_cast<std::uint32_t>(session);
}

/**
 * @brief Find the Winlogon UI process running in the given session.
 * @return the process handle, or `nullptr`.
 *
 * The handle is opened with `PROCESS_QUERY_LIMITED_INFORMATION` and is
 * *not* closed when the process is found — the caller closes it.
 */
HANDLE find_logonui_process(std::uint32_t session_id) {
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return nullptr;

    HANDLE found = nullptr;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, kLogonUi) != 0) continue;
            HANDLE process = ::OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (process == nullptr) continue;
            if (process_session_id(process) == session_id) {
                found = process;
                break;
            }
            ::CloseHandle(process);
        } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return found;
}

/**
 * @brief Run a console tool and wait for it.
 * @param command the full command line (without the executable path).
 * @param exit_code the tool's exit code.
 * @return `true` when the tool was started and waited for.
 */
bool run_console_tool(const wchar_t* executable, const wchar_t* command,
                      DWORD* exit_code) {
    std::wstring line;
    line.reserve(64);
    line += L"\"";
    line += executable;
    line += L"\" ";
    line += command;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    // CREATE_NO_WINDOW: the helper must not flash a console window on
    // the operator's screen — the whole point is that the recovery is
    // invisible.
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> mutable_line(line.begin(), line.end());
    mutable_line.push_back(L'\0');

    if (!::CreateProcessW(
            nullptr, mutable_line.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    ::WaitForSingleObject(pi.hProcess, 30'000);
    if (exit_code != nullptr) {
        DWORD code = 1;
        ::GetExitCodeProcess(pi.hProcess, &code);
        *exit_code = code;
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

std::uint32_t current_session_id() noexcept {
    DWORD session = 0;
    if (!::ProcessIdToSessionId(::GetCurrentProcessId(), &session)) return 0;
    return static_cast<std::uint32_t>(session);
}

bool winlogon_in_this_session() noexcept {
    const std::uint32_t session = current_session_id();
    if (session == 0) return false;
    HANDLE process = find_logonui_process(session);
    if (process == nullptr) return false;
    ::CloseHandle(process);
    return true;
}

std::string input_desktop_name() noexcept {
    HDESK desktop = ::OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (desktop == nullptr) return {};

    std::wstring name(64, L'\0');
    DWORD needed = 0;
    BOOL ok = ::GetUserObjectInformationW(
        desktop, UOI_NAME, name.data(),
        static_cast<DWORD>(name.size() * sizeof(wchar_t)), &needed);
    ::CloseDesktop(desktop);
    if (!ok) return {};

    // Trim at the first NUL (GetUserObjectInformationW does not
    // guarantee one when the buffer was big enough).
    const std::size_t nul = name.find(L'\0');
    if (nul != std::wstring::npos) name.resize(nul);
    if (name.empty()) return {};

    // Narrow to UTF-8 (the names are ASCII in practice — "Default" and
    // "Winlogon" — but the conversion must not assume that).
    const int needed_bytes = ::WideCharToMultiByte(
        CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), nullptr, 0,
        nullptr, nullptr);
    if (needed_bytes <= 0) return {};
    std::string out(static_cast<std::size_t>(needed_bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, name.c_str(),
                          static_cast<int>(name.size()), out.data(),
                          needed_bytes, nullptr, nullptr);
    return out;
}

SessionGuardState windows_query_state() noexcept {
    // The primary signal: the input desktop's name.
    //
    // When the session is unlocked the call succeeds and the name is
    // "Default".  The name check is deliberately *not* the whole
    // answer: a name of "Winlogon" would mean a lock, but the Winlogon
    // desktop denies access, so the read-back below never sees it.
    const std::string desktop = input_desktop_name();
    if (!desktop.empty()) {
        // The desktop is open to us, so the session is interactive.
        // The *name* is deliberately not checked: "Default" is the
        // ordinary one, but anything else we can read is still an
        // unlocked, interactive desktop (a custom desktop, a
        // UAC-shared desktop in some configurations).  The only thing
        // that matters here is that it opened.
        return SessionGuardState::kUsable;
    }

    // The input desktop could not be opened.  The usual reason is that
    // the session is locked and Winlogon owns the input; the Winlogon
    // UI being present *in our own session* is what makes that reading
    // trustworthy.
    if (winlogon_in_this_session()) return SessionGuardState::kLocked;

    // No Winlogon UI: the failure is something else (session 0, a
    // service window station, a container).  The honest answer is
    // "unknown", not "locked".
    return SessionGuardState::kUnknown;
}

bool windows_recover_console_session(std::string* error) noexcept {
    const std::uint32_t session = current_session_id();
    if (session == 0) {
        if (error != nullptr) {
            *error = "the session id could not be determined "
                     "(ProcessIdToSessionId failed)";
        }
        return false;
    }

    // Only a *locked* session needs the recovery; an already-usable
    // one is left alone (the disconnect would drop the operator's
    // session for nothing).
    if (windows_query_state() != SessionGuardState::kLocked) {
        if (error != nullptr) {
            *error = "the session is not locked, so it was left alone";
        }
        return false;
    }

    // The disconnect must run in a helper process: `WTSDisconnectSession()`
    // on the caller's *own* session terminates the caller before the
    // call returns (verified).  `tsdiscon.exe` / `tscon.exe` run in a
    // session of their own and therefore survive.
    std::wstring session_arg = std::to_wstring(session);

    DWORD exit_code = 1;
    if (!run_console_tool(L"tsdiscon.exe", session_arg.c_str(), &exit_code)) {
        if (error != nullptr) {
            *error = "tsdiscon.exe could not be started "
                     "(the terminal-services tools are missing or the "
                     "process is not privileged)";
        }
        return false;
    }
    if (exit_code != 0) {
        if (error != nullptr) {
            *error = "tsdiscon.exe failed with exit code " +
                     std::to_string(exit_code) +
                     " (the recovery needs an elevated process)";
        }
        return false;
    }

    // Give the session a moment to settle before reconnecting it —
    // `tscon` fails with "the session is being disconnected" if it
    // runs while the disconnect is still in flight.
    ::Sleep(2'000);

    const wchar_t* connect = L"tscon.exe";
    std::wstring connect_arg = session_arg + L" /dest:console";
    if (!run_console_tool(connect, connect_arg.c_str(), &exit_code)) {
        if (error != nullptr) {
            *error = "tscon.exe could not be started "
                     "(the terminal-services tools are missing or the "
                     "process is not privileged)";
        }
        return false;
    }
    if (exit_code != 0) {
        if (error != nullptr) {
            *error = "tscon.exe failed with exit code " +
                     std::to_string(exit_code) +
                     " (the recovery needs an elevated process)";
        }
        return false;
    }

    // The reconnect is asynchronous as far as the desktop switch is
    // concerned, so the state is re-read after a short settle.
    ::Sleep(2'000);
    if (windows_query_state() == SessionGuardState::kLocked) {
        if (error != nullptr) {
            *error = "the session was reconnected but is still locked";
        }
        return false;
    }
    return true;
}

} // namespace nimrtc::rd_session::detail
