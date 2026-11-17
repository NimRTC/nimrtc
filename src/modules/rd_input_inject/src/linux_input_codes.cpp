/**
 * @file src/modules/rd_input_inject/src/linux_input_codes.cpp
 * @brief The Linux input-layer codes and the X11 keysyms.
 *
 * The values are transcribed from the platform headers (see the header
 * for why).  They are plain integers here, with no platform type, so
 * this translation unit compiles on every platform -- which is what
 * lets the unit test verify the table on every CI leg.
 */

#include "linux_input_codes.hpp"

namespace nimrtc::rd_input_inject::detail {

// ---------------------------------------------------------------------------
// The uinput vocabulary -- the kernel's `KEY_*` codes.
// ---------------------------------------------------------------------------
//
// Transcribed from `linux/input-event-codes.h`.  These are the
// *hardware* key positions of a US layout: `KEY_A` is 30 because the
// kernel numbers the keys in the order they sit on the keyboard, not
// in the order of any character set.

LinuxKeyCode linux_keycode_for(NeutralKey key) noexcept {
    switch (key) {
        case NeutralKey::kEscape:    return 1;    // KEY_ESC
        case NeutralKey::kTab:       return 15;   // KEY_TAB
        case NeutralKey::kEnter:     return 28;   // KEY_ENTER
        case NeutralKey::kSpace:     return 57;   // KEY_SPACE
        case NeutralKey::kBackspace: return 14;   // KEY_BACKSPACE
        case NeutralKey::kDelete:    return 111;  // KEY_DELETE
        case NeutralKey::kHome:      return 102;  // KEY_HOME
        case NeutralKey::kEnd:       return 107;  // KEY_END
        case NeutralKey::kPageUp:    return 104;  // KEY_PAGEUP
        case NeutralKey::kPageDown:  return 109;  // KEY_PAGEDOWN
        case NeutralKey::kLeft:      return 105;  // KEY_LEFT
        case NeutralKey::kUp:        return 103;  // KEY_UP
        case NeutralKey::kRight:     return 106;  // KEY_RIGHT
        case NeutralKey::kDown:      return 108;  // KEY_DOWN
        case NeutralKey::kShift:     return 42;   // KEY_LEFTSHIFT
        case NeutralKey::kControl:   return 29;   // KEY_LEFTCTRL
        case NeutralKey::kAlt:       return 56;   // KEY_LEFTALT
        case NeutralKey::kMeta:      return 125;  // KEY_LEFTMETA

        case NeutralKey::kF1:  return 59;  // KEY_F1
        case NeutralKey::kF2:  return 60;  // KEY_F2
        case NeutralKey::kF3:  return 61;  // KEY_F3
        case NeutralKey::kF4:  return 62;  // KEY_F4
        case NeutralKey::kF5:  return 63;  // KEY_F5
        case NeutralKey::kF6:  return 64;  // KEY_F6
        case NeutralKey::kF7:  return 65;  // KEY_F7
        case NeutralKey::kF8:  return 66;  // KEY_F8
        case NeutralKey::kF9:  return 67;  // KEY_F9
        case NeutralKey::kF10: return 68;  // KEY_F10
        case NeutralKey::kF11: return 87;  // KEY_F11
        case NeutralKey::kF12: return 88;  // KEY_F12

        case NeutralKey::kA: return 30;  // KEY_A
        case NeutralKey::kB: return 48;  // KEY_B
        case NeutralKey::kC: return 46;  // KEY_C
        case NeutralKey::kD: return 32;  // KEY_D
        case NeutralKey::kE: return 18;  // KEY_E
        case NeutralKey::kF: return 33;  // KEY_F
        case NeutralKey::kG: return 34;  // KEY_G
        case NeutralKey::kH: return 35;  // KEY_H
        case NeutralKey::kI: return 23;  // KEY_I
        case NeutralKey::kJ: return 36;  // KEY_J
        case NeutralKey::kK: return 37;  // KEY_K
        case NeutralKey::kL: return 38;  // KEY_L
        case NeutralKey::kM: return 50;  // KEY_M
        case NeutralKey::kN: return 49;  // KEY_N
        case NeutralKey::kO: return 24;  // KEY_O
        case NeutralKey::kP: return 25;  // KEY_P
        case NeutralKey::kQ: return 16;  // KEY_Q
        case NeutralKey::kR: return 19;  // KEY_R
        case NeutralKey::kS: return 31;  // KEY_S
        case NeutralKey::kT: return 20;  // KEY_T
        case NeutralKey::kU: return 22;  // KEY_U
        case NeutralKey::kV: return 47;  // KEY_V
        case NeutralKey::kW: return 17;  // KEY_W
        case NeutralKey::kX: return 45;  // KEY_X
        case NeutralKey::kY: return 21;  // KEY_Y
        case NeutralKey::kZ: return 44;  // KEY_Z

        case NeutralKey::kNone: break;
    }
    return kLinuxKeyUnknown;
}

LinuxButtonCode linux_button_code_for(rd_input::MouseButton button) noexcept {
    switch (button) {
        case rd_input::MouseButton::kLeft:    return 0x110;  // BTN_LEFT
        case rd_input::MouseButton::kRight:   return 0x111;  // BTN_RIGHT
        case rd_input::MouseButton::kMiddle:  return 0x112;  // BTN_MIDDLE
        case rd_input::MouseButton::kBack:    return 0x113;  // BTN_SIDE
        case rd_input::MouseButton::kForward: return 0x114;  // BTN_EXTRA
        case rd_input::MouseButton::kUnknown: break;
    }
    return kLinuxButtonUnknown;
}

LinuxKeyCode linux_keycode_for_ascii(char ch) noexcept {
    // The US layout's key positions.  These are the `KEY_*` codes of
    // the characters that are typed without a modifier.
    switch (ch) {
        case 'a':  return 30;   // KEY_A
        case 'b':  return 48;   // KEY_B
        case 'c':  return 46;   // KEY_C
        case 'd':  return 32;   // KEY_D
        case 'e':  return 18;   // KEY_E
        case 'f':  return 33;   // KEY_F
        case 'g':  return 34;   // KEY_G
        case 'h':  return 35;   // KEY_H
        case 'i':  return 23;   // KEY_I
        case 'j':  return 36;   // KEY_J
        case 'k':  return 37;   // KEY_K
        case 'l':  return 38;   // KEY_L
        case 'm':  return 50;   // KEY_M
        case 'n':  return 49;   // KEY_N
        case 'o':  return 24;   // KEY_O
        case 'p':  return 25;   // KEY_P
        case 'q':  return 16;   // KEY_Q
        case 'r':  return 19;   // KEY_R
        case 's':  return 31;   // KEY_S
        case 't':  return 20;   // KEY_T
        case 'u':  return 22;   // KEY_U
        case 'v':  return 47;   // KEY_V
        case 'w':  return 17;   // KEY_W
        case 'x':  return 45;   // KEY_X
        case 'y':  return 21;   // KEY_Y
        case 'z':  return 44;   // KEY_Z
        case '0':  return 11;   // KEY_0
        case '1':  return 2;    // KEY_1
        case '2':  return 3;    // KEY_2
        case '3':  return 4;    // KEY_3
        case '4':  return 5;    // KEY_4
        case '5':  return 6;    // KEY_5
        case '6':  return 7;    // KEY_6
        case '7':  return 8;    // KEY_7
        case '8':  return 9;    // KEY_8
        case '9':  return 10;   // KEY_9
        case ' ':  return 57;   // KEY_SPACE
        case '\n': return 28;   // KEY_ENTER
        case '\r': return 28;   // KEY_ENTER
        case '\t': return 15;   // KEY_TAB
        case '-':  return 12;   // KEY_MINUS
        case '=':  return 13;   // KEY_EQUAL
        case '[':  return 26;   // KEY_LEFTBRACE
        case ']':  return 27;   // KEY_RIGHTBRACE
        case ';':  return 39;   // KEY_SEMICOLON
        case '\'': return 40;   // KEY_APOSTROPHE
        case '`':  return 41;   // KEY_GRAVE
        case '\\': return 43;   // KEY_BACKSLASH
        case ',':  return 51;   // KEY_COMMA
        case '.':  return 52;   // KEY_DOT
        case '/':  return 53;   // KEY_SLASH
        default: break;
    }
    return kLinuxKeyUnknown;
}

// ---------------------------------------------------------------------------
// The XTest vocabulary -- the X server's `XK_*` keysyms.
// ---------------------------------------------------------------------------
//
// Transcribed from `X11/keysymdef.h`.  The letters are the Latin-1
// character codes (the X11 rule: for Latin-1, the KeySym *is* the
// character), and the named keys are the ones the X server defines
// above the Latin-1 range.

X11KeySym x11_keysym_for(NeutralKey key) noexcept {
    switch (key) {
        case NeutralKey::kEscape:    return 0xFF1B;  // XK_Escape
        case NeutralKey::kTab:       return 0xFF09;  // XK_Tab
        case NeutralKey::kEnter:     return 0xFF0D;  // XK_Return
        case NeutralKey::kSpace:     return 0x0020;  // XK_space
        case NeutralKey::kBackspace: return 0xFF08;  // XK_BackSpace
        case NeutralKey::kDelete:    return 0xFFFF;  // XK_Delete
        case NeutralKey::kHome:      return 0xFF50;  // XK_Home
        case NeutralKey::kEnd:       return 0xFF57;  // XK_End
        // X11 spells "page up" and "page down" as "prior" and "next".
        case NeutralKey::kPageUp:    return 0xFF55;  // XK_Prior
        case NeutralKey::kPageDown:  return 0xFF56;  // XK_Next
        case NeutralKey::kLeft:      return 0xFF51;  // XK_Left
        case NeutralKey::kUp:        return 0xFF52;  // XK_Up
        case NeutralKey::kRight:     return 0xFF53;  // XK_Right
        case NeutralKey::kDown:      return 0xFF54;  // XK_Down
        case NeutralKey::kShift:     return 0xFFE1;  // XK_Shift_L
        case NeutralKey::kControl:   return 0xFFE3;  // XK_Control_L
        case NeutralKey::kAlt:       return 0xFFE9;  // XK_Alt_L
        case NeutralKey::kMeta:      return 0xFFEB;  // XK_Super_L

        case NeutralKey::kF1:  return 0xFFBE;  // XK_F1
        case NeutralKey::kF2:  return 0xFFBF;  // XK_F2
        case NeutralKey::kF3:  return 0xFFC0;  // XK_F3
        case NeutralKey::kF4:  return 0xFFC1;  // XK_F4
        case NeutralKey::kF5:  return 0xFFC2;  // XK_F5
        case NeutralKey::kF6:  return 0xFFC3;  // XK_F6
        case NeutralKey::kF7:  return 0xFFC4;  // XK_F7
        case NeutralKey::kF8:  return 0xFFC5;  // XK_F8
        case NeutralKey::kF9:  return 0xFFC6;  // XK_F9
        case NeutralKey::kF10: return 0xFFC7;  // XK_F10
        case NeutralKey::kF11: return 0xFFC8;  // XK_F11
        case NeutralKey::kF12: return 0xFFC9;  // XK_F12

        case NeutralKey::kA: return 0x0061;  // XK_a
        case NeutralKey::kB: return 0x0062;  // XK_b
        case NeutralKey::kC: return 0x0063;  // XK_c
        case NeutralKey::kD: return 0x0064;  // XK_d
        case NeutralKey::kE: return 0x0065;  // XK_e
        case NeutralKey::kF: return 0x0066;  // XK_f
        case NeutralKey::kG: return 0x0067;  // XK_g
        case NeutralKey::kH: return 0x0068;  // XK_h
        case NeutralKey::kI: return 0x0069;  // XK_i
        case NeutralKey::kJ: return 0x006A;  // XK_j
        case NeutralKey::kK: return 0x006B;  // XK_k
        case NeutralKey::kL: return 0x006C;  // XK_l
        case NeutralKey::kM: return 0x006D;  // XK_m
        case NeutralKey::kN: return 0x006E;  // XK_n
        case NeutralKey::kO: return 0x006F;  // XK_o
        case NeutralKey::kP: return 0x0070;  // XK_p
        case NeutralKey::kQ: return 0x0071;  // XK_q
        case NeutralKey::kR: return 0x0072;  // XK_r
        case NeutralKey::kS: return 0x0073;  // XK_s
        case NeutralKey::kT: return 0x0074;  // XK_t
        case NeutralKey::kU: return 0x0075;  // XK_u
        case NeutralKey::kV: return 0x0076;  // XK_v
        case NeutralKey::kW: return 0x0077;  // XK_w
        case NeutralKey::kX: return 0x0078;  // XK_x
        case NeutralKey::kY: return 0x0079;  // XK_y
        case NeutralKey::kZ: return 0x007A;  // XK_z

        case NeutralKey::kNone: break;
    }
    return kX11KeySymUnknown;
}

X11ButtonNumber x11_button_number_for(rd_input::MouseButton button) noexcept {
    switch (button) {
        // The X server's conventional button numbering.  Buttons 4 and
        // 5 are the wheel (up and down), and 8 and 9 are back and
        // forward on the usual mice.
        case rd_input::MouseButton::kLeft:    return 1;
        case rd_input::MouseButton::kMiddle:  return 2;
        case rd_input::MouseButton::kRight:   return 3;
        case rd_input::MouseButton::kBack:    return 8;
        case rd_input::MouseButton::kForward: return 9;
        case rd_input::MouseButton::kUnknown: break;
    }
    return kX11ButtonUnknown;
}

X11KeySym x11_keysym_for_ascii(char ch) noexcept {
    switch (ch) {
        // The two control characters that are typed as text and are
        // *not* their own Latin-1 code point.
        case '\n': return 0xFF0D;  // XK_Return
        case '\r': return 0xFF0D;  // XK_Return
        case '\t': return 0xFF09;  // XK_Tab
        default: break;
    }

    // For Latin-1 the KeySym is the character code.  Anything outside
    // the printable ASCII range has no KeySym on this path.
    const auto code = static_cast<unsigned char>(ch);
    if (code < 0x20 || code > 0x7E) return kX11KeySymUnknown;
    return static_cast<X11KeySym>(code);
}

} // namespace nimrtc::rd_input_inject::detail
