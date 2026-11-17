/**
 * @file src/modules/rd_input_inject/src/platform/linux/linux_input.cpp
 * @brief The Linux input-injection backend (`XTest` / uinput).
 */

#if defined(__linux__)

#include "linux_input.hpp"

#include <nimrtc/core/log.hpp>

#include <cstdlib>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#ifdef NIMRTC_HAS_XTEST
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#endif

#ifdef NIMRTC_HAS_UINPUT
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#endif

namespace nimrtc::rd_input_inject::detail {
namespace {

// The Linux input-layer key codes (`linux/input-event-codes.h`).
int keycode_for_neutral(NeutralKey key) noexcept {
    switch (key) {
        case NeutralKey::kEscape:    return KEY_ESC;
        case NeutralKey::kTab:       return KEY_TAB;
        case NeutralKey::kEnter:     return KEY_ENTER;
        case NeutralKey::kSpace:     return KEY_SPACE;
        case NeutralKey::kBackspace: return KEY_BACKSPACE;
        case NeutralKey::kDelete:    return KEY_DELETE;
        case NeutralKey::kHome:      return KEY_HOME;
        case NeutralKey::kEnd:       return KEY_END;
        case NeutralKey::kPageUp:    return KEY_PAGEUP;
        case NeutralKey::kPageDown:  return KEY_PAGEDOWN;
        case NeutralKey::kLeft:      return KEY_LEFT;
        case NeutralKey::kUp:        return KEY_UP;
        case NeutralKey::kRight:     return KEY_RIGHT;
        case NeutralKey::kDown:      return KEY_DOWN;
        case NeutralKey::kShift:     return KEY_LEFTSHIFT;
        case NeutralKey::kControl:   return KEY_LEFTCTRL;
        case NeutralKey::kAlt:       return KEY_LEFTALT;
        case NeutralKey::kMeta:      return KEY_LEFTMETA;

        case NeutralKey::kF1:  return KEY_F1;
        case NeutralKey::kF2:  return KEY_F2;
        case NeutralKey::kF3:  return KEY_F3;
        case NeutralKey::kF4:  return KEY_F4;
        case NeutralKey::kF5:  return KEY_F5;
        case NeutralKey::kF6:  return KEY_F6;
        case NeutralKey::kF7:  return KEY_F7;
        case NeutralKey::kF8:  return KEY_F8;
        case NeutralKey::kF9:  return KEY_F9;
        case NeutralKey::kF10: return KEY_F10;
        case NeutralKey::kF11: return KEY_F11;
        case NeutralKey::kF12: return KEY_F12;

        case NeutralKey::kA: return KEY_A;
        case NeutralKey::kB: return KEY_B;
        case NeutralKey::kC: return KEY_C;
        case NeutralKey::kD: return KEY_D;
        case NeutralKey::kE: return KEY_E;
        case NeutralKey::kF: return KEY_F;
        case NeutralKey::kG: return KEY_G;
        case NeutralKey::kH: return KEY_H;
        case NeutralKey::kI: return KEY_I;
        case NeutralKey::kJ: return KEY_J;
        case NeutralKey::kK: return KEY_K;
        case NeutralKey::kL: return KEY_L;
        case NeutralKey::kM: return KEY_M;
        case NeutralKey::kN: return KEY_N;
        case NeutralKey::kO: return KEY_O;
        case NeutralKey::kP: return KEY_P;
        case NeutralKey::kQ: return KEY_Q;
        case NeutralKey::kR: return KEY_R;
        case NeutralKey::kS: return KEY_S;
        case NeutralKey::kT: return KEY_T;
        case NeutralKey::kU: return KEY_U;
        case NeutralKey::kV: return KEY_V;
        case NeutralKey::kW: return KEY_W;
        case NeutralKey::kX: return KEY_X;
        case NeutralKey::kY: return KEY_Y;
        case NeutralKey::kZ: return KEY_Z;

        case NeutralKey::kNone: break;
    }
    return 0;
}

int button_code_for(rd_input::MouseButton button) noexcept {
    switch (button) {
        case rd_input::MouseButton::kLeft:   return BTN_LEFT;
        case rd_input::MouseButton::kRight:  return BTN_RIGHT;
        case rd_input::MouseButton::kMiddle: return BTN_MIDDLE;
        case rd_input::MouseButton::kBack:   return BTN_SIDE;
        case rd_input::MouseButton::kForward:return BTN_EXTRA;
        case rd_input::MouseButton::kUnknown:break;
    }
    return 0;
}

} // namespace

int linux_keycode_for(NeutralKey key) noexcept {
    return keycode_for_neutral(key);
}

int linux_button_code_for(rd_input::MouseButton button) noexcept {
    return button_code_for(button);
}

// ---------------------------------------------------------------------------
// The backend
// ---------------------------------------------------------------------------

LinuxInjector::LinuxInjector() {
    // ---- X11 / XTest ------------------------------------------------------
#ifdef NIMRTC_HAS_XTEST
    {
        const char* display = std::getenv("DISPLAY");
        if (display != nullptr && display[0] != '\0') {
            Display* dpy = XOpenDisplay(display);
            if (dpy != nullptr) {
                if (XTestQueryExtension(dpy, nullptr, nullptr, nullptr, nullptr)) {
                    mode_ = Mode::kXTest;
                } else {
                    XCloseDisplay(dpy);
                    unavailable_reason_ =
                        "the X server is reachable but the XTEST extension is "
                        "not available";
                }
            } else {
                unavailable_reason_ =
                    "the X display could not be opened (is DISPLAY set, and "
                    "is the Xauthority cookie readable?)";
            }
        }
    }
#endif

    // ---- uinput -----------------------------------------------------------
    //
    // The uinput path is the fallback: it works on Wayland (where XTest
    // does not exist at all) and on an X11 session that lacks XTEST.
    if (mode_ == Mode::kNone) {
        if (open_uinput()) {
            mode_ = Mode::kUinput;
            unavailable_reason_.clear();
        } else if (unavailable_reason_.empty()) {
            unavailable_reason_ =
                "neither XTEST nor a writable /dev/uinput is available";
        }
    }
}

LinuxInjector::~LinuxInjector() {
#ifdef NIMRTC_HAS_XTEST
    // The display is opened per injection (an X display connection is
    // not thread-safe and the injector is driven from one thread), so
    // there is nothing to close here beyond the uinput fd.
#endif
    if (uinput_fd_ >= 0) {
        ::close(uinput_fd_);
        uinput_fd_ = -1;
    }
}

bool LinuxInjector::open_uinput() noexcept {
#ifdef NIMRTC_HAS_UINPUT
    const int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        if (unavailable_reason_.empty()) {
            unavailable_reason_ =
                "/dev/uinput could not be opened (it usually needs root, or "
                "membership of the 'input' group)";
        }
        return false;
    }

    // Enable the events the injector writes.  The device has to
    // advertise them, or the kernel drops them.
    if (::ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ::ioctl(fd, UI_SET_EVBIT, EV_REL) < 0 ||
        ::ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) {
        ::close(fd);
        if (unavailable_reason_.empty()) {
            unavailable_reason_ = "/dev/uinput rejected the event-type setup";
        }
        return false;
    }

    for (const NeutralKey key : {NeutralKey::kEscape, NeutralKey::kTab,
                                 NeutralKey::kEnter, NeutralKey::kSpace,
                                 NeutralKey::kA, NeutralKey::kZ}) {
        const int code = keycode_for_neutral(key);
        if (code != 0) ::ioctl(fd, UI_SET_KEYBIT, code);
    }
    for (const int code : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}) {
        ::ioctl(fd, UI_SET_KEYBIT, code);
    }

    uinput_fd_ = fd;
    return true;
#else
    if (unavailable_reason_.empty()) {
        unavailable_reason_ = "the uinput backend was not built";
    }
    return false;
#endif
}

std::string_view LinuxInjector::backend_name() const noexcept {
    switch (mode_) {
        case Mode::kXTest:  return "linux-xtest";
        case Mode::kUinput: return "linux-uinput";
        case Mode::kNone:   break;
    }
    return "linux-none";
}

bool LinuxInjector::available() const noexcept {
    return mode_ != Mode::kNone;
}

std::string LinuxInjector::unavailable_reason() const noexcept {
    return unavailable_reason_;
}

bool LinuxInjector::write_event(int type, int code, int value) noexcept {
#ifdef NIMRTC_HAS_UINPUT
    if (uinput_fd_ < 0) return false;

    struct input_event ev {};
    ev.type  = static_cast<__u16>(type);
    ev.code  = static_cast<__u16>(code);
    ev.value = static_cast<__s32>(value);

    const ssize_t written = ::write(uinput_fd_, &ev, sizeof(ev));
    if (written != static_cast<ssize_t>(sizeof(ev))) return false;

    // The synchronisation event tells the kernel that the report is
    // complete.  Without it the events are held back.
    struct input_event syn {};
    syn.type  = EV_SYN;
    syn.code  = SYN_REPORT;
    syn.value = 0;
    const ssize_t syn_written = ::write(uinput_fd_, &syn, sizeof(syn));
    return syn_written == static_cast<ssize_t>(sizeof(syn));
#else
    (void)type; (void)code; (void)value;
    return false;
#endif
}

bool LinuxInjector::do_inject_mouse_move(std::int32_t dx, std::int32_t dy) {
#ifdef NIMRTC_HAS_XTEST
    if (mode_ == Mode::kXTest) {
        Display* dpy = XOpenDisplay(nullptr);
        if (dpy == nullptr) return false;
        // X11's relative motion uses the *screen* convention (y grows
        // downwards), which is the same convention the operator's mouse
        // uses -- so no sign flip is needed here.
        //
        // The fourth argument is the event *delay* (an `unsigned long`
        // timestamp), not a pointer: `CurrentTime` means "apply it
        // immediately", which is what the rest of this file passes to
        // the other XTest calls.
        XTestFakeRelativeMotionEvent(dpy, static_cast<int>(dx),
                                     static_cast<int>(dy), CurrentTime);
        XFlush(dpy);
        XCloseDisplay(dpy);
        return true;
    }
#endif
#ifdef NIMRTC_HAS_UINPUT
    if (mode_ == Mode::kUinput) {
        // The kernel's relative motion is the *negation* of the
        // operator's: the operator moves the mouse right, the pointer
        // moves right, and the kernel's `REL_X` counts positive to the
        // right too — but the X11 convention is the opposite (y grows
        // downwards, x grows rightwards, while XTest's y grows
        // *upwards*).  The X11 path negates for that reason; the
        // kernel path does not.
        if (!write_event(EV_REL, REL_X, dx)) return false;
        if (dy != 0) {
            if (!write_event(EV_REL, REL_Y, dy)) return false;
        }
        return true;
    }
#endif
    return false;
}

bool LinuxInjector::do_inject_button(rd_input::MouseButton button,
                                     rd_input::ButtonAction action) {
    const int code = button_code_for(button);
    if (code == 0) return false;

    const int value = (action == rd_input::ButtonAction::kDown) ? 1 : 0;

#ifdef NIMRTC_HAS_XTEST
    if (mode_ == Mode::kXTest) {
        Display* dpy = XOpenDisplay(nullptr);
        if (dpy == nullptr) return false;
        if (value == 1) {
            XTestFakeButtonEvent(dpy, static_cast<unsigned int>(code),
                                 True, CurrentTime);
        } else {
            XTestFakeButtonEvent(dpy, static_cast<unsigned int>(code),
                                 False, CurrentTime);
        }
        XFlush(dpy);
        XCloseDisplay(dpy);
        return true;
    }
#endif
#ifdef NIMRTC_HAS_UINPUT
    if (mode_ == Mode::kUinput) {
        return write_event(EV_KEY, code, value);
    }
#endif
    return false;
}

bool LinuxInjector::do_inject_wheel(std::int32_t dx, std::int32_t dy) {
#ifdef NIMRTC_HAS_XTEST
    if (mode_ == Mode::kXTest) {
        Display* dpy = XOpenDisplay(nullptr);
        if (dpy == nullptr) return false;
        // The X11 wheel axis is vertical-first and counts *up* as
        // positive, while the operator's wheel counts *up* as negative
        // — the two conventions are opposite.
        if (dy != 0) {
            XTestFakeButtonEvent(dpy, 4, True, CurrentTime);
            XTestFakeButtonEvent(dpy, 4, False, CurrentTime);
        }
        if (dx != 0) {
            XTestFakeButtonEvent(dpy, 5, True, CurrentTime);
            XTestFakeButtonEvent(dpy, 5, False, CurrentTime);
        }
        XFlush(dpy);
        XCloseDisplay(dpy);
        return true;
    }
#endif
#ifdef NIMRTC_HAS_UINPUT
    if (mode_ == Mode::kUinput) {
        if (dy != 0) {
            if (!write_event(EV_REL, REL_WHEEL, dy)) return false;
        }
        if (dx != 0) {
            if (!write_event(EV_REL, REL_HWHEEL, dx)) return false;
        }
        return true;
    }
#endif
    return false;
}

bool LinuxInjector::do_inject_key(NeutralKey key,
                                  rd_input::KeyAction action) {
    // The Meta (Super / Windows) key is refused for the same reason it
    // is on the other platforms: it opens the shell.
    if (key == NeutralKey::kMeta) {
        nimrtc::core::log::Logger::instance().warn(
            "rd_input_inject: the Super / Meta key is refused by the "
            "injector (it would open the shell)");
        return false;
    }

    const int code = keycode_for_neutral(key);
    if (code == 0) return false;  // the table does not know the key

    const int value = (action == rd_input::KeyAction::kDown) ? 1 : 0;

#ifdef NIMRTC_HAS_XTEST
    if (mode_ == Mode::kXTest) {
        Display* dpy = XOpenDisplay(nullptr);
        if (dpy == nullptr) return false;
        XTestFakeKeyEvent(dpy, static_cast<unsigned int>(code),
                          value == 1 ? True : False, CurrentTime);
        XFlush(dpy);
        XCloseDisplay(dpy);
        return true;
    }
#endif
#ifdef NIMRTC_HAS_UINPUT
    if (mode_ == Mode::kUinput) {
        return write_event(EV_KEY, code, value);
    }
#endif
    return false;
}

bool LinuxInjector::do_inject_text(const std::string& text) {
    // The text is typed as the *keycode* of the character on a US
    // layout.  Unlike Windows and macOS, the Linux kernel's input
    // layer has no "unicode string" event, so a layout-dependent
    // mapping is unavoidable on this path.  The characters that have a
    // US-layout keycode are typed; the rest are skipped (the caller
    // can send them as a `kText` event on a platform whose backend
    // supports unicode).
    struct AsciiKey { char ch; int code; };
    static const AsciiKey kAscii[] = {
        {'a', KEY_A}, {'b', KEY_B}, {'c', KEY_C}, {'d', KEY_D},
        {'e', KEY_E}, {'f', KEY_F}, {'g', KEY_G}, {'h', KEY_H},
        {'i', KEY_I}, {'j', KEY_J}, {'k', KEY_K}, {'l', KEY_L},
        {'m', KEY_M}, {'n', KEY_N}, {'o', KEY_O}, {'p', KEY_P},
        {'q', KEY_Q}, {'r', KEY_R}, {'s', KEY_S}, {'t', KEY_T},
        {'u', KEY_U}, {'v', KEY_V}, {'w', KEY_W}, {'x', KEY_X},
        {'y', KEY_Y}, {'z', KEY_Z},
        {'0', KEY_0}, {'1', KEY_1}, {'2', KEY_2}, {'3', KEY_3},
        {'4', KEY_4}, {'5', KEY_5}, {'6', KEY_6}, {'7', KEY_7},
        {'8', KEY_8}, {'9', KEY_9},
        {' ', KEY_SPACE}, {'\n', KEY_ENTER}, {'\t', KEY_TAB},
        {'-', KEY_MINUS}, {'=', KEY_EQUAL}, {'[', KEY_LEFTBRACE},
        {']', KEY_RIGHTBRACE}, {';', KEY_SEMICOLON}, {'\'', KEY_APOSTROPHE},
        {'`', KEY_GRAVE}, {'\\', KEY_BACKSLASH}, {',', KEY_COMMA},
        {'.', KEY_DOT}, {'/', KEY_SLASH},
    };

    bool ok = true;
    for (const char ch : text) {
        int code = 0;
        for (const AsciiKey& entry : kAscii) {
            if (entry.ch == ch) { code = entry.code; break; }
        }
        if (code == 0) continue;  // the layout has no key for it

        // The raw code has to be written directly: `do_inject_key()`
        // takes a *neutral* key and maps it through the table, and this
        // path has a raw kernel code already.
#ifdef NIMRTC_HAS_XTEST
        if (mode_ == Mode::kXTest) {
            Display* dpy = XOpenDisplay(nullptr);
            if (dpy == nullptr) { ok = false; break; }
            XTestFakeKeyEvent(dpy, static_cast<unsigned int>(code), True,
                              CurrentTime);
            XTestFakeKeyEvent(dpy, static_cast<unsigned int>(code), False,
                              CurrentTime);
            XFlush(dpy);
            XCloseDisplay(dpy);
            continue;
        }
#endif
#ifdef NIMRTC_HAS_UINPUT
        if (mode_ == Mode::kUinput) {
            if (!write_event(EV_KEY, code, 1)) { ok = false; break; }
            if (!write_event(EV_KEY, code, 0)) { ok = false; break; }
            continue;
        }
#endif
        (void)code;
    }
    return ok;
}

} // namespace nimrtc::rd_input_inject::detail

#endif // __linux__
