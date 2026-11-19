/**
 * @file src/impairment.cpp
 * @brief The impairment shaper's implementation.
 *
 * See the header for the contract.  The implementation is deliberately
 * small: the profile is five knobs, and every one of them is a
 * *decision* about one packet, so the whole shaper is a handful of
 * draws and two lines of rate-limiter bookkeeping.
 */

#include "nimrtc/net_sim/impairment.hpp"

#include <algorithm>
#include <cmath>

namespace nimrtc::net_sim {
namespace {

/** Clamp a percentage into [0, 100]. */
double clamp_pct(double value) noexcept {
    if (!(value > 0.0)) return 0.0;   // also catches NaN
    if (value > 100.0) return 100.0;
    return value;
}

/** Clamp a millisecond value into [0, ...]. */
double clamp_ms(double value) noexcept {
    if (!(value > 0.0)) return 0.0;   // also catches NaN
    return value;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// ImpairmentShaper
// ---------------------------------------------------------------------------

ImpairmentShaper::ImpairmentShaper(const ImpairmentConfig& config) noexcept
    : config_(config) {
    config_.loss_pct          = clamp_pct(config_.loss_pct);
    config_.delay_ms          = clamp_ms(config_.delay_ms);
    config_.jitter_ms         = clamp_ms(config_.jitter_ms);
    config_.reorder_pct       = clamp_pct(config_.reorder_pct);
    config_.reorder_extra_ms  = clamp_ms(config_.reorder_extra_ms);
    config_.max_queue_delay_ms = clamp_ms(config_.max_queue_delay_ms);

    // The generator's seed must not be zero: the xorshift below would
    // never leave zero.
    rng_state_ = config_.seed ? config_.seed : 0x9E3779B97F4A7C15ULL;
}

double ImpairmentShaper::uniform() noexcept {
    // xorshift64* — one round, no state table, and a period long
    // enough that a soak's packet count never comes close to
    // repeating.
    std::uint64_t x = rng_state_;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state_ = x;
    // The top 53 bits are the mantissa of a double.
    return static_cast<double>(x >> 11) * (1.0 / 9007199254740992.0);
}

double ImpairmentShaper::uniform_range(double lo, double hi) noexcept {
    if (!(hi > lo)) return lo;
    return lo + uniform() * (hi - lo);
}

PacketDecision ImpairmentShaper::shape(std::size_t packet_bytes,
                                       std::int64_t now_us) noexcept {
    ++packets_seen_;

    PacketDecision decision;

    // ---- Loss ------------------------------------------------------------
    //
    // The draw comes first: a dropped packet must not consume the
    // link's capacity, exactly as a packet the kernel dropped before
    // it reached the queue never did.
    if (config_.loss_pct > 0.0 &&
        uniform() * 100.0 < config_.loss_pct) {
        decision.dropped = true;
        ++packets_dropped_;
        return decision;
    }

    // ---- The base delay, with the jitter spread around it ---------------
    //
    // Netem's `delay TIME JITTER` spreads the delay uniformly *around*
    // TIME, so the effective delay lies in [TIME - JITTER/2,
    // TIME + JITTER/2].  The same shape here, so the two shapers can be
    // read side by side.
    double delay_ms = config_.delay_ms;
    if (config_.jitter_ms > 0.0) {
        const double half = config_.jitter_ms * 0.5;
        delay_ms += uniform_range(-half, half);
        if (delay_ms < 0.0) delay_ms = 0.0;
    }

    // ---- Reordering ------------------------------------------------------
    //
    // A reordered packet is held back past the packets that followed it.
    // The hold is *additional* to the base delay, so the packet is
    // late, not lost — which is the whole difference between reorder
    // and loss for a receiver.
    if (config_.reorder_pct > 0.0 &&
        uniform() * 100.0 < config_.reorder_pct) {
        delay_ms += config_.reorder_extra_ms;
        decision.reordered = true;
        ++packets_reordered_;
    }

    // ---- The rate limit --------------------------------------------------
    //
    // A rate-limited link delays a packet by the time it needs to put
    // it on the wire (its serialization time) plus whatever was already
    // queued ahead of it.  The bookkeeping is a single "the link is
    // free again at" timestamp.
    if (config_.rate_bps > 0) {
        const double bytes = static_cast<double>(packet_bytes);
        const double service_us =
            (bytes * 8.0 * 1e6) / static_cast<double>(config_.rate_bps);

        std::int64_t start_us = now_us;
        if (link_used_ && link_free_at_us_ > start_us) {
            start_us = link_free_at_us_;
        }
        const std::int64_t finish_us =
            start_us + static_cast<std::int64_t>(service_us + 0.5);
        link_free_at_us_ = finish_us;
        link_used_ = true;

        const std::int64_t queue_us = finish_us - now_us;
        const std::int64_t queue_limit_us =
            static_cast<std::int64_t>(config_.max_queue_delay_ms * 1000.0 + 0.5);
        if (queue_limit_us > 0 && queue_us > queue_limit_us) {
            // The queue is longer than the link is willing to hold: the
            // packet is dropped rather than delayed into irrelevance.
            decision.dropped = true;
            ++packets_dropped_;
            ++packets_dropped_queue_full_;
            return decision;
        }
        if (queue_us > 0) {
            delay_ms += static_cast<double>(queue_us) / 1000.0;
        }
    }

    decision.delay_us = static_cast<std::int64_t>(delay_ms * 1000.0 + 0.5);
    if (decision.delay_us < 0) decision.delay_us = 0;
    return decision;
}

} // namespace nimrtc::net_sim
