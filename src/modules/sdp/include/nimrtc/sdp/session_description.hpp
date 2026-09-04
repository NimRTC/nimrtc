#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/error.hpp>

// =============================================================================
// nimrtc::sdp
// -----------------------------------------------------------------------------
// Session Description Protocol (RFC 4566) with WebRTC extensions
// (RFC 8829, draft-ietf-mmusic-rids, draft-ietf-mmusic-msid, RFC 5763).
//
// P0 contract: data structures + parse / serialise entry points.
// Implementation lands in P1 (Agent 2).
// =============================================================================
namespace nimrtc::sdp {

// -----------------------------------------------------------------------------
// Media / direction enums
// -----------------------------------------------------------------------------
enum class MediaType {
    Audio,
    Video,
    Application,
    Data,
    Other,
};

const char*       to_string(MediaType t) noexcept;
std::optional<MediaType> parse_media_type(std::string_view s) noexcept;

enum class Direction {
    SendRecv,
    SendOnly,
    RecvOnly,
    Inactive,
};

const char*                  to_string(Direction d) noexcept;
std::optional<Direction>     parse_direction(std::string_view s) noexcept;

// -----------------------------------------------------------------------------
// Per-media MSID (RFC 8830 / draft-ietf-mmusic-msid)
// -----------------------------------------------------------------------------
struct Msid {
    std::string stream_id;
    std::string track_id;
};

// -----------------------------------------------------------------------------
// Per-media description
// -----------------------------------------------------------------------------
struct MediaDescription {
    MediaType  type            = MediaType::Audio;
    std::uint16_t port         = 0;
    std::string protocol;          // e.g. "UDP/TLS/RTP/SAVPF", "RTP/SAVPF"
    std::vector<std::string> formats;   // payload type numbers, kept as strings
                                       // (some specs allow ranges, e.g. "96-127")

    Direction   direction      = Direction::SendRecv;
    std::string mid;             // a=mid:<value>  — BUNDLE grouping identifier
    std::string ice_ufrag;
    std::string ice_pwd;
    std::string ice_options;     // e.g. "trickle"
    std::string dtls_fingerprint_algo;  // typically "sha-256"
    std::string dtls_fingerprint_value; // base64
    std::string dtls_setup;      // "actpass" | "active" | "passive" | "holdconn"
    std::string rtcp_mux_value; // "rtcp-mux" or empty when not present

    // a=rtpmap:<fmt> <encoding>/<clock>[/<channels>]
    struct RtpMap {
        std::string  encoding;
        std::uint32_t clock_rate = 0;
        std::uint32_t channels   = 1;
    };
    std::map<std::string, RtpMap> rtpmap;

    // a=fmtp:<fmt> <raw params>
    std::map<std::string, std::string> fmtp;

    // a=rtcp-fb:<fmt> <type> [<param>]
    // stored verbatim to preserve ordering and exact formatting
    std::vector<std::string> rtcp_fb;

    // a=extmap:<id>[/<direction>] <URI> [<params>]
    struct ExtMap {
        int id = 0;
        Direction direction = Direction::SendRecv;
        std::string uri;
        std::map<std::string, std::string> params;
    };
    std::vector<ExtMap> extmap;

    // a=candidate:...   (typically only present in SDP answers received
    // from ICE-lite endpoints; we capture but do not generate).
    std::vector<std::string> candidates;

    // Generic attributes not captured above, in original order.
    std::vector<std::pair<std::string, std::string>> extra_attrs;

    // c= line connection information (default 0.0.0.0 if absent).
    std::string connection_address = "0.0.0.0";
    int         ttl                = 0;
    int         address_family      = 4;   // 4 (IPv4) or 6 (IPv6)

    // a=msid:<stream_id> <track_id> (one per media line that has it).
    // The same data is also mirrored positionally in SessionDescription::msids.
    std::optional<Msid> msid;
};

// -----------------------------------------------------------------------------
// Session description
// -----------------------------------------------------------------------------
struct SessionDescription {
    int version = 0;             // v= line
    std::string session_id;      // o= IN IP4 ... <sess-id>
    std::string session_name = "-";   // s= line
    std::string session_info;       // i= line (session information)
    std::string uri;                  // u= line (URI)

    // Origin (o= line), stored individually so we can rebuild.
    std::string origin_username       = "-";
    std::string origin_session_id;
    std::string origin_session_version;
    std::string origin_address        = "0.0.0.0";

    // a=group:BUNDLE <mids>
    std::vector<std::string> bundle_mids;

    // a=ice-options
    std::string ice_options;

    // a=msid-semantic: WMS <streams>
    std::string msid_semantic_token   = "WMS";
    std::vector<std::string> msid_semantic_streams;

    // a=msid:<stream_id> <track_id>   (one entry per m= line that has it)
    // Indexed positionally — i.e. msid[i] applies to media[i].
    std::vector<Msid> msids;   // positional: msids[i] applies to media[i]

    // Session-level DTLS fingerprint (a=fingerprint).
    std::string dtls_fingerprint_algo;
    std::string dtls_fingerprint_value;
    std::string dtls_setup;

    // Session-level attributes not captured above.
    std::vector<std::pair<std::string, std::string>> extra_attrs;

    // Connection info from session-level c= line, if any.
    std::string session_connection_address;
    int         session_connection_ttl = 0;
    int         session_address_family = 4;

    std::vector<MediaDescription> media;
};

// -----------------------------------------------------------------------------
// Parser
// -----------------------------------------------------------------------------
class Parser {
public:
    // Out-of-line declarations: see parser.cpp. Required by the pImpl
    // idiom — the definitions live where `Impl` is complete.
    Parser();
    ~Parser();

    // Parse full SDP text (UTF-8 ASCII).
    // Returns ProtocolError on malformed input with location hint in message.
    core::Result<SessionDescription> parse(std::string_view sdp_text) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// -----------------------------------------------------------------------------
// Munger — emit SDP text from a SessionDescription.
// WebRTC interop requires attribute order to follow RFC 8829 §5 ("Standard
// ordering of attributes"): see the comment inside the implementation.
// -----------------------------------------------------------------------------
class Munger {
public:
    // Out-of-line declarations: the definitions live in munger.cpp where
    // `Impl` is complete. Without this, any TU that uses Munger as a value
    // fails to compile because the implicit default ctor / dtor would try
    // to instantiate unique_ptr<Impl>::~unique_ptr() without seeing Impl.
    Munger();
    ~Munger();

    core::Result<std::string> to_sdp(const SessionDescription& sdp) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nimrtc::sdp