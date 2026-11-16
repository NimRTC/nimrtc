/**
 * @file nimrtc/rd_input/input_event.hpp
 * @brief The remote-desktop input event vocabulary — mouse, wheel, key, text.
 *
 * Remote-desktop plan G3 (the input event codec) / G4 (batching, coalescing
 * and rate limiting) / G7 (the input safety filter).
 *
 * This header defines the *vocabulary*: what an input event is.  The three
 * mechanisms that operate on the vocabulary live in sibling headers:
 *
 *   - `event_codec.hpp`  — the compact, versioned binary encoding (G3).
 *   - `coalescer.hpp`    — the coalescer and the rate limiter (G4).
 *   - `safety_filter.hpp` — the allow / deny filter (G7).
 *
 * ## Why a module and not a seam
 *
 * Plan G3 says it explicitly: "It is a **module, not a seam**: the transport
 * is DataChannel, and only the payload format is NimRTC's."  The transport
 * is chosen elsewhere; what NimRTC owns here is the *payload format* and the
 * *input-side policy* (coalescing, rate limiting, safety).  So there is no
 * `plugins::I*` interface and nothing in `src/plugins/` — the module is
 * self-contained (AGENTS.md L2 / L3).
 *
 * ## The event shape
 *
 * `InputEvent` is a flat struct with a type tag rather than a
 * `std::variant`.  Two reasons:
 *
 *   1. The hot path is a capture thread that pushes an event and a send
 *      thread that reads it.  A `variant` of five alternatives needs a heap
 *      allocation for the `std::string` payload anyway, and the size of the
 *      whole struct grows to the largest alternative.
 *   2. The *wire* format is not the in-memory format.  The codec compresses
 *      the fields that matter for each type; keeping the in-memory struct
 *      flat means the codec's job stays "read these fields, write these
 *      bytes", which is where a wire-format bug would otherwise hide.
 *
 * The `text` member is the only non-trivial one, and it is only populated
 * for `kText` events.
 *
 * ## Key codes
 *
 * `key_code` is a `std::uint32_t` chosen by the *host* (the capture side),
 * not by this module.  The module provides a small set of named constants
 * (`keys::`) for the common keys so that two NimRTC hosts can agree on the
 * obvious ones, but it deliberately does not define a complete layout: a
 * complete layout is a per-platform decision (a macOS virtual keycode and a
 * Windows virtual key are different numbers, and neither is a USB HID
 * usage id).
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nimrtc::rd_input {

// ---------------------------------------------------------------------------
// Event kinds
// ---------------------------------------------------------------------------

/** The kind of an input event.  The numeric values are part of the wire
 *  format (see `event_codec.hpp`) and must not be reordered. */
enum class NIMRTC_API_STABLE InputEventType : std::uint8_t {
    kMouseMove   = 0,  ///< relative pointer motion (a delta, not a position)
    kMouseButton = 1,  ///< a mouse button press or release
    kMouseWheel  = 2,  ///< a wheel / scroll notch
    kKey         = 3,  ///< a key press or release
    kText        = 4,  ///< committed text (an IME composition result)
};

/** Which mouse button an event refers to. */
enum class NIMRTC_API_STABLE MouseButton : std::uint8_t {
    kUnknown = 0,
    kLeft    = 1,
    kRight   = 2,
    kMiddle  = 3,
    kBack    = 4,
    kForward = 5,
};

/** A button press or a button release. */
enum class NIMRTC_API_STABLE ButtonAction : std::uint8_t {
    kDown = 0,
    kUp   = 1,
};

/** A key press or a key release. */
enum class NIMRTC_API_STABLE KeyAction : std::uint8_t {
    kDown = 0,
    kUp   = 1,
};

/** Modifier-key bits, OR-ed into `InputEvent::modifiers`. */
enum class NIMRTC_API_STABLE Modifier : std::uint32_t {
    kNone     = 0u,
    kShift    = 1u << 0,
    kControl  = 1u << 1,
    kAlt      = 1u << 2,
    kMeta     = 1u << 3,  ///< the Windows / Command key
    kCapsLock = 1u << 4,
};

/** Human-readable event-kind name (for logs and diagnostics). */
NIMRTC_API_STABLE inline std::string_view input_event_type_name(InputEventType t) noexcept {
    switch (t) {
        case InputEventType::kMouseMove:   return "mouse-move";
        case InputEventType::kMouseButton: return "mouse-button";
        case InputEventType::kMouseWheel:  return "mouse-wheel";
        case InputEventType::kKey:         return "key";
        case InputEventType::kText:        return "text";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Named key codes
// ---------------------------------------------------------------------------
//
// A *partial* set: the keys a remote-desktop safety policy most often needs
// to name (the dangerous combinations are Ctrl+Alt+Delete, Alt+Tab,
// Alt+F4, and the Windows key).  A host that wants more uses its own
// platform's virtual-key numbers — `key_code` is an opaque `uint32_t` to
// this module.
namespace keys {

inline constexpr NIMRTC_API_STABLE std::uint32_t kUnknown = 0;
inline constexpr NIMRTC_API_STABLE std::uint32_t kEscape   = 1;
inline constexpr NIMRTC_API_STABLE std::uint32_t kTab      = 2;
inline constexpr NIMRTC_API_STABLE std::uint32_t kEnter    = 3;
inline constexpr NIMRTC_API_STABLE std::uint32_t kReturn   = 3;  ///< an alias
inline constexpr NIMRTC_API_STABLE std::uint32_t kSpace    = 4;
inline constexpr NIMRTC_API_STABLE std::uint32_t kBackspace = 5;
inline constexpr NIMRTC_API_STABLE std::uint32_t kDelete   = 6;
inline constexpr NIMRTC_API_STABLE std::uint32_t kHome     = 7;
inline constexpr NIMRTC_API_STABLE std::uint32_t kEnd      = 8;
inline constexpr NIMRTC_API_STABLE std::uint32_t kPageUp   = 9;
inline constexpr NIMRTC_API_STABLE std::uint32_t kPageDown = 10;
inline constexpr NIMRTC_API_STABLE std::uint32_t kLeft     = 11;
inline constexpr NIMRTC_API_STABLE std::uint32_t kUp       = 12;
inline constexpr NIMRTC_API_STABLE std::uint32_t kRight    = 13;
inline constexpr NIMRTC_API_STABLE std::uint32_t kDown     = 14;
inline constexpr NIMRTC_API_STABLE std::uint32_t kShift    = 15;
inline constexpr NIMRTC_API_STABLE std::uint32_t kControl  = 16;
inline constexpr NIMRTC_API_STABLE std::uint32_t kAlt      = 17;
inline constexpr NIMRTC_API_STABLE std::uint32_t kMeta     = 18;  ///< Win / Cmd
inline constexpr NIMRTC_API_STABLE std::uint32_t kF1       = 19;
inline constexpr NIMRTC_API_STABLE std::uint32_t kF2       = 20;
inline constexpr NIMRTC_API_STABLE std::uint32_t kF3       = 21;
inline constexpr NIMRTC_API_STABLE std::uint32_t kF4       = 22;
inline constexpr NIMRTC_API_STABLE std::uint32_t kF5       = 23;
inline constexpr NIMRTC_API_STABLE std::uint32_t kF6       = 24;
inline constexpr NIMRTC_API_STABLE std::uint32_t kF7       = 25;
inline constexpr NIMRTC_API_STABLE std::uint32_t kF8       = 26;
inline constexpr NIMRTC_API_STABLE std::uint32_t kF9       = 27;
inline constexpr NIMRTC_API_STABLE std::uint32_t kF10      = 28;
inline constexpr NIMRTC_API_STABLE std::uint32_t kF11      = 29;
inline constexpr NIMRTC_API_STABLE std::uint32_t kF12      = 30;

// The printable ASCII letters, in the obvious order.  They are the keys a
// host names most often, and giving them stable numbers lets two NimRTC
// hosts agree on "the A key" without a full layout negotiation.
inline constexpr NIMRTC_API_STABLE std::uint32_t kA = 100;
inline constexpr NIMRTC_API_STABLE std::uint32_t kB = 101;
inline constexpr NIMRTC_API_STABLE std::uint32_t kC = 102;
inline constexpr NIMRTC_API_STABLE std::uint32_t kD = 103;
inline constexpr NIMRTC_API_STABLE std::uint32_t kE = 104;
inline constexpr NIMRTC_API_STABLE std::uint32_t kF = 105;
inline constexpr NIMRTC_API_STABLE std::uint32_t kG = 106;
inline constexpr NIMRTC_API_STABLE std::uint32_t kH = 107;
inline constexpr NIMRTC_API_STABLE std::uint32_t kI = 108;
inline constexpr NIMRTC_API_STABLE std::uint32_t kJ = 109;
inline constexpr NIMRTC_API_STABLE std::uint32_t kK = 110;
inline constexpr NIMRTC_API_STABLE std::uint32_t kL = 111;
inline constexpr NIMRTC_API_STABLE std::uint32_t kM = 112;
inline constexpr NIMRTC_API_STABLE std::uint32_t kN = 113;
inline constexpr NIMRTC_API_STABLE std::uint32_t kO = 114;
inline constexpr NIMRTC_API_STABLE std::uint32_t kP = 115;
inline constexpr NIMRTC_API_STABLE std::uint32_t kQ = 116;
inline constexpr NIMRTC_API_STABLE std::uint32_t kR = 117;
inline constexpr NIMRTC_API_STABLE std::uint32_t kS = 118;
inline constexpr NIMRTC_API_STABLE std::uint32_t kT = 119;
inline constexpr NIMRTC_API_STABLE std::uint32_t kU = 120;
inline constexpr NIMRTC_API_STABLE std::uint32_t kV = 121;
inline constexpr NIMRTC_API_STABLE std::uint32_t kW = 122;
inline constexpr NIMRTC_API_STABLE std::uint32_t kX = 123;
inline constexpr NIMRTC_API_STABLE std::uint32_t kY = 124;
inline constexpr NIMRTC_API_STABLE std::uint32_t kZ = 125;

} // namespace keys

// ---------------------------------------------------------------------------
// InputEvent
// ---------------------------------------------------------------------------

/**
 * @brief One remote-desktop input event.
 *
 * The fields that are meaningful depend on `type`:
 *
 * | type            | meaningful fields                          |
 * |-----------------|--------------------------------------------|
 * | `kMouseMove`    | `dx`, `dy` (relative motion)               |
 * | `kMouseButton`  | `button`, `button_action`, `x`, `y`        |
 * | `kMouseWheel`   | `dx`, `dy` (the scroll amount)             |
 * | `kKey`          | `key_code`, `key_action`, `modifiers`,      |
 * |                 | `repeat`                                   |
 * | `kText`         | `text`                                     |
 *
 * `timestamp_us` is the capture timestamp in microseconds on the *host's*
 * monotonic clock.  It is the host's, not ours: the module never compares
 * two events' timestamps across hosts, and it never interprets them as
 * wall-clock time.
 */
struct NIMRTC_API_STABLE InputEvent {
    /** What kind of event this is. */
    InputEventType type = InputEventType::kMouseMove;

    /** Capture timestamp (microseconds, host monotonic clock). */
    std::uint64_t timestamp_us = 0;

    /** Relative pointer motion (pixels), or the wheel scroll amount. */
    std::int32_t dx = 0;
    std::int32_t dy = 0;

    /** Absolute pointer position (pixels), on the button events that carry
     *  one.  The host is free to leave it at 0 when it does not track a
     *  position (a relative-motion-only capture device). */
    std::int32_t x = 0;
    std::int32_t y = 0;

    /** Which button (button events only). */
    MouseButton button = MouseButton::kUnknown;

    /** Press or release (button events only). */
    ButtonAction button_action = ButtonAction::kDown;

    /** Press or release (key events only). */
    KeyAction key_action = KeyAction::kDown;

    /** The host's key identifier (key events only). */
    std::uint32_t key_code = 0;

    /** Modifier bits, an OR of `Modifier` values (key events only). */
    std::uint32_t modifiers = 0;

    /** True for an auto-repeat key press. */
    bool repeat = false;

    /** The committed text (text events only). */
    std::string text;

    /**
     * @brief The `ref_frame` id this event was generated against (G5).
     *
     * The id identifies the video frame the operator was looking at when
     * the event was produced — plan G5's "the click happened on the frame
     * the operator was looking at".  The id space is the engine's
     * `ref_frame` space: on the wire it is the peer's `frame_seq`, and
     * the receiver resolves it against *its own* displayed-frame table.
     *
     * `std::nullopt` means "no frame reference" — the event was produced
     * before any frame was sent, or the producer does not track frames.
     * It is a legal value, not an error, and it costs **zero** bytes on
     * the wire when the encoder is not asked to include it.
     */
    std::optional<std::uint64_t> ref_frame;
};

/** Structural equality — the round-trip test needs it, and a host that
 *  compares a captured event with a decoded one needs it too. */
NIMRTC_API_STABLE inline bool operator==(const InputEvent& a,
                                          const InputEvent& b) noexcept {
    return a.type == b.type
        && a.timestamp_us == b.timestamp_us
        && a.dx == b.dx
        && a.dy == b.dy
        && a.x == b.x
        && a.y == b.y
        && a.button == b.button
        && a.button_action == b.button_action
        && a.key_action == b.key_action
        && a.key_code == b.key_code
        && a.modifiers == b.modifiers
        && a.repeat == b.repeat
        && a.text == b.text
        && a.ref_frame == b.ref_frame;
}

inline NIMRTC_API_STABLE bool operator!=(const InputEvent& a,
                                         const InputEvent& b) noexcept {
    return !(a == b);
}

// ---------------------------------------------------------------------------
// Constructors
// ---------------------------------------------------------------------------
//
// Small factories rather than aggregate initialisers: a host that fills a
// `InputEvent` by hand has to remember which field belongs to which type,
// and a misplaced field is exactly the kind of bug the codec would then
// faithfully carry to the remote side.

NIMRTC_API_STABLE inline InputEvent make_mouse_move(std::int32_t dx,
                                                     std::int32_t dy,
                                                     std::uint64_t timestamp_us = 0) noexcept {
    InputEvent e;
    e.type = InputEventType::kMouseMove;
    e.dx = dx;
    e.dy = dy;
    e.timestamp_us = timestamp_us;
    return e;
}

NIMRTC_API_STABLE inline InputEvent make_mouse_button(MouseButton button,
                                                       ButtonAction action,
                                                       std::int32_t x = 0,
                                                       std::int32_t y = 0,
                                                       std::uint64_t timestamp_us = 0) noexcept {
    InputEvent e;
    e.type = InputEventType::kMouseButton;
    e.button = button;
    e.button_action = action;
    e.x = x;
    e.y = y;
    e.timestamp_us = timestamp_us;
    return e;
}

NIMRTC_API_STABLE inline InputEvent make_mouse_wheel(std::int32_t dx,
                                                      std::int32_t dy,
                                                      std::uint64_t timestamp_us = 0) noexcept {
    InputEvent e;
    e.type = InputEventType::kMouseWheel;
    e.dx = dx;
    e.dy = dy;
    e.timestamp_us = timestamp_us;
    return e;
}

NIMRTC_API_STABLE inline InputEvent make_key(std::uint32_t key_code,
                                              KeyAction action,
                                              std::uint32_t modifiers = 0,
                                              bool repeat = false,
                                              std::uint64_t timestamp_us = 0) noexcept {
    InputEvent e;
    e.type = InputEventType::kKey;
    e.key_code = key_code;
    e.key_action = action;
    e.modifiers = modifiers;
    e.repeat = repeat;
    e.timestamp_us = timestamp_us;
    return e;
}

NIMRTC_API_STABLE inline InputEvent make_text(std::string text,
                                               std::uint64_t timestamp_us = 0) {
    InputEvent e;
    e.type = InputEventType::kText;
    e.text = std::move(text);
    e.timestamp_us = timestamp_us;
    return e;
}

} // namespace nimrtc::rd_input
