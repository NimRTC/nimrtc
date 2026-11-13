/**
 * @file src/modules/sched/src/scheduler_c_impl.hpp
 * @brief Private glue between the scheduler's public C++ API and its public
 *        C ABI.
 *
 * ## Why this file exists
 *
 * There are two public spellings of the same operations:
 *
 *   - the C++ API in `<nimrtc/sched/sched.hpp>` (`sched_create`,
 *     `sched_select_next_packet`, …), which uses C++ types and lives in
 *     `namespace nimrtc::sched`; and
 *   - the C ABI in `<nimrtc/sched/sched_c.h>` (`nimrtc_sched_create`,
 *     `nimrtc_sched_select_next_packet`, …), which is `extern "C"`, uses
 *     plain C types, and is callable from a C translation unit.
 *
 * Both must exist: the C++ spelling is what the existing (stable) public
 * API promises, and the C spelling is what an embedded / FFI host can
 * actually call.  But there must be exactly **one** implementation, or the
 * two can drift — the classic "two hand-written copies of the same table"
 * defect the architecture remediation plan (D2) calls out for the SDP
 * format parameters.
 *
 * So: the C ABI is the implementation, and the C++ entry points are thin
 * forwarders to it.  This header holds the one piece of state both need —
 * the definition of the opaque handle — plus the small struct conversions.
 *
 * This header is **private to the module**: it is not installed and is not
 * reachable from outside `src/modules/sched/`.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#include <nimrtc/sched/sched.hpp>
#include <nimrtc/sched/sched_c.h>

/**
 * The opaque handle shared by the C++ and the C spellings.
 *
 * `sched_c.h` forward-declares `NimrtcSchedHandle` at global scope so the
 * C header stays free of namespaces; here — outside any namespace — the
 * definition lives, so the definition matches the C header's typedef
 * exactly.  `sched.hpp` re-exports it as `nimrtc::sched::SchedulerHandle`,
 * so the C++ API keeps its old spelling and its old type identity.
 *
 * The layout is deliberately hidden from both public headers: it holds a
 * `std::unique_ptr<Scheduler>`, and a consumer (C or C++) must not depend
 * on the size or the layout of the handle.
 */
struct NimrtcSchedHandle {
    std::unique_ptr<nimrtc::sched::Scheduler> sched;

    explicit NimrtcSchedHandle(nimrtc::plugins::SchedulerConfig cfg)
        : sched(std::make_unique<nimrtc::sched::Scheduler>(std::move(cfg))) {}
};

namespace nimrtc::sched {

/** Translate the C++ priority enum into the C ABI's integer level.
 *
 * The two encodings are numerically identical (High = 0, Normal = 1,
 * Low = 2) and the C ABI documents that contract, so this is a checked
 * cast rather than a reinterpretation: an out-of-range value would be a
 * bug in the caller, and silently mapping it would corrupt the bucket
 * assignment.
 */
[[nodiscard]] inline int32_t to_c_level(StreamPriority p) noexcept {
    switch (p) {
        case StreamPriority::High:   return NIMRTC_SCHED_PRIORITY_HIGH;
        case StreamPriority::Normal: return NIMRTC_SCHED_PRIORITY_NORMAL;
        case StreamPriority::Low:    return NIMRTC_SCHED_PRIORITY_LOW;
    }
    return NIMRTC_SCHED_PRIORITY_NORMAL;
}

/** Translate the C ABI's integer level back into the C++ priority enum.
 *
 * An unknown level degrades to `Normal`, matching the scheduler's own
 * behaviour for a stream that was never assigned a priority.
 */
[[nodiscard]] inline StreamPriority from_c_level(int32_t level) noexcept {
    switch (level) {
        case NIMRTC_SCHED_PRIORITY_HIGH:   return StreamPriority::High;
        case NIMRTC_SCHED_PRIORITY_NORMAL: return StreamPriority::Normal;
        case NIMRTC_SCHED_PRIORITY_LOW:    return StreamPriority::Low;
        default:                           return StreamPriority::Normal;
    }
}

/** Copy the C++ configuration into the C ABI's flat struct. */
[[nodiscard]] inline NimrtcSchedConfig to_c_config(
    const plugins::SchedulerConfig& in) noexcept {
    NimrtcSchedConfig out{};
    out.avg_packet_size_bytes = in.avg_packet_size_bytes;
    out.max_queue_depth       = in.max_queue_depth;
    out.protect_keyframes     = in.protect_keyframes ? 1 : 0;
    out.enabled               = in.enabled ? 1 : 0;
    out.drain_interval_ms     = in.drain_interval_ms;
    return out;
}

/** Copy the C ABI's flat struct into the C++ configuration. */
[[nodiscard]] inline plugins::SchedulerConfig from_c_config(
    const NimrtcSchedConfig& in) noexcept {
    plugins::SchedulerConfig out;
    out.avg_packet_size_bytes = in.avg_packet_size_bytes;
    out.max_queue_depth       = in.max_queue_depth;
    out.protect_keyframes     = in.protect_keyframes != 0;
    out.enabled               = in.enabled != 0;
    out.drain_interval_ms     = in.drain_interval_ms;
    return out;
}

/** Copy the C++ address into the C ABI's flat struct. */
[[nodiscard]] inline NimrtcSchedAddr to_c_addr(const plugins::Addr& in) noexcept {
    NimrtcSchedAddr out{};
    // Both buffers are `kMaxAddrLen` bytes wide; copy only the bytes the
    // source says are meaningful, but zero the rest so a C host never sees
    // uninitialised padding.
    const std::uint32_t n =
        in.len > plugins::kMaxAddrLen ? plugins::kMaxAddrLen : in.len;
    for (std::uint32_t i = 0; i < n; ++i) out.data[i] = in.data[i];
    out.len = n;
    return out;
}

/** Copy the C ABI's flat struct into the C++ address. */
[[nodiscard]] inline plugins::Addr from_c_addr(const NimrtcSchedAddr& in) noexcept {
    plugins::Addr out{};
    const std::uint32_t n =
        in.len > plugins::kMaxAddrLen ? plugins::kMaxAddrLen : in.len;
    for (std::uint32_t i = 0; i < n; ++i) out.data[i] = in.data[i];
    out.len = n;
    return out;
}

namespace detail {

/** The one implementation of the S-family drain.
 *
 * Both the C ABI entry point and the C++ forwarder funnel through here, so
 * the two public spellings cannot disagree about what "drain the highest
 * non-empty bucket" means.
 *
 * @param cb  The stream-aware drain callback; an empty `std::function`
 *            means "drain without a callback" (the packets are drained
 *            and discarded).
 */
[[nodiscard]] int select_next_packet_impl(
    SchedulerHandle*              h,
    int                           max_packets,
    plugins::StreamDrainCallback cb) noexcept;

} // namespace detail

} // namespace nimrtc::sched
