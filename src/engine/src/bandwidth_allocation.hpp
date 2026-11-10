/**
 * @file bandwidth_allocation.hpp
 * @brief F8 — the per-stream share rule of the session bandwidth budget.
 *
 * ## The problem this file solves
 *
 * `IBwe::estimate()` returns **one** number for the whole session: the
 * bandwidth estimate.  A session carries several streams (the audio, the
 * video, and — as the media plan grows — more than one of each), and they
 * cannot all spend the same budget: a 300 kbps link cannot carry a
 * 300 kbps video stream *and* a live voice call.
 *
 * So the estimate has to be **split** before it reaches an encoder.  That
 * split is what this file defines.  It is a pure function of
 * (estimate, demand list) — no state, no clock, no I/O — so it can be
 * unit tested on its own and reasoned about in isolation.
 *
 * ## The rule
 *
 * The streams fall into two classes.
 *
 * **Reserved streams** take their share of the estimate *first*.  A
 * reserved stream is one whose quality is not worth trading away: the
 * audio.  Its allocation is
 *
 *     alloc = clamp( round(estimate * share), floor, ceiling )
 *
 * where `floor` is a **hard** floor — the audio keeps it even when the
 * estimate collapsed below it, because a starved audio stream ends the
 * call while a starved video stream only ends the picture.  A reserved
 * stream that claims more than the estimate holds puts the session into
 * **deficit** (see below).
 *
 * **Elastic streams** share whatever is left.  The remainder
 *
 *     pool = max(0, estimate - sum(reserved allocations))
 *
 * is divided among the elastic streams in proportion to their `weight`
 * (relative units *among the elastic streams* — the weight of a reserved
 * stream does not enter the pool split).  The division uses the
 * largest-remainder (Hamilton) method, so the pool is handed out
 * completely and deterministically: no bit is lost to integer division
 * and no bit is invented.
 *
 * An elastic stream's share is clamped to its own `ceiling` (0 = no
 * ceiling).  The amount a ceiling removes is **not** redistributed to the
 * other elastic streams: a stream that asked for less than its share
 * does not get the surplus, because the surplus exists to serve the
 * stream that *needs* bandwidth, not to fill a pipe.
 *
 * ## Deficit
 *
 * When the reserved streams' allocations exceed the estimate (the audio
 * floor is above the link), the session is in **deficit**: the elastic
 * pool is zero, so the elastic streams get nothing, and the flag says so
 * explicitly.  The reserved streams keep their floors — the floor is the
 * point of a floor — and the deficit is reported rather than hidden.
 *
 * ## Edge cases
 *
 * - **No estimate.**  An estimate of 0 means "no opinion" (the estimator
 *   has not heard from the network yet), not "no bandwidth at all".  The
 *   function then allocates nothing at all and reports no deficit; the
 *   caller keeps whatever allocation it already had.  The engine's audio
 *   path relies on this: it returns early on a zero estimate instead of
 *   reading it as a zero bitrate.
 * - **Floor above ceiling.**  A misconfigured demand whose floor exceeds
 *   its ceiling is clamped to the ceiling — the ceiling is the outer
 *   bound of what the caller said it can use, and it is the one that
 *   wins.  (The engine's audio path applies the same rule: it lowers
 *   the floor to the ceiling before using it.)
 * - **Zero weight.**  An elastic stream with weight 0 takes nothing from
 *   the pool, and its weight does not dilute the other streams' weights.
 * - **Empty demand list.**  The result is an empty allocation: the
 *   estimate is not spent on anything.  The *pool* is still reported
 *   (it holds the whole estimate) -- the caller sees "nothing was
 *   allocated" in the targets, not in an emptied pool.
 *
 * ## Why the engine owns this
 *
 * Decision D3 of the video plan ("the engine computes, the encoder
 * obeys") makes the engine the only place that knows the whole picture:
 * which streams exist, what each of them is worth, and what the link can
 * carry.  The rule therefore lives *inside* the engine, next to the
 * adaptation step that consumes it, and the video rate controller (B2)
 * will be handed the resulting per-stream budget rather than the raw
 * estimate.
 *
 * The header is header-only on purpose: the rule is a pure function, and
 * keeping it inline avoids a new translation unit (and a new entry in
 * the engine's CMake source list) for a dozen lines of arithmetic.
 *
 * @see docs/plan/video-pipeline-v2-plan.md (F8)
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace nimrtc::engine::detail {

/** One stream's claim on the session bandwidth budget.
 *
 *  A demand is either *reserved* (it takes its share of the estimate
 *  first) or *elastic* (it shares what is left).  The two classes use
 *  different fields; the irrelevant ones are ignored. */
struct BandwidthDemand {
    /** True for a reserved stream (the audio), false for an elastic one.
     *
     *  A reserved stream is served before any elastic stream, and its
     *  `floor_bps` is a hard floor. */
    bool reserved = false;

    /** A reserved stream's share of the estimate, in [0.0, 1.0].
     *
     *  Ignored for an elastic stream.  Clamped into range by the rule,
     *  so a misconfigured value cannot make the allocation nonsense. */
    double share = 0.0;

    /** A reserved stream's hard floor, in bits per second.
     *
     *  The allocation never drops below it, even in deficit.  Ignored
     *  for an elastic stream. */
    std::uint32_t floor_bps = 0;

    /** The stream's ceiling, in bits per second.  0 means "no ceiling".
     *
     *  For a reserved stream the ceiling bounds the share; for an
     *  elastic stream it bounds its slice of the pool. */
    std::uint32_t ceiling_bps = 0;

    /** The stream's weight among the elastic streams, in relative units.
     *
     *  A weight of 0 means "takes nothing from the pool".  Ignored for
     *  a reserved stream. */
    std::uint32_t weight = 1;
};

/** The result of the split: one target per demand, plus the totals. */
struct BandwidthAllocation {
    /** The allocation of every demand, in the order the demands were
     *  given.  A reserved stream's target is its clamped share; an
     *  elastic stream's target is its slice of the pool. */
    std::vector<std::uint32_t> targets;

    /** The sum of the reserved streams' targets. */
    std::uint32_t reserved_bps = 0;

    /** The budget left for the elastic streams (what was left of the
     *  estimate after the reserved streams took their share).  It is
     *  reported even when no elastic stream takes it — an unspent pool
     *  is information, not an error. */
    std::uint32_t elastic_pool_bps = 0;

    /** True when the reserved streams claimed more than the estimate.
     *
     *  The elastic pool is then empty and the elastic streams get
     *  nothing; the reserved streams keep their hard floors. */
    bool deficit = false;
};

/** Split a session-wide bandwidth estimate between the streams.
 *
 *  @param estimate_bps  The session estimate, in bits per second.  A
 *                       value of 0 yields an all-zero allocation (no
 *                       estimate, no promise — the caller keeps what it
 *                       had).
 *  @param demands       The streams' demands, in priority order.  The
 *                       reserved streams are served first, in the order
 *                       they appear.
 *
 *  @return The per-stream targets and the totals.  The returned
 *          `targets` vector is parallel to `demands`.
 */
inline BandwidthAllocation allocate_bandwidth(
    std::uint32_t estimate_bps,
    const std::vector<BandwidthDemand>& demands) noexcept {
    BandwidthAllocation out;
    out.targets.assign(demands.size(), 0u);

    // No estimate: no promise.  The caller keeps its current allocation.
    if (estimate_bps == 0) return out;

    // ---- Pass 1 — the reserved streams ---------------------------------
    //
    // They are served first, each one taking its clamped share of the
    // estimate.  Their sum may exceed the estimate: that is the deficit
    // case, and it is reported rather than hidden.
    std::uint64_t reserved_total = 0;
    for (std::size_t i = 0; i < demands.size(); ++i) {
        const BandwidthDemand& d = demands[i];
        if (!d.reserved) continue;

        // A NaN share is treated as 0 — the comparison is written so that
        // it fails for NaN, which is what we want.
        double share = d.share;
        if (!(share > 0.0)) share = 0.0;
        if (share > 1.0) share = 1.0;

        // Round to nearest: the sub-bits of the share must not be lost to
        // truncation, but they must not be invented either.
        std::uint64_t alloc = static_cast<std::uint64_t>(
            static_cast<double>(estimate_bps) * share + 0.5);

        // The floor is hard; the ceiling is the outer bound and wins over
        // a floor that was configured above it.
        if (alloc < d.floor_bps) alloc = d.floor_bps;
        if (d.ceiling_bps > 0 && alloc > d.ceiling_bps) alloc = d.ceiling_bps;

        out.targets[i] = static_cast<std::uint32_t>(alloc);
        reserved_total += alloc;
    }

    // The pool is what the elastic streams may spend.  It is reported
    // even when nobody claims it (an empty demand list, a zero-weight
    // stream): the caller can tell "the estimate was not spent" from
    // "there was no estimate" by looking at the targets, not by
    // looking at an emptied pool.
    out.reserved_bps = reserved_total > 0xFFFFFFFFull
                           ? 0xFFFFFFFFu
                           : static_cast<std::uint32_t>(reserved_total);
    out.deficit = reserved_total > estimate_bps;
    out.elastic_pool_bps =
        reserved_total < estimate_bps
            ? static_cast<std::uint32_t>(estimate_bps - reserved_total)
            : 0u;

    // ---- Pass 2 — the elastic streams ----------------------------------
    //
    // They share the pool in proportion to their weights.  The split is
    // done with exact integer arithmetic (the pool, a weight and the
    // total weight are all bounded by 2^32, so the product fits in 64
    // bits without overflow), and the bits lost to integer division are
    // handed out by the largest-remainder (Hamilton) method.
    if (out.elastic_pool_bps == 0) return out;

    std::uint64_t total_weight = 0;
    for (const BandwidthDemand& d : demands) {
        if (d.reserved) continue;
        total_weight += d.weight;
    }
    if (total_weight == 0) return out;

    const std::uint64_t pool = out.elastic_pool_bps;

    // The eligible streams (weight > 0) and their remainders.
    std::vector<std::size_t> eligible;
    std::vector<std::uint64_t> remainder;
    eligible.reserve(demands.size());
    remainder.reserve(demands.size());

    std::uint64_t handed_out = 0;
    for (std::size_t i = 0; i < demands.size(); ++i) {
        const BandwidthDemand& d = demands[i];
        if (d.reserved) continue;
        if (d.weight == 0) {
            // Takes nothing from the pool; its zero weight does not
            // dilute the other streams' weights either.
            continue;
        }
        const std::uint64_t numerator = pool * d.weight;
        const std::uint64_t base = numerator / total_weight;
        out.targets[i] = static_cast<std::uint32_t>(base);
        handed_out += base;
        eligible.push_back(i);
        remainder.push_back(numerator % total_weight);
    }

    // The bits that integer division dropped: fewer than the number of
    // eligible streams, so they can be handed out one each.
    std::uint64_t leftover = pool - handed_out;
    if (leftover > 0 && !eligible.empty()) {
        std::vector<std::size_t> order(eligible.size());
        for (std::size_t k = 0; k < order.size(); ++k) order[k] = k;
        std::stable_sort(order.begin(), order.end(),
                         [&remainder](std::size_t a, std::size_t b) {
                             if (remainder[a] != remainder[b]) {
                                 return remainder[a] > remainder[b];
                             }
                             // Deterministic tie-break: the earlier stream
                             // wins, so the same input always gives the
                             // same allocation.
                             return a < b;
                         });
        for (std::size_t k = 0; k < order.size() && leftover > 0; ++k) {
            out.targets[eligible[order[k]]] += 1u;
            --leftover;
        }
    }

    // The ceilings are applied last and what they remove is *not*
    // redistributed: the surplus exists to serve the stream that needs
    // bandwidth, not to fill a pipe.
    for (std::size_t i = 0; i < demands.size(); ++i) {
        const BandwidthDemand& d = demands[i];
        if (d.reserved) continue;
        if (d.ceiling_bps > 0 && out.targets[i] > d.ceiling_bps) {
            out.targets[i] = d.ceiling_bps;
        }
    }

    return out;
}

}  // namespace nimrtc::engine::detail
