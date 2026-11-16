// src/modules/rd_input/src/event_codec.cpp
// ----------------------------------------------------------------------------
// The compact, versioned binary encoding of an input event (plan G3).
//
// The wire format is documented in <nimrtc/rd_input/event_codec.hpp>.  This
// file is the only place that knows it — a reader who wants to know what
// goes on the wire reads the header, and a reader who wants to change the
// format changes it here and in one place.
// ----------------------------------------------------------------------------

#include "nimrtc/rd_input/event_codec.hpp"

#include <limits>

namespace nimrtc::rd_input {
namespace {

// ---------------------------------------------------------------------------
// Varint helpers (LEB128)
// ---------------------------------------------------------------------------

/// The zigzag mapping: `(n << 1) ^ (n >> 63)`, computed without UB on the
/// signed shift.
std::uint64_t zigzag_encode(std::int64_t value) noexcept {
    return (static_cast<std::uint64_t>(value) << 1)
         ^ static_cast<std::uint64_t>(value >> 63);
}

/// The inverse of `zigzag_encode`.
std::int64_t zigzag_decode(std::uint64_t value) noexcept {
    return static_cast<std::int64_t>((value >> 1) ^ (~(value & 1) + 1));
}

/// The number of bytes a varint needs.
std::size_t varint_size(std::uint64_t value) noexcept {
    std::size_t n = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++n;
    }
    return n;
}

/// The number of bytes a zigzag varint needs.
std::size_t zigzag_size(std::int64_t value) noexcept {
    return varint_size(zigzag_encode(value));
}

/// Append a varint.
void append_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

/// Append a zigzag varint.
void append_zigzag(std::vector<std::uint8_t>& out, std::int64_t value) {
    append_varint(out, zigzag_encode(value));
}

// ---------------------------------------------------------------------------
// The reader
// ---------------------------------------------------------------------------

/// A cursor over a byte span, with the bounds checks the decoder needs.
class Reader {
public:
    explicit Reader(core::ByteSpan bytes) noexcept : bytes_{bytes} {}

    [[nodiscard]] bool exhausted() const noexcept { return pos_ >= bytes_.size(); }
    [[nodiscard]] std::size_t pos() const noexcept { return pos_; }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() > pos_ ? bytes_.size() - pos_ : 0;
    }

    /// Read a raw byte.
    bool read_u8(std::uint8_t& out) noexcept {
        if (pos_ >= bytes_.size()) return false;
        out = bytes_[pos_++];
        return true;
    }

    /// Read a varint.  A varint longer than 10 bytes cannot fit in 64 bits
    /// and is a malformed input, not a value.
    bool read_varint(std::uint64_t& out) noexcept {
        std::uint64_t value = 0;
        std::uint32_t shift = 0;
        for (std::size_t i = 0; i < 10; ++i) {
            if (pos_ >= bytes_.size()) return false;
            const std::uint8_t byte = bytes_[pos_++];
            value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0) {
                out = value;
                return true;
            }
            shift += 7;
        }
        return false;
    }

    /// Read a zigzag varint.
    bool read_zigzag(std::int64_t& out) noexcept {
        std::uint64_t raw = 0;
        if (!read_varint(raw)) return false;
        out = zigzag_decode(raw);
        return true;
    }

    /// Read a zigzag varint that must fit into 32 bits.
    bool read_zigzag32(std::int32_t& out) noexcept {
        std::int64_t value = 0;
        if (!read_zigzag(value)) return false;
        if (value < std::numeric_limits<std::int32_t>::min()
            || value > std::numeric_limits<std::int32_t>::max()) {
            return false;
        }
        out = static_cast<std::int32_t>(value);
        return true;
    }

    /// Read `len` raw bytes.
    bool read_bytes(std::size_t len, core::ByteSpan& out) noexcept {
        if (remaining() < len) return false;
        out = bytes_.subspan(pos_, len);
        pos_ += len;
        return true;
    }

private:
    core::ByteSpan bytes_;
    std::size_t    pos_ = 0;
};

// ---------------------------------------------------------------------------
// Header flags
// ---------------------------------------------------------------------------

/// The flag bit that says "a timestamp follows the header".
constexpr std::uint8_t kFlagHasTimestamp = 0x01;

/// The flag bit that says "a ref-frame id follows the header" (plan G5).
constexpr std::uint8_t kFlagHasRefFrame = 0x02;

/// The number of bytes the fixed header occupies.
constexpr std::size_t kHeaderSize = 3;  // version + type + flags

/// True when the ref-frame id is to be written.  The option asks for it,
/// but an event that carries no id cannot be written — the flag then stays
/// clear, so the decoder never expects a field that is not there.
bool wants_ref_frame(const InputEvent& event, const EncodeOptions& options) noexcept {
    return options.include_ref_frame && event.ref_frame.has_value();
}

/// Append the header (version, type, flags) and, when the flag says so,
/// the timestamp and the ref-frame id.
void append_header(std::vector<std::uint8_t>& out,
                   const InputEvent& event,
                   const EncodeOptions& options) {
    out.push_back(options.version);
    out.push_back(static_cast<std::uint8_t>(event.type));
    std::uint8_t flags = 0;
    if (options.include_timestamp) {
        flags |= kFlagHasTimestamp;
    }
    if (wants_ref_frame(event, options)) {
        flags |= kFlagHasRefFrame;
    }
    out.push_back(flags);
    if (options.include_timestamp) {
        append_varint(out, event.timestamp_us);
    }
    if (wants_ref_frame(event, options)) {
        append_varint(out, *event.ref_frame);
    }
}

/// The number of bytes the header (plus the optional timestamp and
/// ref-frame id) occupies.
std::size_t header_size(const InputEvent& event, const EncodeOptions& options) noexcept {
    std::size_t n = kHeaderSize;
    if (options.include_timestamp) n += varint_size(event.timestamp_us);
    if (wants_ref_frame(event, options)) n += varint_size(*event.ref_frame);
    return n;
}

// ---------------------------------------------------------------------------
// Decoding (shared by the single-event and the batch decoder)
// ---------------------------------------------------------------------------

/// Decode one event from `reader`.  On failure the reader's position is
/// unspecified; the caller reports the error.
core::Result<InputEvent> decode_event_from(Reader& reader,
                                           const DecodeOptions& options,
                                           std::size_t* consumed) {
    std::uint8_t version = 0;
    if (!reader.read_u8(version)) {
        return core::Result<InputEvent>::fail(
            core::ErrorCode::ProtocolError, "input event: truncated (no version byte)");
    }
    if (version < kMinDecodableEventVersion) {
        return core::Result<InputEvent>::fail(
            core::ErrorCode::ProtocolError,
            "input event: version below the decodable minimum");
    }
    if (version > kEventCodecVersion && !options.allow_newer_version) {
        return core::Result<InputEvent>::fail(
            core::ErrorCode::ProtocolError, "input event: unknown format version");
    }

    std::uint8_t type_byte = 0;
    if (!reader.read_u8(type_byte)) {
        return core::Result<InputEvent>::fail(
            core::ErrorCode::ProtocolError, "input event: truncated (no type byte)");
    }
    if (type_byte > static_cast<std::uint8_t>(InputEventType::kText)) {
        return core::Result<InputEvent>::fail(
            core::ErrorCode::ProtocolError, "input event: unknown event type");
    }

    std::uint8_t flags = 0;
    if (!reader.read_u8(flags)) {
        return core::Result<InputEvent>::fail(
            core::ErrorCode::ProtocolError, "input event: truncated (no flags byte)");
    }

    InputEvent event;
    event.type = static_cast<InputEventType>(type_byte);

    if ((flags & kFlagHasTimestamp) != 0) {
        std::uint64_t ts = 0;
        if (!reader.read_varint(ts)) {
            return core::Result<InputEvent>::fail(
                core::ErrorCode::ProtocolError, "input event: truncated timestamp");
        }
        event.timestamp_us = ts;
    }

    if ((flags & kFlagHasRefFrame) != 0) {
        std::uint64_t ref_frame = 0;
        if (!reader.read_varint(ref_frame)) {
            return core::Result<InputEvent>::fail(
                core::ErrorCode::ProtocolError, "input event: truncated ref-frame id");
        }
        event.ref_frame = ref_frame;
    }

    switch (event.type) {
        case InputEventType::kMouseMove:
            if (!reader.read_zigzag32(event.dx) || !reader.read_zigzag32(event.dy)) {
                return core::Result<InputEvent>::fail(
                    core::ErrorCode::ProtocolError,
                    "input event: truncated mouse-move delta");
            }
            break;

        case InputEventType::kMouseButton: {
            std::uint8_t button = 0;
            std::uint8_t action = 0;
            if (!reader.read_u8(button) || !reader.read_u8(action)) {
                return core::Result<InputEvent>::fail(
                    core::ErrorCode::ProtocolError,
                    "input event: truncated mouse-button");
            }
            if (button > static_cast<std::uint8_t>(MouseButton::kForward)) {
                return core::Result<InputEvent>::fail(
                    core::ErrorCode::ProtocolError, "input event: unknown mouse button");
            }
            if (action > static_cast<std::uint8_t>(ButtonAction::kUp)) {
                return core::Result<InputEvent>::fail(
                    core::ErrorCode::ProtocolError, "input event: unknown button action");
            }
            event.button = static_cast<MouseButton>(button);
            event.button_action = static_cast<ButtonAction>(action);
            if (!reader.read_zigzag32(event.x) || !reader.read_zigzag32(event.y)) {
                return core::Result<InputEvent>::fail(
                    core::ErrorCode::ProtocolError,
                    "input event: truncated mouse-button position");
            }
            break;
        }

        case InputEventType::kMouseWheel:
            if (!reader.read_zigzag32(event.dx) || !reader.read_zigzag32(event.dy)) {
                return core::Result<InputEvent>::fail(
                    core::ErrorCode::ProtocolError, "input event: truncated wheel delta");
            }
            break;

        case InputEventType::kKey: {
            std::uint64_t key_code = 0;
            std::uint8_t action = 0;
            std::uint64_t modifiers = 0;
            std::uint8_t repeat = 0;
            if (!reader.read_varint(key_code) || !reader.read_u8(action)
                || !reader.read_varint(modifiers) || !reader.read_u8(repeat)) {
                return core::Result<InputEvent>::fail(
                    core::ErrorCode::ProtocolError, "input event: truncated key event");
            }
            if (action > static_cast<std::uint8_t>(KeyAction::kUp)) {
                return core::Result<InputEvent>::fail(
                    core::ErrorCode::ProtocolError, "input event: unknown key action");
            }
            if (key_code > std::numeric_limits<std::uint32_t>::max()) {
                return core::Result<InputEvent>::fail(
                    core::ErrorCode::ProtocolError, "input event: key code out of range");
            }
            if (modifiers > std::numeric_limits<std::uint32_t>::max()) {
                return core::Result<InputEvent>::fail(
                    core::ErrorCode::ProtocolError, "input event: modifiers out of range");
            }
            event.key_code = static_cast<std::uint32_t>(key_code);
            event.key_action = static_cast<KeyAction>(action);
            event.modifiers = static_cast<std::uint32_t>(modifiers);
            event.repeat = (repeat != 0);
            break;
        }

        case InputEventType::kText: {
            std::uint64_t length = 0;
            if (!reader.read_varint(length)) {
                return core::Result<InputEvent>::fail(
                    core::ErrorCode::ProtocolError, "input event: truncated text length");
            }
            if (length > options.max_text_bytes) {
                return core::Result<InputEvent>::fail(
                    core::ErrorCode::InvalidArgument,
                    "input event: text payload over the limit");
            }
            core::ByteSpan payload;
            if (!reader.read_bytes(static_cast<std::size_t>(length), payload)) {
                return core::Result<InputEvent>::fail(
                    core::ErrorCode::ProtocolError, "input event: truncated text payload");
            }
            event.text.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
            break;
        }
    }

    if (consumed != nullptr) {
        *consumed = reader.pos();
    }
    return core::Result<InputEvent>::ok(std::move(event));
}

} // namespace

// ---------------------------------------------------------------------------
// encoded_size
// ---------------------------------------------------------------------------

std::size_t encoded_size(const InputEvent& event, const EncodeOptions& options) {
    std::size_t total = header_size(event, options);

    switch (event.type) {
        case InputEventType::kMouseMove:
        case InputEventType::kMouseWheel:
            total += zigzag_size(event.dx);
            total += zigzag_size(event.dy);
            break;
        case InputEventType::kMouseButton:
            total += 2;  // button + action
            total += zigzag_size(event.x);
            total += zigzag_size(event.y);
            break;
        case InputEventType::kKey:
            total += varint_size(event.key_code);
            total += 1;  // action
            total += varint_size(event.modifiers);
            total += 1;  // repeat
            break;
        case InputEventType::kText:
            total += varint_size(event.text.size());
            total += event.text.size();
            break;
    }
    return total;
}

// ---------------------------------------------------------------------------
// encode_event_into
// ---------------------------------------------------------------------------

core::Result<std::size_t> encode_event_into(std::vector<std::uint8_t>& out,
                                             const InputEvent& event,
                                             const EncodeOptions& options) {
    switch (event.type) {
        case InputEventType::kMouseMove:
        case InputEventType::kMouseButton:
        case InputEventType::kMouseWheel:
        case InputEventType::kKey:
        case InputEventType::kText:
            break;
        default:
            return core::Result<std::size_t>::fail(
                core::ErrorCode::InvalidArgument, "unknown input event type");
    }

    const std::size_t before = out.size();
    append_header(out, event, options);

    switch (event.type) {
        case InputEventType::kMouseMove:
            append_zigzag(out, event.dx);
            append_zigzag(out, event.dy);
            break;

        case InputEventType::kMouseButton:
            out.push_back(static_cast<std::uint8_t>(event.button));
            out.push_back(static_cast<std::uint8_t>(event.button_action));
            append_zigzag(out, event.x);
            append_zigzag(out, event.y);
            break;

        case InputEventType::kMouseWheel:
            append_zigzag(out, event.dx);
            append_zigzag(out, event.dy);
            break;

        case InputEventType::kKey:
            append_varint(out, event.key_code);
            out.push_back(static_cast<std::uint8_t>(event.key_action));
            append_varint(out, event.modifiers);
            out.push_back(event.repeat ? 1 : 0);
            break;

        case InputEventType::kText:
            append_varint(out, event.text.size());
            out.insert(out.end(), event.text.begin(), event.text.end());
            break;
    }

    return core::Result<std::size_t>::ok(out.size() - before);
}

// ---------------------------------------------------------------------------
// encode_event
// ---------------------------------------------------------------------------

core::Result<std::vector<std::uint8_t>> encode_event(const InputEvent& event,
                                                       const EncodeOptions& options) {
    std::vector<std::uint8_t> out;
    out.reserve(encoded_size(event, options));
    auto res = encode_event_into(out, event, options);
    if (!res) {
        return core::Result<std::vector<std::uint8_t>>::fail(res.error());
    }
    return core::Result<std::vector<std::uint8_t>>::ok(std::move(out));
}

// ---------------------------------------------------------------------------
// decode_event
// ---------------------------------------------------------------------------

core::Result<InputEvent> decode_event(core::ByteSpan bytes,
                                       const DecodeOptions& options,
                                       std::size_t* consumed) {
    Reader reader{bytes};
    return decode_event_from(reader, options, consumed);
}

// ---------------------------------------------------------------------------
// Batch codec
// ---------------------------------------------------------------------------

core::Result<std::vector<std::uint8_t>> encode_batch(const std::vector<InputEvent>& events,
                                                      const EncodeOptions& options) {
    if (events.size() > kMaxBatchEvents) {
        return core::Result<std::vector<std::uint8_t>>::fail(
            core::ErrorCode::InvalidArgument, "input batch: too many events");
    }

    std::vector<std::uint8_t> out;
    append_varint(out, events.size());

    for (const InputEvent& event : events) {
        auto res = encode_event_into(out, event, options);
        if (!res) {
            return core::Result<std::vector<std::uint8_t>>::fail(res.error());
        }
    }
    return core::Result<std::vector<std::uint8_t>>::ok(std::move(out));
}

core::Result<std::vector<InputEvent>> decode_batch(core::ByteSpan bytes,
                                                    const DecodeOptions& options) {
    Reader reader{bytes};

    std::uint64_t count = 0;
    if (!reader.read_varint(count)) {
        return core::Result<std::vector<InputEvent>>::fail(
            core::ErrorCode::ProtocolError, "input batch: truncated (no event count)");
    }
    if (count > kMaxBatchEvents) {
        return core::Result<std::vector<InputEvent>>::fail(
            core::ErrorCode::InvalidArgument, "input batch: event count over the limit");
    }

    std::vector<InputEvent> events;
    events.reserve(static_cast<std::size_t>(count));

    for (std::uint64_t i = 0; i < count; ++i) {
        auto event = decode_event_from(reader, options, nullptr);
        if (!event) {
            return core::Result<std::vector<InputEvent>>::fail(event.error());
        }
        events.push_back(std::move(event).value());
    }

    return core::Result<std::vector<InputEvent>>::ok(std::move(events));
}

} // namespace nimrtc::rd_input
