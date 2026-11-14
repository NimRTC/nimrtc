/**
 * @file nimrtc/audio_device/audio_device.hpp
 * @brief The audio-device module — PortAudio-backed capture + playback.
 *
 * This is the module half of the remote-desktop plan's Track F (F2).  The
 * *seam* half (F1) lives in `src/plugins/`:
 *
 *   - `nimrtc/plugins/audio_source.hpp` — `plugins::IAudioSource`
 *   - `nimrtc/plugins/audio_sink.hpp`   — `plugins::IAudioSink`
 *   - `nimrtc/plugins/audio_device.hpp` — the shared device vocabulary
 *
 * and this module provides the *implementations* of those seams:
 *
 *   - `PortAudioSourceFactory` — capture from a real microphone
 *   - `PortAudioSinkFactory`   — playback through a real loudspeaker
 *   - `SyntheticSourceFactory` / `SyntheticSinkFactory` — a device that
 *     needs no sound card, so the module's tests run on every CI leg
 *
 * ## Why one library instead of three platform backends
 *
 * The plan's RD-10 rule says: one cross-platform library, not a
 * hand-written backend per platform.  PortAudio is that library — WASAPI
 * / MME / DirectSound on Windows, Core Audio on macOS, ALSA / PulseAudio
 * on Linux — and it is MIT-licensed, so it clears the licence filter.
 *
 * Everything *around* PortAudio (the ring buffer, the format
 * negotiation, the device-loss path, the statistics) is portable and
 * lives in this module, so a host that wants a different backend only
 * has to implement the seam.
 *
 * ## What this module does NOT do
 *
 * It does not resample.  The engine's audio plan (DEC-3) forbids a
 * silent rate change: a device whose native rate differs from the
 * negotiated codec rate is an error the *host* has to resolve (the
 * resampler is a separate work item, F5).  The module therefore
 * negotiates the rate with the device and reports what it got.
 *
 * It does not touch the engine.  The engine's PCM contract
 * (`send_audio()` / `on_audio_frame()`) is unchanged; a host wires the
 * device to the engine by hand.
 *
 * @note Remote-desktop plan F2 (M2-a).
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nimrtc/plugins/audio_device.hpp>
#include <nimrtc/plugins/audio_sink.hpp>
#include <nimrtc/plugins/audio_source.hpp>

// The resampler lives in its own header, but it is included here so a
// host that only knows the module's public API can still reach it (and
// can ask which backend it got).
#include <nimrtc/audio_device/resampler.hpp>

// The playback-progress bookkeeping and the AEC-delay estimator live in
// their own headers too; they are included here for the same reason.
#include <nimrtc/audio_device/playback_progress.hpp>
#include <nimrtc/audio_device/aec_delay.hpp>

#include <optional>

namespace nimrtc::audio_device {

// ---------------------------------------------------------------------------
// Backend availability
// ---------------------------------------------------------------------------

/**
 * @brief Whether the PortAudio backend is compiled in and usable.
 *
 * `false` when the vendored PortAudio source is not available, or when
 * the platform offers no PortAudio host API at all.  The module still
 * works in that case — the synthetic backend is always available.
 */
[[nodiscard]] NIMRTC_API_STABLE bool portaudio_available() noexcept;

/**
 * @brief The PortAudio version string, e.g. "PortAudio V19.8.0-devel".
 *
 * Empty when the PortAudio backend is not compiled in.
 */
[[nodiscard]] NIMRTC_API_STABLE std::string portaudio_version() noexcept;

/**
 * @brief The host APIs PortAudio found on this machine.
 *
 * The names are PortAudio's own ("WASAPI", "MME", "DirectSound",
 * "Core Audio", "ALSA", "PulseAudio", ...).  Empty when PortAudio is
 * not compiled in.
 */
[[nodiscard]] NIMRTC_API_STABLE std::vector<std::string>
portaudio_host_apis() noexcept;

// ---------------------------------------------------------------------------
// Resampler availability (remote-desktop plan F5)
// ---------------------------------------------------------------------------
//
// The resampler itself lives in its own header (`resampler.hpp`), which
// is included here so a host that only knows the module's public API
// can still ask which backend it got:
//
//   resampler_available()  — is the libsamplerate backend compiled in?
//   resampler_backend()    — "libsamplerate" or "linear"
//   Resampler              — the explicit sample-rate converter itself
//
// `false` / `"linear"` means the module's pure-C++ linear-interpolation
// fallback is in use.  The fallback is a *fallback*: it is not
// band-limited, so it aliases above the Nyquist frequency of the lower
// of the two rates.  A host that cares can say so in its own UI — the
// conversion is never silent (the engine's audio plan DEC-3).
//
// The synthetic device is always available, and the resampler is always
// *usable* — only its quality changes.

// ---------------------------------------------------------------------------
// Playback progress and the AEC delay (remote-desktop plans F3 / F4)
// ---------------------------------------------------------------------------
//
// The playback-progress tracker and the AEC-delay estimator live in
// their own headers (`playback_progress.hpp`, `aec_delay.hpp`), which
// are included above so a host that only knows the module's public API
// can still reach them.
//
// The module's *own* sinks keep a tracker and a delay monitor alive
// and report them through the accessors below:
//
//   playback_progress_tracker(sink) — the sink's tracker
//   played_frames(sink)             — the frames the speaker consumed
//   aec_delay_ms(sink)              — the measured loudspeaker ->
//                                     microphone loop delay, in ms
//   aec_delay_headroom_ms(sink)     — the value to hand the 3A as
//                                     `aec_delay_headroom_ms`
//
// The two AEC accessors report `nullopt` when there is no trusted
// estimate yet (too little data, or a correlation below the confidence
// threshold).  A host that needs the *configured* delay instead of the
// measured one configures it on the estimator and attaches the monitor
// to the sink.
//
// The accessors take the sink (or source) as an argument rather than
// reaching for a module-global instance: the module exports no global
// mutable state (L6), so a host with two sinks can tell them apart.
//
// A sink or source that is NOT one of the module's own (a third-party
// implementation) simply does not implement the module's seams; the
// accessors then report "no estimate" / "no tracker", which is honest
// rather than wrong.
//
// The accessors are safe to call concurrently with the device threads.

// ---------------------------------------------------------------------------
// PortAudio factories
// ---------------------------------------------------------------------------

/**
 * @brief Factory for the PortAudio-backed capture device.
 *
 * The factory is stateless — it is safe to create several, and safe to
 * keep one for the lifetime of the process.  Each `create()` call
 * returns a fresh, independent instance.
 */
class NIMRTC_API_STABLE PortAudioSourceFactory final
    : public plugins::IAudioSourceFactory {
public:
    [[nodiscard]] std::string_view id() const noexcept override {
        return "portaudio";
    }

    [[nodiscard]] std::string_view display_name() const noexcept override {
        return "PortAudio capture device (WASAPI / Core Audio / ALSA / PulseAudio)";
    }

    [[nodiscard]] plugins::IAudioSource* create(
        plugins::AudioSourceConfig cfg) const override;

    /**
     * The device ids this factory can open.
     *
     * The ids are PortAudio device indices rendered as strings
     * ("0", "1", ...).  They are stable for as long as the device
     * enumeration does not change (i.e. until something is plugged in
     * or out).
     */
    [[nodiscard]] std::vector<std::string> device_ids() const;
};

/**
 * @brief Factory for the PortAudio-backed playback device.
 */
class NIMRTC_API_STABLE PortAudioSinkFactory final
    : public plugins::IAudioSinkFactory {
public:
    [[nodiscard]] std::string_view id() const noexcept override {
        return "portaudio";
    }

    [[nodiscard]] std::string_view display_name() const noexcept override {
        return "PortAudio playback device (WASAPI / Core Audio / ALSA / PulseAudio)";
    }

    [[nodiscard]] plugins::IAudioSink* create(
        plugins::AudioSinkConfig cfg) const override;

    /** The device ids this factory can open. */
    [[nodiscard]] std::vector<std::string> device_ids() const;
};

// ---------------------------------------------------------------------------
// Synthetic (null) factories
// ---------------------------------------------------------------------------

/**
 * @brief Factory for the synthetic capture device.
 *
 * The synthetic device produces a deterministic tone; it needs no sound
 * card, no permission prompt and no driver.  It is what makes the
 * module's tests runnable on a CI machine with no audio hardware.
 */
class NIMRTC_API_STABLE SyntheticSourceFactory final
    : public plugins::IAudioSourceFactory {
public:
    [[nodiscard]] std::string_view id() const noexcept override {
        return "synthetic";
    }

    [[nodiscard]] std::string_view display_name() const noexcept override {
        return "Synthetic capture device (CI-testable)";
    }

    [[nodiscard]] plugins::IAudioSource* create(
        plugins::AudioSourceConfig cfg) const override;
};

/**
 * @brief Factory for the synthetic playback device.
 */
class NIMRTC_API_STABLE SyntheticSinkFactory final
    : public plugins::IAudioSinkFactory {
public:
    [[nodiscard]] std::string_view id() const noexcept override {
        return "synthetic";
    }

    [[nodiscard]] std::string_view display_name() const noexcept override {
        return "Synthetic playback device (CI-testable)";
    }

    [[nodiscard]] plugins::IAudioSink* create(
        plugins::AudioSinkConfig cfg) const override;
};

// ---------------------------------------------------------------------------
// Convenience entry points
// ---------------------------------------------------------------------------
/**
 * @brief Enumerate the capture devices PortAudio can see.
 *
 * An empty list is a legitimate answer (a machine with no sound card).
 * When the PortAudio backend is not compiled in, the list is empty too.
 */
[[nodiscard]] NIMRTC_API_STABLE std::vector<plugins::AudioDevice>
enumerate_capture_devices() noexcept;

/**
 * @brief Enumerate the playback devices PortAudio can see.
 */
[[nodiscard]] NIMRTC_API_STABLE std::vector<plugins::AudioDevice>
enumerate_playback_devices() noexcept;

// ---------------------------------------------------------------------------
// Backend lookup
// ---------------------------------------------------------------------------

/**
 * @brief The capture backends this build offers.
 *
 * The ids are the same strings the factories report: "portaudio" (when
 * the PortAudio backend is compiled in) and "synthetic" (always).
 */
[[nodiscard]] NIMRTC_API_STABLE std::vector<std::string>
available_source_backends() noexcept;

/** The playback backends this build offers. */
[[nodiscard]] NIMRTC_API_STABLE std::vector<std::string>
available_sink_backends() noexcept;

/**
 * @brief Look up a capture backend by id.
 *
 * @return The factory, or nullptr when no such backend is compiled in.
 *         The returned pointer is to a `const` object with static
 *         storage duration — it stays valid for the lifetime of the
 *         process, and the caller must not delete it.
 */
[[nodiscard]] NIMRTC_API_STABLE const plugins::IAudioSourceFactory*
source_factory(std::string_view id) noexcept;

/**
 * @brief Look up a playback backend by id.
 *
 * @return The factory, or nullptr when no such backend is compiled in.
 */
[[nodiscard]] NIMRTC_API_STABLE const plugins::IAudioSinkFactory*
sink_factory(std::string_view id) noexcept;

// ---------------------------------------------------------------------------
// Playback progress (remote-desktop plan F3)
// ---------------------------------------------------------------------------

/**
 * @brief The playback-progress seam a module sink implements.
 *
 * The module's own sinks (the PortAudio sink and the synthetic sink)
 * each own a `PlaybackProgressTracker`, which reports how many frames
 * the sink accepted and how many the device actually consumed.  A host
 * that wants those counters reaches them through this seam.
 *
 * A sink that is NOT one of the module's own (a third-party sink) does
 * not implement this interface; the module's accessors then report
 * "no tracker" rather than a wrong number.
 */
class NIMRTC_API_STABLE IPlaybackProgressSink {
public:
    virtual ~IPlaybackProgressSink() = default;

    /** The sink's playback-progress tracker. */
    [[nodiscard]] virtual const PlaybackProgressTracker&
    playback_progress() const noexcept = 0;
};

/**
 * @brief The playback-progress tracker of @p sink.
 *
 * @return The tracker, or nullptr when @p sink does not implement the
 *         module's progress seam.
 */
[[nodiscard]] NIMRTC_API_STABLE const PlaybackProgressTracker*
playback_progress_tracker(const plugins::IAudioSink* sink) noexcept;

/**
 * @brief The number of frames @p sink's loudspeaker has consumed.
 *
 * The cumulative played-out frame count.  It is monotonic: it never
 * goes backwards and never counts a frame the device did not consume.
 * This is the number the host hands the engine's
 * `set_render_delivered()` path so the 3A's AEC reference tracks the
 * loudspeaker.
 *
 * @return 0 when @p sink does not implement the module's progress seam.
 */
[[nodiscard]] NIMRTC_API_STABLE std::uint64_t
played_frames(const plugins::IAudioSink* sink) noexcept;

/**
 * @brief The queued-but-not-yet-played backlog of @p sink.
 *
 * The number of frames the sink accepted but the device has not
 * consumed yet.  A large backlog means the AEC reference the 3A sees
 * is stale by that much, so the measured loop delay has to include it.
 *
 * @return 0 when @p sink does not implement the module's progress seam.
 */
[[nodiscard]] NIMRTC_API_STABLE std::size_t
queued_frames(const plugins::IAudioSink* sink) noexcept;

// ---------------------------------------------------------------------------
// The AEC delay (remote-desktop plan F4)
// ---------------------------------------------------------------------------

/**
 * @brief Attach an AEC-delay monitor to @p sink.
 *
 * The monitor is fed with the signal that reached the loudspeaker, so
 * the estimator can correlate it against the microphone capture and
 * find the loop delay.
 *
 * @return false when @p sink does not implement the module's AEC-delay
 *         seam.
 */
[[nodiscard]] NIMRTC_API_STABLE bool
attach_aec_delay_monitor(plugins::IAudioSink* sink,
                          AecDelayMonitor* monitor) noexcept;

/**
 * @brief Attach an AEC-delay monitor to @p source.
 *
 * The monitor is fed with the signal the microphone captured, which is
 * the *other* half of the loop the estimator correlates.
 *
 * @return false when @p source does not implement the module's
 *         AEC-delay seam.
 */
[[nodiscard]] NIMRTC_API_STABLE bool
attach_aec_delay_monitor(plugins::IAudioSource* source,
                          AecDelayMonitor* monitor) noexcept;

/**
 * @brief The measured loudspeaker -> microphone loop delay, in ms.
 *
 * `nullopt` when there is no *trusted* estimate yet: the estimator
 * has not seen enough correlated data, or the correlation peak was
 * below the confidence threshold.  The estimator never guesses.
 */
[[nodiscard]] NIMRTC_API_STABLE std::optional<std::int32_t>
aec_delay_ms(const plugins::IAudioSink* sink) noexcept;

/**
 * @brief The value the host should hand the 3A as
 *        `aec_delay_headroom_ms`.
 *
 * The measured delay plus the configured headroom (the playout backlog
 * and the capture-side buffering the measurement does not cover).
 *
 * `nullopt` when there is no trusted estimate yet.
 */
[[nodiscard]] NIMRTC_API_STABLE std::optional<std::int32_t>
aec_delay_headroom_ms(const plugins::IAudioSink* sink) noexcept;

/**
 * @brief The confidence of the last correlation run, in [0, 1].
 *
 * 1.0 means a perfect match; 0 means "no correlation found" (or "no
 * run yet").  A host that wants to be strict can refuse an estimate
 * whose confidence is below its own threshold.
 */
[[nodiscard]] NIMRTC_API_STABLE float
aec_delay_confidence(const plugins::IAudioSink* sink) noexcept;

} // namespace nimrtc::audio_device
