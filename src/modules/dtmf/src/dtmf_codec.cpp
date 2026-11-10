/**
 * @file src/modules/dtmf/src/dtmf_codec.cpp
 * @brief The RFC 4733 (telephone-event) codec.
 *
 * v1.2.0 — `docs/plan/session-capability-plan.md` D1 / D2.
 *
 * The wire format is four bytes (RFC 4733 §2.3, Figure 1):
 *
 *     0                   1                   2                   3
 *     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |     event     |E|R|   volume  |          duration             |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * Byte 1 carries three fields, and their positions are fixed by the RFC:
 *
 *   - **E** — the end-of-event bit, the *most significant* bit of byte
 *     1 (mask 0x80).  RFC 4733 §2.3.2.
 *   - **R** — the reserved bit (mask 0x40).  RFC 4733 §2.3.3: the
 *     sender MUST set it to zero and the receiver MUST ignore it.
 *   - **volume** — the six low bits of byte 1 (mask 0x3F), the tone
 *     power in dBm0 with the sign dropped (0..63).  RFC 4733 §2.3.4.
 *
 * The **duration** field is the 16-bit big-endian pair of bytes 2 and
 * 3.  RFC 4733 §2.3.5 states its unit: "in timestamp units".  The
 * timestamp rate of the telephone-event payload is the one declared
 * in the SDP's `a=rtpmap:` line — 8000 Hz by default (RFC 4733
 * §2.4) — so one duration unit is 1/8000 s = 0.125 ms, *not* one
 * millisecond.
 *
 * That distinction is not academic: a peer that reads the duration as
 * milliseconds sees a key press that is eight times too long.  The
 * codec therefore converts at the boundary — `decode()` reports
 * milliseconds to the host, `encode()` converts the host's
 * milliseconds back into 8000 Hz timestamp units — and the conversion
 * is the only place the two units meet.
 */

#include <nimrtc/dtmf/dtmf.hpp>

namespace nimrtc::dtmf {

namespace {

/** The payload length of a telephone-event payload (RFC 4733 §2.3). */
constexpr std::size_t kPayloadSize = 4;

/** The end-of-event bit: the most significant bit of byte 1
 *  (RFC 4733 §2.3.2). */
constexpr std::uint8_t kEndBit = 0x80;

/** The reserved bit of byte 1 (RFC 4733 §2.3.3).  The sender must
 *  set it to zero; the receiver ignores it. */
constexpr std::uint8_t kReservedBit = 0x40;

/** The volume field: the six low bits of byte 1 (RFC 4733 §2.3.4). */
constexpr std::uint8_t kVolumeMask = 0x3F;

/** The clock rate of the telephone-event payload type (RFC 4733 §2.4:
 *  "The default clock frequency is 8000 Hz").  It is the unit of the
 *  duration field (§2.3.5) as well as the unit of the RTP timestamp
 *  the event packets carry. */
constexpr std::uint32_t kEventClockRateHz = 8000;

/** Convert a duration in timestamp units into milliseconds.
 *
 *  One timestamp unit is 1/8000 s = 0.125 ms, so the conversion
 *  multiplies by 125 and divides by 1000.  The division is done first
 *  to keep the intermediate value inside a `uint32_t` for the largest
 *  possible duration (65535 units = 8191.875 ms). */
constexpr std::uint16_t units_to_ms(std::uint16_t units) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(units) * 125u) / 1000u);
}

/** Convert a duration in milliseconds into timestamp units.
 *
 *  The inverse of `units_to_ms()`, rounded to the nearest unit so a
 *  host's millisecond value survives the round trip as closely as the
 *  0.125 ms granularity allows. */
constexpr std::uint16_t ms_to_units(std::uint16_t ms) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(ms) * kEventClockRateHz + 500u) / 1000u);
}

} // namespace

// ---------------------------------------------------------------------------
// IPlugin
// ---------------------------------------------------------------------------

const char* TelephoneEventCodec::name() const noexcept {
    return "NimRTC-DTMF-RFC4733";
}

plugins::Status TelephoneEventCodec::open() noexcept {
    // The codec is stateless — there is nothing to allocate.  The open
    // phase exists because the plugin lifecycle demands it.
    return plugins::kOk;
}

void TelephoneEventCodec::close() noexcept {
    // Nothing to release.
}

// ---------------------------------------------------------------------------
// IDtmfCodec
// ---------------------------------------------------------------------------

bool TelephoneEventCodec::decode(std::string_view payload,
                                 plugins::DtmfEvent& out) const {
    // RFC 4733 §2.3: the payload is exactly four bytes.
    if (payload.size() != kPayloadSize) return false;

    const auto* p = reinterpret_cast<const std::uint8_t*>(payload.data());

    const std::uint8_t code = p[0];
    // RFC 4733 §2.1 defines the event codes 0..16.  Anything above is
    // not a telephone event we can interpret.
    if (code > kMaxEventCode) return false;

    const std::uint8_t byte1 = p[1];
    const bool end = (byte1 & kEndBit) != 0;
    // The reserved bit (R) is ignored on receive (RFC 4733 §2.3.3).
    // The volume is parsed for the layout to line up; the host does not
    // consume it today, but it is part of the payload.

    // Duration: the big-endian 16-bit pair of bytes 2 and 3, in
    // timestamp units (RFC 4733 §2.3.5).
    const std::uint16_t units =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[2]) << 8) | p[3]);

    out.code        = code;
    out.duration_ms = units_to_ms(units);
    out.end         = end;
    return true;
}

std::string TelephoneEventCodec::encode(const plugins::DtmfEvent& event) const {
    // RFC 4733 §2.1: the event codes are 0..16.
    if (event.code > kMaxEventCode) return {};

    std::string payload;
    payload.resize(kPayloadSize);
    auto* p = reinterpret_cast<std::uint8_t*>(payload.data());

    p[0] = event.code;
    // Byte 1: the E bit in the most significant position, the reserved
    // bit cleared, and the volume in the six low bits (RFC 4733 §2.3).
    p[1] = static_cast<std::uint8_t>(
        (event.end ? kEndBit : 0x00) |
        (kDefaultVolume & kVolumeMask));
    // Duration: milliseconds converted into 8000 Hz timestamp units.
    const std::uint16_t units = ms_to_units(event.duration_ms);
    p[2] = static_cast<std::uint8_t>((units >> 8) & 0xFF);
    p[3] = static_cast<std::uint8_t>(units & 0xFF);
    return payload;
}

std::uint8_t TelephoneEventCodec::payload_type() const noexcept {
    return kDefaultPayloadType;
}

} // namespace nimrtc::dtmf
