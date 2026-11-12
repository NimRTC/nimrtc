/**
 * @file src/modules/rtp/src/rtcp_tmmbr.cpp
 * @brief TMMBR / TMMBN — RFC 5104 §4.2 implementation.
 *
 * v1.2.0 (session capability plan X3 / X4).  See nimrtc/rtp/rtcp_tmmbr.hpp
 * for the wire layout.
 */
#include <nimrtc/rtp/rtcp_tmmbr.hpp>

#include <algorithm>
#include <limits>
#include <string>

namespace nimrtc::rtp {

namespace {

/// The RTPFB payload type (RFC 4585 §6.4).
constexpr std::uint8_t kPtRtpfb = 205;

/// The FMT values of the two messages (RFC 5104 §4.2).
constexpr std::uint8_t kFmtTmmbr = 3;
constexpr std::uint8_t kFmtTmmbn = 4;

/// The RTPFB header plus the two SSRC fields: 12 bytes (RFC 4585 §6.1).
constexpr std::size_t kRtpfbHeaderSize = 12;

/// One FCI entry is 8 bytes (RFC 5104 §4.2.1.1).
constexpr std::size_t kFciEntrySize = 8;

/// The MxTBR mantissa is 17 bits (RFC 5104 §4.2.1.1).
constexpr std::uint64_t kMaxMantissa = 0x1FFFFu;

/// The MxTBR exponent is 6 bits (RFC 5104 §4.2.1.1).
constexpr std::uint8_t kMaxExponent = 63;

/// The measured overhead is 9 bits (RFC 5104 §4.2.1.1).
constexpr std::uint16_t kMaxOverhead = 511;

inline std::uint16_t read_be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(p[0]) << 8) |
         static_cast<std::uint16_t>(p[1]));
}

inline std::uint32_t read_be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8)  |
            static_cast<std::uint32_t>(p[3]);
}

inline void write_be16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<std::uint8_t>(v & 0xFF);
}

inline void write_be32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<std::uint8_t>(v & 0xFF);
}

/// Decode one FCI entry (RFC 5104 §4.2.1.1).
///
/// The 32-bit tail packs the 6-bit exponent, the 17-bit mantissa and
/// the 9-bit overhead:
///
/// ```text
///  byte 0: | exp (6) | mantissa[16:15]  |
///  byte 1: |        mantissa[14:7]     |
///  byte 2: | mantissa[6:0] | ovh[8]   |
///  byte 3: |          overhead[7:0]     |
/// ```
TmmbrEntry decode_fci(const std::uint8_t* p) noexcept {
    TmmbrEntry entry;
    entry.ssrc = read_be32(p);

    const std::uint8_t exp = static_cast<std::uint8_t>(p[4] >> 2);
    const std::uint64_t mantissa =
        (static_cast<std::uint64_t>(p[4] & 0x03u) << 15) |
        (static_cast<std::uint64_t>(p[5]) << 7) |
         static_cast<std::uint64_t>(p[6] >> 1);

    entry.measured_overhead_bytes = static_cast<std::uint16_t>(
        ((static_cast<std::uint16_t>(p[6] & 0x01u)) << 8) | p[7]);

    // The value is `mantissa * 2^exp`; the shift must not overflow.
    if (mantissa > (std::numeric_limits<std::uint64_t>::max() >> exp)) {
        entry.bitrate_bps = std::numeric_limits<std::uint64_t>::max();
    } else {
        entry.bitrate_bps = mantissa << exp;
    }

    return entry;
}

/// Encode one FCI entry (RFC 5104 §4.2.1.1).
///
/// The bit rate is split into the smallest exponent that fits the
/// 17-bit mantissa — that keeps the most precision — and the mantissa
/// itself.  A bit rate that does not divide exactly is truncated
/// downwards, so the reported limit is never *above* the real one.
void encode_fci(std::uint8_t* p, const TmmbrEntry& entry) noexcept {
    write_be32(p, entry.ssrc);

    std::uint8_t exp = 0;
    std::uint64_t mantissa = entry.bitrate_bps;
    // The smallest exponent whose shifted value fits the 17-bit
    // mantissa.  `bitrate >> 63` is at most 1, so the loop always
    // terminates with a fitting mantissa.
    while (exp < kMaxExponent && (mantissa >> exp) > kMaxMantissa) {
        ++exp;
    }
    mantissa >>= exp;

    const std::uint16_t overhead =
        std::min<std::uint16_t>(entry.measured_overhead_bytes, kMaxOverhead);

    // `exp` is a std::uint8_t, so `exp << 2` promotes to int.  The
    // mantissa half is unsigned 64-bit, and mixing an int with it in
    // one `|` makes Clang's -Wsign-conversion fire on the int side.
    // Widening the exponent to uint64_t keeps both operands unsigned.
    const std::uint64_t exp_field = static_cast<std::uint64_t>(exp) << 2;

    p[4] = static_cast<std::uint8_t>(exp_field |
                                     ((mantissa >> 15) & 0x03u));
    p[5] = static_cast<std::uint8_t>((mantissa >> 7) & 0xFFu);
    p[6] = static_cast<std::uint8_t>(((mantissa & 0x7Fu) << 1) |
                                     ((overhead >> 8) & 0x01u));
    p[7] = static_cast<std::uint8_t>(overhead & 0xFFu);
}

/// The shared body of the two parsers.
///
/// @param raw  The whole RTCP sub-packet, header included.
/// @param fmt  The FMT the caller expects.
core::Result<TmmbrMessage> parse_fci_message(core::ByteSpan raw,
                                              std::uint8_t fmt,
                                              const char* what) noexcept {
    if (raw.size() < kRtpfbHeaderSize) {
        return core::Result<TmmbrMessage>::fail(
            core::ErrorCode::ProtocolError,
            std::string("RTCP ") + what + " too small: " +
            std::to_string(raw.size()) + " < " +
            std::to_string(kRtpfbHeaderSize) + " bytes");
    }

    const std::uint8_t b0 = raw[0];
    const std::uint8_t version = static_cast<std::uint8_t>((b0 >> 6) & 0x03u);
    if (version != 2) {
        return core::Result<TmmbrMessage>::fail(
            core::ErrorCode::ProtocolError,
            std::string("RTCP ") + what + " version != 2: got " +
            std::to_string(version));
    }
    if (raw[1] != kPtRtpfb) {
        return core::Result<TmmbrMessage>::fail(
            core::ErrorCode::ProtocolError,
            std::string("RTCP ") + what + " payload type != 205 (RTPFB): got " +
            std::to_string(static_cast<int>(raw[1])));
    }
    const std::uint8_t got_fmt = static_cast<std::uint8_t>(b0 & 0x1Fu);
    if (got_fmt != fmt) {
        return core::Result<TmmbrMessage>::fail(
            core::ErrorCode::ProtocolError,
            std::string("RTCP ") + what + " FMT != " + std::to_string(fmt) +
            ": got " + std::to_string(got_fmt));
    }

    // The length field delimits the message; never read past it.
    // A length field that reaches past the buffer is malformed: the
    // message was truncated on the way in.
    const std::uint16_t words = read_be16(raw.data() + 2);
    const std::size_t declared =
        (static_cast<std::size_t>(words) + 1u) * 4u;
    if (declared > raw.size()) {
        return core::Result<TmmbrMessage>::fail(
            core::ErrorCode::ProtocolError,
            std::string("RTCP ") + what + " length field claims " +
            std::to_string(declared) + " B, only " +
            std::to_string(raw.size()) + " B available");
    }
    const std::size_t total = declared;

    const std::size_t payload = total - kRtpfbHeaderSize;
    if (payload % kFciEntrySize != 0) {
        return core::Result<TmmbrMessage>::fail(
            core::ErrorCode::ProtocolError,
            std::string("RTCP ") + what + " FCI payload " +
            std::to_string(payload) + " B is not a multiple of " +
            std::to_string(kFciEntrySize) + " B");
    }

    const std::size_t count = payload / kFciEntrySize;
    TmmbrMessage msg;
    msg.sender_ssrc = read_be32(raw.data() + 4);
    msg.entries.reserve(count);
    const std::uint8_t* p = raw.data() + kRtpfbHeaderSize;
    for (std::size_t i = 0; i < count; ++i) {
        msg.entries.push_back(decode_fci(p));
        p += kFciEntrySize;
    }

    return core::Result<TmmbrMessage>::ok(std::move(msg));
}

/// The shared body of the two parsers.
///
/// @param raw  The whole RTCP sub-packet, header included.
/// @param fmt  The FMT the caller expects.
core::Result<std::vector<TmmbrEntry>> parse_fci(core::ByteSpan raw,
                                                 std::uint8_t fmt,
                                                 const char* what) noexcept {
    auto msg = parse_fci_message(raw, fmt, what);
    if (!msg) {
        return core::Result<std::vector<TmmbrEntry>>::fail(msg.error());
    }
    return core::Result<std::vector<TmmbrEntry>>::ok(std::move(msg.value().entries));
}

/// The shared body of the two builders.
core::ByteBuffer build_fci(std::uint8_t fmt,
                           std::uint32_t sender_ssrc,
                           const std::vector<TmmbrEntry>& entries) {
    // RFC 5104 §4.2.1.1: a TMMBR carries at least one FCI entry; a
    // TMMBN may carry none (§4.2.2.2).  The caller decides which
    // message it is building; here the distinction is the caller's.
    const std::size_t total =
        kRtpfbHeaderSize + entries.size() * kFciEntrySize;

    core::ByteBuffer out(total, 0);
    // V=2, FMT, no padding (RFC 4585 §6.1).
    out[0] = static_cast<std::uint8_t>(0x80u | (fmt & 0x1Fu));
    out[1] = kPtRtpfb;
    // The length field counts 32-bit words minus one: 2 + 2*N.
    write_be16(out.data() + 2, static_cast<std::uint16_t>(total / 4u - 1u));
    write_be32(out.data() + 4, sender_ssrc);
    // The "SSRC of media source" field is unused and SHALL be 0
    // (RFC 5104 §4.2.1.2).
    write_be32(out.data() + 8, 0);

    std::uint8_t* p = out.data() + kRtpfbHeaderSize;
    for (const TmmbrEntry& entry : entries) {
        encode_fci(p, entry);
        p += kFciEntrySize;
    }

    return out;
}

}  // namespace


// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

core::Result<std::vector<TmmbrEntry>> parse_tmmbr(core::ByteSpan raw) noexcept {
    return parse_fci(raw, kFmtTmmbr, "TMMBR");
}


core::Result<std::vector<TmmbrEntry>> parse_tmmbn(core::ByteSpan raw) noexcept {
    return parse_fci(raw, kFmtTmmbn, "TMMBN");
}


core::Result<TmmbrMessage> parse_tmmbr_message(core::ByteSpan raw) noexcept {
    return parse_fci_message(raw, kFmtTmmbr, "TMMBR");
}


core::Result<TmmbrMessage> parse_tmmbn_message(core::ByteSpan raw) noexcept {
    return parse_fci_message(raw, kFmtTmmbn, "TMMBN");
}


// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

core::ByteBuffer build_tmmbr(std::uint32_t sender_ssrc,
                             const std::vector<TmmbrEntry>& entries) {
    // RFC 5104 §4.2.1.1: "The FCI field of a TMMBR message SHALL
    // contain one or more FCI entries."  A message with none is
    // malformed, so nothing is built.
    if (entries.empty()) return {};
    return build_fci(kFmtTmmbr, sender_ssrc, entries);
}


core::ByteBuffer build_tmmbn(std::uint32_t sender_ssrc,
                             const std::vector<TmmbrEntry>& entries) {
    // An empty TMMBN is legal and meaningful (RFC 5104 §4.2.2.2).
    return build_fci(kFmtTmmbn, sender_ssrc, entries);
}

}  // namespace nimrtc::rtp
