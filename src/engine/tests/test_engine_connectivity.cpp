/**
 * @file src/engine/tests/test_engine_connectivity.cpp
 * @brief The v3-n4-d6 connectivity (N4 / D6) deterministic tests.
 *
 * ## What this file proves
 *
 * The slice has two engine-visible surfaces, both built from pure
 * functions so they can be pinned without a network:
 *
 *   1. **The connectivity profile (D6).**  `detail::resolve_connectivity_profile`
 *      returns the three named templates (`auto` / `conference` /
 *      `remote-desktop`), an unknown name falls back to `auto`, and
 *      `detail::resolve_stun_server` fills in the default STUN server only
 *      when the host set no explicit one — the explicit key always wins
 *      (the profile carries policy defaults, not seam values).
 *
 *   2. **The connectivity-diagnostics projection (N4).**
 *      `TransportStatsProjector::count_candidates` counts the gathered
 *      candidates by type (a TCP host candidate counts as host;
 *      unparseable lines are skipped), and
 *      `TransportStatsProjector::ice_failure_reason` is the closed
 *      six-word vocabulary in the spec's priority order — first match
 *      reports first, `transport-unsupported` beats `no-candidates`,
 *      and a healthy session is never assigned a fabricated reason.
 *
 * The engine wiring (pre-open STUN defaulting) is exercised end-to-end
 * by the slice's acceptance test against a live engine; this file pins
 * the two pure resolution functions the wiring is built from.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/engine/engine.hpp>

#include "engine_connectivity_profile.hpp"
#include "engine_transport_stats.hpp"

namespace {

namespace nd = nimrtc::engine::detail;

using nd::ConnectivityProfile;
using nd::TransportStatsProjector;

using nimrtc::engine::EngineConfig;
using nimrtc::ice::TurnTransport;

// -----------------------------------------------------------------------------
// D6 — the connectivity profile templates
// -----------------------------------------------------------------------------

TEST(EngineConnectivity, AutoProfileDefaults) {
    // "auto" is the default profile and it is *on*: STUN on, TURN/TLS when
    // the build can speak it and the host asked for it, TCP after UDP.
    const ConnectivityProfile p = nd::resolve_connectivity_profile("auto");
    EXPECT_TRUE(p.stun);
    EXPECT_EQ(p.turn_tls, "when_available");
    EXPECT_TRUE(p.tcp_fallback);
}

TEST(EngineConnectivity, ConferenceMatchesAuto) {
    // "conference" shares the auto defaults: same STUN / TURN/TLS / TCP
    // behaviour, only a different name.
    const ConnectivityProfile c = nd::resolve_connectivity_profile("conference");
    const ConnectivityProfile a = nd::resolve_connectivity_profile("auto");
    EXPECT_EQ(c.stun, a.stun);
    EXPECT_EQ(c.turn_tls, a.turn_tls);
    EXPECT_EQ(c.tcp_fallback, a.tcp_fallback);
}

TEST(EngineConnectivity, RemoteDesktopInsistsOnTurnTls) {
    // The remote-desktop control plane tolerates the extra TLS round-trip
    // far better than a blocked UDP control connection, so its profile
    // insists on TURN/TLS rather than preferring it when available.
    const ConnectivityProfile p =
        nd::resolve_connectivity_profile("remote-desktop");
    EXPECT_TRUE(p.stun);
    EXPECT_EQ(p.turn_tls, "always");
    EXPECT_TRUE(p.tcp_fallback);
}

TEST(EngineConnectivity, UnknownProfileFallsBackToAuto) {
    // The policy is never the reason a session fails (D6): an unknown
    // name resolves to the same defaults as "auto".
    const ConnectivityProfile unknown =
        nd::resolve_connectivity_profile("no-such-profile");
    const ConnectivityProfile auto_p = nd::resolve_connectivity_profile("auto");
    EXPECT_EQ(unknown.stun, auto_p.stun);
    EXPECT_EQ(unknown.turn_tls, auto_p.turn_tls);
    EXPECT_EQ(unknown.tcp_fallback, auto_p.tcp_fallback);
}

// -----------------------------------------------------------------------------
// D6 — the STUN resolution (policy defaults, not seam values)
// -----------------------------------------------------------------------------

TEST(EngineConnectivity, StunFillsDefaultWhenHostSetNothing) {
    // The host set no STUN server: the profile's default STUN server is
    // filled in (a public STUN server needs no credentials).
    //
    // The port is 3478 -- the IANA-assigned STUN port (RFC 8489 §9),
    // ratified by the amended spec's decision three ("为什么是 3478").
    // Google's STUN service also listens on 19302 (which is what the
    // in-tree examples use), but 3478 is the port every STUN
    // deployment agrees on, so it is the one a *default* carries.
    const auto s = nd::resolve_stun_server("auto", "", 0);
    EXPECT_EQ(s.host, "stun.l.google.com");
    EXPECT_EQ(s.port, 3478);
}

TEST(EngineConnectivity, ExplicitStunWinsOverTheProfile) {
    // The profile carries policy defaults, not seam values: an explicit
    // host key always wins over the profile's default.
    const auto s = nd::resolve_stun_server("auto", "stun.example.com", 3478);
    EXPECT_EQ(s.host, "stun.example.com");
    EXPECT_EQ(s.port, 3478);
}

TEST(EngineConnectivity, ExplicitStunWinsInEveryProfile) {
    // Explicit beats profile in remote-desktop too — the profile's STUN
    // bit only decides the *default*, not the seam value.
    const auto s =
        nd::resolve_stun_server("remote-desktop", "turn.example.org", 5349);
    EXPECT_EQ(s.host, "turn.example.org");
    EXPECT_EQ(s.port, 5349);
}

TEST(EngineConnectivity, ExplicitHostWithZeroPortKeepsHost) {
    // A host that sets the host but not the port still gets its host; the
    // port 0 is the host's own choice (the engine applies the pair as
    // given).
    const auto s = nd::resolve_stun_server("auto", "stun.local", 0);
    EXPECT_EQ(s.host, "stun.local");
    EXPECT_EQ(s.port, 0);
}

// -----------------------------------------------------------------------------
// N4 — the candidate-type counts
// -----------------------------------------------------------------------------

TEST(EngineConnectivity, CountCandidatesByType) {
    const std::vector<std::string> lines = {
        // One UDP host candidate.
        "a=candidate:1 1 udp 2122260223 192.168.1.10 50000 typ host",
        // One TCP host candidate — counts as a host candidate.
        "a=candidate:2 1 tcp 2122260222 192.168.1.10 9 typ host tcptype active",
        // One server-reflexive candidate.
        "a=candidate:3 1 udp 1694498815 203.0.113.5 50100 typ srflx "
        "raddr 192.168.1.10 rport 50000",
        // One relayed candidate.
        "a=candidate:4 1 udp 1694498814 203.0.113.9 50200 typ relay "
        "raddr 203.0.113.5 rport 50100",
    };

    const auto counts = TransportStatsProjector::count_candidates(lines);
    EXPECT_EQ(counts.host, 2u);   // the UDP host + the TCP host
    EXPECT_EQ(counts.srflx, 1u);
    EXPECT_EQ(counts.relay, 1u);
}

TEST(EngineConnectivity, CountCandidatesSkipsUnparseableLines) {
    // A line the module's own parser cannot name is not counted as any
    // type — the counts stay truthful to the gathered set.
    const std::vector<std::string> lines = {
        "a=candidate:1 1 udp 2122260223 192.168.1.10 50000 typ host",
        "not a candidate line at all",
        "candidate:too few fields",
        "a=candidate:5 1 udp 123 10.0.0.1 9000 typ nonsense",
    };

    const auto counts = TransportStatsProjector::count_candidates(lines);
    EXPECT_EQ(counts.host, 1u);
    EXPECT_EQ(counts.srflx, 0u);
    EXPECT_EQ(counts.relay, 0u);
}

TEST(EngineConnectivity, CountCandidatesEmptySet) {
    const auto counts = TransportStatsProjector::count_candidates({});
    EXPECT_EQ(counts.host, 0u);
    EXPECT_EQ(counts.srflx, 0u);
    EXPECT_EQ(counts.relay, 0u);
}

// -----------------------------------------------------------------------------
// N4 — the failure-reason vocabulary (closed, priority-ordered)
// -----------------------------------------------------------------------------

using FailureInputs = TransportStatsProjector::FailureInputs;

TEST(EngineConnectivity, FailureReasonNoneWhenHealthy) {
    // A session with candidates, a selected pair and no failure gets
    // "none" — never a fabricated reason.
    FailureInputs in;
    in.gathering_complete = true;
    in.counts.host = 1;
    in.pair_selected = true;
    EXPECT_EQ(TransportStatsProjector::ice_failure_reason(in), "none");
}

TEST(EngineConnectivity, FailureReasonTransportUnsupported) {
    // Highest priority: the host asked for a TURN transport this build
    // cannot speak.
    FailureInputs in;
    in.turn_transport_unsupported = true;
    EXPECT_EQ(TransportStatsProjector::ice_failure_reason(in),
              "transport-unsupported");
}

TEST(EngineConnectivity, FailureReasonTransportUnsupportedBeatsNoCandidates) {
    // Priority order, first match reports first: an unsupported transport
    // outranks a gathering that completed with no candidates.
    FailureInputs in;
    in.gathering_complete = true;
    in.counts.host = 0;
    in.turn_transport_unsupported = true;
    EXPECT_EQ(TransportStatsProjector::ice_failure_reason(in),
              "transport-unsupported");
}

TEST(EngineConnectivity, FailureReasonCredentialsRejected) {
    // The TURN server refused the credentials.
    FailureInputs in;
    in.turn_credentials_rejected = true;
    EXPECT_EQ(TransportStatsProjector::ice_failure_reason(in),
              "credentials-rejected");
}

TEST(EngineConnectivity, FailureReasonNoCandidates) {
    // Gathering completed and nothing came back — not even a host
    // candidate (no local interface matched the bind).
    FailureInputs in;
    in.gathering_complete = true;
    EXPECT_EQ(TransportStatsProjector::ice_failure_reason(in), "no-candidates");
}

TEST(EngineConnectivity, FailureReasonNoPairWhenCandidatesButNoPair) {
    // Candidates exist but the checks have not nominated a pair yet (or
    // never will).
    FailureInputs in;
    in.gathering_complete = true;
    in.counts.host = 1;
    EXPECT_EQ(TransportStatsProjector::ice_failure_reason(in), "no-pair");
}

TEST(EngineConnectivity, FailureReasonChecksFailedOutranksNoPair) {
    // Both facts are present: the agent gave up *and* there is no
    // nominated pair.  "checks-failed" wins -- "no-pair" reads as
    // "still waiting", and a session that already gave up is not
    // waiting (the ratified amendment's decision four).
    FailureInputs in;
    in.gathering_complete = true;
    in.counts.host = 1;
    in.agent_failed = true;
    EXPECT_EQ(TransportStatsProjector::ice_failure_reason(in), "checks-failed");
}

TEST(EngineConnectivity, FailureReasonChecksFailed) {
    // The agent failed (the checks ran and lost) with candidates and no
    // earlier reason matching.
    FailureInputs in;
    in.gathering_complete = true;
    in.counts.host = 1;
    in.pair_selected = true;
    in.agent_failed = true;
    EXPECT_EQ(TransportStatsProjector::ice_failure_reason(in), "checks-failed");
}

TEST(EngineConnectivity, FailureReasonNoneWhileStillGathering) {
    // Gathering has not completed and nothing is known to have failed:
    // "none", not a premature verdict.
    FailureInputs in;
    in.gathering_complete = false;
    in.counts.host = 0;
    EXPECT_EQ(TransportStatsProjector::ice_failure_reason(in), "none");
}

TEST(EngineConnectivity, FailureReasonVocabularyIsClosed) {
    // Exhaustively: over every combination of the six inputs, the output
    // is always one of the six closed-vocabulary words.
    const char* const kVocab[] = {
        "transport-unsupported", "credentials-rejected", "no-candidates",
        "no-pair", "checks-failed", "none",
    };
    for (bool unsupported : {false, true}) {
        for (bool rejected : {false, true}) {
            for (bool complete : {false, true}) {
                for (unsigned host : {0u, 1u}) {
                    for (bool pair : {false, true}) {
                        for (bool failed : {false, true}) {
                            FailureInputs in;
                            in.gathering_complete = complete;
                            in.counts.host = host;
                            in.pair_selected = pair;
                            in.agent_failed = failed;
                            in.turn_transport_unsupported = unsupported;
                            in.turn_credentials_rejected = rejected;
                            const std::string reason =
                                TransportStatsProjector::ice_failure_reason(in);
                            bool found = false;
                            for (const char* w : kVocab) {
                                if (reason == w) { found = true; break; }
                            }
                            EXPECT_TRUE(found) << "unexpected reason: " << reason;
                        }
                    }
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
// N4 — the state/transport name spellings
// -----------------------------------------------------------------------------

TEST(EngineConnectivity, TurnTransportNames) {
    EXPECT_STREQ(TransportStatsProjector::turn_transport_name(
                     TurnTransport::Udp),
                 "udp");
    EXPECT_STREQ(TransportStatsProjector::turn_transport_name(
                     TurnTransport::Tcp),
                 "tcp");
    EXPECT_STREQ(TransportStatsProjector::turn_transport_name(
                     TurnTransport::Tls),
                 "tls");
}

// -----------------------------------------------------------------------------
// D6 — the public config default
// -----------------------------------------------------------------------------

TEST(EngineConnectivity, ConnectivityProfileDefaultsToAuto) {
    // D6-1: the default profile is "auto", which is *on* (STUN on,
    // TURN/TLS when credentials exist, TCP after UDP).
    EngineConfig cfg;
    EXPECT_EQ(cfg.connectivity_profile, "auto");

    // A host may override the profile by name.
    cfg.connectivity_profile = "remote-desktop";
    EXPECT_EQ(cfg.connectivity_profile, "remote-desktop");
}

}  // namespace
