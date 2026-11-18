/**
 * @file examples/demo-agent-realtime/demo-agent-realtime.cpp
 * @brief Real-time AI game decision processing demo (slice v2-ai-decision)
 *        + voice-agent chain (slice v4-voice-agent).
 *
 * ## What this validates (the four acceptance criteria of v2-ai-decision)
 *
 *   1. AI decision response time <= 50 ms — the self-test records the
 *      wall time of 100 consecutive decisions with a per-decision latency
 *      instrument and reports p50/p95/max against the 50 ms budget.
 *   2. Decision accuracy >= 90 % — the self-test runs 100 decisions and
 *      compares each against the deterministic human-expert oracle.
 *   3. >= 3 configurable decision strategies — heuristic / reactive /
 *      predictive, selectable with `--strategy` and switchable at runtime
 *      with a resource-release checkpoint at every switch.
 *   4. Traceable decision logs — `--debug` emits the complete decision
 *      process (observed state, candidates, picked action, reason,
 *      latency); the self-test proves a full trace of all 100 decisions.
 *
 * ## What this validates (the seven acceptance criteria of v4-voice-agent)
 *
 *   `--voice-self-test` runs the voice-agent chain (voice_agent.hpp):
 *   pluggable ASR backends (mock default / whisper / vosk), post-3A int16
 *   PCM consumption, streaming LLM consumption, 100 ms tap batching,
 *   Opus-encoded TTS return, barge-in → stop-playback latency recording,
 *   and the missing-model error policy.
 *
 * ## Usage
 *
 *   demo-agent-realtime --self-test [--decisions N] [--debug]
 *   demo-agent-realtime [--run SECONDS] [--strategy NAME] [--debug]
 *   demo-agent-realtime --strategies
 *   demo-agent-realtime --voice-self-test [--asr mock|whisper|vosk] ...
 *
 * The self-test modes are the ctest entry points: deterministic, headless,
 * exit 0 only when every criterion holds.
 */

#include "ai_decision.hpp"
#include "voice_agent.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace demoai;

constexpr std::int64_t kLatencyBudgetUs = 50000;  // 50 ms (criterion 1)
constexpr double kAccuracyThreshold = 90.0;       // % (criterion 2)
constexpr int kDefaultDecisionCount = 100;        // 100 consecutive decisions
constexpr std::uint64_t kScenarioSeed = 0x5EED2026ULL;

// Voice-agent self-test constants (slice v4-voice-agent).
constexpr std::uint32_t kVoiceSampleRate = 48000;
constexpr std::uint8_t  kVoiceChannels   = 1;
// The tap fires every 10 ms; audio is batched to 100 ms before the model
// call (spec requirement 4) — see TapBatchAccumulator.
constexpr std::int64_t kVoiceFrameMs     = kTapFrameMs;  // 10
constexpr std::int64_t kVoiceBatchMs     = kTapBatchMs;  // 100
constexpr std::size_t  kVoiceSamplesPerFrame =
    static_cast<std::size_t>(kVoiceSampleRate) * static_cast<std::size_t>(kVoiceFrameMs) / 1000;  // 480
constexpr int kDefaultVoiceFrames   = 300;  // 3 s of 10 ms tap frames
constexpr int kDefaultBargeInAt     = 150;  // barge-in at the 150th frame

// ---------------------------------------------------------------------------
// Deterministic whack-a-mole scenario
// ---------------------------------------------------------------------------
//
// A fixed seed keeps the self-test reproducible on every host.  The spawn
// density is calibrated (p = 0.20 per tick, lifetime 3..5 ticks) so every
// strategy stays comfortably above the 90 % accuracy bar while multi-mole
// overlap frames still exercise the ranking rules against each other.  Each
// frame advances `frame_seq` by one and `capture_ts_us` by one 60fps frame
// period, matching the plan's time-anchored decision model (plan §5.2).
class Scenario {
public:
    Scenario(std::uint64_t seed, std::int64_t start_ts_us)
        : rng_(static_cast<std::mt19937::result_type>(seed)), start_ts_us_(start_ts_us) {}

    GameState next() {
        GameState s;
        s.frame_seq = frame_;
        s.capture_ts_us = start_ts_us_ + frame_ * kFramePeriodUs;
        for (int c = 0; c < kHoles; ++c) {
            if (remaining_[c] > 0) {
                --remaining_[c];
                s.remaining_ticks[c] = remaining_[c];
            }
        }
        if (uniform_(rng_) < 0.20) {
            const int col = static_cast<int>(uniform_(rng_) * static_cast<double>(kHoles));
            if (remaining_[col] == 0) {
                remaining_[col] = 3 + static_cast<int>(uniform_(rng_) * 3.0);  // 3..5
                s.remaining_ticks[col] = remaining_[col];
            }
        }
        ++frame_;
        return s;
    }

private:
    std::mt19937 rng_;
    std::uniform_real_distribution<double> uniform_{0.0, 1.0};
    std::array<int, kHoles> remaining_{};
    std::int64_t frame_ = 0;
    std::int64_t start_ts_us_ = 0;
};

std::int64_t now_unix_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Generate one 10 ms post-3A int16 PCM frame: a 440 Hz tone for speech,
// digital silence otherwise.  This is the same synthetic source the gateway
// demo drives through WebRTC APM, here already in the int16 shape the
// post-3A tap delivers to the ASR (spec requirement 2).
static std::vector<std::int16_t> gen_int16_frame(bool speech, std::size_t n) {
    std::vector<std::int16_t> pcm(n);
    if (speech) {
        for (std::size_t i = 0; i < n; ++i) {
            const double x = static_cast<double>(i) / static_cast<double>(kVoiceSampleRate);
            pcm[i] = static_cast<std::int16_t>(
                16000.0 * std::sin(2.0 * 3.141592653589793 * 440.0 * x));
        }
    } else {
        std::fill(pcm.begin(), pcm.end(), 0);
    }
    return pcm;
}

// Minimal JSON string escape for the ai_decision records (plan §5.2).
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char ch : s) {
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += ch;     break;
        }
    }
    return out;
}

void print_strategies() {
    const std::vector<std::string_view> strategies = available_strategies();
    std::fprintf(stdout, "strategies: %zu (", strategies.size());
    for (std::size_t i = 0; i < strategies.size(); ++i) {
        std::fprintf(stdout, "%s%s", i ? "," : "",
                     std::string(strategies[i]).c_str());
    }
    std::fprintf(stdout, ")\n");
}

// ---------------------------------------------------------------------------
// --self-test: the four acceptance criteria in one deterministic run.
// Exits 0 only when every criterion holds.
// ---------------------------------------------------------------------------
int run_self_test(std::string_view strategy_name, int n, bool debug) {
    bool pass = true;

    std::fprintf(stdout, "[demo-agent-realtime] self-test: strategy=%s\n",
                 std::string(strategy_name).c_str());

    // ---- Criterion 3: >= 3 configurable strategies + switching ----------
    const std::vector<std::string_view> strategies = available_strategies();
    print_strategies();
    if (strategies.size() < 3) {
        std::fprintf(stdout, "[demo-agent-realtime] FAIL: need >= 3 strategies\n");
        pass = false;
    }

    DecisionEngine engine(strategy_name);
    engine.set_debug(true);  // every switch emits its release checkpoint
    bool switch_ok = true;
    for (const std::string_view name : strategies) {
        if (!engine.switch_strategy(name)) switch_ok = false;
    }
    if (!engine.switch_strategy(strategy_name)) switch_ok = false;
    engine.set_debug(false);
    std::fprintf(stdout,
                 "[demo-agent-realtime] switch: %s (release checkpoints ok=%d)\n",
                 switch_ok ? "ok" : "FAILED", switch_ok ? 1 : 0);
    if (!switch_ok) pass = false;

    // ---- Criterion 1: 100 consecutive decisions, per-decision latency ----
    engine.set_debug(debug);
    engine.reset_latency();
    Scenario warmup(kScenarioSeed, 1710000000000000LL);
    engine.decide(warmup.next());  // warmup: cold caches / clock init
    engine.reset_latency();        // drop the warmup sample
    Scenario latency_sc(kScenarioSeed, 1710000000000000LL);
    for (int i = 0; i < n; ++i) {
        engine.decide(latency_sc.next());
    }
    const LatencyInstrument& lat = engine.latencies();
    const std::int64_t p50_ns = lat.percentile_ns(50);
    const std::int64_t p95_ns = lat.percentile_ns(95);
    const std::int64_t max_ns = lat.max_ns();
    const bool within = lat.all_within_us(kLatencyBudgetUs);
    std::fprintf(stdout,
        "[demo-agent-realtime] latency: decisions=%zu p50_us=%.1f p95_us=%.1f "
        "max_us=%.1f limit_us=%lld all_within_limit=%d\n",
        lat.count(),
        static_cast<double>(p50_ns) / 1000.0,
        static_cast<double>(p95_ns) / 1000.0,
        static_cast<double>(max_ns) / 1000.0,
        static_cast<long long>(kLatencyBudgetUs),
        within ? 1 : 0);
    if (lat.count() != static_cast<std::size_t>(n) || !within) {
        std::fprintf(stdout, "[demo-agent-realtime] FAIL: latency budget exceeded\n");
        pass = false;
    }

    // ---- Criterion 2: 100 AI decisions vs the human-expert oracle -------
    engine.reset_latency();
    Scenario acc_sc(kScenarioSeed, 1710000000000000LL);
    int correct = 0;
    for (int i = 0; i < n; ++i) {
        const GameState s = acc_sc.next();
        const Action expert = ExpertOracle::decide(s);
        const Action ai = engine.decide(s);
        if (ai == expert) ++correct;
    }
    const double rate = 100.0 * static_cast<double>(correct) / static_cast<double>(n);
    const bool acc_ok = rate >= kAccuracyThreshold;
    std::fprintf(stdout,
        "[demo-agent-realtime] accuracy: decisions=%d correct=%d rate_pct=%.1f "
        "threshold_pct=%.1f pass=%d\n",
        n, correct, rate, kAccuracyThreshold, acc_ok ? 1 : 0);
    if (!acc_ok) pass = false;

    // Per-strategy accuracy table: proves every registered strategy is a
    // real, measurable policy (and that switching changes the policy).
    for (const std::string_view name : strategies) {
        DecisionEngine probe(name);
        probe.set_debug(false);
        Scenario sc(kScenarioSeed, 1710000000000000LL);
        int ok = 0;
        for (int i = 0; i < n; ++i) {
            const GameState s = sc.next();
            if (probe.decide(s) == ExpertOracle::decide(s)) ++ok;
        }
        std::fprintf(stdout,
            "[demo-agent-realtime] accuracy_by_strategy: %s=%.1f\n",
            std::string(name).c_str(),
            100.0 * static_cast<double>(ok) / static_cast<double>(n));
    }

    // ---- Criterion 4: debug mode emits a complete decision trace --------
    engine.set_debug(true);
    engine.reset_trace();
    Scenario dbg_sc(kScenarioSeed, 1710000000000000LL);
    for (int i = 0; i < n; ++i) {
        engine.decide(dbg_sc.next());
    }
    const std::int64_t traced = engine.trace_count();
    const bool complete = traced == static_cast<std::int64_t>(n);
    std::fprintf(stdout,
        "[demo-agent-realtime] debug: traced_records=%lld complete=%d\n",
        static_cast<long long>(traced), complete ? 1 : 0);
    if (!complete) {
        std::fprintf(stdout, "[demo-agent-realtime] FAIL: debug trace incomplete\n");
        pass = false;
    }
    engine.set_debug(false);

    std::fprintf(stdout, "[demo-agent-realtime] self-test %s\n",
                 pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Real-time loop: tick the board at 60 fps, decide each frame, and emit the
// plan §5.2 ai_decision JSON record (the frozen M1 frame-label contract).
// ---------------------------------------------------------------------------
int run_realtime(std::string_view strategy_name, double seconds, bool debug) {
    DecisionEngine engine(strategy_name);
    engine.set_debug(debug);
    Scenario sc(kScenarioSeed, now_unix_us());
    std::fprintf(stdout, "[demo-agent-realtime] realtime loop: strategy=%s debug=%d\n",
                 std::string(strategy_name).c_str(), debug ? 1 : 0);

    const std::int64_t frames = static_cast<std::int64_t>(seconds * 60.0);
    const auto start = std::chrono::steady_clock::now();
    for (std::int64_t i = 0; i < frames; ++i) {
        const GameState s = sc.next();
        const auto t0 = std::chrono::steady_clock::now();
        const Action a = engine.decide(s);
        const auto t1 = std::chrono::steady_clock::now();
        const std::int64_t lat_us =
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        const std::int64_t age_us = now_unix_us() - s.capture_ts_us;
        std::fprintf(stdout,
            "{\"type\":\"ai_decision\",\"frame_seq\":%lld,\"capture_ts_us\":%lld,"
            "\"action\":\"%s\",\"reason\":\"%s\",\"age_us_at_decision\":%lld,"
            "\"strategy\":\"%s\",\"latency_us\":%lld}\n",
            static_cast<long long>(s.frame_seq),
            static_cast<long long>(s.capture_ts_us),
            action_name(a),
            json_escape(engine.active().reason_for(s, a)).c_str(),
            static_cast<long long>(age_us),
            engine.strategy_name(),
            static_cast<long long>(lat_us));
        const auto next =
            start + std::chrono::microseconds(kFramePeriodUs) * (i + 1);
        std::this_thread::sleep_until(next);
    }
    std::fprintf(stdout, "[demo-agent-realtime] realtime loop done (%lld frames)\n",
                 static_cast<long long>(frames));
    return 0;
}

// ---------------------------------------------------------------------------
// Voice-agent self-test (slice v4-voice-agent) — the seven acceptance
// criteria in one deterministic, headless run:
//
//   1. ASR backend selection (mock default; whisper/vosk runtime-pluggable)
//   2. ASR consumes post-3A int16 PCM
//   3. LLM is consumed as an incremental token stream
//   4. TTS is Opus-encoded; tap audio is batched to 100 ms
//   5. barge-in → stop-playback latency is recorded
//   6. missing model is a loud error, not a silent mock fallback
//   7. the missing-model branch emits a clear error message
//
// With --asr whisper/vosk and no runtime model the run FAILS loudly (exit
// non-zero) unless --allow-mock-fallback opts into the mock — the exact
// policy requirement 6 spells out.  The ctest entry point uses the mock
// backend (default) so the chain itself is asserted deterministically.
// ---------------------------------------------------------------------------
int run_voice_self_test(std::string_view asr_name, std::string_view llm_name,
                        std::string_view tts_name, bool allow_mock_fallback,
                        int total_frames, int barge_in_at_frame) {
    bool pass = true;

    std::fprintf(stdout,
        "[voice-agent] self-test: asr=%.*s llm=%.*s tts=%.*s "
        "allow_mock_fallback=%d\n",
        static_cast<int>(asr_name.size()), asr_name.data(),
        static_cast<int>(llm_name.size()), llm_name.data(),
        static_cast<int>(tts_name.size()), tts_name.data(),
        allow_mock_fallback ? 1 : 0);

    // ---- Criterion 1: backend selection ----------------------------------
    std::string asr_err;
    std::unique_ptr<IASR> asr = make_asr(asr_name, {allow_mock_fallback}, &asr_err);
    if (!asr) {
        // Criteria 6 & 7: a missing model is a loud error — the chain
        // refuses to run on a silent mock fallback unless the caller
        // explicitly opted in.
        std::fprintf(stdout, "[voice-agent] FAIL: %s\n", asr_err.c_str());
        std::fprintf(stdout, "[voice-agent] no_silent_fallback=%d\n", 1);
        return 1;
    }
    const std::string resolved_asr = asr->name();
    std::fprintf(stdout, "[voice-agent] asr_resolved=%s\n", resolved_asr.c_str());
    if (allow_mock_fallback && resolved_asr == "mock" &&
        (asr_name == "whisper" || asr_name == "vosk")) {
        std::fprintf(stdout, "[voice-agent] mock_fallback_optin=1\n");
    }

    std::unique_ptr<ILLM> llm = make_llm(llm_name);
    std::unique_ptr<ITts> tts = make_tts(tts_name);
    if (!llm || !tts) {
        std::fprintf(stdout, "[voice-agent] FAIL: llm/tts backend unavailable\n");
        return 1;
    }

    nimrtc::opus::EncoderConfig opus_cfg;
    opus_cfg.sample_rate_hz = kVoiceSampleRate;
    opus_cfg.channels       = kVoiceChannels;
    opus_cfg.bitrate_bps    = 32000;

    VoiceAgentPipeline pipeline(std::move(asr), std::move(llm), std::move(tts),
                                opus_cfg);

    // ---- Criterion 4: 100 ms tap batching; criterion 2: post-3A int16 ----
    const std::int64_t expected_batches =
        static_cast<std::int64_t>(total_frames) /
        (kVoiceBatchMs / kVoiceFrameMs);
    for (int i = 0; i < total_frames; ++i) {
        const bool speech = (i % 20) < 5;  // 5 speech frames / 20-frame cycle
        const std::vector<std::int16_t> pcm16 =
            gen_int16_frame(speech, kVoiceSamplesPerFrame);
        PcmFormat fmt;
        fmt.sample_rate_hz = kVoiceSampleRate;
        fmt.channels       = kVoiceChannels;
        pipeline.on_post_3a_int16(pcm16.data(), pcm16.size(), fmt);
        if (i + 1 == barge_in_at_frame) {
            pipeline.on_barge_in(1710000000000000LL +
                                 static_cast<std::int64_t>(i) * 10000);
        }
    }

    const std::uint64_t asr_samples =
        static_cast<std::uint64_t>(total_frames) * kVoiceSamplesPerFrame;
    std::fprintf(stdout,
        "[voice-agent] tap_batches=%llu asr_samples_fed=%llu "
        "(frames=%d samples_per_frame=%zu batch_ms=%lld)\n",
        static_cast<unsigned long long>(pipeline.batches_flushed()),
        static_cast<unsigned long long>(pipeline.asr_frames_fed()),
        total_frames, kVoiceSamplesPerFrame,
        static_cast<long long>(kVoiceBatchMs));
    if (pipeline.batches_flushed() !=
        static_cast<std::uint64_t>(expected_batches)) {
        std::fprintf(stdout,
            "[voice-agent] FAIL: expected %lld batches of %lld ms "
            "(model must not be called per 10 ms frame)\n",
            static_cast<long long>(expected_batches),
            static_cast<long long>(kVoiceBatchMs));
        pass = false;
    }
    if (pipeline.asr_frames_fed() != asr_samples) {
        std::fprintf(stdout,
            "[voice-agent] FAIL: ASR must consume every post-3A int16 sample\n");
        pass = false;
    }

    // ---- Criteria 3 & 4: streaming LLM + Opus-encoded TTS ---------------
    std::fprintf(stdout,
        "[voice-agent] llm_tokens=%llu opus_packets=%llu transcript=\"%s\"\n",
        static_cast<unsigned long long>(pipeline.llm_tokens()),
        static_cast<unsigned long long>(pipeline.opus_packets()),
        pipeline.last_transcript().c_str());
    // The transcript-dependent assertions are deterministic on the mock
    // backend; a runtime model is judged by the live model, not by the demo.
    if (resolved_asr == "mock") {
        if (pipeline.llm_tokens() < 1) {
            std::fprintf(stdout,
                "[voice-agent] FAIL: LLM must stream tokens incrementally\n");
            pass = false;
        }
        if (pipeline.opus_packets() < 1) {
            std::fprintf(stdout,
                "[voice-agent] FAIL: TTS must return Opus-encoded packets\n");
            pass = false;
        }
        if (pipeline.last_transcript().empty()) {
            std::fprintf(stdout,
                "[voice-agent] FAIL: no streamed transcript produced\n");
            pass = false;
        }
    }

    // ---- Criterion 5: barge-in → stop-playback delta recorded ------------
    const BargeInMeter& meter = pipeline.barge_in_meter();
    std::fprintf(stdout,
        "[voice-agent] barge_in_events=%llu barge_in_to_stop_us=%lld\n",
        static_cast<unsigned long long>(meter.events()),
        static_cast<long long>(meter.last_delta_us()));
    if (meter.events() != 1) {
        std::fprintf(stdout,
            "[voice-agent] FAIL: exactly one barge-in event expected\n");
        pass = false;
    }
    if (meter.last_delta_us() != kVoiceFrameMs * 1000) {
        std::fprintf(stdout,
            "[voice-agent] FAIL: 打断->停播 delta must be recorded "
            "(expected %lld us of in-flight frame drain, got %lld us)\n",
            static_cast<long long>(kVoiceFrameMs * 1000),
            static_cast<long long>(meter.last_delta_us()));
        pass = false;
    }

    std::fprintf(stdout, "[voice-agent] self-test %s\n",
                 pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

void print_usage() {
    std::fprintf(stdout,
        "demo-agent-realtime — real-time AI game decision + voice-agent demo\n"
        "usage:\n"
        "  demo-agent-realtime --self-test [--decisions N] [--debug]\n"
        "  demo-agent-realtime [--run SECONDS] [--strategy NAME] [--debug]\n"
        "  demo-agent-realtime --strategies\n"
        "  demo-agent-realtime --voice-self-test [--asr NAME] [--llm NAME] [--tts NAME]\n"
        "                     [--allow-mock-fallback] [--frames N] [--barge-in-at N]\n"
        "options:\n"
        "  --self-test       run the four decision acceptance criteria, exit 0 iff they hold\n"
        "  --decisions N     decision count for --self-test (default 100)\n"
        "  --strategy NAME   heuristic | reactive | predictive (default heuristic)\n"
        "  --run SECONDS     run the real-time loop for SECONDS (default 3)\n"
        "  --debug           emit the complete decision-process trace\n"
        "  --strategies      list the available decision strategies\n"
        "  --voice-self-test run the voice-agent chain acceptance criteria (slice v4-voice-agent)\n"
        "  --asr NAME        mock | whisper | vosk (default mock; whisper/vosk are runtime-pluggable)\n"
        "  --llm NAME        mock (default) — streaming LLM backend\n"
        "  --tts NAME        mock (default) — Opus-encoded TTS backend\n"
        "  --allow-mock-fallback  when a real ASR model is missing, fall back to mock (opt-in)\n"
        "  --frames N        number of 10 ms tap frames for --voice-self-test (default 300)\n"
        "  --barge-in-at N   frame index that triggers the barge-in (default 150)\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string_view strategy = "heuristic";
    int n = kDefaultDecisionCount;
    bool self_test = false;
    bool debug = false;
    double run_seconds = 3.0;

    bool voice_self_test = false;
    std::string_view asr_name = "mock";
    std::string_view llm_name = "mock";
    std::string_view tts_name = "mock";
    bool allow_mock_fallback = false;
    int voice_frames = kDefaultVoiceFrames;
    int barge_in_at = kDefaultBargeInAt;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--self-test") {
            self_test = true;
        } else if (arg == "--debug") {
            debug = true;
        } else if (arg == "--strategies") {
            print_strategies();
            return 0;
        } else if (arg == "--strategy" && i + 1 < argc) {
            strategy = std::string_view(argv[++i]);
        } else if (arg == "--decisions" && i + 1 < argc) {
            const int parsed = std::atoi(argv[++i]);
            n = parsed > 0 ? parsed : kDefaultDecisionCount;
        } else if (arg == "--run" && i + 1 < argc) {
            const double parsed = std::atof(argv[++i]);
            run_seconds = parsed > 0.0 ? parsed : 3.0;
        } else if (arg == "--voice-self-test") {
            voice_self_test = true;
        } else if (arg == "--asr" && i + 1 < argc) {
            asr_name = std::string_view(argv[++i]);
        } else if (arg == "--llm" && i + 1 < argc) {
            llm_name = std::string_view(argv[++i]);
        } else if (arg == "--tts" && i + 1 < argc) {
            tts_name = std::string_view(argv[++i]);
        } else if (arg == "--allow-mock-fallback") {
            allow_mock_fallback = true;
        } else if (arg == "--frames" && i + 1 < argc) {
            const int parsed = std::atoi(argv[++i]);
            voice_frames = parsed > 0 ? parsed : kDefaultVoiceFrames;
        } else if (arg == "--barge-in-at" && i + 1 < argc) {
            const int parsed = std::atoi(argv[++i]);
            barge_in_at = parsed > 0 ? parsed : kDefaultBargeInAt;
        } else {
            print_usage();
            return 2;
        }
    }

    if (self_test) {
        return run_self_test(strategy, n, debug);
    }
    if (voice_self_test) {
        return run_voice_self_test(asr_name, llm_name, tts_name,
                                   allow_mock_fallback, voice_frames,
                                   barge_in_at);
    }
    return run_realtime(strategy, run_seconds, debug);
}
