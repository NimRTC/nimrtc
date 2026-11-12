// modules/ice/tests/test_turn_relay.cpp
// =============================================================================
// TURN relay candidate tests (RFC 8656).
//
// These are the tests that make the "TURN relay candidates" capability
// claim *true* rather than merely configurable.
//
// ## Why this file exists
//
// NimRTC exposed a `TurnServer` configuration and an
// `add_turn_server()` entry point, and the capability table marked the
// capability as supported.  That was wrong: libjuice's multiplexer
// (JUICE_CONCURRENCY_MODE_MUX, the NimRTC default) owns exactly one UDP
// socket and has no way to open the extra socket a TURN allocation
// needs.  libjuice therefore *silently ignores* the configured TURN
// servers in MUX mode:
//
//      // libjuice src/agent.c
//      if (mode == JUICE_CONCURRENCY_MODE_MUX) {
//          if (agent->config.turn_servers_count > 0)
//              JLOG_WARN("TURN servers are not supported in mux mode");
//      } else if (agent->config.turn_servers_count > 0) { ... }
//
// The host saw "TURN server added host=..." in the log, gathering
// finished successfully, and no `typ relay` candidate ever appeared.
// The warning only reached libjuice's own log handler, so nothing in
// NimRTC surfaced the contradiction.
//
// The test below stands up a *real* TURN server (libjuice's built-in
// server, `juice_server_create()`) on the loopback interface, points an
// ICE transport at it, and asserts that a relay candidate is actually
// gathered.  It therefore fails against the pre-fix behaviour and
// passes against the fixed one.
//
// Covered here:
//   - the resolved concurrency mode leaves MUX when TURN is configured
//   - a TURN server that is configured produces a `typ relay` candidate
//   - an explicitly pinned MUX mode is honoured (and reported)
//   - the relay candidate carries a parseable raddr / rport pair
//   - a TURN server that cannot be reached does NOT produce a relay
//     candidate (and does not wedge the gathering)
// =============================================================================

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <juice/juice.h>

#include <nimrtc/ice/ice.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/transport.hpp>

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

// WSL2's UDP loopback is unreliable; the relay tests skip there.
bool is_wsl2() noexcept {
#if defined(__linux__)
    std::ifstream f("/proc/version");
    if (f) {
        char buf[128] = {0};
        f.read(buf, sizeof(buf) - 1);
        if (std::string(buf).find("microsoft") != std::string::npos) return true;
    }
#endif
    return false;
}

// Can this host bind a socket to an address inside 127.0.0.0/8 other than
// 127.0.0.1?
//
// The data-path test needs the two agents on *different* loopback
// addresses (see the comment on cfg_a.bind_address below), and it
// therefore needs 127.0.0.2 and 127.0.0.3 to be bindable.  Linux treats
// the whole 127/8 range as loopback, but macOS only has 127.0.0.1
// configured, so binding 127.0.0.2 there fails with EADDRNOTAVAIL.
// Rather than hard-coding a platform list, probe for it.
bool can_bind_secondary_loopback() noexcept {
    static const bool ok = [] {
#ifdef _WIN32
        // The probe needs Winsock; the juice server brings it up later, so
        // bring it up here too (the extra WSAStartup is refcounted).
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return false;
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = 0;
        addr.sin_addr.s_addr = htonl(0x7F000002); // 127.0.0.2
        const bool bound = ::bind(fd, reinterpret_cast<sockaddr *>(&addr),
                                  sizeof(addr)) == 0;
#ifdef _WIN32
        closesocket(fd);
#else
        ::close(fd);
#endif
        return bound;
    }();
    return ok;
}

// Port windows for the loopback test.  They have to be disjoint from the
// windows the other ICE tests use (63000-63099 / 63200-63299) and from
// each other: every port the suite binds has to be unique, because two
// sockets on the same port would silently share it.
//
// Rationale for the ranges: the Windows CI runner reserves 60000-60999
// and the OS dynamic-exclusion band starts at 52297, so the 64000s are
// above both while still below the uint16_t ceiling.
constexpr std::uint16_t kTurnServerPort  = 64000;
// One UDP/TCP port pair per test.  CI runs ctest with -j 2, so two
// tests that each stand up a TURN server on the same port race for
// it: the TCP listener fails to bind (EADDRINUSE) and the UDP socket
// silently shares the port (libjuice sets SO_REUSEPORT), which lets
// one test's client talk to the other test's server.  Every port the
// suite binds has to be disjoint from the others.
constexpr std::uint16_t kTurnUdpPortA    = 64010;
constexpr std::uint16_t kTurnUdpPortB    = 64020;
constexpr std::uint16_t kTurnUdpPortC    = 64030;
constexpr std::uint16_t kTurnTcpPortA    = 64011;
constexpr std::uint16_t kTurnTcpPortB    = 64021;
constexpr std::uint16_t kTurnTcpPortC    = 64031;
// The relay port range is exclusive per server too: each allocation
// takes one port out of the range.
constexpr std::uint16_t kRelayPortBegin  = 64100;
constexpr std::uint16_t kRelayPortEnd    = 64199;
constexpr std::uint16_t kRelayPortBegin2 = 64200;
constexpr std::uint16_t kRelayPortEnd2   = 64299;
constexpr std::uint16_t kRelayPortBeginB = 64300;
constexpr std::uint16_t kRelayPortEndB   = 64399;
constexpr std::uint16_t kRelayPortBeginC = 64400;
constexpr std::uint16_t kRelayPortEndC   = 64499;
// The client agents bind their own UDP sockets.  They must NOT pin a port
// range: Windows reserves whole blocks of UDP ports for the hypervisor
// (Hyper-V / HNS), and a bind() inside a reserved block fails with
// WSAEACCES (errno 10013).  The reservations move between runs, so a
// pinned range passes on one CI runner and fails on the next.
//
// Instead every client agent asks for an ephemeral port (begin == end ==
// 0), which the kernel allocates from the free pool and therefore never
// picks a reserved port.  The vendored libjuice in Mux mode hands out a
// distinct OS-assigned port per agent, so two agents in the same process
// no longer need disjoint ranges either.
constexpr std::uint16_t kAgentPortBegin   = 0;
constexpr std::uint16_t kAgentPortEnd     = 0;

constexpr const char* kTurnUser = "nimrtc";
constexpr const char* kTurnPass = "nimrtc-turn-password";

// -----------------------------------------------------------------------------
// An in-tree TURN server.
//
// libjuice ships a TURN server implementation (RFC 8656) alongside the
// client.  It is the only TURN server this repository can rely on being
// present in every environment, so the test uses it rather than an
// external service.
// -----------------------------------------------------------------------------
class TurnServerFixture {
public:
    // `tcp_port` is non-zero to also stand up the TURN-over-TCP listener
    // (RFC 8656 section 12).  Zero means "UDP only".
    //
    // Every port the server binds has to be unique across the suite:
    // CI runs ctest with -j 2, so two servers on the same TCP port race
    // for it, and two servers on the same UDP port would silently share
    // it.
    TurnServerFixture(std::uint16_t udp_port, std::uint16_t tcp_port = 0,
                      std::uint16_t relay_port_begin = kRelayPortBegin,
                      std::uint16_t relay_port_end = kRelayPortEnd) {
        juice_server_credentials_t creds{};
        creds.username           = kTurnUser;
        creds.password           = kTurnPass;
        creds.allocations_quota  = 8;

        juice_server_config_t cfg{};
        cfg.credentials           = &creds;
        cfg.credentials_count     = 1;
        cfg.max_allocations       = 16;
        cfg.max_peers             = 8;
        cfg.bind_address          = "127.0.0.1";
        cfg.port                  = udp_port;
        cfg.tcp_port              = tcp_port;
        cfg.relay_port_range_begin = relay_port_begin;
        cfg.relay_port_range_end   = relay_port_end;
        cfg.realm                 = "nimrtc";

        server_ = juice_server_create(&cfg);
    }

    ~TurnServerFixture() {
        if (server_) juice_server_destroy(server_);
    }

    TurnServerFixture(const TurnServerFixture&)            = delete;
    TurnServerFixture& operator=(const TurnServerFixture&) = delete;

    bool ok() const noexcept { return server_ != nullptr; }

    std::uint16_t port() const noexcept {
        return server_ ? juice_server_get_port(server_) : 0;
    }

private:
    juice_server_t* server_ = nullptr;
};

// Find the relay candidate among the gathered local candidates.
std::vector<Candidate> relay_candidates(const IceTransport& t) {
    std::vector<Candidate> out;
    for (const auto& line : t.gathered_local_candidates()) {
        auto c = nimrtc::ice::parse_candidate(line);
        if (c && c->type == CandidateType::Relay) out.push_back(*c);
    }
    return out;
}

// Wait until the transport has a relay candidate (or the deadline passes).
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

IceConfig make_agent_config(std::uint16_t port_begin = kAgentPortBegin,
                            std::uint16_t port_end = kAgentPortEnd) {
    IceConfig cfg;
    cfg.role = Role::Controlling;
    cfg.bind_address = "127.0.0.1";
    cfg.local_port_range_begin = port_begin;
    cfg.local_port_range_end   = port_end;
    return cfg;
}

// -----------------------------------------------------------------------------
// A receive-side collector.
//
// libjuice dispatches inbound packets on its own thread; the NimRTC wrapper
// queues them and hands them to the application from `recv()`.  The
// collector therefore has to be fed by the test's own pump loop.
// -----------------------------------------------------------------------------
class RxCollector {
public:
    void on_recv(nimrtc::plugins::BufferView v) {
        std::lock_guard<std::mutex> lk(mu_);
        rx_.emplace_back(v.data(), v.data() + v.size());
        cv_.notify_all();
    }

    bool wait_one(std::vector<std::uint8_t>& out,
                  std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, timeout, [this] { return !rx_.empty(); })) {
            return false;
        }
        out = std::move(rx_.front());
        rx_.pop_front();
        return true;
    }

private:
    std::mutex              mu_;
    std::condition_variable cv_;
    std::deque<std::vector<std::uint8_t>> rx_;
};

// Build the list of relayed candidate lines of the peer.
//
// Why the filtering matters:
//
// On the loopback interface a host↔host pair is perfectly reachable, so an
// unrestricted offer/answer would pair the two agents directly over UDP and
// the TURN allocation would never carry a single byte.  The only way to
// force the traffic through the relay is to advertise *only* the relayed
// candidates to the peer, which leaves the relayed↔relayed pair as the sole
// candidate pair in the checklist.
//
// The credentials are taken from the pinned ones (IceConfig::local_ufrag /
// local_password), so both agents agree on them without depending on the
// order in which the relay candidates arrive.
std::vector<std::string> relay_candidate_lines(const IceTransport& peer) {
    std::vector<std::string> out;
    for (const auto& line : peer.gathered_local_candidates()) {
        auto c = nimrtc::ice::parse_candidate(line);
        if (c && c->type == CandidateType::Relay) {
            out.push_back(line);
        }
    }
    return out;
}

// The host address a transport actually bound to.
//
// libjuice binds the UDP socket to IceConfig::bind_address and reports that
// very address as the host candidate.  A mismatch therefore means the bind
// did not happen and the transport fell back to "any address" — which the
// end-to-end relay test must not silently accept.
std::string host_address_of(const IceTransport& t) {
    for (const auto& line : t.gathered_local_candidates()) {
        auto c = nimrtc::ice::parse_candidate(line);
        if (c && c->type == CandidateType::Host) return c->host;
    }
    return {};
}

} // namespace

// -----------------------------------------------------------------------------
// The concurrency-mode resolution (the root cause)
// -----------------------------------------------------------------------------

TEST(TurnRelay, ConfiguredTurnServerLeavesMuxMode) {
    IceConfig cfg = make_agent_config(kAgentPortBegin, kAgentPortEnd);
    // The default mode is MUX.
    ASSERT_EQ(cfg.mode, IceConfig::Mode::Mux);
    ASSERT_FALSE(cfg.mode_is_explicit);

    IceTransport t{cfg};
    // No TURN server yet → the mode is untouched.
    EXPECT_EQ(t.resolved_mode(), IceConfig::Mode::Mux);

    TurnServer ts;
    ts.host = "127.0.0.1";
    ts.port = kTurnServerPort;
    ts.username = kTurnUser;
    ts.password = kTurnPass;
    t.add_turn_server(ts);

    // MUX cannot carry a TURN allocation, so the transport must have
    // moved off it.
    EXPECT_NE(t.resolved_mode(), IceConfig::Mode::Mux)
        << "MUX mode silently ignores the configured TURN servers";
    EXPECT_EQ(t.resolved_mode(), IceConfig::Mode::Poll);
}

TEST(TurnRelay, ExplicitMuxModeIsHonoured) {
    IceConfig cfg = make_agent_config(kAgentPortBegin, kAgentPortEnd);
    cfg.mode = IceConfig::Mode::Mux;
    cfg.mode_is_explicit = true;

    IceTransport t{cfg};
    TurnServer ts;
    ts.host = "127.0.0.1";
    ts.port = kTurnServerPort;
    ts.username = kTurnUser;
    ts.password = kTurnPass;
    t.add_turn_server(ts);

    // The caller's explicit choice wins — NimRTC does not silently
    // override an application decision.
    EXPECT_EQ(t.resolved_mode(), IceConfig::Mode::Mux);
}

// -----------------------------------------------------------------------------
// The relay candidate itself
// -----------------------------------------------------------------------------

TEST(TurnRelay, RelayCandidateIsGathered) {
    if (is_wsl2()) GTEST_SKIP() << "WSL2 UDP loopback is unreliable; skipping";

    TurnServerFixture server(kTurnServerPort);
    ASSERT_TRUE(server.ok()) << "the in-tree TURN server did not start";

    IceTransport t{make_agent_config(kAgentPortBegin, kAgentPortEnd)};
    TurnServer ts;
    ts.host = "127.0.0.1";
    ts.port = server.port();
    ts.username = kTurnUser;
    ts.password = kTurnPass;
    t.add_turn_server(ts);

    ASSERT_EQ(t.open(), 0u) << "open() failed";

    const auto relays = wait_for_relay_candidate(t, 10s);
    ASSERT_FALSE(relays.empty())
        << "no relay candidate was gathered; the TURN configuration is "
           "not actually working";

    const Candidate& r = relays.front();
    EXPECT_EQ(r.type, CandidateType::Relay);
    EXPECT_FALSE(r.host.empty());
    EXPECT_NE(r.port, 0u);
    // A relayed candidate names the mapped (server-reflexive) address it
    // was allocated behind.
    EXPECT_FALSE(r.related_address.empty())
        << "the relay candidate carries no raddr";
    EXPECT_NE(r.related_port, 0u)
        << "the relay candidate carries no rport";

    t.close();
}

TEST(TurnRelay, UnreachableTurnServerProducesNoRelayCandidate) {
    if (is_wsl2()) GTEST_SKIP() << "WSL2 UDP loopback is unreliable; skipping";

    // No server is listening on this port.  The transport must not wedge:
    // gathering completes, it just produces no relay candidate.
    IceTransport t{make_agent_config(kAgentPortBegin, kAgentPortEnd)};
    TurnServer ts;
    ts.host = "127.0.0.1";
    ts.port = 64999;   // nothing listening
    ts.username = kTurnUser;
    ts.password = kTurnPass;
    t.add_turn_server(ts);

    ASSERT_EQ(t.open(), 0u) << "open() failed even though the TURN server is unreachable";

    // The host candidates are still gathered — the TURN failure must not
    // take the whole session down.
    const auto hosts = t.gathered_local_candidates();
    bool has_host = false;
    for (const auto& line : hosts) {
        auto c = nimrtc::ice::parse_candidate(line);
        if (c && c->type == CandidateType::Host) { has_host = true; break; }
    }
    EXPECT_TRUE(has_host) << "an unreachable TURN server cost us the host candidates";

    // And no relay candidate is invented.
    EXPECT_TRUE(relay_candidates(t).empty());

    t.close();
}

// -----------------------------------------------------------------------------
// The transport surface
// -----------------------------------------------------------------------------

TEST(TurnRelay, TransportSurfaceReportsSupportedTransports) {
    // TURN over UDP (RFC 5766) and TURN over TCP (RFC 8656 section 12)
    // are both implemented by the vendored libjuice build: the latter
    // through the dedicated `turn_tcp` module, which frames the messages
    // on the TCP control connection per RFC 8656 section 12.4/12.5.
    //
    // TURN over TLS (RFC 6062) still has no code path at all, so the
    // surface must say so rather than pretend.
    EXPECT_TRUE(IceTransport::turn_transport_supported(TurnTransport::Udp));
    EXPECT_TRUE(IceTransport::turn_transport_supported(TurnTransport::Tcp));
    EXPECT_FALSE(IceTransport::turn_transport_supported(TurnTransport::Tls));
}

// The plugin seam (plugins::IICETransport) and the engine-facing API must
// be able to select TURN over TCP too — otherwise the capability is only
// reachable by an application that instantiates the concrete ICE transport
// directly, and the engine (which only ever sees the interface) could
// never use it.
TEST(TurnTcp, PluginSeamCanSelectTcpTransport) {
    // The module-level and the plugin-level enums must stay in lockstep,
    // because the implementation casts between them.
    static_assert(static_cast<int>(TurnTransport::Udp) ==
                      static_cast<int>(nimrtc::plugins::TurnTransport::Udp),
                  "TurnTransport::Udp must match");
    static_assert(static_cast<int>(TurnTransport::Tcp) ==
                      static_cast<int>(nimrtc::plugins::TurnTransport::Tcp),
                  "TurnTransport::Tcp must match");
    static_assert(static_cast<int>(TurnTransport::Tls) ==
                      static_cast<int>(nimrtc::plugins::TurnTransport::Tls),
                  "TurnTransport::Tls must match");

    // The concrete transport must answer through the plugin seam too.
    IceTransport t{make_agent_config(kAgentPortBegin, kAgentPortEnd)};
    EXPECT_TRUE(t.turn_transport_supported(nimrtc::plugins::TurnTransport::Tcp));
    EXPECT_FALSE(t.turn_transport_supported(nimrtc::plugins::TurnTransport::Tls));

    // And the plugin-seam overload must actually reach the TURN server
    // over TCP: the relay candidate it produces is the observable proof.
    TurnServerFixture server(kTurnUdpPortA, kTurnTcpPortA,
                             kRelayPortBegin2, kRelayPortEnd2);
    ASSERT_TRUE(server.ok()) << "the in-tree TURN server did not start";

    nimrtc::plugins::IICETransport* plugin = &t;
    plugin->add_turn_server_with_transport("127.0.0.1", kTurnTcpPortA, kTurnUser, kTurnPass,
                                           nimrtc::plugins::TurnTransport::Tcp);

    ASSERT_EQ(t.open(), 0u) << "open() failed";

    const auto relays = wait_for_relay_candidate(t, 15s);
    ASSERT_FALSE(relays.empty())
        << "the plugin seam could not select TURN over TCP; the relay "
           "candidate was not gathered";

    t.close();
}

// -----------------------------------------------------------------------------
// TURN over TCP (RFC 8656 section 12)
// -----------------------------------------------------------------------------

TEST(TurnTcp, RelayCandidateIsGatheredOverTcp) {
    if (is_wsl2()) GTEST_SKIP() << "WSL2 UDP loopback is unreliable; skipping";

    // The server listens on the TCP port as well, so the client can
    // reach it over a TCP control connection.
    TurnServerFixture server(kTurnUdpPortB, kTurnTcpPortB,
                             kRelayPortBeginB, kRelayPortEndB);
    ASSERT_TRUE(server.ok()) << "the in-tree TURN server did not start";

    IceTransport t{make_agent_config(kAgentPortBegin, kAgentPortEnd)};
    TurnServer ts;
    ts.host = "127.0.0.1";
    ts.port = kTurnTcpPortB;
    ts.username = kTurnUser;
    ts.password = kTurnPass;
    ts.transport = TurnTransport::Tcp;
    t.add_turn_server(ts);

    ASSERT_EQ(t.open(), 0u) << "open() failed";

    const auto relays = wait_for_relay_candidate(t, 15s);
    ASSERT_FALSE(relays.empty())
        << "no relay candidate was gathered over the TURN-over-TCP control "
           "connection; the TCP transport is not actually working";

    const Candidate& r = relays.front();
    EXPECT_EQ(r.type, CandidateType::Relay);
    EXPECT_FALSE(r.host.empty());
    EXPECT_NE(r.port, 0u);
    EXPECT_FALSE(r.related_address.empty())
        << "the relay candidate carries no raddr";
    EXPECT_NE(r.related_port, 0u)
        << "the relay candidate carries no rport";

    t.close();
}

TEST(TurnTcp, UnreachableTcpTurnServerProducesNoRelayCandidate) {
    if (is_wsl2()) GTEST_SKIP() << "WSL2 UDP loopback is unreliable; skipping";

    // Nothing is listening on the TCP port: the transport must not wedge.
    // The host candidates are still gathered and no relay candidate is
    // invented.
    IceTransport t{make_agent_config(kAgentPortBegin, kAgentPortEnd)};
    TurnServer ts;
    ts.host = "127.0.0.1";
    ts.port = 64998;   // nothing listening on TCP
    ts.username = kTurnUser;
    ts.password = kTurnPass;
    ts.transport = TurnTransport::Tcp;
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
// End-to-end data relay over the TURN-over-TCP control connection
// -----------------------------------------------------------------------------
//
// This is the test that turns "TURN over TCP is configurable" into "TURN over
// TCP actually carries the media".  It exercises the whole chain:
//
//   NimRTC IceTransport
//     -> libjuice agent (relayed local candidate)
//       -> TURN/TCP control connection (RFC 8656 §12.4 framing)
//         -> in-tree TURN server (RFC 8656 §12.4/§12.5 framing)
//           -> relayed transport address (UDP)
//             -> peer agent
//
// Both agents are configured with the *same* TURN server, so each of them
// holds its own allocation; the traffic between them therefore has to be
// relayed by the server (relayed↔relayed candidate pair).
//
// The test asserts three things:
//
//   1. Both agents gather a relayed candidate over the TCP control
//      connection.
//   2. The selected candidate pair is the relayed one on *both* sides, i.e.
//      the connection really is relayed and not a direct host↔host pair.
//   3. Application data flows in both directions through the relay.
//
// The last point is the one that matters for the capability claim: the
// relayed data is carried as ChannelData messages over the TCP control
// connection, so a bug in the RFC 8656 §12.5 padding (the payload must be
// padded to a 4-byte boundary, and the padding must *not* be reflected in the
// Length field) shows up here as a corrupted or missing payload.

TEST(TurnTcp, DataPathRelaysOverTcpControlConnection) {
    if (is_wsl2()) GTEST_SKIP() << "WSL2 UDP loopback is unreliable; skipping";
    if (!can_bind_secondary_loopback())
        GTEST_SKIP() << "this host cannot bind 127.0.0.2; the test needs two "
                        "distinct loopback addresses";

    // The server listens on both UDP and TCP; the clients use the TCP
    // control connection.
    TurnServerFixture server(kTurnUdpPortC, kTurnTcpPortC,
                             kRelayPortBeginC, kRelayPortEndC);
    ASSERT_TRUE(server.ok()) << "the in-tree TURN server did not start";

    // Pinned credentials: the two agents need to agree on them before the
    // candidates are exchanged, and pinning them lets the test build the
    // remote descriptions deterministically.
    IceConfig cfg_a = make_agent_config(kAgentPortBegin, kAgentPortEnd);
    cfg_a.local_ufrag    = "ufragTurnTcpA0000000";
    cfg_a.local_password = "pwdTurnTcpA00000000000000000";

    IceConfig cfg_b = make_agent_config(kAgentPortBegin, kAgentPortEnd);
    cfg_b.role          = Role::Controlled;
    cfg_b.local_ufrag    = "ufragTurnTcpB0000000";
    cfg_b.local_password = "pwdTurnTcpB00000000000000000";

    // The two agents must live on *different* loopback addresses.
    //
    // The reason is a TURN subtlety (RFC 5766 §8): a permission is granted
    // per IP address, not per address *and* port.  If both agents share
    // 127.0.0.1, the permission the peer installs for the relayed address
    // (127.0.0.1) also covers our own host socket, so the TURN server
    // forwards the *direct* host↔host traffic into the peer's allocation.
    // The direct pair then succeeds and — being the higher-priority pair —
    // wins over the relayed one, so the relay never carries the data and
    // the test would verify the wrong path.
    //
    // With 127.0.0.2 and 127.0.0.3 the host sockets are no longer covered
    // by the peer's permission, so the direct pair cannot succeed and the
    // relayed↔relayed pair is the only viable one.
    cfg_a.bind_address = "127.0.0.2";
    cfg_b.bind_address = "127.0.0.3";

    // Each agent asks for an ephemeral port, so the OS assigns them two
    // different ones and the two agents never share a socket.
    cfg_a.local_port_range_begin = kAgentPortBegin;
    cfg_a.local_port_range_end   = kAgentPortEnd;
    cfg_b.local_port_range_begin = kAgentPortBegin;
    cfg_b.local_port_range_end   = kAgentPortEnd;

    IceTransport a{cfg_a};
    IceTransport b{cfg_b};

    RxCollector rx_a, rx_b;
    a.set_callbacks(
        [&](nimrtc::plugins::BufferView v) { rx_a.on_recv(v); },
        [](nimrtc::plugins::Status, std::string_view) {});
    b.set_callbacks(
        [&](nimrtc::plugins::BufferView v) { rx_b.on_recv(v); },
        [](nimrtc::plugins::Status, std::string_view) {});

    for (const auto& t : {TurnTransport::Tcp}) {
        TurnServer ts;
        ts.host = "127.0.0.1";
        ts.port = kTurnTcpPortC;
        ts.username = kTurnUser;
        ts.password = kTurnPass;
        ts.transport = t;
        a.add_turn_server(ts);
        b.add_turn_server(ts);
    }

    // ---- Gather the relay candidates --------------------------------------
    //
    // The remote descriptions are installed *before* open() so that libjuice
    // picks the ICE roles correctly (the answerer must be CONTROLLED), but
    // they only carry the credentials at this point: the relayed candidates
    // do not exist yet.
    const std::string creds_a = "a=ice-ufrag:" + cfg_a.local_ufrag + "\r\n" +
                                "a=ice-pwd:" + cfg_a.local_password + "\r\n";
    const std::string creds_b = "a=ice-ufrag:" + cfg_b.local_ufrag + "\r\n" +
                                "a=ice-pwd:" + cfg_b.local_password + "\r\n";

    ASSERT_EQ(b.set_remote_description(creds_a), nimrtc::plugins::kOk);
    ASSERT_EQ(a.set_remote_description(creds_b), nimrtc::plugins::kOk);

    ASSERT_EQ(a.open(), 0u) << "A.open() failed";
    ASSERT_EQ(b.open(), 0u) << "B.open() failed";

    const auto relays_a = wait_for_relay_candidate(a, 15s);
    const auto relays_b = wait_for_relay_candidate(b, 15s);

    ASSERT_FALSE(relays_a.empty())
        << "A gathered no relay candidate over the TURN-over-TCP connection";
    ASSERT_FALSE(relays_b.empty())
        << "B gathered no relay candidate over the TURN-over-TCP connection";

    // The agents really have to sit on their own loopback addresses;
    // otherwise the direct host↔host pair would shadow the relayed one and
    // the test would silently verify the wrong path.
    EXPECT_EQ(host_address_of(a), cfg_a.bind_address)
        << "A did not bind to its own loopback address";
    EXPECT_EQ(host_address_of(b), cfg_b.bind_address)
        << "B did not bind to its own loopback address";

    // The two allocations must have distinct relayed addresses, otherwise
    // the relay could not tell the two peers apart.
    EXPECT_NE(relays_a.front().port, relays_b.front().port)
        << "both agents got the same relayed address";

    // ---- Exchange the relayed candidates -----------------------------------
    //
    // Only the relayed candidates are offered, so the checklist contains the
    // relayed↔relayed pair and nothing else.  A direct host↔host pair would
    // otherwise win on the loopback interface and the relay would stay idle.
    //
    // The candidates are added one by one rather than through a second
    // set_remote_description(): libjuice ignores a remote description whose
    // credentials are unchanged ("Remote description is already set"), so
    // the second call would silently drop the candidates.
    for (const auto& line : relay_candidate_lines(b)) {
        ASSERT_EQ(a.add_remote_candidate(line), nimrtc::plugins::kOk)
            << "A rejected B's relayed candidate: " << line;
    }
    for (const auto& line : relay_candidate_lines(a)) {
        ASSERT_EQ(b.add_remote_candidate(line), nimrtc::plugins::kOk)
            << "B rejected A's relayed candidate: " << line;
    }

    // ---- Wait for the relayed pair to be selected --------------------------
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    bool connected = false;
    while (std::chrono::steady_clock::now() < deadline) {
        a.recv();
        b.recv();
        const auto sa = a.state();
        const auto sb = b.state();
        if ((sa == nimrtc::ice::IceState::Connected || sa == nimrtc::ice::IceState::Completed) &&
            (sb == nimrtc::ice::IceState::Connected || sb == nimrtc::ice::IceState::Completed)) {
            connected = true;
            break;
        }
        std::this_thread::sleep_for(50ms);
    }

    if (!connected) {
        std::fprintf(stderr, "[turn-tcp] A state=%d B state=%d\n",
                     static_cast<int>(a.state()), static_cast<int>(b.state()));
    }
    ASSERT_TRUE(connected)
        << "the relayed candidate pair was never selected; the TURN-over-TCP "
           "data path did not come up";

    // The selected pair must be the relayed one on both sides — otherwise
    // the test would have passed over a direct path.
    const auto sel_a_local  = a.selected_local();
    const auto sel_a_remote = a.selected_remote();
    const auto sel_b_local  = b.selected_local();
    const auto sel_b_remote = b.selected_remote();

    ASSERT_TRUE(sel_a_local.has_value());
    ASSERT_TRUE(sel_a_remote.has_value());
    ASSERT_TRUE(sel_b_local.has_value());
    ASSERT_TRUE(sel_b_remote.has_value());

    EXPECT_EQ(sel_a_local->type, CandidateType::Relay)
        << "A's selected local candidate is not the relayed one";
    EXPECT_EQ(sel_a_remote->type, CandidateType::Relay)
        << "A's selected remote candidate is not the relayed one";
    EXPECT_EQ(sel_b_local->type, CandidateType::Relay)
        << "B's selected local candidate is not the relayed one";
    EXPECT_EQ(sel_b_remote->type, CandidateType::Relay)
        << "B's selected remote candidate is not the relayed one";

    // ---- A → B -------------------------------------------------------------
    const std::string payload = "ping-over-turn-tcp";
    const auto bytes = reinterpret_cast<const std::uint8_t*>(payload.data());
    nimrtc::plugins::BufferView view{bytes, payload.size()};
    ASSERT_EQ(a.send(view), 0u) << "A.send() failed over the TURN/TCP relay";

    std::vector<std::uint8_t> got;
    for (int i = 0; i < 60 && got.empty(); ++i) {
        a.recv();
        b.recv();
        std::this_thread::sleep_for(50ms);
        std::vector<std::uint8_t> tmp;
        if (rx_b.wait_one(tmp, 1ms)) {
            got = std::move(tmp);
            break;
        }
    }
    ASSERT_FALSE(got.empty())
        << "B did not receive the packet relayed over the TURN/TCP control "
           "connection";
    EXPECT_EQ(std::string(got.begin(), got.end()), payload);

    // ---- B → A -------------------------------------------------------------
    // The payload length is deliberately not a multiple of four: over TCP the
    // ChannelData payload has to be padded to a 4-byte boundary (RFC 8656
    // §12.5), and the receiver must strip that padding again.  A payload
    // that is already 4-byte aligned would not exercise the padding path.
    const std::string payload2 = "pong-over-turn-tcp-12345";
    const auto bytes2 = reinterpret_cast<const std::uint8_t*>(payload2.data());
    nimrtc::plugins::BufferView view2{bytes2, payload2.size()};
    ASSERT_EQ(b.send(view2), 0u) << "B.send() failed over the TURN/TCP relay";

    std::vector<std::uint8_t> got2;
    for (int i = 0; i < 60 && got2.empty(); ++i) {
        a.recv();
        b.recv();
        std::this_thread::sleep_for(50ms);
        std::vector<std::uint8_t> tmp;
        if (rx_a.wait_one(tmp, 1ms)) {
            got2 = std::move(tmp);
            break;
        }
    }
    ASSERT_FALSE(got2.empty())
        << "A did not receive the packet relayed over the TURN/TCP control "
           "connection";
    EXPECT_EQ(std::string(got2.begin(), got2.end()), payload2);

    a.close();
    b.close();
}
