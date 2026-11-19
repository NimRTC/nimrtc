/**
 * @file src/modules/rtx/src/rtx.cpp
 * @brief The RFC 4588 §4.1 retransmission payload format.
 *
 * The whole format is small, and that is the point: the two ends of a
 * retransmission have to agree on it byte-for-byte, so there is exactly
 * one implementation of it in the tree and both sides use it.
 *
 * The normative text (RFC 4588 §4.1):
 *
 *   "The payload of the RTP retransmission packet comprises the
 *    retransmission payload header followed by the payload of the
 *    original RTP packet.  The length of the retransmission payload
 *    header is 2 octets.  This payload header contains only one field,
 *    OSN (original sequence number), which MUST be set to the sequence
 *    number of the associated original RTP packet.  The original RTP
 *    packet payload, including any possible payload headers specific to
 *    the original payload type, MUST be placed right after the
 *    retransmission payload header."
 *
 * So: two octets, big-endian, then the original payload verbatim.
 */

#include <nimrtc/rtx/rtx.hpp>

#include <algorithm>
#include <cstring>

namespace nimrtc::rtx {

namespace {

/** Parse a decimal number out of a string_view.
 *
 *  Returns std::nullopt when the view is empty or carries anything
 *  that is not a digit — a malformed parameter is *absent*, not zero,
 *  because the two mean different things (an absent `apt=` says the
 *  binding was not declared; `apt=0` says it was declared and names
 *  payload type 0). */
std::optional<std::uint32_t> parse_decimal(std::string_view s) noexcept {
    if (s.empty()) return std::nullopt;
    std::uint32_t value = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        // Overflow guard: a payload type is 7 bits and a time is small,
        // so anything past this is malformed rather than large.
        if (value > 0x0FFFFFFFu) return std::nullopt;
        value = value * 10u + static_cast<std::uint32_t>(c - '0');
    }
    return value;
}

/** Find the value of `name=` inside an `a=fmtp:` body.
 *
 *  The body is a semicolon-separated list of `key=value` pairs
 *  (RFC 4566 §6 / the format-parameter syntax of RFC 6184 §8.2), and
 *  the parameter we want may be anywhere in it.  The comparison is
 *  case-insensitive because the parameter names in the IANA registry
 *  are lower-case and a peer that capitalises them is not trying to
 *  say something different.
 *
 *  @return The value that follows `name=`, or std::nullopt. */
std::optional<std::string_view>
find_param(std::string_view body, std::string_view name) noexcept {
    std::size_t pos = 0;
    while (pos <= body.size()) {
        // Isolate the next `key=value` pair.
        std::size_t end = body.find(';', pos);
        if (end == std::string_view::npos) end = body.size();
        std::string_view pair = body.substr(pos, end - pos);

        const std::size_t eq = pair.find('=');
        if (eq != std::string_view::npos) {
            std::string_view key = pair.substr(0, eq);
            // Trim the whitespace a peer may have put around the key.
            while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) {
                key.remove_prefix(1);
            }
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
                key.remove_suffix(1);
            }
            if (key.size() == name.size()) {
                bool same = true;
                for (std::size_t i = 0; i < key.size(); ++i) {
                    const char a = key[i];
                    const char b = name[i];
                    const bool a_lower = (a >= 'A' && a <= 'Z')
                                             ? static_cast<char>(a - 'A' + 'a')
                                             : a;
                    const bool b_lower = (b >= 'A' && b <= 'Z')
                                             ? static_cast<char>(b - 'A' + 'a')
                                             : b;
                    if (a_lower != b_lower) { same = false; break; }
                }
                if (same) {
                    std::string_view value = pair.substr(eq + 1);
                    while (!value.empty() &&
                           (value.front() == ' ' || value.front() == '\t')) {
                        value.remove_prefix(1);
                    }
                    while (!value.empty() &&
                           (value.back() == ' ' || value.back() == '\t')) {
                        value.remove_suffix(1);
                    }
                    return value;
                }
            }
        }
        if (end == body.size()) break;
        pos = end + 1;
    }
    return std::nullopt;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Building an RTX payload
// ---------------------------------------------------------------------------

std::size_t build_payload(core::MutableByteSpan out,
                          std::uint16_t osn,
                          core::ByteSpan original) noexcept {
    const std::size_t needed = kHeaderSize + original.size();
    if (out.size() < needed) return 0;

    // The OSN is written in network byte order (RFC 3550 §5.1's rule
    // for every multi-octet field in an RTP header, applied to the
    // payload header by RFC 4588 §4.1).
    out[0] = static_cast<std::uint8_t>((osn >> 8) & 0xFFu);
    out[1] = static_cast<std::uint8_t>(osn & 0xFFu);

    if (!original.empty()) {
        std::memcpy(out.data() + kHeaderSize, original.data(), original.size());
    }
    return needed;
}

// ---------------------------------------------------------------------------
// Parsing an RTX payload
// ---------------------------------------------------------------------------

core::Result<ParsedPayload> parse_payload(core::ByteSpan payload) noexcept {
    if (payload.size() < kHeaderSize) {
        return core::Result<ParsedPayload>::fail(
            core::ErrorCode::ProtocolError,
            "rtx: payload is shorter than the two-octet RFC 4588 header (" +
                std::to_string(payload.size()) + " bytes)");
    }
    ParsedPayload out;
    out.osn = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(payload[0]) << 8) |
         static_cast<std::uint16_t>(payload[1]));
    out.original = payload.subspan(kHeaderSize);
    return core::Result<ParsedPayload>::ok(out);
}

core::Result<void> parse_payload_into(core::ByteSpan payload,
                                      std::uint16_t& out_osn,
                                      core::ByteSpan& out_body,
                                      core::MutableByteSpan storage) noexcept {
    if (payload.size() < kHeaderSize) {
        return core::Result<void>::fail(
            core::ErrorCode::ProtocolError,
            "rtx: payload is shorter than the two-octet RFC 4588 header (" +
                std::to_string(payload.size()) + " bytes)");
    }
    const std::size_t body_size = payload.size() - kHeaderSize;
    if (storage.size() < body_size) {
        return core::Result<void>::fail(
            core::ErrorCode::InvalidArgument,
            "rtx: the scratch buffer is too small (" +
                std::to_string(storage.size()) + " < " +
                std::to_string(body_size) + ")");
    }
    out_osn = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(payload[0]) << 8) |
         static_cast<std::uint16_t>(payload[1]));
    if (body_size > 0) {
        std::memcpy(storage.data(), payload.data() + kHeaderSize, body_size);
    }
    out_body = core::ByteSpan{storage.data(), body_size};
    return core::Result<void>::make_ok();
}

// ---------------------------------------------------------------------------
// The SDP parameters (RFC 4588 §8.1)
// ---------------------------------------------------------------------------

std::optional<std::uint8_t> parse_apt(std::string_view fmtp_body) noexcept {
    const auto raw = find_param(fmtp_body, "apt");
    if (!raw) return std::nullopt;
    const auto value = parse_decimal(*raw);
    if (!value) return std::nullopt;
    // A payload type is 7 bits (RFC 3551 §4.5: 0..127).
    if (*value > 127u) return std::nullopt;
    return static_cast<std::uint8_t>(*value);
}

std::optional<std::uint32_t> parse_rtx_time_ms(
    std::string_view fmtp_body) noexcept {
    const auto raw = find_param(fmtp_body, "rtx-time");
    if (!raw) return std::nullopt;
    return parse_decimal(*raw);
}

std::string build_fmtp(std::uint8_t apt,
                       std::optional<std::uint32_t> rtx_time_ms) {
    std::string out = "apt=" + std::to_string(apt);
    if (rtx_time_ms.has_value()) {
        out += ";rtx-time=" + std::to_string(*rtx_time_ms);
    }
    return out;
}

} // namespace nimrtc::rtx
