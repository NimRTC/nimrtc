/**
 * @file nimrtc/sched/sched.hpp
 * @brief Unified sending scheduler — L1 packet prioritisation.
 *
 * Design (§8.2/§8.3 NimRTC-V2):
 *   RTP (media) and DataChannel (data) share one priority queue.
 *   Strict descending priority ensures control messages survive congestion.
 *
 * Priority table:
 *   kControl      — control channel, never dropped by scheduler
 *   kAudio        — audio frame
 *   kVideoKeyframe — video I-frame (key frame)
 *   kVideo        — regular video frame
 *   kBestEffort   — telemetry / best-effort data
 *
 * Thread safety: the implementation guards internal state with a mutex;
 * callers must still provide their own memory barriers if the caller
 * and the scheduler tick run on different threads.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include <nimrtc/core/bytes.hpp>   // core::ByteSpan / core::ByteBuffer
#include <nimrtc/core/time.hpp>    // core::TimePoint / core::SteadyClock
#include <nimrtc/plugins/base.hpp> // plugins::Addr

namespace nimrtc::sched {

// ---------------------------------------------------------------------------
// Priority — strictly ordered descending (highest = index 0).
// ---------------------------------------------------------------------------
enum class Priority : std::uint8_t {
    kControl      = 0,  // highest
    kAudio        = 1,
    kVideoKeyframe = 2,
    kVideo        = 3,
    kBestEffort   = 4,  // lowest
};

/** Number of priority levels. */
constexpr std::size_t kPriorityCount = 5;

/** Convert a Priority to its integer rank (lower = higher priority). */
[[nodiscard]] constexpr std::size_t priority_rank(Priority p) noexcept {
    return static_cast<std::size_t>(p);
}

/** Return the next lower priority, or std::nullopt at the bottom. */
[[nodiscard]] constexpr std::optional<Priority> lower_priority(Priority p) noexcept {
    if (p == Priority::kBestEffort) return std::nullopt;
    return static_cast<Priority>(static_cast<std::uint8_t>(p) + 1);
}

// ---------------------------------------------------------------------------
// SchedulerConfig — user-visible configuration.
// ---------------------------------------------------------------------------
struct SchedulerConfig {
    /** Estimated average outbound packet size in bytes.
     *  Used to convert a target bitrate (bps) into packets-per-second budget.
     *  Defaults to a conservative RTP packet MTU estimate. */
    std::uint32_t avg_packet_size_bytes = 1200;

    /** Hard maximum number of packets held across all priority queues.
     *  Packets beyond this limit are dropped at enqueue time (lowest priority
     *  first, then the new packet if it is low-priority). */
    std::uint32_t max_queue_depth = 8192;

    /** When true, kVideoKeyframe is given equal priority to kAudio during
     *  congestion (i.e. keyframes are never demoted below audio). */
    bool protect_keyframes = true;
};

// ---------------------------------------------------------------------------
// PacketRecord — one entry in the send queue.
// ---------------------------------------------------------------------------
struct PacketRecord {
    /** Priority level at the time of enqueue. */
    Priority priority = Priority::kBestEffort;

    /** Read-only view over the caller-owned packet buffer.
     *  The caller is responsible for keeping the buffer alive until drain()
     *  copies the data. */
    core::ByteSpan data;

    /** Destination address for this packet.
     *  Empty (= all-zero) means "use the last-set destination". */
    plugins::Addr dst_addr;

    /** Monotonic timestamp assigned at enqueue time. */
    core::TimePoint enqueue_time;

    /** Estimated packet size in bytes.
     *  If zero, SchedulerImpl estimates it from SchedulerConfig::avg_packet_size_bytes. */
    std::uint32_t estimated_size_bytes = 0;
};

// ---------------------------------------------------------------------------
// SchedulerStats — observable statistics (read by engine / telemetry).
// ---------------------------------------------------------------------------
struct SchedulerStats {
    /** Packets currently held in all queues. */
    std::uint32_t queued_packets = 0;

    /** Packets dropped (queue overflow or BWE-driven congestion). */
    std::uint32_t dropped_packets = 0;

    /** Packets drained and sent since last reset. */
    std::uint32_t sent_packets = 0;

    /** Bytes of payload drained since last reset. */
    std::uint64_t sent_bytes = 0;

    /** Current target bitrate set by last on_bwe_update() (bps). */
    std::uint32_t target_bitrate_bps = 0;

    /** Approximate current egress rate (bps), updated at each drain call. */
    std::uint32_t current_bitrate_bps = 0;
};

// ---------------------------------------------------------------------------
// IScheduler — abstract interface for pluggable scheduler implementations.
// ---------------------------------------------------------------------------

/** Callback invoked once per packet drained by drain_with().
 *  The callback receives the priority of the packet and its payload view.
 *  Returning false stops draining immediately (used to abort early). */
using DrainCallback = std::function<bool(Priority, core::ByteSpan)>;

class IScheduler {
public:
    virtual ~IScheduler() = default;

    /** Enqueue one packet for transmission.
     *
     *  @param p     Packet priority.
     *  @param data  Zero-copy view of the packet payload.
     *  @param dst   Destination address. Empty = use the most recent non-empty
     *               address from a prior enqueue().
     *
     *  Thread-safe: may be called from any thread while drain() runs concurrently.
     */
    virtual void enqueue(Priority p, core::ByteSpan data, plugins::Addr dst) = 0;

    /** Drain up to @p max_packets packets in priority order.
     *
     *  @param max_packets  Upper bound on the number of packets to drain.
     *                      Zero is legal (returns 0 without side effects).
     *
     *  @return  Number of packets actually drained and "sent"
     *           (in the current stub the caller is responsible for the actual
     *           I/O; drain() only removes them from the queue).
     *
     *  Thread-safe.
     */
    virtual int drain(int max_packets) = 0;

    /** Drain up to @p max_packets packets, invoking @p cb for each.
     *
     *  The callback receives the packet's Priority and ByteSpan. If the
     *  callback returns false, draining stops immediately and the remaining
     *  packets (including the one being processed) stay in the queue.
     *
     *  Useful when the caller needs to observe per-packet metadata
     *  (priority, payload contents) during draining.
     *
     *  @return  Number of packets actually drained and removed from the queue.
     */
    virtual int drain_with(int max_packets, DrainCallback cb) = 0;

    /** Notify the scheduler of a new bandwidth estimate from the BWE.
     *
     *  The scheduler uses this to decide how aggressively to drop low-priority
     *  traffic.  A target_bps of 0 signals total sender pause (all except
     *  kControl are eligible for drop).
     *
     *  @param target_bps  Target egress bitrate in bits per second.
     *
     *  Thread-safe.
     */
    virtual void on_bwe_update(std::uint32_t target_bps) = 0;

    /** Reset all internal queues and statistics.
     *  @note Does not reset SchedulerConfig.
     *
     *  Thread-safe.
     */
    virtual void reset() = 0;

    /** Return current statistics snapshot. */
    virtual SchedulerStats stats() const = 0;

    /** Return the current configuration (as passed to the constructor). */
    virtual SchedulerConfig config() const = 0;
};

// ---------------------------------------------------------------------------
// Scheduler — default priority-queue implementation.
// ---------------------------------------------------------------------------
class Scheduler final : public IScheduler {
public:
    /** Construct a scheduler with the given configuration.
     *  @param config  SchedulerConfig with non-default parameters.
     *                 nullptr → use defaults. */
    explicit Scheduler(SchedulerConfig config = {});
    ~Scheduler() override;

    Scheduler(const Scheduler&)            = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&)                 noexcept;
    Scheduler& operator=(Scheduler&&)      noexcept;

    void enqueue(Priority p, core::ByteSpan data, plugins::Addr dst) override;
    int drain(int max_packets) override;
    int drain_with(int max_packets, DrainCallback cb) override;
    void on_bwe_update(std::uint32_t target_bps) override;
    void reset() override;
    SchedulerStats stats() const override;
    SchedulerConfig config() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nimrtc::sched
