/**
 * @file src/modules/rd_input_inject/src/injector_base.cpp
 * @brief The portable dispatch skeleton every backend shares.
 */

#include "injector_base.hpp"

#include <nimrtc/rd_input_inject/key_map.hpp>

namespace nimrtc::rd_input_inject::detail {

std::vector<NeutralKey>
InjectorBase::modifier_keys(std::uint32_t modifiers) noexcept {
    // The bit values are the `rd_input::Modifier` ones.
    std::vector<NeutralKey> keys;
    if (modifiers & 0x0001u) keys.push_back(NeutralKey::kShift);   // kShift
    if (modifiers & 0x0002u) keys.push_back(NeutralKey::kControl); // kControl
    if (modifiers & 0x0004u) keys.push_back(NeutralKey::kAlt);     // kAlt
    // The Meta (Windows / Command) modifier is intentionally omitted —
    // see the header's note on `modifier_keys`.
    return keys;
}

InjectionResult InjectorBase::inject(const rd_input::InputEvent& event) {
    InjectionResult result = apply(event);
    count(result);
    return result;
}

InjectionResult InjectorBase::apply(const rd_input::InputEvent& event) {
    switch (event.type) {
        case rd_input::InputEventType::kMouseMove: {
            // A zero motion is a no-op, not an error.  The coalescer
            // may legitimately produce one (a mouse that did not move
            // between two polls), and refusing it would make the
            // refusal counter meaningless.
            if (event.dx == 0 && event.dy == 0) return InjectionResult::kInjected;

            if (!do_inject_mouse_move(event.dx, event.dy)) {
                count_os_error();
                return InjectionResult::kFailed;
            }
            // The tracker follows the motion, so the host can report
            // where the pointer *should* be even on a platform where
            // querying the OS is expensive.
            pointer_x_ += event.dx;
            pointer_y_ += event.dy;
            return InjectionResult::kInjected;
        }

        case rd_input::InputEventType::kMouseButton:
            return apply_button(event);

        case rd_input::InputEventType::kMouseWheel: {
            if (event.dx == 0 && event.dy == 0) return InjectionResult::kInjected;
            if (!do_inject_wheel(event.dx, event.dy)) {
                count_os_error();
                return InjectionResult::kFailed;
            }
            return InjectionResult::kInjected;
        }

        case rd_input::InputEventType::kKey:
            return apply_key(event);

        case rd_input::InputEventType::kText: {
            if (event.text.empty()) return InjectionResult::kInjected;
            if (!do_inject_text(event.text)) {
                count_os_error();
                return InjectionResult::kFailed;
            }
            return InjectionResult::kInjected;
        }
    }
    return InjectionResult::kRefused;
}

InjectionResult InjectorBase::apply_key(const rd_input::InputEvent& event) {
    const auto neutral = neutral_key_of(event.key_code);
    if (!neutral.has_value()) {
        // An unknown key code is **refused**, never guessed: an injected
        // key with a wrong virtual-key number is a wrong keystroke on
        // the remote machine, which is worse than a missing one.
        return InjectionResult::kRefused;
    }

    // The modifiers the event carries are pressed around the key — but
    // only the ones that are **not already down**.  Re-synthesising a
    // held Shift would release the operator's Shift when the keystroke
    // finished, and the *next* key-up event would then be a spurious
    // release.  This is the bookkeeping that keeps the OS's keyboard
    // state in step with the operator's.
    const std::uint32_t modifiers = event.modifiers;
    std::vector<NeutralKey> pressed_now;

    const bool down = (event.key_action == rd_input::KeyAction::kDown);

    // The modifier set is applied for *both* directions: a key-up
    // event carries the modifiers that were held when the key went
    // down, and the backend has to reproduce that combination.
    if (modifiers != 0) {
        for (NeutralKey mod : modifier_keys(modifiers)) {
            if (key_is_down(mod)) continue;
            if (!do_inject_key(mod, rd_input::KeyAction::kDown)) {
                count_os_error();
                return InjectionResult::kFailed;
            }
            pressed_now.push_back(mod);
        }
    }

    const auto it = pressed_keys_.find(*neutral);
    const bool first_press = (it == pressed_keys_.end());

    // A key that is already down is an **auto-repeat**.  The operator's
    // OS has already de-duplicated the events, so the repeat is
    // forwarded as-is.
    if (!do_inject_key(*neutral, down ? rd_input::KeyAction::kDown
                                      : rd_input::KeyAction::kUp)) {
        count_os_error();
        // The modifiers that were pressed for this event have to be
        // released again, or they would stay stuck down.
        for (auto rit = pressed_now.rbegin(); rit != pressed_now.rend(); ++rit) {
            if (!do_inject_key(*rit, rd_input::KeyAction::kUp)) count_os_error();
        }
        return InjectionResult::kFailed;
    }

    // The modifiers are released again — but only the ones this event
    // pressed.  The ones the operator was already holding stay down.
    for (auto rit = pressed_now.rbegin(); rit != pressed_now.rend(); ++rit) {
        if (!do_inject_key(*rit, rd_input::KeyAction::kUp)) {
            count_os_error();
        }
    }

    if (down) {
        if (first_press) {
            pressed_keys_.emplace(*neutral, KeyState{*neutral, 1});
        } else {
            ++it->second.depth;
        }
    } else {
        // A release of a key that is not down: the OS will report it as
        // a spurious release, which is harmless.  Refusing it would
        // hide a real desynchronisation between the operator's side and
        // the remote side, so the injection is attempted and the state
        // is corrected.
        if (it != pressed_keys_.end()) {
            if (it->second.depth > 1) {
                --it->second.depth;
            } else {
                pressed_keys_.erase(it);
            }
        }
    }
    return InjectionResult::kInjected;
}

InjectionResult InjectorBase::apply_button(const rd_input::InputEvent& event) {
    if (event.button == rd_input::MouseButton::kUnknown) {
        return InjectionResult::kRefused;
    }

    const bool down = (event.button_action == rd_input::ButtonAction::kDown);
    if (!do_inject_button(event.button, event.button_action)) {
        count_os_error();
        return InjectionResult::kFailed;
    }

    if (down) {
        pressed_buttons_.emplace(event.button, ButtonState{event.button});
    } else {
        pressed_buttons_.erase(event.button);
    }
    return InjectionResult::kInjected;
}

std::size_t InjectorBase::release_all() {
    std::size_t released = 0;

    // The buttons first, then the keys: a modifier that is released
    // while a button is still down would make the button's release
    // mean something different than the press did.
    for (const auto& entry : pressed_buttons_) {
        if (do_inject_button(entry.first, rd_input::ButtonAction::kUp)) {
            ++released;
        } else {
            count_os_error();
        }
    }
    pressed_buttons_.clear();

    for (const auto& entry : pressed_keys_) {
        if (do_inject_key(entry.first, rd_input::KeyAction::kUp)) {
            ++released;
        } else {
            count_os_error();
        }
    }
    pressed_keys_.clear();

    return released;
}

} // namespace nimrtc::rd_input_inject::detail
