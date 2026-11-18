/**
 * @file src/engine/src/engine_feedback_guard.cpp
 * @brief PRIVATE engine collaborator: the **receive-side feedback bound**
 *        (v1.3 plan Q4 / video plan T2) — the implementation.
 *
 * The rules and the reasoning live in `engine_feedback_guard.hpp`; this
 * file is the mechanism.
 */

#include "engine_feedback_guard.hpp"

#include <chrono>

namespace nimrtc::engine::detail {

namespace {

/// The engine's monotonic clock, in microseconds.
std::int64_t steady_now_us() noexcept {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

FeedbackGuard::FeedbackGuard()
    : FeedbackGuard([] { return steady_now_us(); }, Config{}) {}

FeedbackGuard::FeedbackGuard(ClockFn clock, Config cfg)
    : cfg_(cfg), clock_(std::move(clock)) {
    if (!clock_) clock_ = [] { return steady_now_us(); };
}

std::int64_t FeedbackGuard::now_us() const {
    return clock_ ? clock_() : 0;
}

FeedbackGuard::QueueResult FeedbackGuard::queue(
    const plugins::VideoFeedbackReport& report, FeedbackSource source) {
    QueueResult result;
    const std::int64_t now = now_us();

    // ---- The key-frame request ------------------------------------------
    //
    // The rate limit is *global*: whichever door raised the request,
    // the peer sees at most one per interval.  The two doors are
    // counted separately so that a host can tell which one is firing.
    if (report.want_keyframe) {
        ++counters_.pli_requests;
        if (source == FeedbackSource::kLoss) {
            ++counters_.pli_from_loss;
        } else {
            ++counters_.pli_from_decode_failure;
        }

        const bool allowed =
            (cfg_.min_pli_interval_us <= 0) ||
            !have_last_pli_ ||
            (now - last_pli_us_) >= cfg_.min_pli_interval_us;

        if (allowed) {
            ++counters_.pli_allowed;
            last_pli_us_   = now;
            have_last_pli_ = true;
            result.pli_allowed = true;
            if (!pending_.want_keyframe) {
                pending_.want_keyframe = true;
                result.pli_added = true;
            }
        } else {
            ++counters_.pli_suppressed;
            result.pli_suppressed = true;
        }
    }

    // ---- The NACK candidates --------------------------------------------
    //
    // The list is capped, and the *oldest* entries are the ones
    // dropped: RFC 4585 §6.2.1 shapes the request around the recent
    // losses, and the sender's retransmission cache only holds the
    // recent packets, so a request for an old loss is a request the
    // peer cannot answer.
    if (!report.missing_seqs.empty()) {
        counters_.nack_seqs_offered += report.missing_seqs.size();

        const bool unbounded = cfg_.max_nack_seqs < 0;
        const std::size_t cap = unbounded
            ? static_cast<std::size_t>(-1)
            : static_cast<std::size_t>(cfg_.max_nack_seqs);

        const std::size_t held = pending_.missing_seqs.size();
        const std::size_t room = (cap > held) ? (cap - held) : 0;

        if (room == 0) {
            // No room at all: every candidate is dropped.
            counters_.nack_seqs_dropped += report.missing_seqs.size();
            result.nack_seqs_dropped = report.missing_seqs.size();
        } else if (report.missing_seqs.size() > room) {
            // Only the *newest* candidates fit — the ones the sender's
            // cache is most likely to still hold.
            const std::size_t dropped = report.missing_seqs.size() - room;
            counters_.nack_seqs_dropped += dropped;
            result.nack_seqs_dropped = dropped;
            pending_.missing_seqs.insert(
                pending_.missing_seqs.end(),
                report.missing_seqs.end() -
                    static_cast<std::ptrdiff_t>(room),
                report.missing_seqs.end());
            result.nack_seqs_added = room;
        } else {
            pending_.missing_seqs.insert(
                pending_.missing_seqs.end(),
                report.missing_seqs.begin(),
                report.missing_seqs.end());
            result.nack_seqs_added = report.missing_seqs.size();
        }
    }

    if (!has_pending_ &&
        (!pending_.missing_seqs.empty() || pending_.want_keyframe)) {
        has_pending_      = true;
        pending_since_us_ = now;
    }

    return result;
}

bool FeedbackGuard::take(plugins::VideoFeedbackReport& out) {
    if (!has_pending_) return false;

    // ---- The age check ----------------------------------------------------
    //
    // A report that sat in the queue while the link was down says
    // nothing about the picture any more, and sending it costs a round
    // trip: the losses it names have either been retransmitted already
    // or are long gone.
    if (cfg_.max_report_age_us > 0) {
        const std::int64_t age = now_us() - pending_since_us_;
        if (age > cfg_.max_report_age_us) {
            ++counters_.reports_expired;
            pending_ = plugins::VideoFeedbackReport{};
            has_pending_ = false;
            // The gate reopens: a request that was never sent must not
            // keep the *next* one out.
            have_last_pli_ = false;
            return false;
        }
    }

    out = pending_;
    pending_ = plugins::VideoFeedbackReport{};
    has_pending_ = false;
    return true;
}

}  // namespace nimrtc::engine::detail
