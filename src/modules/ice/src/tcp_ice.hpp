// tcp_ice.hpp — the ICE-over-TCP connectivity-check driver (RFC 6544).
//
// libjuice owns the UDP ICE path and drops TCP candidates on arrival, so
// the TCP path is NimRTC's own collaborator inside src/modules/ice/
// (ADR-028, Option A): gather host TCP candidates (active + passive),
// exchange them over the seam's existing trickle surface, run STUN
// connectivity checks over the framed TCP stream, and nominate the pair
// that wins.  The `IceTransport` seam reports the winner through
// `selected_pair()`.
//
// The STUN message encode/decode below is the *second* STUN
// implementation in this codebase (RFC 8489 §6) — libjuice has the first
// one, for UDP.  The functions are exposed so a unit test can pin the
// wire format field by field (the slice's risk 1).

#ifndef NIMRTC_ICE_TCP_ICE_HPP
#define NIMRTC_ICE_TCP_ICE_HPP

#include <nimrtc/ice/tcp_candidate.hpp>
#include "tcp_framing.hpp"
#include "tcp_socket.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace nimrtc::ice {

// ---------------------------------------------------------------------------
// STUN message building / verification (RFC 8489 §6, RFC 8445 §7.1.2)
// ---------------------------------------------------------------------------

/// Build an ICE connectivity-check Binding Request, authenticated with
/// MESSAGE-INTEGRITY (HMAC-SHA1 keyed with the *remote* ICE password).
///
/// @param username       "local_ufrag:remote_ufrag" (RFC 8445 §7.1.2.3)
/// @param password       the remote agent's ICE password
/// @param priority       the local candidate's priority (PRIORITY attr)
/// @param tiebreaker     the ICE-CONTROLLING tiebreaker (8 bytes BE)
/// @param use_candidate  whether to carry USE-CANDIDATE (nomination)
/// @param transaction_id 12-byte transaction id
std::vector<std::uint8_t> stun_build_binding_request(
    const std::string& username, const std::string& password,
    std::uint32_t priority, std::uint64_t tiebreaker, bool use_candidate,
    std::span<const std::uint8_t> transaction_id);

/// Verify a Binding Request (the accepting side): well-formed, correct
/// magic cookie, and a valid MESSAGE-INTEGRITY keyed with the *local*
/// password.  Returns the request's USERNAME and transaction id when
/// valid.
struct StunRequestInfo {
    bool valid = false;
    std::string username;
    std::array<std::uint8_t, 12> transaction_id{};
};
StunRequestInfo stun_verify_binding_request(std::span<const std::uint8_t> msg,
                                            const std::string& password);

/// Build the authenticated Binding Success Response to a validated
/// request.  Echoes the request's username and transaction id, carries
/// the XOR-MAPPED-ADDRESS, and signs with MESSAGE-INTEGRITY keyed with
/// the *local* password.
std::vector<std::uint8_t> stun_build_binding_response(
    const std::string& username, const std::string& password,
    std::span<const std::uint8_t> transaction_id,
    const std::string& mapped_host, std::uint16_t mapped_port);

/// Verify a Binding Success Response (the initiating side): correct
/// message type, matching transaction id, valid MESSAGE-INTEGRITY keyed
/// with the *remote* password, and a parseable XOR-MAPPED-ADDRESS.
/// Returns the mapped "ip:port" when valid.
std::optional<std::string> stun_verify_binding_response(
    std::span<const std::uint8_t> msg, const std::string& password,
    std::span<const std::uint8_t> transaction_id);

// ---------------------------------------------------------------------------
// TcpIceDriver — gather, exchange, check, nominate
// ---------------------------------------------------------------------------

/// Owns the TCP half of ICE: the two gathered host candidates (passive
/// listener + active connector), the remote TCP candidates trickled in
/// through the seam, and the background connectivity-check thread.
class TcpIceDriver {
public:
    struct Config {
        /// The local address to bind the passive listener and active
        /// connector to ("127.0.0.1" for the loopback shape).
        std::string bind_host;
        std::string local_ufrag;
        std::string local_password;
    };

    explicit TcpIceDriver(Config config);
    ~TcpIceDriver();

    TcpIceDriver(const TcpIceDriver&)            = delete;
    TcpIceDriver& operator=(const TcpIceDriver&) = delete;

    /// Gather the two host TCP candidates.  Appends them to `out` (the
    /// caller publishes them as local candidates).  Returns false when
    /// the sockets cannot be made — the TCP fallback is unavailable.
    bool gather(std::vector<TcpCandidate>& out);

    /// The two gathered candidates (empty until gather() succeeds).
    /// Thread-safe.
    std::vector<TcpCandidate> local_candidates() const;

    /// Feed the remote credentials from the peer's SDP.  Thread-safe.
    void set_remote_credentials(std::string remote_ufrag, std::string remote_password);

    /// Feed a remote TCP candidate (trickle ICE, RFC 8838).  Thread-safe.
    void add_remote(TcpCandidate candidate);

    /// Start the background check thread.  Idempotent.
    void start();

    /// Stop the check thread and close the sockets.  Idempotent.
    void stop();

    /// The nominated local/remote pair, if one was verified.  Set at most
    /// once — the winner never flaps.  Thread-safe.
    std::optional<std::pair<TcpCandidate, TcpCandidate>> nominated() const;

    /// Invoked exactly once, from the check thread, when a pair is
    /// verified (the IceTransport publishes it to the seam).
    std::function<void(const TcpCandidate& local, const TcpCandidate& remote)> on_nominated;

private:
    void thread_main();
    bool try_outbound();
    bool try_inbound();
    /// One outbound check: connect the active socket to `remote`, run the
    /// STUN request/response, and nominate on success.
    bool outbound_check(const TcpCandidate& remote, const TcpCandidate& local_active,
                        const std::string& remote_ufrag,
                        const std::string& remote_password);
    void nominate(const TcpCandidate& local, const TcpCandidate& remote);
    /// A freshly bound active socket (the advertised candidate port must
    /// match the source port of the outbound connection).  Caller holds mu_.
    bool rebind_active();
    /// Read one complete framed STUN message with a total time budget.
    bool read_one_frame(TcpSocket& sock, std::vector<std::uint8_t>& msg, int budget_ms);
    /// Match a connection source "ip:port" to a remote TCP candidate.
    std::optional<TcpCandidate> find_remote(const std::string& host,
                                            std::uint16_t port);

    Config config_;
    mutable std::mutex mu_;

    std::vector<TcpCandidate> remote_;
    std::string remote_ufrag_;
    std::string remote_password_;

    std::vector<TcpCandidate> local_;  // gathered candidates (passive, active)
    std::optional<std::pair<TcpCandidate, TcpCandidate>> nominated_;

    /// Our ICE-CONTROLLING tiebreaker (RFC 8445 §7.3.1.1).  Both peers in
    /// the symmetric test shape act as controlling agents.
    std::uint64_t tiebreaker_ = 0;

    std::atomic<bool> stop_{false};
    std::thread thread_;

    TcpSocket passive_listener_;
    TcpSocket active_socket_;
};

}  // namespace nimrtc::ice

#endif  // NIMRTC_ICE_TCP_ICE_HPP
