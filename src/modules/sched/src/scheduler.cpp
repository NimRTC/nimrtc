/**
 * @file src/modules/sched/src/scheduler.cpp
 * @brief Scheduler default implementation — priority-queue with BWE integration.
 *
 * ## Design
 *
 * - Five std::deque sub-queues (one per Priority), indexed by priority_rank().
 *   std::deque is preferred over std::queue because drain() needs to iterate
 *   without allocating a temporary vector.
 *
 * - All public entry points acquire impl_->mu_ (std::mutex) for thread safety.
 *
 * - BWE congestion response (on_bwe_update):
 *     The sender runs on a *time-based credit* (token bucket): every
 *     drain() grants credit proportional to the wall-clock time that
 *     elapsed since the previous drain, and every packet that leaves the
 *     queue consumes its estimated size from that credit.  The effective
 *     send rate is therefore independent of how often the host calls
 *     drain() — a host ticking every 20 ms and a host ticking every
 *     200 ms send at the same bitrate.
 *
 *     On top of the credit, a congestion *window* (one
 *     SchedulerConfig::drain_interval_ms worth of bytes) bounds how much
 *     data may stay queued: packets are trimmed from the lowest priority
 *     upwards until the queue fits.
 *
 * - kControl is NEVER dropped by the scheduler — only discarded if the
 *   caller drops the entire stream (BWE target_bps == 0).
 *
 * - On enqueue() overflow (total > max_queue_depth): drop from kBestEffort
 *   first, then kVideo, then kVideoKeyframe, then kAudio, then give up
 *   and drop the incoming packet.
 */

#include <nimrtc/sched/sched.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nimrtc/core/log.hpp>  // core::log::Logger
#include <nimrtc/pacing/pacing.hpp>  // pacing::TokenBucket (v1.2.0 M3, P1)

namespace nimrtc::sched {

namespace {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

constexpr auto kLog = core::log::Level::Debug;

// ---------------------------------------------------------------------------
// PacketRecord — one entry in the send queue (PIMPL internal type).
// ---------------------------------------------------------------------------
struct PacketRecord {
    plugins::Priority priority = plugins::Priority::kBestEffort;
    // stream_id is non-zero when the packet was enqueued via enqueue_for_stream().
    // Zero means legacy per-packet priority path.
    std::uint32_t     stream_id = 0;
    // Owned packet data — the scheduler copies the caller's buffer at enqueue
    // time, so the caller does not need to keep it alive until drain().
    std::vector<std::uint8_t> owned_data;
    plugins::Addr     dst_addr;
    core::TimePoint   enqueue_time;
    std::uint32_t     estimated_size_bytes = 0;
};

// Format-string attribute required by Clang/GCC to suppress
// -Wformat-nonliteral on this variadic helper (the format string
// comes from the caller; the compiler cannot prove it is a literal).
// Forward-declare with the attribute, then define separately so GCC
// (which rejects attributes on inline function *definitions*) is
// happy with the same syntax as Clang.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
inline void sched_debug(const char* fmt, ...);

inline void sched_debug(const char* fmt, ...) {
    if (core::log::Logger::instance().level() <= kLog) {
        char buf[256];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        core::log::Logger::instance().debug(buf);
    }
}

/** Bytes the sender may emit during one congestion-window horizon.
 *
 *  `drain_interval_ms` is the scheduler's *horizon*: how much outbound
 *  data may stay queued.  It is deliberately independent of the host's
 *  tick rate — the tolerated backlog must not change when the host drains
 *  more or less often.
 *
 *  The *send rate* is enforced separately, by the time-based credit in
 *  drain_with(), which follows the measured wall-clock interval between
 *  drains.
 *
 *  `drain_interval_ms == 0` is treated as 1 ms (i.e. one packet's worth
 *  of budget at most) rather than as "unlimited", so a misconfigured
 *  scheduler throttles instead of flooding the wire. */
[[nodiscard]] constexpr std::uint64_t
bytes_per_drain_interval(std::uint32_t target_bps,
                          std::uint32_t drain_interval_ms) noexcept {
    const std::uint64_t bytes_per_sec = static_cast<std::uint64_t>(target_bps) / 8;
    const std::uint64_t interval = drain_interval_ms == 0 ? 1u : drain_interval_ms;
    return (bytes_per_sec * interval) / 1000;
}

/** Estimate the byte size of one PacketRecord.
 *  Uses PacketRecord::estimated_size_bytes if non-zero, otherwise falls back
 *  to cfg.avg_packet_size_bytes. */
[[nodiscard]] std::uint32_t
estimated_packet_bytes(const PacketRecord& r,
                       std::uint32_t avg_size) noexcept {
    return r.estimated_size_bytes != 0 ? r.estimated_size_bytes : avg_size;
}

/** Clamp a measured interval to a sane range.
 *  1 ms avoids a zero-length budget (which would stall); 1000 ms stops a
 *  long idle gap from turning into an unbounded burst. */
[[nodiscard]] constexpr std::uint32_t
clamp_interval_ms(std::int64_t ms) noexcept {
    if (ms < 1) return 1u;
    if (ms > 1000) return 1000u;
    return static_cast<std::uint32_t>(ms);
}

/** The paced class of a per-packet priority.
 *
 *  Returns the index into the scheduler's per-class token buckets, or
 *  -1 for `kControl` — the control plane **bypasses** the pacer: RTX,
 *  NACK and RTCP feedback must not wait behind a saturated media
 *  bucket. */
[[nodiscard]] constexpr int
paced_class_index(plugins::Priority p) noexcept {
    switch (p) {
        case plugins::Priority::kAudio:
            return static_cast<int>(plugins::PacedClass::kAudio);
        case plugins::Priority::kVideoKeyframe:
        case plugins::Priority::kVideo:
            return static_cast<int>(plugins::PacedClass::kVideo);
        case plugins::Priority::kBestEffort:
            return static_cast<int>(plugins::PacedClass::kData);
        case plugins::Priority::kControl:
            break;
    }
    return -1;  // the control plane bypasses the pacer
}

// ---------------------------------------------------------------------------
// BatchItem — one entry of a drain batch (PIMPL internal type).
// ---------------------------------------------------------------------------
//
// A drain collects the packets it is about to dispatch into a **batch**
// and then hands them to the callback *without* the queue lock held.  The
// batch entry carries the accounting that the settle phase needs (the
// estimated size, the queue the packet came from and the class it was
// charged to), so that the "undo" path — the veto path — can put the
// packets back and refund what they consumed.
struct BatchItem {
    PacketRecord rec;
    /** The estimated wire size of the packet, in bytes. */
    std::uint32_t est_bytes = 0;
    /** P-family: the priority-queue index the packet came from.
     *  S-family: the StreamPriority bucket index it came from. */
    std::size_t   src_index = 0;
    /** S-family only: the paced-class FIFO index inside the bucket. */
    std::size_t   src_class = 0;
    /** S-family only: the bucket's priority level. */
    StreamPriority prio = StreamPriority::Normal;
    /** The paced class the packet was charged to, or -1 when the packet
     *  is not paced at all (control plane, or a class with rate 0). */
    int           class_idx = -1;
    /** True when the session BWE credit was charged for this packet. */
    bool          credit_charged = false;
};

/** The scratch batch of the drain that is currently running.
 *
 *  It is **thread-local** so that two concurrent drains never share it
 *  while the queue lock is released during the dispatch phase, and it is
 *  reused across calls so the send path performs no heap allocation per
 *  drain. */
thread_local std::vector<BatchItem> g_drain_batch;

/** The nesting depth of the drain that is currently running on this
 *  thread.  A drain callback may (indirectly) call back into the
 *  scheduler; a nested drain would interleave with the outer one and
 *  corrupt the accounting, so it is refused. */
thread_local int g_drain_depth = 0;

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Scheduler::Impl — all state lives here (PIMPL)
// ---------------------------------------------------------------------------

struct Scheduler::Impl {
    explicit Impl(SchedulerConfig cfg) noexcept
        : config_(cfg)
        , last_dst_addr_{}
    {}

    // ---- Configuration ---------------------------------------------------
    SchedulerConfig config_;

    // ---- Queue state -----------------------------------------------------
    // Index by priority_rank: [kControl, kAudio, kVideoKeyframe, kVideo, kBestEffort]
    std::array<std::deque<PacketRecord>, kPriorityCount> queues_{};

    // ---- Bandwidth state ------------------------------------------------
    std::uint32_t target_bps_ = 0;          // from last on_bwe_update()
    // True once on_bwe_update() has been called at least once.  While
    // false the scheduler applies NO per-drain limit (an unbounded
    // queue drains as fast as the caller asks).  This flag exists
    // because "no packets allowed" is a legitimate value after a
    // BWE update (a budget too small for a single packet) and must not
    // be confused with "no limit".
    bool bwe_limit_active_ = false;
    std::uint32_t packets_sent_this_cycle_ = 0; // reset each drain() call

    // ---- Send credit (token bucket) -------------------------------------
    //
    // The scheduler's time base.  Instead of assuming a drain cadence,
    // the scheduler *measures* the interval between two consecutive
    // drain() calls and grants send credit proportional to the elapsed
    // time:
    //
    //     credit += (target_bps / 8) * elapsed_ms      [milli-bytes]
    //
    // Every packet that leaves the wire consumes its estimated size from
    // the credit.  The credit is capped at one second's worth, so a long
    // idle gap cannot turn into an unbounded burst.
    //
    // Because the credit is driven by *wall-clock* time and not by the
    // number of drain() calls, the effective send rate is identical in a
    // host that ticks every 20 ms and in a host that ticks every 200 ms.
    //
    // The credit is kept in milli-bytes so the fractional remainder of a
    // packet's cost is not lost between drains.
    std::uint64_t credit_milli_bytes_ = 0;

    // ---- Credit clock (shared by both families) -------------------------
    //
    // The credit is granted against the *total* wall-clock time since the
    // last grant — no matter which family asked for it.  The P-family and
    // the S-family therefore cannot both claim the same elapsed time:
    // a session that drains both families every 20 ms grants the credit
    // for 20 ms once, not twice.  Two independent clocks would double the
    // effective send rate, which is exactly the bug A9 is about.
    core::TimePoint credit_last_time_{};
    bool            has_credit_last_ = false;

    // ---- Measured drain cadence (P-family) ------------------------------
    core::TimePoint last_drain_time_{};
    bool            has_last_drain_ = false;
    std::uint32_t   measured_interval_ms_ = 0;  // 0 = not measured yet
    // Bytes emitted by the last P-family drain — used to report the
    // observed egress rate in stats().
    std::uint64_t   bytes_last_drain_ = 0;

    // ---- Measured drain cadence (S-family) ------------------------------
    core::TimePoint stream_last_drain_time_{};
    bool            has_stream_last_drain_ = false;
    std::uint32_t   stream_measured_interval_ms_ = 0;
    std::uint64_t   stream_bytes_last_drain_ = 0;

    // ---- Per-class pacing buckets (v1.2.0 M3, video plan P2) ----------
    //
    // The session credit above is the **aggregate** guard: the total
    // number of bytes that may leave the machine is bounded by the BWE
    // target.  The buckets below are the **distribution**: each traffic
    // class (audio / video / data) has its own token bucket, so a
    // saturated video stream cannot take the audio's share of the link.
    //
    // A class whose rate is 0 is not paced at all — the bucket is
    // unlimited, and the class is bounded only by the session credit.
    // That is the default, so a scheduler nobody configured behaves
    // exactly as before.
    std::array<pacing::TokenBucket, plugins::kPacedClassCount> class_buckets_{};
    std::array<std::uint32_t, plugins::kPacedClassCount> class_rate_bps_{};

    // Packets that were held back because their class had no credit.
    // Telemetry for P4 (the pacing counters surfaced through the stats
    // surface).
    std::uint64_t paced_blocked_total_ = 0;

    // ---- S-family (per-stream) priority state (DC-2) -------------------
    // stream_id → StreamPriority.  Unseen streams default to Normal.
    std::unordered_map<std::uint32_t, StreamPriority> stream_priority_{};
    // stream_id → the paced traffic class the stream's packets are
    // charged to.  Unseen streams default to PacedClass::kData — the
    // conservative choice, because a stream whose traffic class is
    // unknown must not eat a class budget it does not own.
    std::unordered_map<std::uint32_t, plugins::PacedClass> stream_paced_class_{};
    // Three per-stream buckets, indexed by StreamPriority rank (High=0,
    // Normal=1, Low=2).  Each bucket holds **one FIFO per paced traffic
    // class**: a class owns its own budget, so a stream whose class is
    // out of credit must not block the streams of the other classes in
    // the same bucket (and vice versa).  Within a class the order is
    // exact FIFO.
    using StreamClassQueues = std::array<std::deque<PacketRecord>,
                                          plugins::kPacedClassCount>;
    std::array<StreamClassQueues, kStreamPriorityCount> stream_buckets_{};

    // ---- Last destination address (for empty dst in enqueue) -----------
    plugins::Addr last_dst_addr_;

    // ---- Statistics ------------------------------------------------------
    std::uint32_t dropped_total_   = 0;
    std::uint32_t sent_total_     = 0;
    std::uint64_t sent_bytes_total_ = 0;

    // ---- Packet buffer pool (P1 hardening, 2026-11-09) ------------------
    //
    // Free-list of packet payload buffers.  A buffer that was drained and
    // dispatched is pushed back here instead of being freed, and the next
    // `acquire_packet_buffer()` pops it.  In steady state the send path
    // therefore performs NO heap allocation per packet.
    //
    // The list is bounded by the queue depth: at most `max_queue_depth`
    // buffers can be in flight, so the pool never holds more than that.
    std::vector<std::vector<std::uint8_t>> buffer_pool_;

    /** Maximum number of buffers the pool may hold.  Bounded by the
     *  queue depth: at most `max_queue_depth` packets can be in flight,
     *  so the pool never holds more than that. */
    [[nodiscard]] std::size_t pool_limit() const noexcept {
        return static_cast<std::size_t>(config_.max_queue_depth);
    }

    /** Put one buffer back onto the free-list.  The buffer keeps its
     *  capacity (that is the whole point) but its contents are wiped so
     *  the pool does not keep stale packet bytes alive. */
    void recycle_buffer_locked(std::vector<std::uint8_t>&& buffer) noexcept {
        if (buffer.capacity() == 0) return;
        if (buffer_pool_.size() >= pool_limit()) return;  // pool is full
        buffer.clear();
        buffer_pool_.push_back(std::move(buffer));
    }

    /** Recycle every buffer still queued in `q` and empty the queue. */
    void recycle_queue_locked(std::deque<PacketRecord>& q) noexcept {
        while (!q.empty()) {
            recycle_buffer_locked(std::move(q.front().owned_data));
            q.pop_front();
        }
    }

    // ---- S-family (per-stream) statistics (DC-2) ------------------------
    std::uint32_t stream_dropped_total_ = 0;
    std::uint32_t stream_sent_total_   = 0;

    // ---- Thread safety --------------------------------------------------
    std::mutex mu_;

    // ---- Send-credit bookkeeping ---------------------------------------
    //
    // The credit is granted per drain call, proportionally to the measured
    // wall-clock time since the previous grant.  The P-family and the
    // S-family share **one** clock and **one** credit pool: the session
    // may not exceed the bandwidth estimate in total, whichever family
    // spends it, and the elapsed time may only be credited once.  Two
    // independent pools (or two independent clocks) would double the
    // effective send rate.
    //
    // Returns the elapsed time (in microseconds) the credit was granted
    // for, so the caller can report the observed drain cadence.
    std::uint64_t grant_credit(core::TimePoint now) noexcept {
        std::uint64_t elapsed_us = 0;
        if (has_credit_last_) {
            elapsed_us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    now - credit_last_time_).count());
        } else {
            // The very first drain has no predecessor to measure against:
            // it uses the configured drain interval as the seed.
            elapsed_us =
                static_cast<std::uint64_t>(config_.drain_interval_ms) * 1000;
        }
        credit_last_time_ = now;
        has_credit_last_ = true;

        if (bwe_limit_active_ && target_bps_ > 0) {
            // credit += bytes_per_sec * elapsed_us / 1000  [milli-bytes]
            const std::uint64_t bytes_per_sec =
                static_cast<std::uint64_t>(target_bps_) / 8;
            credit_milli_bytes_ += (bytes_per_sec * elapsed_us) / 1000;
            // Cap the credit at one second's worth so a long idle gap
            // cannot turn into an unbounded burst when the host resumes.
            const std::uint64_t cap_milli = bytes_per_sec * 1000;
            if (credit_milli_bytes_ > cap_milli) {
                credit_milli_bytes_ = cap_milli;
            }
        }
        return elapsed_us;
    }

    /** Give back credit for bytes that did **not** leave the machine.
     *
     *  Used when a drain callback vetoes a packet: the packet was taken
     *  out of the queue and its credit was spent, but the bytes never
     *  crossed the wire, so the credit must be returned. */
    void refund_credit(std::uint64_t bytes) noexcept {
        if (!bwe_limit_active_ || target_bps_ == 0 || bytes == 0) return;
        credit_milli_bytes_ += bytes * 1000;
        const std::uint64_t cap_milli =
            (static_cast<std::uint64_t>(target_bps_) / 8) * 1000;
        if (credit_milli_bytes_ > cap_milli) {
            credit_milli_bytes_ = cap_milli;
        }
    }

    /** The paced traffic class of a stream.
     *
     *  Unseen streams default to `PacedClass::kData` — the conservative
     *  choice, because a stream whose traffic class is unknown must not
     *  consume a budget it does not own. */
    [[nodiscard]] plugins::PacedClass
    paced_class_for_stream(std::uint32_t stream_id) const noexcept {
        auto it = stream_paced_class_.find(stream_id);
        if (it != stream_paced_class_.end()) return it->second;
        return plugins::PacedClass::kData;
    }
};

// ---------------------------------------------------------------------------
// Scheduler public interface
// ---------------------------------------------------------------------------

Scheduler::Scheduler(plugins::SchedulerConfig config)
    : impl_(std::make_unique<Impl>(
          config.avg_packet_size_bytes == 0
              ? plugins::SchedulerConfig{} : config)) {}

Scheduler::~Scheduler() = default;
Scheduler::Scheduler(Scheduler&&) noexcept = default;
Scheduler& Scheduler::operator=(Scheduler&&) noexcept = default;

const char* Scheduler::name() const noexcept {
    return "nimrtc::sched::Scheduler (strict-priority, 5 queues, BWE-aware)";
}

plugins::Status Scheduler::open() noexcept {
    return plugins::kOk;
}

void Scheduler::close() noexcept {
    // No external resources; reset() is the explicit state-cleanup path.
}

void Scheduler::enqueue(plugins::Priority p, core::ByteSpan data,
                        plugins::Addr dst) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mu_);

    // Copy path: hand off to the shared helper with an externally-owned
    // vector.  The buffer comes from the pool (P1 hardening, 2026-11-09),
    // so the steady-state cost of this path is a memcpy, not a malloc.
    std::vector<std::uint8_t> buf;
    if (!impl_->buffer_pool_.empty()) {
        buf = std::move(impl_->buffer_pool_.back());
        impl_->buffer_pool_.pop_back();
    }
    buf.assign(data.data(), data.data() + data.size());
    enqueue_locked(p, std::move(buf), dst);
}

void Scheduler::enqueue_owned(plugins::Priority p,
                              std::vector<std::uint8_t>&& owned_data,
                              plugins::Addr dst) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mu_);
    enqueue_locked(p, std::move(owned_data), dst);
}

void Scheduler::enqueue_locked(plugins::Priority p,
                               std::vector<std::uint8_t>&& owned_data,
                               plugins::Addr dst) noexcept {
    // Helper: total packets in all queues.
    const auto total_queued = [&]() -> std::uint32_t {
        std::uint32_t sum = 0;
        for (const auto& q : impl_->queues_) sum += static_cast<std::uint32_t>(q.size());
        return sum;
    };

    // Remember last non-empty destination address.
    if (dst.len != 0) {
        impl_->last_dst_addr_ = dst;
    } else {
        dst = impl_->last_dst_addr_;
    }

    // ---- Hard cap: max_queue_depth --------------------------------------
    if (total_queued() >= impl_->config_.max_queue_depth) {
        // Overflow: drop from lowest priority upwards until we have room.
        for (std::size_t i = kPriorityCount; i-- > 0;) {
            auto& q = impl_->queues_[i];
            if (!q.empty()) {
                // Recycle the dropped packet's buffer (P1 hardening).
                impl_->recycle_buffer_locked(std::move(q.front().owned_data));
                q.pop_front();
                ++impl_->dropped_total_;
                break;  // one drop per enqueue is sufficient
            }
        }
        // If queues are still full after dropping one (should be rare),
        // silently discard the incoming packet rather than infinite-looping.
        if (total_queued() >= impl_->config_.max_queue_depth) {
            ++impl_->dropped_total_;
            // The incoming packet is discarded — recycle its buffer too.
            impl_->recycle_buffer_locked(std::move(owned_data));
            sched_debug("Scheduler: all queues full after overflow drop, "
                        "dropping incoming packet");
            return;
        }
    }

    PacketRecord rec;
    rec.priority           = p;
    // Caller transferred ownership — move into the queue record.
    rec.owned_data         = std::move(owned_data);
    rec.dst_addr           = dst;
    rec.enqueue_time       = core::SteadyClock::now();
    rec.estimated_size_bytes = 0;  // use average from config

    impl_->queues_[plugins::priority_rank(p)].push_back(std::move(rec));
    sched_debug("Scheduler: enqueued priority=%u, queue depth now %u",
                static_cast<unsigned>(p), total_queued());
}

int Scheduler::drain(int max_packets) noexcept {
    return drain_with(max_packets, nullptr);
}

int Scheduler::drain_with(int max_packets,
                          plugins::DrainCallback cb) noexcept {
    if (max_packets <= 0) return 0;

    // A drain callback may re-enter the scheduler (for example by
    // enqueueing a packet).  A nested drain would interleave with this
    // one and corrupt the accounting, so it is refused rather than
    // silently corrupting the state.
    if (g_drain_depth > 0) return 0;

    // ---- Phase 1: collect (locked) --------------------------------------
    //
    // The packets are moved out of the queues into a thread-local batch
    // while the lock is held, and the callback is invoked in phase 2
    // **without** the lock.  Holding the lock across the callback meant
    // holding it across `ice_t_->send()` — a blocking UDP write — which
    // froze every other producer (audio, video, control) for the
    // duration of the write.  The batch is the decoupling: the lock
    // protects the queues, not the network.
    g_drain_batch.clear();
    g_drain_depth = 1;

    int sent = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mu_);

        impl_->packets_sent_this_cycle_ = 0;

        // ---- Grant the send credit for the period this drain covers -----
        //
        // The credit is proportional to the *measured* wall-clock time
        // since the previous drain, so the effective send rate is the
        // same whether the host ticks every 20 ms or every 200 ms.  The
        // very first drain has no predecessor to measure against and
        // uses the configured SchedulerConfig::drain_interval_ms as the
        // seed.
        const auto now = core::SteadyClock::now();
        const std::uint64_t elapsed_us = impl_->grant_credit(now);
        impl_->last_drain_time_ = now;
        impl_->has_last_drain_ = true;
        impl_->measured_interval_ms_ = static_cast<std::uint32_t>(
            clamp_interval_ms(static_cast<std::int64_t>(elapsed_us / 1000)));
        impl_->bytes_last_drain_ = 0;

        for (std::size_t idx = 0;
             idx < kPriorityCount && static_cast<int>(g_drain_batch.size()) < max_packets;
             ++idx) {
            auto& q = impl_->queues_[idx];

            // The control plane bypasses the pacer entirely (v1.2.0 M3, P2):
            // RTX / NACK / RTCP feedback must never wait behind a saturated
            // media bucket, and it is the *media* traffic that has to be
            // shaped.  The control packets are small and rare, so letting
            // them through does not measurably change the load on the link.
            const bool is_control =
                idx == plugins::priority_rank(plugins::Priority::kControl);

            // The paced class of this queue (-1 for the control plane).
            const int class_idx = is_control ? -1 : paced_class_index(
                static_cast<plugins::Priority>(idx));

            while (!q.empty() &&
                   static_cast<int>(g_drain_batch.size()) < max_packets) {
                // If a BWE limit is active, stop once the send credit is
                // exhausted: the packet stays queued and goes out on a later
                // drain, when enough time has passed to earn the credit.
                // The credit is only consumed once the packet really left
                // the queue — a vetoed packet keeps its credit.
                std::uint32_t peek_bytes = 0;
                if (!is_control && impl_->bwe_limit_active_) {
                    peek_bytes = estimated_packet_bytes(
                        q.front(), impl_->config_.avg_packet_size_bytes);
                    const std::uint64_t cost_milli =
                        static_cast<std::uint64_t>(peek_bytes) * 1000;
                    if (impl_->credit_milli_bytes_ < cost_milli) {
                        break;  // not enough credit yet — keep the packet queued
                    }
                }

                // The per-class pacer (v1.2.0 M3, P2).  A class whose rate is
                // 0 is not paced; a class with a rate holds the packet back
                // until its own bucket has earned the bytes.  The packet
                // stays queued, so the class keeps its place in the queue.
                const bool class_paced =
                    class_idx >= 0 &&
                    impl_->class_rate_bps_[static_cast<std::size_t>(class_idx)] > 0;
                if (class_paced) {
                    peek_bytes = estimated_packet_bytes(
                        q.front(), impl_->config_.avg_packet_size_bytes);
                    // `available()` refills the bucket as a side effect, so it
                    // is not a const member — the bucket must be reached
                    // through a mutable reference here.
                    auto& bucket =
                        impl_->class_buckets_[static_cast<std::size_t>(class_idx)];
                    if (bucket.available(now) < peek_bytes) {
                        ++impl_->paced_blocked_total_;
                        break;  // this class is out of credit — keep the packet
                    }
                }

                // Take ownership of the packet.  The record leaves the queue
                // here and lives in the batch until the dispatch phase
                // settles it.
                BatchItem item;
                item.rec = std::move(q.front());
                q.pop_front();
                item.est_bytes = estimated_packet_bytes(
                    item.rec, impl_->config_.avg_packet_size_bytes);
                item.src_index = idx;
                item.class_idx = class_paced ? class_idx : -1;

                // Charge the credit and the pacing bucket **now**, while
                // the lock is held.  A veto in phase 2 refunds both.
                if (!is_control && impl_->bwe_limit_active_) {
                    impl_->credit_milli_bytes_ -=
                        static_cast<std::uint64_t>(item.est_bytes) * 1000;
                    item.credit_charged = true;
                }
                if (class_paced) {
                    impl_->class_buckets_[static_cast<std::size_t>(class_idx)]
                        .try_consume(item.est_bytes, now);
                }

                g_drain_batch.push_back(std::move(item));
            }
        }
    }  // <- the queue lock is released here

    // ---- Phase 2: dispatch (unlocked) -----------------------------------
    //
    // The callback runs without the queue lock, so a blocking network
    // write inside it cannot stall the producers.
    if (cb) {
        for (auto& item : g_drain_batch) {
            core::ByteSpan span{item.rec.owned_data.data(),
                                item.rec.owned_data.size()};
            if (!cb(item.rec.priority, span)) {
                // The caller vetoed the packet.  Everything from this
                // packet onwards goes back into its queue — the packet
                // itself and the ones that were collected but not yet
                // dispatched — and the credit and pacing tokens they
                // consumed are refunded.
                //
                // The records are put back as-is (not counted as sent), so
                // a vetoing transport does not lose the packets and the
                // statistics stay truthful.  Draining stops entirely — a
                // veto means "the transport is not accepting traffic right
                // now", so lower-priority queues must not be attempted
                // either.
                const std::size_t veto_index =
                    static_cast<std::size_t>(&item - g_drain_batch.data());
                undo_batch(veto_index, /*s_family=*/false);
                g_drain_depth = 0;
                g_drain_batch.clear();
                return sent;
            }
            ++sent;
        }
    } else {
        sent = static_cast<int>(g_drain_batch.size());
    }

    // ---- Phase 3: settle (locked) --------------------------------------
    //
    // The packets really left the machine: the statistics are updated and
    // the buffers go back onto the free-list.
    {
        std::lock_guard<std::mutex> lock(impl_->mu_);
        for (auto& item : g_drain_batch) {
            ++impl_->packets_sent_this_cycle_;
            ++impl_->sent_total_;
            impl_->sent_bytes_total_ += item.est_bytes;
            impl_->bytes_last_drain_ += item.est_bytes;
            // The packet has been dispatched: the drain callback is done
            // with the bytes, so the buffer can go back onto the free-list
            // instead of being freed (P1 hardening, 2026-11-09).  This is
            // what removes the per-packet heap allocation from the send
            // path.
            impl_->recycle_buffer_locked(std::move(item.rec.owned_data));
        }
    }

    g_drain_depth = 0;
    g_drain_batch.clear();
    return sent;
}

void Scheduler::on_bwe_update(std::uint32_t target_bps) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mu_);

    impl_->target_bps_       = target_bps;

    if (target_bps == 0) {
        // Total sender pause: keep all kControl, drop everything else.
        // The BWE limit is left inactive (not "0 credit") so the surviving
        // kControl packets still drain — a pause is expressed by emptying
        // the non-control queues, not by blocking the control plane.
        impl_->credit_milli_bytes_ = 0;
        impl_->bwe_limit_active_   = false;
        for (std::size_t idx = 1; idx < kPriorityCount; ++idx) {
            impl_->dropped_total_ +=
                static_cast<std::uint32_t>(impl_->queues_[idx].size());
            impl_->recycle_queue_locked(impl_->queues_[idx]);
        }
        sched_debug("Scheduler: sender pause — all non-control packets dropped");
        return;
    }

    // The congestion-window horizon: how much backlog (in bytes) may
    // stay queued before the scheduler starts trimming low-priority
    // queues.  This is a *fixed* horizon (the configured
    // SchedulerConfig::drain_interval_ms), deliberately independent of
    // how fast the host ticks: the amount of data we tolerate in the
    // send queue must not change with the host's tick rate.
    //
    // The *send rate* is enforced separately, by the time-based credit in
    // drain_with(), which follows the measured wall-clock interval
    // between drains.
    const std::uint32_t horizon_ms =
        impl_->config_.drain_interval_ms == 0 ? 1u
                                              : impl_->config_.drain_interval_ms;

    // Drain-window budget (used for the queue trim below):
    //   bytes_per_drain   = (target_bps / 8) * horizon_ms / 1000
    //
    // NOTE: the previous implementation stored *packets per second* in
    // allowed_per_drain_ and compared it against the per-drain counter,
    // which over-allowed by a factor of (1000 / interval_ms) — a 125 ms
    // drain period let through 8x the budgeted rate.  The send rate is
    // now enforced by the time-based credit in drain_with(), which does
    // not depend on the number of drain() calls.
    const std::uint64_t budget_bytes =
        bytes_per_drain_interval(target_bps, horizon_ms);
    impl_->bwe_limit_active_  = true;

    sched_debug("Scheduler: BWE update target=%u bps, horizon=%u ms, "
                "window budget=%llu B",
                target_bps, horizon_ms,
                static_cast<unsigned long long>(budget_bytes));

    // Congestion signalled: trim the queue so its total byte cost fits
    // within one drain interval's budget.  Packets trimmed from the
    // lowest priority queues are dropped (not "sent"); packets retained
    // stay queued and will be sent by the next drain() call.
    //
    // With the default 125 ms time base this yields:
    //   - 100 kbps → ~1562 bytes/drain → fits 1 packet of 1000 B; a 5-deep
    //     best-effort queue (5000 B) is dropped entirely.
    //   - 256 kbps → ~4000 bytes/drain → fits 4 packets of 1000 B; all
    //     4 moderate-congestion packets preserved.
    //   - 10 kbps → ~156 bytes/drain → fits 0 packets → all dropped.
    //   - 5 Mbps → ~78 KiB/drain → no drops.
    //   - 20 ms host tick → the same bitrate yields 1/6 of the above
    //     budget per drain, i.e. the same rate per second.

    // Compute total queue bytes excluding kControl (which is never dropped).
    auto total_noncontrol_bytes = [&]() -> std::uint64_t {
        std::uint64_t sum = 0;
        for (std::size_t idx = 1; idx < kPriorityCount; ++idx) {
            const auto& q = impl_->queues_[idx];
            sum += static_cast<std::uint64_t>(q.size()) *
                   impl_->config_.avg_packet_size_bytes;
        }
        return sum;
    };

    if (total_noncontrol_bytes() <= budget_bytes) {
        // Queue fits within budget — nothing to drop.
        return;
    }

    // Walk from lowest priority upwards; for each queue make an atomic
    // decision: either keep the whole queue (subtract from budget) or
    // drop the whole queue.  This produces predictable per-priority
    // trimming that matches the test expectations (no partial-queue
    // survivors from a single priority level).
    std::uint64_t remaining_budget = budget_bytes;
    for (std::size_t idx = kPriorityCount; idx-- > 0;) {
        if (idx == plugins::priority_rank(plugins::Priority::kControl)) continue;

        auto& q = impl_->queues_[idx];
        const std::uint64_t queue_bytes =
            static_cast<std::uint64_t>(q.size()) *
            impl_->config_.avg_packet_size_bytes;

        if (queue_bytes <= remaining_budget) {
            // Whole queue fits — keep it.
            remaining_budget -= queue_bytes;
            // Continue to next (higher-priority) queue.
        } else {
            // Even a single packet in this queue is too expensive to keep
            // given the remaining budget — drop the whole queue.
            impl_->dropped_total_ += static_cast<std::uint32_t>(q.size());
            impl_->recycle_queue_locked(q);
            // remaining_budget unchanged: dropping frees no additional bytes
            // for higher-priority queues — they're still over budget.
            // Continue iterating so the loop reaches kAudio / kVideoKeyframe
            // and decides on those queues too.
        }
    }
}

void Scheduler::reset() noexcept {
    std::lock_guard<std::mutex> lock(impl_->mu_);

    for (auto& q : impl_->queues_) {
        impl_->recycle_queue_locked(q);
    }
    impl_->target_bps_          = 0;
    impl_->credit_milli_bytes_  = 0;
    impl_->bwe_limit_active_    = false;
    impl_->packets_sent_this_cycle_ = 0;
    impl_->has_last_drain_      = false;
    impl_->measured_interval_ms_ = 0;
    impl_->bytes_last_drain_     = 0;
    impl_->has_credit_last_     = false;
    impl_->has_stream_last_drain_ = false;
    impl_->stream_measured_interval_ms_ = 0;
    impl_->stream_bytes_last_drain_ = 0;
    impl_->dropped_total_       = 0;
    impl_->sent_total_          = 0;
    impl_->sent_bytes_total_    = 0;
    impl_->stream_priority_.clear();
    impl_->stream_paced_class_.clear();
    for (auto& bucket : impl_->stream_buckets_) {
        for (auto& class_queue : bucket) {
            impl_->recycle_queue_locked(class_queue);
        }
    }
    impl_->stream_dropped_total_ = 0;
    impl_->stream_sent_total_   = 0;
    impl_->last_dst_addr_       = {};

    // The pacing buckets start over as well: the credit they hold
    // belonged to the session that just ended.
    const auto now = core::SteadyClock::now();
    for (auto& bucket : impl_->class_buckets_) {
        bucket.reset(now);
    }
    impl_->paced_blocked_total_ = 0;
}

// ---------------------------------------------------------------------------
// Per-class pacing (v1.2.0 M3, video plan P2)
// ---------------------------------------------------------------------------

void Scheduler::set_pacing_rate(plugins::PacedClass cls,
                                 std::uint32_t bps) noexcept {
    const std::size_t idx = static_cast<std::size_t>(cls);
    if (idx >= plugins::kPacedClassCount) return;

    std::lock_guard<std::mutex> lock(impl_->mu_);

    // An unchanged rate must not disturb the bucket: re-applying the
    // configuration on every engine tick would otherwise throw away the
    // credit the bucket has earned.
    if (impl_->class_rate_bps_[idx] == bps) return;

    impl_->class_rate_bps_[idx] = bps;

    pacing::PacerConfig cfg;
    cfg.rate_bps = bps;
    // The credit that was already earned is kept (and clamped to the new
    // burst ceiling by set_config): lowering the rate does not throw away
    // what was earned, raising it does not grant a free burst.
    impl_->class_buckets_[idx].set_config(cfg);

    sched_debug("Scheduler: pacing rate for class %zu set to %u bps",
                idx, bps);
}

std::uint32_t Scheduler::pacing_rate(plugins::PacedClass cls) const noexcept {
    const std::size_t idx = static_cast<std::size_t>(cls);
    if (idx >= plugins::kPacedClassCount) return 0;

    std::lock_guard<std::mutex> lock(impl_->mu_);
    return impl_->class_rate_bps_[idx];
}

plugins::SchedulerStats Scheduler::stats() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mu_);

    plugins::SchedulerStats s;
    for (std::size_t i = 0; i < kPriorityCount; ++i) {
        s.queued_packets += static_cast<std::uint32_t>(impl_->queues_[i].size());
    }
    // The S-family buckets are queues too — a packet waiting in a stream
    // bucket is just as "queued" as one waiting in a priority queue.
    for (std::size_t i = 0; i < kStreamPriorityCount; ++i) {
        for (const auto& class_queue : impl_->stream_buckets_[i]) {
            s.queued_packets +=
                static_cast<std::uint32_t>(class_queue.size());
        }
    }
    s.dropped_packets   = impl_->dropped_total_;
    s.sent_packets      = impl_->sent_total_;
    s.sent_bytes        = impl_->sent_bytes_total_;
    s.target_bitrate_bps = impl_->target_bps_;
    // Observed egress rate of the last drain: bytes emitted during the
    // measured drain interval, expressed in bits per second.  This is
    // the *actual* rate, which — unlike the target — reflects what the
    // host's tick cadence allowed.
    s.current_bitrate_bps = 0;
    if (impl_->has_last_drain_ && impl_->measured_interval_ms_ > 0) {
        s.current_bitrate_bps = static_cast<std::uint32_t>(
            (impl_->bytes_last_drain_ * 8 * 1000) / impl_->measured_interval_ms_);
    }
    s.paced_blocked_packets = impl_->paced_blocked_total_;
    return s;
}

plugins::SchedulerConfig Scheduler::config() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mu_);
    return impl_->config_;
}

// ---------------------------------------------------------------------------
// Packet buffer pool (P1 hardening, 2026-11-09)
// ---------------------------------------------------------------------------
//
// The send path used to allocate and free one heap buffer per outbound
// packet.  With the pool the buffer is taken from a bounded free-list,
// filled by the caller, handed to the scheduler with `enqueue_owned()`,
// and returned to the list once the packet has been drained and
// dispatched.  In steady state the send path performs no heap allocation.
//
// The pool is bounded by `max_queue_depth`: at most that many packets can
// be in flight, so the pool never holds more memory than the queue could
// anyway.

std::vector<std::uint8_t>
Scheduler::acquire_packet_buffer(std::size_t capacity_hint) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mu_);

    std::vector<std::uint8_t> buffer;
    if (!impl_->buffer_pool_.empty()) {
        buffer = std::move(impl_->buffer_pool_.back());
        impl_->buffer_pool_.pop_back();
    }
    // The recycled buffer usually already has enough capacity (the packet
    // sizes are stable).  Only when it is too small do we pay for a
    // reallocation.
    if (capacity_hint != 0 && buffer.capacity() < capacity_hint) {
        buffer.reserve(capacity_hint);
    }
    return buffer;
}

void Scheduler::release_packet_buffer(
    std::vector<std::uint8_t>&& buffer) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mu_);
    impl_->recycle_buffer_locked(std::move(buffer));
}

// ===============================================================================
// DC-2: S-family (per-stream) priority methods
// ===============================================================================

void Scheduler::set_stream_priority(std::uint32_t stream_id,
                                     StreamPriority level) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mu_);

    // Find the previous priority for this stream (if any).
    auto it = impl_->stream_priority_.find(stream_id);
    const StreamPriority prev =
        (it != impl_->stream_priority_.end()) ? it->second
                                              : StreamPriority::Normal;

    // If the level didn't change, no-op (preserves FIFO position).
    if (prev == level) {
        impl_->stream_priority_[stream_id] = level;
        return;
    }

    // Move all already-queued packets for this stream from the old bucket
    // to the new bucket.  We splice the matching deque entries into the
    // new bucket in their original order.  This satisfies DC-2's
    // "set_stream_priority moves the stream between buckets atomically"
    // contract: subsequent drain() observes the new ordering immediately.
    //
    // `nimrtc::sched::stream_priority_rank` is called *qualified* here:
    // `StreamPriority` is an alias of `plugins::StreamPriority`, so an
    // unqualified call would also find `plugins::stream_priority_rank`
    // through ADL and the two candidates would be ambiguous.
    auto& old_bucket =
        impl_->stream_buckets_[nimrtc::sched::stream_priority_rank(prev)];
    auto& new_bucket =
        impl_->stream_buckets_[nimrtc::sched::stream_priority_rank(level)];

    // A bucket holds one FIFO per paced class, so the migration walks the
    // class queues in turn.  Within a class the packets keep their
    // relative order; the packets of the moved stream are appended to the
    // tail of the destination class queue.
    std::size_t moved_count = 0;
    for (std::size_t c = 0; c < plugins::kPacedClassCount; ++c) {
        auto& old_class_queue = old_bucket[c];
        auto& new_class_queue = new_bucket[c];

        std::deque<PacketRecord> moved;
        std::deque<PacketRecord> retained;
        while (!old_class_queue.empty()) {
            PacketRecord rec = std::move(old_class_queue.front());
            old_class_queue.pop_front();
            if (rec.stream_id == stream_id) {
                moved.push_back(std::move(rec));
            } else {
                retained.push_back(std::move(rec));
            }
        }
        old_class_queue = std::move(retained);
        while (!moved.empty()) {
            new_class_queue.push_back(std::move(moved.front()));
            moved.pop_front();
        }
        moved_count += moved.size();
    }

    impl_->stream_priority_[stream_id] = level;
    sched_debug("Scheduler: stream %u priority %u -> %u, moved %zu packets",
                stream_id, static_cast<unsigned>(prev),
                static_cast<unsigned>(level), moved_count);
}

StreamPriority Scheduler::get_stream_priority(std::uint32_t stream_id) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mu_);
    auto it = impl_->stream_priority_.find(stream_id);
    if (it != impl_->stream_priority_.end()) return it->second;
    return StreamPriority::Normal;  // default
}

plugins::PacedClass
Scheduler::stream_paced_class(std::uint32_t stream_id) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mu_);
    return impl_->paced_class_for_stream(stream_id);
}

void Scheduler::set_stream_paced_class(std::uint32_t stream_id,
                                       plugins::PacedClass cls) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mu_);
    impl_->stream_paced_class_[stream_id] = cls;
}

void Scheduler::enqueue_for_stream_owned(std::uint32_t stream_id,
                                         std::vector<std::uint8_t>&& owned_data,
                                         plugins::Addr dst) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mu_);

    // Resolve the stream's current priority.
    StreamPriority level = StreamPriority::Normal;
    auto it = impl_->stream_priority_.find(stream_id);
    if (it != impl_->stream_priority_.end()) {
        level = it->second;
    } else {
        // First time we see this stream — store Normal so repeated calls
        // hit the fast map lookup path.
        impl_->stream_priority_.emplace(stream_id, StreamPriority::Normal);
    }

    // Remember last non-empty destination.
    if (dst.len != 0) {
        impl_->last_dst_addr_ = dst;
    } else {
        dst = impl_->last_dst_addr_;
    }

    PacketRecord rec;
    rec.priority           = plugins::Priority::kBestEffort;
    rec.stream_id          = stream_id;
    rec.owned_data         = std::move(owned_data);
    rec.dst_addr           = dst;
    rec.enqueue_time       = core::SteadyClock::now();
    rec.estimated_size_bytes = 0;

    // The packet goes into the FIFO of its stream's *paced class* inside
    // the bucket of its StreamPriority.  See the comment on
    // `Impl::stream_buckets_`.
    const auto class_idx =
        static_cast<std::size_t>(impl_->paced_class_for_stream(stream_id));
    auto& bucket = impl_->stream_buckets_[nimrtc::sched::stream_priority_rank(level)];
    bucket[class_idx].push_back(std::move(rec));
    sched_debug("Scheduler: enqueued stream=%u into bucket=%u class=%zu",
                stream_id, static_cast<unsigned>(level), class_idx);
}

void Scheduler::enqueue_for_stream(std::uint32_t stream_id,
                                   core::ByteSpan data,
                                   plugins::Addr dst) noexcept {
    std::vector<std::uint8_t> buf;
    buf.assign(data.data(), data.data() + data.size());
    enqueue_for_stream_owned(stream_id, std::move(buf), dst);
}

// ---------------------------------------------------------------------------
// S-family drain — the flow-controlled counterpart of drain_with()
// ---------------------------------------------------------------------------
//
// The S-family shares the session's send credit and the per-class pacing
// buckets with the P-family (A9-a / A9-b).  Before this was wired up the
// S-family path bypassed both, so a stream that used the per-stream
// priority API could push the total egress rate arbitrarily far past the
// bandwidth estimate.  The two families now draw from the *same* credit
// pool against the *same* clock, so the session's send rate is bounded
// no matter which family spends it.

int Scheduler::select_next_packet(int max_packets,
                                  plugins::StreamDrainCallback cb) noexcept {
    if (max_packets <= 0) return 0;

    // A nested drain would interleave with the running one — see the
    // matching guard in drain_with().
    if (g_drain_depth > 0) return 0;

    g_drain_batch.clear();
    g_drain_depth = 1;

    int sent = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mu_);

        // The credit is granted against the shared clock: the S-family
        // does not get its own time base.  A session that drains both
        // families every 20 ms therefore grants 20 ms worth of credit —
        // once — not twice.
        const auto now = core::SteadyClock::now();
        const std::uint64_t elapsed_us = impl_->grant_credit(now);
        impl_->stream_last_drain_time_ = now;
        impl_->has_stream_last_drain_ = true;
        impl_->stream_measured_interval_ms_ = static_cast<std::uint32_t>(
            clamp_interval_ms(static_cast<std::int64_t>(elapsed_us / 1000)));
        impl_->stream_bytes_last_drain_ = 0;

        // Walk from highest priority (0 = High) to lowest (2 = Low).
        bool budget_exhausted = false;
        for (std::size_t idx = 0;
             idx < kStreamPriorityCount &&
             static_cast<int>(g_drain_batch.size()) < max_packets &&
             !budget_exhausted;
             ++idx) {
            auto& bucket = impl_->stream_buckets_[idx];

            // The bucket priority determines the StreamPriority reported
            // to the callback.
            const StreamPriority bucket_prio = static_cast<StreamPriority>(idx);

            // Each paced class owns its own FIFO inside the bucket, so a
            // class whose budget is exhausted does not block the other
            // classes.  The classes are visited round-robin so that no
            // class can starve the others.
            bool progress = true;
            while (progress &&
                   static_cast<int>(g_drain_batch.size()) < max_packets) {
                progress = false;
                for (std::size_t c = 0;
                     c < plugins::kPacedClassCount &&
                     static_cast<int>(g_drain_batch.size()) < max_packets;
                     ++c) {
                    auto& class_queue = bucket[c];
                    if (class_queue.empty()) continue;

                    // ---- A9-a: the shared session credit ----------------
                    // The S-family packets are charged to the same credit
                    // pool as the P-family packets.  Without this check a
                    // stream on the S-family could exceed the BWE budget.
                    //
                    // The session credit is a *global* budget: when it is
                    // exhausted, no packet of any stream can go, so the
                    // whole selection stops here.
                    if (impl_->bwe_limit_active_) {
                        const std::uint32_t peek_bytes = estimated_packet_bytes(
                            class_queue.front(),
                            impl_->config_.avg_packet_size_bytes);
                        const std::uint64_t cost_milli =
                            static_cast<std::uint64_t>(peek_bytes) * 1000;
                        if (impl_->credit_milli_bytes_ < cost_milli) {
                            // The budget is spent — stop the selection, but
                            // keep the packets that were already collected.
                            budget_exhausted = true;
                            break;
                        }
                    }

                    // ---- A9-b: the per-class pacing bucket --------------
                    const bool class_paced = impl_->class_rate_bps_[c] > 0;
                    if (class_paced) {
                        const std::uint32_t peek_bytes = estimated_packet_bytes(
                            class_queue.front(),
                            impl_->config_.avg_packet_size_bytes);
                        auto& class_bucket = impl_->class_buckets_[c];
                        if (class_bucket.available(now) < peek_bytes) {
                            ++impl_->paced_blocked_total_;
                            continue;  // this class waits — try the next one
                        }
                    }

                    BatchItem item;
                    item.rec = std::move(class_queue.front());
                    class_queue.pop_front();
                    item.est_bytes = estimated_packet_bytes(
                        item.rec, impl_->config_.avg_packet_size_bytes);
                    item.src_index = idx;
                    item.src_class = c;
                    item.prio = bucket_prio;
                    item.class_idx = class_paced ? static_cast<int>(c) : -1;

                    // Charge the credit and the pacing bucket now; a veto
                    // refunds both.
                    if (impl_->bwe_limit_active_) {
                        impl_->credit_milli_bytes_ -=
                            static_cast<std::uint64_t>(item.est_bytes) * 1000;
                        item.credit_charged = true;
                    }
                    if (class_paced) {
                        impl_->class_buckets_[c].try_consume(item.est_bytes, now);
                    }

                    g_drain_batch.push_back(std::move(item));
                    progress = true;
                }
            }
        }
    }  // <- the queue lock is released here

    // ---- Dispatch (unlocked) --------------------------------------------
    if (cb) {
        for (auto& item : g_drain_batch) {
            core::ByteSpan span{item.rec.owned_data.data(),
                                item.rec.owned_data.size()};
            if (!cb(item.rec.stream_id, item.prio, span)) {
                // Veto — put the packets back and refund what they spent.
                const std::size_t veto_index =
                    static_cast<std::size_t>(&item - g_drain_batch.data());
                undo_batch(veto_index, /*s_family=*/true);
                g_drain_depth = 0;
                g_drain_batch.clear();
                return sent;
            }
            ++sent;
        }
    } else {
        sent = static_cast<int>(g_drain_batch.size());
    }

    // ---- Settle (locked) -------------------------------------------------
    {
        std::lock_guard<std::mutex> lock(impl_->mu_);
        for (auto& item : g_drain_batch) {
            ++impl_->stream_sent_total_;
            ++impl_->sent_total_;
            impl_->sent_bytes_total_ += item.est_bytes;
            impl_->stream_bytes_last_drain_ += item.est_bytes;
            impl_->recycle_buffer_locked(std::move(item.rec.owned_data));
        }
    }

    g_drain_depth = 0;
    g_drain_batch.clear();
    return sent;
}

int Scheduler::select_next_packet(int max_packets,
                                  std::nullptr_t) noexcept {
    // Drain the packets without dispatching them.  They are accounted as
    // sent (they left the queue), exactly as the callback-less `drain()`
    // does for the P-family.
    return select_next_packet(
        max_packets,
        [](std::uint32_t, StreamPriority, core::ByteSpan) -> bool {
            return true;
        });
}

int Scheduler::select_next_packet(int max_packets,
                                  plugins::DrainCallback cb) noexcept {
    // The legacy shape only sees the per-packet Priority and the byte
    // span; the stream identity and the bucket level are not part of it.
    // The adapter reports them through the richer callback and drops the
    // stream-specific parts.
    return select_next_packet(
        max_packets,
        [cb](std::uint32_t /*stream_id*/, StreamPriority /*level*/,
             core::ByteSpan span) -> bool {
            if (!cb) return true;
            return cb(plugins::Priority::kBestEffort, span);
        });
}

// ---------------------------------------------------------------------------
// undo_batch — the veto path
// ---------------------------------------------------------------------------
//
// The drain collects the packets it is about to dispatch into a batch
// (under the lock) and then dispatches them (without the lock).  When the
// callback vetoes a packet, every packet from the vetoed one onwards must
// go back where it came from — and the credit and the pacing tokens the
// collection spent must be returned, because those bytes never left the
// machine.
//
// The packets are restored at the FRONT of their queue, in their original
// order: they were queued before the packets that are still in the queue,
// so putting them back at the front preserves the FIFO order of the
// queue.  (The previous behaviour silently *dropped* the vetoed packet,
// which lost data and left the "dropped" statistic understating the real
// loss.)

void Scheduler::undo_batch(std::size_t first_vetoed, bool s_family) noexcept {
    if (first_vetoed >= g_drain_batch.size()) return;

    std::lock_guard<std::mutex> lock(impl_->mu_);

    // ---- Refund what the collection spent ------------------------------
    std::uint64_t refund_bytes = 0;
    for (std::size_t i = first_vetoed; i < g_drain_batch.size(); ++i) {
        const auto& item = g_drain_batch[i];
        if (item.credit_charged) {
            refund_bytes += item.est_bytes;
        }
        if (item.class_idx >= 0) {
            // The pacing bucket must get the bytes back as well, otherwise
            // a vetoing transport would slowly starve its own class.
            impl_->class_buckets_[static_cast<std::size_t>(item.class_idx)]
                .refund(item.est_bytes);
        }
    }
    if (refund_bytes > 0) {
        impl_->refund_credit(refund_bytes);
    }

    // ---- Put the packets back ------------------------------------------
    //
    // The batch is ordered by queue: all the packets of queue 0 first,
    // then queue 1, and so on.  Restoring them in *reverse* batch order
    // with `push_front` therefore restores each queue in its original
    // FIFO order.
    for (std::size_t i = g_drain_batch.size(); i-- > first_vetoed;) {
        auto& item = g_drain_batch[i];
        if (s_family) {
            impl_->stream_buckets_[item.src_index][item.src_class]
                .push_front(std::move(item.rec));
        } else {
            impl_->queues_[item.src_index].push_front(std::move(item.rec));
        }
    }
}

// ===============================================================================
// C ABI wrapper implementations
// ===============================================================================

// SchedulerHandle is forward-declared in sched.hpp as an opaque type.
// The full definition lives here so the C ABI functions can dereference
// it without exposing Scheduler's internals through the header.
struct SchedulerHandle {
    std::unique_ptr<Scheduler> sched;
    explicit SchedulerHandle(plugins::SchedulerConfig cfg)
        : sched(std::make_unique<Scheduler>(cfg)) {}
};

SchedulerHandle* sched_create(const plugins::SchedulerConfig* config) noexcept {
    try {
        plugins::SchedulerConfig cfg = config ? *config : plugins::SchedulerConfig{};
        return new SchedulerHandle(cfg);
    } catch (...) {
        return nullptr;
    }
}

void sched_destroy(SchedulerHandle* h) noexcept {
    delete h;
}

void sched_set_stream_priority(SchedulerHandle* h,
                               std::uint32_t stream_id,
                               StreamPriority level) noexcept {
    if (!h) return;
    h->sched->set_stream_priority(stream_id, level);
}

StreamPriority sched_get_stream_priority(const SchedulerHandle* h,
                                        std::uint32_t stream_id) noexcept {
    if (!h) return StreamPriority::Normal;
    return h->sched->get_stream_priority(stream_id);
}

void sched_enqueue_stream_owned(SchedulerHandle* h,
                                std::uint32_t stream_id,
                                const std::uint8_t* data,
                                std::size_t len,
                                const plugins::Addr* dst) noexcept {
    if (!h) return;
    plugins::Addr addr = dst ? *dst : plugins::Addr{};
    std::vector<std::uint8_t> buf;
    if (data && len > 0) buf.assign(data, data + len);
    h->sched->enqueue_for_stream_owned(stream_id, std::move(buf), addr);
}

int sched_select_next_packet(SchedulerHandle* h,
                             int max_packets,
                             void (*on_packet)(std::uint32_t stream_id,
                                               StreamPriority level,
                                               const std::uint8_t* data,
                                               std::size_t len,
                                               void* user_data),
                             void* user_data) noexcept {
    if (!h) return 0;
    if (!on_packet) {
        // Drain without a callback.
        return h->sched->select_next_packet(max_packets, nullptr);
    }
    // The C function pointer is wrapped into the stream-aware callback,
    // so the C ABI caller receives the real stream id and the real bucket
    // priority — not placeholders.
    return h->sched->select_next_packet(
        max_packets,
        [on_packet, user_data](std::uint32_t stream_id, StreamPriority level,
                               core::ByteSpan span) -> bool {
            on_packet(stream_id, level, span.data(), span.size(), user_data);
            return true;
        });
}

}  // namespace nimrtc::sched
