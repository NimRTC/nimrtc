/**
 * @file src/modules/rd_input_inject/src/macos_keycodes.cpp
 * @brief The neutral-key -> macOS-keycode mapping.
 *
 * ## Why this file is compiled on every platform
 *
 * The mapping is pure data: a `switch` over the neutral layout that
 * returns a table entry.  Nothing in it is Apple-specific, so it is
 * compiled everywhere -- which is what lets the unit test verify the
 * table on the Windows and Linux legs too, where the macOS backend
 * itself is never compiled.
 *
 * The alternative -- hiding the mapping inside the Apple-only backend
 * file -- makes the table untestable off an Apple machine, and a table
 * that can only be checked on one platform rots silently.
 */

#include "macos_keycodes.hpp"

namespace nimrtc::rd_input_inject::detail {

MacVKCode macos_keycode_for(NeutralKey key) noexcept {
    switch (key) {
        case NeutralKey::kEscape:    return kMacVK_Escape;
        case NeutralKey::kTab:       return kMacVK_Tab;
        case NeutralKey::kEnter:     return kMacVK_Return;
        case NeutralKey::kSpace:     return kMacVK_Space;
        case NeutralKey::kBackspace: return kMacVK_Delete;
        case NeutralKey::kDelete:    return kMacVK_ForwardDelete;
        case NeutralKey::kHome:      return kMacVK_Home;
        case NeutralKey::kEnd:       return kMacVK_End;
        case NeutralKey::kPageUp:    return kMacVK_PageUp;
        case NeutralKey::kPageDown:  return kMacVK_PageDown;
        case NeutralKey::kLeft:      return kMacVK_LeftArrow;
        case NeutralKey::kUp:        return kMacVK_UpArrow;
        case NeutralKey::kRight:     return kMacVK_RightArrow;
        case NeutralKey::kDown:      return kMacVK_DownArrow;
        case NeutralKey::kShift:     return kMacVK_Shift;
        case NeutralKey::kControl:   return kMacVK_Control;
        case NeutralKey::kAlt:       return kMacVK_Option;
        case NeutralKey::kMeta:      return kMacVK_Command;

        case NeutralKey::kF1:  return kMacVK_F1;
        case NeutralKey::kF2:  return kMacVK_F2;
        case NeutralKey::kF3:  return kMacVK_F3;
        case NeutralKey::kF4:  return kMacVK_F4;
        case NeutralKey::kF5:  return kMacVK_F5;
        case NeutralKey::kF6:  return kMacVK_F6;
        case NeutralKey::kF7:  return kMacVK_F7;
        case NeutralKey::kF8:  return kMacVK_F8;
        case NeutralKey::kF9:  return kMacVK_F9;
        case NeutralKey::kF10: return kMacVK_F10;
        case NeutralKey::kF11: return kMacVK_F11;
        case NeutralKey::kF12: return kMacVK_F12;

        case NeutralKey::kA: return kMacVK_A;
        case NeutralKey::kB: return kMacVK_B;
        case NeutralKey::kC: return kMacVK_C;
        case NeutralKey::kD: return kMacVK_D;
        case NeutralKey::kE: return kMacVK_E;
        case NeutralKey::kF: return kMacVK_F;
        case NeutralKey::kG: return kMacVK_G;
        case NeutralKey::kH: return kMacVK_H;
        case NeutralKey::kI: return kMacVK_I;
        case NeutralKey::kJ: return kMacVK_J;
        case NeutralKey::kK: return kMacVK_K;
        case NeutralKey::kL: return kMacVK_L;
        case NeutralKey::kM: return kMacVK_M;
        case NeutralKey::kN: return kMacVK_N;
        case NeutralKey::kO: return kMacVK_O;
        case NeutralKey::kP: return kMacVK_P;
        case NeutralKey::kQ: return kMacVK_Q;
        case NeutralKey::kR: return kMacVK_R;
        case NeutralKey::kS: return kMacVK_S;
        case NeutralKey::kT: return kMacVK_T;
        case NeutralKey::kU: return kMacVK_U;
        case NeutralKey::kV: return kMacVK_V;
        case NeutralKey::kW: return kMacVK_W;
        case NeutralKey::kX: return kMacVK_X;
        case NeutralKey::kY: return kMacVK_Y;
        case NeutralKey::kZ: return kMacVK_Z;

        case NeutralKey::kNone: break;
    }
    return kMacVKUnknown;
}

} // namespace nimrtc::rd_input_inject::detail
