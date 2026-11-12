// modules/ice/tests/ice_test_ports.hpp
// =============================================================================
// Port windows for the ICE module's tests.
//
// WHY THIS FILE EXISTS
// --------------------
//
// `gtest_discover_tests` registers every gtest case as its own ctest test,
// and ctest runs them in parallel (`ctest -j 2`).  Each ctest test is a
// *separate process*, so two ICE test cases run side by side on the same
// machine.
//
// libjuice picks the local port with `get_next_port_in_range()`, which
// walks a *process-local* counter seeded from a random value.  Two
// processes therefore pick their ports independently, and two cases that
// share a port window can pick the same port.
//
// That is normally harmless -- but libjuice's UDP socket sets
// SO_REUSEPORT (NimRTC's own patch, see commit 450dde2), so a second
// bind on the same port *succeeds* and the two sockets share it.  The
// kernel then hands an incoming datagram to only one of them.
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
// The fix is to give every concurrently-running test case its own port
// window, so two processes can never pick the same port.  The windows
// live here, in one place, so that adding a test means adding a window
// rather than picking a port at random.
//
// The windows are disjoint from each other *and* from the port ranges
// used elsewhere in the tree (the TURN relay tests use 64000-64679, the
// engine tests use 51000-52900).
// =============================================================================

#ifndef NIMRTC_ICE_TESTS_ICE_TEST_PORTS_HPP
#define NIMRTC_ICE_TESTS_ICE_TEST_PORTS_HPP

#include <cstdint>

namespace nimrtc::ice::test_ports {

// One window per concurrently-running test process.
//
// The order matters only for readability -- each entry gets its own
// slice of the band below.  Do not reuse an entry for a second agent
// unless both agents live in the *same* test case (see below).
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

// The band the windows are carved out of.
//
// 63000-63599 is above the OS dynamic-exclusion band (52297-59006) seen
// on workstations and above the 60000-60999 range the Windows CI runner
// keeps reserving, yet still below the uint16_t ceiling.  It is also
// below the TURN relay tests' 64000-64679.
constexpr std::uint16_t kBandBegin = 63000;

// Width of a single window.
//
// The width does not have to be large: the windows are disjoint, so two
// test cases can never land on the same port no matter how narrow the
// window is.  50 is a comfortable margin against an unrelated process
// that happens to grab a port in the band.
constexpr std::uint16_t kWindowSize = 50;

// The windows are laid out as
//
//     begin(w) = kBandBegin + index(w) * kWindowSize
//     end(w)   = begin(w) + kWindowSize - 1
//
// The arithmetic is done in std::uint32_t and narrowed at the very end:
// GCC's -Werror=conversion rejects an `int` -> `std::uint16_t` narrowing
// even when the value provably fits.
constexpr std::uint16_t window_begin(Window w) noexcept {
    const auto index = static_cast<std::uint32_t>(w);
    return static_cast<std::uint16_t>(kBandBegin + index * kWindowSize);
}

constexpr std::uint16_t window_end(Window w) noexcept {
    return static_cast<std::uint16_t>(window_begin(w) + kWindowSize - 1);
}

// The windows must stay inside the band and must not reach into the
// TURN relay tests' range.
static_assert(static_cast<std::uint32_t>(kBandBegin) +
                      static_cast<std::uint32_t>(Window::RestartTwoAgentsB) * kWindowSize +
                      kWindowSize <=
                  64000u,
              "the ICE test port windows run into the TURN relay tests' range");

} // namespace nimrtc::ice::test_ports

#endif // NIMRTC_ICE_TESTS_ICE_TEST_PORTS_HPP
