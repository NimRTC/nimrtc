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
(586 pre-existing + 4 new `EngineTimeline` cases). Two further registered
cases — `SctpUsrsctpTest.LoopbackDatagramRoundTrip` and
`SctpUsrsctpTest.PartialReliableTtlDrop` — are reported as *not run*:
`tests/test_sctp_usrsctp.cpp` disables them under `#if defined(_WIN32) ||
defined(__APPLE__)` because `usrsctp_connect()` never completes on those
runners. That is an intentional platform skip, not a coverage hole in this
plan.

### Engine-level test — **landed**

`tests/test_engine_timeline.cpp` (4 cases, ctest #558–561) covers the
wiring this plan describes:

| Case | Pins |
|---|---|
| `DisabledEngineAdvertisesNoAbsSendTimeExtmap` | flag off ⇒ no `a=extmap` in the offer, both extension ids report 0 |
| `EnabledEngineAdvertisesAbsSendTimeExtmap` | flag on ⇒ offer carries the URI **at the id `abs_send_time_ext_id()` reports** (the silent "present but wrong id" bug, RFC 5285 §6) |
| `ReconstructionAccessorsAreEmptyWithoutSenderReport` | no inbound SR ⇒ all three accessors return `nullopt`; abs-send-time alone may never fabricate an epoch |
| `TimelineIsOptInByDefault` | `timeline_enabled` defaults false; `sr_period_ms` defaults to `kSrPeriodMs` so the two cannot drift |

### Known gaps

### 1. Per-SSRC clock rate — **closed (landed)**

This was originally recorded here as a correctness bug: the engine never
called `set_ssrc_clock_rate()`, so every SSRC fell back to the 90 kHz
default and a 48 kHz audio SSRC was reconstructed off by 90000/48000 ≈
1.875×.

The fix has since landed, in `NimRTCEngine::handle_rtp()`
(`src/engine/src/engine.cpp`), on the `config_.timeline_enabled` branch
that already ran for every inbound packet:

| Step | Code |
|---|---|
| Resolve the rate from the same payload-type demux used for media routing — video PT ⇒ 90000 (RFC 6184 §5.6 H.264), otherwise `EngineConfig::audio_codec.clock_rate` | `clock_rate` local |
| Pin it on the tracker, once per SSRC | `clock_rate_pinned_ssrcs.insert(...).second` gate → `sr_tracker.set_ssrc_clock_rate(ssrc, clock_rate)` |
| Leave the extrapolation call itself rate-agnostic | `capture_ts_us_for(..., /*clock_rate_hz=*/0, ...)` — 0 means "use the pinned rate" |

Two details worth keeping:

- The pin is **before** `observe_timeline()`, so the very first packet on a
  new SSRC already reconstructs against the right rate instead of the 90 kHz
  fallback.
- The gate is an explicit `std::unordered_set`, **not** the tracker's
  `state_for()` accessor. `state_for()` would insert on lookup, so asking
  "have I pinned this yet?" would itself allocate and dirty the tracker on
  every packet across the whole pre-SR window — the exact hot path this
  avoids. The set also keeps a second SSRC on the same payload type (RFC 5956
  simulcast) on its own entry.

Known residual (v1.2 M1 — closed): the video rate now resolves from the
peer's SDP `a=rtpmap` first (`remote_clock_rate_hz`, collected in
`process_remote_sdp()`), and only falls back to the literal 90000 when the
peer did not declare the payload type. `tests/test_engine_timeline.cpp`
`NonStandardVideoClockRateFromRtpmapWins` pins this: a rewritten
`H264/48000` offer reconstructs 48000 ticks as exactly 1 s (the 90 kHz
fallback would give ~533 ms).

### 2. The reconstruction path itself is untested — **closed (landed)**

`test_engine_timeline.cpp` now has three reconstruction cases that push
real wire bytes through `NimRTCEngine::feed_media_inbound()` — the same
RTCP-demux → RTP-parse → `handle_rtcp()` / `handle_rtp()` tail
`on_transport_recv()` uses, factored out as the private
`dispatch_media_inbound()` so the test cannot drift onto a parallel
implementation:

| Case | What it pins |
|---|---|
| `SenderReportFromTheWireAnchorsReconstruction` | SR ingested, probe packet reconstructed at **48 kHz** — the gap-#1 regression guard |
| `AbsSendTimeRefinementOverridesMediaClockEstimate` | the refined value wins over the media-clock estimate (5% rate error) |
| `DisabledEngineIgnoresInboundSenderReport` | the gate is symmetric: an inbound SR is dropped, not just an outbound one |

The seam exists because `feed_srtp_inbound()` cannot reach this path: it
needs a completed DTLS handshake to derive SRTP keys, and a shell-strategy
engine has no peer to complete one against. It is `NIMRTC_API_UNSTABLE`
and documented as a test seam in `engine.hpp`.

**Mutation-verified.** Reverting the audio clock-rate line in
`handle_rtp()` to the 90 kHz default makes
`SenderReportFromTheWireAnchorsReconstruction` fail with 533 333 µs
instead of 1 000 000 — exactly the 90000/48000 = 1.875× error. The case
is load-bearing, not decorative.

Residual: the RTP-side arithmetic itself is still covered separately by
`src/modules/rtp/tests/test_timeline.cpp`; there is no test that drives a
*real* two-engine DTLS/SRTP session end to end and checks the timeline
against it. That needs a paired handshake, not a seam.

### 3. `sender_recv_skew_us` — **landed (`ee8228e`)**

`timeline.cpp` now computes the RFC 3550 §8.1 skew when a monotonic
origin is set, and `handle_rtp()` threads `steady_us` + `&skew_us`
through `observe_timeline()` into `skew_us_by_ssrc` /
`last_skew_us`. Engine accessors `last_sender_recv_skew_us()` /
`sender_recv_skew_us_for_ssrc()` publish it. Without an origin the
value stays 0 by design; single samples carry jitter, the trend is
the skew.

### 4. Fitted slope — **landed (`ee8228e`)**

`SlopeFilterConfig` (`min_samples` / `max_samples` /
`ewma_alpha_permille`) drives a bounded-window least-squares fit with
EWMA smoothing, 5% guard vs nominal, wrap-immune cumulative positions.
Sub-millisecond for sessions of a few minutes; degrades after that.

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

1. ~~Wire `set_ssrc_clock_rate()`~~ — **done**, see gap #1 above.
2. ~~Engine-level reconstruction case~~ — **done**, see gap #2 above.
3. ~~slope estimation + `sender_recv_skew_us` (ADR-017 Decisions 3–4)~~ — **done (`ee8228e`)**, see gaps #3–#4 above.
4. v1.1 validation (B3) — **documented, no new seam**.
   - Long-session slope quality: fitted slope (least-squares + EWMA,
     5% guard, wrap-immune) holds sub-millisecond reconstruction for
     sessions of a few minutes; degrades after that as drift
     accumulates. Single-SR linear extrapolation remains the fallback
     when the SR window is too small.
   - Paired-session gap: still no test that runs a real two-engine
     DTLS/SRTP session end to end and checks the timeline against it.
     `tests/test_engine_timeline.cpp` drives real wire bytes through
     `dispatch_media_inbound()`, but `feed_srtp_inbound()` needs a
     completed DTLS handshake. Recorded as known gap, not a v1.1
     blocker.