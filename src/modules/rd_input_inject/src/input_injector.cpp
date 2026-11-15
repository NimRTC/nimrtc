/**
 * @file src/modules/rd_input_inject/src/input_injector.cpp
 * @brief The platform dispatch — `create_input_injector()`.
 *
 * ## The dispatch
 *
 * The choice is **compile-time** (the `#if` below) plus a **run-time**
 * availability check, because a backend can be compiled in and still be
 * unusable:
 *
 *   - the macOS backend needs the Accessibility permission;
 *   - the Linux backend needs XTEST *or* a writable `/dev/uinput`;
 *   - the Windows backend needs an interactive session.
 *
 * When the platform's backend is not usable, the **stub** is returned
 * with the platform's reason attached.  That way a host gets a
 * non-null injector whose `available()` is false and whose
 * `unavailable_reason()` says what is missing — which is exactly what
 * a product needs to tell the operator "input control needs the
 * Accessibility permission".
 */

#include <nimrtc/rd_input_inject/input_injector.hpp>

#include "stub_injector.hpp"

#ifdef _WIN32
#include "platform/windows/windows_input.hpp"
#elif defined(__APPLE__)
#include "platform/macos/macos_input.hpp"
#elif defined(__linux__)
#include "platform/linux/linux_input.hpp"
#endif

namespace nimrtc::rd_input_inject {

InputInjector* create_input_injector() {
#ifdef _WIN32
    auto* injector = new detail::WindowsInjector();
    if (injector->available()) return injector;
    // The Windows backend exists but cannot inject here (session 0, a
    // secure desktop, ...).  The stub carries the reason.
    const std::string reason = injector->unavailable_reason();
    delete injector;
    return new detail::StubInjector(reason);
#elif defined(__APPLE__)
    auto* injector = new detail::MacosInjector();
    if (injector->available()) return injector;
    const std::string reason = injector->unavailable_reason();
    delete injector;
    return new detail::StubInjector(reason);
#elif defined(__linux__)
    auto* injector = new detail::LinuxInjector();
    if (injector->available()) return injector;
    const std::string reason = injector->unavailable_reason();
    delete injector;
    return new detail::StubInjector(reason);
#else
    return new detail::StubInjector(
        "this platform has no input-injection backend");
#endif
}

std::string_view default_injector_backend_name() noexcept {
#ifdef _WIN32
    return "windows-sendinput";
#elif defined(__APPLE__)
    return "macos-cgevent";
#elif defined(__linux__)
    return "linux-xtest / linux-uinput";
#else
    return "stub";
#endif
}

} // namespace nimrtc::rd_input_inject
