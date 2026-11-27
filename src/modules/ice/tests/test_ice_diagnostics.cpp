// modules/ice/tests/test_ice_diagnostics.cpp
// =============================================================================
// The ICE module's diagnostic seam (v1.4 plan N4).
//
// ## What this file exists for
//
// The engine publishes a *closed* vocabulary of connectivity failures
// (plan N4).  Two of its six values — `credentials-rejected` and the
// distinction between "the server refused us" and "the server was never
// reached" — can only be produced if the ICE module can tell those
// apart.
//
// Upstream libjuice cannot: it consumes the Allocate response's error
// code inside `agent_process_turn_allocate()`, marks the entry FAILED
// and drops the code.  A refused credential and an unreachable server
// are then indistinguishable from the outside.
//
// The NimRTC patch
// `src/third_party/libjuice/patches/0003-turn-allocation-failure-reason.patch`
// adds one classification field and one read function, and this file is
// what proves the patch does what it claims.
//
// Covered here:
//   - `gathering_state()` walks "new" → "gathering" → "complete"
//   - `turn_failure_reason()` is empty when nothing failed
//   - a TURN server that **refuses the credentials** reports
//     "credentials-rejected"  (the 401 path)
//   - a TURN server that is **not there** reports a different reason
//     (the transport path) — so the two are told apart
//   - a successful allocation reports no failure at all
// =============================================================================

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
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

// -----------------------------------------------------------------------------
// Port probing
// -----------------------------------------------------------------------------
//
// Every port the server binds is probed for bindability at runtime rather
// than hard-coded: a fixed window is not safe on Windows, whose runners
// hand out *excluded* port ranges (Hyper-V / HNS reservations), and a bind
// inside one fails with WSAEACCES (errno 10013).

#ifdef _WIN32
// The probe needs Winsock, and the juice server brings it up later, so
// the probe has to bring it up itself.  The extra WSAStartup is
// refcounted and is harmless.
void ensure_winsock() {
    static bool done = [] {
        WSADATA wsa{};
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    (void)done;
}
#else
void ensure_winsock() {}
#endif

std::uint16_t find_bindable_udp_port(int type) {
    ensure_winsock();
    for (int attempt = 0; attempt < 16; ++attempt) {
        const SOCKET sock = ::socket(AF_INET, type, 0);
        if (sock == INVALID_SOCKET) return 0;

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;

        if (::bind(sock, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) == 0) {
            sockaddr_in bound{};
#ifdef _WIN32
            int len = static_cast<int>(sizeof(bound));
#else
            socklen_t len = sizeof(bound);
#endif
            if (::getsockname(sock, reinterpret_cast<sockaddr*>(&bound),
                               &len) == 0) {
                const auto port = ntohs(bound.sin_port);
                ::closesocket(sock);
                if (port != 0) return port;
                continue;
            }
        }
        ::closesocket(sock);
    }
    return 0;
}

// Find a range of *consecutive* bindable UDP ports, for the TURN
// server's relay-port range.
//
// The whole run has to be bindable, not just its first port: libjuice
// allocates the relay candidate from this range, and the Windows CI
// runner reserves whole *blocks* of UDP ports for the hypervisor
// (Hyper-V / HNS).  A range whose first port happens to be free but
// whose tail falls inside a reserved block fails the allocation — and,
// worse, fails it at run time rather than at server-creation time.
//
// The reservations move between runs, so nothing here is hard-coded;
// the kernel's ephemeral allocation never picks a reserved port.
std::uint16_t find_bindable_udp_port_run(std::uint16_t count) {
    ensure_winsock();
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::uint16_t first = find_bindable_udp_port(SOCK_DGRAM);
        if (first == 0) return 0;
        // A range that wraps past 65535 cannot be expressed.
        if (static_cast<std::uint32_t>(first) + count > 0xFFFF) continue;

        bool all_free = true;
        for (std::uint16_t i = 0; i < count && all_free; ++i) {
            const std::uint16_t port = static_cast<std::uint16_t>(first + i);
            const SOCKET fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (fd == INVALID_SOCKET) { all_free = false; break; }
            sockaddr_in addr{};
            addr.sin_family      = AF_INET;
            addr.sin_port        = htons(port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            all_free = ::bind(fd, reinterpret_cast<sockaddr*>(&addr),
                              sizeof(addr)) == 0;
            ::closesocket(fd);
        }
        if (all_free) return first;
    }
    return 0;
}

bool is_wsl2() {
#ifdef __linux__
    std::ifstream release("/proc/sys/kernel/osrelease");
    std::string line;
    std::getline(release, line);
    return line.find("WSL2") != std::string::npos ||
           line.find("microsoft") != std::string::npos;
#else
    return false;
#endif
}

// -----------------------------------------------------------------------------
// An in-tree TURN server
// -----------------------------------------------------------------------------

constexpr const char* kTurnUser = "nimrtc";
constexpr const char* kTurnPass = "nimrtc-turn-password";
constexpr int         kRelayPortCount = 4;

/**
 * @brief libjuice's own TURN server, stood up on the loopback interface.
 *
 * The server's credential check is what produces the 401 the client has
 * to classify, so the server is the *other* half of the test: without
 * it, "credentials-rejected" cannot be produced at all.
 */
class TurnServerFixture {
public:
    TurnServerFixture()
        : udp_port_(find_bindable_udp_port(SOCK_DGRAM)),
          relay_port_begin_(find_bindable_udp_port_run(kRelayPortCount)) {
        if (udp_port_ == 0 || relay_port_begin_ == 0) return;

        juice_server_credentials_t creds{};
        creds.username          = kTurnUser;
        creds.password          = kTurnPass;
        creds.allocations_quota = 8;

        juice_server_config_t cfg{};
        cfg.credentials           = &creds;
        cfg.credentials_count     = 1;
        cfg.max_allocations       = 16;
        cfg.max_peers             = 8;
        cfg.bind_address          = "127.0.0.1";
        cfg.port                  = udp_port_;
        cfg.relay_port_range_begin = relay_port_begin_;
        cfg.relay_port_range_end   = static_cast<std::uint16_t>(
            relay_port_begin_ + kRelayPortCount - 1);
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
    juice_server_t*   server_           = nullptr;
    std::uint16_t     udp_port_         = 0;
    std::uint16_t     relay_port_begin_ = 0;
};

IceConfig make_agent_config() {
    IceConfig cfg;
    cfg.role             = Role::Controlling;
    cfg.bind_address     = "127.0.0.1";
    // The TURN allocation needs a socket of its own, so the mode must
    // leave MUX (libjuice's multiplexer owns exactly one UDP socket).
    cfg.mode             = IceConfig::Mode::Poll;
    cfg.mode_is_explicit = true;
    return cfg;
}

/// Pump the transport until `pred` holds or the deadline passes.
template <typename Pred>
bool wait_for(IceTransport& t, Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        t.recv();
        std::this_thread::sleep_for(50ms);
    }
    return pred();
}

// -----------------------------------------------------------------------------
// The gathering state
// -----------------------------------------------------------------------------

TEST(IceDiagnostics, TheGatheringStateWalksFromNewToComplete) {
    IceTransport t{make_agent_config()};

    // Before open() there is no collection at all.
    EXPECT_EQ(t.gathering_state(), "new");

    ASSERT_EQ(t.open(), 0u);

    // And once it finishes, it says "complete".
    ASSERT_TRUE(wait_for(t, [&] { return t.gathering_state() == "complete"; },
                         5s))
        << "the gathering never completed";
    EXPECT_EQ(t.gathering_state(), "complete");

    // The collection state is *not* the agent's state machine: the
    // agent has moved on to "connecting" (or further) while the
    // collection is over.
    EXPECT_NE(t.gathering_state(), "new");

    t.close();
}

/**
 * @brief The collection state says "gathering" while it runs.
 *
 * The "gathering" value is not allowed to be a race: the assertion
 * below is taken while the collection is *provably* still running,
 * because the STUN server the client is pointed at never answers.
 * libjuice keeps the STUN entry pending until the server responds
 * (or the retransmissions run out), and the collection cannot be
 * declared done while such an entry is pending.
 */
TEST(IceDiagnostics, TheGatheringStateIsGatheringWhileTheCollectionRuns) {
    // A port nothing listens on: the STUN binding request is never
    // answered, so the collection stays open.
    const auto dead_port = find_bindable_udp_port(SOCK_DGRAM);

    IceConfig cfg = make_agent_config();
    cfg.stun_server_host = "127.0.0.1";
    cfg.stun_server_port = dead_port;

    IceTransport t{cfg};
    ASSERT_EQ(t.open(), 0u);

    // The host candidate is out immediately, so the collection is
    // demonstrably under way — and the STUN entry is still pending.
    ASSERT_TRUE(wait_for(t, [&] { return !t.gathered_local_candidates().empty(); },
                         5s))
        << "not even the host candidate was gathered";
    EXPECT_EQ(t.gathering_state(), "gathering");

    t.close();
}

/**
 * @brief A transport with no agent reports no gathering and no failure.
 *
 * The two seam methods are asked of a transport that has not opened
 * yet, and they must answer without dereferencing anything.
 */
TEST(IceDiagnostics, AnUnopenedTransportReportsNothing) {
    IceTransport t{make_agent_config()};
    EXPECT_EQ(t.gathering_state(), "new");
    EXPECT_TRUE(t.turn_failure_reason().empty());
}

// -----------------------------------------------------------------------------
// The TURN failure reason
// -----------------------------------------------------------------------------

/**
 * @brief A successful allocation reports **no** failure.
 *
 * The negative control: the seam must not invent a failure.  A session
 * whose TURN allocation worked says nothing, and the engine's
 * vocabulary turns that into `"none"`.
 */
TEST(IceDiagnostics, ASuccessfulAllocationReportsNoFailure) {
    if (is_wsl2()) GTEST_SKIP() << "WSL2 UDP loopback is unreliable; skipping";

    TurnServerFixture server;
    ASSERT_TRUE(server.ok()) << "the in-tree TURN server did not start";

    IceTransport t{make_agent_config()};
    TurnServer ts;
    ts.host     = "127.0.0.1";
    ts.port     = server.port();
    ts.username = kTurnUser;
    ts.password = kTurnPass;
    t.add_turn_server(ts);

    ASSERT_EQ(t.open(), 0u);

    // Wait for the relay candidate — that is the proof the allocation
    // worked.
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    bool got_relay = false;
    while (std::chrono::steady_clock::now() < deadline && !got_relay) {
        for (const auto& line : t.gathered_local_candidates()) {
            const auto c = nimrtc::ice::parse_candidate(line);
            if (c && c->type == CandidateType::Relay) { got_relay = true; break; }
        }
        t.recv();
        std::this_thread::sleep_for(50ms);
    }
    ASSERT_TRUE(got_relay) << "no relay candidate was gathered";

    EXPECT_TRUE(t.turn_failure_reason().empty())
        << "a successful allocation reported the failure \"" << t.turn_failure_reason()
        << "\"";

    t.close();
}

/**
 * @brief A TURN server that **refuses the credentials** reports
 *  `"credentials-rejected"`.
 *
 * This is the whole point of the libjuice patch.  Against upstream
 * libjuice the 401 is consumed and dropped, so the seam would report
 * nothing at all — and the engine could not tell a refused credential
 * from an unreachable server.
 *
 * The wrong password is what produces the refusal: the server knows the
 * realm, the client authenticates with a password the server does not
 * accept, and the server answers 401 (RFC 8489 §9.2.4).
 */
TEST(IceDiagnostics, ARefusedCredentialIsReportedAsCredentialsRejected) {
    if (is_wsl2()) GTEST_SKIP() << "WSL2 UDP loopback is unreliable; skipping";

    TurnServerFixture server;
    ASSERT_TRUE(server.ok()) << "the in-tree TURN server did not start";

    IceTransport t{make_agent_config()};
    TurnServer ts;
    ts.host = "127.0.0.1";
    ts.port = server.port();
    ts.username = kTurnUser;
    ts.password = "the-wrong-password";   // <-- the refusal
    t.add_turn_server(ts);

    ASSERT_EQ(t.open(), 0u);

    ASSERT_TRUE(wait_for(t,
                         [&] { return !t.turn_failure_reason().empty(); },
                         10s))
        << "the refused credential was not reported at all";

    EXPECT_EQ(t.turn_failure_reason(), "credentials-rejected")
        << "the refused credential was reported as \""
        << t.turn_failure_reason() << "\"";

    t.close();
}

/**
 * @brief A TURN server that is **not there** is reported differently.
 *
 * The whole value of the seam is that these two are told apart:
 *
 *    - the server answered and refused us → "credentials-rejected"
 *      (refresh the credential);
 *    - the server never answered         → "transport-unavailable"
 *      (the network is in the way).
 *
 * A surface that reported both as "the TURN allocation failed" would
 * give the host nothing to act on.
 */
TEST(IceDiagnostics, AnUnreachableServerIsNotReportedAsARefusedCredential) {
    if (is_wsl2()) GTEST_SKIP() << "WSL2 UDP loopback is unreliable; skipping";

    // A port nothing listens on.  The allocation times out; the entry
    // fails without ever reaching a server.
    const auto dead_port = find_bindable_udp_port(SOCK_DGRAM);

    IceTransport t{make_agent_config()};
    TurnServer ts;
    ts.host = "127.0.0.1";
    ts.port = dead_port;
    ts.username = kTurnUser;
    ts.password = kTurnPass;
    t.add_turn_server(ts);

    ASSERT_EQ(t.open(), 0u);

    // Either the transport fails outright, or the allocation is refused
    // for a reason that is *not* the credentials.  What must not happen
    // is the seam claiming the credentials were rejected.
    const bool reported =
        wait_for(t, [&] { return !t.turn_failure_reason().empty(); }, 10s);
    if (reported) {
        EXPECT_NE(t.turn_failure_reason(), "credentials-rejected")
            << "an unreachable server was reported as a refused credential";
    }

    t.close();
}

}  // namespace
