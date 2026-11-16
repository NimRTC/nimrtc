/**
 * @file src/engine/src/engine_stage_timing.hpp
 * @brief The per-stage CPU-time instrumentation (remote-desktop plan J1).
 *
 * ## What this is
 *
 * The plan's exit measurement is written in terms of *time*:
 *
 *   > "CPU time per frame (capture → encode → packetize) — within §B.6's
 *   >  budget on the hardware path; within 2× of it on the software path"
 *
 * Nothing in the tree measured that, so the budget was unenforced.  This
 * file is the measurement.
 *
 * ## The shape
 *
 * Each stage of the pipeline is a `Stage` enum value.  For every stage
 * the engine keeps:
 *
 *   - the total time spent in that stage,
 *   - the number of samples, and
 *   - the largest single sample.
 *
 * The **mean** is `total / samples`; the **p95** is computed on demand
 * from a bounded reservoir of the most recent samples (see
 * `StageTimings::percentile_us()`).
 *
 * ## Why the internal unit is nanoseconds
 *
 * The *reported* unit is microseconds — that is the unit the plan's
 * budget is written in, and it is what a host wants to read.  But the
 * *stored* unit is nanoseconds, and the conversion happens at the
 * boundary.
 *
 * The reason is the fast stages.  Popping a frame out of the capture
 * device's queue takes a couple of hundred nanoseconds — genuinely
 * sub-microsecond work.  Measured in microseconds and truncated, that
 * is **zero**, and a zero reading is indistinguishable from "the stage
 * never ran".  Storing nanoseconds keeps the resolution, so the totals
 * are right and the means are right; only the *presentation* rounds.
 *
 * ## Why the reservoir is bounded
 *
 * A p95 over *all* samples would need every sample kept forever, which
 * is a leak in a long-running session.  The reservoir keeps the last
 * `kReservoirSize` samples in a ring buffer, so the p95 is the p95 of
 * the *recent* past — which is what an operator watching a live session
 * wants ("how long is this frame taking *now*?"), and it costs a fixed
 * amount of memory.
 *
 * ## The cost of measuring
 *
 * A `std::chrono::steady_clock` pair is two reads of a monotonic clock —
 * on Windows that is `QueryPerformanceCounter`, roughly 20–30 ns.  Over
 * a 1080p30 frame budget of 33 ms that is 0.001 %, so the measurement
 * does not perturb what it measures.  The clock is read only around the
 * stages, never inside the codec.
 *
 * ## Threading
 *
 * The engine is single-threaded (RD-6): the tick thread is the only
 * thread that touches these counters.  `StageTimings` is therefore not
 * synchronised — it is a plain value type owned by the engine's state.
 */

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

namespace nimrtc::engine {

/**
 * @brief The pipeline stages the engine measures.
 *
 * The order matches the plan's §B.6 budget table.  A stage's `kNone`
 * value is "not measured" and is reported as zero rather than as a
 * misleading average.
 */
enum class Stage : std::uint8_t {
    kNone = 0,
    kCapture = 1,  ///< popping the frame from the capture device
    kConvert = 2,  ///< colour / format conversion (BGRA → I420, scaling)
    kEncode = 3,   ///< the codec's encode call
    kPacketize = 4,///< RTP packetization (STAP-A / FU-A) + SRTP
    kDecode = 5,   ///< the codec's decode call
    kRender = 6,   ///< the sink's render call
    kCount = 7,
};

/**
 * @brief The number of stages (the size of every per-stage array).
 */
inline constexpr std::size_t kStageCount = static_cast<std::size_t>(Stage::kCount);

/**
 * @brief The per-stage timing accumulator.
 *
 * A plain value type: the engine owns one and it is only ever touched
 * from the tick thread.
 */
class StageTimings {
public:
    /**
     * @brief How many recent samples the percentile reservoir keeps.
     *
     * 512 samples at 30 fps is ~17 seconds of history — long enough for
     * a p95 to be meaningful, short enough that the memory cost (a few
     * kilobytes) never matters.
     */
    static constexpr std::size_t kReservoirSize = 512;

    /** A running timer for one stage. */
    class Scope {
    public:
        Scope(StageTimings& owner, Stage stage) noexcept
            : owner_(&owner), stage_(stage),
              start_(std::chrono::steady_clock::now()) {}
        ~Scope() {
            const auto end = std::chrono::steady_clock::now();
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                end - start_).count();
            owner_->record(stage_, static_cast<std::uint64_t>(ns < 0 ? 0 : ns));
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        StageTimings* owner_;
        Stage        stage_;
        std::chrono::steady_clock::time_point start_;
    };

    /**
     * @brief Record one sample for a stage.
     *
     * @param stage  The stage.
     * @param nanos  The sample's duration in **nanoseconds**.
     */
    void record(Stage stage, std::uint64_t nanos) noexcept {
        const auto idx = static_cast<std::size_t>(stage);
        if (idx >= kStageCount) return;
        totals_ns_[idx] += nanos;
        maxima_ns_[idx] = std::max(maxima_ns_[idx], nanos);
        ++samples_[idx];

        // The reservoir: a ring buffer of the most recent samples, used
        // for the percentile.  It is per-stage so that a stage which
        // runs once per frame and a stage which runs once per keyframe
        // do not evict each other.
        auto& ring = reservoirs_[idx];
        ring[ring_head_[idx]] = nanos;
        ring_head_[idx] = (ring_head_[idx] + 1) % kReservoirSize;
        if (ring_count_[idx] < kReservoirSize) ++ring_count_[idx];
    }

    /** The total time spent in a stage, in microseconds. */
    [[nodiscard]] std::uint64_t total_us(Stage stage) const noexcept {
        const auto idx = static_cast<std::size_t>(stage);
        return idx < kStageCount ? ns_to_us(totals_ns_[idx]) : 0;
    }

    /**
     * @brief The total time spent in a stage, in nanoseconds.
     *
     * The exact value: the microsecond view rounds, so a stage that
     * costs less than a microsecond reports zero there.
     */
    [[nodiscard]] std::uint64_t total_ns(Stage stage) const noexcept {
        const auto idx = static_cast<std::size_t>(stage);
        return idx < kStageCount ? totals_ns_[idx] : 0;
    }

    /** The largest single sample for a stage, in nanoseconds. */
    [[nodiscard]] std::uint64_t max_ns(Stage stage) const noexcept {
        const auto idx = static_cast<std::size_t>(stage);
        return idx < kStageCount ? maxima_ns_[idx] : 0;
    }

    /** The mean time for a stage, in nanoseconds (0 when never sampled). */
    [[nodiscard]] std::uint64_t mean_ns(Stage stage) const noexcept {
        const auto idx = static_cast<std::size_t>(stage);
        if (idx >= kStageCount) return 0;
        const auto n = samples_[idx];
        return n ? totals_ns_[idx] / n : 0;
    }

    /** The number of samples recorded for a stage. */
    [[nodiscard]] std::uint64_t samples(Stage stage) const noexcept {
        const auto idx = static_cast<std::size_t>(stage);
        return idx < kStageCount ? samples_[idx] : 0;
    }

    /** The largest single sample for a stage, in microseconds. */
    [[nodiscard]] std::uint64_t max_us(Stage stage) const noexcept {
        const auto idx = static_cast<std::size_t>(stage);
        return idx < kStageCount ? ns_to_us(maxima_ns_[idx]) : 0;
    }

    /** The mean time for a stage, in microseconds (0 when never sampled). */
    [[nodiscard]] std::uint64_t mean_us(Stage stage) const noexcept {
        const auto idx = static_cast<std::size_t>(stage);
        if (idx >= kStageCount) return 0;
        const auto n = samples_[idx];
        return n ? ns_to_us(totals_ns_[idx] / n) : 0;
    }

    /**
     * @brief The `percentile`-th percentile of the recent samples.
     *
     * @param stage       The stage.
     * @param percentile  0..100.
     * @return The value in microseconds, or 0 when the stage was never
     *         sampled.
     *
     * The percentile is computed over the reservoir (the most recent
     * `kReservoirSize` samples), not over all history — see the file
     * header for why.
     */
    [[nodiscard]] std::uint64_t percentile_us(Stage stage,
                                             unsigned percentile) const noexcept {
        const auto idx = static_cast<std::size_t>(stage);
        if (idx >= kStageCount) return 0;
        const auto n = ring_count_[idx];
        if (n == 0) return 0;
        percentile = std::min(percentile, 100u);

        auto sorted = samples_sorted(idx);
        if (sorted.empty()) return 0;
        // Nearest-rank: the smallest value at or above the requested
        // percentile.  For p95 with 512 samples that is index 486.
        const std::size_t rank =
            static_cast<std::size_t>((percentile * sorted.size() + 99) / 100);
        const std::size_t pos = rank == 0 ? 0 : rank - 1;
        return ns_to_us(sorted[std::min(pos, sorted.size() - 1)]);
    }

    /** Forget every sample (used when the session restarts). */
    void reset() noexcept {
        totals_ns_.fill(0);
        maxima_ns_.fill(0);
        samples_.fill(0);
        for (std::size_t i = 0; i < kStageCount; ++i) {
            reservoirs_[i].fill(0);
            ring_head_[i] = 0;
            ring_count_[i] = 0;
        }
    }

private:
    /** Nanoseconds → microseconds, rounded to nearest. */
    static std::uint64_t ns_to_us(std::uint64_t nanos) noexcept {
        return (nanos + 500u) / 1000u;
    }

    /** A sorted copy of the reservoir for one stage. */
    [[nodiscard]] std::vector<std::uint64_t> samples_sorted(std::size_t idx) const {
        std::vector<std::uint64_t> out;
        const auto n = ring_count_[idx];
        if (n == 0) return out;
        out.reserve(n);
        // The samples live at indices [head - n, head) modulo the ring
        // size, oldest first: `head` points one past the newest sample.
        // (Getting this wrong reads the *empty* part of the ring while
        // the ring is not yet full, which silently corrupts the
        // percentile.)
        const std::size_t start = (ring_head_[idx] + kReservoirSize - n) % kReservoirSize;
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(reservoirs_[idx][(start + i) % kReservoirSize]);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    std::array<std::uint64_t, kStageCount> totals_ns_{};
    std::array<std::uint64_t, kStageCount> maxima_ns_{};
    std::array<std::uint64_t, kStageCount> samples_{};
    std::array<std::array<std::uint64_t, kReservoirSize>, kStageCount> reservoirs_{};
    std::array<std::size_t, kStageCount> ring_head_{};
    std::array<std::size_t, kStageCount> ring_count_{};
};

}  // namespace nimrtc::engine
