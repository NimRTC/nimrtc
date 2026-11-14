/**
 * @file nimrtc/audio_device/synthetic_device.hpp
 * @brief The synthetic audio device — the module's CI-testable backend.
 *
 * This is the audio counterpart of `video_capture`'s `SyntheticCapture`:
 * a device that needs no sound card, no driver, no permission prompt
 * and no timing luck, so that the *entire* portable device layer can be
 * exercised on every CI leg, on every platform.
 *
 * ## Two modes
 *
 * The synthetic device has two run modes, and the difference matters:
 *
 *   - **pumped** (the default in the tests): the host drives the device
 *     by calling `pump()` explicitly.  Every frame is produced at a
 *     deterministic point, so an assertion on the *exact* sample
 *     values is meaningful.
 *   - **real-time**: a background thread produces frames at the
 *     device's nominal rate.  This is what exercises the threading
 *     (the ring buffer, the playback-progress callback) the way a real
 *     device does.
 *
 * ## The device-loss injection
 *
 * A microphone that is unplugged mid-call is a *normal* event, not a
 * crash — the plan's acceptance criterion for F2 is exactly that: "a
 * device-loss event is surfaced as a clean error, not a crash".
 *
 * The synthetic device therefore has `simulate_device_loss()`, which
 * makes the next `pump()` (or the next real-time tick) report the
 * device as lost: the frame callback stops firing, `stats()` reports
 * `device_lost == true` and an incremented `device_errors`, and no
 * further frames are produced.  The device does not abort, does not
 * spin and does not throw.
 *
 * @note Remote-desktop plan F2. The module's reference device.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <nimrtc/audio_device/aec_delay.hpp>
#include <nimrtc/audio_device/playback_progress.hpp>
#include <nimrtc/audio_device/ring_buffer.hpp>
#include <nimrtc/plugins/audio_sink.hpp>
#include <nimrtc/plugins/audio_source.hpp>

namespace nimrtc::audio_device {

// ---------------------------------------------------------------------------
// SyntheticAudioSource
// ---------------------------------------------------------------------------

/**
 * @brief A capture device that synthesises a deterministic tone.
 *
 * Implements `plugins::IAudioSource`.
 */
class NIMRTC_API_STABLE SyntheticAudioSource final
    : public plugins::IAudioSource,
      public IAecDelaySource {
public:
    explicit SyntheticAudioSource(plugins::AudioSourceConfig cfg) noexcept;
    ~SyntheticAudioSource() override;

    SyntheticAudioSource(const SyntheticAudioSource&)            = delete;
    SyntheticAudioSource& operator=(const SyntheticAudioSource&) = delete;

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override { return "synthetic-audio-source"; }

    plugins::Status open() noexcept override;
    void close() noexcept override;

    // ---- IAudioSource -----------------------------------------------------
    [[nodiscard]] std::vector<plugins::AudioDevice>
    enumerate_devices() const noexcept override;

    [[nodiscard]] std::vector<plugins::AudioDeviceFormat>
    supported_formats(const std::string& device_id) const noexcept override;

    [[nodiscard]] plugins::AudioDeviceCapabilities
    capabilities() const noexcept override;

    [[nodiscard]] bool is_open() const noexcept override { return open_.load(); }

    [[nodiscard]] plugins::AudioSourceConfig config() const noexcept override {
        return cfg_;
    }

    void set_on_audio_frame(plugins::AudioFrameCallback cb) noexcept override;

    plugins::Status read_samples(float* dst,
                                std::size_t capacity,
                                std::size_t* out_frames) noexcept override;

    [[nodiscard]] plugins::AudioDeviceStats stats() const noexcept override;

    // ---- Synthetic-specific controls -------------------------------------

    /**
     * Produce one frame.
     *
     * In *pumped* mode this is the only way frames appear, so a test
     * controls exactly when each frame arrives.  In *real-time* mode the
     * background thread calls it instead.
     *
     * @return kOk when a frame was produced; `kErrNotReady` when the
     *         device is closed; `kErrHardwareError` when the device
     *         has been marked lost.
     */
    plugins::Status pump() noexcept;

    /** Produce @p n frames back to back. */
    plugins::Status pump(std::size_t n) noexcept;

    /**
     * Run the device in real-time mode: a background thread produces
     * frames at the device's nominal rate.
     *
     * @param frames_per_buffer The device quantum (default 480 frames
     *                           = 10 ms at 48 kHz).
     */
    plugins::Status start_realtime(std::size_t frames_per_buffer) noexcept;

    /** Stop the real-time thread (if it is running). */
    void stop_realtime() noexcept;

    /** True iff the real-time thread is running. */
    [[nodiscard]] bool is_realtime() const noexcept { return rt_running_.load(); }

    /**
     * Make the device report itself as lost.
     *
     * The next `pump()` returns `kErrHardwareError`, the frame callback
     * stops firing, and `stats()` reports `device_lost == true`.
     */
    void simulate_device_loss() noexcept { device_lost_.store(true); }

    /** Undo `simulate_device_loss()`. */
    void clear_device_loss() noexcept { device_lost_.store(false); }

    /** True iff the device is currently marked lost. */
    [[nodiscard]] bool is_device_lost() const noexcept {
        return device_lost_.load();
    }

    /** The frequency of the synthesised tone, in Hz. */
    [[nodiscard]] double tone_frequency_hz() const noexcept { return tone_hz_; }

    /** Set the frequency of the synthesised tone. */
    void set_tone_frequency_hz(double hz) noexcept { tone_hz_ = hz; }

    // ---- IAecDelaySource (remote-desktop plan F4) --------------------------

    /**
     * Attach (or detach, with nullptr) the AEC-delay monitor.
     *
     * The monitor is fed with the samples the device captured, so the
     * estimator can correlate them against the played-out signal.
     */
    void set_aec_delay_monitor(AecDelayMonitor* monitor) noexcept override {
        aec_monitor_.store(monitor, std::memory_order_relaxed);
    }

    /** The attached monitor, or nullptr when none is attached. */
    [[nodiscard]] AecDelayMonitor* aec_delay_monitor() const noexcept override {
        return aec_monitor_.load(std::memory_order_relaxed);
    }

private:
    void produce_frame() noexcept;
    void realtime_loop(std::size_t frames_per_buffer) noexcept;

    plugins::AudioSourceConfig cfg_;
    AudioRingBuffer queue_;
    plugins::AudioFrameCallback cb_;

    /** The F4 monitor (not owned — the host keeps it alive). */
    std::atomic<AecDelayMonitor*> aec_monitor_ {nullptr};

    std::vector<float> scratch_;
    std::atomic<bool> open_ {false};
    std::atomic<bool> device_lost_ {false};
    std::atomic<bool> rt_running_ {false};
    std::atomic<std::uint64_t> frame_seq_ {0};
    std::atomic<std::uint64_t> frames_processed_ {0};
    std::atomic<std::uint64_t> overruns_ {0};
    std::atomic<std::uint64_t> device_errors_ {0};

    std::thread rt_thread_;
    double tone_hz_ = 440.0;
    double phase_ = 0.0;
};

// ---------------------------------------------------------------------------
// SyntheticAudioSink
// ---------------------------------------------------------------------------

/**
 * @brief A playback device that consumes samples and counts them.
 *
 * Implements `plugins::IAudioSink`.  This is the F3 substrate: the sink
 * knows exactly how many samples it consumed, so a host can hand that
 * number to the engine's `set_render_delivered()` and the 3A gets an
 * aligned AEC reference.
 */
class NIMRTC_API_STABLE SyntheticAudioSink final
    : public plugins::IAudioSink,
      public IPlaybackProgressSink,
      public IAecDelaySink {
public:
    explicit SyntheticAudioSink(plugins::AudioSinkConfig cfg) noexcept;
    ~SyntheticAudioSink() override;

    SyntheticAudioSink(const SyntheticAudioSink&)            = delete;
    SyntheticAudioSink& operator=(const SyntheticAudioSink&) = delete;

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override { return "synthetic-audio-sink"; }

    plugins::Status open() noexcept override;
    void close() noexcept override;

    // ---- IAudioSink -------------------------------------------------------
    [[nodiscard]] std::vector<plugins::AudioDevice>
    enumerate_devices() const noexcept override;

    [[nodiscard]] std::vector<plugins::AudioDeviceFormat>
    supported_formats(const std::string& device_id) const noexcept override;

    [[nodiscard]] plugins::AudioDeviceCapabilities
    capabilities() const noexcept override;

    [[nodiscard]] bool is_open() const noexcept override { return open_.load(); }

    [[nodiscard]] plugins::AudioSinkConfig config() const noexcept override {
        return cfg_;
    }

    plugins::Status write(const float* samples,
                           std::size_t num_samples,
                           std::uint16_t channels) noexcept override;

    [[nodiscard]] std::uint64_t played_samples() const noexcept override {
        return played_.load();
    }

    void set_on_playback_progress(
        plugins::AudioPlaybackCallback cb) noexcept override;

    [[nodiscard]] plugins::AudioDeviceStats stats() const noexcept override;

    // ---- Synthetic-specific controls -------------------------------------

    /**
     * Consume one quantum of the queued samples.
     *
     * This is what advances `played_samples()`.  In *pumped* mode the
     * host calls it explicitly (so the counter is deterministic); in
     * *real-time* mode a background thread calls it at the device rate.
     *
     * @return kOk when a quantum was consumed; `kErrNotReady` when the
     *         queue is empty (an underrun — silence is played).
     */
    plugins::Status pump() noexcept;

    /** Consume @p n quanta back to back. */
    plugins::Status pump(std::size_t n) noexcept;

    /** Run the device in real-time mode. */
    plugins::Status start_realtime(std::size_t frames_per_buffer) noexcept;

    /** Stop the real-time thread (if it is running). */
    void stop_realtime() noexcept;

    /** True iff the real-time thread is running. */
    [[nodiscard]] bool is_realtime() const noexcept { return rt_running_.load(); }

    /**
     * Make the device report itself as lost.
     *
     * The next `pump()` returns `kErrHardwareError` and stops
     * consuming — the queued samples stay queued, and the played-sample
     * counter stops advancing.
     */
    void simulate_device_loss() noexcept { device_lost_.store(true); }

    /** Undo `simulate_device_loss()`. */
    void clear_device_loss() noexcept { device_lost_.store(false); }

    /** True iff the device is currently marked lost. */
    [[nodiscard]] bool is_device_lost() const noexcept {
        return device_lost_.load();
    }

    // ---- IPlaybackProgressSink (remote-desktop plan F3) --------------------

    /** The sink's playback-progress tracker. */
    [[nodiscard]] const PlaybackProgressTracker&
    playback_progress() const noexcept override {
        return progress_;
    }

    // ---- IAecDelaySink (remote-desktop plan F4) ----------------------------

    /**
     * Attach (or detach, with nullptr) the AEC-delay monitor.
     *
     * The monitor is fed with the samples the sink hands to the
     * device, so the estimator can correlate them against the
     * microphone capture.
     */
    void set_aec_delay_monitor(AecDelayMonitor* monitor) noexcept override {
        aec_monitor_.store(monitor, std::memory_order_relaxed);
    }

    /** The attached monitor, or nullptr when none is attached. */
    [[nodiscard]] AecDelayMonitor* aec_delay_monitor() const noexcept override {
        return aec_monitor_.load(std::memory_order_relaxed);
    }

private:
    void realtime_loop(std::size_t frames_per_buffer) noexcept;

    plugins::AudioSinkConfig cfg_;
    AudioRingBuffer queue_;
    plugins::AudioPlaybackCallback progress_cb_;

    /** The F3 bookkeeping: what was queued and what was played. */
    PlaybackProgressTracker progress_;

    /** The F4 monitor (not owned — the host keeps it alive). */
    std::atomic<AecDelayMonitor*> aec_monitor_ {nullptr};

    std::vector<float> scratch_;
    std::atomic<bool> open_ {false};
    std::atomic<bool> device_lost_ {false};
    std::atomic<bool> rt_running_ {false};
    std::atomic<std::uint64_t> played_ {0};
    std::atomic<std::uint64_t> frames_processed_ {0};
    std::atomic<std::uint64_t> frames_dropped_ {0};
    std::atomic<std::uint64_t> underruns_ {0};
    std::atomic<std::uint64_t> device_errors_ {0};

    std::thread rt_thread_;
    std::size_t quantum_frames_ = 480;
};

} // namespace nimrtc::audio_device
