// modules/ice/tests/test_ice_tcp.cpp
// =============================================================================
// ICE-over-TCP (RFC 6544) integration tests on the loopback interface.
//
// The slice's main acceptance shape: a "TCP-only firewall" — UDP made
// unusable, TCP the only path.  Concretely (ADR-028 Decision 5's "or
// disabled outright"): the two agents exchange remote SDPs that carry the
// ICE credentials but *no* UDP candidates.  libjuice's check list is then
// empty, its connectivity timer (ICE_PAC_TIMEOUT, 39.5 s) expires, and
// the agent reports Failed.  The NimRTC TCP driver takes over: gathers
// host TCP candidates (active + passive), the peers trickle them to each
// other, the connectivity checks run over the framed TCP stream, and each
// agent nominates exactly one pair.
//
// The negative control (UDP works → protocol == "udp") and N2-1 (no TCP
// candidates gathered while UDP works) are asserted in the same file, all
// loopback-only and deterministic (slice acceptance criterion 9).
// =============================================================================

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nimrtc/ice/ice.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/ice_transport.hpp>

namespace {

using nimrtc::ice::IceConfig;
using nimrtc::ice::IceState;
using nimrtc::ice::IceTransport;
using nimrtc::ice::Role;

inline bool is_wsl2() noexcept {
#if defined(__linux__)
    std::ifstream f("/proc/version");
    if (f) {
        char buf[128] = {0};
        f.read(buf, sizeof(buf) - 1);
        if (std::strstr(buf, "microsoft")) return true;
    }
#endif
    return false;
}

/** Keep only the a=ice-ufrag / a=ice-pwd lines of an SDP — the shape that
 *  makes libjuice's check list empty (the "UDP made unusable" firewall). */
std::string keep_credential_lines(const std::string& sdp) {
    std::istringstream iss{sdp};
    std::string line;
    std::string out;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("a=ice-ufrag:", 0) == 0 || line.rfind("a=ice-pwd:", 0) == 0) {
            out += line + "\r\n";
        }
    }
    return out;
}

bool wait_for_state(IceTransport& t, IceState want, int timeout_ms) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        t.recv();  // keep libjuice's callbacks flowing
        if (t.state() == want) return true;
        std::this_thread::sleep_for(50ms);
    }
    return t.state() == want;
}

bool has_tcp_candidate(const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        if (line.find(" TCP ") != std::string::npos) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Main acceptance shape: TCP-only firewall → both peers select a TCP pair
// ---------------------------------------------------------------------------

TEST(IceTcpLoopback, TcpOnlyShapeSelectsTcpPair) {
    if (is_wsl2()) GTEST_SKIP() << "WSL2 loopback is unreliable; skipping";

    IceConfig cfg_a;
    cfg_a.role = Role::Controlling;
    cfg_a.bind_address = "127.0.0.1";
    IceConfig cfg_b = cfg_a;
    cfg_b.role = Role::Controlled;

    IceTransport a{cfg_a};
    IceTransport b{cfg_b};
    ASSERT_EQ(a.open(), 0u) << "A.open() failed";
    ASSERT_EQ(b.open(), 0u) << "B.open() failed";

    // Exchange SDPs stripped of candidates: the remote check lists stay
    // empty, so libjuice's connectivity timer expires and both agents
    // report Failed (RFC 8863 §7.2.1.3).  This is the concrete "UDP made
    // unusable" shape (ADR-028 Decision 5).
    const std::string stripped_a = keep_credential_lines(a.local_description());
    const std::string stripped_b = keep_credential_lines(b.local_description());
    ASSERT_FALSE(stripped_a.empty()) << "A produced no ice-ufrag/ice-pwd";
    ASSERT_FALSE(stripped_b.empty()) << "B produced no ice-ufrag/ice-pwd";
    ASSERT_EQ(b.set_remote_description(stripped_a), nimrtc::plugins::kOk);
    ASSERT_EQ(a.set_remote_description(stripped_b), nimrtc::plugins::kOk);

    // The PAC timer is 39.5 s (libjuice agent.h); give the fallback a wide
    // margin.  Both agents must reach Failed before TCP takes over.
    ASSERT_TRUE(wait_for_state(a, IceState::Failed, 70000))
        << "A never reached Failed (state=" << static_cast<int>(a.state()) << ")";
    ASSERT_TRUE(wait_for_state(b, IceState::Failed, 70000))
        << "B never reached Failed (state=" << static_cast<int>(b.state()) << ")";

    // Criterion 1: after the fallback the local candidate surface carries a
    // TCP host candidate with a tcptype, in both local_description() and
    // gathered_local_candidates().
    const std::string sdp_a_late = a.local_description();
    const std::string sdp_b_late = b.local_description();
    EXPECT_NE(sdp_a_late.find(" TCP "), std::string::npos)
        << "A.local_description() has no TCP candidate:\n" << sdp_a_late;
    EXPECT_NE(sdp_a_late.find("tcptype "), std::string::npos);
    EXPECT_NE(sdp_b_late.find(" TCP "), std::string::npos);
    const auto gathered_a = a.gathered_local_candidates();
    const auto gathered_b = b.gathered_local_candidates();
    EXPECT_TRUE(has_tcp_candidate(gathered_a)) << "A gathered no TCP candidate";
    EXPECT_TRUE(has_tcp_candidate(gathered_b)) << "B gathered no TCP candidate";

    // Exchange the TCP candidates (trickle ICE, RFC 8838) and let the
    // drivers run their connectivity checks.
    for (const auto& line : gathered_a) {
        if (line.find(" TCP ") != std::string::npos) {
            ASSERT_EQ(b.add_remote_candidate(line), nimrtc::plugins::kOk);
        }
    }
    for (const auto& line : gathered_b) {
        if (line.find(" TCP ") != std::string::npos) {
            ASSERT_EQ(a.add_remote_candidate(line), nimrtc::plugins::kOk);
        }
    }

    // Criterion 5: both peers select a pair whose protocol reads "tcp".
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    auto pa = a.selected_pair();
    auto pb = b.selected_pair();
    while (std::chrono::steady_clock::now() < deadline) {
        a.recv();
        b.recv();
        pa = a.selected_pair();
        pb = b.selected_pair();
        if (pa && pb && pa->local.protocol == "tcp" && pb->local.protocol == "tcp") {
            break;
        }
        std::this_thread::sleep_for(50ms);
    }

    ASSERT_TRUE(pa.has_value()) << "A never selected a pair";
    ASSERT_TRUE(pb.has_value()) << "B never selected a pair";
    EXPECT_EQ(pa->local.protocol,  "tcp");
    EXPECT_EQ(pa->remote.protocol, "tcp");
    EXPECT_EQ(pb->local.protocol,  "tcp");
    EXPECT_EQ(pb->remote.protocol, "tcp");
    // The tcptype half is populated for a TCP pair (RFC 6544 §4.1).
    EXPECT_FALSE(pa->local.tcptype.empty());
    EXPECT_FALSE(pa->remote.tcptype.empty());
    // The concrete candidate accessors agree with the seam's projection.
    ASSERT_TRUE(a.selected_local().has_value());
    ASSERT_TRUE(b.selected_local().has_value());
    EXPECT_EQ(a.selected_local()->transport, "TCP");
    EXPECT_EQ(b.selected_local()->transport, "TCP");
    // The candidates are host candidates, never mistaken for another type.
    EXPECT_EQ(pa->local.type,  "host");
    EXPECT_EQ(pb->local.type,  "host");

    a.close();
    b.close();
}

// ---------------------------------------------------------------------------
// Negative control: UDP works → protocol == "udp", and N2-1: no TCP
// candidates were ever gathered (not "gathered but unselected").
// ---------------------------------------------------------------------------

TEST(IceTcpLoopback, UdpWorksSelectsUdpAndGathersNoTcpCandidates) {
    if (is_wsl2()) GTEST_SKIP() << "WSL2 loopback is unreliable; skipping";

    using namespace std::chrono_literals;

    IceConfig cfg_a;
    cfg_a.role = Role::Controlling;
    cfg_a.bind_address = "127.0.0.1";
    IceConfig cfg_b = cfg_a;
    cfg_b.role = Role::Controlled;

    IceTransport a{cfg_a};
    IceTransport b{cfg_b};
    ASSERT_EQ(a.open(), 0u);
    ASSERT_EQ(b.open(), 0u);

    // Full SDP exchange — the UDP path can and does nominate a pair.
    const std::string sdp_a = a.local_description();
    const std::string sdp_b = b.local_description();
    ASSERT_EQ(b.set_remote_description(sdp_a), nimrtc::plugins::kOk);
    ASSERT_EQ(a.set_remote_description(sdp_b), nimrtc::plugins::kOk);

    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline) {
        a.recv();
        b.recv();
        if (a.state() == IceState::Connected || a.state() == IceState::Completed) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    const auto pair = a.selected_pair();
    ASSERT_TRUE(pair.has_value()) << "UDP loopback did not nominate a pair";
    // Criterion 6: the UDP-working path selects a UDP pair, not TCP.
    EXPECT_EQ(pair->local.protocol,  "udp");
    EXPECT_EQ(pair->remote.protocol, "udp");
    EXPECT_TRUE(pair->local.tcptype.empty())
        << "a UDP pair must carry no tcptype";
    // Criterion 7 (N2-1): while UDP works, no TCP candidate is gathered —
    // the fallback did not even start.
    EXPECT_FALSE(has_tcp_candidate(a.gathered_local_candidates()))
        << "A gathered TCP candidates while UDP worked";
    EXPECT_FALSE(has_tcp_candidate(b.gathered_local_candidates()))
        << "B gathered TCP candidates while UDP worked";
    const std::string sdp = a.local_description();
    EXPECT_EQ(sdp.find("tcptype"), std::string::npos)
        << "A's SDP advertises tcptype while UDP works";

    a.close();
    b.close();
}

}  // namespace
