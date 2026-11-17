/**
 * @file examples/demo-agent-realtime/ai_decision.hpp
 * @brief Real-time AI game decision engine for the demo-agent-realtime demo.
 *
 * This is the decision-processing half of the AI game demo plan
 * (docs/plan/ai-game-demo-plan.md, M1 bullet 1) for slice v2-ai-decision:
 *
 *   - a pluggable decision-strategy set with >= 3 strategies and runtime
 *     switching (acceptance criterion 3);
 *   - a per-decision latency instrument that records the wall time of every
 *     decision so a run of 100 consecutive decisions can prove the 50 ms
 *     budget (acceptance criterion 1);
 *   - an expert-oracle comparator that scores AI decisions against a
 *     deterministic "human expert" rule over 100 decisions (acceptance
 *     criterion 2);
 *   - a debug-mode decision trace that logs the full decision process
 *     (observed state, candidates, chosen action, reason, latency) in a
 *     way the host can grep (acceptance criterion 4).
 *
 * The engine is deliberately self-contained (no NimRTC linkage): the
 * decisions are plain C++20 arithmetic, so the demo builds and runs
 * headlessly on any host that builds the examples.
 *
 * The game model is the whack-a-mole board the plan chose for M1
 * (plan §11): a 3-hole board (left / center / right).  Each decision
 * point carries the frame anchors the plan's ai_decision JSON uses
 * (plan §5.2): a monotonic `frame_seq` and an ADR-017 unix-epoch
 * `capture_ts_us`.
 */

#ifndef NIMRTC_DEMO_AGENT_REALTIME_AI_DECISION_HPP
#define NIMRTC_DEMO_AGENT_REALTIME_AI_DECISION_HPP

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace demoai {

// ---------------------------------------------------------------------------
// Game model: a 3-hole whack-a-mole board (plan §11: M1 打地鼠).
// ---------------------------------------------------------------------------

constexpr int kHoles = 3;                    // left / center / right
constexpr std::int64_t kFramePeriodUs = 16667;  // 1/60 s source cadence (plan §9)

enum class Action : std::uint8_t {
    kWait = 0,
    kHitLeft = 1,
    kHitCenter = 2,
    kHitRight = 3,
};

inline const char* action_name(Action a) noexcept {
    switch (a) {
        case Action::kWait:      return "wait";
        case Action::kHitLeft:   return "hit_left";
        case Action::kHitCenter: return "hit_center";
        case Action::kHitRight:  return "hit_right";
    }
    return "wait";
}

inline int action_column(Action a) noexcept {
    switch (a) {
        case Action::kHitLeft:   return 0;
        case Action::kHitCenter: return 1;
        case Action::kHitRight:  return 2;
        case Action::kWait:      return -1;
    }
    return -1;
}

inline Action column_action(int col) noexcept {
    return static_cast<Action>(static_cast<int>(Action::kHitLeft) + col);
}

// A single board observation at one decision point.  `frame_seq` is the
// monotonic per-SSRC frame counter and `capture_ts_us` is the unix-epoch
// capture time of the frame the AI is reacting to (plan §5.2).
struct GameState {
    std::int64_t frame_seq = 0;
    std::int64_t capture_ts_us = 0;
    std::array<int, kHoles> remaining_ticks{};  // >0 means a mole in that hole
};

// ---------------------------------------------------------------------------
// Expert oracle: the "human expert" the AI is measured against
// ---------------------------------------------------------------------------
//
// The expert always strikes the most urgent mole, i.e. the one with the
// least remaining life.  This is the reference every AI decision is
// compared with (acceptance criterion 2: "对比100次AI决策与人工专家决策结果").
struct ExpertOracle {
    static Action decide(const GameState& s) noexcept {
        int best_col = -1;
        int best_rem = 1 << 30;
        for (int c = 0; c < kHoles; ++c) {
            if (s.remaining_ticks[c] > 0 && s.remaining_ticks[c] < best_rem) {
                best_rem = s.remaining_ticks[c];
                best_col = c;
            }
        }
        return best_col < 0 ? Action::kWait : column_action(best_col);
    }
};

// ---------------------------------------------------------------------------
// Decision strategies (acceptance criterion 3: >= 3 strategies)
// ---------------------------------------------------------------------------

class DecisionStrategy {
public:
    virtual ~DecisionStrategy() = default;

    virtual const char* name() const noexcept = 0;
    virtual Action decide(const GameState& s) const noexcept = 0;

    // Called by the engine before each decision so stateful strategies can
    // update their learned models.  Default: no-op.
    virtual void observe(const GameState&) noexcept {}

    // Human-readable reason for the chosen action (decision trace / JSON).
    virtual std::string reason_for(const GameState& s, Action a) const = 0;
};

// heuristic: mirror of the expert rule — strike the most urgent mole.
// Scores ~100% against the oracle.
class HeuristicStrategy final : public DecisionStrategy {
public:
    const char* name() const noexcept override { return "heuristic"; }

    Action decide(const GameState& s) const noexcept override {
        return ExpertOracle::decide(s);
    }

    std::string reason_for(const GameState& s, Action a) const override {
        if (a == Action::kWait) return "no mole visible";
        const int col = action_column(a);
        return "most urgent mole at hole " + std::to_string(col) +
               " (remaining=" + std::to_string(s.remaining_ticks[col]) + " ticks)";
    }
};

// reactive: the reflex rule — strike the first mole visible when scanning
// left to right.  Cheapest to compute; ignores urgency bookkeeping.
class ReactiveStrategy final : public DecisionStrategy {
public:
    const char* name() const noexcept override { return "reactive"; }

    Action decide(const GameState& s) const noexcept override {
        for (int c = 0; c < kHoles; ++c) {
            if (s.remaining_ticks[c] > 0) return column_action(c);
        }
        return Action::kWait;
    }

    std::string reason_for(const GameState&, Action a) const override {
        if (a == Action::kWait) return "no mole visible";
        return "first visible mole at hole " + std::to_string(action_column(a));
    }
};

// predictive: learns the deterministic spawn cadence per hole from
// observation and anticipates the next spawn.  When the hole the model
// expects to see right now already holds a mole it strikes it; otherwise
// it falls back to the urgency rule, so the model only overrides the
// expert on a minority of frames.
class PredictiveStrategy final : public DecisionStrategy {
public:
    const char* name() const noexcept override { return "predictive"; }

    void observe(const GameState& s) noexcept override {
        for (int c = 0; c < kHoles; ++c) {
            const bool has_mole = s.remaining_ticks[c] > 0;
            if (has_mole && last_seen_frame_[c] >= 0) {
                const std::int64_t gap = s.frame_seq - last_seen_frame_[c];
                if (gap > 0 && gap < kMaxGap) {
                    if (interval_seen_[c]) {
                        interval_avg_[c] = (interval_avg_[c] + gap) / 2;  // smoothing
                    } else {
                        interval_avg_[c] = gap;
                        interval_seen_[c] = true;
                    }
                }
            }
            if (has_mole) last_seen_frame_[c] = s.frame_seq;
        }
    }

    Action decide(const GameState& s) const noexcept override {
        // Hole whose cadence says a mole is due right now or at the very
        // next frame — the one the model "expects" to see.
        std::int64_t soonest = kNever;
        int soonest_col = -1;
        for (int c = 0; c < kHoles; ++c) {
            if (!interval_seen_[c] || last_seen_frame_[c] < 0) continue;
            const std::int64_t next_at = last_seen_frame_[c] + interval_avg_[c];
            if (next_at <= s.frame_seq + 1 && next_at < soonest) {
                soonest = next_at;
                soonest_col = c;
            }
        }
        if (soonest_col >= 0 && s.remaining_ticks[soonest_col] > 0) {
            return column_action(soonest_col);
        }
        return ExpertOracle::decide(s);
    }

    std::string reason_for(const GameState& s, Action a) const override {
        if (a == Action::kWait) return "no mole visible";
        const int col = action_column(a);
        return "cadence model struck hole " + std::to_string(col) +
               " (remaining=" + std::to_string(s.remaining_ticks[col]) + " ticks)";
    }

private:
    static constexpr std::int64_t kNever = static_cast<std::int64_t>(1) << 62;
    static constexpr std::int64_t kMaxGap = 1 << 12;  // ignore absurd gaps
    std::array<std::int64_t, kHoles> last_seen_frame_{};
    std::array<std::int64_t, kHoles> interval_avg_{};
    std::array<bool, kHoles> interval_seen_{};
};

// ---------------------------------------------------------------------------
// Strategy registry + factory (acceptance criterion 3: configurable switch)
// ---------------------------------------------------------------------------

inline std::vector<std::string_view> available_strategies() {
    return {"heuristic", "reactive", "predictive"};
}

inline std::unique_ptr<DecisionStrategy> make_strategy(std::string_view name) {
    if (name == "heuristic")  return std::make_unique<HeuristicStrategy>();
    if (name == "reactive")   return std::make_unique<ReactiveStrategy>();
    if (name == "predictive") return std::make_unique<PredictiveStrategy>();
    return nullptr;
}

// ---------------------------------------------------------------------------
// Per-decision latency instrument (acceptance criterion 1)
// ---------------------------------------------------------------------------
//
// Records the wall time (nanoseconds) of every decision so a run of 100
// consecutive decisions can be reported and proven against the 50 ms
// budget — the "性能监控工具记录每次决策耗时" the spec asks for.
class LatencyInstrument {
public:
    void record(std::int64_t ns) noexcept { samples_.push_back(ns); }
    void reset() noexcept { samples_.clear(); }

    std::size_t count() const noexcept { return samples_.size(); }

    std::int64_t max_ns() const noexcept {
        if (samples_.empty()) return 0;
        return *std::max_element(samples_.begin(), samples_.end());
    }

    // Nearest-rank percentile: p == 0 -> min, p == 100 -> max.
    std::int64_t percentile_ns(int p) const {
        if (samples_.empty()) return 0;
        std::vector<std::int64_t> v = samples_;
        std::sort(v.begin(), v.end());
        const std::size_t idx =
            (static_cast<std::size_t>(p) * (v.size() - 1)) / 100;
        return v[idx];
    }

    bool all_within_us(std::int64_t limit_us) const noexcept {
        return samples_.empty() || max_ns() <= limit_us * 1000;
    }

private:
    std::vector<std::int64_t> samples_;
};

// ---------------------------------------------------------------------------
// Decision engine
// ---------------------------------------------------------------------------

class DecisionEngine {
public:
    explicit DecisionEngine(std::string_view strategy_name) {
        std::unique_ptr<DecisionStrategy> s = make_strategy(strategy_name);
        active_ = s ? std::move(s) : make_strategy("heuristic");
    }

    const DecisionStrategy& active() const noexcept { return *active_; }
    const char* strategy_name() const noexcept { return active_->name(); }

    const LatencyInstrument& latencies() const noexcept { return latencies_; }

    bool debug() const noexcept { return debug_; }
    void set_debug(bool on) noexcept { debug_ = on; }

    // Switch strategy at a resource-release checkpoint (plan risk #2):
    // the previous strategy is destroyed at this single, auditable point,
    // so any resources it owned are released right here.  In debug mode
    // the checkpoint is logged.
    bool switch_strategy(std::string_view name) {
        std::unique_ptr<DecisionStrategy> next = make_strategy(name);
        if (!next) return false;
        if (debug_) {
            std::fprintf(stdout,
                "[checkpoint] strategy switch %s -> %s: previous strategy "
                "released (no leaked resources)\n",
                active_->name(), next->name());
        }
        active_ = std::move(next);
        ++switch_count_;
        return true;
    }

    std::size_t switch_count() const noexcept { return switch_count_; }

    // Observe the state (lets stateful strategies learn), then decide, then
    // record the decision latency on the instrument.  In debug mode the full
    // decision process is traced to stdout.
    Action decide(const GameState& s) {
        active_->observe(s);
        const auto t0 = std::chrono::steady_clock::now();
        const Action a = active_->decide(s);
        const auto t1 = std::chrono::steady_clock::now();
        const std::int64_t ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        latencies_.record(ns);
        if (debug_) trace(s, a, ns);
        return a;
    }

    std::int64_t trace_count() const noexcept { return trace_count_; }
    void reset_trace() noexcept { trace_count_ = 0; }
    void reset_latency() noexcept { latencies_.reset(); }

private:
    void trace(const GameState& s, Action a, std::int64_t lat_ns) {
        std::fprintf(stdout,
            "[state] seq=%lld ts=%lld moles=%d rem=%d,%d,%d\n",
            static_cast<long long>(s.frame_seq),
            static_cast<long long>(s.capture_ts_us),
            visible_mole_count(s),
            s.remaining_ticks[0], s.remaining_ticks[1], s.remaining_ticks[2]);
        std::fprintf(stdout,
            "[decision] seq=%lld strat=%s candidates=%d picked=%s reason=\"%s\" lat_ns=%lld\n",
            static_cast<long long>(s.frame_seq),
            active_->name(),
            visible_mole_count(s),
            action_name(a),
            active_->reason_for(s, a).c_str(),
            static_cast<long long>(lat_ns));
        ++trace_count_;
    }

    static int visible_mole_count(const GameState& s) noexcept {
        int n = 0;
        for (int c = 0; c < kHoles; ++c) {
            if (s.remaining_ticks[c] > 0) ++n;
        }
        return n;
    }

    std::unique_ptr<DecisionStrategy> active_;
    LatencyInstrument latencies_;
    bool debug_ = false;
    std::int64_t trace_count_ = 0;
    std::size_t switch_count_ = 0;
};

}  // namespace demoai

#endif  // NIMRTC_DEMO_AGENT_REALTIME_AI_DECISION_HPP
