/**
 * @file src/modules/rd_input_inject/src/macos_keycodes.hpp
 * @brief The macOS virtual-keycode table and its neutral-key mapping.
 *
 * ## Why the table is a portable header
 *
 * The `kVK_*` constants live in the Carbon / HIToolbox `Events.h`,
 * which the modern SDKs ship only as a deprecated compatibility shim.
 * The IOKit HID event map does **not** define them either -- that
 * header carries the keymap *structures*, not the keycodes.  A module
 * that cannot name its own keycodes cannot build on a stock SDK.
 *
 * So the backend carries the table.  The table is *data*, not code: it
 * is plain integers with no Apple type in it, and the translation unit
 * that holds it is compiled on **every** platform.  That is what lets
 * the unit test (tests/test_rd_input_inject.cpp) verify the table on
 * every CI leg -- including the Windows and Linux ones, where the
 * macOS backend itself is never compiled.  A keycode table that can
 * only be checked on an Apple machine is a keycode table that rots
 * silently.
 *
 * ## The values
 *
 * These are the ANSI-layout *hardware* key positions, which is exactly
 * what `CGEventCreateKeyboardEvent` expects: the window server maps
 * them to characters through the active input source.  The platform
 * freezes them, so they cannot drift.
 *
 * ## The sentinel
 *
 * A key the table does not know is reported as `kMacVKUnknown`, which
 * is 0xFFFF and **not** 0 -- because 0x00 is the real keycode for
 * 'A'.  The two must not be confused: a confused 'A' would either be
 * refused outright, or (worse) be mistaken for the "no key" code and
 * inject nothing while claiming success.
 */

#pragma once

#include <cstdint>

#include <nimrtc/rd_input_inject/key_map.hpp>

namespace nimrtc::rd_input_inject::detail {

/** The macOS virtual keycode for a neutral key. */
using MacVKCode = std::uint16_t;

/** The "this table does not know the key" sentinel (NOT 0 -- see above). */
inline constexpr MacVKCode kMacVKUnknown = 0xFFFF;

/** The ANSI-layout hardware key positions. */
enum MacVK : MacVKCode {
    kMacVK_A = 0x00,
    kMacVK_S = 0x01,
    kMacVK_D = 0x02,
    kMacVK_F = 0x03,
    kMacVK_H = 0x04,
    kMacVK_G = 0x05,
    kMacVK_Z = 0x06,
    kMacVK_X = 0x07,
    kMacVK_C = 0x08,
    kMacVK_V = 0x09,
    kMacVK_B = 0x0B,
    kMacVK_Q = 0x0C,
    kMacVK_W = 0x0D,
    kMacVK_E = 0x0E,
    kMacVK_R = 0x0F,
    kMacVK_Y = 0x10,
    kMacVK_T = 0x11,
    kMacVK_O = 0x1F,
    kMacVK_U = 0x20,
    kMacVK_I = 0x22,
    kMacVK_P = 0x23,
    kMacVK_Return = 0x24,
    kMacVK_L = 0x25,
    kMacVK_J = 0x26,
    kMacVK_K = 0x28,
    kMacVK_N = 0x2D,
    kMacVK_M = 0x2E,
    kMacVK_Tab = 0x30,
    kMacVK_Space = 0x31,
    kMacVK_Delete = 0x33,
    kMacVK_Escape = 0x35,
    kMacVK_Command = 0x37,
    kMacVK_Shift = 0x38,
    kMacVK_Option = 0x3A,
    kMacVK_Control = 0x3B,
    kMacVK_F5 = 0x60,
    kMacVK_F6 = 0x61,
    kMacVK_F7 = 0x62,
    kMacVK_F3 = 0x63,
    kMacVK_F8 = 0x64,
    kMacVK_F9 = 0x65,
    kMacVK_F11 = 0x67,
    kMacVK_F10 = 0x6D,
    kMacVK_F12 = 0x6F,
    kMacVK_Home = 0x73,
    kMacVK_PageUp = 0x74,
    kMacVK_ForwardDelete = 0x75,
    kMacVK_F4 = 0x76,
    kMacVK_End = 0x77,
    kMacVK_F2 = 0x78,
    kMacVK_PageDown = 0x79,
    kMacVK_F1 = 0x7A,
    kMacVK_LeftArrow = 0x7B,
    kMacVK_RightArrow = 0x7C,
    kMacVK_DownArrow = 0x7D,
    kMacVK_UpArrow = 0x7E,
};

/**
 * @brief The macOS virtual keycode for a neutral key.
 *
 * @return the keycode, or `kMacVKUnknown` when the table does not know
 *         the key.  (0xFFFF and not 0, because 0x00 is the real code
 *         for 'A'.)
 */
MacVKCode macos_keycode_for(NeutralKey key) noexcept;

} // namespace nimrtc::rd_input_inject::detail
