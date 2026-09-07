/**
 * @file nimrtc/plugins/scheduler.hpp
 * @brief IScheduler — pluggable unified sending scheduler (NimRTC §2.5 / §8.3).
 *
 * RTP (media) and DataChannel (data) share ONE priority queue. The default
 * policy is strict descending priority:
 *
 *   kControl > kAudio > kVideoKeyframe > kVideo > kBestEffort
 *
 * Implementations react to `on_bwe_update(target_bps)` to drop low-priority
 * traffic when the bandwidth budget shrinks — `kControl` is never dropped by
 * the scheduler (D8, §2.5 / §11.5.1).
 *
 * Replace this interface to plug in:
 *   - A custom strategy (e.g. weighted fair queueing for SFU fan-out).
 *   - Per-stream / per-channel priority reordering.
 *   - A passthrough queue (no priority / no BWE) for SFU forwarding (§2.2).
 *
 * ## Implementing a custom scheduler plugin
 *
 * 1. Implement `plugins::IScheduler`.
 * 2. Register: `PluginRegistry::instance().register_scheduler("my_q", factory);`
 * 3. (P2) Set `NimRTCEngine::Config::scheduler_name = "my_q"` to wire it
 *    into `send_audio` / `send_data` paths (P1: `drain()` is invoked by
 *    the engine tick but only through optional callbacks; not wired yet).
 *
 * ## Thread safety
 *
 * All public methods are thread-safe (internal mutex). `enqueue()` may be
 * called concurrently with `drain()` / `on_bwe_update()`. Callbacks
 * (DrainCallback) are invoked while the scheduler mutex is held.
 *
 * @note P1 — interface is stable; engine integration lands in P2 (§13).
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_SCHEDULER_HPP
#define NIMRTC_PLUGINS_SCHEDULER_HPP

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string_view>

#include <nimrtc/core/bytes.hpp>   // core::ByteSpan

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Priority — strictly ordered descending (highest = enum value 0)
// ---------------------------------------------------------------------------
enum class Priority : std::uint8_t {
    kControl      = 0,  // highest — control messages, never dropped by scheduler
    kAudio        = 1,
    kVideoKeyframe = 2, // video I-frame (key frame)
    kVideo        = 3,
    kBestEffort   = 4,  // lowest — telemetry / best-effort data
};

/** Number of priority levels. */
constexpr std::size_t kPriorityCount = 5;

/** Convert a Priority to its integer rank (lower = higher priority). */
[[nodiscard]] constexpr std::size_t priority_rank(Priority p) noexcept {
    return static_cast<std::size_t>(p);
}

// ---------------------------------------------------------------------------
// SchedulerConfig
// ---------------------------------------------------------------------------
struct SchedulerConfig {
    /** Estimated average outbound packet size in bytes.
     *  Used to convert a target bitrate (bps) into a packets-per-second
     *  budget. Defaults to a conservative RTP packet MTU estimate. */
    std::uint32_t avg_packet_size_bytes = 1200;

    /** Hard maximum number of packets held across all priority queues.
     *  Packets beyond this limit are dropped at enqueue time (lowest
     *  priority first, then the new packet if it is low-priority). */
    std::uint32_t max_queue_depth = 8192;

    /** When true, kVideoKeyframe is treated equal to kAudio under
     *  congestion (keyframes are never demoted below audio). */
    bool protect_keyframes = true;
};

// ---------------------------------------------------------------------------
// SchedulerStats — observable statistics
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

    /** Current target bitrate set by last `on_bwe_update()` (bps). */
    std::uint32_t target_bitrate_bps = 0;

    /** Approximate current egress rate (bps), updated at each drain call. */
    std::uint32_t current_bitrate_bps = 0;
};

// ---------------------------------------------------------------------------
// DrainCallback
// ---------------------------------------------------------------------------

/** Callback invoked once per packet drained by `drain_with()`.
 *  Returning false stops draining immediately. */
using DrainCallback = std::function<bool(Priority, core::ByteSpan)>;

// ---------------------------------------------------------------------------
// IScheduler
// ---------------------------------------------------------------------------

/** Pluggable sending scheduler.
 *
 *  - `enqueue()` is zero-copy (takes a `ByteSpan` view of the caller-owned
 *    buffer; caller must keep it alive until `drain()` copies the data).
 *  - `drain()` / `drain_with()` remove packets in strict priority order
 *    (highest first) under the configured `max_queue_depth` and BWE budget.
 *  - `on_bwe_update()` is the input from the BWE plugin; implementations
 *    must drop lowest-priority traffic first, never `kControl`.
 */
class IScheduler : public IPlugin {
public:
    ~IScheduler() override = default;

    // IPlugin -----------------------------------------------------------
    const char* name() const noexcept override = 0;
    Status      open() noexcept override = 0;
    void        close() noexcept override = 0;

    // Scheduler contract ------------------------------------------------

    /** Enqueue one packet. Empty `dst` = use the most recent non-empty
     *  destination from a prior `enqueue()`. Thread-safe. */
    virtual void enqueue(Priority p, core::ByteSpan data, Addr dst) noexcept = 0;

    /** Drain up to `max_packets` packets in priority order. Returns the
     *  number actually drained. Thread-safe. */
    virtual int drain(int max_packets) noexcept = 0;

    /** Drain up to `max_packets` packets, invoking `cb` for each. If `cb`
     *  returns false, draining stops immediately and the current packet
     *  stays in the queue. Returns the number of packets actually
     *  drained. Thread-safe. */
    virtual int drain_with(int max_packets, DrainCallback cb) noexcept = 0;

    /** Notify the scheduler of a new bandwidth estimate from the BWE.
     *  `target_bps == 0` signals total sender pause (all except `kControl`
     *  are eligible for drop). Thread-safe. */
    virtual void on_bwe_update(std::uint32_t target_bps) noexcept = 0;

    /** Reset all internal queues and statistics. Does NOT reset the
     *  configuration. Thread-safe. */
    virtual void reset() noexcept = 0;

    /** Return current statistics snapshot. */
    virtual SchedulerStats stats() const noexcept = 0;

    /** Return the current configuration. */
    virtual SchedulerConfig config() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/** Factory for scheduler implementations. */
class ISchedulerFactory {
public:
    virtual ~ISchedulerFactory() = default;
    virtual std::string_view id()          const noexcept = 0;
    virtual std::string_view display_name()const noexcept = 0;

    /** Create a new scheduler instance with the given configuration. */
    virtual IScheduler* create(SchedulerConfig config) const = 0;
};

/** Template helper — creates a factory that owns a scheduler type T. */
template<class T>
class SimpleSchedulerFactory : public ISchedulerFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleSchedulerFactory(std::string_view id,
                                    std::string_view name) noexcept
        : id_(id), name_(name) {}

    std::string_view id()          const noexcept override { return id_; }
    std::string_view display_name()const noexcept override { return name_; }
    IScheduler* create(SchedulerConfig config) const override {
        return new T(config);
    }
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_SCHEDULER_HPP
