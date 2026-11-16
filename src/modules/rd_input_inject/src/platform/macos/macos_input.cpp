/**
 * @file src/modules/rd_input_inject/src/platform/macos/macos_input.cpp
 * @brief The macOS input-injection backend (`CGEvent`).
 */

#if defined(__APPLE__)

#include "macos_input.hpp"

#import <ApplicationServices/ApplicationServices.h>
// The virtual keycodes (`kVK_Escape`, `kVK_ANSI_A`, ...) live in the
// IOKit HID event map, which the ApplicationServices umbrella header
// does NOT pull in.  Without this import every `kVK_*` is undeclared.
#import <IOKit/hidsystem/ev_keymap.h>
#import <Foundation/Foundation.h>

#include <nimrtc/core/log.hpp>

namespace nimrtc::rd_input_inject::detail {
namespace {

// The macOS virtual keycodes.  These are the *hardware* key positions
// on an ANSI layout, not characters — the window server maps them to
// characters through the active input source.
CGKeyCode keycode_for_neutral(NeutralKey key) noexcept {
    switch (key) {
        case NeutralKey::kEscape:   return kVK_Escape;
        case NeutralKey::kTab:      return kVK_Tab;
        case NeutralKey::kEnter:    return kVK_Return;
        case NeutralKey::kSpace:    return kVK_Space;
        case NeutralKey::kBackspace:return kVK_Delete;
        case NeutralKey::kDelete:   return kVK_ForwardDelete;
        case NeutralKey::kHome:     return kVK_Home;
        case NeutralKey::kEnd:      return kVK_End;
        case NeutralKey::kPageUp:   return kVK_PageUp;
        case NeutralKey::kPageDown: return kVK_PageDown;
        case NeutralKey::kLeft:     return kVK_LeftArrow;
        case NeutralKey::kUp:       return kVK_UpArrow;
        case NeutralKey::kRight:    return kVK_RightArrow;
        case NeutralKey::kDown:     return kVK_DownArrow;
        case NeutralKey::kShift:    return kVK_Shift;
        case NeutralKey::kControl:  return kVK_Control;
        case NeutralKey::kAlt:      return kVK_Option;
        case NeutralKey::kMeta:     return kVK_Command;

        case NeutralKey::kF1:  return kVK_F1;
        case NeutralKey::kF2:  return kVK_F2;
        case NeutralKey::kF3:  return kVK_F3;
        case NeutralKey::kF4:  return kVK_F4;
        case NeutralKey::kF5:  return kVK_F5;
        case NeutralKey::kF6:  return kVK_F6;
        case NeutralKey::kF7:  return kVK_F7;
        case NeutralKey::kF8:  return kVK_F8;
        case NeutralKey::kF9:  return kVK_F9;
        case NeutralKey::kF10: return kVK_F10;
        case NeutralKey::kF11: return kVK_F11;
        case NeutralKey::kF12: return kVK_F12;

        case NeutralKey::kA: return kVK_ANSI_A;
        case NeutralKey::kB: return kVK_ANSI_B;
        case NeutralKey::kC: return kVK_ANSI_C;
        case NeutralKey::kD: return kVK_ANSI_D;
        case NeutralKey::kE: return kVK_ANSI_E;
        case NeutralKey::kF: return kVK_ANSI_F;
        case NeutralKey::kG: return kVK_ANSI_G;
        case NeutralKey::kH: return kVK_ANSI_H;
        case NeutralKey::kI: return kVK_ANSI_I;
        case NeutralKey::kJ: return kVK_ANSI_J;
        case NeutralKey::kK: return kVK_ANSI_K;
        case NeutralKey::kL: return kVK_ANSI_L;
        case NeutralKey::kM: return kVK_ANSI_M;
        case NeutralKey::kN: return kVK_ANSI_N;
        case NeutralKey::kO: return kVK_ANSI_O;
        case NeutralKey::kP: return kVK_ANSI_P;
        case NeutralKey::kQ: return kVK_ANSI_Q;
        case NeutralKey::kR: return kVK_ANSI_R;
        case NeutralKey::kS: return kVK_ANSI_S;
        case NeutralKey::kT: return kVK_ANSI_T;
        case NeutralKey::kU: return kVK_ANSI_U;
        case NeutralKey::kV: return kVK_ANSI_V;
        case NeutralKey::kW: return kVK_ANSI_W;
        case NeutralKey::kX: return kVK_ANSI_X;
        case NeutralKey::kY: return kVK_ANSI_Y;
        case NeutralKey::kZ: return kVK_ANSI_Z;

        case NeutralKey::kNone: break;
    }
    return 0;
}

/** The macOS button number for a mouse button. */
CGMouseButton cg_button_for(rd_input::MouseButton button) noexcept {
    switch (button) {
        case rd_input::MouseButton::kLeft:   return kCGMouseButtonLeft;
        case rd_input::MouseButton::kRight:  return kCGMouseButtonRight;
        case rd_input::MouseButton::kMiddle: return kCGMouseButtonCenter;
        default: break;
    }
    return kCGMouseButtonLeft;
}

} // namespace

CGKeyCode macos_keycode_for(NeutralKey key) noexcept {
    return keycode_for_neutral(key);
}

// ---------------------------------------------------------------------------
// The backend
// ---------------------------------------------------------------------------

MacosInjector::MacosInjector() {
    // The accessibility probe.  Without the permission, `CGEventPost`
    // silently drops the events, so a backend that does not check is a
    // backend that reports success while nothing happens.
    if (!AXIsProcessTrusted()) {
        unavailable_reason_ =
            "the process does not hold the Accessibility permission "
            "(System Settings -> Privacy & Security -> Accessibility); "
            "CGEventPost would silently drop the events";
    }
}

MacosInjector::~MacosInjector() = default;

bool MacosInjector::available() const noexcept {
    return unavailable_reason_.empty();
}

std::string MacosInjector::unavailable_reason() const noexcept {
    return unavailable_reason_;
}

bool MacosInjector::post(CGEventRef event) noexcept {
    if (event == nullptr) return false;
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    return true;
}

bool MacosInjector::do_inject_mouse_move(std::int32_t dx, std::int32_t dy) {
    CGEventRef event = CGEventCreateMouseEvent(
        nullptr, kCGEventMouseMoved,
        CGPoint{static_cast<CGFloat>(pointer_x_ + dx),
                 static_cast<CGFloat>(pointer_y_ + dy)},
        kCGMouseButtonLeft);
    return post(event);
}

bool MacosInjector::do_inject_button(rd_input::MouseButton button,
                                     rd_input::ButtonAction action) {
    if (button == rd_input::MouseButton::kUnknown) return false;

    // The macOS event type distinguishes the two buttons by their own
    // event type, not by a "down" flag.
    CGEventType type;
    if (button == rd_input::MouseButton::kRight) {
        type = (action == rd_input::ButtonAction::kDown)
                   ? kCGEventRightMouseDown : kCGEventRightMouseUp;
    } else {
        type = (action == rd_input::ButtonAction::kDown)
                   ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
    }

    CGEventRef event = CGEventCreateMouseEvent(
        nullptr, type,
        CGPoint{static_cast<CGFloat>(pointer_x_),
                 static_cast<CGFloat>(pointer_y_)},
        cg_button_for(button));
    return post(event);
}

bool MacosInjector::do_inject_wheel(std::int32_t dx, std::int32_t dy) {
    // The wheel is expressed in *lines*; the scroll event carries the
    // integer delta.
    CGEventRef event = CGEventCreateScrollWheelEvent(
        nullptr, kCGScrollEventUnitLine, 2,
        static_cast<int32_t>(dy), static_cast<int32_t>(dx));
    return post(event);
}

bool MacosInjector::do_inject_key(NeutralKey key,
                                  rd_input::KeyAction action) {
    // The Command key is refused for the same reason the Windows key is
    // on Windows: it opens the shell, and a remote-desktop agent that
    // can press it can lock the operator out.
    if (key == NeutralKey::kMeta) {
        nimrtc::core::log::Logger::instance().warn(
            "rd_input_inject: the Command / Meta key is refused by the "
            "injector (it would open the shell)");
        return false;
    }

    const CGKeyCode keycode = keycode_for_neutral(key);
    if (keycode == 0) return false;  // the table does not know the key

    CGEventRef event = CGEventCreateKeyboardEvent(
        nullptr, keycode, action == rd_input::KeyAction::kDown);
    return post(event);
}

bool MacosInjector::do_inject_text(const std::string& text) {
    // The text is typed as *unicode* key events, so the characters
    // appear exactly as they are — no keyboard layout involved, no
    // dead keys, no Option-key puzzles.
    //
    // `CGEventKeyboardSetUnicodeString` takes a UniChar buffer, so the
    // UTF-8 is transcoded first.  The text is expected to be valid
    // UTF-8 (the vocabulary says so), and an invalid sequence is
    // skipped rather than aborting the whole string.
    bool ok = true;
    NSString* ns = [[NSString alloc] initWithBytes:text.data()
                                            length:text.size()
                                          encoding:NSUTF8StringEncoding];
    if (ns == nil) return false;

    const NSUInteger length = ns.length;
    for (NSUInteger i = 0; i < length; ++i) {
        unichar ch = [ns characterAtIndex:i];
        CGEventRef down = CGEventCreateKeyboardEvent(nullptr, 0, true);
        if (down == nullptr) { ok = false; continue; }
        CGEventKeyboardSetUnicodeString(down, 1, &ch);
        CGEventPost(kCGHIDEventTap, down);
        CFRelease(down);

        CGEventRef up = CGEventCreateKeyboardEvent(nullptr, 0, false);
        if (up == nullptr) { ok = false; continue; }
        CGEventKeyboardSetUnicodeString(up, 1, &ch);
        CGEventPost(kCGHIDEventTap, up);
        CFRelease(up);
    }
    return ok;
}

} // namespace nimrtc::rd_input_inject::detail

#endif // __APPLE__
