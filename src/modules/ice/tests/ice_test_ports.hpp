// modules/ice/tests/ice_test_ports.hpp
// =============================================================================
// Port allocation for the ICE module's tests.
//
// WHY THIS FILE EXISTS
// --------------------
//
// `gtest_discover_tests` registers every gtest case as its own ctest test,
// and ctest runs them in parallel (`ctest -j 2`).  Each ctest test is a
// *separate process*, so two ICE test cases run side by side on the same
// machine.
//
// libjuice picks its local port with `get_next_port_in_range()`, which
// walks a *process-local* counter seeded from a random value.  Two
// processes therefore pick their ports independently, and two cases that
// share a port window can pick the same port.
//
// That is normally harmless -- but libjuice's UDP socket sets
// SO_REUSEPORT (NimRTC's own patch), so a second bind on the same port
// *succeeds* and the two sockets share it.  The kernel then hands an
// incoming datagram to only one of them.
//
// libjuice's mux thread wakes up with a 0-byte datagram sent to its own
// address (`conn_mux_interrupt` -> `udp_sendto_self`).  With two sockets
// sharing the port, the kernel may give that datagram to the *other*
// process's socket.  The interrupting process then stays in poll() for
// its full 60 s timeout, `juice_destroy()` blocks in thread_join() inside
// `conn_mux_registry_cleanup`, and the test hangs until ctest kills it.
//
// That is exactly the CI failure we saw:
//
//     conn_mux.c:287: Waiting for connections thread
//     <no further output>            <- blocked in thread_join()
//     ...
//     360 - IceRestart.RestartMintsNewCredentials (Timeout)
//
// THE TWO PROBLEMS THIS FILE HAS TO SOLVE
// ---------------------------------------
//
// (1) Two concurrently-running test processes must never be given the
//     same port.  Otherwise the SO_REUSEPORT sharing above happens and
//     the test hangs for 60 s.
//
// (2) The ports must be ones the machine will actually let us bind.
//
// (2) is the reason a plain table of hardcoded windows is not enough.
// The Windows CI runner reserves *100-port blocks* against the ephemeral
// range, and the block it reserves moves between runs.  Observed on
// 2026-11-12, all on the Windows CI runner:
//
//     127.0.0.1:[63000,63099]   errno=10013   (WSAEACCES)
//     127.0.0.1:[63200,63299]   errno=10013
//     127.0.0.1:[63600,63649]   errno=10013
//     127.0.0.1:[64200,64299]   errno=10013
//     127.0.0.1:[64500,64599]   errno=10013
//
// A hardcoded 50-port window that happens to land inside one of those
// blocks fails outright, and no fixed number can predict where the
// runner will reserve tomorrow.
//
// THE DESIGN
// ----------
//
// The band is cut into a *fixed* number of fixed-size slots, and each
// test case is assigned one slot by name.  Two properties follow:
//
//   * The slot boundaries do not depend on the machine, so two
//     concurrently-running processes compute the *same* table and pick
//     *different* slots.  Problem (1) is solved by construction, with no
//     race and no lock file.
//
//   * A slot is much wider than the blocks the runner reserves, so a
//     reserved block inside a slot leaves the rest of the slot usable.
//     libjuice walks the whole range and retries on EADDRINUSE/EACCES,
//     so a partially-reserved slot is not a problem at all.  Problem (2)
//     is solved by width rather than by guessing.
//
// A probe is still used, but only to answer one question: "is this slot
// entirely unusable?"  When it is, the test skips instead of hanging
// for 60 s and failing the run.  The probe deliberately does *not*
// narrow the range it reports -- narrowing would make the table depend
// on the machine, and two processes probing a moment apart could then
// disagree about where a slot starts and hand out overlapping ports.
// =============================================================================

#ifndef NIMRTC_ICE_TESTS_ICE_TEST_PORTS_HPP
#define NIMRTC_ICE_TESTS_ICE_TEST_PORTS_HPP

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace nimrtc::ice::test_ports {

// The band the slots are carved out of.
//
// 61000-63999 is above the Linux ephemeral range's upper end (60999) and
// above the OS dynamic-exclusion band (52297-59006) seen on
// workstations, yet still below the uint16_t ceiling.  It is also below
// the TURN relay tests' 64000-64679.
//
// The exact numbers do not matter -- the probe skips whatever the OS
// refuses -- but keeping them in one place makes the band easy to audit.
constexpr std::uint16_t kBandBegin = 61000;
constexpr std::uint16_t kBandEnd = 63999;

// Width of a single slot.
//
// This has to be comfortably wider than the 100-port blocks the Windows
// CI runner reserves, so that a reserved block inside a slot cannot take
// the whole slot out of service.  230 leaves more than half of a slot
// usable even in the worst case, and 13 slots of 230 still fit inside
// the 3000-port band.
//
// It also has to be comfortably larger than the number of sockets one
// test case opens (a couple of agents, one or two sockets each).
constexpr std::uint16_t kSlotSize = 230;

// The number of slots -- one per test case.
//
// Keep this in sync with the enum below.
constexpr std::uint16_t kSlotCount = 13;

// The windows, in the order the test cases use them.
//
// The order matters only for readability -- each entry gets its own
// slot of the band.  Do not reuse an entry for a second agent unless
// both agents live in the *same* test case (see below).
enum class Window {
    // src/modules/ice/tests/test_ice.cpp
    IceLoopbackConnectA,
    IceLoopbackConnectB,
    IceLoopbackRandomUfragA,
    IceLoopbackRandomUfragB,

    // src/modules/ice/tests/test_ice_restart.cpp
    RestartOnClosed,
    RestartMintsNewCredentials,
    RestartRegathersCandidates,
    RestartKeepsRemoteCandidates,
    RestartCloseResets,
    RestartPeerDetectedA,
    RestartPeerDetectedB,
    RestartTwoAgentsA,
    RestartTwoAgentsB,
};

// The first port of the slot that `w` owns.
//
// The arithmetic is done in std::uint32_t and narrowed at the very end:
// GCC's -Werror=conversion rejects an `int` -> `std::uint16_t` narrowing
// even when the value provably fits.
constexpr std::uint16_t window_begin(Window w) noexcept {
    const auto index = static_cast<std::uint32_t>(w);
    return static_cast<std::uint16_t>(kBandBegin + index * kSlotSize);
}

constexpr std::uint16_t window_end(Window w) noexcept {
    return static_cast<std::uint16_t>(window_begin(w) + kSlotSize - 1);
}

// The slots must stay inside the band.
static_assert(static_cast<std::uint32_t>(kBandBegin) +
                      static_cast<std::uint32_t>(kSlotCount - 1) * kSlotSize +
                      kSlotSize <=
                  static_cast<std::uint32_t>(kBandEnd) + 1u,
              "the ICE test port slots run past the end of the band");

// Make sure Winsock is up.
//
// On Windows the socket calls need Winsock to be initialised first:
// `socket()` fails with WSANOTINITIALISED until `WSAStartup` has run.
// The startup is reference-counted, and it is done exactly once here
// rather than per probe -- the probe runs over hundreds of ports.
//
// The `WSACleanup` counterpart is deliberately not called: the process
// is about to exit anyway, and an unbalanced cleanup could tear Winsock
// down while libjuice still holds sockets open.
inline void ensure_winsock_started() noexcept {
#if defined(_WIN32)
    static const bool ok = [] {
        WSADATA wsa{};
        return ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    (void)ok;
#endif
}

// Can a UDP socket be bound to `port` on the loopback interface?
//
// The socket is opened with the same options libjuice uses, so a port
// this rejects is a port libjuice would reject too.  `SO_REUSEADDR` and
// `SO_REUSEPORT` are set first precisely so that the probe does not
// report "in use" for a port that libjuice would happily share.
inline bool port_is_bindable(std::uint16_t port) noexcept {
#if defined(_WIN32)
    ensure_winsock_started();
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    const char on = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
    ::setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    const bool ok = ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::closesocket(s);
    return ok;
#else
    const int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;
    const int on = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
    ::setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    const bool ok = ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(s);
    return ok;
#endif
}

// Is the slot that `w` owns usable at all?
//
// The slot is wide enough that a reserved block inside it does not
// matter -- libjuice walks the range and retries.  This only answers
// the "is *every* port in the slot reserved" question, so that the test
// can skip instead of hanging for 60 s.
//
// The probe runs once per process (the first call memoises), so the
// answer is stable for the lifetime of the test case.
inline bool window_is_usable(Window w) noexcept {
    // The table is built once per process (the first call memoises), so the
    // answer is stable for the lifetime of the test case.
    //
    // std::array rather than a raw array: a lambda cannot return a raw
    // array, and MSVC rejects the substitution that would work around it.
    static const std::array<bool, kSlotCount> table = [] {
        std::array<bool, kSlotCount> out{};
        for (std::uint16_t i = 0; i < kSlotCount; ++i) {
            const auto slot = static_cast<Window>(static_cast<std::uint32_t>(i));
            const std::uint16_t begin = window_begin(slot);
            const std::uint16_t end = window_end(slot);
            bool any = false;
            for (std::uint32_t p = begin; p <= end; ++p) {
                if (port_is_bindable(static_cast<std::uint16_t>(p))) {
                    any = true;
                    break;
                }
            }
            out[i] = any;
        }
        return out;
    }();

    return table[static_cast<std::uint32_t>(w)];
}

// Skip the current test when its slot has no bindable port at all.
//
// This is the "the OS reserved everything in my slot" case.  It is rare,
// but when it happens the test would otherwise hang for libjuice's 60 s
// mux timeout and fail the whole run -- a skip says the truth: the
// machine could not run this test.
#define NIMRTC_ICE_TEST_SKIP_IF_SLOT_UNUSABLE(...)                                     \
    do {                                                                               \
        for (auto w : {__VA_ARGS__}) {                                                 \
            if (!nimrtc::ice::test_ports::window_is_usable(w)) {                        \
                GTEST_SKIP() << "no bindable port in this machine's slot; skipping";   \
            }                                                                          \
        }                                                                              \
    } while (false)

} // namespace nimrtc::ice::test_ports

#endif // NIMRTC_ICE_TESTS_ICE_TEST_PORTS_HPP
