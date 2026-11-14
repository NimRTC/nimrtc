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

} // namespace nimrtc::audio_device
