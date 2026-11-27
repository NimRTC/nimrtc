/**
 * @file src/engine/src/engine_transport_stats.hpp
 * @brief PRIVATE engine collaborator: the transport-plane statistics projection.
 *
 * Architecture remediation plan §4.3 (D1 — engine split): the engine's
 * internal state is split into *named collaborators*, one per concern, so
 * that a reader looking for "where does the engine's transport
 * statistics come from?" finds it in one named place rather than
 * scattered across the media and transport translation units.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * ## What this is
 *
 * The *transport plane* — the ICE → DTLS → SRTP path the session runs
 * over — has three parts, and the v1.2 plan's **S5** row asks for all
 * three on the statistics surface.
 *
 * ## Why *nothing* is stored here
 *
 * This collaborator holds **no state at all**.  That is deliberate, and
 * it is the whole point:
 *
 *   - The ICE state and the selected candidate pair are owned by the ICE
 *     transport (the plugin), and the engine reads them through the
 *     `plugins::IICETransport` seam.  Copying them into the engine
 *     would create a second authority that could disagree with the
 *     first — and the ICE agent's state *changes on its own thread*, so
 *     a copy would go stale.
 *   - The DTLS state is owned by the DTLS session (a `weak_ptr` handle
 *     the engine holds).
 *   - The SRTP counters are owned by the SRTP context.
 *   - The round-trip time is owned by the network-feedback collector —
 *     and the engine already reads it there to feed the bandwidth
 *     estimator, so the statistics surface reads the *same* figure
 *     rather than measuring a second one.
 *
 * So the "state" here is only the *shape* of the projection: which
 * source answers which field.  Keeping it in one named place is what
 * makes the projection reviewable — a reader sees the whole transport
 * surface at once, and sees that every number has exactly one owner.
 *
 * ## Why the RTT is not measured twice
 *
 * The plan's DS-6 row says the RTT is "取自引擎已有的 DTLS / SRTP /
 * RTCP 反馈面，**不新增探针**" — taken from the feedback surface the
 * engine already has, with **no new probe**.  The engine already
 * consumes the peer's Receiver Reports through the network-feedback
 * collector, and the collector already derives the RTT from the LSR /
 * DLSR pair against our own Sender Report (RFC 3550 §6.4.1).  The
 * bandwidth estimator consumes that figure.  This surface republishes
 * it; a second measurement would be a second answer to the same
 * question, and the two would disagree.
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/dtls/dtls_types.hpp>
#include <nimrtc/ice/ice.hpp>
#include <nimrtc/plugins/ice_transport.hpp>

namespace nimrtc::engine::detail {

/** The transport-plane statistics vocabulary (v1.2 plan Track S / S5).
 *
 *  A stateless helper: each method maps one owner's enumeration onto
 *  the lower-cased string the public surface publishes. */
struct TransportStatsProjector {
    /** The ICE agent's state, lower-cased: "disconnected", "gathering",
     *  "connecting", "connected", "completed" or "failed".
     *
     *  The same vocabulary as `NimRTCEngine::ice_state_string()`, so a
     *  host that reads both sees the same word.  The one difference is
     *  the *absent* transport: `ice_state_string()` answers "closed"
     *  there, while the statistics surface answers the empty string —
     *  "we have no transport" and "the transport is in the state called
     *  closed" are different facts, and a host diagnosing a session
     *  that never started needs to tell them apart. */
    static const char* ice_state_name(plugins::IceState state) noexcept;

    /** The DTLS session's state, lower-cased and hyphen-separated:
     *  "closed", "initial", "hello-verify", "hello-sent",
     *  "hello-received", "certificate-received", "key-exchange",
     *  "change-cipher-spec", "finished", "connected" or "failed".
     *
     *  The DTLS module spells its states in CamelCase ("HelloVerify"),
     *  because they are log labels for engineers.  The statistics
     *  surface publishes them in the same lower-cased, hyphen-separated
     *  shape as the ICE states, so a host can print either without a
     *  per-value conversion table.
     *
     *  The mapping is *derived* from the module's own name rather than
     *  copied from it: the engine lower-cases the module's CamelCase
     *  name and inserts a hyphen before each capital.  A new DTLS state
     *  therefore appears on this surface automatically — there is no
     *  second table here that could drift out of step with the
     *  module's. */
    static std::string dtls_state_name(nimrtc::dtls::DtlsState state);

    // ---- Connectivity diagnostics (v1.4 plan Track N / N4) ----------------


    /** The TURN transport the relayed path uses, lower-cased: "udp",
     *  "tcp" or "tls". */
    static const char* turn_transport_name(ice::TurnTransport transport) noexcept;

    /** The per-type counts of the gathered local candidates. */
    struct CandidateCounts {
        std::uint32_t host = 0;
        std::uint32_t srflx = 0;
        std::uint32_t relay = 0;
    };

    /** Count the local candidates by type.
     *
     *  Every line is parsed with the ICE module's own candidate grammar;
     *  an unparseable line is skipped rather than counted (a candidate
     *  the module emitted but its own parser cannot name is not counted
     *  as any type).  A TCP host candidate counts as a host candidate. */
    static CandidateCounts count_candidates(
        const std::vector<std::string>& lines) noexcept;

    /** The facts the failure-reason vocabulary is decided on. */
    struct FailureInputs {
        bool gathering_complete = false;
        CandidateCounts counts;
        bool pair_selected = false;
        bool agent_failed = false;
        bool turn_transport_unsupported = false;
        bool turn_credentials_rejected = false;
    };

    /** The closed failure-reason vocabulary, in the spec's priority order
     *  (first match reports first): "transport-unsupported" >
     *  "credentials-rejected" > "no-candidates" > "no-pair" >
     *  "checks-failed" > "none".  "none" means no failure has been
     *  established — a healthy session is never assigned a fabricated
     *  reason. */
    static std::string ice_failure_reason(const FailureInputs& in) noexcept;
};

}  // namespace nimrtc::engine::detail
