/**
 * @file src/engine/src/engine_audio_device_pump.cpp
 * @brief The audio device → outbound pump and the device open/close
 *        helpers (remote-desktop plan D1).
 *
 * ## What this file is
 *
 * The engine's *automatic* audio capture path: it pops one frame from
 * the audio source device's bounded queue and runs it through the very
 * same outbound chain the host drives with `send_audio()` — without the
 * host ever pushing PCM.  With a microphone plug-in in place, an engine
 * opened with `audio_source_name = "auto"` produces an outbound audio
 * stream on its own.
 *
 * ## Why the pump lives here, and not in the audio_device module
 *
 * RD-6: the engine is a single-threaded orchestrator.  The *capture
 * device* owns its own thread and fills its bounded queue (that is the
 * device's business); the engine only ever **polls** that queue, from
 * `tick()`.  So the 3A and the codec are touched from exactly one
 * thread — the tick thread — no matter what the capture backend does
 * internally.  The pump therefore does *not* install the source's
 * frame-delivery callback: a callback would run on the device thread,
 * and the 3A / codec seams are not thread-safe.
 *
 * ## Open-failure semantics (different from video!)
 *
 * An `"auto"` device that cannot open is a *normal* machine (no
 * microphone, an occupied port) — the engine warns and degrades to "no
 * device", and `open()` still succeeds.  An *explicit* id that cannot
 * open is a hard error, exactly like the video side's non-empty
 * `video_capture_name` (spec §五).  The distinction is wired into the
 * `PluginFailureMode` passed to `pal::make_plugin`.
 */
#include "engine_device_policy.hpp"
#include "engine_impl.hpp"

#include <cstddef>
#include <cstdint>

namespace nimrtc::engine {

namespace {

/** The pump's read capacity, in samples per channel.
 *
 *  One second at 48 kHz.  The pull path delivers at most one frame per
 *  `read_samples()` call, and the device's own cadence (10–20 ms) is
 *  far below this, so the buffer never truncates a frame. */
constexpr std::size_t kAudioDeviceFrameCapacity = 48000;

/** Resolve `EngineConfig::audio_source_name` / `audio_sink_name` down to
 *  a concrete backend id, applying the "auto" rule.
 *
 *  Empty (host feeds / plays PCM itself) and `"none"` (explicit
 *  opt-out) resolve to "" before this is reached.  `"auto"` resolves
 *  through runtime availability (`resolve_auto_audio_backend`); an
 *  empty result is a *degrade* — the engine warns and continues without
 *  a device.
 *
 *  @param name      The configured name ("" / "none" handled by the
 *                   caller; only "auto" and explicit ids reach here).
 *  @param capture   true for the source side, false for the sink side.
 *  @param is_registered The registry-membership check for the side.
 *  @param auto_what The configuration field name, for the warn line.
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


uint32_t NimRTCEngine::init_audio_devices() noexcept {
    uint32_t first_error = 0;

    // ---- Capture device (the microphone) ------------------------------
    //
    // A non-empty name is the host asking for a device.  "auto" is not a
    // promise (a machine with no microphone is normal); an explicit id
    // is.  So the failure modes differ: "auto" degrades with a warn and
    // `open()` still succeeds, an explicit id reports a hard error and
    // fails `open()` (spec §五).
    if (!config_.audio_source_name.empty() &&
        config_.audio_source_name != "none" && !audio_source_) {
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
                audio_source_,
                config_.audio_source_config,
                {.err_open_failed = core::kEngineInternal,
                 .err_missing = core::kEngineInternal,
                 .open_fail_mode = is_auto
                     ? pal::PluginFailureMode::Warn
                     : pal::PluginFailureMode::Error,
                 .missing_mode = is_auto
                     ? pal::PluginFailureMode::Warn
                     : pal::PluginFailureMode::Error,
                 .on_error = on_error_});
            if (opened) {
                // The id the Stats reports is the one actually negotiated,
                // not the one the host requested: a device may fall back
                // to a different device or format (the seam's contract).
                const plugins::AudioSourceConfig negotiated =
                    audio_source_->config();
                impl_->audio_state.audio_source_device_id =
                    negotiated.device_id;
                impl_->audio_state.audio_source_backend = resolved;
            } else if (!is_auto && first_error == 0) {
                // An explicit id is a promise: it could not be resolved or
                // opened, so the session fails exactly like the video
                // side's non-empty `video_capture_name` (criterion 12).
                first_error = core::kEngineInternal;
            }
        }
    }

    // ---- Playback device (the loudspeaker) ----------------------------
    if (!config_.audio_sink_name.empty() &&
        config_.audio_sink_name != "none" && !audio_sink_) {
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
                audio_sink_,
                config_.audio_sink_config,
                {.err_open_failed = core::kEngineInternal,
                 .err_missing = core::kEngineInternal,
                 .open_fail_mode = is_auto
                     ? pal::PluginFailureMode::Warn
                     : pal::PluginFailureMode::Error,
                 .missing_mode = is_auto
                     ? pal::PluginFailureMode::Warn
                     : pal::PluginFailureMode::Error,
                 .on_error = on_error_});
            if (opened) {
                const plugins::AudioSinkConfig negotiated =
                    audio_sink_->config();
                impl_->audio_state.audio_sink_device_id = negotiated.device_id;
                impl_->audio_state.audio_sink_backend = resolved;
                impl_->audio_state.audio_sink_played_baseline =
                    audio_sink_->played_samples();
            } else if (!is_auto && first_error == 0) {
                first_error = core::kEngineInternal;
            }
        }
    }

    return first_error;
}


void NimRTCEngine::close_audio_devices() noexcept {
    if (audio_source_) {
        audio_source_->close();
        audio_source_.reset();
    }
    if (audio_sink_) {
        audio_sink_->close();
        audio_sink_.reset();
    }
    impl_->audio_state.audio_source_device_id.clear();
    impl_->audio_state.audio_source_backend.clear();
    impl_->audio_state.audio_sink_device_id.clear();
    impl_->audio_state.audio_sink_backend.clear();
    impl_->audio_state.audio_sink_played_baseline = 0;
}


bool NimRTCEngine::pump_audio_capture() noexcept {
    if (!is_open()) return false;
    if (!audio_source_) return false;

    auto& astate = impl_->audio_state;

    // ---- Pull one frame (non-blocking) --------------------------------
    //
    // The device's queue is empty most of the time: the capture cadence
    // (10–20 ms) is comparable to the tick rate, and `read_samples()`
    // never blocks, so an empty queue is the normal case.
    if (impl_->scratch.audio_device_pcm_buf.size() < kAudioDeviceFrameCapacity) {
        impl_->scratch.audio_device_pcm_buf.resize(kAudioDeviceFrameCapacity);
    }
    std::size_t frames = 0;
    const plugins::Status rc = audio_source_->read_samples(
        impl_->scratch.audio_device_pcm_buf.data(),
        kAudioDeviceFrameCapacity,
        &frames);
    if (rc != plugins::kOk) {
        // An empty queue is not an error.  Anything else (a device that
        // lost its backend, a malformed read) is.
        if (rc != plugins::kErrNotReady) {
            ++astate.audio_device_pump_errors;
        }
        return false;
    }
    if (frames == 0) return false;

    // ---- Feed the outbound path ---------------------------------------
    //
    // The *same* chain the host drives with `send_audio()` — 3A → codec
    // → RTP → SRTP → transport — so the automatic and the manual paths
    // cannot drift apart.  The 3A and the codec are touched here, on the
    // tick thread, never on the capture device's thread (RD-6).
    if (send_audio(impl_->scratch.audio_device_pcm_buf.data(), frames) != 0) {
        ++astate.audio_device_pump_errors;
        return false;
    }
    ++astate.audio_device_pumped_frames;
    return true;
}

}  // namespace nimrtc::engine
