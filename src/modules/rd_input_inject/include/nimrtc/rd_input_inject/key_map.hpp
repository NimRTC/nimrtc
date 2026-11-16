/**
 * @file nimrtc/rd_input_inject/key_map.hpp
 * @brief The platform-neutral key layout and its translation to the
 *        platform's own virtual-key numbers.
 *
 * ## Why this file exists
 *
 * `rd_input::InputEvent::key_code` is an opaque `uint32_t`, and the
 * vocabulary's own header explains why: a macOS virtual keycode, a
 * Windows virtual key and a USB HID usage id are three different
 * numbers, and a portable wire format cannot pick one of them.
 *
 * So the wire format carries a **platform-neutral** key code, and each
 * platform needs a table that maps it onto its own numbers.  This file
 * owns the *portable* half of that arrangement:
 *
 *   - `NeutralKey` — the platform-neutral layout: the full set of keys
 *     the injector knows, in one enumeration;
 *   - `neutral_key_of()` — the mapping from the vocabulary's `key_code`
 *     to a `NeutralKey`;
 *   - `is_neutral_key_known()` / `neutral_key_name()` — the
 *     introspection a host needs to report what it supports.
 *
 * The *platform* half — the `NeutralKey` → the platform's number table
 * — lives next to the backend that uses it (`windows.hpp`, `macos.hpp`,
 * `linux.hpp`).
 *
 * ## The rule for an unknown key
 *
 * A key the table does not know is **refused**, never guessed.  An
 * injected key with a wrong virtual-key number is a *wrong* keystroke
 * on the remote machine — it types a character the operator never
 * pressed, or closes a window they never opened.  A refused key is
 * merely a missing one, which is a much smaller harm.
 *
 * @see nimrtc/rd_input_inject/input_injector.hpp
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace nimrtc::rd_input_inject {

/**
 * @brief The platform-neutral key layout.
 *
 * The values are the **vocabulary's** codes — the same numbers the
 * `rd_input::keys::` constants use — so a `NeutralKey` and a
 * `rd_input::keys::` constant denote the same key.  The enumeration
 * exists so that a *table* can be written against a closed set (the
 * compiler then catches a missing case), not so that the wire format
 * can change.
 */
enum class NIMRTC_API_STABLE NeutralKey : std::uint16_t {
    kNone = 0,

    // ---- The editing / navigation keys --------------------------------
    kEscape = 1,
    kTab = 2,
    kEnter = 3,
    kSpace = 4,
    kBackspace = 5,
    kDelete = 6,
    kHome = 7,
    kEnd = 8,
    kPageUp = 9,
    kPageDown = 10,
    kLeft = 11,
    kUp = 12,
    kRight = 13,
    kDown = 14,

    // ---- The modifiers --------------------------------------------------
    kShift = 15,
    kControl = 16,
    kAlt = 17,
    kMeta = 18,

    // ---- The function keys ----------------------------------------------
    kF1 = 19,
    kF2 = 20,
    kF3 = 21,
    kF4 = 22,
    kF5 = 23,
    kF6 = 24,
    kF7 = 25,
    kF8 = 26,
    kF9 = 27,
    kF10 = 28,
    kF11 = 29,
    kF12 = 30,

    // ---- The printable letters -------------------------------------------
    kA = 100,
    kB = 101,
    kC = 102,
    kD = 103,
    kE = 104,
    kF = 105,
    kG = 106,
    kH = 107,
    kI = 108,
    kJ = 109,
    kK = 110,
    kL = 111,
    kM = 112,
    kN = 113,
    kO = 114,
    kP = 115,
    kQ = 116,
    kR = 117,
    kS = 118,
    kT = 119,
    kU = 120,
    kV = 121,
    kW = 122,
    kX = 123,
    kY = 124,
    kZ = 125,
};

/**
 * @brief Map the vocabulary's `key_code` onto the neutral layout.
 *
 * @return the neutral key, or `std::nullopt` when the code is not one
 *         the layout knows.  The digits and the punctuation are
 *         **not** in the layout: they are reachable through the *text*
 *         event kind, which carries the committed characters directly
 *         and therefore needs no key table at all.
 */
NIMRTC_API_STABLE std::optional<NeutralKey>
neutral_key_of(std::uint32_t key_code) noexcept;

/** True when the layout knows the code. */
NIMRTC_API_STABLE bool is_neutral_key_known(std::uint32_t key_code) noexcept;

/** The neutral layout's name for a key ("" when unknown). */
NIMRTC_API_STABLE std::string_view neutral_key_name(NeutralKey key) noexcept;

/** The number of keys the neutral layout knows. */
NIMRTC_API_STABLE std::size_t neutral_key_count() noexcept;

} // namespace nimrtc::rd_input_inject
