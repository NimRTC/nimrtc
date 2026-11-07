# Audio Pipeline — Development Plan and Version Plan

> **What this document is.** The executable counterpart of
> `docs/plan/audio-pipeline-spec.md`. The specification fixes *what* the
> audio pipeline must do (the interface level, item by item); this plan
> fixes *how the work is cut up*, *in which order the pieces may land*,
> *what proves each piece*, and *which release line carries it*.
>
> The specification deliberately contains no schedule and no version
> numbers (DEC-4); they live here.
>
> | | |
> |---|---|
> | Plan opened | 2026-11-07 |
> | Baseline | `main` @ `v1.1.0` release-candidate cut (see `docs/plan/v1.1.0-release-notes.md`) |
> | Scope class | Audio pipeline — new plugin seams, wire-format completion, no transport rewrite |
> | Status | **Development plan + version plan written; the ladder amendment in §6.2 was fixed by the owner on 2026-11-06 (the compressed `v1.2.0` carries WP-0 … WP-11)** |
> | Owner | BDFL |
> | Related | `docs/plan/audio-pipeline-spec.md` (the binding spec), ADR-001 (plugin system), ADR-008 (PCM tap), ADR-015 (API stability), ADR-017 (cross-end timeline), `docs/plan/video-pipeline-v2-plan.md` (the same shape on the video side), `docs/plan/v1.1-plan.md` §5.6 (version ladder) |

---

## 1. Scope

### 1.1 What this plan covers

| Source | Items | Covered by |
|---|---|---|
| Spec Part A (P0 — make the receive path real) | A-1 … A-10 | WP-4, WP-5, WP-7 |
| Spec Part B (P1 — the interfaces) | B-1 … B-17 | WP-1, WP-3, WP-5, WP-6, WP-7, WP-8, WP-9 |
| Spec Part C (P2 — quality and platform) | C-1 … C-5 | WP-5 (C-1 only), WP-12 (C-2 … C-5) |
| Spec Part D (error handling) | the `0x1A40`–`0x1A48` range | WP-7 (mapping), WP-11 (coverage) |
| Spec Part E (the P1 public SDK) | E-1 … E-9 | WP-10 |
| Spec Part V (verification) | V-1 … V-5 | WP-11 |

### 1.2 What this plan does not cover

* **The transport.** No change to ICE, DTLS, SRTP or the RTP stack is
  implied; the audio pipeline consumes the existing seams.
* **The video pipeline.** Its stages are planned in
  `docs/plan/video-pipeline-v2-plan.md`; §6.3 of this document only
  records how the two plans share infrastructure.
* **The codec implementations themselves.** Opus stays the only
  production codec; G.711 (C-5) is a later work package.
* **The device layer** (C-3) — it belongs to the host application, and
  the engine's contract towards it is fixed in the specification only.

---

## 2. How the work is cut

The specification's Part G gives the dependency order; the work packages
below are the unit of delivery that the dependency graph actually
supports. Each package is independently reviewable, independently
testable, and has a defined "done".

Two rules shape the split:

* **A work package never mixes a seam with its implementation.** The
  seams (WP-1) land first and alone, because every later package — and
  every existing module that wants to use them — compiles against them.
* **A work package that changes the public API is a package on its
  own** (WP-10), because it needs the API-freeze baseline taken before
  it starts (WP-0) and the freeze scan re-run before it ends.

| WP | Title | Spec items | Depends on | Ships in |
|---|---|---|---|---|
| WP-0 | Baseline and guard rails | — | — | v1.2.0 |
| WP-1 | The two plugin seams | B-1, B-2, B-15 (part 1) | WP-0 | v1.2.0 |
| WP-2 | Opus-side decoder | B-10, A-2 | WP-1 | v1.2.0 |
| WP-3 | The `audio_rx` module | B-2 (impl), B-3, B-7, B-8, B-9 | WP-1 | v1.2.0 |
| WP-4 | Clock-driven playout | A-1, A-3, A-4, A-5 | WP-3 | v1.2.0 |
| WP-5 | Send path: framing, state, no silent loss | A-6, A-7, A-8, B-5, B-6, C-1 | WP-0 | v1.2.0 |
| WP-6 | Negotiation, PT routing, SDP completion | B-4, B-14 | WP-2, WP-5 | v1.2.0 |
| WP-7 | Engine wiring and failure policy | A-9, A-10, B-11, B-15 (part 2), B-16 | WP-1, WP-3, WP-6 | v1.2.0 |
| WP-8 | RTCP receiver reports and statistics | B-12, B-13 | WP-7 | v1.2.0 |
| WP-9 | Cross-end capture timestamp | B-17 | WP-7, ADR-017 | v1.2.0 |
| WP-10 | The P1 public SDK | E-1 … E-9 | WP-7, WP-8, WP-9 | v1.2.0 |
| WP-11 | Verification and interop | V-1 … V-5 | WP-2 … WP-10 | v1.2.0 |
| WP-12 | P2 quality and platform | C-2, C-3, C-4, C-5 | WP-11 | v1.4.0 |

> **The "Ships in" column is the ladder of §6.2, as the owner fixed it
> on 2026-11-06.** See §6 before anyone schedules the work.

### 2.1 Progress log

**WP-0 (baseline) — recorded 2026-11-07, Windows 10 x86_64, MSVC 2022,
Debug.** The five mechanical gates of spec V-5 on the untouched tree:

| Gate | Result |
|---|---|
| `node scripts/api_freeze_scan.js` | 106 public headers, 707 symbols, `Missing == 0` |
| `python scripts/check_api_marker_includes.py` | OK — every marker-using header includes `api_version.hpp` |
| `bash scripts/check-release-freeze.sh` | 7/7 PASS (sfu, api, api-symbols, vendor, version, submodules, root) |
| `bash scripts/check-root-clean.sh` | PASS — no scratch / log / keylog artefacts at the root |
| `pwsh scripts/check-markdown-utf8.ps1` | clean (checked with the Python equivalent on this machine — PowerShell 7 is not installed; the script's own recursion overflows under Windows PowerShell 5.1) |

Acceptance evidence at the same point: `ctest --test-dir build -C Debug`
→ **697 / 697 passed, 0 failed**; the acceptance runner's Case D (real
Chrome interop over the full audio path) is exercised by
`tools/run_e2e_acceptance.py --case d`.

**After WP-1 (the two plugin seams) — 2026-11-07.** The scan reports
**109 public headers** (106 + `plugins/audio_decoder.hpp` +
`plugins/audio_receiver.hpp` + the module header `audio_rx/audio_rx.hpp`)
and `Missing == 0`, which is exactly the E-8 prediction.  The symbol
count moves 707 → 729.

| Gate (after WP-1) | Result |
|---|---|
| `node scripts/api_freeze_scan.js` | 109 headers, 729 symbols, `Missing == 0` |
| `python scripts/check_api_marker_includes.py` | OK |
| `bash scripts/check-release-freeze.sh` | 7/7 PASS |
| `bash scripts/check-root-clean.sh` | PASS |
| `ctest --test-dir build -C Debug` | **706 / 706 passed, 0 failed** (697 baseline + 9 entries: the 6 cases of `tests/test_audio_seams.cpp` plus its binary-level entry, and the `audio_rx` module's placeholder test plus its binary-level entry) |

**One defect found and fixed inside WP-1.**  The two new registration
macros initially stringified their id argument (`#id`), so a call of the
form `NIMRTC_REGISTER_AUDIO_DECODER("opus", &factory)` stored the id
`"opus"` *with* the quote characters — the lookup by `opus` then failed.
The two audio-seam macros now take the id literal as-is; the legacy
`NIMRTC_REGISTER_*` family (which stringifies a bare identifier token)
is left untouched, and the difference is documented at each macro.

**After WP-2 (the Opus-side decoder) — 2026-11-07.**  The Opus module
now publishes a `plugins::IAudioDecoder` under the id `"opus"` in the
registry's audio-decoder category, next to the codec plugin that uses
the same id in the codec category (ids are per category, DEC-6).  The
plugin lives in `src/modules/opus/src/`, so the **public header set does
not change**: the scan still reports 109 headers and 729 symbols.

| Gate (after WP-2) | Result |
|---|---|
| `node scripts/api_freeze_scan.js` | 109 headers, 729 symbols, `Missing == 0` |
| `python scripts/check_api_marker_includes.py` | OK |
| `ctest --test-dir build -C Debug` | **719 / 719 passed, 0 failed** (706 + 13: the 12 cases of `src/modules/opus/tests/test_opus_audio_decoder.cpp` plus its binary-level entry) |

**The gap detection lives in the decoder, not in the receiver.**  The
Opus plugin tracks the RTP timestamp of the last decoded frame; when the
next packet jumps forward it treats the jump as a loss, and — when the
stream negotiated in-band FEC — decodes the packet in FEC mode first, so
the *lost* frame is reconstructed from the redundancy the peer embedded
in the newer packet and reported with `fec_recovered == true`
(B-8).  The receiver therefore only has to hold the concealment slot for
one frame period (the "one-frame lookahead") and replace it with the
FEC frame when one arrives; it needs no codec-specific code.

**After WP-3 (the `audio_rx` module) — 2026-11-07.**  The module now
implements the receive pipeline: it resolves its jitter buffer and its
decoder through `core::PluginRegistry` (the ids the engine puts into
`AudioReceiverConfig`), walks the media timeline on the RTP timestamps,
conceals the slots that never arrived, downmixes to the configured output
channel count, and plays out **only** when the host's `tick()` asks it
to.  The module depends on `nimrtc::core` and `nimrtc::plugins` only —
it links neither the Opus nor the JB module.

The seam side landed with it: `src/modules/audio_rx/src/receiver_plugin.cpp`
implements the `plugins::IAudioReceiver` adapter and its factory, and
registers the pipeline under the id **`"nimrtc_audio_rx"`** in the
registry's audio-receiver category.  The factory returns a **null
product** when the jitter buffer or the decoder plugin is not registered:
there is no fallback receiver (B-16), so the caller turns the null into
the hard error `kAudioReceiverMissing`.  The null product is also the
only way to report the condition, because the engine installs the
callbacks *after* `create()` — the pipeline cannot report through its own
error callback at that point.

The module's tests use **their own doubles** for `plugins::IJB` and
`plugins::IAudioDecoder` rather than the Opus and JB modules — that is
what keeps the module independently buildable and testable (L3), and it
is the same property WP-7 relies on from the other side.

| Gate (after WP-3) | Result |
|---|---|
| `node scripts/api_freeze_scan.js` | 109 headers, 730 symbols, `Missing == 0` (the module's public header gains one symbol: `register_default_plugins`) |
| `python scripts/check_api_marker_includes.py` | OK |
| `ctest --test-dir build -C Debug` | **729 / 729 passed, 0 failed** (719 - the module's 2 placeholder entries + its 12 new entries) |

**After WP-6 (negotiation, payload-type routing, SDP completion) —
2026-11-07.**  The engine's demux rule changed from "anything that is
not the video payload type is audio" to a **negotiated routing table**
built from the peer's own `a=rtpmap:` lines.  The two tables (one per
m-section — a payload type is only meaningful within the m-section that
declared it, RFC 3550 §5.14) live in a private engine header,
`src/engine/src/audio_routing.hpp`, so the **public header set does not
change**: the scan still reports 109 headers and 730 symbols.

A payload type in neither table is counted and dropped with
`kAudioPayloadTypeUnknown` and is **never** decoded (defect F-8: the old
rule handed telephone-event, RED and unsupported-codec packets to the
audio decoder as if they were audio).  The report is throttled to the
first occurrence and every 50th one, so a peer that keeps sending a
stream we never negotiated cannot flood the host.

The payload type we accept is the one we can actually decode — the
lookup goes through the registry's audio-decoder slot, so the answer
reflects the plugins the host really installed.  A peer that offers only
codecs we cannot decode is a *configuration* condition, not a per-packet
anomaly: `kAudioCodecUnsupported` is reported exactly once and the audio
m-line is rejected in the answer (RFC 3264 §6.1).

The offer now carries `a=ptime` / `a=maxptime`, the Opus `a=fmtp` that
describes what NimRTC actually does (in-band FEC on, the stream's
channel count) and the `ssrc-audio-level` `a=extmap` that Chrome needs
before it reports `audioLevel` in `inbound-rtp` (F-22).  The per-engine
SSRC in the description is the very value the packet path stamps, so the
two cannot drift apart.  The answer carries exactly the negotiated
payload type — its own `a=rtpmap:` and the *negotiated subset* of the
peer's `a=fmtp:` parameters, not a blind echo.

| Gate (after WP-6) | Result |
|---|---|
| `node scripts/api_freeze_scan.js` | 109 headers, 730 symbols, `Missing == 0` |
| `python scripts/check_api_marker_includes.py` | OK |
| `ctest --test-dir build -C Debug` | **737 / 737 passed, 0 failed** (729 + the 7 cases of `tests/test_engine_audio_state.cpp` plus its binary-level entry) |
| `python tools/run_e2e_acceptance.py --case d` | **PASS** — real Chrome interop over the full audio path: `sdpOfferSeen`, `audioReceived` and `rtpPackets>0` all OK |

**The routing table is inert until a remote description arrives.**  While
`pt_routing_active` is false the demux keeps its historical behaviour,
so a host that drives the engine without an SDP exchange (the test
seams, the loopback harness) is unaffected.  That is a deliberate
choice, not an oversight: the routing table is only meaningful once the
peer's description has told us what it will actually send.

**WP-4 is half landed, and the plan says so.**  The clock-driven playout
itself is in the module: `tick()` is the only thing that makes the
pipeline produce output, a gap produces exactly the missing number of
concealed frames, and the output cadence follows the media time rather
than the arrival (spec A-1 / A-3 / A-4).  What is **not** done yet is
the V-2 loss-injection test ("constant cadence, concealed counter ==
drop count"): that test drives two engines over the loopback transport,
so it needs the engine wiring — it is part of WP-7, not of WP-4 alone.

**After WP-7 (engine wiring and failure policy) — 2026-11-07.**  The
engine stopped owning the receive path.  It held its own
`jb::JitterBuffer`, fell back to a concrete `opus::Decoder`, and then
reinterpreted the *compressed* RTP payload as float PCM — the host got
noise and the 3A echo canceller got a bit-stream instead of a reference
signal (defect F-1).

The engine now resolves a **decoder** and a **receive pipeline** through
the registry's typed slots (`EngineConfig::audio_decoder_name`, default
`"opus"`; `EngineConfig::audio_receiver_name`, default
`"nimrtc_audio_rx"`) and drives `plugins::IAudioReceiver` alone.  The
plan's checkable condition holds: `src/engine/` contains **no** include
of `nimrtc/jb/*` and **no** include of `nimrtc/opus/*` — only a comment
that names them.  What replaced them is a *link* dependency on the
`audio_rx` module (`src/engine/CMakeLists.txt`) and the matching entry
in `kDefaultRegistrars[]` (`src/core/src/pal_default_registrars.cpp`),
which is guarded by `NIMRTC_HAS_AUDIO_RX` so the entry exists only when
the module is actually built.

The failure policy is B-16 verbatim: a missing decoder or a missing
pipeline is a hard error that fires `on_error` **exactly once** and
disables the audio track; the inbound packets are counted and dropped,
never decoded.  There is no fallback receiver and no fallback encoder —
a missing codec plugin is reported (`kAudioEncodeFailed`, throttled)
rather than silently putting a *different* codec's bit-stream on the
wire.

The 3A render reference is now the **decoded** frame — the frame that is
about to be played out — so the echo canceller finally has a reverse
stream (spec A-10).

| Gate (after WP-7) | Result |
|---|---|
| `node scripts/api_freeze_scan.js` | 109 headers, 730 symbols, `Missing == 0` |
| `python scripts/check_api_marker_includes.py` | OK |
| `ctest --test-dir build -C Debug` | **741 / 741 passed, 0 failed** (737 + the 4 new cases of `tests/test_engine_audio_state.cpp` plus its binary-level entry) |
| `grep -r "nimrtc/\(jb\|opus\)/" src/engine/` | only a comment — no include remains |

**What is still open after WP-7.**  The V-2 loss-injection test (the
"constant cadence, concealed counter == drop count" row) is the last
piece of WP-4 and it belongs to WP-11.  WP-8 (RTCP receiver reports and
the statistics counters), WP-9 (the cross-end capture timestamp),
WP-10 (the P1 public SDK) and WP-11 (verification and interop) are
still ahead.

---

## 3. The work packages in detail

### WP-0 — Baseline and guard rails

**Why first.** Every later package adds public symbols or changes the
wire behaviour. ADR-015 requires a freeze baseline *before* the public
API moves; a package that lands first and a baseline taken afterwards
cannot prove that the change was additive.

**Work.**

* Run and record the five mechanical gates of spec V-5 on the untouched
  tree: `node scripts/api_freeze_scan.js`,
  `python scripts/check_api_marker_includes.py`,
  `bash scripts/check-release-freeze.sh`,
  `bash scripts/check-root-clean.sh`,
  `pwsh scripts/check-markdown-utf8.ps1`.
* Record the current public-header count (106) and the current
  `Missing` count (0) in the package's notes; the spec's E-8 predicts
  106 → 109 after the two plugin headers and the module header exist.
* Record the current acceptance evidence: `python tools/run_e2e_acceptance.py
  --case abc` and `--case d` / `--case e` green.

**Done when** the five gates are green and the numbers are written down
in the package notes.

### WP-1 — The two plugin seams

**Spec items.** B-1 (`plugins::IAudioDecoder`), B-2
(`plugins::IAudioReceiver`), and the registry half of B-15.

**Files.**

| Path | Change |
|---|---|
| `src/plugins/include/nimrtc/plugins/audio_decoder.hpp` | **new** — `AudioDecodeFormat`, `AudioDecodedFrame`, `IAudioDecoder`, `IAudioDecoderFactory` (E-8, 4 symbols) |
| `src/plugins/include/nimrtc/plugins/audio_receiver.hpp` | **new** — `AudioReceiverConfig`, `AudioOutputFrame`, `IAudioReceiver`, `IAudioReceiverFactory` (E-8, 4 symbols) |
| `src/core/include/nimrtc/core/registry.hpp` | two forward declarations, two `TypedRegistry` slots, the `kAudioDecoder` / `kAudioReceiver` enumerators plus their `switch` cases, the two registration macros, the two back-compat includes (B-15) |
| `src/engine/include/nimrtc/engine/pal/engine_plugin_resolver.hpp` | `resolve_audio_decoder()` / `resolve_audio_receiver()`, mirroring `resolve_codec` / `resolve_audio3a` |
| `src/core/include/nimrtc/core/registry.hpp` | `namespace audio_rx { void register_default_plugins() noexcept; }` forward declaration |

**Constraints.** `src/plugins/` stays header-only and implementation-free
(L2); `src/core/` stays header-only (L1). The seams carry no engine
concept, no Opus concept, and no jitter-buffer concept.

**Done when** the headers compile standalone, every symbol carries a
stability marker, the API-freeze scan reports `Missing == 0`, and the
header count is 108.

### WP-2 — Opus-side decoder

**Spec items.** B-10 (Opus-side decoder extensions), A-2 (decoding on the
receive path).

**Files.** `src/modules/opus/src/audio_decoder_plugin.cpp` (**new**,
compiled into the existing `nimrtc_opus` target — no new library),
`src/modules/opus/include/nimrtc/opus/opus.hpp` (extended concrete API:
depacketise + PLC + FEC entry points),
`src/modules/opus/src/opus.cpp`.

**Behaviour.** `decode_packet()` consumes an RFC 7587 payload and yields
the PCM of the expected length; `decode_plc()` yields the requested
duration and is flagged as concealed; FEC recovery keeps the previous
packet and retries with FEC.

**Done when** `src/modules/opus/tests/test_opus_audio_decoder.cpp` and
the extended `test_opus_packetise.cpp` are green (V-1 rows 1–4).

### WP-3 — The `audio_rx` module

**Spec items.** B-2 (implementation), B-3 (jitter buffer via the seam,
with the right clock rate), B-7 (concealment), B-8 (in-band FEC
recovery), B-9 (downmix).

**New module layout** (L3 — self-contained, own CMakeLists, own
include/, own src/, own tests/):

```
src/modules/audio_rx/
├── CMakeLists.txt
├── include/nimrtc/audio_rx/audio_rx.hpp
├── src/receiver.cpp
├── src/receiver_helpers.cpp
└── tests/test_audio_rx.cpp
```

**Dependencies.** `nimrtc::core` and `nimrtc::plugins` only. The jitter
buffer and the decoder are *injected* as plugin interfaces, so the module
has **no** dependency on `src/modules/jb` or `src/modules/opus` (B-15).
That is what keeps the module independently buildable and testable.

**Shared infrastructure, not a second implementation.** The receiver
must use the shared adaptive playout strategy that `v1.1.0` already
introduced for the video jitter buffer (`src/modules/jb`, the shared
strategy module) — it must not grow its own playout heuristic. This is
the single most important "do not duplicate" rule of this plan (§6.3).

**How the injection actually happens (decided 2026-11-07).** The seam
`plugins::IAudioReceiverFactory::create(cfg)` takes only an
`AudioReceiverConfig`, so the two injected plugins have to be *named*
somewhere. The two fields appended to `AudioReceiverConfig` do that:

| Field | Default | Filled by the engine from |
|---|---|---|
| `jb_plugin_id` | `"adaptive"` | `EngineConfig::jb_name` |
| `decoder_plugin_id` | `"opus"` | `EngineConfig::audio_decoder_name` |

The module's factory resolves both through `core::PluginRegistry` — the
same ids the engine resolves for itself (DEC-6: ids are per category,
so `"opus"` names both the Opus codec plugin and the Opus decoder
plugin). The module therefore still depends on `nimrtc::core` and
`nimrtc::plugins` only, and a missing plugin yields a `nullptr` factory
product, which the engine turns into the B-16 hard error — there is no
fallback receiver.

**One seam change on the way: `IJB::configure()`.** B-3 requires the
receiver to configure the jitter buffer with the *stream's* clock rate
(48000 for Opus), because a `clock_rate_hz` of 0 must never silently
mean 90 kHz for an audio stream (F-6). `plugins::IJB` had no way to do
that (`IJBFactory::create()` takes no arguments), so the interface gains
one virtual, `configure(const JBConfig&)`, which the `jb` module's
adapter implements by translating into the concrete `jb::Config`. The
seam default of `JBConfig::clock_rate_hz` moves from 90000 to 0
("unspecified"), and the adapter keeps the historical 90 kHz only for
the streams that are *not* configured (the video path).

**Done when** `src/modules/audio_rx/tests/test_audio_rx.cpp` covers the
V-1 rows "receiver timeline", "receiver cadence", "receiver downmix",
and the module builds and links without the Opus or JB modules present.

### WP-4 — Clock-driven playout

**Spec items.** A-1 (playout becomes clock-driven), A-3 (concealment
driven by the timeline, not by decode failure), A-4 (constant output
cadence), A-5 (audio level and channel handling).

**Files.** `src/modules/audio_rx/src/receiver.cpp` (the tick path).

**Behaviour.** `IAudioReceiver::tick(now)` pops by *media time*, not by
arrival; a gap produces exactly the missing number of concealed frames;
the output sample count equals media time × rate (received +
concealed); the tick rate is ≥ 100 Hz and a slower host tick degrades
granularity without breaking continuity (O-4).

**Done when** the V-1 rows "receiver timeline" and "receiver cadence"
are green, and the loss-injection test of V-2 (constant cadence, concealed
counter == drop count) passes.

### WP-5 — Send path: framing, state, no silent loss

**Spec items.** A-6 (per-engine send state), A-7 (framing and
timestamps), A-8 (no silent audio loss), B-5 (RFC 7587 framing), B-6
(frame accumulator), C-1 (bitrate adaptation).

**Files.** `src/engine/src/engine.cpp` (or the engine TU that owns
audio), `src/engine/include/nimrtc/engine/engine.hpp` (`EngineConfig`
gains the audio fields of E-3), `src/modules/opus/` (the Opus-side
packetiser helper).

**Behaviour.** Each engine owns its own SSRC, sequence number and
timestamp base (the current shared static state is defect F-12); PCM is
accumulated into 20 ms slices at the negotiated rate; an unsupported rate
is a hard error (A-7 / O-5) instead of silence; an encode failure
increments the error counter and fires `on_error` while the timeline
still advances (A-8).

**Done when** the V-1 rows "sender framing", "two engines, one process",
and "encoder error reporting" are green.

### WP-6 — Negotiation, payload-type routing, SDP completion

**Spec items.** B-4 (PT routing and negotiation), B-14 (SDP completion).

**Files.** `src/engine/src/engine_sdp.cpp` (or the SDP TU),
`src/modules/audio_rx/` (the routing table lives with the receiver).

**Behaviour.** The remote SDP's payload types populate the routing
table; an unknown payload type is rejected with
`kAudioPayloadTypeUnknown` and never decoded; the local offer carries
`a=ptime`, `a=maxptime`, the Opus `a=fmtp`, the audio-level `a=extmap`
and the per-engine SSRC.

**Done when** the V-1 rows "PT routing" and "SDP contents" are green, and
Chrome accepts the offer (V-3).

### WP-7 — Engine wiring and failure policy

**Spec items.** A-9 (the receive path rejects what it cannot decode),
A-10 (the 3A render reference is the real decoded signal), B-11 (engine
wiring), B-15 (registration half), B-16 (plugin resolution and failure
policy), and the Part D error-code mapping (`0x1A40`–`0x1A48`).

**Files.** `src/engine/src/engine.cpp` (+ the other engine TUs),
`src/engine/CMakeLists.txt` (link dependency on `nimrtc_audio_rx`),
`src/core/src/pal_default_registrars.cpp` (the `audio_rx` registration
entry), `src/modules/opus/` (registers its decoder under
`kAudioDecoder` with id `"opus"`).

**Behaviour.** A missing decoder or receiver plugin is a hard error
(O-7) that fires `on_error` exactly once and disables the audio track;
there is no built-in fallback receiver. The engine only ever sees
`plugins::IAudioReceiver` — no concrete module header is included by the
engine (L4). The 3A render reference is the decoded signal, not the
inbound bit-stream.

**Done when** the V-1 rows "registry slots" and "missing-receiver
failure" are green, and `src/engine/` contains no include of
`nimrtc/jb/*` or `nimrtc/opus/*` (the two concrete includes the spec
§3 calls out as the reason the seam exists).

### WP-8 — RTCP receiver reports and statistics

**Spec items.** B-12 (RR for audio), B-13 (audio statistics).

**Files.** `src/engine/src/engine_rtcp.cpp`, `src/engine/include/nimrtc/engine/engine.hpp`
(the 12 new `Stats` members of E-8, appended at the end of the struct).

**Behaviour.** The engine sends a receiver report for the audio
source; the statistics expose the audio counters Chrome reports
`inbound-rtp` for. `Stats` stays `NIMRTC_API_UNSTABLE` (O-11).

**Done when** the V-4 criterion "Chrome reports `inbound-rtp` loss and
jitter for NimRTC's audio" holds, and the new counters are visible in the
engine statistics test.

### WP-9 — Cross-end capture timestamp

**Spec items.** B-17.

**Files.** `src/engine/src/engine_timeline.cpp` (the ADR-017 mapping),
`src/engine/include/nimrtc/engine/engine.hpp` (the `capture_time_us`
field of `AudioFrameInfo`).

**Behaviour.** `capture_time_us` is filled from the ADR-017 cross-end
timeline; 0 means "not yet reconstructible" and is **never** faked from
the local playout clock.

**Done when** the V-1 row "capture timestamp" is green (0 without a
sender report, a real value with one).

### WP-10 — The P1 public SDK

**Spec items.** E-1 … E-9.

**Files.** `src/engine/include/nimrtc/engine/engine.hpp` (7 new symbols
per E-8), `docs/api/engine_api.md`, `docs/api/engine_config.md`,
`docs/zh/architecture.md` (the Chinese layout is governed by ADR-012).

**The 21 new public symbols** are exactly the inventory of E-8. Nothing
may be added beyond that list without a spec amendment.

**Compatibility rules.**

* `EngineConfig` grows **8 members, appended at the end** of the struct
  (E-6) so that aggregate initialisation of existing hosts keeps
  compiling.
* The legacy `set_on_audio_frame()` stays; it is a published STABLE
  symbol and cannot be removed without a MAJOR bump (ADR-015 §Decision
  3). It is documented as *legacy* and is not the recommended surface.
  Its `size_t` argument keeps its current meaning — total interleaved
  floats (O-2).
* Every new symbol carries a marker; the API-freeze scan must report
  `Missing == 0` and the header count 109.

**Done when** the V-4 criteria 7 and 8 hold, and the migration guide of
E-9 is in `docs/api/engine_api.md`.

### WP-11 — Verification and interop

**Spec items.** V-1 … V-5.

**Work.**

* The unit tests of V-1, in the locations the spec names.
* `tests/test_engine_audio_loopback.cpp` (V-2): two engines over the
  loopback transport, loss injection, tick-rate independence (30 Hz and
  200 Hz).
* `tests/test_engine_audio_state.cpp` (V-1 rows that need two engines,
  the registry and the SDP).
* Interop (V-3): extend `interop/chrome/test_chrome_opus.html` so the
  **Chrome → NimRTC** direction is observable, keep the NimRTC → Chrome
  assertions green, update the SDP fixture
  `interop/fixtures/chrome_opus_offer.sdp` and regenerate
  `chrome_opus_offer.expected.md`.
* The CI gates of V-5 (API freeze, marker include hygiene, release
  freeze, root cleanliness, markdown encoding).

**Done when** all of V-1 … V-5 are green and the acceptance runner
(`tools/run_e2e_acceptance.py --case d`) is green with the audio
assertions in place.

### WP-12 — P2 quality and platform (later line)

**Spec items.** C-2 (DTX / VAD coupling), C-3 (device layer contract),
C-4 (A/V synchronisation), C-5 (G.711).

**Note.** C-4 depends on WP-9 (the capture timestamp) *and* on the
device layer of C-3; C-5 depends on a second decoder implementation and
therefore on the decoder seam being proven by more than one codec.

---

## 4. Module placement and the L1–L7 constraints

| Constraint | How this plan respects it |
|---|---|
| L1 — `src/core/` is header-only and exposes stable types only | WP-1 adds two `TypedRegistry` slots and two enumerators to `registry.hpp`; no implementation, no new state beyond the existing registry pattern |
| L2 — `src/plugins/` holds interfaces only | WP-1's two new headers contain interfaces and their factory interfaces; no implementation |
| L3 — every module is self-contained | WP-3 creates `src/modules/audio_rx/` with its own `CMakeLists.txt`, `include/`, `src/`, `tests/` |
| L4 — the engine only orchestrates plugins | WP-7 removes the two concrete includes the engine has today (`nimrtc/jb/jitter_buffer.hpp`, `nimrtc/opus/opus.hpp`) and adds a link dependency, not an include |
| L5 — vendored code lives only in `src/third_party/` | This plan adds no third-party code |
| L6 — no global mutable state | WP-5 moves the shared static send state (F-12) into per-engine state |
| L7 — the protected domains are not overridable | `policy/`, `.zelto/` and `AGENTS.md` are untouched by this plan |

---

## 5. Verification matrix

| Spec item | Verification | Where |
|---|---|---|
| A-1, A-3, A-4 | "receiver timeline", "receiver cadence" | `src/modules/audio_rx/tests/test_audio_rx.cpp` |
| A-2 | "Opus decoder plugin round-trip" | `src/modules/opus/tests/test_opus_audio_decoder.cpp` |
| A-5, B-9 | "receiver downmix" | `src/modules/audio_rx/tests/test_audio_rx.cpp` |
| A-6 | "two engines, one process" | `tests/test_engine_audio_state.cpp` |
| A-7, B-5, B-6 | "sender framing" | `tests/test_engine_audio_state.cpp` |
| A-8 | "encoder error reporting" | `tests/test_engine_audio_state.cpp` |
| A-9, B-16 | "missing-receiver failure" | `tests/test_engine_audio_state.cpp` |
| A-10 | 3A render reference is the decoded signal | `tests/test_engine_audio_loopback.cpp` |
| B-1, B-2, B-15 | "registry slots" | `tests/test_engine_audio_state.cpp` |
| B-3 | jitter buffer used through the seam at 48 kHz | `src/modules/audio_rx/tests/test_audio_rx.cpp` |
| B-4 | "PT routing" | `tests/test_engine_audio_state.cpp` |
| B-7 | PLC frame properties | `src/modules/opus/tests/test_opus.cpp` (extend) |
| B-8 | "FEC recovery" | `src/modules/opus/tests/test_opus_audio_decoder.cpp` |
| B-10 | "depacketise of all RFC 7587 codes" | `src/modules/opus/tests/test_opus_packetise.cpp` (extend) |
| B-11 | engine resolves decoder + receiver | `tests/test_engine_audio_state.cpp` |
| B-12 | Chrome reports `inbound-rtp` for NimRTC audio | V-3 interop |
| B-13 | audio statistics counters | engine statistics test |
| B-14 | "SDP contents" | `tests/test_engine_audio_state.cpp` + V-3 |
| B-17 | "capture timestamp" | `tests/test_engine_audio_state.cpp` |
| E-1 … E-9 | V-4 criteria 7 and 8 + the freeze scans | `scripts/api_freeze_scan.js`, `scripts/check_api_marker_includes.py`, `scripts/check-release-freeze.sh` |
| Part D | error-code mapping | `tests/test_engine_audio_state.cpp` |
| C-1 … C-5 | WP-12 | later line |

**CI gates that must be green for the audio line to ship** (all five of
V-5, plus the acceptance runner's Case D):

```
node scripts/api_freeze_scan.js
python scripts/check_api_marker_includes.py
bash scripts/check-release-freeze.sh
bash scripts/check-root-clean.sh
pwsh scripts/check-markdown-utf8.ps1
python tools/run_e2e_acceptance.py --case abc
python tools/run_e2e_acceptance.py --case d
```

---

## 6. Version plan

### 6.1 The ladder before this plan

`docs/plan/v1.1-plan.md` §5.6 (BDFL, 2026-11-06) fixed the ladder:

| Version | Content |
|---|---|
| `v1.1.0` | The release described by the v1.1 plan — the only feature-bearing release of the `v1.1` line |
| `v1.1.x` | Bug fixes, security fixes, build/toolchain regressions, documentation corrections. No new public API, no new behaviour, no new dependency |
| `v1.2.0` | The first part of the video-pipeline rewrite, the usrsctp removal, the SM4-GCM SRTP implementation, and whatever the soak/C-track evidence forces |
| `v1.3.0` / `v1.4.0` | The remaining video-pipeline stages (hardware codec negotiation, the codec plug-in contract for H.265/AV1, the rendering-side alignment work) |

**Consequence for this plan: no audio item lands in `v1.1.0` or
`v1.1.x`.** The audio pipeline adds 21 public symbols and two new plugin
categories — by the §5.6 rule that is new behaviour, so it cannot ship
in a patch line.

### 6.2 Ladder amendment — owner decision, 2026-11-06

**The owner compressed the ladder** (the earlier draft of this plan had
proposed a separate `v1.3.0` for the audio body; that proposal is
withdrawn). The compressed `v1.2.0` carries the audio pipeline's P0+P1
body **together with** the video pipeline's Part B, instead of pushing
the audio work one release line further out.

| Version | Content (fixed) |
|---|---|
| `v1.1.0` | unchanged — quality + packaging + hardening; the tag is cut when the C-track gates close |
| `v1.1.x` | unchanged — fixes only |
| `v1.2.0` | the video Part B (feedback → BWE → pacing → rate-control closure, the encoder plug-in seam, the profile → mode table), **plus the audio pipeline P0+P1 body: WP-0 … WP-11**, plus the usrsctp removal and SM4-GCM SRTP |
| `v1.3.0` | the video codec-breadth / hardware-first stage (unchanged from the video plan, §B.1) |
| `v1.4.0` | the video device layer / receive experience **and** the audio P2 body (WP-12: DTX/VAD, device layer, A/V sync, G.711) |

**Why this ordering.**

1. The audio P0 body is a *repair* of the receive path (F-1 … F-11 are
   confirmed defects: no decoding at all on the receive path). It is
   valuable, but it is valuable as a coherent release: "the audio
   pipeline works end to end" reads better in one release than split
   across two.
2. The audio and the video Part B work share the same timeline and
   feedback machinery (ADR-017, §6.3 below), so they belong to the same
   release cycle; the intra-release order is the dependency order of
   §2, not a version line.
3. One release line for both bodies keeps the public-API review of that
   release a single exercise instead of two, and avoids the version
   churn that the owner rejected.
4. `v1.1.x` stays clean: no patch release of the `v1.1` line may carry
   the audio seam.

**What this plan does NOT decide.** The dates, the person-days and the
split of `v1.2.0` into reviewable merges. Those follow the plan
approval.

### 6.3 Shared infrastructure with the video plan

The audio and the video pipelines must not grow two answers to the same
question. The shared points, and the decision for each:

| Shared concern | Decision |
|---|---|
| Playout / buffer strategy | Use the shared adaptive playout strategy that `v1.1.0` already introduced (`src/modules/jb`); the audio receiver injects it through the jitter-buffer seam. No second heuristic in `audio_rx` |
| Plugin seams and the registry | The audio seams follow the exact pattern of the existing typed registry categories (B-15) and the `resolve_*` resolvers (B-16). One pattern, two categories |
| Cross-end timeline | Both pipelines read the same ADR-017 mapping; B-17 reuses it rather than defining a second capture-time model |
| Feedback → BWE → pacing | The video Part B work in `v1.2.0` establishes the loop; the audio path joins it as a consumer rather than re-implementing the pacing |
| Pack / payload framing | RFC 7587 framing for audio (B-5) sits next to the video RTP path; both use the engine's per-engine SSRC / sequence / timestamp state from A-6 |
| Error codes | The audio range `0x1A40`–`0x1A48` extends the existing block; the video side keeps its own ranges — one numbering scheme, no overlap |

---

## 7. Open items — status

The specification's §11 lists eleven points where it had to make a call
that the owner may want to revisit. Each has a default, so the
implementation is never blocked. This plan takes **all eleven defaults**
and records them as the working assumption:

| # | Decision taken | Status |
|---|---|---|
| O-1 | New seam + `src/modules/audio_rx/` module | default accepted |
| O-2 | The legacy callback's `size_t` is total interleaved floats | default accepted |
| O-3 | One frame of latency for FEC recovery is accepted and flagged per frame | default accepted |
| O-4 | The audio tick rate rises from ≥ 30 Hz to ≥ 100 Hz (documentation change) | default accepted |
| O-5 | `pcm_sample_rate_hz != 48000` is a hard error until a resampler exists | default accepted |
| O-6 | The `0x1A40`–`0x1A48` error-code range | default accepted |
| O-7 | A missing decoder or receiver is a hard error, no fallback receiver | default accepted |
| O-8 | Downmix to `pcm_channels` by default, switchable | default accepted |
| O-9 | `capture_time_us` comes from the ADR-017 timeline, never faked | default accepted |
| O-10 | `audio_ptime_ms` configurable (10/20/40/60), default 20 | default accepted |
| O-11 | `Engine::Stats` stays `NIMRTC_API_UNSTABLE` | default accepted |

**The one item that is not a default but a plan decision** is the ladder
amendment of §6.2 — it changes a release line, so it belongs to the
owner. The owner fixed it on 2026-11-06: the compressed `v1.2.0`
carries the audio P0+P1 body together with the video Part B.

---

## 8. Risks and mitigations

| Risk | Mitigation |
|---|---|
| The public API grows by 21 symbols and the freeze scan goes red | WP-0 takes the baseline *before* any change; WP-10 is a package of its own; the inventory in E-8 is exhaustive, and anything beyond it needs a spec amendment |
| The engine keeps (or regains) concrete module includes, breaking L4 | WP-7's "done" condition is that `src/engine/` contains no `nimrtc/jb/*` or `nimrtc/opus/*` include — a checkable condition, not a review opinion |
| The audio receiver grows its own playout heuristic instead of using the shared strategy | WP-3's "done" condition names the shared strategy module; §6.3 records the decision |
| The legacy callback's semantics drift while the new callback is added | The V-1 row "legacy callback semantics" pins the relation between the two callbacks (`num_samples == samples_per_channel * channels` of the new frame) |
| `capture_time_us` is faked from the local clock to "make it work" | The V-1 row "capture timestamp" asserts 0 without a sender report; B-17 and O-9 state the rule twice |
| The tick-rate change (O-4) breaks an existing host that ticks at 30 Hz | The V-2 tick-rate independence test drives 30 Hz and 200 Hz and requires continuous audio at both |
| The receive path's thread affinity is assumed rather than verified | The specification's §3.3 makes this a verification task (V-2), not an assumption: if `handle_rtp` runs on the transport thread, the receiver gets an internal mutex |
| The engine's `send_audio` contract changes break a host that pushes arbitrary block sizes | The frame accumulator (B-6) keeps accepting any block size; the V-1 row "sender framing" asserts one packet per 20 ms and a continuous timestamp |
| The audio work and the video Part B work collide in `v1.2.0` | §6.2 puts both bodies in `v1.2.0` on purpose; the collision points are named in §6.3 *before* the work starts, and the intra-release order is the dependency order of §2 |

---

## 9. Definition of done

The audio pipeline line is done when **all** of the following hold:

1. WP-0 … WP-11 are merged, each with its own "done" condition met.
2. All five V-5 gates are green; the API-freeze scan reports
   `Missing == 0` and the public-header count is 109.
3. V-1, V-2, V-3 and V-4 are green, including the acceptance runner's
   Case D with the audio assertions.
4. The public API matches the E-8 inventory exactly, and the E-9
   migration guide is published.
5. The Chinese documentation mirrors the English change (ADR-012).
6. The ladder amendment of §6.2 is applied in the release note of the
   carrying version (`v1.2.0`), which states the audio scope
   accurately.

---

## 10. References

* `docs/plan/audio-pipeline-spec.md` — the binding specification
  (Part A/B/C/D/E/V/G, §11 open items, §12 traceability).
* `docs/plan/v1.1-plan.md` §5.6 — the version ladder this plan amends.
* `docs/plan/video-pipeline-v2-plan.md` — the video side of the same
  shape, and the other half of §6.3.
* `docs/adr/ADR-001-plugin-system.md` — the plugin contract the two new
  seams follow.
* `docs/adr/ADR-008-pcm-tap.md` — the PCM tap the legacy callback rests
  on.
* `docs/adr/ADR-012-zh-docs-layout.md` — the Chinese documentation
  layout.
* `docs/adr/ADR-015-api-stability-policy.md` — the stability markers and
  the freeze rules.
* `docs/adr/ADR-017-cross-end-timeline.md` — the timeline B-17 reads.
* `docs/api/engine_api.md`, `docs/api/engine_config.md` — where the new
  SDK is documented.
