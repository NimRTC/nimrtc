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
