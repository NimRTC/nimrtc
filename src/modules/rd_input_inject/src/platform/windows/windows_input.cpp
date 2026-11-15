/**
 * @file src/modules/rd_input_inject/src/platform/windows/windows_input.cpp
 * @brief The Windows input-injection backend (`SendInput`).
 */

#ifdef _WIN32

#include "windows_input.hpp"

#include <nimrtc/core/log.hpp>

#include <array>

namespace nimrtc::rd_input_inject::detail {
namespace {

// ---------------------------------------------------------------------------
// The virtual-key table
// ---------------------------------------------------------------------------
//
// The Windows virtual-key codes.  The neutral layout's codes are the
// vocabulary's, and the two numbering schemes are unrelated — which is
// exactly why the table exists.
//
// A key that is not in the table is *refused*: an injected key with a
// wrong virtual-key number types a character the operator never
// pressed.
WORD vk_for_neutral(NeutralKey key) noexcept {
    switch (key) {
        case NeutralKey::kEscape:    return VK_ESCAPE;
        case NeutralKey::kTab:       return VK_TAB;
        case NeutralKey::kEnter:     return VK_RETURN;
        case NeutralKey::kSpace:     return VK_SPACE;
        case NeutralKey::kBackspace: return VK_BACK;
        case NeutralKey::kDelete:    return VK_DELETE;
        case NeutralKey::kHome:      return VK_HOME;
        case NeutralKey::kEnd:       return VK_END;
        case NeutralKey::kPageUp:    return VK_PRIOR;
        case NeutralKey::kPageDown:  return VK_NEXT;
        case NeutralKey::kLeft:      return VK_LEFT;
        case NeutralKey::kUp:        return VK_UP;
        case NeutralKey::kRight:     return VK_RIGHT;
        case NeutralKey::kDown:      return VK_DOWN;
        case NeutralKey::kShift:     return VK_SHIFT;
        case NeutralKey::kControl:   return VK_CONTROL;
        case NeutralKey::kAlt:       return VK_MENU;
        case NeutralKey::kMeta:      return VK_LWIN;

        case NeutralKey::kF1:  return VK_F1;
        case NeutralKey::kF2:  return VK_F2;
        case NeutralKey::kF3:  return VK_F3;
        case NeutralKey::kF4:  return VK_F4;
        case NeutralKey::kF5:  return VK_F5;
        case NeutralKey::kF6:  return VK_F6;
        case NeutralKey::kF7:  return VK_F7;
        case NeutralKey::kF8:  return VK_F8;
        case NeutralKey::kF9:  return VK_F9;
        case NeutralKey::kF10: return VK_F10;
        case NeutralKey::kF11: return VK_F11;
        case NeutralKey::kF12: return VK_F12;

        case NeutralKey::kA: return 'A';
        case NeutralKey::kB: return 'B';
        case NeutralKey::kC: return 'C';
        case NeutralKey::kD: return 'D';
        case NeutralKey::kE: return 'E';
        case NeutralKey::kF: return 'F';
        case NeutralKey::kG: return 'G';
        case NeutralKey::kH: return 'H';
        case NeutralKey::kI: return 'I';
        case NeutralKey::kJ: return 'J';
        case NeutralKey::kK: return 'K';
        case NeutralKey::kL: return 'L';
        case NeutralKey::kM: return 'M';
        case NeutralKey::kN: return 'N';
        case NeutralKey::kO: return 'O';
        case NeutralKey::kP: return 'P';
        case NeutralKey::kQ: return 'Q';
        case NeutralKey::kR: return 'R';
        case NeutralKey::kS: return 'S';
        case NeutralKey::kT: return 'T';
        case NeutralKey::kU: return 'U';
        case NeutralKey::kV: return 'V';
        case NeutralKey::kW: return 'W';
        case NeutralKey::kX: return 'X';
        case NeutralKey::kY: return 'Y';
        case NeutralKey::kZ: return 'Z';

        case NeutralKey::kNone: break;
    }
    return 0;
}

// The keys Windows distinguishes as "extended": they live on the
// numeric keypad or on the right-hand side of the keyboard, and the
// flag is what tells the keyboard driver which one is meant.
bool vk_is_extended(WORD vk) noexcept {
    switch (vk) {
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_UP:
        case VK_RIGHT:
        case VK_DOWN:
        case VK_NUMLOCK:
        case VK_DIVIDE:
        case VK_RCONTROL:
        case VK_RMENU:
            return true;
        default:
            return false;
    }
}

// The mouse button -> the Windows button flag.
DWORD mouse_button_flag(rd_input::MouseButton button) noexcept {
    switch (button) {
        case rd_input::MouseButton::kLeft:   return MOUSEEVENTF_LEFTDOWN;
        case rd_input::MouseButton::kRight:  return MOUSEEVENTF_RIGHTDOWN;
        case rd_input::MouseButton::kMiddle: return MOUSEEVENTF_MIDDLEDOWN;
        case rd_input::MouseButton::kBack:   return MOUSEEVENTF_XDOWN;
        case rd_input::MouseButton::kForward:return MOUSEEVENTF_XDOWN;
        case rd_input::MouseButton::kUnknown:break;
    }
    return 0;
}

/** The XBUTTON id for the extended buttons. */
DWORD mouse_xbutton(rd_input::MouseButton button) noexcept {
    switch (button) {
        case rd_input::MouseButton::kBack:    return XBUTTON1;
        case rd_input::MouseButton::kForward: return XBUTTON2;
        default: break;
    }
    return 0;
}

/** Build a keyboard `INPUT`. */
INPUT make_key_input(WORD vk, bool down, bool extended) noexcept {
    INPUT input{};
    input.type           = INPUT_KEYBOARD;
    input.ki.wVk         = vk;
    input.ki.wScan       = 0;
    input.ki.dwFlags     = down ? 0u : KEYEVENTF_KEYUP;
    if (extended) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    return input;
}

} // namespace

WORD windows_vk_for(NeutralKey key) noexcept {
    return vk_for_neutral(key);
}

WORD windows_vk_for_modifier(NeutralKey key) noexcept {
    switch (key) {
        case NeutralKey::kShift:   return VK_SHIFT;
        case NeutralKey::kControl: return VK_CONTROL;
        case NeutralKey::kAlt:     return VK_MENU;
        case NeutralKey::kMeta:    return VK_LWIN;
        default: break;
    }
    return 0;
}

bool windows_vk_is_extended(WORD vk) noexcept {
    return vk_is_extended(vk);
}

// ---------------------------------------------------------------------------
// The backend
// ---------------------------------------------------------------------------

WindowsInjector::WindowsInjector() {
    // The availability probe.  `SendInput` itself always *exists* on a
    // Windows host; what can be missing is the ability to inject:
    //
    //   - a service running in session 0 cannot inject into the
    //     interactive session (and that is a security boundary, not a
    //     bug), and
    //   - a process running as a *service* has no window station that
    //     the desktop is on.
    //
    // The probe is therefore about the *session*, not about the API.
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) {
        unavailable_reason_ = "the process session could not be determined";
        return;
    }
    if (session == 0) {
        unavailable_reason_ =
            "the process runs in session 0 (the service session); it cannot "
            "inject into an interactive desktop";
        return;
    }
    // A desktop that is not the "default" one is the Winlogon / secure
    // desktop, which is exactly where injection must not go.
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (desktop == nullptr) {
        unavailable_reason_ = "the input desktop could not be opened";
        return;
    }
    CloseDesktop(desktop);
}

WindowsInjector::~WindowsInjector() = default;

bool WindowsInjector::available() const noexcept {
    return unavailable_reason_.empty();
}

std::string WindowsInjector::unavailable_reason() const noexcept {
    return unavailable_reason_;
}

bool WindowsInjector::send_input(const INPUT& input) noexcept {
    // `SendInput` takes a non-const `LPINPUT`, even though it only
    // reads the structures.  The const_cast is confined to this one
    // function, so the rest of the backend keeps the const-correct
    // signature.
    const UINT sent = ::SendInput(1, const_cast<LPINPUT>(&input),
                                  sizeof(INPUT));
    if (sent == 1) return true;

    // The error tells the host *why* the OS said no.  The most common
    // cause is UIPI (a higher-integrity window), which is a security
    // boundary and not something to work around.
    const DWORD err = GetLastError();
    nimrtc::core::log::Logger::instance().warn(
        "rd_input_inject: SendInput failed (GetLastError=" +
        std::to_string(err) + ")");
    return false;
}

bool WindowsInjector::do_inject_mouse_move(std::int32_t dx, std::int32_t dy) {
    INPUT input{};
    input.type           = INPUT_MOUSE;
    input.mi.dx          = dx;
    input.mi.dy          = dy;
    input.mi.mouseData   = 0;
    input.mi.dwFlags     = MOUSEEVENTF_MOVE;
    return send_input(input);
}

bool WindowsInjector::do_inject_button(rd_input::MouseButton button,
                                       rd_input::ButtonAction action) {
    const DWORD flag = mouse_button_flag(button);
    if (flag == 0) return false;

    INPUT input{};
    input.type       = INPUT_MOUSE;
    input.mi.dwFlags = flag;
    if (action == rd_input::ButtonAction::kUp) {
        input.mi.dwFlags = (flag == MOUSEEVENTF_LEFTDOWN)   ? MOUSEEVENTF_LEFTUP
                         : (flag == MOUSEEVENTF_RIGHTDOWN)  ? MOUSEEVENTF_RIGHTUP
                         : (flag == MOUSEEVENTF_MIDDLEDOWN) ? MOUSEEVENTF_MIDDLEUP
                                                            : MOUSEEVENTF_XUP;
    }
    if (flag == MOUSEEVENTF_XDOWN) {
        input.mi.mouseData = mouse_xbutton(button);
    }
    return send_input(input);
}

bool WindowsInjector::do_inject_wheel(std::int32_t dx, std::int32_t dy) {
    // The wheel is expressed in *notches*: `WHEEL_DELTA` is 120.
    INPUT input{};
    input.type       = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(dy * WHEEL_DELTA);
    if (!send_input(input)) return false;

    // A horizontal scroll is a separate call with the other flag.
    if (dx != 0) {
        INPUT h{};
        h.type       = INPUT_MOUSE;
        h.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        h.mi.mouseData = static_cast<DWORD>(dx * WHEEL_DELTA);
        return send_input(h);
    }
    return true;
}

bool WindowsInjector::do_inject_key(NeutralKey key,
                                    rd_input::KeyAction action) {
    // The Windows key is refused: see `InjectorBase::modifier_keys`.  A
    // remote-desktop agent that can press the Windows key can lock the
    // operator out of the machine they are driving.
    if (key == NeutralKey::kMeta) {
        nimrtc::core::log::Logger::instance().warn(
            "rd_input_inject: the Windows / Meta key is refused by the "
            "injector (it would open the shell)");
        return false;
    }

    const WORD vk = vk_for_neutral(key);
    if (vk == 0) return false;  // the table does not know the key

    const bool down = (action == rd_input::KeyAction::kDown);
    const INPUT input = make_key_input(vk, down, vk_is_extended(vk));
    return send_input(input);
}

bool WindowsInjector::do_inject_text(const std::string& text) {
    // The text is injected with `KEYEVENTF_UNICODE`, which types the
    // characters exactly as they are — no keyboard layout involved, no
    // dead keys, no AltGr puzzles.  That is the whole reason the
    // vocabulary has a *text* event kind.
    bool ok = true;
    for (const char ch : text) {
        INPUT input{};
        input.type       = INPUT_KEYBOARD;
        input.ki.wVk     = 0;
        input.ki.wScan   = static_cast<WORD>(static_cast<unsigned char>(ch));
        input.ki.dwFlags = KEYEVENTF_UNICODE;
        if (!send_input(input)) ok = false;

        INPUT up = input;
        up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        if (!send_input(up)) ok = false;
    }
    return ok;
}

} // namespace nimrtc::rd_input_inject::detail

#endif // _WIN32
