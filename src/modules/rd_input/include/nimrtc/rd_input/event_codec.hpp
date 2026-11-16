/**
 * @file nimrtc/rd_input/event_codec.hpp
 * @brief The compact, versioned binary encoding of an input event (plan G3).
 *
 * ## Why a bespoke encoding
 *
 * The transport is a DataChannel (the browser's `RTCDataChannel` and
 * NimRTC's own), so the payload could be JSON.  It is not, because the
 * input path is the *latency-critical* path and the events are small and
 * frequent:
 *
 *   - A mouse-move delta is a **handful of bytes** (4 bytes for a typical
 *     delta), where the JSON form is 60+.
 *   - A key press is 5 bytes where the JSON form is 80+.
 *   - The decoder does no allocation for the common event types, so a
 *     1000 Hz pointer does not become a 1000 Hz allocation storm on the
 *     receive thread.
 *
 * ## The wire format (version 1)
 *
 * Every event starts with a three-byte header:
 *
 * ```
 *   byte 0 : format version (currently 1)
 *   byte 1 : event type   (InputEventType)
 *   byte 2 : flags        (bit 0 = "a timestamp follows",
 *                          bit 1 = "a ref-frame id follows")
 * ```
 *
 * and continues with a type-specific body:
 *
 * | type            | body                                                       |
 * |-----------------|------------------------------------------------------------|
 * | `kMouseMove`   | `dx` (zigzag varint), `dy` (zigzag varint)                  |
 * | `kMouseButton` | `button` (u8), `action` (u8), `x` (zigzag varint), `y`      |
 * | `kMouseWheel`  | `dx` (zigzag varint), `dy` (zigzag varint)                  |
 * | `kKey`         | `key_code` (varint), `action` (u8), `modifiers` (varint),   |
 * |                | `repeat` (u8)                                              |
 * | `kText`        | `length` (varint), then `length` raw UTF-8 bytes            |
 * ```
 *
 * A *varint* is LEB128: 7 payload bits per byte, little-endian groups, the
 * high bit meaning "more bytes follow".  A *zigzag varint* is a signed
 * varint: the sign bit is folded into the low bit so that small negative
 * numbers stay short (`-1` is one byte).
 *
 * The `timestamp_us` field is deliberately **not** on the wire by default:
 * the receiver does not need it to apply the event, and a varint timestamp
 * would cost 5-6 bytes on every single event.  A host that wants it (for
 * telemetry, or for the G5 input-to-frame binding) turns it on with
 * `EncodeOptions::include_timestamp`; the flags byte then says so, and the
 * field follows the header as a varint.  The flags byte is what makes the
 * decoder self-describing: a decoder never has to *agree* with the sender
 * about whether a timestamp is present — it reads the flag.
 *
 * The `ref_frame` id (plan G5 — the frame the operator was looking at)
 * is carried the same way: `EncodeOptions::include_ref_frame` puts it on
 * the wire as a varint immediately after the optional timestamp, and
 * flags bit 1 says so.  A sender that does not bind its events to frames
 * pays nothing for the feature — the flag bit stays clear and no bytes
 * are added.
 *
 * ## Versioning
 *
 * The version byte is the *first* byte of every event, so a decoder can
 * reject an event it does not understand before it parses anything else.
 * A decoder configured with `DecodeOptions::allow_newer_version` accepts a
 * version it does not know and parses it under the *current* rules — which
 * is only safe if the format is append-only, so the default is to reject.
 *
 * ### Unknown flag bits are **ignored**, not rejected
 *
 * The flags byte is the extension point that keeps the version at 1.
 * A decoder reads only the bits it knows (bit 0 = timestamp, bit 1 =
 * ref-frame id) and **ignores every other bit** — an event whose flags
 * byte carries an unknown bit still decodes, and the bit does not
 * change the outcome.  That is the append-only-friendly answer, and it
 * is the documented choice.
 *
 * **The honest limit of that choice.**  The optional fields appear in a
 * *fixed order* (timestamp, then ref-frame id) and each is
 * self-describing through its own flag bit, but a decoder that does not
 * know bit 1 has no way to *skip* the field it does not know — the
 * varint's length is not knowable.  So:
 *
 *   - a **new** decoder reading an **old** event is fine (the flag is
 *     clear, so no field is read);
 *   - an **old** decoder reading a **new** event that carries a
 *     ref-frame id is **not** fine: it ignores the flag bit and then
 *     mis-reads the field's bytes as the event body.
 *
 * That asymmetry is why the **version byte is the strong guard** and
 * the flags byte is only a soft one.  A decoder with
 * `allow_newer_version == false` rejects an unknown version outright,
 * so a future format change that is *not* backwards compatible must
 * bump `kEventCodecVersion` — and a sender that sets the ref-frame
 * flag should only do so against a peer known to support it (which in
 * practice means: the host turns `EncodeOptions::include_ref_frame`
 * on only when the peer negotiated the feature).
 *
 * The practical consequence, stated plainly: **the ref-frame extension
 * is additive and costs nothing when it is off, but a peer that
 * predates it must not be sent events that carry it.**
 *
 * ## Batch framing
 *
 * `encode_batch` / `decode_batch` frame a *list* of events:
 *
 * ```
 *   varint  event count
 *   event   event 0
 *   event   event 1
 *   ...
 * ```
 *
 * Each event inside a batch carries its own version byte, so a batch is a
 * concatenation of independently-decodable events.  The count is a *hint*
 * for the receiver's allocation, not a trust boundary: the decoder stops
 * when the input is exhausted and reports how many events it actually read.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>

#include "nimrtc/rd_input/input_event.hpp"

namespace nimrtc::rd_input {

// ---------------------------------------------------------------------------
// Format constants
// ---------------------------------------------------------------------------

/** The wire-format version this build writes. */
inline constexpr NIMRTC_API_STABLE std::uint8_t kEventCodecVersion = 1;

/** The oldest wire-format version this build can decode. */
inline constexpr NIMRTC_API_STABLE std::uint8_t kMinDecodableEventVersion = 1;

/// The largest number of events a single batch may carry.  A receiver that
///  is told "there are 4 billion events" must not allocate for them.
inline constexpr NIMRTC_API_STABLE std::size_t kMaxBatchEvents = 4096;

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

/** Encoder options. */
struct NIMRTC_API_STABLE EncodeOptions {
    /** Write the capture timestamp onto the wire.  Off by default: the
     *  receiver does not need it to apply the event, and it costs 5-6
     *  bytes per event. */
    bool include_timestamp = false;

    /** Write the `ref_frame` id (plan G5) onto the wire.  Off by default:
     *  a sender that does not bind its events to frames pays nothing. */
    bool include_ref_frame = false;

    /** The version byte to write.  Defaults to `kEventCodecVersion`; a
     *  lower value exists so a test can prove that a decoder rejects an
     *  unknown version. */
    std::uint8_t version = kEventCodecVersion;
};

/** Decoder options. */
struct NIMRTC_API_STABLE DecodeOptions {
    /** Accept a version byte this build does not know, and parse the event
     *  under the current rules.  Off by default: the format is not
     *  guaranteed append-only across versions, so an unknown version is
     *  rejected rather than guessed at. */
    bool allow_newer_version = false;

    /** The largest text payload a single event may carry.  A receiver that
     *  trusts a length field from the network is a receiver that can be
     *  made to allocate a gigabyte. */
    std::size_t max_text_bytes = 4096;
};

// ---------------------------------------------------------------------------
// Single-event codec
// ---------------------------------------------------------------------------

/**
 * @brief Encode one event into a fresh byte buffer.
 *
 * @return the encoded bytes, or an error if the event cannot be encoded
 *         (an unknown event type, or a text payload over the limit).
 */
NIMRTC_API_STABLE core::Result<std::vector<std::uint8_t>>
encode_event(const InputEvent& event,
             const EncodeOptions& options = EncodeOptions{});

/**
 * @brief Decode one event from `bytes`.
 *
 * @param bytes      the buffer to decode from.
 * @param options    decoder options.
 * @param consumed   (out) the number of bytes the event occupied.  May be
 *                   null.  On an error the value is unspecified.
 */
NIMRTC_API_STABLE core::Result<InputEvent>
decode_event(core::ByteSpan bytes,
             const DecodeOptions& options = DecodeOptions{},
             std::size_t* consumed = nullptr);

/** Encode one event into an existing buffer (append). */
NIMRTC_API_STABLE core::Result<std::size_t>
encode_event_into(std::vector<std::uint8_t>& out,
                  const InputEvent& event,
                  const EncodeOptions& options = EncodeOptions{});

// ---------------------------------------------------------------------------
// Batch codec
// ---------------------------------------------------------------------------

/**
 * @brief Encode a list of events into one batch frame.
 *
 * The events are concatenated; each carries its own version byte, so a
 * batch is a sequence of independently-decodable events.
 */
NIMRTC_API_STABLE core::Result<std::vector<std::uint8_t>>
encode_batch(const std::vector<InputEvent>& events,
             const EncodeOptions& options = EncodeOptions{});

/**
 * @brief Decode a batch frame into a list of events.
 *
 * The decoder stops at the end of the input.  A batch whose declared count
 * exceeds the number of events actually present is an error — the count is
 * a framing promise, and a broken one means the sender and the receiver
 * disagree about where the frame ends.
 */
NIMRTC_API_STABLE core::Result<std::vector<InputEvent>>
decode_batch(core::ByteSpan bytes,
             const DecodeOptions& options = DecodeOptions{});

// ---------------------------------------------------------------------------
// Size helpers
// ---------------------------------------------------------------------------

/**
 * @brief The encoded size of `event`, in bytes.
 *
 * The exact value the encoder would produce, so a host that wants to size
 * a buffer up front does not have to guess.
 */
NIMRTC_API_STABLE std::size_t encoded_size(const InputEvent& event,
                                            const EncodeOptions& options = EncodeOptions{});

} // namespace nimrtc::rd_input
