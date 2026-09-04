#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include <nimrtc/core/time.hpp>

// =============================================================================
// nimrtc::audio3a
// -----------------------------------------------------------------------------
// Audio 3A processing — AEC (Acoustic Echo Cancellation),
// ANS (Ambient Noise Suppression), AGC (Automatic Gain Control).
//
// P1 delivers:
//   - WebRTC APM integration (via vendor)
//   - NullAudio3A stub (for testing without DSP)
//   - Metrics: VAD, TX/RX levels
//
// Thread safety:
//   - Audio3A is NOT thread-safe. Caller must ensure push_render and
//     push_capture are called from the same thread, or serialize access.
// =============================================================================
namespace nimrtc::audio3a {

// -----------------------------------------------------------------------------
// Config
// -----------------------------------------------------------------------------
struct Config {
    // Audio format
    std::uint32_t sample_rate_hz = 48000;
    std::uint8_t  capture_channels = 1;
    std::uint8_t  render_channels = 1;

    // AEC settings
    bool          enable_aec = true;
    std::int32_t  aec_delay_headroom_ms = 50;

    // ANS settings (0=off, 1=low, 2=medium, 3=high)
    std::uint8_t  ans_level = 2;

    // AGC settings
    bool          enable_agc = true;
    std::int32_t  agc_target_dbfs = -3;  // target level in dBFS

    // VAD (Voice Activity Detection)
    bool          enable_vad = false;

    // Level reporting
    bool          report_tx_level = false;
};

// -----------------------------------------------------------------------------
// Types
// -----------------------------------------------------------------------------

// Audio frame — interleaved PCM samples
struct Frame {
    float*        samples = nullptr;     // interleaved: [ch0_s0, ch1_s0, ch0_s1, ch1_s1, ...]
    std::size_t   num_samples = 0;      // samples per channel
    std::size_t   num_channels = 1;
    std::uint32_t sample_rate_hz = 48000;
    core::TimePoint timestamp;
};

// Level metrics (dBFS: dB relative to full scale)
struct LevelStats {
    float capture_level_dbfs = -96.0f;
    float render_level_dbfs = -96.0f;
    bool  vad_active = false;
};

// -----------------------------------------------------------------------------
// IAudio3A — pluggable interface
// -----------------------------------------------------------------------------
class IAudio3A {
public:
    virtual ~IAudio3A() = default;

    // Initialize the processor with given config.
    virtual bool init(const Config& config) = 0;

    // Process capture audio (microphone path, before encoding).
    // Samples are modified in-place.
    virtual void process_capture(Frame& frame) = 0;

    // Process render audio (loudspeaker reference for AEC).
    virtual void process_render(const Frame& frame) = 0;

    // Report that render samples have been delivered to hardware.
    // Used by AEC to track acoustic delay.
    virtual void on_render_delivered(std::size_t num_samples) = 0;

    // Request VAD report on next process_capture.
    virtual void request_vad_report() = 0;

    // Request level report on next process_capture.
    virtual void request_level_report() = 0;

    // Get current stats.
    virtual LevelStats stats() const = 0;

    // Reset internal state.
    virtual void reset() = 0;
};

// -----------------------------------------------------------------------------
// NullAudio3A — no-op stub for testing
// -----------------------------------------------------------------------------
class NullAudio3A : public IAudio3A {
public:
    bool init(const Config& config) override;
    void process_capture(Frame& frame) override;
    void process_render(const Frame& frame) override;
    void on_render_delivered(std::size_t num_samples) override;
    void request_vad_report() override;
    void request_level_report() override;
    LevelStats stats() const override;
    void reset() override;

private:
    Config config_;
    LevelStats stats_{};
    bool vad_requested_ = false;
    bool level_requested_ = false;
};

// -----------------------------------------------------------------------------
// WebRtcAudio3A — WebRTC APM integration
// Requires NIMRTC_VENDOR_WEBRTC_APM to be enabled.
// -----------------------------------------------------------------------------
// #ifdef NIMRTC_VENDOR_WEBRTC_APM
// class WebRtcAudio3A : public IAudio3A { ... };
// #endif

// -----------------------------------------------------------------------------
// Factory
// -----------------------------------------------------------------------------
std::unique_ptr<IAudio3A> create_null_audio3a();
std::unique_ptr<IAudio3A> create_audio3a(std::string_view type);

} // namespace nimrtc::audio3a
