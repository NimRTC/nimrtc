# HN Replies Cheatsheet — NimRTC v0.11.0

> **Purpose:** 1-line answers to the 5 highest-frequency HN questions, plus a
> 3-item "if asked" appendix. Use these as-is for the first comment; expand
> to a full paragraph only if the question gets ≥3 upvotes or the asker is
> a known contributor.
>
> **Style:** honest, no marketing language. If a number is missing, say so
> — don't make it up. If something is deferred, name the version.
>
> **Companion files:**
> - `show-hn-post.md` §Reply-A / B / C — long-form templates for
>   Pion/mediasoup/LiveKit, libwebrtc comparison, and benchmarks.
> - `crosspost-plan.md` — when to post and where.
> - `../blog/2026-10-10-why-nimrtc.md` — fallback link for "tell me more".

---

## Tier 1 — the 5 most likely

### 1. "Why not contribute to libwebrtc instead of forking the spec?"

> Wire interop, not codebase interop. libwebrtc's build system + Chromium
> monorepo culture doesn't accept the kinds of swaps (crypto, profile
> compilation, pre/post-3A PCM tap) we need. RFCs are public; the
> implementation isn't a fork — it's a from-scratch in-house reimplementation
> of the wire plane.
>
> ↳ Long-form: `show-hn-post.md` §Reply-B.

### 2. "How does it compare to Pion / mediasoup / LiveKit?"

> Different layer of the stack and different language. Pion is Go; mediasoup
> and LiveKit are server-side SFU only. NimRTC is C++20 and ships both P2P
> client *and* relay from the same source tree (relay profile skips L2
> codecs — no runtime dispatch, compile-time layer omission).
>
> ↳ Long-form: `show-hn-post.md` §Reply-A.
> ↳ Comparison table: `README.md` §"How NimRTC compares".

### 3. "What's the actual glass-to-glass latency / MOS?"

> Audio-only loopback on the 4-platform CI: ~30 ms p50 / ~80 ms p99 (Opus
> 48 kHz mono, single 20 ms packet, no retransmit). MOS not measured —
> the loopback path hides network jitter, and we don't have a clean
> interop-network benchmark yet. **Honest answer:** p50/p99 are real,
> MOS is unknown. Repro: `python tools/run_e2e_acceptance.py`, artifacts
> in `build/e2e/`. Profile-specific numbers (P2 / P3 with BWE) are
> scheduled for the P3 milestone, not v0.11.0.
>
> ↳ Long-form: `show-hn-post.md` §Reply-C.

### 4. "Why is DataChannel deferred? What about WebTransport / QUIC?"

> DataChannel-over-SCTP-over-DTLS is being deprecated by Chrome in favor
> of WebTransport over QUIC. We made a deliberate call not to ship the
> SCTP path on trunk; the v0.10.x `UsrsctpSocket` class is `@deprecated`
> and still compiles for opt-in callers, but is not registered by
> default. v0.11.0 commits to the WebTransport/QUIC forward direction
> in v1.x. The in-process `test_datachannel_engine` 5/5 PASS covers the
> `IDataChannel` API surface against the stub SCTP socket.
>
> ↳ Full rationale: `docs/plan/v0.11-plan.md` §2.2 (out of scope) and
> §10 (forward direction).

### 5. "Show me a video / can I try it in the browser?"

> Yes — WASM loopback demo: https://nimrtc.github.io/nimrtc/wasm/ (two
> in-browser peers handshake over the loopback interface, no signaling
> server). 30-second screen-cap of the `demo-agent-gateway` running on
> Windows: `docs/demos/demo-agent-gateway-30s.mp4`. 5-step local
> loopback: `README.md` §"Quick Start" §"Run the loopback-p2p smoke test".
>
> **Caveat:** the WASM build is single-threaded, audio-only; full video
> on WASM is not in v0.11.0.

---

## Tier 2 — if asked (3 lower-frequency but plausible)

### 6. "Is DTLS FIPS-validated? What's the GMSSL status?"

> DTLS via wolfSSL v5.9.2 (FIPS-ready build available upstream; we have
> not run a FIPS validation pass on the embedded configuration). GMSSL
> is a compile-time swap: `EngineConfig::dtls_name = "gmssl"` in
> v0.12.0 (registered through `core::PluginRegistry::register_dtls_session("gmssl")`,
> behind `NIMRTC_ENABLE_DTLS_GMSSL=ON`, default `OFF`). The GMSSL
> production rollout is on the P4 roadmap behind a paid enterprise
> reference (per `README.md` §Roadmap).

### 7. "iOS / Android support?"

> Roadmap, not P1. The C++20 core is portable; Swift/Kotlin bindings and
> platform-managed capture are tracked for v1.0+. If you need mobile
> today, the OSS answer in this ecosystem is Pion (Go) or LiveKit (Go).
> We're not trying to be the libwebrtc of mobile — we're trying to be
> the readable C++ engine for server + embedded + desktop.

### 8. "How big is the team? How long did this take?"

> Solo maintainer, ~11 months of calendar time, ~2-3 person-months of
> focused engineering. Commit history is public; every release note in
> `CHANGELOG.md` links to the relevant PRs. P0 (vendoring + scaffolding)
> shipped in v0.9.x; PAL refactor in v0.10.x; v0.11.0 is the first
> release that ships a P2 differentiating feature (PCM tap).

---

## Style reminders for HN comments

- **Don't editorialize.** HN hates "I'm excited to share". Open with the
  answer, not the context.
- **Don't quote the README.** Link to it instead; HN readers will check
  the source if they want the detail.
- **Cite a number, not a feeling.** "30ms p50" beats "low latency".
- **If you don't know, say so.** "MOS not measured" is a stronger
  answer than a made-up 4.2.
- **Don't argue with detractors in-thread.** Reply once with facts, then
  stop. HN's scoring punishes thread-jacking.
- **Don't tag the same person twice** in two consecutive comments — HN
  flags this as astroturfing.
- **First 30 minutes matter most.** Have all 5 tier-1 answers
  pre-typed in a scratch buffer before the post goes live.
