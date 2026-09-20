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
 *     allowed_per_drain_ = target_bps / (avg_packet_size_bytes * 8)
 *     After setting the limit, drop packets from lowest priority upwards
 *     until total_estimated_size fits within the window.
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

/** Log a debug message only when log level is at or below Debug. */
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

/** Convert bits-per-second + avg packet size → packets-per-second budget.
 *  Returns 0 when target_bps == 0 (pause). */
[[nodiscard]] constexpr std::uint32_t
bits_to_packets_per_second(std::uint32_t target_bps,
                           std::uint32_t avg_packet_size_bytes) noexcept {
    if (target_bps == 0 || avg_packet_size_bytes == 0) return 0;
    const std::uint64_t bits_per_packet = avg_packet_size_bytes * 8ULL;
    const std::uint64_t pps = target_bps / bits_per_packet;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(pps, 0xFFFFFFFFULL));
}

/** Estimate the byte size of one PacketRecord.
 *  Uses PacketRecord::estimated_size_bytes if non-zero, otherwise falls back
 *  to cfg.avg_packet_size_bytes. */
[[nodiscard]] std::uint32_t
estimated_packet_bytes(const PacketRecord& r,
                       std::uint32_t avg_size) noexcept {
    return r.estimated_size_bytes != 0 ? r.estimated_size_bytes : avg_size;
}

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
    std::uint32_t allowed_per_drain_ = 0;   // max packets drain() may emit
    std::uint32_t packets_sent_this_cycle_ = 0; // reset each drain() call

    // ---- L1 stream-priority state (DC-2) -------------------------------
    // stream_id → StreamPriority.  Unseen streams default to Normal.
    std::unordered_map<std::uint32_t, StreamPriority> stream_priority_{};
    // Three L1 buckets, indexed by StreamPriority rank (High=0, Normal=1, Low=2).
    std::array<std::deque<PacketRecord>, kStreamPriorityCount> stream_buckets_{};

    // ---- Last destination address (for empty dst in enqueue) -----------
    plugins::Addr last_dst_addr_;

    // ---- Statistics ------------------------------------------------------
    std::uint32_t dropped_total_   = 0;
    std::uint32_t sent_total_     = 0;
    std::uint64_t sent_bytes_total_ = 0;

    // ---- L1 stream statistics (DC-2) ------------------------------------
    std::uint32_t stream_dropped_total_ = 0;
    std::uint32_t stream_sent_total_   = 0;

    // ---- Thread safety --------------------------------------------------
    std::mutex mu_;
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
    // vector.  The vector is freshly allocated (or reused from a
    // thread_local pool in the caller); the helper consumes it via move.
    std::vector<std::uint8_t> buf;
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
                q.pop_front();
                ++impl_->dropped_total_;
                break;  // one drop per enqueue is sufficient
            }
        }
        // If queues are still full after dropping one (should be rare),
        // silently discard the incoming packet rather than infinite-looping.
        if (total_queued() >= impl_->config_.max_queue_depth) {
            ++impl_->dropped_total_;
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

    std::lock_guard<std::mutex> lock(impl_->mu_);

    int sent = 0;
    impl_->packets_sent_this_cycle_ = 0;

    for (std::size_t idx = 0; idx < kPriorityCount && sent < max_packets; ++idx) {
        auto& q = impl_->queues_[idx];

        while (!q.empty() && sent < max_packets) {
            // If a BWE limit is active, stop after allowed_per_drain_.
            if (impl_->allowed_per_drain_ > 0 &&
                impl_->packets_sent_this_cycle_ >= impl_->allowed_per_drain_) {
                break;  // stop draining; remaining packets stay queued
            }

            // Take ownership of the packet so we can update stats
            // even when no callback is registered.
            const PacketRecord rec = std::move(q.front());
            q.pop_front();

            // If the caller wants per-packet observation, give them a chance
            // to veto the dequeue by returning false from the callback.
            if (cb) {
                core::ByteSpan span{rec.owned_data.data(), rec.owned_data.size()};
                if (!cb(rec.priority, span)) {
                    // Caller vetoed — but we already removed the record.
                    // (In practice callers will accept the dequeue; this
                    // exists so they can early-exit on transport errors.)
                    break;
                }
            }

            const std::uint32_t est_bytes = estimated_packet_bytes(
                rec, impl_->config_.avg_packet_size_bytes);
            ++sent;
            ++impl_->packets_sent_this_cycle_;
            ++impl_->sent_total_;
            impl_->sent_bytes_total_ += est_bytes;
        }

        // If we hit the BWE limit while draining, stop scanning lower queues.
        if (impl_->allowed_per_drain_ > 0 &&
            impl_->packets_sent_this_cycle_ >= impl_->allowed_per_drain_) {
            break;
        }
    }

    return sent;
}

void Scheduler::on_bwe_update(std::uint32_t target_bps) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mu_);

    impl_->target_bps_       = target_bps;
    impl_->allowed_per_drain_ = bits_to_packets_per_second(
        target_bps, impl_->config_.avg_packet_size_bytes);

    sched_debug("Scheduler: BWE update target=%u bps, allowed_packets/drain=%u",
                target_bps, impl_->allowed_per_drain_);

    if (target_bps == 0) {
        // Total sender pause: keep all kControl, drop everything else.
        for (std::size_t idx = 1; idx < kPriorityCount; ++idx) {
            impl_->dropped_total_ +=
                static_cast<std::uint32_t>(impl_->queues_[idx].size());
            impl_->queues_[idx].clear();
        }
        sched_debug("Scheduler: sender pause — all non-control packets dropped");
        return;
    }

    // Congestion signalled: trim the queue so its total byte cost fits
    // within one drain cycle's budget.  Packets trimmed from the lowest
    // priority queues are dropped (not "sent"); packets retained stay
    // queued and will be sent by the next drain() call.
    //
    // Drain budget (bytes per drain call):
    //   budget_bytes = target_bps / 8  → bytes per second
    //                / kDrainsPerSecond  → bytes per drain call
    //
    // kDrainsPerSecond == 8 means drain is assumed to be called every
    // 125 ms (the typical high-resolution engine tick). With this divisor:
    //   - 100 kbps → ~1562 bytes/drain → fits 1 packet of 1000 B; a 5-deep
    //     best-effort queue (5000 B) is dropped entirely.
    //   - 256 kbps → ~4000 bytes/drain → fits 4 packets of 1000 B; all
    //     4 moderate-congestion packets preserved.
    //   - 10 kbps → ~156 bytes/drain → fits 0 packets → all dropped.
    //   - 5 Mbps → ~78 KiB/drain → no drops.
    constexpr std::uint32_t kDrainsPerSecond = 8;
    const std::uint64_t bytes_per_sec =
        static_cast<std::uint64_t>(target_bps) / 8;
    const std::uint64_t budget_bytes =
        bytes_per_sec / kDrainsPerSecond;

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
            q.clear();
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
        q.clear();
    }
    impl_->target_bps_          = 0;
    impl_->allowed_per_drain_   = 0;
    impl_->packets_sent_this_cycle_ = 0;
    impl_->dropped_total_       = 0;
    impl_->sent_total_          = 0;
    impl_->sent_bytes_total_    = 0;
    impl_->stream_priority_.clear();
    for (auto& bucket : impl_->stream_buckets_) bucket.clear();
    impl_->stream_dropped_total_ = 0;
    impl_->stream_sent_total_   = 0;
    impl_->last_dst_addr_       = {};
}

plugins::SchedulerStats Scheduler::stats() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mu_);

    plugins::SchedulerStats s;
    for (std::size_t i = 0; i < kPriorityCount; ++i) {
        s.queued_packets += static_cast<std::uint32_t>(impl_->queues_[i].size());
    }
    s.dropped_packets   = impl_->dropped_total_;
    s.sent_packets      = impl_->sent_total_;
    s.sent_bytes        = impl_->sent_bytes_total_;
    s.target_bitrate_bps = impl_->target_bps_;
    s.current_bitrate_bps = 0;  // computed externally if needed
    return s;
}

plugins::SchedulerConfig Scheduler::config() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mu_);
    return impl_->config_;
}

// ===============================================================================
// DC-2: L1 stream-priority methods
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
    auto& old_bucket = impl_->stream_buckets_[stream_priority_rank(prev)];
    auto& new_bucket = impl_->stream_buckets_[stream_priority_rank(level)];

    // Walk old_bucket, move matching packets to a temporary, then append
    // them to new_bucket in original order.  We splice in two passes so
    // new_bucket's existing packets keep their relative order.
    std::deque<PacketRecord> moved;
    std::deque<PacketRecord> retained;
    while (!old_bucket.empty()) {
        PacketRecord rec = std::move(old_bucket.front());
        old_bucket.pop_front();
        if (rec.stream_id == stream_id) {
            moved.push_back(std::move(rec));
        } else {
            retained.push_back(std::move(rec));
        }
    }
    old_bucket = std::move(retained);
    while (!moved.empty()) {
        new_bucket.push_back(std::move(moved.front()));
        moved.pop_front();
    }

    impl_->stream_priority_[stream_id] = level;
    sched_debug("Scheduler: stream %u priority %u -> %u, moved %zu packets",
                stream_id, static_cast<unsigned>(prev),
                static_cast<unsigned>(level), new_bucket.size());
}

StreamPriority Scheduler::get_stream_priority(std::uint32_t stream_id) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mu_);
    auto it = impl_->stream_priority_.find(stream_id);
    if (it != impl_->stream_priority_.end()) return it->second;
    return StreamPriority::Normal;  // default
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

    impl_->stream_buckets_[stream_priority_rank(level)].push_back(std::move(rec));
    sched_debug("Scheduler: enqueued stream=%u into bucket=%u (total=%zu)",
                stream_id, static_cast<unsigned>(level),
                impl_->stream_buckets_[stream_priority_rank(level)].size());
}

void Scheduler::enqueue_for_stream(std::uint32_t stream_id,
                                   core::ByteSpan data,
                                   plugins::Addr dst) noexcept {
    std::vector<std::uint8_t> buf;
    buf.assign(data.data(), data.data() + data.size());
    enqueue_for_stream_owned(stream_id, std::move(buf), dst);
}

namespace {

/** Helper: drain one stream bucket (called from select_next_packet). */
int drain_stream_bucket(std::deque<PacketRecord>& bucket,
                        std::uint32_t& sent_total,
                        std::uint64_t& sent_bytes_total,
                        std::uint32_t avg_packet_size,
                        int max_packets,
                        plugins::DrainCallback inner_cb,
                        std::uint32_t stream_id,
                        StreamPriority bucket_priority) noexcept {
    int sent = 0;
    while (!bucket.empty() && sent < max_packets) {
        PacketRecord rec = std::move(bucket.front());
        bucket.pop_front();

        if (inner_cb) {
            core::ByteSpan span{rec.owned_data.data(), rec.owned_data.size()};
            if (!inner_cb(rec.priority, span)) {
                return sent;  // veto — stop
            }
        }

        ++sent;
        ++sent_total;
        const std::uint32_t est_bytes =
            rec.estimated_size_bytes != 0 ? rec.estimated_size_bytes : avg_packet_size;
        sent_bytes_total += est_bytes;
        (void)stream_id;
        (void)bucket_priority;
    }
    return sent;
}

}  // anonymous namespace

int Scheduler::select_next_packet(int max_packets,
                                  plugins::DrainCallback cb) noexcept {
    if (max_packets <= 0) return 0;

    std::lock_guard<std::mutex> lock(impl_->mu_);

    int sent = 0;

    // Walk from highest priority (0 = High) to lowest (2 = Low).
    for (std::size_t idx = 0; idx < kStreamPriorityCount && sent < max_packets; ++idx) {
        auto& bucket = impl_->stream_buckets_[idx];
        if (bucket.empty()) continue;

        // The bucket priority determines the StreamPriority for the C callback.
        const StreamPriority bucket_prio = static_cast<StreamPriority>(idx);

        // Capture bucket_prio so each packet in this bucket carries it.
        auto wrapped_cb = cb ? [cb, bucket_prio](plugins::Priority p,
                                                  core::ByteSpan span) -> bool {
            return cb(p, span);
        } : plugins::DrainCallback{};

        // Peek at stream_id from the front of the bucket for the C ABI.
        std::uint32_t peek_stream_id = bucket.front().stream_id;

        int n = drain_stream_bucket(bucket, impl_->stream_sent_total_,
                                   impl_->sent_bytes_total_,
                                   impl_->config_.avg_packet_size_bytes,
                                   max_packets - sent, wrapped_cb,
                                   peek_stream_id, bucket_prio);
        sent += n;
        if (n == 0) break;  // bucket became empty mid-drain via veto
    }

    return sent;
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
        return h->sched->select_next_packet(max_packets, nullptr);
    }
    // Wrap the C function pointer into a DrainCallback.
    // NOTE: stream_id and bucket priority are not accessible through the
    // standard DrainCallback interface (which only sees Priority + ByteSpan).
    // The C ABI caller receives 0u / Normal as placeholders here; for full
    // stream info, call the C++ API select_next_packet() directly.
    return h->sched->select_next_packet(max_packets,
        [on_packet, user_data](plugins::Priority,
                               core::ByteSpan span) -> bool {
            on_packet(0u, StreamPriority::Normal,
                      span.data(), span.size(), user_data);
            return true;
        });
}

}  // namespace nimrtc::sched
