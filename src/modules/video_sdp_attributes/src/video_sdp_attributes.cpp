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
    switch (kind) {
        case CodecKind::kH264: h264.~H264Fmtp(); break;
        case CodecKind::kVP8:  vp8.~Vp8Fmtp();   break;
        case CodecKind::kVP9:  vp9.~Vp9Fmtp();   break;
        default: break;
    }
}

ParsedFmtp::ParsedFmtp(const ParsedFmtp& other) : kind(other.kind) {
    switch (kind) {
        case CodecKind::kH264: new (&h264) H264Fmtp(other.h264); break;
        case CodecKind::kVP8:  new (&vp8)  Vp8Fmtp(other.vp8);   break;
        case CodecKind::kVP9:  new (&vp9)  Vp9Fmtp(other.vp9);   break;
        default: new (&h264) H264Fmtp(); break;
    }
}

ParsedFmtp& ParsedFmtp::operator=(const ParsedFmtp& other) {
    if (this == &other) return *this;
    this->~ParsedFmtp();
    kind = other.kind;
    switch (kind) {
        case CodecKind::kH264: new (&h264) H264Fmtp(other.h264); break;
        case CodecKind::kVP8:  new (&vp8)  Vp8Fmtp(other.vp8);   break;
        case CodecKind::kVP9:  new (&vp9)  Vp9Fmtp(other.vp9);   break;
        default: new (&h264) H264Fmtp(); break;
    }
    return *this;
}

ParsedFmtp::ParsedFmtp(ParsedFmtp&& other) noexcept : kind(other.kind) {
    switch (kind) {
        case CodecKind::kH264: new (&h264) H264Fmtp(std::move(other.h264)); break;
        case CodecKind::kVP8:  new (&vp8)  Vp8Fmtp(std::move(other.vp8));   break;
        case CodecKind::kVP9:  new (&vp9)  Vp9Fmtp(std::move(other.vp9));   break;
        default: new (&h264) H264Fmtp(); break;
    }
}

ParsedFmtp& ParsedFmtp::operator=(ParsedFmtp&& other) noexcept {
    if (this == &other) return *this;
    this->~ParsedFmtp();
    kind = other.kind;
    switch (kind) {
        case CodecKind::kH264: new (&h264) H264Fmtp(std::move(other.h264)); break;
        case CodecKind::kVP8:  new (&vp8)  Vp8Fmtp(std::move(other.vp8));   break;
        case CodecKind::kVP9:  new (&vp9)  Vp9Fmtp(std::move(other.vp9));   break;
        default: new (&h264) H264Fmtp(); break;
    }
    return *this;
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
        || level_asymmetry_allowed.has_value();
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
// parse_fmtp — dispatch
// ---------------------------------------------------------------------------

core::Result<ParsedFmtp> parse_fmtp(CodecKind kind,
                                    std::string_view fmtp_raw) noexcept {
    ParsedFmtp out;
    out.kind = kind;
    switch (kind) {
        case CodecKind::kH264: {
            auto r = parse_h264(fmtp_raw);
            if (!r) {
                return core::Result<ParsedFmtp>::fail(
                    r.error().code(), r.error().message());
            }
            out.h264 = std::move(r.value());
            return core::Result<ParsedFmtp>::ok(std::move(out));
        }
        case CodecKind::kVP8: {
            auto r = parse_vp8(fmtp_raw);
            if (!r) {
                return core::Result<ParsedFmtp>::fail(
                    r.error().code(), r.error().message());
            }
            out.vp8 = std::move(r.value());
            return core::Result<ParsedFmtp>::ok(std::move(out));
        }
        case CodecKind::kVP9: {
            auto r = parse_vp9(fmtp_raw);
            if (!r) {
                return core::Result<ParsedFmtp>::fail(
                    r.error().code(), r.error().message());
            }
            out.vp9 = std::move(r.value());
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

std::string build_fmtp(const ParsedFmtp& cfg) noexcept {
    switch (cfg.kind) {
        case CodecKind::kH264: return build_h264_fmtp(cfg.h264);
        case CodecKind::kVP8:  return build_vp8_fmtp(cfg.vp8);
        case CodecKind::kVP9:  return build_vp9_fmtp(cfg.vp9);
        default: return {};
    }
}

} // namespace nimrtc::video_sdp_attributes
