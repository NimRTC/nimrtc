// modules/ice/tests/test_connectivity_diagnostics.cpp
// =============================================================================
// N4 connectivity-diagnostics tests (v1.4 plan Track N / N4).
//
// ## What this file proves
//
// The N4 statistics surface adds three module-backed facts to the engine's
// transport snapshot:
//
//   - the *gathering phase* (`ice_gathering_state`: new / gathering /
//     complete) — the state of the candidate-gathering action alone,
//     deliberately independent of the ICE agent's state machine;
//   - the transport the relayed path actually uses (`turn_transport_used`),
//     attributed by the configured TURN server order;
//   - the two TURN failure facts (`transport-unsupported` /
//     `credentials-rejected`) that libjuice exposes through no public
//     callback and therefore has to be observed by the module.
//
// Covered here:
//   - gathering_state() is New before open, Gathering while a STUN server
//     is still pending, Complete once gathering finishes
//   - an agent can be Connected while its gathering is still running (a
//     pending server-reflexive candidate) — the two states are independent
//   - turn_transport_used() reports the transport of the server a relayed
//     candidate actually came from (live UDP and live TCP allocations)
//   - a credentials rejection sets the flag (wrong password against the
//     in-tree server) and a fresh gathering round clears it
//   - the default flags are false and the diagnostics free function
//     resolves the concrete transport
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
using nimrtc::ice::GatheringState;
using nimrtc::ice::IceConfig;
using nimrtc::ice::IceTransport;
using nimrtc::ice::Role;
using nimrtc::ice::TurnServer;
using nimrtc::ice::TurnTransport;

using namespace std::chrono_literals;

// WSL2's UDP loopback is unreliable; the network tests skip there.
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

// Find a bindable UDP (or TCP, by `type`) port on the loopback interface.
//
// Windows reserves whole blocks of ports for the hypervisor (Hyper-V /
// HNS); a bind inside a reserved block fails with WSAEACCES (errno 10013).
// The kernel's ephemeral allocation never picks a reserved port, so asking
// it for a free port and reading the port back is the robust answer.
std::uint16_t find_bindable_udp_port(int type = SOCK_DGRAM) noexcept {
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
#endif
    for (int attempt = 0; attempt < 16; ++attempt) {
        const int fd = socket(AF_INET, type, 0);
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

// Find a run of consecutive bindable UDP ports, for a TURN server's
// relay-port range.
std::uint16_t find_bindable_udp_port_run(std::uint16_t count) noexcept {
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::uint16_t first = find_bindable_udp_port();
        if (first == 0) return 0;
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
            all_free = ::bind(fd, reinterpret_cast<sockaddr*>(&addr),
                              sizeof(addr)) == 0;
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

constexpr std::uint16_t kRelayPortCount = 8;

constexpr const char* kTurnUser = "nimrtc";
constexpr const char* kTurnPass = "nimrtc-turn-password";

// The client agents let the OS assign their local UDP port.
IceConfig make_agent_config() {
    IceConfig cfg;
    cfg.role = Role::Controlling;
    cfg.bind_address = "127.0.0.1";
    return cfg;
}

// -----------------------------------------------------------------------------
// An in-tree TURN server (libjuice's own server, RFC 8656).
// -----------------------------------------------------------------------------
class TurnServerFixture {
public:
    // `with_tcp` also stands up the TURN-over-TCP listener
    // (RFC 8656 section 12); otherwise the server is UDP-only.
    explicit TurnServerFixture(bool with_tcp = false)
        : udp_port_(find_bindable_udp_port(SOCK_DGRAM)),
          tcp_port_(with_tcp ? find_bindable_udp_port(SOCK_STREAM) : 0),
          relay_port_begin_(find_bindable_udp_port_run(kRelayPortCount)) {
        if (udp_port_ == 0 || relay_port_begin_ == 0) return;

        juice_server_credentials_t creds{};
        creds.username           = kTurnUser;
        creds.password           = kTurnPass;
        creds.allocations_quota  = 8;

        juice_server_config_t cfg{};
        cfg.credentials            = &creds;
        cfg.credentials_count      = 1;
        cfg.max_allocations        = 16;
        cfg.max_peers              = 8;
        cfg.bind_address           = "127.0.0.1";
        cfg.port                   = udp_port_;
        cfg.tcp_port               = tcp_port_;
        cfg.relay_port_range_begin = relay_port_begin_;
        cfg.relay_port_range_end   = static_cast<std::uint16_t>(
            relay_port_begin_ + kRelayPortCount - 1);
        cfg.realm                  = "nimrtc";

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

    std::uint16_t tcp_port() const noexcept { return tcp_port_; }

private:
    juice_server_t*   server_           = nullptr;
    std::uint16_t     udp_port_         = 0;
    std::uint16_t     tcp_port_         = 0;
    std::uint16_t     relay_port_begin_ = 0;
};

// -----------------------------------------------------------------------------
// A blackhole STUN server.
//
// A bound UDP socket that never answers: every STUN Binding request the
// agent sends lands in the socket buffer and is silently dropped.  The
// server-reflexive entry therefore stays *pending* — gathering does not
// complete — for the whole libjuice retransmission window (~23.5 s),
// which is what lets a test observe the "gathering" phase (and a
// Connected agent that is still gathering).
//
// The socket has to be bound (not merely a closed port): a bound port
// never produces an ICMP unreachable, so the agent cannot learn anything
// about the server faster than its retransmission schedule.
class BlackholeStunServer {
public:
    BlackholeStunServer() : port_(find_bindable_udp_port()) {
        if (port_ == 0) return;
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) { port_ = 0; return; }
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port_);
        addr.sin_addr.s_addr = htonl(0x7F000001);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
#ifdef _WIN32
            closesocket(fd_);
#else
            ::close(fd_);
#endif
            fd_ = -1;
            port_ = 0;
        }
    }

    ~BlackholeStunServer() {
        if (fd_ >= 0) {
#ifdef _WIN32
            closesocket(fd_);
#else
            ::close(fd_);
#endif
        }
    }

    BlackholeStunServer(const BlackholeStunServer&)            = delete;
    BlackholeStunServer& operator=(const BlackholeStunServer&) = delete;

    bool ok() const noexcept { return port_ != 0; }

    std::uint16_t port() const noexcept { return port_; }

private:
    std::uint16_t port_ = 0;
    int           fd_   = -1;
};

// Find the relay candidates among the gathered local candidates.
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

} // namespace

// -----------------------------------------------------------------------------
// The gathering phase (acceptance criterion 1)
// -----------------------------------------------------------------------------

TEST(ConnectivityDiagnostics, GatheringStateNewBeforeOpen) {
    IceTransport t{make_agent_config()};

    // No agent yet: gathering has not started.
    EXPECT_EQ(t.gathering_state(), GatheringState::New);

    const auto diag = nimrtc::ice::connectivity_diagnostics_of(t);
    ASSERT_TRUE(diag.gathering.has_value());
    EXPECT_EQ(*diag.gathering, GatheringState::New);
}

TEST(ConnectivityDiagnostics, GatheringStateGatheringWhileStunPending) {
    if (is_wsl2()) GTEST_SKIP() << "WSL2 UDP loopback is unreliable";

    BlackholeStunServer stun;
    ASSERT_TRUE(stun.ok()) << "no bindable port for the blackhole STUN server";

    IceConfig cfg = make_agent_config();
    cfg.stun_server_host = "127.0.0.1";
    cfg.stun_server_port = stun.port();

    IceTransport t{cfg};
    ASSERT_EQ(t.open(), 0u) << "open() failed";

    // The server-reflexive entry is still pending (the blackhole never
    // answers), so the gathering *action* is in progress — even though
    // the ICE agent itself has already left its Gathering state (libjuice
    // moves on once the host candidates are gathered, agent.c
    // `agent_gather_candidates`).
    EXPECT_EQ(t.gathering_state(), GatheringState::Gathering);

    t.close();
}

TEST(ConnectivityDiagnostics, GatheringStateCompleteAfterDone) {
    if (is_wsl2()) GTEST_SKIP() << "WSL2 UDP loopback is unreliable";

    // No STUN / TURN: gathering finishes as soon as the host candidates
    // are gathered (synchronously at open()).
    IceTransport t{make_agent_config()};
    ASSERT_EQ(t.open(), 0u) << "open() failed";

    ASSERT_TRUE(t.wait_for_gathering(5000)) << "gathering never finished";
    EXPECT_EQ(t.gathering_state(), GatheringState::Complete);

    // Criterion 2: the per-type counts over the gathered set sum to the
    // number of gathered candidate lines — every line the module emitted
    // is a line its own parser can name.
    const auto lines = t.gathered_local_candidates();
    std::uint32_t host = 0, srflx = 0, relay = 0;
    for (const auto& line : lines) {
        const auto c = nimrtc::ice::parse_candidate(line);
        ASSERT_TRUE(c.has_value()) << "gathered line did not parse: " << line;
        switch (c->type) {
            case nimrtc::ice::CandidateType::Host:  ++host;  break;
            case nimrtc::ice::CandidateType::Srflx: ++srflx; break;
            case nimrtc::ice::CandidateType::Relay: ++relay; break;
            default: FAIL() << "unexpected local candidate type";
        }
    }
    ASSERT_FALSE(lines.empty()) << "a completed gathering must have candidates";
    EXPECT_EQ(host + srflx + relay, static_cast<std::uint32_t>(lines.size()))
        << "the per-type counts must sum to the gathered line count";

    t.close();
}

// The independence half of criterion 1: an agent can be Connected while
// its gathering is still running.
TEST(ConnectivityDiagnostics, ConnectedWhileStillGathering) {
    if (is_wsl2()) GTEST_SKIP() << "WSL2 UDP loopback is unreliable";

    // Agent A gathers against a blackhole STUN server: its gathering stays
    // in progress for the whole retransmission window.  Agent B has no
    // STUN server.  The host↔host pair connects A and B while A's
    // gathering is still pending.
    BlackholeStunServer stun;
    ASSERT_TRUE(stun.ok()) << "no bindable port for the blackhole STUN server";

    IceConfig cfg_a = make_agent_config();
    cfg_a.stun_server_host = "127.0.0.1";
    cfg_a.stun_server_port = stun.port();
    cfg_a.local_ufrag     = "ufragAaaaaaaaaaaaaa";
    cfg_a.local_password  = "pwdAaaaaaaaaaaaaaaaaaaa";

    IceConfig cfg_b = make_agent_config();
    cfg_b.role = Role::Controlled;
    cfg_b.local_ufrag     = "ufragBbbbbbbbbbbbbbb";
    cfg_b.local_password  = "pwdBbbbbbbbbbbbbbbbbbb";

    IceTransport a{cfg_a};
    IceTransport b{cfg_b};
    a.set_callbacks({}, [](nimrtc::plugins::Status, std::string_view) {});
    b.set_callbacks({}, [](nimrtc::plugins::Status, std::string_view) {});

    ASSERT_EQ(a.open(), 0u) << "A.open() failed";
    ASSERT_EQ(b.open(), 0u) << "B.open() failed";

    const std::string sdp_a = a.local_description();
    const std::string sdp_b = b.local_description();
    ASSERT_FALSE(sdp_a.empty());
    ASSERT_FALSE(sdp_b.empty());
    ASSERT_EQ(b.set_remote_description(sdp_a), nimrtc::plugins::kOk);
    ASSERT_EQ(a.set_remote_description(sdp_b), nimrtc::plugins::kOk);

    // Wait for both sides to reach Connected/Completed.
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    auto sa = nimrtc::ice::IceState::Disconnected;
    while (std::chrono::steady_clock::now() < deadline) {
        a.recv();
        b.recv();
        sa = a.state();
        const auto sb = b.state();
        if ((sa == nimrtc::ice::IceState::Connected ||
             sa == nimrtc::ice::IceState::Completed) &&
            (sb == nimrtc::ice::IceState::Connected ||
             sb == nimrtc::ice::IceState::Completed)) {
            break;
        }
        std::this_thread::sleep_for(50ms);
    }

    const bool a_ok = (sa == nimrtc::ice::IceState::Connected ||
                       sa == nimrtc::ice::IceState::Completed);
    ASSERT_TRUE(a_ok) << "A never reached Connected (state="
                      << static_cast<int>(sa) << ")";

    // A is connected, yet its gathering is still in progress — the two
    // states are independent.
    EXPECT_EQ(a.gathering_state(), GatheringState::Gathering)
        << "a Connected agent's gathering must still be able to be running";

    a.close();
    b.close();
}

// -----------------------------------------------------------------------------
// turn_transport_used (acceptance criterion 4)
// -----------------------------------------------------------------------------

TEST(ConnectivityDiagnostics, TurnTransportUsedUdpRelay) {
    if (is_wsl2()) GTEST_SKIP() << "WSL2 UDP loopback is unreliable";

    TurnServerFixture server;
    ASSERT_TRUE(server.ok()) << "the in-tree TURN server did not start";

    IceConfig cfg = make_agent_config();
    IceTransport t{cfg};
    TurnServer ts;
    ts.host     = "127.0.0.1";
    ts.port     = server.port();
    ts.username = kTurnUser;
    ts.password = kTurnPass;
    // The default transport is Udp — the server is reached over plain
    // TURN/UDP.
    t.add_turn_server(ts);

    ASSERT_EQ(t.open(), 0u) << "open() failed";

    const auto relays = wait_for_relay_candidate(t, 15s);
    ASSERT_FALSE(relays.empty())
        << "no relay candidate over TURN/UDP — the allocation failed";

    EXPECT_EQ(t.turn_transport_used(), TurnTransport::Udp);

    // And the engine-facing free function resolves the same fact.
    const auto diag = nimrtc::ice::connectivity_diagnostics_of(t);
    ASSERT_TRUE(diag.turn_transport_used.has_value());
    EXPECT_EQ(*diag.turn_transport_used, TurnTransport::Udp);

    t.close();
}

TEST(ConnectivityDiagnostics, TurnTransportUsedTcpRelay) {
    if (is_wsl2()) GTEST_SKIP() << "WSL2 UDP loopback is unreliable";

    TurnServerFixture server(true /* with_tcp */);
    ASSERT_TRUE(server.ok()) << "the in-tree TURN server did not start";

    IceConfig cfg = make_agent_config();
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
        << "no relay candidate over TURN/TCP — the allocation failed";

    // The transport is the one the server was configured with: the
    // attribution reads the configured server's transport, so a stream
    // transport is reported as the stream transport, not as "udp".
    EXPECT_EQ(t.turn_transport_used(), TurnTransport::Tcp);

    t.close();
}

// -----------------------------------------------------------------------------
// The TURN failure facts (acceptance criterion 3's TURN half)
// -----------------------------------------------------------------------------

TEST(ConnectivityDiagnostics, CredentialsRejectedFlagLivesAndDiesWithTheRound) {
    if (is_wsl2()) GTEST_SKIP() << "WSL2 UDP loopback is unreliable";

    // A gathering round with no TURN server: the flag must be off (open()
    // clears the process-wide latch at the start of the round).
    {
        IceTransport t{make_agent_config()};
        ASSERT_EQ(t.open(), 0u) << "open() failed";
        EXPECT_FALSE(t.turn_credentials_rejected())
            << "a fresh gathering round must not report a credentials rejection";
        t.close();
    }

    // Now point at the in-tree server with the WRONG password: the server
    // answers the Allocate with 401, libjuice logs "TURN authentication
    // failed" (agent.c, `agent_process_turn_allocate`), and the module's
    // log hook captures it.
    TurnServerFixture server;
    ASSERT_TRUE(server.ok()) << "the in-tree TURN server did not start";

    IceConfig cfg = make_agent_config();
    IceTransport t{cfg};
    TurnServer ts;
    ts.host     = "127.0.0.1";
    ts.port     = server.port();
    ts.username = kTurnUser;
    ts.password = "wrong-password";
    t.add_turn_server(ts);

    ASSERT_EQ(t.open(), 0u) << "open() failed";

    const auto deadline = std::chrono::steady_clock::now() + 10s;
    bool rejected = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (t.turn_credentials_rejected()) { rejected = true; break; }
        t.recv();
        std::this_thread::sleep_for(50ms);
    }
    EXPECT_TRUE(rejected)
        << "the wrong TURN password must be reported as a credentials rejection";

    // No relay can come from a rejected allocation.
    EXPECT_TRUE(relay_candidates(t).empty());

    t.close();

    // The flag is process-wide and cleared by the next gathering round: a
    // fresh open() (no TURN server) must not inherit the rejection.
    {
        IceTransport t2{make_agent_config()};
        ASSERT_EQ(t2.open(), 0u) << "re-open failed";
        EXPECT_FALSE(t2.turn_credentials_rejected())
            << "a fresh gathering round must not inherit the previous rejection";
        t2.close();
    }
}

TEST(ConnectivityDiagnostics, UnsupportedTurnTransportFlagDefaultsOff) {
    // This build speaks every TURN transport (Udp / Tcp / Tls), so the
    // flag can never be set here — but its default must be off and the
    // diagnostics free function must resolve it.
    IceTransport t{make_agent_config()};
    EXPECT_FALSE(t.has_unsupported_turn_transport());

    const auto diag = nimrtc::ice::connectivity_diagnostics_of(t);
    EXPECT_FALSE(diag.turn_transport_unsupported);
    EXPECT_FALSE(diag.turn_credentials_rejected);
}
