/**
 * @file src/modules/rd_input_inject/src/linux_input_codes.hpp
 * @brief The Linux input-layer codes and the X11 keysyms, side by side.
 *
 * ## Why the table is a portable header
 *
 * The Linux backend has **two** injection backends, and they speak
 * **two different vocabularies**:
 *
 *   - the **uinput** path writes `input_event` structures into the
 *     kernel, so it needs the kernel's own codes (`KEY_A`, `BTN_LEFT`
 *     from `linux/input-event-codes.h`);
 *   - the **XTest** path asks the X server to fake an event, so it
 *     needs the X server's own vocabulary: a **KeySym** for a key
 *     (`XK_a`, `XK_Return`) and a **button number** for a button
 *     (`1` = left, `2` = middle, `3` = right).
 *
 * The two numbering schemes are unrelated.  `KEY_A` is 30 and `XK_a`
 * is 0x61; `BTN_LEFT` is 0x110 and the X11 left button is `1`.
 * Passing one to the other does not fail loudly — the X server
 * receives a key it does not recognise and injects *nothing*, or
 * injects the *wrong* key.  The device then reports success while
 * the operator's keystrokes go nowhere.
 *
 * The X11 side goes one step further: the X server does not take a
 * KeySym either, but a **keycode** — the number it assigned to a
 * physical key on the *active* layout.  The KeySym is what the table
 * carries (it is layout-independent and therefore testable), and
 * `XKeysymToKeycode()` does the layout-aware conversion at the call
 * site.
 *
 * The values therefore live in a table that carries **no platform
 * type**: plain integers, compiled on **every** platform.  That is
 * what lets the unit test (`tests/test_rd_input_inject.cpp`) verify
 * the table on every CI leg -- including the Windows one, where the
 * Linux backend is never compiled.  A table that could only be
 * checked on a Linux machine is a table that rots silently between
 * two Linux machines.
 *
 * ## Where the values come from
 *
 * They are transcribed from the platform headers, not from memory:
 *
 *   - the kernel codes from `linux/input-event-codes.h`;
 *   - the keysyms from `X11/keysymdef.h`.
 *
 * Both are frozen by their platform, so they cannot drift, and the
 * unit test spells the expected values out literally so that a wrong
 * *entry* is caught rather than merely a change.
 *
 * ## The sentinel
 *
 * A key or a button the table does not know is reported as `0`, which
 * is `KEY_RESERVED`, `NoSymbol` and "no button" respectively.  None
 * of the entries below is `0`, so the two cannot be confused.
 */

#pragma once

#include <cstdint>

#include <nimrtc/rd_input/input_event.hpp>
#include <nimrtc/rd_input_inject/key_map.hpp>

namespace nimrtc::rd_input_inject::detail {

/** The Linux input-layer key code (the kernel's `KEY_*`). */
using LinuxKeyCode = std::uint16_t;

/** The Linux input-layer button code (the kernel's `BTN_*`). */
using LinuxButtonCode = std::uint16_t;

/** An X11 KeySym (the `XK_*` constant). */
using X11KeySym = std::uint32_t;

/** An X11 button number (`1` = left, `2` = middle, `3` = right, ...). */
using X11ButtonNumber = unsigned int;

/** "This table does not know the key" (see the sentinel note above). */
inline constexpr LinuxKeyCode kLinuxKeyUnknown = 0;

/** "This table does not know the button." */
inline constexpr LinuxButtonCode kLinuxButtonUnknown = 0;

/** "This table does not know the key." */
inline constexpr X11KeySym kX11KeySymUnknown = 0;

/** "This table does not know the button." */
inline constexpr X11ButtonNumber kX11ButtonUnknown = 0;

// ---------------------------------------------------------------------------
// The uinput vocabulary
// ---------------------------------------------------------------------------

/**
 * @brief The kernel input-layer key code for a neutral key.
 *
 * These are the `KEY_*` constants from `linux/input-event-codes.h` --
 * the codes the kernel's input layer uses.
 *
 * @return the code, or `kLinuxKeyUnknown` when the table does not know
 *         the key.
 */
LinuxKeyCode linux_keycode_for(NeutralKey key) noexcept;

/**
 * @brief The kernel input-layer button code for a mouse button.
 *
 * These are the `BTN_*` constants.
 *
 * @return the code, or `kLinuxButtonUnknown` when the table does not
 *         know the button.
 */
LinuxButtonCode linux_button_code_for(rd_input::MouseButton button) noexcept;

/**
 * @brief The kernel input-layer key code for a printable ASCII character.
 *
 * The mapping is the **US layout's**: the key that carries `-` on a US
 * keyboard is `KEY_MINUS`, whatever that key produces on the layout
 * that is actually active.  The Linux kernel's input layer has no
 * "unicode string" event, so a layout-dependent mapping is
 * unavoidable on this path.
 *
 * @return the code, or `kLinuxKeyUnknown` when the character has no
 *         key on the US layout.
 */
LinuxKeyCode linux_keycode_for_ascii(char ch) noexcept;

// ---------------------------------------------------------------------------
// The XTest vocabulary
// ---------------------------------------------------------------------------

/**
 * @brief The X11 KeySym for a neutral key.
 *
 * These are the `XK_*` constants from `X11/keysymdef.h` -- the X
 * server's *symbolic* name for a key ("the key that produces 'a'").
 *
 * The X server does not take a KeySym directly: `XTestFakeKeyEvent()`
 * wants a **keycode**, the number the server assigned to a physical
 * key on the *active* layout.  `XKeysymToKeycode()` is the mapping,
 * and it is layout-aware -- the same KeySym lands on a different
 * keycode on a different layout.  The KeySym is therefore what the
 * backend asks for, and the conversion happens at the call site.
 *
 * The KeySym is **not** the kernel's key code: `KEY_A` is 30 and
 * `XK_a` is 0x61, and the two numbering schemes are unrelated.
 *
 * @return the KeySym, or `kX11KeySymUnknown` when the table does not
 *         know the key.
 */
X11KeySym x11_keysym_for(NeutralKey key) noexcept;

/**
 * @brief The X11 button number for a mouse button.
 *
 * The X server numbers the buttons `1` (left), `2` (middle),
 * `3` (right), `4` and `5` (the wheel) and, conventionally, `8` and
 * `9` (back and forward).  These are the numbers
 * `XTestFakeButtonEvent()` expects -- not the kernel's `BTN_*` codes.
 *
 * @return the button number, or `kX11ButtonUnknown` when the table
 *         does not know the button.
 */
X11ButtonNumber x11_button_number_for(rd_input::MouseButton button) noexcept;

/**
 * @brief The X11 KeySym for a printable ASCII character.
 *
 * For the Latin-1 range the X11 KeySym **is** the character's code
 * point, so the mapping is the identity -- with the two control
 * characters that are *not* Latin-1 spelled out (`\n` is `XK_Return`
 * and `\t` is `XK_Tab`).
 *
 * @return the KeySym, or `kX11KeySymUnknown` when the character has
 *         no KeySym on this path.
 */
X11KeySym x11_keysym_for_ascii(char ch) noexcept;

} // namespace nimrtc::rd_input_inject::detail
