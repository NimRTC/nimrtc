/**
 * @file src/engine/src/engine_audio_state.hpp
 * @brief PRIVATE engine collaborator: the audio plane's per-engine state.
 *
 * Architecture remediation plan §4.3 (D1 — engine split): this state used
 * to live as forty-odd loose members inside `NimRTCEngine::Impl`, which
 * made the engine's internal header a catch-all and scattered one
 * feature's state across the file.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * ## What lives here
 *
 *   - the **send** state: SSRC, RTP sequence number, RTP timestamp, the
 *     frame accumulator and the send counters;
 *   - the **receive** state: the RFC 3550 Appendix A.1 sequence
 *     bookkeeping the RTCP receiver report needs, the negotiated
 *     payload-type routing, the negotiated Opus format parameters and
 *     the receive counters;
 *   - the **DTMF** (RFC 4733) plane: the per-SSRC receive accumulator and
 *     the transmit queue that emits the repeats;
 *   - the audio half of the outbound Sender Report counters.
 *
 * The media hot path reads and writes these fields on every frame; they
 * are plain members (no indirection) so the access stays a single load.
 *
 * ## Threading
 *
 * Not thread-safe by itself — the engine's contract is that the send
 * path, the receive path and the tick loop are serialised by the
 * caller.  The one exception is `negotiated_pt` on the video side
 * (a separate collaborator) which is an atomic because the negotiation
 * runs on the signalling thread while the packet path stamps the RTP
 * header on another.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include <nimrtc/core/time.hpp>

#include "audio_routing.hpp"

namespace nimrtc::engine::detail {

/** The audio plane's per-engine state. */
struct AudioState {
    // ---- Inbound audio tracking (v1.2.0, audio pipeline WP-8) ---------
    //
    // The RTCP receiver report (RFC 3550 §6.4.2) needs the *sequence*
    // bookkeeping of the inbound audio stream: the extended highest
    // sequence number, the cumulative packet loss and the interarrival
    // jitter.  The receive pipeline does not track those — it counts
    // packets and plays out frames — so the engine keeps the numbers
    // here, next to the statistics it publishes (spec B-12 / B-13).
    //
    // The state follows RFC 3550 Appendix A.1: a *source* (SSRC) has
    // one sequence space, and a stream that restarts (a new SSRC, or a
    // large backward jump) resets it.
    struct AudioRxState {
        /** The SSRC of the inbound audio stream; 0 = nothing seen yet. */
        std::uint32_t ssrc = 0;

        /** The first sequence number of the current sequence space
         *  (RFC 3550's `base_seq`). */
        std::uint16_t base_seq = 0;

        /** The extended highest sequence number received
         *  (RFC 3550's `max_seq`): cycles in the high 16 bits. */
        std::uint32_t max_seq = 0;

        /** Number of packets received in the current sequence space. */
        std::uint64_t received = 0;

        /** The `expected` / `received` counters as of the last report
         *  (RFC 3550 A.3) — the fraction-lost computation is a
         *  *difference* between two intervals, so the previous values
         *  have to be remembered. */
        std::uint64_t expected_prior = 0;
        std::uint64_t received_prior = 0;

        /** Interarrival jitter, in RTP timestamp units
         *  (RFC 3550 §6.4.1). */
        std::uint32_t jitter = 0;

        /** The transit time (arrival − RTP timestamp) the last packet
         *  produced.  The jitter estimate is the EWMA of the
         *  *differences* of the transit times.
         *
         *  The value is a *signed* 32-bit number because the transit
         *  time is modular over 2^32 (RFC 3550 A.1): the difference
         *  between two transit times has to be taken modulo 2^32 as
         *  well, or a wrap of the RTP timestamp would look like a
         *  multi-second arrival gap. */
        std::int32_t  last_transit = 0;
        bool          have_transit = false;

        /** The middle 32 bits of the NTP timestamp of the last Sender
         *  Report received for this SSRC (the RR's LSR field), and
         *  the instant we received it (the DLSR field). */
        std::uint32_t last_sr_lsr = 0;
        std::int64_t  last_sr_recv_us = 0;
    } audio_rx;

    /** Our own SSRC as the sender of the RTCP reports (RFC 3550 §6.1).
     *
     *  The receiver report is sent by *us* as the receiver of the
     *  media, so it needs an SSRC of its own — distinct from the media
     *  SSRCs, which identify the streams rather than the reporter.
     *  0 until the first report is built. */
    std::uint32_t rtcp_sender_ssrc_ = 0;

    /** When the last receiver report went out (steady clock). */
    std::chrono::steady_clock::time_point rr_last_sent{};

    // ---- Negotiated payload-type routing (B-4) --------------------------
    //
    // The table below is the *negotiated* routing: the payload types
    // the peer declared on the matching m-line of its description, and
    // which we therefore accept on the wire.  It replaces the old rule
    // "anything that is not the video payload type is audio" (defect
    // F-8), which pushed telephone-event, RED and unsupported-codec
    // packets into the audio pipeline.
    //
    // The table is empty until a remote description has been processed.
    // While it is empty the demux keeps its historical behaviour, so a
    // host that drives the engine without an SDP exchange (the test
    // seams, the loopback harness) is unaffected.
    std::map<std::uint8_t, AudioPayloadRoute> audio_pt_routes;

    /** The payload type the negotiation selected for the inbound audio
     *  stream; 0 when nothing was selected. */
    std::uint8_t negotiated_audio_pt = 0;

    /** The Opus format parameters the peer sent for the selected
     *  payload type, parsed (B-4).  WP-7 feeds them into the receive
     *  pipeline's configuration. */
    OpusFmtpParams negotiated_opus_fmtp;

    /** The format parameters the answer echoed back — the negotiated
     *  subset of `negotiated_opus_fmtp` (B-4). */
    std::string negotiated_opus_fmtp_text;

    /** The negotiation concluded that we cannot decode the peer's audio.
     *
     *  Set when every payload type the peer offered names an encoding
     *  that has no decoder plugin (A-9 / B-4).  The receive track is
     *  then disabled: inbound audio packets are counted and dropped
     *  instead of being handed to a decoder that does not exist. */
    bool audio_rx_disabled = false;

    /** Inbound audio packets dropped because the audio track is off.
     *
     *  The track is off when the host disabled it, when the negotiation
     *  found nothing decodable, or when the receive pipeline could not
     *  be created.  The packets are counted and never decoded. */
    std::uint64_t audio_rx_dropped_packets = 0;

    /** The `kAudioCodecUnsupported` error was already reported for this
     *  session.  The condition is reported exactly once (spec Part D). */
    bool audio_codec_unsupported_reported = false;

    /** Inbound audio packets dropped because their payload type is not
     *  in the negotiated routing table (`kAudioPayloadTypeUnknown`). */
    std::uint64_t audio_pt_unknown_count = 0;

    /** The extmap id the peer advertised for the ssrc-audio-level
     *  extension; 0 when the peer did not advertise it. */
    int remote_audio_level_ext_id = 0;

    /** The extmap id WE advertise for the ssrc-audio-level extension
     *  (B-14).  It is also the id our outbound packets will carry.
     *
     *  RFC 5285 §6 negotiates the id per direction.  When we answer a
     *  peer's offer we echo the id the peer advertised; when we are the
     *  offerer we keep the id we advertised and read the peer's echo
     *  back. */
    int local_audio_level_ext_id = 0;

    /** True once a remote description has been processed.
     *
     *  While this is false the demux keeps its historical behaviour
     *  ("not the video payload type ⇒ audio"), so a host that drives
     *  the engine without an SDP exchange — the test seams, the
     *  loopback harness — is unaffected by the routing table. */
    bool pt_routing_active = false;

    // ---- Audio send state (A-6: per engine, never shared) -------------
    //
    // The SSRC, the RTP sequence number and the RTP timestamp used to
    // live in function-local `static std::atomic` variables inside
    // send_audio().  Two engines in one process therefore shared one
    // sequence space and one SSRC (defect F-12), and the SDP generator
    // had to hard-code the same value to stay in lockstep with the
    // packet path.  They are per-engine members now, and the SDP
    // generator reads `audio_ssrc` — the same member the packet path
    // stamps — so the two cannot drift apart again.
    std::uint16_t audio_seq = 0;
    std::uint32_t audio_ts  = 0;

    // ---- Frame accumulator (B-6) --------------------------------------
    //
    // send_audio() accepts any block size.  The processed PCM is
    // appended here and only whole frames are encoded, so the wire
    // format does not depend on how the host chunks its capture.
    // The buffer holds interleaved samples; `audio_accum_frames` counts
    // the per-channel samples already in it.
    std::vector<float> audio_accum;
    std::size_t audio_accum_frames = 0;

    // ---- Audio send counters (A-8) -----------------------------------
    //
    // An encode failure must never disappear: it is counted here and
    // reported through on_error (throttled to one report per second so
    // a persistently broken stream cannot flood the host's callback).
    std::uint64_t audio_frames_sent   = 0;
    std::uint64_t audio_bytes_sent    = 0;
    std::uint64_t audio_encode_errors = 0;
    std::uint64_t audio_samples_in    = 0;
    std::chrono::steady_clock::time_point audio_last_error_report{};

    // ---- Audio receive counters (WP-7) ---------------------------------
    //
    // The pipeline owns the detailed statistics (packets, bytes, lost,
    // jitter); these four count what the *engine* handed to the host,
    // which is what the engine-level statistics report (spec B-13).
    std::uint64_t audio_frames_received       = 0;
    std::uint64_t audio_samples_received      = 0;
    std::uint64_t audio_frames_concealed      = 0;
    std::uint64_t audio_frames_fec_recovered  = 0;

    // ---- DTMF receive state (v1.2.0, session capability plan D1 / D2) --
    //
    // The state of the telephone-event receive path.  RFC 4733 §2.5.1.2
    // describes the *sender* as repeating the event packet while the key
    // is held, each repeat carrying a larger duration; the receiver
    // therefore accumulates the duration across the repeats and reports
    // the event once, when the packet with the end bit arrives.
    //
    // The accumulator is per-SSRC: a peer may run more than one
    // telephone-event stream, and mixing their durations would report
    // key presses that never happened.
    struct DtmfRxState {
        /** The event code of the event in progress. */
        std::uint8_t  code = 0;
        /** The largest duration reported so far, in milliseconds. */
        std::uint16_t duration_ms = 0;
        /** True when an event is in progress (a packet was seen that did
         *  not carry the end bit). */
        bool          active = false;
    };
    std::map<std::uint32_t, DtmfRxState> dtmf_rx_by_ssrc;

    /** The number of inbound telephone-event packets the engine decoded.
     *
     *  This is the D4 counter: a telephone-event payload counts as
     *  "unknown audio payload type" only when DTMF was *not* negotiated
     *  (decision D4 of the session capability plan). */
    std::uint64_t dtmf_packets_received = 0;

    /** The number of inbound telephone-event packets that were
     *  malformed and therefore dropped (a wrong length, an event code
     *  outside 0..16). */
    std::uint64_t dtmf_packets_malformed = 0;

    /** The number of completed DTMF events reported to the host. */
    std::uint64_t dtmf_events_delivered = 0;

    /** The number of outbound telephone-event packets the engine sent. */
    std::uint64_t dtmf_packets_sent = 0;

    // ---- DTMF transmit state (RFC 4733 §2.5) -------------------------
    //
    // RFC 4733 §2.5.1.2 describes the *sender*: while the key is held
    // the event packet is repeated, each repeat carrying a larger
    // duration, and the packet that closes the event carries the
    // end-of-event bit.  A single packet would therefore tell the peer
    // nothing about how long the key was held — and a peer that
    // expects the repetition treats a lone packet as a truncated
    // event.
    //
    // The engine therefore *queues* the event and lets the tick loop
    // emit the repeats at the RFC's interval.  The queue is a deque
    // because a host may press the next key before the previous event
    // has finished, and the events must not overtake each other.
    struct DtmfTxEvent {
        /** The RFC 4733 event code of the key press. */
        std::uint8_t  code = 0;
        /** The total duration of the key press, in milliseconds. */
        std::uint16_t duration_ms = 0;
        /** How many packets of this event have been sent already. */
        int            packets_sent = 0;
        /** The instant the event was queued. */
        core::SteadyClock::time_point start{};
        /** The instant the next packet may be sent. */
        core::SteadyClock::time_point next_send{};
    };
    std::deque<DtmfTxEvent> dtmf_tx_queue;

    // ---- The audio half of the outbound Sender Report -----------------
    //
    // The `sr_*` counters carry the values an SR body reports for the
    // audio stream (RFC 3550 §6.4.1 sender info).  The audio stream is
    // the SR's own media source because it is always the first one out.
    std::uint32_t audio_ssrc            = 0;
    std::uint32_t audio_rtp_timestamp   = 0;
    std::uint32_t audio_packet_count    = 0;
    std::uint32_t audio_octet_count     = 0;

    /** Wall-clock instant (µs since the Unix epoch) of the **last outbound
     *  audio packet**, sampled at the very moment its abs-send-time
     *  element was stamped.
     *
     *  The Sender Report pairs an NTP instant with an RTP timestamp, and
     *  for the pair to describe *one* point on the media timeline the two
     *  must refer to the same moment.  RFC 3550 §6.4.1 words the NTP
     *  field as "the wallclock time when the report was sent" while the
     *  RTP timestamp is that of the last packet sent — a pairing that is
     *  internally inconsistent by exactly the gap between the two events.
     *
     *  That gap is not a rounding detail: with a 1 s Sender-Report
     *  cadence it is up to a full second, and with continuous audio it
     *  is one frame interval.  A receiver that reconstructs the capture
     *  instant from the pair inherits the whole offset — every packet
     *  comes out systematically late, and the one-way delay reads a
     *  second too small.  The abs-send-time refinement does not rescue
     *  it either: the refinement adds the *elapsed* time since the
     *  anchor to the SR's NTP instant, so a late NTP instant stays late.
     *
     *  So the engine stamps the send instant on the packet itself and
     *  reuses it in the SR.  The SR's NTP field then denotes the exact
     *  instant of the packet whose RTP timestamp it carries — the same
     *  instant the abs-send-time field encodes, to within the 15.26 µs
     *  abs-send-time quantisation.  The peer's reconstruction is
     *  therefore free of the report-cadence bias, and the one-way delay
     *  measures the wire, not the report schedule.
     *
     *  Zero means "no packet sent yet"; the SR path already refuses to
     *  emit a report before the first packet. */
    std::int64_t audio_last_send_unix_us = 0;
};

}  // namespace nimrtc::engine::detail
