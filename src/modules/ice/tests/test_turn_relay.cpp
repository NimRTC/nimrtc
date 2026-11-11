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
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <juice/juice.h>

#include <nimrtc/ice/ice.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/transport.hpp>

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

// Port windows for the loopback test.  They have to be disjoint from the
// windows the other ICE tests use (63000-63099 / 63200-63299) and from
// each other, because libjuice sets SO_REUSEPORT and two sockets on the
// same port would silently share it.
//
// Rationale for the ranges: the Windows CI runner reserves 60000-60999
// and the OS dynamic-exclusion band starts at 52297, so the 64000s are
// above both while still below the uint16_t ceiling.
constexpr std::uint16_t kTurnServerPort  = 64000;
constexpr std::uint16_t kTurnTcpPort     = 64001;
constexpr std::uint16_t kRelayPortBegin  = 64100;
constexpr std::uint16_t kRelayPortEnd    = 64199;
constexpr std::uint16_t kAgentPortBegin  = 64200;
constexpr std::uint16_t kAgentPortEnd    = 64299;

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
    explicit TurnServerFixture(std::uint16_t tcp_port = 0) {
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
        cfg.port                  = kTurnServerPort;
        cfg.tcp_port              = tcp_port;
        cfg.relay_port_range_begin = kRelayPortBegin;
        cfg.relay_port_range_end   = kRelayPortEnd;
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

IceConfig make_agent_config() {
    IceConfig cfg;
    cfg.role = Role::Controlling;
    cfg.bind_address = "127.0.0.1";
    cfg.local_port_range_begin = kAgentPortBegin;
    cfg.local_port_range_end   = kAgentPortEnd;
    return cfg;
}

} // namespace

// -----------------------------------------------------------------------------
// The concurrency-mode resolution (the root cause)
// -----------------------------------------------------------------------------

TEST(TurnRelay, ConfiguredTurnServerLeavesMuxMode) {
    IceConfig cfg = make_agent_config();
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
    IceConfig cfg = make_agent_config();
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

    TurnServerFixture server;
    ASSERT_TRUE(server.ok()) << "the in-tree TURN server did not start";

    IceTransport t{make_agent_config()};
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
    IceTransport t{make_agent_config()};
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

// -----------------------------------------------------------------------------
// TURN over TCP (RFC 8656 section 12)
// -----------------------------------------------------------------------------

TEST(TurnTcp, RelayCandidateIsGatheredOverTcp) {
    if (is_wsl2()) GTEST_SKIP() << "WSL2 UDP loopback is unreliable; skipping";

    // The server listens on the TCP port as well, so the client can
    // reach it over a TCP control connection.
    TurnServerFixture server(kTurnTcpPort);
    ASSERT_TRUE(server.ok()) << "the in-tree TURN server did not start";

    IceTransport t{make_agent_config()};
    TurnServer ts;
    ts.host = "127.0.0.1";
    ts.port = kTurnTcpPort;
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
    IceTransport t{make_agent_config()};
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
