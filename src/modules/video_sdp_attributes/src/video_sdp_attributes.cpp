/**
 * @file src/modules/video_sdp_attributes/src/video_sdp_attributes.cpp
 * @brief H.264/VP8/VP9 SDP fmtp parsers/builders.
 *
 * See video_sdp_attributes.hpp for design notes.
 */

#include <nimrtc/video_sdp_attributes/video_sdp_attributes.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace nimrtc::video_sdp_attributes {

namespace {

// ---------------------------------------------------------------------------
// Case-insensitive ASCII helpers (fmtp keys are case-insensitive per RFC 4566)
// ---------------------------------------------------------------------------

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

std::string_view strip(std::string_view s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

bool is_hex_char(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_base64_char(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
}

// ---------------------------------------------------------------------------
// URL-decode for fmtp values (RFC 4566 §6 "escape non-ASCII; convert
// CR/LF to NUL/CRLF/CR" but in practice SDP values use %xx for byte
// escaping; we keep it simple and support %xx only).
// ---------------------------------------------------------------------------

std::string url_decode(std::string_view in) noexcept {
    std::string out;
    out.reserve(in.size());
    auto hexv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    for (std::size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '%' && i + 2 < in.size()) {
            int hi = hexv(in[i + 1]);
            int lo = hexv(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(c);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Public: ParsedFmtp — manual ctor/dtor because of union
// ---------------------------------------------------------------------------

ParsedFmtp::ParsedFmtp() noexcept : kind(CodecKind::kUnknown) {
    new (&h264) H264Fmtp();
}

ParsedFmtp::~ParsedFmtp() {
    destroy_active();
}

ParsedFmtp::ParsedFmtp(const ParsedFmtp& other) : kind(other.kind) {
    switch (kind) {
        case CodecKind::kH264: new (&h264) H264Fmtp(other.h264); break;
        case CodecKind::kH265: new (&h265) H265Fmtp(other.h265); break;
        case CodecKind::kVP8:  new (&vp8)  Vp8Fmtp(other.vp8);   break;
        case CodecKind::kVP9:  new (&vp9)  Vp9Fmtp(other.vp9);   break;
        case CodecKind::kAV1:  new (&av1)  Av1Fmtp(other.av1);   break;
        default: new (&h264) H264Fmtp(); break;
    }
}

ParsedFmtp& ParsedFmtp::operator=(const ParsedFmtp& other) {
    if (this == &other) return *this;
    destroy_active();
    kind = other.kind;
    switch (kind) {
        case CodecKind::kH264: new (&h264) H264Fmtp(other.h264); break;
        case CodecKind::kH265: new (&h265) H265Fmtp(other.h265); break;
        case CodecKind::kVP8:  new (&vp8)  Vp8Fmtp(other.vp8);   break;
        case CodecKind::kVP9:  new (&vp9)  Vp9Fmtp(other.vp9);   break;
        case CodecKind::kAV1:  new (&av1)  Av1Fmtp(other.av1);   break;
        default: new (&h264) H264Fmtp(); break;
    }
    return *this;
}

ParsedFmtp::ParsedFmtp(ParsedFmtp&& other) noexcept : kind(other.kind) {
    switch (kind) {
        case CodecKind::kH264: new (&h264) H264Fmtp(std::move(other.h264)); break;
        case CodecKind::kH265: new (&h265) H265Fmtp(std::move(other.h265)); break;
        case CodecKind::kVP8:  new (&vp8)  Vp8Fmtp(std::move(other.vp8));   break;
        case CodecKind::kVP9:  new (&vp9)  Vp9Fmtp(std::move(other.vp9));   break;
        case CodecKind::kAV1:  new (&av1)  Av1Fmtp(std::move(other.av1));   break;
        default: new (&h264) H264Fmtp(); break;
    }
}

ParsedFmtp& ParsedFmtp::operator=(ParsedFmtp&& other) noexcept {
    if (this == &other) return *this;
    destroy_active();
    kind = other.kind;
    switch (kind) {
        case CodecKind::kH264: new (&h264) H264Fmtp(std::move(other.h264)); break;
        case CodecKind::kH265: new (&h265) H265Fmtp(std::move(other.h265)); break;
        case CodecKind::kVP8:  new (&vp8)  Vp8Fmtp(std::move(other.vp8));   break;
        case CodecKind::kVP9:  new (&vp9)  Vp9Fmtp(std::move(other.vp9));   break;
        case CodecKind::kAV1:  new (&av1)  Av1Fmtp(std::move(other.av1));   break;
        default: new (&h264) H264Fmtp(); break;
    }
    return *this;
}

void ParsedFmtp::destroy_active() noexcept {
    switch (kind) {
        case CodecKind::kH264: h264.~H264Fmtp(); break;
        case CodecKind::kH265: h265.~H265Fmtp(); break;
        case CodecKind::kVP8:  vp8.~Vp8Fmtp();   break;
        case CodecKind::kVP9:  vp9.~Vp9Fmtp();   break;
        case CodecKind::kAV1:  av1.~Av1Fmtp();   break;
        default: h264.~H264Fmtp(); break;
    }
}

void ParsedFmtp::set_h264(H264Fmtp v) {
    destroy_active();
    new (&h264) H264Fmtp(std::move(v));
    kind = CodecKind::kH264;
}

void ParsedFmtp::set_h265(H265Fmtp v) {
    destroy_active();
    new (&h265) H265Fmtp(std::move(v));
    kind = CodecKind::kH265;
}

void ParsedFmtp::set_vp8(Vp8Fmtp v) {
    destroy_active();
    new (&vp8) Vp8Fmtp(std::move(v));
    kind = CodecKind::kVP8;
}

void ParsedFmtp::set_vp9(Vp9Fmtp v) {
    destroy_active();
    new (&vp9) Vp9Fmtp(std::move(v));
    kind = CodecKind::kVP9;
}

void ParsedFmtp::set_av1(Av1Fmtp v) {
    destroy_active();
    new (&av1) Av1Fmtp(std::move(v));
    kind = CodecKind::kAV1;
}

// ---------------------------------------------------------------------------
// H264Fmtp::has_any
// ---------------------------------------------------------------------------

bool H264Fmtp::has_any() const noexcept {
    return packetization_mode.has_value()
        || profile_level_id.has_value()
        || sprop_sps.has_value()
        || sprop_pps.has_value()
        || max_mbps.has_value()
        || max_fs.has_value()
        || max_smbps.has_value()
        || max_br.has_value()
        || max_dpb.has_value()
        || level_asymmetry_allowed.has_value()
        || x_google_content_hint.has_value();
}

bool H265Fmtp::has_any() const noexcept {
    return profile_id.has_value()
        || level_id.has_value()
        || tier_flag.has_value()
        || tx_mode.has_value()
        || sprop_vps.has_value()
        || sprop_sps.has_value()
        || sprop_pps.has_value()
        || max_fr.has_value()
        || max_fs.has_value()
        || max_br.has_value();
}

bool Av1Fmtp::has_any() const noexcept {
    return profile.has_value()
        || level_idx.has_value()
        || tier.has_value()
        || bit_depth.has_value()
        || monochrome.has_value()
        || chroma_subsampling.has_value()
        || color_primaries.has_value()
        || transfer_characteristics.has_value()
        || matrix_coefficients.has_value();
}

// ---------------------------------------------------------------------------
// split_fmtp_pairs
// ---------------------------------------------------------------------------

std::vector<std::pair<std::string, std::string>>
split_fmtp_pairs(std::string_view body) noexcept {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t i = 0;
    while (i < body.size()) {
        // Find next semicolon or end.
        std::size_t sc = body.find(';', i);
        std::string_view pair = body.substr(i, sc == std::string_view::npos
                                              ? body.size() - i
                                              : sc - i);
        pair = strip(pair);
        if (!pair.empty()) {
            // Split on first '='.
            std::size_t eq = pair.find('=');
            if (eq == std::string_view::npos) {
                // Boolean attribute.
                out.emplace_back(url_decode(pair), std::string{});
            } else {
                std::string_view key = strip(pair.substr(0, eq));
                std::string_view val = strip(pair.substr(eq + 1));
                out.emplace_back(url_decode(key), url_decode(val));
            }
        }
        if (sc == std::string_view::npos) break;
        i = sc + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// parse_int_value
// ---------------------------------------------------------------------------

std::optional<std::int64_t>
parse_int_value(std::string_view value) noexcept {
    if (value.empty()) return std::nullopt;
    char buf[32];
    if (value.size() >= sizeof(buf)) return std::nullopt;
    std::memcpy(buf, value.data(), value.size());
    buf[value.size()] = '\0';

    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(buf, &end, 10);
    // Reject: no conversion, overflow, OR trailing garbage (we require
    // the entire string to be a base-10 integer, not "12abc").
    if (end == buf || errno != 0 || *end != '\0') return std::nullopt;
    return static_cast<std::int64_t>(v);
}

// ---------------------------------------------------------------------------
// codec_from_rtpmap_encoding
// ---------------------------------------------------------------------------

CodecKind codec_from_rtpmap_encoding(std::string_view encoding) noexcept {
    if (iequals(encoding, "H264")) return CodecKind::kH264;
    if (iequals(encoding, "H265")) return CodecKind::kH265;
    if (iequals(encoding, "VP8"))  return CodecKind::kVP8;
    if (iequals(encoding, "VP9"))  return CodecKind::kVP9;
    if (iequals(encoding, "AV1"))  return CodecKind::kAV1;
    return CodecKind::kUnknown;
}

// ---------------------------------------------------------------------------
// parse_h264
// ---------------------------------------------------------------------------

core::Result<H264Fmtp> parse_h264(std::string_view fmtp_raw) noexcept {
    H264Fmtp out;
    auto pairs = split_fmtp_pairs(fmtp_raw);

    for (const auto& [k, v] : pairs) {
        if (iequals(k, "packetization-mode")) {
            auto val = parse_int_value(v);
            if (!val) {
                return core::Result<H264Fmtp>::fail(
                    core::ErrorCode::InvalidArgument,
                    "h264 packetization-mode: not an integer");
            }
            if (*val < 0 || *val > 2) {
                return core::Result<H264Fmtp>::fail(
                    core::ErrorCode::InvalidArgument,
                    "h264 packetization-mode: out of range");
            }
            out.packetization_mode =
                static_cast<PacketizationMode>(*val);
        } else if (iequals(k, "profile-level-id")) {
            // Hex string, e.g. "42e01f" — 6 ASCII hex chars.
            if (v.size() != 6) {
                return core::Result<H264Fmtp>::fail(
                    core::ErrorCode::InvalidArgument,
                    "h264 profile-level-id: must be 6 hex chars");
            }
            for (char c : v) {
                if (!is_hex_char(c)) {
                    return core::Result<H264Fmtp>::fail(
                        core::ErrorCode::InvalidArgument,
                        "h264 profile-level-id: non-hex char");
                }
            }
            out.profile_level_id = std::string(v);
        } else if (iequals(k, "sprop-parameter-sets")) {
            // "Z0LAHtkA,aM4G4g==" — comma-separated base64 SPS,PPS.
            auto comma = v.find(',');
            if (comma == std::string::npos) {
                return core::Result<H264Fmtp>::fail(
                    core::ErrorCode::InvalidArgument,
                    "h264 sprop-parameter-sets: missing comma");
            }
            std::string sps(v.substr(0, comma));
            std::string pps(v.substr(comma + 1));
            for (char c : sps) {
                if (!is_base64_char(c) && c != '\0') {
                    return core::Result<H264Fmtp>::fail(
                        core::ErrorCode::InvalidArgument,
                        "h264 sprop-sps: non-base64 char");
                }
            }
            for (char c : pps) {
                if (!is_base64_char(c) && c != '\0') {
                    return core::Result<H264Fmtp>::fail(
                        core::ErrorCode::InvalidArgument,
                        "h264 sprop-pps: non-base64 char");
                }
            }
            out.sprop_sps = std::move(sps);
            out.sprop_pps = std::move(pps);
        } else if (iequals(k, "max-mbps")) {
            auto val = parse_int_value(v);
            if (!val || *val < 0) {
                return core::Result<H264Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "h264 max-mbps");
            }
            out.max_mbps = static_cast<std::uint32_t>(*val);
        } else if (iequals(k, "max-fs")) {
            auto val = parse_int_value(v);
            if (!val || *val < 0) {
                return core::Result<H264Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "h264 max-fs");
            }
            out.max_fs = static_cast<std::uint32_t>(*val);
        } else if (iequals(k, "max-smbps")) {
            auto val = parse_int_value(v);
            if (!val || *val < 0) {
                return core::Result<H264Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "h264 max-smbps");
            }
            out.max_smbps = static_cast<std::uint32_t>(*val);
        } else if (iequals(k, "max-br")) {
            auto val = parse_int_value(v);
            if (!val || *val < 0) {
                return core::Result<H264Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "h264 max-br");
            }
            out.max_br = static_cast<std::uint32_t>(*val);
        } else if (iequals(k, "max-dpb")) {
            auto val = parse_int_value(v);
            if (!val || *val < 0) {
                return core::Result<H264Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "h264 max-dpb");
            }
            out.max_dpb = static_cast<std::uint32_t>(*val);
        } else if (iequals(k, "level-asymmetry-allowed")) {
            // Boolean.
            if (v.empty() || iequals(v, "1") || iequals(v, "true")) {
                out.level_asymmetry_allowed = true;
            } else if (iequals(v, "0") || iequals(v, "false")) {
                out.level_asymmetry_allowed = false;
            } else {
                return core::Result<H264Fmtp>::fail(
                    core::ErrorCode::InvalidArgument,
                    "h264 level-asymmetry-allowed: bad value");
            }
        } else if (iequals(k, "x-google-content-hint")) {
            // The content hint (remote-desktop plan B7).  The three
            // values RFC 8692 §3 defines are the only ones that mean
            // anything; anything else is dropped rather than carried
            // around as a string nobody can act on.
            if (iequals(v, "motion") || iequals(v, "detail") || iequals(v, "text")) {
                out.x_google_content_hint = std::string(v);
            }
        }
        // Unknown keys are silently ignored — SDP has a 'permissive parse' culture.
    }
    return core::Result<H264Fmtp>::ok(std::move(out));
}

// ---------------------------------------------------------------------------
// parse_vp8
// ---------------------------------------------------------------------------

core::Result<Vp8Fmtp> parse_vp8(std::string_view fmtp_raw) noexcept {
    Vp8Fmtp out;
    auto pairs = split_fmtp_pairs(fmtp_raw);
    for (const auto& [k, v] : pairs) {
        if (iequals(k, "max-fr")) {
            auto val = parse_int_value(v);
            if (!val || *val < 0) {
                return core::Result<Vp8Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "vp8 max-fr");
            }
            out.max_fr = static_cast<std::uint32_t>(*val);
        } else if (iequals(k, "max-fs")) {
            auto val = parse_int_value(v);
            if (!val || *val < 0) {
                return core::Result<Vp8Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "vp8 max-fs");
            }
            out.max_fs = static_cast<std::uint32_t>(*val);
        }
    }
    return core::Result<Vp8Fmtp>::ok(std::move(out));
}

// ---------------------------------------------------------------------------
// parse_vp9
// ---------------------------------------------------------------------------

core::Result<Vp9Fmtp> parse_vp9(std::string_view fmtp_raw) noexcept {
    Vp9Fmtp out;
    auto pairs = split_fmtp_pairs(fmtp_raw);
    for (const auto& [k, v] : pairs) {
        if (iequals(k, "profile-id")) {
            auto val = parse_int_value(v);
            if (!val || *val < 0 || *val > 3) {
                return core::Result<Vp9Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "vp9 profile-id");
            }
            out.profile_id = static_cast<std::uint32_t>(*val);
        } else if (iequals(k, "profile-mapping")) {
            out.profile_mapping = std::string(v);
        } else if (iequals(k, "max-fr")) {
            auto val = parse_int_value(v);
            if (!val || *val < 0) {
                return core::Result<Vp9Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "vp9 max-fr");
            }
            out.max_fr = static_cast<std::uint32_t>(*val);
        } else if (iequals(k, "max-fs")) {
            auto val = parse_int_value(v);
            if (!val || *val < 0) {
                return core::Result<Vp9Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "vp9 max-fs");
            }
            out.max_fs = static_cast<std::uint32_t>(*val);
        } else if (iequals(k, "max-br")) {
            auto val = parse_int_value(v);
            if (!val || *val < 0) {
                return core::Result<Vp9Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "vp9 max-br");
            }
            out.max_br = static_cast<std::uint32_t>(*val);
        }
    }
    return core::Result<Vp9Fmtp>::ok(std::move(out));
}

// ---------------------------------------------------------------------------
// parse_h265
// ---------------------------------------------------------------------------

core::Result<H265Fmtp> parse_h265(std::string_view fmtp_raw) noexcept {
    H265Fmtp out;
    auto pairs = split_fmtp_pairs(fmtp_raw);
    for (const auto& [k, v] : pairs) {
        if (iequals(k, "profile-id")) {
            auto val = parse_int_value(v);
            if (!val || *val < 0 || *val > 3) {
                return core::Result<H265Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "h265 profile-id");
            }
            out.profile_id = static_cast<std::uint32_t>(*val);
        } else if (iequals(k, "level-id")) {
            auto val = parse_int_value(v);
            if (!val || *val < 0 || *val > 255) {
                return core::Result<H265Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "h265 level-id");
            }
            out.level_id = static_cast<std::uint32_t>(*val);
        } else if (iequals(k, "tier-flag")) {
            auto val = parse_int_value(v);
            if (!val || *val < 0 || *val > 1) {
                return core::Result<H265Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "h265 tier-flag");
            }
            out.tier_flag = static_cast<std::uint32_t>(*val);
        } else if (iequals(k, "tx-mode")) {
            // RFC 7798 §4.4: `tx-mode` is a *string* token, one of
            // "SRST" / "MRST" / "MRMT".  The comparison is
            // case-insensitive (the RFC's own examples use upper
            // case, but a peer may send lower case), and the token
            // is stored upper-cased so a parse → build round-trip is
            // stable.
            std::string token;
            for (char c : v) {
                if (c == ' ' || c == '\t') continue;
                token.push_back(
                    static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c));
            }
            if (token == "SRST" || token == "MRST" || token == "MRMT") {
                out.tx_mode = token;
            } else {
                return core::Result<H265Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "h265 tx-mode");
            }
        } else if (iequals(k, "sprop-vps") || iequals(k, "sprop-sps") ||
                   iequals(k, "sprop-pps")) {
            // RFC 7798 §4.4 defines three *separate* parameters:
            // `sprop-vps`, `sprop-sps` and `sprop-pps`.  (The H.264
            // convention of one combined `sprop-parameter-sets` is
            // handled below for legacy peers.)
            for (char c : v) {
                if (!is_base64_char(c)) {
                    return core::Result<H265Fmtp>::fail(
                        core::ErrorCode::InvalidArgument,
                        "h265 sprop-parameter: non-base64 char");
                }
            }
            if (iequals(k, "sprop-vps")) {
                out.sprop_vps = std::string(v);
            } else if (iequals(k, "sprop-sps")) {
                out.sprop_sps = std::string(v);
            } else {
                out.sprop_pps = std::string(v);
            }
        } else if (iequals(k, "sprop-parameter-sets")) {
            // "vps,sps,pps" — base64, comma separated.  Any subset is
            // accepted; the entries are kept in the order given.
            //
            // NOTE: `v` is a `std::string`, so `v.substr(...)` yields a
            // *temporary* `std::string`.  A `std::string_view` bound to it
            // would dangle at the end of the full expression — the
            // sub-view must therefore be taken from `v` directly.
            std::vector<std::string_view> parts;
            std::size_t start = 0;
            while (start <= v.size()) {
                const std::size_t comma = v.find(',', start);
                std::string_view part = (comma == std::string_view::npos)
                                            ? std::string_view(v).substr(start)
                                            : std::string_view(v).substr(start,
                                                                        comma - start);
                part = strip(part);
                if (!part.empty()) {
                    for (char c : part) {
                        if (!is_base64_char(c)) {
                            return core::Result<H265Fmtp>::fail(
                                core::ErrorCode::InvalidArgument,
                                "h265 sprop-parameter-sets: non-base64 char");
                        }
                    }
                    parts.push_back(part);
                }
                if (comma == std::string_view::npos) break;
                start = comma + 1;
            }
            if (parts.size() > 3) {
                return core::Result<H265Fmtp>::fail(
                    core::ErrorCode::InvalidArgument,
                    "h265 sprop-parameter-sets: at most three entries");
            }
            // The order is VPS, SPS, PPS — the H.265 analogue of the
            // H.264 "sps,pps" pair.
            if (parts.size() >= 1) out.sprop_vps = std::string(parts[0]);
            if (parts.size() >= 2) out.sprop_sps = std::string(parts[1]);
            if (parts.size() >= 3) out.sprop_pps = std::string(parts[2]);
        } else if (iequals(k, "max-fr")) {
            auto val = parse_int_value(v);
            if (!val || *val < 0) {
                return core::Result<H265Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "h265 max-fr");
            }
            out.max_fr = static_cast<std::uint32_t>(*val);
        } else if (iequals(k, "max-fs")) {
            auto val = parse_int_value(v);
            if (!val || *val < 0) {
                return core::Result<H265Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "h265 max-fs");
            }
            out.max_fs = static_cast<std::uint32_t>(*val);
        } else if (iequals(k, "max-br")) {
            auto val = parse_int_value(v);
            if (!val || *val < 0) {
                return core::Result<H265Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "h265 max-br");
            }
            out.max_br = static_cast<std::uint32_t>(*val);
        }
        // Unknown keys are silently ignored — SDP has a 'permissive parse'
        // culture.
    }
    return core::Result<H265Fmtp>::ok(std::move(out));
}

// ---------------------------------------------------------------------------
// parse_av1
// ---------------------------------------------------------------------------

core::Result<Av1Fmtp> parse_av1(std::string_view fmtp_raw) noexcept {
    Av1Fmtp out;
    auto pairs = split_fmtp_pairs(fmtp_raw);
    for (const auto& [k, v] : pairs) {
        // Copy the structured binding into a plain local before the lambda
        // captures it: capturing a structured binding by reference is a
        // C++20 feature (P1091) that Clang still rejects, so the copy
        // keeps one spelling compiling under both GCC and Clang.
        const std::string_view value = v;
        auto int_param = [&](std::optional<std::uint32_t>& dst,
                             std::int64_t lo,
                             std::int64_t hi) -> bool {
            auto val = parse_int_value(value);
            if (!val || *val < lo || *val > hi) return false;
            dst = static_cast<std::uint32_t>(*val);
            return true;
        };
        if (iequals(k, "profile")) {
            if (!int_param(out.profile, 0, 3)) {
                return core::Result<Av1Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "av1 profile");
            }
        } else if (iequals(k, "level-idx")) {
            if (!int_param(out.level_idx, 0, 31)) {
                return core::Result<Av1Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "av1 level-idx");
            }
        } else if (iequals(k, "tier")) {
            if (!int_param(out.tier, 0, 1)) {
                return core::Result<Av1Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "av1 tier");
            }
        } else if (iequals(k, "bit-depth")) {
            if (!int_param(out.bit_depth, 8, 12)) {
                return core::Result<Av1Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "av1 bit-depth");
            }
        } else if (iequals(k, "monochrome")) {
            if (!int_param(out.monochrome, 0, 1)) {
                return core::Result<Av1Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "av1 monochrome");
            }
        } else if (iequals(k, "chroma-subsampling")) {
            if (!int_param(out.chroma_subsampling, 0, 3)) {
                return core::Result<Av1Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "av1 chroma-subsampling");
            }
        } else if (iequals(k, "color-primaries")) {
            if (!int_param(out.color_primaries, 0, 255)) {
                return core::Result<Av1Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "av1 color-primaries");
            }
        } else if (iequals(k, "transfer-characteristics")) {
            if (!int_param(out.transfer_characteristics, 0, 255)) {
                return core::Result<Av1Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "av1 transfer-characteristics");
            }
        } else if (iequals(k, "matrix-coefficients")) {
            if (!int_param(out.matrix_coefficients, 0, 255)) {
                return core::Result<Av1Fmtp>::fail(
                    core::ErrorCode::InvalidArgument, "av1 matrix-coefficients");
            }
        }
        // Unknown keys are silently ignored — SDP has a 'permissive parse'
        // culture.
    }
    return core::Result<Av1Fmtp>::ok(std::move(out));
}

// ---------------------------------------------------------------------------
// parse_fmtp — dispatch
// ---------------------------------------------------------------------------

core::Result<ParsedFmtp> parse_fmtp(CodecKind kind,
                                    std::string_view fmtp_raw) noexcept {
    ParsedFmtp out;
    switch (kind) {
        case CodecKind::kH264: {
            auto r = parse_h264(fmtp_raw);
            if (!r) {
                return core::Result<ParsedFmtp>::fail(
                    r.error().code(), r.error().message());
            }
            out.set_h264(std::move(r.value()));
            return core::Result<ParsedFmtp>::ok(std::move(out));
        }
        case CodecKind::kH265: {
            auto r = parse_h265(fmtp_raw);
            if (!r) {
                return core::Result<ParsedFmtp>::fail(
                    r.error().code(), r.error().message());
            }
            out.set_h265(std::move(r.value()));
            return core::Result<ParsedFmtp>::ok(std::move(out));
        }
        case CodecKind::kVP8: {
            auto r = parse_vp8(fmtp_raw);
            if (!r) {
                return core::Result<ParsedFmtp>::fail(
                    r.error().code(), r.error().message());
            }
            out.set_vp8(std::move(r.value()));
            return core::Result<ParsedFmtp>::ok(std::move(out));
        }
        case CodecKind::kVP9: {
            auto r = parse_vp9(fmtp_raw);
            if (!r) {
                return core::Result<ParsedFmtp>::fail(
                    r.error().code(), r.error().message());
            }
            out.set_vp9(std::move(r.value()));
            return core::Result<ParsedFmtp>::ok(std::move(out));
        }
        case CodecKind::kAV1: {
            auto r = parse_av1(fmtp_raw);
            if (!r) {
                return core::Result<ParsedFmtp>::fail(
                    r.error().code(), r.error().message());
            }
            out.set_av1(std::move(r.value()));
            return core::Result<ParsedFmtp>::ok(std::move(out));
        }
        default:
            return core::Result<ParsedFmtp>::fail(
                core::ErrorCode::NotImplemented,
                "parse_fmtp: unsupported codec kind");
    }
}

// ---------------------------------------------------------------------------
// Build — convert typed config to a fmtp body string
// ---------------------------------------------------------------------------

namespace {

void append_kv(std::string& out, const char* k, std::string_view v) {
    if (!out.empty() && out.back() != ';') out.push_back(';');
    out.append(k);
    out.push_back('=');
    out.append(v.data(), v.size());
}

} // namespace

std::string build_h264_fmtp(const H264Fmtp& cfg) noexcept {
    std::string out;
    if (cfg.packetization_mode) {
        append_kv(out, "packetization-mode",
                  std::to_string(static_cast<int>(*cfg.packetization_mode)));
    }
    if (cfg.profile_level_id) {
        append_kv(out, "profile-level-id", *cfg.profile_level_id);
    }
    if (cfg.sprop_sps && cfg.sprop_pps) {
        std::string combined = *cfg.sprop_sps + "," + *cfg.sprop_pps;
        append_kv(out, "sprop-parameter-sets", combined);
    } else if (cfg.sprop_sps) {
        append_kv(out, "sprop-parameter-sets", *cfg.sprop_sps);
    }
    if (cfg.max_mbps)  append_kv(out, "max-mbps",  std::to_string(*cfg.max_mbps));
    if (cfg.max_fs)    append_kv(out, "max-fs",    std::to_string(*cfg.max_fs));
    if (cfg.max_smbps) append_kv(out, "max-smbps", std::to_string(*cfg.max_smbps));
    if (cfg.max_br)    append_kv(out, "max-br",    std::to_string(*cfg.max_br));
    if (cfg.max_dpb)   append_kv(out, "max-dpb",   std::to_string(*cfg.max_dpb));
    if (cfg.level_asymmetry_allowed) {
        append_kv(out, "level-asymmetry-allowed", "1");
    }
    if (cfg.x_google_content_hint && !cfg.x_google_content_hint->empty()) {
        append_kv(out, "x-google-content-hint", *cfg.x_google_content_hint);
    }
    return out;
}

std::string build_vp8_fmtp(const Vp8Fmtp& cfg) noexcept {
    std::string out;
    if (cfg.max_fr) append_kv(out, "max-fr", std::to_string(*cfg.max_fr));
    if (cfg.max_fs) append_kv(out, "max-fs", std::to_string(*cfg.max_fs));
    return out;
}

std::string build_vp9_fmtp(const Vp9Fmtp& cfg) noexcept {
    std::string out;
    if (cfg.profile_id) {
        append_kv(out, "profile-id", std::to_string(*cfg.profile_id));
    }
    if (cfg.profile_mapping) {
        append_kv(out, "profile-mapping", *cfg.profile_mapping);
    }
    if (cfg.max_fr) append_kv(out, "max-fr", std::to_string(*cfg.max_fr));
    if (cfg.max_fs) append_kv(out, "max-fs", std::to_string(*cfg.max_fs));
    if (cfg.max_br) append_kv(out, "max-br", std::to_string(*cfg.max_br));
    return out;
}

std::string build_h265_fmtp(const H265Fmtp& cfg) noexcept {
    std::string out;
    if (cfg.profile_id) {
        append_kv(out, "profile-id", std::to_string(*cfg.profile_id));
    }
    if (cfg.level_id) {
        append_kv(out, "level-id", std::to_string(*cfg.level_id));
    }
    if (cfg.tier_flag) {
        append_kv(out, "tier-flag", std::to_string(*cfg.tier_flag));
    }
    if (cfg.tx_mode) {
        // RFC 7798 §4.4 — a string token, not a number.
        append_kv(out, "tx-mode", *cfg.tx_mode);
    }
    // RFC 7798 §4.4 defines three separate parameters.  NimRTC does not
    // put the VPS/SPS/PPS in the SDP (the WebRTC HEVC profile sends the
    // parameter sets in-band), so these are only emitted when a host
    // explicitly populates them.
    if (cfg.sprop_vps) append_kv(out, "sprop-vps", *cfg.sprop_vps);
    if (cfg.sprop_sps) append_kv(out, "sprop-sps", *cfg.sprop_sps);
    if (cfg.sprop_pps) append_kv(out, "sprop-pps", *cfg.sprop_pps);
    if (cfg.max_fr) append_kv(out, "max-fr", std::to_string(*cfg.max_fr));
    if (cfg.max_fs) append_kv(out, "max-fs", std::to_string(*cfg.max_fs));
    if (cfg.max_br) append_kv(out, "max-br", std::to_string(*cfg.max_br));
    return out;
}

std::string build_av1_fmtp(const Av1Fmtp& cfg) noexcept {
    std::string out;
    if (cfg.profile)     append_kv(out, "profile", std::to_string(*cfg.profile));
    if (cfg.level_idx)   append_kv(out, "level-idx", std::to_string(*cfg.level_idx));
    if (cfg.tier)        append_kv(out, "tier", std::to_string(*cfg.tier));
    if (cfg.bit_depth)   append_kv(out, "bit-depth", std::to_string(*cfg.bit_depth));
    if (cfg.monochrome)  append_kv(out, "monochrome", std::to_string(*cfg.monochrome));
    if (cfg.chroma_subsampling) {
        append_kv(out, "chroma-subsampling", std::to_string(*cfg.chroma_subsampling));
    }
    if (cfg.color_primaries) {
        append_kv(out, "color-primaries", std::to_string(*cfg.color_primaries));
    }
    if (cfg.transfer_characteristics) {
        append_kv(out, "transfer-characteristics",
                  std::to_string(*cfg.transfer_characteristics));
    }
    if (cfg.matrix_coefficients) {
        append_kv(out, "matrix-coefficients",
                  std::to_string(*cfg.matrix_coefficients));
    }
    return out;
}

std::string build_fmtp(const ParsedFmtp& cfg) noexcept {
    switch (cfg.kind) {
        case CodecKind::kH264: return build_h264_fmtp(cfg.h264);
        case CodecKind::kH265: return build_h265_fmtp(cfg.h265);
        case CodecKind::kVP8:  return build_vp8_fmtp(cfg.vp8);
        case CodecKind::kVP9:  return build_vp9_fmtp(cfg.vp9);
        case CodecKind::kAV1:  return build_av1_fmtp(cfg.av1);
        default: return {};
    }
}

} // namespace nimrtc::video_sdp_attributes
