/**
 * @file src/modules/bwe/src/arrival_trend.hpp
 * @brief Internal helper — the transport-cc delay trend (ADR-018 Decision 4).
 *
 * ## Why this file exists
 *
 * ADR-018 §"Decision 4 — What the transport-cc path actually measures"
 * says the transport-cc path derives three things from the arrival-time
 * series: the **receive rate**, the **delay trend**, and the **loss**.
 *
 * The first and the third are derived by the collector
 * (`src/modules/net_feedback/`, which owns the wire format and the
 * aggregation).  The delay trend is a property of the *estimator's
 * algorithm* — it is the congestion signal, not a measurement — so it is
 * derived here, inside the module that owns the algorithm.  This header is
 * module-internal: it is not installed, not part of the public API, and
 * carries no stability marker.
 *
 * ## What the trend is
 *
 * The **inter-arrival spacing** of the received media packets.  On an
 * uncongested path the spacing is the send spacing.  When a queue builds
 * up in front of a bottleneck the packets leave it later, and the spacing
 * grows.  The trend therefore compares the current spacing against the
 * smallest spacing seen recently: a *rise* is the congestion signal that
 * loss-based feedback cannot produce — it arrives before the packets start
 * being dropped (ADR-018 §"The two signals are not interchangeable").
 *
 * ## The caveats, stated plainly
 *
 * * The spacing grows for two reasons: queue buildup (congestion) and a
 *   slower send rate (the sender throttled itself).  The arrival series
 *   alone cannot tell them apart.  The estimator therefore **invalidates
 *   the baseline whenever it moves its own target bitrate**: the send
 *   spacing follows the target, so the old baseline describes a different
 *   operating point, and comparing against it would turn the estimator's
 *   own action into a congestion signal (a decrease spiral).  A rate
 *   change driven by anything else — the future rate controller, the
 *   pacer — is invisible to the estimator; the send-side accounting of
 *   v1.2.0 M3/M5 is what will close that gap.
 * * The **first** delta of a transport-cc feedback straddles the gap
 *   between two feedback packets (it is measured from the previous
 *   feedback's reference time), so it is not a packet spacing and is
 *   excluded.
 * * A **non-positive** delta means the packet arrived *before* its
 *   predecessor (reordering).  That is not a spacing either, and it is
 *   excluded — which is exactly why transport-cc survives reordering
 *   where a fraction-lost counter does not.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace nimrtc::bwe::detail {

/// The transport-cc delay trend.
///
/// Not thread-safe; the estimator owns one and drives it from the engine
/// thread.
class ArrivalTrend {
public:
    /// The outcome of one arrival-series analysis.
    struct Sample {
        /// The series carried enough packets to form a spacing.
        bool valid = false;
        /// The spacing grew beyond the threshold: the queue is building.
        bool overuse = false;
        /// The mean inter-arrival spacing of this sample, in milliseconds.
        double spacing_ms = 0.0;
        /// The baseline the spacing was compared against, in milliseconds.
        double baseline_ms = 0.0;
    };

    /// Analyse one transport-cc arrival series.
    ///
    /// @param arrival_deltas_ms  `BweFeedback::arrival_deltas_ms`: the
    ///                           per-packet arrival deltas, in
    ///                           milliseconds.
    Sample update(const std::vector<double>& arrival_deltas_ms) {
        // The spacing samples are the deltas *between* two received
        // packets.  The first delta straddles the gap between two
        // feedback packets, so it is skipped; a non-positive delta is a
        // reordering, so it is skipped as well.
        double sum = 0.0;
        std::size_t count = 0;
        for (std::size_t i = 1; i < arrival_deltas_ms.size(); ++i) {
            const double delta = arrival_deltas_ms[i];
            if (delta <= 0.0) {
                continue;
            }
            sum += delta;
            ++count;
        }

        Sample sample;
        if (count < kMinSpacingSamples) {
            // Too few packets: ADR-018 Decision 4 forbids driving the
            // estimate from a single transport-cc sample.
            return sample;
        }

        sample.valid = true;
        sample.spacing_ms = sum / static_cast<double>(count);

        if (window_.empty()) {
            // The first series establishes the baseline; there is nothing
            // to compare it against yet.
            window_.push_back(sample.spacing_ms);
            sample.baseline_ms = sample.spacing_ms;
            return sample;
        }

        double baseline = window_.front();
        for (const double spacing : window_) {
            baseline = (std::min)(baseline, spacing);
        }
        sample.baseline_ms = baseline;

        const double growth = sample.spacing_ms - baseline;
        sample.overuse = (growth > 0.0) &&
                         (sample.spacing_ms > baseline * kOveruseRatio) &&
                         (growth >= kOveruseAbsoluteMs);

        window_.push_back(sample.spacing_ms);
        if (window_.size() > kBaselineWindow) {
            window_.erase(window_.begin());
        }
        return sample;
    }

    /// Drop the baseline.
    ///
    /// The caller calls this whenever the send rate changed *by
    /// construction* — that is, whenever the estimator moved its own
    /// target bitrate — and whenever the arrival series stopped being
    /// continuous (the policy fell back to the loss/RTT path).  The
    /// baseline describes one operating point; after a rate change it
    /// describes another.
    void invalidate() noexcept { window_.clear(); }

    /// Forget everything (session restart).
    void reset() noexcept { window_.clear(); }

private:
    /// How many recent samples the baseline is drawn from.
    static constexpr std::size_t kBaselineWindow = 8;
    /// How many spacings a series must carry to be usable.
    static constexpr std::size_t kMinSpacingSamples = 4;
    /// How much the spacing must grow, relative to the baseline, to count
    /// as overuse.
    static constexpr double kOveruseRatio = 1.25;
    /// The absolute floor of that growth, in milliseconds.  The arrival
    /// timestamps are quantized to 250 us, so a smaller growth is noise.
    static constexpr double kOveruseAbsoluteMs = 0.5;

    /// The recent sample means, oldest first.
    std::vector<double> window_;
};

}  // namespace nimrtc::bwe::detail
