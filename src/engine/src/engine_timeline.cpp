/**
 * @file src/engine/src/engine_timeline.cpp
 * @brief Engine Timeline plane (v1.1 R1). ADR-017 observe/handle path, Sender-Report emission, capture/skew accessors, ref_frame id <-> capture-ts map (T1/T2)..
 *
 * v1.1 R1 split of engine.cpp - mechanical move,
 * behaviour unchanged. Shares engine_impl.hpp.
 */
#include "engine_impl.hpp"

namespace nimrtc::engine {


void NimRTCEngine::maybe_send_sender_report() noexcept {
    // Cross-end timeline (ADR-017 §"Decision 5"). Gated so that with
    // `timeline_enabled = false` the engine emits *no* RTCP at all: the
    // flag must cost nothing on the wire when off.
    if (!config_.timeline_enabled || config_.sr_period_ms == 0) return;
    if (!ice_t_ || !is_ice_connected()) return;

    const auto now = core::SteadyClock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - impl_->sr_last_sent).count();
    if (elapsed < static_cast<std::int64_t>(config_.sr_period_ms)) return;
    impl_->sr_last_sent = now;

    // No audio has gone out yet — an SR with a zero RTP timestamp and a
    // zero packet count describes no media position, and would poison the
    // peer's extrapolation anchor. Wait for the first frame instead.
    if (impl_->audio_packet_count == 0) return;

    // RFC 3550 §6.4.1: the SR pairs the sender's wall-clock instant with
    // the RTP timestamp of the media it describes.
    //
    // The two must denote the *same* instant, or the pair is internally
    // inconsistent and every receiver that reconstructs from it inherits
    // the gap.  RFC 3550 words the NTP field as "the wallclock time when
    // the report was sent" while the RTP timestamp is that of the last
    // packet sent — two different moments, and the difference between
    // them (up to a whole report period) is the bias.
    //
    // The engine therefore uses the *packet's* send instant, recorded
    // when the packet was built and stamped with its abs-send-time
    // (see `Impl::audio_last_send_unix_us`).  That is the same instant
    // the abs-send-time field encodes, so the peer's reconstruction has
    // no report-cadence bias: the one-way delay it reports is the wire,
    // not the report schedule.
    //
    // Falling back to "now" would only be correct if no packet had been
    // sent yet — but that case is already excluded above (a report with
    // a zero packet count describes no media position).
    const std::int64_t send_unix_us = impl_->audio_last_send_unix_us;
    if (send_unix_us <= 0) return;
    const rtp::Ntp64 sr_ntp = rtp::ntp64_from(
        std::chrono::system_clock::time_point{
            std::chrono::microseconds{send_unix_us}});
    rtp::SrBuilder sr;
    sr.set_ssrc(impl_->audio_ssrc)
      .set_ntp_timestamp(sr_ntp.value)
      .set_rtp_timestamp(impl_->audio_rtp_timestamp)
      .set_sender_packet_count(impl_->audio_packet_count)
      .set_sender_octet_count(impl_->audio_octet_count);
    const core::ByteBuffer sr_bytes = sr.build();
    if (sr_bytes.empty()) return;

    // RTCP leaves the engine as SRTCP (RFC 3711 §3.4) once SRTP is keyed,
    // which is a *different* crypto context from SRTP — using protect_rtp
    // here would produce a packet the peer's unprotect_rtcp rejects, and
    // the SR would be silently dropped.
    core::ByteSpan out(sr_bytes.data(), sr_bytes.size());
    std::vector<std::uint8_t> enc;
    if (impl_->srtp_installed && impl_->srtp) {
        auto* sess = impl_->srtp->get_session(impl_->audio_ssrc,
                                              /*outgoing=*/true);
        if (!sess) return;   // SRTP installed but no keying yet — skip.
        auto r = sess->protect_rtcp(out);
        if (!r) {
            core::log::Logger::instance().warn(
                "engine[timeline]: protect_rtcp failed; Sender Report dropped");
            return;
        }
        enc.assign(r.value().data(),
                   r.value().data() + r.value().size());
        out = core::ByteSpan(enc.data(), enc.size());
    }

    plugins::BufferView bv{out.data(), out.size()};
    if (ice_t_->send(bv, {}) != 0) {
        core::log::Logger::instance().warn(
            "engine[timeline]: Sender Report send failed");
        return;
    }

    // ---- Tell the feedback collector about our own Sender Report ---------
    //
    // The Receiver Report the peer sends back describes *our* stream, and
    // its LSR / DLSR pair only becomes a round-trip time when the
    // collector knows when that report left (RFC 3550 §6.4.1).  The
    // middle 32 bits of the NTP timestamp we put on the wire are what
    // the peer will echo back as its LSR, so the collector matches the
    // two.
    if (net_feedback_) {
        net_feedback_->on_local_sender_report(sr_ntp.mid_32(),
                                              core::SteadyClock::now());
    }
}


void NimRTCEngine::observe_timeline(const rtp::PacketView& pv) noexcept {
    // No negotiated id => the peer never advertised abs-send-time for its
    // sending direction, so there is no element to look for. Walking the
    // block to discover that on every packet would be wasted work, so
    // skip straight out.
    const std::uint16_t ext_id = impl_->timeline.remote_abs_send_time_id;
    if (ext_id == 0 || !pv.extension.has_value()) {
        return;
    }

    auto elem = rtp::find_extension_element(pv.extension->data,
                                            pv.extension->type,
                                            ext_id);
    if (!elem) {
        return;  // NotFound (peer sent no abs-send-time) or malformed.
    }
    const core::ByteSpan e = elem.value();
    if (e.size() < 3) {
        return;
    }

    // 24-bit big-endian sender send-instant counter, 65536 Hz.
    const std::uint32_t ast24 =
        (static_cast<std::uint32_t>(e[0]) << 16) |
        (static_cast<std::uint32_t>(e[1]) <<  8) |
         static_cast<std::uint32_t>(e[2]);

    // Two clocks, one instant: the monotonic reading is what the tracker
    // needs (it combines it with the monotonic origin to place our own
    // clock on the peer's wall-clock timeline), and the wall-clock
    // reading is what the one-way delay is computed from (arrival −
    // capture, both on the Unix timeline).  The wall-clock value is
    // derived from the monotonic one through the origin sampled at
    // open(), so the two never disagree by more than the sampling jitter.
    const std::int64_t steady_us = detail::steady_now_us();
    const std::int64_t unix_us = impl_->timeline.boot_epoch_offset_us + steady_us;

    // The 7-argument overload records the (rtp_ts, ast) observation itself
    // and refines the SR-based estimate per ADR-017 §"Decision 2". The
    // `true` is exact: we only reach this point after resolving the
    // negotiated element from the wire, so the packet really does carry it.
    // Without the flag the call would resolve to the 6-argument overload,
    // which ignores the field entirely — no observation recorded, no anchor
    // established, and the refinement silently dead.
    // clock_rate_hz = 0 => use the rate configured via set_ssrc_clock_rate()
    // for this SSRC (the engine wires that from the SDP rtpmap).
    //
    // The skew out-parameter is the inter-clock discrepancy in the
    // sense of RFC 3550 §8.1 (ADR-017 §"Decision 4").  It is only
    // meaningful when the engine set a monotonic origin — which it always
    // does at open() — so the value is captured and published rather than
    // discarded.
    std::int64_t skew_us = 0;
    auto cap = impl_->timeline.sr_tracker.capture_ts_us_for(
        pv.ssrc, pv.timestamp, ast24,
        /*has_abs_send_time=*/true,
        /*clock_rate_hz=*/0,
        steady_us,
        /*out_sender_recv_skew_us=*/&skew_us);
    if (!cap) {
        return;  // No SR for this SSRC yet, or the packet predates it.
    }

    impl_->timeline.capture_ts_us_by_ssrc[pv.ssrc] = *cap;
    impl_->timeline.arrival_us_by_ssrc[pv.ssrc]    = unix_us;
    impl_->timeline.skew_us_by_ssrc[pv.ssrc]       = skew_us;
    impl_->timeline.last_capture_us                = *cap;
    impl_->timeline.last_arrival_us                = unix_us;
    impl_->timeline.last_skew_us                   = skew_us;
}


void NimRTCEngine::handle_rtp(const rtp::PacketView& pv) noexcept {
    // ---- Cross-end timeline capture reconstruction (ADR-017) -----------
    //
    // Runs before the media demux below and independently of it: both the
    // audio and the video path need the reconstructed capture instant,
    // and neither knows about the seam. Doing it here means a host that
    // only pulls RTP (no decoded frames) still gets timestamps.
    if (config_.timeline_enabled) {
        // Pin this SSRC's media clock rate before anything can ask the
        // tracker for a capture instant — the reconstruction depends on
        // it, and the tracker has no way to learn it from the wire.
        //
        // The rate is a property of the codec the *peer* sends with, so
        // the peer's own `a=rtpmap:` line is the authoritative source
        // (RFC 3550 §6.4.1: the Sender Report does not carry it).  It
        // was collected in `process_remote_sdp()` into
        // `remote_clock_rate_hz`; the fallbacks below only apply when
        // the peer did not declare the payload type.
        //
        // `SenderReportTracker` falls back to 90 kHz when an SSRC has no
        // configured rate, which is correct for video (RFC 6184 H.264) and
        // ~2x wrong for Opus at 48 kHz — the reconstructed capture instant
        // would drift by 87.5% of the media duration.
        //
        // The gate is an explicit per-SSRC set rather than `state_for()`: the
        // tracker returns nullopt until the first SR arrives, so using it here
        // would re-enter this branch on every packet for the whole pre-SR
        // window — the exact hot path this is trying to stay off. The set also
        // keeps a second SSRC on the same PT (RFC 5956 simulcast) on its own
        // entry, so each gets configured once.
        std::uint32_t clock_rate = 0;
        {
            // The negotiated video payload type (C9 / C11) is the one
            // the peer actually sends video on; the locally configured
            // number is only the fallback for a host that never
            // negotiated.
            const std::uint8_t negotiated_vpt =
                impl_->negotiated_video_pt.load(std::memory_order_relaxed);
            const std::uint8_t local_video_pt =
                impl_->video_payload_type != 0
                    ? impl_->video_payload_type
                    : config_.video_receiver_tuning.payload_type;
            auto rit = impl_->timeline.remote_clock_rate_hz.find(pv.payload_type);
            if (rit != impl_->timeline.remote_clock_rate_hz.end()) {
                clock_rate = rit->second;
            } else if (pv.payload_type == local_video_pt ||
                       (negotiated_vpt != 0 && pv.payload_type == negotiated_vpt)) {
                clock_rate = 90000;                    // RFC 6184 §5.6 H.264
            } else {
                clock_rate = config_.audio_codec.clock_rate;  // 48 kHz for Opus
            }
        }
        if (clock_rate != 0 &&
            impl_->timeline.clock_rate_pinned_ssrcs.insert(pv.ssrc).second) {
            impl_->timeline.sr_tracker.set_ssrc_clock_rate(pv.ssrc, clock_rate);
        }

        // The abs-send-time refinement only applies when the peer actually
        // negotiated the extension; `observe_timeline()` re-checks that and
        // returns immediately when it did not.
        observe_timeline(pv);
    }

    // ---- DTMF (RFC 4733) -------------------------------------------------
    //
    // A telephone-event payload is *not* audio: it is a four-byte event
    // header that rides on the audio m-line (RFC 4733 §2.1).  It
    // therefore has an exit of its own, next to the audio routing
    // table (the session capability plan, D1) — handing it to an
    // `IAudioDecoder` would push four bytes of event header into an
    // Opus decoder.
    //
    // The branch sits *before* the unknown-payload-type check, and that
    // placement is the whole of decision D4:
    //
    //   * DTMF negotiated — the payload is decoded and reported to the
    //     host, and it is *not* counted as an unknown audio payload
    //     type;
    //   * DTMF not negotiated — the payload type is in no routing table,
    //     so the check below counts it as unknown and drops it, exactly
    //     as it did before DTMF existed.
    //
    // The packet is still *noted* as a received packet of the audio
    // stream: RFC 4733 §2.1 puts the event packets in the audio
    // stream's sequence space, so skipping them in the bookkeeping
    // would report the key presses as lost audio.
    if (impl_->pt_routing_active && negotiated_dtmf_pt_ != 0 &&
        pv.payload_type == negotiated_dtmf_pt_) {
        const auto dtmf_recv_ts_us = plugins::TimestampUs{
            static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    core::SteadyClock::now().time_since_epoch()).count())};
        note_inbound_audio_packet(pv.ssrc, pv.seq, pv.timestamp,
                                  dtmf_recv_ts_us);
        handle_dtmf_payload(pv.ssrc, pv.payload);
        return;
    }

    // ---- Payload-type routing (B-4) --------------------------------------
    //
    // The routing table comes from the peer's SDP: only the payload
    // types the peer declared on the matching m-line are ours.  A
    // payload type in neither table is an anomaly — it is counted and
    // dropped with `kAudioPayloadTypeUnknown`, never decoded.  The old
    // rule ("anything that is not the video payload type is audio")
    // pushed telephone-event, RED and unsupported-codec packets into
    // the audio pipeline (defect F-8).
    //
    // Before a remote description has been processed the table is empty
    // and the demux keeps its historical behaviour, so a host that
    // drives the engine without an SDP exchange — the test seams, the
    // loopback harness — is unaffected.
    bool route_to_video = false;
    bool route_to_audio = false;

    if (impl_->pt_routing_active) {
        route_to_audio = impl_->negotiated_audio_pt != 0 &&
                         pv.payload_type == impl_->negotiated_audio_pt;
        // Video: only the *negotiated* payload type may reach the video
        // pipeline.  The old rule — "any payload type the peer declared
        // on its video m-line" — let a peer send VP8 on a payload type
        // we never agreed to have it decoded as H.264 (C11).  When no
        // description exchange happened the historical rule still
        // applies, so a host that drives the engine without SDP is
        // unaffected.
        const std::uint8_t negotiated_vpt =
            impl_->negotiated_video_pt.load(std::memory_order_relaxed);
        route_to_video = video_receiver_ && !impl_->video_rx_disabled &&
                         (negotiated_vpt != 0
                              ? pv.payload_type == negotiated_vpt
                              : impl_->video_pt_routes.count(pv.payload_type) > 0);
        if (!route_to_audio && !route_to_video) {
            ++impl_->audio_pt_unknown_count;
            // Spec Part D: per-packet errors are counted but not
            // reported individually — the first occurrence and every
            // 50th one are.  Reporting each drop would flood the host
            // when a peer keeps sending a stream we never negotiated.
            if (impl_->audio_pt_unknown_count == 1 ||
                impl_->audio_pt_unknown_count %
                    detail::kAudioPayloadTypeReportInterval == 0) {
                if (on_error_) {
                    on_error_(core::kAudioPayloadTypeUnknown,
                              "inbound RTP payload type " +
                              std::to_string(pv.payload_type) +
                              " is not in the negotiated routing table");
                }
            }
            return;
        }
    } else {
        const std::uint8_t negotiated_vpt =
            impl_->negotiated_video_pt.load(std::memory_order_relaxed);
        const std::uint8_t local_video_pt =
            impl_->video_payload_type != 0
                ? impl_->video_payload_type
                : config_.video_receiver_tuning.payload_type;
        route_to_video = video_receiver_ &&
                         (pv.payload_type == local_video_pt ||
                          (negotiated_vpt != 0 &&
                           pv.payload_type == negotiated_vpt));
        route_to_audio = !route_to_video;
    }

    if (route_to_video) {
        // Remember the peer's video SSRC: the NACK / PLI we send must
        // name the media source it is about (RFC 4585 §6.2.1).  The peer
        // picks its own SSRC, so it can only be learned from the wire.
        impl_->remote_video_ssrc_ = pv.ssrc;

        plugins::VideoRtpPacket vp;
        vp.ssrc          = pv.ssrc;
        vp.seq           = pv.seq;
        vp.rtp_timestamp = pv.timestamp;
        vp.payload_type  = pv.payload_type;
        vp.marker        = pv.marker;
        vp.recv_ts_us    = plugins::TimestampUs{
            static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    core::SteadyClock::now().time_since_epoch()).count())};
        // BufferView = core::ByteSpan = std::span<const uint8_t>
        vp.payload       = pv.payload;
        video_receiver_->push_rtp(vp, vp.recv_ts_us);
        video_receiver_->tick(vp.recv_ts_us);
        return;
    }

    // ---- Audio path (v1.2.0, audio pipeline WP-7) -----------------------
    //
    // The engine owns neither a jitter buffer nor a decoder: the whole
    // receive pipeline is a *plugin* (spec B-2 / B-11), so the inbound
    // packet goes straight into it and the decoded frames come back
    // through the frame callback.
    //
    // The old path pushed the packet into an engine-owned
    // `jb::JitterBuffer` and then reinterpreted the RTP payload as raw
    // float PCM — it never decoded anything (defect F-1: "the receive
    // path has no decoding at all").  What the host receives now is the
    // *decoded* signal, which is also what the 3A render reference must
    // be (spec A-10).
    if (!audio_receiver_ || impl_->audio_rx_disabled) {
        // The audio track is off: either the host disabled it
        // (`audio_receiver_name == ""`), the negotiation found nothing
        // decodable, or the pipeline could not be created (a hard error
        // that was already reported through `on_error_`).  The packets
        // are counted and dropped — they are never decoded.
        ++impl_->audio_rx_dropped_packets;
        return;
    }

    const auto recv_ts_us = plugins::TimestampUs{
        static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                core::SteadyClock::now().time_since_epoch()).count())};

    // The RTCP receiver report needs the *sequence* bookkeeping of the
    // inbound audio stream (RFC 3550 §6.4.2) — the extended highest
    // sequence number, the cumulative loss and the interarrival
    // jitter.  The pipeline counts packets but does not track the
    // sequence numbers, so the engine does that here (spec B-12).
    note_inbound_audio_packet(pv.ssrc, pv.seq, pv.timestamp, recv_ts_us);

    audio_receiver_->push(pv.payload.data(), pv.payload.size(),
                          pv.timestamp, pv.seq, pv.ssrc, recv_ts_us);
    // The pipeline is clock-driven (spec A-1): pushing the packet only
    // fills the jitter buffer; the playout happens on `tick()`.  We tick
    // here as well so a host that only receives packets (no tick loop)
    // still gets audio, and so the pipeline's playout clock advances in
    // step with the arrival of the media.
    audio_receiver_->tick(recv_ts_us);
}


// -----------------------------------------------------------------------------
// Ref-frame timeline map (v1.1 T1/T2, UNSTABLE)
// -----------------------------------------------------------------------------
//
// Local frame-id <-> capture-ts registry. ref_frame id == frame_seq
// (reusing the STABLE field avoids any ABI break on VideoSourceFrame).
// Same benign single-threaded pattern as the ADR-017 accessors above.

void NimRTCEngine::record_video_ref_frame(std::uint64_t ref_frame_id,
                                          std::int64_t capture_ts_us) noexcept {
    // The table owns the eviction policy: it is bounded, so when the
    // registry is full the oldest id falls out to make room for the
    // newest one.  Recording an id that is already present refreshes it
    // in place (last-writer-wins, insertion order unchanged).
    impl_->ref_frames.record(ref_frame_id, capture_ts_us);
}


std::optional<std::int64_t> NimRTCEngine::ref_frame_to_capture_ts(
   std::uint64_t ref_frame_id) const noexcept {
    if (impl_ == nullptr) return std::nullopt;
    std::int64_t capture_ts_us = 0;
    if (!impl_->ref_frames.lookup(ref_frame_id, capture_ts_us)) {
        return std::nullopt;
    }
    return capture_ts_us;
}


std::optional<std::uint64_t> NimRTCEngine::capture_ts_to_ref_frame(
    std::int64_t capture_ts_us) const noexcept {
    if (impl_ == nullptr) return std::nullopt;
    // The registry used to be a `std::map`, so a timestamp shared by
    // several ids resolved to the *smallest* id.  The table is
    // unordered, so the smallest match is selected explicitly to keep
    // the observable behaviour identical.
    return impl_->ref_frames.find_smallest_with_timestamp(capture_ts_us);
}


std::optional<std::int64_t> NimRTCEngine::data_message_frame_age_us(
    std::uint64_t ref_frame_id) const noexcept {
    const auto ts = ref_frame_to_capture_ts(ref_frame_id);
    if (!ts.has_value()) return std::nullopt;
    const std::int64_t now_us = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            core::SteadyClock::now().time_since_epoch()).count());
    return now_us - *ts;
}


// -----------------------------------------------------------------------------
// Cross-end timeline (ADR-017)
// -----------------------------------------------------------------------------
//
// The engine side of the timeline seam. The reconstruction arithmetic is
// `nimrtc::rtp::SenderReportTracker`; what lives here is the wiring that
// feeds it (inbound RTCP SRs + inbound abs-send-time elements) and the
// read-only diagnostics the public API exposes.
//
// Note on thread safety: the transport callbacks that feed the tracker
// (`handle_inbound` / `handle_rtp`) are already serialised by the
// transport layer onto a single thread, so the tracker itself is only
// ever touched from that one thread. The read-only accessors below read
// the cached `last_*` fields without locking; they are the same benign
// racy-read pattern the rest of this file already uses for diagnostics
// (`stats()`, `last_open_rc()`), and every field they expose is a
// `std::optional<std::int64_t>` written as a single aligned store.
// -----------------------------------------------------------------------------
//
// Absent a negotiated `a=extmap` id for abs-send-time (peer never
// advertised it), the extension is not read and no capture instant is
// reconstructed — `remote_abs_send_time_id` stays 0 and every
// accessor returns `std::nullopt`. That is the correct pre-negotiation
// state, not an error.
// -----------------------------------------------------------------------------

std::uint16_t NimRTCEngine::abs_send_time_ext_id() const noexcept {
    return impl_->timeline.local_abs_send_time_id;
}


std::uint16_t NimRTCEngine::remote_abs_send_time_ext_id() const noexcept {
    return impl_->timeline.remote_abs_send_time_id;
}


std::optional<std::int64_t> NimRTCEngine::last_capture_ts_us() const noexcept {
    return impl_->timeline.last_capture_us;
}

std::optional<std::int64_t>

NimRTCEngine::capture_ts_us_for_ssrc(std::uint32_t ssrc) const noexcept {
    auto it = impl_->timeline.capture_ts_us_by_ssrc.find(ssrc);
    if (it != impl_->timeline.capture_ts_us_by_ssrc.end()) {
        return it->second;
    }
    return std::nullopt;
}


std::optional<std::int64_t> NimRTCEngine::last_one_way_delay_us() const noexcept {
    if (!impl_->timeline.last_capture_us || !impl_->timeline.last_arrival_us) {
        return std::nullopt;
    }
    // arrival − capture, i.e. elapsed time between the sender's capture
    // instant and our receipt of the packet. This is the sign that makes
    // the value usable as a latency budget: positive when the packet took
    // time to reach us, zero at the theoretical floor, negative only if
    // the two clocks disagree (which is itself worth surfacing).
    //
    // The previous order (capture − arrival) reported the negation, so a
    // 4 s transit came back as −4 s — a number no consumer of a latency
    // metric can act on.
    return *impl_->timeline.last_arrival_us - *impl_->timeline.last_capture_us;
}

std::optional<std::int64_t>

NimRTCEngine::last_sender_recv_skew_us() const noexcept {
    return impl_->timeline.last_skew_us;
}

std::optional<std::int64_t>

NimRTCEngine::sender_recv_skew_us_for_ssrc(std::uint32_t ssrc) const noexcept {
    auto it = impl_->timeline.skew_us_by_ssrc.find(ssrc);
    if (it != impl_->timeline.skew_us_by_ssrc.end()) {
        return it->second;
    }
    return std::nullopt;
}

}  // namespace nimrtc::engine
