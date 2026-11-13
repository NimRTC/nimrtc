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

namespace nimrtc::engine {

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

/** Copy one plane row-block from a caller-owned source into a staged buffer.
 *
 *  @param dst        Destination plane base (already offset to the plane).
 *  @param dst_stride Destination row stride in bytes.
 *  @param src        Source plane base.
 *  @param src_stride Source row stride in bytes.
 *  @param rows       Number of rows to copy.
 *  @param row_bytes  Number of bytes to copy per row.
 */
void copy_plane_rows(std::uint8_t* dst, std::int32_t dst_stride,
                     const std::uint8_t* src, std::int32_t src_stride,
                     std::uint32_t rows, std::size_t row_bytes) noexcept {
    if (!dst || !src || rows == 0 || row_bytes == 0) return;

    // The strides are signed because a layout can describe a bottom-up
    // plane (row 0 at the highest address).  Keep the arithmetic signed
    // so that case works, instead of narrowing them to size_t and
    // wrapping a negative stride into a huge offset.
    const std::ptrdiff_t d_step = dst_stride;
    const std::ptrdiff_t s_step = src_stride;
    std::uint8_t* d = dst;
    const std::uint8_t* s = src;
    for (std::uint32_t row = 0; row < rows; ++row) {
        std::memcpy(d, s, row_bytes);
        d += d_step;
        s += s_step;
    }
}

/** Stage a caller-owned `plugins::VideoSourceFrame` into an owned
 *  `video_frame::VideoFrameBuffer` that the codec can read.
 *
 *  Why this exists: the plugin-level `VideoSourceFrame` is a *view* — its
 *  `plane_*` pointers belong to the source and are only valid for the
 *  duration of the callback.  The codec seam, in contrast, takes a
 *  `plugins::VideoFrame` whose `buffer` is a refcounted
 *  `VideoFrameBuffer` that must stay alive across the `encode()` call.
 *  Software codecs can ignore the pixels entirely, but a hardware encoder
 *  (NVENC) reads them: with a null `buffer` it rejects the frame with
 *  `kErrInvalidParam`.  So the engine owns the copy.
 *
 *  The staged buffer always uses the *codec-facing* pixel format (I420 by
 *  default) rather than the source's own format, so a source that emits
 *  NV12 is converted once here instead of forcing every codec to handle
 *  both layouts.
 *
 *  Note on `plane_offset_*`: the plugin producers (e.g. the memory video
 *  source) already resolve the offsets into the `plane_y/u/v` pointers, so
 *  the pointers are treated as absolute plane bases here and the offset
 *  fields are ignored — adding them again would read past the plane.
 *
 *  @param frame  Caller-owned source frame (planes valid for this call).
 *  @param fmt    Target pixel format for the staged buffer.
 *  @param pool   Pool the staged buffer's storage comes from (Phase 3 / C1).
 *  @return The staged buffer, or nullptr when the frame cannot be staged
 *          (no planes, zero dimensions, or an unsupported source format).
 */
std::shared_ptr<video_frame::VideoFrameBuffer> stage_source_frame(
        const plugins::VideoSourceFrame& frame,
        plugins::VideoPixelFormat fmt,
        video_frame::VideoFrameBufferPool& pool) noexcept {
    if (frame.width == 0 || frame.height == 0) return nullptr;
    if (!frame.plane_y) return nullptr;

    // Only I420-family sources can be staged without a colour-space
    // conversion.  Packed formats (BGRA / RGBA) would need a YUV matrix
    // the engine does not own — the caller must convert first.
    const bool source_is_nv12 = frame.format == plugins::VideoPixelFormat::kNV12;
    const bool source_is_planar_420 =
        frame.format == plugins::VideoPixelFormat::kI420
     || frame.format == plugins::VideoPixelFormat::kYV12
     || source_is_nv12
     || frame.format == plugins::VideoPixelFormat::kUnknown;
    if (!source_is_planar_420) return nullptr;

    // The target must be a planar 4:2:0 layout — that is what every
    // hardware encoder consumes today.
    if (fmt != plugins::VideoPixelFormat::kI420
     && fmt != plugins::VideoPixelFormat::kYV12) {
        return nullptr;
    }

    const auto layout = video_frame::compute_layout(fmt, frame.width, frame.height);
    if (layout.buffer_size == 0) return nullptr;

    // Phase 3 / C1: take the storage from the pool instead of allocating
    // a fresh frame-sized block per frame.  At 1080p30 that was a ~3 MB
    // allocation *and* a ~3 MB memset every 33 ms, on top of the copy the
    // staging itself does.
    auto staged = pool.acquire(layout);
    if (!staged) return nullptr;

    const std::uint32_t half_w = (frame.width + 1u) / 2u;
    const std::uint32_t half_h = (frame.height + 1u) / 2u;

    // The source strides default to a tight packing when the producer did
    // not set them (e.g. a hand-built test frame).
    const std::int32_t src_stride_y = frame.stride_y > 0
        ? frame.stride_y : static_cast<std::int32_t>(frame.width);
    // NV12's chroma plane is interleaved, so one row of it spans the full
    // width; a planar 4:2:0 chroma row spans half the width.
    const std::int32_t src_stride_c = frame.stride_u > 0
        ? frame.stride_u
        : static_cast<std::int32_t>(source_is_nv12 ? frame.width : half_w);

    if (source_is_nv12) {
        // NV12 source: one interleaved UV plane.  De-interleave it into the
        // staged I420 U and V planes (this also copies the luma plane).
        if (!frame.plane_u) return nullptr;
        video_frame::convert_nv12_to_i420(
            frame.width, frame.height,
            frame.plane_y, src_stride_y,
            frame.plane_u, src_stride_c,
            staged->plane(0), layout.strides[0],
            staged->plane(1), layout.strides[1],
            staged->plane(2), layout.strides[2]);
        return staged;
    }

    // I420 / YV12 source: three separate planes.
    if (!frame.plane_u || !frame.plane_v) return nullptr;

    // ---- Luma ---------------------------------------------------------------
    copy_plane_rows(staged->plane(0), layout.strides[0],
                    frame.plane_y, src_stride_y,
                    frame.height, frame.width);

    // ---- Chroma -------------------------------------------------------------
    copy_plane_rows(staged->plane(1), layout.strides[1],
                    frame.plane_u, src_stride_c,
                    half_h, half_w);
    copy_plane_rows(staged->plane(2), layout.strides[2],
                    frame.plane_v, src_stride_c,
                    half_h, half_w);
    return staged;
}

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
        (void)pal::make_plugin<&pal::resolve_video_capture>(
            config_.video_capture_name,
            video_capture_,
            plugins::CaptureConfig{},                  // default config
            {.err_open_failed = core::kVideoCaptureOpenFailed,
             .err_missing = core::kVideoCapturePluginMissing,
             .open_fail_mode = pal::PluginFailureMode::Error,
             .missing_mode = pal::PluginFailureMode::Error,
             .on_error = on_error_});
    }

    // ---- Video Sink --------------------------------------------------------
    if (!video_sink_) {
        // PAL Slice 1.5 — replaces the 5-step "factory → create → null-check
        // → open → report" dance with one helper call. The Error/Error
        // policy matches the original on_error_ path.
        (void)pal::make_plugin<&pal::resolve_video_sink>(
            config_.video_sink_name,
            video_sink_,
            plugins::VideoSinkConfig{},                  // default config
            {.err_open_failed = core::kVideoSinkOpenFailed,
             .err_missing = core::kVideoSinkPluginMissing,
             .open_fail_mode = pal::PluginFailureMode::Error,
             .missing_mode = pal::PluginFailureMode::Error,
             .on_error = on_error_});
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

    if (!config_.video_codec_name.empty() && !video_codec_) {
        plugins::VideoCodecConfig cfg{};
        cfg.width = config_.video_width;
        cfg.height = config_.video_height;
        cfg.fps = config_.video_fps;
        cfg.bitrate_bps = 1'000'000;
        cfg.keyframe_interval = 60;
        cfg.pixel_format = plugins::VideoPixelFormat::kI420;
        cfg.codec = detail::codec_kind_of(codec_family);
        cfg.payload_type = config_.video_receiver_tuning.payload_type;
        cfg.name = config_.video_codec_name;
        (void)pal::make_plugin<&pal::resolve_video_codec>(
            config_.video_codec_name,
            video_codec_,
            std::move(cfg),
            {.err_open_failed = core::kVideoCodecOpenFailed,
             .err_missing = core::kVideoCodecPluginMissing,
             .open_fail_mode = pal::PluginFailureMode::Error,
             .missing_mode = pal::PluginFailureMode::Error,
             .on_error = on_error_});
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
        // video_sender packet callback: build RTP header → SRTP protect → enqueue
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

                // ---- Resolve the decoded geometry ------------------------------
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
                if (video_codec_->decode(enc, raw_out, nullptr) != plugins::kOk) {
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
                plugins::VideoSourceFrame sink_frame{};
                sink_frame.width   = w;
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

                const plugins::Status render_rc = video_sink_->render(sink_frame);
                // Phase 4.2 / D2: the frame reached the sink.  This is
                // the "rendered" event — the freeze detector in the
                // stats sink measures the gap between two of these.
                if (render_rc == plugins::kOk) {
                    impl_->video_state.video_stats.on_frame_rendered(
                        static_cast<std::int64_t>(enc.info.capture_ts_us));
                } else {
                    impl_->video_state.video_stats.on_frame_dropped(
                        detail::VideoDropReason::kDecoderError);
                }
            });
    }

    return plugins::kOk;
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
    // The video-quality statistics sink (Phase 4.2 / D2) is *not* a
    // plugin — it has no open()/close().  Resetting it here clears the
    // per-session counters so a re-open() does not report the previous
    // session's freezes and drops.
    impl_->video_state.video_stats.reset();
    // The capture device (remote-desktop plan A1) is a *device*: it may
    // own an OS handle (a DXGI duplication, a PipeWire stream), so it
    // has to be closed explicitly rather than merely dropped.
    close_all(video_capture_);
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
                "send_video: the zero-copy encode failed — falling back "
                "to the CPU staging path");
        }
    } else if (zero_copy_requested && frame_has_gpu && !zero_copy_capable) {
        // The host asked for zero-copy but the codec cannot consume a
        // handle.  Count the missed opportunity so the host can see that
        // the flag alone does not buy anything with this codec.
        ++impl_->video_state.video_zero_copy_fallbacks;
    }

    if (!encoded_ok) {
        // The codec-facing pixel format comes from the codec's own config
        // (I420 for every backend today).
        const auto codec_fmt = video_codec_->config().pixel_format;
        raw.buffer = stage_source_frame(frame, codec_fmt, impl_->scratch.video_frame_pool);
        if (!raw.buffer) {
            // The frame cannot be staged (no planes, zero dimensions, or a
            // packed source format).  Software codecs can still encode a
            // synthetic frame without pixels, so we only *warn* here and let
            // the codec decide — a hardware codec will report the failure
            // through its own error path.
            core::log::Logger::instance().warn(
                "send_video: the source frame could not be staged into a "
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
        const auto enc_rc = video_codec_->encode(raw,
                                                 impl_->scratch.video_enc_buf.data(),
                                                 impl_->scratch.video_enc_buf.capacity(),
                                                 encoded);
        if (enc_rc != plugins::kOk) {
            // Codec may need a larger buffer than reserved (e.g. very high
            // bitrate IDR).  Resize to the codec's expected capacity and retry
            // exactly once to avoid an unbounded loop on a misbehaving codec.
            const std::size_t needed = encoded.payload.size();
            if (needed == 0 || needed > impl_->scratch.video_enc_buf.capacity() * 4) {
                return plugins::kErrInternal;  // give up on pathological sizes
            }
            impl_->scratch.video_enc_buf.reserve(needed);
            if (video_codec_->encode(raw,
                                     impl_->scratch.video_enc_buf.data(),
                                     impl_->scratch.video_enc_buf.capacity(),
                                     encoded) != plugins::kOk) {
                return plugins::kErrInternal;
            }
        }
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
    const auto push_rc = video_sender_->push_frame(encoded, frame.rtp_timestamp);
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
    // Phase 3 / C3: the zero-copy path counters.
    out.video_zero_copy_frames    = impl_->video_state.video_zero_copy_frames;
    out.video_zero_copy_fallbacks = impl_->video_state.video_zero_copy_fallbacks;

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
    return out;
}

}  // namespace nimrtc::engine
