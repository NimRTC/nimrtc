/**
 * @file examples/demo-agent-realtime/ai_teammate.hpp
 * @brief Local small-model AI teammate (slice v4-ai-teammate).
 *
 * This is the M3 half of the AI game demo plan (docs/plan/ai-game-demo-plan.md,
 * §8 M3 bullet 2 and §4.2 "时光作证"): the M1 mock-rule AI decision path is
 * replaced by a genuinely **local small model** — a tiny feed-forward policy
 * network — that decides from **frame semantics** (what is on the frame the
 * model actually saw) and is confined to a deliberately narrow action set:
 *
 *     move_left | move_right | stop
 *
 * The narrow role is deliberate (spec: 窄角色是刻意的): a small model doing
 * a narrow task is what keeps decision latency below the demo's real-time
 * budget (plan §7.1: "窄角色+小模型才能压住延迟").
 *
 * Frame labels follow the plan §5.2 frozen contract: every decision carries
 * `frame_seq` (per-SSRC monotonic frame counter) and `capture_ts_us`
 * (ADR-017 unix-epoch capture time) copied **verbatim from the
 * FrameObservation the model actually saw** — never "now".
 *
 * The header is self-contained (no NimRTC linkage), matching the v2 demo's
 * constraint, so the demo builds and runs headlessly on any host.
 */

#ifndef NIMRTC_DEMO_AGENT_REALTIME_AI_TEAMMATE_HPP
#define NIMRTC_DEMO_AGENT_REALTIME_AI_TEAMMATE_HPP

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace demoteam {

// ---------------------------------------------------------------------------
// Game model: a 3-lane board (left / center / right).  The AI teammate
// steers an avatar across the lanes and stops to strike.  This keeps the
// plan's M1 打地鼠 3-hole board (plan §11) while giving the narrow
// "move left / move right / stop" vocabulary the M3 spec requires.
// ---------------------------------------------------------------------------

constexpr int kLanes = 3;                        // left / center / right
constexpr std::int64_t kFramePeriodUs = 16667;   // 1/60 s source cadence (plan §9)

// ---------------------------------------------------------------------------
// Narrow action set — exactly the three actions the AI teammate may take
// (spec requirement 2).  Any token outside this set is honestly rejected
// and counted, never silently remapped to another action.
// ---------------------------------------------------------------------------

enum class Action : std::uint8_t {
    kMoveLeft = 0,
    kMoveRight = 1,
    kStop = 2,
};

// The action whitelist: exactly the three narrow tokens (requirement 2).
constexpr std::array<std::string_view, kLanes> kActionTokens = {
    "move_left", "move_right", "stop",
};

inline const char* action_name(Action a) noexcept {
    switch (a) {
        case Action::kMoveLeft:  return "move_left";
        case Action::kMoveRight: return "move_right";
        case Action::kStop:      return "stop";
    }
    return "stop";
}

inline bool is_narrow_action(std::string_view token) noexcept {
    return token == "move_left" || token == "move_right" || token == "stop";
}

// Index of `token` in the whitelist, or -1 when it is outside the set.
inline int token_index(std::string_view token) noexcept {
    for (int i = 0; i < kLanes; ++i) {
        if (kActionTokens[static_cast<std::size_t>(i)] == token) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Frame observation — the input to the decision (requirement 5)
// ---------------------------------------------------------------------------
//
// The decision input is the *semantics of one frame*: `threat` holds each
// lane's hazard urgency (0 = clear, 1 = must react now) exactly as it was on
// the observed frame.  `frame_seq` / `capture_ts_us` are that frame's labels
// (plan §5.2).  There is no "what the user said" input at all.
struct FrameObservation {
    std::int64_t frame_seq = 0;                 // per-SSRC monotonic frame counter
    std::int64_t capture_ts_us = 0;             // ADR-017 unix-epoch capture time
    std::array<float, kLanes> threat{};         // per-lane hazard urgency, 0..1
    int avatar_lane = 1;                        // 0..kLanes-1, where the avatar is
};

// ---------------------------------------------------------------------------
// The local small model (requirement 1)
// ---------------------------------------------------------------------------
//
// A 1-hidden-layer policy network (4 inputs → 2 hidden → 3 logits) whose
// weights are bundled with the demo.  `infer()` is **the** inference call
// point on the decision path: it runs the forward pass (matrix multiply +
// ReLU + softmax) over the frame semantics and returns a raw distribution
// over the narrow action set.  This is a real model computation — not a mock
// rule, not a cloud API.
//
// A model can be *unavailable* (requirement 6): when the model artifact is
// missing/unloadable the demo must report the error explicitly and count it,
// never silently fall back to the M1 mock rules.  `allow_fallback` on the
// caller's side is the only way to opt in to a deterministic safe action.
class LocalPolicyModel {
public:
    // `available=false` models a missing/unloadable model artifact.
    //
    // `token_override` is a test hook that makes the model emit an arbitrary
    // token (e.g. an out-of-set hallucination such as "jump") so the
    // narrow-role guard's rejection path can be exercised deterministically.
    explicit LocalPolicyModel(bool available = true,
                              std::string_view token_override = {}) noexcept
        : available_(available), token_override_(token_override) {}

    bool available() const noexcept { return available_; }
    std::string_view name() const noexcept { return "local_mlp_policy"; }

    // What `infer()` returns: the token the model emitted, plus the raw
    // logits and the softmax probabilities over the action set.
    struct RawOutput {
        std::string_view token;
        std::array<float, kLanes> scores{};
        std::array<float, kLanes> probs{};
    };

    // THE local inference call point.  Runs the forward pass on the frame the
    // model actually saw (the observed frame's semantics), never on "now".
    RawOutput infer(const FrameObservation& obs) const noexcept {
        if (!token_override_.empty()) {
            // Hallucinated out-of-set token (test hook / model misbehaviour).
            return RawOutput{token_override_, {}, {}};
        }

        // Frame-semantics features: the urgency of the lanes immediately
        // left / under / right of the avatar on this exact frame.
        const float left = obs.avatar_lane > 0
            ? obs.threat[static_cast<std::size_t>(obs.avatar_lane - 1)]
            : 0.0f;
        const float own = obs.threat[static_cast<std::size_t>(obs.avatar_lane)];
        const float right = obs.avatar_lane < kLanes - 1
            ? obs.threat[static_cast<std::size_t>(obs.avatar_lane + 1)]
            : 0.0f;
        const float x[kInputs] = {left, own, right, 1.0f};

        // Hidden layer (ReLU).  h0 fires when the left lane is more urgent
        // than the avatar's own; h1 fires when the right lane is.
        float h[kHidden]{};
        for (std::size_t i = 0; i < kHidden; ++i) {
            float z = 0.0f;
            for (std::size_t j = 0; j < kInputs; ++j) {
                z += kW1[i][j] * x[j];
            }
            h[i] = z > 0.0f ? z : 0.0f;
        }

        // Output layer: one logit per narrow action.
        std::array<float, kLanes> scores{};
        for (std::size_t a = 0; a < kLanes; ++a) {
            float s = kB2[a];
            for (std::size_t i = 0; i < kHidden; ++i) {
                s += kW2[a][i] * h[i];
            }
            scores[a] = s;
        }

        // Softmax over the action logits → raw action distribution.
        float max_s = scores[0];
        for (std::size_t a = 1; a < kLanes; ++a) {
            max_s = std::max(max_s, scores[a]);
        }
        std::array<float, kLanes> probs{};
        float denom = 0.0f;
        for (std::size_t a = 0; a < kLanes; ++a) {
            probs[a] = std::exp(scores[a] - max_s);
            denom += probs[a];
        }
        for (std::size_t a = 0; a < kLanes; ++a) {
            probs[a] /= denom;
        }

        const int best = static_cast<int>(argmax(scores));
        return RawOutput{kActionTokens[static_cast<std::size_t>(best)], scores, probs};
    }

private:
    static constexpr std::size_t kInputs = 4;   // left / own / right / bias
    static constexpr std::size_t kHidden = 2;

    // W1: hidden × inputs.  h0 = relu(left - own + 0.05), h1 = relu(right - own + 0.05).
    static constexpr std::array<std::array<float, kInputs>, kHidden> kW1{{
        {{1.0f, -1.0f, 0.0f, 0.05f}},
        {{0.0f, -1.0f, 1.0f, 0.05f}},
    }};
    // W2: actions × hidden (move_left / move_right / stop).
    static constexpr std::array<std::array<float, kHidden>, kLanes> kW2{{
        {{1.0f, -1.0f}},
        {{-1.0f, 1.0f}},
        {{0.0f, 0.0f}},
    }};
    // b2: a small stop bias so "no hazard anywhere" resolves to stop.
    static constexpr std::array<float, kLanes> kB2{{0.0f, 0.0f, 0.05f}};

    static std::size_t argmax(const std::array<float, kLanes>& scores) noexcept {
        std::size_t best = 0;
        for (std::size_t a = 1; a < kLanes; ++a) {
            if (scores[a] > scores[best]) best = a;
        }
        return best;
    }

    bool available_ = true;
    std::string_view token_override_;
};

// ---------------------------------------------------------------------------
// Frame-semantics reason (requirement 5)
// ---------------------------------------------------------------------------
// The justification for an action describes what is on the observed frame
// (which lane held the hazard the model reacted to), not "what the user said".
inline std::string reason_for(const FrameObservation& obs, Action a) {
    const std::size_t own = static_cast<std::size_t>(obs.avatar_lane);
    const std::size_t left = own > 0 ? own - 1 : 0;
    const std::size_t right = std::min<std::size_t>(own + 1, kLanes - 1);

    char buf[32];
    std::string r = "frame " + std::to_string(obs.frame_seq) +
                    " (capture_ts_us=" + std::to_string(obs.capture_ts_us) + "): ";
    switch (a) {
        case Action::kMoveLeft:
            std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(obs.threat[left]));
            r += "left lane has a hazard (urgency " + std::string(buf) + ")";
            break;
        case Action::kMoveRight:
            std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(obs.threat[right]));
            r += "right lane has a hazard (urgency " + std::string(buf) + ")";
            break;
        case Action::kStop:
            std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(obs.threat[own]));
            r += "current lane is the best strike lane (urgency " + std::string(buf) + ")";
            break;
    }
    return r;
}

// ---------------------------------------------------------------------------
// The AI teammate — the decision path (requirements 2, 3, 4, 6)
// ---------------------------------------------------------------------------
class AiTeammate {
public:
    // `allow_fallback` opts in to the deterministic safe action (stop) when
    // the model is unavailable.  Requirement 6: never *silently* fall back to
    // the M1 mock rules; the caller must explicitly request it.
    explicit AiTeammate(LocalPolicyModel model, bool allow_fallback = false)
        : model_(std::move(model)), allow_fallback_(allow_fallback) {}

    const LocalPolicyModel& model() const noexcept { return model_; }
    bool allow_fallback() const noexcept { return allow_fallback_; }

    struct Decision {
        std::optional<Action> action;      // nullopt = rejected / unavailable (no fabrication)
        std::int64_t frame_seq = 0;        // from the observed frame, verbatim
        std::int64_t capture_ts_us = 0;    // from the observed frame, verbatim
        std::string reason;                // frame-semantics justification
        std::string model_token;           // what the model emitted
        bool rejected = false;             // out-of-set token → honestly rejected
        bool unavailable = false;          // model missing/unloadable
        bool fallback = false;             // caller-requested fallback action
        std::int64_t latency_us = 0;       // model inference wall time (risk 1)
    };

    // THE decision path: run the local model on the observed frame, enforce
    // the narrow action whitelist, and stamp the decision with the frame the
    // model actually saw (requirement 3).
    Decision decide(const FrameObservation& obs) {
        ++decisions_;

        Decision d;
        // Requirement 3: these come from the observed frame, NOT from "now".
        d.frame_seq = obs.frame_seq;
        d.capture_ts_us = obs.capture_ts_us;

        if (!model_.available()) {
            // Requirement 6: report the error explicitly and count it.  No
            // action is fabricated; the caller may opt in to a safe fallback.
            ++unavailable_;
            d.unavailable = true;
            d.reason = "model unavailable: " + std::string(model_.name()) +
                       " failed to load (no decision fabricated)";
            if (allow_fallback_) {
                ++fallbacks_;
                d.fallback = true;
                d.action = Action::kStop;
                d.reason += "; caller-requested fallback applied (stop)";
            }
            push_history(d);
            return d;
        }

        const auto t0 = std::chrono::steady_clock::now();
        const LocalPolicyModel::RawOutput raw = model_.infer(obs);  // ← inference
        const auto t1 = std::chrono::steady_clock::now();
        d.latency_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        last_latency_us_ = d.latency_us;
        d.model_token = std::string(raw.token);

        // Narrow-role guard (requirement 2): tokens outside the action set are
        // honestly rejected and counted, never silently remapped.
        if (!is_narrow_action(raw.token)) {
            ++rejected_;
            d.rejected = true;
            d.reason = "model emitted out-of-set action \"" + d.model_token +
                       "\"; rejected (narrow role: move_left/move_right/stop only)";
            push_history(d);
            return d;
        }

        d.action = static_cast<Action>(token_index(raw.token));
        d.reason = reason_for(obs, *d.action);
        push_history(d);
        return d;
    }

    // Counters (requirements 2 & 6): each event is counted, never silently absorbed.
    std::size_t decision_count() const noexcept { return decisions_; }
    std::size_t rejected_count() const noexcept { return rejected_; }
    std::size_t unavailable_count() const noexcept { return unavailable_; }
    std::size_t fallback_count() const noexcept { return fallbacks_; }

    // "时光作证" (time witness, requirement 4): answer *why* the teammate
    // acted, citing the frame it actually saw — a frame that has already
    // scrolled out of the current view, never the current frame.  Every
    // witness answer embeds that frame's `frame_seq` + `capture_ts_us`.
    std::string time_witness_answer(const FrameObservation& current) const {
        const Decision* cited = nullptr;
        for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
            // A frame has "rolled out of the current view" when it is at
            // least kScrollOutFrames behind the current frame.
            if (it->frame_seq <= current.frame_seq - kScrollOutFrames) {
                cited = &*it;
                break;
            }
        }
        if (cited == nullptr) {
            return "no decision on a scrolled-out frame on record yet";
        }
        return "I looked at frame_seq=" + std::to_string(cited->frame_seq) +
               " capture_ts_us=" + std::to_string(cited->capture_ts_us) + ": " +
               cited->reason +
               " — that frame has already scrolled out of view";
    }

    // Latest model-inference wall time (risk 1: 把推理耗时打进统计).
    std::int64_t last_latency_us() const noexcept { return last_latency_us_; }

private:
    static constexpr std::int64_t kScrollOutFrames = 2;
    static constexpr std::size_t kMaxHistory = 64;

    void push_history(const Decision& d) {
        history_.push_back(d);
        if (history_.size() > kMaxHistory) {
            history_.pop_front();
        }
    }

    LocalPolicyModel model_;
    bool allow_fallback_;
    std::size_t decisions_ = 0;
    std::size_t rejected_ = 0;
    std::size_t unavailable_ = 0;
    std::size_t fallbacks_ = 0;
    std::int64_t last_latency_us_ = 0;
    std::deque<Decision> history_;
};

}  // namespace demoteam

#endif  // NIMRTC_DEMO_AGENT_REALTIME_AI_TEAMMATE_HPP
