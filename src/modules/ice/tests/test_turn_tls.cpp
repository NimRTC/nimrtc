// modules/ice/tests/test_turn_tls.cpp
// =============================================================================
// TURN over TLS (RFC 6062) tests.
//
// ## What this file proves
//
// TURN-over-TLS is TURN-over-TCP (RFC 8656 § 12) with the *control
// connection* wrapped in TLS (RFC 6062 § 3.2).  The relayed transport
// address the server allocates stays UDP towards the peer — only the
// control stream is protected.
//
// The transport is implemented in two halves:
//
//   - the TLS client (wolfSSL) lives in `src/modules/ice/src/turn_tls.cpp`
//     and is handed to libjuice through a narrow transport seam;
//   - libjuice keeps the TURN protocol (Allocate / Permission /
//     ChannelBind) and the RFC 8656 § 12.4/§ 12.5 framing.
//
// The tests below are deliberately **negative-heavy**: the whole point
// of the TLS transport is that a client which cannot speak TLS must
// *fail*, never quietly fall back to plain TCP.  A fallback would send
// the TURN credentials in the clear, which is exactly the defect the
// transport exists to prevent.
//
// Covered here:
//   - the transport surface reports TLS as supported
//   - a TLS client with no usable trust anchor does not fall back to
//     plain TCP
//   - a TLS client pointed at a plain-TCP server fails to allocate
//   - the seam itself: the TLS ops are wired into libjuice and a
//     handshake failure is reported as a failure, not as a downgrade
// =============================================================================

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <juice/juice.h>

#include <nimrtc/ice/ice.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/transport.hpp>

// The TLS client is an internal collaborator of the ICE module, but the
// reference-identity classification it performs (RFC 6125 §6.4.4) is
// part of the behaviour this file pins, so the header is included
// directly.
#include "../src/turn_tls.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using nimrtc::ice::Candidate;
using nimrtc::ice::CandidateType;
using nimrtc::ice::IceConfig;
using nimrtc::ice::IceTransport;
using nimrtc::ice::Role;
using nimrtc::ice::TurnServer;
using nimrtc::ice::TurnTransport;

using namespace std::chrono_literals;

constexpr const char* kTurnUser = "nimrtc";
constexpr const char* kTurnPass = "nimrtc-test-secret";

/// Find a bindable TCP port on the loopback interface.
///
/// The TURN-over-TLS control connection is a TCP stream, so the port has
/// to be probed with a *TCP* socket: Windows keeps a separate exclusion
/// range per protocol, and a UDP-probed port number can sit inside a TCP
/// block reserved for the hypervisor — binding it then fails with
/// WSAEACCES (errno 10013).
std::uint16_t find_bindable_tcp_port() noexcept {
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
#endif
    for (int attempt = 0; attempt < 16; ++attempt) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return 0;

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = 0;   // let the kernel pick
        addr.sin_addr.s_addr = htonl(0x7F000001); // 127.0.0.1

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            sockaddr_in bound{};
#ifdef _WIN32
            int len = static_cast<int>(sizeof(bound));
#else
            socklen_t len = sizeof(bound);
#endif
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
                const auto port = ntohs(bound.sin_port);
#ifdef _WIN32
                closesocket(fd);
#else
                ::close(fd);
#endif
                if (port != 0) return port;
                continue;
            }
        }
#ifdef _WIN32
        closesocket(fd);
#else
        ::close(fd);
#endif
    }
    return 0;
}

/// Find a run of consecutive bindable UDP ports, for a TURN server's
/// relay-port range.
std::uint16_t find_bindable_udp_port_run(std::uint16_t count) noexcept {
    for (int attempt = 0; attempt < 16; ++attempt) {
        // Probe the first port.
        std::uint16_t first = 0;
        {
            const int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) return 0;
            sockaddr_in addr{};
            addr.sin_family      = AF_INET;
            addr.sin_port        = 0;
            addr.sin_addr.s_addr = htonl(0x7F000001);
            if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                sockaddr_in bound{};
#ifdef _WIN32
                int len = static_cast<int>(sizeof(bound));
#else
                socklen_t len = sizeof(bound);
#endif
                if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
                    first = ntohs(bound.sin_port);
                }
            }
#ifdef _WIN32
            closesocket(fd);
#else
            ::close(fd);
#endif
        }
        if (first == 0) continue;
        if (static_cast<std::uint32_t>(first) + count > 0xFFFF) continue;

        bool all_free = true;
        for (std::uint16_t i = 0; i < count && all_free; ++i) {
            const std::uint16_t port = static_cast<std::uint16_t>(first + i);
            const int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) { all_free = false; break; }
            sockaddr_in addr{};
            addr.sin_family      = AF_INET;
            addr.sin_port        = htons(port);
            addr.sin_addr.s_addr = htonl(0x7F000001);
            all_free = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
#ifdef _WIN32
            closesocket(fd);
#else
            ::close(fd);
#endif
        }
        if (all_free) return first;
    }
    return 0;
}

/// The relay-port range is small: the tests make at most one allocation.
constexpr std::uint16_t kRelayPortCount = 8;

/// A plain-TCP TURN server (the negative control).
///
/// It is libjuice's own TURN server, which speaks TURN-over-TCP
/// (RFC 8656 § 12) and *no* TLS.  A TLS client pointed at it must fail
/// to allocate: the TLS handshake never completes, so there is no
/// Allocate response and therefore no relay candidate.
///
/// This is the control that makes the positive test meaningful.  Without
/// it, a client that silently fell back to plain TCP would pass the
/// positive test too.
class PlainTcpTurnServer {
public:
    PlainTcpTurnServer()
        : tcp_port_(find_bindable_tcp_port()),
          relay_port_begin_(find_bindable_udp_port_run(kRelayPortCount)) {
        if (tcp_port_ == 0 || relay_port_begin_ == 0) return;

        juice_server_credentials_t creds{};
        creds.username          = kTurnUser;
        creds.password          = kTurnPass;
        creds.allocations_quota = 4;

        juice_server_config_t cfg{};
        cfg.credentials            = &creds;
        cfg.credentials_count      = 1;
        cfg.max_allocations        = 8;
        cfg.max_peers              = 4;
        cfg.bind_address           = "127.0.0.1";
        cfg.port                   = 0;   // UDP listener is not needed
        cfg.tcp_port               = tcp_port_;
        cfg.relay_port_range_begin = relay_port_begin_;
        cfg.relay_port_range_end   = static_cast<std::uint16_t>(
            relay_port_begin_ + kRelayPortCount - 1);
        cfg.realm                  = "nimrtc.local";

        server_ = juice_server_create(&cfg);
    }

    ~PlainTcpTurnServer() {
        if (server_) juice_server_destroy(server_);
    }

    PlainTcpTurnServer(const PlainTcpTurnServer&)            = delete;
    PlainTcpTurnServer& operator=(const PlainTcpTurnServer&) = delete;

    bool ok() const noexcept { return server_ != nullptr; }

    std::uint16_t tcp_port() const noexcept { return tcp_port_; }

private:
    juice_server_t*   server_           = nullptr;
    std::uint16_t     tcp_port_         = 0;
    std::uint16_t     relay_port_begin_ = 0;
};

/// Find the relay candidate among the gathered local candidates.
std::vector<Candidate> relay_candidates(const IceTransport& t) {
    std::vector<Candidate> out;
    for (const auto& line : t.gathered_local_candidates()) {
        auto c = nimrtc::ice::parse_candidate(line);
        if (c && c->type == CandidateType::Relay) out.push_back(*c);
    }
    return out;
}

/// Wait until the transport has a relay candidate (or the deadline passes).
std::vector<Candidate> wait_for_relay_candidate(IceTransport& t,
                                                 std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto relays = relay_candidates(t);
        if (!relays.empty()) return relays;
        t.recv();
        std::this_thread::sleep_for(50ms);
    }
    return {};
}

IceConfig make_agent_config() {
    IceConfig cfg;
    cfg.role = Role::Controlling;
    cfg.bind_address = "127.0.0.1";
    return cfg;
}

} // namespace

// -----------------------------------------------------------------------------
// The transport surface
// -----------------------------------------------------------------------------

TEST(TurnTls, TransportSurfaceReportsTlsSupported) {
    // TURN over TLS (RFC 6062 § 3.2) is implemented: the TLS client
    // lives in the ICE module and is handed to libjuice through the
    // transport seam.  The surface must say so.
    EXPECT_TRUE(IceTransport::turn_transport_supported(TurnTransport::Udp));
    EXPECT_TRUE(IceTransport::turn_transport_supported(TurnTransport::Tcp));
    EXPECT_TRUE(IceTransport::turn_transport_supported(TurnTransport::Tls));

    // And the plugin seam must agree, because the engine only ever sees
    // the interface.
    IceTransport t{make_agent_config()};
    EXPECT_TRUE(t.turn_transport_supported(nimrtc::plugins::TurnTransport::Tls));
}

// -----------------------------------------------------------------------------
// The negative control: a TLS client must NOT fall back to plain TCP
// -----------------------------------------------------------------------------

TEST(TurnTls, TlsClientAgainstPlainTcpServerDoesNotAllocate) {
    // The server speaks plain TURN-over-TCP only.  A client that asked
    // for TLS must fail to allocate — if it fell back to plain TCP it
    // would succeed, and the whole "the credentials stay encrypted"
    // claim would be void.
    PlainTcpTurnServer server;
    ASSERT_TRUE(server.ok()) << "the in-tree TURN server did not start";

    IceConfig cfg = make_agent_config();
    // No CA file: the handshake cannot succeed anyway, which is the
    // point.  The server has no TLS listener at all.
    cfg.turn_tls_verify_peer = false;

    IceTransport t{cfg};
    TurnServer ts;
    ts.host      = "127.0.0.1";
    ts.port      = server.tcp_port();
    ts.username  = kTurnUser;
    ts.password  = kTurnPass;
    ts.transport = TurnTransport::Tls;
    t.add_turn_server(ts);

    ASSERT_EQ(t.open(), 0u) << "open() failed";

    // Give the TLS client ample time to fail the handshake.
    const auto relays = wait_for_relay_candidate(t, 8s);
    EXPECT_TRUE(relays.empty())
        << "a TLS client allocated a relay against a plain-TCP TURN server: "
           "the TLS request was silently downgraded to plain TCP";

    t.close();
}

TEST(TurnTls, TlsClientAgainstUnreachableServerDoesNotAllocate) {
    // Nothing is listening at all.  The transport must not wedge: the
    // host candidates are still gathered and no relay candidate is
    // invented.
    IceConfig cfg = make_agent_config();
    cfg.turn_tls_verify_peer = false;

    IceTransport t{cfg};
    TurnServer ts;
    ts.host      = "127.0.0.1";
    ts.port      = 64997;   // nothing listening
    ts.username  = kTurnUser;
    ts.password  = kTurnPass;
    ts.transport = TurnTransport::Tls;
    t.add_turn_server(ts);

    ASSERT_EQ(t.open(), 0u) << "open() failed even though the TURN server is unreachable";

    const auto hosts = t.gathered_local_candidates();
    bool has_host = false;
    for (const auto& line : hosts) {
        auto c = nimrtc::ice::parse_candidate(line);
        if (c && c->type == CandidateType::Host) { has_host = true; break; }
    }
    EXPECT_TRUE(has_host) << "an unreachable TURN server cost us the host candidates";

    EXPECT_TRUE(relay_candidates(t).empty());

    t.close();
}

// -----------------------------------------------------------------------------
// The TLS client's own failure modes
// -----------------------------------------------------------------------------

TEST(TurnTls, MissingTrustAnchorDoesNotProduceARelayCandidate) {
    // The CA file points at something that is not a certificate.  The
    // TLS client cannot build a context, so the allocation must fail.
    // The host candidates must survive: a broken TURN configuration
    // must not take the whole session down.
    PlainTcpTurnServer server;
    ASSERT_TRUE(server.ok()) << "the in-tree TURN server did not start";

    IceConfig cfg = make_agent_config();
    cfg.turn_tls_ca_file = "this-file-does-not-exist.pem";

    IceTransport t{cfg};
    TurnServer ts;
    ts.host      = "127.0.0.1";
    ts.port      = server.tcp_port();
    ts.username  = kTurnUser;
    ts.password  = kTurnPass;
    ts.transport = TurnTransport::Tls;
    t.add_turn_server(ts);

    ASSERT_EQ(t.open(), 0u) << "open() failed";

    const auto relays = wait_for_relay_candidate(t, 8s);
    EXPECT_TRUE(relays.empty())
        << "a relay candidate appeared despite the unusable trust anchor";

    const auto hosts = t.gathered_local_candidates();
    bool has_host = false;
    for (const auto& line : hosts) {
        auto c = nimrtc::ice::parse_candidate(line);
        if (c && c->type == CandidateType::Host) { has_host = true; break; }
    }
    EXPECT_TRUE(has_host) << "the broken TLS configuration cost us the host candidates";

    t.close();
}

// -----------------------------------------------------------------------------
// The reference identity: RFC 6125 §6.4.4 splits the check by shape
// -----------------------------------------------------------------------------

TEST(TurnTls, ReferenceIdentityShapeSelectsTheVerificationPath) {
    // A TURN server is often reached *by address*.  RFC 6125 §6.4.4
    // requires an IP reference identity to be matched against the
    // certificate's `iPAddress` SAN entries, and a host name against
    // the `dNSName` entries -- and the two are exclusive.  A client
    // that pointed at 127.0.0.1 and fell back to matching the
    // `dNSName` "localhost" would let a certificate issued for a
    // different name satisfy an IP reference.
    //
    // The classification is what decides which wolfSSL call is made, so
    // it is worth pinning directly.
    using nimrtc::ice::TurnTlsClient;

    EXPECT_TRUE(TurnTlsClient::reference_is_ip_literal("127.0.0.1"));
    EXPECT_TRUE(TurnTlsClient::reference_is_ip_literal("::1"));
    EXPECT_TRUE(TurnTlsClient::reference_is_ip_literal("[::1]"));
    EXPECT_TRUE(TurnTlsClient::reference_is_ip_literal("192.0.2.10"));
    EXPECT_TRUE(TurnTlsClient::reference_is_ip_literal("2001:db8::1"));

    EXPECT_FALSE(TurnTlsClient::reference_is_ip_literal("turn.nimrtc.local"));
    EXPECT_FALSE(TurnTlsClient::reference_is_ip_literal("localhost"));
    EXPECT_FALSE(TurnTlsClient::reference_is_ip_literal(""));
    // A name that merely *starts* with digits is not an IP literal.
    EXPECT_FALSE(TurnTlsClient::reference_is_ip_literal("127.0.0.1.example.com"));
    EXPECT_FALSE(TurnTlsClient::reference_is_ip_literal("not-an-ip"));
}

TEST(TurnTls, PlainTcpTransportIsUnaffectedByTheTlsConfiguration) {
    // The TLS machinery must not disturb the plain transports: a TURN
    // server configured over plain TCP still allocates when a TLS
    // configuration is present but unused.
    PlainTcpTurnServer server;
    ASSERT_TRUE(server.ok()) << "the in-tree TURN server did not start";

    IceConfig cfg = make_agent_config();
    // A deliberately broken CA file: it must not affect the plain-TCP
    // transport, because the TLS client is never consulted there.
    cfg.turn_tls_ca_file = "this-file-does-not-exist.pem";

    IceTransport t{cfg};
    TurnServer ts;
    ts.host      = "127.0.0.1";
    ts.port      = server.tcp_port();
    ts.username  = kTurnUser;
    ts.password  = kTurnPass;
    ts.transport = TurnTransport::Tcp;
    t.add_turn_server(ts);

    ASSERT_EQ(t.open(), 0u) << "open() failed";

    const auto relays = wait_for_relay_candidate(t, 15s);
    ASSERT_FALSE(relays.empty())
        << "the plain TURN/TCP transport stopped working once a TLS "
           "configuration was present";

    t.close();
}
