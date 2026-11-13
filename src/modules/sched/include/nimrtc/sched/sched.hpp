/**
 * @file nimrtc/sched/sched.hpp
 * @brief Unified sending scheduler — send-priority scheduling (§8.2 / §8.3).
 *
 * ## Terminology: two independent queue families
 *
 * One `Scheduler` instance holds two *separate* queue families.  They are
 * named below with scheduler-local labels that are deliberately **not** the
 * architecture's L0–L3 layering: in `docs/zh/architecture.md` §2.1 "L1" means
 * the *transport* layer, which is where this module lives.  So "L1" must
 * never be used inside this module to mean the stream family.
 *
 *   - **P-family — per-packet priority**: `plugins::Priority`, 5 levels,
 *     drained by `drain()` / `drain_with()`.
 *   - **S-family — per-stream priority**: `sched::StreamPriority`, 3 buckets,
 *     drained by `select_next_packet()`.
 *
 * The two families are **not** composed into a single total order:
 * `drain()` / `drain_with()` only ever return P-family packets,
 * `select_next_packet()` only ever returns S-family packets, and neither
 * side compares a P-family level against an S-family bucket.  A ladder
 * written as "control > audio > keyframe > video > best-effort" therefore
 * describes **one** family only — see the P-family ladder below.
 *
 * ## S-family: per-stream strict-priority buckets (DC-2)
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
 * ## Flow control: both families share one budget
 *
 *   The S-family is **not** a bypass of the congestion control.  The
 *   packets it emits are charged to the **same** session send credit
 *   (the BWE-derived token bucket) and to the **same** per-class pacing
 *   buckets as the P-family packets, and the two families share **one**
 *   credit clock: the elapsed wall-clock time between two grants is
 *   credited once, no matter which family asked for it.
 *
 *   Two independent pools would double the effective send rate, so a
 *   session that drains both families would put twice as many bytes on
 *   the wire as the bandwidth estimate allows.  (This is exactly the bug
 *   the architecture remediation plan lists as A9.)
 *
 *   The paced class of a stream is chosen with `set_stream_paced_class()`
 *   and defaults to `PacedClass::kData` — the conservative choice, because
 *   a stream whose traffic class is unknown must not consume a budget it
 *   does not own.  Each class owns its own FIFO inside a bucket, so a
 *   class whose budget is exhausted does not starve the other classes.
 *
 * ## Scope note (DC-2 under the default `web_transport` backend)
 *
 *   The DC-2 guarantee is **intra-scheduler ordering only**: it orders
 *   packets that are both enqueued into this `Scheduler` instance
 *   (P-family `enqueue_*()` vs S-family `enqueue_for_stream_*()`).
 *
 *   Under the default `EngineConfig::datachannel_backend = "web_transport"`,
 *   media rides DTLS-SRTP over ICE/UDP while the DataChannel rides a
 *   **separate QUIC connection** (`WebTransportDataChannel` sends straight
 *   through `IWebTransportSession::send_datagram()` / stream `send()` and
 *   never enqueues into this `Scheduler`; `NimRTCEngine::tick()` only
 *   drains the P-family via `drain_with()`). The two connections run
 *   independent congestion control, so a video-heavy media path does not
 *   share a scheduler queue with — and therefore cannot be pre-empted by —
 *   the data path at the wire level. Cross-connection priority would require
 *   joint pacing above both connections and is **not** what DC-2 provides.
 *
 *   No shipped backend puts the two planes on one connection: the legacy
 *   `"sctp"` backend was retired in v1.2.0 (WP-A / DEC-4 = A1) and is now a
 *   test-only in-process usrsctp loopback socket that never reaches a peer,
 *   so it cannot provide wire-level cross-plane priority either.
 *
 *   The honest statement, then: **the scheduler arbitrates within a
 *   connection**.  The S-family is *available and flow-controlled* — it is
 *   the per-stream priority landing point for the multi-stream (M4) case,
 *   where a screen stream and a camera stream share one connection and
 *   must be ordered against each other.  The current data plane does not
 *   consume it, because the data plane rides its own connection.
 *
 * ## P-family: per-packet priority (§8.3)
 *
 *   The original 5-level `plugins::Priority` ladder is preserved for callers
 *   that route packets without a stream abstraction:
 *
 *     kControl > kAudio > kVideoKeyframe > kVideo > kBestEffort
 *
 *   It is orthogonal to the S-family buckets above.  Note that
 *   `kBestEffort` is the lowest *packet* level, not a promise that
 *   DataChannel traffic is scheduled here — whether data-plane packets
 *   enter this family at all depends on the backend (see the scope note).
 *
 * Thread safety: the implementation guards internal state with a mutex;
 * callers must still provide their own memory barriers if the caller
 * and the scheduler tick run on different threads.
 *
 * @note P1 — interface is stable; engine integration lands in P2 (§13).
 */

#pragma once


// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>
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

// The C ABI.  Included here because the C++ entry points at the bottom of
// this header are forwarders to it, and because `SchedulerHandle` is an
// alias of the C ABI's `NimrtcSchedHandle`.  The C header is pure C11 and
// has no C++ dependencies of its own.
#include <nimrtc/sched/sched_c.h>

// Re-export plugin types under nimrtc::sched:: for backwards-compatibility.
namespace nimrtc::sched {
    using NIMRTC_API_STABLE Priority           = plugins::Priority;
    using NIMRTC_API_STABLE PacedClass         = plugins::PacedClass;
    using NIMRTC_API_STABLE SchedulerConfig    = plugins::SchedulerConfig;
    using NIMRTC_API_STABLE SchedulerStats     = plugins::SchedulerStats;
    using NIMRTC_API_STABLE DrainCallback      = plugins::DrainCallback;
    using NIMRTC_API_STABLE StreamDrainCallback = plugins::StreamDrainCallback;
    using NIMRTC_API_STABLE IScheduler         = plugins::IScheduler;
    NIMRTC_API_STABLE constexpr std::size_t kPriorityCount = plugins::kPriorityCount;

    // NOTE: `plugins::priority_rank` is a free function in `nimrtc::plugins::`
    // and is picked up through ADL on the `Priority` argument (which is
    // `plugins::Priority`). We do NOT re-declare it here to avoid shadowing
    // ambiguity.
    //
    // `sched::lower_priority()` below is *our* helper that walks one priority
    // step down and returns std::nullopt at the bottom. It is not re-exported
    // from `plugins::` because priority traversal is a sched-module concept.

    /** Return the next lower priority, or std::nullopt at the bottom. */
    [[nodiscard]] NIMRTC_API_STABLE constexpr std::optional<Priority> lower_priority(Priority p) noexcept {
        if (p == Priority::kBestEffort) return std::nullopt;
        return static_cast<Priority>(static_cast<std::uint8_t>(p) + 1);
    }

    // =======================================================================
    // DC-2: S-family (per-stream) priority buckets
    // =======================================================================

    /** Per-stream priority — three buckets drive strict-priority scheduling.
     *
     *  The engine maps stream types at registration time:
     *    High   — RTX, NACK, RTCP feedback
     *    Normal — audio, video keyframes, video frames
     *    Low    — padding packets
     *
     *  Lower ordinal = higher priority (matches `plugins::Priority` convention).
     *
     *  @note The type now lives in `plugins::` so that the S-family seam can
     *        be part of the public `IScheduler` interface (a third-party
     *        scheduler must be able to implement the per-stream family).
     *        This alias keeps `nimrtc::sched::StreamPriority` working. */
    using NIMRTC_API_STABLE StreamPriority = plugins::StreamPriority;

    /** Number of per-stream priority buckets (High / Normal / Low). */
    NIMRTC_API_STABLE constexpr std::size_t kStreamPriorityCount =
        plugins::kStreamPriorityCount;

    /** Convert a StreamPriority to its integer rank (0 = High, 2 = Low). */
    [[nodiscard]] NIMRTC_API_STABLE constexpr std::size_t
    stream_priority_rank(StreamPriority p) noexcept {
        return plugins::stream_priority_rank(p);
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
class NIMRTC_API_STABLE Scheduler final : public plugins::IScheduler {
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

    // ---- Per-class pacing (v1.2.0 M3, video plan P2) -------------------
    //
    // One token bucket per traffic class (plugins::PacedClass): the
    // session budget is the aggregate guard, the class buckets are the
    // distribution.  The control plane (Priority::kControl) has no class
    // and bypasses the pacer entirely, so RTX / NACK / RTCP feedback is
    // never delayed by a saturated media bucket.
    void              set_pacing_rate(plugins::PacedClass cls,
                                       std::uint32_t bps) noexcept override;
    std::uint32_t     pacing_rate(plugins::PacedClass cls) const noexcept override;

    // ---- Packet buffer pool (P1 hardening, 2026-11-09) ----------------
    //
    // The built-in scheduler keeps a bounded free-list of packet buffers.
    // A buffer that was drained and dispatched goes back onto the list, so
    // the steady-state send path performs no heap allocation per packet.
    // The pool is bounded by `max_queue_depth` so a burst cannot make the
    // scheduler hoard memory.
    std::vector<std::uint8_t> acquire_packet_buffer(
        std::size_t capacity_hint) noexcept override;
    void release_packet_buffer(std::vector<std::uint8_t>&& buffer) noexcept override;

    // =====================================================================
    // DC-2: S-family (per-stream) priority API
    // =====================================================================
    //
    // These override the `plugins::IScheduler` defaults declared above; the
    // concrete scheduler *does* implement the per-stream family.

    /** Set the per-stream priority for a stream.  All subsequent packets
     *  enqueued via `enqueue_for_stream()` for this `stream_id` will be
     *  placed in the matching bucket.  Already-queued packets are moved to
     *  the new bucket atomically.  Thread-safe. */
    void              set_stream_priority(std::uint32_t stream_id,
                                          StreamPriority level) noexcept override;

    /** Return the current per-stream priority of a stream.  Returns Normal
     *  if the stream has not been assigned a priority yet.  Thread-safe. */
    [[nodiscard]] StreamPriority
                    get_stream_priority(std::uint32_t stream_id) const noexcept override;

    /** The paced traffic class of a stream's packets.
     *
     *  Defaults to `PacedClass::kData` — a stream whose traffic class is
     *  unknown must not consume a budget it does not own. */
    [[nodiscard]] plugins::PacedClass
                    stream_paced_class(std::uint32_t stream_id) const noexcept override;

    /** Assign a paced traffic class to a stream.
     *
     *  A multi-stream session (screen capture + camera, say) usually wants
     *  the screen stream paced by the video class and the control/pointer
     *  stream paced by the data class. */
    void              set_stream_paced_class(std::uint32_t stream_id,
                                             plugins::PacedClass cls) noexcept override;

    /** Zero-copy enqueue for a specific stream.
     *
     *  The packet is placed in the bucket corresponding to the stream's
     *  current `StreamPriority` (default Normal).  Use `set_stream_priority`
     *  before or after enqueueing to control the bucket.
     *
     *  Thread-safe. */
    void              enqueue_for_stream_owned(std::uint32_t stream_id,
                                               std::vector<std::uint8_t>&& owned_data,
                                               plugins::Addr dst) noexcept override;

    /** Copying enqueue for a specific stream.
     *
     *  Copies `data` into an internal buffer before queueing.
     *  See `enqueue_for_stream_owned()` for the zero-copy variant.
     *
     *  Thread-safe. */
    void              enqueue_for_stream(std::uint32_t stream_id,
                                         core::ByteSpan data,
                                         plugins::Addr dst) noexcept override;

    /** Drain up to `max_packets` from the highest non-empty stream bucket.
     *
     *  Walks High → Normal → Low and emits packets from the first
     *  non-empty queue.  Does NOT interact with the legacy 5-level
     *  per-packet priority queues (drain / drain_with).
     *
     *  The callback receives the stream id and the stream's priority level.
     *  Returning false stops the selection and the packet stays queued.
     *
     *  Thread-safe. */
    int               select_next_packet(int max_packets,
                                         plugins::StreamDrainCallback cb) noexcept override;

    /** Convenience overload: the legacy `DrainCallback` shape.
     *
     *  The stream id and the stream's priority level are not visible
     *  through `DrainCallback` (it only sees the per-packet `Priority` and
     *  the byte span), so this overload reports `Priority::kBestEffort`
     *  and lets the caller read the stream identity from the payload or
     *  from `get_stream_priority()`.  It exists so that existing callers
     *  keep compiling. */
    int               select_next_packet(int max_packets,
                                         plugins::DrainCallback cb) noexcept;

    /** Drain without a callback.
     *
     *  `nullptr` would be ambiguous between the `DrainCallback` and the
     *  `StreamDrainCallback` overloads, so the literal `nullptr` is
     *  matched by this overload instead.  It drains the packets and
     *  discards them (they are accounted as sent, exactly as the
     *  callback-less `drain()` does). */
    int               select_next_packet(int max_packets,
                                         std::nullptr_t) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    /** Locked (caller holds impl_->mu_) enqueue helper.  Used by both
     *  the public `enqueue()` (copy) and `enqueue_owned()` (move) paths
     *  after acquiring the mutex.  Consumes `owned_data` via move. */
    void enqueue_locked(plugins::Priority p,
                        std::vector<std::uint8_t>&& owned_data,
                        plugins::Addr dst) noexcept;

    /** Put a batch of collected packets back into their queues and refund
     *  the credit and the pacing tokens they consumed.
     *
     *  Called from the *veto* path of `drain_with()` / `select_next_packet()`
     *  when the drain callback refuses a packet.  The packets from
     *  `first_vetoed` onwards are restored — in their original order — at
     *  the FRONT of the queue they came from, so the next drain sees them
     *  again exactly as if they had never been collected.
     *
     *  @param first_vetoed  The index of the vetoed packet in the batch.
     *  @param s_family      true for the per-stream queues, false for the
     *                       per-packet priority queues. */
    void undo_batch(std::size_t first_vetoed, bool s_family) noexcept;
};

// =============================================================================
// The S-family entry points — the C++ spelling of the C ABI
// =============================================================================
//
// These entry points are the **S-family (per-stream) interface** — the
// one public capability that expresses "prioritise this stream over that
// one".  They are not legacy leftovers: the per-stream priority family
// is a supported capability (see the scope note at the top of this
// header).
//
// ## Two spellings, one implementation
//
// The functions below use C++ types and live in `namespace nimrtc::sched`.
// The **same** operations are also available through a genuine `extern "C"`
// ABI in `<nimrtc/sched/sched_c.h>`, which a C host or an FFI binding can
// actually include and link against.
//
// The C++ functions here are thin forwarders to the C ABI, so there is
// exactly one implementation and the two spellings cannot drift apart.
//
// (The C ABI was *not* always real: the entry points used to be plain
// C++ functions with C++ signatures, so the linker emitted C++-mangled
// symbols and no C translation unit could name their parameter types.
// A C program could neither include the header nor link the library.
// See `docs/plan/architecture-remediation-plan.md` §4.4 / item D5.)
//
// `sched_select_next_packet()` reports the **real** stream id and the
// **real** bucket priority — not placeholders.

/** Opaque handle to a Scheduler instance.
 *
 *  The same object the C ABI hands out as `NimrtcSchedHandle`; the alias
 *  keeps the C++ spelling (and its type identity) unchanged. */
using NIMRTC_API_STABLE SchedulerHandle = ::NimrtcSchedHandle;

/** Allocate a new Scheduler and return an opaque handle.
 *  Pass nullptr for `config` to use defaults. */
[[nodiscard]] NIMRTC_API_STABLE SchedulerHandle*
sched_create(const plugins::SchedulerConfig* config) noexcept;

/** Destroy a SchedulerHandle returned by sched_create(). */
NIMRTC_API_STABLE void sched_destroy(SchedulerHandle* h) noexcept;

/** Set the priority bucket of a stream. */
NIMRTC_API_STABLE void sched_set_stream_priority(SchedulerHandle* h,
                               std::uint32_t stream_id,
                               StreamPriority level) noexcept;

/** Return the priority bucket of a stream.  Returns Normal if not found. */
[[nodiscard]] NIMRTC_API_STABLE StreamPriority
sched_get_stream_priority(const SchedulerHandle* h,
                          std::uint32_t stream_id) noexcept;

/** Enqueue one packet for a stream.
 *  Pass nullptr for `data` and `len=0` to indicate an empty payload.
 *  Pass nullptr for `dst` to reuse the most recent destination. */
NIMRTC_API_STABLE void sched_enqueue_stream_owned(SchedulerHandle* h,
                               std::uint32_t stream_id,
                               const std::uint8_t* data,
                               std::size_t len,
                               const plugins::Addr* dst) noexcept;

/** Drain up to `max_packets` packets from the highest non-empty stream
 *  bucket.
 *  Pass nullptr for `cb` to drain without a callback.
 *  Returns the number of packets drained. */
[[nodiscard]] NIMRTC_API_STABLE int
sched_select_next_packet(SchedulerHandle* h,
                         int max_packets,
                         void (*on_packet)(std::uint32_t stream_id,
                                           StreamPriority level,
                                           const std::uint8_t* data,
                                           std::size_t len,
                                           void* user_data),
                         void* user_data) noexcept;

} // namespace nimrtc::sched
