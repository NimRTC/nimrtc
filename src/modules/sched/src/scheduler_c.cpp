/**
 * @file src/modules/sched/src/scheduler_c.cpp
 * @brief The scheduler's C ABI — the implementation behind
 *        `<nimrtc/sched/sched_c.h>`.
 *
 * ## What this file is for
 *
 * The architecture remediation plan (Phase 4, item D5) retained the
 * scheduler's "C ABI" entry points on the grounds that the C ABI is the
 * stable way for an embedded host to reach the per-stream priority
 * (S-family) capability — the one public capability that expresses
 * "prioritise this stream over that one".
 *
 * Retaining them is only worthwhile if a C host can actually *call* them.
 * The entry points as they stood were C++ functions in a C++ namespace
 * with C++ signatures and no `extern "C"` linkage, so:
 *
 *   - the linker emitted C++-mangled names
 *     (`?sched_create@sched@nimrtc@@YAPEAUSchedulerHandle@12@...`), and
 *   - the parameter types (`plugins::SchedulerConfig*`,
 *     `plugins::Addr*`, `plugins::StreamPriority`) cannot be named by a C
 *     translation unit at all.
 *
 * A C program could therefore neither include the header nor link against
 * the library.  This file provides the real thing: `extern "C"` functions
 * with plain C types, declared in `sched_c.h`.
 *
 * ## One implementation, two spellings
 *
 * The C++ entry points in `sched.hpp` (`sched_create`, …) are implemented
 * here as thin forwarders to the C ABI below, so there is exactly one
 * implementation of the S-family operations and the two public spellings
 * cannot drift apart.
 *
 * ## A note on `noexcept`
 *
 * The definitions below deliberately do **not** carry `noexcept`, because
 * the declarations in `sched_c.h` cannot: a C header has no `noexcept`.
 * Since C++17 `noexcept` is part of a function's *type*, so a definition
 * with `noexcept` would not match the header's declaration and the
 * compiler would reject the pair.
 *
 * The exception-safety contract is therefore enforced *inside* the
 * functions instead: every entry point that can allocate catches
 * everything, so nothing ever propagates across the C boundary.  A
 * `noexcept` that the declaration cannot express would be a promise the
 * compiler cannot check; a `try`/`catch` that the implementation performs
 * is one it actually keeps.
 */

#include "scheduler_c_impl.hpp"

#include <cstring>
#include <new>
#include <utility>
#include <vector>

namespace nimrtc::sched {

namespace {

/** Validate a C ABI priority level.
 *
 * The C ABI documents HIGH/NORMAL/LOW; anything else is a caller bug.  The
 * scheduler's own bucket count is the authority — a level outside
 * `[0, kStreamPriorityCount)` would index past the bucket array, so it is
 * rejected rather than clamped.
 */
[[nodiscard]] inline bool is_valid_c_level(int32_t level) noexcept {
    return level >= NIMRTC_SCHED_PRIORITY_HIGH &&
           level < static_cast<int32_t>(kStreamPriorityCount);
}

} // namespace

namespace detail {

int select_next_packet_impl(SchedulerHandle*              h,
                            int                           max_packets,
                            plugins::StreamDrainCallback cb) noexcept {
    if (!h) return 0;
    if (max_packets < 0) return 0;
    if (!cb) {
        // Drain without a callback.
        return h->sched->select_next_packet(max_packets, nullptr);
    }
    return h->sched->select_next_packet(max_packets, std::move(cb));
}

} // namespace detail
} // namespace nimrtc::sched

// =============================================================================
// The C ABI
// =============================================================================

extern "C" {

NimrtcSchedHandle* nimrtc_sched_create(const NimrtcSchedConfig* config) {
    try {
        // A NULL config means "the library defaults" — which is *not* the
        // same as a zeroed struct, because `protect_keyframes` defaults to
        // true and `enabled` to false.  The C header documents this.
        nimrtc::plugins::SchedulerConfig cfg =
            config ? nimrtc::sched::from_c_config(*config)
                   : nimrtc::plugins::SchedulerConfig{};
        return new NimrtcSchedHandle(std::move(cfg));
    } catch (...) {
        return nullptr;
    }
}

void nimrtc_sched_destroy(NimrtcSchedHandle* handle) {
    // `delete` on a null pointer is a no-op, and the destructor cannot
    // throw (it releases a unique_ptr).
    delete handle;
}

void nimrtc_sched_set_stream_priority(NimrtcSchedHandle* handle,
                                       uint32_t            stream_id,
                                       int32_t             level) {
    if (!handle) return;
    if (!nimrtc::sched::is_valid_c_level(level)) return;
    handle->sched->set_stream_priority(stream_id,
                                        nimrtc::sched::from_c_level(level));
}

int32_t nimrtc_sched_get_stream_priority(const NimrtcSchedHandle* handle,
                                          uint32_t                stream_id) {
    if (!handle) return NIMRTC_SCHED_PRIORITY_NORMAL;
    return nimrtc::sched::to_c_level(
        handle->sched->get_stream_priority(stream_id));
}

void nimrtc_sched_enqueue_stream_owned(NimrtcSchedHandle*      handle,
                                        uint32_t               stream_id,
                                        const uint8_t*         data,
                                        size_t                 len,
                                        const NimrtcSchedAddr* dst) {
    if (!handle) return;

    try {
        // The C ABI copies the payload: a C host cannot hand over ownership
        // of a heap block in a way the C++ scheduler can adopt (that needs
        // `std::vector<uint8_t>&&`), and copying keeps the C contract
        // simple and safe — the caller may free `data` the moment this
        // returns.
        std::vector<std::uint8_t> buf;
        if (data && len > 0) {
            buf.assign(data, data + len);
        }

        nimrtc::plugins::Addr addr;
        if (dst) addr = nimrtc::sched::from_c_addr(*dst);

        handle->sched->enqueue_for_stream_owned(stream_id, std::move(buf), addr);
    } catch (...) {
        // A failed allocation must not cross the C boundary.  The packet is
        // dropped, exactly as the C++ API's own allocation failure path
        // drops it.
    }
}

int32_t nimrtc_sched_select_next_packet(
    NimrtcSchedHandle* handle,
    int32_t             max_packets,
    void (*on_packet)(uint32_t       stream_id,
                      int32_t        level,
                      const uint8_t* data,
                      size_t         len,
                      void*          user_data),
    void* user_data) {
    if (!handle) return 0;
    if (max_packets < 0) return 0;

    if (!on_packet) {
        // Drain without a callback.
        return nimrtc::sched::detail::select_next_packet_impl(
            handle, static_cast<int>(max_packets), nullptr);
    }

    try {
        // The C function pointer is wrapped into the stream-aware C++
        // callback, so the C ABI caller receives the real stream id and
        // the real bucket priority — not placeholders.
        return nimrtc::sched::detail::select_next_packet_impl(
            handle, static_cast<int>(max_packets),
            [on_packet, user_data](std::uint32_t stream_id,
                                   nimrtc::plugins::StreamPriority level,
                                   nimrtc::core::ByteSpan span) -> bool {
                on_packet(stream_id, nimrtc::sched::to_c_level(level),
                          span.data(), span.size(), user_data);
                return true;
            });
    } catch (...) {
        return 0;
    }
}

} // extern "C"

// =============================================================================
// The C++ spellings — thin forwarders to the C ABI above
// =============================================================================
//
// These keep the pre-existing public C++ API (`sched_create`, …) working
// exactly as before, while making the C ABI the single implementation.

namespace nimrtc::sched {

SchedulerHandle* sched_create(const plugins::SchedulerConfig* config) noexcept {
    if (!config) {
        return nimrtc_sched_create(nullptr);
    }
    const NimrtcSchedConfig c = to_c_config(*config);
    return nimrtc_sched_create(&c);
}

void sched_destroy(SchedulerHandle* h) noexcept {
    nimrtc_sched_destroy(h);
}

void sched_set_stream_priority(SchedulerHandle* h,
                                std::uint32_t    stream_id,
                                StreamPriority   level) noexcept {
    nimrtc_sched_set_stream_priority(h, stream_id, to_c_level(level));
}

StreamPriority sched_get_stream_priority(const SchedulerHandle* h,
                                          std::uint32_t          stream_id) noexcept {
    return from_c_level(nimrtc_sched_get_stream_priority(h, stream_id));
}

void sched_enqueue_stream_owned(SchedulerHandle*    h,
                                std::uint32_t       stream_id,
                                const std::uint8_t* data,
                                std::size_t          len,
                                const plugins::Addr* dst) noexcept {
    NimrtcSchedAddr c_addr;
    if (dst) c_addr = to_c_addr(*dst);
    nimrtc_sched_enqueue_stream_owned(h, stream_id, data, len,
                                      dst ? &c_addr : nullptr);
}

int sched_select_next_packet(SchedulerHandle* h,
                             int              max_packets,
                             void (*on_packet)(std::uint32_t       stream_id,
                                               StreamPriority     level,
                                               const std::uint8_t* data,
                                               std::size_t         len,
                                               void*              user_data),
                             void* user_data) noexcept {
    if (!on_packet) {
        return nimrtc::sched::detail::select_next_packet_impl(
            h, max_packets, nullptr);
    }
    // The two callbacks differ only in how they spell the priority (a
    // scoped C++ enum vs. the C ABI's int32_t), so the C++ pointer is
    // adapted through a lambda rather than reinterpret_cast-ed: a
    // reinterpret_cast between two *different* function-pointer types is
    // undefined behaviour, and the lambda makes the conversion explicit
    // and total.
    return nimrtc::sched::detail::select_next_packet_impl(
        h, max_packets,
        [on_packet, user_data](std::uint32_t stream_id, StreamPriority level,
                               nimrtc::core::ByteSpan span) -> bool {
            on_packet(stream_id, level, span.data(), span.size(), user_data);
            return true;
        });
}

} // namespace nimrtc::sched
