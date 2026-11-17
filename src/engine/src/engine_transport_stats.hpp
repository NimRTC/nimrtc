/**
 * @file src/engine/src/engine_transport_stats.hpp
 * @brief PRIVATE engine collaborator: the transport-plane statistics projection.
 *
 * Architecture remediation plan 搂4.3 (D1 鈥?engine split): the engine's
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
 * The *transport plane* 鈥?the ICE 鈫?DTLS 鈫?SRTP path the session runs
 * over 鈥?has three parts, and the v1.2 plan's **S5** row asks for all
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
 *     first 鈥?and the ICE agent's state *changes on its own thread*, so
 *     a copy would go stale.
 *   - The DTLS state is owned by the DTLS session (a `weak_ptr` handle
 *     the engine holds).
 *   - The SRTP counters are owned by the SRTP context.
 *   - The round-trip time is owned by the network-feedback collector 鈥? *     and the engine already reads it there to feed the bandwidth
 *     estimator, so the statistics surface reads the *same* figure
 *     rather than measuring a second one.
 *
 * So the "state" here is only the *shape* of the projection: which
 * source answers which field.  Keeping it in one named place is what
 * makes the projection reviewable 鈥?a reader sees the whole transport
 * surface at once, and sees that every number has exactly one owner.
 *
 * ## Why the RTT is not measured twice
 *
 * The plan's DS-6 row says the RTT is "鍙栬嚜寮曟搸宸叉湁鐨?DTLS / SRTP /
 * RTCP 鍙嶉闈紝**涓嶆柊澧炴帰閽?*" 鈥?taken from the feedback surface the
 * engine already has, with **no new probe**.  The engine already
 * consumes the peer's Receiver Reports through the network-feedback
 * collector, and the collector already derives the RTT from the LSR /
 * DLSR pair against our own Sender Report (RFC 3550 搂6.4.1).  The
 * bandwidth estimator consumes that figure.  This surface republishes
 * it; a second measurement would be a second answer to the same
 * question, and the two would disagree.
 */
#pragma once

#include <string>

#include <nimrtc/dtls/dtls_types.hpp>
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
     *  there, while the statistics surface answers the empty string 鈥?     *  "we have no transport" and "the transport is in the state called
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
     *  therefore appears on this surface automatically 鈥?there is no
     *  second table here that could drift out of step with the
     *  module's. */
    static std::string dtls_state_name(nimrtc::dtls::DtlsState state);
};

}  // namespace nimrtc::engine::detail
