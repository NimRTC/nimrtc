# Plan: cross-end timeline engine wiring (ADR-017 follow-up)

| | |
|---|---|
| Status | **landed (v1.0.0-rc.1)** — remaining work tracked below |
| Decision record | `docs/adr/ADR-017-cross-end-timeline.md` |
| Owns | `src/engine/` timeline wiring; the RTP-side arithmetic stays in `src/modules/rtp/` |

## What this plan covers

ADR-017 defines the timeline seam in `src/modules/rtp/timeline.hpp`: NTP↔RTP-TS
mapping from RTCP Sender Reports, with WebRTC abs-send-time refining the
estimate. That half is done. This plan covers the **engine** half — turning
`EngineConfig::timeline_enabled` into actual behaviour.

## Landed in this cycle

| Piece | Where |
|---|---|
| `EngineConfig::timeline_enabled` + `sr_period_ms` | `engine.hpp` |
| Offer advertises `a=extmap` for abs-send-time | `create_offer()` |
| Answer echoes it with the same id | `process_remote_sdp()` (answer branch) |
| Peer id parsed from the offer's `a=extmap` | `process_remote_sdp()` |
| Outbound abs-send-time on built RTP packets | `PacketBuilder::set_abs_send_time(t, id)` |
| Outbound SR on the `sr_period_ms` cadence | `Impl::sr_last_sent` / `sr_send_buf` |
| Inbound RTCP demux (compound SR walk) | `handle_rtcp()` |
| Inbound SR ingestion | `sr_tracker.on_sender_report()` |
| Inbound abs-send-time extraction | `observe_timeline()` |
| Public read accessors | `last_capture_ts_us()` / `capture_ts_us_for_ssrc()` / `last_one_way_delay_us()` |

Verified: `ctest -C Release` → 590/590 pass on MSVC 14.43 / Release
(586 pre-existing + 4 new `EngineTimeline` cases).

### Engine-level test — **landed**

`tests/test_engine_timeline.cpp` (4 cases, ctest #555–558) covers the
wiring this plan describes:

| Case | Pins |
|---|---|
| `DisabledEngineAdvertisesNoAbsSendTimeExtmap` | flag off ⇒ no `a=extmap` in the offer, both extension ids report 0 |
| `EnabledEngineAdvertisesAbsSendTimeExtmap` | flag on ⇒ offer carries the URI **at the id `abs_send_time_ext_id()` reports** (the silent "present but wrong id" bug, RFC 5285 §6) |
| `ReconstructionAccessorsAreEmptyWithoutSenderReport` | no inbound SR ⇒ all three accessors return `nullopt`; abs-send-time alone may never fabricate an epoch |
| `TimelineIsOptInByDefault` | `timeline_enabled` defaults false; `sr_period_ms` defaults to `kSrPeriodMs` so the two cannot drift |

### Known gaps

### 1. Per-SSRC clock rate is not wired from SDP — **correctness bug**

`SenderReportTracker` extrapolates against a per-SSRC media clock rate. That
rate is *not* in the Sender Report (RFC 3550 §6.4.1 carries only the RTP
timestamp); it comes from the SDP `a=rtpmap` line. The engine never calls
`set_ssrc_clock_rate()`, so every SSRC falls back to the 90 kHz default.

For video that is accidentally correct. For 48 kHz audio it is wrong by
90000/48000 ≈ 1.875×: every audio capture instant is stretched by that factor,
so `last_capture_ts_us()` reports a time well in the future and
`last_one_way_delay_us()` is garbage.

**This is the one gap that makes the feature actively misleading rather than
merely incomplete.** An agent profile driving closed-loop latency on the audio
path would act on a wrong number.

Fix: on SDP processing, walk each media section's `rtpmap`, and for the SSRC
bound to that m-line call `set_ssrc_clock_rate(ssrc, clock_rate)`. The RTP
layer does not currently carry an SSRC→m-line mapping, so the pragmatic first
cut is to apply the audio section's rate to the audio SSRC and the video
section's to the video SSRC, using the configured SSRCs from
`EngineConfig::audio_codec` / `video_sender_tuning`.

### 2. The reconstruction path itself is untested

`test_engine_timeline.cpp` covers the gate, the SDP negotiation, and
the empty-state invariant — but not the reconstruction. Nothing drives
a synthetic SR + an abs-send-time-bearing RTP packet through
`handle_rtcp()` / `observe_timeline()` and asserts the returned capture
instant. That is precisely the gap that would have caught #1.

### 3. `sender_recv_skew_us` is always 0

Reserved for v1.1 by ADR-017 Decision 4 — needs paired local-monotonic
samples at both the SR instant and the RTP instant threaded through.

## Reference-citation corrections

While landing this, the RFC numbers used for abs-send-time across the codebase
were corrected. Both were wrong:

- **RFC 6051** is *"Rapid Synchronisation of RTP Flows"* (Perkins/Schierl,
  2010) — a different document, not abs-send-time.
- **RFC 9143** is *"Negotiating Media Multiplexing Using SDP"* (Holmberg et
  al., 2022) — SDP `m=` multiplexing, not transport-wide-CC and not
  abs-send-time.

abs-send-time comes from the expired `draft-ietf-avtcore-abs-send-time` and is
a de-facto WebRTC standard with no RFC number; cite the URI. Transport-wide
CC feedback is **RFC 8888** (that citation was also wrong). RFC 5285 / RFC 8285
cover the one-byte and two-byte header-extension forms and were already
correct.

## Next steps

1. Wire `set_ssrc_clock_rate()` from SDP `rtpmap` (gap #1) — **do this first**,
   it is a correctness fix.
2. Extend `test_engine_timeline.cpp` with a reconstruction case (gap #2) —
   it pins gap #1 closed.
3. v1.1: slope estimation + `sender_recv_skew_us` (ADR-017 Decisions 3–4).