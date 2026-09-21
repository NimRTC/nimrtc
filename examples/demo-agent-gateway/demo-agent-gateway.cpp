/**
 * @file examples/demo-agent-gateway/demo-agent-gateway.cpp
 * @brief demo-agent-gateway — PCM tap → streaming ASR → LLM mock flow demo.
 *
 * ## What this validates
 *
 *   1. Profile loading with `Builder::load_profile("agent-gateway")`
 *   2. PCM tap installation (pre/post 3A) on the IAudio3A plugin
 *   3. Mock ASR: energy-based VAD (RMS > threshold = "speech")
 *   4. Mock LLM: keyword-based response ("hello" → simulated reply)
 *   5. Tap callbacks fire correctly with mock PCM data
 *
 * ## Why we instantiate IAudio3A from the registry directly
 *
 *   The engine wires audio3a via `EngineConfig::audio3a_name`. When the
 *   configured plugin id resolves AND its `open()` succeeds, `engine.audio3a()`
 *   returns the live instance. Otherwise the engine falls back to its concrete
 *   `NullAudio3A` and resets the plugin pointer, so the accessor returns
 *   nullptr. To keep the demo resilient across build configurations
 *   (vendored WebRTC APM may or may not be present), we instantiate the
 *   audio3a plugin directly from `core::PluginRegistry` — exactly the same
 *   code path the engine uses in `init_modules_once()`.
 *
 * ## Usage
 *   demo-agent-gateway              # run for 3 seconds
 *   demo-agent-gateway [seconds]    # run for N seconds
 *
 * Exit code: 0 (always).
 */

/* _CRT_SECURE_NO_WARNINGS is provided globally by cmake/NimRTCOptions.cmake. */

#define _USE_MATH_DEFINES
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nimrtc/assembly/profiles.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>

namespace {

using clk = std::chrono::steady_clock;
using namespace nimrtc;
using namespace nimrtc::assembly;
using namespace nimrtc::engine;
using namespace nimrtc::plugins;

constexpr uint32_t kSampleRate     = 48000;
constexpr uint8_t  kChannels       = 1;
constexpr int      kFrameMs        = 10;       // 10 ms per frame
constexpr size_t   kSamplesPerFrame = kSampleRate * kFrameMs / 1000;  // 480

// RMS in dBFS, 0 dBFS = full scale. Adds 1e-10 to avoid log(0).
static double rms_dbfs(const float* s, size_t n) {
    if (n == 0) return -96.0;
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) { double v = s[i]; sum += v * v; }
    return 20.0 * std::log10(std::sqrt(sum / static_cast<double>(n)) + 1e-10);
}
static double rms_dbfs_i16(const int16_t* s, size_t n) {
    if (n == 0) return -96.0;
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) { double v = s[i] / 32768.0; sum += v * v; }
    return 20.0 * std::log10(std::sqrt(sum / static_cast<double>(n)) + 1e-10);
}
static std::vector<int16_t> float_to_int16(const float* s, size_t n) {
    std::vector<int16_t> r(n);
    for (size_t i = 0; i < n; ++i) {
        float v = s[i] * 32767.0f;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        r[i] = static_cast<int16_t>(v);
    }
    return r;
}

// Mock ASR: energy-based VAD. If level > threshold emit a stub transcript.
static void mock_asr(const int16_t* pcm, size_t n) {
    constexpr float kVad = -40.0f;  // dBFS threshold for "speech"
    float lvl = static_cast<float>(rms_dbfs_i16(pcm, n));
    if (lvl > kVad) {
        std::string text = "hello world";
        std::fprintf(stderr, "[ASR]   speech: %.1f dBFS -> \"%s\"\n", double(lvl), text.c_str());
        if (text.find("hello") != std::string::npos)
            std::fprintf(stderr, "[LLM]   Hi! I heard you say: '%s'\n", text.c_str());
    } else {
        std::fprintf(stderr, "[ASR]   silence: %.1f dBFS\n", double(lvl));
    }
}

// Generate a 440Hz tone or silent frame at 48 kHz mono.
static std::vector<float> gen_frame(bool speech, size_t n) {
    static const std::vector<float> tone = [](size_t n_frames) {
        std::vector<float> t(n_frames);
        for (size_t i = 0; i < n_frames; ++i) {
            double x = double(i) / double(kSampleRate);
            t[i] = static_cast<float>(16000.0 * std::sin(2.0 * M_PI * 440.0 * x) / 32768.0);
        }
        return t;
    }(kSamplesPerFrame);
    return speech ? tone : std::vector<float>(n, 0.0f);
}

// Try the configured id first; fall back to "webrtc" (always-available stub).
static std::unique_ptr<IAudio3A> obtain_audio3a(std::string_view preferred_id) {
    auto& reg = core::PluginRegistry::instance();
    auto try_open = [&](std::string_view id, const char* label) -> std::unique_ptr<IAudio3A> {
        const IAudio3AFactory* f = reg.get_audio3a(id);
        if (!f) return nullptr;
        std::unique_ptr<IAudio3A> p{f->create()};
        if (!p) return nullptr;
        p->set_callbacks([](bool){}, [](float){},
                         [](Status, std::string_view){});
        if (p->open() != kOk) return nullptr;
        std::fprintf(stderr, "[Audio3A] obtained plugin '%s' from registry\n", label);
        return p;
    };
    if (!preferred_id.empty())
        if (auto p = try_open(preferred_id, std::string(preferred_id).c_str())) return p;
    return try_open("webrtc", "webrtc (Null stub)");
}

int run_demo(double seconds) {
    std::fprintf(stderr, "[demo-agent-gateway] starting with agent-gateway profile\n");

    // ---- Load profile via Builder --------------------------------------
    Profile profile = Builder{}.load_profile("agent-gateway").build();
    std::fprintf(stderr, "[Profile] agent-gateway profile loaded\n");
    std::fprintf(stderr, "  - audio3a: %.*s\n",
                 int(profile.audio3a_name.size()), profile.audio3a_name.data());
    std::fprintf(stderr, "  - codec:   %.*s\n",
                 int(profile.codec_name.size()), profile.codec_name.data());
    std::fprintf(stderr, "  - jb_delay: %d ms\n",
                 int(profile.jitter_buffer.initial_delay_ms));

    // ---- Engine config (audio3a off on engine — we drive it ourselves) -
    EngineConfig cfg;
    cfg.local_bind_address    = "127.0.0.1";
    cfg.local_port_range_begin = 52000;
    cfg.local_port_range_end   = 52099;
    cfg.pcm_sample_rate_hz    = profile.audio_sample_rate_hz;
    cfg.pcm_channels          = profile.audio_channels;
    cfg.audio3a_name          = "";  // demo drives audio3a via registry instance
    cfg.jb_initial_delay_ms   = int(profile.jitter_buffer.initial_delay_ms);

    NimRTCEngine engine(cfg);
    uint32_t rc = engine.open();
    std::fprintf(stderr, "[Engine] open: %u\n", rc);

    std::unique_ptr<IAudio3A> a3a = obtain_audio3a(profile.audio3a_name);
    if (!a3a) {
        std::fprintf(stderr, "[ERROR] audio3a plugin unavailable\n");
        engine.close();
        return 1;
    }

    // ---- Install PCM taps ----------------------------------------------
    a3a->set_pre_process_tap([](const float* s, const PcmFrameMetadata& m) {
        std::fprintf(stderr,
            "[Tap] pre  frame: ts=%lld, samples=%zu, rate=%u, ch=%u, level=%.1f dBFS\n",
            static_cast<long long>(m.timestamp_us), m.num_samples, m.sample_rate_hz,
            m.num_channels, rms_dbfs(s, m.num_samples));
    });
    a3a->set_post_process_tap(
        // Float tap: convert to int16 and feed mock ASR.
        [](const float* s, const PcmFrameMetadata& m) {
            auto buf = float_to_int16(s, m.num_samples);
            mock_asr(buf.data(), buf.size());
        },
        // Int16 tap: primary path for ASR.
        [](const int16_t* s, const PcmFrameMetadata& m) {
            std::fprintf(stderr,
                "[Tap] post frame: ts=%lld, samples=%zu, rate=%u, ch=%u, level=%.1f dBFS\n",
                static_cast<long long>(m.timestamp_us), m.num_samples, m.sample_rate_hz,
                m.num_channels, rms_dbfs_i16(s, m.num_samples));
            mock_asr(s, m.num_samples);
        });
    std::fprintf(stderr, "[Demo] PCM taps installed\n");

    // ---- Drive 10 ms frames through process_capture() ----------------
    std::fprintf(stderr, "[Demo] starting tap test loop (%.1f seconds)\n", seconds);
    auto deadline = clk::now() + std::chrono::milliseconds(long(seconds * 1000));
    int frame_count = 0;
    while (clk::now() < deadline) {
        bool is_speech = (frame_count % 20 < 5);   // 5 speech frames / 20 cycle
        auto pcm = gen_frame(is_speech, kSamplesPerFrame);
        a3a->process_capture(pcm.data(), pcm.size(), kChannels);
        engine.tick();
        ++frame_count;
        std::this_thread::sleep_for(std::chrono::milliseconds(kFrameMs));
    }
    std::fprintf(stderr, "[Demo] processed %d frames in %.1f seconds\n",
                 frame_count, seconds);

    // ---- Cleanup ------------------------------------------------------
    a3a->set_pre_process_tap(nullptr);
    a3a->set_post_process_tap(nullptr, nullptr);
    a3a->close();
    engine.close();
    std::fprintf(stderr, "[demo-agent-gateway] done\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    double seconds = 3.0;
    if (argc >= 2) seconds = std::atof(argv[1]);
    std::fprintf(stderr, "NimRTC demo-agent-gateway: %.1f seconds\n", seconds);
    return run_demo(seconds);
}
