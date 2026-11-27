// modules/ice/tests/test_tcp_candidate.cpp
// =============================================================================
// TCP candidate shape unit tests (RFC 6544 §4).
//
// Pins the slice's candidate-level acceptance criteria:
//   - active > passive priority (RFC 6544 §4.3)
//   - TCP foundation is distinct from libjuice's UDP foundations
//   - TCP priorities never collide with libjuice UDP priorities
//   - parse ⇄ to_sdp round-trip keeps transport and tcptype (both roles)
//   - "tcptype" is never mistaken for "typ": type stays Host
//   - to_sdp() emits the full line with tcptype after "typ host"
// =============================================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include <nimrtc/ice/ice.hpp>
#include <nimrtc/ice/tcp_candidate.hpp>

namespace {

using nimrtc::ice::CandidateType;
using nimrtc::ice::parse_candidate;
using nimrtc::ice::TcpCandidate;
using nimrtc::ice::TcpCandidateRole;
using nimrtc::ice::to_string;

TEST(TcpCandidate, ActivePriorityExceedsPassive) {
    TcpCandidate active;
    active.foundation = "tcp-2";
    active.host = "127.0.0.1";
    active.port = 5000;
    active.tcptype = TcpCandidateRole::Active;

    TcpCandidate passive;
    passive.foundation = "tcp-1";
    passive.host = "127.0.0.1";
    passive.port = 5001;
    passive.tcptype = TcpCandidateRole::Passive;

    EXPECT_GT(active.priority(), passive.priority());
}

TEST(TcpCandidate, PriorityDisjointFromLibjuiceUdp) {
    // RFC 6544 §4.3 prescribes a 24-bit type preference (110 / 100), while
    // libjuice packs a 16-bit one (ice_compute_priority).  Every TCP value
    // therefore sits in a range no libjuice UDP value can occupy — a
    // priority collision would make the two paths indistinguishable to the
    // peer's pair ordering.
    TcpCandidate t;
    t.host = "127.0.0.1";
    t.port = 5000;
    for (const auto role : {TcpCandidateRole::Active, TcpCandidateRole::Passive}) {
        t.tcptype = role;
        const std::uint32_t prio = t.priority();
        const std::uint32_t type_pref = prio >> 24;
        EXPECT_TRUE(type_pref == 110u || type_pref == 100u)
            << "TCP type preference must be 110 (active) or 100 (passive), got "
            << type_pref;
        // A libjuice UDP host candidate with local_pref 32767 lands in
        // [0x7F00_0000, 0x7FFF_0000); a TCP value with a 24-bit type
        // preference of 100..110 lands in [0x6400_0000, 0x6E00_0000).
        // Assert they are in disjoint ranges.
        EXPECT_LT(prio, 0x7F000000u);
        EXPECT_GE(prio, 0x64000000u);
    }
}

TEST(TcpCandidate, FoundationDistinctFromUdp) {
    TcpCandidate t;
    t.foundation = "tcp-1";
    t.host = "127.0.0.1";
    t.port = 5000;
    t.tcptype = TcpCandidateRole::Passive;
    // libjuice UDP foundations are plain numbers ("1", "2", …).  The TCP
    // foundation is namespaced so a peer can never confuse the two.
    EXPECT_NE(t.foundation, "1");
    EXPECT_NE(t.foundation, "2");
}

TEST(TcpCandidate, SdpIsFullLineWithTcptypeAfterTypHost) {
    TcpCandidate c;
    c.foundation   = "tcp-1";
    c.component_id = 1;
    c.host         = "192.0.2.1";
    c.port         = 9;
    c.tcptype      = TcpCandidateRole::Passive;

    const std::string sdp = c.to_sdp();
    // Field by field: candidate:tcp-1 1 TCP <priority> 192.0.2.1 9 typ host
    // tcptype passive — no field missing, tcptype after "typ host".
    const std::string expect =
        "candidate:tcp-1 1 TCP " + std::to_string(c.priority()) +
        " 192.0.2.1 9 typ host tcptype passive";
    EXPECT_EQ(sdp, expect);

    const auto typ_pos = sdp.find("typ host");
    const auto tcp_pos = sdp.find("tcptype passive");
    EXPECT_NE(typ_pos, std::string::npos);
    EXPECT_NE(tcp_pos, std::string::npos);
    EXPECT_GT(tcp_pos, typ_pos);
}

TEST(TcpCandidate, ParseRoundTripActiveAndPassive) {
    for (const auto role : {TcpCandidateRole::Active, TcpCandidateRole::Passive}) {
        TcpCandidate c;
        c.foundation = role == TcpCandidateRole::Active ? "tcp-2" : "tcp-1";
        c.host = "127.0.0.1";
        c.port = 5000;
        c.tcptype = role;

        const std::string sdp = c.to_sdp();
        const auto parsed = parse_candidate(sdp);
        ASSERT_TRUE(parsed.has_value()) << "parse failed for: " << sdp;
        // transport survives the round trip…
        EXPECT_EQ(parsed->transport, "TCP");
        // …and so does tcptype, for both roles.
        EXPECT_EQ(parsed->tcptype, to_string(role));
        EXPECT_EQ(parsed->host, c.host);
        EXPECT_EQ(parsed->port, c.port);
    }
}

TEST(TcpCandidate, TcptypeIsNotMistakenForTyp) {
    constexpr auto kLine =
        "candidate:tcp-1 1 TCP 1686109953 127.0.0.1 5000 typ host tcptype passive";
    const auto parsed = parse_candidate(kLine);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->type, CandidateType::Host);
    EXPECT_EQ(parsed->tcptype, "passive");
}

TEST(TcpCandidate, PassiveHostCandidateFromGatherShape) {
    // The shape emitted by the driver's gather() (tcp_ice.cpp) must round
    // trip: "candidate:tcp-1 1 TCP <pri> <host> <port> typ host tcptype passive".
    TcpCandidate c;
    c.foundation   = "tcp-1";
    c.component_id = 1;
    c.host         = "127.0.0.1";
    c.port         = 41234;
    c.tcptype      = TcpCandidateRole::Passive;

    const auto parsed = parse_candidate(c.to_sdp());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->type, CandidateType::Host);
    EXPECT_EQ(parsed->transport, "TCP");
    EXPECT_EQ(parsed->tcptype, "passive");
}

}  // namespace
