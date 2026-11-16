/**
 * @file src/modules/rd_input_inject/src/platform/windows/windows_input.hpp
 * @brief The Windows input-injection backend (`SendInput`).
 *
 * ## Why `SendInput`
 *
 * Windows offers three ways to synthesise input, and only one of them
 * is usable for a remote-desktop product:
 *
 *   - `PostMessage` / `SendMessage` with `WM_KEYDOWN` — sends the
 *     message to *one window*.  It never reaches the focused control of
 *     a different process, it does not work with the UIPI-protected
 *     windows, and it does not compose with the physical keyboard
 *     state (so a key the user is holding looks released).
 *   - `keybd_event` — the documented-as-obsolete predecessor of
 *     `SendInput`; it is a compatibility shim over the same machinery
 *     and offers nothing extra.
 *   - `SendInput` — injects into the **system's** input stream, so it
 *     goes through the same focus, UIPI and keyboard-state machinery as
 *     a physical key.  It also reports a per-call error code, which is
 *     what lets the injector distinguish "the OS said no" from "the
 *     event was refused".
 *
 * `SendInput` it is.
 *
 * ## The UIPI caveat
 *
 * `SendInput` cannot inject into a window that is running at a *higher*
 * integrity level than the injector (User Interface Privilege
 * Isolation).  That is a **security feature**, and it is not something
 * the injector can or should work around: a remote-desktop agent that
 * could type into an elevated prompt would be a privilege-escalation
 * hole.  The backend reports the failure honestly.
 *
 * ## The virtual-key table
 *
 * The Windows virtual-key codes are *not* the neutral layout's codes,
 * so the backend carries the table.  A key the table does not know is
 * refused (see `input_injector.hpp`).
 */

#pragma once

#ifdef _WIN32

#include <Windows.h>

#include <string>

#include <nimrtc/rd_input_inject/key_map.hpp>

#include "../../injector_base.hpp"

namespace nimrtc::rd_input_inject::detail {

/**
 * @brief The Windows virtual-key code for a neutral key.
 *
 * @return the virtual-key code, or 0 when the table does not know the
 *         key (0 is not a valid virtual key, so it is a safe "no
 *         mapping" marker).
 */
WORD windows_vk_for(NeutralKey key) noexcept;

/**
 * @brief The Windows virtual-key code for a neutral *modifier*.
 *
 * The modifiers are the keys that stay held while another key is
 * pressed, so they need the "extended key" flag for the ones Windows
 * distinguishes (the right-hand Ctrl / Alt, the arrow keys).
 */
WORD windows_vk_for_modifier(NeutralKey key) noexcept;

/**
 * @brief Whether a virtual key needs the `KEYEVENTF_EXTENDEDKEY` flag.
 */
bool windows_vk_is_extended(WORD vk) noexcept;

/** The Windows backend. */
class WindowsInjector final : public InjectorBase {
public:
    WindowsInjector();
    ~WindowsInjector() override;

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return "windows-sendinput";
    }

    [[nodiscard]] bool available() const noexcept override;
    [[nodiscard]] std::string unavailable_reason() const noexcept override;

protected:
    bool do_inject_mouse_move(std::int32_t dx, std::int32_t dy) override;
    bool do_inject_button(rd_input::MouseButton button,
                          rd_input::ButtonAction action) override;
    bool do_inject_wheel(std::int32_t dx, std::int32_t dy) override;
    bool do_inject_key(NeutralKey key,
                       rd_input::KeyAction action) override;
    bool do_inject_text(const std::string& text) override;

private:
    /** Send one `INPUT` structure. */
    bool send_input(const INPUT& input) noexcept;

    /** The reason the backend is unavailable (empty when available). */
    std::string unavailable_reason_;
};

} // namespace nimrtc::rd_input_inject::detail

#endif // _WIN32
