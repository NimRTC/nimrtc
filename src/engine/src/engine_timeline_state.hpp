/**
 * @file src/engine/src/engine_timeline_state.hpp
 * @brief PRIVATE engine collaborator: the ADR-017 cross-end timeline state.
 *
 * Architecture remediation plan §4.3 (D1 — engine split): this state
 * used to live as a dozen loose members inside `NimRTCEngine::Impl`,
 * which made the engine's internal header a catch-all and scattered one
 * feature's state across the file.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * ## What the state is
 *
 * ADR-017's cross-end timeline seam: the four things that hang off it
 * (§"Decision 5" — the engine side):
 *
 *   1. extmap negotiation in create_offer() / process_remote_sdp()
 *      (`local_abs_send_time_id` / `remote_abs_send_time_id`);
 *   2. outbound abs-send-time on every RTP packet we build
 *      (`monotonic_origin_ntp64` anchors the encoder);
 *   3. outbound Sender Reports on `EngineConfig::sr_period_ms`
 *      (`sr_last_sent`, plus the per-stream RTP-TS / packet / octet
 *      counters the SR body carries);
 *   4. inbound SR + abs-send-time into `sr_tracker`, whose output is
 *      exposed through the `capture_ts_us_*` accessors.
 *
 * While `EngineConfig::timeline_enabled` is false, every field here
 * stays at its default and no abs-send-time / SR is ever emitted.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

#include <nimrtc/rtp/timeline.hpp>

namespace nimrtc::engine::detail {

/** The cross-end timeline state.
 *
 *  Not thread-safe: the engine's tick loop is the only writer and the
 *  RTP receive path the only reader, on the same thread. */
struct TimelineState {
    /** The inbound Sender Report / abs-send-time tracker.
     *
     *  It owns the per-SSRC media-clock pinning, so the media hot path
     *  does no per-packet map work for an SSRC it has already seen. */
    rtp::SenderReportTracker sr_tracker;

    /** The RTP header-extension id WE advertise for abs-send-time in our
     *  own SDP.  This is the id the *peer* will expect on the wire, so it
     *  is what outbound packets are built with.  0 = not advertised. */
    std::uint16_t local_abs_send_time_id = 0;

    /** The extension id the PEER advertised for its own abs-send-time.
     *  Inbound packets are parsed with this id.  0 = the peer never
     *  advertised the extension, so no inbound refinement is possible. */
    std::uint16_t remote_abs_send_time_id = 0;

    /** Monotonic-clock origin, shared by the abs-send-time encoder and
     *  `SenderReportTracker::set_monotonic_origin()`.
     *
     *  The value is the wall-clock instant (RFC 5905) that corresponds
     *  to a local **monotonic** reading of zero — i.e. the boot epoch.
     *  A monotonic sample of `m` µs therefore sits at the Unix instant
     *  `unix_us(origin) + m`, which is what both the encoder and the
     *  tracker's skew computation assume.
     *
     *  It is NOT "the wall clock at open()": that value would make the
     *  tracker's `origin + monotonic_reading` double-count the epoch
     *  (the tracker adds the monotonic reading to the origin) and every
     *  skew sample would be off by the whole Unix epoch. */
    rtp::Ntp64 monotonic_origin_ntp64{0};

    /** Local wall clock (µs since Unix epoch) at open() minus the steady
     *  clock reading taken at the same instant.  Lets the abs-send-time
     *  encoder convert a send-time monotonic stamp into Unix-anchored
     *  microseconds without a second clock read per packet.
     *
     *  This is the same quantity as `unix_us(monotonic_origin_ntp64)` —
     *  kept as a plain integer so the per-packet path does not have to
     *  re-derive it from the NTP64. */
    std::int64_t boot_epoch_offset_us = 0;

    /** Last reconstructed capture instant per inbound SSRC (µs since Unix
     *  epoch, on the *peer's* wall clock).  Ordered so `last_capture_ts`
     *  can report the most recently updated stream. */
    std::map<std::uint32_t, std::int64_t> capture_ts_us_by_ssrc;

    /** Local arrival instant (µs since Unix epoch) of the same frame, used
     *  to derive the one-way delay.  Parallel to the map above. */
    std::map<std::uint32_t, std::int64_t> arrival_us_by_ssrc;

    /** Capture instant + arrival of the most recent inbound frame across
     *  all SSRCs — backs `last_capture_ts_us()` / `last_one_way_delay_us()`. */
    std::optional<std::int64_t> last_capture_us;
    std::optional<std::int64_t> last_arrival_us;

    /** Inter-clock skew of the most recent inbound frame (µs) — the
     *  change since the last Sender Report of the offset between our
     *  clock and the peer's (RFC 3550 §8.1).  Backs
     *  `last_sender_recv_skew_us()`. */
    std::optional<std::int64_t> last_skew_us;

    /** Inter-clock skew per inbound SSRC — backs
     *  `sender_recv_skew_us_for_ssrc()`. */
    std::map<std::uint32_t, std::int64_t> skew_us_by_ssrc;

    /** SSRCs whose media clock rate has already been pinned on the tracker.
     *  Membership means "configured once" — the payload-type → clock-rate
     *  resolution in `handle_rtp()` runs only on the first packet of each
     *  SSRC, keeping the media hot path free of per-packet map work. */
    std::set<std::uint32_t> clock_rate_pinned_ssrcs;

    /** Payload type → media clock rate (Hz), taken from the *remote*
     *  SDP's `a=rtpmap:` lines.
     *
     *  The media clock rate is a property of the codec the *peer* sends
     *  with, so the peer's own description is the authoritative source
     *  (RFC 3550 §6.4.1: the Sender Report does not carry it).  A
     *  payload type may appear in several m-sections; the first
     *  declaration wins and a conflicting later one is logged and
     *  ignored — the RTP layer does not track which m-section an SSRC
     *  belongs to, so a per-m-line table could not be consulted
     *  anyway. */
    std::unordered_map<std::uint8_t, std::uint32_t> remote_clock_rate_hz;
};

}  // namespace nimrtc::engine::detail
