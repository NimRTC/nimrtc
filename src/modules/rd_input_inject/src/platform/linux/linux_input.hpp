/**
 * @file src/modules/rd_input_inject/src/platform/linux/linux_input.hpp
 * @brief The Linux input-injection backend (`XTest` / uinput).
 *
 * ## Two backends, one seam
 *
 * Linux has no single "inject into the system" call, because the
 * desktop situation is not uniform:
 *
 *   - **X11** — `XTest` (libXtst) synthesises events through the X
 *     server's virtual-device extension.  It works on an X11 session
 *     and it is what the `X11` desktop expects.  It does **not** work
 *     on Wayland.
 *   - **Wayland** — the compositor owns the input devices, and the
 *     supported path is a **uinput** device: the injector creates a
 *     virtual kernel input device (`/dev/uinput`) and writes the
 *     `input_event` structures to it.  The compositor then treats the
 *     events as coming from a real device.
 *
 * The backend picks XTest when an X server is reachable and uinput
 * otherwise, and reports which one it chose.  When neither is
 * available the backend is *not available* — the reason is reported
 * rather than the injector pretending to work.
 *
 * ## The privileges
 *
 * uinput needs write access to `/dev/uinput`, which is normally
 * restricted to the `input` group — and on most distributions that
 * requires the process to be root.  XTest needs an X display and
 * (with Xauthority) the right cookie.
 *
 * ## The keycode table
 *
 * The X11 keysyms and the Linux input codes are *not* the neutral
 * layout's codes, so the backend carries the table.  A key the table
 * does not know is refused (see `input_injector.hpp`).
 */

#pragma once

#if defined(__linux__)

#include <string>

#include <nimrtc/rd_input_inject/key_map.hpp>

#include "../../injector_base.hpp"

namespace nimrtc::rd_input_inject::detail {

/**
 * @brief The Linux input-event key code for a neutral key.
 *
 * These are the `KEY_*` constants from `linux/input-event-codes.h` —
 * the codes the kernel's input layer uses.
 *
 * @return the code, or 0 when the table does not know the key.
 */
int linux_keycode_for(NeutralKey key) noexcept;

/**
 * @brief The Linux button code for a mouse button.
 *
 * These are the `BTN_*` constants.
 *
 * @return the code, or 0 when the table does not know the button.
 */
int linux_button_code_for(rd_input::MouseButton button) noexcept;

/** The Linux backend. */
class LinuxInjector final : public InjectorBase {
public:
    LinuxInjector();
    ~LinuxInjector() override;

    [[nodiscard]] std::string_view backend_name() const noexcept override;
    [[nodiscard]] bool available() const noexcept override;
    [[nodiscard]] std::string unavailable_reason() const noexcept override;

protected:
    bool do_inject_mouse_move(std::int32_t dx, std::int32_t dy) override;
    bool do_inject_button(rd_input::MouseButton button,
                          rd_input::ButtonAction action) override;
    bool do_inject_wheel(std::int32_t dx, std::int32_t dy) override;
    bool do_inject_key(NeutralKey key, rd_input::KeyAction action) override;
    bool do_inject_text(const std::string& text) override;

private:
    /** Which backend this instance uses. */
    enum class Mode {
        kNone,     ///< no backend is usable
        kXTest,    ///< the X11 virtual-device extension
        kUinput,   ///< a virtual kernel input device
    };

    /** Open the uinput device (when the mode is `kUinput`). */
    bool open_uinput() noexcept;

    /** Write one `input_event` to the uinput device. */
    bool write_event(int type, int code, int value) noexcept;

    /** The reason the backend is unavailable (empty when available). */
    std::string unavailable_reason_;

    /** Which backend is in use. */
    Mode mode_ = Mode::kNone;

    /** The uinput file descriptor (-1 when uinput is not in use). */
    int uinput_fd_ = -1;
};

} // namespace nimrtc::rd_input_inject::detail

#endif // __linux__
