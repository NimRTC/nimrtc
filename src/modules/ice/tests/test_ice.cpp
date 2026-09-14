// modules/ice/tests/test_ice.cpp
// =============================================================================
// ICE transport smoke tests.
//
// Verifies the IceTransport / libjuice integration:
//   - factory ID / display name
//   - construction + open + close lifecycle
//   - local candidate gathering produces at least one candidate
//   - selected-pair accessors remain empty until ICE completes
//   - two-agent loopback (connectivity check + data path)
//
// The two-agent test actually drives libjuice end-to-end on the loopback
// interface (127.0.0.1) and is the strongest validation we can run without
// spinning up a STUN/TURN server.
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>

// Detect WSL2: on Linux, /proc/version contains "microsoft" when running
// under WSL2 (but not plain WSL1 or native Linux).
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
#include <thread>
#include <vector>

#include <nimrtc/ice/ice.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/transport.hpp>

namespace {

using nimrtc::ice::Candidate;
using nimrtc::ice::CandidateType;
using nimrtc::ice::IceConfig;
using nimrtc::ice::IceState;
using nimrtc::ice::IceTransport;
using nimrtc::ice::IceTransportFactory;
using nimrtc::ice::Role;
using nimrtc::ice::parse_candidate;

// -----------------------------------------------------------------------------
// Factory
// -----------------------------------------------------------------------------

TEST(IceTransportFactory, IdAndName) {
    IceTransportFactory f;
    EXPECT_EQ(f.id(), "ice");
    EXPECT_FALSE(f.display_name().empty());
}

TEST(IceTransportFactory, CreateReturnsTransport) {
    IceTransportFactory f;
    auto* raw = f.create();
    ASSERT_NE(raw, nullptr);
    // The factory returns plugins::IICETransport* (via the overridden
    // IICETransportFactory::create which forwards to create_ice()).
    // The concrete IceTransport is the runtime type, so we can downcast.
    std::unique_ptr<IceTransport> t{dynamic_cast<IceTransport*>(raw)};
    ASSERT_NE(t.get(), nullptr);
    EXPECT_NE(t->name(), nullptr);
    // open()/close() lifecycle (no network reachability assumed for gathering)
    EXPECT_EQ(t->open(), 0u);          // kOk
    EXPECT_NE(t->state(), IceState::Failed);
    t->close();
    EXPECT_EQ(t->state(), IceState::Disconnected);
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

TEST(IceTransport, OpenGathersAtLeastOneHostCandidate) {
    IceConfig cfg;
    cfg.role = Role::Controlling;
    cfg.bind_address = "127.0.0.1";   // loopback: deterministic, no firewall

    IceTransport t{cfg};
    ASSERT_EQ(t.open(), 0u);

    // Local description must contain at least one host candidate.
    const std::string sdp = t.local_description();
    EXPECT_FALSE(sdp.empty()) << "libjuice produced no SDP";

    // ufrag / pwd attributes are present.
    EXPECT_NE(sdp.find("a=ice-ufrag:"), std::string::npos);
    EXPECT_NE(sdp.find("a=ice-pwd:"),    std::string::npos);
    EXPECT_NE(sdp.find("a=candidate:"),  std::string::npos);

    t.close();
}

TEST(IceTransport, SelectedPairEmptyUntilConnected) {
    IceConfig cfg;
    cfg.bind_address = "127.0.0.1";
    IceTransport t{cfg};
    ASSERT_EQ(t.open(), 0u);

    EXPECT_FALSE(t.selected_local().has_value());
    EXPECT_FALSE(t.selected_remote().has_value());

    t.close();
}

// -----------------------------------------------------------------------------
// Candidate SDP parsing round-trip
// -----------------------------------------------------------------------------

TEST(CandidateSdp, ParseHostCandidate) {
    constexpr auto kLine =
        "candidate:1 1 UDP 2113929471 192.0.2.10 5000 typ host";
    auto parsed = parse_candidate(kLine);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->type,         CandidateType::Host);
    EXPECT_EQ(parsed->foundation,   "1");
    EXPECT_EQ(parsed->component_id, 1u);
    EXPECT_EQ(parsed->transport,    "UDP");
    EXPECT_EQ(parsed->priority,     2113929471u);
    EXPECT_EQ(parsed->host,         "192.0.2.10");
    EXPECT_EQ(parsed->port,         5000u);
}

TEST(CandidateSdp, ParseSrflxWithRelatedAddress) {
    constexpr auto kLine =
        "candidate:2 1 UDP 1677729535 1.2.3.4 5000 typ srflx raddr 10.0.0.1 rport 4000";
    auto parsed = parse_candidate(kLine);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->type,           CandidateType::Srflx);
    EXPECT_EQ(parsed->host,           "1.2.3.4");
    EXPECT_EQ(parsed->port,           5000u);
    EXPECT_EQ(parsed->related_address,"10.0.0.1");
    EXPECT_EQ(parsed->related_port,   4000u);
}

TEST(CandidateSdp, AcceptsLeadingAEquals) {
    constexpr auto kLine =
        "a=candidate:1 1 UDP 1 127.0.0.1 1234 typ host";
    auto parsed = parse_candidate(kLine);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->type, CandidateType::Host);
}

// -----------------------------------------------------------------------------
// Round-trip: parse → to_sdp → parse again
// -----------------------------------------------------------------------------

TEST(CandidateSdp, RoundTrip) {
    Candidate original;
    original.type         = CandidateType::Relay;
    original.foundation   = "abc";
    original.component_id = 1;
    original.transport    = "UDP";
    original.priority     = 12345;
    original.host         = "203.0.113.7";
    original.port         = 49152;

    const std::string sdp = original.to_sdp();
    auto parsed = parse_candidate(sdp);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->type,         CandidateType::Relay);
    EXPECT_EQ(parsed->foundation,   "abc");
    EXPECT_EQ(parsed->component_id, 1u);
    EXPECT_EQ(parsed->transport,    "UDP");
    EXPECT_EQ(parsed->priority,     12345u);
    EXPECT_EQ(parsed->host,         "203.0.113.7");
    EXPECT_EQ(parsed->port,         49152u);
}

// -----------------------------------------------------------------------------
// Reopen after close
// -----------------------------------------------------------------------------

TEST(IceTransport, ReopenAfterClose) {
    IceConfig cfg;
    cfg.bind_address = "127.0.0.1";
    IceTransport t{cfg};
    ASSERT_EQ(t.open(), 0u);
    t.close();
    EXPECT_EQ(t.open(), 0u);          // second open() should also succeed
    EXPECT_FALSE(t.local_description().empty());
    t.close();
}

// -----------------------------------------------------------------------------
// Two-agent loopback — full ICE connection + data path
// -----------------------------------------------------------------------------
//
// Drives libjuice end-to-end on 127.0.0.1: both agents gather host
// candidates, exchange SDP, run connectivity checks, nominate a pair, and
// we then push a packet through to confirm the data path is open. This is
// the smoke test that catches "compiles but doesn't actually work" failures
// in the libjuice wrapper (e.g. wrong concurrency mode, broken SDP parsing,
// callback dispatch bugs).
//
// IMPORTANT: Windows Defender may silently block the first inbound UDP on a
// fresh port. libjuice's STUN retransmits will eventually time out and the
// test will fail with Connected never reached. If that happens, allow
// NimRTC through the firewall and rerun.

namespace {

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
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::vector<std::uint8_t>> rx_;
};

void log_error(const char* who, nimrtc::plugins::Status s,
               std::string_view msg) {
    std::fprintf(stderr, "[%s err] status=%u (%s) %.*s\n",
                 who, s, nimrtc::plugins::status_string(s),
                 static_cast<int>(msg.size()), msg.data());
}

} // namespace

TEST(IceTransportLoopback, TwoAgentsConnectAndExchangeData) {
    using namespace std::chrono_literals;
    using nimrtc::ice::IceConfig;
    using nimrtc::ice::IceState;
    using nimrtc::ice::IceTransport;
    using nimrtc::ice::Role;

    // WSL2's UDP loopback is unreliable; skip on that platform.
    if (is_wsl2()) GTEST_SKIP() << "WSL2 UDP loopback is unreliable; skipping ICE test";

    // ---- Configs ------------------------------------------------------------
    //
    // Both agents bind to 127.0.0.1 with disjoint port ranges so they don't
    // collide on the OS UDP port allocator. Role is fixed (offerer = controlling)
    // because we manually pump SDP, bypassing the engine's offer/answer dance.

    IceConfig cfg_a;
    cfg_a.role = Role::Controlling;
    cfg_a.bind_address = "127.0.0.1";
    cfg_a.local_port_range_begin = 50000;
    cfg_a.local_port_range_end   = 50099;
    cfg_a.local_ufrag     = "ufragAaaaaaaaaaaaaa";   // 4..256 chars per RFC 5245
    cfg_a.local_password  = "pwdAaaaaaaaaaaaaaaaaaaa";

    IceConfig cfg_b = cfg_a;
    cfg_b.role = Role::Controlled;
    cfg_b.local_port_range_begin = 50100;
    cfg_b.local_port_range_end   = 50199;
    cfg_b.local_ufrag     = "ufragBbbbbbbbbbbbbbb";
    cfg_b.local_password  = "pwdBbbbbbbbbbbbbbbbbbb";

    // ---- Transports ---------------------------------------------------------
    IceTransport a{cfg_a};
    IceTransport b{cfg_b};

    RxCollector rx_a, rx_b;
    a.set_callbacks(
        [&](nimrtc::plugins::BufferView v) { rx_a.on_recv(v); },
        [](nimrtc::plugins::Status s, std::string_view msg) { log_error("A", s, msg); });
    b.set_callbacks(
        [&](nimrtc::plugins::BufferView v) { rx_b.on_recv(v); },
        [](nimrtc::plugins::Status s, std::string_view msg) { log_error("B", s, msg); });

    ASSERT_EQ(a.open(), 0u) << "A.open() failed";
    ASSERT_EQ(b.open(), 0u) << "B.open() failed";

    // ---- SDP exchange -------------------------------------------------------
    const std::string sdp_a = a.local_description();
    const std::string sdp_b = b.local_description();
    ASSERT_FALSE(sdp_a.empty()) << "libjuice produced empty SDP for A";
    ASSERT_FALSE(sdp_b.empty()) << "libjuice produced empty SDP for B";

    ASSERT_EQ(b.set_remote_description(sdp_a), nimrtc::plugins::kOk) << "B rejected A's SDP";
    ASSERT_EQ(a.set_remote_description(sdp_b), nimrtc::plugins::kOk) << "A rejected B's SDP";

    // ---- Wait for both sides to reach Connected -----------------------------
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    IceState sa = IceState::Disconnected, sb = IceState::Disconnected;
    while (std::chrono::steady_clock::now() < deadline) {
        // Drain rx queues so libjuice's callbacks aren't starved.
        a.recv();
        b.recv();
        sa = a.state();
        sb = b.state();
        if (sa == IceState::Connected && sb == IceState::Connected) break;
        std::this_thread::sleep_for(50ms);
    }

    // Both agents may reach Connected (3) or skip straight to Completed (4)
    // if loopback connectivity is symmetric. Both are acceptable.
    const bool a_ok = (sa == IceState::Connected || sa == IceState::Completed);
    const bool b_ok = (sb == IceState::Connected || sb == IceState::Completed);
    std::vector<std::uint8_t> got, got2;

    if (!a_ok || !b_ok) {
        // Best-effort diagnostics before failing.
        std::fprintf(stderr,
                     "[loopback] A state=%d B state=%d\n",
                     static_cast<int>(sa), static_cast<int>(sb));
        std::fprintf(stderr, "[loopback] A.sdp=%s\n", sdp_a.c_str());
        std::fprintf(stderr, "[loopback] B.sdp=%s\n", sdp_b.c_str());
    }
    EXPECT_TRUE(a_ok) << "A never reached Connected/Completed (state="
                      << static_cast<int>(sa) << ")";
    EXPECT_TRUE(b_ok) << "B never reached Connected/Completed (state="
                      << static_cast<int>(sb) << ")";

    // Skip the data-path check if connectivity failed — the EXPECTs above
    // already flagged the test as failed; we don't want to mask that with
    // a spurious send() error.
    if (!a_ok || !b_ok) {
        a.close();
        b.close();
        return;
    }

    // ---- Send A → B ---------------------------------------------------------
    const std::string payload = "ping_from_a_12345";
    const auto bytes = reinterpret_cast<const std::uint8_t*>(payload.data());
    nimrtc::plugins::BufferView view{bytes, payload.size()};
    ASSERT_EQ(a.send(view), 0u) << "A.send() failed";
    // Drain both queues to flush the recv path: libjuice dispatches the
    // inbound packet on its MUX thread; recv() moves it to the engine.
    for (int i = 0; i < 20; ++i) {
        a.recv();
        b.recv();
        std::this_thread::sleep_for(10ms);
        std::vector<std::uint8_t> tmp;
        if (rx_b.wait_one(tmp, 1ms)) {
            got = std::move(tmp);
            break;
        }
    }
    ASSERT_TRUE(!got.empty()) << "B did not receive the packet";
    EXPECT_EQ(std::string(got.begin(), got.end()), payload);

    // ---- Send B → A ---------------------------------------------------------
    const std::string payload2 = "pong_from_b_67890";
    const auto bytes2 = reinterpret_cast<const std::uint8_t*>(payload2.data());
    nimrtc::plugins::BufferView view2{bytes2, payload2.size()};
    ASSERT_EQ(b.send(view2), 0u) << "B.send() failed";
    for (int i = 0; i < 20; ++i) {
        a.recv();
        b.recv();
        std::this_thread::sleep_for(10ms);
        std::vector<std::uint8_t> tmp;
        if (rx_a.wait_one(tmp, 1ms)) {
            got2 = std::move(tmp);
            break;
        }
    }
    ASSERT_TRUE(!got2.empty()) << "A did not receive the packet";
    EXPECT_EQ(std::string(got2.begin(), got2.end()), payload2);

    // ---- Selected-pair accessors must now be populated ----------------------
    EXPECT_TRUE(a.selected_local().has_value());
    EXPECT_TRUE(a.selected_remote().has_value());
    EXPECT_TRUE(b.selected_local().has_value());
    EXPECT_TRUE(b.selected_remote().has_value());

    a.close();
    b.close();
}

// -----------------------------------------------------------------------------
// Two-agent loopback — random (libjuice-generated) ufrag/pwd
// -----------------------------------------------------------------------------
//
// Mirrors the previous test but does NOT pre-configure ufrag/pwd in IceConfig.
// This is the path the NimRTCEngine uses (EngineConfig has no ufrag field).
// If random ufrags break connectivity, we need to investigate libjuice's
// credential handling — the previous (pre-configured) test passing is not
// sufficient evidence of correctness.

TEST(IceTransportLoopback, TwoAgentsWithRandomUfrag) {
    using namespace std::chrono_literals;
    using nimrtc::ice::IceConfig;
    using nimrtc::ice::IceState;
    using nimrtc::ice::IceTransport;
    using nimrtc::ice::Role;

    // WSL2's UDP loopback is unreliable; skip on that platform.
    if (is_wsl2()) GTEST_SKIP() << "WSL2 UDP loopback is unreliable; skipping ICE test";

    IceConfig cfg_a;
    cfg_a.role = Role::Controlling;
    cfg_a.bind_address = "127.0.0.1";
    cfg_a.local_port_range_begin = 50000;
    cfg_a.local_port_range_end   = 50099;
    // local_ufrag / local_password left EMPTY → libjuice generates random.

    IceConfig cfg_b = cfg_a;
    cfg_b.role = Role::Controlled;
    cfg_b.local_port_range_begin = 50100;
    cfg_b.local_port_range_end   = 50199;

    IceTransport a{cfg_a};
    IceTransport b{cfg_b};

    ASSERT_EQ(a.open(), 0u);
    ASSERT_EQ(b.open(), 0u);

    const std::string a_ufrag = a.local_ufrag();
    const std::string b_ufrag = b.local_ufrag();
    ASSERT_FALSE(a_ufrag.empty());
    ASSERT_FALSE(b_ufrag.empty());
    ASSERT_NE(a_ufrag, b_ufrag);
    // Regression: libjuice uses \r\n line endings; local_ufrag() must
    // strip the \r (or callers comparing against parsed SDP values fail).
    ASSERT_EQ(a_ufrag.find('\r'), std::string::npos);
    ASSERT_EQ(a_ufrag.find('\n'), std::string::npos);

    RxCollector rx_a, rx_b;
    a.set_callbacks(
        [&](nimrtc::plugins::BufferView v) { rx_a.on_recv(v); },
        [](nimrtc::plugins::Status s, std::string_view msg) { log_error("A", s, msg); });
    b.set_callbacks(
        [&](nimrtc::plugins::BufferView v) { rx_b.on_recv(v); },
        [](nimrtc::plugins::Status s, std::string_view msg) { log_error("B", s, msg); });

    const std::string sdp_a = a.local_description();
    const std::string sdp_b = b.local_description();
    ASSERT_FALSE(sdp_a.empty());
    ASSERT_FALSE(sdp_b.empty());

    ASSERT_EQ(b.set_remote_description(sdp_a), nimrtc::plugins::kOk);
    ASSERT_EQ(a.set_remote_description(sdp_b), nimrtc::plugins::kOk);

    const auto deadline = std::chrono::steady_clock::now() + 10s;
    IceState sa = IceState::Disconnected, sb = IceState::Disconnected;
    while (std::chrono::steady_clock::now() < deadline) {
        a.recv();
        b.recv();
        sa = a.state();
        sb = b.state();
        if ((sa == IceState::Connected || sa == IceState::Completed) &&
            (sb == IceState::Connected || sb == IceState::Completed)) break;
        std::this_thread::sleep_for(50ms);
    }

    EXPECT_TRUE(sa == IceState::Connected || sa == IceState::Completed)
        << "A.state=" << static_cast<int>(sa);
    EXPECT_TRUE(sb == IceState::Connected || sb == IceState::Completed)
        << "B.state=" << static_cast<int>(sb);

    a.close();
    b.close();
}

}  // namespace
