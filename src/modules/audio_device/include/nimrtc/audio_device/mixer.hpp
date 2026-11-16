/**
 * @file nimrtc/audio_device/mixer.hpp
 * @brief The two-source audio mixer (remote-desktop plan F6).
 *
 * ## Why a mixer at all
 *
 * A remote-desktop session that shares *the system's output* has **two**
 * audio sources, not one:
 *
 *   - the **microphone** (the operator's voice), and
 *   - the **system audio** (whatever the desktop is playing).
 *
 * The plan's F6 acceptance criterion says the two are "mixed or kept
 * separate by configuration".  This class is that configuration, made
 * **visible**: `AudioMixMode::kMix` produces one mixed stream, and
 * `AudioMixMode::kSeparate` produces two distinct streams that are
 * never combined.
 *
 * ## The mixing rule (deterministic)
 *
 * The mix is a **saturating add with headroom**:
 *
 * @code
 *   out = clamp(headroom * (mic_gain * mic + sys_gain * sys), -1, +1)
 * @endcode
 *
 * The arithmetic is fixed-point-free but *exactly reproducible*: the
 * operations happen in the stated order, in `float`, and the clamp is
 * the only non-linearity.  A test can therefore assert the exact mixed
 * sample values.
 *
 * The default headroom is 1.0 and the default gains are 1.0, so the
 * default mix is a plain saturating add.  A host that wants room for
 * the sum (because the microphone and the system audio can both be
 * loud) lowers the headroom — the *conversion is never silent*, so a
 * host can show "system audio attenuated by X dB" in its own UI.
 *
 * ## Different rates and channel counts
 *
 * The two inputs do **not** have to agree:
 *
 *   - a **rate** mismatch is converted with the module's existing
 *     `Resampler` (F5) — this module does NOT contain a second
 *     resampler;
 *   - a **channel-count** mismatch is converted explicitly: a stereo
 *     input is down-mixed to mono by averaging, a mono input is
 *     up-mixed to stereo by duplication, and an input with more than
 *     two channels is averaged down.
 *
 * The rate conversion happens on the **int16** domain (that is the
 * `Resampler`'s domain), so a converted stream carries the converter's
 * 16-bit quantisation.  A stream that is already at the output rate
 * goes through **no** conversion at all and stays bit-exact.
 *
 * ## Thread safety
 *
 * **This class is NOT thread-safe.**  One mixer belongs to one session;
 * the calls on it (`push_*`, `pull`) must be serialised externally.
 *
 * @note Remote-desktop plan F6 (M2-c).
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace nimrtc::audio_device {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/**
 * @brief How the two sources are combined.
 *
 * The mode is part of the **configuration**, not a runtime toggle of
 * an opaque pipeline: `AudioMixer::mode()` reports what is in force
 * and the statistics show which path produced the frames.
 */
enum class NIMRTC_API_STABLE AudioMixMode : std::uint8_t {
    /** The two sources are summed into ONE output stream. */
    kMix = 0,

    /** The two sources are kept as TWO distinct output streams. */
    kSeparate = 1,
};

/** Human-readable mode name (for logging / UI). */
[[nodiscard]] NIMRTC_API_STABLE std::string_view audio_mix_mode_name(
    AudioMixMode mode) noexcept;

/**
 * @brief The mixer's configuration.
 */
struct NIMRTC_API_STABLE AudioMixerConfig {
    /** How the two sources are combined. */
    AudioMixMode mode = AudioMixMode::kMix;

    /** The rate of the mixed output, in Hz.  A source that runs at a
     *  different rate is converted with the module's `Resampler`. */
    std::uint32_t output_rate_hz = 48000;

    /** The channel count of the output (1 = mono, 2 = stereo).
     *  0 is normalised to 1 (mono). */
    std::uint16_t output_channels = 1;

    /** The linear gain applied to the microphone leg. */
    float microphone_gain = 1.0f;

    /** The linear gain applied to the system-audio leg. */
    float system_gain = 1.0f;

    /** The headroom applied to the SUM of the two legs, before the
     *  saturating clamp.  1.0 = no attenuation. */
    float headroom = 1.0f;
};

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

/**
 * @brief The result of a mixer operation.
 */
enum class NIMRTC_API_STABLE AudioMixStatus : std::uint8_t {
    /** The operation succeeded. */
    kOk = 0,

    /** A null buffer with a non-zero frame count, or an invalid
     *  configuration. */
    kInvalidParam = 1,

    /** The pull had nothing to deliver (both sides were empty). */
    kNotReady = 2,

    /** The underlying rate conversion failed. */
    kInternalError = 3,
};

/** Human-readable status name (for logging / UI). */
[[nodiscard]] NIMRTC_API_STABLE std::string_view audio_mix_status_name(
    AudioMixStatus status) noexcept;

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

/**
 * @brief The mixer's counters.
 *
 * These are the numbers that make the configuration **visible**: a
 * host can tell "the two sources were mixed" from "the two sources
 * were kept apart" by looking at which counter advanced.
 */
struct NIMRTC_API_STABLE AudioMixerStats {
    /** Microphone frames handed to the mixer. */
    std::uint64_t microphone_frames_in = 0;

    /** System-audio frames handed to the mixer. */
    std::uint64_t system_frames_in = 0;

    /** Frames produced by the **mixed** output (the `kMix` path). */
    std::uint64_t mixed_frames_out = 0;

    /** Frames produced on the microphone leg alone (the `kSeparate`
     *  path, or the leg consumed by the mix). */
    std::uint64_t microphone_frames_out = 0;

    /** Frames produced on the system-audio leg alone (the `kSeparate`
     *  path, or the leg consumed by the mix). */
    std::uint64_t system_frames_out = 0;

    /** The number of frames that went through a rate conversion. */
    std::uint64_t resampled_frames = 0;

    /** True iff the microphone leg was rate-converted. */
    bool microphone_resampled = false;

    /** True iff the system-audio leg was rate-converted. */
    bool system_resampled = false;
};

// ---------------------------------------------------------------------------
// AudioMixer
// ---------------------------------------------------------------------------

/**
 * @brief Combines two audio sources — the microphone and the system
 *        audio — into one or two output streams.
 *
 * ## The block model
 *
 * The mixer is fed **blocks** and produces **blocks**:
 *
 *   1. the host pushes the microphone block and the system-audio block
 *      (`push_microphone()`, `push_system()`);
 *   2. the host pulls the output block (`pull()`).
 *
 * The two legs are aligned **by block order**, not by capture
 * timestamp: a block pushed as the Nth microphone block is mixed with
 * the Nth system-audio block.  A host that needs sample-accurate
 * alignment across two independent devices has to align the blocks
 * itself before pushing them.
 */
class NIMRTC_API_STABLE AudioMixer {
public:
    /**
     * Construct a mixer.
     *
     * An invalid configuration (a zero output rate, an output channel
     * count above 2) is NOT an exception: the mixer is constructed in a
     * failed state and every `push_*()` / `pull()` returns
     * `AudioMixStatus::kInvalidParam`.  Use `valid()` to check.
     */
    explicit AudioMixer(AudioMixerConfig cfg = {}) noexcept;

    ~AudioMixer();

    AudioMixer(const AudioMixer&)            = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;
    AudioMixer(AudioMixer&&) noexcept;
    AudioMixer& operator=(AudioMixer&&) noexcept;

    // ---- Configuration ----------------------------------------------------

    /** The configuration in force. */
    [[nodiscard]] const AudioMixerConfig& config() const noexcept;

    /** The mode in force (`kMix` or `kSeparate`). */
    [[nodiscard]] AudioMixMode mode() const noexcept;

    /**
     * Replace the configuration.
     *
     * The buffered audio is **dropped**: a rate or channel-count change
     * invalidates what is already buffered.
     */
    void set_config(AudioMixerConfig cfg) noexcept;

    /** True iff the configuration was accepted. */
    [[nodiscard]] bool valid() const noexcept;

    /** Drop the buffered audio and the counters' working state (the
     *  lifetime counters are kept). */
    void reset() noexcept;

    // ---- Feeding ----------------------------------------------------------

    /**
     * Feed a block of microphone PCM.
     *
     * @param samples        The interleaved float PCM.
     * @param num_frames     Samples per channel.
     * @param channels       The channel count of @p samples (1 or 2).
     * @param sample_rate_hz The rate @p samples were captured at.
     *
     * A rate mismatch is converted to the output rate with the
     * module's `Resampler`; a channel mismatch is converted to the
     * output channel count as described in the header.
     */
    AudioMixStatus push_microphone(const float* samples,
                                   std::size_t num_frames,
                                   std::uint16_t channels,
                                   std::uint32_t sample_rate_hz) noexcept;

    /** Feed a block of system-audio PCM (the second source). */
    AudioMixStatus push_system(const float* samples,
                               std::size_t num_frames,
                               std::uint16_t channels,
                               std::uint32_t sample_rate_hz) noexcept;

    // ---- Draining ---------------------------------------------------------

    /**
     * Pull the next output block.
     *
     * In `kMix` mode the mixed stream is written into @p mixed and
     * @p microphone / @p system are left empty.  In `kSeparate` mode
     * @p microphone and @p system receive the two legs and @p mixed is
     * left empty.
     *
     * @return `kOk` when a block was produced; `kNotReady` when both
     *         legs were empty.
     */
    AudioMixStatus pull(std::vector<float>& mixed,
                        std::vector<float>& microphone,
                        std::vector<float>& system) noexcept;

    // ---- Queries ----------------------------------------------------------

    /** The counters. */
    [[nodiscard]] AudioMixerStats stats() const noexcept;

    /**
     * The number of frames currently buffered on the microphone leg.
     */
    [[nodiscard]] std::size_t microphone_frames_available() const noexcept;

    /**
     * The number of frames currently buffered on the system-audio leg.
     */
    [[nodiscard]] std::size_t system_frames_available() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nimrtc::audio_device
