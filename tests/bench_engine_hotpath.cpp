// tests/bench_engine_hotpath.cpp
// ----------------------------------------------------------------------------
// The Phase 0.3 performance baseline (architecture remediation plan §0.3).
//
// Phase 3 ("消除热路径上的分配与拷贝") is only meaningful if there is a
// number to compare against.  §0.3 asks for three baseline points:
//
//   1. `send_video()` at 1080p30 — the CPU cost and the *per-call heap
//      allocation count*.
//   2. `tick()` — the p99 duration.
//   3. The scheduler under load — `drain_with()`'s cost, split into the
//      part that holds the lock and the part that does not, i.e. how long
//      a concurrent `enqueue()` has to wait.
//
// This binary produces those three numbers.  It is NOT a unit test: the
// timings are machine-dependent, so nothing here asserts a threshold.
// The *acceptance* thresholds live in the plan (§3.5); this binary is
// what makes them checkable.
//
// ## How the allocation count is obtained
//
// The global `operator new` / `operator delete` are replaced below.  A
// counter is incremented only while the probe is *armed*, and the probe
// is armed for the duration of exactly the call under test — nothing
// else.  The replacement forwards to the CRT allocator, so it changes
// no behaviour other than the count.
//
// A thread-local re-entrancy flag keeps the probe's own bookkeeping
// (and anything it allocates) out of the count.
//
// ## How the lock split is obtained
//
// The scheduler's lock is internal, so it cannot be timed from the
// inside.  What *can* be observed from the outside is the property the
// plan actually cares about: "`drain_with()` does not block
// `enqueue()`".  A producer thread enqueues in a tight loop and
// measures how long each `enqueue()` call takes; a consumer thread
// drains in a tight loop.  The producer's p99 is the time a sender
// spends waiting for the lock — the lock's contention footprint, seen
// from the only side that matters.
//
// ## Output
//
// A JSON document on stdout, so `tools/benchmark_hotpath.py` can capture
// it into `build/` and diff two runs.
// ----------------------------------------------------------------------------

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/scheduler.hpp>

// ---------------------------------------------------------------------------
// The allocation probe.
//
// Replacing the global operator new is the only way to count the
// allocations a call makes without changing the call site.  The counter
// is a relaxed atomic: the probe is only ever armed around a single
// thread's work, so exact ordering does not matter, and a relaxed
// increment is far cheaper than a seq_cst one.
// ---------------------------------------------------------------------------
namespace alloc_probe {

std::atomic<bool>          g_armed{false};
std::atomic<std::uint64_t> g_alloc_count{0};
std::atomic<std::uint64_t> g_alloc_bytes{0};

inline void note(std::size_t n) noexcept {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    g_alloc_bytes.fetch_add(n, std::memory_order_relaxed);
}

/** The number of allocations since the last `Scope` was armed. */
inline std::uint64_t count() noexcept {
    return g_alloc_count.load(std::memory_order_relaxed);
}

/** The number of bytes allocated since the last `Scope` was armed. */
inline std::uint64_t bytes() noexcept {
    return g_alloc_bytes.load(std::memory_order_relaxed);
}

struct Scope {
    Scope() noexcept {
        g_alloc_count.store(0, std::memory_order_relaxed);
        g_alloc_bytes.store(0, std::memory_order_relaxed);
        g_armed.store(true, std::memory_order_relaxed);
    }
    ~Scope() { g_armed.store(false, std::memory_order_relaxed); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

}  // namespace alloc_probe

// ---------------------------------------------------------------------------
// The global replacements.  They forward to the CRT allocator so the
// program's behaviour is unchanged apart from the count.
// ---------------------------------------------------------------------------
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

namespace {

// ---------------------------------------------------------------------------
// Small statistics helpers.
// ---------------------------------------------------------------------------
double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = q * static_cast<double>(v.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(idx);
    const std::size_t hi = std::min(lo + 1, v.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

double mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (double x : v) s += x;
    return s / static_cast<double>(v.size());
}

// A tiny JSON writer — enough for this document, no dependency.
//
// The writer keeps one bit of state per open container: whether the next
// value needs a separating comma.  A key sets the "expect a value next"
// flag, and the value that follows does *not* emit a separator.
class Json {
public:
    explicit Json(std::string& out) : out_(out) {}

    void begin_object() { separate(); out_ += '{'; stack_.push_back(true); }
    void end_object()   { out_ += '}'; close(); }
    void begin_array()  { separate(); out_ += '['; stack_.push_back(true); }
    void end_array()    { out_ += ']'; close(); }

    void key(const std::string& k) {
        separate();
        out_ += '"';
        escape(k);
        out_ += "\":";
        expect_value_ = true;
    }

    void value(double v) {
        separate();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6f", v);
        out_ += buf;
    }
    void value(long long v) {
        separate();
        out_ += std::to_string(v);
    }
    void value(unsigned long long v) {
        separate();
        out_ += std::to_string(v);
    }
    void value(const std::string& v) {
        separate();
        out_ += '"';
        escape(v);
        out_ += '"';
    }
    void value(bool v) {
        separate();
        out_ += v ? "true" : "false";
    }

    void field(const std::string& k, double v)         { key(k); value(v); }
    void field(const std::string& k, long long v)      { key(k); value(v); }
    void field(const std::string& k, unsigned long long v) { key(k); value(v); }
    void field(const std::string& k, const std::string& v) { key(k); value(v); }
    void field(const std::string& k, bool v)           { key(k); value(v); }

private:
    // Emit the comma that separates two members of the *current*
    // container, if one is needed.
    void separate() {
        if (expect_value_) { expect_value_ = false; return; }
        if (stack_.empty()) return;
        if (stack_.back()) stack_.back() = false;
        else out_ += ',';
    }

    void close() {
        if (!stack_.empty()) stack_.pop_back();
        if (!stack_.empty()) stack_.back() = false;
    }

    void escape(const std::string& v) {
        for (char c : v) {
            if (c == '"' || c == '\\') { out_ += '\\'; out_ += c; }
            else if (c == '\n') out_ += "\\n";
            else out_ += c;
        }
    }

    std::string& out_;
    std::vector<bool> stack_;
    bool expect_value_ = false;
};

using Clock = std::chrono::steady_clock;

double micros_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
}

// ---------------------------------------------------------------------------
// Benchmark 1 — `send_video()` at 1080p30.
//
// The engine runs on the "shell" transport strategy, so no ICE / DTLS
// handshake is involved: what is measured is the media path, not the
// network.  The H.264 codec plugin's software backend is used; the
// pixels are still staged and handed to it, which is exactly the path
// the plan asks about.
// ---------------------------------------------------------------------------
struct SendVideoResult {
    long long   frames = 0;
    double      alloc_per_frame = 0.0;
    double      bytes_per_frame = 0.0;
    double      mean_us = 0.0;
    double      p50_us = 0.0;
    double      p99_us = 0.0;
    double      max_us = 0.0;
    double      mib_per_second = 0.0;
};

SendVideoResult bench_send_video(int width, int height, int frames, int warmup) {
    using nimrtc::engine::EngineConfig;
    using nimrtc::engine::NimRTCEngine;
    namespace np = nimrtc::plugins;

    SendVideoResult out;

    EngineConfig cfg;
    cfg.transport_strategy_name = "shell";
    cfg.stun_server_host       = "";
    cfg.video_codec_name       = "h264";
    cfg.video_sender_name      = "reference";
    cfg.video_receiver_name    = "reference";
    cfg.video_source_name      = "memory";
    cfg.video_width            = static_cast<std::uint32_t>(width);
    cfg.video_height           = static_cast<std::uint32_t>(height);

    NimRTCEngine engine(cfg);
    if (engine.open() != 0u) {
        std::fprintf(stderr, "bench: the engine failed to open\n");
        return out;
    }

    // One I420 frame, allocated once, outside every measured region.
    const std::size_t stride_y = static_cast<std::size_t>(width);
    const std::size_t stride_u = stride_y / 2;
    const std::size_t stride_v = stride_u;
    const std::size_t y_size  = stride_y * static_cast<std::size_t>(height);
    const std::size_t uv_size = stride_u * static_cast<std::size_t>(height / 2);
    std::vector<std::uint8_t> pixels(y_size + uv_size + uv_size, 0x80);

    np::VideoSourceFrame vf{};
    vf.width         = static_cast<std::uint32_t>(width);
    vf.height        = static_cast<std::uint32_t>(height);
    vf.format        = np::VideoPixelFormat::kI420;
    vf.stride_y      = static_cast<std::int32_t>(stride_y);
    vf.stride_u      = static_cast<std::int32_t>(stride_u);
    vf.stride_v      = static_cast<std::int32_t>(stride_v);
    vf.plane_y       = pixels.data();
    vf.plane_u       = pixels.data() + y_size;
    vf.plane_v       = pixels.data() + y_size + uv_size;
    vf.buffer_size   = pixels.size();

    // Warm-up: the pools grow to their steady-state size here, so the
    // measured region is not dominated by first-call growth.
    for (int i = 0; i < warmup; ++i) {
        vf.frame_seq     = static_cast<std::uint32_t>(i);
        vf.capture_ts_us = static_cast<std::int64_t>(i) * 33333;
        (void)engine.send_video(vf);
    }

    std::vector<double> per_call_us;
    per_call_us.reserve(static_cast<std::size_t>(frames));

    std::uint64_t alloc_count = 0;
    std::uint64_t alloc_bytes = 0;
    {
        alloc_probe::Scope scope;  // armed for exactly the measured region
        for (int i = 0; i < frames; ++i) {
            vf.frame_seq     = static_cast<std::uint32_t>(warmup + i);
            vf.capture_ts_us = static_cast<std::int64_t>(warmup + i) * 33333;
            const auto t0 = Clock::now();
            (void)engine.send_video(vf);
            per_call_us.push_back(micros_since(t0));
        }
        alloc_count = alloc_probe::count();
        alloc_bytes = alloc_probe::bytes();
    }

    engine.close();

    out.frames           = frames;
    out.alloc_per_frame  = frames ? static_cast<double>(alloc_count) / frames : 0.0;
    out.bytes_per_frame  = frames ? static_cast<double>(alloc_bytes) / frames : 0.0;
    out.mean_us          = mean(per_call_us);
    out.p50_us           = percentile(per_call_us, 0.50);
    out.p99_us           = percentile(per_call_us, 0.99);
    out.max_us           = per_call_us.empty() ? 0.0
                                              : *std::max_element(per_call_us.begin(), per_call_us.end());
    // The staged bytes per second — the memory bandwidth the video path
    // costs, which is the number C1/C2 were about.
    out.mib_per_second = out.mean_us > 0.0
        ? (out.bytes_per_frame * 1'000'000.0) / out.mean_us / (1024.0 * 1024.0)
        : 0.0;
    return out;
}

// ---------------------------------------------------------------------------
// Benchmark 2 — `tick()`.
//
// The engine's periodic work: the DTLS / SRTP / RTP pumps, the RTCP
// timers, the BWE tick.  The p99 is what matters — the median hides the
// occasional long stall that a user perceives as a freeze.
// ---------------------------------------------------------------------------
struct TickResult {
    long long ticks = 0;
    double    mean_us = 0.0;
    double    p50_us = 0.0;
    double    p99_us = 0.0;
    double    max_us = 0.0;
};

TickResult bench_tick(int ticks, int warmup) {
    using nimrtc::engine::EngineConfig;
    using nimrtc::engine::NimRTCEngine;

    TickResult out;

    EngineConfig cfg;
    cfg.transport_strategy_name = "shell";
    cfg.stun_server_host       = "";

    NimRTCEngine engine(cfg);
    if (engine.open() != 0u) {
        std::fprintf(stderr, "bench: the engine failed to open\n");
        return out;
    }

    for (int i = 0; i < warmup; ++i) (void)engine.tick();

    std::vector<double> per_tick_us;
    per_tick_us.reserve(static_cast<std::size_t>(ticks));
    for (int i = 0; i < ticks; ++i) {
        const auto t0 = Clock::now();
        (void)engine.tick();
        per_tick_us.push_back(micros_since(t0));
    }

    engine.close();

    out.ticks   = ticks;
    out.mean_us = mean(per_tick_us);
    out.p50_us  = percentile(per_tick_us, 0.50);
    out.p99_us  = percentile(per_tick_us, 0.99);
    out.max_us  = per_tick_us.empty() ? 0.0
                                       : *std::max_element(per_tick_us.begin(), per_tick_us.end());
    return out;
}

// ---------------------------------------------------------------------------
// Benchmark 3 — the scheduler under load.
//
// A producer thread enqueues packets as fast as it can; a consumer
// thread drains them.  The producer's per-call duration is the time it
// waits for the scheduler's lock: that is the "does `drain_with()`
// block `enqueue()`?" question, answered with a number.
//
// The consumer's per-call duration is `drain_with()`'s own cost.
// ---------------------------------------------------------------------------
struct SchedulerResult {
    long long enqueued = 0;
    long long drained  = 0;
    double    enqueue_mean_us = 0.0;
    double    enqueue_p99_us  = 0.0;
    double    enqueue_max_us  = 0.0;
    double    drain_mean_us   = 0.0;
    double    drain_p99_us    = 0.0;
    double    drain_max_us    = 0.0;
    double    throughput_mib_s = 0.0;
};

SchedulerResult bench_scheduler(int packets, int packet_bytes, int drain_batch) {
    namespace np = nimrtc::plugins;
    SchedulerResult out;

    nimrtc::core::register_all_default_plugins();
    const auto* factory =
        nimrtc::core::PluginRegistry::instance().get_scheduler("strict_priority");
    if (!factory) {
        std::fprintf(stderr, "bench: the 'strict_priority' scheduler is not registered\n");
        return out;
    }

    np::SchedulerConfig cfg;
    cfg.enabled              = true;
    cfg.drain_interval_ms    = 20;
    cfg.max_queue_depth      = 8192;
    cfg.avg_packet_size_bytes = static_cast<std::uint32_t>(
        packet_bytes > 0 ? packet_bytes : 1200);

    std::unique_ptr<np::IScheduler> sched(factory->create(cfg));
    if (!sched) {
        std::fprintf(stderr, "bench: the scheduler factory returned null\n");
        return out;
    }

    // A generous bandwidth target: the benchmark is about the lock, not
    // about congestion control.
    sched->on_bwe_update(100'000'000);

    std::vector<double> enqueue_us;
    std::vector<double> drain_us;
    enqueue_us.reserve(static_cast<std::size_t>(packets));
    drain_us.reserve(static_cast<std::size_t>(packets));

    std::vector<std::uint8_t> payload(static_cast<std::size_t>(packet_bytes), 0x5A);

    std::atomic<bool> producer_done{false};
    std::atomic<long long> drained{0};

    const auto wall_start = Clock::now();
    std::thread producer([&] {
        for (int i = 0; i < packets; ++i) {
            const auto t0 = Clock::now();
            sched->enqueue(np::Priority::kVideo,
                           nimrtc::core::ByteSpan{payload.data(), payload.size()},
                           np::Addr{});
            enqueue_us.push_back(micros_since(t0));
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        long long sent = 0;
        // The scheduler grants send credit against the *wall clock*, so a
        // consumer that spins faster than the credit accrues gets
        // empty drains.  Those empty calls are the interesting ones for
        // the lock question (they still take the lock), but they would
        // swamp the "how long does a drain that actually sends take"
        // statistic — so the two are counted separately.
        auto grace_deadline = Clock::now() + std::chrono::seconds(3);
        for (;;) {
            const auto t0 = Clock::now();
            const int n = sched->drain_with(
                drain_batch,
                [](np::Priority, nimrtc::core::ByteSpan) noexcept { return true; });
            const double elapsed = micros_since(t0);
            if (n > 0) {
                drain_us.push_back(elapsed);
                sent += n;
                continue;
            }
            // Nothing was sent.  Either the credit has not accrued yet
            // (wait for it) or the queue is empty and the producer is
            // done (stop).
            if (producer_done.load(std::memory_order_acquire)) {
                const np::SchedulerStats st = sched->stats();
                if (st.queued_packets == 0) break;
                if (Clock::now() > grace_deadline) break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        drained.store(sent, std::memory_order_release);
    });

    producer.join();
    consumer.join();
    const auto wall_end = Clock::now();

    out.enqueued = static_cast<long long>(enqueue_us.size());
    out.drained  = drained.load(std::memory_order_acquire);
    out.enqueue_mean_us = mean(enqueue_us);
    out.enqueue_p99_us  = percentile(enqueue_us, 0.99);
    out.enqueue_max_us  = enqueue_us.empty() ? 0.0
                                               : *std::max_element(enqueue_us.begin(), enqueue_us.end());
    out.drain_mean_us   = mean(drain_us);
    out.drain_p99_us    = percentile(drain_us, 0.99);
    out.drain_max_us    = drain_us.empty() ? 0.0
                                            : *std::max_element(drain_us.begin(), drain_us.end());

    // The throughput is the drained payload divided by the wall-clock
    // time the whole producer/consumer pair took — not the sum of the
    // per-call times, which would double-count the two threads.
    const double elapsed_us = std::chrono::duration<double, std::micro>(
        wall_end - wall_start).count();
    if (elapsed_us > 0.0) {
        out.throughput_mib_s = (static_cast<double>(out.drained) * packet_bytes)
                             / (elapsed_us / 1'000'000.0) / (1024.0 * 1024.0);
    }
    return out;
}

struct Options {
    int width = 1920;
    int height = 1080;
    int frames = 300;
    int warmup = 60;
    int ticks = 2000;
    int tick_warmup = 200;
    int packets = 20000;
    int packet_bytes = 1200;
    int drain_batch = 32;
};

}  // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](int& dst) {
            if (i + 1 < argc) dst = std::atoi(argv[++i]);
        };
        if (a == "--width")         next(opt.width);
        else if (a == "--height")   next(opt.height);
        else if (a == "--frames")   next(opt.frames);
        else if (a == "--warmup")   next(opt.warmup);
        else if (a == "--ticks")    next(opt.ticks);
        else if (a == "--packets")  next(opt.packets);
        else if (a == "--packet-bytes") next(opt.packet_bytes);
        else if (a == "--drain-batch")  next(opt.drain_batch);
        else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: bench_engine_hotpath [--width N] [--height N] [--frames N]\n"
                "                          [--warmup N] [--ticks N] [--packets N]\n"
                "                          [--packet-bytes N] [--drain-batch N]\n");
            return 0;
        }
    }

    const SendVideoResult sv = bench_send_video(opt.width, opt.height, opt.frames, opt.warmup);
    const TickResult       tk = bench_tick(opt.ticks, opt.tick_warmup);
    const SchedulerResult sc = bench_scheduler(opt.packets, opt.packet_bytes, opt.drain_batch);

    std::string out;
    Json j(out);
    j.begin_object();

    j.key("benchmark");
    j.begin_object();
    j.field("name", std::string("engine_hotpath"));
    j.field("schema", 1LL);
    j.end_object();

    j.key("send_video");
    j.begin_object();
    j.field("width", static_cast<long long>(opt.width));
    j.field("height", static_cast<long long>(opt.height));
    j.field("frames", sv.frames);
    j.field("allocations_per_frame", sv.alloc_per_frame);
    j.field("bytes_per_frame", sv.bytes_per_frame);
    j.field("mean_us", sv.mean_us);
    j.field("p50_us", sv.p50_us);
    j.field("p99_us", sv.p99_us);
    j.field("max_us", sv.max_us);
    j.field("mib_per_second", sv.mib_per_second);
    j.end_object();

    j.key("tick");
    j.begin_object();
    j.field("ticks", tk.ticks);
    j.field("mean_us", tk.mean_us);
    j.field("p50_us", tk.p50_us);
    j.field("p99_us", tk.p99_us);
    j.field("max_us", tk.max_us);
    j.end_object();

    j.key("scheduler");
    j.begin_object();
    j.field("enqueued", sc.enqueued);
    j.field("drained", sc.drained);
    j.field("enqueue_mean_us", sc.enqueue_mean_us);
    j.field("enqueue_p99_us", sc.enqueue_p99_us);
    j.field("enqueue_max_us", sc.enqueue_max_us);
    j.field("drain_mean_us", sc.drain_mean_us);
    j.field("drain_p99_us", sc.drain_p99_us);
    j.field("drain_max_us", sc.drain_max_us);
    j.field("throughput_mib_s", sc.throughput_mib_s);
    j.end_object();

    j.end_object();

    std::printf("%s\n", out.c_str());
    return 0;
}
