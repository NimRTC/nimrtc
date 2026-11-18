/**
 * @file tests/test_engine_feedback_guard.cpp
 * @brief The **receive-side feedback bound** on its own (v1.3 plan Q4 /
 *        video plan T2).
 *
 * The bound itself is a private engine collaborator
 * (`src/engine/src/engine_feedback_guard.hpp`), so the test includes that
 * header directly — the same pattern as
 * `tests/test_engine_rate_window.cpp` (the rate window's *rule*) and
 * `tests/test_engine_capture_cadence.cpp` (the capture cadence's rule).
 *
 * ## Why the clock is injected
 *
 * Every rule in the guard is a function of time:
 *
 *   - the key-frame request is rate-limited to one per interval;
 *   - a pending report expires once it is too old to be worth sending.
 *
 * A wall-clock gate cannot be tested deterministically, so the guard
 * takes its clock as a parameter.  The engine passes the monotonic clock;
 * this test passes a *virtual* one, which it advances by hand.  The 30 s
 * synthetic-loss run below therefore takes microseconds of real time and
 * asserts the same numbers every time, on every machine.
 *
 * ## What the 30 s run asserts
 *
 * The acceptance for Q4 is that a lossy link does not turn the receiver's
 * feedback into an amplification storm.  A 30 s run at 30 fps with 30 %
 * loss raises ~900 key-frame requests; the bound must let at most
 *
 *     30 s / 250 ms + 1 = 121
 *
 * of them through, and must count every one it dropped.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "engine_feedback_guard.hpp"

namespace {

using nimrtc::engine::detail::FeedbackGuard;
using nimrtc::engine::detail::FeedbackSource;
namespace np = nimrtc::plugins;

/// A virtual clock the test advances by hand.
class VirtualClock {
public:
    std::int64_t now_us() const noexcept { return now_us_; }
    void advance_us(std::int64_t delta) noexcept { now_us_ += delta; }
    void advance_ms(std::int64_t delta) noexcept { advance_us(delta * 1000); }

private:
    std::int64_t now_us_ = 0;
};

/** A report that asks for a key frame (no NACK candidates). */
np::VideoFeedbackReport pli_report() {
    np::VideoFeedbackReport report{};
    report.want_keyframe = true;
    return report;
}

/** A report that names `count` missing sequence numbers, oldest first. */
np::VideoFeedbackReport nack_report(std::uint16_t first, std::size_t count) {
    np::VideoFeedbackReport report{};
    report.missing_seqs.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        report.missing_seqs.push_back(static_cast<std::uint16_t>(first + i));
    }
    return report;
}

// ---------------------------------------------------------------------------
// The key-frame rate limit
// ---------------------------------------------------------------------------

TEST(FeedbackGuard, the_first_key_frame_request_passes_immediately) {
    VirtualClock clock;
    FeedbackGuard guard([&] { return clock.now_us(); });

    const auto result = guard.queue(pli_report(), FeedbackSource::kLoss);

    EXPECT_TRUE(result.pli_allowed);
    EXPECT_TRUE(result.pli_added);
    EXPECT_FALSE(result.pli_suppressed);
    EXPECT_EQ(guard.counters().pli_requests, 1u);
    EXPECT_EQ(guard.counters().pli_allowed, 1u);
    EXPECT_EQ(guard.counters().pli_suppressed, 0u);
}

TEST(FeedbackGuard, a_second_request_inside_the_interval_is_suppressed) {
    VirtualClock clock;
    FeedbackGuard guard([&] { return clock.now_us(); });

    guard.queue(pli_report(), FeedbackSource::kLoss);

    // 100 ms later — still inside the 250 ms interval.
    clock.advance_ms(100);
    const auto result = guard.queue(pli_report(), FeedbackSource::kLoss);

    EXPECT_FALSE(result.pli_allowed);
    EXPECT_TRUE(result.pli_suppressed);
    EXPECT_EQ(guard.counters().pli_requests, 2u);
    EXPECT_EQ(guard.counters().pli_allowed, 1u);
    EXPECT_EQ(guard.counters().pli_suppressed, 1u);
}

TEST(FeedbackGuard, a_request_at_the_interval_boundary_passes) {
    VirtualClock clock;
    FeedbackGuard guard([&] { return clock.now_us(); });

    guard.queue(pli_report(), FeedbackSource::kLoss);

    // Exactly one interval later — the interval has elapsed, so the
    // request passes.  (The comparison is `>=`, not `>`, so a peer that
    // answers exactly on time is not penalised.)
    clock.advance_ms(250);
    const auto result = guard.queue(pli_report(), FeedbackSource::kLoss);

    EXPECT_TRUE(result.pli_allowed);
    EXPECT_EQ(guard.counters().pli_allowed, 2u);
}

TEST(FeedbackGuard, the_limit_is_global_across_both_doors) {
    VirtualClock clock;
    FeedbackGuard guard([&] { return clock.now_us(); });

    // The loss door fires.
    guard.queue(pli_report(), FeedbackSource::kLoss);

    // The decode-failure door fires 10 ms later — it must NOT get its own
    // allowance.  The peer's encoder is the shared resource the limit
    // protects: a per-door limit would let the two doors add up to twice
    // the rate.
    clock.advance_ms(10);
    const auto result = guard.queue(pli_report(), FeedbackSource::kDecodeFailure);

    EXPECT_FALSE(result.pli_allowed);
    EXPECT_TRUE(result.pli_suppressed);
    EXPECT_EQ(guard.counters().pli_suppressed, 1u);
}

TEST(FeedbackGuard, the_two_doors_are_counted_separately) {
    VirtualClock clock;
    FeedbackGuard guard([&] { return clock.now_us(); });

    guard.queue(pli_report(), FeedbackSource::kLoss);
    clock.advance_ms(300);
    guard.queue(pli_report(), FeedbackSource::kDecodeFailure);

    EXPECT_EQ(guard.counters().pli_requests, 2u);
    EXPECT_EQ(guard.counters().pli_from_loss, 1u);
    EXPECT_EQ(guard.counters().pli_from_decode_failure, 1u);
    EXPECT_EQ(guard.counters().pli_allowed, 2u);
}

TEST(FeedbackGuard, a_zero_interval_disables_the_limit) {
    VirtualClock clock;
    FeedbackGuard::Config cfg{};
    cfg.min_pli_interval_us = 0;
    FeedbackGuard guard([&] { return clock.now_us(); }, cfg);

    guard.queue(pli_report(), FeedbackSource::kLoss);
    clock.advance_ms(1);
    const auto result = guard.queue(pli_report(), FeedbackSource::kLoss);

    EXPECT_TRUE(result.pli_allowed);
    EXPECT_EQ(guard.counters().pli_allowed, 2u);
    EXPECT_EQ(guard.counters().pli_suppressed, 0u);
}

// ---------------------------------------------------------------------------
// The NACK cap
// ---------------------------------------------------------------------------

TEST(FeedbackGuard, the_nack_list_never_exceeds_the_cap) {
    VirtualClock clock;
    FeedbackGuard guard([&] { return clock.now_us(); });

    // Offer 200 sequence numbers into a report that may carry 64.
    guard.queue(nack_report(1000, 200), FeedbackSource::kLoss);

    EXPECT_EQ(guard.pending_nack_count(), 64u)
        << "the report must carry at most the configured number of sequence "
           "numbers";
    EXPECT_EQ(guard.counters().nack_seqs_offered, 200u);
    EXPECT_EQ(guard.counters().nack_seqs_dropped, 136u);
}

TEST(FeedbackGuard, the_cap_keeps_the_newest_losses) {
    VirtualClock clock;
    FeedbackGuard guard([&] { return clock.now_us(); });

    // Offer 1..200; only 64 fit.
    guard.queue(nack_report(1, 200), FeedbackSource::kLoss);

    np::VideoFeedbackReport out{};
    ASSERT_TRUE(guard.take(out));
    ASSERT_EQ(out.missing_seqs.size(), 64u);

    // The *newest* losses are the ones kept: RFC 4585 §6.2.1 shapes the
    // request around the recent losses, and the sender's retransmission
    // cache only holds the recent packets — a request for an old loss is
    // a request the peer cannot answer.
    EXPECT_EQ(out.missing_seqs.front(), 137u)
        << "the cap must keep the newest sequence numbers, not the oldest";
    EXPECT_EQ(out.missing_seqs.back(), 200u);
}

TEST(FeedbackGuard, the_cap_is_across_the_whole_pending_report) {
    VirtualClock clock;
    FeedbackGuard guard([&] { return clock.now_us(); });

    // Two reports that each fit, but not together.
    guard.queue(nack_report(1, 40), FeedbackSource::kLoss);
    guard.queue(nack_report(100, 40), FeedbackSource::kLoss);

    EXPECT_EQ(guard.pending_nack_count(), 64u)
        << "the cap bounds the *pending report*, not each report that goes "
           "into it";
    EXPECT_EQ(guard.counters().nack_seqs_dropped, 16u);
}

TEST(FeedbackGuard, a_negative_cap_means_unbounded) {
    VirtualClock clock;
    FeedbackGuard::Config cfg{};
    cfg.max_nack_seqs = -1;
    FeedbackGuard guard([&] { return clock.now_us(); }, cfg);

    guard.queue(nack_report(1, 200), FeedbackSource::kLoss);

    EXPECT_EQ(guard.pending_nack_count(), 200u);
    EXPECT_EQ(guard.counters().nack_seqs_dropped, 0u);
}

// ---------------------------------------------------------------------------
// The report's age
// ---------------------------------------------------------------------------

TEST(FeedbackGuard, a_fresh_pending_report_is_returned) {
    VirtualClock clock;
    FeedbackGuard guard([&] { return clock.now_us(); });

    guard.queue(pli_report(), FeedbackSource::kLoss);
    clock.advance_ms(100);

    np::VideoFeedbackReport out{};
    EXPECT_TRUE(guard.take(out));
    EXPECT_TRUE(out.want_keyframe);
    EXPECT_EQ(guard.counters().reports_expired, 0u);
}

TEST(FeedbackGuard, a_stale_pending_report_is_dropped_not_sent) {
    VirtualClock clock;
    FeedbackGuard guard([&] { return clock.now_us(); });

    guard.queue(pli_report(), FeedbackSource::kLoss);
    // 2 s later — the report is older than the 1 s maximum age.
    clock.advance_ms(2000);

    np::VideoFeedbackReport out{};
    EXPECT_FALSE(guard.take(out))
        << "a report that says nothing about the picture any more must not "
           "be sent";
    EXPECT_EQ(guard.counters().reports_expired, 1u);
    EXPECT_FALSE(guard.has_pending());
}

TEST(FeedbackGuard, an_expired_report_does_not_keep_the_next_one_out) {
    VirtualClock clock;
    FeedbackGuard guard([&] { return clock.now_us(); });

    guard.queue(pli_report(), FeedbackSource::kLoss);
    clock.advance_ms(2000);

    np::VideoFeedbackReport out{};
    ASSERT_FALSE(guard.take(out));

    // The gate reopens: a request that was never sent must not keep the
    // next one out.
    const auto result = guard.queue(pli_report(), FeedbackSource::kLoss);
    EXPECT_TRUE(result.pli_allowed)
        << "a request that was dropped for staleness must not consume the "
           "next request's allowance";
}

TEST(FeedbackGuard, a_zero_age_means_never_expire) {
    VirtualClock clock;
    FeedbackGuard::Config cfg{};
    cfg.max_report_age_us = 0;
    FeedbackGuard guard([&] { return clock.now_us(); }, cfg);

    guard.queue(pli_report(), FeedbackSource::kLoss);
    clock.advance_ms(60'000);

    np::VideoFeedbackReport out{};
    EXPECT_TRUE(guard.take(out));
    EXPECT_EQ(guard.counters().reports_expired, 0u);
}

// ---------------------------------------------------------------------------
// The 30 s synthetic-loss run (the Q4 acceptance)
// ---------------------------------------------------------------------------

TEST(FeedbackGuard, a_thirty_second_lossy_run_does_not_storm) {
    VirtualClock clock;
    FeedbackGuard guard([&] { return clock.now_us(); });

    // ---- The scenario ---------------------------------------------------
    //
    // 30 fps for 30 s, with 30 % of the frames lost.  Every lost frame
    // makes the jitter buffer raise a key-frame request — the *demand*,
    // unthrottled.  900 frames, ~270 of them lost.
    //
    // The bound is what stands between that demand and the peer: without
    // it, the receiver would ask for 270 key frames in 30 s, and the
    // peer would answer with 270 IDRs — each one bigger than the frame
    // it replaced.
    constexpr std::int64_t kFrameIntervalUs = 33'333;   // 30 fps
    constexpr std::int64_t kRunUs            = 30'000'000;  // 30 s
    constexpr std::int64_t kMinPliIntervalUs = 250'000;   // 4 Hz
    constexpr std::size_t   kCap             = 64;
    // Each lost frame names 100 missing packets — a burst far wider than
    // one report may carry, so the cap really bites (a single report may
    // not hold 100 sequence numbers).
    constexpr std::size_t   kMissingPerLoss  = 100;

    std::uint64_t frames = 0;
    std::uint64_t lost   = 0;
    std::size_t   max_pending_nacks = 0;
    std::size_t   sent_seqs    = 0;   // the candidates that went out
    std::size_t   reports_sent = 0;   // the reports that went out

    for (std::int64_t t = 0; t < kRunUs; t += kFrameIntervalUs) {
        clock.advance_us(kFrameIntervalUs);
        ++frames;

        // Every third frame is lost (30 %).
        const bool is_lost = (frames % 10) >= 7;
        if (!is_lost) continue;
        ++lost;

        // The loss report: the missing packets *and* the request for a
        // key frame (the picture is incomplete).
        np::VideoFeedbackReport report = nack_report(
            static_cast<std::uint16_t>(frames & 0xFFFF), kMissingPerLoss);
        report.want_keyframe = true;

        const auto result = guard.queue(report, FeedbackSource::kLoss);
        EXPECT_TRUE(result.pli_allowed || result.pli_suppressed)
            << "a key-frame request must be either allowed or suppressed";

        // The pending report never carries more than the cap.
        const std::size_t pending = guard.pending_nack_count();
        max_pending_nacks = std::max(max_pending_nacks, pending);
        EXPECT_LE(pending, kCap)
            << "the pending report exceeded the NACK cap at t=" << t;

        // The engine flushes the report roughly every 20 ms; the run
        // flushes at the same cadence so the pending report does not
        // simply accumulate.
        if ((t / 20'000) != ((t - kFrameIntervalUs) / 20'000)) {
            np::VideoFeedbackReport out{};
            if (guard.take(out)) {
                EXPECT_LE(out.missing_seqs.size(), kCap);
                sent_seqs += out.missing_seqs.size();
                ++reports_sent;
            }
        }
    }

    // Flush whatever is left.
    np::VideoFeedbackReport last{};
    if (guard.take(last)) {
        EXPECT_LE(last.missing_seqs.size(), kCap);
        sent_seqs += last.missing_seqs.size();
        ++reports_sent;
    }

    // ---- The demand -----------------------------------------------------
    EXPECT_GT(lost, 250u) << "the scenario must actually be lossy";
    EXPECT_EQ(guard.counters().pli_requests, lost)
        << "every lost frame must be counted as a key-frame request";

    // ---- The bound ------------------------------------------------------
    //
    // 30 s at 4 Hz is 120 intervals; the first request is immediate, so
    // at most 121 requests may go out.
    constexpr std::uint64_t kMaxAllowed = 121;
    EXPECT_LE(guard.counters().pli_allowed, kMaxAllowed)
        << "the rate limit let more key-frame requests through than the "
           "acceptance allows";

    // The demand is *bursty* — three lost frames in a row, then a clean
    // stretch — so the limiter lets one request through per burst, not
    // one per interval: the bursts are 333 ms apart, and each burst spans
    // only 67 ms, so the first request of a burst passes and the other two
    // are held back.  The lower bound says the limiter is not so tight
    // that the receiver stops asking for the key frames it needs.
    EXPECT_GE(guard.counters().pli_allowed, 80u)
        << "the rate limit must not be so tight that the receiver stops "
           "asking for the key frames it needs";

    // Every request is accounted for: allowed + suppressed == requested.
    EXPECT_EQ(guard.counters().pli_allowed + guard.counters().pli_suppressed,
              guard.counters().pli_requests)
        << "a request may not vanish without being counted";

    // ---- The NACK bound -------------------------------------------------
    //
    // The candidates that were *offered* vastly exceed the candidates that
    // could go out: every lost frame names 100 missing packets, and one
    // report may carry 64.  What matters is that every report that went
    // out stayed within the cap.
    EXPECT_GT(guard.counters().nack_seqs_offered, 0u);
    EXPECT_GT(guard.counters().nack_seqs_dropped, 0u)
        << "the cap must have bitten at least once — otherwise the "
           "scenario never exercised it";
    EXPECT_EQ(guard.counters().nack_seqs_offered,
              guard.counters().nack_seqs_dropped + sent_seqs)
        << "every offered sequence number must be either dropped or sent";
    EXPECT_LE(sent_seqs, kCap * reports_sent)
        << "the reports that went out must not have carried more than the "
           "cap each";

    // ---- No IDR storm ---------------------------------------------------
    //
    // The peer answers every key-frame request with an IDR, so the number
    // of IDRs the peer produces in 30 s is the number of PLIs we sent.
    // The acceptance is that it stays at or below 121.
    EXPECT_LE(guard.counters().pli_allowed, kMaxAllowed)
        << "the receiver asked for a key frame storm";
}

// ---------------------------------------------------------------------------
// The worst case: a link that loses *everything*
// ---------------------------------------------------------------------------
//
// The 30 % run above is the realistic case.  This one is the adversarial
// one: every single frame is lost, so the receiver raises a key-frame
// request on *every* frame — 900 of them in 30 s, unthrottled.
//
// Without the bound the peer would answer with 900 IDRs in 30 s, each one
// bigger than the frame it replaced, and the link would spend all its
// bandwidth on key frames.  With the bound the peer sees at most 121.

TEST(FeedbackGuard, a_total_loss_run_does_not_storm_either) {
    VirtualClock clock;
    FeedbackGuard guard([&] { return clock.now_us(); });

    constexpr std::int64_t kFrameIntervalUs = 33'333;
    constexpr std::int64_t kRunUs            = 30'000'000;

    std::uint64_t frames = 0;
    for (std::int64_t t = 0; t < kRunUs; t += kFrameIntervalUs) {
        clock.advance_us(kFrameIntervalUs);
        ++frames;

        np::VideoFeedbackReport report = nack_report(
            static_cast<std::uint16_t>(frames & 0xFFFF), 20);
        report.want_keyframe = true;
        guard.queue(report, FeedbackSource::kLoss);
    }

    // 901 frames (30 s at 33 333 µs), each one lost; at most 121 of the
    // requests may go out.
    EXPECT_EQ(guard.counters().pli_requests, 901u);
    EXPECT_LE(guard.counters().pli_allowed, 121u)
        << "the receiver asked the peer for a key frame storm";
    // The frames are on a 33.3 ms grid, so a request can only go out on a
    // frame boundary that is at least 250 ms after the previous one: that
    // is every 8th frame, so 113 in 30 s.  The point of the lower bound
    // is that the limiter must not starve the peer of key frames.
    EXPECT_GE(guard.counters().pli_allowed, 110u)
        << "the limiter must let a request through every interval — the "
           "peer must not be left without a key frame";
    EXPECT_EQ(guard.counters().pli_allowed + guard.counters().pli_suppressed,
              guard.counters().pli_requests)
        << "every request must be either allowed or suppressed";
}

// ---------------------------------------------------------------------------
// The report's own shape
// ---------------------------------------------------------------------------

TEST(FeedbackGuard, an_empty_report_leaves_nothing_pending) {
    VirtualClock clock;
    FeedbackGuard guard([&] { return clock.now_us(); });

    guard.queue(np::VideoFeedbackReport{}, FeedbackSource::kLoss);

    EXPECT_FALSE(guard.has_pending());
    np::VideoFeedbackReport out{};
    EXPECT_FALSE(guard.take(out));
}

TEST(FeedbackGuard, taking_the_report_clears_it) {
    VirtualClock clock;
    FeedbackGuard guard([&] { return clock.now_us(); });

    guard.queue(pli_report(), FeedbackSource::kLoss);
    ASSERT_TRUE(guard.has_pending());

    np::VideoFeedbackReport out{};
    ASSERT_TRUE(guard.take(out));

    EXPECT_FALSE(guard.has_pending());
    EXPECT_FALSE(guard.take(out))
        << "a taken report must not be taken twice";
}

TEST(FeedbackGuard, the_counters_can_be_reset) {
    VirtualClock clock;
    FeedbackGuard guard([&] { return clock.now_us(); });

    guard.queue(pli_report(), FeedbackSource::kLoss);
    ASSERT_EQ(guard.counters().pli_requests, 1u);

    guard.reset_counters();
    EXPECT_EQ(guard.counters().pli_requests, 0u);
}

}  // namespace
