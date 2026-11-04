/**
 * @file src/modules/audio3a/src/webrtc_audio3a.cpp
 * @brief WebRtcAudio3A — WebRTC Audio Processing Module (APM) wrapper.
 *
 * Wraps the real WebRTC Audio Processing Module (APM) C++ API
 * (`webrtc::AudioProcessingBuilder` + `webrtc::AudioProcessing`) to
 * implement nimrtc::audio3a::IAudio3A.
 *
 * The APM provides:
 *   - AEC:  Acoustic Echo Cancellation (desktop AEC or mobile AECm)
 *   - ANS:  Ambient Noise Suppression (kLow / kModerate / kHigh / kVeryHigh)
 *   - AGC:  Automatic Gain Control (GainController2: adaptive digital with
 *          fixed_digital gain_db / saturation_margin / compression_gain_db)
 *   - HPF:  High-pass filter (removes low-frequency noise / DC)
 *   - VAD:  Voice Activity Detection (via Config::voice_detection_enabled +
 *          AudioProcessingStats::voice_detected after each ProcessStream)
 *
 * ## Audio frame shape
 *
 * WebRTC APM processes audio in ~10 ms frames (`AudioProcessing::GetFrameSize`).
 * Our IAudio3A interface takes arbitrary-length frames (interleaved float PCM).
 * We accumulate samples into a 10 ms scratch buffer (`kChunkSizeMs * rate / 1000`
 * samples per channel) and flush to the APM whenever the scratch buffer is full.
 * Any leftover samples are kept until the next call.
 *
 * ## Thread safety
 *
 * WebRtcAudio3A is NOT thread-safe. The caller must ensure that
 * process_capture(), process_render() and on_render_delivered() are called
 * from the same thread (or externally serialized).
 */

#include <nimrtc/audio3a/audio3a.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include <nimrtc/core/log.hpp>

#if defined(NIMRTC_USE_WEBRTC_APM)
  // WebRTC APM C++ API (real implementation path).
  #include <api/audio/audio_processing.h>
  #include <api/audio/audio_processing_statistics.h>
  #include <api/scoped_refptr.h>
#endif

namespace nimrtc::audio3a {

namespace {

#if defined(NIMRTC_USE_WEBRTC_APM)

// =============================================================================
// Mapping helpers: NimRTC Config → webrtc::AudioProcessing::Config
// =============================================================================

webrtc::AudioProcessing::Config::NoiseSuppression::Level
map_ans_level(std::uint8_t level) {
    // NimRTC level: 0=off, 1=low, 2=medium, 3=high.
    switch (level) {
        case 0:  return webrtc::AudioProcessing::Config::NoiseSuppression::kLow;
        case 1:  return webrtc::AudioProcessing::Config::NoiseSuppression::kLow;
        case 2:  return webrtc::AudioProcessing::Config::NoiseSuppression::kModerate;
        default: return webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
    }
}

#endif  // NIMRTC_USE_WEBRTC_APM

// =============================================================================
// Layout helpers
//
// The APM float API is deinterleaved (one float* per channel) while
// nimrtc::audio3a::Frame is interleaved.  These helpers convert between the
// two layouts without allocating.
//
// They are only reachable from the real-APM code path, so they are compiled
// exclusively when NIMRTC_USE_WEBRTC_APM is defined -- otherwise GCC's
// -Werror=unused-function rejects the stub-only build of this translation
// unit.
// =============================================================================

#if defined(NIMRTC_USE_WEBRTC_APM)

// Interleaved → planar.  Copies `n` frames (per channel) from `src` into the
// per-channel buffers pointed to by `dst`.
void copy_interleaved_to_planar(const float* src, std::size_t n, int channels,
                                float* const* dst) {
    for (int ch = 0; ch < channels; ++ch) {
        float* d = dst[ch];
        for (std::size_t i = 0; i < n; ++i) {
            d[i] = src[i * static_cast<std::size_t>(channels)
                       + static_cast<std::size_t>(ch)];
        }
    }
}

// Planar → interleaved.  Writes `n` frames (per channel) from the per-channel
// buffers pointed to by `src` into `dst`.  Templated on the pointer-array type
// so both `float*[]` and `const float* const[]` can be passed.
template <typename PtrArray>
void copy_planar_to_interleaved(const PtrArray& src, std::size_t n, int channels,
                                float* dst) {
    for (int ch = 0; ch < channels; ++ch) {
        const float* s = src[ch];
        for (std::size_t i = 0; i < n; ++i) {
            dst[i * static_cast<std::size_t>(channels)
                + static_cast<std::size_t>(ch)] = s[i];
        }
    }
}

#endif  // NIMRTC_USE_WEBRTC_APM

}  // anonymous namespace

// =============================================================================
// WebRtcAudio3A implementation
// =============================================================================

struct WebRtcAudio3A::Impl {
#if defined(NIMRTC_USE_WEBRTC_APM)
    rtc::scoped_refptr<webrtc::AudioProcessing> apm;
    webrtc::StreamConfig capture_config;   // input format
    webrtc::StreamConfig capture_out_config;
    webrtc::StreamConfig render_config;
    webrtc::StreamConfig render_out_config;

    // 10 ms scratch buffers per channel (interleaved storage for I/O; APM
    // float API is deinterleaved: per-channel float* pointers).
    int frame_size = 0;            // samples per channel per 10 ms
    int capture_channels = 1;
    int render_channels  = 1;
    int sample_rate_hz  = 48000;

    // Per-channel deinterleaved scratch buffers.
    std::vector<std::vector<float>> capture_scratch;  // [ch][frame_size]
    std::vector<std::vector<float>> render_scratch;
    std::vector<std::vector<float>> capture_out_scratch;
    std::vector<std::vector<float>> render_out_scratch;

    // Carries samples that didn't fit into the last full 10 ms frame.
    // Stored deinterleaved per channel so flushing is a single memcpy.
    std::vector<std::vector<float>> capture_residual;
    std::vector<std::vector<float>> render_residual;
    std::size_t capture_residual_fill = 0;  // samples per channel
    std::size_t render_residual_fill  = 0;

    // Output level stats derived from RMS over the most recent process_capture.
    LevelStats stats{};

    // Acoustic delay tracking — fed by on_render_delivered().
    //
    // `system_delay_ms` is the render→capture latency estimate reported to
    // the APM via set_stream_delay_ms().  It comes from
    // Config::aec_delay_headroom_ms — the caller's estimate of how long the
    // loudspeaker signal takes to reach the microphone.  It is a *delay*, not
    // a cumulative playback counter: the APM uses it to align the echo
    // canceller's reference path with the capture path.
    std::int32_t system_delay_ms = 50;
    std::size_t render_delivered_samples = 0;  // cumulative, for diagnostics only
    std::int32_t last_set_delay_ms = 0;
    bool delay_ever_set = false;

    // Number of samples whose 3A-processed output could not be handed back
    // to the caller because they straddled a 10 ms APM frame boundary (the
    // in-place API gives us nowhere to put them).  Non-zero means the caller
    // is pushing frames whose length is not a multiple of the APM frame
    // size — see the class-level docs in audio3a.hpp.
    std::size_t unaligned_output_samples = 0;
    bool warned_alignment = false;

    // Request flags.
    bool vad_requested = false;
    bool level_requested = false;

    // Last-applied NimRTC config (kept for runtime re-tuning if needed).
    Config config;
#endif  // NIMRTC_USE_WEBRTC_APM
};

WebRtcAudio3A::WebRtcAudio3A() : impl_(std::make_unique<Impl>()) {}

WebRtcAudio3A::~WebRtcAudio3A() = default;

bool WebRtcAudio3A::init(const Config& config) {
#if defined(NIMRTC_USE_WEBRTC_APM)
    impl_->config = config;

    impl_->sample_rate_hz   = static_cast<int>(config.sample_rate_hz);
    impl_->capture_channels = std::max<std::uint8_t>(1, config.capture_channels);
    impl_->render_channels  = std::max<std::uint8_t>(1, config.render_channels);

    // The render→capture system delay the echo canceller is told about.
    // The host measures the loudspeaker→microphone latency and passes it in;
    // the APM needs it to align its reference path with the capture path.
    impl_->system_delay_ms = config.aec_delay_headroom_ms;

    if (impl_->sample_rate_hz < 8000 || impl_->sample_rate_hz > 384000) {
        core::log::Logger::instance().error(
            "WebRtcAudio3A: sample rate out of supported range (8000-384000 Hz): "
            + std::to_string(impl_->sample_rate_hz));
        return false;
    }

    impl_->frame_size = webrtc::AudioProcessing::GetFrameSize(impl_->sample_rate_hz);
    if (impl_->frame_size <= 0) {
        core::log::Logger::instance().error(
            "WebRtcAudio3A: GetFrameSize returned non-positive frame size");
        return false;
    }

    // Build StreamConfig descriptors (sample rate + num channels).
    impl_->capture_config = webrtc::StreamConfig(impl_->sample_rate_hz,
                                                 impl_->capture_channels);
    impl_->capture_out_config = webrtc::StreamConfig(impl_->sample_rate_hz,
                                                     impl_->capture_channels);
    impl_->render_config = webrtc::StreamConfig(impl_->sample_rate_hz,
                                                impl_->render_channels);
    impl_->render_out_config = webrtc::StreamConfig(impl_->sample_rate_hz,
                                                    impl_->render_channels);

    // Build the APM with our config.
    webrtc::AudioProcessingBuilder builder;
    webrtc::AudioProcessing::Config apm_config;

    // High-pass filter (always on for voice-band audio).
    apm_config.high_pass_filter.enabled = true;

    // Echo canceller (desktop AEC by default).
    apm_config.echo_canceller.enabled = config.enable_aec;
    apm_config.echo_canceller.mobile_mode = false;
    // NimRTC stores a "headroom" in ms; APM doesn't have an equivalent knob,
    // but we record the value in our config and apply it via set_stream_delay_ms
    // once on_render_delivered() reports playback progress.

    // Noise suppression.
    apm_config.noise_suppression.enabled = config.ans_level > 0;
    apm_config.noise_suppression.level   = map_ans_level(config.ans_level);

    // AGC2 (modern adaptive digital AGC).
    apm_config.gain_controller2.enabled = config.enable_agc;
    if (config.enable_agc) {
        apm_config.gain_controller2.fixed_digital.gain_db =
            static_cast<float>(config.agc_target_dbfs);  // already negative
        apm_config.gain_controller2.adaptive_digital.enabled = true;
    }

    // Disable legacy AGC1 (AGC2 is the modern path; using both can fight).
    apm_config.gain_controller1.enabled = false;

    // VAD — this WebRTC APM API surfaces voice detection only through
    // AudioProcessingStats::voice_detected after each ProcessStream(); the
    // presence of detection is controlled by an installed EchoDetector /
    // ResidualEchoDetector and is always aggregated by the APM. We don't
    // need a config knob — request_vad_report() in our interface flags a
    // capture poll, and stats().vad_active will reflect it.
    //
    // (Config::CaptureLevelAdjustment, PreAmplifier, etc. exist but do not
    // gate voice detection. Keep the existing Config as-is.)

    // Use mono-downmixed capture for stable AEC behavior.
    apm_config.pipeline.multi_channel_capture = false;
    apm_config.pipeline.multi_channel_render  = false;

    builder.SetConfig(apm_config);

    impl_->apm = builder.Create();
    if (!impl_->apm) {
        core::log::Logger::instance().error(
            "WebRtcAudio3A: AudioProcessingBuilder().Create() returned null");
        return false;
    }

    // (Re-)Initialize the APM with our processing format.  ProcessingConfig
    // exposes input_stream/output_stream/reverse_input_stream/reverse_output_stream
    // — there is no `pipeline` field on ProcessingConfig in this API version.
    webrtc::ProcessingConfig pconfig;
    pconfig.input_stream() =
        webrtc::StreamConfig(impl_->sample_rate_hz, impl_->capture_channels);
    pconfig.output_stream() =
        webrtc::StreamConfig(impl_->sample_rate_hz, impl_->capture_channels);
    pconfig.reverse_input_stream() =
        webrtc::StreamConfig(impl_->sample_rate_hz, impl_->render_channels);
    pconfig.reverse_output_stream() =
        webrtc::StreamConfig(impl_->sample_rate_hz, impl_->render_channels);
    int rc = impl_->apm->Initialize(pconfig);
    if (rc != webrtc::AudioProcessing::kNoError) {
        core::log::Logger::instance().error(
            "WebRtcAudio3A: APM Initialize() failed with code "
            + std::to_string(rc));
        impl_->apm = nullptr;
        return false;
    }

    // Allocate per-channel scratch buffers.
    auto alloc_scratch = [](int channels, int frame_size) {
        std::vector<std::vector<float>> buf(static_cast<std::size_t>(channels));
        for (auto& v : buf) v.assign(static_cast<std::size_t>(frame_size), 0.0f);
        return buf;
    };
    impl_->capture_scratch       = alloc_scratch(impl_->capture_channels, impl_->frame_size);
    impl_->capture_out_scratch   = alloc_scratch(impl_->capture_channels, impl_->frame_size);
    impl_->render_scratch        = alloc_scratch(impl_->render_channels, impl_->frame_size);
    impl_->render_out_scratch    = alloc_scratch(impl_->render_channels, impl_->frame_size);
    impl_->capture_residual      = alloc_scratch(impl_->capture_channels, impl_->frame_size);
    impl_->render_residual       = alloc_scratch(impl_->render_channels, impl_->frame_size);
    impl_->capture_residual_fill = 0;
    impl_->render_residual_fill  = 0;
    impl_->render_delivered_samples = 0;
    impl_->last_set_delay_ms = 0;
    impl_->delay_ever_set = false;
    impl_->unaligned_output_samples = 0;
    impl_->warned_alignment = false;

    // Seed the echo canceller's delay estimate.  Without this the AEC has no
    // idea how far the loudspeaker signal lags the microphone signal and
    // cannot converge.
    if (config.enable_aec) {
        const std::int32_t clamped =
            std::clamp<std::int32_t>(impl_->system_delay_ms, 0, 999);
        impl_->apm->set_stream_delay_ms(clamped);
        impl_->last_set_delay_ms = clamped;
        impl_->delay_ever_set = true;
    }

    reset();

    core::log::Logger::instance().info(
        std::string("WebRtcAudio3A: initialized (real WebRTC APM, rate=")
        + std::to_string(impl_->sample_rate_hz)
        + ", capture_ch=" + std::to_string(impl_->capture_channels)
        + ", render_ch=" + std::to_string(impl_->render_channels)
        + ", AEC=" + std::to_string(static_cast<int>(config.enable_aec))
        + ", ANS=" + std::to_string(static_cast<int>(config.ans_level))
        + ", AGC=" + std::to_string(static_cast<int>(config.enable_agc))
        + ", VAD=" + std::to_string(static_cast<int>(config.enable_vad))
        + ")");
    return true;
#else
    // No WebRTC APM source available — refuse init so callers fall back to
    // NullAudio3A explicitly rather than silently getting zero processing.
    (void)config;
    core::log::Logger::instance().error(
        "WebRtcAudio3A: built without NIMRTC_USE_WEBRTC_APM (WebRTC APM source "
        "not populated). Use create_null_audio3a() instead.");
    return false;
#endif
}

#if defined(NIMRTC_USE_WEBRTC_APM)

static float rms_dbfs(const float* p, std::size_t n) {
    if (!p || n == 0) return -96.0f;
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double s = static_cast<double>(p[i]);
        sum += s * s;
    }
    const double rms = std::sqrt(sum / static_cast<double>(n));
    if (rms < 1e-7) return -96.0f;
    return static_cast<float>(20.0 * std::log10(rms));
}

/** RMS of an interleaved buffer, averaged over all channels.
 *
 *  The samples of all channels are pooled into one RMS — for a mono stream
 *  that is the plain RMS of the channel, for multichannel it is the RMS over
 *  the interleaved sample stream. */
static float rms_dbfs_interleaved(const float* p, std::size_t frames, int channels) {
    if (!p || frames == 0 || channels <= 0) return -96.0f;
    const std::size_t total = frames * static_cast<std::size_t>(channels);
    return rms_dbfs(p, total);
}

#endif  // NIMRTC_USE_WEBRTC_APM

void WebRtcAudio3A::process_capture(Frame& frame) {
#if defined(NIMRTC_USE_WEBRTC_APM)
    if (!impl_->apm || !frame.samples || frame.num_samples == 0) return;

    const std::size_t total_frames_in = frame.num_samples;
    const int channels = impl_->capture_channels;
    const int frame_size = impl_->frame_size;
    std::size_t cursor = 0;  // samples consumed from input

    // Measure the capture level in the INPUT domain — the level of the
    // signal as it arrives from the microphone, before any 3A processing.
    //
    // The 3A output is a processed signal: ANS legitimately silences a
    // stationary tone, AEC silences the echo component, and AGC changes the
    // level by design.  Measuring the output would report "the mic is dead"
    // for a perfectly healthy input.  See the LevelStats contract in
    // audio3a.hpp.
    if (impl_->config.report_tx_level) {
        impl_->stats.capture_level_dbfs = rms_dbfs_interleaved(
            frame.samples, total_frames_in, channels);
    }

    // Pull the APM's VAD verdict (if enabled and requested) into stats.
    auto refresh_vad = [&]() {
        if (!impl_->config.enable_vad) {
            impl_->stats.vad_active = false;
            impl_->vad_requested = false;
            return;
        }
        if (!impl_->vad_requested) return;
        auto apm_stats = impl_->apm->GetStatistics();
        if (apm_stats.voice_detected.has_value()) {
            impl_->stats.vad_active = *apm_stats.voice_detected;
        }
        impl_->vad_requested = false;
    };

    // 1) Top up the leftover residual from the previous call.
    //
    // The APM only accepts whole 10 ms frames, so a call whose sample count
    // is not a multiple of the frame size leaves a remainder behind.  The
    // remainder is *carried over* to the next call (never discarded) and is
    // combined with the next call's head to form a full frame.
    //
    // That combined frame straddles the call boundary, so only the part
    // belonging to *this* call can be written back into the caller's
    // in-place buffer; the part belonging to the previous call was already
    // handed out unprocessed and there is nowhere to put its processed
    // counterpart.  Those samples are counted in unaligned_output_samples.
    if (impl_->capture_residual_fill > 0) {
        const std::size_t old_fill = impl_->capture_residual_fill;
        const std::size_t need =
            static_cast<std::size_t>(frame_size) - old_fill;
        const std::size_t take = std::min(need, total_frames_in);

        // Append this call's first `take` samples to the residual buffer.
        for (int ch = 0; ch < channels; ++ch) {
            float* r = impl_->capture_residual[static_cast<std::size_t>(ch)].data();
            for (std::size_t i = 0; i < take; ++i) {
                r[old_fill + i] =
                    frame.samples[i * static_cast<std::size_t>(channels)
                                 + static_cast<std::size_t>(ch)];
            }
        }
        cursor = take;
        impl_->capture_residual_fill = old_fill + take;

        if (impl_->capture_residual_fill == static_cast<std::size_t>(frame_size)) {
            // Build a per-channel pointer array for the APM float API.
            std::vector<float*> in_ptrs(static_cast<std::size_t>(channels));
            std::vector<float*> out_ptrs(static_cast<std::size_t>(channels));
            for (int ch = 0; ch < channels; ++ch) {
                in_ptrs[ch]  = impl_->capture_residual[static_cast<std::size_t>(ch)].data();
                out_ptrs[ch] = impl_->capture_out_scratch[static_cast<std::size_t>(ch)].data();
            }
            int rc = impl_->apm->ProcessStream(
                in_ptrs.data(), impl_->capture_config, impl_->capture_out_config,
                out_ptrs.data());
            if (rc != webrtc::AudioProcessing::kNoError) {
                core::log::Logger::instance().warn(
                    "WebRtcAudio3A: ProcessStream returned code "
                    + std::to_string(rc));
            } else {
                // Write back only the slice that belongs to this call
                // (output samples [old_fill, old_fill + take)).
                for (int ch = 0; ch < channels; ++ch) {
                    const float* src =
                        impl_->capture_out_scratch[static_cast<std::size_t>(ch)].data();
                    for (std::size_t i = 0; i < take; ++i) {
                        frame.samples[i * static_cast<std::size_t>(channels)
                                     + static_cast<std::size_t>(ch)] =
                            src[old_fill + i];
                    }
                }
                impl_->unaligned_output_samples += old_fill;
                if (old_fill > 0 && !impl_->warned_alignment) {
                    impl_->warned_alignment = true;
                    core::log::Logger::instance().warn(
                        "WebRtcAudio3A: capture frames are not aligned to the "
                        "10 ms APM frame size; samples straddling a frame "
                        "boundary are processed but their processed output "
                        "cannot be returned in place. Push a multiple of "
                        + std::to_string(frame_size) + " samples per channel "
                        "for full 3A effect.");
                }

                refresh_vad();
            }
            impl_->capture_residual_fill = 0;
        }
    }

    // 2) Process as many full 10 ms frames as possible from the remaining input.
    while (cursor + static_cast<std::size_t>(frame_size) <= total_frames_in) {
        // Deinterleave `frame_size` frames into per-channel scratch.
        std::vector<float*> in_ptrs(static_cast<std::size_t>(channels));
        std::vector<float*> out_ptrs(static_cast<std::size_t>(channels));
        for (int ch = 0; ch < channels; ++ch) {
            in_ptrs[ch]  = impl_->capture_scratch[static_cast<std::size_t>(ch)].data();
            out_ptrs[ch] = impl_->capture_out_scratch[static_cast<std::size_t>(ch)].data();
        }
        copy_interleaved_to_planar(
            frame.samples + cursor * static_cast<std::size_t>(channels),
            static_cast<std::size_t>(frame_size), channels, in_ptrs.data());

        int rc = impl_->apm->ProcessStream(
            in_ptrs.data(), impl_->capture_config, impl_->capture_out_config,
            out_ptrs.data());
        if (rc != webrtc::AudioProcessing::kNoError) {
            core::log::Logger::instance().warn(
                "WebRtcAudio3A: ProcessStream returned code "
                + std::to_string(rc));
        } else {
            // Re-interleave processed output back into caller's buffer.
            copy_planar_to_interleaved(
                out_ptrs,
                static_cast<std::size_t>(frame_size), channels,
                frame.samples + cursor * static_cast<std::size_t>(channels));

            refresh_vad();
            impl_->level_requested = false;
        }
        cursor += static_cast<std::size_t>(frame_size);
    }

    // 3) Stash leftover samples into the residual for the next call.
    //
    // The residual buffer is only ever written when it is empty (step 1
    // either emptied it or consumed the whole input), so the carry-over
    // accumulates instead of overwriting — no sample is ever dropped.
    if (cursor < total_frames_in) {
        const std::size_t leftover = total_frames_in - cursor;
        for (int ch = 0; ch < channels; ++ch) {
            float* r = impl_->capture_residual[static_cast<std::size_t>(ch)].data();
            for (std::size_t i = 0; i < leftover; ++i) {
                r[i] = frame.samples[(cursor + i) * static_cast<std::size_t>(channels)
                                     + static_cast<std::size_t>(ch)];
            }
        }
        impl_->capture_residual_fill = leftover;

        if (!impl_->warned_alignment) {
            impl_->warned_alignment = true;
            core::log::Logger::instance().warn(
                "WebRtcAudio3A: capture frames are not aligned to the 10 ms "
                "APM frame size (" + std::to_string(leftover)
                + " sample(s) carried over). Samples that straddle a frame "
                "boundary are processed, but their processed output cannot be "
                "returned in place. Push a multiple of "
                + std::to_string(frame_size)
                + " samples per channel for full 3A effect.");
        }
    }
#else
    (void)frame;
#endif
}

void WebRtcAudio3A::process_render(const Frame& frame) {
#if defined(NIMRTC_USE_WEBRTC_APM)
    if (!impl_->apm || !frame.samples || frame.num_samples == 0) return;

    const std::size_t total_frames_in = frame.num_samples;
    const int channels = impl_->render_channels;
    const int frame_size = impl_->frame_size;
    std::size_t cursor = 0;

    // Render level is measured in the INPUT domain as well — the level of
    // the loudspeaker reference signal as it arrives, not the level of the
    // APM's reverse-stream output.  See the LevelStats contract in
    // audio3a.hpp.
    impl_->stats.render_level_dbfs = rms_dbfs_interleaved(
        frame.samples, total_frames_in, channels);

    // 1) Drain any leftover residual.
    if (impl_->render_residual_fill > 0) {
        const std::size_t need =
            static_cast<std::size_t>(frame_size) - impl_->render_residual_fill;
        const std::size_t take = std::min(need, total_frames_in);
        for (int ch = 0; ch < channels; ++ch) {
            for (std::size_t i = 0; i < take; ++i) {
                impl_->render_residual[static_cast<std::size_t>(ch)]
                                      [impl_->render_residual_fill + i] =
                    frame.samples[(cursor + i) * static_cast<std::size_t>(channels)
                                  + static_cast<std::size_t>(ch)];
            }
        }
        cursor += take;
        impl_->render_residual_fill += take;
        if (impl_->render_residual_fill == static_cast<std::size_t>(frame_size)) {
            std::vector<float*> in_ptrs(static_cast<std::size_t>(channels));
            std::vector<float*> out_ptrs(static_cast<std::size_t>(channels));
            for (int ch = 0; ch < channels; ++ch) {
                in_ptrs[ch]  = impl_->render_residual[static_cast<std::size_t>(ch)].data();
                out_ptrs[ch] = impl_->render_out_scratch[static_cast<std::size_t>(ch)].data();
            }
            int rc = impl_->apm->ProcessReverseStream(
                in_ptrs.data(), impl_->render_config, impl_->render_out_config,
                out_ptrs.data());
            if (rc != webrtc::AudioProcessing::kNoError) {
                core::log::Logger::instance().warn(
                    "WebRtcAudio3A: ProcessReverseStream returned code "
                    + std::to_string(rc));
            } else {
                // The render level is measured on the input domain (see
                // above); nothing to publish from the APM output.
            }
            impl_->render_residual_fill = 0;
        }
    }

    // 2) Full 10 ms chunks.
    while (cursor + static_cast<std::size_t>(frame_size) <= total_frames_in) {
        for (int ch = 0; ch < channels; ++ch) {
            float* dst = impl_->render_scratch[static_cast<std::size_t>(ch)].data();
            for (int i = 0; i < frame_size; ++i) {
                dst[i] = frame.samples[(cursor + static_cast<std::size_t>(i))
                                      * static_cast<std::size_t>(channels)
                                      + static_cast<std::size_t>(ch)];
            }
        }
        std::vector<float*> in_ptrs(static_cast<std::size_t>(channels));
        std::vector<float*> out_ptrs(static_cast<std::size_t>(channels));
        for (int ch = 0; ch < channels; ++ch) {
            in_ptrs[ch]  = impl_->render_scratch[static_cast<std::size_t>(ch)].data();
            out_ptrs[ch] = impl_->render_out_scratch[static_cast<std::size_t>(ch)].data();
        }
        int rc = impl_->apm->ProcessReverseStream(
            in_ptrs.data(), impl_->render_config, impl_->render_out_config,
            out_ptrs.data());
        if (rc != webrtc::AudioProcessing::kNoError) {
            core::log::Logger::instance().warn(
                "WebRtcAudio3A: ProcessReverseStream returned code "
                + std::to_string(rc));
        } else {
            // Render level measured on the input domain (see above).
        }
        cursor += static_cast<std::size_t>(frame_size);
    }

    // 3) Residual — carry the remainder over to the next call.
    //
    // The residual buffer is empty here (step 1 either emptied it or
    // consumed the whole input), so this accumulates rather than overwrites:
    // no reference sample is ever dropped.
    if (cursor < total_frames_in) {
        const std::size_t leftover = total_frames_in - cursor;
        for (int ch = 0; ch < channels; ++ch) {
            float* r = impl_->render_residual[static_cast<std::size_t>(ch)].data();
            for (std::size_t i = 0; i < leftover; ++i) {
                r[i] = frame.samples[(cursor + i) * static_cast<std::size_t>(channels)
                                     + static_cast<std::size_t>(ch)];
            }
        }
        impl_->render_residual_fill += leftover;
    }
#else
    (void)frame;
#endif
}

void WebRtcAudio3A::on_render_delivered(std::size_t num_samples) {
#if defined(NIMRTC_USE_WEBRTC_APM)
    if (!impl_->apm) return;
    impl_->render_delivered_samples += num_samples;

    if (!impl_->config.enable_aec) return;

    // The APM needs the *system* delay between the render reference (the
    // loudspeaker signal) and the capture signal (the microphone) — not a
    // cumulative playback counter.  The estimate comes from
    // Config::aec_delay_headroom_ms, which the host sets from the measured
    // loudspeaker→microphone latency.  Feeding a cumulative counter here
    // made the reported delay grow without bound, which the echo canceller
    // cannot use.
    const std::int32_t clamped =
        std::clamp<std::int32_t>(impl_->system_delay_ms, 0, 999);

    int rc = impl_->apm->set_stream_delay_ms(clamped);
    if (rc != webrtc::AudioProcessing::kNoError) {
        // kBadStreamParameterWarning (-13) just means APM hasn't observed a
        // matching ProcessStream() yet for this delay reading — that is normal
        // during the first frames of a session.  Only log at debug verbosity.
        if (rc != webrtc::AudioProcessing::kBadStreamParameterWarning) {
            core::log::Logger::instance().warn(
                "WebRtcAudio3A: set_stream_delay_ms(" + std::to_string(clamped)
                + ") returned code " + std::to_string(rc));
        }
    }
    impl_->last_set_delay_ms = clamped;
    impl_->delay_ever_set = true;
#else
    (void)num_samples;
#endif
}

void WebRtcAudio3A::request_vad_report() {
#if defined(NIMRTC_USE_WEBRTC_APM)
    impl_->vad_requested = true;
#endif
}

void WebRtcAudio3A::request_level_report() {
#if defined(NIMRTC_USE_WEBRTC_APM)
    impl_->level_requested = true;
#endif
}

LevelStats WebRtcAudio3A::stats() const {
#if defined(NIMRTC_USE_WEBRTC_APM)
    return impl_->stats;
#else
    return {};
#endif
}

void WebRtcAudio3A::reset() {
#if defined(NIMRTC_USE_WEBRTC_APM)
    if (impl_->apm) {
        // Re-Initialize clears internal state but keeps config.
        webrtc::ProcessingConfig pconfig;
        pconfig.input_stream() =
            webrtc::StreamConfig(impl_->sample_rate_hz, impl_->capture_channels);
        pconfig.output_stream() =
            webrtc::StreamConfig(impl_->sample_rate_hz, impl_->capture_channels);
        pconfig.reverse_input_stream() =
            webrtc::StreamConfig(impl_->sample_rate_hz, impl_->render_channels);
        pconfig.reverse_output_stream() =
            webrtc::StreamConfig(impl_->sample_rate_hz, impl_->render_channels);
        impl_->apm->Initialize(pconfig);
        // Re-Initialize resets the echo canceller's internal state, so the
        // system delay has to be handed over again.
        if (impl_->config.enable_aec) {
            const std::int32_t clamped =
                std::clamp<std::int32_t>(impl_->system_delay_ms, 0, 999);
            impl_->apm->set_stream_delay_ms(clamped);
            impl_->last_set_delay_ms = clamped;
            impl_->delay_ever_set = true;
        }
    }
    impl_->stats = LevelStats{};
    impl_->capture_residual_fill = 0;
    impl_->render_residual_fill  = 0;
    impl_->render_delivered_samples = 0;
    impl_->last_set_delay_ms = 0;
    impl_->delay_ever_set = false;
    impl_->unaligned_output_samples = 0;
    impl_->warned_alignment = false;
    impl_->vad_requested = false;
    impl_->level_requested = false;
    // Zero scratch buffers.
    for (auto& v : impl_->capture_residual) std::fill(v.begin(), v.end(), 0.0f);
    for (auto& v : impl_->render_residual)  std::fill(v.begin(), v.end(), 0.0f);
#endif
}

// =============================================================================
// Factory functions
// =============================================================================

std::unique_ptr<IAudio3A> create_webrtc_audio3a() {
    return std::make_unique<WebRtcAudio3A>();
}

bool webrtc_apm_compiled_in() noexcept {
#if defined(NIMRTC_USE_WEBRTC_APM)
    return true;
#else
    return false;
#endif
}

std::unique_ptr<IAudio3A> create_audio3a(std::string_view type) {
    if (type == "null" || type == "stub") {
        return create_null_audio3a();
    }
    if (type == "webrtc" || type == "webrtc_apm") {
        return create_webrtc_audio3a();
    }
    // Unknown type, default to null.
    core::log::Logger::instance().warn(
        std::string("audio3a: unknown type '") + std::string(type) + "', defaulting to null");
    return create_null_audio3a();
}

}  // namespace nimrtc::audio3a
