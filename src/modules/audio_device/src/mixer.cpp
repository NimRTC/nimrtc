// audio_device/src/mixer.cpp
// The two-source audio mixer — see
// include/nimrtc/audio_device/mixer.hpp for the contract.
//
// The mixer is deliberately SMALL and deliberately EXPLICIT:
//
//   - the two legs are aligned by *block order*, not by timestamp;
//   - the rate conversion is the module's existing `Resampler` (F5) —
//     this file contains NO second resampler;
//   - the channel conversion is written out in full, because "mono vs
//     stereo" is a decision a host must be able to see;
//   - the mix itself is a saturating add with headroom, so a test can
//     assert the exact sample values.

#include <nimrtc/audio_device/mixer.hpp>
#include <nimrtc/audio_device/resampler.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace nimrtc::audio_device {

namespace {

// The mixer's channel ceiling: mono and stereo are the two shapes the
// engine's PCM contract uses, and the F6 acceptance criterion talks
// about "mono / stereo" explicitly.
constexpr std::uint16_t kMaxOutputChannels = 2;

// Clamp a float sample into [-1, +1].
float clamp_unit(float v) noexcept {
    if (v > 1.0f) {
        return 1.0f;
    }
    if (v < -1.0f) {
        return -1.0f;
    }
    return v;
}

// float [-1, 1] -> int16.  The scale is 32767 so that +1.0 maps to
// the maximum positive int16 exactly (and -1.0 maps to -32767, which
// keeps the conversion symmetric and round-trippable).
std::int16_t float_to_int16(float v) noexcept {
    const float scaled = v * 32767.0f;
    if (scaled >= 32767.0f) {
        return 32767;
    }
    if (scaled <= -32768.0f) {
        return -32768;
    }
    return static_cast<std::int16_t>(std::lrint(scaled));
}

// int16 -> float [-1, 1].
float int16_to_float(std::int16_t v) noexcept {
    return static_cast<float>(v) / 32767.0f;
}

/**
 * @brief One leg of the mixer: the microphone or the system audio.
 *
 * A leg buffers the frames that have been converted to the output
 * format (output rate, output channel count) and are waiting to be
 * mixed (or drained, in `kSeparate` mode).
 */
struct Leg {
    /** The rate the last block was pushed at (0 = not pushed yet). */
    std::uint32_t input_rate_hz = 0;

    /** The channel count the last block was pushed with. */
    std::uint16_t input_channels = 0;

    /** The pending frames, in the output format. */
    std::vector<float> buffer;

    /** The rate converter.  Created lazily, because a leg that runs at
     *  the output rate needs NO conversion at all (and must stay
     *  bit-exact). */
    std::unique_ptr<Resampler> resampler;

    /** The configuration the current converter was built with. */
    std::uint32_t resampler_in_rate = 0;
    std::uint32_t resampler_out_rate = 0;
    std::uint16_t resampler_channels = 0;

    /** True iff this leg went through a rate conversion. */
    bool resampled = false;

    /** Drop the buffered frames and the converter's state. */
    void clear() noexcept {
        buffer.clear();
        if (resampler) {
            resampler->reset();
        }
        resampled = false;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// AudioMixer::Impl
// ---------------------------------------------------------------------------

struct AudioMixer::Impl {
    AudioMixerConfig cfg;
    bool valid = false;

    Leg mic;
    Leg sys;

    AudioMixerStats stats;

    /** Normalise and validate the configuration. */
    void apply_config(AudioMixerConfig c) noexcept {
        cfg = std::move(c);
        valid = true;
        if (cfg.output_rate_hz == 0) {
            valid = false;
            return;
        }
        if (cfg.output_channels == 0) {
            cfg.output_channels = 1;
        }
        if (cfg.output_channels > kMaxOutputChannels) {
            valid = false;
            return;
        }
        if (!(cfg.headroom >= 0.0f) || !std::isfinite(cfg.headroom)) {
            valid = false;
            return;
        }
        if (!std::isfinite(cfg.microphone_gain) ||
            !std::isfinite(cfg.system_gain)) {
            valid = false;
            return;
        }
    }

    /**
     * Convert a block's channel count to the output channel count.
     *
     * The rules are explicit:
     *
     *   - mono in, mono out  : copy;
     *   - mono in, stereo out : duplicate;
     *   - stereo in, mono out : average;
     *   - N > 2 in            : average down to mono, then duplicate
     *                          when the output is stereo.
     */
    static void convert_channels(const float* in,
                                 std::size_t frames,
                                 std::uint16_t in_channels,
                                 std::uint16_t out_channels,
                                 std::vector<float>& out) {
        out.resize(frames * out_channels);
        if (in == nullptr || frames == 0) {
            return;
        }
        if (in_channels == out_channels) {
            std::copy(in, in + frames * in_channels, out.begin());
            return;
        }
        if (out_channels == 1) {
            // Down-mix: average the input channels.
            for (std::size_t i = 0; i < frames; ++i) {
                float acc = 0.0f;
                for (std::uint16_t ch = 0; ch < in_channels; ++ch) {
                    acc += in[i * in_channels + ch];
                }
                out[i] = acc / static_cast<float>(in_channels);
            }
            return;
        }
        // Stereo out.
        if (in_channels == 1) {
            for (std::size_t i = 0; i < frames; ++i) {
                const float v = in[i];
                out[i * 2] = v;
                out[i * 2 + 1] = v;
            }
            return;
        }
        // More than two input channels: average to mono, then duplicate.
        for (std::size_t i = 0; i < frames; ++i) {
            float acc = 0.0f;
            for (std::uint16_t ch = 0; ch < in_channels; ++ch) {
                acc += in[i * in_channels + ch];
            }
            const float mono = acc / static_cast<float>(in_channels);
            out[i * 2] = mono;
            out[i * 2 + 1] = mono;
        }
    }

    /**
     * Push a block into a leg.
     *
     * @param out_frames The number of frames the leg buffered.
     */
    AudioMixStatus push(Leg& leg,
                         const float* samples,
                         std::size_t num_frames,
                         std::uint16_t channels,
                         std::uint32_t sample_rate_hz,
                         std::uint64_t& out_frames) noexcept {
        out_frames = 0;
        if (!valid) {
            return AudioMixStatus::kInvalidParam;
        }
        if (samples == nullptr && num_frames > 0) {
            return AudioMixStatus::kInvalidParam;
        }
        if (num_frames == 0) {
            return AudioMixStatus::kOk;
        }
        if (channels == 0) {
            channels = 1;
        }
        if (sample_rate_hz == 0) {
            sample_rate_hz = cfg.output_rate_hz;
        }

        // ---- Step 1: the channel count ---------------------------------
        std::vector<float> converted;
        convert_channels(samples, num_frames, channels, cfg.output_channels,
                         converted);

    // ---- Step 2: the rate ------------------------------------------
    if (sample_rate_hz == cfg.output_rate_hz) {
        // No conversion: the block stays bit-exact.
        leg.resampled = false;
        if (leg.resampler) {
            // The leg's rate changed back to the output rate: the
            // converter's state is stale and must go.
            leg.resampler.reset();
        }
        leg.input_rate_hz = sample_rate_hz;
        leg.input_channels = cfg.output_channels;
        leg.buffer.insert(leg.buffer.end(), converted.begin(), converted.end());
        out_frames = num_frames;
        return AudioMixStatus::kOk;
    }

        // The converter is (re)created when the leg's input rate or the
        // mixer's output format changed.  It keeps its state across
        // pushes, so the stream stays continuous.
        if (!leg.resampler || leg.resampler_in_rate != sample_rate_hz ||
            leg.resampler_out_rate != cfg.output_rate_hz ||
            leg.resampler_channels != cfg.output_channels) {
            leg.resampler = std::make_unique<Resampler>(
                sample_rate_hz, cfg.output_rate_hz, cfg.output_channels,
                default_resampler_quality());
            if (!leg.resampler->valid()) {
                leg.resampler.reset();
                return AudioMixStatus::kInternalError;
            }
            leg.resampler_in_rate = sample_rate_hz;
            leg.resampler_out_rate = cfg.output_rate_hz;
            leg.resampler_channels = cfg.output_channels;
        }
        leg.input_rate_hz = sample_rate_hz;
        leg.input_channels = cfg.output_channels;
        leg.resampled = true;

        // The `Resampler` works on interleaved int16; the mixer's
        // surface is float.  The conversion is explicit (and the
        // 16-bit quantisation it carries is part of the converter's
        // documented behaviour).
        std::vector<std::int16_t> in16(converted.size());
        for (std::size_t i = 0; i < converted.size(); ++i) {
            in16[i] = float_to_int16(converted[i]);
        }

        std::vector<std::int16_t> out16;
        const ResampleStatus st =
            leg.resampler->resample(in16.data(), num_frames, out16);
        if (st != ResampleStatus::kOk) {
            return AudioMixStatus::kInternalError;
        }

        const std::size_t produced = out16.size() / cfg.output_channels;
        leg.buffer.reserve(leg.buffer.size() + out16.size());
        for (std::size_t i = 0; i < out16.size(); ++i) {
            leg.buffer.push_back(int16_to_float(out16[i]));
        }
        out_frames = produced;
        return AudioMixStatus::kOk;
    }

    /** The number of buffered frames on a leg. */
    static std::size_t frames_of(const Leg& leg, std::uint16_t channels) noexcept {
        return channels == 0 ? 0 : leg.buffer.size() / channels;
    }
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

AudioMixer::AudioMixer(AudioMixerConfig cfg) noexcept
    : impl_(std::make_unique<Impl>()) {
    impl_->apply_config(std::move(cfg));
}

AudioMixer::~AudioMixer() = default;

AudioMixer::AudioMixer(AudioMixer&&) noexcept = default;

AudioMixer& AudioMixer::operator=(AudioMixer&&) noexcept = default;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

const AudioMixerConfig& AudioMixer::config() const noexcept {
    return impl_->cfg;
}

AudioMixMode AudioMixer::mode() const noexcept {
    return impl_->cfg.mode;
}

void AudioMixer::set_config(AudioMixerConfig cfg) noexcept {
    const bool rate_changed = cfg.output_rate_hz != impl_->cfg.output_rate_hz;
    const bool channels_changed = cfg.output_channels != impl_->cfg.output_channels;
    impl_->apply_config(std::move(cfg));
    if (rate_changed || channels_changed) {
        // The buffered audio was converted into the old format; it can
        // no longer be mixed with the new one.
        impl_->mic.clear();
        impl_->sys.clear();
    }
}

bool AudioMixer::valid() const noexcept {
    return impl_->valid;
}

void AudioMixer::reset() noexcept {
    impl_->mic.clear();
    impl_->sys.clear();
}

// ---------------------------------------------------------------------------
// Feeding
// ---------------------------------------------------------------------------

AudioMixStatus AudioMixer::push_microphone(const float* samples,
                                           std::size_t num_frames,
                                           std::uint16_t channels,
                                           std::uint32_t sample_rate_hz) noexcept {
    std::uint64_t produced = 0;
    const AudioMixStatus st = impl_->push(impl_->mic, samples, num_frames,
                                          channels, sample_rate_hz, produced);
    if (st == AudioMixStatus::kOk) {
        impl_->stats.microphone_frames_in += num_frames;
    }
    return st;
}

AudioMixStatus AudioMixer::push_system(const float* samples,
                                       std::size_t num_frames,
                                       std::uint16_t channels,
                                       std::uint32_t sample_rate_hz) noexcept {
    std::uint64_t produced = 0;
    const AudioMixStatus st = impl_->push(impl_->sys, samples, num_frames,
                                          channels, sample_rate_hz, produced);
    if (st == AudioMixStatus::kOk) {
        impl_->stats.system_frames_in += num_frames;
    }
    return st;
}

// ---------------------------------------------------------------------------
// Draining
// ---------------------------------------------------------------------------

AudioMixStatus AudioMixer::pull(std::vector<float>& mixed,
                                std::vector<float>& microphone,
                                std::vector<float>& system) noexcept {
    mixed.clear();
    microphone.clear();
    system.clear();

    if (!impl_->valid) {
        return AudioMixStatus::kInvalidParam;
    }

    const std::uint16_t ch = impl_->cfg.output_channels;

    if (impl_->cfg.mode == AudioMixMode::kSeparate) {
        // The two legs are drained independently — they are NEVER
        // combined.
        const std::size_t mic_frames = Impl::frames_of(impl_->mic, ch);
        const std::size_t sys_frames = Impl::frames_of(impl_->sys, ch);
        if (mic_frames == 0 && sys_frames == 0) {
            return AudioMixStatus::kNotReady;
        }
        if (mic_frames > 0) {
            microphone.assign(impl_->mic.buffer.begin(),
                              impl_->mic.buffer.begin() +
                                  static_cast<std::ptrdiff_t>(mic_frames * ch));
            impl_->mic.buffer.erase(impl_->mic.buffer.begin(),
                                    impl_->mic.buffer.begin() +
                                        static_cast<std::ptrdiff_t>(mic_frames * ch));
        }
        if (sys_frames > 0) {
            system.assign(impl_->sys.buffer.begin(),
                          impl_->sys.buffer.begin() +
                              static_cast<std::ptrdiff_t>(sys_frames * ch));
            impl_->sys.buffer.erase(impl_->sys.buffer.begin(),
                                    impl_->sys.buffer.begin() +
                                        static_cast<std::ptrdiff_t>(sys_frames * ch));
        }
        impl_->stats.microphone_frames_out += mic_frames;
        impl_->stats.system_frames_out += sys_frames;
        return AudioMixStatus::kOk;
    }

    // ---- kMix -----------------------------------------------------------
    // The two legs are aligned by *block order*: the Nth microphone
    // frame is mixed with the Nth system-audio frame.  The shorter leg
    // bounds the mixed block.
    const std::size_t mic_frames = Impl::frames_of(impl_->mic, ch);
    const std::size_t sys_frames = Impl::frames_of(impl_->sys, ch);
    const std::size_t n = std::min(mic_frames, sys_frames);
    if (n == 0) {
        return AudioMixStatus::kNotReady;
    }

    const float mg = impl_->cfg.microphone_gain;
    const float sg = impl_->cfg.system_gain;
    const float hr = impl_->cfg.headroom;

    mixed.resize(n * ch);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::uint16_t c = 0; c < ch; ++c) {
            const float m = impl_->mic.buffer[i * ch + c];
            const float s = impl_->sys.buffer[i * ch + c];
            // The mix is a saturating add with headroom.  The order of
            // the operations is fixed, so the result is exactly
            // reproducible.
            const float sum = mg * m + sg * s;
            mixed[i * ch + c] = clamp_unit(hr * sum);
        }
    }

    impl_->mic.buffer.erase(impl_->mic.buffer.begin(),
                            impl_->mic.buffer.begin() +
                                static_cast<std::ptrdiff_t>(n * ch));
    impl_->sys.buffer.erase(impl_->sys.buffer.begin(),
                            impl_->sys.buffer.begin() +
                                static_cast<std::ptrdiff_t>(n * ch));

    impl_->stats.mixed_frames_out += n;
    impl_->stats.microphone_frames_out += n;
    impl_->stats.system_frames_out += n;
    return AudioMixStatus::kOk;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

AudioMixerStats AudioMixer::stats() const noexcept {
    AudioMixerStats s = impl_->stats;
    s.microphone_resampled = impl_->mic.resampled;
    s.system_resampled = impl_->sys.resampled;
    s.resampled_frames = 0;
    return s;
}

std::size_t AudioMixer::microphone_frames_available() const noexcept {
    return Impl::frames_of(impl_->mic, impl_->cfg.output_channels);
}

std::size_t AudioMixer::system_frames_available() const noexcept {
    return Impl::frames_of(impl_->sys, impl_->cfg.output_channels);
}

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------

std::string_view audio_mix_mode_name(AudioMixMode mode) noexcept {
    switch (mode) {
        case AudioMixMode::kMix:     return "mix";
        case AudioMixMode::kSeparate: return "separate";
        default:                     return "unknown";
    }
}

std::string_view audio_mix_status_name(AudioMixStatus status) noexcept {
    switch (status) {
        case AudioMixStatus::kOk:           return "ok";
        case AudioMixStatus::kInvalidParam: return "invalid parameter";
        case AudioMixStatus::kNotReady:      return "not ready";
        case AudioMixStatus::kInternalError: return "internal error";
        default:                             return "unknown";
    }
}

} // namespace nimrtc::audio_device
