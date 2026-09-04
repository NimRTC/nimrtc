/**
 * @file nimrtc/plugins/audio3a.hpp
 * @brief IAudio3A — pluggable audio processing (AEC / ANS / AGC) interface.
 *
 * Replace this to use WebRTC APM, SpeexDSP, or a proprietary DSP chain.
 *
 * ## Implementing a custom 3A plugin
 *
 * 1. Implement `IAudio3A` for your DSP pipeline.
 * 2. Register: `PluginRegistry::instance().register_audio3a("my_3a", factory);`
 * 3. Set `NimRTCEngine::Config::audio3a_name = "my_3a"`.
 *
 * ## Thread safety
 *
 * `process_capture()` and `process_render()` may be called concurrently
 * from different threads. Implementations must be thread-safe.
 *
 * @note P0 scaffold — interface stable, binary layout TBD P1.
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_AUDIO3A_HPP
#define NIMRTC_PLUGINS_AUDIO3A_HPP

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

struct Audio3AConfig {
    std::string_view name;
    uint32_t sample_rate_hz = 48000;
    uint8_t  capture_channels = 1;
    uint8_t  render_channels = 1;
    bool enable_aec = true;
    int aec_delay_est_ms = 50;
    uint8_t ans_level = 2;
    bool enable_agc = true;
    int agc_target_dbfs = -3;
    bool enable_vad = false;
    bool report_tx_level = false;
};

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

using VadCallback     = std::function<void(bool speech)>;
using LevelCallback   = std::function<void(float dbfs)>;
using AudioErrorCallback = std::function<void(Status, std::string_view)>;

// ---------------------------------------------------------------------------
// IAudio3A
// ---------------------------------------------------------------------------

class IAudio3A : public IPlugin {
public:
    virtual void set_callbacks(VadCallback         on_vad,
                              LevelCallback       on_level,
                              AudioErrorCallback  on_error) noexcept = 0;

    /** Process captured audio (microphone path, before encoding).
     *  @param samples  PCM samples in float [-1.0, 1.0], size = num_channels * num_samples.
     *  @param num_samples  Number of samples per channel.
     *  @return kOk on success. */
    virtual Status
    process_capture(float* samples,
                    size_t num_samples,
                    size_t num_channels) noexcept = 0;

    /** Process rendered audio (loudspeaker path, for AEC reference). */
    virtual Status
    process_render(const float* samples,
                   size_t num_samples,
                   size_t num_channels) noexcept = 0;

    virtual void set_render_delivered(size_t num_samples) noexcept = 0;
    virtual void request_vad_report() noexcept = 0;
    virtual void request_level_report() noexcept = 0;

    struct Stats {
        uint64_t capture_frames_processed = 0;
        uint64_t render_frames_processed   = 0;
        float    last_capture_level_dbfs  = -96.0f;
        float    last_render_level_dbfs    = -96.0f;
        bool     vad_active = false;
    };
    virtual Stats stats() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

class IAudio3AFactory {
public:
    virtual ~IAudio3AFactory() = default;
    virtual std::string_view id()          const noexcept = 0;
    virtual std::string_view display_name()const noexcept = 0;
    virtual IAudio3A* create()             const = 0;
};

template<class T>
class SimpleAudio3AFactory : public IAudio3AFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleAudio3AFactory(std::string_view id, std::string_view name) noexcept
        : id_(id), name_(name) {}
    virtual std::string_view id()          const noexcept override { return id_; }
    virtual std::string_view display_name()const noexcept override { return name_; }
    virtual IAudio3A* create()             const override { return new T(); }
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_AUDIO3A_HPP
