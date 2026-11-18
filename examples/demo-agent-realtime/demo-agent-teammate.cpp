/**
 * @file examples/demo-agent-realtime/demo-agent-teammate.cpp
 * @brief Local small-model AI teammate demo (slice v4-ai-teammate).
 *
 * ## What this validates (the five acceptance criteria of the spec)
 *
 *   1. A local-model inference call point on the decision path — the
 *      self-test runs 100 decisions through `LocalPolicyModel::infer()` and
 *      reports the inference latency (risk 1: 把推理耗时打进统计).
 *   2. The action set is exactly {move_left, move_right, stop}; out-of-set
 *      model outputs are honestly rejected and counted, never silently
 *      remapped.
 *   3. Every decision carries `frame_seq` + `capture_ts_us` copied verbatim
 *      from the frame the model actually saw — the self-test asserts the
 *      decision anchors equal the observed frame's, not the current time.
 *   4. "时光作证" answers 100% embed `frame_seq` + `capture_ts_us` and cite a
 *      frame that has already scrolled out of the current view (strictly
 *      older than the current frame).
 *   5. A missing/unloadable model reports the error explicitly and counts it;
 *      the only way to get an action is the caller's explicit `allow_fallback`
 *      opt-in (never a silent mock-rule fallback).
 *
 * ## Usage
 *
 *   demo-agent-teammate --self-test
 *   demo-agent-teammate [--run SECONDS] [--unavailable] [--fallback] [--debug]
 *
 * The self-test mode is the ctest entry point: deterministic, headless,
 * exits 0 only when every criterion holds.
 */

#include "ai_teammate.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace demoteam;

constexpr std::uint64_t kScenarioSeed = 0xA17EA71ULL;  // "A-Team" M1
constexpr std::int64_t kObservedTsUs = 1710000000000000LL;  // fixed, never "now"

std::int64_t now_unix_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
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

// ---------------------------------------------------------------------------
// Deterministic 3-lane hazard scenario.
// ---------------------------------------------------------------------------
// A fixed seed keeps the self-test reproducible on every host.  Each frame
// advances `frame_seq` by one and `capture_ts_us` by one 60fps frame period,
// matching the plan's time-anchored decision model (plan §5.2).
class LaneScenario {
public:
    LaneScenario(std::uint64_t seed, std::int64_t start_ts_us)
        : rng_(static_cast<std::mt19937::result_type>(seed)), start_ts_us_(start_ts_us) {}

    FrameObservation next() {
        FrameObservation f;
        f.frame_seq = frame_;
        f.capture_ts_us = start_ts_us_ + frame_ * kFramePeriodUs;
        f.avatar_lane = avatar_;
        for (int c = 0; c < kLanes; ++c) {
            if (remaining_[c] > 0) {
                --remaining_[c];
                f.threat[c] = remaining_[c] > 0
                    ? static_cast<float>(remaining_[c]) / kMaxLifetime
                    : 0.0f;
            }
        }
        if (uniform_(rng_) < 0.25) {
            const int col = static_cast<int>(uniform_(rng_) * static_cast<double>(kLanes));
            if (remaining_[col] == 0) {
                remaining_[col] = 3 + static_cast<int>(uniform_(rng_) * 3.0);  // 3..5
                f.threat[col] = static_cast<float>(remaining_[col]) / kMaxLifetime;
            }
        }
        ++frame_;
        return f;
    }

    void apply(Action a) noexcept {
        if (a == Action::kMoveLeft && avatar_ > 0) {
            --avatar_;
        } else if (a == Action::kMoveRight && avatar_ < kLanes - 1) {
            ++avatar_;
        }
    }

private:
    static constexpr int kMaxLifetime = 5;

    std::mt19937 rng_;
    std::uniform_real_distribution<double> uniform_{0.0, 1.0};
    std::array<int, kLanes> remaining_{};
    std::int64_t frame_ = 0;
    std::int64_t start_ts_us_ = 0;
    int avatar_ = 1;
};

FrameObservation make_obs(std::int64_t frame_seq, std::int64_t ts_us,
                          float t0, float t1, float t2, int avatar = 1) {
    FrameObservation f;
    f.frame_seq = frame_seq;
    f.capture_ts_us = ts_us;
    f.threat[0] = t0;
    f.threat[1] = t1;
    f.threat[2] = t2;
    f.avatar_lane = avatar;
    return f;
}

// ---------------------------------------------------------------------------
// --self-test: the five acceptance criteria in one deterministic run.
// Exits 0 only when every criterion holds.
// ---------------------------------------------------------------------------
int run_self_test() {
    bool pass = true;
    std::fprintf(stdout, "[demo-agent-teammate] self-test\n");

    // ---- Criterion 1: local-model inference call point + latency -----------
    {
        LocalPolicyModel model(true);
        AiTeammate teammate(model, false);
        std::fprintf(stdout,
            "[teammate] model: name=%s available=%d inference_point=infer\n",
            std::string(model.name()).c_str(), model.available() ? 1 : 0);

        LaneScenario sc(kScenarioSeed, kObservedTsUs);
        std::vector<std::int64_t> lat_us;
        bool all_actions = true;
        const int n = 100;
        for (int i = 0; i < n; ++i) {
            const FrameObservation f = sc.next();
            const AiTeammate::Decision d = teammate.decide(f);
            if (d.action) {
                sc.apply(*d.action);
            } else {
                all_actions = false;
            }
            lat_us.push_back(d.latency_us);
        }
        std::sort(lat_us.begin(), lat_us.end());
        const std::int64_t p50 = lat_us.empty() ? 0 : lat_us[lat_us.size() / 2];
        const std::int64_t max = lat_us.empty() ? 0 : lat_us.back();
        std::fprintf(stdout,
            "[teammate] inference_latency_us: decisions=%zu p50_us=%lld max_us=%lld\n",
            lat_us.size(),
            static_cast<long long>(p50),
            static_cast<long long>(max));
        const bool c1 = lat_us.size() == static_cast<std::size_t>(n) &&
                        all_actions &&
                        teammate.rejected_count() == 0 &&
                        teammate.unavailable_count() == 0;
        std::fprintf(stdout, "[teammate] criterion1: all_actions=%d rejected=%zu unavailable=%zu pass=%d\n",
            all_actions ? 1 : 0, teammate.rejected_count(), teammate.unavailable_count(),
            c1 ? 1 : 0);
        if (!c1) pass = false;
    }

    // ---- Criterion 2: narrow action set + out-of-set rejection ------------
    {
        // A model that hallucinates out-of-set tokens ("jump", "attack").
        // available=true so the decision path runs inference, whose output
        // token is then rejected by the narrow-role guard.
        LocalPolicyModel noisy_jump(true, "jump");
        AiTeammate guard_jump(noisy_jump, false);
        const FrameObservation f = make_obs(7, kObservedTsUs, 0.5f, 0.0f, 0.0f);
        const AiTeammate::Decision d_jump = guard_jump.decide(f);
        const bool jump_ok = d_jump.rejected && !d_jump.action &&
                             guard_jump.rejected_count() == 1;

        LocalPolicyModel noisy_attack(true, "attack");
        AiTeammate guard_attack(noisy_attack, false);
        const AiTeammate::Decision d_attack = guard_attack.decide(f);
        const bool attack_ok = d_attack.rejected && !d_attack.action &&
                               guard_attack.rejected_count() == 1;

        const bool c2 = jump_ok && attack_ok;
        std::fprintf(stdout,
            "[teammate] whitelist: allowed=%d %s,%s,%s rejected=%d no_silent_substitute=%d pass=%d\n",
            kLanes,
            std::string(kActionTokens[0]).c_str(),
            std::string(kActionTokens[1]).c_str(),
            std::string(kActionTokens[2]).c_str(),
            2,
            (jump_ok && attack_ok) ? 1 : 0,
            c2 ? 1 : 0);
        if (!c2) pass = false;
    }

    // ---- Criterion 3: decision anchors come from the observed frame -------
    {
        LocalPolicyModel model(true);
        AiTeammate teammate(model, false);
        const FrameObservation obs = make_obs(42, kObservedTsUs, 0.0f, 0.0f, 0.9f);
        const AiTeammate::Decision d = teammate.decide(obs);
        const std::int64_t now_us = now_unix_us();
        const bool match = d.frame_seq == obs.frame_seq &&
                           d.capture_ts_us == obs.capture_ts_us;
        // "not_now": the decision's capture_ts is the observed frame's fixed
        // value, not the current wall time.
        const bool not_now = match && obs.capture_ts_us != now_us;
        std::fprintf(stdout,
            "[teammate] anchors: observed_frame=%lld observed_ts=%lld "
            "decision_frame=%lld decision_ts=%lld match=%d not_now=%d pass=%d\n",
            static_cast<long long>(obs.frame_seq),
            static_cast<long long>(obs.capture_ts_us),
            static_cast<long long>(d.frame_seq),
            static_cast<long long>(d.capture_ts_us),
            match ? 1 : 0,
            not_now ? 1 : 0,
            (match && not_now) ? 1 : 0);
        if (!match || !not_now) pass = false;
    }

    // ---- Criterion 4: "时光作证" cites a frame that already scrolled out --
    {
        const std::int64_t kRefFrame = 42;
        const std::int64_t kCurFrame = 50;
        LocalPolicyModel model(true);
        AiTeammate teammate(model, false);
        teammate.decide(make_obs(kRefFrame, kObservedTsUs, 0.0f, 0.0f, 0.9f));
        const FrameObservation current =
            make_obs(kCurFrame, kObservedTsUs + (kCurFrame - kRefFrame) * kFramePeriodUs,
                     0.0f, 0.0f, 0.0f);
        const std::string ans = teammate.time_witness_answer(current);
        const bool older = ans.find("frame_seq=" + std::to_string(kRefFrame)) != std::string::npos &&
                           kRefFrame < kCurFrame;
        const bool has_ts = ans.find("capture_ts_us=" + std::to_string(kObservedTsUs)) != std::string::npos;
        std::fprintf(stdout,
            "[teammate] witness: referenced_frame=%lld current_frame=%lld older=%d has_ts=%d pass=%d\n",
            static_cast<long long>(kRefFrame),
            static_cast<long long>(kCurFrame),
            older ? 1 : 0,
            has_ts ? 1 : 0,
            (older && has_ts) ? 1 : 0);
        std::fprintf(stdout, "[teammate] witness_answer: %s\n", json_escape(ans).c_str());
        if (!older || !has_ts) pass = false;
    }

    // ---- Criterion 5: model unavailable → error + count; opt-in fallback --
    {
        LocalPolicyModel missing(false);   // unloadable model artifact
        AiTeammate teammate(missing, false);
        const FrameObservation f = make_obs(7, kObservedTsUs, 0.5f, 0.0f, 0.0f);
        const AiTeammate::Decision d = teammate.decide(f);
        const bool no_action = d.unavailable && !d.action && !d.fallback &&
                               teammate.unavailable_count() == 1;
        const bool error_reported = d.reason.find("model unavailable") != std::string::npos;

        AiTeammate teammate_fb(missing, true);   // caller explicitly opts in
        const AiTeammate::Decision d_fb = teammate_fb.decide(f);
        const bool fallback_ok = d_fb.unavailable && d_fb.fallback &&
                                 d_fb.action == Action::kStop &&
                                 teammate_fb.fallback_count() == 1 &&
                                 teammate_fb.unavailable_count() == 1;

        const bool c5 = no_action && error_reported && fallback_ok;
        std::fprintf(stdout,
            "[teammate] unavailable: default_no_action=%d error_reported=%d "
            "unavailable_count=%zu fallback_opt_in=%s fallback_count=%zu pass=%d\n",
            no_action ? 1 : 0,
            error_reported ? 1 : 0,
            teammate.unavailable_count(),
            fallback_ok ? "stop" : "FAIL",
            teammate_fb.fallback_count(),
            c5 ? 1 : 0);
        if (!c5) pass = false;
    }

    std::fprintf(stdout, "[teammate] self-test %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Real-time loop: tick the lane board at 60 fps, decide each frame with the
// local model, and emit the plan §5.2 ai_decision JSON record.
// ---------------------------------------------------------------------------
int run_realtime(double seconds, bool model_unavailable, bool allow_fallback,
                 bool debug) {
    LocalPolicyModel model(!model_unavailable);
    AiTeammate teammate(model, allow_fallback);
    LaneScenario sc(kScenarioSeed, now_unix_us());
    std::fprintf(stdout,
        "[demo-agent-teammate] realtime loop: model=%s available=%d allow_fallback=%d\n",
        std::string(model.name()).c_str(), model.available() ? 1 : 0,
        allow_fallback ? 1 : 0);

    const std::int64_t frames = static_cast<std::int64_t>(seconds * 60.0);
    const auto start = std::chrono::steady_clock::now();
    for (std::int64_t i = 0; i < frames; ++i) {
        const FrameObservation f = sc.next();
        const AiTeammate::Decision d = teammate.decide(f);
        if (d.action) sc.apply(*d.action);
        const std::int64_t age_us = now_unix_us() - f.capture_ts_us;
        const char* action = "rejected";
        if (d.unavailable) {
            action = "unavailable";
        } else if (d.action) {
            action = action_name(*d.action);
        }
        std::fprintf(stdout,
            "{\"type\":\"ai_decision\",\"frame_seq\":%lld,\"capture_ts_us\":%lld,"
            "\"action\":\"%s\",\"reason\":\"%s\",\"age_us_at_decision\":%lld,"
            "\"model\":\"%s\",\"latency_us\":%lld}\n",
            static_cast<long long>(f.frame_seq),
            static_cast<long long>(f.capture_ts_us),
            action,
            json_escape(d.reason).c_str(),
            static_cast<long long>(age_us),
            std::string(teammate.model().name()).c_str(),
            static_cast<long long>(d.latency_us));
        if (debug) {
            std::fprintf(stdout,
                "[teammate] decision: seq=%lld model=%s action=%s rejected=%d unavailable=%d lat_us=%lld\n",
                static_cast<long long>(f.frame_seq),
                std::string(teammate.model().name()).c_str(),
                action,
                d.rejected ? 1 : 0,
                d.unavailable ? 1 : 0,
                static_cast<long long>(d.latency_us));
        }
        const auto next = start + std::chrono::microseconds(kFramePeriodUs) * (i + 1);
        std::this_thread::sleep_until(next);
    }
    std::fprintf(stdout,
        "[demo-agent-teammate] realtime loop done (%lld frames) rejected=%zu unavailable=%zu fallback=%zu\n",
        static_cast<long long>(frames),
        teammate.rejected_count(),
        teammate.unavailable_count(),
        teammate.fallback_count());
    return 0;
}

void print_usage() {
    std::fprintf(stdout,
        "demo-agent-teammate — local small-model AI teammate demo\n"
        "usage:\n"
        "  demo-agent-teammate --self-test\n"
        "  demo-agent-teammate [--run SECONDS] [--unavailable] [--fallback] [--debug]\n"
        "options:\n"
        "  --self-test       run the five acceptance criteria, exit 0 iff they hold\n"
        "  --run SECONDS     run the real-time decision loop for SECONDS (default 3)\n"
        "  --unavailable     force the local model to be unavailable (error + count)\n"
        "  --fallback        opt in to the deterministic safe action when unavailable\n"
        "  --debug           emit a per-decision trace\n");
}

}  // namespace

int main(int argc, char** argv) {
    bool self_test = false;
    bool model_unavailable = false;
    bool allow_fallback = false;
    bool debug = false;
    double run_seconds = 3.0;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--self-test") {
            self_test = true;
        } else if (arg == "--debug") {
            debug = true;
        } else if (arg == "--unavailable") {
            model_unavailable = true;
        } else if (arg == "--fallback") {
            allow_fallback = true;
        } else if (arg == "--run" && i + 1 < argc) {
            const double parsed = std::atof(argv[++i]);
            run_seconds = parsed > 0.0 ? parsed : 3.0;
        } else {
            print_usage();
            return 2;
        }
    }

    if (self_test) {
        return run_self_test();
    }
    return run_realtime(run_seconds, model_unavailable, allow_fallback, debug);
}
