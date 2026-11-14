/**
 * @file nimrtc/audio_device/system_audio.hpp
 * @brief The system-audio (loopback) capture device (remote-desktop plan F6).
 *
 * ## What "system audio" is
 *
 * "Share the tab's audio" / "share the system's output" is a
 * remote-desktop feature.  It is a **second audio source**, not a
 * variant of the microphone: the host captures what the machine is
 * *playing* and sends it alongside (or instead of) the operator's
 * voice.
 *
 * ## The seam
 *
 * The system-audio device is presented through the **same**
 * `plugins::IAudioSource` seam the microphone uses, so a host selects
 * it exactly the way it selects the microphone:
 *
 * @code
 *   auto* mic  = audio_device::source_factory("portaudio")->create(mic_cfg);
 *   auto* sys  = audio_device::source_factory("system-audio")->create(sys_cfg);
 * @endcode
 *
 * The two devices are **independent instances**: they can be opened at
 * the same time, they negotiate their formats independently, and the
 * `AudioMixer` (see `mixer.hpp`) combines them.
 *
 * ## Device identity
 *
 * A system-audio device has a **distinct identity** from every
 * microphone:
 *
 *   - its **device id** is prefixed with `system-audio:` — the
 *     microphone ids are `portaudio:<index>` and `synthetic:0`, so the
 *     two namespaces never collide;
 *   - it carries a **kind** (`SystemAudioDeviceKind`) that says *what
 *     kind* of loopback it is (a WASAPI loopback, a Core Audio tap, a
 *     PulseAudio monitor, ...);
 *   - it carries a **backend** (`SystemAudioBackend`) that says *which*
 *     platform mechanism implements it.
 *
 * The system-audio device is **never** registered as "the microphone":
 * it is not in the microphone's device list, and its id is not in the
 * microphone's namespace.
 *
 * ## Per-platform support
 *
 *   - **Windows** — WASAPI **loopback**: the render endpoints are
 *     enumerated through `IMMDeviceEnumerator::EnumAudioEndpoints(eRender)`
 *     and the default one is opened with `eRender` + `eConsole` +
 *     `AUDCLNT_STREAMFLAGS_LOOPBACK`.  The captured PCM is the mix
 *     format of the endpoint, converted to the module's float PCM.
 *   - **macOS** — Core Audio needs a **tap / aggregate device** (there
 *     is no system-wide loopback stream).  The seam exists; opening a
 *     device that is not a tap fails with an **explicit** error that
 *     says exactly that.
 *   - **Linux** — PulseAudio / PipeWire expose the system's output as
 *     a **monitor source** (`<sink>.monitor`).  The seam exists; when
 *     no monitor source can be enumerated the factory reports an
 *     **explicit** error.
 *
 * On any platform where the loopback cannot be opened the factory
 * reports a **clean, enumerated error** — never silence, never a
 * crash.  `SystemAudioSourceFactory::status()` carries the reason.
 *
 * @note Remote-desktop plan F6 (M2-c).
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nimrtc/audio_device/ring_buffer.hpp>
#include <nimrtc/plugins/audio_source.hpp>

namespace nimrtc::audio_device {

// ---------------------------------------------------------------------------
// Device vocabulary
// ---------------------------------------------------------------------------

/**
 * @brief The platform mechanism that implements a system-audio device.
 */
enum class NIMRTC_API_STABLE SystemAudioBackend : std::uint8_t {
    /** No loopback mechanism is available on this build / platform. */
    kUnavailable = 0,

    /** Windows: WASAPI loopback (`AUDCLNT_STREAMFLAGS_LOOPBACK`). */
    kWasapiLoopback = 1,

    /** macOS: a Core Audio tap / aggregate device. */
    kCoreAudioTap = 2,

    /** Linux: a PulseAudio / PipeWire monitor source. */
    kPulseAudioMonitor = 3,

    /** The CI-testable synthetic system-audio source. */
    kSynthetic = 4,
};

/** Human-readable backend name (for logging / UI). */
[[nodiscard]] NIMRTC_API_STABLE std::string_view
system_audio_backend_name(SystemAudioBackend backend) noexcept;

/**
 * @brief What kind of loopback a system-audio device is.
 *
 * The kind is part of the device's **identity**: a host can tell a
 * whole-machine capture from a per-application tap without parsing the
 * device's name.
 */
enum class NIMRTC_API_STABLE SystemAudioDeviceKind : std::uint8_t {
    /** The whole machine's output (WASAPI loopback, PulseAudio
     *  monitor). */
    kLoopback = 0,

    /** A per-application / per-tap capture (macOS aggregate device). */
    kTap = 1,

    /** The CI-testable synthetic source. */
    kSynthetic = 2,
};

/** Human-readable kind name (for logging / UI). */
[[nodiscard]] NIMRTC_API_STABLE std::string_view
system_audio_device_kind_name(SystemAudioDeviceKind kind) noexcept;

/**
 * @brief One system-audio device.
 *
 * The id is the id the seam uses: it is passed back to `open()` /
 * `create()` verbatim and it is **distinct** from every microphone id.
 */
struct NIMRTC_API_STABLE SystemAudioDevice {
    /** The stable device id, e.g. "system-audio:wasapi:default". */
    std::string id;

    /** Human-readable name, e.g. "System Output (Loopback)". */
    std::string name;

    /** The kind of loopback this device is. */
    SystemAudioDeviceKind kind = SystemAudioDeviceKind::kLoopback;

    /** The mechanism that implements it. */
    SystemAudioBackend backend = SystemAudioBackend::kUnavailable;

    /** The channel count the device delivers (0 = unknown). */
    std::uint16_t channels = 0;

    /** The rate the device delivers, in Hz (0 = unknown). */
    std::uint32_t sample_rate_hz = 0;
};

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

/**
 * @brief Why the system-audio factory is (or is not) usable.
 */
enum class NIMRTC_API_STABLE SystemAudioStatus : std::uint8_t {
    /** The factory is usable: at least one device was enumerated. */
    kAvailable = 0,

    /** The platform has no loopback mechanism at all in this build. */
    kUnsupported = 1,

    /** The platform has a loopback mechanism, but no device could be
     *  opened or enumerated (e.g. macOS without a tap device). */
    kNoDevice = 2,

    /** The platform refused to hand out the device list (a COM or
     *  service failure). */
    kBackendError = 3,
};

/** Human-readable status name (for logging / UI). */
[[nodiscard]] NIMRTC_API_STABLE std::string_view
system_audio_status_name(SystemAudioStatus status) noexcept;

/**
 * @brief The factory's status: the code **and** the human-readable
 *        reason.
 */
struct NIMRTC_API_STABLE SystemAudioFactoryStatus {
    /** The status code. */
    SystemAudioStatus code = SystemAudioStatus::kUnsupported;

    /** The backend that implements (or would implement) the loopback. */
    SystemAudioBackend backend = SystemAudioBackend::kUnavailable;

    /**
     * The human-readable reason.
     *
     * On a platform without loopback support this says **what** is
     * missing ("macOS requires a Core Audio tap / aggregate device",
     * "no PulseAudio monitor source was found", ...).  It is never
     * empty when the status is not `kAvailable`.
     */
    std::string reason;
};

// ---------------------------------------------------------------------------
// SystemAudioSourceFactory
// ---------------------------------------------------------------------------

/**
 * @brief Factory for the system-audio (loopback) capture device.
 *
 * The factory is stateless — it is safe to create several and to keep
 * one for the lifetime of the process.  Each `create()` call returns a
 * fresh, independent instance.
 */
class NIMRTC_API_STABLE SystemAudioSourceFactory final
    : public plugins::IAudioSourceFactory {
public:
    [[nodiscard]] std::string_view id() const noexcept override {
        return "system-audio";
    }

    [[nodiscard]] std::string_view display_name() const noexcept override {
        return "System audio loopback (WASAPI loopback / Core Audio tap / "
               "PulseAudio monitor)";
    }

    [[nodiscard]] plugins::IAudioSource* create(
        plugins::AudioSourceConfig cfg) const override;

    /**
     * Open a system-audio device, reporting the failure reason.
     *
     * This is the seam's **explicit** error path: when the platform
     * cannot open the requested device, the returned pointer is null
     * and @p error says **why** — e.g. "macOS requires a Core Audio
     * tap / aggregate device".  The failure is never silence.
     */
    [[nodiscard]] std::unique_ptr<plugins::IAudioSource> open_device(
        plugins::AudioSourceConfig cfg,
        std::string& error) const;

    /** The device ids this factory can open. */
    [[nodiscard]] std::vector<std::string> device_ids() const;

    /**
     * The system-audio devices this factory can open.
     *
     * Every device carries a **distinct identity**: the id lives in
     * the `system-audio:` namespace, which no microphone id uses.
     */
    [[nodiscard]] std::vector<SystemAudioDevice> devices() const;

    /** The backend that implements the loopback on this platform. */
    [[nodiscard]] SystemAudioBackend backend() const noexcept;

    /** True iff at least one device can be opened. */
    [[nodiscard]] bool available() const noexcept;

    /**
     * The factory's status.
     *
     * When `available()` is false the status carries an **explicit**
     * reason: the factory never fails silently and never reports an
     * empty list as a success.
     */
    [[nodiscard]] SystemAudioFactoryStatus status() const;
};

// ---------------------------------------------------------------------------
// SyntheticSystemAudioSource
// ---------------------------------------------------------------------------

/**
 * @brief A system-audio device that synthesises a deterministic tone.
 *
 * It is the system-audio counterpart of `SyntheticAudioSource`: it
 * needs no sound card, so the whole system-audio path (the factory,
 * the identity, the mixer's second leg) is testable on every CI leg.
 *
 * It implements `plugins::IAudioSource` exactly like the microphone's
 * synthetic device does — the same format negotiation, the same
 * callback / pull delivery, the same device-loss path.
 */
class NIMRTC_API_STABLE SyntheticSystemAudioSource final
    : public plugins::IAudioSource {
public:
    explicit SyntheticSystemAudioSource(
        plugins::AudioSourceConfig cfg) noexcept;
    ~SyntheticSystemAudioSource() override;

    SyntheticSystemAudioSource(const SyntheticSystemAudioSource&)            = delete;
    SyntheticSystemAudioSource& operator=(const SyntheticSystemAudioSource&) = delete;

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override {
        return "synthetic-system-audio-source";
    }

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
     * @return kOk when a frame was produced; `kErrNotReady` when the
     *         device is closed; `kErrHardwareError` when the device
     *         has been marked lost.
     */
    plugins::Status pump() noexcept;

    /** Produce @p n frames back to back. */
    plugins::Status pump(std::size_t n) noexcept;

    /** Run the device in real-time mode (a background thread). */
    plugins::Status start_realtime(std::size_t frames_per_buffer) noexcept;

    /** Stop the real-time thread (if it is running). */
    void stop_realtime() noexcept;

    /** True iff the real-time thread is running. */
    [[nodiscard]] bool is_realtime() const noexcept { return rt_running_.load(); }

    /** Make the device report itself as lost (the clean-error path). */
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

private:
    void produce_frame() noexcept;
    void realtime_loop(std::size_t frames_per_buffer) noexcept;

    plugins::AudioSourceConfig cfg_;
    AudioRingBuffer queue_;
    plugins::AudioFrameCallback cb_;

    std::vector<float> scratch_;
    std::atomic<bool> open_ {false};
    std::atomic<bool> device_lost_ {false};
    std::atomic<bool> rt_running_ {false};
    std::atomic<std::uint64_t> frame_seq_ {0};
    std::atomic<std::uint64_t> frames_processed_ {0};
    std::atomic<std::uint64_t> overruns_ {0};
    std::atomic<std::uint64_t> device_errors_ {0};

    std::thread rt_thread_;
    double tone_hz_ = 880.0;
    double phase_ = 0.0;
};

// ---------------------------------------------------------------------------
// SyntheticSystemAudioSourceFactory
// ---------------------------------------------------------------------------
/**
 * @brief Factory for the synthetic system-audio source.
 *
 * Its id is `"synthetic-system-audio"` — distinct from both the
 * microphone's `"synthetic"` and the real loopback's `"system-audio"`,
 * so a host can never confuse the CI device for a real one.
 */
class NIMRTC_API_STABLE SyntheticSystemAudioSourceFactory final
    : public plugins::IAudioSourceFactory {
public:
    [[nodiscard]] std::string_view id() const noexcept override {
        return "synthetic-system-audio";
    }

    [[nodiscard]] std::string_view display_name() const noexcept override {
        return "Synthetic system audio device (CI-testable)";
    }

    [[nodiscard]] plugins::IAudioSource* create(
        plugins::AudioSourceConfig cfg) const override;

    /** The device ids this factory can open. */
    [[nodiscard]] std::vector<std::string> device_ids() const;
};

} // namespace nimrtc::audio_device
