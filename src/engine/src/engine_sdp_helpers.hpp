/**
 * @file src/engine/src/engine_sdp_helpers.hpp
 * @brief PRIVATE engine helpers: SDP parsing, direction algebra and
 *        byte-level encodings (base64, hex).
 *
 * Architecture remediation plan §4.3 (D1 — engine split): these helpers
 * used to live in `engine_impl.hpp`, which made the engine's internal
 * header a 1100-line catch-all.  They are grouped here by *concern* so
 * that `engine_impl.hpp` can shrink to the state it actually owns.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).  It is header-only on purpose —
 * every function here is a small, `inline` leaf that is called from
 * more than one engine translation unit, and inlining them keeps the
 * hot paths free of a cross-TU call.
 *
 * ## What belongs here
 *
 *   - the SDP direction algebra (`to_sdp_direction`, `from_sdp_direction`,
 *     `invert_sdp_direction`, `intersect_direction`);
 *   - the payload-type parser;
 *   - the base64 and hex-colon decoders (DTLS fingerprint, ICE ufrag /
 *     password).
 */
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/time.hpp>
#include <nimrtc/sdp/session_description.hpp>
#include <nimrtc/plugins/transport.hpp>

namespace nimrtc::engine::detail {

/** Parse a payload type out of an SDP `a=rtpmap:<fmt>` token.
 *
 *  The token is the format as it appeared in the SDP: a decimal number
 *  for both the static (0..95) and the dynamic (96..127) ranges.  A
 *  wildcard or a non-numeric token yields false.
 *
 *  Deliberately hand-rolled rather than `std::stoul`: the caller runs
 *  inside a `noexcept` function, where a thrown `std::invalid_argument`
 *  would terminate the process. */
inline bool parse_payload_type(std::string_view fmt, std::uint8_t& out) noexcept {
    if (fmt.empty() || fmt.size() > 3) return false;
    std::uint32_t v = 0;
    for (char c : fmt) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<std::uint32_t>(c - '0');
        if (v > 127) return false;
    }
    out = static_cast<std::uint8_t>(v);
    return true;
}

/** A fresh session id for the SDP `o=` line: milliseconds since the epoch
 *  plus 16 random bits.  Unique enough for the lifetime of one process,
 *  and it carries a timestamp so the id alone hints at the ordering. */
inline std::string make_session_id() {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    std::random_device rd;
    std::mt19937_64 mt(rd());
    return std::to_string(now) + std::to_string(mt() & 0xFFFF);
}

/** Decode a base64 string into bytes.
 *
 *  Used for the ICE ufrag / password and for anything else that arrives
 *  base64-encoded in an SDP attribute.  Unknown bytes are skipped
 *  rather than treated as an error: a malformed attribute must not
 *  abort the SDP parse, and the caller's validation (the ufrag has to
 *  be non-empty, the fingerprint has to have the right length) is the
 *  real gate. */
inline std::vector<std::uint8_t> base64_decode(std::string_view sv) {
    static const auto kIdx = []{
        std::array<int8_t, 256> t{};
        t.fill(-1);
        const char* a =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) t[static_cast<uint8_t>(a[i])] = static_cast<int8_t>(i);
        return t;
    }();
    std::vector<std::uint8_t> raw;
    raw.reserve((sv.size() * 3) / 4);
    int val = 0, bits = 0;
    for (char c : sv) {
        if (c == '=') break;
        // `char` is signed on x86_64 but UNSIGNED on aarch64 and on
        // macOS arm64, so a `c < 0` guard would be an always-false
        // comparison there -- rejected by -Werror=type-limits.  It is
        // also redundant: kIdx is a 256-entry table initialised to -1
        // and only the 64 base64 alphabet bytes are given a non-negative
        // index, so kIdx[byte] < 0 already rejects every non-alphabet
        // byte, including all bytes >= 0x80.  Iterate over the byte
        // value directly instead of the (signedness-dependent) char.
        const auto uc = static_cast<unsigned char>(c);
        const int8_t idx = kIdx[uc];
        if (idx < 0) continue;
        val = (val << 6) | idx;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            raw.push_back(static_cast<std::uint8_t>((val >> bits) & 0xff));
        }
    }
    return raw;
}

/** Decode an RFC 8122 DTLS fingerprint value: uppercase hex bytes
 *  separated by ':'.
 *
 *  Example: "AA:BB:CC:..." -> {0xAA, 0xBB, 0xCC, ...}.  Tolerant of
 *  whitespace.  Returns an empty vector on malformed input. */
inline std::vector<std::uint8_t> hex_colon_decode(std::string_view sv) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        return -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve(sv.size() / 3 + 1);
    int hi = -1;
    for (char c : sv) {
        if (c == ':' || c == ' ' || c == '\t') {
            if (hi >= 0) return {};     // dangling half-byte
            continue;
        }
        int v = hex(c);
        if (v < 0) return {};
        if (hi < 0) hi = v;
        else {
            out.push_back(static_cast<std::uint8_t>((hi << 4) | v));
            hi = -1;
        }
    }
    if (hi >= 0) return {};            // dangling half-byte at end
    return out;
}

/** `plugins::Direction` → `sdp::Direction` (the two enums carry the same
 *  four RFC 4566 states, one-to-one).
 *
 *  v1.2.0 (session capability plan, N4): the session model speaks
 *  `plugins::Direction`, the SDP layer speaks `sdp::Direction`. */
inline sdp::Direction to_sdp_direction(plugins::Direction dir) noexcept {
    switch (dir) {
        case plugins::Direction::kSendOnly: return sdp::Direction::SendOnly;
        case plugins::Direction::kRecvOnly: return sdp::Direction::RecvOnly;
        case plugins::Direction::kInactive:  return sdp::Direction::Inactive;
        case plugins::Direction::kSendRecv:  break;
    }
    return sdp::Direction::SendRecv;
}

/** `sdp::Direction` → `plugins::Direction`. */
inline plugins::Direction from_sdp_direction(sdp::Direction dir) noexcept {
    switch (dir) {
        case sdp::Direction::SendOnly: return plugins::Direction::kSendOnly;
        case sdp::Direction::RecvOnly: return plugins::Direction::kRecvOnly;
        case sdp::Direction::Inactive:  return plugins::Direction::kInactive;
        case sdp::Direction::SendRecv:  break;
    }
    return plugins::Direction::kSendRecv;
}

/** The inverse of a media direction — what *we* can do when the peer
   *  says what *it* can do (RFC 3264 §6.1).
 *
 *  `sendrecv` stays `sendrecv`, `sendonly` becomes `recvonly`,
 *  `recvonly` becomes `sendonly`, `inactive` stays `inactive`. */
inline sdp::Direction invert_sdp_direction(sdp::Direction dir) noexcept {
    switch (dir) {
        case sdp::Direction::SendOnly: return sdp::Direction::RecvOnly;
        case sdp::Direction::RecvOnly: return sdp::Direction::SendOnly;
        case sdp::Direction::Inactive:  return sdp::Direction::Inactive;
        case sdp::Direction::SendRecv:  break;
    }
    return sdp::Direction::SendRecv;
}

/** Intersect two media directions (RFC 3264 §6.1: the answer's
 *  direction is the offerer's capability intersected with the
 *  answerer's).
 *
 *  The intersection is the *weaker* of the two in each direction:
 *  `ours` says what we can do, `theirs` says what the peer can do,
 *  and the negotiated direction says what both can do.
 *
 *  | ours     | theirs   | result    |
 *  |----------|----------|-----------|
 *  | sendrecv | sendrecv | sendrecv  |
 *  | sendrecv | sendonly | sendonly  |
 *  | sendrecv | recvonly | recvonly  |
 *  | sendrecv | inactive | inactive  |
 *  | sendonly | sendrecv | sendonly  |
 *  | sendonly | sendonly | sendonly  |
 *  | sendonly | recvonly | inactive  |
 *  | sendonly | inactive | inactive  |
 *  | recvonly | *        | recvonly  |
 *  | inactive | *        | inactive  |
 *
 *  v1.2.0 (session capability plan, N4 / OD-A): the answer path uses
 *  this to honour the session model's transceiver direction instead of
 *  assuming a sendrecv answerer. */
inline sdp::Direction intersect_direction(sdp::Direction ours,
                                           sdp::Direction theirs) noexcept {
    if (ours == sdp::Direction::Inactive || theirs == sdp::Direction::Inactive) {
        return sdp::Direction::Inactive;
    }
    const bool we_send  = (ours == sdp::Direction::SendOnly ||
                           ours == sdp::Direction::SendRecv);
    const bool we_recv  = (ours == sdp::Direction::RecvOnly ||
                           ours == sdp::Direction::SendRecv);
    const bool they_send = (theirs == sdp::Direction::SendOnly ||
                            theirs == sdp::Direction::SendRecv);
    const bool they_recv = (theirs == sdp::Direction::RecvOnly ||
                            theirs == sdp::Direction::SendRecv);
    const bool send = we_send && they_recv;
    const bool recv = we_recv && they_send;
    if (send && recv) return sdp::Direction::SendRecv;
    if (send)         return sdp::Direction::SendOnly;
    if (recv)         return sdp::Direction::RecvOnly;
    return sdp::Direction::Inactive;
}

}  // namespace nimrtc::engine::detail
