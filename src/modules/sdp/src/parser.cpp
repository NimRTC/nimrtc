/**
 * @file nimrtc/sdp/src/parser.cpp
 * @brief SDP Parser implementation (RFC 4566 + WebRTC extensions).
 *
 * Parses SDP text into a SessionDescription. Line-oriented, ASCII-only.
 * Well-formed lines populate the session; malformed lines produce a
 * ProtocolError and the first few bytes of the offending line for context.
 *
 * @note P1 — full RFC 4566 + WebRTC extensions.
 */

#include <nimrtc/sdp/session_description.hpp>

#include <algorithm>
#include <cctype>   // isdigit, isspace
#include <charconv> // from_chars
#include <cstring> // strchr
#include <optional>
#include <string_view>
#include <utility>

#include <nimrtc/core/error.hpp>

namespace nimrtc::sdp {

namespace {

// Helpers ----------------------------------------------------------------

[[maybe_unused]]
constexpr std::string_view kProtocol = "UDP/TLS/RTP/SAVPF";

[[nodiscard]] inline bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] inline bool is_space(char c) noexcept {
    return c == ' ' || c == '\t';
}

[[nodiscard]] inline bool is_eol(char c) noexcept {
    return c == '\r' || c == '\n';
}

/** Split a line "key=value" at the first '='. */
[[nodiscard]] inline std::pair<std::string_view, std::string_view>
split_at(std::string_view line, char delim) noexcept {
    auto pos = line.find(delim);
    if (pos == std::string_view::npos) return {line, {}};
    return {line.substr(0, pos), line.substr(pos + 1)};
}

/** Trim leading and trailing whitespace from a string_view. */
[[nodiscard]] inline std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

/** Parse a uint16_t from a string_view, or return nullopt. */
[[nodiscard]] std::optional<std::uint16_t>
parse_uint16(std::string_view s) noexcept {
    if (s.empty()) return std::nullopt;
    std::uint16_t result = 0;
    for (char c : s) {
        if (!is_digit(c)) return std::nullopt;
        result = static_cast<std::uint16_t>(result * 10 + (c - '0'));
    }
    return result;
}

/** Parse a uint32_t from a string_view, or return nullopt. */
[[nodiscard]] std::optional<std::uint32_t>
parse_uint32(std::string_view s) noexcept {
    if (s.empty()) return std::nullopt;
    std::uint64_t result = 0;
    for (char c : s) {
        if (!is_digit(c)) return std::nullopt;
        result = result * 10 + static_cast<std::uint64_t>(c - '0');
        if (result > 0xFFFFFFFFULL) return std::nullopt;
    }
    return static_cast<std::uint32_t>(result);
}

/** Consume a quoted or unquoted token (stops at space or end). */
[[nodiscard]] std::string_view next_token(std::string_view& s) noexcept {
    s = trim(s);
    if (s.empty()) return {};
    auto pos = s.find(' ');
    auto token = s.substr(0, pos);
    s = (pos == std::string_view::npos) ? std::string_view{} : s.substr(pos + 1);
    return token;
}

// -----------------------------------------------------------------------
// Per-line parsers (each returns void on success, error string on failure)
// -----------------------------------------------------------------------

void parse_origin(std::string_view value,
                  SessionDescription& out,
                  std::string& err) {
    (void)err; // reserved for future error reporting
    // o=<username> <sess-id> <sess-version> <nettype> <addrtype> <unicast-address>
    auto sv = value;
    out.origin_username = std::string(next_token(sv));
    out.origin_session_id = std::string(next_token(sv));
    out.origin_session_version = std::string(next_token(sv));
    (void)next_token(sv); // nettype (IN)
    (void)next_token(sv); // addrtype (IP4/IP6)
    out.origin_address = std::string(trim(sv));
}

void parse_connection(std::string_view value,
                      std::string& conn_addr,
                      int& ttl,
                      int& family,
                      std::string& err) {
    (void)err; // reserved for future error reporting
    // c=IN IP4 <addr>[/<ttl>]
    auto sv = value;
    auto nettype = next_token(sv); (void)nettype;
    auto addrtype = next_token(sv);
    family = (addrtype == "IP6") ? 6 : 4;
    auto addr = trim(sv);
    // Remove TTL suffix if present
    if (auto slash = addr.find('/'); slash != std::string_view::npos) {
        auto ttl_str = addr.substr(slash + 1);
        if (auto t = parse_uint16(ttl_str)) ttl = static_cast<int>(*t);
        addr = addr.substr(0, slash);
    } else {
        ttl = 0;
    }
    conn_addr = std::string(addr);
}

void parse_bandwidth(std::string_view value,
                    std::string& modifier,
                    std::uint64_t& value_out) {
    // b=<modifier>:<value>
    auto sv = value;
    modifier = std::string(next_token(sv));
    auto val_str = trim(sv);
    value_out = 0;
    for (char c : val_str) {
        if (is_digit(c)) {
            value_out = value_out * 10 + static_cast<std::uint64_t>(c - '0');
        }
    }
}

void parse_media_desc(std::string_view value,
                      MediaDescription& out,
                      std::string& err) {
    (void)err; // reserved for future error reporting
    // m=<media> <port> <proto> <fmt> ...
    auto sv = value;
    auto media = next_token(sv);
    if (media == "audio") out.type = MediaType::Audio;
    else if (media == "video") out.type = MediaType::Video;
    else if (media == "application") out.type = MediaType::Application;
    else if (media == "data") out.type = MediaType::Data;
    else out.type = MediaType::Other;

    if (auto port = parse_uint16(next_token(sv))) out.port = *port;
    out.protocol = std::string(next_token(sv));

    // Remaining tokens are formats (space-separated; some specs allow ranges
    // like "96-127", which we keep verbatim as strings).
    while (true) {
        sv = trim(sv);
        if (sv.empty()) break;
        auto fmt = next_token(sv);
        if (fmt.empty()) break;
        out.formats.emplace_back(fmt);
    }
}

void parse_rtpmap(std::string_view value,
                  std::string& fmt,
                  MediaDescription::RtpMap& map_out,
                  std::string& err) {
    (void)err; // reserved for future error reporting
    // a=rtpmap:<fmt> <encoding>/<clock>[/<channels>]
    auto sv = value;
    fmt = std::string(next_token(sv));
    auto encoding_str = next_token(sv);
    auto slash = encoding_str.find('/');
    map_out.encoding = std::string(
        slash == std::string_view::npos ? encoding_str : encoding_str.substr(0, slash));
    auto remainder = slash == std::string_view::npos
                     ? std::string_view{}
                     : encoding_str.substr(slash + 1);
    auto slash2 = remainder.find('/');
    auto clock_str = slash2 == std::string_view::npos ? remainder : remainder.substr(0, slash2);
    if (auto r = parse_uint32(clock_str)) map_out.clock_rate = *r;
    if (slash2 != std::string_view::npos) {
        auto chan_str = remainder.substr(slash2 + 1);
        if (auto r = parse_uint32(chan_str)) map_out.channels = *r;
    }
}

void parse_extmap(std::string_view value,
                  MediaDescription::ExtMap& out,
                  std::string& err) {
    (void)err; // reserved for future error reporting
    // a=extmap:<id>[/<direction>] <URI> [<params>]
    //
    // The id field is *optionally* followed by '/' and a direction token
    // (sendrecv/sendonly/recvonly/inactive). RFC 5285 §6 / RFC 8285.
    auto sv = value;
    auto id_str = next_token(sv);

    // Look for "/<direction>" suffix on the id token.
    auto slash = id_str.find('/');
    std::string_view dir_part;
    if (slash != std::string_view::npos) {
        dir_part = id_str.substr(slash + 1);
        id_str   = id_str.substr(0, slash);
    }

    if (auto id_val = parse_uint16(id_str)) {
        out.id = static_cast<int>(*id_val);
    }

    if (!dir_part.empty()) {
        if (auto d = parse_direction(dir_part)) {
            out.direction = *d;
        }
    }

    out.uri = std::string(next_token(sv));
    sv = trim(sv);
    while (!sv.empty()) {
        auto param_token = next_token(sv);
        auto eq = param_token.find('=');
        if (eq != std::string_view::npos) {
            auto key = std::string(param_token.substr(0, eq));
            auto val = std::string(param_token.substr(eq + 1));
            out.params.emplace(std::move(key), std::move(val));
        } else if (!param_token.empty()) {
            out.params.emplace(std::string(param_token), std::string{});
        }
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

struct Parser::Impl {
    std::string_view remaining;
    std::string error_msg;
    std::size_t line_number = 0;

    explicit Impl(std::string_view text) : remaining(text) {}

    /** Parse the next line (stopping at \r, \n, or end). Advances remaining. */
    [[nodiscard]] std::pair<std::string_view, std::string_view> next_line() noexcept {
        if (remaining.empty()) return {{}, {}};
        std::size_t end = 0;
        while (end < remaining.size() && !is_eol(remaining[end])) ++end;
        // Skip \r\n or \n or \r
        std::size_t consumed = end;
        if (consumed < remaining.size() && remaining[consumed] == '\r') ++consumed;
        if (consumed < remaining.size() && remaining[consumed] == '\n') ++consumed;
        auto line = remaining.substr(0, end);
        remaining = remaining.substr(consumed);
        ++line_number;
        return {line, {}};
    }

    [[nodiscard]] bool is_session_line(std::string_view line) noexcept {
        return !line.empty() && line[0] >= 'a' && line[0] <= 'z';
    }
};

// NOTE: Parser::Impl is constructed on the stack inside parse(); the
// class-level impl_ member declared in the header is currently unused.
// Kept around for future state (e.g. preset extension maps), but the
// ctor and dtor must still be defined out-of-line so the pImpl member
// doesn't force implicit-dtor instantiation in TUs that only see the
// header.
Parser::Parser() = default;
Parser::~Parser() = default;

core::Result<SessionDescription> Parser::parse(std::string_view sdp_text) const {
    Impl impl(sdp_text);

    SessionDescription session;
    std::optional<MediaDescription> cur_media; // in-progress media block

    while (!impl.remaining.empty()) {
        auto [line, _2] = impl.next_line(); (void)_2;
        if (line.empty()) continue;

        // Strip leading char (e.g., 'v=', 'a=')
        if (line.size() < 2 || line[1] != '=') {
            impl.error_msg = "malformed line (expected <letter>=...): " + std::string(line);
            return core::Result<SessionDescription>::fail(
                core::ErrorCode::ProtocolError, impl.error_msg);
        }
        char type = line[0];
        auto value = line.substr(2);

        switch (type) {
        case 'v': {
            // v=0
            if (auto v = parse_uint16(trim(value))) {
                session.version = static_cast<int>(*v);
            }
            break;
        }
        case 'o': {
            parse_origin(value, session, impl.error_msg);
            break;
        }
        case 's': {
            session.session_name = std::string(trim(value));
            break;
        }
        case 'i': {
            if (!cur_media) session.session_info = std::string(trim(value));
            else cur_media->extra_attrs.emplace_back("i", std::string(trim(value)));
            break;
        }
        case 'u': {
            session.uri = std::string(trim(value));
            break;
        }
        case 'c': {
            std::string addr; int ttl = 0; int fam = 4;
            parse_connection(value, addr, ttl, fam, impl.error_msg);
            if (!cur_media) {
                session.session_connection_address = addr;
                session.session_connection_ttl = ttl;
                session.session_address_family = fam;
            } else {
                cur_media->connection_address = addr;
                cur_media->ttl = ttl;
                cur_media->address_family = fam;
            }
            break;
        }
        case 'b': {
            std::string modifier; std::uint64_t bw = 0;
            parse_bandwidth(value, modifier, bw);
            (void)modifier; (void)bw;
            // TODO: store in session or media
            break;
        }
        case 't': {
            // t=<start-time> <stop-time> — stored in extra_attrs for now
            if (!cur_media) session.extra_attrs.emplace_back("t", std::string(trim(value)));
            break;
        }
        case 'r': {
            if (!cur_media) session.extra_attrs.emplace_back("r", std::string(trim(value)));
            break;
        }
        case 'z': {
            if (!cur_media) session.extra_attrs.emplace_back("z", std::string(trim(value)));
            break;
        }
        case 'k': {
            if (!cur_media) session.extra_attrs.emplace_back("k", std::string(trim(value)));
            break;
        }
        case 'a': {
            // Many sub-types
            auto av = trim(value);
            if (av == "rtcp-mux") {
                if (!cur_media) {
                    // session-level rtcp-mux (rare but valid)
                    session.extra_attrs.emplace_back("a", "rtcp-mux");
                } else {
                    cur_media->rtcp_mux_value = "rtcp-mux";
                }
            } else if (av == "sendonly" || av == "recvonly" ||
                       av == "sendrecv" || av == "inactive") {
                // RFC 4566 §6.4 — media-level direction attribute.
                // Session-level direction (used by e.g. SIP pre-offer) is
                // accepted via extra_attrs for round-trip safety.
                if (auto d = parse_direction(av)) {
                    if (cur_media) cur_media->direction = *d;
                    else            session.extra_attrs.emplace_back("a", std::string(av));
                }
            } else if (av.starts_with("ice-ufrag:")) {
                auto ufrag = av.substr(10);  // skip "ice-ufrag:" (10 chars)
                if (!cur_media) session.extra_attrs.emplace_back("ice-ufrag", std::string(ufrag));
                else cur_media->ice_ufrag = std::string(ufrag);
            } else if (av.starts_with("ice-pwd:")) {
                auto pwd = av.substr(8);
                if (!cur_media) session.extra_attrs.emplace_back("ice-pwd", std::string(pwd));
                else cur_media->ice_pwd = std::string(pwd);
            } else if (av.starts_with("ice-options:")) {
                auto opts = av.substr(13);  // skip "ice-options:" (13 chars)
                if (!cur_media) session.ice_options = std::string(opts);
                else cur_media->ice_options = std::string(opts);
            } else if (av.starts_with("fingerprint:")) {
                // a=fingerprint:<algo> <hash> — note no space after the colon;
                // the colon is the 12th char and "sha-256" starts right at index 12.
                auto fp = av.substr(12);
                auto [algo, hash] = split_at(fp, ' ');
                if (!cur_media) {
                    session.dtls_fingerprint_algo = std::string(algo);
                    session.dtls_fingerprint_value = std::string(hash);
                } else {
                    cur_media->dtls_fingerprint_algo = std::string(algo);
                    cur_media->dtls_fingerprint_value = std::string(hash);
                }
            } else if (av.starts_with("setup:")) {
                auto setup = av.substr(6);  // skip "setup:" (6 chars)
                if (!cur_media) session.dtls_setup = std::string(setup);
                else cur_media->dtls_setup = std::string(setup);
            } else if (av.starts_with("connection-address:")) {
                if (cur_media) cur_media->connection_address = std::string(av.substr(17));
            } else if (av.starts_with("mid:")) {
                if (cur_media) cur_media->mid = std::string(av.substr(4));
            } else if (av.starts_with("rtpmap:")) {
                if (cur_media) {
                    std::string fmt_str;
                    MediaDescription::RtpMap map;
                    parse_rtpmap(av.substr(7), fmt_str, map, impl.error_msg);
                    cur_media->rtpmap.emplace(fmt_str, map);
                }
            } else if (av.starts_with("fmtp:")) {
                if (cur_media) {
                    auto fp = av.substr(5);
                    auto [fmt_key, params] = split_at(fp, ' ');
                    cur_media->fmtp.emplace(std::string(fmt_key), std::string(params));
                }
            } else if (av.starts_with("rtcp-fb:")) {
                if (cur_media) {
                    cur_media->rtcp_fb.push_back(std::string(av.substr(8)));
                }
            } else if (av.starts_with("extmap:")) {
                if (cur_media) {
                    MediaDescription::ExtMap em;
                    parse_extmap(av.substr(7), em, impl.error_msg);
                    cur_media->extmap.push_back(em);
                }
            } else if (av.starts_with("candidate:")) {
                if (cur_media) {
                    cur_media->candidates.push_back(std::string(av.substr(10)));
                }
            } else if (av.starts_with("group:")) {
                // a=group:BUNDLE <mid1> <mid2> ...
                auto gv = av.substr(6);
                if (gv.starts_with("BUNDLE")) {
                    auto mids = gv.substr(6);  // skip "BUNDLE"
                    while (!mids.empty()) {
                        auto token = next_token(mids);  // advances mids
                        if (token.empty()) break;
                        session.bundle_mids.emplace_back(std::string(token));
                    }
                }
            } else if (av.starts_with("msid-semantic:")) {
                auto ms = av.substr(15);  // skip "msid-semantic: " (14+1 = 15 chars)
                auto [token, streams] = split_at(ms, ' ');
                session.msid_semantic_token = std::string(trim(token));
                while (!trim(streams).empty()) {
                    session.msid_semantic_streams.emplace_back(std::string(trim(streams)));
                    streams = {};
                    (void)next_token(streams);
                }
            } else if (av.starts_with("msid:")) {
                if (cur_media) {
                    auto msid_str = av.substr(5);
                    auto [stream_id, track_id] = split_at(msid_str, ' ');
                    Msid ms;
                    ms.stream_id = std::string(trim(stream_id));
                    ms.track_id  = std::string(trim(track_id));
                    cur_media->msid = std::move(ms);
                    // Note: positional session.msids[] is filled at media
                    // finalisation time (when we hit the next 'm=' line).
                }
            } else {
                // Generic: store as extra attr
                auto eq = av.find(' ');
                if (eq == std::string_view::npos) eq = av.size();
                auto key = av.substr(0, eq);
                auto val = trim(av.substr(eq));
                if (!cur_media) session.extra_attrs.emplace_back(std::string(key), std::string(val));
                else cur_media->extra_attrs.emplace_back(std::string(key), std::string(val));
            }
            break;
        }
        case 'm': {
            // Finalize previous media block
            if (cur_media) {
                // msids[] mirrors the media[] array positionally: push the current
                // entry (may be empty if no a=msid: was seen) to keep indices in sync.
                session.msids.push_back(cur_media->msid.value_or(Msid{}));
                // Inherit session-level fingerprint if media-level is absent
                if (cur_media->dtls_fingerprint_algo.empty() &&
                    !session.dtls_fingerprint_algo.empty()) {
                    cur_media->dtls_fingerprint_algo   = session.dtls_fingerprint_algo;
                    cur_media->dtls_fingerprint_value = session.dtls_fingerprint_value;
                }
                session.media.push_back(std::move(*cur_media));
            }
            // Start new media block
            cur_media.emplace();
            parse_media_desc(value, *cur_media, impl.error_msg);
            break;
        }
        default:
            // Unknown type: ignore
            break;
        }
    }

    // Finalize last media block
    if (cur_media) {
        session.msids.push_back(cur_media->msid.value_or(Msid{}));
        // Inherit session-level fingerprint if media-level is absent
        if (cur_media->dtls_fingerprint_algo.empty() &&
            !session.dtls_fingerprint_algo.empty()) {
            cur_media->dtls_fingerprint_algo   = session.dtls_fingerprint_algo;
            cur_media->dtls_fingerprint_value = session.dtls_fingerprint_value;
        }
        session.media.push_back(std::move(*cur_media));
    }

    return core::Result<SessionDescription>::ok(std::move(session));
}

// Direction helpers (to_string / parse_direction) live in munger.cpp —
// they need a single canonical TU so we don't end up with ODR surprises
// from the `inline` declarations in the header.

} // namespace nimrtc::sdp
