/**
 * @file nimrtc/plugins/bwe.hpp
 * @brief IBwe — pluggable Bandwidth Estimation interface (NimRTC §2.5 / §6).
 *
 * BWE controls the sender's outbound bitrate target. Implementations react to
 * loss / RTT / REMB feedback and produce a target + pacing bitrate that the
 * engine's scheduler + codec consume.
 *
 * Replace this interface to plug in:
 *   - A custom strategy (e.g. on-device BWE for embedded);
 *   - A Goog-CC compatible mainline (P3 default, see §6 / §11.5.1);
 *   - BBR (internal experiment only, NOT advertised — §6);
 *   - A passthrough / fixed-rate policy for SFU forwarding (§2.2 rule 3).
 *
 * ## Implementing a custom BWE plugin
 *
 * 1. Implement `plugins::IBwe` over your estimation algorithm.
 * 2. Register: `PluginRegistry::instance().register_bwe("my_bwe", factory);`
 * 3. (P3) Set `NimRTCEngine::Config::bwe_name = "my_bwe"` to wire it into
 *    the engine; today the engine does not yet drive the BWE — the
 *    scheduler reads its estimates manually (see
 *    `scheduler::on_bwe_update`).
 *
 * ## Thread safety
 *
 * Implementations must be safe to call from a single engine thread; they
 * do NOT need to be reentrant across threads. The P1 AIMD implementation
 * (see `nimrtc/bwe/bwe.hpp`) is fully reentrant (no shared state) but
 * later Goog-CC / BBR will likely carry a mutex.
 *
 * @note P1 — interface is stable; engine integration lands in P3 (§13).
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_BWE_HPP
#define NIMRTC_PLUGINS_BWE_HPP

#include <cstdint>
#include <optional>
#include <string_view>

#include <nimrtc/core/time.hpp>   // core::TimePoint / core::Milliseconds

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/** Tunable parameters for a BWE implementation.
 *
 *  Implementations are free to ignore fields that don't apply to their
 *  algorithm (e.g. a fixed-rate BWE only reads `initial_bitrate_bps`).
 *  Field semantics follow §6. */
struct BweConfig {
    /** Initial target bitrate (bits per second). */
    std::uint32_t initial_bitrate_bps = 1'000'000;   // 1 Mbps

    /** Hard upper cap the BWE will never exceed. */
    std::uint32_t max_bitrate_bps = 10'000'000;       // 10 Mbps

    /** Minimum floor (preserves tiny frames; protects against starvation). */
    std::uint32_t min_bitrate_bps = 30'000;           // 30 kbps

    /** AIMD-only: additive increase per second (bps). */
    std::uint32_t increase_bps = 50'000;             // 50 kbps/s

    /** AIMD-only: multiplicative decrease factor on loss (0 < factor <= 1). */
    double decrease_factor = 0.5;

    /** Minimum interval between two consecutive bitrate changes. */
    core::Milliseconds change_interval = core::Milliseconds{200};

    /** Exponential moving average alpha (0 = no smoothing, 1 = no change). */
    double smoothing_alpha = 0.1;
};

// ---------------------------------------------------------------------------
// Feedback — input signals to BWE
// ---------------------------------------------------------------------------

/** Network / media feedback consumed by BWE.on_feedback().
 *
 *  Loss rate is the ratio over a single RTCP reporting window; RTT is the
 *  smoothed round-trip time; REMB is the receiver-reported estimate (when
 *  present, override local if higher after sanity checks). */
struct BweFeedback {
    /** Loss rate in [0.0, 1.0]. */
    double loss_rate = 0.0;

    /** Round-trip time estimate. */
    core::Milliseconds rtt{};

    /** Receiver-reported estimate (REMB / CE / DLRR-derived). */
    std::optional<std::uint32_t> remb_bps;

    /** Monotonic timestamp of the feedback. */
    core::TimePoint timestamp;

    /** Packets received in this window. */
    std::uint32_t packets_received = 0;

    /** Packets lost in this window. */
    std::uint32_t packets_lost = 0;
};

// ---------------------------------------------------------------------------
// Estimate — output of BWE
// ---------------------------------------------------------------------------

/** Latest bandwidth estimate + observability metadata. */
struct BweEstimate {
    /** Target egress bitrate (bps) the sender should aim for. */
    std::uint32_t target_bitrate_bps = 0;

    /** Pacing bitrate (bps) — may be slightly higher than target to
     *  smooth bursts; the scheduler uses this for `allowed_per_drain_`. */
    std::uint32_t pacing_bitrate_bps = 0;

    /** Timestamp when this estimate was produced. */
    core::TimePoint timestamp;

    /** Why the most recent change happened (for telemetry / observability). */
    enum class Reason {
        Initial,      // first estimate after construction / reset
        AIMDIncrease, // additive increase phase (no loss)
        AIMDDecrease, // multiplicative decrease on loss
        REMBOverride, // receiver estimate higher than ours
        RttIncrease,  // RTT spike suggests congestion
    };
    Reason reason = Reason::Initial;
};

// ---------------------------------------------------------------------------
// IBwe
// ---------------------------------------------------------------------------

/** Pluggable bandwidth estimator.
 *
 *  Implementations are pure consumers of feedback signals — they do NOT
 *  send packets or own pacing timers. The caller (engine / scheduler)
 *  owns the wire side and decides how to act on the estimate. */
class IBwe : public IPlugin {
public:
    ~IBwe() override = default;

    // IPlugin -----------------------------------------------------------
    const char* name() const noexcept override = 0;
    Status      open() noexcept override = 0;
    void        close() noexcept override = 0;

    // BWE contract --------------------------------------------------------

    /** Inject a feedback sample. Idempotent w.r.t. estimate(). */
    virtual void on_feedback(BweFeedback feedback) noexcept = 0;

    /** Latest estimate snapshot. */
    virtual BweEstimate estimate() const noexcept = 0;

    /** Live reconfigure. Thread-safety: caller guarantees no concurrent
     *  calls with on_feedback() / estimate(). */
    virtual void update_config(BweConfig config) noexcept = 0;

    /** Current configuration (as last set via ctor or update_config). */
    virtual BweConfig config() const noexcept = 0;

    /** Reset internal state (e.g. on stream restart). */
    virtual void reset() noexcept = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/** Factory for BWE implementations. */
class IBweFactory {
public:
    virtual ~IBweFactory() = default;
    virtual std::string_view id()          const noexcept = 0;
    virtual std::string_view display_name()const noexcept = 0;

    /** Create a new BWE instance with the given initial config. */
    virtual IBwe* create(BweConfig config) const = 0;
};

/** Template helper — creates a factory that owns a BWE type T. */
template<class T>
class SimpleBweFactory : public IBweFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleBweFactory(std::string_view id,
                              std::string_view name) noexcept
        : id_(id), name_(name) {}

    std::string_view id()          const noexcept override { return id_; }
    std::string_view display_name()const noexcept override { return name_; }
    IBwe* create(BweConfig config) const override {
        return new T(translate_config_(config));
    }

private:
    /** Forward a plugins::BweConfig to the concrete module's config type.
     *  For the AIMD default, both shapes are identical (see
     *  modules/bwe/include/nimrtc/bwe/bwe.hpp) — we forward through a
     *  cast when layouts match. */
    static auto translate_config_(BweConfig cfg) {
        // The plugin and module configs share the exact same field layout
        // (both defined in this header ecosystem). Implementations that
        // diverge (e.g. Goog-CC) wrap their own config class; their
        // adapter's create() does the translation explicitly.
        return cfg;
    }
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_BWE_HPP
