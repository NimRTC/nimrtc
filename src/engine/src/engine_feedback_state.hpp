/**
 * @file src/engine/src/engine_feedback_state.hpp
 * @brief PRIVATE engine collaborator: the RTCP feedback / BWE / pacing state.
 *
 * Architecture remediation plan §4.3 (D1 — engine split): this state
 * used to live as loose members inside `NimRTCEngine::Impl`, which
 * made the engine's internal header a catch-all and scattered one
 * feature's state across the file.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * ## What the state is
 *
 * Two closely-related concerns, both about *what the engine observed on
 * the RTCP path*:
 *
 *   1. **The network-feedback loop** (v1.2.0 M1, F2/F6).  The loop is:
 *      inbound RTCP -> engine demux -> collector -> snapshot ->
 *      `IBwe::on_feedback()`.  The counters make the loop observable, and
 *      the de-duplication state makes it *once per report* rather than
 *      once per engine tick.
 *   2. **The pacing split** (v1.2.0 M3, P2).  The demand list and the
 *      resulting allocation are the pacer's view of the session's
 *      streams; the allocation is kept by reference rather than returned
 *      by value so the *private* split helper does not have to expose
 *      the private `BandwidthAllocation` type through the public header.
 *
 * The RTCP XR counters live in `engine_xr_state.hpp` instead: the XR
 * state is the *authority* on what may be sent, so it belongs with the
 * negotiated flags it gates, not here.
 *
 * ## Not thread-safe
 *
 * The engine's tick loop is the only writer and the RTCP demux path the
 * only reader, on the same thread — the same contract every other
 * engine collaborator keeps.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

#include <nimrtc/core/time.hpp>

#include "bandwidth_allocation.hpp"

namespace nimrtc::engine::detail {

/** The RTCP-feedback / BWE / pacing state.
 *
 *  Not thread-safe: see the header's note. */
struct FeedbackState {
    // ---- Network feedback loop (v1.2.0 M1, F2/F6) --------------------
    //
    // The loop is: inbound RTCP -> engine demux -> collector -> snapshot
    // -> IBwe::on_feedback().  These two counters make the loop
    // observable: `net_feedback_blocks` counts the RTCP blocks that were
    // routed to the collector, `bwe_feedback_samples` counts the
    // aggregated snapshots that were actually handed to the estimator.
    //
    // The last two fields are the de-duplication state: a snapshot is
    // fed only when it carries new information (its timestamp moved),
    // so the estimator sees one sample per feedback report rather than
    // one per engine tick.
    std::uint64_t net_feedback_blocks   = 0;
    std::uint64_t bwe_feedback_samples  = 0;
    core::TimePoint last_fed_feedback_ts{};
    bool           has_fed_feedback     = false;

    /** True once the engine has reported that the transport-cc path was
     *  required but is unavailable (ADR-018 Decision 2's "fail loudly"
     *  mode).  The report is made once per session. */
    bool transport_cc_unavailable_reported = false;

    // ---- RTCP Extended Reports (v1.2.0, session plan X1) --------------
    //
    // The number of inbound RTCP XR sub-packets that were routed to the
    // XR consumer.  It is the observable that proves the wiring is
    // closed: a peer that sends extended reports and a counter that
    // stays at zero mean the routing is broken.
    std::uint64_t xr_report_blocks = 0;

    // ---- Pacing split (v1.2.0 M3, P2) ---------------------------------
    //
    // The demand list of the bandwidth split, kept as a member so that
    // the per-tick split does not allocate a vector every time.  The
    // contents are rewritten (never reallocated) on each tick.
    std::vector<BandwidthDemand> pacing_demands_;

    /** The result of the last D5 split (the per-stream bandwidth
     *  shares).
     *
     *  The allocation lives here rather than being returned by value, so
     *  the *private* split helper does not have to expose the private
     *  `BandwidthAllocation` type through the public header.  The
     *  pacer and the rate-control path read the same object, which is
     *  what makes their numbers agree by construction. */
    BandwidthAllocation pacing_allocation_;

    /** The sum of the video streams' encoder ceilings, in bits per
     *  second.  The video *class* rate is capped by it: the video may
     *  not be paced faster than it encodes. */
    std::uint32_t video_ceiling_total_bps = 0;
};

}  // namespace nimrtc::engine::detail
