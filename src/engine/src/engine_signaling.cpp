/**
 * @file src/engine/src/engine_signaling.cpp
 * @brief Engine Signaling plane (v1.1 R1). create_offer / process_remote_sdp..
 *
 * v1.1 R1 split of engine.cpp - mechanical move,
 * behaviour unchanged. Shares engine_impl.hpp.
 */
#include "engine_impl.hpp"

namespace nimrtc::engine {

namespace {

/** Can we decode a stream whose RTP encoding name is @p encoding?
 *
 *  The audio pipeline's decoder seam is the registry's *audio-decoder*
 *  slot (`plugins::IAudioDecoderFactory`), and the ids there are the
 *  lower-cased RTP encoding names — the Opus decoder registers under
 *  "opus" (B-15).  A host that implements its own codec registers it
 *  under the encoding it implements, so the lookup answers the question
 *  the negotiation actually asks: "do we have a decoder for this?".
 *
 *  @param encoding  The lower-cased RTP encoding name.
 *  @return          true when a decoder plugin exists for it. */
bool audio_encoding_decodable(const std::string& encoding) {
    if (encoding.empty()) return false;
    return core::PluginRegistry::instance().get_audio_decoder(encoding) != nullptr;
}

} // namespace


bool NimRTCEngine::wants_transport_cc() const noexcept {
    // A collector is what turns transport-cc feedback into an estimate,
    // so without one there is nothing to negotiate for.  A host that
    // pinned the policy to the loss/RTT path does not want the feedback
    // either.
    if (config_.net_feedback_name.empty()) return false;
    return detail::parse_net_feedback_policy(config_.net_feedback_signal) !=
           plugins::NetFeedbackPolicy::kLossRtt;
}

std::string NimRTCEngine::create_offer() noexcept {
    if (ice_t_) ice_t_->wait_for_gathering(1500);

    sdp::SessionDescription sdp;
    sdp.version = 0;
    sdp.origin_username         = "-";
    sdp.origin_session_id      = detail::make_session_id();
    sdp.origin_session_version = "1";
    sdp.origin_address          = "0.0.0.0";
    sdp.session_name            = "-";
    sdp.bundle_mids = {"0"};
    sdp.msid_semantic_token = "WMS";

    // Remember the session id so `process_remote_sdp()` can recognise the
    // peer's answer (RFC 3264 §5.1: the answer echoes the offer's session
    // id).  Without it the offerer would treat the answer as a fresh offer
    // and keep its default DTLS role.
    impl_->local_session_id = sdp.origin_session_id;

    // Helper: append ICE candidates stripped of any prefix the libjuice
    // collector added (the munger re-emits "a=candidate:" itself).
    auto push_candidates_into = [&](sdp::MediaDescription& m) {
        if (!ice_t_) return;
        auto gathered = ice_t_->gathered_local_candidates();
        constexpr std::string_view kFullPrefix = "a=candidate:";
        constexpr std::string_view kHalfPrefix = "candidate:";
        for (auto& c : gathered) {
            if (c.size() >= kFullPrefix.size() &&
                c.substr(0, kFullPrefix.size()) == kFullPrefix) {
                m.candidates.push_back(std::string(c.substr(kFullPrefix.size())));
            } else if (c.size() >= kHalfPrefix.size() &&
                       c.substr(0, kHalfPrefix.size()) == kHalfPrefix) {
                m.candidates.push_back(std::string(c.substr(kHalfPrefix.size())));
            } else {
                m.candidates.push_back(c);
            }
        }
    };

    // ---- Audio m-line builder (RFC 8829 §5, Opus via RFC 7587) ---------
    //
    // The audio m-line is built by a lambda so the media plan can emit
    // one m-line per audio transceiver, each with its own mid and
    // direction (N3 / T2).  The *content* of the m-line is the same for
    // every audio transceiver — the payload type, the codec parameters
    // and the ICE / DTLS credentials are per-session, not per-m-line.
    auto build_audio_media = [&](const std::string& mid,
                                 sdp::Direction dir) -> sdp::MediaDescription {
            sdp::MediaDescription audio;
    audio.type            = sdp::MediaType::Audio;
    audio.port            = 9;
    audio.protocol        = "UDP/TLS/RTP/SAVPF";
    // Build the format string key from the configurable payload type.
    const std::string pt = std::to_string(config_.audio_codec.payload_type);
    audio.formats         = {pt};
    audio.direction       = dir;
    audio.mid             = mid;
    audio.rtcp_mux_value  = "rtcp-mux";
    audio.ice_ufrag       = ice_t_ ? ice_t_->local_ufrag() : "";
    audio.ice_pwd         = ice_t_ ? ice_t_->local_password() : "";
    audio.ice_options     = "trickle";
    audio.dtls_setup      = "actpass";

    if (auto d = impl_->dtls.lock()) {
        audio.dtls_fingerprint_algo  = "sha-256";
        audio.dtls_fingerprint_value = d->local_fingerprint().hex_colon;
    }

    sdp::MediaDescription::RtpMap codec_map;
    codec_map.encoding   = config_.audio_codec.encoding;
    codec_map.clock_rate = config_.audio_codec.clock_rate;
    codec_map.channels   = config_.audio_codec.channels;
    audio.rtpmap[pt]     = codec_map;

    // a=fmtp:<pt> ... (B-14)
    //
    // The parameters must describe what NimRTC *actually does*, not
    // what a peer might like: the decoder recovers a lost frame from
    // the redundancy the sender embeds in the next packet (B-8), so
    // in-band FEC has to be signalled or the peer will not send it.
    //
    // A host that configured an explicit `fmtp` string keeps it — the
    // configuration is the more specific instruction.  Otherwise the
    // Opus defaults are derived from what the engine really sends:
    // the smallest Opus frame as `minptime`, in-band FEC on, and the
    // channel count of the *stream* (`pcm_channels`), which is not the
    // channel count of the encoding (Opus always supports two).
    if (!config_.audio_codec.fmtp.empty()) {
        audio.fmtp[pt] = config_.audio_codec.fmtp;
    } else if (detail::decoder_id_for_encoding(config_.audio_codec.encoding) == "opus") {
        audio.fmtp[pt] = detail::build_local_opus_fmtp(config_.pcm_channels);
    }

    // a=ssrc:<audio_ssrc> cname:nimrtc-audio (RFC 5576 section 4.1).
    // Chrome only populates `inbound-rtp` (and fires `ontrack` audio
    // playout stats) when the offer SDP carries an a=ssrc line whose
    // value matches the RTP-header SSRC on the wire.  The wire value is
    // `Impl::audio_ssrc` — the same per-engine member the packet path
    // stamps — so the two cannot drift apart.  The value used to be the
    // constant 0xDEADBEEF, which two engines in one process shared.
    {
        char ssrc_line[96];
        std::snprintf(ssrc_line, sizeof(ssrc_line),
                      "%u cname:nimrtc-audio",
                      static_cast<unsigned>(impl_->audio_ssrc));
        audio.extra_attrs.emplace_back("ssrc", ssrc_line);
    }

    // a=ptime / a=maxptime (RFC 7587 §6.1, RFC 8866).
    //
    // The advertised ptime is the duration of one outbound audio packet
    // — the frame size the encoder works with.  maxptime is the largest
    // packet the peer may send us; the engine accepts up to one frame
    // per packet, so it mirrors ptime here.
    {
        audio.extra_attrs.emplace_back("ptime",
                                       std::to_string(config_.audio_ptime_ms));
        audio.extra_attrs.emplace_back("maxptime",
                                       std::to_string(config_.audio_ptime_ms));
    }

    // ---- a=rtcp-fb:<pt> transport-cc (RFC 4585 §4.1) --------------------
    //
    // transport-cc is the receiver-side congestion signal ADR-018
    // prefers: per-packet arrival times, so it survives reordering and
    // exposes queueing delay before loss does.  It has to be
    // *negotiated* — a peer sends transport-cc feedback only when the
    // negotiated description carries the attribute for that m-section —
    // so not advertising it means never getting it.
    //
    // ADR-018 Decision 6: the offer carries it when the host has a
    // transport-cc-capable collector configured.  A host that pinned
    // the policy to the loss/RTT path does not advertise it at all.
    if (wants_transport_cc()) {
        audio.rtcp_fb.push_back(pt + " transport-cc");
    }

    // ---- Cross-end timeline extmap (ADR-017) ------------------------------
    //
    // a=extmap:4 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time
    //
    // RFC 5285 §6 negotiates the id *per direction*: the id a sender uses on
    // the wire is the one it advertised itself, and it is the receiver's job to
    // echo it back.  So advertising `kAbsSendTimeDefaultLocalId` here means:
    // "packets I send carry abs-send-time under id 4".  The peer's answer
    // carries its own id, which is what we parse *their* packets with.
    //
    // Direction is sendrecv (not sendonly) because both ends of a NimRTC
    // session are symmetric here — the remote-answer path resolves the peer
    // id separately (see process_remote_sdp).  The id is deliberately left at
    // the well-known 4 rather than randomised: Chrome's WebRTC stack has used
    // 4 for abs-send-time for years and a mismatch here is a silent
    // "extension present but wrong id" bug on the peer.
    if (config_.timeline_enabled) {
        sdp::MediaDescription::ExtMap ast;
        ast.id        = static_cast<int>(rtp::kAbsSendTimeDefaultLocalId);
        ast.direction = sdp::Direction::SendRecv;
        ast.uri       = std::string(rtp::kAbsSendTimeUri);
        audio.extmap.push_back(ast);
        impl_->local_abs_send_time_id = rtp::kAbsSendTimeDefaultLocalId;
    }

    // a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level
    //
    // RFC 6464 defines the extension; RFC 5285 §6 negotiates its id
    // per direction.  Chrome reports `audioLevel` in `inbound-rtp`
    // only when the description carries this extension (F-22), so the
    // offer has to advertise it.
    //
    // Id 1 is the id Chrome has used for this extension for years, and
    // it does not collide with the abs-send-time id (4) that shares
    // the same m-line.  The id we advertise is also the id our own
    // outbound packets will carry (see `local_audio_level_ext_id`).
    {
        sdp::MediaDescription::ExtMap al;
        al.id        = detail::kAudioLevelDefaultLocalId;
        al.direction = sdp::Direction::SendRecv;
        al.uri       = std::string(detail::kAudioLevelExtUri);
        audio.extmap.push_back(al);
        impl_->local_audio_level_ext_id = detail::kAudioLevelDefaultLocalId;
    }

    push_candidates_into(audio);

    return audio;
    };

    // ---- Video m-line (RFC 8829 §5, H.264 via RFC 6184) -----------------
    // Emit a video m-line only when at least one video plugin was resolved
    // (video_receiver_ / video_sender_ / video_codec_).  The default H.264
    // codec plugin is decoder-only — for encode we'd need a real codec
    // plugin — but the SDP/SDP-answer plumbing is correct regardless so
    // that browsers see the negotiation succeed even on a one-way demo.
    //
    // The builder is a lambda so the media plan can emit one m-line per
    // video transceiver, each with its own mid and direction (N3 / T2).
    auto build_video_media = [&](const std::string& mid,
                                 sdp::Direction dir) -> sdp::MediaDescription {
        sdp::MediaDescription video;
        video.type     = sdp::MediaType::Video;
        video.port     = 9;
        video.protocol = "UDP/TLS/RTP/SAVPF";
        // The payload type the offer names is the one our outbound
        // packets carry (the sender's tuning), because a `sendrecv`
        // m-section uses one payload type for both directions.  Using
        // the receiver's tuning here would advertise a number the
        // packet path never stamps (C9 / C11).
        const std::string vpt =
            std::to_string(config_.video_sender_tuning.payload_type);
        video.formats         = {vpt};
        video.direction       = dir;
        video.mid             = mid;
        video.rtcp_mux_value  = "rtcp-mux";
        video.ice_ufrag       = ice_t_ ? ice_t_->local_ufrag() : "";
        video.ice_pwd         = ice_t_ ? ice_t_->local_password() : "";
        video.ice_options     = "trickle";
        video.dtls_setup      = "actpass";

        if (auto d = impl_->dtls.lock()) {
            video.dtls_fingerprint_algo  = "sha-256";
            video.dtls_fingerprint_value = d->local_fingerprint().hex_colon;
        }

        // a=rtpmap:<vpt> H264/90000  (RFC 6184 §5.6)
        sdp::MediaDescription::RtpMap video_map;
        video_map.encoding   = "H264";
        video_map.clock_rate = 90000;
        video_map.channels   = 1;             // H.264 clock is 90 kHz, no audio channels
        video.rtpmap[vpt]    = video_map;

        // Cross-end timeline extmap (ADR-017).  Same id and rationale as the
        // audio m-line: the ids are negotiated per m-section in RFC 5285 §6,
        // and Chrome uses 4 on both.  Advertising the same id on both keeps a
        // peer that multiplexes by mid (rather than per-m-line) unambiguous.
        if (config_.timeline_enabled) {
            sdp::MediaDescription::ExtMap ast;
            ast.id        = static_cast<int>(rtp::kAbsSendTimeDefaultLocalId);
            ast.direction = sdp::Direction::SendRecv;
            ast.uri       = std::string(rtp::kAbsSendTimeUri);
            video.extmap.push_back(ast);
        }

        // a=fmtp:<vpt> profile-level-id=42E01F;packetization-mode=1;
        //                  sprop-parameter-sets=Z0LAHtkA,aM4G4g==
        // Baseline profile, level 3.1 — matches the synthetic SPS/PPS our
        // H.264 stub encoder emits (see h264::StubEncoder::encode).  When
        // a real codec plugin replaces the stub, the codec should override
        // these via a getter or config (out of scope for this round).
        if (video_codec_) {
            video.fmtp[vpt] =
                "profile-level-id=42E01F;"
                "packetization-mode=1;"
                "level-asymmetry-allowed=1;"
                "sprop-parameter-sets=Z0LAHtkA,aM4G4g==";
        } else {
            video.fmtp[vpt] =
                "profile-level-id=42E01F;packetization-mode=1";
        }

        // a=rtcp-fb:<vpt> nack pli  (RFC 4585 §6.2.1 + WebRTC §5.2)
        // NACK + Picture Loss Indication are the two feedbacks Chrome's
        // SFU expects on a recvonly/sendrecv video track.  We add both
        // unconditionally so the answer side mirrors them verbatim.
        video.rtcp_fb.push_back(vpt + " nack");
        video.rtcp_fb.push_back(vpt + " nack pli");
        video.rtcp_fb.push_back(vpt + " goog-remb");

        // a=rtcp-fb:<vpt> transport-cc (RFC 4585 §4.1, ADR-018
        // Decision 6).  The video stream is the bandwidth hog, so the
        // transport-cc arrival series is the signal that matters most
        // for it — but the attribute has to be negotiated, and the
        // collector is what turns the feedback into an estimate.
        if (wants_transport_cc()) {
            video.rtcp_fb.push_back(vpt + " transport-cc");
        }

        // a=ssrc:<video_ssrc> cname:nimrtc-video  (RFC 5576 §4.1)
        // WebRTC browsers count inbound-rtp only when the answer SDP
        // carries an a=ssrc line that matches the wire SSRC.  Emit it
        // here on the offer side as well so we have it for renegotiation.
        char ssrc_line[96];
        std::snprintf(ssrc_line, sizeof(ssrc_line),
                      "%u cname:nimrtc-video",
                      static_cast<unsigned>(config_.video_sender_tuning.ssrc));
        video.extra_attrs.emplace_back("ssrc", ssrc_line);

        push_candidates_into(video);
        return video;
    };

    // ---- DataChannel m=application line (RFC 8831 / RFC 8260) ----------
    //
    // P2 simplification (RFC 8831 §5 "SCTP over DTLS"):
    //   We emit the canonical `m=application ... UDP/DTLS/SCTP <port>`
    //   line + an `a=sctp-port:<port>` attribute when
    //   `cfg.datachannel_name` is non-empty.  This is the bare minimum
    //   Chrome needs to populate its peer connection's SCTP transport
    //   and run DCEP (Data Channel Establishment Protocol, RFC 8832).
    //
    //   We deliberately do NOT emit `a=dcep` attributes or full DCEP
    //   handshake support — DCEP handshake is a **v1.x WebTransport /
    //   QUIC** re-design (see `docs/plan/v0.11-plan.md` §2.2 / §10).
    //   v0.11.0 emits only the `m=application` line + `a=sctp-port:<port>`,
    //   which is the minimum Chrome needs to populate its peer
    //   connection's SCTP transport.
    //
    //   Chrome tolerates this minimal form: as long as both sides
    //   advertise `m=application` with the same SCTP port and the
    //   DTLS fingerprint matches, the SCTP association is negotiated
    //   in-band once usrsctp starts emitting INIT chunks.
    //
    //   v0.15.0 (WebTransport product route): when
    //   `datachannel_backend == "web_transport"` the DataChannel rides
    //   a dedicated QUIC connection (msquic), not the ICE/DTLS 5-tuple,
    //   so no `m=application` line is emitted.  The backend dispatch
    //   uses `resolved_dc_factory_id_` (authoritative post-open value)
    //   with a pre-open fallback to the raw config, so unit tests that
    //   call create_offer() without open() still observe the split.
    const bool wt_backend =
        (!resolved_dc_factory_id_.empty()
             ? (resolved_dc_factory_id_ == "web_transport")
             : (config_.datachannel_backend == "web_transport"));

    // The application m-line is built by a lambda for the same reason as
    // the other two: the media plan drives it, one m-line per data
    // transceiver (N3 / T2).
    auto build_app_media = [&](const std::string& mid,
                               sdp::Direction dir) -> sdp::MediaDescription {
        sdp::MediaDescription app;
        app.type     = sdp::MediaType::Application;
        app.port     = 9;
        // Per RFC 8260 §6 / RFC 8831 §5: `UDP/DTLS/SCTP` is the
        // canonical protocol token for SCTP-over-DTLS.  The legacy
        // `DTLS/SCTP` token from RFC 8261 is also seen in the wild;
        // emitting both via the protocol string is fine — the parser
        // round-trips whichever the peer picked.
        app.protocol = "UDP/DTLS/SCTP";
        // RFC 8831 §6.1: the format list is empty for SCTP
        // (the payload type is implicit).  We keep it as an empty
        // list rather than a placeholder — the munger emits
        // `m=application 9 UDP/DTLS/SCTP` with no trailing formats.
        app.formats.clear();
        app.direction       = dir;
        // The application mid is allocated by the session model, so it
        // is whatever mid the data transceiver owns (normally "2").
        app.mid             = mid;
        app.rtcp_mux_value  = "";    // rtcp-mux is RTP-specific; not valid for SCTP.
        app.ice_ufrag       = ice_t_ ? ice_t_->local_ufrag() : "";
        app.ice_pwd         = ice_t_ ? ice_t_->local_password() : "";
        app.ice_options     = "trickle";
        app.dtls_setup      = "actpass";

        if (auto d = impl_->dtls.lock()) {
            app.dtls_fingerprint_algo  = "sha-256";
            app.dtls_fingerprint_value = d->local_fingerprint().hex_colon;
        }

        // a=sctp-port:<port>  (RFC 8831 §6.1 — mandatory for SCTP).
        // Using extra_attrs keeps the munger's a= emission order stable
        // (extra_attrs come after rtcp-fb / extmap).
        app.extra_attrs.emplace_back(
            "sctp-port", std::to_string(config_.sctp_port));

        // a=max-message-size:<bytes>  (RFC 8831 §6.2 — optional, but
        // Chrome emits it; matching the peer's value avoids
        // `Bundled SCTP negotiation` warnings in chrome://webrtc-internals).
        // 6556 matches Chrome's default since M85.
        app.extra_attrs.emplace_back("max-message-size", "6556");

        push_candidates_into(app);
        return app;
    };

    // ---- The media plan (v1.2.0, session capability plan N3 / T2) ----
    //
    // The media list is not a fixed two-m-line affair any more: it is
    // the session model's transceiver list.  A track the host added
    // shows up as a new m-line, a removed track keeps its m-line and
    // turns it `inactive` (OD-A), and a mid never moves (T3).
    ensure_base_session_state();

    const bool want_data   = (!wt_backend && !config_.datachannel_name.empty());
    const bool want_video  = (video_receiver_ || video_sender_);

    struct MediaPlanEntry {
        std::string    mid;
        sdp::MediaType type = sdp::MediaType::Audio;
        sdp::Direction dir  = sdp::Direction::SendRecv;
    };
    std::vector<MediaPlanEntry> media_plan;

    if (impl_->session_model) {
        for (const auto& t : impl_->session_model->transceivers()) {
            sdp::MediaType type = sdp::MediaType::Audio;
            if (t.kind == plugins::MediaKind::kVideo) {
                type = sdp::MediaType::Video;
            } else if (t.kind == plugins::MediaKind::kData) {
                type = sdp::MediaType::Application;
            }
            // Audio is the default: the transceiver's kind decides the
            // media type, and `kAudio` keeps the type above.
            // A media section we cannot serve is not advertised: no
            // video pipeline means no m=video, no SCTP backend means
            // no m=application.
            if (type == sdp::MediaType::Video && !want_video) continue;
            if (type == sdp::MediaType::Application && !want_data) continue;
            media_plan.push_back(
                {t.mid, type, detail::to_sdp_direction(t.direction)});
        }
    } else {
        // No session model (the host disabled it, or the module is not
        // built): the pre-v1.2.0 fixed media list.
        media_plan.push_back({"0", sdp::MediaType::Audio, sdp::Direction::SendRecv});
        if (want_video) {
            media_plan.push_back({"1", sdp::MediaType::Video, sdp::Direction::SendRecv});
        }
        if (want_data) {
            media_plan.push_back({"2", sdp::MediaType::Application, sdp::Direction::SendRecv});
        }
    }

    // A session model that produced no transceiver at all would leave
    // the offer without a single m-line, and no peer can negotiate an
    // offer that carries no media.  The audio m-line is the engine's
    // baseline, so it is always present.
    if (media_plan.empty()) {
        media_plan.push_back({"0", sdp::MediaType::Audio, sdp::Direction::SendRecv});
    }

    // Stable order: the mids sort numerically ("0" audio, "1" video,
    // "2" data, then whatever the host added), so the media list keeps
    // the shape it always had.
    const auto mid_sort_key = [](const std::string& mid) {
        std::uint64_t numeric = 0;
        bool numeric_mid = !mid.empty();
        for (char c : mid) {
            if (c < '0' || c > '9') { numeric_mid = false; break; }
            numeric = numeric * 10 + static_cast<std::uint64_t>(c - '0');
        }
        return std::pair<std::uint64_t, std::string>{
            numeric_mid ? numeric : ~std::uint64_t{0}, mid};
    };
    std::stable_sort(media_plan.begin(), media_plan.end(),
                     [&mid_sort_key](const MediaPlanEntry& a,
                                    const MediaPlanEntry& b) {
                         return mid_sort_key(a.mid) < mid_sort_key(b.mid);
                     });

    for (const auto& entry : media_plan) {
        switch (entry.type) {
            case sdp::MediaType::Audio:
                sdp.media.push_back(build_audio_media(entry.mid, entry.dir));
                break;
            case sdp::MediaType::Video:
                sdp.media.push_back(build_video_media(entry.mid, entry.dir));
                break;
            case sdp::MediaType::Application:
                sdp.media.push_back(build_app_media(entry.mid, entry.dir));
                break;
            default:
                break;
        }
    }

    // The BUNDLE group lists every mid we emitted (RFC 8829 §5.2.1).
    sdp.bundle_mids.clear();
    for (const auto& m : sdp.media) {
        if (!m.mid.empty()) sdp.bundle_mids.push_back(m.mid);
    }

    // The session changed → the offer we just produced is the new
    // baseline, so the "needs renegotiation" flag goes back to false
    // (N2).
    impl_->session_dirty = false;

    auto sdp_str = impl_->sdp_munger->to_sdp(sdp);
    if (!sdp_str) {
        if (on_error_) on_error_(core::kEngineSdpCorrupt, "SDP munger failed");
        return {};
    }
    impl_->local_sdp = std::move(sdp);
    return sdp_str.value();
}

std::optional<std::string>

NimRTCEngine::process_remote_sdp(std::string_view remote_sdp) noexcept {
    // P0 (v0.12.0): guard against post-close use.  Same rationale as
    // on_transport_recv(): after close() the underlying dtls session is
    // gone but a delayed signaling message may still call this entry
    // point and deref impl_->dtls->set_role() / set_peer_fingerprint()
    // below.  Mirrors the DataChannel guard pattern at line 1341.
    if (state_ != State::kOpen) return std::nullopt;
    auto parsed = impl_->sdp_parser->parse(remote_sdp);
    if (!parsed) {
        if (on_error_) on_error_(core::kEngineSdpCorrupt, "SDP parse failed");
        return std::nullopt;
    }

    const auto& remote = parsed.value();
    core::log::Logger::instance().info(
        std::string("process_remote_sdp: parsed ok; media.size=") + std::to_string(remote.media.size())
        + " extra_attrs.size=" + std::to_string(remote.extra_attrs.size()));

    // ---- Offer or answer? (RFC 3264 §5.1) --------------------------------
    //
    // The answer echoes the offer's `o=` session id, so the offerer can
    // tell "this is the answer to my offer" from "this is a fresh offer"
    // by comparing the ids.  The distinction is load-bearing:
    //
    //   * an **offer** leaves the DTLS roles undecided (RFC 5763 §5 — the
    //     offerer says "actpass"), so we answer and pick a role;
    //   * an **answer** *commits* the roles, so we adopt the role the
    //     answer implies and must NOT answer an answer.
    //
    // Before this distinction existed the engine answered every remote
    // description, which meant an offerer never adopted the role its
    // answer committed it to — and two NimRTC engines could therefore
    // never complete a DTLS handshake (both ended up as servers).
    const bool remote_is_answer =
        !impl_->local_session_id.empty() &&
        remote.origin_session_id == impl_->local_session_id;
    core::log::Logger::instance().info(
        std::string("process_remote_sdp: remote description is an ")
        + (remote_is_answer ? "ANSWER" : "OFFER")
        + (remote_is_answer ? " to our offer" : " — we answer it"));

    // ---- Remote media clock rates (ADR-017 §"Decision 6") ----------------
    //
    // The timeline needs each SSRC's media clock rate, and the only
    // place that rate is written down is the peer's `a=rtpmap:` line
    // (RFC 3550 §6.4.1: the Sender Report does not carry it).  The
    // peer *sends* the media, so its description is authoritative for
    // the inbound packets — not our own configuration.
    //
    // Collecting it here means `handle_rtp()` can pin the right rate on
    // the tracker per SSRC instead of assuming "video ⇒ 90 kHz", which
    // happens to be right for every standardised video codec but is a
    // guess rather than a reading of the wire.
    impl_->remote_clock_rate_hz.clear();
    for (const auto& m : remote.media) {
        for (const auto& [fmt, map] : m.rtpmap) {
            std::uint8_t pt = 0;
            if (!detail::parse_payload_type(fmt, pt)) continue;
            if (map.clock_rate == 0) continue;
            auto [it, inserted] =
                impl_->remote_clock_rate_hz.emplace(pt, map.clock_rate);
            if (!inserted && it->second != map.clock_rate) {
                // The same payload type declared with two different
                // rates across m-sections.  The RTP layer does not
                // track which m-section an SSRC belongs to, so the
                // first declaration wins and the conflict is logged.
                core::log::Logger::instance().warn(
                    "engine[timeline]: payload type " + std::to_string(pt) +
                    " declared with two clock rates (" +
                    std::to_string(it->second) + " and " +
                    std::to_string(map.clock_rate) +
                    "); keeping the first");
            }
        }
    }
    // RFC 3551 §4.4: the static payload types have a fixed meaning and
    // need no `a=rtpmap:` line.  Cover the audio ones a peer may
    // legally omit the line for; the dynamic range (96..127) always
    // carries an rtpmap.
    struct StaticRate {
        std::uint8_t  pt;
        std::uint32_t hz;
    };
    static constexpr StaticRate kStaticRates[] = {
        {0, 8000},    // PCMU
        {8, 8000},    // PCMA
        {9, 8000},    // G722
    };
    for (const auto& sr : kStaticRates) {
        impl_->remote_clock_rate_hz.emplace(sr.pt, sr.hz);
    }
    if (!impl_->remote_clock_rate_hz.empty()) {
        std::string rates;
        for (const auto& [pt, rate] : impl_->remote_clock_rate_hz) {
            if (!rates.empty()) rates += ", ";
            rates += std::to_string(pt) + "→" + std::to_string(rate) + "Hz";
        }
        core::log::Logger::instance().info(
            "engine[timeline]: remote payload-type clock rates: " + rates);
    }

    // ---- Negotiated RTCP feedback capabilities (v1.2.0 M2, F5) ---------
    //
    // The `a=rtcp-fb` lines of the remote description are the only
    // authority on which receiver-side feedback the peer will send
    // (RFC 4585 §4.1).  The collector needs that fact to decide whether
    // the transport-cc path may drive the estimate (ADR-018 Decision 2):
    // a peer that did not advertise transport-cc will never send it, so
    // driving the estimate from it would starve the estimator.
    //
    // The remote description is the peer's offer (we are the answerer)
    // or the peer's answer (we are the offerer); either way it carries
    // the negotiated set — RFC 3264 §6.1 restricts an answer's feedback
    // types to the offer's, so the answer's set is a subset of what the
    // offer could have agreed on.
    if (net_feedback_) {
        plugins::NetFeedbackCapabilities caps;
        for (const auto& m : remote.media) {
            for (const std::string& entry : m.rtcp_fb) {
                if (detail::rtcp_fb_has_type(entry, "transport-cc")) {
                    caps.transport_cc = true;
                }
                if (detail::rtcp_fb_has_type(entry, "goog-remb")) {
                    caps.remb = true;
                }
            }
        }
        net_feedback_->set_peer_capabilities(caps);
        core::log::Logger::instance().info(
            std::string("engine[bwe]: peer feedback capabilities: transport-cc=") +
            (caps.transport_cc ? "yes" : "no") + ", goog-remb=" +
            (caps.remb ? "yes" : "no"));
    }

    // ---- Negotiated payload-type routing (B-4) --------------------------
    //
    // The peer's own `a=rtpmap:` lines are the only authority on what
    // the peer will put on the wire (RFC 3264 §6.1: the answer may only
    // use payload types the offer contained).  The tables below become
    // the demux's routing table: a payload type that is in neither of
    // them is an anomaly, and an anomaly is counted and dropped — never
    // decoded (defect F-8: the old rule was "anything that is not the
    // video payload type is audio", which pushed telephone-event, RED
    // and unsupported-codec packets into the audio pipeline).
    //
    // The two tables are kept apart because a payload type is only
    // meaningful within the m-section that declared it (RFC 3550
    // §5.14: the payload type is scoped to the RTP session, and an
    // m-line that is rejected contributes no RTP session at all).
    impl_->audio_pt_routes.clear();
    impl_->video_pt_routes.clear();
    impl_->negotiated_audio_pt = 0;
    impl_->negotiated_opus_fmtp = detail::OpusFmtpParams{};
    impl_->negotiated_opus_fmtp_text.clear();
    impl_->audio_rx_disabled = false;
    impl_->audio_codec_unsupported_reported = false;
    impl_->pt_routing_active = true;

    for (const auto& m : remote.media) {
        if (m.type != sdp::MediaType::Audio &&
            m.type != sdp::MediaType::Video) {
            continue;
        }
        auto& table = (m.type == sdp::MediaType::Audio)
                          ? impl_->audio_pt_routes
                          : impl_->video_pt_routes;
        for (const auto& fmt : m.formats) {
            std::uint8_t pt = 0;
            if (!detail::parse_payload_type(fmt, pt)) continue;

            detail::AudioPayloadRoute route;
            auto it = m.rtpmap.find(fmt);
            if (it != m.rtpmap.end()) {
                route.encoding   = detail::decoder_id_for_encoding(
                    it->second.encoding);
                route.clock_rate = it->second.clock_rate;
                route.channels   = static_cast<std::uint8_t>(
                    it->second.channels == 0 ? 1 : it->second.channels);
            } else if (m.type == sdp::MediaType::Audio &&
                       detail::static_audio_payload_type(pt, route)) {
                // RFC 3551 §4.5 makes the `a=rtpmap:` line optional for
                // the static payload types, so the table can fill those
                // in on its own.  The dynamic range (96..127) always
                // carries an rtpmap; a dynamic payload type without one
                // is malformed and simply produces no route.
            } else {
                continue;
            }
            table[pt] = route;
        }
    }

    // ---- Which audio payload type do we accept? (B-4) -------------------
    //
    // Two constraints meet here:
    //
    //   * RFC 3264 §6.1 — the answer may only use a payload type the
    //     offer contained.
    //   * A-9 — we may only accept a payload type whose encoding we can
    //     actually decode.  A peer that offers a codec we do not
    //     implement produces `kAudioCodecUnsupported` at negotiation
    //     time, and the audio track is disabled.
    //
    // The selection prefers the payload type we advertise ourselves, so
    // the answer and our own offer stay in sync; otherwise the first
    // offered payload type we can decode wins (the table is ordered by
    // payload type, so the choice is deterministic).
    if (!impl_->audio_pt_routes.empty()) {
        const std::uint8_t preferred = config_.audio_codec.payload_type;
        std::uint8_t chosen = 0;
        for (const auto& [pt, route] : impl_->audio_pt_routes) {
            if (!audio_encoding_decodable(route.encoding)) continue;
            if (chosen == 0) chosen = pt;
            if (pt == preferred) { chosen = pt; break; }
        }
        impl_->negotiated_audio_pt = chosen;

        if (chosen == 0) {
            // The peer offered audio, but nothing in it is decodable
            // here.  This is a configuration condition, not a per-packet
            // anomaly, so it is reported once and disables the track.
            impl_->audio_rx_disabled = true;
            if (!impl_->audio_codec_unsupported_reported && on_error_) {
                impl_->audio_codec_unsupported_reported = true;
                on_error_(core::kAudioCodecUnsupported,
                          "the peer offered an audio codec NimRTC cannot "
                          "decode — the audio track is disabled");
            }
        } else {
            core::log::Logger::instance().info(
                "engine[audio]: negotiated inbound payload type " +
                std::to_string(chosen) + " (encoding " +
                impl_->audio_pt_routes[chosen].encoding + ")");
        }

        // The `a=fmtp:` parameters of the selected payload type are
        // *parsed*, not echoed blindly (B-4).  Only the Opus parameters
        // are meaningful to us; another encoding's parameters are
        // ignored rather than rejected (RFC 3264 §6.1: a peer that
        // sends a parameter we do not know must still be negotiable).
        const std::string selected = std::to_string(impl_->negotiated_audio_pt);
        for (const auto& m : remote.media) {
            if (m.type != sdp::MediaType::Audio) continue;
            auto fit = m.fmtp.find(selected);
            if (fit == m.fmtp.end()) break;
            if (impl_->audio_pt_routes[impl_->negotiated_audio_pt].encoding ==
                "opus") {
                impl_->negotiated_opus_fmtp = detail::parse_opus_fmtp(fit->second);
                impl_->negotiated_opus_fmtp_text =
                    detail::build_negotiated_opus_fmtp(impl_->negotiated_opus_fmtp);
            }
            break;
        }
    }

    // ---- Which video payload type did we negotiate? (C9 / C11) ----------
    //
    // The video side mirrors the audio side above (B-4) and closes the
    // defect C9 names: the engine used to answer a video m-line by
    // copying the offer's `a=rtpmap:` / `a=fmtp:` lines verbatim while
    // stamping its outbound RTP with a *locally configured* payload
    // type.  The two could name different numbers — the peer then
    // receives video on a payload type the negotiated description never
    // mentions, and drops it.
    //
    // RFC 3264 §6.1 constrains the answer to the payload types the
    // offer contained, so the negotiated video payload type is the one
    // that appears in *both* descriptions.  Everything downstream — the
    // answer's format list, the outbound RTP header, the inbound demux
    // — then uses that one number.
    impl_->negotiated_video = detail::VideoNegotiationResult{};
    impl_->negotiated_video_pt.store(0, std::memory_order_relaxed);
    impl_->video_rx_disabled = false;
    impl_->video_codec_unsupported_reported = false;

    // H.264 is the only video codec NimRTC implements today: the H.264
    // module ships the packetizer, the depacketizer and the codec
    // plug-in.  The list grows with the codec-breadth work of the video
    // plan (C4 / C6 / C8: H.265, AV1, VP8, VP9); each new codec adds
    // one line here and the negotiation picks it up automatically.
    //
    // The codec also has to be *present*: without a video codec plug-in
    // the engine cannot decode a single frame, so an H.264 offer is no
    // more decodable than a VP9 one.
    const auto video_encoding_decodable = [this](const std::string& enc) {
        if (enc == "h264") return video_codec_ != nullptr;
        return false;
    };

    const sdp::MediaDescription* remote_video = nullptr;
    for (const auto& m : remote.media) {
        if (m.type != sdp::MediaType::Video) continue;
        remote_video = &m;
        break;
    }

    if (remote_video != nullptr && !remote_video->formats.empty()) {
        // The payload type our own sender stamps into the outbound RTP
        // header (`EngineConfig::video_sender_tuning.payload_type`).
        //
        // It plays two roles:
        //   * when we are the OFFERER and receive an answer, the answer
        //     may only name a payload type our offer contained
        //     (RFC 3264 §6.1), so anything else is filtered out;
        //   * when the peer offers several supported payload types, this
        //     one wins, so the outbound path keeps a stable number.
        const std::uint8_t our_video_pt =
            config_.video_sender_tuning.payload_type;

        std::uint8_t chosen = 0;
        std::string  chosen_encoding;
        std::uint32_t chosen_clock_rate = 0;

        for (const auto& fmt : remote_video->formats) {
            std::uint8_t pt = 0;
            if (!detail::parse_payload_type(fmt, pt)) continue;
            if (remote_is_answer && pt != our_video_pt) continue;
            auto rit = remote_video->rtpmap.find(fmt);
            if (rit == remote_video->rtpmap.end()) continue;
            const std::string enc_lower =
                detail::decoder_id_for_encoding(rit->second.encoding);
            if (!video_encoding_decodable(enc_lower)) continue;
            if (chosen == 0) {
                chosen              = pt;
                chosen_encoding     = rit->second.encoding;
                chosen_clock_rate   = rit->second.clock_rate;
            }
            if (pt == our_video_pt) {
                chosen              = pt;
                chosen_encoding     = rit->second.encoding;
                chosen_clock_rate   = rit->second.clock_rate;
                break;
            }
        }

        if (chosen != 0) {
            impl_->negotiated_video.valid         = true;
            impl_->negotiated_video.payload_type  = chosen;
            impl_->negotiated_video.encoding       = chosen_encoding;
            impl_->negotiated_video.clock_rate    = chosen_clock_rate;
            auto fit = remote_video->fmtp.find(std::to_string(chosen));
            if (fit != remote_video->fmtp.end()) {
                impl_->negotiated_video.fmtp = fit->second;
            }
            impl_->negotiated_video_pt.store(chosen, std::memory_order_relaxed);
            core::log::Logger::instance().info(
                "engine[video]: negotiated payload type " +
                std::to_string(chosen) + " (encoding " + chosen_encoding + ")");
        } else {
            // The peer offered video, but nothing in it is decodable
            // here.  This is a configuration condition, not a per-packet
            // anomaly, so it is reported once and disables the track.
            impl_->negotiated_video.codec_unsupported = true;
            impl_->video_rx_disabled = true;
            if (!impl_->video_codec_unsupported_reported && on_error_) {
                impl_->video_codec_unsupported_reported = true;
                on_error_(core::kVideoCodecUnsupported,
                          "the peer offered a video codec NimRTC cannot "
                          "decode — the video track is disabled");
            }
        }
    }

    // ---- The audio-level extension id (B-14) ---------------------------
    //
    // RFC 5285 §6: the extension id is negotiated per direction.  The
    // id the peer advertised is the id it will send under, so that is
    // the id we parse their inbound packets with.  When we answer the
    // peer's offer we echo that same id back — Chrome rejects an
    // extension whose id changed between the offer and the answer.
    for (const auto& m : remote.media) {
        if (m.type != sdp::MediaType::Audio) continue;
        for (const auto& em : m.extmap) {
            if (em.uri != detail::kAudioLevelExtUri) continue;
            if (em.direction == sdp::Direction::Inactive) continue;
            if (em.id <= 0 || em.id > 255) continue;
            impl_->remote_audio_level_ext_id = em.id;
            if (!remote_is_answer) {
                // We are the answerer: the answer echoes the offered id,
                // and our own outbound packets use the same one.
                impl_->local_audio_level_ext_id = em.id;
            }
            break;
        }
        break;
    }

    // Apply ICE credentials + candidates through the plugin seam.
    //
    // RFC 8829 / WebRTC BUNDLE: when the SDP uses BUNDLE, ice-ufrag/ice-pwd
    // often live at the *session* level (since there is effectively one
    // transport), not at the media level.  Try session-level first, then
    // fall back to media-level — whichever has credentials wins.
    std::string ice_block;

    // 1. Session-level fallback (Chrome BUNDLE answer).  The concrete
    //    sdp::SessionDescription stores session-level a= attributes in
    //    extra_attrs (when cur_media was nullopt at parse time).
    std::string sess_ufrag, sess_pwd;
    for (const auto& [k, v] : remote.extra_attrs) {
        if      (k == "ice-ufrag" && !v.empty()) sess_ufrag = v;
        else if (k == "ice-pwd"   && !v.empty()) sess_pwd   = v;
    }
    auto flush_ufrag_pwd = [&](const std::string& ufrag,
                               const std::string& pwd) {
        if (ufrag.empty() || pwd.empty()) return false;
        ice_block += "a=ice-ufrag:" + ufrag + "\n";
        ice_block += "a=ice-pwd:"   + pwd   + "\n";
        return true;
    };

    bool wrote_creds = false;
    if (!sess_ufrag.empty() || !sess_pwd.empty()) {
        wrote_creds = flush_ufrag_pwd(sess_ufrag, sess_pwd);
    }
    core::log::Logger::instance().info(
        std::string("process_remote_sdp: sess_ufrag='") + sess_ufrag
        + "' sess_pwd.len=" + std::to_string(sess_pwd.size())
        + " wrote_creds=" + (wrote_creds ? "1" : "0"));

    // 2. Media-level candidates (always appended).  Use the *first* media
    //    block that has credentials; otherwise use session credentials.
    //
    // Note: `sdp::SessionDescription::media[i]` is the concrete MediaDescription
    // (NOT the plugins::SdpMedia wrapper), which exposes dedicated ice_ufrag,
    // ice_pwd and candidates fields.  This is the engine's internal parser
    // (impl_->sdp_parser), not the ISDP plugin.
    for (const auto& m : remote.media) {
        core::log::Logger::instance().info(
            std::string("process_remote_sdp: media ice_ufrag='") + m.ice_ufrag
            + "' ice_pwd.len=" + std::to_string(m.ice_pwd.size())
            + " candidates=" + std::to_string(m.candidates.size())
            + " dtls_setup='" + m.dtls_setup + "'");
        if (!wrote_creds) {
            wrote_creds = flush_ufrag_pwd(m.ice_ufrag, m.ice_pwd);
        }
        for (const auto& c : m.candidates) {
            ice_block += "a=candidate:" + c + "\n";
        }
        // Don't break: subsequent media blocks may carry more candidates.
    }

    core::log::Logger::instance().info(
        std::string("process_remote_sdp: ice_block.size=") + std::to_string(ice_block.size())
        + " ice_t_=" + (ice_t_ ? "yes" : "no")
        + " wrote_creds=" + (wrote_creds ? "1" : "0"));

    if (wrote_creds && !ice_block.empty() && ice_t_) {
        ice_t_->set_remote_description(ice_block);
    }

    // Configure DTLS role + peer fingerprint from the FIRST m-line we
    // recognise (audio / video / application).  The DTLS session is
    // BUNDLE-shared, so all m= lines use the same DTLS role + fingerprint
    // — we only need to read the offer's setup once.
    for (const auto& rm : remote.media) {
        if (rm.type != sdp::MediaType::Audio &&
            rm.type != sdp::MediaType::Video &&
            rm.type != sdp::MediaType::Application) {
            continue;
        }
        if (auto d = impl_->dtls.lock()) {
        // DTLS role is determined by the remote peer's `a=setup` attribute
        // (RFC 5763 §5):
        //
        //   remote "actpass" → the peer did not commit to a role; we are the
        //                      answerer and we decide.  WebRTC convention
        //                      (libwebrtc / Chrome): answerer is the DTLS
        //                      Client (sends ClientHello).  We therefore
        //                      become Client and advertise "active".
        //   remote "active"  → the peer will send ClientHello; we are Server
        //                      (passive), advertise "passive".
        //   remote "passive" → the peer is waiting for our ClientHello;
        //                      we are Client (active), advertise "active".
        //   remote ""        → missing setup attribute; treat as "actpass"
        //                      (we answerer, become Client, advertise "active").
        //
        // The resulting local setup attribute is also captured for the
        // answer SDP below.
        std::string peer_setup = rm.dtls_setup;
        if (peer_setup.empty()) peer_setup = "actpass";

        nimrtc::dtls::DtlsRole desired_role  = nimrtc::dtls::DtlsRole::Server;
        std::string    local_setup_attr = "passive";

        if (remote_is_answer) {
            // ---- We are the OFFERER: the answer commits the roles ------
            //
            // RFC 5763 §5: the answerer picks "active" (it is the DTLS
            // client) or "passive" (it is the DTLS server), and the
            // offerer takes the complementary role.  This is the branch
            // that lets two NimRTC engines pair up: our answerer always
            // prefers to be the server (see below), so the answer says
            // "passive" and we — the offerer — become the client and send
            // the ClientHello.
            //
            // "active" in the answer ⇒ the peer is the client ⇒ we are the
            // server.  "passive" ⇒ the peer is the server ⇒ we are the
            // client.  "actpass" in an *answer* is non-compliant (RFC 5763
            // §5 allows it only in an offer); we fall back to the WebRTC
            // convention, in which the offerer is the DTLS server.
            if (peer_setup == "passive") {
                desired_role     = nimrtc::dtls::DtlsRole::Client;
                local_setup_attr = "active";
            } else if (peer_setup == "active") {
                desired_role     = nimrtc::dtls::DtlsRole::Server;
                local_setup_attr = "passive";
            } else {
                desired_role     = nimrtc::dtls::DtlsRole::Server;
                local_setup_attr = "passive";
            }
        } else {
            // ---- We are the ANSWERER: we pick the role ------------------
            //
            // DEBUG-TEMP: force NimRTC to be the DTLS SERVER (passive) for
            // Chrome interop.  Recent Chrome 124+ BoringSSL rejects every
            // variant of anonymous client response to CertificateRequest
            // (empty list → handshake_failure, omitted → unexpected_message,
            // self-signed cert → certificate_unknown) — but as the SERVER we
            // never receive a CertReq so the question doesn't arise.  This
            // is purely a workaround for the Chrome-side rejection of RFC
            // 5246 §7.4.6 anonymous clients; restore the original logic
            // once Chrome's BoringSSL relaxes this.
            //
            // The counterpart of this policy is the offerer branch above:
            // a peer that answers "passive" gets a client, so a
            // NimRTC-to-NimRTC session still has exactly one ClientHello
            // sender.
            desired_role     = nimrtc::dtls::DtlsRole::Server;
            local_setup_attr = "passive";
        }
        d->set_role(desired_role);
        impl_->dtls_is_client  = (desired_role == nimrtc::dtls::DtlsRole::Client);
        impl_->dtls_local_setup = local_setup_attr;
        core::log::Logger::instance().info(
            std::string("process_remote_sdp: dtls role resolved peer_setup='")
            + peer_setup + "' local_setup='" + local_setup_attr
            + "' role=" + (desired_role == nimrtc::dtls::DtlsRole::Client ? "Client" : "Server"));

            if (!rm.dtls_fingerprint_algo.empty() &&
                !rm.dtls_fingerprint_value.empty()) {
                // RFC 8122: peer fingerprint value is colon-separated UPPER hex,
                // NOT base64.  Use hex_colon_decode; fall back to base64 if the
                // value clearly isn't hex (older peers / future deprecation).
                auto bytes = detail::hex_colon_decode(rm.dtls_fingerprint_value);
                if (bytes.empty()) {
                    bytes = detail::base64_decode(rm.dtls_fingerprint_value);
                }
                d->set_peer_fingerprint(rm.dtls_fingerprint_algo, std::move(bytes));
            }
        }
        break;
    }

    if (ice_t_) ice_t_->wait_for_gathering(1500);

    // ---- Cross-end timeline: the peer's abs-send-time extension id ------
    //
    // RFC 5285 §6: the extension id is negotiated per direction.  The id
    // the peer advertised in ITS description is the id it will send under,
    // so that is the id we parse their inbound packets with.  We accept the
    // extension if — and only if — we offered it too; advertising an
    // extension the peer never offered is not a negotiation (RFC 3264 §6.1)
    // and Chrome silently drops it.
    //
    // This runs for BOTH descriptions — an offer (we are the answerer) and
    // an answer (we are the offerer) — because in both cases the peer's
    // description carries the id it will use when it sends to us.  It is
    // read from the FIRST m-section that advertises it and then reused for
    // the remaining ones: Chrome uses the same id across its audio and
    // video m-lines, and a per-m-section id would require tracking which
    // m-line a given SSRC belongs to before we could parse it — which the
    // RTP layer does not currently carry.
    if (config_.timeline_enabled) {
        for (const auto& rm : remote.media) {
            if (rm.type == sdp::MediaType::Application) continue;
            for (const auto& em : rm.extmap) {
                if (em.uri != rtp::kAbsSendTimeUri) continue;
                if (em.direction == sdp::Direction::Inactive) continue;
                // Direction semantics: SendOnly means the peer sends and we
                // receive, so its packets carry the extension.  SendRecv
                // means both.  RecvOnly means the peer only receives, so it
                // will not send us the extension.
                const bool peer_sends =
                    em.direction == sdp::Direction::SendRecv ||
                    em.direction == sdp::Direction::SendOnly;
                if (!peer_sends) continue;
                if (em.id <= 0 || em.id > 255) continue;
                if (impl_->remote_abs_send_time_id == 0) {
                    impl_->remote_abs_send_time_id =
                        static_cast<std::uint16_t>(em.id);
                    core::log::Logger::instance().info(
                        "engine[timeline]: peer advertised abs-send-time "
                        "under extension id " + std::to_string(em.id));
                }
                break;
            }
            if (impl_->remote_abs_send_time_id != 0) break;
        }
    }

    // ---- An answer needs no answer ---------------------------------------
    //
    // We are the offerer and the peer answered: the ICE credentials, the
    // DTLS role and the peer fingerprint are already applied above, and
    // there is nothing left to send back.  Returning an *empty* string
    // (rather than std::nullopt) keeps the "the call succeeded" signal
    // that every caller in the tree checks for — an answer is a successful
    // call that produces no SDP.
    if (remote_is_answer) {
        core::log::Logger::instance().info(
            "process_remote_sdp: answer applied — no answer generated "
            "(dtls role=" +
            std::string(impl_->dtls_is_client ? "Client" : "Server") + ")");
        // The peer's answer closes the current negotiation round, so the
        // session is no longer "dirty" (N2).
        impl_->session_dirty = false;
        return std::string{};
    }

    // ---- Adopt the peer's m-lines as transceivers (T2 / T3) -----------
    //
    // The peer's offer carries one m-line per media section, each with
    // its own mid.  Each of them becomes a transceiver in our session
    // model, so:
    //
    //   - the mid the peer chose is the mid we answer with (RFC 3264
    //     §6.1: the answer mirrors the offer's m-sections one-to-one),
    //   - a transceiver the peer offered but we never created (the peer
    //     has more media than we do) is visible to the host through
    //     `transceivers()` — it can attach a track to it or stop it.
    //
    // A transceiver that already exists (our base audio / video / data
    // ones) keeps its identity; only its direction is refreshed, and
    // only from what the peer *offered* — our own capability is
    // intersected with it when the answer is built.
    if (impl_->session_model) {
        for (const auto& rm : remote.media) {
            if (rm.type != sdp::MediaType::Audio &&
                rm.type != sdp::MediaType::Video &&
                rm.type != sdp::MediaType::Application) {
                continue;
            }
            const plugins::MediaKind kind =
                (rm.type == sdp::MediaType::Video)
                    ? plugins::MediaKind::kVideo
                    : (rm.type == sdp::MediaType::Application)
                          ? plugins::MediaKind::kData
                          : plugins::MediaKind::kAudio;

            const std::string mid = rm.mid.empty()
                ? std::to_string(remote.media.size())
                : rm.mid;

            // Does a transceiver already own that mid?
            bool found = false;
            for (const auto& t : impl_->session_model->transceivers()) {
                if (t.mid == mid) { found = true; break; }
            }
            if (!found) {
                // A transceiver we had no counterpart for.  Its initial
                // direction is the *inverse* of what the peer offered:
                // a peer that offers `sendonly` sends, so we receive.
                const plugins::Direction peer_can =
                    detail::from_sdp_direction(rm.direction);
                const plugins::Direction ours =
                    (peer_can == plugins::Direction::kSendOnly)
                        ? plugins::Direction::kRecvOnly
                    : (peer_can == plugins::Direction::kRecvOnly)
                        ? plugins::Direction::kSendOnly
                        : peer_can;
                const std::string created =
                    impl_->session_model->add_transceiver_at(mid, kind, ours);
                if (created.empty()) continue;
            }
            // An existing transceiver keeps *our* direction — the peer's
            // capability is intersected with it when the answer is built,
            // and overwriting it here would throw our side away.
        }
    }

    sdp::SessionDescription ans;
    ans.version = 0;
    ans.origin_username         = "-";
    // RFC 3264 §5.1: the answer carries the *offer's* session id and the
    // offer's session version incremented by one.  Echoing the id is what
    // lets the offerer recognise the answer (and adopt the DTLS role it
    // commits); a fresh id here would leave the offerer guessing.
    ans.origin_session_id      = remote.origin_session_id;
    ans.origin_session_version = "1";
    {
        // The offer's version is a decimal counter ("1", "2", …).  Bump it
        // when it parses; a non-numeric version is left alone rather than
        // mangled.
        const std::string& v = remote.origin_session_version;
        bool numeric = !v.empty() && v.size() <= 9;
        for (char c : v) {
            if (c < '0' || c > '9') { numeric = false; break; }
        }
        if (numeric) {
            ans.origin_session_version =
                std::to_string(std::stoul(v) + 1);
        } else if (v.empty()) {
            ans.origin_session_version = "1";
        } else {
            ans.origin_session_version = v;
        }
    }
    ans.origin_address          = "0.0.0.0";
    ans.session_name            = "-";
    ans.bundle_mids             = remote.bundle_mids;
    ans.msid_semantic_token     = "WMS";

    for (const auto& rm : remote.media) {
        // Support audio (RTP), video (RTP/H.264), and application (data
        // channel, DTLS/SCTP).  Skipping other types avoids leaking
        // unknown m= sections into the answer.
        if (rm.type != sdp::MediaType::Audio &&
            rm.type != sdp::MediaType::Video &&
            rm.type != sdp::MediaType::Application) {
            continue;
        }
        sdp::MediaDescription am;
        am.type = rm.type;
        am.port = 9;
        am.protocol = rm.protocol.empty() ? "UDP/TLS/RTP/SAVPF" : rm.protocol;
        am.formats  = rm.formats;
        // RFC 3264 §6.1: the answer's media direction is the answerer's
        // capability intersected with the offer's direction.  For our
        // demo the answerer always has SendRecv capability, so the
        // answer direction reduces to the inverse of the offer for
        // asymmetric offers:
        //   offer sendrecv -> answer sendrecv
        //   offer sendonly -> answer recvonly
        //   offer recvonly -> answer sendonly
        //   offer inactive -> answer inactive
        am.direction = ([&]() {
            using D = sdp::Direction;
            switch (rm.direction) {
                case D::SendRecv: return D::SendRecv;
                case D::SendOnly: return D::RecvOnly;
                case D::RecvOnly: return D::SendOnly;
                case D::Inactive: return D::Inactive;
            }
            return D::SendRecv;
        })();
        // Preserve the remote mid when present (BUNDLE); fall back to a
        // positional assignment by index for the legacy single-m-line case.
        am.mid      = rm.mid.empty() ? std::to_string(remote.media.size()) : rm.mid;

        // ---- Session model overrides the direction (N4 / OD-A) ----------
        //
        // The direction computed above is "what the *peer* offered,
        // inverted" — the answerer's capability is assumed to be
        // sendrecv.  When the session model knows the transceiver that
        // owns this mid, the *session* is the authority on what we can
        // actually do: a transceiver the host stopped, or one whose
        // track was removed, must be answered `inactive` (OD-A), and a
        // sendonly transceiver must not answer `recvonly`.
        //
        // The mid is matched against the transceiver list, so a
        // transceiver the peer did not offer is simply not found and
        // the historical behaviour stands.
        if (impl_->session_model && !am.mid.empty()) {
            for (const auto& t : impl_->session_model->transceivers()) {
                if (t.mid != am.mid) continue;
                // Our own capability, intersected with the peer's.
                const plugins::Direction ours = t.direction;
                const sdp::Direction peer = rm.direction;
                am.direction = detail::intersect_direction(
                    detail::to_sdp_direction(ours), peer);
                break;
            }
        }
        // rtcp-mux is RTP-specific — invalid on an SCTP m-line.  The
        // munger emits `a=rtcp-mux` whenever the field is non-empty;
        // we set it only for audio/video to avoid spurious lines
        // on the SCTP m-line.
        am.rtcp_mux_value =
            (rm.type == sdp::MediaType::Application) ? "" : "rtcp-mux";
        am.ice_ufrag = local_ufrag();
        am.ice_pwd   = local_password();
        am.dtls_setup = impl_->dtls_local_setup.empty()
                            ? "active"
                            : impl_->dtls_local_setup;
        core::log::Logger::instance().info(
            std::string("process_remote_sdp: answer SDP setup='") + am.dtls_setup +
            "' (peer wanted '" + rm.dtls_setup + "')");
        if (auto d = impl_->dtls.lock()) {
            am.dtls_fingerprint_algo  = "sha-256";
            am.dtls_fingerprint_value = d->local_fingerprint().hex_colon;
        }

        // For audio: carry rtpmap/fmtp from offer.
        //
        // The a=ssrc line is appended for the same reason the video
        // block below does it: Chrome's inbound-rtp counter only
        // advances when the description names the SSRC that the RTP
        // header actually carries.  The value is the engine's own
        // per-engine SSRC (A-6) — the same member the packet path
        // stamps, so the description and the wire cannot disagree.
        if (rm.type == sdp::MediaType::Audio) {
            // RFC 3264 §6.1: the answer's format list must be a subset
            // of the offer's, and it names the payload type the peer
            // will actually send on.  We answer with exactly the one we
            // accept — a payload type we merely *could* accept but did
            // not select would leave the peer free to send on a codec
            // we never agreed to decode.
            if (impl_->negotiated_audio_pt != 0) {
                const std::string selected =
                    std::to_string(impl_->negotiated_audio_pt);
                am.formats = {selected};

                // Only the selected payload type's own `a=rtpmap:` line
                // survives into the answer.
                auto rit = rm.rtpmap.find(selected);
                if (rit != rm.rtpmap.end()) am.rtpmap[selected] = rit->second;

                // The `a=fmtp:` line is the *negotiated* subset of what
                // the peer sent (B-4), not a blind echo of it.  When the
                // peer said nothing about the format parameters we
                // advertise our own: in-band FEC has to be signalled or
                // the peer will not embed the redundancy the decoder
                // needs to recover a lost frame (B-8).
                const std::string negotiated =
                    impl_->negotiated_opus_fmtp_text.empty()
                        ? detail::build_local_opus_fmtp(config_.pcm_channels)
                        : impl_->negotiated_opus_fmtp_text;
                if (!negotiated.empty()) am.fmtp[selected] = negotiated;
            } else {
                // Nothing the peer offered is decodable here, so the
                // m-line is rejected (RFC 3264 §6.1: a rejected m-line
                // carries port 0 and no format list).
                am.port    = 0;
                am.formats.clear();
                am.rtpmap.clear();
                am.fmtp.clear();
            }

            char audio_ssrc_line[96];
            std::snprintf(audio_ssrc_line, sizeof(audio_ssrc_line),
                          "%u cname:nimrtc-audio",
                          static_cast<unsigned>(impl_->audio_ssrc));
            am.extra_attrs.emplace_back("ssrc", audio_ssrc_line);
            am.extra_attrs.emplace_back("ptime",
                                         std::to_string(config_.audio_ptime_ms));
            am.extra_attrs.emplace_back("maxptime",
                                         std::to_string(config_.audio_ptime_ms));

            // ---- a=rtcp-fb echo (RFC 3264 §6.1) --------------------------
            //
            // The answer carries the subset of the offer's feedback types
            // that we support.  The transport-cc entry is the one that
            // matters for the bandwidth loop (ADR-018): a peer only sends
            // transport-cc feedback when the negotiated description
            // carries the attribute for that m-section, so dropping it
            // here would silently disable the preferred signal.
            //
            // The payload type is the *negotiated* one (the answer's
            // format list), not the peer's: RFC 4585 §4.1 scopes the
            // attribute to a payload type within its m-section.
            if (impl_->negotiated_audio_pt != 0) {
                for (const std::string& entry : rm.rtcp_fb) {
                    if (!detail::rtcp_fb_entry_supported(entry)) continue;
                    const std::string_view types = detail::rtcp_fb_types(entry);
                    if (types.empty()) continue;
                    am.rtcp_fb.push_back(std::to_string(impl_->negotiated_audio_pt) +
                                         " " + std::string(types));
                }
            }

            // a=extmap:<id> urn:ietf:params:rtp-hdrext:ssrc-audio-level
            //
            // RFC 5285 §6: an answer that drops an extension the offer
            // advertised is a rejection of that extension, and Chrome
            // treats it as one.  The id must be the one the offer used —
            // either the id the peer advertised (we are the answerer) or
            // the one we advertised ourselves (we are the offerer).
            {
                sdp::MediaDescription::ExtMap al;
                al.id = impl_->local_audio_level_ext_id != 0
                            ? impl_->local_audio_level_ext_id
                            : detail::kAudioLevelDefaultLocalId;
                impl_->local_audio_level_ext_id = al.id;
                al.direction = sdp::Direction::SendRecv;
                al.uri       = std::string(detail::kAudioLevelExtUri);
                am.extmap.push_back(al);
            }
        }

        // For video: name exactly the payload type that was negotiated.
        //
        // The old code copied the offer's `a=rtpmap:` / `a=fmtp:` /
        // `a=rtcp-fb:` lines verbatim, which told the peer we would
        // decode every codec it offered — including the ones NimRTC has
        // no decoder for (C9).  RFC 3264 §6.1 wants the answer to carry
        // the subset that was actually negotiated, which is what
        // `negotiated_video` holds.
        if (rm.type == sdp::MediaType::Video) {
            if (impl_->negotiated_video.valid) {
                const std::string selected =
                    std::to_string(impl_->negotiated_video.payload_type);
                am.formats = {selected};

                // Only the negotiated payload type's own `a=rtpmap:`
                // line survives into the answer.
                auto rit = rm.rtpmap.find(selected);
                if (rit != rm.rtpmap.end()) am.rtpmap[selected] = rit->second;

                // The `a=fmtp:` line tells the receiver how to read the
                // bitstream (RFC 6184 §5.6: profile-level-id and
                // packetization-mode).  It is echoed as the peer sent
                // it — the parameters describe the *peer's* encoder.
                auto fit = rm.fmtp.find(selected);
                if (fit != rm.fmtp.end()) am.fmtp[selected] = fit->second;

                // The feedback set is the offer's, rewritten onto the
                // negotiated payload type (RFC 4585 §4.1 scopes an
                // `a=rtcp-fb:` entry to a payload type) and filtered to
                // the types we support.  "transport-cc" is in the
                // supported set — dropping it would silently disable
                // the preferred bandwidth signal (ADR-018).
                for (const std::string& entry : rm.rtcp_fb) {
                    if (!detail::rtcp_fb_entry_supported(entry)) continue;
                    const std::string_view types = detail::rtcp_fb_types(entry);
                    if (types.empty()) continue;
                    am.rtcp_fb.push_back(selected + " " + std::string(types));
                }
            } else {
                // Nothing the peer offered is decodable here, so the
                // m-line is rejected (RFC 3264 §6.1: a rejected m-line
                // carries port 0 and no format list).
                am.port    = 0;
                am.formats.clear();
                am.rtpmap.clear();
                am.fmtp.clear();
                am.rtcp_fb.clear();
            }
            char ssrc_line[96];
            std::snprintf(ssrc_line, sizeof(ssrc_line),
                          "%u cname:nimrtc-video",
                          static_cast<unsigned>(config_.video_sender_tuning.ssrc));
            am.extra_attrs.emplace_back("ssrc", ssrc_line);
        }

        // ---- Cross-end timeline extmap (ADR-017) --------------------------
        //
        // The *peer's* extension id was resolved above (before the answer
        // construction) so that the offer path and the answer path share
        // one resolution.  What is left here is the *echo*: RFC 5285 §6
        // requires the answer to carry the same id we advertised in our own
        // offer, otherwise the peer treats the extension as rejected and
        // never parses our abs-send-time.
        //
        // For data channel (application): carry sctp-port / max-message-size
        // attributes verbatim.  We use `extra_attrs` (the catch-all
        // a= bucket in `MediaDescription`) for `sctp-port` because the
        // parser only stores known SDP attributes in dedicated fields;
        // `sctp-port` isn't a dedicated field on `MediaDescription`
        // yet (P2 follow-up).  The munger re-emits
        // `a=sctp-port:<port>` from extra_attrs verbatim, which is
        // exactly what Chrome's SCTP-over-DTLS stack needs to see.
        if (rm.type == sdp::MediaType::Application) {
            for (auto& [fmt, params] : rm.fmtp) {
                // params looks like "max-message-size=1073741823"
                am.fmtp[fmt] = params;
            }
            // Echo any peer-supplied `a=sctp-port:` value.  When the
            // peer didn't include one, fall back to the engine's own
            // configured port so the answer never omits the attribute
            // (RFC 8831 §6.1 calls sctp-port mandatory for SCTP).
            bool found_sctp_port = false;
            for (const auto& [k, v] : rm.extra_attrs) {
                if (k == "sctp-port" && !v.empty()) {
                    am.extra_attrs.emplace_back(k, v);
                    found_sctp_port = true;
                }
            }
            if (!found_sctp_port) {
                am.extra_attrs.emplace_back(
                    "sctp-port", std::to_string(config_.sctp_port));
            }
            // Likewise for max-message-size — echo if present, default
            // to 6556 (Chrome M85+) when absent.
            bool found_max_msg = false;
            for (const auto& [k, v] : rm.extra_attrs) {
                if (k == "max-message-size" && !v.empty()) {
                    am.extra_attrs.emplace_back(k, v);
                    found_max_msg = true;
                }
            }
            if (!found_max_msg) {
                am.extra_attrs.emplace_back("max-message-size", "6556");
            }
        }

        // Echo the abs-send-time extmap back on the RTP m-sections (ADR-017).
        // RFC 5285 §6: an answer that omits an extension the offer advertised
        // is a rejection of that extension, and Chrome treats it as one — the
        // peer then never parses our abs-send-time.  The id must be the SAME
        // one we advertised in our own offer (or, when answering a peer's
        // offer, the one the peer advertised for its send direction).
        //
        // `local_abs_send_time_id` is set by create_offer(); when this engine
        // is the answerer and never produced an offer, it is still 0 and we
        // fall back to the documented default — the answer then declares id 4
        // and we build our outbound packets with the same id.
        if (config_.timeline_enabled &&
            rm.type != sdp::MediaType::Application) {
            const std::uint16_t answer_id =
                impl_->local_abs_send_time_id != 0
                    ? impl_->local_abs_send_time_id
                    : rtp::kAbsSendTimeDefaultLocalId;
            impl_->local_abs_send_time_id = answer_id;
            sdp::MediaDescription::ExtMap ast;
            ast.id        = static_cast<int>(answer_id);
            ast.direction = sdp::Direction::SendRecv;
            ast.uri       = std::string(rtp::kAbsSendTimeUri);
            am.extmap.push_back(ast);
        }

        if (ice_t_) {
            auto gathered = ice_t_->gathered_local_candidates();
            for (auto& c : gathered) {
                // libjuice emits candidates with the "a=candidate:" prefix.
                // The munger adds its own "a=candidate:" so strip the prefix.
                constexpr std::string_view kFullPrefix = "a=candidate:";
                constexpr std::string_view kHalfPrefix = "candidate:";
                if (c.size() >= kFullPrefix.size() &&
                    c.substr(0, kFullPrefix.size()) == kFullPrefix) {
                    am.candidates.push_back(std::string(c.substr(kFullPrefix.size())));
                } else if (c.size() >= kHalfPrefix.size() &&
                           c.substr(0, kHalfPrefix.size()) == kHalfPrefix) {
                    am.candidates.push_back(std::string(c.substr(kHalfPrefix.size())));
                } else {
                    am.candidates.push_back(c);
                }
            }
        }

        ans.media.push_back(std::move(am));
    }

    // Rebuild BUNDLE mids from the answer's media sections so a remote
    // multi-m-line BUNDLE offer produces a matching multi-m-line answer.
    if (!ans.media.empty()) {
        ans.bundle_mids.clear();
        for (const auto& m : ans.media) {
            if (!m.mid.empty()) ans.bundle_mids.push_back(m.mid);
        }
        if (ans.bundle_mids.empty()) {
            ans.bundle_mids = {"0"};
        }
    }

    auto ans_str = impl_->sdp_munger->to_sdp(ans);
    if (!ans_str) {
        if (on_error_) on_error_(core::kEngineSdpCorrupt, "SDP munger failed");
        return std::nullopt;
    }
    return ans_str.value();
}

}  // namespace nimrtc::engine
