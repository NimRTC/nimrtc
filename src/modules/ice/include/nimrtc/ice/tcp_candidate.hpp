// tcp_candidate.hpp — the TCP candidate shape NimRTC gathers and serialises
// for ICE-over-TCP (RFC 6544).
//
// libjuice's ICE layer is UDP-only (it drops every candidate whose
// transport is not "UDP"), so the TCP path is NimRTC's own (ADR-028,
// Option A).  This header owns the *shape* of a gathered host TCP
// candidate and its SDP serialisation; the socket / framing / check
// driver live in src/ (tcp_socket.*, tcp_framing.*, tcp_ice.*).
//
// RFC 6544 §4.5 defines host candidates with a tcptype — active (this
// agent initiates the connection) or passive (this agent listens) — and
// nothing else: server-reflexive and relayed TCP candidates are out of
// scope for this slice.  Both roles are gathered and emitted (confirmed
// 2026-11-21), so two peers that each emit both always have a pair — an
// active on one side can connect to a passive on the other.

// Stability classification: NIMRTC_API_STABLE (visual marker;
// the empty macro is erased by the preprocessor, so a compile-time check
// cannot detect it — the classification sweep walks the file at runtime,
// per ADR-015).

#ifndef NIMRTC_ICE_TCP_CANDIDATE_HPP
#define NIMRTC_ICE_TCP_CANDIDATE_HPP

#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <string>

namespace nimrtc::ice {

/// The connection role of a TCP candidate (RFC 6544 §4.1).
enum class NIMRTC_API_STABLE TcpCandidateRole {
    Active,   ///< This agent initiates the TCP connection.
    Passive,  ///< This agent listens and accepts the connection.
};

/// The `tcptype` token of a candidate line ("active" / "passive").
inline NIMRTC_API_STABLE const char* to_string(TcpCandidateRole role) {
    return role == TcpCandidateRole::Active ? "active" : "passive";
}

/// A gathered host TCP candidate (RFC 6544 §4.5).
///
/// The foundation is namespaced ("tcp-1", "tcp-2", …) so a peer can
/// never confuse a TCP candidate with a libjuice UDP candidate, whose
/// foundation is a plain number ("1", "2", …).
struct NIMRTC_API_STABLE TcpCandidate {
    std::string foundation;        ///< "tcp-1", "tcp-2", … (distinct from UDP)
    std::uint8_t component_id = 1; ///< 1 for RTP, 2 for RTCP
    std::string host;              ///< IP literal (IPv4, like UdpSocket)
    std::uint16_t port = 0;
    TcpCandidateRole tcptype = TcpCandidateRole::Active;

    /// RFC 6544 §4.3: an active candidate (we connect) is preferred over
    /// a passive one (we listen), and TCP candidates rank below UDP host
    /// candidates of the same component.  The value never collides with a
    /// libjuice UDP priority: libjuice packs a 16-bit type preference
    /// (ice_compute_priority), so a 24-bit type preference puts every TCP
    /// value in a range disjoint from every UDP value.
    std::uint32_t priority() const noexcept;

    /// SDP candidate body without the leading "a=", e.g.
    /// "candidate:tcp-1 1 TCP 1853882113 127.0.0.1 5000 typ host tcptype active".
    std::string to_sdp() const;
};

inline NIMRTC_API_STABLE std::uint32_t TcpCandidate::priority() const noexcept {
    // RFC 6544 §4.3 prescribes the type preferences: 110 for active,
    // 100 for passive.  RFC 8445 §5.1.2.1 computes the priority as
    //   (2^24)*type_preference + (2^8)*local_preference + component_id.
    // The local preference 32767 mirrors the NAT-preference guidance for
    // IPv4 in RFC 8445 §5.1.2.1 (libjuice uses the same value for its
    // AF_INET "family preference").
    const std::uint32_t type_pref = (tcptype == TcpCandidateRole::Active) ? 110u : 100u;
    const std::uint32_t local_pref = 32767u;
    return (type_pref << 24) | (local_pref << 8) | component_id;
}

inline std::string TcpCandidate::to_sdp() const {
    std::string s = "candidate:";
    s += foundation;
    s += ' ';
    s += std::to_string(static_cast<unsigned>(component_id));
    s += " TCP ";
    s += std::to_string(priority());
    s += ' ';
    s += host;
    s += ' ';
    s += std::to_string(port);
    s += " typ host tcptype ";
    s += to_string(tcptype);
    return s;
}

}  // namespace nimrtc::ice

#endif  // NIMRTC_ICE_TCP_CANDIDATE_HPP
