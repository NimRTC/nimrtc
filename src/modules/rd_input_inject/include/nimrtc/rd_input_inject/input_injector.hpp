/**
 * @file nimrtc/rd_input_inject/input_injector.hpp
 * @brief The OS input-injection seam — the device side of a remote-desktop
 *        session actually driving the remote machine's mouse and keyboard.
 *
 * ## What this is for
 *
 * The `rd_input` module owns the *vocabulary* of an input event (the wire
 * format, the coalescer, the safety filter).  It does **not** own the
 * act of making the remote operating system believe that a key was pressed.
 * That is this header's job, and it is a separate module because the two
 * halves fail for entirely different reasons:
 *
 *   - the *vocabulary* half fails with a **wire** bug (a mis-encoded
 *     event, a lost message, a replayed one);
 *   - the *injection* half fails with an **OS** bug (the window is not
 *     focused, the session is on a secure desktop, the virtual key code
 *     does not exist on this layout, the UAC prompt ate the input).
 *
 * Keeping them apart means a CI leg that has no windowing system at all
 * can still test the vocabulary, and a leg that has no operator can
 * still test the injector.
 *
 * ## Why a module and not a seam
 *
 * `src/plugins/` holds interfaces only (AGENTS.md L2).  This is an
 * *interface* in the strict sense — the engine does not drive it, the
 * **example application** does — but the interface would have no second
 * implementation to abstract over, and putting it in `src/plugins/` would
 * imply the engine resolves it through the registry.  It does not: the
 * device-side example constructs the injector directly, because the
 * injector is a **product** concern (the plan's Track G / G7 territory),
 * not an engine capability.
 *
 * So this is a **module**: self-contained, with its own `CMakeLists.txt`,
 * `include/`, `src/` and `tests/`, depending only on `nimrtc::core` and
 * `nimrtc::rd_input` (the vocabulary it consumes).
 *
 * ## The platform dispatch
 *
 * Three backends, one per platform family:
 *
 * | platform | backend                        | header            |
 * |----------|--------------------------------|-------------------|
 * | Windows  | `SendInput` (user32)           | `windows.hpp`     |
 * | macOS    | `CGEvent` (ApplicationServices) | `macos.hpp`       |
 * | Linux    | `XTest` (libXtst) / uinput      | `linux.hpp`       |
 *
 * The dispatch happens **once**, in `create_input_injector()`, and the
 * portable half of the module — the *event translation* from the
 * vocabulary's `key_code` to the platform's virtual-key number, the
 * coordinate mapping, and the injection *policy* — is platform-neutral
 * and unit-tested on every CI leg.
 *
 * ## The virtual-key problem
 *
 * `rd_input::InputEvent::key_code` is an **opaque** `uint32_t`.  The
 * module's own header documents why: "a complete layout is a per-platform
 * decision (a macOS virtual keycode and a Windows virtual key are
 * different numbers, and neither is a USB HID usage id)".
 *
 * So the injector needs a **translation table** from the vocabulary's
 * codes to the platform's codes.  Two options exist:
 *
 *   1. The vocabulary's codes *are* the platform's codes.  Then the
 *      injector needs no table, but a Windows host and a macOS host
 *      cannot talk to each other — which defeats the purpose of a
 *      *portable* wire format.
 *   2. The vocabulary's codes are a **platform-neutral** layout, and each
 *      platform has a table.  Then the two hosts agree, and the table is
 *      the only platform-specific part.
 *
 * NimRTC takes option 2.  The `keys::` constants in `rd_input` are the
 * platform-neutral layout (a small, deliberately incomplete set — the
 * keys a remote-desktop safety policy most often needs to name), and
 * each backend carries the table that maps them onto its own virtual-key
 * numbers.  A key the table does not know is **refused**, not guessed:
 * an injected key with a wrong virtual-key number is a *wrong* keystroke,
 * which is worse than a missing one.
 *
 * @see src/modules/rd_input/include/nimrtc/rd_input/input_event.hpp
 * @see docs/plan/remote-desktop-plan.md §Track G
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <string>
#include <string_view>

#include <nimrtc/core/error.hpp>

#include "nimrtc/rd_input/input_event.hpp"

namespace nimrtc::rd_input_inject {

// ---------------------------------------------------------------------------
// The result of one injection
// ---------------------------------------------------------------------------

/**
 * @brief The outcome of one injection attempt.
 *
 * The distinction between "refused" and "failed" matters: a **refused**
 * event never reached the OS (the injector rejected it — an unknown key,
 * a forbidden combination, a missing table entry), while a **failed** one
 * did reach the OS and the OS said no.  The first is a configuration
 * problem, the second an environment problem.
 */
enum class NIMRTC_API_STABLE InjectionResult : std::uint8_t {
    /** The event was injected. */
    kInjected = 0,
    /** The injector refused the event before touching the OS. */
    kRefused = 1,
    /** The OS rejected the injection (or the platform has no backend). */
    kFailed = 2,
};

/** Human-readable result name (for logs and diagnostics). */
NIMRTC_API_STABLE inline std::string_view injection_result_name(
    InjectionResult r) noexcept {
    switch (r) {
        case InjectionResult::kInjected: return "injected";
        case InjectionResult::kRefused:  return "refused";
        case InjectionResult::kFailed:   return "failed";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// The counters
// ---------------------------------------------------------------------------

/** The injector's counters. */
struct NIMRTC_API_STABLE InjectionStats {
    /** The events injected successfully. */
    std::uint64_t injected = 0;
    /** The events refused by the injector itself. */
    std::uint64_t refused = 0;
    /** The events the OS rejected. */
    std::uint64_t failed = 0;
    /** The individual OS calls that failed (a batch may fail in part). */
    std::uint64_t os_errors = 0;
};

// ---------------------------------------------------------------------------
// The injector
// ---------------------------------------------------------------------------

/**
 * @brief The OS input injector.
 *
 * ## Threading
 *
 * **Not** thread-safe.  The injector is driven from the host's control
 * loop — one thread, one event at a time — which is the same discipline
 * the `rd_input` module's coalescer and the engine's control channel
 * follow.  A host that captures input on one thread and injects on
 * another must put a queue between them.
 *
 * ## The safe state
 *
 * The injector owns the *state* of the injected input: which keys are
 * currently down, which mouse buttons are currently down, and where the
 * pointer is.  That state is what makes the **release-everything** path
 * possible — the dead-man switch (plan G6) fires, and the remote side
 * must not be left with a key stuck down.
 */
class NIMRTC_API_STABLE InputInjector {
public:
    virtual ~InputInjector() = default;

    // ---- The identity -------------------------------------------------------

    /** The backend's name ("windows-sendinput", "macos-cgevent", ...). */
    [[nodiscard]] virtual std::string_view backend_name() const noexcept = 0;

    /** Whether the backend is usable on this host right now.
     *
     * A backend can be *compiled in* but not *usable*: the macOS
     * backend needs accessibility permission, the Linux backend needs a
     * reachable X server (or a writable `/dev/uinput`), and the Windows
     * backend needs a session that is not the Winlogon one. */
    [[nodiscard]] virtual bool available() const noexcept = 0;

    /** Why the backend is unavailable (empty when it is available). */
    [[nodiscard]] virtual std::string unavailable_reason() const noexcept = 0;

    // ---- The injection ------------------------------------------------------

    /**
     * @brief Inject one event.
     *
     * @param event  the event to inject.
     * @return what happened.
     *
     * @note The injector tracks the pressed keys / buttons so that
     *       `release_all()` can undo them.  An event that the injector
     *       *refuses* does not change that state.
     */
    virtual InjectionResult inject(const rd_input::InputEvent& event) = 0;

    /**
     * @brief Release every key and button the injector believes is down.
     *
     * This is the **safe state** the dead-man switch enters.  It is
     * idempotent, and it is the one call that must work even when the
     * session is otherwise broken — so it never refuses and never fails
     * hard: it reports how many keys it released.
     *
     * @return the number of keys and buttons released.
     */
    virtual std::size_t release_all() = 0;

    // ---- The state ----------------------------------------------------------

    /** The injector's counters. */
    [[nodiscard]] virtual InjectionStats stats() const noexcept = 0;

    /** How many keys the injector believes are currently down. */
    [[nodiscard]] virtual std::size_t keys_down() const noexcept = 0;

    /** How many mouse buttons the injector believes are currently down. */
    [[nodiscard]] virtual std::size_t buttons_down() const noexcept = 0;

    /**
     * @brief The pointer's absolute position, in the platform's own units.
     *
     * On Windows this is a *virtual screen* coordinate (which may be
     * negative on a multi-monitor layout); on macOS it is a Cocoa
     * point; on Linux it is a root-window pixel.  The values are only
     * meaningful relative to the same platform.
     */
    [[nodiscard]] virtual std::int32_t pointer_x() const noexcept = 0;
    [[nodiscard]] virtual std::int32_t pointer_y() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// The factory
// ---------------------------------------------------------------------------

/**
 * @brief The backend that fits this host.
 *
 * The dispatch is **compile-time** (the `#if` in the implementation) plus
 * a runtime availability check, because the macOS and Linux backends can
 * be compiled in and still be unusable at run time.
 *
 * @return the injector, or nullptr when this platform has no backend at
 *         all (which cannot happen on a supported platform — the stub
 *         backend is always available and always refuses).
 */
NIMRTC_API_STABLE InputInjector* create_input_injector();

/**
 * @brief The name of the backend `create_input_injector()` would pick.
 *
 * Exposed separately so a host can *report* the backend without
 * constructing it (and so the command line can print it).
 */
NIMRTC_API_STABLE std::string_view default_injector_backend_name() noexcept;

} // namespace nimrtc::rd_input_inject
