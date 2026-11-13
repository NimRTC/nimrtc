/**
 * @file nimrtc/sched/sched_c.h
 * @brief The scheduler's **C** ABI — the S-family (per-stream priority)
 *        entry points for C hosts and FFI consumers.
 *
 * ## Why this file exists
 *
 * `nimrtc/sched/sched.hpp` declares a set of `sched_*` functions that were
 * introduced as "the C ABI for embedded hosts".  They were **not** actually
 * a C ABI:
 *
 *   - they had no `extern "C"` linkage, so the linker emitted *C++-mangled*
 *     symbols (`?sched_create@sched@nimrtc@@YAPEAUSchedulerHandle@12@...`);
 *   - their signatures used C++ types (`plugins::SchedulerConfig*`,
 *     `plugins::Addr*`, `plugins::StreamPriority`), none of which a C
 *     translation unit can even name;
 *   - the header itself was C++-only (`<optional>`, `namespace`, …), so a C
 *     host could not `#include` it.
 *
 * The result was a set of functions that no C program could call and no
 * C program could link against — a "C ABI" in name only.  The architecture
 * remediation plan (Phase 4, item D5) retained these entry points on the
 * grounds that the C ABI is the *stable* way for an embedded host to reach
 * the per-stream priority family; retaining them is only worthwhile if they
 * are genuinely callable from C.
 *
 * This header is the real thing: pure C11, `extern "C"` linkage, plain C
 * types, and no dependency beyond `<stdint.h>` / `<stddef.h>`.
 *
 * ## Relationship to the C++ API
 *
 * The C++ declarations in `sched.hpp` remain, and are now implemented as
 * **thin forwarders** to the functions declared here.  There is exactly one
 * implementation; the C++ spelling and the C spelling cannot drift.
 *
 * ## Stability
 *
 * These entry points are `NIMRTC_API_STABLE` in the ADR-015 sense: their
 * names, their numeric constants, and the layout of the two structs below
 * are part of the public ABI and will not change within v1.x.
 *
 * @see docs/adr/ADR-015-api-stability-policy.md
 */

#ifndef NIMRTC_SCHED_SCHED_C_H
#define NIMRTC_SCHED_SCHED_C_H

#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Linkage
 *
 * The C ABI is `extern "C"` so the symbols are unmangled and a C host's
 * linker can find them by name.
 * ------------------------------------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Export marker
 *
 * The module builds with hidden default visibility
 * (`C_VISIBILITY_PRESET hidden` in cmake/NimRTCOptions.cmake), so the C
 * entry points need an explicit default-visibility attribute on ELF
 * platforms or they would not be exported from a shared build.
 *
 * On Windows the static archives are linked directly, so the attribute
 * expands to nothing.
 * ------------------------------------------------------------------------- */
#if defined(_WIN32) || defined(__CYGWIN__)
    #define NIMRTC_SCHED_C_API
#elif defined(__GNUC__) || defined(__clang__)
    #define NIMRTC_SCHED_C_API __attribute__((visibility("default")))
#else
    #define NIMRTC_SCHED_C_API
#endif

/* -------------------------------------------------------------------------
 * Version marker
 *
 * Lets a C host assert that the library it links against is new enough to
 * carry the C ABI at all.  Bump the minor number when adding an entry
 * point; the major number changes only on an incompatible change.
 * ------------------------------------------------------------------------- */
#define NIMRTC_SCHED_C_ABI_VERSION_MAJOR 1
#define NIMRTC_SCHED_C_ABI_VERSION_MINOR 0

/* -------------------------------------------------------------------------
 * Opaque handle
 *
 * A `NimrtcSchedHandle` is created by nimrtc_sched_create() and destroyed
 * by nimrtc_sched_destroy().  The layout is deliberately hidden: it holds a
 * C++ `Scheduler` instance, and a C host must not depend on its size.
 * ------------------------------------------------------------------------- */
typedef struct NimrtcSchedHandle NimrtcSchedHandle;

/* -------------------------------------------------------------------------
 * Per-stream priority — the S-family buckets
 *
 * These are the C spelling of `plugins::StreamPriority`.  The numeric
 * values match the C++ enum exactly (High = 0, Normal = 1, Low = 2) and
 * lower ordinal means *higher* priority.
 * ------------------------------------------------------------------------- */
#define NIMRTC_SCHED_PRIORITY_HIGH   0
#define NIMRTC_SCHED_PRIORITY_NORMAL 1
#define NIMRTC_SCHED_PRIORITY_LOW    2

/* -------------------------------------------------------------------------
 * Configuration
 *
 * The C mirror of `plugins::SchedulerConfig`.  Every field has the same
 * meaning and the same default as its C++ counterpart; a zero-initialised
 * `NimrtcSchedConfig` is therefore *not* the same as the C++ default
 * (where `protect_keyframes` and `enabled` default to true/false
 * respectively), so a C host that wants the library defaults must pass
 * NULL to nimrtc_sched_create() instead of a zeroed struct.
 *
 * `protect_keyframes` and `enabled` are `int` rather than `bool` so that
 * the struct has the same layout under every C compiler and ABI (C's
 * `_Bool` is not guaranteed to be a byte).
 * ------------------------------------------------------------------------- */
typedef struct NimrtcSchedConfig {
    uint32_t avg_packet_size_bytes; /**< Estimated average outbound packet size (bytes). */
    uint32_t max_queue_depth;       /**< Hard cap on the total number of queued packets. */
    int32_t  protect_keyframes;     /**< Non-zero: never demote a keyframe below audio. */
    int32_t  enabled;               /**< Non-zero: the host intends to use the scheduler. */
    uint32_t drain_interval_ms;     /**< Congestion-window horizon, in milliseconds. */
} NimrtcSchedConfig;

/* -------------------------------------------------------------------------
 * Destination address
 *
 * The C mirror of `plugins::Addr`: an opaque, protocol-agnostic address
 * whose *format* is owned by the transport implementation.  `data` carries
 * the encoded address; `len` says how many of those bytes are meaningful.
 *
 * `NIMRTC_SCHED_MAX_ADDR_LEN` matches `plugins::kMaxAddrLen`.
 * ------------------------------------------------------------------------- */
#define NIMRTC_SCHED_MAX_ADDR_LEN 64

typedef struct NimrtcSchedAddr {
    uint8_t  data[NIMRTC_SCHED_MAX_ADDR_LEN];
    uint32_t len;
} NimrtcSchedAddr;

/* -------------------------------------------------------------------------
 * The C ABI
 * ------------------------------------------------------------------------- */

/**
 * Allocate a new Scheduler and return an opaque handle.
 *
 * @param config  NULL for the library defaults; otherwise a pointer to the
 *                caller's configuration, which is copied.
 * @return A handle, or NULL if the allocation failed.
 */
NIMRTC_SCHED_C_API NimrtcSchedHandle*
nimrtc_sched_create(const NimrtcSchedConfig* config);

/**
 * Destroy a handle returned by nimrtc_sched_create().
 *
 * Passing NULL is a no-op.
 */
NIMRTC_SCHED_C_API void
nimrtc_sched_destroy(NimrtcSchedHandle* handle);

/**
 * Assign a priority bucket to a stream.
 *
 * @param level  One of NIMRTC_SCHED_PRIORITY_{HIGH,NORMAL,LOW}.  A value
 *               outside that range is ignored (the stream keeps its
 *               current level).
 */
NIMRTC_SCHED_C_API void
nimrtc_sched_set_stream_priority(NimrtcSchedHandle* handle,
                                  uint32_t            stream_id,
                                  int32_t             level);

/**
 * Return the priority bucket of a stream.
 *
 * @return One of NIMRTC_SCHED_PRIORITY_*.  A stream that was never
 *         assigned a priority reports NIMRTC_SCHED_PRIORITY_NORMAL.
 */
NIMRTC_SCHED_C_API int32_t
nimrtc_sched_get_stream_priority(const NimrtcSchedHandle* handle,
                                  uint32_t                stream_id);

/**
 * Enqueue one packet for a stream.
 *
 * The scheduler copies the payload, so the caller keeps ownership of
 * `data` and may free or reuse it as soon as this call returns.
 *
 * @param data  The payload; may be NULL only when `len` is 0.
 * @param len   The payload length in bytes.
 * @param dst   The destination address; NULL means "reuse the most recent
 *              non-empty destination", which is the behaviour a host
 *              sending to a single peer expects.
 */
NIMRTC_SCHED_C_API void
nimrtc_sched_enqueue_stream_owned(NimrtcSchedHandle*       handle,
                                   uint32_t                stream_id,
                                   const uint8_t*          data,
                                   size_t                  len,
                                   const NimrtcSchedAddr*  dst);

/**
 * Drain up to `max_packets` packets from the highest non-empty stream
 * bucket.
 *
 * The buckets are walked High → Normal → Low and packets are emitted from
 * the first non-empty one, so RTX / NACK / RTCP feedback (High) always
 * pre-empts video and padding (Normal / Low).
 *
 * @param max_packets  The maximum number of packets to drain.
 * @param on_packet    Called once per drained packet with the *real* stream
 *                     id and the *real* bucket priority.  May be NULL, in
 *                     which case the packets are drained and discarded
 *                     (they are still accounted as sent).
 * @param user_data    Passed through to `on_packet` unchanged.
 * @return The number of packets drained.
 */
NIMRTC_SCHED_C_API int32_t
nimrtc_sched_select_next_packet(NimrtcSchedHandle* handle,
                                 int32_t           max_packets,
                                 void (*on_packet)(uint32_t            stream_id,
                                                   int32_t             level,
                                                   const uint8_t*      data,
                                                   size_t              len,
                                                   void*               user_data),
                                 void*              user_data);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NIMRTC_SCHED_SCHED_C_H */
