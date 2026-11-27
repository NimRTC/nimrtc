/**
 * @file src/engine/src/engine_transport_stats.cpp
 * @brief The transport-plane statistics projection (v1.2 plan Track S / S5).
 *
 * This translation unit owns the transport statistics surface: the
 * vocabulary for the two state machines it reports on (ICE and DTLS) and
 * the projection of the three transport planes onto one `TransportStats`.
 *
 * See `engine_transport_stats.hpp` for why the projection itself holds
 * no state.
 */
#include "engine_transport_stats.hpp"

#include "engine_impl.hpp"

#include <cctype>
#include <cstring>
#include <string>

// The concrete DTLS module header, for `DtlsSession::state_name()` — the
// canonical spelling of each DTLS state.  The engine is allowed to name
// the DTLS module (it is on the L4 exception list in AGENTS.md, and the
// engine's own state-transition logger already makes this exact call).
#include <nimrtc/dtls/dtls.hpp>

namespace nimrtc::engine::detail {

const char* TransportStatsProjector::ice_state_name(
    plugins::IceState state) noexcept {
    switch (state) {
        case plugins::IceState::Disconnected: return "disconnected";
        case plugins::IceState::Gathering:    return "gathering";
        case plugins::IceState::Connecting:   return "connecting";
        case plugins::IceState::Connected:    return "connected";
        case plugins::IceState::Completed:    return "completed";
        case plugins::IceState::Failed:       return "failed";
    }
    return "unknown";
}

std::string TransportStatsProjector::dtls_state_name(
    nimrtc::dtls::DtlsState state) {
    // The DTLS module's own name is the source of truth here: it is the
    // canonical spelling of each state, and it lives with the
    // implementation that produces the states.  The statistics surface
    // only *re-spells* it, so a state the module adds later shows up
    // here without anyone editing this file.
    //
    // The concrete `DtlsSession::state_name()` is reached through the
    // module's public header — allowed by the L4 exception in AGENTS.md
    // (the DTLS module is on the engine's exemption list), and it is the
    // same call the engine's state-transition logger already makes
    // (see `engine_transport.cpp`).
    const char* camel = nimrtc::dtls::DtlsSession::state_name(state);
    if (camel == nullptr) return {};

    std::string out;
    out.reserve(std::strlen(camel) + 4);  // room for the inserted hyphens
    for (const char* p = camel; *p != '\0'; ++p) {
        const auto ch = static_cast<unsigned char>(*p);
        if (std::isupper(ch)) {
            // A capital starts a new word — except the first one, which
            // is simply the capital of the first word.  "HelloVerify"
            // becomes "hello-verify"; "Closed" becomes "closed".
            if (!out.empty()) out.push_back('-');
            out.push_back(static_cast<char>(std::tolower(ch)));
        } else {
            out.push_back(*p);
        }
    }
    return out;
}

}  // namespace nimrtc::engine::detail

// ---------------------------------------------------------------------------
// The projection
// ---------------------------------------------------------------------------

namespace nimrtc::engine {

namespace {

/** Project one end of the seam's candidate pair onto the engine's shape. */
IceCandidateInfo to_engine_candidate(const plugins::IceCandidate& c) {
    IceCandidateInfo out;
    out.address  = c.address;
    out.port     = c.port;
    out.protocol = c.protocol;
    out.type     = c.type;
    return out;
}

/** Project the seam's candidate pair onto the engine's shape.
 *
 *  Both ends are published: a diagnosis usually needs to know where the
 *  traffic leaves from *and* where it arrives, and the two ends can
 *  differ in kind (a local host candidate reaching a remote relay
 *  candidate is the shape of a TURN-relayed path). */
IceCandidatePairInfo to_engine_pair(const plugins::IceCandidatePair& p) {
    IceCandidatePairInfo out;
    out.local  = to_engine_candidate(p.local);
    out.remote = to_engine_candidate(p.remote);
    return out;
}

/** Count the gathered candidates by type.
 *
 *  The seam publishes the candidates as **SDP attribute bodies** (RFC 8839
 *  §5.1), each ending in a `typ <type>` field (RFC 8445 §5.1.1).  The
 *  engine counts them here rather than asking the seam for three
 *  counters: the candidate list is the seam's own shape, and a second
 *  set of counters would be a second answer to the same question.
 *
 *  `prflx` (peer-reflexive, RFC 8445 §5.1.1.4) is deliberately **not**
 *  counted: it is a candidate the *peer* discovered, not one this agent
 *  gathered, so it appears in no local gathering list. */
void count_candidates_by_type(const std::vector<std::string>& candidates,
                              std::uint32_t& host,
                              std::uint32_t& srflx,
                              std::uint32_t& relay) {
    for (const auto& line : candidates) {
        // The `typ` field is the last one on the line by construction
        // (RFC 8839 §5.1), so the search runs from the end.  Looking for
        // "typ " with the space keeps "type" from matching.
        const auto pos = line.rfind(" typ ");
        if (pos == std::string::npos) continue;
        const auto rest = line.substr(pos + 5);
        if (rest.starts_with("host"))      ++host;
        else if (rest.starts_with("srflx")) ++srflx;
        else if (rest.starts_with("relay")) ++relay;
    }
}

}  // namespace

void NimRTCEngine::append_transport_stats(TransportStats& out) const noexcept {
    // ---- ICE (RFC 8445) --------------------------------------------------
    //
    // The ICE transport is the authority on the agent's state and on the
    // pair it nominated.  The engine reads both through the plugin seam
    // and stores nothing: the agent's state changes on its own thread,
    // so a copy in the engine would be a second answer that could
    // disagree with the first.
    if (ice_t_ != nullptr) {
        out.ice_state = detail::TransportStatsProjector::ice_state_name(
            ice_t_->state());
        out.ice_restart_count = ice_t_->restart_count();

        // ---- The gathering action ----------------------------------------
        //
        // The collection's own state, read from the seam.  It is
        // deliberately *not* derived from `ice_state`: an agent can be
        // connected and still be gathering (RFC 8445 §5.1.1.5), and
        // conflating the two would answer a different question.
        out.ice_gathering_state = std::string(ice_t_->gathering_state());

        // The candidate counts come from the seam's own candidate list:
        // the list is the seam's shape, and three parallel counters
        // would be a second answer to the same question.
        count_candidates_by_type(ice_t_->gathered_local_candidates(),
                                 out.ice_host_candidates,
                                 out.ice_srflx_candidates,
                                 out.ice_relay_candidates);

        const auto pair = ice_t_->selected_pair();
        if (pair) {
            out.selected_pair = to_engine_pair(*pair);

            // The relay bit is *derived* from the pair's candidate types
            // (RFC 8445 §5.1.2.1): a pair whose remote end is a relay
            // candidate goes through a TURN server.  It is published as
            // its own field because it is the one bit a host actually
            // branches on — a relayed path has a different latency
            // profile and a different failure mode than a direct one.
            out.relayed = (pair->local.type == "relay") ||
                          (pair->remote.type == "relay");
        }

        // ---- Why it failed ------------------------------------------------
        //
        // The six-value vocabulary, by a fixed priority order: the first
        // match wins.  The order runs from the most *specific* cause to
        // the least, because the specific one is the one a host can act
        // on — "the TURN transport is unsupported" and "there are no
        // candidates" are both true of the same broken session, and only
        // the first is the cause.
        //
        // `checks-failed` sits **above** `no-pair` on purpose.  A session
        // whose agent gave up has candidates and no nominated pair, so
        // the two facts are both present — but "no pair" reads as
        // "still waiting for one", and a session that already gave up is
        // not waiting.  "no-pair" is the *waiting* answer, so it is the
        // least severe of the five and sits last.
        //
        // A session with nothing wrong reports "none": the engine does
        // not invent a reason, and "none" is a fact too.
        const std::string turn_failure(ice_t_->turn_failure_reason());
        // Whether the seam can speak the TURN transport the host asked
        // for is asked *here*, at the moment the stats are read: it is a
        // property of the build and the seam, not a snapshot of one
        // moment in the session's life.
        const bool turn_unsupported = turn_configured_ &&
                                      !ice_t_->turn_transport_supported(
                                          turn_transport_);
        if (turn_unsupported) {
            out.ice_failure_reason = "transport-unsupported";
        } else if (turn_failure == "credentials-rejected") {
            out.ice_failure_reason = "credentials-rejected";
        } else if (out.ice_gathering_state == "complete" &&
                   out.ice_host_candidates == 0 &&
                   out.ice_srflx_candidates == 0 &&
                   out.ice_relay_candidates == 0) {
            out.ice_failure_reason = "no-candidates";
        } else if (out.ice_state == "failed") {
            out.ice_failure_reason = "checks-failed";
        } else if (!pair && out.ice_gathering_state == "complete") {
            out.ice_failure_reason = "no-pair";
        } else {
            out.ice_failure_reason = "none";
        }

        // ---- The TURN transport in use ------------------------------------
        //
        // Read from the engine's own TURN configuration (the same list it
        // handed to the seam), not from the seam: the engine knows what
        // it asked for, and that is what this field reports.
        out.turn_transport_used = turn_transport_name_;
    }

    // ---- The round-trip time --------------------------------------------
    //
    // The RTT comes from the network-feedback collector, which derives it
    // from the peer's Receiver Reports against our own Sender Report
    // (RFC 3550 §6.4.1).  This is the *same* figure the bandwidth
    // estimator consumes — the surface republishes it rather than
    // measuring a second time (plan DS-6: no new probes).
    if (net_feedback_ != nullptr) {
        const auto snap = net_feedback_->snapshot();
        out.has_rtt = snap.has_rtt;
        out.rtt_ms  = snap.has_rtt
            ? static_cast<std::uint32_t>(snap.rtt.count() < 0 ? 0
                                                              : snap.rtt.count())
            : 0u;
    }

    // ---- DTLS (RFC 6347) -------------------------------------------------
    //
    // The DTLS session lives in the transport stack, and the engine holds
    // a weak_ptr to it (the stack owns it).  An expired handle means the
    // stack is gone, which is exactly "no DTLS session".
    auto dtls = impl_ != nullptr ? impl_->dtls.lock()
                                  : std::shared_ptr<dtls::IDtlsSession>{};
    if (dtls) {
        out.dtls_state =
            detail::TransportStatsProjector::dtls_state_name(dtls->state());
        out.dtls_fingerprint = dtls->local_fingerprint().hex_colon;

        // The role is the one the negotiation resolved.  The engine
        // resolves it from the peer's `a=setup` attribute (RFC 5763 §5)
        // and remembers it as the SDP spelling of *our* setup attribute:
        // "active" means we send the ClientHello (we are the client),
        // "passive" means we wait for it (we are the server).  Before
        // the negotiation runs the attribute is empty — and "the role is
        // not negotiated yet" is a different fact from "we are the
        // server", so the surface reports it as empty rather than
        // guessing.
        if (impl_ != nullptr) {
            const auto& setup = impl_->session_state.dtls_local_setup;
            if (setup == "active") {
                out.dtls_role = "client";
            } else if (setup == "passive") {
                out.dtls_role = "server";
            }
        }
    }

    // ---- SRTP (RFC 3711) -------------------------------------------------
    //
    // The SRTP context owns the counters; the engine holds it outright.
    // The counters are read from the context rather than kept here, so
    // the numbers on this surface and the numbers the SRTP layer
    // reports are the same numbers.
    out.srtp_installed = impl_ != nullptr && impl_->security.srtp_installed;
    if (impl_ != nullptr && impl_->srtp) {
        const auto s = impl_->srtp->total_stats();
        out.srtp_rtp_packets_protected   = s.rtp_packets_encrypted;
        out.srtp_rtp_packets_unprotected = s.rtp_packets_decrypted;
        out.srtp_rtcp_packets_protected  = s.rtcp_packets_encrypted;
        out.srtp_rtcp_packets_unprotected = s.rtcp_packets_decrypted;

        // The failures are two kinds, and the surface sums them into one
        // number because a host's question is "did anything fail to
        // authenticate?" rather than "which of the two ways did it fail
        // in?".  A replay is a legitimate-looking packet that arrived
        // twice, and a decryption failure is one that did not
        // authenticate at all; both mean the SRTP layer refused to
        // hand the payload up.
        out.srtp_auth_failures = s.decryption_failures + s.replay_attacks_dropped;
    }

    // The drop counter is the engine's own: the number of packets the
    // engine had to send but could not encrypt.  It is the same figure
    // the flat `Stats::srtp_drops` carries, repeated here so a host
    // that reads only the transport entry has the complete picture of
    // the SRTP plane.
    if (impl_ != nullptr) {
        out.srtp_drops = impl_->security.srtp_stats_drop > 0
            ? static_cast<std::uint64_t>(impl_->security.srtp_stats_drop)
            : 0u;
    }
}

}  // namespace nimrtc::engine
