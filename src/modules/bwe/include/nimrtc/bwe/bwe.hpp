#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include <nimrtc/core/time.hpp>

// =============================================================================
// nimrtc::bwe
// -----------------------------------------------------------------------------
// Bandwidth Estimator — pluggable congestion control interface.
//
// P1 delivers:
//   - Abstract IBwe interface
//   - AIMD (Additive Increase Multiplicative Decrease) default implementation
//   - Fixed-rate fallback
//
// P3 will replace AIMD with Goog-CC-compatible mainline as the default.
// BBR is internal experiment only (not for external docs).
//
// Design notes:
//   - Bwe is a pure consumer of feedback signals (loss rate, RTT, RTCP REMB).
//   - It does NOT send packets; the caller owns the pacing.
//   - All estimates are in bits per second (bps).
// =============================================================================
namespace nimrtc::bwe {

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------
struct Config {
    // Initial target bitrate (bits per second)
    std::uint32_t initial_bitrate_bps = 1'000'000;  // 1 Mbps

    // Hard cap: bwe will never suggest more than this
    std::uint32_t max_bitrate_bps = 10'000'000;  // 10 Mbps

    // Minimum bitrate floor
    std::uint32_t min_bitrate_bps = 30'000;  // 30 kbps (for tiny frames)

    // AIMD parameters
    // Increase rate: add this many bps per second when no loss
    std::uint32_t increase_bps = 50'000;  // 50 kbps/s

    // Decrease factor: multiply by this when loss detected (0 < factor <= 1)
    double       decrease_factor = 0.5;   // cut in half on loss

    // Minimum time between two consecutive bitrate changes (ms)
    core::Milliseconds change_interval = core::Milliseconds{200};

    // Bitrate change smoothing (exponential moving average alpha)
    // 0.0 = no smoothing, 1.0 = never change
    double smoothing_alpha = 0.1;
};

// -----------------------------------------------------------------------------
// Feedback — signals from the network / media path
// -----------------------------------------------------------------------------
struct Feedback {
    // Packet loss rate observed over the last window [0.0, 1.0]
    //   0.0 = no loss, 1.0 = 100% loss
    double loss_rate = 0.0;

    // Round-trip time estimate (ms)
    core::Milliseconds rtt{};

    // Optional: receiver-reported estimated bandwidth (from REMB)
    std::optional<std::uint32_t> remb_bps;

    // Timestamp of this feedback (monotonic clock)
    core::TimePoint timestamp;

    // Number of packets received in this window
    std::uint32_t packets_received = 0;

    // Number of packets lost in this window
    std::uint32_t packets_lost = 0;
};

// -----------------------------------------------------------------------------
// Estimate — output of BWE
// -----------------------------------------------------------------------------
struct Estimate {
    // Current target bitrate (bps) — what sender should aim for
    std::uint32_t target_bitrate_bps = 0;

    // Pacing bitrate (bps) — may be slightly higher than target for burst smoothing
    std::uint32_t pacing_bitrate_bps = 0;

    // Timestamp when this estimate was produced
    core::TimePoint timestamp;

    // Reason for last change (for observability)
    enum class Reason {
        Initial,      // first estimate
        AIMDIncrease, // additive increase phase
        AIMDDecrease, // multiplicative decrease on loss
        REMBOverride, // receiver estimate higher than ours
        RttIncrease,  // RTT spike suggests congestion
    };
    Reason reason = Reason::Initial;
};

// -----------------------------------------------------------------------------
// IBwe — interface for pluggable BWE implementations
// -----------------------------------------------------------------------------
class IBwe {
public:
    virtual ~IBwe() = default;

    // Inject network feedback. Called whenever RTCP report / REMB is received.
    // Returns the updated estimate (same as last call to estimate()).
    virtual void on_feedback(Feedback feedback) = 0;

    // Get current bitrate estimate.
    virtual Estimate estimate() const = 0;

    // Live reconfigure. Thread-safety: caller guarantees no concurrent calls.
    virtual void update_config(Config config) = 0;

    virtual Config config() const = 0;

    // Reset internal state (e.g. on stream restart).
    virtual void reset() = 0;
};

// -----------------------------------------------------------------------------
// Bwe — default implementation: AIMD
// -----------------------------------------------------------------------------
class Bwe : public IBwe {
public:
    explicit Bwe(Config config = {});
    ~Bwe() override;

    Bwe(const Bwe&)            = delete;
    Bwe& operator=(const Bwe&) = delete;
    Bwe(Bwe&&)                 noexcept;
    Bwe& operator=(Bwe&&)      noexcept;

    void on_feedback(Feedback feedback) override;
    Estimate estimate() const override;
    void update_config(Config config) override;
    Config config() const override;
    void reset() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nimrtc::bwe
