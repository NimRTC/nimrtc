/**
 * @file src/engine/src/engine_media.cpp
 * @brief Engine Media plane (v1.1 R1). Audio modules + send_audio, video plugins + send path, RTCP feedback queues, stats()..
 *
 * v1.1 R1 split of engine.cpp - mechanical move,
 * behaviour unchanged. Shares engine_impl.hpp.
 */
#include "engine_impl.hpp"

namespace nimrtc::engine {


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
        plugins::CodecConfig codec_cfg{};
        codec_cfg.sample_rate_hz = config_.pcm_sample_rate_hz;
        codec_cfg.channels       = config_.pcm_channels;
        codec_cfg.bitrate_bps    = 64000;
        codec_cfg.complexity     = 10;
        codec_cfg.fec_enabled    = true;
        codec_cfg.dtx_enabled    = false;
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
            impl_->audio_rx_disabled = true;
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
            impl_->audio_rx_disabled = true;
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
        rx_cfg.target_delay_ms        = static_cast<std::uint16_t>(
            config_.jb_initial_delay_ms < 0 ? 0 : config_.jb_initial_delay_ms);
        rx_cfg.min_delay_ms           = static_cast<std::uint16_t>(
            config_.jb_min_delay_ms < 0 ? 0 : config_.jb_min_delay_ms);
        rx_cfg.max_delay_ms           = static_cast<std::uint16_t>(
            config_.jb_max_delay_ms < 0 ? 0 : config_.jb_max_delay_ms);
        // The engine advertises `useinbandfec=1` in its own SDP, so the
        // decoder must be told to use in-band FEC as well (spec B-8).
        rx_cfg.enable_fec_recovery    = true;
        rx_cfg.enable_downmix         = true;

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
            impl_->audio_rx_disabled = true;
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

    ++impl_->audio_frames_received;
    impl_->audio_samples_received += samples_per_ch;
    if (frame.concealed)     ++impl_->audio_frames_concealed;
    if (frame.fec_recovered) ++impl_->audio_frames_fec_recovered;

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


void NimRTCEngine::init_video_plugins() noexcept {
    // PHASE 1 — create all plugins (order: sink, source, receiver, sender,
    // codec). The 5-step factory dance for each plugin is consolidated
    // into pal::make_plugin() (PAL Slice 1.5); see make_plugin.hpp.
    // PHASE 2 — wire pipeline callbacks (down below the make_plugin
    // blocks).  Splitting the function into two phases ensures that by
    // the time any callback fires, every plugin slot is already filled.

    // ---- Our RTCP feedback SSRC (v1.1 Q3) ---------------------------------
    //
    // RFC 3550 §6.1 identifies the sender of an RTCP packet by its SSRC.
    // The feedback we emit (NACK / PLI) is sent by *us* as the receiver of
    // the media, so it needs an SSRC of its own — distinct from the media
    // SSRCs, which identify the streams rather than the reporter.
    if (impl_->video_feedback_ssrc_ == 0) {
        std::random_device rd;
        std::mt19937_64 mt(rd());
        impl_->video_feedback_ssrc_ = static_cast<std::uint32_t>(mt() & 0xFFFFFFFFu);
        if (impl_->video_feedback_ssrc_ == 0) impl_->video_feedback_ssrc_ = 1;
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
        cfg.width  = 640;
        cfg.height = 480;
        cfg.fps    = 30;
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

    // ---- Video Receiver ----------------------------------------------------
    if (!video_receiver_) {
        plugins::VideoReceiverConfig cfg{};
        cfg.codec        = plugins::VideoCodecKind::kH264;
        cfg.ssrc         = config_.video_receiver_tuning.ssrc;
        cfg.payload_type = config_.video_receiver_tuning.payload_type;
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
        cfg.codec        = plugins::VideoCodecKind::kH264;
        cfg.ssrc         = config_.video_sender_tuning.ssrc;
        cfg.payload_type = config_.video_sender_tuning.payload_type;
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

    // ---- Video Codec (H.264) -----------------------------------------------
    if (!config_.video_codec_name.empty() && !video_codec_) {
        plugins::VideoCodecConfig cfg{};
        cfg.width = 640;
        cfg.height = 480;
        cfg.fps = 30;
        cfg.bitrate_bps = 1'000'000;
        cfg.keyframe_interval = 60;
        cfg.pixel_format = plugins::VideoPixelFormat::kI420;
        cfg.codec = plugins::VideoCodecKind::kH264;
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

    // ========================================================================
    // PHASE 2 — wire all pipeline callbacks (all plugins now exist)
    // ========================================================================

    // ---- Video send pipeline: source → codec → sender → RTP+SRTP → scheduler
    //
    // Tracking state: the codec's force_keyframe() causes the NEXT encoded
    // frame to be a keyframe. We track it in impl_->video_keyframe_pending_
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
                // ---- Build 12-byte RTP header ---------------------------------
                // byte 0: V=2 P=0 X=0 CC=0 → 0x80
                // byte 1: M=X(1) PT(7)
                std::uint8_t pt = config_.video_sender_tuning.payload_type;
                std::uint8_t hdr[12] = {
                    0x80,                             // version, no extensions
                    static_cast<std::uint8_t>((marker ? 0x80 : 0x00) | (pt & 0x7F)),
                    static_cast<std::uint8_t>((seq >> 8) & 0xFF),
                    static_cast<std::uint8_t>(seq & 0xFF),
                    static_cast<std::uint8_t>((rtp_ts >> 24) & 0xFF),
                    static_cast<std::uint8_t>((rtp_ts >> 16) & 0xFF),
                    static_cast<std::uint8_t>((rtp_ts >>  8) & 0xFF),
                    static_cast<std::uint8_t>(rtp_ts & 0xFF),
                    static_cast<std::uint8_t>((config_.video_sender_tuning.ssrc >> 24) & 0xFF),
                    static_cast<std::uint8_t>((config_.video_sender_tuning.ssrc >> 16) & 0xFF),
                    static_cast<std::uint8_t>((config_.video_sender_tuning.ssrc >>  8) & 0xFF),
                    static_cast<std::uint8_t>(config_.video_sender_tuning.ssrc & 0xFF),
                };

                // ---- Assemble full RTP packet ----------------------------------
                std::vector<std::uint8_t> rtp_pkt;
                rtp_pkt.reserve(12 + payload.size());
                rtp_pkt.insert(rtp_pkt.end(), hdr, hdr + 12);
                rtp_pkt.insert(rtp_pkt.end(),
                               payload.data(), payload.data() + payload.size());

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
                if (marker && impl_->video_keyframe_pending_.load(std::memory_order_acquire)) {
                    prio = plugins::Priority::kVideoKeyframe;
                    impl_->video_keyframe_pending_.store(false, std::memory_order_release);
                }

                // v1.1 V1: count emitted RTP packets/bytes (post-protect size).
                ++impl_->video_packets_sent;
                impl_->video_bytes_sent += rtp_pkt.size();

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
                ++impl_->video_frames_received;
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

                const std::uint32_t w = enc.payload.size() > 0 ? 640 : 0;
                const std::uint32_t h = 480;

                // Lazily allocate / resize decode buffers to the frame size.
                // Stride is aligned to 16 bytes.
                auto align16 = [](std::uint32_t v) -> std::uint32_t {
                    return (v + 15u) & ~15u;
                };
                const std::uint32_t stride_y = align16(w);
                const std::uint32_t stride_uv = align16(w / 2);
                const std::size_t y_size = static_cast<std::size_t>(stride_y) * h;
                const std::size_t uv_size = static_cast<std::size_t>(stride_uv) * (h / 2);
                const std::size_t total = y_size + uv_size + uv_size;

                // Use a ping-pong buffer: alternate between two allocations.
                std::size_t buf_idx = impl_->video_decode_buf_idx_.fetch_add(1)
                                      % 2;
                auto& buf = impl_->video_decode_bufs[buf_idx];
                if (buf.size() < total) buf.resize(total);

                // Set plane pointers into the flat buffer.
                std::uint8_t* planes[3] = {
                    buf.data(),
                    buf.data() + y_size,
                    buf.data() + y_size + uv_size,
                };

                // Decode.
                plugins::VideoFrame raw_out{};
                raw_out.info.capture_ts_us = enc.info.capture_ts_us;
                raw_out.info.frame_seq     = enc.info.frame_seq;
                raw_out.info.rtp_timestamp = enc.info.rtp_timestamp;

                if (video_codec_->decode(enc, raw_out, planes) != plugins::kOk) {
                    return;
                }

                // Render to sink.
                plugins::VideoSourceFrame sink_frame{};
                sink_frame.width   = w;
                sink_frame.height  = h;
                sink_frame.format  = plugins::VideoPixelFormat::kI420;
                sink_frame.stride_y = static_cast<std::int32_t>(stride_y);
                sink_frame.stride_u = static_cast<std::int32_t>(stride_uv);
                sink_frame.stride_v = static_cast<std::int32_t>(stride_uv);
                sink_frame.plane_y  = planes[0];
                sink_frame.plane_u  = planes[1];
                sink_frame.plane_v  = planes[2];
                sink_frame.buffer_size = total;
                sink_frame.capture_ts_us = enc.info.capture_ts_us;
                sink_frame.frame_seq     = enc.info.frame_seq;
                sink_frame.rtp_timestamp = enc.info.rtp_timestamp;

                video_sink_->render(sink_frame);
            });
    }
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
}


uint32_t NimRTCEngine::send_audio(const float* pcm_samples, std::size_t num_samples) noexcept {
    if (!is_open()) return core::kEngineNotReady;
    if (!pcm_samples || num_samples == 0) return core::kEngineInvalidParam;
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
    if (impl_->audio_pcm_buf.size() < total_floats) {
        impl_->audio_pcm_buf.resize(total_floats);
    }
    std::memcpy(impl_->audio_pcm_buf.data(), pcm_samples,
                total_floats * sizeof(float));
    float* pcm = impl_->audio_pcm_buf.data();

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
        impl_->audio_encode_errors++;
        if (on_error_) {
            on_error_(core::kAudioEncodeFailed,
                      "send_audio: the configured audio frame size is zero");
        }
        return core::kEngineInvalidParam;
    }

    // ---- Accumulate the processed PCM (B-6) ----------------------------
    //
    // send_audio() accepts any block size.  The processed PCM is
    // appended to a per-engine accumulator and only whole frames are
    // encoded, so the wire format does not depend on how the host
    // chunks its capture.  The residual stays in the accumulator across
    // calls; a partial frame is never encoded.
    impl_->audio_accum.insert(impl_->audio_accum.end(), pcm, pcm + total_floats);
    impl_->audio_samples_in += num_samples;

    // Errors are reported at a throttled rate: a host that feeds the
    // engine PCM the encoder cannot take would otherwise get one
    // callback per frame.
    auto report_error = [&](std::uint32_t code, const char* msg) {
        ++impl_->audio_encode_errors;
        const auto now = std::chrono::steady_clock::now();
        if (impl_->audio_last_error_report ==
                std::chrono::steady_clock::time_point{} ||
            now - impl_->audio_last_error_report >= std::chrono::seconds(1)) {
            impl_->audio_last_error_report = now;
            if (on_error_) on_error_(code, msg);
        }
    };

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
        if (impl_->audio_codec_pkt.size() < needed) {
            impl_->audio_codec_pkt.resize(needed);
        }
        std::size_t codec_len = 0;
        if (codec_plugin_) {
            codec_len = codec_plugin_->encode(frame_pcm, frame_samples,
                                             impl_->audio_codec_pkt.data(),
                                             impl_->audio_codec_pkt.size());
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
            ++impl_->audio_seq;                       // gap in the sequence
            impl_->audio_ts += static_cast<std::uint32_t>(frame_samples);
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
            impl_->negotiated_audio_pt != 0
                ? impl_->negotiated_audio_pt
                : config_.audio_codec.payload_type;

        rtp::PacketBuilder pb;
        pb.set_ssrc(impl_->audio_ssrc)
          .set_seq(impl_->audio_seq++)
          .set_timestamp(impl_->audio_ts)
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
        impl_->audio_last_send_unix_us = impl_->boot_epoch_offset_us + steady_us;
        if (impl_->local_abs_send_time_id != 0) {
            const std::uint32_t ast24 = rtp::encode_abs_send_time_24(
                std::chrono::microseconds{steady_us},
                impl_->monotonic_origin_ntp64);
            pb.set_abs_send_time(rtp::AbsSendTime{ast24},
                                 impl_->local_abs_send_time_id);
        }
    }

        pb.set_payload(core::ByteSpan(impl_->audio_codec_pkt.data(), codec_len));
        auto pkt_buf = pb.build();
        if (pkt_buf.empty()) {
            report_error(core::kAudioEncodeFailed,
                         "send_audio: the RTP packet could not be built");
            impl_->audio_ts += static_cast<std::uint32_t>(frame_samples);
            return 0;
        }

        // Count for the Sender Report body (RFC 3550 §6.4.1 sender info). The
        // RTP timestamp is sampled *before* SRTP so the SR and the media packet
        // it anchors describe the same media position; the octet count is the
        // payload length, which is what the peer subtracts to check.
        const std::uint32_t cur_ts = impl_->audio_ts;
        ++impl_->audio_packet_count;
        impl_->audio_octet_count += static_cast<std::uint32_t>(codec_len);
        impl_->audio_rtp_timestamp = cur_ts;
        ++impl_->audio_frames_sent;
        impl_->audio_bytes_sent += pkt_buf.size();

        // The media clock advances by the frame duration whether or not the
        // packet reaches the wire (A-7): a dropped frame leaves a gap in
        // the sequence number, never a gap in the timeline.
        impl_->audio_ts += static_cast<std::uint32_t>(frame_samples);

        // SRTP protect (or pass-through when not yet keyed).
        // Reuse Impl::audio_send_buf (P0#4) — one allocation ever, then in-place.
        core::ByteSpan out_payload(pkt_buf.data(), pkt_buf.size());
        if (impl_->srtp_installed && impl_->srtp) {
            auto* sess = impl_->srtp->get_session(impl_->audio_ssrc,
                                                   /*outgoing=*/true);
            if (!sess) {
                // SRTP installed but no session for this SSRC — drop.
                return 0;
            }
            auto enc = sess->protect_rtp(out_payload, impl_->audio_ssrc, cur_ts);
            if (!enc) return 0;
            impl_->audio_send_buf.assign(enc.value().data(),
                                         enc.value().data() + enc.value().size());
        } else {
            impl_->audio_send_buf.assign(out_payload.data(),
                                         out_payload.data() + out_payload.size());
        }

        // ---- Scheduler: enqueue for priority-drain (move, P0#3) --------------
        // The scheduler takes ownership of audio_send_buf via enqueue_owned.
        // After this call, audio_send_buf is in MOVED-FROM state; we must not
        // touch it.  The scheduler will release it on drain.
        if (scheduler_) {
            scheduler_->enqueue_owned(plugins::Priority::kAudio,
                                      std::move(impl_->audio_send_buf),
                                      plugins::Addr{});
            return 0;
        }

        // Fallback: no scheduler — send directly through ICE (transport profile).
        plugins::BufferView bv{impl_->audio_send_buf.data(),
                               impl_->audio_send_buf.size()};
        const auto send_rc = ice_t_->send(bv, {});
        // Clear so the next call starts fresh.
        impl_->audio_send_buf.clear();
        impl_->audio_send_buf.shrink_to_fit();   // release backing if needed
        return send_rc;
    };

    // ---- Drain the accumulator -----------------------------------------
    //
    // Every whole frame the accumulator holds becomes exactly one RTP
    // packet.  A partial frame stays behind for the next call.
    std::uint32_t rc = 0;
    std::size_t emitted_frames = 0;
    std::size_t available = impl_->audio_accum.size() / channels;
    while (available >= frame_samples) {
        const float* frame_pcm = impl_->audio_accum.data() +
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
        impl_->audio_accum.erase(
            impl_->audio_accum.begin(),
            impl_->audio_accum.begin() +
                static_cast<std::ptrdiff_t>(emitted_frames * channels));
    }
    impl_->audio_accum_frames = impl_->audio_accum.size() / channels;
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
    // P0: only the metadata is propagated; the actual pixel storage is
    // owned by `frame.plane_y/u/v` and must outlive the encode call.
    plugins::VideoFrame raw{};
    raw.info.capture_ts_us = frame.capture_ts_us;
    raw.info.frame_seq     = frame.frame_seq;
    raw.info.rtp_timestamp = frame.rtp_timestamp;

    // Encode — reuse Impl::video_enc_buf (P0#4).  Reserve a typical IDR
    // size on first use; subsequent calls within the same capacity reuse
    // the storage and only pay the per-byte conversion cost inside the
    // codec.  H.264 frames can spike above 64 KiB (high-bitrate 1080p60)
    // — when that happens we resize and pay one reallocation.
    constexpr std::size_t kInitialVideoEncCap = 64 * 1024;
    if (impl_->video_enc_buf.capacity() < kInitialVideoEncCap) {
        impl_->video_enc_buf.reserve(kInitialVideoEncCap);
    }
    plugins::EncodedVideoFrame encoded{};
    const auto enc_rc = video_codec_->encode(raw,
                                             impl_->video_enc_buf.data(),
                                             impl_->video_enc_buf.capacity(),
                                             encoded);
    if (enc_rc != plugins::kOk) {
        // Codec may need a larger buffer than reserved (e.g. very high
        // bitrate IDR).  Resize to the codec's expected capacity and retry
        // exactly once to avoid an unbounded loop on a misbehaving codec.
        const std::size_t needed = encoded.payload.size();
        if (needed == 0 || needed > impl_->video_enc_buf.capacity() * 4) {
            return plugins::kErrInternal;  // give up on pathological sizes
        }
        impl_->video_enc_buf.reserve(needed);
        if (video_codec_->encode(raw,
                                 impl_->video_enc_buf.data(),
                                 impl_->video_enc_buf.capacity(),
                                 encoded) != plugins::kOk) {
            return plugins::kErrInternal;
        }
    }

    // Mark keyframe so the sender packet callback gives it kVideoKeyframe
    // priority (consumed on the marker=true packet).
    if (encoded.is_keyframe) {
        impl_->video_keyframe_pending_.store(true, std::memory_order_release);
    }

    // Packetize (the sender's packet callback handles RTP header assembly
    // + SRTP protect + scheduler enqueue).
    const auto push_rc = video_sender_->push_frame(encoded, frame.rtp_timestamp);
    if (push_rc == plugins::kOk) {
        // v1.1 V1: one encoded frame accepted by the sender pipeline.
        ++impl_->video_frames_sent;
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
            impl_->video_feedback_applied += static_cast<std::uint64_t>(actions);
        }
        const std::uint32_t keyframe_requests =
            video_sender_->take_keyframe_requests();
        if (keyframe_requests > 0) {
            impl_->video_keyframes_applied += keyframe_requests;
            if (video_codec_) {
                (void)video_codec_->force_keyframe();
            }
        }
    }

    if (!config_.timeline_enabled) {
        // The engine MUST NOT schedule SRs while the flag is false
        // (ADR-017 §"Decision 5"), and symmetrically there is no anchor to
        // maintain — an inbound SR is of no use to a disabled seam.
        return;
    }

    // Compound RTCP packets are a concatenation of sub-packets sharing one
    // header (RFC 3550 §6.1). Walk them so an SR that follows an RR in the
    // same compound datagram is not missed. A malformed length field ends
    // the walk rather than throwing — the rest of the datagram is
    // unparseable, but that is not an error worth failing the read path for.
    std::size_t off = 0;
    while (off + 4 <= view.size()) {
        const std::uint8_t count = static_cast<std::uint8_t>(view[off] & 0x1F);
        const std::uint32_t words = static_cast<std::uint32_t>(view[off + 2]) << 8 |
                                    static_cast<std::uint32_t>(view[off + 3]);
        const std::size_t sub_len = (static_cast<std::size_t>(words) + 1u) * 4u;
        if (sub_len < 4u || off + sub_len > view.size()) {
            break;
        }

        const std::uint8_t ptype = view[off + 1];
        if (ptype == 200) {  // SR
            rtp::Parser rp;
            auto sr = rp.parse_sr(view.subspan(off, sub_len));
            if (sr) {
                // The tracker wants our *monotonic* clock reading at
                // receipt: it combines it with the monotonic origin to
                // place our own clock on the peer's wall-clock timeline
                // (ADR-017 §"Decision 4").  Passing a wall-clock reading
                // here would double-count the epoch and make the skew
                // meaningless.
                impl_->sr_tracker.on_sender_report(sr.value(),
                                                  detail::steady_now_us());
            }
        }
        (void)count;  // report-block count is parsed inside parse_sr()

        off += sub_len;
    }
}


void NimRTCEngine::queue_video_feedback(
    const plugins::VideoFeedbackReport& report) noexcept {
    if (report.missing_seqs.empty() && !report.want_keyframe) return;

    // v1.1 V1: outbound feedback queued (NACK seqs + keyframe requests).
    impl_->video_nack_seqs_queued += report.missing_seqs.size();
    if (report.want_keyframe) ++impl_->video_keyframes_q;

    // Merge into the pending report: two gaps detected back to back must
    // not cost two round trips, and a NACK list plus a key-frame request
    // belong in the same feedback packet anyway.
    if (!impl_->has_pending_video_feedback_) {
        impl_->pending_video_feedback_ = report;
        impl_->has_pending_video_feedback_ = true;
        return;
    }
    impl_->pending_video_feedback_.missing_seqs.insert(
        impl_->pending_video_feedback_.missing_seqs.end(),
        report.missing_seqs.begin(), report.missing_seqs.end());
    impl_->pending_video_feedback_.want_keyframe =
        impl_->pending_video_feedback_.want_keyframe || report.want_keyframe;
}


void NimRTCEngine::flush_video_feedback() noexcept {
    if (!impl_->has_pending_video_feedback_) return;
    // Take the report out of the queue *before* sending: a send failure
    // must not make the engine retry the same report forever.
    const plugins::VideoFeedbackReport report =
        impl_->pending_video_feedback_;
    impl_->pending_video_feedback_ = plugins::VideoFeedbackReport{};
    impl_->has_pending_video_feedback_ = false;

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
        impl_->video_feedback_ssrc_,   // we are the feedback sender
        impl_->remote_video_ssrc_,     // the peer's video stream is the media source
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


NimRTCEngine::Stats NimRTCEngine::stats() const noexcept {
    Stats out;
    out.srtp_drops = impl_->srtp_stats_drop;
    // v1.1 V1: cumulative video counters (additive; zero when unused).
    out.video_frames_sent       = impl_->video_frames_sent;
    out.video_packets_sent      = impl_->video_packets_sent;
    out.video_bytes_sent        = impl_->video_bytes_sent;
    out.video_frames_received   = impl_->video_frames_received;
    out.video_nack_seqs_queued  = impl_->video_nack_seqs_queued;
    out.video_keyframes_q       = impl_->video_keyframes_q;
    out.video_feedback_applied  = impl_->video_feedback_applied;
    out.video_keyframes_applied = impl_->video_keyframes_applied;
    // WP-5: the audio send counters.  `audio_encode_errors` is the one
    // that matters to a host: it counts the PCM that was accepted and
    // then could not be encoded, which used to disappear silently.
    out.audio_frames_sent     = impl_->audio_frames_sent;
    out.audio_bytes_sent      = impl_->audio_bytes_sent;
    out.audio_encode_errors   = impl_->audio_encode_errors;
    out.audio_samples_in      = impl_->audio_samples_in;
    out.audio_samples_pending = impl_->audio_accum_frames;
    return out;
}

}  // namespace nimrtc::engine
