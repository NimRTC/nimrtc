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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/video_capture/synthetic_capture.hpp>

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
        return out;
    }

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
        j.field("backend", std::string("synthetic"));
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

    j.end_object();

    std::printf("%s\n", out.c_str());
    return 0;
}
