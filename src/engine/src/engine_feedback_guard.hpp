/**
 * @file src/engine/src/engine_feedback_guard.hpp
 * @brief PRIVATE engine collaborator: the **receive-side feedback bound**
 *        (v1.3 plan Q4 / video plan T2).
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * ## What this file is
 *
 * The receiver's feedback to the peer used to be *merged* rather than
 * *counted*.  Three separate things could go wrong, and all three are
 * feedback amplification:
 *
 *   1. **The key-frame request had no rate limit of its own.**  The
 *      engine's decode-failure path had a 250 ms throttle, but the
 *      jitter buffer's own loss report did not — and both went into
 *      the *same* pending report, so a stream that both lost packets
 *      and failed to decode asked for key frames at twice the rate.
 *
 *   2. **The NACK list was uncapped.**  Every detected gap added its
 *      sequence numbers to a list that was merged into the pending
 *      report without a limit.  A peer that drops 30 % of a 30 fps
 *      stream produces a list that grows with the loss, and the RTCP
 *      report grows with it.
 *
 *   3. **The pending report had no age.**  A report that sat in the
 *      queue while the link was down was stale by the time it went
 *      out: the losses it named had either been retransmitted already
 *      or were long gone.
 *
 * This collaborator owns the **bound** on all three, plus the **per-door
 * counting** that makes the bound observable.
 *
 * ## The two doors
 *
 * There are exactly two ways the receiver can ask for a key frame, and
 * they mean different things:
 *
 *   - **the loss door** — the jitter buffer saw a frame it could not
 *     assemble, so the picture is incomplete;
 *   - **the decode-failure door** — the decoder refused a frame that
 *     assembled perfectly, so the bitstream is undecodable.
 *
 * Both used to vanish into the same merged report.  The guard counts
 * each separately, so a host can tell "the network is lossy" from "the
 * decoder cannot decode what it is being sent" — the two call for
 * completely different fixes.
 *
 * ## The rules
 *
 * - **The PLI rate limit is global, not per-door.**  One key-frame
 *   request per interval, whichever door raised it: the peer's encoder
 *   is the shared resource the limit protects, so a per-door limit
 *   would let the two doors add up to twice the rate.
 *
 * - **The NACK cap keeps the *most recent* losses.**  RFC 4585 §6.2.1
 *   shapes the request around the recent losses, and the sender's
 *   retransmission cache only holds the recent packets — a request for
 *   an old loss is a request the peer cannot answer.  So when the
 *   list overflows, the *oldest* entries are the ones dropped.
 *
 * - **A stale report is dropped, not sent.**  A report older than the
 *   maximum age says nothing about the picture any more, and sending
 *   it costs a round trip.
 *
 * ## Why the clock is injected
 *
 * The rule is a function of time, and the acceptance for Q4 is a
 * **deterministic 30 s synthetic-loss run** that runs in CI.  A
 * wall-clock gate cannot be tested deterministically, so the guard
 * takes its clock as a parameter: the engine passes the monotonic
 * clock, the test passes a virtual one.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <nimrtc/plugins/video_pipeline.hpp>

namespace nimrtc::engine::detail {

/**
 * @brief Where a feedback report came from.
 *
 * The two doors mean different things:
 *
 *   - **the loss door** — the jitter buffer saw a frame it could not
 *     assemble, so the picture is incomplete;
 *   - **the decode-failure door** — the decoder refused a frame that
 *     assembled perfectly, so the bitstream is undecodable.
 *
 * Before v1.3 Q4 both doors vanished into the same merged pending
 * report, so a host could not tell "the network is lossy" from "the
 * decoder cannot decode what it is being sent" — the two call for
 * completely different fixes.
 *
 * This is the **definition**; the public header
 * (`nimrtc/engine/engine.hpp`) only forward-declares it, because a
 * public header may not include an engine-private header (Layout
 * Invariant 4).
 */
enum class FeedbackSource : std::uint8_t {
    /** The jitter buffer detected a loss (an incomplete frame). */
    kLoss = 0,
    /** The decoder refused a frame that assembled perfectly. */
    kDecodeFailure = 1,
};

/**
 * @brief The receive-side feedback bound.
 *
 * Header-only in its *interface*; the mechanism lives in
 * `engine_feedback_guard.cpp`, next to the other engine collaborators.
 */
class FeedbackGuard {
public:
    /** What one `queue()` call contributed to the pending report. */
    struct QueueResult {
        /** How many NACK sequence numbers made it into the report. */
        std::size_t nack_seqs_added = 0;
        /** How many NACK sequence numbers the cap discarded. */
        std::size_t nack_seqs_dropped = 0;
        /** True when the report's key-frame request passed the gate. */
        bool pli_allowed = false;
        /** True when the report's key-frame request was suppressed. */
        bool pli_suppressed = false;
        /** True when the pending report carries a key-frame request that
         *  this call put there (an earlier one may already have). */
        bool pli_added = false;
    };

    /** The guard's tunable bounds. */
    struct Config {
        /**
         * The minimum interval between two key-frame requests, in
         * microseconds.
         *
         * The default (250 ms) is RFC 5104 §4.3.1.1's "about 4 Hz",
         * and it is the same interval the engine's own decode-failure
         * throttle used — so the two ends of the loop agree on the
         * rate: the request goes out at most four times a second, and
         * the peer produces at most four IDRs a second in answer.
         *
         * Zero disables the gate.
         */
        std::int64_t min_pli_interval_us = 250'000;

        /**
         * The maximum number of NACK sequence numbers one report may
         * carry.
         *
         * The default (64) is roughly one NACK FCI's worth of sequence
         * numbers per report: the loss that a single RTCP packet can
         * describe without the report itself becoming a liability.
         *
         * Zero means "no NACKs at all"; a negative value means
         * "unbounded" (the pre-v1.3 behaviour, kept so a host that
         * wants it can ask for it).
         */
        std::int64_t max_nack_seqs = 64;

        /**
         * The maximum age of a pending report, in microseconds.
         *
         * A report that sat in the queue for longer than this is
         * dropped instead of being sent: the losses it names have
         * either been retransmitted already or are long gone.
         *
         * Zero means "never expire".
         */
        std::int64_t max_report_age_us = 1'000'000;
    };

    /** What the guard counted. */
    struct Counters {
        /** Key-frame requests raised, whichever door raised them. */
        std::uint64_t pli_requests = 0;
        /** Key-frame requests that passed the rate limit. */
        std::uint64_t pli_allowed = 0;
        /** Key-frame requests the rate limit dropped. */
        std::uint64_t pli_suppressed = 0;
        /** Key-frame requests raised by the **loss** door. */
        std::uint64_t pli_from_loss = 0;
        /** Key-frame requests raised by the **decode-failure** door. */
        std::uint64_t pli_from_decode_failure = 0;
        /** NACK sequence numbers offered to the guard. */
        std::uint64_t nack_seqs_offered = 0;
        /** NACK sequence numbers the cap discarded. */
        std::uint64_t nack_seqs_dropped = 0;
        /** Pending reports dropped for being too old. */
        std::uint64_t reports_expired = 0;
    };

    /** The guard's clock: microseconds on a monotonic timeline. */
    using ClockFn = std::function<std::int64_t()>;

    /** A guard with the default bounds and the real monotonic clock. */
    FeedbackGuard();

    /**
     * @brief A guard with an explicit clock.
     *
     * The engine passes the monotonic clock; the unit test passes a
     * virtual one, which is what makes the 30 s synthetic-loss run
     * deterministic.
     */
    explicit FeedbackGuard(ClockFn clock, Config cfg = Config{});

    /** Replace the bounds.  The clock is kept. */
    void set_config(const Config& cfg) noexcept { cfg_ = cfg; }

    /** The bounds in force. */
    const Config& config() const noexcept { return cfg_; }

    /** What the guard counted. */
    const Counters& counters() const noexcept { return counters_; }

    /** Forget every counter (a new session). */
    void reset_counters() noexcept { counters_ = Counters{}; }

    /**
     * @brief Fold a report into the pending one, under the bound.
     *
     * The report is *not* what goes on the wire: the guard caps the
     * NACK list, decides whether the key-frame request passes the rate
     * limit, and merges what is left into the pending report.
     *
     * @param report The report the receiver raised.
     * @param source Which door raised it.
     * @return What the report contributed (see `QueueResult`).
     */
    QueueResult queue(const plugins::VideoFeedbackReport& report,
                      FeedbackSource source);

    /**
     * @brief Take the pending report, if it is fresh enough to send.
     *
     * A report older than `max_report_age_us` is dropped (and counted
     * in `reports_expired`) instead of being returned.
     *
     * @param out Receives the pending report when there is one.
     * @return True when `out` was filled.
     */
    bool take(plugins::VideoFeedbackReport& out);

    /** Whether a pending report is waiting. */
    [[nodiscard]] bool has_pending() const noexcept { return has_pending_; }

    /** The number of sequence numbers the pending report carries. */
    [[nodiscard]] std::size_t pending_nack_count() const noexcept {
        return pending_.missing_seqs.size();
    }

    /** Whether the pending report asks for a key frame. */
    [[nodiscard]] bool pending_wants_keyframe() const noexcept {
        return pending_.want_keyframe;
    }

private:
    std::int64_t now_us() const;

    Config   cfg_{};
    ClockFn  clock_{};
    Counters counters_{};

    plugins::VideoFeedbackReport pending_{};
    bool         has_pending_      = false;
    std::int64_t pending_since_us_ = 0;
    std::int64_t last_pli_us_      = 0;
    bool         have_last_pli_    = false;
};

}  // namespace nimrtc::engine::detail
