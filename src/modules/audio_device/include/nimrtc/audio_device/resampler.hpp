/**
 * @file nimrtc/audio_device/resampler.hpp
 * @brief The explicit sample-rate converter (remote-desktop plan F5).
 *
 * ## Why this exists
 *
 * The engine's audio plan (DEC-3) forbids a **silent** rate change: a
 * device whose native rate differs from the negotiated codec rate is an
 * error the *host* has to resolve.  This class is that resolution —
 * made **explicit**.  The conversion is never invisible: it is
 * reported through `stats()` (input rate, output rate, frames in,
 * frames out, and the fact that a conversion happened at all), so a
 * host can surface it in its own UI and the engine keeps DEC-3.
 *
 * ## The implementation
 *
 * The production converter is **`libsamplerate`** (Secret Rabbit Code,
 * BSD-2-Clause), vendored under `src/third_party/libsamplerate/`.  It
 * is chosen over SpeexDSP because SpeexDSP's resampler has been
 * unmaintained since its 2022-06 release, and over a hand-written
 * converter because a band-limited sinc interpolator is not a toy.
 *
 * The module also carries a **pure-C++ linear-interpolation fallback**
 * for builds where the vendored library is not compiled in
 * (`NIMRTC_VENDORED_LIBSAMPLERATE=OFF`).  The fallback is a *fallback*:
 * it is not band-limited, so it aliases above the Nyquist frequency of
 * the lower of the two rates.  `backend()` reports which one is in use
 * — `"libsamplerate"` or `"linear"` — so a host never has to guess.
 *
 * ## The PCM format
 *
 * The converter works on **interleaved signed 16-bit PCM** (`int16_t`),
 * the format every audio device that is not the engine's own float
 * pipeline speaks.  A frame is one sample *per channel*; a stereo
 * frame occupies two contiguous `int16_t`.
 *
 * ## Stream continuity
 *
 * The converter keeps its internal state across calls, so a stream
 * resampled in ten 10 ms blocks is (sample-for-sample) the same stream
 * as one resampled in a single 100 ms block.  That is what makes the
 * class usable as a live device->engine adapter rather than a
 * one-shot file converter.
 *
 * @note Remote-desktop plan F5 (M2-b).
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nimrtc::audio_device {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/**
 * @brief The converter quality hint.
 *
 * The hint is advisory: the effective converter depends on the backend
 * that is compiled in, and `Resampler::stats()` reports what was
 * actually used (`stats().converter`).  Under the libsamplerate
 * backend the "best" hint falls back to the *medium* sinc converter,
 * because the vendored build deliberately leaves upstream's ~9 MB
 * "best" coefficient table out.
 */
enum class NIMRTC_API_STABLE ResamplerQuality : std::uint8_t {
    /** The fastest converter — lowest CPU cost, widest aliasing. */
    kFast = 0,

    /** The default: a good quality / CPU balance. */
    kMedium = 1,

    /** The highest quality the backend offers. */
    kBest = 2,
};

/**
 * @brief The result of a resample call.
 *
 * The errors are *distinct* so a host can tell them apart without
 * parsing a string:
 *
 *   - `kOk` — the block was converted (possibly into zero frames: a
 *     converter that is still filling its internal buffer legitimately
 *     produces nothing on the first call).
 *   - `kInvalidConfig` — the constructor was handed an impossible
 *     configuration (a zero rate, a zero channel count, or a rate ratio
 *     outside the converter's supported range).  The object is left
 *     unusable and every subsequent call keeps returning this.
 *   - `kInvalidInput` — a null buffer with a non-zero frame count.
 *   - `kInternalError` — the underlying converter failed, or the
 *     object was used after `flush()` without a `reset()`.
 */
enum class NIMRTC_API_STABLE ResampleStatus : std::uint8_t {
    kOk = 0,
    kInvalidConfig = 1,
    kInvalidInput = 2,
    kInternalError = 3,
};

/** Human-readable status name (for logging / UI). */
[[nodiscard]] NIMRTC_API_STABLE std::string_view
resample_status_name(ResampleStatus status) noexcept;

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

/**
 * @brief The conversion counters.
 *
 * These are the numbers a host surfaces to make the conversion
 * **visible** — the plan's DEC-3 requirement.  `conversion_performed`
 * stays false while the input rate equals the output rate, so a host
 * can tell "the device already ran at the codec rate" from "the device
 * ran at 44.1 kHz and we converted it".
 */
struct NIMRTC_API_STABLE ResamplerStats {
    /** The input sample rate the converter was configured with, in Hz. */
    std::uint32_t input_rate_hz = 0;

    /** The output sample rate the converter was configured with, in Hz. */
    std::uint32_t output_rate_hz = 0;

    /** The ratio the converter runs at (output / input). */
    double ratio = 0.0;

    /** Total input frames handed to the converter. */
    std::uint64_t input_frames = 0;

    /** Total output frames the converter produced. */
    std::uint64_t output_frames = 0;

    /** The channel count the converter was configured with. */
    std::uint16_t channels = 0;

    /** True iff the input rate differs from the output rate, i.e. a
     *  rate conversion actually takes place. */
    bool conversion_performed = false;

    /** The backend that is in use: "libsamplerate" or "linear". */
    std::string backend;

    /** The effective converter, e.g. "sinc-medium" or "linear".  This is
     *  the *actual* converter, not the requested quality hint. */
    std::string converter;
};

// ---------------------------------------------------------------------------
// Resampler
// ---------------------------------------------------------------------------

/**
 * @brief An explicit sample-rate converter.
 *
 * ## Thread safety
 *
 * **This class is NOT thread-safe.**  A single instance belongs to a
 * single stream, and the calls on it (`resample()`, `flush()`,
 * `reset()`) must be serialised externally.  The *query* methods
 * (`backend()`, `stats()`, `input_rate_hz()`, ...) are const reads of
 * the counters and are safe to call concurrently *only* when no
 * conversion is running on the same instance.  Two resamplers (one per
 * stream direction) are independent and may run on different threads.
 *
 * ## Lifetime
 *
 * The converter owns an opaque backend state object.  Destroying the
 * `Resampler` releases it.
 */
class NIMRTC_API_STABLE Resampler {
public:
    /**
     * Construct a converter.
     *
     * @param input_rate_hz   The rate the incoming PCM is at, in Hz.
     * @param output_rate_hz  The rate the outgoing PCM must be at, in Hz.
     * @param channels        The channel count (1 = mono, 2 = stereo).
     * @param quality         The quality hint (advisory).
     *
     * An invalid configuration (a zero rate, a zero channel count, or
     * a rate ratio outside the converter's supported range) is NOT an
     * exception and NOT a crash: the object is constructed in a failed
     * state and every `resample()` call returns
     * `ResampleStatus::kInvalidConfig`.  Use `valid()` to check.
     */
    Resampler(std::uint32_t input_rate_hz,
              std::uint32_t output_rate_hz,
              std::uint16_t channels,
              ResamplerQuality quality = ResamplerQuality::kMedium) noexcept;

    ~Resampler();

    Resampler(const Resampler&)            = delete;
    Resampler& operator=(const Resampler&) = delete;
    Resampler(Resampler&&) noexcept;
    Resampler& operator=(Resampler&&) noexcept;

    // ---- Conversion -------------------------------------------------------

    /**
     * Convert a block of interleaved PCM16.
     *
     * The output block may be **shorter or longer** than the input
     * block: a downsampling conversion produces fewer frames, an
     * upsampling one more.  The internal state is kept across calls,
     * so a stream stays continuous.
     *
     * @param in        The interleaved input PCM.  May be null only when
     *                  @p in_frames is 0.
     * @param in_frames The number of input frames (samples *per channel*).
     * @param out       Receives the interleaved output PCM.  It is
     *                  cleared before the conversion, so a caller never
     *                  sees a stale tail.
     * @return `kOk` on success; `kInvalidConfig` when the object is in
     *         a failed state; `kInvalidInput` for a null buffer with a
     *         non-zero frame count; `kInternalError` when the backend
     *         converter failed.
     */
    ResampleStatus resample(const std::int16_t* in,
                             std::size_t in_frames,
                             std::vector<std::int16_t>& out);

    /**
     * Flush the converter's tail at end-of-stream.
     *
     * A converter with an internal filter holds a partial tail: the
     * frames that are "in flight" when the stream ends.  `flush()`
     * drains them into @p out.  The object is NOT usable for further
     * conversion afterwards without a `reset()` — a `resample()` call
     * after a flush returns `ResampleStatus::kInternalError`.
     *
     * @param out Receives the tail frames.  It is cleared first.
     * @return `kOk` when the tail was drained (possibly zero frames);
     *         `kInvalidConfig` when the object is in a failed state.
     */
    ResampleStatus flush(std::vector<std::int16_t>& out);

    /**
     * Reset the converter's internal state.
     *
     * The counters are NOT reset — `stats()` keeps reporting the
     * lifetime totals, so a host's "total frames converted" figure
     * survives a stream restart.
     */
    void reset() noexcept;

    // ---- Queries ----------------------------------------------------------

    /** True iff the configuration was accepted. */
    [[nodiscard]] bool valid() const noexcept;

    /** The input rate, in Hz. */
    [[nodiscard]] std::uint32_t input_rate_hz() const noexcept;

    /** The output rate, in Hz. */
    [[nodiscard]] std::uint32_t output_rate_hz() const noexcept;

    /** The channel count. */
    [[nodiscard]] std::uint16_t channels() const noexcept;

    /**
     * The conversion ratio in force (output / input).
     *
     * This is the ratio the *backend* runs at — the same value the
     * converter was constructed with.
     */
    [[nodiscard]] double ratio() const noexcept;

    /**
     * The backend in use: `"libsamplerate"` or `"linear"`.
     *
     * The string is the same one `stats().backend` carries.
     */
    [[nodiscard]] std::string backend() const;

    /** The conversion counters. */
    [[nodiscard]] ResamplerStats stats() const;

    /**
     * The number of output frames the converter still owes.
     *
     * A converter with an internal filter holds a partial tail: the
     * frames that are "in flight" between the input and the output.
     * This is the number of frames `flush()` will produce at
     * end-of-stream.  It is normally a small constant (the filter's
     * group delay), not a growing backlog.
     */
    [[nodiscard]] std::size_t output_frames_available() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------

/**
 * @brief The conversion ratio for a pair of rates.
 *
 * @return `output_rate / input_rate`, or 0.0 when either rate is zero.
 */
[[nodiscard]] NIMRTC_API_STABLE double
resample_ratio(std::uint32_t input_rate, std::uint32_t output_rate) noexcept;

/**
 * @brief The default converter quality hint.
 *
 * `kMedium`: a good quality / CPU balance for the §B.6 budget, which
 * puts the resample of a 10 ms stereo block at ≤ 0.3 ms.
 */
[[nodiscard]] NIMRTC_API_STABLE ResamplerQuality
default_resampler_quality() noexcept;

/**
 * @brief Whether the libsamplerate backend is compiled in.
 *
 * `false` means the module's pure-C++ linear-interpolation fallback is
 * in use.
 */
[[nodiscard]] NIMRTC_API_STABLE bool resampler_available() noexcept;

/**
 * @brief The backend the module's resampler uses.
 *
 * `"libsamplerate"` when the vendored library is compiled in,
 * `"linear"` otherwise.  The same string `Resampler::backend()`
 * returns.
 */
[[nodiscard]] NIMRTC_API_STABLE std::string resampler_backend() noexcept;

} // namespace nimrtc::audio_device
