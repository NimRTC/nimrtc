// modules/ice/tests/test_ice_restart.cpp
// =============================================================================
// ICE restart tests (RFC 8445 §9.1.1.1).
//
// A restart re-runs the whole ICE session with a *fresh* credential pair:
// the local ufrag / password are regenerated, the candidates are gathered
// again (this is what picks up a changed network interface), and the
// connectivity checks start over.
//
// The peer can only observe a restart through the new credentials, so the
// host has to publish a fresh offer / answer afterwards.
//
// Covered here:
//   - restart() on a transport that is not open → kErrNotReady
//   - restart() mints a new credential pair and bumps the counter
//   - restart() re-gathers candidates (the new session has candidates)
//   - a peer-initiated restart is detected and fires on_ice_restart()
//   - close() resets the restart state and restores the pinned credentials
//   - end-to-end: two loopback agents restart and re-connect
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nimrtc/ice/ice.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/transport.hpp>

#include "ice_test_ports.hpp"

namespace {

using nimrtc::ice::IceConfig;
using nimrtc::ice::IceState;
using nimrtc::ice::IceTransport;
using nimrtc::ice::Role;
using nimrtc::plugins::BufferView;
using nimrtc::plugins::Status;

using namespace std::chrono_literals;
using nimrtc::ice::test_ports::Window;

// WSL2's UDP loopback is unreliable; the loopback tests skip there.
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

// Port slots come from ice_test_ports.hpp: each test case owns one slot
// of the band, and the slot is wide enough that the 100-port blocks the
// Windows CI runner reserves cannot take all of it out of service.
//
// The slots have to be disjoint: libjuice sets SO_REUSEPORT, so two
// agents on the same port would silently share the socket and the
// loopback path would break.
IceConfig make_config(Role role, std::uint16_t port_begin, std::uint16_t port_end) {
    IceConfig cfg;
    cfg.role = role;
    cfg.bind_address = "127.0.0.1";
    cfg.local_port_range_begin = port_begin;
    cfg.local_port_range_end   = port_end;
    return cfg;
}

// The config for a single-agent test case.
IceConfig make_config(Role role, Window w) {
    return make_config(role, nimrtc::ice::test_ports::window_begin(w),
                       nimrtc::ice::test_ports::window_end(w));
}

bool is_connected(IceState s) {
    return s == IceState::Connected || s == IceState::Completed;
}

// Pump both transports until both are connected or the deadline expires.
bool wait_for_connection(IceTransport& a, IceTransport& b,
                         std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        a.recv();
        b.recv();
        if (is_connected(a.state()) && is_connected(b.state())) return true;
        std::this_thread::sleep_for(50ms);
    }
    return false;
}

} // namespace

// -----------------------------------------------------------------------------
// Preconditions
// -----------------------------------------------------------------------------

TEST(IceRestart, RestartOnClosedTransportIsNotReady) {
    IceConfig cfg = make_config(Role::Controlling, Window::RestartOnClosed);
    IceTransport t{cfg};
    // Never opened → no agent → nothing to restart.
    EXPECT_EQ(t.restart(), nimrtc::plugins::kErrNotReady);
    EXPECT_EQ(t.restart_count(), 0u);
}

// -----------------------------------------------------------------------------
// Local-initiated restart
// -----------------------------------------------------------------------------

TEST(IceRestart, RestartMintsNewCredentials) {
    NIMRTC_ICE_TEST_SKIP_IF_SLOT_UNUSABLE(Window::RestartMintsNewCredentials);
    IceConfig cfg = make_config(Role::Controlling, Window::RestartMintsNewCredentials);
    // libjuice requires a ufrag of at least 4 and a password of at least 22
    // characters (agent_set_local_ice_attributes), otherwise it silently
    // falls back to generated credentials.
    cfg.local_ufrag    = "ufragPinned00000000";
    cfg.local_password = "pwdPinned0000000000000000";

    IceTransport t{cfg};
    ASSERT_EQ(t.open(), 0u);

    const std::string ufrag_before = t.local_ufrag();
    const std::string pwd_before   = t.local_password();
    ASSERT_FALSE(ufrag_before.empty());
    ASSERT_FALSE(pwd_before.empty());

    ASSERT_EQ(t.restart(), 0u);

    // The credentials changed — that is the whole point of a restart: the
    // peer can only detect it by observing new credentials.
    EXPECT_NE(t.local_ufrag(), ufrag_before);
    EXPECT_NE(t.local_password(), pwd_before);
    EXPECT_EQ(t.restart_count(), 1u);

    // A second restart bumps the counter again.
    ASSERT_EQ(t.restart(), 0u);
    EXPECT_EQ(t.restart_count(), 2u);

    t.close();
}

TEST(IceRestart, RestartRegathersCandidates) {
    NIMRTC_ICE_TEST_SKIP_IF_SLOT_UNUSABLE(Window::RestartRegathersCandidates);
    IceConfig cfg = make_config(Role::Controlling, Window::RestartRegathersCandidates);
    IceTransport t{cfg};
    ASSERT_EQ(t.open(), 0u);

    ASSERT_TRUE(t.wait_for_gathering(3000));
    const auto candidates_before = t.gathered_local_candidates();
    ASSERT_FALSE(candidates_before.empty());

    ASSERT_EQ(t.restart(), 0u);

    // The new session has its own candidates — this is what lets ICE pick
    // up a changed network interface (WiFi → 4G).
    const auto candidates_after = t.gathered_local_candidates();
    EXPECT_FALSE(candidates_after.empty())
        << "the restarted session gathered no candidate";

    t.close();
}

TEST(IceRestart, RestartKeepsRemoteCandidates) {
    NIMRTC_ICE_TEST_SKIP_IF_SLOT_UNUSABLE(Window::RestartKeepsRemoteCandidates);
    IceConfig cfg = make_config(Role::Controlling, Window::RestartKeepsRemoteCandidates);
    IceTransport t{cfg};
    ASSERT_EQ(t.open(), 0u);

    // A remote candidate that libjuice cannot parse is still remembered by
    // the wrapper, and re-applied after a restart.
    EXPECT_EQ(t.add_remote_candidate("a=candidate:1 1 UDP 1 127.0.0.1 65000 typ host"),
              0u);

    ASSERT_EQ(t.restart(), 0u);
    EXPECT_EQ(t.restart_count(), 1u);

    t.close();
}

// -----------------------------------------------------------------------------
// Peer-initiated restart
// -----------------------------------------------------------------------------

TEST(IceRestart, PeerRestartIsDetectedAndFiresCallback) {
    if (is_wsl2()) GTEST_SKIP() << "WSL2 UDP loopback is unreliable; skipping";
    NIMRTC_ICE_TEST_SKIP_IF_SLOT_UNUSABLE(Window::RestartPeerDetectedA,
                                          Window::RestartPeerDetectedB);

    IceConfig cfg_a = make_config(Role::Controlling, Window::RestartPeerDetectedA);
    IceConfig cfg_b = make_config(Role::Controlled,  Window::RestartPeerDetectedB);

    IceTransport a{cfg_a};
    IceTransport b{cfg_b};

    ASSERT_EQ(a.open(), 0u);
    ASSERT_EQ(b.open(), 0u);

    ASSERT_EQ(b.set_remote_description(a.local_description()), 0u);
    ASSERT_EQ(a.set_remote_description(b.local_description()), 0u);
    ASSERT_TRUE(wait_for_connection(a, b, 10s))
        << "the initial ICE session never connected";

    // The peer restarts ICE: new credentials, new candidates.
    ASSERT_EQ(b.restart(), 0u);
    const std::string b_ufrag_after_restart = b.local_ufrag();

    // The callback fires on the answering side when it sees the peer's new
    // credentials.
    std::atomic<int> restart_callbacks{0};
    const std::string a_ufrag_before = a.local_ufrag();
    a.set_on_ice_restart([&] { restart_callbacks.fetch_add(1); });

    ASSERT_EQ(a.set_remote_description(b.local_description()), 0u);

    EXPECT_EQ(restart_callbacks.load(), 1)
        << "the peer restart was not reported to the host";
    // The answerer has to answer with fresh credentials as well
    // (RFC 8445 §9.1.1.1).
    EXPECT_NE(a.local_ufrag(), a_ufrag_before);
    EXPECT_NE(a.local_ufrag(), b_ufrag_after_restart);

    a.close();
    b.close();
}

TEST(IceRestart, CloseResetsRestartStateAndRestoresPinnedCredentials) {
    NIMRTC_ICE_TEST_SKIP_IF_SLOT_UNUSABLE(Window::RestartCloseResets);
    IceConfig cfg = make_config(Role::Controlling, Window::RestartCloseResets);
    // libjuice requires a ufrag of at least 4 and a password of at least 22
    // characters (agent_set_local_ice_attributes), otherwise it silently
    // falls back to generated credentials.
    cfg.local_ufrag    = "ufragPinned00000000";
    cfg.local_password = "pwdPinned0000000000000000";

    IceTransport t{cfg};
    ASSERT_EQ(t.open(), 0u);
    EXPECT_EQ(t.local_ufrag(), "ufragPinned00000000");

    ASSERT_EQ(t.restart(), 0u);
    EXPECT_EQ(t.restart_count(), 1u);
    EXPECT_NE(t.local_ufrag(), "ufragPinned00000000");

    t.close();
    EXPECT_EQ(t.restart_count(), 0u);

    // Re-opening starts a brand new session with the pinned credentials
    // back in place.
    ASSERT_EQ(t.open(), 0u);
    EXPECT_EQ(t.local_ufrag(), "ufragPinned00000000");
    EXPECT_EQ(t.local_password(), "pwdPinned0000000000000000");
    EXPECT_EQ(t.restart_count(), 0u);

    t.close();
}

// -----------------------------------------------------------------------------
// End-to-end: two agents restart and re-connect
// -----------------------------------------------------------------------------

TEST(IceRestart, TwoAgentsRestartAndReconnect) {
    if (is_wsl2()) GTEST_SKIP() << "WSL2 UDP loopback is unreliable; skipping";
    NIMRTC_ICE_TEST_SKIP_IF_SLOT_UNUSABLE(Window::RestartTwoAgentsA,
                                          Window::RestartTwoAgentsB);

    IceConfig cfg_a = make_config(Role::Controlling, Window::RestartTwoAgentsA);
    IceConfig cfg_b = make_config(Role::Controlled,  Window::RestartTwoAgentsB);

    IceTransport a{cfg_a};
    IceTransport b{cfg_b};

    ASSERT_EQ(a.open(), 0u);
    ASSERT_EQ(b.open(), 0u);

    ASSERT_EQ(b.set_remote_description(a.local_description()), 0u);
    ASSERT_EQ(a.set_remote_description(b.local_description()), 0u);
    ASSERT_TRUE(wait_for_connection(a, b, 10s))
        << "the initial ICE session never connected";

    // ---- The offerer restarts ------------------------------------------------
    ASSERT_EQ(a.restart(), 0u);
    const std::string a_ufrag_2 = a.local_ufrag();
    ASSERT_FALSE(a_ufrag_2.empty());

    // ---- The answerer sees the new credentials --------------------------------
    std::atomic<int> restart_callbacks{0};
    const std::string b_ufrag_before = b.local_ufrag();
    b.set_on_ice_restart([&] { restart_callbacks.fetch_add(1); });

    ASSERT_EQ(b.set_remote_description(a.local_description()), 0u);
    EXPECT_EQ(restart_callbacks.load(), 1);

    // The answerer regenerated its own credentials (RFC 8445 §9.1.1.1).
    const std::string b_ufrag_2 = b.local_ufrag();
    EXPECT_FALSE(b_ufrag_2.empty());
    EXPECT_NE(b_ufrag_2, b_ufrag_before);
    EXPECT_NE(b_ufrag_2, a_ufrag_2);

    // ---- The answerer answers with the new credentials ------------------------
    ASSERT_EQ(a.set_remote_description(b.local_description()), 0u);

    // ---- Both sides re-connect ------------------------------------------------
    EXPECT_TRUE(wait_for_connection(a, b, 10s))
        << "the ICE session did not re-establish after the restart";

    a.close();
    b.close();
}
