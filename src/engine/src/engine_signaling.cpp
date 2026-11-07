/**
 * @file src/engine/src/engine_signaling.cpp
 * @brief Engine Signaling plane (v1.1 R1). create_offer / process_remote_sdp..
 *
 * v1.1 R1 split of engine.cpp - mechanical move,
 * behaviour unchanged. Shares engine_impl.hpp.
 */
#include "engine_impl.hpp"

namespace nimrtc::engine {


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

    sdp::MediaDescription audio;
    audio.type            = sdp::MediaType::Audio;
    audio.port            = 9;
    audio.protocol        = "UDP/TLS/RTP/SAVPF";
    // Build the format string key from the configurable payload type.
    const std::string pt = std::to_string(config_.audio_codec.payload_type);
    audio.formats         = {pt};
    audio.direction       = sdp::Direction::SendRecv;
    audio.mid             = "0";
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
    if (!config_.audio_codec.fmtp.empty())
        audio.fmtp[pt] = config_.audio_codec.fmtp;

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
    push_candidates_into(audio);

    sdp.media.push_back(std::move(audio));

    // ---- Video m-line (RFC 8829 §5, H.264 via RFC 6184) -----------------
    // Emit a video m-line only when at least one video plugin was resolved
    // (video_receiver_ / video_sender_ / video_codec_).  The default H.264
    // codec plugin is decoder-only — for encode we'd need a real codec
    // plugin — but the SDP/SDP-answer plumbing is correct regardless so
    // that browsers see the negotiation succeed even on a one-way demo.
    if (video_receiver_ || video_sender_) {
        sdp::MediaDescription video;
        video.type     = sdp::MediaType::Video;
        video.port     = 9;
        video.protocol = "UDP/TLS/RTP/SAVPF";
        const std::string vpt =
            std::to_string(config_.video_receiver_tuning.payload_type);
        video.formats         = {vpt};
        video.direction       = sdp::Direction::SendRecv;
        video.mid             = "1";           // second BUNDLE mid
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
        sdp.media.push_back(std::move(video));
        // Make sure both mids are in the BUNDLE group.
        sdp.bundle_mids = {"0", "1"};
    }

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
    if (!wt_backend && !config_.datachannel_name.empty()) {
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
        app.direction       = sdp::Direction::SendRecv;
        // Third BUNDLE mid — keep mids 0=audio, 1=video, 2=application
        // to match the wire order.
        app.mid             = "2";
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
        sdp.media.push_back(std::move(app));
        // Update BUNDLE mids — the application mid always appends.
        // We rebuild from sdp.media to keep the order in sync (audio=0,
        // video=1, application=2).  When video is disabled, the audio mid
        // is still 0 and the application mid slides to 1 to keep the
        // BUNDLE group contiguous.
        sdp.bundle_mids.clear();
        for (const auto& m : sdp.media) {
            if (!m.mid.empty()) sdp.bundle_mids.push_back(m.mid);
        }
    }

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
        return std::string{};
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
            am.rtpmap = rm.rtpmap;
            am.fmtp   = rm.fmtp;
            char audio_ssrc_line[96];
            std::snprintf(audio_ssrc_line, sizeof(audio_ssrc_line),
                          "%u cname:nimrtc-audio",
                          static_cast<unsigned>(impl_->audio_ssrc));
            am.extra_attrs.emplace_back("ssrc", audio_ssrc_line);
            am.extra_attrs.emplace_back("ptime",
                                         std::to_string(config_.audio_ptime_ms));
            am.extra_attrs.emplace_back("maxptime",
                                         std::to_string(config_.audio_ptime_ms));
        }

        // For video: carry rtpmap/fmtp/rtcp-fb from offer verbatim.  The
        // remote codec params (profile-level-id, packetization-mode, etc.)
        // are what the remote decoder expects to see in our answer; the
        // engine doesn't renegotiate them.  We also append the a=ssrc
        // line so Chrome's inbound-rtp counter advances on the wire SSRC
        // we emit.
        if (rm.type == sdp::MediaType::Video) {
            am.rtpmap   = rm.rtpmap;
            am.fmtp     = rm.fmtp;
            am.rtcp_fb  = rm.rtcp_fb;
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
