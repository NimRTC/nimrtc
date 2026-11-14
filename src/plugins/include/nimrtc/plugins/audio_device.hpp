/**
 * @file nimrtc/plugins/audio_device.hpp
 * @brief The shared audio-device vocabulary (format, capabilities, devices).
 *
 * The audio *device* layer (remote-desktop plan F1 / F2) needs a small
 * vocabulary that both the capture side and the playback side speak:
 * the PCM format a device is asked to run at, what a device can do, and
 * which devices exist at all.
 *
 * It lives in its own header because both `audio_source.hpp` and
 * `audio_sink.hpp` need it, and because neither of them is "the parent"
 * of the other: a host may want to enumerate devices without ever
 * opening one, and a device implementation may support only capture or
 * only playback.
 *
 * ## Relationship to the engine's PCM contract
 *
 * The engine's existing PCM contract (`send_audio()` / `on_audio_frame()`)
 * is **unchanged** by this seam.  The device layer speaks the same
 * interleaved-float PCM, so a host can pipe a capture device straight
 * into `send_audio()` and a decoded frame straight into a sink without
 * a conversion layer.  What the seam adds on top is the *device* half:
 * enumeration, format negotiation and capabilities.
 *
 * ## What is NOT here
 *
 * No OS headers, no PortAudio types, no sample buffers.  The device
 * describes its own output through `AudioDeviceFormat` and the
 * implementation translates.
 *
 * @note Remote-desktop plan F1 (M2-a). Interface is stable.
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include <nimrtc/core/api_version.hpp>
#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_AUDIO_DEVICE_HPP
#define NIMRTC_PLUGINS_AUDIO_DEVICE_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// PCM format
// ---------------------------------------------------------------------------

/** The sample encoding a device is asked to run at.
 *
 *  The engine's PCM contract is interleaved float in [-1.0, 1.0]; a
 *  device that can only deliver `kInt16` is converted by the device
 *  layer, so the host never sees the conversion. */
enum class NIMRTC_API_STABLE AudioSampleFormat : std::uint8_t {
    kUnknown = 0,
    kFloat32 = 1,  ///< interleaved float, [-1.0, 1.0] (the engine's format)
    kInt16   = 2,  ///< interleaved signed 16-bit PCM
};

/** Human-readable sample-format name (for logging / UI). */
inline NIMRTC_API_STABLE std::string_view audio_sample_format_name(
        AudioSampleFormat f) noexcept {
    switch (f) {
        case AudioSampleFormat::kFloat32: return "float32";
        case AudioSampleFormat::kInt16:   return "int16";
        default:                          return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Format negotiation
// ---------------------------------------------------------------------------

/**
 * @brief The PCM format a device is asked to run at.
 *
 * A zero field means "the device's default" — the negotiation is a
 * *request*, and the caller reads back the format actually in force
 * after `open()` (a device may have to fall back to a different rate).
 */
struct NIMRTC_API_STABLE AudioDeviceFormat {
    /** Sample rate in Hz. 0 = device default (usually 48 000). */
    std::uint32_t sample_rate_hz = 0;

    /** Channel count (1 = mono, 2 = stereo). 0 = device default. */
    std::uint16_t channels = 0;

    /** Frames per buffer (the device's internal quantum). 0 = device
     *  default; the value in force is reported back after `open()`. */
    std::uint32_t frames_per_buffer = 0;

    /** The sample encoding. `kUnknown` = the device's default (float32
     *  on every backend NimRTC ships). */
    AudioSampleFormat format = AudioSampleFormat::kFloat32;
};

// ---------------------------------------------------------------------------
// Device identity
// ---------------------------------------------------------------------------

/** One audio device (an input, an output, or a duplex endpoint). */
struct NIMRTC_API_STABLE AudioDevice {
    /** Stable id within the backend, e.g. "portaudio:3" or
     *  "synthetic:0".  Opaque to the engine — it is passed back to
     *  `open()` verbatim. */
    std::string id;

    /** Human-readable name, e.g. "Speakers (Realtek Audio)". */
    std::string name;

    /** The device can capture. */
    bool can_capture = false;

    /** The device can play back. */
    bool can_playback = false;

    /** Maximum input channel count (0 = the device has no input). */
    std::uint16_t max_input_channels = 0;

    /** Maximum output channel count (0 = the device has no output). */
    std::uint16_t max_output_channels = 0;

    /** The device's native / default sample rate in Hz. */
    std::uint32_t default_sample_rate_hz = 0;
};

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------

/**
 * @brief What an audio-device backend can do.
 *
 * Queried before the device is opened, so a host can pick a backend and
 * a format *before* the first sample is captured or played.
 *
 * Every field is a **backend-wide** statement:
 *
 *   - `capture` / `playback` — the backend implements the capture side /
 *     the playback side at all.  A capture-only backend reports
 *     `playback == false`, and asking it for a sink is refused at
 *     `open()` time rather than failing silently.
 *   - `enumeration` — the device list is real (not hard-coded).  The
 *     synthetic backend reports `false`: its list is fixed, and saying
 *     so is more honest than pretending.
 *   - `hot_plug` — the backend notices a device being unplugged and
 *     surfaces it as a clean error instead of a crash.
 *   - `max_sample_rate_hz` — the highest rate the backend can run at
 *     (0 = unbounded).
 *   - `min_frames_per_buffer` / `max_frames_per_buffer` — the quantum
 *     range the backend accepts (0 = unbounded / backend default).
 *   - `backend` — the backend identifier, e.g. "portaudio", "synthetic".
 */
struct NIMRTC_API_STABLE AudioDeviceCapabilities {
    /** The backend implements the capture side. */
    bool capture = false;

    /** The backend implements the playback side. */
    bool playback = false;

    /** The device list is enumerated from the platform. */
    bool enumeration = false;

    /** The backend detects device loss (hot-unplug). */
    bool hot_plug = false;

    /** Highest sample rate the backend can run at (0 = unbounded). */
    std::uint32_t max_sample_rate_hz = 0;

    /** Smallest quantum the backend accepts (0 = backend default). */
    std::uint32_t min_frames_per_buffer = 0;

    /** Largest quantum the backend accepts (0 = unbounded). */
    std::uint32_t max_frames_per_buffer = 0;

    /** Backend identifier, e.g. "portaudio", "synthetic". */
    std::string backend;
};

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

/** Device-side counters, surfaced through the engine's stats snapshot. */
struct NIMRTC_API_STABLE AudioDeviceStats {
    /** Frames delivered to the consumer (capture) or handed to the
     *  device (playback). */
    std::uint64_t frames_processed = 0;

    /** Frames dropped because a buffer was full. */
    std::uint64_t frames_dropped = 0;

    /** Underruns: the playback side wanted samples and the queue was
     *  empty, so silence was played. */
    std::uint64_t underruns = 0;

    /** Overruns: the capture side produced samples and the queue was
     *  full, so the oldest samples were dropped. */
    std::uint64_t overruns = 0;

    /** The device reported an error (device lost, driver failure). */
    std::uint64_t device_errors = 0;

    /** Times the device was lost and had to be reopened. */
    std::uint64_t device_restarts = 0;

    /** Bytes copied between the device and the engine's buffers. */
    std::uint64_t bytes_copied = 0;

    /** The device is currently lost (unplugged / disabled). */
    bool device_lost = false;
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_AUDIO_DEVICE_HPP
