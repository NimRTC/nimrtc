/**
 * @file nimrtc/plugins/audio_source.hpp
 * @brief IAudioSource — pluggable audio *capture* device interface.
 *
 * The engine's existing PCM contract (`send_audio()`) takes interleaved
 * float PCM from the host.  That contract is unchanged by this seam:
 * what the seam adds is the *device* half — the microphone.
 *
 * @code
 *   IAudioSource  ──device enumeration, negotiation, capabilities
 *        │
 *        ▼
 *   PCM frames  ──> the host's own capture loop  ──> engine.send_audio()
 * @endcode
 *
 * A capture device is *push*: the OS hands the plugin a buffer on the
 * device's own thread.  The plugin therefore hands the frames to the
 * host through a callback (the primary path) and keeps a bounded queue
 * so a host that prefers to *pull* can do that too.
 *
 * ## Two delivery paths
 *
 *   - the **callback** path: `set_on_audio_frame(cb)` fires on the
 *     plugin's device thread with a borrowed view of the frame.  The
 *     callback must not block.
 *   - the **pull** path: `read_samples()` pops the next frame from the
 *     bounded queue.  A host that wants to run the capture on *its*
 *     thread (so the device thread never runs host code) uses this.
 *
 * A device implementation may support either or both; the pull path is
 * optional (a device that only pushes returns `kErrUnsupported` from
 * `read_samples()`).
 *
 * ## Device loss
 *
 * A microphone that is unplugged mid-call is a *normal* event, not a
 * crash.  A device that notices it reports it through `stats()` (the
 * `device_lost` flag and the `device_errors` counter) and stops
 * delivering frames — it does not spin and it does not abort the
 * process.
 *
 * ## What is NOT here
 *
 * No OS headers, no PortAudio types.  The device describes its output
 * through `AudioDeviceFormat` and the implementation translates.
 *
 * @note Remote-desktop plan F1 (M2-a). Interface is stable.
 * @see audio_sink.hpp for the playback counterpart.
 * @see audio_device.hpp for the shared device vocabulary.
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include <nimrtc/core/api_version.hpp>
#include "nimrtc/plugins/base.hpp"
#include "nimrtc/plugins/audio_device.hpp"

#ifndef NIMRTC_PLUGINS_AUDIO_SOURCE_HPP
#define NIMRTC_PLUGINS_AUDIO_SOURCE_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

/**
 * @brief One captured frame of PCM.
 *
 * The `samples` pointer is owned by the capture instance and stays valid
 * **until the callback returns** (callback path) or until the next
 * `read_samples()` call (pull path).  A consumer that wants to keep the
 * samples must copy them.
 *
 * The samples are **interleaved**: `num_samples` is the count *per
 * channel*, so the buffer holds `num_samples * num_channels` floats.
 */
struct NIMRTC_API_STABLE AudioFrame {
    /** Pointer to the interleaved float PCM, [-1.0, 1.0].
     *  Null when the frame carries only metadata. */
    const float* samples = nullptr;

    /** Samples per channel. */
    std::size_t num_samples = 0;

    /** Channel count. */
    std::uint16_t num_channels = 0;

    /** The rate the samples were captured at. */
    std::uint32_t sample_rate_hz = 0;

    /** Monotonic capture timestamp, microseconds. */
    std::int64_t capture_ts_us = 0;

    /** Per-device frame sequence number (strictly increasing). */
    std::uint64_t frame_seq = 0;
};

/**
 * @brief Frame-delivery callback.
 *
 * Invoked on the capture device's own thread when a frame arrives.  It
 * must not block: the implementation enqueues and returns.
 */
using NIMRTC_API_STABLE AudioFrameCallback =
    std::function<void(const AudioFrame& frame)>;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/** How a capture instance is configured. */
struct NIMRTC_API_STABLE AudioSourceConfig {
    /** The device to open. Left empty, the backend picks its default
     *  input device. */
    std::string device_id;

    /** The format to negotiate. Zero fields mean "the device's
     *  default". */
    AudioDeviceFormat format;

    /** Maximum frames held in the bounded queue (the pull path's
     *  buffer). 0 = backend default. */
    std::uint32_t queue_frames = 0;
};

// ---------------------------------------------------------------------------
// IAudioSource
// ---------------------------------------------------------------------------

/**
 * @brief An audio *capture* device (a microphone).
 *
 * Thread safety: the enumeration and query methods are const and safe
 * to call concurrently; `open()` / `close()` must be serialized
 * externally.  `read_samples()` may be called from the host's own
 * thread while the device thread pushes frames.
 */
class NIMRTC_API_STABLE IAudioSource : public IPlugin {
public:
    // ---- Enumeration ------------------------------------------------------

    /** List the devices this backend can capture from.
     *
     *  An empty list is a legitimate answer (a CI machine with no
     *  sound card) — it is not an error. */
    [[nodiscard]] virtual std::vector<AudioDevice> enumerate_devices()
        const noexcept = 0;

    /** The formats @p device can capture at. */
    [[nodiscard]] virtual std::vector<AudioDeviceFormat> supported_formats(
        const std::string& device_id) const noexcept = 0;

    /** What this backend can do. */
    [[nodiscard]] virtual AudioDeviceCapabilities capabilities()
        const noexcept = 0;

    // ---- Lifecycle --------------------------------------------------------

    /**
     * Open the device and begin capturing.
     *
     * The configuration given to the factory at creation time is the
     * *request*; after `open()` succeeds, `config()` reports the format
     * that was actually negotiated — the caller must read it back rather
     * than assume the request was granted (a device may have to fall
     * back to a different rate or channel count).
     */
    virtual Status open() noexcept override = 0;

    /** Stop capturing and release the device. Idempotent. */
    virtual void close() noexcept override = 0;

    /** True iff the device is currently open. */
    [[nodiscard]] virtual bool is_open() const noexcept = 0;

    /** The configuration actually in force (post-negotiation). */
    [[nodiscard]] virtual AudioSourceConfig config() const noexcept = 0;

    // ---- Frame delivery ---------------------------------------------------

    /**
     * Install the frame-delivery callback.
     *
     * Optional: a consumer that only pulls via `read_samples()` need not
     * set it.  The callback fires on the device's own thread.
     */
    virtual void set_on_audio_frame(AudioFrameCallback cb) noexcept = 0;

    /**
     * Pull the next frame from the bounded queue.
     *
     * @param dst       Receives the interleaved float PCM.
     * @param capacity  The number of *samples per channel* @p dst can
     *                  hold.
     * @param out_frames Receives the number of samples per channel
     *                  actually written.
     * @return kOk when a frame was delivered; `kErrNotReady` when the
     *         queue is empty; `kErrUnsupported` when the device does not
     *         implement the pull path.
     */
    virtual Status read_samples(float* dst,
                                std::size_t capacity,
                                std::size_t* out_frames) noexcept = 0;

    /** Counters. */
    [[nodiscard]] virtual AudioDeviceStats stats() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/** Factory for capture-device plugin instances. */
class NIMRTC_API_STABLE IAudioSourceFactory {
public:
    virtual ~IAudioSourceFactory() = default;

    /** Unique identifier, e.g. "portaudio", "synthetic". */
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;

    /** Short human-readable name. */
    [[nodiscard]] virtual std::string_view display_name() const noexcept = 0;

    /** Create a capture instance. The returned instance is in
     *  kConstructed state; the caller calls `open()`. */
    [[nodiscard]] virtual IAudioSource* create(AudioSourceConfig cfg) const = 0;
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_AUDIO_SOURCE_HPP
