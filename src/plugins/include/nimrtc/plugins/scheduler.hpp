/**
 * @file nimrtc/plugins/scheduler.hpp
 * @brief IScheduler — pluggable unified sending scheduler (NimRTC §2.5 / §8.3).
 *
 * The default policy is strict descending priority over the **per-packet**
 * levels:
 *
 *   kControl > kAudio > kVideoKeyframe > kVideo > kBestEffort
 *
 * A concrete scheduler may additionally provide a **per-stream** priority
 * family (three buckets: High / Normal / Low) that orders whole streams
 * against each other.  The two families live in separate queues and are
 * never compared with each other, so the ladder above describes the
 * per-packet family only.  See `nimrtc/sched/sched.hpp` §"Terminology".
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
#include <nimrtc/core/api_version.hpp>
#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_SCHEDULER_HPP
#define NIMRTC_PLUGINS_SCHEDULER_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string_view>
#include <vector>

#include <nimrtc/core/bytes.hpp>   // core::ByteSpan

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Priority — strictly ordered descending (highest = enum value 0)
// ---------------------------------------------------------------------------
enum class NIMRTC_API_STABLE Priority : std::uint8_t {
    kControl      = 0,  // highest — control messages, never dropped by scheduler
    kAudio        = 1,
    kVideoKeyframe = 2, // video I-frame (key frame)
    kVideo        = 3,
    kBestEffort   = 4,  // lowest — telemetry / best-effort data
};

/** Number of priority levels. */
constexpr NIMRTC_API_STABLE std::size_t kPriorityCount = 5;

/** Convert a Priority to its integer rank (lower = higher priority). */
[[nodiscard]] constexpr NIMRTC_API_STABLE std::size_t priority_rank(Priority p) noexcept {
    return static_cast<std::size_t>(p);
}

// ---------------------------------------------------------------------------
// PacedClass — the traffic classes that are paced independently
// ---------------------------------------------------------------------------

/** The traffic classes the scheduler paces independently.
 *
 *  The mapping from the per-packet Priority ladder onto the classes is
 *  fixed:
 *
 *    kControl                -> (no class)  — the pacer is bypassed
 *    kAudio                  -> kAudio
 *    kVideoKeyframe, kVideo  -> kVideo
 *    kBestEffort             -> kData
 *
 *  `kControl` has **no** class on purpose.  RTX, NACK and RTCP feedback
 *  must never be delayed by a saturated media bucket: they are small,
 *  they are rare, and the far end is already blocked waiting for them.
 *  Delaying feedback costs more than the bytes it occupies.
 *
 *  The classes exist so that the session's bandwidth budget can be
 *  **split** instead of being taken by whichever stream happens to have
 *  packets queued.  A video stream that saturates the link no longer
 *  starves the audio, because each class has its own token bucket.
 */
enum class NIMRTC_API_STABLE PacedClass : std::uint8_t {
    kAudio = 0,  /**< The audio stream (Priority::kAudio). */
    kVideo = 1,  /**< The video stream (Priority::kVideoKeyframe + kVideo). */
    kData  = 2,  /**< The best-effort / data lane (Priority::kBestEffort). */
};

/** The number of paced traffic classes. */
constexpr NIMRTC_API_STABLE std::size_t kPacedClassCount = 3;

// ---------------------------------------------------------------------------
// SchedulerConfig
// ---------------------------------------------------------------------------
struct NIMRTC_API_STABLE SchedulerConfig {
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

    /** Master switch.  When false (the default), the engine does not
     *  resolve or open a scheduler at open() and send_audio() sends
     *  directly through ICE.  Matches the docs in EngineConfig. */
    bool enabled = false;

    /** The congestion-window horizon, in milliseconds.
     *
     *  This value has **two** roles:
     *
     *  1. It bounds how much outbound data may stay queued: when the BWE
     *     estimate shrinks, packets are trimmed from the lowest priority
     *     upwards until the queue fits into one horizon's worth of bytes
     *     at the estimated rate.  A fixed horizon keeps the tolerated
     *     backlog independent of the host's tick rate.
     *
     *  2. It seeds the *measured* drain interval.  After the first
     *     `drain()` the scheduler measures the wall-clock time between
     *     two drains and grants send credit proportional to that elapsed
     *     time, so the effective send rate is the same whether the host
     *     ticks every 20 ms or every 200 ms.  This value is only used
     *     until the first measurement exists.
     *
     *  @note Appended at the end of the struct (v1.1.1) so that existing
     *  aggregate initialisation of the preceding fields keeps compiling. */
    std::uint32_t drain_interval_ms = 125;
};

// ---------------------------------------------------------------------------
// SchedulerStats — observable statistics
// ---------------------------------------------------------------------------
struct NIMRTC_API_STABLE SchedulerStats {
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

    /** The number of times a packet was held back because its traffic
     *  class had no pacing credit (v1.2.0 M3, video plan P2).
     *
     *  A non-zero value means the pacer is doing its job: the packets
     *  were not lost, they waited for the bucket to earn the bytes.  It
     *  is the signal that a class is over its budget.
     *
     *  @note Appended at the end of the struct so that existing aggregate
     *  initialisation of the preceding fields keeps compiling. */
    std::uint64_t paced_blocked_packets = 0;
};

// ---------------------------------------------------------------------------
// DrainCallback
// ---------------------------------------------------------------------------

/** Callback invoked once per packet drained by `drain_with()`.
 *  Returning false stops draining immediately. */
using NIMRTC_API_STABLE DrainCallback = std::function<bool(Priority, core::ByteSpan)>;

// ---------------------------------------------------------------------------
// IScheduler
// ---------------------------------------------------------------------------

/** Pluggable sending scheduler.
 *
 *  - `enqueue()` takes a `ByteSpan` view of the caller-owned buffer and
 *    copies it.  Use this when the caller's buffer is short-lived or shared.
 *  - `enqueue_owned()` takes ownership of a `std::vector<uint8_t>` via
 *    move (zero-copy).  Use this when the caller has just allocated the
 *    buffer for the sole purpose of sending it (the engine's send_audio
 *    / send_video paths use this overload to avoid one heap allocation
 *    and one buffer copy per outbound packet).
 *  - `drain()` / `drain_with()` remove packets in strict priority order
 *    (highest first) under the configured `max_queue_depth` and BWE budget.
 *  - `on_bwe_update()` is the input from the BWE plugin; implementations
 *    must drop lowest-priority traffic first, never `kControl`.
 */
class NIMRTC_API_STABLE IScheduler : public IPlugin {
public:
    ~IScheduler() override = default;

    // IPlugin -----------------------------------------------------------
    const char* name() const noexcept override = 0;
    Status      open() noexcept override = 0;
    void        close() noexcept override = 0;

    // Scheduler contract ------------------------------------------------

    /** Enqueue one packet. Empty `dst` = use the most recent non-empty
     *  destination from a prior `enqueue()`. Thread-safe.
     *
     *  This overload **copies** `data` into an internal buffer.  Use when
     *  the caller's buffer is short-lived or shared with another consumer
     *  (e.g. logging, async error tracking).  For the zero-copy path, see
     *  `enqueue_owned()` below. */
    virtual void enqueue(Priority p, core::ByteSpan data, Addr dst) noexcept = 0;

    /** Zero-copy enqueue.  Transfers ownership of `owned_data` into the
     *  scheduler.  After this call returns the caller must NOT touch the
     *  vector — its storage is now owned by the scheduler queue and will
     *  be freed when the packet is drained and dispatched.
     *
     *  Use this from hot paths (engine send_audio / send_video) to avoid
     *  one heap allocation + one buffer copy per outbound packet.  Empty
     *  `dst` = use the most recent non-empty destination.
     *
     *  Thread-safe.  Default implementation falls back to copying `data`
     *  via `enqueue()` for plugins that haven't implemented the move
     *  fast-path — see Scheduler::enqueue_owned for the default behavior
     *  in the built-in strict_priority scheduler. */
    virtual void enqueue_owned(Priority p,
                               std::vector<std::uint8_t>&& owned_data,
                               Addr dst) noexcept = 0;

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

    // ---- Per-class pacing (v1.2.0 M3, video plan P2) -------------------
    //
    // The scheduler shapes the outbound traffic with one token bucket per
    // traffic class (see PacedClass).  The *session* budget that
    // `on_bwe_update()` reports is the aggregate guard — the total may
    // not exceed it — and the class buckets are the *distribution*: they
    // split that budget between the audio, the video and the data lane,
    // so a saturated video stream cannot eat the audio's share.
    //
    // The default for every class is 0, which means "this class is not
    // paced".  A scheduler that nobody configured therefore paces
    // nothing, and the behaviour of an existing implementation is
    // unchanged.

    /** Set the pacing rate of one traffic class, in bits per second.
     *
     *  0 means the class is **not paced** — its packets leave as fast as
     *  the session budget allows.  The rates of the classes are expected
     *  to add up to at most the session target; the class buckets then
     *  split the budget between the streams.
     *
     *  A rate change keeps the credit the class has already earned (the
     *  credit is clamped to the new burst ceiling), so lowering the rate
     *  does not throw away what was earned and raising it does not grant
     *  a free burst.
     *
     *  Thread-safe. */
    virtual void set_pacing_rate(PacedClass cls, std::uint32_t bps) noexcept {
        (void)cls;
        (void)bps;
    }

    /** The pacing rate of one traffic class, in bits per second.
     *  0 = the class is not paced.  Thread-safe. */
    virtual std::uint32_t pacing_rate(PacedClass cls) const noexcept {
        (void)cls;
        return 0;
    }

    // ---- Packet buffer pool (P1 hardening, 2026-11-09) ------------------
    //
    // The outbound hot path used to perform one heap allocation AND one
    // free per packet: the engine built the packet in a member vector,
    // moved it into the scheduler (leaving the member in a moved-from
    // state, so the NEXT packet had to allocate again) and the scheduler
    // freed the buffer when the packet was drained.  At 1000 packets per
    // second that is 1000 malloc/free pairs per second on the send path.
    //
    // The pool closes that loop: the caller acquires a buffer, builds the
    // packet in it, hands it over with `enqueue_owned()`, and the
    // scheduler returns the buffer to the pool once the packet has been
    // drained and dispatched.  In steady state the send path performs no
    // heap allocation at all.
    //
    // The buffer is recycled AFTER the drain callback returned, so the
    // callback may keep using the bytes for the duration of the call only.

    /** Acquire a reusable packet buffer from the scheduler's pool.
     *
     *  `capacity_hint` is a hint in bytes.  The returned buffer may have
     *  a different capacity; the caller resizes it before use.  The
     *  default implementation returns an empty vector with `capacity_hint`
     *  bytes of reserved capacity and no recycling.
     *
     *  Thread-safe. */
    virtual std::vector<std::uint8_t> acquire_packet_buffer(
        std::size_t capacity_hint) noexcept {
        std::vector<std::uint8_t> buffer;
        if (capacity_hint != 0) buffer.reserve(capacity_hint);
        return buffer;
    }

    /** Return a buffer to the scheduler's pool for reuse.
     *
     *  Only buffers previously obtained from `acquire_packet_buffer()` of
     *  the SAME scheduler instance should be released here.  The default
     *  implementation simply destroys the buffer.
     *
     *  Thread-safe. */
    virtual void release_packet_buffer(std::vector<std::uint8_t>&& buffer) noexcept {
        (void)buffer;  // destroyed on return — no pool in the default impl
    }
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/** Factory for scheduler implementations. */
class NIMRTC_API_STABLE ISchedulerFactory {
public:
    virtual ~ISchedulerFactory() = default;
    virtual std::string_view id()          const noexcept = 0;
    virtual std::string_view display_name()const noexcept = 0;

    /** Create a new scheduler instance with the given configuration. */
    virtual IScheduler* create(SchedulerConfig config) const = 0;
};

/** Template helper — creates a factory that owns a scheduler type T. */
template<class T>
class NIMRTC_API_STABLE SimpleSchedulerFactory : public ISchedulerFactory {
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
