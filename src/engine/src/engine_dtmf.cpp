/**
 * @file src/engine/src/engine_dtmf.cpp
 * @brief The engine's DTMF plane (v1.2.0, session capability plan D1-D4).
 *
 * RFC 4733 carries DTMF digits *in band*: a `telephone-event` payload
 * type rides on the audio m-line, carrying a four-byte header instead
 * of audio samples.  This file holds everything the engine needs on
 * both sides of that path:
 *
 *   - **the plugin resolution** (`resolve_dtmf_codec()`), so the engine
 *     reaches the codec through the registry's DTMF slot and never
 *     names a concrete module (constraint L4);
 *   - **the send path** (`send_dtmf()`), which builds the RTP packet
 *     and hands it to the scheduler;
 *   - **the receive path** (`handle_dtmf_payload()`), which decodes an
 *     inbound telephone-event payload, accumulates the duration across
 *     the repeats RFC 4733 §2.5.1.2 describes, and reports the
 *     completed event to the host.
 *
 * ## The three negotiation states
 *
 * DTMF is **off by default** (decision OD-B of the session capability
 * plan), and the routing table has to behave differently in each of
 * the three states.  This is decision D4, and it is the reason the
 * DTMF branch sits *before* the unknown-payload-type check in
 * `handle_rtp()`:
 *
 * | state                        | a telephone-event packet    |
 * |------------------------------|-----------------------------|
 * | DTMF off, peer sent no PT    | counted as an unknown audio payload type and dropped |
 * | DTMF on, peer sent no PT     | counted as an unknown audio payload type and dropped |
 * | DTMF on, peer sent a PT      | decoded and reported to the host |
 *
 * The third row is the only one where the payload is *not* an
 * anomaly: the peer declared the payload type in its description, so
 * the engine knows what it is.
 *
 * ## The RTP timestamp
 *
 * RFC 4733 §2.1 requires that the event packets use "the same sequence
 * number and timestamp base as the regular audio channel".  The
 * telephone-event packets therefore share the audio sequence space and
 * the audio timestamp — they are sent on the same SSRC.  The one
 * difference is the marker bit: RFC 4733 §2.2.2 requires the *first*
 * packet of an event to carry it, which is what tells the peer "a new
 * event starts here".
 */

#include "engine_impl.hpp"

namespace nimrtc::engine {

namespace {

/** The largest RFC 4733 event code (16, the "flash" event).
 *
 *  RFC 4733 §2.1 defines the codes 0..16; anything above is not an
 *  event we know how to send. */
constexpr std::uint8_t kMaxDtmfEventCode = 16;

/** The RTP payload type the telephone-event stream uses when the SDP
 *  negotiated one.
 *
 *  RFC 4733 §2.3.2 negotiates the payload type out of band; 126 is
 *  the value Chrome uses and the engine's default
 *  (`EngineConfig::dtmf_payload_type`). */
constexpr std::uint8_t kDefaultDtmfPayloadType = 126;

/** The SRTP authentication tag size, in bytes.
 *
 *  The scheduler hands out packet buffers that already reserve room for
 *  the SRTP auth tag, so the DTMF send path asks for
 *  `payload + tag` bytes.  It mirrors the same constant in
 *  `engine_media.cpp`: the two paths protect different packets but with
 *  the same profile, so the tag size is the same. */
constexpr std::size_t kSrtpAuthTagBytes = 10;

} // anonymous namespace

// ---------------------------------------------------------------------------
// Plugin resolution
// ---------------------------------------------------------------------------

void NimRTCEngine::resolve_dtmf_codec() noexcept {
    dtmf_codec_.reset();

    // DTMF is off by default (OD-B): the host has to ask for it, so
    // the common path does not even look at the registry.
    if (!config_.dtmf_enabled) return;

    // An empty plugin id disables the DTMF path explicitly, the same
    // way `audio_decoder_name == ""` disables the audio receive path.
    if (config_.dtmf_codec_name.empty()) {
        core::log::Logger::instance().info(
            "engine[dtmf]: DTMF is disabled by configuration "
            "(dtmf_codec_name is empty)");
        return;
    }

    // The codec is resolved through the registry's DTMF slot, so the
    // answer reflects the plugins the host really installed.  A
    // missing codec is a *silent* degradation: DTMF is an optional
    // capability, and a host that enabled it without building the
    // module should get "DTMF does not work here" rather than a
    // startup failure of the whole engine.
    auto& reg = core::PluginRegistry::instance();
    const plugins::IDtmfCodecFactory* factory =
        pal::resolve_dtmf(config_.dtmf_codec_name);
    if (factory == nullptr) {
        core::log::Logger::instance().warn(
            std::string("engine[dtmf]: the DTMF codec plugin '") +
            std::string(config_.dtmf_codec_name) +
            "' is not registered — the DTMF path is disabled");
        return;
    }

    std::unique_ptr<plugins::IDtmfCodec> codec(factory->create());
    if (!codec) {
        core::log::Logger::instance().warn(
            std::string("engine[dtmf]: the DTMF codec plugin '") +
            std::string(config_.dtmf_codec_name) +
            "' returned null — the DTMF path is disabled");
        return;
    }

    if (codec->open() != plugins::kOk) {
        core::log::Logger::instance().warn(
            std::string("engine[dtmf]: the DTMF codec plugin '") +
            std::string(config_.dtmf_codec_name) +
            "' failed to open — the DTMF path is disabled");
        return;
    }

    dtmf_codec_ = std::move(codec);
}

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

void NimRTCEngine::set_on_dtmf(DtmfEventCallback cb) noexcept {
    on_dtmf_ = std::move(cb);
}

bool NimRTCEngine::send_dtmf(std::uint8_t event_code,
                             std::uint16_t duration_ms) noexcept {
    // --- Preconditions -------------------------------------------------
    //
    // Every rejection is silent by design: `send_dtmf()` returns a
    // bool, and the host is expected to check it.  A DTMF event that
    // cannot be sent is not an engine fault — the host simply asked
    // for something the session cannot do.
    if (!is_open()) return false;
    if (!dtmf_codec_) return false;
    if (event_code > kMaxDtmfEventCode) return false;

    // The peer must have negotiated a telephone-event payload type:
    // sending one on a payload type the peer did not declare would
    // put an unnegotiated stream on the wire (RFC 3264 §6.1).
    const std::uint8_t pt = negotiated_dtmf_pt_;
    if (pt == 0) return false;

    // A zero duration means "the event lasts forever" on the wire
    // (RFC 4733 §2.3.5), which is a *state* — and a key press is not
    // a state.  The RFC also forbids sending a zero duration for a
    // non-state event, so a zero duration is rejected here rather than
    // silently sent.
    if (duration_ms == 0) return false;

    // --- Encode --------------------------------------------------------
    plugins::DtmfEvent event;
    event.code        = event_code;
    event.duration_ms = duration_ms;
    // The event ends here: this is the final packet of the event, so
    // it carries the end bit (RFC 4733 §2.5.1.2).
    event.end         = true;

    const std::string payload = dtmf_codec_->encode(event);
    if (payload.empty()) return false;

    // --- Packetise -----------------------------------------------------
    //
    // RFC 4733 §2.1: the event packets share the audio stream's
    // sequence number space and timestamp base.  The marker bit marks
    // the beginning of a new event (RFC 4733 §2.2.2) — and since this
    // is the only packet of the event, it is both the first and the
    // last.
    //
    // The timestamp is the *current* audio timestamp, not an advanced
    // one: the event does not consume audio time, and advancing the
    // audio clock for a DTMF event would leave a gap in the media
    // timeline.
    rtp::PacketBuilder pb;
    pb.set_ssrc(impl_->audio_ssrc)
      .set_seq(impl_->audio_seq++)
      .set_timestamp(impl_->audio_ts)
      .set_payload_type(pt)
      .set_marker(true);

    pb.set_payload(core::ByteSpan(
        reinterpret_cast<const std::uint8_t*>(payload.data()),
        payload.size()));

    auto pkt_buf = pb.build();
    if (pkt_buf.empty()) {
        core::log::Logger::instance().error(
            "engine[dtmf]: the RTP packet could not be built");
        return false;
    }

    // --- Protect and send ----------------------------------------------
    //
    // The telephone-event packet rides the same SRTP context as the
    // audio: the same SSRC, the same session, the same key.
    std::vector<std::uint8_t> send_buf;
    if (scheduler_) {
        send_buf = scheduler_->acquire_packet_buffer(
            pkt_buf.size() + kSrtpAuthTagBytes);
    }

    const core::ByteSpan out_payload(pkt_buf.data(), pkt_buf.size());
    if (impl_->srtp_installed && impl_->srtp) {
        auto* sess = impl_->srtp->get_session(impl_->audio_ssrc,
                                               /*outgoing=*/true);
        if (!sess) return false;
        auto enc = sess->protect_rtp(out_payload, impl_->audio_ssrc,
                                     impl_->audio_ts);
        if (!enc) return false;
        send_buf.assign(enc.value().data(),
                        enc.value().data() + enc.value().size());
    } else {
        send_buf.assign(out_payload.data(),
                        out_payload.data() + out_payload.size());
    }

    ++impl_->audio_packet_count;
    impl_->audio_octet_count += static_cast<std::uint32_t>(payload.size());

    if (scheduler_) {
        scheduler_->enqueue_owned(plugins::Priority::kAudio,
                                  std::move(send_buf),
                                  plugins::Addr{});
        return true;
    }

    // Fallback: no scheduler — send directly through ICE.
    plugins::BufferView bv{send_buf.data(), send_buf.size()};
    return ice_t_->send(bv, {}) == 0;
}

// ---------------------------------------------------------------------------
// The receive path
// ---------------------------------------------------------------------------

void NimRTCEngine::handle_dtmf_payload(const rtp::PacketView& pv) noexcept {
    // --- Decode --------------------------------------------------------
    plugins::DtmfEvent event;
    const std::string_view payload(
        reinterpret_cast<const char*>(pv.payload.data()),
        pv.payload.size());

    if (!dtmf_codec_ || !dtmf_codec_->decode(payload, event)) {
        // A malformed telephone-event payload is a peer bug or an
        // attack.  It is counted and dropped — never guessed at.
        ++impl_->dtmf_packets_malformed;
        return;
    }

    ++impl_->dtmf_packets_received;

    // --- Accumulate the duration (RFC 4733 §2.5.1.2) ------------------
    //
    // The sender repeats the event packet while the key is held, each
    // repeat carrying a larger duration, and the final packet carries
    // the end bit.  The receiver therefore accumulates across the
    // repeats and reports the event once.
    //
    // The accumulator is per-SSRC, so two telephone-event streams do
    // not mix their durations.
    auto& acc = impl_->dtmf_rx_by_ssrc[pv.ssrc];

    if (!acc.active) {
        // A new event begins here (RFC 4733 §2.2.2: the marker bit
        // marks the beginning of a new event).
        acc.active      = true;
        acc.code        = event.code;
        acc.duration_ms = event.duration_ms;
    } else if (acc.code == event.code) {
        // A repeat of the event in progress: keep the largest duration
        // reported so far.  The duration field is cumulative, so the
        // repeats carry a growing value.
        if (event.duration_ms > acc.duration_ms) {
            acc.duration_ms = event.duration_ms;
        }
    } else {
        // A different event code on the same SSRC: the previous event
        // was never ended (its end packet was lost).  Report the
        // previous one with the duration it accumulated, then start the
        // new event — the alternative is to lose the first key press
        // entirely.
        plugins::DtmfEvent finished;
        finished.code        = acc.code;
        finished.duration_ms = acc.duration_ms;
        finished.end         = true;
        ++impl_->dtmf_events_delivered;
        if (on_dtmf_) on_dtmf_(finished);

        acc.active      = true;
        acc.code        = event.code;
        acc.duration_ms = event.duration_ms;
    }

    if (!event.end) return;   // the event is still in progress

    // --- Report the completed event -------------------------------------
    acc.active = false;

    plugins::DtmfEvent completed;
    completed.code        = acc.code;
    completed.duration_ms = acc.duration_ms;
    completed.end         = true;

    ++impl_->dtmf_events_delivered;
    if (on_dtmf_) on_dtmf_(completed);
}

} // namespace nimrtc::engine
