/**
 * @file nimrtc/plugins/audio_sink.hpp
 * @brief IAudioSink — pluggable audio *playback* device interface.
 *
 * The engine's existing PCM contract (`on_audio_frame()`) hands decoded
 * PCM to the host.  That contract is unchanged by this seam: what the
 * seam adds is the *device* half — the loudspeaker.
 *
 * @code
 *   decoded PCM  ──> engine.set_on_audio_frame(cb)  ──> IAudioSink::write()
 * @endcode
 *
 * ## Playback progress (remote-desktop plan F3's substrate)
 *
 * A sink knows **how many samples actually reached the device**.  That
 * number is what an acoustic echo canceller needs: the AEC reference
 * path must know where in the *played-out* signal the capture sits, or
 * the reference is misaligned by the buffer + driver latency.
 *
 * Two ways to get it:
 *
 *   - `played_samples()` — the cumulative count of samples the device
 *     has consumed.  It is monotonic; a host polls it.
 *   - `set_on_playback_progress(cb)` — the callback fires whenever the
 *     device consumed a block, carrying the new cumulative count.  A
 *     host that wants the number pushed (rather than polled) uses this.
 *
 * The engine's own `set_render_delivered()` path (the 3A reference) is
 * wired to this by the *host* — the device layer never reaches into
 * the engine.
 *
 * ## Queueing
 *
 * A sink buffers: `write()` hands samples over, the device thread pulls
 * them at the device's pace.  The buffer is bounded — when the device
 * is slower than the producer, the *oldest* samples are dropped (a
 * loudspeaker that plays stale audio is worse than one that skips).
 * The drop is counted in `stats().frames_dropped`.
 *
 * ## Device loss
 *
 * A speaker that is unplugged mid-call is a *normal* event, not a
 * crash.  A sink that notices it reports it through `stats()` (the
 * `device_lost` flag and the `device_errors` counter) and stops
 * consuming — it does not spin and it does not abort the process.
 *
 * @note Remote-desktop plan F1 (M2-a). Interface is stable.
 * @see audio_source.hpp for the capture counterpart.
 * @see audio_device.hpp for the shared device vocabulary.
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include <nimrtc/core/api_version.hpp>
#include "nimrtc/plugins/base.hpp"
#include "nimrtc/plugins/audio_device.hpp"

#ifndef NIMRTC_PLUGINS_AUDIO_SINK_HPP
#define NIMRTC_PLUGINS_AUDIO_SINK_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/** How a playback instance is configured. */
struct NIMRTC_API_STABLE AudioSinkConfig {
    /** The device to open. Left empty, the backend picks its default
     *  output device. */
    std::string device_id;

    /** The format to negotiate. Zero fields mean "the device's
     *  default". */
    AudioDeviceFormat format;

    /** Maximum frames the device queue may hold. 0 = backend
     *  default.  When the queue is full the *oldest* frames are
     *  dropped, so the loudspeaker plays the newest audio. */
    std::uint32_t queue_frames = 0;

    /** Human-readable sink identifier (e.g. "speakers", "null").
     *  Used for logging. */
    std::string label;
};

// ---------------------------------------------------------------------------
// Playback progress
// ---------------------------------------------------------------------------

/**
 * @brief Playback-progress callback.
 *
 * Invoked on the device's own thread after the device consumed a block
 * of samples.  @p played_samples is the *cumulative* number of samples
 * the device has consumed since the stream was opened — it never goes
 * backwards.
 */
using NIMRTC_API_STABLE AudioPlaybackCallback =
    std::function<void(std::uint64_t played_samples)>;

// ---------------------------------------------------------------------------
// IAudioSink
// ---------------------------------------------------------------------------

/**
 * @brief An audio *playback* device (a loudspeaker).
 *
 * Thread safety: the enumeration and query methods are const and safe
 * to call concurrently; `open()` / `close()` / `write()` must be
 * serialized externally.
 */
class NIMRTC_API_STABLE IAudioSink : public IPlugin {
public:
    // ---- Enumeration ------------------------------------------------------

    /** List the devices this backend can play back through.
     *
     *  An empty list is a legitimate answer (a CI machine with no
     *  sound card) — it is not an error. */
    [[nodiscard]] virtual std::vector<AudioDevice> enumerate_devices()
        const noexcept = 0;

    /** The formats @p device can play at. */
    [[nodiscard]] virtual std::vector<AudioDeviceFormat> supported_formats(
        const std::string& device_id) const noexcept = 0;

    /** What this backend can do. */
    [[nodiscard]] virtual AudioDeviceCapabilities capabilities()
        const noexcept = 0;

    // ---- Lifecycle --------------------------------------------------------

    /**
     * Open the device and begin playing.
     *
     * The configuration given to the factory at creation time is the
     * *request*; after `open()` succeeds, `config()` reports the format
     * that was actually negotiated — the caller must read it back rather
     * than assume the request was granted.
     */
    virtual Status open() noexcept override = 0;

    /** Stop playing and release the device. Idempotent. */
    virtual void close() noexcept override = 0;

    /** True iff the device is currently open. */
    [[nodiscard]] virtual bool is_open() const noexcept = 0;

    /** The configuration actually in force (post-negotiation). */
    [[nodiscard]] virtual AudioSinkConfig config() const noexcept = 0;

    // ---- Playback ---------------------------------------------------------

    /**
     * Hand samples to the device.
     *
     * @param samples     Interleaved float PCM, [-1.0, 1.0].
     * @param num_samples Samples *per channel*; the buffer holds
     *                    `num_samples * channels` floats.
     * @return kOk when the samples were accepted (queued or played);
     *         `kErrInvalidParam` on a malformed call; `kErrClosed`
     *         when the device is not open.
     */
    virtual Status write(const float* samples,
                         std::size_t num_samples,
                         std::uint16_t channels) noexcept = 0;

    /**
     * The number of samples the device has actually played out.
     *
     * This is the **cumulative** count since the stream was opened, and
     * it is monotonic: it never goes backwards and never counts a
     * sample the device did not consume.  A host that wires this to the
     * engine's `set_render_delivered()` gives the 3A an aligned AEC
     * reference.
     */
    [[nodiscard]] virtual std::uint64_t played_samples() const noexcept = 0;

    /**
     * Install the playback-progress callback.
     *
     * Optional: a host that polls `played_samples()` need not set it.
     * The callback fires on the device's own thread and must not
     * block.
     */
    virtual void set_on_playback_progress(AudioPlaybackCallback cb) noexcept = 0;

    /**
     * The number of frames *queued* for playback but not yet consumed.
     *
     * Additive (remote-desktop plan F3).  A host that drives the AEC
     * reference needs to know how far behind the loudspeaker is: a
     * large backlog means the reference the 3A sees is stale by that
     * much, and the measured loop delay must include it.
     *
     * A sink that cannot report a backlog (a device that plays what it
     * is given, synchronously) returns 0.
     */
    [[nodiscard]] virtual std::size_t queued_frames() const noexcept {
        return 0;
    }

    /**
     * The monotonic instant (microseconds) at which the device consumed
     * its most recent block.
     *
     * Additive (remote-desktop plan F3).  0 when nothing has been
     * played yet.
     */
    [[nodiscard]] virtual std::int64_t last_playback_us() const noexcept {
        return 0;
    }

    /** Counters. */
    [[nodiscard]] virtual AudioDeviceStats stats() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/** Factory for playback-device plugin instances. */
class NIMRTC_API_STABLE IAudioSinkFactory {
public:
    virtual ~IAudioSinkFactory() = default;

    /** Unique identifier, e.g. "portaudio", "synthetic". */
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;

    /** Short human-readable name. */
    [[nodiscard]] virtual std::string_view display_name() const noexcept = 0;

    /** Create a playback instance. The returned instance is in
     *  kConstructed state; the caller calls `open()`. */
    [[nodiscard]] virtual IAudioSink* create(AudioSinkConfig cfg) const = 0;
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_AUDIO_SINK_HPP
