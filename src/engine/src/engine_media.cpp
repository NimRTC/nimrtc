/**
 * @file src/engine/src/engine_media.cpp
 * @brief Engine Media plane (v1.1 R1). Audio modules + send_audio, video plugins + send path, RTCP feedback queues, stats()..
 *
 * v1.1 R1 split of engine.cpp - mechanical move,
 * behaviour unchanged. Shares engine_impl.hpp.
 */
#include "engine_impl.hpp"

// v1.2.0 (video plan F8): the per-stream share rule of the session
// bandwidth budget.  The audio path is the first consumer — the audio
// is the session's *reserved* stream.
#include "bandwidth_allocation.hpp"

// Remote-desktop plan B12 / B13: the ONE place the engine computes the
// encoder's parameter set.
#include "engine_video_params.hpp"

// Remote-desktop plan H1 / H2: the staging step is shared by every
// video send path (the default stream's and each extra stream's), so
// the colour-space rule lives in one header rather than in this
// translation unit's anonymous namespace.
#include "engine_video_staging.hpp"

namespace nimrtc::engine {

// The staging step's names are used unqualified below; they come from
// the shared collaborator (see the include above).
using detail::ConversionKind;
using detail::stage_source_frame;

namespace {

/** Default audio encoder bitrate for a mono stream, in bits per second.
 *
 *  The engine is the policy layer for the audio path, so it owns the
 *  default bitrate the codec plugin is configured with.  32 kbps is the
 *  mono number every browser negotiates; 64 kbps is the stereo one. */
constexpr int kDefaultAudioBitrateMonoBps   = 32000;
constexpr int kDefaultAudioBitrateStereoBps = 64000;

/** Size of the SRTP authentication tag (RFC 3711 §3.5 — the default
 *  AES_CM_128_HMAC_SHA1_80 profile appends a 10-byte tag).
 *
 *  Used as the capacity hint when acquiring an outbound packet buffer from
 *  the scheduler's pool, so the protected packet fits without a
 *  reallocation. */
constexpr std::size_t kSrtpAuthTagBytes = 10;

} // anonymous namespace


// ---------------------------------------------------------------------------
// Phase 4 — audio3a + codec plugin creation.
//
// Original order: set_callbacks() BEFORE open().  We preserve that
// ordering via make_plugin's post_create hook.  The audio3a plugin's
// on_error callback bridges into the engine's on_error_ via a
// captured-this lambda.
//
// Failure policy:
//   - audio3a — silent on missing (concrete NullAudio3A fallback below
//     covers the gap), Error on open fail (callers want to know the
//     configured plugin misbehaved even though we fall back).
//   - codec — Silent on missing / open fail / create returns null.  The
//     engine falls back to the statically-linked opus module
//     (NIMRTC_HAS_OPUS).  Callers do not need an error notification
//     because the fallback is "the same codec from a different path".
//     This matches the pre-Slice-1.5 behaviour.
//
// Returns void — failures surface through `on_error_` and the
// `unique_ptr` slot is reset to `nullptr` so the fallback path runs.
// ---------------------------------------------------------------------------
void NimRTCEngine::init_audio_modules() noexcept {
    // ---- Audio3A --------------------------------------------------------
    (void)pal::make_plugin<&pal::resolve_audio3a>(
        config_.audio3a_name,
        audio3a_plugin_,
        /*config=*/nullptr,                // IAudio3AFactory::create()
                                           // takes no config (see
                                           // nullptr_t branch in helper)
        {.err_open_failed = core::kAudio3APluginOpenFailed,
         .open_fail_mode = pal::PluginFailureMode::Error,
         .missing_mode = pal::PluginFailureMode::Silent,  // missing → silent fallback
         .on_error = on_error_},
        /*post_create=*/[this](plugins::IAudio3A& p) {
            p.set_callbacks(
                [](bool) {}, [](float) {},
                [this](std::uint32_t err, std::string_view msg) {
                    if (on_error_) on_error_(err, msg);
                });
        });
    // Report a degraded 3A backend explicitly.
    //
    // The audio3a plugin may have fallen back to a passthrough stub when the
    // requested backend (e.g. the WebRTC APM) is not available in this
    // build.  The instance carries that state in its name, so surface it
    // here instead of letting a passthrough pass as "3A" unnoticed.
    if (audio3a_plugin_) {
        const char* pname = audio3a_plugin_->name();
        if (pname != nullptr && std::strstr(pname, "DEGRADED") != nullptr) {
            core::log::Logger::instance().error(
                std::string("3A backend is DEGRADED: ") + pname +
                " — the configured 3A implementation is not running; "
                "AEC/ANS/AGC are inactive.");
        }
    }
    if (!audio3a_plugin_) {
        impl_->audio3a_concrete = std::make_unique<audio3a::NullAudio3A>();
        audio3a::Config a3a;
        a3a.sample_rate_hz    = config_.pcm_sample_rate_hz;
        a3a.capture_channels = config_.pcm_channels;
        a3a.render_channels   = config_.pcm_channels;
        impl_->audio3a_concrete->init(a3a);
    }

    // ---- Codec ---------------------------------------------------------
    {
        const int bitrate_ceiling = resolve_audio_bitrate_ceiling();
        plugins::CodecConfig codec_cfg{};
        codec_cfg.sample_rate_hz = config_.pcm_sample_rate_hz;
        codec_cfg.channels       = config_.pcm_channels;
        // The audio encoder's target bitrate.  0 means "codec default":
        // 32 kbps for mono (the default `pcm_channels` is 1), 64 kbps
        // for stereo.  The resolved value is also the ceiling of the
        // BWE-linked adaptation (see maybe_update_audio_bitrate()).
        codec_cfg.bitrate_bps    = bitrate_ceiling;
        codec_cfg.complexity     = 10;
        codec_cfg.fec_enabled    = config_.audio_use_inband_fec;
        // The in-band redundancy (spec B-8): the encoder embeds a
        // low-bitrate copy of the previous frame in every frame, which
        // is what lets the receiver reconstruct a lost frame instead of
        // concealing it.
        codec_cfg.packet_loss_perc = config_.audio_redundancy_percent;
        // DTX is only honoured when the peer signals `usedtx=1`
        // (spec E-3); the SDP side of that negotiation lives in
        // `engine_signaling.cpp`.  The encoder is configured with the
        // host's request, and the *offer* advertises it.
        codec_cfg.dtx_enabled    = config_.audio_use_dtx;
        codec_cfg.vad_enabled    = false;
        codec_cfg.payload_type   = config_.audio_codec.payload_type;
        codec_cfg.name           = config_.codec_name;
        (void)pal::make_plugin<&pal::resolve_codec>(
            config_.codec_name,
            codec_plugin_,
            std::move(codec_cfg),
            {.open_fail_mode = pal::PluginFailureMode::Silent,
             .missing_mode = pal::PluginFailureMode::Silent,
             .on_error = on_error_});
        // The adaptation controller starts at the ceiling and only ever
        // moves down from there (unless the host lowers the ceiling).
        audio_bitrate_ceiling_bps_ = bitrate_ceiling;
        audio_bitrate_current_bps_ = bitrate_ceiling;
        // WP-7: the concrete `opus::Encoder` fallback is gone.  The
        // engine reaches the encoder through the codec seam only, so
        // `src/engine/` names no concrete module (constraint L4).  When
        // the configured codec plugin is missing, `send_audio()` reports
        // the failure (throttled) instead of silently encoding with a
        // different codec — a silent codec swap would put a bit-stream
        // on the wire that the peer did not negotiate.
    }

    // ---- Audio receive pipeline (v1.2.0, audio pipeline WP-7) ---------
    //
    // The receive side of the audio path is a plugin, not engine code
    // (spec B-11 / B-16): the engine resolves a *decoder* and a
    // *receiver* through the registry and drives the receiver alone.
    //
    // Failure policy (B-16) — there is no fallback receiver:
    //   - an empty `audio_decoder_name` / `audio_receiver_name` disables
    //     the audio track explicitly and silently (the host asked for
    //     it);
    //   - a decoder that is not registered is a hard error
    //     (`kAudioDecoderMissing`) that fires `on_error` exactly once;
    //   - a receiver that cannot be created is a hard error
    //     (`kAudioReceiverMissing`) that fires `on_error` exactly once.
    // In both error cases the audio track stays off: inbound packets are
    // counted and dropped, never decoded.
    {
        const bool want_decoder  = !config_.audio_decoder_name.empty();
        const bool want_receiver = !config_.audio_receiver_name.empty();

        if (!want_decoder || !want_receiver) {
            // Explicitly disabled by the host — no error, no pipeline.
            impl_->audio_state.audio_rx_disabled = true;
            core::log::Logger::instance().info(
                "engine[audio]: the audio receive pipeline is disabled by "
                "configuration (decoder=" +
                std::string(config_.audio_decoder_name) +
                ", receiver=" + std::string(config_.audio_receiver_name) + ")");
            return;
        }

        // ---- Step 1: the decoder -------------------------------------
        //
        // The receiver is useless without a decoder, so the decoder is
        // resolved first.  The lookup goes through the registry's
        // audio-decoder slot, so the answer reflects the plugins the
        // host really installed.
        auto& reg = core::PluginRegistry::instance();
        if (reg.get_audio_decoder(config_.audio_decoder_name) == nullptr) {
            impl_->audio_state.audio_rx_disabled = true;
            if (on_error_) {
                on_error_(core::kAudioDecoderMissing,
                          "the audio decoder plugin '" +
                          std::string(config_.audio_decoder_name) +
                          "' is not registered — the audio track is disabled");
            }
            return;
        }

        // ---- Step 2: the receiver -----------------------------------
        plugins::AudioReceiverConfig rx_cfg{};
        rx_cfg.jb_plugin_id           = std::string(config_.jb_name);
        rx_cfg.decoder_plugin_id      = std::string(config_.audio_decoder_name);
        rx_cfg.clock_rate_hz          = config_.audio_codec.clock_rate;
        rx_cfg.decoder_sample_rate_hz = config_.pcm_sample_rate_hz;
        rx_cfg.decoder_channels       = config_.audio_codec.channels;
        rx_cfg.output_channels        = config_.pcm_channels;
        rx_cfg.frame_duration_ms      = config_.audio_ptime_ms;
        // The audio stream's own target depth (spec E-3).  A host that
        // left it at the default gets the module's own default; a host
        // that configured it gets exactly that value.  The old
        // `jb_initial_delay_ms` (the *video* jitter buffer's knob) is
        // not the audio stream's knob — using it here made the audio
        // depth depend on a video setting.
        rx_cfg.target_delay_ms        = static_cast<std::uint16_t>(
            config_.audio_jitter_target_delay_ms < 0
                ? 0 : config_.audio_jitter_target_delay_ms);
        rx_cfg.min_delay_ms           = static_cast<std::uint16_t>(
            config_.jb_min_delay_ms < 0 ? 0 : config_.jb_min_delay_ms);
        rx_cfg.max_delay_ms           = static_cast<std::uint16_t>(
            config_.jb_max_delay_ms < 0 ? 0 : config_.jb_max_delay_ms);
        rx_cfg.max_concealed_frames   = static_cast<std::uint16_t>(
            config_.audio_max_concealed_frames < 0
                ? 0 : config_.audio_max_concealed_frames);
        // In-band FEC is what the SDP signals, so the decoder must be
        // told to use it as well (spec B-8) — and the host must be able
        // to turn it off (spec E-3).
        rx_cfg.enable_fec_recovery    = config_.audio_use_inband_fec;
        // The redundancy the peer is assumed to send — the decoder's
        // own hint (spec B-8).  It is the same value the encoder was
        // given, so the two ends agree on how much redundancy is in
        // flight.
        rx_cfg.packet_loss_perc       = config_.audio_redundancy_percent;
        rx_cfg.enable_downmix         = config_.audio_downmix_to_pcm_channels;
        if (!config_.audio_downmix_to_pcm_channels) {
            // Without the downmix the pipeline keeps the decoder's own
            // channel count (spec B-9).
            rx_cfg.output_channels = config_.audio_codec.channels;
        }

        const bool created = pal::make_plugin<&pal::resolve_audio_receiver>(
            config_.audio_receiver_name,
            audio_receiver_,
            std::move(rx_cfg),
            {.err_open_failed = core::kAudioReceiverMissing,
             .err_missing = core::kAudioReceiverMissing,
             .open_fail_mode = pal::PluginFailureMode::Error,
             .missing_mode = pal::PluginFailureMode::Error,
             .on_error = on_error_},
            /*post_create=*/[this](plugins::IAudioReceiver& r) {
                // The callbacks are installed *after* the factory has
                // created the receiver (and opened it), so the receiver
                // cannot report a missing plugin through them — that is
                // why the factory returns a null product instead (B-16).
                r.set_callbacks(
                    [this](const plugins::AudioOutputFrame& frame) {
                        on_audio_frame_decoded(frame);
                    },
                    [this](plugins::Status status, std::string_view msg) {
                        if (on_error_) {
                            on_error_(core::kAudioDecodeFailed, msg);
                        }
                        (void)status;
                    });
            });

        if (!created || !audio_receiver_) {
            // `make_plugin` already reported the hard error exactly
            // once; the audio track stays off.
            impl_->audio_state.audio_rx_disabled = true;
            audio_receiver_.reset();
        }
    }
}


// ---------------------------------------------------------------------------
// Audio receive callback (v1.2.0, audio pipeline WP-7)
// ---------------------------------------------------------------------------
//
// The receive pipeline hands the *decoded* frame over.  The engine's job
// here is small but not trivial:
//
//   * The 3A render reference must be the **decoded** signal, not the
//     inbound bit-stream (spec A-10).  The echo canceller subtracts the
//     loudspeaker signal from the microphone capture; feeding it the
//     compressed payload would make it converge on noise — and the old
//     code path did exactly that (it reinterpreted the RTP payload as
//     float PCM, defect F-1).
//   * The legacy PCM tap keeps its historical meaning: the `size_t`
//     argument is the number of **interleaved** floats (spec O-2), i.e.
//     `samples_per_channel * channels`.
//
// The frame's PCM belongs to the receiver and is only valid for the
// duration of this call (the seam's contract).
void NimRTCEngine::on_audio_frame_decoded(const plugins::AudioOutputFrame& frame) noexcept {
    if (frame.pcm == nullptr || frame.samples_per_channel == 0) return;

    const std::size_t samples_per_ch = frame.samples_per_channel;
    const std::uint8_t channels = frame.channels != 0 ? frame.channels : 1;
    const std::size_t total_floats = samples_per_ch * channels;

    ++impl_->audio_state.audio_frames_received;
    impl_->audio_state.audio_samples_received += samples_per_ch;
    if (frame.concealed)     ++impl_->audio_state.audio_frames_concealed;
    if (frame.fec_recovered) ++impl_->audio_state.audio_frames_fec_recovered;

    // AEC reference path.
    //
    // The remote audio we are about to hand to the host for loudspeaker
    // playback is exactly the signal the echo canceller has to subtract
    // from the microphone capture.  Feeding it to the 3A plugin *before*
    // the frame reaches the output keeps the capture and reference paths
    // aligned.  Without this the APM has no reverse stream and the echo
    // canceller cannot converge at all.
    if (audio3a_plugin_) {
        audio3a_plugin_->process_render(frame.pcm, samples_per_ch, channels);
    }

    if (on_audio_frame_) {
        on_audio_frame_(frame.pcm, total_floats);
    }

    // The detailed callback fires *after* the legacy one (spec E-2):
    // both receive the same buffer, and the detailed one adds the
    // metadata (RTP timestamp, playout instant, capture instant, the
    // concealment / FEC flags).
    deliver_audio_frame_info(frame);

    // The frame has been handed over for playback — tell the 3A plugin
    // that the reference signal reached the speaker so it can track the
    // system delay.
    if (audio3a_plugin_) {
        audio3a_plugin_->set_render_delivered(samples_per_ch);
    }

    // ---- The audio side of the render clock (remote-desktop plan E5) ---
    //
    // The video half of the A/V sync compares the picture's capture
    // instant with the *sound* that is at the speaker now.  The sound's
    // capture instant is the peer's capture timestamp of the frame that
    // was just played out — the same ADR-017 reconstruction the
    // detailed audio callback publishes.
    //
    // It is recorded here (rather than in the detailed callback) because
    // the detailed callback is optional — a host that did not install
    // it would otherwise silently lose the audio reference and the
    // render clock would never align anything.
    {
        const std::int64_t capture_us = audio_capture_time_us(
            impl_ != nullptr ? impl_->audio_state.audio_rx.ssrc : 0,
            frame.rtp_timestamp);
        if (capture_us != 0) {
            impl_->render.audio_playout_capture_us = capture_us;
        }
    }
}


void NimRTCEngine::set_video_sink(std::unique_ptr<plugins::IVideoSink> sink) noexcept {
    if (video_sink_) {
        video_sink_->close();
        video_sink_.reset();
    }
    video_sink_ = std::move(sink);
    if (video_sink_) {
        if (video_sink_->open() != plugins::kOk) {
            if (on_error_) on_error_(core::kVideoSinkOpenFailed, "video_sink open failed");
            video_sink_.reset();
        }
    }
}


void NimRTCEngine::set_video_source(
    std::unique_ptr<plugins::IVideoSource> source) noexcept {
    if (video_source_) {
        video_source_->close();
        video_source_.reset();
    }
    video_source_ = std::move(source);
    if (video_source_) {
        if (video_source_->open() != plugins::kOk) {
            if (on_error_) on_error_(core::kVideoSourceOpenFailed, "video_source open failed");
            video_source_.reset();
        }
    }
}


uint32_t NimRTCEngine::init_video_plugins() noexcept {
    // PHASE 1 — create all plugins (order: sink, source, receiver, sender,
    // codec). The 5-step factory dance for each plugin is consolidated
    // into pal::make_plugin() (PAL Slice 1.5); see make_plugin.hpp.
    // PHASE 2 — wire pipeline callbacks (down below the make_plugin
    // blocks).  Splitting the function into two phases ensures that by
    // the time any callback fires, every plugin slot is already filled.

    // ---- Video quality statistics sink (Phase 4.2 / D2) ------------------
    //
    // The sink is the *single* owner of the per-stream quality counters
    // (frames, bytes, bitrate EMA, freeze detection, QP accumulation,
    // drop attribution).  Before this wiring the module was a complete,
    // tested implementation that no engine ever loaded — the worst state
    // a module can be in ("the feature exists, so it must work").
    //
    // The sink is created here (not in the constructor) so a session
    // that never touches video does not pay for it, and so a re-open()
    // starts from a clean slate.
    impl_->video_state.video_stats.ensure_created();

    // Remote-desktop plan H1: the video stream set's clock starts with
    // the session, so the per-stream rates (H4) are measured against a
    // well-defined origin from the first frame on.
    impl_->video_state.video_streams.mark_started(detail::steady_now_us());

    // Remote-desktop plan D5: the profile may declare the *default*
    // stream's share.  The declaration is applied here — at open(), when
    // the default stream becomes real — and `set_video_stream_weight()`
    // overrides it afterwards.
    //
    // A weight of 0 for the default stream is refused for the same
    // reason `set_video_stream_weight()` refuses it: the default stream
    // is the session's primary video path, and a host that wants it to
    // sit out must remove the video path, not mute it.
    {
        const auto declared = config_.video_stream_weights.find(
            std::string(detail::kDefaultVideoStreamId));
        if (declared != config_.video_stream_weights.end() &&
            declared->second > 0) {
            impl_->video_state.video_streams.default_weight = declared->second;
        }
    }

    // ---- Our RTCP feedback SSRC (v1.1 Q3) ---------------------------------
    //
    // RFC 3550 §6.1 identifies the sender of an RTCP packet by its SSRC.
    // The feedback we emit (NACK / PLI) is sent by *us* as the receiver of
    // the media, so it needs an SSRC of its own — distinct from the media
    // SSRCs, which identify the streams rather than the reporter.
    if (impl_->video_state.video_feedback_ssrc_ == 0) {
        std::random_device rd;
        std::mt19937_64 mt(rd());
        impl_->video_state.video_feedback_ssrc_ = static_cast<std::uint32_t>(mt() & 0xFFFFFFFFu);
        if (impl_->video_state.video_feedback_ssrc_ == 0) impl_->video_state.video_feedback_ssrc_ = 1;
    }

    // ---- Video Capture device (remote-desktop plan A1) --------------------
    //
    // The capture seam is *optional*: an empty video_capture_name means
    // "the host feeds frames itself", which is the normal case.  A
    // non-empty name is a hard promise — the user asked for a specific
    // capture device — so an unknown id or a device that refuses to open
    // is reported as an error rather than silently downgraded to "no
    // capture device".  Silently ignoring it would turn a typo into a
    // session that quietly never produces a frame.
    //
    // Note that the capture device is NOT wired into the encoder here.
    // The engine hands the device to the host through video_capture();
    // turning a device into a frame stream is an adapter's job, and the
    // automatic capture → encoder pump is remote-desktop plan item A10.
    if (!config_.video_capture_name.empty() && !video_capture_) {
        // The capture request comes from the host (`EngineConfig::
        // video_capture_config`).  It is handed to the device verbatim:
        // the device owns the negotiation, and the engine must not
        // second-guess which display or which size the host meant.
        //
        // One exception: `EngineConfig::video_zero_copy` asks the *send*
        // path to avoid a host copy, which only makes sense if the
        // capture side can deliver a GPU surface too.  When the host
        // left the format's `zero_copy` flag alone and asked for
        // zero-copy, the request says so — and a device that cannot do
        // it is reported at `open()` time, which is exactly what the
        // check below is for.
        plugins::CaptureConfig capture_cfg = config_.video_capture_config;
        if (capture_cfg.format.zero_copy == false && config_.video_zero_copy) {
            capture_cfg.format.zero_copy = true;
        }

        (void)pal::make_plugin<&pal::resolve_video_capture>(
            config_.video_capture_name,
            video_capture_,
            std::move(capture_cfg),
            {.err_open_failed = core::kVideoCaptureOpenFailed,
             .err_missing = core::kVideoCapturePluginMissing,
             .open_fail_mode = pal::PluginFailureMode::Error,
             .missing_mode = pal::PluginFailureMode::Error,
             .on_error = on_error_});
    }

    // ---- The capture request vs. the device's capabilities ---------------
    //
    // The frozen `CaptureCapabilities` / `CaptureFormat` semantics
    // (remote-desktop plan A1) are checked HERE, at `open()` time.  An
    // unsupported combination — a zero-copy format on a device that
    // cannot do zero-copy, dirty-rect metadata the device does not
    // produce, a size above the device's maximum — is a *configuration*
    // condition, and reporting it up front is the whole point: the
    // alternative is a session that opens cleanly and never produces a
    // frame, which is indistinguishable from a broken peer.
    if (video_capture_) {
        const plugins::CaptureCapabilities caps = video_capture_->capabilities();
        // The *request* is what is checked — not the negotiated result.
        // The negotiated format is by construction within the device's
        // capabilities (the device would have refused otherwise); the
        // request is what can be wrong.
        const plugins::CaptureConfig& requested = config_.video_capture_config;

        std::vector<plugins::CaptureFormat> offered;
        if (!requested.device_id.empty()) {
            offered = video_capture_->supported_formats(requested.device_id);
        }

        // The zero-copy request derived from `EngineConfig::video_zero_copy`
        // has to be part of what is checked, so the check sees the same
        // request the device was given.
        plugins::CaptureConfig effective = requested;
        if (!effective.format.zero_copy && config_.video_zero_copy) {
            effective.format.zero_copy = true;
        }

        const detail::CaptureCapabilityCheck check =
            detail::check_capture_capabilities(caps, effective, offered);
        if (!check.ok()) {
            const std::string msg =
                std::string{"engine: the capture device '"} +
                config_.video_capture_name +
                "' does not support the requested configuration (" +
                std::string{detail::capture_violation_name(check.violation)} +
                "); the request is refused before the first frame.";
            core::log::Logger::instance().error(msg);
            if (on_error_) on_error_(core::kVideoCaptureUnsupported, msg);
        }
    }

    // ---- Video Sink --------------------------------------------------------
    if (!video_sink_) {
        // PAL Slice 1.5 — replaces the 5-step "factory → create → null-check
        // → open → report" dance with one helper call. The Error/Error
        // policy matches the original on_error_ path.
        (void)pal::make_plugin<&pal::resolve_video_sink>(
            config_.video_sink_name,
            video_sink_,
            resolve_sink_config(),                    // plan E7: the presentation
            {.err_open_failed = core::kVideoSinkOpenFailed,
             .err_missing = core::kVideoSinkPluginMissing,
             .open_fail_mode = pal::PluginFailureMode::Error,
             .missing_mode = pal::PluginFailureMode::Error,
             .on_error = on_error_});
    }

    // ---- Video Bitrate Controller (remote-desktop plan D1) -----------------
    //
    // The ladder is resolved through the registry's
    // video-bitrate-controller slot, never by naming a concrete module
    // (Layout Invariant 4).  An empty id means "no controller": the
    // encoder then keeps the bitrate it was configured with, which is
    // the pre-D4 behaviour.
    if (!video_bitrate_controller_ && !config_.video_bitrate_controller_name.empty()) {
        plugins::VideoBitrateControllerConfig bcfg{};
        bcfg.mode               = config_.video_ladder_mode;
        bcfg.source_width       = config_.video_width;
        bcfg.source_height      = config_.video_height;
        bcfg.source_fps         = config_.video_fps;
        bcfg.min_height         = config_.video_resolution_floor_height;
        if (config_.video_start_bitrate_bps > 0) {
            bcfg.source_bitrate_bps = config_.video_start_bitrate_bps;
        }

        const auto* factory =
            pal::resolve_video_bitrate_controller(
                config_.video_bitrate_controller_name);
        if (factory == nullptr) {
            if (on_error_) {
                on_error_(core::kEngineInvalidParam,
                          "engine: no video bitrate controller plugin named '" +
                              config_.video_bitrate_controller_name + "'");
            }
            impl_->session_state.last_open_rc = core::kEngineInvalidParam;
            shutdown_video_plugins();
            return core::kEngineInvalidParam;
        }

        std::unique_ptr<plugins::IVideoBitrateController> owned(
            factory->create(bcfg));
        if (!owned) {
            if (on_error_) {
                on_error_(core::kEngineInvalidParam,
                          "engine: the video bitrate controller plugin "
                          "failed to create a controller");
            }
            impl_->session_state.last_open_rc = core::kEngineInvalidParam;
            shutdown_video_plugins();
            return core::kEngineInvalidParam;
        }
        if (owned->open() != plugins::kOk) {
            if (on_error_) {
                on_error_(core::kEngineInvalidParam,
                          "engine: the video bitrate controller plugin "
                          "failed to open");
            }
            impl_->session_state.last_open_rc = core::kEngineInvalidParam;
            shutdown_video_plugins();
            return core::kEngineInvalidParam;
        }
        video_bitrate_controller_ = std::move(owned);
    }

    // ---- Video Source ------------------------------------------------------
    if (!video_source_) {
        plugins::VideoSourceConfig cfg{};
        cfg.width  = config_.video_width;
        cfg.height = config_.video_height;
        cfg.fps    = config_.video_fps;
        cfg.ssrc   = config_.video_sender_tuning.ssrc;
        (void)pal::make_plugin<&pal::resolve_video_source>(
            config_.video_source_name,
            video_source_,
            std::move(cfg),
            {.err_open_failed = core::kVideoSourceOpenFailed,
             .err_missing = core::kVideoSourcePluginMissing,
             .open_fail_mode = pal::PluginFailureMode::Error,
             .missing_mode = pal::PluginFailureMode::Error,
             .on_error = on_error_});
    }

    // ---- Video Codec (H.264 / H.265 / AV1) ---------------------------------
    //
    // The codec is created FIRST (before the receiver and the sender),
    // because the receiver and the sender need to know *which* codec
    // family the session uses: their configs carry a `VideoCodecKind`
    // that selects the packetizer / depacketizer, and their payload
    // type has to match the codec's own.
    //
    // The family is derived from the plugin id (`EngineConfig::
    // video_codec_name`) and cross-checked against the instantiated
    // codec's `kind()`.  The plugin id is the user's instruction; the
    // `kind()` is the plugin's own claim.  When they disagree the
    // plugin's claim wins — the *wire* format follows what the codec
    // actually produces, and the SDP is derived from the same value so
    // the two cannot drift apart.
    detail::VideoCodecFamily codec_family =
        detail::codec_family_from_plugin_id(config_.video_codec_name);

    // The values the capability check (B1) compares against.  They are
    // captured before the config object is moved into the factory,
    // because after the move the config is in a moved-from state.
    const std::uint32_t want_bitrate_bps = 1'000'000;
    const plugins::VideoPixelFormat want_pixel_format =
        plugins::VideoPixelFormat::kI420;

    if (!config_.video_codec_name.empty() && !video_codec_) {
        plugins::VideoCodecConfig cfg{};
        cfg.width = config_.video_width;
        cfg.height = config_.video_height;
        cfg.fps = config_.video_fps;
        cfg.bitrate_bps = want_bitrate_bps;
        cfg.keyframe_interval = 60;
        cfg.pixel_format = want_pixel_format;
        cfg.codec = detail::codec_kind_of(codec_family);
        cfg.payload_type = config_.video_receiver_tuning.payload_type;
        cfg.name = config_.video_codec_name;

        // ---- The backend preference (remote-desktop plan B2) --------------
        //
        // `EngineConfig::video_codec_backend` says which *class* of
        // backend to use: hardware only, software only, or (the
        // default) "hardware first, software as the fallback".
        //
        // An unrecognised spelling is an error rather than a silent
        // fallback: a host that wrote "hardwre" and got software would
        // have a 1080p30 budget it cannot meet, and would not know why.
        plugins::VideoCodecBackendPreference backend_pref =
            plugins::VideoCodecBackendPreference::kAuto;
        if (!plugins::parse_video_codec_backend_preference(
                config_.video_codec_backend, backend_pref)) {
            const std::string msg =
                std::string{"engine: the video codec backend preference '"} +
                config_.video_codec_backend +
                "' is not one of 'auto', 'hardware', 'software'";
            core::log::Logger::instance().error(msg);
            if (on_error_) on_error_(core::kEngineInvalidParam, msg);
            impl_->session_state.last_open_rc = core::kEngineInvalidParam;
            shutdown_video_plugins();
            return core::kEngineInvalidParam;
        }
        cfg.backend_preference = backend_pref;
        impl_->video_state.video_codec_backend_preference = backend_pref;

        const bool codec_created = pal::make_plugin<&pal::resolve_video_codec>(
            config_.video_codec_name,
            video_codec_,
            std::move(cfg),
            {.err_open_failed = core::kVideoCodecOpenFailed,
             .err_missing = core::kVideoCodecPluginMissing,
             .open_fail_mode = pal::PluginFailureMode::Error,
             .missing_mode = pal::PluginFailureMode::Error,
             .on_error = on_error_});

        // Plan B2: the codec could not be created.  When the host pinned
        // a backend class, the reason is almost always "no backend of
        // that class exists here" — and that deserves its own message,
        // because the generic "plugin missing" wording sends the reader
        // looking for a *registration* problem that does not exist.
        if (!codec_created && !video_codec_ &&
            backend_pref != plugins::VideoCodecBackendPreference::kAuto) {
            const std::string msg =
                std::string{"engine: the video codec backend class '"} +
                std::string{plugins::video_codec_backend_preference_name(
                    backend_pref)} +
                "' is not available on this machine (the codec plugin '" +
                config_.video_codec_name +
                "' could not satisfy it). Use 'auto' to fall back to "
                "software automatically.";
            core::log::Logger::instance().error(msg);
            if (on_error_) on_error_(core::kVideoCodecPluginMissing, msg);
            impl_->session_state.last_open_rc = core::kVideoCodecPluginMissing;
            shutdown_video_plugins();
            return core::kVideoCodecPluginMissing;
        }
    }

    // ---- The capability check (remote-desktop plan B1) ----------------------
    //
    // B1's acceptance criterion says it in one sentence:
    //
    //   > "the engine rejects an unsupported resolution at `open()` with a
    //   >  clear error instead of failing at the first frame"
    //
    // The capability query is what makes that possible.  Without it the
    // engine had no idea what the codec could do, so an impossible
    // request (4096x2160 on a codec that tops out at 1920x1080) was
    // accepted at `open()` and only failed on the *first frame* — which
    // reads, to the host, like a network problem.
    //
    // The check runs after the codec is open (an unopened codec cannot
    // be asked about its limits) and before the codec family is
    // resolved, so the failure is reported at the right place in the
    // sequence: the session does not start with a codec that cannot
    // deliver what was asked for.
    if (video_codec_) {
        const plugins::VideoCodecCapabilities caps = video_codec_->capabilities();
        const std::uint32_t want_w = config_.video_width;
        const std::uint32_t want_h = config_.video_height;
        const std::uint32_t want_fps = config_.video_fps;

        const bool bad_size = want_w != 0 && want_h != 0 &&
                              !caps.accepts_size(want_w, want_h);
        const bool bad_fps  = !caps.accepts_fps(want_fps);
        const bool bad_rate = !caps.accepts_bitrate(want_bitrate_bps);
        const bool bad_fmt  = !caps.accepts_pixel_format(want_pixel_format);

        if (bad_size || bad_fps || bad_rate || bad_fmt) {
            std::string why;
            if (bad_size) {
                why = std::string{"the requested resolution "} +
                      std::to_string(want_w) + "x" + std::to_string(want_h) +
                      " is outside the codec's supported range";
                if (caps.max_width != 0 || caps.max_height != 0) {
                    why += std::string{" (maximum "} +
                           std::to_string(caps.max_width) + "x" +
                           std::to_string(caps.max_height) + ")";
                }
            } else if (bad_fps) {
                why = std::string{"the requested frame rate "} +
                      std::to_string(want_fps) +
                      " fps exceeds the codec's maximum";
                if (caps.max_fps != 0) {
                    why += std::string{" (maximum "} + std::to_string(caps.max_fps) +
                           " fps)";
                }
            } else if (bad_rate) {
                why = std::string{"the requested bitrate "} +
                      std::to_string(want_bitrate_bps) +
                      " bps exceeds the codec's maximum";
                if (caps.max_bitrate_bps != 0) {
                    why += std::string{" (maximum "} +
                           std::to_string(caps.max_bitrate_bps) + " bps)";
                }
            } else {
                why = "the requested pixel format is not accepted by the codec";
            }

            const std::string msg =
                std::string{"engine: the video codec '"} +
                config_.video_codec_name + "' cannot deliver the requested "
                "stream: " + why +
                ". The request is rejected at open() rather than failing at "
                "the first frame.";
            core::log::Logger::instance().error(msg);
            if (on_error_) on_error_(core::kVideoCodecUnsupported, msg);
            impl_->session_state.last_open_rc = core::kVideoCodecUnsupported;
            shutdown_video_plugins();
            return core::kVideoCodecUnsupported;
        }
    }

    // The instantiated codec is the authority on the codec family.  A
    // composite codec (NVENC + NVDEC) reports the family of the
    // sub-codecs it wraps, so this works for the hardware path too.
    //
    // v1.2.0 (remediation plan B6): the resolution order is now
    // *explicit* — `kind()` first, the plugin id only as a fallback,
    // and when neither yields a family the engine says so instead of
    // continuing with `kUnknown`.
    if (video_codec_) {
        const plugins::VideoCodecKind kind = video_codec_->kind();
        const detail::VideoCodecFamily from_plugin =
            detail::codec_family_from_plugin_id(config_.video_codec_name);
        detail::VideoCodecFamily from_kind =
            detail::codec_family_from_codec_kind(kind);
        if (from_kind != detail::VideoCodecFamily::kUnknown) {
            codec_family = from_kind;
        } else if (from_plugin != detail::VideoCodecFamily::kUnknown) {
            codec_family = from_plugin;
        } else {
            // Neither the codec nor its id names a family the engine
            // knows.  Continuing would put an empty `a=rtpmap:`
            // encoding on the wire — a malformed offer that no peer can
            // parse.  This is a plugin-contract violation (see
            // `IVideoCodec::kind()`), so it is reported loudly.
            const std::string msg =
                std::string{"engine: video codec '"} +
                config_.video_codec_name +
                "' does not identify a codec family: kind() returned "
                "kUnknown and the plugin id does not name one either "
                "(expected h264 / h265 / hevc / av1). The SDP encoding "
                "name cannot be derived.";
            core::log::Logger::instance().error(msg);
            if (on_error_) on_error_(core::kEngineInvalidParam, msg);
            impl_->session_state.last_open_rc = core::kEngineInvalidParam;
            // Leave the video plugins down: the engine cannot describe
            // this codec, so advertising a video m-line would be a lie.
            shutdown_video_plugins();
            return core::kEngineInvalidParam;
        }
    }

    // The effective payload type: the host's explicit choice, or the
    // family's conventional default (see `resolve_payload_type`).
    const std::uint8_t video_pt = detail::resolve_payload_type(
        codec_family, config_.video_sender_tuning.payload_type);

    impl_->video_state.video_codec_family = codec_family;
    impl_->video_state.video_payload_type  = video_pt;

    // ---- Video Receiver ----------------------------------------------------
    if (!video_receiver_) {
        plugins::VideoReceiverConfig cfg{};
        cfg.codec        = detail::codec_kind_of(codec_family);
        cfg.ssrc         = config_.video_receiver_tuning.ssrc;
        cfg.payload_type = video_pt;
        cfg.expect_fu_a  = config_.video_receiver_tuning.expect_fu_a;
        cfg.max_inflight_frames =
            config_.video_receiver_tuning.max_inflight_frames;
        cfg.max_jitter_buffer_ms =
            config_.video_receiver_tuning.max_jitter_buffer_ms;
        cfg.initial_jitter_buffer_ms =
            config_.video_receiver_tuning.initial_jitter_buffer_ms;
        cfg.emit_nacks  = config_.video_receiver_tuning.emit_nacks;
        cfg.adaptive_playout = config_.video_receiver_tuning.adaptive_playout;
        (void)pal::make_plugin<&pal::resolve_video_receiver>(
            config_.video_receiver_name,
            video_receiver_,
            std::move(cfg),
            {.err_open_failed = core::kVideoReceiverOpenFailed,
             .err_missing = core::kVideoReceiverPluginMissing,
             .open_fail_mode = pal::PluginFailureMode::Error,
             .missing_mode = pal::PluginFailureMode::Error,
             .on_error = on_error_});
    }

    // ---- Video Sender ------------------------------------------------------
    if (!video_sender_) {
        plugins::VideoSenderConfig cfg{};
        cfg.codec        = detail::codec_kind_of(codec_family);
        cfg.ssrc         = config_.video_sender_tuning.ssrc;
        cfg.payload_type = video_pt;
        cfg.mtu          = config_.video_sender_tuning.mtu;
        cfg.initial_seq  = config_.video_sender_tuning.initial_seq;
        cfg.packetization_mode =
            plugins::VideoPacketizationMode::kNonInterleaved;
        (void)pal::make_plugin<&pal::resolve_video_sender>(
            config_.video_sender_name,
            video_sender_,
            std::move(cfg),
            {.err_open_failed = core::kVideoSenderOpenFailed,
             .err_missing = core::kVideoSenderPluginMissing,
             .open_fail_mode = pal::PluginFailureMode::Error,
             .missing_mode = pal::PluginFailureMode::Error,
             .on_error = on_error_});
    }

    // ---- The decoded-frame delivery path (remote-desktop plan E1) --------
    //
    // The sink may implement the CPU path, the GPU path, or both.  The
    // engine decides **once**, here at `open()` time, which route a
    // decoded frame takes:
    //
    //   GPU path  — the decoder produces a native surface and the sink
    //               presents it (no host copy).  Requires *both* the
    //               decoder's `supports_zero_copy_decode()` and the
    //               sink's `supports_gpu_render()`.
    //   CPU path  — the decoder writes host pixels.  The universal
    //               fallback; requires the sink's
    //               `supports_cpu_render()`.
    //
    // The rule itself lives in `video_render_path.hpp` as a pure
    // function, so every combination is unit-tested without an engine.
    //
    // An *incompatible* pair — a sink that can take neither — is
    // reported here, at `open()` time, as `kVideoSinkIncompatible`.
    // Reporting it later (on the first frame) would mean a session that
    // looks healthy and displays nothing at all.
    //
    // This block sits *after* the codec was created: the decoder's
    // capability is one of the three inputs.
    {
        detail::VideoRenderPathInputs inputs;
        inputs.decoder_zero_copy =
            video_codec_ && video_codec_->supports_zero_copy_decode();
        inputs.sink_gpu_render = video_sink_ && video_sink_->supports_gpu_render();
        inputs.sink_cpu_render = video_sink_ && video_sink_->supports_cpu_render();

        const detail::VideoRenderPath path =
            detail::decide_video_render_path(inputs);
        impl_->video_state.video_render_path = path;

        if (path == detail::VideoRenderPath::kIncompatible) {
            const std::string msg =
                std::string{"engine: the video sink '"} +
                config_.video_sink_name +
                "' cannot accept the decoded frames this session produces: "
                "it accepts neither host-memory frames "
                "(supports_cpu_render() == false) nor GPU surfaces "
                "(supports_gpu_render() == false), and the codec "
                "'" + config_.video_codec_name +
                "' does not report zero-copy decode support.";
            core::log::Logger::instance().error(msg);
            if (on_error_) on_error_(core::kVideoSinkIncompatible, msg);
        } else if (path == detail::VideoRenderPath::kGpuSurface) {
            // The GPU path needs a surface to decode into.  The codec
            // owns that pool (`output_pool()`), and the contract says it
            // is pre-allocated in the steady state.  A codec that
            // claims zero-copy decode support but publishes no pool
            // cannot actually deliver a surface — the path is
            // downgraded to the CPU staging path rather than failing
            // on every frame.
            impl_->video_state.video_decode_pool = video_codec_->output_pool();
            if (impl_->video_state.video_decode_pool) {
                core::log::Logger::instance().info(
                    "engine: the decoded-frame delivery path is the GPU "
                    "surface path (no host copy)");
            } else {
                impl_->video_state.video_render_path =
                    detail::VideoRenderPath::kCpuStaging;
                core::log::Logger::instance().warn(
                    "engine: the codec reports zero-copy decode support but "
                    "publishes no output pool — the decoded-frame delivery "
                    "path falls back to the CPU staging path");
            }
        } else {
            core::log::Logger::instance().info(
                "engine: the decoded-frame delivery path is the CPU "
                "staging path");
        }
    }

    // ========================================================================
    // PHASE 2 — wire all pipeline callbacks (all plugins now exist)
    // ========================================================================

    // ---- Video send pipeline: source → codec → sender → RTP+SRTP → scheduler
    //
    // Tracking state: the codec's force_keyframe() causes the NEXT encoded
    // frame to be a keyframe. We track it in impl_->video_state.video_keyframe_pending_
    // (atomic bool) so it survives across send_video() calls.
    //
    // The video_source frame callback is NOT wired to the encode path
    // here (IVideoCodec / IVideoSender are NOT thread-safe; the source
    // cadence thread would race with any concurrent user thread).
    // Users drive video through send_video() — see engine.hpp.

    if (video_sender_) {
        wire_default_sender();
    }

    // ---- Video receive pipeline: receiver → codec → sink
    //
    // On each assembled frame from the receiver:
    //   1. Decode via video_codec_
    //   2. Render via video_sink_
    //   3. Fire on_video_frame_ (NAL delivery, for existing users)
    // -------------------------------------------------------------------------
    if (video_receiver_) {
        // ---- Receiver feedback bridge (v1.1 Q3) ---------------------------
        //
        // The receiver detects the loss and produces the RTCP bytes; the
        // engine owns the transport, so it protects them (SRTCP) and puts
        // them on the wire.  The receiver plugin is asked for the bytes
        // rather than handing the engine an RTCP encoder — the engine
        // must not know the RTCP wire format (L4: no concrete module
        // headers in the engine).
        video_receiver_->set_feedback_callback(
            [this](const plugins::VideoFeedbackReport& report) {
                queue_video_feedback(report);
            });

        video_receiver_->set_frame_callback(
            [this](const plugins::EncodedVideoFrame& enc,
                   plugins::TimestampUs /*now_us*/) {
                // v1.1 V1: one assembled receiver frame (pre-decode).
                ++impl_->video_state.video_frames_received;
                // Phase 4.2 / D2: the video-quality statistics sink.  The
                // frame has arrived but not been decoded yet, so this is
                // the "received" event: it carries the frame's size (for
                // the bitrate window), the keyframe flag (for the
                // inter-keyframe EMA) and the QP (for the quality sum).
                impl_->video_state.video_stats.on_frame_received(
                    static_cast<std::int64_t>(enc.info.capture_ts_us),
                    static_cast<std::uint32_t>(enc.payload.size()),
                    enc.is_keyframe,
                    static_cast<int>(enc.qp));
                // v1.1 T1: record the inbound ref_frame mapping too, so a
                // host holding a frame_seq from either direction resolves.
                record_video_ref_frame(
                    static_cast<std::uint64_t>(enc.info.frame_seq),
                    static_cast<std::int64_t>(enc.info.capture_ts_us));
                // Fire the NAL-level callback (existing behaviour).
                if (on_video_frame_) {
                    on_video_frame_(enc.payload.data(),
                                   enc.payload.size(),
                                   enc.is_keyframe);
                }

                // Decode + render if the codec and sink are available.
                if (!video_codec_ || !video_sink_) return;

                // ---- The delivery path (remote-desktop plan E1) --------------
                //
                // Two routes, decided at `open()` time (see
                // `video_render_path.hpp`):
                //
                //   GPU path — the decoder fills a native surface and the
                //             sink presents it.  No host copy.
                //   CPU path — the decoder writes host pixels.
                //
                // The GPU branch is attempted first; when it cannot run
                // (no pool, no surface, the decoder refuses) the frame
                // falls back to the CPU path, which is the universal
                // fallback every codec implements.
                if (impl_->video_state.video_render_path ==
                    detail::VideoRenderPath::kGpuSurface) {
                    // ---- The render clock (remote-desktop plan E5) ---------
                    //
                    // The clock decides *before* the frame is decoded: a
                    // frame that has to be dropped is dropped without
                    // paying for the decode, and a frame that has to be
                    // held is held before the decode so the hold does
                    // not add to the decode's latency.
                    std::int64_t hold_us = 0;
                    if (!apply_render_sync(
                            static_cast<std::int64_t>(enc.info.capture_ts_us),
                            &hold_us)) {
                        impl_->video_state.video_stats.on_frame_dropped(
                            detail::VideoDropReason::kDecoderError);
                        return;
                    }

                    const plugins::Status gpu_rc = render_decoded_gpu_frame(
                        enc, static_cast<std::int64_t>(enc.info.capture_ts_us));
                    if (gpu_rc == plugins::kOk) {
                        ++impl_->video_state.video_rx_gpu_frames;
                        impl_->video_state.video_stats.on_frame_decoded(
                            static_cast<std::int64_t>(enc.info.capture_ts_us));
                        impl_->video_state.video_stats.on_frame_rendered(
                            static_cast<std::int64_t>(enc.info.capture_ts_us));
                        // Plan E9: the frame reached the sink — record
                        // what the operator is now looking at.
                        note_displayed_frame(
                            static_cast<std::int64_t>(enc.info.capture_ts_us),
                            enc.info.frame_seq);
                        return;
                    }
                    // The GPU path could not deliver this frame.  Fall
                    // through to the CPU path, which is exactly what the
                    // fallback is for — but count the miss so the host
                    // can see that the GPU path is not working.
                    ++impl_->video_state.video_zero_copy_fallbacks;
                }

                // ---- Resolve the decoded geometry ------------------------------
                //
                // ---- The render clock (remote-desktop plan E5) ---------------
                //
                // The CPU path asks the render clock *before* the decode,
                // for the same reason the GPU path does: a frame that has
                // to be dropped should not pay for the decode, and a
                // frame that has to be held should be held before the
                // decode so the hold does not add to the decode's own
                // latency.
                {
                    std::int64_t hold_us = 0;
                    if (!apply_render_sync(
                            static_cast<std::int64_t>(enc.info.capture_ts_us),
                            &hold_us)) {
                        impl_->video_state.video_stats.on_frame_dropped(
                            detail::VideoDropReason::kDecoderError);
                        return;
                    }
                }

                //
                // The decode buffers must match the *actual* geometry of
                // the incoming frame, not a hard-coded 640×480.  Two
                // sources are available:
                //
                //   1. The codec's own report.  `IVideoCodec::decode()`
                //       publishes the decoded geometry through the
                //       `VideoFrame`'s buffer layout (the codec allocates
                //       a `VideoFrameBuffer` with the native layout when
                //       the caller does not supply buffers).  The engine
                //       therefore asks the codec to own the allocation
                //       and reads the geometry back from the frame it
                //       produced.
                //   2. The configured geometry.  The stub codecs do not
                //       parse the SPS, so their decoded geometry falls
                //       back to `VideoCodecConfig::width` / `height` —
                //       which the engine now forwards from
                //       `EngineConfig::video_width` / `video_height`.
                //
                // The engine used to hard-code 640×480 here, which
                // silently mis-sized the decode buffers for every other
                // resolution (and could write past the end of the
                // buffer).
                //
                // Strategy: decode into codec-owned buffers (the codec
                // knows the native geometry), then hand the *codec's*
                // planes to the sink.  This removes the engine's
                // geometry guess entirely.
                plugins::VideoFrame raw_out{};
                raw_out.info.capture_ts_us = enc.info.capture_ts_us;
                raw_out.info.frame_seq     = enc.info.frame_seq;
                raw_out.info.rtp_timestamp = enc.info.rtp_timestamp;

                // Decode with no caller-supplied buffers: the codec
                // allocates (and knows) the native geometry.
                //
                // Plan J1: the decode stage's own cost.
                const auto decode_start = std::chrono::steady_clock::now();
                const auto decode_rc = video_codec_->decode(enc, raw_out, nullptr);
                const auto decode_end = std::chrono::steady_clock::now();
                impl_->video_state.stage_timings.record(
                    Stage::kDecode,
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            decode_end - decode_start).count()));
                if (decode_rc != plugins::kOk) {
                    // The decoder refused the frame.  That is a
                    // decoder-side drop (Phase 4.2 / D2), and it is
                    // counted as such so a host can see a stream that
                    // arrives but never decodes.
                    impl_->video_state.video_stats.on_frame_dropped(
                        detail::VideoDropReason::kDecoderError);
                    return;
                }

                // Phase 4.2 / D2: the frame decoded successfully.
                impl_->video_state.video_stats.on_frame_decoded(
                    static_cast<std::int64_t>(enc.info.capture_ts_us));

                const std::shared_ptr<video_frame::VideoFrameBuffer> cpu_buf =
                    raw_out.cpu_buffer();
                if (!cpu_buf) return;

                const video_frame::VideoFrameLayout& layout = cpu_buf->layout();
                const std::uint32_t w = layout.width;
                const std::uint32_t h = layout.height;
                if (w == 0 || h == 0) return;

                const std::uint32_t stride_y =
                    static_cast<std::uint32_t>(layout.strides[0]);
                const std::uint32_t stride_uv =
                    static_cast<std::uint32_t>(layout.strides[1]);

                // The engine's ping-pong buffers are no longer used for
                // decoding: the codec owns the pixel memory (that is the
                // only party that knows the true geometry).  The engine
                // keeps its own buffers for the *HW* path, where the
                // zero-copy decoder hands back a GPU surface instead of
                // CPU planes.
                std::uint8_t* planes[3] = {
                    cpu_buf->plane(0),
                    cpu_buf->plane(1),
                    cpu_buf->plane(2),
                };

                // Render to sink.
                plugins::VideoSourceFrame sink_frame{};                sink_frame.width   = w;
                sink_frame.height  = h;
                // The format follows the codec's actual output layout, not
                // a hard-coded I420: an NV12 decoder produces an NV12
                // frame, and telling the sink "I420" would make it
                // mis-interpret the chroma planes.
                sink_frame.format  = layout.format;
                sink_frame.stride_y = static_cast<std::int32_t>(stride_y);
                sink_frame.stride_u = static_cast<std::int32_t>(stride_uv);
                sink_frame.stride_v = static_cast<std::int32_t>(layout.strides[2]);
                sink_frame.plane_y  = planes[0];
                sink_frame.plane_u  = planes[1];
                sink_frame.plane_v  = planes[2];
                sink_frame.buffer_size = layout.buffer_size;
                sink_frame.capture_ts_us = enc.info.capture_ts_us;
                sink_frame.frame_seq     = enc.info.frame_seq;
                sink_frame.rtp_timestamp = enc.info.rtp_timestamp;

                // Plan J1: the render stage's timer starts here.
                const auto render_start = std::chrono::steady_clock::now();

                const plugins::Status render_rc = video_sink_->render(sink_frame);
                // Plan J1: the render stage — the sink's own cost, which
                // on the GPU path is the present and on the CPU path is
                // the pixel copy into the window.
                impl_->video_state.stage_timings.record(
                    Stage::kRender,
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() -
                            render_start).count()));
                // Phase 4.2 / D2: the frame reached the sink.  This is
                // the "rendered" event — the freeze detector in the
                // stats sink measures the gap between two of these.
                if (render_rc == plugins::kOk) {
                    impl_->video_state.video_stats.on_frame_rendered(
                        static_cast<std::int64_t>(enc.info.capture_ts_us));
                    // Plan E9: the frame reached the sink — record what
                    // the operator is now looking at, so a host can
                    // answer "the user clicked on what they were
                    // looking at".
                    note_displayed_frame(
                        static_cast<std::int64_t>(enc.info.capture_ts_us),
                        enc.info.frame_seq);
                } else {
                    impl_->video_state.video_stats.on_frame_dropped(
                        detail::VideoDropReason::kDecoderError);
                }
                ++impl_->video_state.video_rx_cpu_frames;
            });
    }

    return plugins::kOk;
}


plugins::Status NimRTCEngine::render_decoded_gpu_frame(
        const plugins::EncodedVideoFrame& enc,
        std::int64_t capture_ts_us) noexcept {
    if (!video_codec_ || !video_sink_) return plugins::kErrNotReady;

    // The surface comes from the codec's own output pool — the codec
    // knows the native geometry and the backend-specific allocation
    // (D3D11 texture, VA surface, ...).  Without a pool there is
    // nowhere to decode into.
    const std::shared_ptr<plugins::GpuBufferPool>& pool =
        impl_->video_state.video_decode_pool;
    if (!pool) return plugins::kErrUnsupported;

    const plugins::VideoCodecConfig codec_cfg = video_codec_->config();
    const std::uint32_t w = codec_cfg.width;
    const std::uint32_t h = codec_cfg.height;
    if (w == 0 || h == 0) return plugins::kErrUnsupported;

    plugins::GpuBuffer* surface = pool->acquire(w, h, codec_cfg.pixel_format);
    if (!surface) return plugins::kErrHardwareError;

    // The decoder acquires one reference on the surface it fills (see
    // `IVideoCodec::decode_zero_copy()`), and the engine needs one of
    // its own to hold the surface across the `render()` call.  So the
    // count goes: 1 (the pool's) → 2 (the decoder's) → 3 (the engine's).
    // The engine releases its own reference when the frame is done.
    surface->acquire_ref();

    // Plan J1: the GPU decode's own cost.
    const auto gpu_decode_start = std::chrono::steady_clock::now();
    const plugins::Status decode_rc =
        video_codec_->decode_zero_copy(enc, *surface);
    impl_->video_state.stage_timings.record(
        Stage::kDecode,
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - gpu_decode_start).count()));
    if (decode_rc != plugins::kOk) {
        surface->release_ref();   // the engine's reference
        surface->release_ref();   // the pool's reference
        return decode_rc;
    }

    // The timeline metadata travels with the frame so a sink that paces
    // or timestamps its output has what it needs.
    plugins::VideoFrameInfo info{};
    info.capture_ts_us = capture_ts_us;
    info.frame_seq     = enc.info.frame_seq;
    info.rtp_timestamp = enc.info.rtp_timestamp;

    // Plan J1: the GPU path's decode + render costs.
    const auto gpu_start = std::chrono::steady_clock::now();
    const plugins::Status render_rc = video_sink_->render(*surface, info);
    impl_->video_state.stage_timings.record(
        Stage::kRender,
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - gpu_start).count()));

    // The engine's reference goes away; the decoder's stays until the
    // next decode (that is the documented contract).
    surface->release_ref();

    return render_rc;
}


void NimRTCEngine::shutdown_video_plugins() noexcept {
    auto close_all = [](auto& p) {
        if (p) p->close();
        p.reset();
    };
    stop_video();   // stop video source if running
    close_all(video_source_);
    close_all(video_sink_);
    close_all(video_receiver_);
    close_all(video_sender_);
    close_all(video_codec_);
    // Remote-desktop plan D1: the rate controller closes with the rest
    // of the video plugins.
    close_all(video_bitrate_controller_);
    // The video-quality statistics sink (Phase 4.2 / D2) is *not* a
    // plugin — it has no open()/close().  Resetting it here clears the
    // per-session counters so a re-open() does not report the previous
    // session's freezes and drops.
    impl_->video_state.video_stats.reset();
    // The capture device (remote-desktop plan A1) is a *device*: it may
    // own an OS handle (a DXGI duplication, a PipeWire stream), so it
    // has to be closed explicitly rather than merely dropped.
    close_all(video_capture_);
    // Remote-desktop plan H1 / H2: the *extra* video streams own their
    // own plugin instances, so they are closed with the rest of the
    // video plugins.  (The default stream's instances are the members
    // closed above.)
    shutdown_extra_video_streams();
}


uint32_t NimRTCEngine::send_audio(const float* pcm_samples, std::size_t num_samples) noexcept {
    if (!is_open()) return core::kEngineNotReady;

    // The error reporter is defined up front because *every* rejection
    // on this path has to be visible (spec A-8: "an invalid frame size,
    // an encoder error, or an empty buffer never disappears.  Each
    // occurrence increments a statistics counter and, at a throttled
    // rate, fires `on_error`").  A host whose PCM never reaches the
    // wire has to be able to see *why*.
    auto report_error = [&](std::uint32_t code, const char* msg) {
        ++impl_->audio_state.audio_encode_errors;
        const auto now = std::chrono::steady_clock::now();
        if (impl_->audio_state.audio_last_error_report ==
                std::chrono::steady_clock::time_point{} ||
            now - impl_->audio_state.audio_last_error_report >= std::chrono::seconds(1)) {
            impl_->audio_state.audio_last_error_report = now;
            if (on_error_) on_error_(code, msg);
        }
    };

    if (!pcm_samples || num_samples == 0) {
        // An empty buffer is a host-side mistake, and it used to be
        // invisible: the call returned `kEngineInvalidParam` and the
        // host heard silence with no explanation.  The counter grows
        // and the callback fires, so the mistake is visible (A-8).
        report_error(core::kAudioEncodeFailed,
                     "send_audio: the PCM buffer is empty");
        return core::kEngineInvalidParam;
    }

    // Gate on DTLS being Connected — otherwise RTP packets would reach the
    // peer over plaintext UDP (no SRTP key), or get sent into a half-open
    // session that the peer has already torn down (e.g. after a fatal
    // DTLS alert).  Without this check, demo-p2p would continue logging
    // "audio frames sent=N rc=0x0" after the handshake has transitioned
    // to Failed.
    auto d_check = impl_->dtls.lock();
    if (!d_check || !d_check->is_connected()) {
        return core::kEngineNotReady;
    }

    // Copy the capture PCM into an engine-owned buffer.  The 3A stage
    // processes audio in place, but the caller's buffer is declared const —
    // writing through a const_cast would be undefined behaviour and would
    // silently mutate the caller's data.  The copy is what 3A mutates below
    // and what the encoder consumes.
    const std::size_t channels =
        static_cast<std::size_t>(std::max<std::uint8_t>(1, config_.pcm_channels));
    const std::size_t total_floats = num_samples * channels;
    if (impl_->scratch.audio_pcm_buf.size() < total_floats) {
        impl_->scratch.audio_pcm_buf.resize(total_floats);
    }
    std::memcpy(impl_->scratch.audio_pcm_buf.data(), pcm_samples,
                total_floats * sizeof(float));
    float* pcm = impl_->scratch.audio_pcm_buf.data();

    // 3A — prefer plugin adapter; fall back to concrete NullAudio3A.
    if (audio3a_plugin_) {
        // Plugin API takes raw (ptr, samples, channels) — same shape we have.
        audio3a_plugin_->process_capture(
            pcm,
            num_samples,
            config_.pcm_channels);
    } else if (impl_->audio3a_concrete) {
        audio3a::Frame frame;
        frame.samples        = pcm;
        frame.num_samples    = num_samples;
        frame.num_channels   = config_.pcm_channels;
        frame.sample_rate_hz = config_.pcm_sample_rate_hz;
        impl_->audio3a_concrete->process_capture(frame);
    }

    // One RTP packet carries one Opus frame of `audio_ptime_ms`
    // milliseconds (RFC 7587 code 0 framing).  The frame length follows
    // from the configuration, so the accumulator never has to guess.
    //
    // The check comes *before* the accumulator is touched: a zero frame
    // size means the engine can never drain the accumulator, so taking
    // the PCM in first would grow it without bound.
    const std::size_t frame_samples =
        (static_cast<std::size_t>(config_.pcm_sample_rate_hz) *
         static_cast<std::uint16_t>(config_.audio_ptime_ms)) / 1000;
    if (frame_samples == 0) {
        report_error(core::kAudioEncodeFailed,
                     "send_audio: the configured audio frame size is zero");
        return core::kEngineInvalidParam;
    }

    // ---- Accumulate the processed PCM (B-6) ----------------------------
    //
    // send_audio() accepts any block size.  The processed PCM is
    // appended to a per-engine accumulator and only whole frames are
    // encoded, so the wire format does not depend on how the host
    // chunks its capture.  The residual stays in the accumulator across
    // calls; a partial frame is never encoded.
    impl_->audio_state.audio_accum.insert(impl_->audio_state.audio_accum.end(), pcm, pcm + total_floats);
    impl_->audio_state.audio_samples_in += num_samples;

    // ---- Emit one RTP packet per frame --------------------------------
    auto emit_frame = [&](const float* frame_pcm) -> std::uint32_t {
        // Codec — the plugin seam only (v1.2.0, audio pipeline WP-7).
        //
        // The concrete `opus::Encoder` fallback is gone: the engine
        // orchestrates plugins and never names a concrete module
        // (constraint L4).  When the configured codec plugin is missing
        // the frame is reported as lost (throttled) instead of being
        // encoded with a *different* codec — a silent codec swap would
        // put a bit-stream on the wire that the peer never negotiated.
        //
        // Reuse Impl::audio_codec_pkt (P0#4) — capacity grows monotonically.
        const std::size_t needed = frame_samples * sizeof(float) + 64;
        if (impl_->scratch.audio_codec_pkt.size() < needed) {
            impl_->scratch.audio_codec_pkt.resize(needed);
        }
        std::size_t codec_len = 0;
        if (codec_plugin_) {
            codec_len = codec_plugin_->encode(frame_pcm, frame_samples,
                                             impl_->scratch.audio_codec_pkt.data(),
                                             impl_->scratch.audio_codec_pkt.size());
            if (codec_len == 0) {
                // A-8: the frame is lost, but never silently.  The counter
                // grows, the host is told (throttled), and the media clock
                // keeps running — the peer must see a *loss* (a gap in the
                // sequence number), not a hole in the timeline.
                report_error(core::kAudioEncodeFailed,
                             "send_audio: the audio encoder rejected a frame");
            }
        } else {
            // WP-7: there is no concrete fallback encoder any more — the
            // engine reaches the encoder through the codec seam only
            // (constraint L4).  A missing codec plugin means the engine
            // cannot encode at all; that is reported (throttled) instead
            // of silently producing nothing on the wire.
            report_error(core::kAudioEncodeFailed,
                         "send_audio: no audio codec plugin is available");
        }
        if (codec_len == 0) {
            // The frame is lost, but never silently: the sequence number
            // leaves a gap and the media clock keeps running, so the peer
            // sees a *loss* rather than a hole in the timeline (A-8).
            ++impl_->audio_state.audio_seq;                       // gap in the sequence
            impl_->audio_state.audio_ts += static_cast<std::uint32_t>(frame_samples);
            return 0;
        }

        // ---- RTP header (A-6: per-engine state) ----------------------
        //
        // The payload type is the *negotiated* one (B-4): RFC 3264 §6.1
        // binds both ends of the session to the payload type the answer
        // confirmed, so a peer that mapped Opus onto a different number
        // must see that number on the wire.  Before a negotiation has
        // happened the configured one is used.
        const std::uint8_t outbound_pt =
            impl_->audio_state.negotiated_audio_pt != 0
                ? impl_->audio_state.negotiated_audio_pt
                : config_.audio_codec.payload_type;

        rtp::PacketBuilder pb;
        pb.set_ssrc(impl_->audio_state.audio_ssrc)
          .set_seq(impl_->audio_state.audio_seq++)
          .set_timestamp(impl_->audio_state.audio_ts)
          .set_payload_type(outbound_pt)
          .set_marker(false);

    // ---- Cross-end timeline: stamp abs-send-time (ADR-017) ---------------
    // Only when the seam is on *and* the id was negotiated. The encoded
    // value is the sender's wall-clock send instant (24 bits at 65536 Hz,
    // 256 s wrap) on the Unix timeline; the receiver resolves the wrap
    // against our Sender Report, which is why the two are gated together.
    //
    // The same reading also records the packet's send instant for the
    // Sender Report (see `Impl::audio_last_send_unix_us`): the report
    // must pair its NTP instant with the RTP timestamp of a packet that
    // was *actually sent at that instant*, or every reconstruction
    // downstream inherits the gap between the packet and the report.
    //
    // The monotonic reading is taken once and serves both purposes — the
    // abs-send-time encoder turns it into the wall-clock instant on the
    // Unix timeline, and the SR reuses the same value.
    if (config_.timeline_enabled) {
        // The encoder sums `unix_us(origin) + monotonic_us`; the origin is
        // the wall clock at monotonic zero and the term is the monotonic
        // reading, so the sum is the send instant on the Unix timeline.
        // Both are already available — no second clock read per packet.
        const std::int64_t steady_us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                core::SteadyClock::now().time_since_epoch()).count();
        // `boot_epoch_offset_us` is the wall clock at monotonic zero, so
        // the sum is this packet's send instant on the Unix timeline —
        // the same basis the abs-send-time encoder uses.
        impl_->audio_state.audio_last_send_unix_us = impl_->timeline.boot_epoch_offset_us + steady_us;
        if (impl_->timeline.local_abs_send_time_id != 0) {
            const std::uint32_t ast24 = rtp::encode_abs_send_time_24(
                std::chrono::microseconds{steady_us},
                impl_->timeline.monotonic_origin_ntp64);
            pb.set_abs_send_time(rtp::AbsSendTime{ast24},
                                 impl_->timeline.local_abs_send_time_id);
        }
    }

        pb.set_payload(core::ByteSpan(impl_->scratch.audio_codec_pkt.data(), codec_len));

        // ---- Serialise into the reusable scratch (Phase 3 / C4) --------------
        //
        // `pb.build()` would allocate a fresh buffer for every audio
        // packet.  `build_into()` writes into the engine's per-session
        // scratch instead, so the steady-state send path performs no heap
        // allocation for the RTP header + payload copy.
        const std::size_t pkt_size = pb.compute_size();
        if (impl_->scratch.audio_rtp_scratch.size() < pkt_size) {
            impl_->scratch.audio_rtp_scratch.resize(pkt_size);
        }
        const std::size_t pkt_written =
            pb.build_into(impl_->scratch.audio_rtp_scratch.data(),
                          impl_->scratch.audio_rtp_scratch.size());
        if (pkt_written == 0) {
            report_error(core::kAudioEncodeFailed,
                         "send_audio: the RTP packet could not be built");
            impl_->audio_state.audio_ts += static_cast<std::uint32_t>(frame_samples);
            return 0;
        }
        core::ByteSpan pkt_span(impl_->scratch.audio_rtp_scratch.data(), pkt_written);

        // Count for the Sender Report body (RFC 3550 §6.4.1 sender info). The
        // RTP timestamp is sampled *before* SRTP so the SR and the media packet
        // it anchors describe the same media position; the octet count is the
        // payload length, which is what the peer subtracts to check.
        const std::uint32_t cur_ts = impl_->audio_state.audio_ts;
        ++impl_->audio_state.audio_packet_count;
        impl_->audio_state.audio_octet_count += static_cast<std::uint32_t>(codec_len);
        impl_->audio_state.audio_rtp_timestamp = cur_ts;
        ++impl_->audio_state.audio_frames_sent;
        impl_->audio_state.audio_bytes_sent += pkt_span.size();

        // The media clock advances by the frame duration whether or not the
        // packet reaches the wire (A-7): a dropped frame leaves a gap in
        // the sequence number, never a gap in the timeline.
        impl_->audio_state.audio_ts += static_cast<std::uint32_t>(frame_samples);

        // SRTP protect (or pass-through when not yet keyed).
        //
        // The outbound buffer is taken from the scheduler's packet pool
        // (P1 hardening, 2026-11-09) and handed back to it with
        // `enqueue_owned()`.  The previous code moved a MEMBER vector into
        // the scheduler, which left the member in a moved-from state —
        // so every single packet allocated a fresh buffer and the
        // previous one was freed on drain.  With the pool the steady-state
        // send path performs no heap allocation.
        //
        // Phase 3: without a scheduler the engine writes straight to ICE,
        // so the packet is built in the engine's own scratch buffer
        // (`send()` writes it out synchronously — the bytes do not escape
        // the call).  Before this the no-scheduler path allocated a fresh
        // vector per packet.
        std::vector<std::uint8_t>& send_buf = scheduler_
            ? impl_->scratch.audio_pkt_pooled
            : impl_->scratch.audio_pkt_scratch;
        if (scheduler_) {
            send_buf = scheduler_->acquire_packet_buffer(
                pkt_span.size() + kSrtpAuthTagBytes);
        }
        core::ByteSpan out_payload(pkt_span.data(), pkt_span.size());
        if (impl_->srtp_installed && impl_->srtp) {
            auto* sess = impl_->srtp->get_session(impl_->audio_state.audio_ssrc,
                                                   /*outgoing=*/true);
            if (!sess) {
                // SRTP installed but no session for this SSRC — drop.
                return 0;
            }
            auto enc = sess->protect_rtp(out_payload, impl_->audio_state.audio_ssrc, cur_ts);
            if (!enc) return 0;
            send_buf.assign(enc.value().data(),
                            enc.value().data() + enc.value().size());
        } else {
            send_buf.assign(out_payload.data(),
                            out_payload.data() + out_payload.size());
        }

        // ---- Scheduler: enqueue for priority-drain (move, P0#3) --------------
        // The scheduler takes ownership of the buffer via enqueue_owned and
        // returns it to its pool once the packet has been dispatched.
        if (scheduler_) {
            scheduler_->enqueue_owned(plugins::Priority::kAudio,
                                      std::move(send_buf),
                                      plugins::Addr{});
            return 0;
        }

        // Fallback: no scheduler — send directly through ICE (transport profile).
        plugins::BufferView bv{send_buf.data(), send_buf.size()};
        return ice_t_->send(bv, {});
    };

    // ---- Drain the accumulator -----------------------------------------
    //
    // Every whole frame the accumulator holds becomes exactly one RTP
    // packet.  A partial frame stays behind for the next call.
    std::uint32_t rc = 0;
    std::size_t emitted_frames = 0;
    std::size_t available = impl_->audio_state.audio_accum.size() / channels;
    while (available >= frame_samples) {
        const float* frame_pcm = impl_->audio_state.audio_accum.data() +
                                 emitted_frames * frame_samples * channels;
        const std::uint32_t frame_rc = emit_frame(frame_pcm);
        emitted_frames += frame_samples;
        available -= frame_samples;
        if (frame_rc != 0) {
            // A transport failure stops the burst; the frames already
            // emitted stay emitted.
            rc = frame_rc;
            break;
        }
    }

    if (emitted_frames > 0) {
        impl_->audio_state.audio_accum.erase(
            impl_->audio_state.audio_accum.begin(),
            impl_->audio_state.audio_accum.begin() +
                static_cast<std::ptrdiff_t>(emitted_frames * channels));
    }
    impl_->audio_state.audio_accum_frames = impl_->audio_state.audio_accum.size() / channels;
    return rc;
}


plugins::Status NimRTCEngine::start_video() noexcept {
    if (!is_open()) return plugins::kErrNotReady;
    if (video_source_) {
        video_source_->start();
        return plugins::kOk;
    }
    return plugins::kErrNotReady;
}


void NimRTCEngine::stop_video() noexcept {
    if (video_source_) {
        video_source_->stop();
    }
}


plugins::Status NimRTCEngine::send_video(
    const plugins::VideoSourceFrame& frame) noexcept {
    if (!is_open()) return plugins::kErrNotReady;
    if (!video_codec_ || !video_sender_) return plugins::kErrNotReady;
    return encode_and_send_video_frame(frame);
}


// ---------------------------------------------------------------------------
// The encoder's parameter set (remote-desktop plan B12 / B13)
// ---------------------------------------------------------------------------
//
// The engine computes the parameters in one place (see
// `engine_video_params.hpp`) and pushes them to the codec here.  This
// function is the *only* place the engine talks to the codec about
// "what should you be producing" — the bitrate controller, the ladder
// and the capture pump all reach the codec through it.
//
// The push is skipped in the steady state: the parameters are recomputed
// and compared with the ones in force, and only a real difference
// triggers a `set_params()` call.  That keeps the per-frame cost at two
// integer compares.

void NimRTCEngine::apply_video_encoder_params(
    const plugins::VideoSourceFrame& frame) noexcept {
    if (!video_codec_) return;

    auto& vstate = impl_->video_state;

    // ---- The inputs -------------------------------------------------------
    //
    // The *configured* geometry is the starting point.  But the frame in
    // hand is the authority on what is actually being sent: the ladder
    // may have stepped the resolution down, or the capture device may
    // have renegotiated.  When the frame disagrees with the
    // configuration, the frame wins — it is what the peer will receive.
    detail::VideoEncoderParamInputs in;
    in.width  = config_.video_width;
    in.height = config_.video_height;
    in.fps    = config_.video_fps;
    // The bitrate comes from the codec's own configuration: the engine
    // does not run a video rate controller yet (the ladder / BWE work is
    // plan item D1–D4, in M3), so the codec's current value *is* the
    // value in force.  Reading it back keeps this function from
    // clobbering a bitrate the host set on the codec directly.
    in.bitrate_bps = video_codec_->config().bitrate_bps;
    in.keyframe_interval = 60;
    in.pixel_format = video_codec_->config().pixel_format;
    in.content_hint = config_.video_content_hint;
    // The colour description comes from the *frame*: the picture's own
    // colour description is what the encoder has to code (plan C5).
    in.color = frame.color;

    // The ROI (plan C4): the host's rectangle is in *capture* pixels.
    // The capture resolution is the frame's own size — the frame knows
    // what it is — and the scaling into the output resolution happens in
    // `compute_video_encoder_params()`, so the ROI survives a ladder
    // resolution change.
    in.roi = config_.video_roi;
    in.capture_width = frame.width;
    in.capture_height = frame.height;

    if (frame.width != 0 && frame.height != 0) {
        in.width  = frame.width;
        in.height = frame.height;
    }

    // ---- The rate controller's level (remote-desktop plan D4) -----------
    //
    // The controller owns the ladder; the engine turns its level into
    // the encoder's parameter set.  The controller's *target bitrate*
    // is what reaches the encoder — the bitrate the codec is asked to
    // produce, not only the pacer rate (plan D4's acceptance).
    //
    // The geometry comes from the controller too, but only when the
    // controller actually moved it: the frame in hand is still the
    // authority on what is being sent (a capture device may renegotiate
    // at any time, and the frame knows).
    if (video_bitrate_controller_) {
        const plugins::VideoLadderLevel level =
            video_bitrate_controller_->current_level();
        if (level.bitrate_bps > 0) {
            in.bitrate_bps = level.bitrate_bps;
        }
        if (level.fps > 0) {
            in.fps = level.fps;
        }
        // The controller only overrides the geometry when the *ladder*
        // says so.  A controller that has not moved keeps the frame's
        // geometry.
        if (vstate.video_encoder_params_set &&
            (level.width != vstate.video_encoder_params.width ||
             level.height != vstate.video_encoder_params.height)) {
            in.width  = level.width;
            in.height = level.height;
        }
    }

    const plugins::VideoEncoderParams wanted =
        detail::compute_video_encoder_params(in);

    // ---- Has anything changed? --------------------------------------------
    //
    // Before the first push there is nothing to compare against, so the
    // parameters are pushed unconditionally.  The codec was already
    // configured with the same geometry at `open()` time, so this is a
    // no-op for the codec — but it establishes the baseline the
    // comparison uses.
    if (vstate.video_encoder_params_set &&
        vstate.video_encoder_params.width  == wanted.width  &&
        vstate.video_encoder_params.height == wanted.height &&
        vstate.video_encoder_params.fps == wanted.fps &&
        vstate.video_encoder_params.bitrate_bps == wanted.bitrate_bps &&
        vstate.video_encoder_params.keyframe_interval == wanted.keyframe_interval &&
        vstate.video_encoder_params.pixel_format == wanted.pixel_format &&
        vstate.video_encoder_params.content_hint == wanted.content_hint &&
        vstate.video_encoder_params.qp == wanted.qp &&
        vstate.video_encoder_params.color.range == wanted.color.range &&
        vstate.video_encoder_params.color.matrix == wanted.color.matrix) {
        return;
    }

    // ---- A geometry change is a parameter-set change (B13) ----------------
    //
    // The peer cannot decode a frame whose geometry its parameter sets
    // do not describe.  The encoder must therefore restart and emit the
    // new parameter set with the next frame — no PLI round-trip.
    const bool restart =
        vstate.video_encoder_params_set &&
        detail::video_params_need_restart(vstate.video_encoder_params, wanted);

    const plugins::Status rc = video_codec_->set_params(wanted);
    if (rc != plugins::kOk) {
        core::log::Logger::instance().warn(
            "engine: the video codec rejected the parameter set "
            "(the encoder keeps its previous parameters)");
        return;
    }

    vstate.video_encoder_params     = wanted;
    vstate.video_encoder_params_set = true;

    if (restart) {
        ++vstate.video_encoder_restarts;
        core::log::Logger::instance().info(
            "engine: the video geometry changed to " +
            std::to_string(wanted.width) + "x" +
            std::to_string(wanted.height) +
            " — the encoder restarted and the new parameter set travels "
            "with the next frame");
    }
}


    plugins::Status NimRTCEngine::encode_and_send_video_frame(
    const plugins::VideoSourceFrame& frame) noexcept {
    if (!is_open()) return plugins::kErrNotReady;
    if (!video_codec_ || !video_sender_) return plugins::kErrNotReady;

    // ---- The parameter set (remote-desktop plan B12 / B13) ---------------
    //
    // The engine computes the encoder's parameters in ONE place and
    // pushes them here, before the frame is encoded.  Two things can
    // make the parameters change:
    //
    //   1. the host changed the configuration (rare — the ladder and
    //      the bitrate controller both go through this path);
    //   2. the *geometry* of the frame changed (the ladder stepped
    //      down, or the capture device renegotiated).
    //
    // Case 2 is the one that matters for correctness (B13): a geometry
    // change is a parameter-set change, so the encoder must restart and
    // emit the new SPS / PPS with this frame.  Without it the peer's
    // decoder is looking at a frame whose size its parameter sets do
    // not describe, and the picture breaks until the next PLI.
    //
    // The check is cheap (two integer compares) and happens on the tick
    // thread, so it costs nothing in the steady state.
    apply_video_encoder_params(frame);

    // Translate plugins::VideoSourceFrame → plugins::VideoFrame for the codec.
    //
    // The source frame is a *view* into the producer's own storage, valid
    // only for this call; the codec seam wants a refcounted buffer that
    // stays alive across `encode()`.  So the engine stages its own copy
    // (see `stage_source_frame` above).  A software codec may ignore the
    // pixels, but a hardware encoder (NVENC) *must* have them — with a
    // null buffer it rejects the frame with kErrInvalidParam.
    plugins::VideoFrame raw{};
    raw.info.capture_ts_us = frame.capture_ts_us;
    raw.info.frame_seq     = frame.frame_seq;
    raw.info.rtp_timestamp = frame.rtp_timestamp;

    // ---- Phase 3 / C3: the GPU zero-copy path -----------------------------
    //
    // A hardware capture source hands the engine a *native* GPU handle
    // rather than CPU pixels (`VideoSourceFrame::gpu_handle`).  When the
    // host opted in (`EngineConfig::video_zero_copy`) and the codec can
    // consume a handle directly, the engine passes the handle straight
    // through — no staging copy, no de-interleave, no 3 MB memcpy per
    // frame.
    //
    // The zero-copy branch is only taken when ALL of the following hold:
    //
    //   1. the host enabled `EngineConfig::video_zero_copy`;
    //   2. the source frame carries a live GPU handle;
    //   3. the codec reports `supports_zero_copy_encode()`.
    //
    // Otherwise the CPU staging path runs exactly as before, so the flag
    // is always safe to turn on.  The counters make the decision
    // observable from the outside.
    const bool zero_copy_requested = config_.video_zero_copy;
    const bool zero_copy_capable    = video_codec_->supports_zero_copy_encode();
    const bool frame_has_gpu       = frame.has_gpu_handle();

    plugins::EncodedVideoFrame encoded{};
    bool encoded_ok = false;

    if (zero_copy_requested && zero_copy_capable && frame_has_gpu) {
        // Wrap the borrowed handle in a `GpuBuffer` so the codec seam can
        // take it.  The wrapper does NOT own the surface — the source
        // keeps it alive for the duration of this call, which is exactly
        // the lifetime contract `encode_zero_copy()` documents ("the
        // caller keeps the GpuBuffer alive until the encoder returns").
        video_frame::GpuHandle handle{};
        handle.backend           = frame.gpu_backend;
        handle.handle            = frame.gpu_handle;
        handle.native_pitch_bytes = frame.gpu_pitch_bytes;
        handle.native_fd         = -1;

        video_frame::GpuBuffer gpu(handle,
                                   frame.width,
                                   frame.height,
                                   frame.format);

        const plugins::Status zc_rc =
            video_codec_->encode_zero_copy(gpu, encoded);
        if (zc_rc == plugins::kOk) {
            encoded_ok = true;
            ++impl_->video_state.video_zero_copy_frames;
        } else {
            // The codec refused the handle (wrong backend, surface busy,
            // driver error, ...).  Fall through to the CPU path *only* if
            // the source also gave us CPU pixels — otherwise there is
            // nothing to stage and the frame is simply lost.
            ++impl_->video_state.video_zero_copy_fallbacks;
            core::log::Logger::instance().warn(
                "engine: the zero-copy encode failed — falling back "
                "to the CPU staging path");
        }
    } else if (zero_copy_requested && frame_has_gpu && !zero_copy_capable) {
        // The host asked for zero-copy but the codec cannot consume a
        // handle.  Count the missed opportunity so the host can see that
        // the flag alone does not buy anything with this codec.
        ++impl_->video_state.video_zero_copy_fallbacks;
    }

    if (!encoded_ok) {
        // Codec-facing pixel format comes from the codec's own config
        // (I420 for every backend today).
        const auto codec_fmt = video_codec_->config().pixel_format;
        // Plan J1: the conversion stage — the colour transform and the
        // copy the engine does to get the source's pixels into a
        // codec-facing buffer.  On the zero-copy path this stage does
        // not exist, which is exactly what the budget wants to see: a
        // zero sample count.
        const auto convert_start = std::chrono::steady_clock::now();
        ConversionKind conv_kind = ConversionKind::kNone;
        raw.buffer = stage_source_frame(frame, codec_fmt,
                                        impl_->scratch.video_frame_pool,
                                        &conv_kind);
        const auto convert_end = std::chrono::steady_clock::now();
        impl_->video_state.stage_timings.record(
            Stage::kConvert,
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    convert_end - convert_start).count()));
        // Plan C3: report the format work that was done, so a host can
        // see whether the conversion is costing anything — and see it
        // stay at zero on the zero-copy path.
        if (conv_kind == ConversionKind::kConvert) {
            ++impl_->video_state.video_format_conversions;
        } else if (conv_kind == ConversionKind::kCopy) {
            ++impl_->video_state.video_format_copies;
        }
        impl_->video_state.video_format_canonical = codec_fmt;
        impl_->video_state.video_format_source    = frame.format;
        if (!raw.buffer) {
            // The frame cannot be staged (no planes, zero dimensions, or a
            // packed source format).  Software codecs can still encode a
            // synthetic frame without pixels, so we only *warn* here and let
            // the codec decide — a hardware codec will report the failure
            // through its own error path.
            core::log::Logger::instance().warn(
                "engine: the source frame could not be staged into a "
                "codec-facing pixel buffer (the encoder receives no pixels)");
        }

        // Encode — reuse Impl::video_enc_buf (P0#4).  Reserve a typical IDR
        // size on first use; subsequent calls within the same capacity reuse
        // the storage and only pay the per-byte conversion cost inside the
        // codec.  H.264 frames can spike above 64 KiB (high-bitrate 1080p60)
        // — when that happens we resize and pay one reallocation.
        constexpr std::size_t kInitialVideoEncCap = 64 * 1024;
        if (impl_->scratch.video_enc_buf.capacity() < kInitialVideoEncCap) {
            impl_->scratch.video_enc_buf.reserve(kInitialVideoEncCap);
        }
        // Plan J1: the encode stage's own cost.  The codec is the stage
        // the plan's budget is really about ("capture → encode →
        // packetize"), and the codec already reports its own latency in
        // `VideoCodecStats::encode_latency_us_total` — but that number
        // is the codec's *self-report*, measured from inside the codec.
        // The engine's measurement brackets the whole call, so a codec
        // that lies about its own latency cannot hide from the budget.
        std::uint64_t encode_ns = 0;
        {
            const auto enc_start = std::chrono::steady_clock::now();
            const auto enc_rc = video_codec_->encode(raw,
                                                     impl_->scratch.video_enc_buf.data(),
                                                     impl_->scratch.video_enc_buf.capacity(),
                                                     encoded);
            const auto enc_end = std::chrono::steady_clock::now();
            encode_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    enc_end - enc_start).count());
            if (enc_rc != plugins::kOk) {
                // Codec may need a larger buffer than reserved (e.g. very high
                // bitrate IDR).  Resize to the codec's expected capacity and retry
                // exactly once to avoid an unbounded loop on a misbehaving codec.
                const std::size_t needed = encoded.payload.size();
                if (needed == 0 || needed > impl_->scratch.video_enc_buf.capacity() * 4) {
                    impl_->video_state.stage_timings.record(Stage::kEncode, encode_ns);
                    return plugins::kErrInternal;  // give up on pathological sizes
                }
                impl_->scratch.video_enc_buf.reserve(needed);
                const auto retry_start = std::chrono::steady_clock::now();
                const auto retry_rc = video_codec_->encode(raw,
                                                          impl_->scratch.video_enc_buf.data(),
                                                          impl_->scratch.video_enc_buf.capacity(),
                                                          encoded);
                const auto retry_end = std::chrono::steady_clock::now();
                encode_ns += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        retry_end - retry_start).count());
                impl_->video_state.stage_timings.record(Stage::kEncode, encode_ns);
                if (retry_rc != plugins::kOk) {
                    return plugins::kErrInternal;
                }
            } else {
                impl_->video_state.stage_timings.record(Stage::kEncode, encode_ns);
            }
        }
    } else {
        // The zero-copy path: the codec consumed a native GPU handle, so
        // there was no staging copy — but the *encode* still costs time,
        // and the budget is about the whole stage.  Measure it here too,
        // or the hardware path would look artificially free.
        impl_->video_state.stage_timings.record(Stage::kEncode, 0);
        // Plan C3: no format work happened at all on this path — the
        // picture reached the encoder as the native surface the device
        // produced.
        ++impl_->video_state.video_format_zero_copy;
        impl_->video_state.video_format_source = frame.format;
    }

    // Mark keyframe so the sender packet callback gives it kVideoKeyframe
    // priority (consumed on the marker=true packet).
    if (encoded.is_keyframe) {
        impl_->video_state.video_keyframe_pending_.store(true, std::memory_order_release);
    }

    // Phase 4.2 / D2: record the encoded frame in the *outbound* quality
    // statistics.  The QP is the encoder's own quality signal — it says
    // how much detail the encoder had to drop to hit the bitrate — so it
    // belongs to the outbound stream, not to the inbound one.
    impl_->video_state.video_stats.on_frame_encoded(
        static_cast<std::int64_t>(encoded.info.capture_ts_us),
        static_cast<std::uint32_t>(encoded.payload.size()),
        encoded.is_keyframe,
        static_cast<int>(encoded.qp));

    // Packetize (the sender's packet callback handles RTP header assembly
    // + SRTP protect + scheduler enqueue).
    //
    // Plan J1: the packetize stage is the third term of the plan's
    // budget ("capture → encode → packetize").  It is measured around
    // the whole `push_frame` call, so it covers the RTP header assembly,
    // the STAP-A / FU-A fragmentation and the SRTP protection.
    const auto packetize_start = std::chrono::steady_clock::now();
    const auto push_rc = video_sender_->push_frame(encoded, frame.rtp_timestamp);
    const auto packetize_end = std::chrono::steady_clock::now();
    impl_->video_state.stage_timings.record(
        Stage::kPacketize,
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                packetize_end - packetize_start).count()));
    if (push_rc == plugins::kOk) {
        // v1.1 V1: one encoded frame accepted by the sender pipeline.
        ++impl_->video_state.video_frames_sent;
        // v1.1 T1: record the outbound ref_frame mapping (id == frame_seq).
        record_video_ref_frame(static_cast<std::uint64_t>(frame.frame_seq),
                               static_cast<std::int64_t>(frame.capture_ts_us));
    }
    return push_rc;
}


void NimRTCEngine::handle_rtcp(plugins::BufferView view) noexcept {
    // ---- Video feedback (NACK / PLI / FIR) --------------------------------
    //
    // This runs *before* the timeline gate: the feedback loop is a media
    // quality feature, not a timeline feature, and it must work with
    // `timeline_enabled = false` (which only disables the SR / abs-send-
    // time seam).
    //
    // The sender plugin knows how to answer a NACK (retransmit) and how
    // to answer a PLI / FIR (arm the key-frame marker).  The engine owns
    // the encoder, so a key-frame request has to be forwarded there too:
    // the packetizer cannot produce an IDR by itself.
    if (video_sender_) {
        const int actions = video_sender_->apply_feedback_rtcp(view);
        // v1.1 V1: inbound feedback actions (retransmissions + keyframe reqs).
        if (actions > 0) {
            impl_->video_state.video_feedback_applied += static_cast<std::uint64_t>(actions);
        }
        const std::uint32_t keyframe_requests =
            video_sender_->take_keyframe_requests();
        if (keyframe_requests > 0) {
            impl_->video_state.video_keyframes_applied += keyframe_requests;
            if (video_codec_) {
                (void)video_codec_->force_keyframe();
            }
        }
    }

    // The peer's Sender Report is parsed whether or not the cross-end
    // timeline is enabled: the report's *LSR* field (the middle 32
    // bits of the peer's NTP timestamp) is a fact about the wire that
    // our receiver report has to carry (RFC 3550 §6.4.1), and the peer
    // sends its Sender Reports whether we track the timeline or not.
    // Only the *tracker* feed below is gated — with the timeline off
    // the engine MUST NOT maintain an anchor (ADR-017 §"Decision 5").

    // Compound RTCP packets are a concatenation of sub-packets sharing one
    // header (RFC 3550 §6.1). Walk them so an SR that follows an RR in the
    // same compound datagram is not missed, and so each sub-packet reaches
    // *its* consumer: the Sender Report to the cross-end timeline, the
    // receiver-side network feedback (RR / REMB / transport-cc) to the
    // net-feedback collector.
    //
    // The walk itself lives in the `rtp` module (`rtp::demux_rtcp`), which
    // is forgiving: a sub-packet whose length field does not fit ends the
    // walk rather than throwing — the rest of the datagram is unparseable,
    // but that is not an error worth failing the read path for, and the
    // well-formed head of the datagram must not be thrown away.
    const core::ByteSpan compound(view.data(), view.size());
    // Phase 3 / C6: demultiplex into the engine's reusable vector, so
    // the receive path performs no heap allocation per RTCP datagram.
    rtp::demux_rtcp(compound, impl_->scratch.rtcp_blocks_scratch);
    const std::vector<rtp::RtcpBlock>& blocks = impl_->scratch.rtcp_blocks_scratch;
    const core::TimePoint now = core::SteadyClock::now();

    for (const rtp::RtcpBlock& blk : blocks) {
        switch (blk.kind) {
            case rtp::RtcpBlockKind::kSenderReport: {
                rtp::Parser rp;
                auto sr = rp.parse_sr(blk.packet);
                if (sr) {
                    // The peer's Sender Report is the anchor of the
                    // receiver report we send back: its middle 32 bits are
                    // the LSR field, and the instant we received it is the
                    // DLSR field (RFC 3550 §6.4.1).  This is recorded
                    // whether or not the cross-end timeline is enabled —
                    // the peer's report is a fact about the wire, not about
                    // our feature set.
                    if (sr.value().ssrc == impl_->audio_state.audio_rx.ssrc) {
                        impl_->audio_state.audio_rx.last_sr_lsr =
                            rtp::Ntp64{sr.value().ntp_timestamp}.mid_32();
                        impl_->audio_state.audio_rx.last_sr_recv_us = detail::steady_now_us();
                    }

                    if (!config_.timeline_enabled) {
                        break;  // no anchor to maintain — nothing to feed
                    }

                    // The tracker wants our *monotonic* clock reading at
                    // receipt: it combines it with the monotonic origin to
                    // place our own clock on the peer's wall-clock timeline
                    // (ADR-017 §"Decision 4").  Passing a wall-clock reading
                    // here would double-count the epoch and make the skew
                    // meaningless.
                    impl_->timeline.sr_tracker.on_sender_report(sr.value(),
                                                      detail::steady_now_us());
                }
                break;
            }

            case rtp::RtcpBlockKind::kReceiverReport:
                // The receiver-side network feedback goes to the collector,
                // which aggregates it into one snapshot (v1.2.0 M1, F1/F2).
                // The collector decides what it understands — the payload
                // type and the FMT are handed over so it can tell a REMB
                // from a PLI and a transport-cc from a NACK.
                if (net_feedback_) {
                    net_feedback_->on_rtcp_block(blk.payload_type, blk.fmt,
                                                 blk.packet, now);
                    ++impl_->net_feedback_blocks;
                }
                break;

            case rtp::RtcpBlockKind::kRtpFeedback:
                // The RTPFB packet type carries several feedback
                // messages, told apart by the FMT (RFC 4585 §6.4):
                // NACK (1), TMMBR (3), TMMBN (4), transport-cc (15).
                //
                // A TMMBR is *not* receiver-side network feedback: it
                // is the peer telling us how much we may send, so it
                // goes to the TMMBR path (v1.2.0, session plan X5)
                // rather than to the bandwidth collector.  Everything
                // else (NACK, transport-cc) is the collector's business.
                if (blk.fmt == 3) {   // TMMBR — RFC 5104 §4.2.1
                    handle_inbound_tmmbr(blk.packet);
                    break;
                }
                // Phase 4.2 / D2: a NACK (RFC 4585 §6.2.1) is the peer
                // asking us to retransmit packets it lost.  The feedback
                // describes the **outbound** stream (what we sent), so it
                // is recorded on the outbound sink — the inbound sink
                // would say the *peer's* receive quality is bad, which is
                // not what a NACK tells us.
                if (blk.fmt == 1) {
                    impl_->video_state.video_stats.on_rtcp_feedback_tx(
                        detail::VideoFeedbackKind::kNACK);
                }
                if (net_feedback_) {
                    net_feedback_->on_rtcp_block(blk.payload_type, blk.fmt,
                                                 blk.packet, now);
                    ++impl_->net_feedback_blocks;
                }
                break;

            case rtp::RtcpBlockKind::kPayloadSpecificFeedback:
                // The receiver-side network feedback goes to the collector,
                // which aggregates it into one snapshot (v1.2.0 M1, F1/F2).
                // The collector decides what it understands — the payload
                // type and the FMT are handed over so it can tell a REMB
                // from a PLI and a transport-cc from a NACK.
                //
                // Phase 4.2 / D2: a PLI (FMT 1) or a FIR (FMT 4) is the
                // peer saying "I lost picture data, send me a key frame".
                // It describes the **outbound** stream (what we sent), so
                // it is recorded on the outbound sink: the peer could not
                // decode our stream because a reference frame was lost on
                // the way to it.
                if (blk.fmt == 1) {   // PLI — RFC 4585 §6.3.1
                    impl_->video_state.video_stats.on_rtcp_feedback_tx(
                        detail::VideoFeedbackKind::kPLI);
                } else if (blk.fmt == 4) {   // FIR — RFC 5104 §4.3.1
                    impl_->video_state.video_stats.on_rtcp_feedback_tx(
                        detail::VideoFeedbackKind::kFIR);
                }
                if (net_feedback_) {
                    net_feedback_->on_rtcp_block(blk.payload_type, blk.fmt,
                                                 blk.packet, now);
                    ++impl_->net_feedback_blocks;
                }
                break;

            case rtp::RtcpBlockKind::kExtendedReport:
                // The RFC 3611 extended report goes to the XR consumer,
                // which decodes the report blocks for the host (v1.2.0,
                // session capability plan X1).  The consumer decides
                // what it understands — a PT that is not 207 is never
                // its business.
                if (xr_report_) {
                    xr_report_->on_rtcp_block(blk.payload_type, blk.packet, now);
                    ++impl_->xr_report_blocks;
                }
                // The Receiver Reference Time block (BT = 4) is the
                // one block that needs the *engine's* attention: our
                // next DLRR answers it (RFC 3611 §4.5).  The consumer
                // decodes it for the host; the engine remembers it so
                // the DLRR can carry the right LRR / DLRR pair.
                note_peer_reference_time(blk.packet);
                break;

            default:
                // SDES, BYE, APP and anything this build does not know
                // about: not ours.
                break;
        }
    }
}


void NimRTCEngine::queue_video_feedback(
    const plugins::VideoFeedbackReport& report) noexcept {
    if (report.missing_seqs.empty() && !report.want_keyframe) return;

    // v1.1 V1: outbound feedback queued (NACK seqs + keyframe requests).
    impl_->video_state.video_nack_seqs_queued += report.missing_seqs.size();
    if (report.want_keyframe) ++impl_->video_state.video_keyframes_q;

    // Phase 4.2 / D2: record the feedback we are about to send.  A NACK
    // means the *peer* lost picture data on the **inbound** stream (the
    // one it receives from us) — so it is recorded on the inbound sink,
    // where the freeze / drop counters live: the peer's loss is a loss
    // *in the stream we sent*, which is our inbound direction.
    //
    // A key-frame request means the peer is already resynchronising
    // because a reference frame never arrived.
    if (!report.missing_seqs.empty()) {
        impl_->video_state.video_stats.on_rtcp_feedback(
            detail::VideoFeedbackKind::kNACK);
    }
    if (report.want_keyframe) {
        impl_->video_state.video_stats.on_rtcp_feedback(
            detail::VideoFeedbackKind::kPLI);
    }

    // Merge into the pending report: two gaps detected back to back must
    // not cost two round trips, and a NACK list plus a key-frame request
    // belong in the same feedback packet anyway.
    if (!impl_->video_state.has_pending_video_feedback_) {
        impl_->video_state.pending_video_feedback_ = report;
        impl_->video_state.has_pending_video_feedback_ = true;
        return;
    }
    impl_->video_state.pending_video_feedback_.missing_seqs.insert(
        impl_->video_state.pending_video_feedback_.missing_seqs.end(),
        report.missing_seqs.begin(), report.missing_seqs.end());
    impl_->video_state.pending_video_feedback_.want_keyframe =
        impl_->video_state.pending_video_feedback_.want_keyframe || report.want_keyframe;
}


void NimRTCEngine::flush_video_feedback() noexcept {
    if (!impl_->video_state.has_pending_video_feedback_) return;
    // Take the report out of the queue *before* sending: a send failure
    // must not make the engine retry the same report forever.
    const plugins::VideoFeedbackReport report =
        impl_->video_state.pending_video_feedback_;
    impl_->video_state.pending_video_feedback_ = plugins::VideoFeedbackReport{};
    impl_->video_state.has_pending_video_feedback_ = false;

    if (!video_receiver_) return;
    if (!ice_t_ || !is_ice_connected()) return;

    // The receiver plugin owns the RTCP wire format; the engine only
    // carries the bytes.  (L4 — the engine must not include a concrete
    // module header, so it cannot build a NACK itself.)
    const std::size_t needed = video_receiver_->feedback_rtcp_size(report);
    if (needed == 0) return;

    std::vector<std::uint8_t> buf(needed);
    const std::size_t written = video_receiver_->build_feedback_rtcp(
        report,
        impl_->video_state.video_feedback_ssrc_,   // we are the feedback sender
        impl_->video_state.remote_video_ssrc_,     // the peer's video stream is the media source
        core::MutableByteSpan{buf.data(), buf.size()});
    if (written == 0 || written > buf.size()) return;

    // RTCP leaves the engine as SRTCP (RFC 3711 §3.4) once SRTP is keyed
    // — a different crypto context from SRTP, so protect_rtp would
    // produce a packet the peer's unprotect_rtcp rejects.
    core::ByteSpan out(buf.data(), written);
    std::vector<std::uint8_t> enc;
    if (impl_->srtp_installed && impl_->srtp) {
        auto* sess = impl_->srtp->get_session(
            config_.video_sender_tuning.ssrc, /*outgoing=*/true);
        if (!sess) return;   // SRTP installed but no keying yet — skip.
        auto r = sess->protect_rtcp(out);
        if (!r) {
            core::log::Logger::instance().warn(
                "engine[feedback]: protect_rtcp failed; feedback dropped");
            return;
        }
        enc.assign(r.value().data(), r.value().data() + r.value().size());
        out = core::ByteSpan(enc.data(), enc.size());
    }

    plugins::BufferView bv{out.data(), out.size()};
    if (ice_t_->send(bv, {}) != plugins::kOk) {
        core::log::Logger::instance().warn(
            "engine[feedback]: RTCP feedback send failed");
    }
}


// ---------------------------------------------------------------------------
// Adaptive audio bitrate (BWE-linked)
// ---------------------------------------------------------------------------
//
// The engine keeps the audio encoder's bitrate inside the band the
// bandwidth estimate allows:
//
//   budget = estimate.target_bitrate_bps * Config::audio_bwe_share
//   target = clamp(budget, floor = audio_min_bitrate_bps,
//                          ceiling = audio_bitrate_bps)
//
// The step towards the target is rate-limited so the encoder never
// oscillates: a drop is applied immediately (at most -50 % per step,
// so a congestion collapse is answered within a couple of intervals)
// while a rise is at most +10 % per step (so a flickering estimate
// cannot pump the encoder up and down).  A deviation of less than 10 %
// is ignored altogether (hysteresis).
//
// The bitrate is pushed to the codec through `ICodec::update_config()`,
// which reconfigures the running encoder in place — the codec state and
// its statistics survive the step.

int NimRTCEngine::resolve_audio_bitrate_ceiling() const noexcept {
    if (config_.audio_bitrate_bps > 0) return config_.audio_bitrate_bps;
    // The codec default: 32 kbps for mono, 64 kbps for stereo.
    return config_.pcm_channels >= 2 ? kDefaultAudioBitrateStereoBps
                                     : kDefaultAudioBitrateMonoBps;
}

void NimRTCEngine::maybe_update_audio_bitrate() noexcept {
    if (!codec_plugin_ || !bwe_) return;

    const int current = audio_bitrate_current_bps_;
    if (current <= 0) return;

    // ---- The peer's TMMBR limit (v1.2.0, session plan X5) -----------
    //
    // RFC 5104 §4.2.1.2: a TMMBR "indicates the maximum bitrate the
    // sender of the packet ... is willing to receive".  The TMMBN we
    // answer it with (RFC 5104 §4.2.2.2) *acknowledges* that bound, so
    // from that moment on the limit is a promise, not a suggestion.
    //
    // The scheduler budget and the pacer are already clamped by it (see
    // engine_transport.cpp), but the *encoder* is what actually decides
    // how many bits go on the wire — a pacer that drops the excess just
    // loses frames.  So the limit has to reach the encoder too, or the
    // engine would encode above the ceiling it promised in the TMMBN.
    //
    // A limit of 0 means "no limit" (the peer has not sent a TMMBR).
    const std::uint32_t peer_limit = impl_->xr.peer_tmmbr_limit_bps;

    // ---- Rate limit: one step per `audio_bitrate_update_interval_ms` ----
    const auto now = std::chrono::steady_clock::now();
    if (audio_bitrate_last_step_.time_since_epoch().count() != 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - audio_bitrate_last_step_).count();
        const int interval = config_.audio_bitrate_update_interval_ms > 0
                                 ? config_.audio_bitrate_update_interval_ms
                                 : 1000;
        if (elapsed < interval) return;
    }

    const std::uint32_t bwe_bps = bwe_->estimate().target_bitrate_bps;

    // ---- The peer's promise outranks the "pinned encoder" switch ----
    //
    // `Config::audio_bitrate_adaptive = false` says "do not move the
    // encoder because of the *estimate*".  It is not a licence to break
    // a promise the engine already made on the wire: the TMMBN that
    // acknowledged the TMMBR told the peer "this bound is in force".
    // RFC 5104 §4.2.2.2 makes the bounding set a commitment, so the
    // encoder is brought under the limit even when the estimate-driven
    // adaptation is off.
    if (!config_.audio_bitrate_adaptive) {
        if (peer_limit > 0 && current > static_cast<int>(peer_limit)) {
            apply_audio_bitrate(static_cast<int>(peer_limit), current,
                                bwe_bps, peer_limit);
        }
        return;
    }

    if (bwe_bps == 0 && peer_limit == 0) {
        // No estimate yet — keep the current bitrate rather than
        // reading "0" as "no bandwidth at all".
        return;
    }
    audio_bitrate_last_step_ = now;

    // The effective estimate is the tighter of the two: the link's own
    // estimate and the ceiling the peer asked for.  When only the peer
    // limit is known (no estimate yet), the peer limit is the estimate.
    std::uint32_t estimate_bps = bwe_bps;
    if (peer_limit > 0 &&
        (estimate_bps == 0 || estimate_bps > peer_limit)) {
        estimate_bps = peer_limit;
    }

    // ---- Target = the reserved share of the estimate -------------------
    //
    // The split rule itself lives in `bandwidth_allocation.hpp` (v1.2.0,
    // video plan F8): the session's estimate is divided between the
    // streams, the *reserved* ones first.  The audio is the only
    // reserved stream today; the video's elastic share of what is left
    // arrives with the rate controller (B2), which is handed a per
    // stream budget rather than the raw estimate.
    //
    // The rule clamps the floor to the ceiling (a ceiling configured
    // below the floor wins), so the two are passed through unchanged.
    //
    // ---- The peer's limit wins over the local floor -------------------
    //
    // The local floor (`Config::audio_min_bitrate_bps`) exists so that a
    // congestion collapse does not starve the audio.  The peer's TMMBR
    // limit is a *different* kind of constraint: it is a promise the
    // engine makes in its TMMBN, and exceeding it means the engine
    // breaks its own promise — the peer has told us it cannot receive
    // more, and a promise is not something a local floor may override.
    //
    // So when the peer's limit is below the local floor, the peer limit
    // wins and the floor is lowered to it.  (OD-E decision 3:
    // "peer-wins".)  A TMMBN is a commitment, and a commitment to send
    // *less* is not something the sender may quietly exceed.
    int ceiling = audio_bitrate_ceiling_bps_ > 0 ? audio_bitrate_ceiling_bps_
                                                  : current;
    int floor_bps = config_.audio_min_bitrate_bps > 0
                        ? config_.audio_min_bitrate_bps
                        : 1000;
    if (peer_limit > 0) {
        if (ceiling > static_cast<int>(peer_limit)) {
            ceiling = static_cast<int>(peer_limit);
        }
        if (floor_bps > static_cast<int>(peer_limit)) {
            floor_bps = static_cast<int>(peer_limit);
        }
    }

    detail::BandwidthDemand audio_demand = detail::reserved_audio_demand(
        config_.audio_bwe_share,
        static_cast<std::uint32_t>(floor_bps),
        static_cast<std::uint32_t>(ceiling));

    const detail::BandwidthAllocation alloc =
        detail::allocate_bandwidth(estimate_bps, {audio_demand});
    if (alloc.targets.empty() || alloc.targets[0] == 0) return;
    int target = static_cast<int>(alloc.targets[0]);
    if (target < floor_bps) target = floor_bps;
    if (target > ceiling) target = ceiling;

    if (target == current) return;

    // ---- Hysteresis + ramp limits -------------------------------------
    const int hysteresis = std::max(1, current / 10);   // 10 % of current
    if (target > current) {
        // Rise slowly: at most +10 % per step.
        if (target < current + hysteresis) return;
        target = current + hysteresis;
    } else {
        // Fall fast: at most -50 % per step.
        //
        // ...except when the peer's limit is already being violated.
        // A limit that is in force *now* is not something to "ramp down
        // to" over the next few steps — the promise is already broken,
        // and the rate-limited ramp would keep it broken for seconds.
        // So the encoder drops straight to the target (which is already
        // clamped to the limit by the ceiling above) in one step.
        if (peer_limit > 0 && current > static_cast<int>(peer_limit)) {
            target = std::min(target, static_cast<int>(peer_limit));
        } else {
            if (target > current - hysteresis) return;
            target = std::max(target, current / 2);
        }
    }
    if (target < floor_bps) target = floor_bps;
    if (target > ceiling) target = ceiling;
    if (target == current) return;

    apply_audio_bitrate(target, current, bwe_bps, peer_limit);
}

/** Push a new bitrate into the running encoder (in-place reconfigure).
 *
 *  The codec state and its statistics survive the step — only the
 *  target rate changes.  A codec plugin that refuses the new rate is
 *  left alone (and the engine keeps the old rate), because a rejected
 *  reconfigure must not leave the engine believing it is sending at a
 *  rate it is not. */
void NimRTCEngine::apply_audio_bitrate(int target, int current,
                                        std::uint32_t bwe_bps,
                                        std::uint32_t peer_limit) noexcept {
    plugins::CodecConfig cfg = codec_plugin_->config();
    cfg.bitrate_bps = target;
    if (codec_plugin_->update_config(cfg) != plugins::kOk) {
        core::log::Logger::instance().warn(
            "engine: audio bitrate update rejected by the codec plugin");
        return;
    }
    audio_bitrate_current_bps_ = target;

    core::log::Logger::instance().info(
        "engine: audio encoder bitrate " + std::to_string(current) +
        " -> " + std::to_string(target) + " bps (bwe=" +
        std::to_string(bwe_bps) + ", peer_tmmbr_limit=" +
        std::to_string(peer_limit) + ")");
}

NimRTCEngine::Stats NimRTCEngine::stats() const noexcept {

    Stats out;
    out.srtp_drops = impl_->srtp_stats_drop;
    // v1.1 V1: cumulative video counters (additive; zero when unused).
    out.video_frames_sent       = impl_->video_state.video_frames_sent;
    out.video_packets_sent      = impl_->video_state.video_packets_sent;
    out.video_bytes_sent        = impl_->video_state.video_bytes_sent;
    out.video_frames_received   = impl_->video_state.video_frames_received;
    out.video_nack_seqs_queued  = impl_->video_state.video_nack_seqs_queued;
    out.video_keyframes_q       = impl_->video_state.video_keyframes_q;
    out.video_feedback_applied  = impl_->video_state.video_feedback_applied;
    out.video_keyframes_applied = impl_->video_state.video_keyframes_applied;

    // Remote-desktop plan H4: the per-stream statistics list.  The flat
    // `video_*` fields above describe the **default** stream; the list
    // carries one entry per *active* stream (the default one included),
    // each with its own bitrate, ladder level, keyframe rate and backend
    // id.
    append_video_stream_stats(out.video_streams);
    // Phase 3 / C3: the zero-copy path counters.
    out.video_zero_copy_frames    = impl_->video_state.video_zero_copy_frames;
    out.video_zero_copy_fallbacks = impl_->video_state.video_zero_copy_fallbacks;
    // Remote-desktop plan E1: the receive-side delivery-path counters.
    out.video_rx_gpu_frames       = impl_->video_state.video_rx_gpu_frames;
    out.video_rx_cpu_frames       = impl_->video_state.video_rx_cpu_frames;

    // ---- Video quality statistics (Phase 4.2 / D2) ----------------------
    //
    // The numbers come from the `nimrtc::video_stats` sinks — the module
    // that owns the freeze detection, the QP accumulation, the bitrate
    // window and the drop attribution.  The engine does not re-derive
    // them: the module is the single authority, exactly as the SDP
    // attributes module became the single authority for `a=fmtp:`.
    //
    // The two directions are kept apart (Chrome's `inbound-rtp` /
    // `outbound-rtp` split): the freeze count and the drop attribution
    // describe what the *peer* sent us, the QP describes what *we* sent.
    if (impl_->video_state.video_stats.active()) {
        const auto rx = impl_->video_state.video_stats.snapshot_rx();
        out.video_freeze_count    = rx.freeze_count;
        out.video_freeze_total_ms = rx.freeze_total_ms;
        out.video_last_freeze_ms  = rx.last_freeze_ms;
        out.video_keyframe_interval_avg_ms = rx.keyframe_interval_avg_ms;
        out.video_frames_dropped_jb = rx.frames_dropped_jb;
        out.video_frames_dropped_decoder = rx.frames_dropped_decoder;
        out.video_frames_dropped_dependency = rx.frames_dropped_dependency;
        out.video_current_bitrate_bps = rx.current_bitrate_bps;
        out.video_avg_bitrate_bps    = rx.avg_bitrate_bps;
        out.video_bytes_received   = rx.bytes_received;
        out.video_frames_decoded   = rx.frames_decoded;
        out.video_frames_rendered  = rx.frames_rendered;

        const auto tx = impl_->video_state.video_stats.snapshot_tx();
        out.video_qp_sum           = tx.qp_sum;
        out.video_qp_count         = tx.qp_count;
        out.video_tx_frames_encoded = tx.frames_received;
        out.video_tx_bytes_encoded  = tx.bytes_received;
        // The dependency losses the *peer* reported about our stream
        // (a PLI / FIR it sent us): our transmit side lost a reference
        // frame, so the peer could not decode what followed.
        out.video_tx_frames_dropped_dependency = tx.frames_dropped_dependency;
    }
    // v1.2.0 M1: the feedback loop's own counters.  `bwe_feedback_samples`
    // staying at zero means the loop is not closed — the estimator is
    // never fed, so the bandwidth target never moves.
    out.net_feedback_blocks   = impl_->net_feedback_blocks;
    out.bwe_feedback_samples  = impl_->bwe_feedback_samples;
    // v1.2.0 (session plan X1): the XR routing counter.
    out.xr_report_blocks      = impl_->xr_report_blocks;
    if (bwe_) {
        out.bandwidth_estimate_bps = bwe_->estimate().target_bitrate_bps;
    }
    // WP-5: the audio send counters.  `audio_encode_errors` is the one
    // that matters to a host: it counts the PCM that was accepted and
    // then could not be encoded, which used to disappear silently.
    out.audio_frames_sent     = impl_->audio_state.audio_frames_sent;
    out.audio_bytes_sent      = impl_->audio_state.audio_bytes_sent;
    out.audio_encode_errors   = impl_->audio_state.audio_encode_errors;
    out.audio_samples_in      = impl_->audio_state.audio_samples_in;
    out.audio_samples_pending = impl_->audio_state.audio_accum_frames;

    // ---- Audio receive counters (v1.2.0, audio pipeline WP-8) ---------
    //
    // The split follows the ownership: the *engine* counts what it
    // handed to the host (frames, samples, concealed frames, FEC
    // recoveries), and the *receive pipeline* counts what happened on
    // the wire (packets lost, decode errors, jitter, playout delay).
    // The pipeline owns the detailed bookkeeping — it is the only
    // party that knows which slots were concealed and how deep the
    // jitter buffer was — so the numbers come from there and are not
    // re-derived here (spec B-13).
    out.audio_packets_sent         = impl_->audio_state.audio_packet_count;
    out.audio_frames_received      = impl_->audio_state.audio_frames_received;
    out.audio_samples_received     = impl_->audio_state.audio_samples_received;
    out.audio_concealed_frames     = impl_->audio_state.audio_frames_concealed;
    out.audio_fec_recovered_frames = impl_->audio_state.audio_frames_fec_recovered;

    if (audio_receiver_) {
        const auto rx = audio_receiver_->stats();
        out.audio_packets_lost     = rx.packets_lost;
        out.audio_decode_errors    = rx.decode_errors;
        out.audio_jitter_ms        = rx.jitter_ms;
        out.audio_playout_delay_ms = rx.current_delay_ms;
    }

    // ---- DTMF counters (v1.2.0, session capability plan D1–D4) ---------
    //
    // The telephone-event plane has counters of its own because a
    // telephone-event payload never reaches the audio pipeline: the
    // audio counters above say nothing about it.  `dtmf_payload_type`
    // is the negotiated payload type (RFC 4733 §2.3.2); zero means the
    // peer never negotiated one — the state in which a received
    // telephone-event payload counts as an unknown audio payload type
    // (decision D4).
    out.dtmf_packets_sent      = impl_->audio_state.dtmf_packets_sent;
    out.dtmf_packets_received  = impl_->audio_state.dtmf_packets_received;
    out.dtmf_packets_malformed = impl_->audio_state.dtmf_packets_malformed;
    out.dtmf_events_delivered  = impl_->audio_state.dtmf_events_delivered;
    out.dtmf_payload_type      = negotiated_dtmf_pt_;

    // The bitrate the outbound audio encoder currently runs at — the
    // value the BWE-linked adaptation last applied (it starts at the
    // configured ceiling).
    out.audio_encoder_bitrate_bps =
        static_cast<std::uint32_t>(audio_bitrate_current_bps_ > 0
                                       ? audio_bitrate_current_bps_
                                       : 0);

    // ---- Capture statistics (remote-desktop plan A11) --------------------
    //
    // The capture device's own counters, reported verbatim.  The device
    // is the only party that knows how many frames it produced and how
    // many its bounded queue had to drop, so the engine does not
    // re-derive them — it asks.
    if (video_capture_) {
        const plugins::CaptureStats cs = video_capture_->stats();
        out.video_capture_frames_captured  = cs.frames_captured;
        out.video_capture_frames_dropped   = cs.frames_dropped;
        out.video_capture_device_errors    = cs.device_errors;
        out.video_capture_device_restarts  = cs.device_restarts;
        out.video_capture_format_changes   = cs.format_changes;
        out.video_capture_bytes_copied     = cs.bytes_copied;
        out.video_capture_frames_zero_copy = cs.frames_zero_copy;
        out.video_capture_frames_cpu_staged = cs.frames_cpu_staged;
    }

    // ---- The capture → engine pump (remote-desktop plan A10) ------------
    out.video_capture_pumped_frames   = impl_->video_state.video_capture_pumped_frames;
    out.video_capture_pump_errors     = impl_->video_state.video_capture_pump_errors;
    out.video_capture_pumped_metadata = impl_->video_state.video_capture_pumped_metadata;

    // ---- Format normalisation (remote-desktop plan C3) ------------------
    //
    // The conversion count is the number a host reads to decide whether
    // the pipeline is doing colour work it does not need to.  On the
    // zero-copy path it stays at zero.
    out.video_format_conversions = impl_->video_state.video_format_conversions;
    out.video_format_copies      = impl_->video_state.video_format_copies;
    out.video_format_zero_copy   = impl_->video_state.video_format_zero_copy;
    out.video_format_canonical   =
        std::string{plugins::pixel_format_name(
            impl_->video_state.video_format_canonical)};
    out.video_format_source =
        std::string{plugins::pixel_format_name(
            impl_->video_state.video_format_source)};

    // ---- Codec telemetry (remote-desktop plan B15) ------------------------
    //
    // The codec knows which backend it is, whether that backend is
    // hardware, and how long its encodes take.  The engine reports the
    // answers rather than guessing them from the plugin id.
    if (video_codec_) {
        out.video_codec_backend = std::string{video_codec_->backend_id()};
        out.video_codec_hardware_accelerated = video_codec_->is_hardware_accelerated();
        out.video_codec_backend_preference =
            std::string{plugins::video_codec_backend_preference_name(
                impl_->video_state.video_codec_backend_preference)};

        const plugins::VideoCodecStats cs = video_codec_->stats();
        out.video_codec_frames_zero_copy = cs.frames_encoded_zero_copy;
        out.video_codec_frames_staged   = cs.frames_encoded_staged;
        out.video_codec_keyframes       = cs.keyframes_encoded;
        out.video_codec_bytes_copied    = cs.bytes_copied;
        out.video_codec_encode_latency_max_us = cs.encode_latency_us_max;
        if (cs.frames_encoded > 0 && cs.encode_latency_us_total > 0) {
            out.video_codec_encode_latency_avg_us =
                cs.encode_latency_us_total / cs.frames_encoded;
        }
    }

    // ---- Sink statistics (remote-desktop plan A11 / E8) ------------------
    //
    // The sink's counters are the renderer's own: how many frames it
    // drew, how many it had to drop under back-pressure, and the rate
    // the presentation clock actually achieved.
    if (video_sink_) {
        const plugins::VideoSinkStats ss = video_sink_->stats();
        out.video_sink_frames_rendered        = ss.frames_rendered;
        out.video_sink_frames_dropped         = ss.frames_dropped;
        out.video_sink_frames_dropped_present = ss.frames_dropped_present;
        out.video_sink_frames_presented        = ss.frames_presented;
        out.video_sink_render_errors          = ss.render_errors;
        out.video_sink_present_fps             = ss.present_fps;
    }

    // The outbound video bitrate: the encoded bytes over the last
    // second, as measured by the video-quality sink's bitrate window.
    if (impl_->video_state.video_stats.active()) {
        out.video_tx_bitrate_bps =
            impl_->video_state.video_stats.snapshot_tx().current_bitrate_bps;
    }

    // ---- The per-stage CPU-time instrumentation (plan J1) ---------------
    //
    // The engine's own measurement of each pipeline stage, reported
    // verbatim.  This is the number §B.6's budget is written against —
    // without it the budget is a sentence in a document rather than
    // something the code checks.
    {
        const auto& t = impl_->video_state.stage_timings;

        const auto fill = [&t](Stage stage, Stats::StageTiming& dst) {
            dst.samples = t.samples(stage);
            dst.mean_us = t.mean_us(stage);
            dst.p95_us  = t.percentile_us(stage, 95);
            dst.max_us  = t.max_us(stage);
            dst.total_us = t.total_us(stage);
        };

        fill(Stage::kCapture,   out.video_stage_capture);
        fill(Stage::kConvert,   out.video_stage_convert);
        fill(Stage::kEncode,    out.video_stage_encode);
        fill(Stage::kPacketize, out.video_stage_packetize);
        fill(Stage::kDecode,    out.video_stage_decode);
        fill(Stage::kRender,    out.video_stage_render);

        // The budget is written against the *outbound* chain
        // (capture → encode → packetize), so that is what the total
        // sums: the convert stage is in the middle because the plan's
        // chain has it there, and on the zero-copy path it is zero —
        // which is the point.
        out.video_stage_total_us =
            out.video_stage_capture.mean_us + out.video_stage_convert.mean_us +
            out.video_stage_encode.mean_us + out.video_stage_packetize.mean_us;
        out.video_stage_total_p95_us =
            out.video_stage_capture.p95_us + out.video_stage_convert.p95_us +
            out.video_stage_encode.p95_us + out.video_stage_packetize.p95_us;
    }

    // ---- The render clock (remote-desktop plan E5) -----------------------
    //
    // The counters the render clock keeps: how many frames it presented,
    // dropped for sync and held back, and how much hold time it
    // promised.  They come from the controller — the engine does not
    // re-derive them.
    out.video_sync = video_sync_stats();

    return out;
}

// ---------------------------------------------------------------------------
// The default stream's RTP send path
// ---------------------------------------------------------------------------
//
// The default stream is the legacy single-stream path, so its packet
// callback carries the pre-H1 behaviour verbatim.  It lives in its own
// method (rather than inline in init_video_plugins()) because a codec
// swap rebuilds the sender, and the rebuilt sender needs the very same
// callback -- see set_video_codec(stream_id, name).
void NimRTCEngine::wire_default_sender() noexcept {
    if (!video_sender_) return;
    video_sender_->set_packet_callback(
        [this](plugins::BufferView payload,
               std::uint32_t rtp_ts,
               std::uint16_t seq,
               bool marker) {
            // ---- Build the RTP packet (rtp::PacketBuilder) -----------------
            //
            // The video path used to hand-assemble the 12-byte RTP
            // header and hard-code "no extensions" (X=0).  That left
            // the video stream without the ADR-017 abs-send-time
            // extension, so the receiver-side timeline had no
            // send-instant for video and the bandwidth estimator
            // could not correlate video packets with their wall
            // clock send time.  The audio path already used
            // `rtp::PacketBuilder`; both paths now share it, so a
            // fix to the RTP header lands in one place.
            //
            // The payload type is the *negotiated* one (C9 / C11):
            // it is the number the description exchange settled on,
            // so the peer recognises the stream.  Before a
            // negotiation happened the locally configured number is
            // the fallback, which keeps a host that drives the
            // engine without SDP working.
            const std::uint8_t negotiated_pt =
                impl_->video_state.negotiated_video_pt.load(std::memory_order_relaxed);
            const std::uint8_t local_pt =
                impl_->video_state.video_payload_type != 0
                    ? impl_->video_state.video_payload_type
                    : config_.video_sender_tuning.payload_type;
            const std::uint8_t pt = negotiated_pt != 0 ? negotiated_pt : local_pt;

            rtp::PacketBuilder pb;
            pb.set_ssrc(config_.video_sender_tuning.ssrc)
              .set_seq(seq)
              .set_timestamp(rtp_ts)
              .set_payload_type(pt)
              .set_marker(marker);

            // ---- Cross-end timeline: stamp abs-send-time (ADR-017) --------
            // Same contract as the audio path: only when the seam is
            // on *and* the extension id was negotiated (the id is set
            // by create_offer() / set_remote_description()).  The
            // monotonic reading is taken once and serves both the
            // abs-send-time encoder and the Sender Report's
            // instant-vs-timestamp pairing.
            if (config_.timeline_enabled) {
                const std::int64_t steady_us =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        core::SteadyClock::now().time_since_epoch()).count();
                impl_->video_state.video_last_send_unix_us =
                    impl_->timeline.boot_epoch_offset_us + steady_us;
                if (impl_->timeline.local_abs_send_time_id != 0) {
                    const std::uint32_t ast24 = rtp::encode_abs_send_time_24(
                        std::chrono::microseconds{steady_us},
                        impl_->timeline.monotonic_origin_ntp64);
                    pb.set_abs_send_time(rtp::AbsSendTime{ast24},
                                         impl_->timeline.local_abs_send_time_id);
                }
            }

            pb.set_payload(core::ByteSpan(payload.data(), payload.size()));

            // ---- Serialise into the reusable scratch (Phase 3 / C4) ------
            //
            // Same reasoning as the audio path: `pb.build()` would
            // allocate a fresh buffer per video packet.  The scratch is
            // read immediately below — the bytes are copied into the
            // scheduler's pooled outbound buffer — so it never escapes
            // this callback.
            const std::size_t built_size = pb.compute_size();
            if (impl_->scratch.video_rtp_scratch.size() < built_size) {
                impl_->scratch.video_rtp_scratch.resize(built_size);
            }
            const std::size_t built_written =
                pb.build_into(impl_->scratch.video_rtp_scratch.data(),
                              impl_->scratch.video_rtp_scratch.size());
            if (built_written == 0) {
                if (on_error_) {
                    on_error_(core::kVideoEncodeFailed,
                              "video send: the RTP packet could not be built");
                }
                return;
            }

            // ---- Assemble full RTP packet ----------------------------------
            // The buffer comes from the scheduler's packet pool (P1
            // hardening, 2026-11-09): it is handed back to the pool
            // once the packet has been drained and dispatched, so the
            // steady-state send path performs no heap allocation.
            //
            // Phase 3: when there is NO scheduler (the "transport"
            // profile, where the engine writes straight to ICE) the
            // packet used to be built in a fresh local vector — one
            // heap allocation per packet, forever.  The transport
            // `send()` is a synchronous write (the bytes do not
            // escape the call), so the engine's own scratch buffer
            // can hold it.
            std::vector<std::uint8_t>& rtp_pkt = scheduler_
                ? impl_->scratch.video_pkt_pooled
                : impl_->scratch.video_pkt_scratch;
            if (scheduler_) {
                rtp_pkt = scheduler_->acquire_packet_buffer(
                    built_written + kSrtpAuthTagBytes);
            }
            rtp_pkt.assign(impl_->scratch.video_rtp_scratch.begin(),
                           impl_->scratch.video_rtp_scratch.begin() +
                               static_cast<std::ptrdiff_t>(built_written));

            // ---- SRTP protect (if DTLS is connected) ----------------------
            if (impl_->srtp_installed && impl_->srtp) {
                auto* sess = impl_->srtp->get_session(
                    config_.video_sender_tuning.ssrc, /*outgoing=*/true);
                if (!sess) {
                    // SRTP not ready — drop this packet silently.
                    return;
                }
                core::ByteSpan span(rtp_pkt.data(), rtp_pkt.size());
                auto enc = sess->protect_rtp(span,
                    config_.video_sender_tuning.ssrc, rtp_ts);
                if (!enc) return;
                rtp_pkt.assign(enc.value().data(),
                               enc.value().data() + enc.value().size());
            }

            // ---- Enqueue to scheduler (move, P0#3) or send directly -------
            plugins::Priority prio = plugins::Priority::kVideo;
            if (marker && impl_->video_state.video_keyframe_pending_.load(std::memory_order_acquire)) {
                prio = plugins::Priority::kVideoKeyframe;
                impl_->video_state.video_keyframe_pending_.store(false, std::memory_order_release);
            }

            // v1.1 V1: count emitted RTP packets/bytes (post-protect size).
            ++impl_->video_state.video_packets_sent;
            impl_->video_state.video_bytes_sent += rtp_pkt.size();

            if (scheduler_) {
                scheduler_->enqueue_owned(prio,
                                          std::move(rtp_pkt),
                                          plugins::Addr{});
            } else if (ice_t_) {
                plugins::BufferView bv{rtp_pkt.data(), rtp_pkt.size()};
                ice_t_->send(bv, {});
            }
        });
}

}  // namespace nimrtc::engine
