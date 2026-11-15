/**
 * @file src/modules/rd_input_inject/src/key_map.cpp
 * @brief The platform-neutral key layout (portable — compiled on every leg).
 */

#include <nimrtc/rd_input_inject/key_map.hpp>

namespace nimrtc::rd_input_inject {
namespace {

// The layout, as a table.  A table (rather than a switch) keeps the
// count and the iteration in one place, and it makes the "unknown code"
// case a lookup that fails — which is exactly the semantics the header
// promises.
struct KeyEntry {
    NeutralKey key;
    std::string_view name;
};

constexpr KeyEntry kKeys[] = {
    {NeutralKey::kEscape,   "Escape"},
    {NeutralKey::kTab,      "Tab"},
    {NeutralKey::kEnter,    "Enter"},
    {NeutralKey::kSpace,    "Space"},
    {NeutralKey::kBackspace,"Backspace"},
    {NeutralKey::kDelete,   "Delete"},
    {NeutralKey::kHome,     "Home"},
    {NeutralKey::kEnd,      "End"},
    {NeutralKey::kPageUp,   "PageUp"},
    {NeutralKey::kPageDown, "PageDown"},
    {NeutralKey::kLeft,     "Left"},
    {NeutralKey::kUp,       "Up"},
    {NeutralKey::kRight,    "Right"},
    {NeutralKey::kDown,     "Down"},
    {NeutralKey::kShift,    "Shift"},
    {NeutralKey::kControl,  "Control"},
    {NeutralKey::kAlt,      "Alt"},
    {NeutralKey::kMeta,     "Meta"},
    {NeutralKey::kF1,       "F1"},
    {NeutralKey::kF2,       "F2"},
    {NeutralKey::kF3,       "F3"},
    {NeutralKey::kF4,       "F4"},
    {NeutralKey::kF5,       "F5"},
    {NeutralKey::kF6,       "F6"},
    {NeutralKey::kF7,       "F7"},
    {NeutralKey::kF8,       "F8"},
    {NeutralKey::kF9,       "F9"},
    {NeutralKey::kF10,      "F10"},
    {NeutralKey::kF11,      "F11"},
    {NeutralKey::kF12,      "F12"},
    {NeutralKey::kA,        "A"},
    {NeutralKey::kB,        "B"},
    {NeutralKey::kC,        "C"},
    {NeutralKey::kD,        "D"},
    {NeutralKey::kE,        "E"},
    {NeutralKey::kF,        "F"},
    {NeutralKey::kG,        "G"},
    {NeutralKey::kH,        "H"},
    {NeutralKey::kI,        "I"},
    {NeutralKey::kJ,        "J"},
    {NeutralKey::kK,        "K"},
    {NeutralKey::kL,        "L"},
    {NeutralKey::kM,        "M"},
    {NeutralKey::kN,        "N"},
    {NeutralKey::kO,        "O"},
    {NeutralKey::kP,        "P"},
    {NeutralKey::kQ,        "Q"},
    {NeutralKey::kR,        "R"},
    {NeutralKey::kS,        "S"},
    {NeutralKey::kT,        "T"},
    {NeutralKey::kU,        "U"},
    {NeutralKey::kV,        "V"},
    {NeutralKey::kW,        "W"},
    {NeutralKey::kX,        "X"},
    {NeutralKey::kY,        "Y"},
    {NeutralKey::kZ,        "Z"},
};

} // namespace

std::optional<NeutralKey> neutral_key_of(std::uint32_t key_code) noexcept {
    // The table is small and the codes are sparse, so a linear scan is
    // faster than a binary search (the branch predictor handles it
    // perfectly) and it keeps the table in one place.
    for (const KeyEntry& entry : kKeys) {
        if (static_cast<std::uint32_t>(entry.key) == key_code) {
            return entry.key;
        }
    }
    return std::nullopt;
}

bool is_neutral_key_known(std::uint32_t key_code) noexcept {
    return neutral_key_of(key_code).has_value();
}

std::string_view neutral_key_name(NeutralKey key) noexcept {
    for (const KeyEntry& entry : kKeys) {
        if (entry.key == key) return entry.name;
    }
    return {};
}

std::size_t neutral_key_count() noexcept {
    return sizeof(kKeys) / sizeof(kKeys[0]);
}

} // namespace nimrtc::rd_input_inject
