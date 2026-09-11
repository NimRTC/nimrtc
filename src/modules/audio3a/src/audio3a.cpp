/**
 * @file src/modules/audio3a/src/audio3a.cpp
 * @brief Audio3A — null and stub implementations (P1).
 *
 * ## Implementations
 *
 * - **NullAudio3A**: no-op pass-through for testing
 * - **WebRtcAudio3A**: WebRTC APM integration (P1, requires vendor)
 *
 * ## WebRTC APM Integration (when NIMRTC_VENDOR_WEBRTC_APM=ON)
 *
 * WebRTC APM provides:
 *   - AECm (mobile AEC) / AEC (desktop) — echo cancellation
 *   - NS (Noise Suppression) — low/medium/high
 *   - AGC (Automatic Gain Control) — fixed/dynamic digital/analog
 *   - VAD (Voice Activity Detection)
 *
 * Integration notes:
 *   - APM is instantiated per audio stream
 *   - Render delay must be reported via StreamConfig::set_stream_analog_level()
 *   - Processing delay is typically < 5ms
 */

#include <nimrtc/audio3a/audio3a.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <nimrtc/core/log.hpp>

namespace nimrtc::audio3a {

namespace {

constexpr float kDbfsMin = -96.0f;

// Calculate RMS level in dBFS
float calculate_rms_dbfs(const float* samples, std::size_t count) {
    if (count == 0 || samples == nullptr) {
        return kDbfsMin;
    }

    float sum = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        sum += samples[i] * samples[i];
    }
    double rms = std::sqrt(static_cast<double>(sum) / static_cast<double>(count));
    if (rms < 1e-10) {
        return kDbfsMin;
    }
    return static_cast<float>(20.0 * std::log10(rms));
}

// Simple VAD using energy threshold
bool simple_vad(const float* samples, std::size_t count) {
    if (count == 0 || samples == nullptr) {
        return false;
    }

    float energy = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        energy += std::abs(samples[i]);
    }
    double avg = static_cast<double>(energy) / static_cast<double>(count);

    // Threshold: consider speech if average amplitude > 0.01
    return avg > 0.01;
}

// Gain adjustment (simple AGC)
void apply_gain(float* samples, std::size_t count, float gain_db) {
    if (gain_db == 0.0f || count == 0 || samples == nullptr) {
        return;
    }

    float gain_linear = std::pow(10.0f, gain_db / 20.0f);
    for (std::size_t i = 0; i < count; ++i) {
        samples[i] *= gain_linear;
        // Clip to [-1.0, 1.0]
        samples[i] = std::clamp(samples[i], -1.0f, 1.0f);
    }
}

} // anonymous namespace

// -----------------------------------------------------------------------------
// NullAudio3A
// -----------------------------------------------------------------------------

bool NullAudio3A::init(const Config& config) {
    config_ = config;
    reset();
    return true;
}

void NullAudio3A::process_capture(Frame& frame) {
    // Calculate capture level
    std::size_t total_samples = frame.num_samples * frame.num_channels;
    float level = calculate_rms_dbfs(frame.samples, total_samples);

    // Apply gain if AGC enabled (simple implementation)
    if (config_.enable_agc && level < static_cast<float>(config_.agc_target_dbfs)) {
        float gain_db = static_cast<float>(config_.agc_target_dbfs) - level;
        apply_gain(frame.samples, total_samples, gain_db * 0.5f);  // conservative gain
        level = calculate_rms_dbfs(frame.samples, total_samples);
    }

    stats_.capture_level_dbfs = level;

    // VAD if requested
    if (vad_requested_) {
        stats_.vad_active = simple_vad(frame.samples, total_samples);
        vad_requested_ = false;
    }

    if (level_requested_) {
        level_requested_ = false;
    }
}

void NullAudio3A::process_render(const Frame& frame) {
    std::size_t total_samples = frame.num_samples * frame.num_channels;
    stats_.render_level_dbfs = calculate_rms_dbfs(frame.samples, total_samples);
}

void NullAudio3A::on_render_delivered(std::size_t num_samples) {
    // No-op for null implementation
    (void)num_samples;
}

void NullAudio3A::request_vad_report() {
    vad_requested_ = true;
}

void NullAudio3A::request_level_report() {
    level_requested_ = true;
}

LevelStats NullAudio3A::stats() const {
    return stats_;
}

void NullAudio3A::reset() {
    stats_ = LevelStats{};
    vad_requested_ = false;
    level_requested_ = false;
}

// -----------------------------------------------------------------------------
// Factory
// -----------------------------------------------------------------------------

std::unique_ptr<IAudio3A> create_null_audio3a() {
    return std::make_unique<NullAudio3A>();
}

// Note: create_audio3a() is defined in webrtc_audio3a.cpp (which also covers
// the "null" / "stub" branches via create_null_audio3a() above).

} // namespace nimrtc::audio3a
