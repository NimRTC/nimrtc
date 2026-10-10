/**
 * @file nimrtc/sdp/src/munger.cpp
 * @brief SDP Munger — emit SDP text from SessionDescription.
 *
 * RFC 8829 §5 mandates a specific attribute ordering for WebRTC compatibility.
 * The order within each media block is:
 *   ice-ufrag / ice-pwd / ice-lite / ice-options
 *   fingerprint
 *   setup (actpass/active/passive)
 *   connection-address (c=)
 *   rtcp-mux / rtcp-mux-require
 *   mid
 *   rtpmap (per fmt)
 *   fmtp (per fmt)
 *   rtcp-fb (per fmt)
 *   extmap (ascending id)
 *   candidate (ICE candidates)
 *   ssrc / ssrc-group (SSRC lines)
 *   direction (sendrecv/sendonly/recvonly/inactive)
 *
 * @note P1 — RFC 4566 + RFC 8829.
 */

#include <nimrtc/sdp/session_description.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>   // snprintf
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/bytes.hpp>

namespace nimrtc::sdp {

namespace {

// Small output buffer helpers -----------------------------------------------

/** Append formatted integer to string. */
inline void append_int(std::string& out, std::uint64_t v) {
    char buf[24];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, static_cast<std::size_t>(ptr - buf));
}

/** Append a single "a=<key><value>\r\n" line. The caller provides the
 *  full attribute name including "a=" prefix and any trailing ":" that
 *  separates it from the value. Examples:
 *      attr(out, "a=ice-ufrag:", ufrag);   // -> "a=ice-ufrag:<ufrag>\r\n"
 *      attr(out, "a=rtcp-mux",   "");     // -> "a=rtcp-mux\r\n"   (flag)
 *      attr(out, "a=sendonly",   "");     // -> "a=sendonly\r\n"   (flag)
 */
inline void attr(std::string& out, const char* key, std::string_view value) {
    out.append(key);
    out.append(value.data(), value.size());
    out += "\r\n";
}

/** Append "a=key\r\n" (no value). */
inline void flag(std::string& out, const char* key) {
    out += 'a';
    out += '=';
    out += key;
    out += "\r\n";
}

// Key/value helper kept for future attribute emission (k= lines). Currently
// unused because all media attributes are written via the dedicated helpers
// below; suppress -Wunused-function on toolchains that warn here.
#if defined(__clang__)
[[maybe_unused]]
#endif
inline void key_val(std::string& out, char key, std::string_view value) {
    out += key;
    out += '=';
    out.append(value.data(), value.size());
    out += "\r\n";
}

// Media-level attribute emission in RFC 8829 §5 order ------------------------

void emit_media_attrs(std::string& out, const MediaDescription& m) {
    // ICE credentials first
    if (!m.ice_ufrag.empty())
        attr(out, "a=ice-ufrag:", m.ice_ufrag);
    if (!m.ice_pwd.empty())
        attr(out, "a=ice-pwd:", m.ice_pwd);
    if (!m.ice_options.empty())
        attr(out, "a=ice-options:", m.ice_options);

    // DTLS
    if (!m.dtls_fingerprint_algo.empty() && !m.dtls_fingerprint_value.empty()) {
        out += "a=fingerprint:";
        out += m.dtls_fingerprint_algo;
        out += ' ';
        out += m.dtls_fingerprint_value;
        out += "\r\n";
    }
    if (!m.dtls_setup.empty()) {
        attr(out, "a=setup:", m.dtls_setup);
    }

    // Connection (c=) — RFC 8829 §5: required in media level even if session-level present
    if (!m.connection_address.empty()) {
        out += "c=IN IP";
        out += (m.address_family == 6) ? '6' : '4';
        out += ' ';
        out += m.connection_address;
        if (m.ttl > 0) {
            out += '/';
            append_int(out, m.ttl);
        }
        out += "\r\n";
    }

    // rtcp-mux
    if (!m.rtcp_mux_value.empty())
        flag(out, "rtcp-mux");

    // mid (BUNDLE identifier)
    if (!m.mid.empty())
        attr(out, "a=mid:", m.mid);

    // Direction
    if (m.direction != Direction::SendRecv)
        attr(out, "a=", to_string(m.direction));

    // msid (RFC 8830)
    if (m.msid.has_value()) {
        out += "a=msid:";
        out += m.msid->stream_id;
        if (!m.msid->track_id.empty()) {
            out += ' ';
            out += m.msid->track_id;
        }
        out += "\r\n";
    }

    // rtpmap (one per format)
    for (auto& [fmt, map] : m.rtpmap) {
        out += "a=rtpmap:";
        out += fmt;
        out += ' ';
        out += map.encoding;
        out += '/';
        append_int(out, map.clock_rate);
        if (map.channels != 1) {
            out += '/';
            append_int(out, map.channels);
        }
        out += "\r\n";
    }

    // fmtp (per format)
    for (auto& [fmt, params] : m.fmtp) {
        out += "a=fmtp:";
        out += fmt;
        out += ' ';
        out += params;
        out += "\r\n";
    }

    // rtcp-fb (per format)
    for (auto& fb : m.rtcp_fb) {
        out += "a=rtcp-fb:";
        out += fb;
        out += "\r\n";
    }

    // extmap (ascending id)
    auto sorted_ext = m.extmap;
    std::sort(sorted_ext.begin(), sorted_ext.end(),
              [](const MediaDescription::ExtMap& a, const MediaDescription::ExtMap& b) {
                  return a.id < b.id;
              });
    for (auto& e : sorted_ext) {
        out += "a=extmap:";
        append_int(out, e.id);
        if (e.direction != Direction::SendRecv) {
            out += '/';
            out += to_string(e.direction);
        }
        out += ' ';
        out += e.uri;
        for (auto& [k, v] : e.params) {
            out += ' ';
            out += k;
            if (!v.empty()) {
                out += '=';
                out += v;
            }
        }
        out += "\r\n";
    }

    // ICE candidates
    for (auto& cand : m.candidates) {
        out += "a=candidate:";
        out += cand;
        out += "\r\n";
    }

    // Generic extra attrs (in original order)
    for (auto& [k, v] : m.extra_attrs) {
        out += 'a';
        out += '=';
        out += k;
        if (!v.empty()) {
            out += ':';
            out += v;
        }
        out += "\r\n";
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Munger
// ---------------------------------------------------------------------------

// Impl is currently unused (the munger is fully stateless) but the member
// is kept in the header to allow future state (e.g. delta-tracking). The
// type must be defined out-of-line so that unique_ptr<Impl>::~unique_ptr()
// can be instantiated where Impl is complete — see ~Munger() below.
struct Munger::Impl {};

// Out-of-line ctor and dtor: see the rationale in session_description.hpp.
// Both must be defined where Impl is complete; declaring them in the header
// as `= default` still forces implicit instantiation in every TU that uses
// Munger as a value.
Munger::Munger() = default;
Munger::~Munger() = default;

core::Result<std::string> Munger::to_sdp(const SessionDescription& sdp) const {
    std::string out;
    out.reserve(1024); // heuristic

    // v=
    out += "v=";
    append_int(out, sdp.version);
    out += "\r\n";

    // o=  (RFC 4566 §5.2: one origin line required)
    out += "o=";
    out += sdp.origin_username.empty() ? "-" : sdp.origin_username;
    out += ' ';
    out += sdp.origin_session_id.empty() ? "0" : sdp.origin_session_id;
    out += ' ';
    out += sdp.origin_session_version.empty() ? "0" : sdp.origin_session_version;
    out += " IN IP4 ";
    out += sdp.origin_address.empty() ? "0.0.0.0" : sdp.origin_address;
    out += "\r\n";

    // s=
    out += "s=";
    out += sdp.session_name.empty() ? "-" : sdp.session_name;
    out += "\r\n";

    // i= (session info, optional)
    if (!sdp.session_info.empty()) {
        out += "i=";
        out += sdp.session_info;
        out += "\r\n";
    }

    // u= (uri, optional)
    if (!sdp.uri.empty()) {
        out += "u=";
        out += sdp.uri;
        out += "\r\n";
    }

    // c= (session-level connection — RFC 8829 says media-level is sufficient,
    // but some endpoints expect session-level too)
    if (!sdp.session_connection_address.empty()) {
        out += "c=IN IP";
        out += (sdp.session_address_family == 6) ? '6' : '4';
        out += ' ';
        out += sdp.session_connection_address;
        out += "\r\n";
    }

    // b= (bandwidth — not written by munger, caller uses session/extra_attrs)

    // t= (timing — required by RFC 4566)
    out += "t=0 0\r\n";

    // a=group:BUNDLE (if any mids)
    if (!sdp.bundle_mids.empty()) {
        out += "a=group:BUNDLE";
        for (auto& mid : sdp.bundle_mids) {
            out += ' ';
            out += mid;
        }
        out += "\r\n";
    }

    // a=ice-options
    if (!sdp.ice_options.empty()) {
        attr(out, "a=ice-options:", sdp.ice_options);
    }

    // a=msid-semantic
    if (!sdp.msid_semantic_token.empty()) {
        out += "a=msid-semantic: ";
        out += sdp.msid_semantic_token;
        for (auto& s : sdp.msid_semantic_streams) {
            out += ' ';
            out += s;
        }
        out += "\r\n";
    }

    // Session-level DTLS fingerprint
    if (!sdp.dtls_fingerprint_algo.empty() && !sdp.dtls_fingerprint_value.empty()) {
        out += "a=fingerprint:";
        out += sdp.dtls_fingerprint_algo;
        out += ' ';
        out += sdp.dtls_fingerprint_value;
        out += "\r\n";
    }
    if (!sdp.dtls_setup.empty()) {
        attr(out, "a=setup:", sdp.dtls_setup);
    }

    // Session-level extra attrs
    for (auto& [k, v] : sdp.extra_attrs) {
        out += 'a';
        out += '=';
        out += k;
        if (!v.empty()) {
            out += ':';
            out += v;
        }
        out += "\r\n";
    }

    // Media descriptions
    for (auto& m : sdp.media) {
        // m=
        out += "m=";
        switch (m.type) {
            case MediaType::Audio:    out += "audio"; break;
            case MediaType::Video:    out += "video"; break;
            case MediaType::Application: out += "application"; break;
            case MediaType::Data:    out += "data"; break;
            default:                out += "text"; break;
        }
        out += ' ';
        append_int(out, m.port);
        out += ' ';
        out += m.protocol.empty() ? "UDP/TLS/RTP/SAVPF" : m.protocol;
        for (auto& fmt : m.formats) {
            out += ' ';
            out += fmt;
        }
        out += "\r\n";

        // Media-level attributes in RFC 8829 §5 order
        emit_media_attrs(out, m);
    }

    return core::Result<std::string>::ok(std::move(out));
}

// ---------------------------------------------------------------------------
// Enum helpers — declared in the header, defined here so the inline body
// stays in a single TU and we keep ODR-safety across modules.
// ---------------------------------------------------------------------------

const char* to_string(MediaType t) noexcept {
    switch (t) {
        case MediaType::Audio:       return "audio";
        case MediaType::Video:       return "video";
        case MediaType::Application: return "application";
        case MediaType::Data:        return "data";
        case MediaType::Other:       return "text";   // RFC 4566 fallback bucket
    }
    return "text";
}

std::optional<MediaType> parse_media_type(std::string_view s) noexcept {
    if (s == "audio")       return MediaType::Audio;
    if (s == "video")       return MediaType::Video;
    if (s == "application") return MediaType::Application;
    if (s == "data")        return MediaType::Data;
    return std::nullopt;          // unknown / "text" / typos → caller decides
}

const char* to_string(Direction d) noexcept {
    switch (d) {
        case Direction::SendRecv: return "sendrecv";
        case Direction::SendOnly: return "sendonly";
        case Direction::RecvOnly: return "recvonly";
        case Direction::Inactive: return "inactive";
    }
    return "sendrecv";
}

std::optional<Direction> parse_direction(std::string_view s) noexcept {
    if (s == "sendrecv") return Direction::SendRecv;
    if (s == "sendonly") return Direction::SendOnly;
    if (s == "recvonly") return Direction::RecvOnly;
    if (s == "inactive") return Direction::Inactive;
    return std::nullopt;
}

} // namespace nimrtc::sdp
