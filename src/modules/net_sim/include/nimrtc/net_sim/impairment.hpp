/**
 * @file nimrtc/net_sim/impairment.hpp
 * @brief The **network impairment shaper** — the portable, deterministic
 *        model of what a bad link does to a packet.
 *
 * ## Why this exists
 *
 * The v1.3 plan's Q5 asks for a long soak "across NAT + netem" — a
 * measurement of how the engine behaves when the network misbehaves.
 *  Two ways to make a network misbehave exist in this tree already:
 *
 *   - `tools/benchmark_weaknet_netem.sh` — the **kernel-side** shaper
 *     (`tc qdisc netem` on `lo` inside a throwaway network namespace).
 *     It is the most honest shaper there is, but it needs root, it
 *     needs Linux, and it cannot be exercised from a unit test.
 *   - `tools/benchmark_weaknet.py` — the **in-process** shaper.  It is
 *     portable and deterministic, but it measures the *transport floor*:
 *     it never touches the engine.
 *
 * This shaper is the missing third thing: the **engine-grade** model of
 * the same impairments, expressed as a small, testable object that the
 * NAT relay (see `nat_relay.hpp`) and the soak harness both drive.
 *  It is deliberately *not* a re-implementation of netem's queueing
 * discipline — it is the impairment *profile* (loss / delay / jitter /
 * reorder / rate), expressed so that the same profile can be applied
 * in-process on every platform and asserted in a unit test.
 *
 * ## The profile
 *
 * | knob         | meaning                                                          |
 * |--------------|------------------------------------------------------------------|
 * | `loss_pct`   | uniform drop probability, in percent                             |
 * | `delay_ms`   | the base one-way delay                                           |
 * | `jitter_ms`  | uniform spread **around** the base delay (netem's shape)         |
 * | `reorder_pct`| the share of packets that is held back so it arrives *late*      |
 * | `rate_bps`   | the link's capacity; 0 means "unlimited"                         |
 *
 * The `reorder` knob is the one the v1.3 plan calls out explicitly
 * ("netem's `reorder` is not used at all" — the plan's §8 verification
 * table).  Netem implements reordering by *delaying* a share of the
 * packets past their successors; this shaper does the same, and the
 * unit test asserts that the successors really do go first.
 *
 * ## Determinism
 *
 * Every random draw comes from one seeded generator, so a run with the
 * same seed and the same packet sequence produces the same decisions on
 * every machine.  That is what makes the soak's numbers comparable
 * between runs and what makes the unit test meaningful.
 *
 * @see nimrtc/net_sim/nat_relay.hpp — the relay that applies this profile
 * @see docs/plan/v1.3-plan.md §Q5 — the soak this profile exists for
 */

#ifndef NIMRTC_NET_SIM_IMPAIRMENT_HPP
#define NIMRTC_NET_SIM_IMPAIRMENT_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstddef>
#include <cstdint>

namespace nimrtc::net_sim {

// ---------------------------------------------------------------------------
// The profile
// ---------------------------------------------------------------------------

/**
 * @brief The impairment profile applied to one direction of a link.
 *
 * The defaults describe a **clean** link: nothing is dropped, nothing is
 * delayed, nothing is reordered, and the rate is unlimited.  A profile
 * is therefore opt-in by construction — a caller that forgets to fill
 * the profile in gets a working link, not a broken one.
 */
struct NIMRTC_API_STABLE ImpairmentConfig {
    /** Uniform drop probability, in percent. Clamped to [0, 100]. */
    double loss_pct = 0.0;

    /** The base one-way delay, in milliseconds. */
    double delay_ms = 0.0;

    /** The uniform spread **around** `delay_ms`, in milliseconds.
     *
     *  Netem's `delay TIME JITTER` spreads the delay uniformly around
     *  `TIME`, so the effective delay lies in
     *  `[delay - jitter/2, delay + jitter/2]`.  This shaper uses the
     *  same shape, so the two are directly comparable. */
    double jitter_ms = 0.0;

    /** The share of packets (in percent) that is held back so that it
     *  arrives *after* the packets that followed it.
     *
     *  This is netem's `reorder` knob.  The held packet is not lost —
     *  it is late, which is what makes reordering different from loss
     *  for a receiver (a late packet can still be assembled; a lost
     *  one cannot). */
    double reorder_pct = 0.0;

    /** How much later a reordered packet arrives, in milliseconds.
     *
     *  The default (25 ms) is long enough to overtake the packets that
     *  follow it at ordinary frame rates, and short enough not to look
     *  like loss to a receiver that gives up after a few frames. */
    double reorder_extra_ms = 25.0;

    /** The link's capacity in bits per second; 0 means "unlimited".
     *
     *  A rate-limited link does not merely delay a packet: it delays
     *  it by the time the link needs to put it on the wire (its
     *  serialization time) plus whatever was already queued ahead of
     *  it.  The shaper models exactly that. */
    std::uint32_t rate_bps = 0;

    /** The longest queueing delay the link will absorb, in milliseconds.
     *
     *  A packet that would have to wait longer than this is **dropped**
     *  instead: a link with an unbounded queue turns a congestion
     *  collapse into an unbounded latency, and the soak's green
     *  criteria care about the latency.  The default (500 ms) is well
     *  above any sane playout deadline. */
    double max_queue_delay_ms = 500.0;

    /** The seed of the shaper's generator.  The same seed and the same
     *  packet sequence give the same decisions on every machine. */
    std::uint64_t seed = 20261119;
};

// ---------------------------------------------------------------------------
// The decision
// ---------------------------------------------------------------------------

/**
 * @brief What the shaper decided to do with one packet.
 */
struct NIMRTC_API_STABLE PacketDecision {
    /** The packet is dropped: it never reaches the other side. */
    bool dropped = false;

    /** How much later than "right now" the packet should be delivered,
     *  in microseconds.  Zero means "deliver immediately". */
    std::int64_t delay_us = 0;

    /** The packet was held back to reorder it.  The delay already
     *  includes the extra hold. */
    bool reordered = false;
};

// ---------------------------------------------------------------------------
// The shaper
// ---------------------------------------------------------------------------

/**
 * @brief Applies an impairment profile to a packet stream.
 *
 * ## Threading
 *
 * **Not** thread-safe: one shaper belongs to one direction of one
 *  link, and the caller serialises the calls (the relay's own thread
 *  does).
 *
 * ## Cost
 *
 * One call per packet, no allocation, no I/O.  The generator is the
 *  only state besides the rate limiter's bookkeeping.
 */
class NIMRTC_API_STABLE ImpairmentShaper {
public:
    /** Build a shaper for the given profile. */
    explicit ImpairmentShaper(const ImpairmentConfig& config) noexcept;

    /**
     * @brief Decide what happens to one packet.
     *
     * @param packet_bytes  the packet's size on the wire, in bytes.
     * @param now_us        the current time, in microseconds on a
     *                      monotonic clock.  The value is only used for
     *                      the rate limiter's bookkeeping, so it may be
     *                      any monotonically increasing sequence.
     * @return the decision; see `PacketDecision`.
     */
    PacketDecision shape(std::size_t packet_bytes, std::int64_t now_us) noexcept;

    /** The packets the shaper has seen. */
    std::uint64_t packets_seen() const noexcept { return packets_seen_; }

    /** The packets the shaper dropped (loss, or the queue bound). */
    std::uint64_t packets_dropped() const noexcept { return packets_dropped_; }

    /** The packets the shaper dropped because the queue was too long. */
    std::uint64_t packets_dropped_queue_full() const noexcept {
        return packets_dropped_queue_full_;
    }

    /** The packets the shaper held back to reorder them. */
    std::uint64_t packets_reordered() const noexcept { return packets_reordered_; }

private:
    ImpairmentConfig config_;
    std::uint64_t packets_seen_        = 0;
    std::uint64_t packets_dropped_      = 0;
    std::uint64_t packets_dropped_queue_full_ = 0;
    std::uint64_t packets_reordered_    = 0;

    // The rate limiter's bookkeeping: the instant the link becomes free
    // again.  Zero means "the link has never been used".
    std::int64_t link_free_at_us_ = 0;
    bool        link_used_        = false;

    // The generator state (a 64-bit xorshift — the profile only needs
    // uniform draws, and a hand-rolled generator keeps the module free
    // of <random>'s implementation-defined distribution behaviour).
    std::uint64_t rng_state_ = 0;

    /** A uniform draw in [0, 1). */
    double uniform() noexcept;

    /** A uniform draw in [lo, hi). */
    double uniform_range(double lo, double hi) noexcept;
};

} // namespace nimrtc::net_sim

#endif // NIMRTC_NET_SIM_IMPAIRMENT_HPP
