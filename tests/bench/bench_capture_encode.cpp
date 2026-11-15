// tests/bench/bench_capture_encode.cpp
// ----------------------------------------------------------------------------
// The capture / encode micro-benchmark (remote-desktop plan J5).
//
// ## What this binary measures
//
// The plan's exit criterion for M1 is written in terms of *numbers*, and
// three of them have never been measured:
//
//   1. **The capture rate** — frames/s per megapixel, on the portable
//      capture path.
//   2. **The encode cost** — ms per frame, per backend (hardware vs
//      software), per resolution.
//   3. **The per-frame CPU budget** — the §B.6 table, stage by stage
//      (capture -> convert -> scale -> encode -> packetize).
//
// The third one is the reason this binary drives the **real engine**
// rather than calling the codec directly: the per-stage numbers are the
// engine's own J1 instrumentation (`Stats::video_stage_capture` and
// friends), and they only exist for a frame that went through the whole
// pipeline — the capture pump, the format conversion, the encoder and
// the packetizer.
//
// ## How the frames are produced
//
// The portable synthetic capture device (`video_capture::SyntheticCapture`)
// is the frame source.  It is the *same* device every CI leg runs, and it
// is the reference implementation the platform backends (DXGI, WGC,
// ScreenCaptureKit, PipeWire) are written against — so the numbers it
// produces are the portable layer's cost, not a fake.
//
// Two measurements are taken:
//
//   - **Capture throughput** — the device alone: `produce_one()` (which
//     renders the picture and copies it into the bounded queue) followed
//     by `pop_frame()`.  That is the frames/s per megapixel number.
//   - **Pipeline cost** — the engine: the device is driven through the
//     engine's capture pump, so the frame goes through convert -> encode
//     -> packetize as well, and the J1 counters fill in.
//
// ## Backends
//
// The encode cost is measured once per backend *class*, which is what
// `EngineConfig::video_codec_backend` selects:
//
//   - "auto"     — whatever the machine picks (hardware first).
//   - "hardware" — hardware backends only; the engine *fails to open*
//     when there is none, and that failure is recorded as a fact rather
//     than papered over with a software number.
//   - "software" — software backends only.
//
// The backend that actually ran is read back from the engine's stats
// (`video_codec_backend` / `video_codec_hardware_accelerated`), so the
// artifact says "nvenc_h264" or "openh264_h264" or "software-stub" —
// never a guess.
//
// ## Output
//
// A JSON document on stdout.  `tools/benchmark_capture_encode.py` runs
// this binary, adds the machine description and the §B.6 budget check,
// and writes the committed artifact.
//
// This is NOT a ctest case: every number in it is machine-dependent, so
// a threshold assertion here would be a flaky test rather than a
// measurement.  It follows the `bench_engine_hotpath` pattern exactly.
// ----------------------------------------------------------------------------

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/video_capture/synthetic_capture.hpp>
#if defined(_WIN32)
#include <windows.h>

#include <nimrtc/video_capture/windows_capture.hpp>
#endif

namespace {

// ---------------------------------------------------------------------------
// A tiny JSON writer — enough for this document, no dependency.
//
// (Same shape as the one in `tests/bench_engine_hotpath.cpp`; the two
// binaries are separate executables, so the helper is duplicated rather
// than moved into a shared header.)
// ---------------------------------------------------------------------------
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

    void field(const std::string& k, double v)            { key(k); value(v); }
    void field(const std::string& k, long long v)         { key(k); value(v); }
    void field(const std::string& k, unsigned long long v){ key(k); value(v); }
    void field(const std::string& k, const std::string& v){ key(k); value(v); }
    void field(const std::string& k, bool v)              { key(k); value(v); }

private:
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
// The measurement shapes.
// ---------------------------------------------------------------------------

/** One resolution the benchmark sweeps over. */
struct Resolution {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;

    [[nodiscard]] double megapixels() const noexcept {
        return (static_cast<double>(width) * static_cast<double>(height)) / 1'000'000.0;
    }

    [[nodiscard]] std::string label() const {
        return std::to_string(width) + "x" + std::to_string(height);
    }
};

/** The capture-device throughput, on the portable (synthetic) path. */
struct CaptureResult {
    std::string backend;         // "synthetic", "windows-dxgi", "windows-wgc"
    bool        opened = false;  // the device actually opened
    std::string error;           // why it did not open
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    long long   frames = 0;
    double      seconds = 0.0;
    double      fps = 0.0;
    double      megapixels = 0.0;
    double      fps_per_megapixel = 0.0;
    /** The mean cost of `produce_one()` — rendering the picture and
     *  copying it into the bounded queue. */
    double      produce_mean_us = 0.0;
    /** The mean cost of `pop_frame()` — the queue handoff. */
    double      pop_mean_us = 0.0;
    /** The mean cost of one produced+popped frame, end to end. */
    double      frame_mean_us = 0.0;
    /** Zero-copy evidence (plan B10): frames delivered as GPU surfaces. */
    long long   frames_zero_copy = 0;
    /** Frames staged into host memory (the fallback path). */
    long long   frames_cpu_staged = 0;
    /** Bytes copied between CPU and GPU. Zero means fully zero-copy. */
    long long   bytes_copied = 0;
    /** Frames the device dropped (the queue was full). */
    long long   frames_dropped = 0;
};

/** The pipeline cost for one (backend class, resolution) pair. */
struct PipelineResult {
    std::string preference;      // "auto" / "hardware" / "software"
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    bool          opened = false;
    std::string   error;         // why the engine refused to open
    std::string   backend;       // the backend that actually ran
    bool          hardware_accelerated = false;
    long long     frames = 0;    // frames the pump actually encoded
    double        seconds = 0.0;
    double        frames_per_second = 0.0;
    double        capture_mean_us = 0.0;
    double        convert_mean_us = 0.0;
    double        encode_mean_us = 0.0;
    double        packetize_mean_us = 0.0;
    double        total_mean_us = 0.0;
    double        total_p95_us = 0.0;
    double        encode_p95_us = 0.0;
    double        encode_max_us = 0.0;
    double        encode_ms_per_frame = 0.0;
    double        megapixels = 0.0;
    /** The number of samples each stage collected.  A stage with zero
     *  samples did not run at all (the zero-copy path has no convert
     *  stage) — which is a fact, not a missing value. */
    long long     capture_samples = 0;
    long long     convert_samples = 0;
    long long     encode_samples = 0;
    long long     packetize_samples = 0;
    /** The codec's *own* self-reported encode latency (plan B15).  It is
     *  measured from inside the codec, so it is a useful cross-check on
     *  the engine's bracketed measurement. */
    double        codec_encode_latency_avg_us = 0.0;
    double        codec_encode_latency_max_us = 0.0;
    /** The number of frames the codec actually encoded. */
    long long     codec_frames_encoded = 0;
    long long     codec_bytes_encoded = 0;
    /** The engine's format-normalisation counters. */
    long long     format_conversions = 0;
    long long     format_copies = 0;
};

// ---------------------------------------------------------------------------
// Measurement 1 — the capture device's throughput.
//
// The device is driven directly: `produce_one()` renders the picture and
// queues it, `pop_frame()` takes it out.  The two costs are reported
// separately because they are different things — the first is the
// device's own work (a platform backend's equivalent is the desktop
// duplication), the second is the queue handoff the engine's pump pays.
// ---------------------------------------------------------------------------
CaptureResult bench_capture(const Resolution& res, int frames, int warmup) {
    namespace np = nimrtc::plugins;

    CaptureResult out;
    out.backend = "synthetic";
    out.width  = res.width;
    out.height = res.height;
    out.megapixels = res.megapixels();

    np::CaptureConfig cfg;
    cfg.format.width  = res.width;
    cfg.format.height = res.height;
    cfg.format.fps    = 30;
    cfg.format.tag    = "bgra";
    cfg.queue_depth   = 4;

    nimrtc::video_capture::SyntheticCapture device(cfg);
    if (device.open() != np::kOk) {
        std::fprintf(stderr, "bench: the synthetic capture device refused to open\n");
        out.error = "the synthetic capture device refused to open";
        return out;
    }
    out.opened = true;

    np::CaptureFrame frame;

    // Warm-up: the first frames pay for the queue's first allocation and
    // the device's scratch buffer.
    for (int i = 0; i < warmup; ++i) {
        (void)device.produce_one();
        (void)device.pop_frame(frame);
    }

    double produce_us = 0.0;
    double pop_us = 0.0;

    const auto wall_start = Clock::now();
    for (int i = 0; i < frames; ++i) {
        const auto t0 = Clock::now();
        (void)device.produce_one();
        const auto t1 = Clock::now();
        (void)device.pop_frame(frame);
        const auto t2 = Clock::now();
        produce_us += micros_since(t0);
        pop_us += micros_since(t1);
    }
    const auto wall_end = Clock::now();

    device.close();

    out.frames  = frames;
    out.seconds = std::chrono::duration<double>(wall_end - wall_start).count();
    out.fps = out.seconds > 0.0 ? static_cast<double>(frames) / out.seconds : 0.0;
    out.fps_per_megapixel =
        out.megapixels > 0.0 ? out.fps / out.megapixels : 0.0;
    out.produce_mean_us = frames ? produce_us / frames : 0.0;
    out.pop_mean_us = frames ? pop_us / frames : 0.0;
    out.frame_mean_us = out.produce_mean_us + out.pop_mean_us;
    return out;
}

// ---------------------------------------------------------------------------
// Measurement 1b — the *platform* capture backends' throughput.
//
// The synthetic device is the portable layer's cost.  The numbers that
// matter for a real remote desktop are the platform backends': DXGI
// Desktop Duplication and Windows Graphics Capture.  They are measured
// the same way — the device is opened, frames are pulled, and the wall
// clock is read — but the *source* of the frames is the desktop.
//
// A platform backend that cannot open on this machine (no display, a
// secure desktop, a refused consent) is **not** a failure: it is a
// missing measurement, and it is recorded as such.  The artifact never
// claims a number it did not measure.
// ---------------------------------------------------------------------------
#if defined(_WIN32)

/**
 * @brief The desktop activator — makes the desktop *change* during a
 *        measurement window.
 *
 * DXGI Desktop Duplication and WGC deliver a frame **only when the
 * desktop changes**.  On an idle machine nothing changes, so the
 * backends deliver nothing and a throughput measurement has nothing to
 * measure.  That is not a backend limitation — it is the whole point
 * of a dirty-rect-based capture path — but it does mean the rate can
 * only be measured while something moves.
 *
 * The activator jiggles the mouse pointer across the desktop.  The
 * pointer's original position is restored when the window closes, so
 * the benchmark leaves the machine as it found it.
 *
 * It is deliberately the *cheapest* possible change: the compositor
 * only has to re-blit the cursor, so the measured rate is the
 * backend's delivery rate and not the compositor's redraw rate.
 */
class DesktopActivator {
public:
    DesktopActivator() {
        if (::GetCursorPos(&saved_)) {
            valid_ = true;
        }
    }

    ~DesktopActivator() {
        if (valid_) {
            ::SetCursorPos(saved_.x, saved_.y);
        }
    }

    DesktopActivator(const DesktopActivator&)            = delete;
    DesktopActivator& operator=(const DesktopActivator&) = delete;

    /** Move the pointer one step. Called from the measurement loop. */
    void step() const {
        if (!valid_) return;
        // A small, fast sweep: the pointer moves far enough to dirty
        // the screen, but not far enough to disturb the user.
        static LONG x = 0;
        x += 24;
        if (x > 240) x = 0;
        ::SetCursorPos(saved_.x + x, saved_.y + ((x / 24) % 2 ? 8 : -8));
    }

private:
    POINT saved_{};
    bool  valid_ = false;
};

/**
 * @brief Measure one Windows capture backend.
 *
 * The device is driven through the `IVideoCapture` seam: the engine
 * does exactly this, so the number is the number the product pays.
 *
 * The measurement runs over a **time window**, not over a frame count.
 * That is not a stylistic choice: a desktop-duplication backend
 * delivers a frame when the desktop *changes*, so on a static desktop
 * it delivers nothing at all.  A "count N frames" loop would then
 * measure how long the loop waited, not how fast the backend is.  The
 * window makes the two cases distinguishable:
 *
 *  - a busy desktop yields a frame rate near the compositor's cadence;
 *  - a static desktop yields *no* frames, which the artifact records
 *    as a static-desktop observation rather than as a zero-fps backend.
 *
 * The `CaptureStats` counters are read *before* and *after* the window
 * and subtracted, so the warm-up frames are excluded from the count.
 */
CaptureResult bench_windows_capture(nimrtc::video_capture::WindowsCaptureBackend backend,
                                    int frames,
                                    int warmup) {
    namespace np = nimrtc::plugins;

    CaptureResult out;
    out.backend = nimrtc::video_capture::windows_capture_backend_name(backend);
    out.opened = false;

    // The resolution is the *display's* own: a desktop-duplication
    // backend captures whatever the display is, not an arbitrary
    // resolution, so the requested one is only a hint.
    np::CaptureConfig cfg;
    cfg.format.width  = 1920;
    cfg.format.height = 1080;
    cfg.format.fps    = 30;
    cfg.format.tag    = "bgra";
    cfg.queue_depth   = 4;

    nimrtc::video_capture::WindowsCaptureOptions opts;
    opts.backend = backend;

    nimrtc::video_capture::WindowsCapture device(cfg, opts);
    const np::Status status = device.open();
    if (status != np::kOk) {
        out.error = "the capture device refused to open (status=" +
                    std::to_string(static_cast<int>(status)) + ")";
        std::fprintf(stderr, "bench: %s refused to open: %s\n", out.backend.c_str(),
                     out.error.c_str());
        return out;
    }
    out.opened = true;

    // The device's own resolution is the one that was measured.
    const np::CaptureConfig active = device.config();
    out.width  = active.format.width;
    out.height = active.format.height;
    out.megapixels = out.width && out.height
                         ? (static_cast<double>(out.width) *
                            static_cast<double>(out.height)) / 1'000'000.0
                         : 0.0;

    np::CaptureFrame frame;

    // Warm-up: the first frames pay for the backend's own allocations
    // (the D3D11 device, the texture pool, the duplication object).
    for (int i = 0; i < warmup; ++i) {
        (void)device.pop_frame(frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // The counters are the source of truth for "how many frames did the
    // backend actually deliver"; the queue is drained alongside so the
    // backend is never blocked by a full queue.
    const np::CaptureStats before = device.stats();

    // The window is long enough to hold a meaningful number of frames
    // at the compositor's cadence, and short enough to keep the
    // benchmark quick.
    //
    // The activator makes the desktop change during the window, so the
    // backends actually deliver.  Without it a static desktop would
    // yield no frames and the rate could not be measured at all.
    DesktopActivator activator;

    const auto window = std::chrono::milliseconds(2000);
    const auto wall_start = Clock::now();
    const auto deadline = wall_start + window;
    while (Clock::now() < deadline) {
        activator.step();
        (void)device.pop_frame(frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto wall_end = Clock::now();

    const np::CaptureStats after = device.stats();

    device.close();

    const long long delivered = static_cast<long long>(
        after.frames_captured - before.frames_captured);
    out.frames  = delivered;
    out.seconds = std::chrono::duration<double>(wall_end - wall_start).count();
    out.fps = out.seconds > 0.0 ? static_cast<double>(delivered) / out.seconds : 0.0;
    out.fps_per_megapixel =
        out.megapixels > 0.0 ? out.fps / out.megapixels : 0.0;
    // The platform backend's own per-frame cost is not separable from
    // the wait: the frame arrives when the desktop changes.  What *is*
    // separable is the queue handoff, which the mean pop cost carries.
    out.frame_mean_us =
        out.frames > 0 ? (out.seconds * 1'000'000.0) / static_cast<double>(out.frames) : 0.0;

    // The zero-copy evidence (plan B10): the device's own counters say
    // how many frames never crossed the PCI bus and how many bytes were
    // copied instead.  A backend that staged everything reports
    // `frames_zero_copy == 0` and a non-zero `bytes_copied` — the
    // artifact says so rather than claiming zero-copy.
    out.frames_zero_copy =
        static_cast<long long>(after.frames_zero_copy - before.frames_zero_copy);
    out.frames_cpu_staged =
        static_cast<long long>(after.frames_cpu_staged - before.frames_cpu_staged);
    out.bytes_copied =
        static_cast<long long>(after.bytes_copied - before.bytes_copied);
    out.frames_dropped =
        static_cast<long long>(after.frames_dropped - before.frames_dropped);

    // A backend that delivered nothing over the window was measuring a
    // *static* desktop, not a broken one.  The distinction matters: the
    // artifact must not report "0 fps capture" as if the backend were
    // the bottleneck.
    if (delivered == 0) {
        out.error = "no frames were delivered over the measurement window "
                    "(the desktop was static — DXGI and WGC only deliver on "
                    "a change)";
    }
    return out;
}

#endif // defined(_WIN32)

// ---------------------------------------------------------------------------
// Measurement 1c — the zero-copy path, measured.
//
// Plan B10 asks for *evidence* that the GPU path is real: frames that
// reach the encoder as native surfaces, never crossing the PCI bus.
// The device counts that itself (`CaptureStats::frames_zero_copy` vs
// `bytes_copied`), but only when a GPU consumer is attached — without
// one, the device is right to stage to the CPU.
//
// So the measurement attaches a GPU consumer that holds each frame
// alive for one iteration, exactly like a hardware encoder does, and
// reports what the device counted.  The number of frames that arrived
// through the GPU callback is measured *independently* of the device's
// own counter, so the two can be cross-checked: if the callback saw
// fewer frames than the device claims, the artifact says so.
// ---------------------------------------------------------------------------
#if defined(_WIN32)

/** The zero-copy evidence for one Windows capture backend. */
struct ZeroCopyResult {
    std::string backend;
    bool        opened = false;
    std::string error;
    /** Frames the GPU callback actually received. */
    long long   gpu_frames_received = 0;
    /** Frames the device claims it delivered zero-copy. */
    long long   frames_zero_copy = 0;
    /** Frames the device staged into host memory instead. */
    long long   frames_cpu_staged = 0;
    /** Bytes copied CPU↔GPU. Zero means the fast path was taken. */
    long long   bytes_copied = 0;
    /** True when the fast path was really taken. */
    bool        zero_copy = false;
};

ZeroCopyResult bench_windows_zero_copy(
    nimrtc::video_capture::WindowsCaptureBackend backend,
    int frames,
    int warmup) {
    namespace np = nimrtc::plugins;

    ZeroCopyResult out;
    out.backend = nimrtc::video_capture::windows_capture_backend_name(backend);

    np::CaptureConfig cfg;
    cfg.format.width  = 1920;
    cfg.format.height = 1080;
    cfg.format.fps    = 30;
    cfg.format.tag    = "bgra";
    cfg.queue_depth   = 4;

    nimrtc::video_capture::WindowsCaptureOptions opts;
    opts.backend = backend;
    opts.allow_zero_copy = true;

    nimrtc::video_capture::WindowsCapture device(cfg, opts);
    if (device.open() != np::kOk) {
        out.error = "the capture device refused to open";
        std::fprintf(stderr, "bench: %s refused to open (zero-copy probe)\n",
                     out.backend.c_str());
        return out;
    }
    out.opened = true;

    // The GPU consumer: it receives the surface and holds it for one
    // iteration — the lifetime a hardware encoder needs.
    std::vector<std::shared_ptr<nimrtc::video_frame::GpuBuffer>> held;
    held.reserve(4);
    std::atomic<long long> received{0};

    device.set_gpu_frame_callback(
        [&](nimrtc::video_capture::GpuCaptureFrame&& frame) {
            received.fetch_add(1, std::memory_order_relaxed);
            held.push_back(std::move(frame.gpu_buffer));
            // Keep the window small: the point is the delivery, not the
            // retention.  A real encoder drains it every frame.
            if (held.size() > 3) held.erase(held.begin());
        });

    const auto wall_start = Clock::now();
    const auto deadline = wall_start + std::chrono::seconds(5);
    DesktopActivator activator;
    while (received.load(std::memory_order_relaxed) < frames &&
           Clock::now() < deadline) {
        activator.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto wall_end = Clock::now();

    // Let the device settle its counters after the last frame.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const np::CaptureStats stats = device.stats();
    out.gpu_frames_received = received.load(std::memory_order_relaxed);
    out.frames_zero_copy   = static_cast<long long>(stats.frames_zero_copy);
    out.frames_cpu_staged  = static_cast<long long>(stats.frames_cpu_staged);
    out.bytes_copied       = static_cast<long long>(stats.bytes_copied);
    // The fast path is proven when the GPU callback received frames and
    // the device copied no bytes to the host.  Zero received frames
    // means the desktop was static — an *absence of evidence*, not a
    // failure of the zero-copy path, and it is reported as such.
    out.zero_copy = out.gpu_frames_received > 0 && out.bytes_copied == 0 &&
                    out.frames_zero_copy > 0;
    if (out.gpu_frames_received == 0) {
        out.error = "no GPU frames were delivered over the probe window "
                    "(the desktop was static — DXGI and WGC only deliver on "
                    "a change)";
    }

    (void)warmup;
    device.close();
    (void)wall_end;
    return out;
}

#endif // defined(_WIN32)

// ---------------------------------------------------------------------------
// Measurement 2 — the pipeline cost, per backend class and resolution.
//
// The engine owns the codec, so this is the *real* path: the capture
// pump pops the frame, the engine converts it to the codec's canonical
// format, the codec encodes it and the sender packetizes it.  Every one
// of those steps is timed by the engine itself (plan J1), so the numbers
// that come out are exactly the ones §B.6's budget is written against.
//
// The transport strategy is "shell": no ICE, no DTLS, no network.  What
// is measured is the CPU cost of the media path, not the wire.
// ---------------------------------------------------------------------------
PipelineResult bench_pipeline(const std::string& preference,
                               const Resolution& res,
                               int frames,
                               int warmup) {
    using nimrtc::engine::EngineConfig;
    using nimrtc::engine::NimRTCEngine;
    namespace np = nimrtc::plugins;

    PipelineResult out;
    out.preference = preference;
    out.width  = res.width;
    out.height = res.height;
    out.megapixels = res.megapixels();

    EngineConfig cfg;
    cfg.transport_strategy_name = "shell";
    cfg.stun_server_host       = "";
    cfg.video_codec_name       = "h264";
    cfg.video_sender_name      = "reference";
    cfg.video_receiver_name    = "reference";
    cfg.video_source_name      = "memory";
    cfg.video_sink_name        = "headless";
    cfg.video_width            = res.width;
    cfg.video_height           = res.height;
    cfg.video_fps              = 30;
    cfg.video_codec_backend    = preference;

    // The capture device: the synthetic one, at the resolution under
    // test.  The device id is the one the device publishes, so the
    // engine's capability check sees the real format list.
    cfg.video_capture_name = "synthetic";
    cfg.video_capture_config.device_id = "synthetic:0";
    cfg.video_capture_config.format.width  = res.width;
    cfg.video_capture_config.format.height = res.height;
    cfg.video_capture_config.format.fps    = 30;
    cfg.video_capture_config.queue_depth   = 4;
    cfg.video_capture_pump = true;

    NimRTCEngine engine(cfg);
    const std::uint32_t rc = engine.open();
    if (rc != 0u) {
        out.opened = false;
        out.error = "the engine refused to open (rc=" + std::to_string(rc) + ")";
        return out;
    }
    out.opened = true;

    // The engine owns the device; the benchmark needs to *drive* it, so
    // the concrete synthetic type is recovered from the seam handle.
    auto* device = dynamic_cast<nimrtc::video_capture::SyntheticCapture*>(
        engine.video_capture());
    if (device == nullptr) {
        out.error = "the engine did not resolve the synthetic capture device";
        engine.close();
        return out;
    }

    // Warm-up: the first frames pay for the codec's own warm-up (an
    // IDR, the buffer pool's first allocation, the encoder session's
    // first frame), so they are not part of the measurement.
    for (int i = 0; i < warmup; ++i) {
        (void)device->produce_one();
        (void)engine.tick();
    }

    const auto wall_start = Clock::now();
    for (int i = 0; i < frames; ++i) {
        (void)device->produce_one();
        (void)engine.tick();
    }
    const auto wall_end = Clock::now();

    const auto stats = engine.stats();
    out.backend = stats.video_codec_backend;
    out.hardware_accelerated = stats.video_codec_hardware_accelerated;
    out.frames = static_cast<long long>(stats.video_capture_pumped_frames);
    out.seconds = std::chrono::duration<double>(wall_end - wall_start).count();
    out.frames_per_second =
        out.seconds > 0.0 ? static_cast<double>(out.frames) / out.seconds : 0.0;

    out.capture_mean_us   = static_cast<double>(stats.video_stage_capture.mean_us);
    out.convert_mean_us   = static_cast<double>(stats.video_stage_convert.mean_us);
    out.encode_mean_us    = static_cast<double>(stats.video_stage_encode.mean_us);
    out.packetize_mean_us = static_cast<double>(stats.video_stage_packetize.mean_us);
    out.total_mean_us     = static_cast<double>(stats.video_stage_total_us);
    out.total_p95_us      = static_cast<double>(stats.video_stage_total_p95_us);
    out.encode_p95_us     = static_cast<double>(stats.video_stage_encode.p95_us);
    out.encode_max_us     = static_cast<double>(stats.video_stage_encode.max_us);
    out.encode_ms_per_frame = out.encode_mean_us / 1000.0;

    out.capture_samples   = static_cast<long long>(stats.video_stage_capture.samples);
    out.convert_samples   = static_cast<long long>(stats.video_stage_convert.samples);
    out.encode_samples    = static_cast<long long>(stats.video_stage_encode.samples);
    out.packetize_samples = static_cast<long long>(stats.video_stage_packetize.samples);

    out.codec_encode_latency_avg_us =
        static_cast<double>(stats.video_codec_encode_latency_avg_us);
    out.codec_encode_latency_max_us =
        static_cast<double>(stats.video_codec_encode_latency_max_us);
    out.codec_frames_encoded =
        static_cast<long long>(stats.video_codec_frames_staged +
                               stats.video_codec_frames_zero_copy);
    out.codec_bytes_encoded = 0;   // the engine does not surface it
    out.format_conversions = static_cast<long long>(stats.video_format_conversions);
    out.format_copies      = static_cast<long long>(stats.video_format_copies);

    engine.close();
    return out;
}

struct Options {
    int frames = 60;
    int warmup = 15;
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
        if (a == "--frames")        next(opt.frames);
        else if (a == "--warmup")   next(opt.warmup);
        else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: bench_capture_encode [--frames N] [--warmup N]\n"
                "\n"
                "Measures the capture throughput (frames/s per megapixel) and\n"
                "the per-stage CPU cost of the capture -> convert -> encode ->\n"
                "packetize chain, per backend class (auto / hardware / software)\n"
                "and per resolution.  Prints a JSON document on stdout.\n");
            return 0;
        }
    }

    // The plugin registry has to know the capture device and the codec.
    nimrtc::core::register_all_default_plugins();

    const Resolution resolutions[] = {
        {640, 480},
        {1280, 720},
        {1920, 1080},
    };
    const char* preferences[] = {"auto", "hardware", "software"};

    std::vector<CaptureResult> captures;
    for (const auto& res : resolutions) {
        captures.push_back(bench_capture(res, opt.frames, opt.warmup));
    }

    // The platform backends' throughput (Windows only).  A backend that
    // cannot open is recorded as "not measured" rather than skipped.
    std::vector<CaptureResult> platform_captures;
#if defined(_WIN32)
    {
        using nimrtc::video_capture::WindowsCaptureBackend;
        platform_captures.push_back(bench_windows_capture(
            WindowsCaptureBackend::kDxgiDesktopDuplication, opt.frames, opt.warmup));
        platform_captures.push_back(bench_windows_capture(
            WindowsCaptureBackend::kWgc, opt.frames, opt.warmup));
    }
#endif

    std::vector<PipelineResult> pipelines;
    for (const char* pref : preferences) {
        for (const auto& res : resolutions) {
            pipelines.push_back(bench_pipeline(pref, res, opt.frames, opt.warmup));
        }
    }

    std::string out;
    Json j(out);
    j.begin_object();

    j.key("benchmark");
    j.begin_object();
    j.field("name", std::string("capture_encode"));
    j.field("schema", 1LL);
    j.field("frames", static_cast<long long>(opt.frames));
    j.field("warmup", static_cast<long long>(opt.warmup));
    j.field("hardware_concurrency",
            static_cast<long long>(std::thread::hardware_concurrency()));
    j.end_object();

    j.key("capture");
    j.begin_array();
    for (const auto& c : captures) {
        j.begin_object();
        j.field("backend", c.backend);
        j.field("opened", c.opened);
        if (!c.error.empty()) j.field("error", c.error);
        j.field("width", static_cast<long long>(c.width));
        j.field("height", static_cast<long long>(c.height));
        j.field("frames", c.frames);
        j.field("seconds", c.seconds);
        j.field("fps", c.fps);
        j.field("megapixels", c.megapixels);
        j.field("fps_per_megapixel", c.fps_per_megapixel);
        j.field("produce_mean_us", c.produce_mean_us);
        j.field("pop_mean_us", c.pop_mean_us);
        j.field("frame_mean_us", c.frame_mean_us);
        j.end_object();
    }
    j.end_array();

    // The platform backends' throughput.  A backend that could not open
    // is present with `"opened": false` — the artifact records the
    // missing measurement rather than hiding it.
    j.key("platform_capture");
    j.begin_array();
    for (const auto& c : platform_captures) {
        j.begin_object();
        j.field("backend", c.backend);
        j.field("opened", c.opened);
        if (!c.error.empty()) j.field("error", c.error);
        j.field("width", static_cast<long long>(c.width));
        j.field("height", static_cast<long long>(c.height));
        j.field("frames", c.frames);
        j.field("seconds", c.seconds);
        j.field("fps", c.fps);
        j.field("megapixels", c.megapixels);
        j.field("fps_per_megapixel", c.fps_per_megapixel);
        j.field("frame_mean_us", c.frame_mean_us);
        j.field("frames_zero_copy", c.frames_zero_copy);
        j.field("frames_cpu_staged", c.frames_cpu_staged);
        j.field("bytes_copied", c.bytes_copied);
        j.field("frames_dropped", c.frames_dropped);
        j.end_object();
    }
    j.end_array();

    j.key("pipeline");
    j.begin_array();
    for (const auto& p : pipelines) {
        j.begin_object();
        j.field("backend_preference", p.preference);
        j.field("width", static_cast<long long>(p.width));
        j.field("height", static_cast<long long>(p.height));
        j.field("opened", p.opened);
        if (!p.error.empty()) j.field("error", p.error);
        j.field("backend", p.backend);
        j.field("hardware_accelerated", p.hardware_accelerated);
        j.field("frames", p.frames);
        j.field("seconds", p.seconds);
        j.field("frames_per_second", p.frames_per_second);
        j.field("megapixels", p.megapixels);
        j.key("stage_us");
        j.begin_object();
        j.field("capture", p.capture_mean_us);
        j.field("convert", p.convert_mean_us);
        j.field("encode", p.encode_mean_us);
        j.field("packetize", p.packetize_mean_us);
        j.field("total", p.total_mean_us);
        j.end_object();
        j.field("encode_p95_us", p.encode_p95_us);
        j.field("encode_max_us", p.encode_max_us);
        j.field("total_p95_us", p.total_p95_us);
        j.field("encode_ms_per_frame", p.encode_ms_per_frame);
        j.key("stage_samples");
        j.begin_object();
        j.field("capture", p.capture_samples);
        j.field("convert", p.convert_samples);
        j.field("encode", p.encode_samples);
        j.field("packetize", p.packetize_samples);
        j.end_object();
        j.field("codec_encode_latency_avg_us", p.codec_encode_latency_avg_us);
        j.field("codec_encode_latency_max_us", p.codec_encode_latency_max_us);
        j.field("codec_frames_encoded", p.codec_frames_encoded);
        j.field("format_conversions", p.format_conversions);
        j.field("format_copies", p.format_copies);
        j.end_object();
    }
    j.end_array();

    // The zero-copy evidence (plan B10).  `zero_copy: true` means the
    // GPU callback received frames AND the device copied no bytes — the
    // pixels never entered host memory.
    j.key("zero_copy");
    j.begin_array();
#if defined(_WIN32)
    {
        using nimrtc::video_capture::WindowsCaptureBackend;
        const ZeroCopyResult probes[] = {
            bench_windows_zero_copy(WindowsCaptureBackend::kDxgiDesktopDuplication,
                                    opt.frames, opt.warmup),
            bench_windows_zero_copy(WindowsCaptureBackend::kWgc,
                                    opt.frames, opt.warmup),
        };
        for (const auto& z : probes) {
            j.begin_object();
            j.field("backend", z.backend);
            j.field("opened", z.opened);
            if (!z.error.empty()) j.field("error", z.error);
            j.field("gpu_frames_received", z.gpu_frames_received);
            j.field("frames_zero_copy", z.frames_zero_copy);
            j.field("frames_cpu_staged", z.frames_cpu_staged);
            j.field("bytes_copied", z.bytes_copied);
            j.field("zero_copy", z.zero_copy);
            j.end_object();
        }
    }
#endif
    j.end_array();

    j.end_object();

    std::printf("%s\n", out.c_str());
    return 0;
}
