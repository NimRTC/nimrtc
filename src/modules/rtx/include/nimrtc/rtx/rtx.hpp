/**
 * @file nimrtc/rtx/rtx.hpp
 * @brief The RFC 4588 retransmission payload format: OSN framing + SDP
 *        parameter parsing.
 *
 * ## What this module is
 *
 * RFC 4588 defines a *payload format* for retransmitted RTP packets.  A
 * retransmission is sent on a **different payload type** than the
 * original, so the receiver can tell the two apart, and it carries the
 * original packet's sequence number in a two-octet header so the
 * receiver can put the payload back where it belongs.
 *
 * The wire format (RFC 4588 §4.1):
 *
 * ```
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                          RTP Header                           |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |         OSN                   |  Original RTP Packet Payload  |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * ```
 *
 * The retransmission payload header is **exactly two octets** and holds
 * exactly one field: **OSN** (original sequence number), in network
 * byte order (big-endian).  The original packet's payload — including
 * any payload header specific to the original payload type — follows
 * immediately.
 *
 * @note **There is no `0x90` marker byte.**  Some descriptions of RTX
 *       mention one; RFC 4588 §4.1 does not define it, and a receiver
 *       that follows the RFC would read the marker as the high byte of
 *       the OSN.  This module implements the normative format.
 *
 * ## What this module is NOT
 *
 * It is a **format** module, not a transport.  It does not:
 *
 *   - build or parse RTP headers (that is `nimrtc::rtp`);
 *   - decide *whether* to retransmit (that is the sender's policy);
 *   - send or receive anything (that is the engine and the transport);
 *   - demultiplex the RTX stream from the media stream (that is the
 *     payload-type routing, which the engine owns).
 *
 * What it owns is the arithmetic that is easy to get wrong and that
 * both ends must agree on byte-for-byte: the two-octet framing, the
 * byte order, and the `apt=` SDP parameter that binds a retransmission
 * payload type to the payload type it protects.
 *
 * ## The SDP parameters (RFC 4588 §8.1)
 *
 *   - `a=rtpmap:<pt> rtx/90000` — the retransmission payload type.
 *     The clock rate MUST equal the clock rate of the associated
 *     original payload type (RFC 4588 §4).
 *   - `a=fmtp:<pt> apt=<original-pt>` — the **associated payload
 *     type**: how the receiver knows which stream a retransmission
 *     belongs to.
 *   - `a=fmtp:<pt> rtx-time=<ms>` — OPTIONAL: how long the sender keeps
 *     a packet available for retransmission.  The absence of the
 *     parameter means the maximum retransmission time is undefined.
 *
 * ## Dependencies
 *
 * `nimrtc::core` only (byte spans and results).  No vendor libraries;
 * pure C++20.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>

namespace nimrtc::rtx {

// ---------------------------------------------------------------------------
// The wire format's constants
// ---------------------------------------------------------------------------

/** The size of the RFC 4588 §4.1 retransmission payload header.
 *
 *  The header is exactly two octets: the OSN, in network byte order.
 *  Nothing else is allowed before the original payload. */
NIMRTC_API_STABLE inline constexpr std::size_t kHeaderSize = 2;

/** The size of a *complete* RTX payload: the header plus an original
 *  payload of at least one byte.
 *
 *  A retransmission whose original payload was empty is not a thing
 *  worth sending, so the minimum useful RTX payload is three octets. */
NIMRTC_API_STABLE inline constexpr std::size_t kMinPayloadSize = kHeaderSize + 1;

/** The MIME subtype RFC 4588 §8 registers for this payload format. */
NIMRTC_API_STABLE inline constexpr const char* kEncodingName = "rtx";

/** The clock rate of a video retransmission stream.
 *
 *  RFC 4588 §4: "the timestamp clockrate used by the retransmission
 *  payload type MUST be the same as the one used by the associated
 *  original payload type".  Every video codec NimRTC negotiates runs
 *  on the 90 kHz video clock (RFC 6184 §5.6, RFC 7798 §4.1), so 90000
 *  is the value the `a=rtpmap:` line carries. */
NIMRTC_API_STABLE inline constexpr std::uint32_t kVideoClockRateHz = 90000;

// ---------------------------------------------------------------------------
// Building an RTX payload
// ---------------------------------------------------------------------------

/** Wrap an original packet's payload in the RFC 4588 §4.1 header.
 *
 *  @param out      Destination buffer; must have room for
 *                  `kHeaderSize + original.size()` bytes.
 *  @param osn      The **original** sequence number — the sequence
 *                  number the original packet was sent under.  It is
 *                  written in network byte order (big-endian).
 *  @param original The original packet's payload, verbatim (including
 *                  any payload header the original payload type defines).
 *  @return The number of bytes written, or 0 when the destination is
 *          too small (nothing is written in that case). */
NIMRTC_API_STABLE std::size_t build_payload(core::MutableByteSpan out,
                                             std::uint16_t osn,
                                             core::ByteSpan original) noexcept;

/** The size an RTX payload for `original` would occupy. */
NIMRTC_API_STABLE constexpr std::size_t payload_size_for(
    core::ByteSpan original) noexcept {
    return kHeaderSize + original.size();
}

// ---------------------------------------------------------------------------
// Parsing an RTX payload
// ---------------------------------------------------------------------------

/** The result of unwrapping an RTX payload. */
struct NIMRTC_API_STABLE ParsedPayload {
    /** The original sequence number (OSN), restored from the header. */
    std::uint16_t osn = 0;

    /** The original packet's payload — the bytes after the two-octet
     *  header, exactly as the original packet carried them. */
    core::ByteSpan original{};
};

/** Unwrap an RFC 4588 §4.1 retransmission payload.
 *
 *  @param payload The RTX payload (header + original payload).
 *  @return The restored original sequence number and payload, or a
 *          failure when the payload is too short to carry a header. */
NIMRTC_API_STABLE core::Result<ParsedPayload>
parse_payload(core::ByteSpan payload) noexcept;

/** Unwrap an RTX payload into caller-provided storage.
 *
 *  The variant that avoids an allocation on the hot path: the caller
 *  owns the buffer the original payload is copied into.
 *
 *  @param payload The RTX payload.
 *  @param out_osn  Receives the original sequence number.
 *  @param out_body Receives a span into `storage` holding the original
 *                  payload.
 *  @param storage  The scratch buffer the original payload is copied
 *                  into.  It must be at least
 *                  `payload.size() - kHeaderSize` bytes.
 *  @return kOk, or a failure when the payload is too short or the
 *          storage is too small. */
NIMRTC_API_STABLE core::Result<void>
parse_payload_into(core::ByteSpan payload,
                   std::uint16_t& out_osn,
                   core::ByteSpan& out_body,
                   core::MutableByteSpan storage) noexcept;

// ---------------------------------------------------------------------------
// The SDP parameters (RFC 4588 §8.1)
// ---------------------------------------------------------------------------

/** Parse the `apt=` (associated payload type) out of an `a=fmtp:` body.
 *
 *  RFC 4588 §8.1: the `apt` parameter MUST be used to map the
 *  retransmission payload type to the associated original stream payload
 *  type.  The body may carry other parameters too (`rtx-time`, and
 *  parameters of the original payload type), so the body is scanned
 *  rather than compared.
 *
 *  @param fmtp_body The fmtp body, e.g. "apt=102" or
 *                   "apt=102;rtx-time=200".  NOT including the
 *                   leading "a=fmtp:<pt> ".
 *  @return The associated payload type, or std::nullopt when the body
 *          carries no `apt=` parameter or its value is not a number. */
NIMRTC_API_STABLE std::optional<std::uint8_t>
parse_apt(std::string_view fmtp_body) noexcept;

/** Parse the `rtx-time=` parameter out of an `a=fmtp:` body.
 *
 *  RFC 4588 §8.1: the parameter is OPTIONAL and states how long (in
 *  milliseconds, measured from the first transmission) the sender keeps
 *  a packet available for retransmission.  Its absence means the maximum
 *  retransmission time is undefined.
 *
 *  @param fmtp_body The fmtp body.
 *  @return The value in milliseconds, or std::nullopt when the body
 *          carries no `rtx-time=` parameter or its value is not a
 *          number. */
NIMRTC_API_STABLE std::optional<std::uint32_t>
parse_rtx_time_ms(std::string_view fmtp_body) noexcept;

/** Build an `a=fmtp:` body for a retransmission payload type.
 *
 *  @param apt         The associated (original) payload type.
 *  @param rtx_time_ms The `rtx-time` value in milliseconds, or
 *                     std::nullopt to omit the parameter.
 *  @return The fmtp body, e.g. "apt=102" or "apt=102;rtx-time=200". */
NIMRTC_API_STABLE std::string build_fmtp(std::uint8_t apt,
                                          std::optional<std::uint32_t> rtx_time_ms);

} // namespace nimrtc::rtx
