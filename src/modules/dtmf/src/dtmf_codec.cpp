/**
 * @file src/modules/dtmf/src/dtmf_codec.cpp
 * @brief The RFC 4733 (telephone-event) codec.
 *
 * v1.2.0 — `docs/plan/session-capability-plan.md` D1 / D2.
 *
 * The wire format is four bytes (RFC 4733 §2.3.1):
 *
 *     0                   1                   2                   3
 *     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |   event code  |E|   volume   |          duration           |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * The decoder is deliberately strict: a payload of the wrong length, or
 * one carrying an event code outside 0..16, is *rejected* rather than
 * guessed at.  A malformed telephone-event payload is a peer bug (or
 * an attack), and silently decoding it into "some key" would hand the
 * host a fabricated event.
 */

#include <nimrtc/dtmf/dtmf.hpp>

namespace nimrtc::dtmf {

namespace {

/** The payload length of a telephone-event payload (RFC 4733 §2.3.1). */
constexpr std::size_t kPayloadSize = 4;

/** The bit of byte 1 that carries the end-of-event flag. */
constexpr std::uint8_t kEndBit = 0x01;

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
    // RFC 4733 §2.3.1: the payload is exactly four bytes.
    if (payload.size() != kPayloadSize) return false;

    const auto* p = reinterpret_cast<const std::uint8_t*>(payload.data());

    const std::uint8_t code = p[0];
    // RFC 4733 §2.1 defines the event codes 0..16.  Anything above is
    // not a telephone event we can interpret.
    if (code > kMaxEventCode) return false;

    const std::uint8_t byte1 = p[1];
    const bool end = (byte1 & kEndBit) != 0;
    // The volume (bits 6..1 of byte 1) is not consumed by the host
    // today, but it is part of the payload and has to be parsed for
    // the layout to line up.

    const std::uint16_t duration_ms =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[2]) << 8) | p[3]);

    out.code       = code;
    out.duration_ms = duration_ms;
    out.end        = end;
    return true;
}

std::string TelephoneEventCodec::encode(const plugins::DtmfEvent& event) const {
    // RFC 4733 §2.1: the event codes are 0..16.
    if (event.code > kMaxEventCode) return {};

    std::string payload;
    payload.resize(kPayloadSize);
    auto* p = reinterpret_cast<std::uint8_t*>(payload.data());

    p[0] = event.code;
    p[1] = static_cast<std::uint8_t>(
        (event.end ? kEndBit : 0x00) |
        static_cast<std::uint8_t>((kDefaultVolume & 0x3F) << 1));
    p[2] = static_cast<std::uint8_t>((event.duration_ms >> 8) & 0xFF);
    p[3] = static_cast<std::uint8_t>(event.duration_ms & 0xFF);
    return payload;
}

std::uint8_t TelephoneEventCodec::payload_type() const noexcept {
    return kDefaultPayloadType;
}

} // namespace nimrtc::dtmf
