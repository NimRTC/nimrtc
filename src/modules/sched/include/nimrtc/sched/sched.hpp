/**
 * @file nimrtc/sched/sched.hpp
 * @brief Unified sending scheduler — L1 packet prioritisation (§8.2 / §8.3).
 *
 * ## L1: Per-stream strict-priority buckets (DC-2)
 *
 *   Streams are identified by a caller-supplied `stream_id` (uint32_t).
 *   Each stream carries a `StreamPriority` (High / Normal / Low).  The
 *   scheduler maintains three separate FIFO queues, one per bucket.
 *   `select_next_packet()` always drains the highest non-empty bucket
 *   before touching lower buckets — guaranteeing that RTX / NACK (High)
 *   always pre-empt video / padding (Normal / Low).
 *
 *   Priority defaults:
 *     High   — RTX, NACK, RTCP feedback
 *     Normal — audio, video keyframes and regular frames
 *     Low    — padding, padding RTCP
 *
 * ## L0: Per-packet priority (pre-existing §8.3)
 *
 *   The original 5-level `plugins::Priority` (kControl … kBestEffort) is
 *   preserved for callers that route packets without a stream abstraction.
 *   It is orthogonal to the stream-bucket layer above.
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
#include <vector>

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

    // =======================================================================
    // DC-2: L1 stream-priority buckets
    // =======================================================================

    /** L1 stream priority — three buckets drive strict-priority scheduling.
     *
     *  The engine maps stream types at registration time:
     *    High   — RTX, NACK, RTCP feedback
     *    Normal — audio, video keyframes, video frames
     *    Low    — padding packets
     *
     *  Lower ordinal = higher priority (matches `plugins::Priority` convention). */
    enum class StreamPriority : std::uint8_t {
        High   = 0,  /**< RTX / NACK / urgent feedback — always sent first. */
        Normal = 1,  /**< Audio / video — sent when High bucket is empty. */
        Low    = 2,  /**< Padding — sent only when High and Normal are empty. */
    };

    /** Number of L1 priority buckets (High / Normal / Low). */
    constexpr std::size_t kStreamPriorityCount = 3;

    /** Convert a StreamPriority to its integer rank (0 = High, 2 = Low). */
    [[nodiscard]] constexpr std::size_t
    stream_priority_rank(StreamPriority p) noexcept {
        return static_cast<std::size_t>(p);
    }

} // namespace nimrtc::sched

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
    void              enqueue_owned(plugins::Priority p,
                                    std::vector<std::uint8_t>&& owned_data,
                                    plugins::Addr dst) noexcept override;
    int               drain(int max_packets) noexcept override;
    int               drain_with(int max_packets,
                                plugins::DrainCallback cb) noexcept override;
    void              on_bwe_update(std::uint32_t target_bps) noexcept override;
    void              reset() noexcept override;
    plugins::SchedulerStats  stats()  const noexcept override;
    plugins::SchedulerConfig config() const noexcept override;

    // =====================================================================
    // DC-2: L1 stream-priority API
    // =====================================================================

    /** Set the L1 priority for a stream.  All subsequent packets enqueued
     *  via `enqueue_for_stream()` for this `stream_id` will be placed in
     *  the matching bucket.  Existing packets already in queues are not
     *  moved.  Thread-safe. */
    void              set_stream_priority(std::uint32_t stream_id,
                                          StreamPriority level) noexcept;

    /** Return the current L1 priority of a stream.  Returns Normal if the
     *  stream has not been assigned a priority yet.  Thread-safe. */
    [[nodiscard]] StreamPriority
                    get_stream_priority(std::uint32_t stream_id) const noexcept;

    /** Zero-copy enqueue for a specific stream.
     *
     *  The packet is placed in the bucket corresponding to the stream's
     *  current `StreamPriority` (default Normal).  Use `set_stream_priority`
     *  before or after enqueueing to control the bucket.
     *
     *  Thread-safe. */
    void              enqueue_for_stream_owned(std::uint32_t stream_id,
                                               std::vector<std::uint8_t>&& owned_data,
                                               plugins::Addr dst) noexcept;

    /** Copying enqueue for a specific stream.
     *
     *  Copies `data` into an internal buffer before queueing.
     *  See `enqueue_for_stream_owned()` for the zero-copy variant.
     *
     *  Thread-safe. */
    void              enqueue_for_stream(std::uint32_t stream_id,
                                         core::ByteSpan data,
                                         plugins::Addr dst) noexcept;

    /** Drain up to `max_packets` from the highest non-empty stream bucket.
     *
     *  Walks High → Normal → Low and emits packets from the first
     *  non-empty queue.  Does NOT interact with the legacy 5-level
     *  per-packet priority queues (drain / drain_with).
     *
     *  Thread-safe. */
    int               select_next_packet(int max_packets,
                                         plugins::DrainCallback cb) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    /** Locked (caller holds impl_->mu_) enqueue helper.  Used by both
     *  the public `enqueue()` (copy) and `enqueue_owned()` (move) paths
     *  after acquiring the mutex.  Consumes `owned_data` via move. */
    void enqueue_locked(plugins::Priority p,
                        std::vector<std::uint8_t>&& owned_data,
                        plugins::Addr dst) noexcept;
};

// =============================================================================
// C ABI wrapper — opaque handle + C linkage for FFI consumers
// =============================================================================

/** Opaque handle to a Scheduler instance for the C ABI. */
struct SchedulerHandle;

/** Allocate a new Scheduler and return an opaque handle.
 *  Pass nullptr for `config` to use defaults. */
[[nodiscard]] SchedulerHandle*
sched_create(const plugins::SchedulerConfig* config) noexcept;

/** Destroy a SchedulerHandle returned by sched_create(). */
void sched_destroy(SchedulerHandle* h) noexcept;

/** C wrapper for set_stream_priority. */
void sched_set_stream_priority(SchedulerHandle* h,
                               std::uint32_t stream_id,
                               StreamPriority level) noexcept;

/** C wrapper for get_stream_priority. Returns Normal if not found. */
[[nodiscard]] StreamPriority
sched_get_stream_priority(const SchedulerHandle* h,
                          std::uint32_t stream_id) noexcept;

/** C wrapper for enqueue_for_stream_owned.
 *  Pass nullptr for `data` and `len=0` to indicate an empty payload. */
void sched_enqueue_stream_owned(SchedulerHandle* h,
                               std::uint32_t stream_id,
                               const std::uint8_t* data,
                               std::size_t len,
                               const plugins::Addr* dst) noexcept;

/** C wrapper for select_next_packet.
 *  Pass nullptr for `cb` to drain without a callback.
 *  Returns the number of packets drained. */
[[nodiscard]] int
sched_select_next_packet(SchedulerHandle* h,
                         int max_packets,
                         void (*on_packet)(std::uint32_t stream_id,
                                           StreamPriority level,
                                           const std::uint8_t* data,
                                           std::size_t len,
                                           void* user_data),
                         void* user_data) noexcept;

} // namespace nimrtc::sched
