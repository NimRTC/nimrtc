// audio_device/src/system_audio_platform.hpp
// The *private* platform seam behind the system-audio factory.
//
// This header is NOT public: it lives under `src/`, not under
// `include/nimrtc/`, so it never appears in the installed API surface
// (and never in the API-freeze sweep).  It exists so the portable
// factory in `system_audio.cpp` and the platform backends can talk to
// each other without dragging the platform headers into the public
// header.
//
// The contract is deliberately small: a probe (what the platform can
// do, and why not) and a create (open a device).

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nimrtc/audio_device/system_audio.hpp>
#include <nimrtc/plugins/audio_source.hpp>

namespace nimrtc::audio_device::detail {

/** One platform-level system-audio device. */
struct PlatformSystemAudioDevice {
    /** The device id in the `system-audio:` namespace. */
    std::string id;

    /** Human-readable name. */
    std::string name;

    /** The kind of loopback. */
    SystemAudioDeviceKind kind = SystemAudioDeviceKind::kLoopback;

    /** The channel count the device delivers (0 = unknown). */
    std::uint16_t channels = 0;

    /** The rate the device delivers, in Hz (0 = unknown). */
    std::uint32_t sample_rate_hz = 0;
};

/** What the platform can do, and — when it cannot — why. */
struct PlatformSystemAudioInfo {
    /** The mechanism that implements (or would implement) the
     *  loopback. */
    SystemAudioBackend backend = SystemAudioBackend::kUnavailable;

    /** The status code. */
    SystemAudioStatus status = SystemAudioStatus::kUnsupported;

    /** The human-readable reason (never empty when the status is not
     *  `kAvailable`). */
    std::string reason;

    /** The enumerated devices. */
    std::vector<PlatformSystemAudioDevice> devices;
};

/**
 * Probe the platform for system-audio (loopback) support.
 *
 * The probe NEVER fails: it returns a status and, when the status is
 * not `kAvailable`, an explicit reason.
 */
PlatformSystemAudioInfo probe_platform_system_audio();

/**
 * Open a platform system-audio device.
 *
 * @param cfg   The requested configuration.
 * @param error Receives the human-readable reason when the call
 *              returns nullptr.
 * @return The opened source, or nullptr on failure.  A nullptr is
 *         always accompanied by an explicit @p error — never silence.
 */
std::unique_ptr<plugins::IAudioSource> create_platform_system_audio(
    const plugins::AudioSourceConfig& cfg,
    std::string& error);

} // namespace nimrtc::audio_device::detail
