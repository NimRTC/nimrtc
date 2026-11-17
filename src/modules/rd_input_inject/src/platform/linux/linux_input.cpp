/**
 * @file src/modules/rd_input_inject/src/platform/linux/linux_input.cpp
 * @brief The Linux input-injection backend (`XTest` / uinput).
 */

#if defined(__linux__)

#include "linux_input.hpp"

#include "../../linux_input_codes.hpp"

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

// The key and button tables are *not* here: they live in
// `../../linux_input_codes.hpp`, which carries no platform type and is
// therefore compiled (and unit-tested) on every CI leg.  What is
// here is the part that is genuinely Linux-and-X11-specific: the two
// injection vocabularies and the calls that consume them.
//
// The two vocabularies are unrelated, and mixing them up does not
// fail loudly -- the X server accepts a key it does not recognise and
// injects nothing, so the device reports success while the
// operator's keystrokes go nowhere:
//
//   - uinput speaks the kernel's codes (`KEY_A`, `BTN_LEFT`);
//   - XTest speaks the X server's (`XK_a` -- which the call site
//     converts to a layout-specific keycode -- and the button number
//     `1`).

} // namespace

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
                // `XTestQueryExtension()` writes to **all five** of its
                // out-parameters -- it is not a "may write" API -- so
                // passing `nullptr` for any of them is a null-pointer
                // write inside libXtst, and the injector crashes the
                // process on construction.
                //
                // The values are not needed here (the return value is
                // the answer), so they go into locals that are simply
                // ignored.
                int event_base = 0;
                int error_base = 0;
                int major = 0;
                int minor = 0;
                if (XTestQueryExtension(dpy, &event_base, &error_base,
                                        &major, &minor)) {
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
        const LinuxKeyCode code = linux_keycode_for(key);
        if (code != kLinuxKeyUnknown) ::ioctl(fd, UI_SET_KEYBIT, code);
    }
    for (const LinuxButtonCode code :
         {linux_button_code_for(rd_input::MouseButton::kLeft),
          linux_button_code_for(rd_input::MouseButton::kRight),
          linux_button_code_for(rd_input::MouseButton::kMiddle)}) {
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
    const int value = (action == rd_input::ButtonAction::kDown) ? 1 : 0;

#ifdef NIMRTC_HAS_XTEST
    if (mode_ == Mode::kXTest) {
        // The X server numbers its buttons 1..5 (and 8, 9); the
        // kernel's `BTN_*` codes are a different vocabulary and are
        // meaningless here.
        const X11ButtonNumber number = x11_button_number_for(button);
        if (number == kX11ButtonUnknown) return false;

        Display* dpy = XOpenDisplay(nullptr);
        if (dpy == nullptr) return false;
        if (value == 1) {
            XTestFakeButtonEvent(dpy, number, True, CurrentTime);
        } else {
            XTestFakeButtonEvent(dpy, number, False, CurrentTime);
        }
        XFlush(dpy);
        XCloseDisplay(dpy);
        return true;
    }
#endif
#ifdef NIMRTC_HAS_UINPUT
    if (mode_ == Mode::kUinput) {
        const LinuxButtonCode code = linux_button_code_for(button);
        if (code == kLinuxButtonUnknown) return false;
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

    // The two backends speak different vocabularies, so the code is
    // resolved *per backend* below rather than once here.
    const int value = (action == rd_input::KeyAction::kDown) ? 1 : 0;

#ifdef NIMRTC_HAS_XTEST
    if (mode_ == Mode::kXTest) {
        // XTest speaks the X server's vocabulary, and the X server
        // wants a **keycode** -- the number it assigned to a physical
        // key on the *active* keyboard layout -- not a KeySym and
        // certainly not the kernel's key code.  `XKeysymToKeycode()` is
        // the mapping, and it is layout-aware: the same KeySym lands on
        // a different keycode on a different layout.
        //
        // A KeySym the active layout has no key for maps to keycode 0,
        // which is not a key: injecting it would send a bogus event
        // that the server drops, so the call is refused instead.
        const X11KeySym sym = x11_keysym_for(key);
        if (sym == kX11KeySymUnknown) return false;  // the table's gap

        Display* dpy = XOpenDisplay(nullptr);
        if (dpy == nullptr) return false;
        const KeyCode code = XKeysymToKeycode(dpy, sym);
        if (code == 0) {
            XCloseDisplay(dpy);
            return false;  // the active layout has no key for it
        }
        XTestFakeKeyEvent(dpy, code, value == 1 ? True : False, CurrentTime);
        XFlush(dpy);
        XCloseDisplay(dpy);
        return true;
    }
#endif
#ifdef NIMRTC_HAS_UINPUT
    if (mode_ == Mode::kUinput) {
        const LinuxKeyCode code = linux_keycode_for(key);
        if (code == kLinuxKeyUnknown) return false;  // the table's gap
        return write_event(EV_KEY, code, value);
    }
#endif
    return false;
}

bool LinuxInjector::do_inject_text(const std::string& text) {
    // The text is typed as the *key* of the character on a US layout.
    // Unlike Windows and macOS, neither the Linux kernel's input layer
    // nor the X11 protocol has a "unicode string" event, so a
    // layout-dependent mapping is unavoidable on this path.  The
    // characters that have a US-layout key are typed; the rest are
    // skipped (the caller can send them as a `kText` event on a
    // platform whose backend supports unicode).
    //
    // The two backends need the character's key in their own
    // vocabulary: the kernel's `KEY_*` code on the uinput path, the
    // X server's KeySym on the XTest path.  Neither is the other's.
    bool ok = true;
    for (const char ch : text) {
#ifdef NIMRTC_HAS_XTEST
        if (mode_ == Mode::kXTest) {
            const X11KeySym sym = x11_keysym_for_ascii(ch);
            if (sym == kX11KeySymUnknown) continue;  // no KeySym for it

            Display* dpy = XOpenDisplay(nullptr);
            if (dpy == nullptr) { ok = false; break; }
            const KeyCode code = XKeysymToKeycode(dpy, sym);
            if (code == 0) {
                // The active layout has no key for this character.
                XCloseDisplay(dpy);
                continue;
            }
            XTestFakeKeyEvent(dpy, code, True, CurrentTime);
            XTestFakeKeyEvent(dpy, code, False, CurrentTime);
            XFlush(dpy);
            XCloseDisplay(dpy);
            continue;
        }
#endif
#ifdef NIMRTC_HAS_UINPUT
        if (mode_ == Mode::kUinput) {
            const LinuxKeyCode code = linux_keycode_for_ascii(ch);
            if (code == kLinuxKeyUnknown) continue;  // no key for it

            if (!write_event(EV_KEY, code, 1)) { ok = false; break; }
            if (!write_event(EV_KEY, code, 0)) { ok = false; break; }
            continue;
        }
#endif
    }
    return ok;
}

} // namespace nimrtc::rd_input_inject::detail

#endif // __linux__
