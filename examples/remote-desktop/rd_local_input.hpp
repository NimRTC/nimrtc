/**
 * @file examples/remote-desktop/rd_local_input.hpp
 * @brief The operator's **local** mouse and keyboard capture.
 *
 * ## Why this file exists
 *
 * The remote-desktop plan has an *injection* module (`rd_input_inject`,
 * the device side) but no *capture* module: the plan's Track G covers
 * the vocabulary (G3), the coalescing (G4), the frame binding (G5), the
 * dead-man switch (G6) and the safety filter (G7) — but the act of
 * **reading** the operator's keyboard and mouse is a *product* concern,
 * exactly like the act of injecting them on the other end.
 *
 * So the capture lives here, in the example, and not in `src/`:
 *
 *   - it is the operator's own machine, and a product may want a
 *     completely different capture strategy (a browser's
 *     `pointerrawupdate`, a gamepad, a KVM);
 *   - the plan deliberately keeps the signalling service and the
 *     session driver in `examples/` (decision RD-7);
 *   - there is no second implementation to abstract over, so a
 *     `plugins::I*` seam would be a lie (AGENTS.md L2).
 *
 * ## What it produces
 *
 * `rd_input::InputEvent`s in the vocabulary's **platform-neutral** key
 * layout — the same `rd_input::keys::` codes the injector translates
 * into the device's own virtual-key numbers.  A Windows operator
 * driving a macOS device therefore works, because both ends speak the
 * neutral layout.
 *
 * ## The polling model
 *
 * The capture is a **poller**, not an event-driven hook:
 *
 *   - Windows: `GetAsyncKeyState()` for the keyboard and
 *     `GetCursorPos()` for the pointer;
 *   - the other platforms: a stub that reports "nothing captured".
 *
 * Polling is the right model for the *example* because it needs no
 * window, no message loop and no elevated rights — the properties that
 * make it runnable in CI and in a headless container.  A product that
 * wants the lower latency of a raw-input hook replaces this file; the
 * rest of the viewer is unchanged, because it only ever sees
 * `rd_input::InputEvent`s.
 *
 * ## The dead-man switch
 *
 * The right mouse button is the **dead-man switch** (plan G6): the
 * operator holds it while they drive the remote machine, and the device
 * enters its safe state the moment it is released.  The button is
 * reported as a normal `kMouseButton` event, and the device's
 * watchdog recognises it (see `DeadManConfig::deadman_button`).
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nimrtc/rd_input/input_event.hpp>

#ifdef _WIN32
    #include <windows.h>
#endif

namespace rd_local {

namespace rdi = nimrtc::rd_input;

/**
 * @brief The operator's local mouse and keyboard.
 *
 * The class is **not** thread-safe: the viewer polls it from its own
 * loop, on one thread.
 */
class LocalInputCapture {
public:
    LocalInputCapture() = default;

    /** Whether this platform has a capture implementation at all. */
    [[nodiscard]] static bool supported() noexcept {
#ifdef _WIN32
        return true;
#else
        return false;
#endif
    }

    /** A human-readable name for the capture backend. */
    [[nodiscard]] static std::string backend_name() {
#ifdef _WIN32
        return "windows-getasynckeystate";
#else
        return "none";
#endif
    }

    /**
     * @brief Poll the devices and return the events since the last poll.
     *
     * @param now_us the host's monotonic clock, in microseconds.
     * @return the events, in the order they happened.
     */
    std::vector<rdi::InputEvent> poll(std::uint64_t now_us) {
        std::vector<rdi::InputEvent> events;
#ifdef _WIN32
        poll_windows(now_us, events);
#else
        (void)now_us;
        (void)events;
#endif
        return events;
    }

    /** The pointer's last known position, in virtual-screen pixels. */
    [[nodiscard]] int pointer_x() const noexcept { return pointer_x_; }
    [[nodiscard]] int pointer_y() const noexcept { return pointer_y_; }

private:
    /** The pointer's last known position, in virtual-screen pixels. */
    //
    // These live outside the `_WIN32` block because the accessors below
    // are part of the class on every platform -- only the *polling* is
    // Windows-only.
    int pointer_x_ = 0;
    int pointer_y_ = 0;

#ifdef _WIN32
    /** The keyboard's state at the previous poll. */
    bool key_down_[256] = {};
    bool key_known_[256] = {};

    /** The mouse buttons' state at the previous poll. */
    bool button_down_[3] = {};

    /** Whether the pointer's position is known yet. */
    bool pointer_known_ = false;

    void poll_windows(std::uint64_t now_us, std::vector<rdi::InputEvent>& out) {
        // ---- The pointer ---------------------------------------------------
        POINT pt{};
        if (::GetCursorPos(&pt)) {
            if (pointer_known_) {
                const int dx = static_cast<int>(pt.x) - pointer_x_;
                const int dy = static_cast<int>(pt.y) - pointer_y_;
                if (dx != 0 || dy != 0) {
                    rdi::InputEvent e = rdi::make_mouse_move(dx, dy, now_us);
                    e.x = static_cast<std::int32_t>(pt.x);
                    e.y = static_cast<std::int32_t>(pt.y);
                    out.push_back(e);
                }
            }
            pointer_x_ = static_cast<int>(pt.x);
            pointer_y_ = static_cast<int>(pt.y);
            pointer_known_ = true;
        }

        // ---- The mouse buttons ---------------------------------------------
        //
        // The three buttons are reported as the vocabulary's own codes.
        // The *right* button is the dead-man switch, and it is reported
        // like any other button — the device's watchdog is what gives
        // it its meaning.
        struct ButtonPoll {
            int vk;
            rdi::MouseButton button;
        };
        static constexpr ButtonPoll kButtons[] = {
            {VK_LBUTTON, rdi::MouseButton::kLeft},
            {VK_RBUTTON, rdi::MouseButton::kRight},
            {VK_MBUTTON, rdi::MouseButton::kMiddle},
        };
        for (std::size_t i = 0; i < std::size(kButtons); ++i) {
            const bool down = (::GetAsyncKeyState(kButtons[i].vk) & 0x8000) != 0;
            if (down != button_down_[i]) {
                button_down_[i] = down;
                out.push_back(rdi::make_mouse_button(
                    kButtons[i].button,
                    down ? rdi::ButtonAction::kDown : rdi::ButtonAction::kUp,
                    static_cast<std::int32_t>(pointer_x_),
                    static_cast<std::int32_t>(pointer_y_),
                    now_us));
            }
        }

        // ---- The keyboard --------------------------------------------------
        //
        // Only the keys the neutral layout knows are reported.  A key the
        // layout does not know (the digits, the punctuation) is reachable
        // through the *text* event kind, which a product with a proper
        // IME / keyboard-layout layer produces.  This example does not
        // implement that layer, so those keys are simply not sent — a
        // missing keystroke is much better than a wrong one.
        for (int i = 0; i < 256; ++i) {
            const std::uint32_t code = neutral_code_of_vk(static_cast<int>(i));
            if (code == 0) continue;
            const bool down = (::GetAsyncKeyState(i) & 0x8000) != 0;
            if (!key_known_[i]) {
                key_known_[i] = true;
                key_down_[i] = down;
                continue;  // the first poll establishes the baseline
            }
            if (down == key_down_[i]) continue;
            key_down_[i] = down;
            out.push_back(rdi::make_key(
                code,
                down ? rdi::KeyAction::kDown : rdi::KeyAction::kUp,
                0,
                false,
                now_us));
        }
    }

    /**
     * @brief The neutral-layout code for a Windows virtual key.
     *
     * @return the code, or 0 when the key is not in the neutral layout.
     */
    static std::uint32_t neutral_code_of_vk(int vk) noexcept {
        switch (vk) {
            case VK_ESCAPE:  return rdi::keys::kEscape;
            case VK_TAB:     return rdi::keys::kTab;
            case VK_RETURN:  return rdi::keys::kEnter;
            case VK_SPACE:   return rdi::keys::kSpace;
            case VK_BACK:    return rdi::keys::kBackspace;
            case VK_DELETE:  return rdi::keys::kDelete;
            case VK_HOME:    return rdi::keys::kHome;
            case VK_END:     return rdi::keys::kEnd;
            case VK_PRIOR:   return rdi::keys::kPageUp;
            case VK_NEXT:    return rdi::keys::kPageDown;
            case VK_LEFT:    return rdi::keys::kLeft;
            case VK_UP:      return rdi::keys::kUp;
            case VK_RIGHT:   return rdi::keys::kRight;
            case VK_DOWN:    return rdi::keys::kDown;
            case VK_SHIFT:   return rdi::keys::kShift;
            case VK_CONTROL: return rdi::keys::kControl;
            case VK_MENU:    return rdi::keys::kAlt;
            case VK_LWIN:    return rdi::keys::kMeta;
            case VK_RWIN:    return rdi::keys::kMeta;
            case VK_F1:      return rdi::keys::kF1;
            case VK_F2:      return rdi::keys::kF2;
            case VK_F3:      return rdi::keys::kF3;
            case VK_F4:      return rdi::keys::kF4;
            case VK_F5:      return rdi::keys::kF5;
            case VK_F6:      return rdi::keys::kF6;
            case VK_F7:      return rdi::keys::kF7;
            case VK_F8:      return rdi::keys::kF8;
            case VK_F9:      return rdi::keys::kF9;
            case VK_F10:     return rdi::keys::kF10;
            case VK_F11:     return rdi::keys::kF11;
            case VK_F12:     return rdi::keys::kF12;
            default: break;
        }
        // The letters: 'A'..'Z' map onto the layout's 100..125.
        if (vk >= 'A' && vk <= 'Z') {
            return rdi::keys::kA + static_cast<std::uint32_t>(vk - 'A');
        }
        return 0;
    }
#endif
};

} // namespace rd_local
