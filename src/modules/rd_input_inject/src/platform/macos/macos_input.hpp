/**
 * @file src/modules/rd_input_inject/src/platform/macos/macos_input.hpp
 * @brief The macOS input-injection backend (`CGEvent`).
 *
 * ## Why `CGEvent`
 *
 * macOS offers three ways to synthesise input:
 *
 *   - `CGEventPost` with a synthesised `CGEvent` — goes into the
 *     system event stream, so it goes through the same focus and
 *     accessibility checks as a physical event.
 *   - `NSEvent` + `-[NSApplication sendEvent:]` — delivers to *one
 *     application*, so it cannot reach a full-screen app or the
 *     Dock, and it does not compose with the physical keyboard state.
 *   - `IOHIDManager` — injects at the HID layer, below the window
 *     server.  It needs the `com.apple.security.device.hid` entitlement
 *     (an App Store signing requirement) and it bypasses the
 *     accessibility model, so it is not something a desktop agent can
 *     sign for.
 *
 * `CGEvent` it is.
 *
 * ## The accessibility permission
 *
 * `CGEventPost` requires the process to hold the
 * `com.apple.security.device.accessibility` permission (the
 * "Input Monitoring" / "Accessibility" prompt in System Settings).  A
 * process without it gets the events *silently dropped* — the call
 * "succeeds" and nothing happens.  The backend probes for the
 * permission with `AXIsProcessTrusted()` and reports it honestly
 * through `available()` / `unavailable_reason()`.
 *
 * ## The virtual-key table
 *
 * The macOS virtual keycodes are *not* the neutral layout's codes, so
 * the module carries the table -- in the portable header
 * `../../macos_keycodes.hpp`, so that it can be unit tested on every
 * CI leg.  A key the table does not know is refused (see
 * `input_injector.hpp`).
 */

#pragma once

#if defined(__APPLE__)

#include <CoreGraphics/CoreGraphics.h>

#include <string>

#include <nimrtc/rd_input_inject/key_map.hpp>

#include "../../injector_base.hpp"

namespace nimrtc::rd_input_inject::detail {

/** The macOS backend. */
class MacosInjector final : public InjectorBase {
public:
    MacosInjector();
    ~MacosInjector() override;

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return "macos-cgevent";
    }

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
    /** Post one event and report whether the system accepted it. */
    bool post(CGEventRef event) noexcept;

    /** The reason the backend is unavailable (empty when available). */
    std::string unavailable_reason_;
};

} // namespace nimrtc::rd_input_inject::detail

#endif // __APPLE__
