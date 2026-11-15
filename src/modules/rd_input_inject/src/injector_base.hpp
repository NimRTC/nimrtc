/**
 * @file src/modules/rd_input_inject/src/injector_base.hpp
 * @brief The portable half of the input injector — the state tracking and
 *        the dispatch skeleton every backend shares.
 *
 * ## What is portable and what is not
 *
 * The **state** is portable: which keys are down, which buttons are
 * down, where the pointer is.  So is the *dispatch skeleton*: the
 * decision about what an event means (a move, a button, a wheel, a key,
 * a text) and the bookkeeping around it.
 *
 * The **act** is not portable: `SendInput`, `CGEvent` and `XTest` are
 * three different APIs with three different units and three different
 * ways to express "shift is held while this key goes down".  That part
 * is the backend's job, behind `do_inject_*()`.
 *
 * ## Why the state lives here
 *
 * The dead-man switch (plan G6) has to release every key the operator
 * was holding when the control path went quiet.  That is impossible
 * without a record of what was pressed — and the record has to be
 * maintained by the thing that presses the keys, because nobody else
 * sees them go down.  Hence the base class.
 *
 * The base class is *not* in the public header: it is an implementation
 * detail of the module, and a host has no business reaching into it.
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <nimrtc/rd_input_inject/input_injector.hpp>
#include <nimrtc/rd_input_inject/key_map.hpp>

namespace nimrtc::rd_input_inject::detail {

/**
 * @brief The shared state and skeleton of every backend.
 *
 * A backend derives from this and implements the `do_*` hooks.  The
 * hooks return `true` when the OS accepted the call.
 */
class InjectorBase : public InputInjector {
public:
    /** The state of one pressed key. */
    struct KeyState {
        /** The neutral key. */
        NeutralKey key = NeutralKey::kNone;
        /** How many times the key is down (auto-repeat presses one key
         *  several times before the release). */
        std::uint32_t depth = 0;
    };

    /** The state of one pressed mouse button. */
    struct ButtonState {
        rd_input::MouseButton button = rd_input::MouseButton::kUnknown;
    };

    // ---- InputInjector -----------------------------------------------------

    [[nodiscard]] InjectionStats stats() const noexcept override {
        return stats_;
    }

    [[nodiscard]] std::size_t keys_down() const noexcept override {
        return pressed_keys_.size();
    }

    [[nodiscard]] std::size_t buttons_down() const noexcept override {
        return pressed_buttons_.size();
    }

    [[nodiscard]] std::int32_t pointer_x() const noexcept override {
        return pointer_x_;
    }

    [[nodiscard]] std::int32_t pointer_y() const noexcept override {
        return pointer_y_;
    }

    InjectionResult inject(const rd_input::InputEvent& event) override;

    std::size_t release_all() override;

protected:
    // ---- The backend hooks -------------------------------------------------

    /** Inject a pointer motion (relative, in pixels). */
    virtual bool do_inject_mouse_move(std::int32_t dx, std::int32_t dy) = 0;

    /** Inject a button press or release. */
    virtual bool do_inject_button(rd_input::MouseButton button,
                                  rd_input::ButtonAction action) = 0;

    /** Inject a wheel scroll. */
    virtual bool do_inject_wheel(std::int32_t dx, std::int32_t dy) = 0;

    /** Inject a key press or release.
     *
     * The **modifiers** are handled by the base class (which presses
     * and releases them around the key), so a backend only ever sees
     * the key itself.  That keeps the "is the modifier already held?"
     * bookkeeping in one place — and it is the bookkeeping that
     * decides whether a held Shift gets released by a keystroke that
     * merely *mentioned* it. */
    virtual bool do_inject_key(NeutralKey key,
                               rd_input::KeyAction action) = 0;

    /** Inject committed text (a string of characters). */
    virtual bool do_inject_text(const std::string& text) = 0;

    /** The pointer's current position, as the OS reports it.
     *
     * The base class cannot know it, so a backend that tracks the
     * pointer itself may override this to report the tracked value. */
    virtual std::int32_t query_pointer_x() const noexcept { return pointer_x_; }
    virtual std::int32_t query_pointer_y() const noexcept { return pointer_y_; }

    /** Record the counters for one outcome. */
    void count(InjectionResult result) noexcept {
        switch (result) {
            case InjectionResult::kInjected: ++stats_.injected; break;
            case InjectionResult::kRefused:  ++stats_.refused;  break;
            case InjectionResult::kFailed:   ++stats_.failed;   break;
        }
    }

    /** Note an OS-level failure (one failed OS call). */
    void count_os_error() noexcept { ++stats_.os_errors; }

    /**
     * @brief Whether a key is currently down, per the injector's state.
     *
     * A backend needs this to avoid **re-synthesising** a modifier the
     * operator is already holding: pressing a held Shift down again and
     * releasing it afterwards would release the operator's Shift, and
     * the subsequent key-up event would then be a spurious release.
     */
    [[nodiscard]] bool key_is_down(NeutralKey key) const noexcept {
        return pressed_keys_.find(key) != pressed_keys_.end();
    }

    /** The pointer's tracked position. */
    std::int32_t pointer_x_ = 0;
    std::int32_t pointer_y_ = 0;

    /**
     * @brief The neutral keys a modifier bit-set names.
     *
     * The bit values are the `rd_input::Modifier` ones, so the
     * vocabulary's bits and this function's input are the same number
     * space.  The **Meta** modifier is deliberately absent: the
     * Windows / Command key opens the shell, and a remote-desktop
     * agent that can press it can lock the operator out of the machine
     * they are driving.  The safety filter (plan G7) is the right
     * place to *deny* it; the injector refuses to synthesise it on its
     * own, so a backend that honours the key unconditionally is not
     * reachable.
     */
    static std::vector<NeutralKey> modifier_keys(std::uint32_t modifiers) noexcept;

private:
    /** Apply one event and update the state. */
    InjectionResult apply(const rd_input::InputEvent& event);

    /** Handle one key event. */
    InjectionResult apply_key(const rd_input::InputEvent& event);

    /** Handle one button event. */
    InjectionResult apply_button(const rd_input::InputEvent& event);

    InjectionStats stats_{};
    std::map<NeutralKey, KeyState> pressed_keys_;
    std::map<rd_input::MouseButton, ButtonState> pressed_buttons_;
};

} // namespace nimrtc::rd_input_inject::detail
