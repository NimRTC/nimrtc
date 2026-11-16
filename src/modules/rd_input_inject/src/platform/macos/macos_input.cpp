/**
 * @file src/modules/rd_input_inject/src/platform/macos/macos_input.cpp
 * @brief The macOS input-injection backend (`CGEvent`).
 */

#if defined(__APPLE__)

#include "macos_input.hpp"

// The virtual-keycode table and its neutral-key mapping.  The table is
// portable (see ../../macos_keycodes.hpp) so that it can be unit
// tested on every CI leg; only the *use* of it is macOS-specific.
#include "macos_keycodes.hpp"

#import <ApplicationServices/ApplicationServices.h>
#import <Foundation/Foundation.h>

#include <nimrtc/core/log.hpp>

namespace nimrtc::rd_input_inject::detail {
namespace {

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

    const MacVKCode keycode = macos_keycode_for(key);
    // The sentinel is 0xFFFF, NOT 0 -- 0x00 is the real keycode for 'A'.
    if (keycode == kMacVKUnknown) return false;  // the table does not know the key

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
