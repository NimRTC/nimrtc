/**
 * @file nimrtc/sched/sched.hpp
 * @brief Unified sending scheduler — L1 packet prioritisation (§8.2 / §8.3).
 *
 * Design:
 *   RTP (media) and DataChannel (data) share one priority queue.
 *   Strict descending priority ensures control messages survive congestion.
 *
 * Priority table:
 *   kControl       — control channel, never dropped by scheduler
 *   kAudio         — audio frame
 *   kVideoKeyframe — video I-frame (key frame)
 *   kVideo         — regular video frame
 *   kBestEffort    — telemetry / best-effort data
 *
 * The module re-exports the plugin types (`nimrtc::plugins::Priority`,
 * `SchedulerConfig`, `SchedulerStats`, `DrainCallback`, `IScheduler`) under
 * `nimrtc::sched::` so callers that link only `nimrtc::sched` see the same
 * types everyone else sees. The concrete default implementation is
 * `nimrtc::sched::Scheduler`, which inherits from `plugins::IScheduler`.
 *
 * Thread safety: the implementation guards internal state with a mutex;
 * callers must still provide their own memory barriers if the caller
 * and the scheduler tick run on different threads.
 *
 * @note P1 — interface is stable; engine integration lands in P2 (§13).
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>

#include <nimrtc/core/bytes.hpp>    // core::ByteSpan / core::ByteBuffer
#include <nimrtc/core/time.hpp>     // core::TimePoint / core::SteadyClock
#include <nimrtc/plugins/base.hpp>  // plugins::Addr
#include <nimrtc/plugins/scheduler.hpp>  // plugins::IScheduler + types

// Re-export plugin types under nimrtc::sched:: for backwards-compatibility.
namespace nimrtc::sched {
    using Priority           = plugins::Priority;
    using SchedulerConfig    = plugins::SchedulerConfig;
    using SchedulerStats     = plugins::SchedulerStats;
    using DrainCallback      = plugins::DrainCallback;
    using IScheduler         = plugins::IScheduler;
    constexpr std::size_t kPriorityCount = plugins::kPriorityCount;

    // NOTE: `plugins::priority_rank` is a free function in `nimrtc::plugins::`
    // and is picked up through ADL on the `Priority` argument (which is
    // `plugins::Priority`). We do NOT re-declare it here to avoid shadowing
    // ambiguity.
    //
    // `sched::lower_priority()` below is *our* helper that walks one priority
    // step down and returns std::nullopt at the bottom. It is not re-exported
    // from `plugins::` because priority traversal is a sched-module concept.

    /** Return the next lower priority, or std::nullopt at the bottom. */
    [[nodiscard]] constexpr std::optional<Priority> lower_priority(Priority p) noexcept {
        if (p == Priority::kBestEffort) return std::nullopt;
        return static_cast<Priority>(static_cast<std::uint8_t>(p) + 1);
    }
}

// =============================================================================
// nimrtc::sched — concrete default implementation
// =============================================================================
namespace nimrtc::sched {

/** Strict-priority default implementation.
 *
 *  Used by the default `"strict_priority"` plugin id. A weighted-fair or
 *  EDF scheduler can register under a different id and the engine will
 *  resolve it through `core::PluginRegistry::get_scheduler()`. */
class Scheduler final : public plugins::IScheduler {
public:
    explicit Scheduler(plugins::SchedulerConfig config = {});
    ~Scheduler() override;

    Scheduler(const Scheduler&)            = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&)                 noexcept;
    Scheduler& operator=(Scheduler&&)      noexcept;

    // plugins::IPlugin --------------------------------------------------
    const char*       name()  const noexcept override;
    plugins::Status   open()        noexcept override;
    void              close()       noexcept override;

    // plugins::IScheduler -----------------------------------------------
    void              enqueue(plugins::Priority p, core::ByteSpan data,
                              plugins::Addr dst) noexcept override;
    int               drain(int max_packets) noexcept override;
    int               drain_with(int max_packets,
                                plugins::DrainCallback cb) noexcept override;
    void              on_bwe_update(std::uint32_t target_bps) noexcept override;
    void              reset() noexcept override;
    plugins::SchedulerStats  stats()  const noexcept override;
    plugins::SchedulerConfig config() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nimrtc::sched
