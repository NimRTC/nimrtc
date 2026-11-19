// src/modules/net_sim/tests/test_net_sim_nat_relay.cpp
// ----------------------------------------------------------------------------
// The simulated NAT's unit tests.
//
// The relay is the v1.3 plan's Q5 "NAT leg": an address-translating
// relay, not a TURN server.  What has to be true of it:
//
//   * the traffic reaches the other peer;
//   * the traffic arrives with the **source address the receiving peer
//     was told to expect** -- the relay's own address, never the
//     sending peer's real one.  That is the translation, and it is
//     what makes the pair look like a NAT to both peers;
//   * neither peer can see the other's real address;
//   * the impairment profile reaches the wire (a lossy profile really
//     loses packets, a slow profile really is slow).
//
// The tests use loopback sockets, so they run on every platform.
// ----------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <nimrtc/net_sim/nat_relay.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace {

#ifdef _WIN32
using socket_t   = SOCKET;
using addr_len_t = int;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
void close_socket(socket_t s) noexcept { ::closesocket(s); }
#else
using socket_t   = int;
using addr_len_t = ::socklen_t;
constexpr socket_t kInvalidSocket = -1;
void close_socket(socket_t s) noexcept { (void)::close(s); }
#endif

/**
 * The byte-order conversions, spelled so that they compile everywhere.
 *
 * On Windows these are **functions**, so the global-scope qualifier
 * (`::htonl`) is valid.  On macOS and the BSDs they are **macros**
 * (`#define htonl(x) __DARWIN_OSSwapInt32(x)`), and a macro cannot be
 * named with a `::` in front of it -- the compiler rejects it with
 * "expected unqualified-id".  The test binds real loopback sockets on
 * every platform, so it cannot dodge the question by being Windows-only.
 *
 * So the calls go through these wrappers, which call the unqualified
 * name: a macro expands there, and on Windows the function is found in
 * the global namespace as it is.  This is the same spelling
 * `tests/bench/bench_saturation.cpp` already uses.
 */
std::uint32_t host_to_network32(std::uint32_t v) noexcept {
    return htonl(v);
}

std::uint16_t host_to_network16(std::uint16_t v) noexcept {
    return htons(v);
}

/** The port part of a "host:port" endpoint. */
std::uint16_t endpoint_port(const std::string& endpoint) {
    const std::size_t colon = endpoint.rfind(':');
    return static_cast<std::uint16_t>(std::stoi(endpoint.substr(colon + 1)));
}

/** A plain UDP socket bound to the loopback, used as a test peer. */
class TestPeer {
public:
    TestPeer() {
#ifdef _WIN32
        static bool initialised = false;
        if (!initialised) {
            WSADATA data{};
            ::WSAStartup(MAKEWORD(2, 2), &data);
            initialised = true;
        }
#endif
        fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = 0;
        // `INADDR_LOOPBACK` is an `int` literal on macOS and an
        // `unsigned int` on Linux glibc, so the cast is explicit: the
        // signedness difference is not the compiler's business to
        // guess at.
        addr.sin_addr.s_addr = host_to_network32(
            static_cast<std::uint32_t>(INADDR_LOOPBACK));
        ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

        // Non-blocking: `receive()` treats "would block" as "nothing yet".
#ifdef _WIN32
        u_long on = 1;
        ::ioctlsocket(fd_, FIONBIO, &on);
#else
        const int flags = ::fcntl(fd_, F_GETFL, 0);
        ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
#endif

        sockaddr_in bound{};
        addr_len_t len = sizeof(bound);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&bound), &len);
        port_ = ntohs(bound.sin_port);
    }

    ~TestPeer() {
        if (fd_ != kInvalidSocket) close_socket(fd_);
    }

    TestPeer(const TestPeer&) = delete;
    TestPeer& operator=(const TestPeer&) = delete;

    void send_to(const std::string& endpoint, const std::string& payload) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        const std::size_t colon = endpoint.rfind(':');
        // `sin_port` is in network byte order; `stoi` gives a host-order
        // integer, so it has to be converted -- without `htons` the
        // datagram goes to a different port than the one printed in the
        // endpoint and the relay never sees it.
        addr.sin_port = host_to_network16(static_cast<std::uint16_t>(
            std::stoi(endpoint.substr(colon + 1))));
        ::inet_pton(AF_INET, endpoint.substr(0, colon).c_str(), &addr.sin_addr);
        // The length argument is `int` on Winsock and `size_t` on the
        // BSDs.  A single spelling cannot satisfy both: the `int` cast
        // Windows needs is a narrowing conversion on the BSDs, and
        // `-Wsign-conversion` reports it.  The cast is therefore made
        // where the declaration requires it.
#ifdef _WIN32
        const int length = static_cast<int>(payload.size());
#else
        const std::size_t length = payload.size();
#endif
        const auto n = ::sendto(fd_, payload.data(), length, 0,
                                 reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (n < 0) {
            std::fprintf(stderr, "[peer] sendto failed (%ld)\n",
                         static_cast<long>(n));
        }
    }

    /** Wait for a datagram; returns "" when none arrives in time. */
    std::string receive(std::chrono::milliseconds timeout,
                        std::uint16_t* source_port = nullptr) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::vector<char> buffer(4096);
        while (std::chrono::steady_clock::now() < deadline) {
            // Non-blocking read: "would block" means nothing has arrived
            // yet.  A `select()`/`poll()`-based reader cannot be used
            // here -- Windows' `FD_SETSIZE` is 64 and a Winsock handle
            // is not a small integer, so both `FD_SET()` and
            // `WSAPoll()` silently drop the socket and the test would
            // wait forever for traffic that has in fact arrived.
            sockaddr_in src{};
            addr_len_t len = sizeof(src);
            // Same reason as in `send_to()`: the length argument is
            // `int` on Winsock and `size_t` on the BSDs.
#ifdef _WIN32
            const int length = static_cast<int>(buffer.size());
#else
            const std::size_t length = buffer.size();
#endif
            const auto n = ::recvfrom(fd_, buffer.data(), length, 0,
                                      reinterpret_cast<sockaddr*>(&src), &len);
            if (n > 0) {
                if (source_port) *source_port = ntohs(src.sin_port);
                return std::string(buffer.data(), static_cast<std::size_t>(n));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return {};
    }

    std::uint16_t port() const { return port_; }

private:
    socket_t fd_ = kInvalidSocket;
    std::uint16_t port_ = 0;
};

/** The port part of a "host:port" endpoint. */
using nimrtc::net_sim::ImpairmentConfig;
using nimrtc::net_sim::NatRelay;

// ---------------------------------------------------------------------------
// The translation
// ---------------------------------------------------------------------------

TEST(NetSimNatRelay, TheRelayAdvertisesTwoDistinctEndpoints) {
    NatRelay::Config cfg;
    NatRelay relay(cfg);

    std::string error;
    ASSERT_TRUE(relay.start(&error)) << error;
    EXPECT_FALSE(relay.endpoint_a().empty());
    EXPECT_FALSE(relay.endpoint_b().empty());
    EXPECT_NE(relay.endpoint_a(), relay.endpoint_b())
        << "the two peers must not share one relay endpoint";
    relay.stop();
}

TEST(NetSimNatRelay, TheTrafficReachesTheOtherPeer) {
    NatRelay::Config cfg;
    NatRelay relay(cfg);
    std::string error;
    ASSERT_TRUE(relay.start(&error)) << error;

    TestPeer a;
    TestPeer b;

    a.send_to(relay.endpoint_b(), "hello-from-a");
    b.send_to(relay.endpoint_a(), "hello-from-b");

    const std::string at_b = b.receive(std::chrono::milliseconds(2000));
    const std::string at_a = a.receive(std::chrono::milliseconds(2000));

    EXPECT_EQ(at_b, "hello-from-a");
    EXPECT_EQ(at_a, "hello-from-b");
    relay.stop();
}

TEST(NetSimNatRelay, TheSourceAddressIsTranslated) {
    // This is the *point* of the relay: peer B never sees peer A's
    // real address.  It sees the relay's endpoint_a() -- the address
    // the SDP told it to expect -- and that is what makes the pair
    // look like a NAT to both peers.
    NatRelay::Config cfg;
    NatRelay relay(cfg);
    std::string error;
    ASSERT_TRUE(relay.start(&error)) << error;

    TestPeer a;
    TestPeer b;

    // The relay learns each peer from the first packet that arrives on
    // its socket, so both have to knock.  A's knock is the traffic whose
    // source address this test inspects.
    b.send_to(relay.endpoint_a(), "knock-from-b");
    // Let the relay learn B before A's packet arrives, so the packet
    // that arrives at B was forwarded rather than buffered-then-flushed
    // (either is fine, but this keeps the assertion unambiguous).
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    a.send_to(relay.endpoint_b(), "knock-from-a");

    std::uint16_t source_port = 0;
    std::string at_b;
    for (int i = 0; i < 2 && at_b.empty(); ++i) {
        at_b = b.receive(std::chrono::milliseconds(1000), &source_port);
    }
    EXPECT_EQ(at_b, "knock-from-a");
    EXPECT_EQ(source_port, endpoint_port(relay.endpoint_a()))
        << "the packet must arrive with the relay's source address";
    EXPECT_NE(source_port, a.port())
        << "the sending peer's real address must not leak through";
    relay.stop();
}

TEST(NetSimNatRelay, BothDirectionsAreTranslated) {
    NatRelay::Config cfg;
    NatRelay relay(cfg);
    std::string error;
    ASSERT_TRUE(relay.start(&error)) << error;

    TestPeer a;
    TestPeer b;

    // Both peers knock: the relay can only forward once it has learned
    // both addresses.
    a.send_to(relay.endpoint_b(), "from-a");
    b.send_to(relay.endpoint_a(), "from-b");

    std::uint16_t source_port = 0;
    const std::string at_a =
        a.receive(std::chrono::milliseconds(2000), &source_port);
    EXPECT_EQ(at_a, "from-b");
    EXPECT_EQ(source_port, endpoint_port(relay.endpoint_b()));
    EXPECT_NE(source_port, b.port());
    relay.stop();
}

TEST(NetSimNatRelay, TheRelayForwardsEveryPacket) {
    NatRelay::Config cfg;
    NatRelay relay(cfg);
    std::string error;
    ASSERT_TRUE(relay.start(&error)) << error;

    TestPeer a;
    TestPeer b;

    // Knock from both sides so the relay learns the pair.
    a.send_to(relay.endpoint_b(), "knock-a");
    b.send_to(relay.endpoint_a(), "knock-b");
    ASSERT_EQ(b.receive(std::chrono::milliseconds(2000)), "knock-a");
    ASSERT_EQ(a.receive(std::chrono::milliseconds(2000)), "knock-b");

    constexpr int kPackets = 50;
    for (int i = 0; i < kPackets; ++i) {
        a.send_to(relay.endpoint_b(), "packet-" + std::to_string(i));
    }

    int received = 0;
    for (int i = 0; i < kPackets; ++i) {
        if (!b.receive(std::chrono::milliseconds(500)).empty()) ++received;
    }
    EXPECT_EQ(received, kPackets);

    const NatRelay::Stats stats = relay.stats();
    EXPECT_GE(stats.packets_forwarded, static_cast<std::uint64_t>(kPackets) + 2);
    relay.stop();
}

// ---------------------------------------------------------------------------
// The impairments reach the wire
// ---------------------------------------------------------------------------

TEST(NetSimNatRelay, TheLossProfileDropsOnTheWire) {
    NatRelay::Config cfg;
    cfg.impaired = true;
    ImpairmentConfig profile;
    profile.loss_pct = 100.0;
    cfg.impairment  = profile;

    NatRelay relay(cfg);
    std::string error;
    ASSERT_TRUE(relay.start(&error)) << error;

    TestPeer a;
    TestPeer b;
    b.send_to(relay.endpoint_a(), "knock-from-b");
    a.send_to(relay.endpoint_b(), "doomed");

    EXPECT_TRUE(b.receive(std::chrono::milliseconds(500)).empty())
        << "a packet the profile drops must not arrive";
    EXPECT_GE(relay.stats().packets_dropped, 1u);
    relay.stop();
}

TEST(NetSimNatRelay, TheDelayProfileDelaysOnTheWire) {
    NatRelay::Config cfg;
    cfg.impaired = true;
    ImpairmentConfig profile;
    profile.delay_ms = 300.0;
    cfg.impairment  = profile;

    NatRelay relay(cfg);
    std::string error;
    ASSERT_TRUE(relay.start(&error)) << error;

    TestPeer a;
    TestPeer b;
    b.send_to(relay.endpoint_a(), "knock-from-b");

    const auto start = std::chrono::steady_clock::now();
    a.send_to(relay.endpoint_b(), "late");
    const std::string received = b.receive(std::chrono::milliseconds(2000));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    EXPECT_EQ(received, "late");
    EXPECT_GE(elapsed, 250) << "the delay profile must delay the packet";
    relay.stop();
}

TEST(NetSimNatRelay, AnUnimpairedRelayChangesNothing) {
    NatRelay::Config cfg;
    cfg.impaired = false;
    ImpairmentConfig profile;
    profile.loss_pct = 100.0;   // ignored: the relay is not impaired
    cfg.impairment  = profile;

    NatRelay relay(cfg);
    std::string error;
    ASSERT_TRUE(relay.start(&error)) << error;

    TestPeer a;
    TestPeer b;
    b.send_to(relay.endpoint_a(), "knock-from-b");
    a.send_to(relay.endpoint_b(), "through");

    EXPECT_EQ(b.receive(std::chrono::milliseconds(2000)), "through");
    EXPECT_EQ(relay.stats().packets_dropped, 0u);
    relay.stop();
}

} // anonymous namespace
