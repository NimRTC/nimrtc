/**
 * @file src/modules/video_stats/tests/test_video_stats_alloc.cpp
 * @brief The video_stats module's hot-path allocation budget.
 *
 * The engine drives this module's sink from the **send** path
 * (`NimRTCEngine::send_video()`), and the remediation plan's §3.5 budget
 * says that path must perform *zero* heap allocations per call.  That
 * budget is asserted end-to-end by the `hotpath_zero_allocs` ctest case
 * (`tests/bench_engine_hotpath.cpp`), but that case measures the whole
 * engine: when it fails it says nothing about *which* module put the
 * allocation on the hot path.
 *
 * This file is the module-level guard.  It lives in its own binary
 * because the allocation probe replaces the global `operator new` /
 * `operator delete`, and doing so inside a binary that also carries
 * the general-purpose module tests would make every unrelated
 * allocation in those tests show up in the measurement.
 *
 * ## What is measured
 *
 * The number of heap allocations performed by `IVideoStatsSink`'s
 * event-push methods over a long run.  The counter is *not* the total
 * allocation count of the run — it is the count *after* the warm-up, so
 * that the one-off growth of a container is not counted, but a
 * per-frame allocation (the bug this file guards against) is.
 *
 * ## The bug this guards against
 *
 * `video_stats` used to keep its bitrate samples in a `std::deque`.
 * MSVC's deque grows one element per block, so every `push_back` on the
 * send path allocated — a 300-frame run allocated 306 times, which is
 * exactly one allocation per frame and blew the §3.5 budget.  The
 * samples now live in a fixed-capacity ring buffer, and this test fails
 * if anything reintroduces a growing container on the hot path.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>

#include <nimrtc/video_stats/video_stats.hpp>

// ---------------------------------------------------------------------------
// The allocation probe.
//
// The probe is **not** defined in a sanitizer build: a sanitizer runtime
// ships its own strong definitions of `operator new`, and a definition
// here would collide with it at link time.
// ---------------------------------------------------------------------------
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  define NIMRTC_VIDEO_STATS_ALLOC_PROBE 0
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
      __has_feature(memory_sanitizer)
#    define NIMRTC_VIDEO_STATS_ALLOC_PROBE 0
#  else
#    define NIMRTC_VIDEO_STATS_ALLOC_PROBE 1
#  endif
#else
#  define NIMRTC_VIDEO_STATS_ALLOC_PROBE 1
#endif

namespace alloc_probe {

std::atomic<bool>          g_armed{false};
std::atomic<std::uint64_t> g_alloc_count{0};

inline void note(std::size_t) noexcept {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
}

inline std::uint64_t count() noexcept {
    return g_alloc_count.load(std::memory_order_relaxed);
}

struct Scope {
    Scope() noexcept {
        g_alloc_count.store(0, std::memory_order_relaxed);
        g_armed.store(true, std::memory_order_relaxed);
    }
    ~Scope() { g_armed.store(false, std::memory_order_relaxed); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

}  // namespace alloc_probe

// The global replacements live at *global* scope on purpose: a
// non-member `operator new` / `operator delete` may not be declared
// inside a namespace (C++ [expr.new] / [expr.delete]).
#if NIMRTC_VIDEO_STATS_ALLOC_PROBE

void* operator new(std::size_t n) {
    alloc_probe::note(n);
    if (n == 0) n = 1;
    void* p = std::malloc(n);
    if (!p) throw std::bad_alloc();
    return p;
}

void* operator new[](std::size_t n) { return ::operator new(n); }

void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    alloc_probe::note(n);
    if (n == 0) n = 1;
    return std::malloc(n);
}

void* operator new[](std::size_t n, const std::nothrow_t& nt) noexcept {
    return ::operator new(n, nt);
}

#if defined(__cpp_aligned_new)
void* operator new(std::size_t n, std::align_val_t a) {
    alloc_probe::note(n);
    if (n == 0) n = 1;
    const std::size_t align = static_cast<std::size_t>(a);
    void* p = nullptr;
#if defined(_MSC_VER)
    p = _aligned_malloc(n, align);
#else
    if (posix_memalign(&p, align, n) != 0) p = nullptr;
#endif
    if (!p) throw std::bad_alloc();
    return p;
}

void* operator new[](std::size_t n, std::align_val_t a) {
    return ::operator new(n, a);
}

void operator delete(void* p, std::align_val_t a) noexcept {
    (void)a;
    if (!p) return;
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

void operator delete[](void* p, std::align_val_t a) noexcept {
    ::operator delete(p, a);
}
#endif

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

#endif  // NIMRTC_VIDEO_STATS_ALLOC_PROBE

namespace {

namespace vs = nimrtc::video_stats;

#if NIMRTC_VIDEO_STATS_ALLOC_PROBE

// The number of frames the measured run pushes through the sink.  Large
// enough that a per-frame allocation is unmistakable, small enough
// that the test stays fast.
constexpr int kMeasuredFrames = 5000;

TEST(VideoStatsAlloc, TheEventPushPathAllocatesNothingPerFrame) {
    auto sink = vs::create_stats_sink({});
    ASSERT_NE(sink, nullptr);

    // Warm-up: the sink's own containers reach their steady-state size
    // here.  A growing container would keep allocating during the
    // measured region; a fixed one will not.
    for (int i = 0; i < 100; ++i) {
        sink->on_frame_received(static_cast<std::int64_t>(i) * 33'000,
                                 4096, /*is_keyframe=*/false, /*qp=*/-1);
    }

    std::uint64_t allocations = 0;
    {
        alloc_probe::Scope scope;   // armed for exactly the measured region
        for (int i = 0; i < kMeasuredFrames; ++i) {
            sink->on_frame_received(static_cast<std::int64_t>(i) * 33'000,
                                     4096, /*is_keyframe=*/false, /*qp=*/-1);
            sink->on_frame_decoded(static_cast<std::int64_t>(i) * 33'000);
            sink->on_frame_rendered(static_cast<std::int64_t>(i) * 33'000);
            sink->on_qp_sample(30);
        }
        allocations = alloc_probe::count();
    }

    EXPECT_EQ(allocations, 0u)
        << "the stats sink allocated " << allocations << " times over "
        << kMeasuredFrames
        << " frames — the engine's send path requires zero allocations per "
           "frame, and the bitrate window must stay a fixed-capacity ring";
}

// The same criterion in its aggregate form: the allocation count must not
// grow with the number of frames.  This is the form that survives a
// legitimate one-off allocation on the first measured frame (e.g. a
// snapshot's own scratch buffer), while still catching a container that
// grows once per frame.
TEST(VideoStatsAlloc, TheAllocationCountDoesNotGrowWithTheFrameCount) {
    auto sink = vs::create_stats_sink({});

    for (int i = 0; i < 100; ++i) {
        sink->on_frame_received(static_cast<std::int64_t>(i) * 33'000, 4096,
                                 false, -1);
    }

    auto measure = [&](int frames) -> std::uint64_t {
        alloc_probe::Scope scope;
        for (int i = 0; i < frames; ++i) {
            sink->on_frame_received(static_cast<std::int64_t>(i) * 33'000, 4096,
                                     false, -1);
        }
        return alloc_probe::count();
    };

    const std::uint64_t short_run = measure(200);
    const std::uint64_t long_run  = measure(2000);

    EXPECT_EQ(long_run - short_run, 0u)
        << "the allocation count grew with the frame count: 200 frames cost "
        << short_run << " allocations, 2000 frames cost " << long_run
        << " — the per-frame cost must be zero";
}

#endif  // NIMRTC_VIDEO_STATS_ALLOC_PROBE

}  // namespace
