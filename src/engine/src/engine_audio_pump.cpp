/**
 * @file src/engine/src/engine_audio_pump.cpp
 * @brief The audio device plane: capture pump, playback write, AEC alignment
 *        (v1.4 D1, ADR-040).
 *
 * ## What this file is
 *
 * The audio counterpart of `engine_video_pump.cpp`.  The engine's
 * *automatic* audio path: it pulls PCM from the capture device's
 * bounded queue and drives it through the very same `send_audio()`
 * chain the host uses, and it writes the decoded PCM to the playback
 * device.
 *
 * ## Why the pump calls `send_audio()`
 *
 * Because the outbound path is **already** complete: the 3A's
 * `process_capture()`, the encoder, the RTP packetiser and the
 * send gate (`ice_send_blocked`) all live in `send_audio()`.  The
 * pump therefore does the minimum — "read a frame, hand it over" —
 * and every one of those stages is reused verbatim.  There is no
 * second implementation of the outbound path to drift.
 *
 * ## Why the pump lives here, and not in the device module
 *
 * The same reason the video pump lives in the engine: the engine is a
 * single-threaded orchestrator.  The *device* owns its own thread and
 * fills its bounded queue (that is the device's business); the
 * *engine* only ever **polls** that queue, and it does so from
 * `tick()`.  So the codec is touched from exactly one thread — the
 * tick thread — no matter what the capture backend does internally.
 *
 * ## The AEC reference alignment (the D1 payoff)
 *
 * The 3A's echo canceller needs to know where in the *played-out*
 * signal the capture sits.  Before D1 the engine reported
 * `set_render_delivered(frame's sample count)` — that is, "the
 * samples were handed to the host", which is *not* the same as "the
 * loudspeaker played them": the device's buffer and driver latency
 * sit between the two.
 *
 * With a playback device present, the engine reports the device's
 * **cumulative played-out sample count** instead.  That is the number
 * the AEC reference must track, and it is the whole reason the device
 * path is wired by the engine rather than by the host (remote-desktop
 * plans F3 / F4).
 *
 * The two paths are **parallel, not alternatives**: the host's
 * `on_audio_frame` callback keeps firing (it is `NIMRTC_API_STABLE`),
 * and the device write is *appended* to it.  What changes is only
 * *who* reports the render reference — the device path reports it
 * when a device is present, the host path reports it when there is
 * none.  Reporting both would double the 3A's reference count.
 */
#include "engine_device_policy.hpp"
#include "engine_impl.hpp"

#include <nimrtc/core/log.hpp>
#include <nimrtc/engine/pal/engine_plugin_resolver.hpp>
#include <nimrtc/engine/pal/make_plugin.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace nimrtc::engine {

namespace {

/** The capture pump's read size, in samples per channel.
 *
 *  The engine has no resampler in the stack (the audio plan's DEC-3),
 *  so the pump reads at the device's own quantum and hands the block
 *  to `send_audio()`, which accepts any block size.  4096 frames is
 *  the upper bound of what a sane device quantum is; a device that
 *  negotiated a larger one is capped rather than growing the buffer
 *  without limit. */
constexpr std::size_t kCaptureReadFrames = 4096;

/** Resolve an audio device name down to a concrete backend id.
 *
 *  Empty (the host feeds / plays the PCM itself) and `"none"` (the
 *  explicit opt-out) never reach here.  `"auto"` resolves through the
 *  runtime registry (`detail::resolve_auto_audio_backend`); an empty
 *  result is a *degrade* -- the engine warns and continues without a
 *  device, because a machine with no sound card is a normal machine.
 *
 *  @param name          The configured name.
 *  @param capture       true for the source side, false for the sink.
 *  @param is_registered The registry-membership check for the side.
 *  @param auto_what     The configuration field name, for the warn line.
 *  @return The backend id to open, or "" to continue without a device. */
std::string resolve_audio_device_name(std::string_view name,
                                      bool capture,
                                      bool (*is_registered)(
                                          std::string_view id) noexcept,
                                      std::string_view auto_what) noexcept {
    std::string resolved{name};
    if (resolved == "auto") {
        resolved = detail::resolve_auto_audio_backend(capture, is_registered);
        if (resolved.empty()) {
            core::log::Logger::instance().warn(
                std::string{"engine: "} + std::string{auto_what} +
                "=\"auto\" did not resolve to a registered audio backend "
                "on this platform; continuing without an audio device");
        }
    }
    return resolved;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// The device lifecycle
// ---------------------------------------------------------------------------

uint32_t NimRTCEngine::init_audio_devices() noexcept {
    auto& astate = impl_->audio_state;
    uint32_t first_error = 0;

    // The backends the registry answered with, kept for the Stats
    // (criterion 13): the host asked for a *name*, and the backend that
    // opened is a fact of its own.
    std::string resolved_source_backend;
    std::string resolved_sink_backend;

    // Idempotent: a second `open()` (or a `pre_open()` that already
    // brought the plane up) must not resolve the devices again — the
    // host's "none" opt-out has to stay an opt-out.
    if (astate.audio_devices_initialized) return 0;
    astate.audio_devices_initialized = true;

    // ---- The capture device --------------------------------------------
    //
    // `"none"` is the explicit opt-out: the host feeds the PCM by hand
    // through `send_audio()`.  An empty name means "the default
    // device" — the backend picks its own default input.
    if (!config_.audio_source_name.empty() &&
        config_.audio_source_name != "none") {
        // The request is the host's (`EngineConfig::audio_source_config`),
        // handed to the device verbatim: the device owns the
        // negotiation, and the engine must not second-guess which
        // microphone or which format the host meant.
        //
        // The failure mode follows the *name*: an `"auto"` backend is a
        // recommendation (a machine with no sound card is normal), so it
        // degrades with a warn; an explicit id is a promise, so it is a
        // hard error and `open()` fails (ADR-029 §2.4).
        const bool is_auto = config_.audio_source_name == "auto";
        const std::string resolved = resolve_audio_device_name(
            config_.audio_source_name,
            /*capture=*/true,
            [](std::string_view id) noexcept {
                return pal::resolve_audio_source(id) != nullptr;
            },
            "audio_source_name");
        if (!resolved.empty()) {
            const bool opened = pal::make_plugin<&pal::resolve_audio_source>(
                resolved,
                astate.audio_source,
                config_.audio_source_config,
                {.err_open_failed = core::kAudioSourceOpenFailed,
                 .err_missing = core::kAudioSourcePluginMissing,
                 .open_fail_mode = is_auto
                     ? pal::PluginFailureMode::Warn
                     : pal::PluginFailureMode::Error,
                 .missing_mode = is_auto
                     ? pal::PluginFailureMode::Warn
                     : pal::PluginFailureMode::Error,
                 .on_error = on_error_});
            if (!opened && !is_auto && first_error == 0) {
                first_error = core::kAudioSourceOpenFailed;
            } else if (opened) {
                resolved_source_backend = resolved;
            }
        }
    }

    if (astate.audio_source) {
        // The *negotiated* format — what the device actually got, not
        // what the engine asked for.  A device may have to fall back to
        // a different rate or channel count, and the pump has to know
        // which.
        astate.audio_source_negotiated = astate.audio_source->config();
        astate.audio_source_device_id =
            astate.audio_source_negotiated.device_id;
        // The *backend* that answered the request is a separate fact
        // from the device id (criterion 13): the host asked for a name,
        // the registry answered with a backend, and the backend
        // answered with a device.  The Stats report the middle one.
        astate.audio_source_backend = resolved_source_backend;

        // The engine's PCM contract is fixed (`pcm_sample_rate_hz` /
        // `pcm_channels`), and there is no resampler in the stack, so a
        // device that negotiated a different format cannot be pumped
        // into `send_audio()`: the samples would be mis-timestamped and
        // the stream would drift.  The pump refuses to run rather than
        // corrupt the audio, and the reason is logged.
        const auto& fmt = astate.audio_source_negotiated.format;
        const std::uint32_t want_rate = config_.pcm_sample_rate_hz;
        const std::uint16_t want_channels = config_.pcm_channels;
        if (want_rate != 0 && fmt.sample_rate_hz != 0 &&
            fmt.sample_rate_hz != want_rate) {
            core::log::Logger::instance().warn(
                "engine: the audio capture device negotiated " +
                std::to_string(fmt.sample_rate_hz) + " Hz but the engine's "
                "PCM rate is " + std::to_string(want_rate) +
                " Hz; the capture pump stays off (there is no resampler "
                "in the stack)");
        } else if (want_channels != 0 && fmt.channels != 0 &&
                   fmt.channels != want_channels) {
            core::log::Logger::instance().warn(
                "engine: the audio capture device negotiated " +
                std::to_string(fmt.channels) + " channel(s) but the engine's "
                "PCM format is " + std::to_string(want_channels) +
                "; the capture pump stays off");
        }
    }

    // ---- The playback device -------------------------------------------
    if (!config_.audio_sink_name.empty() &&
        config_.audio_sink_name != "none") {
        const bool is_auto = config_.audio_sink_name == "auto";
        const std::string resolved = resolve_audio_device_name(
            config_.audio_sink_name,
            /*capture=*/false,
            [](std::string_view id) noexcept {
                return pal::resolve_audio_sink(id) != nullptr;
            },
            "audio_sink_name");
        if (!resolved.empty()) {
            const bool opened = pal::make_plugin<&pal::resolve_audio_sink>(
                resolved,
                astate.audio_sink,
                config_.audio_sink_config,
                {.err_open_failed = core::kAudioSinkOpenFailed,
                 .err_missing = core::kAudioSinkPluginMissing,
                 .open_fail_mode = is_auto
                     ? pal::PluginFailureMode::Warn
                     : pal::PluginFailureMode::Error,
                 .missing_mode = is_auto
                     ? pal::PluginFailureMode::Warn
                     : pal::PluginFailureMode::Error,
                 .on_error = on_error_});
            if (!opened && !is_auto && first_error == 0) {
                first_error = core::kAudioSinkOpenFailed;
            } else if (opened) {
                resolved_sink_backend = resolved;
            }
        }
    }

    if (astate.audio_sink) {
        astate.audio_sink_negotiated = astate.audio_sink->config();
        astate.audio_sink_device_id = astate.audio_sink_negotiated.device_id;
        astate.audio_sink_backend = resolved_sink_backend;
    }

    return first_error;
}

void NimRTCEngine::shutdown_audio_devices() noexcept {
    auto& astate = impl_->audio_state;

    // The devices own OS resources (a PortAudio stream, a WASAPI
    // capture), so they are closed explicitly rather than merely
    // dropped.
    if (astate.audio_source) {
        astate.audio_source->close();
        astate.audio_source.reset();
    }
    if (astate.audio_sink) {
        astate.audio_sink->close();
        astate.audio_sink.reset();
    }
    astate.audio_source_device_id.clear();
    astate.audio_sink_device_id.clear();
    astate.audio_source_backend.clear();
    astate.audio_sink_backend.clear();
    astate.audio_capture_scratch.clear();
    astate.audio_capture_scratch.shrink_to_fit();
    // The AEC reference resumes from zero with the next device: a
    // re-opened device restarts its played-sample counter, so the
    // baseline has to follow or the reference would run backwards.
    astate.audio_render_delivered_reported = 0;
}

// ---------------------------------------------------------------------------
// The capture pump
// ---------------------------------------------------------------------------

void NimRTCEngine::pump_audio_capture() noexcept {
    if (!is_open()) return;
    if (!config_.audio_source_pump) return;

    auto& astate = impl_->audio_state;
    if (!astate.audio_source) return;

    // ---- The device's health -------------------------------------------
    //
    // Two things are checked before the frame is read, because both are
    // about the *device* rather than about a frame:
    //
    //   1. **Did the device lose itself?**  A microphone that was
    //      unplugged mid-call is a *normal* event, not a crash.  The
    //      device reports it through its error counter and stops
    //      delivering frames — the engine must notice, or the session
    //      would look like a microphone that never delivers anything.
    //   2. **Did the device renegotiate?**  A format switch is legal,
    //      and it is *visible* in the stats instead of being invisible.
    recover_audio_device();
    publish_audio_device_counters();

    // ---- The format check ----------------------------------------------
    //
    // The pump refuses to run when the device's negotiated format does
    // not match the engine's PCM format: the samples would be
    // mis-timestamped and the stream would drift.  There is no
    // resampler in the stack (the audio plan's DEC-3).
    const auto& fmt = astate.audio_source_negotiated.format;
    const std::uint32_t want_rate = config_.pcm_sample_rate_hz;
    const std::uint16_t want_channels = config_.pcm_channels;
    if ((want_rate != 0 && fmt.sample_rate_hz != 0 &&
         fmt.sample_rate_hz != want_rate) ||
        (want_channels != 0 && fmt.channels != 0 &&
         fmt.channels != want_channels)) {
        return;
    }

    // ---- Read one block (non-blocking) ---------------------------------
    //
    // The device's queue is empty most of the time: the capture cadence
    // (10–20 ms) is slower than the tick rate.  `read_samples()` never
    // blocks, so an empty queue is the normal case and costs nothing.
    const std::size_t channels =
        static_cast<std::size_t>(fmt.channels != 0 ? fmt.channels : 1);
    const std::size_t capacity = kCaptureReadFrames * channels;
    if (astate.audio_capture_scratch.size() < capacity) {
        astate.audio_capture_scratch.resize(capacity);
    }

    std::size_t frames = 0;
    const plugins::Status rc = astate.audio_source->read_samples(
        astate.audio_capture_scratch.data(), capacity, &frames);
    if (rc != plugins::kOk) {
        // `kErrNotReady` is the normal "the queue is empty" answer.
        // Anything else (a hardware error, a closed device) is visible
        // through the device's own error counter, which
        // `recover_audio_device()` above already looked at -- and it is
        // counted here too, so `audio_device_pump_errors` reports the
        // pump's own failures.
        if (rc != plugins::kErrNotReady) {
            ++astate.audio_device_pump_errors;
        }
        return;
    }
    if (frames == 0) return;

    ++astate.audio_capture_frames;

    // ---- Hand the block to the outbound path ---------------------------
    //
    // `send_audio()` takes *samples per channel* and knows the channel
    // count from `EngineConfig::pcm_channels` — the same count the
    // format check above verified.
    if (send_audio(astate.audio_capture_scratch.data(), frames) != 0) {
        ++astate.audio_device_pump_errors;
        return;
    }
    ++astate.audio_capture_frames_sent;
    // The *published* pump counter: `Stats::audio_device_pumped_frames`
    // is the proof that the pump actually moved, so it counts the frames
    // that reached the outbound path -- the same event the engine's own
    // `audio_capture_frames_sent` counts, under the name the public
    // stats surface uses.
    ++astate.audio_device_pumped_frames;
}

// ---------------------------------------------------------------------------
// The device's health
// ---------------------------------------------------------------------------

bool NimRTCEngine::recover_audio_device() noexcept {
    auto& astate = impl_->audio_state;
    if (!astate.audio_source) return false;

    // The device's error counter only ever grows, so a plain comparison
    // would fire on every tick.  The baseline is what makes the
    // comparison meaningful.
    const plugins::AudioDeviceStats stats = astate.audio_source->stats();
    if (stats.device_errors <= astate.audio_capture_seen_device_errors) {
        return true;
    }

    astate.audio_capture_seen_device_errors = stats.device_errors;
    ++astate.audio_capture_device_errors;

    core::log::Logger::instance().warn(
        "engine: the audio capture device reported an error; reopening it");

    // A device that is reopened restarts its counters from zero, so the
    // baselines have to follow -- otherwise the next delta would be a
    // huge underflowed number.
    //
    // The device is **closed first**.  A lost device is torn down and
    // built up again, not "re-opened" in place: `open()` on an already
    // open device is a no-op for most backends (and would leave the
    // stale device behind), so the reopen would silently do nothing.
    astate.audio_source->close();
    const plugins::Status rc = astate.audio_source->open();
    if (rc != plugins::kOk) {
        core::log::Logger::instance().error(
            "engine: the audio capture device could not be reopened; "
            "the capture pump is stopped");
        if (on_error_) {
            on_error_(core::kAudioSourceOpenFailed,
                      "engine: the audio capture device could not be "
                      "reopened; the capture pump is stopped");
        }
        return false;
    }

    ++astate.audio_capture_restarts;

    // The device may have renegotiated while it was away.
    astate.audio_source_negotiated = astate.audio_source->config();

    const plugins::AudioDeviceStats fresh = astate.audio_source->stats();
    astate.audio_capture_seen_dropped = fresh.frames_dropped;
    astate.audio_capture_seen_device_errors = fresh.device_errors;

    return true;
}

void NimRTCEngine::publish_audio_device_counters() noexcept {
    auto& astate = impl_->audio_state;
    if (!astate.audio_source) return;

    // The counters are read *after* the read and compared against the
    // baseline the pump recorded last time.  Reading them *before* the
    // read (and taking a delta) would miss everything that happened
    // between the previous tick and this one -- most importantly the
    // frames the device dropped while the queue was full, because those
    // drops are counted by the *producer* thread and never by the read.
    const plugins::AudioDeviceStats stats = astate.audio_source->stats();

    if (stats.frames_processed >= astate.audio_capture_seen_frames) {
        // The device's own "frames processed" counts the frames it
        // *produced*, which is the number the host wants to compare
        // against the frames the engine actually sent.
        astate.audio_capture_frames = stats.frames_processed;
    } else {
        astate.audio_capture_seen_frames = stats.frames_processed;
    }

    if (stats.frames_dropped >= astate.audio_capture_seen_dropped) {
        astate.audio_capture_dropped +=
            stats.frames_dropped - astate.audio_capture_seen_dropped;
        astate.audio_capture_seen_dropped = stats.frames_dropped;
    } else {
        astate.audio_capture_seen_dropped = stats.frames_dropped;
    }
}

// ---------------------------------------------------------------------------
// The playback path
// ---------------------------------------------------------------------------

void NimRTCEngine::write_audio_to_sink(const float* pcm,
                                        std::size_t samples_per_channel,
                                        std::uint16_t channels) noexcept {
    if (!config_.audio_sink_pump) return;

    auto& astate = impl_->audio_state;
    if (!astate.audio_sink) return;
    if (pcm == nullptr || samples_per_channel == 0) return;

    const plugins::Status rc = astate.audio_sink->write(
        pcm, samples_per_channel, channels);
    if (rc != plugins::kOk) {
        // A device that refuses a block is a *device* problem, not an
        // engine problem: the stream keeps flowing and the host's own
        // callback still gets the PCM.  The reason is logged once per
        // failure — the counter in `stats()` carries the count.
        ++astate.audio_playback_write_errors;
        core::log::Logger::instance().warn(
            "engine: the audio playback device refused a block "
            "(status=" + std::to_string(static_cast<int>(rc)) + ")");
        return;
    }
    ++astate.audio_playback_frames_written;
}

void NimRTCEngine::report_playback_progress_to_3a() noexcept {
    auto& astate = impl_->audio_state;
    if (!astate.audio_sink) return;
    if (!audio3a_plugin_) return;

    // The device's count is **cumulative**; the 3A's
    // `set_render_delivered()` accumulates what it is given.  The
    // engine therefore reports the *delta* since the last report — the
    // same number of samples the loudspeaker actually consumed since
    // then, which is exactly what the AEC reference must track.
    const std::uint64_t played = astate.audio_sink->played_samples();
    if (played <= astate.audio_render_delivered_reported) {
        // A device that was reopened restarts at zero, so the counter
        // can go *backwards*.  The reference must not follow it back.
        if (played < astate.audio_render_delivered_reported) {
            astate.audio_render_delivered_reported = played;
        }
        return;
    }
    const std::uint64_t delta =
        played - astate.audio_render_delivered_reported;
    astate.audio_render_delivered_reported = played;

    astate.audio_playback_samples_played = played;
    audio3a_plugin_->set_render_delivered(static_cast<std::size_t>(delta));
}

// ---------------------------------------------------------------------------
// The public surface
// ---------------------------------------------------------------------------

plugins::IAudioSource* NimRTCEngine::audio_source() noexcept {
    return impl_ ? impl_->audio_state.audio_source.get() : nullptr;
}

const plugins::IAudioSource* NimRTCEngine::audio_source() const noexcept {
    return impl_ ? impl_->audio_state.audio_source.get() : nullptr;
}

plugins::IAudioSink* NimRTCEngine::audio_sink() noexcept {
    return impl_ ? impl_->audio_state.audio_sink.get() : nullptr;
}

const plugins::IAudioSink* NimRTCEngine::audio_sink() const noexcept {
    return impl_ ? impl_->audio_state.audio_sink.get() : nullptr;
}

std::vector<plugins::AudioDevice>
NimRTCEngine::audio_capture_devices() const noexcept {
    if (!impl_) return {};
    const plugins::IAudioSource* source = impl_->audio_state.audio_source.get();
    if (!source) return {};
    return source->enumerate_devices();
}

std::vector<plugins::AudioDevice>
NimRTCEngine::audio_playback_devices() const noexcept {
    if (!impl_) return {};
    const plugins::IAudioSink* sink = impl_->audio_state.audio_sink.get();
    if (!sink) return {};
    return sink->enumerate_devices();
}

}  // namespace nimrtc::engine
