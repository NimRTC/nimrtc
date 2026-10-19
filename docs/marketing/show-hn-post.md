# Show HN: NimRTC — a from-scratch C++20 WebRTC engine

> **Status:** draft — title **#1** is selected. Target post window: Tue/Wed
> morning US time. First post targets ~400 words; "How" / "Benchmarks" /
> "What I want from HN" go in follow-up comments, not the first post.
>
> **Final chosen title:**
> `Show HN: NimRTC – A from-scratch C++20 WebRTC engine, ~5.6MB static lib`

---

## Selected title (locked)

1. ✓ **SELECTED — `Show HN: NimRTC – A from-scratch C++20 WebRTC engine, ~5.6MB static lib`**

Rationale: 3 hits in one title — (a) "Show HN" tag, (b) "WebRTC" keyword
for search, (c) "5.6MB static lib" is the single most-memorable number in
the project and the differentiator vs. libwebrtc at 150–250 MB. Most
information per character for a 70-character HN title limit.

## Alternatives (for HN comment / future posts; do not retitle live)

These were the runner-ups from the A/B shortlist. Keep for follow-up
content (Twitter / lobste.rs / r/cpp / r/programming) — do **not** retitle
on HN, the "controversy" ranking punishes title edits.

- #2 — `Show HN: I rebuilt WebRTC's media plane in C++20 (no SCTP, ~50k LOC)`
  *(most contrarian; good lobste.rs hook)*
- #3 — `Show HN: NimRTC – Embeddable WebRTC for Xinchuang / embedded Linux aarch64`
  *(niche keyword; better for /r/cpp or 中国-OSS channels)*
- #4 — `Show HN: Why I stopped fighting libwebrtc and wrote a new C++ WebRTC engine`
  *(most personal; better as a blog-post title than a HN title)*

---

## First post (target ~400 words)

Hi HN — I've been building NimRTC, a from-scratch C++20 implementation of the
WebRTC media plane, on and off, since late 2025. v0.11.0 just shipped and
Chrome ↔ NimRTC audio + video interop is green on the full 4-platform CI
matrix (Windows, Linux x86_64, macOS arm64, Linux aarch64).

### Why a new WebRTC engine in 2026

The honest answer is that libwebrtc is the de-facto engine, and on a good day
I would just use it. But:

- **It's enormous.** A static link is 150–250 MB. The Chromium monorepo
  dominates every build I do.
- **Crypto and codecs are black-box.** I work on Xinchuang / GM/T-regulated
  deployments where the audit trail is the point — forking libwebrtc to
  swap the DTLS backend is not a happy path.
- **AI Agents / teleop need things libwebrtc doesn't expose.** A clean
  pre/post-3A PCM tap (so a wake-word model or a streaming ASR can listen
  to either the raw mic or the cleaned signal) is a 200-line file in
  NimRTC and a 4-week integration in libwebrtc.
- **Embedded Linux aarch64.** Robotics / industrial SBCs need a real RTC
  stack at single-digit MB, not a 250MB binary.

### What NimRTC looks like

- **L0–L3 compile-time layering** with a Plugin Adaptation Layer (PAL).
  Same source tree ships a full P2P client *and* an SFU gateway (SFU
  skips L2 codecs). ~50k LOC of C++20.
- **Header-only plugin interfaces** for transport, ICE, RTP, SDP, jitter
  buffer, audio 3A, codecs, video source/sink, DataChannel, hardware
  codecs. The seam surface is small, and the public API doesn't change
  when you swap a plugin.
- **Pluggable crypto.** DTLS backend resolves through a registry keyed on
  `EngineConfig::dtls_name`. Today: OpenSSL, wolfSSL, mbedTLS. GMSSL is
  the planned next step (P4) for Xinchuang.

### What's working vs. what's not (be honest)

- ✅ Chrome ↔ NimRTC audio + video Case D, 4 platforms.
- ✅ 5 assembly profiles (`relay` / `transport` / `agent` / `agent-gateway` / `relay-agent`) with
  strict JSON↔C++ round-trip tests.
- ✅ Pre/post-3A PCM tap with race-free setter semantics.
- ❌ DataChannel over SCTP-over-DTLS is **deferred to v1.x** — Chrome
  is moving DataChannel to WebTransport over QUIC and I followed that
  signal. v1.x will land on msquic.
- ❌ iOS / Android native builds are roadmap, not P1.
- ❌ Production-grade quality lands in P3 / P4. v0.11.0 is a "Beta 前哨"
  — API experimental through v1.0.0.

### Try it

```
git clone --recurse-submodules https://github.com/NimRTC/nimrtc
cd nimrtc
cmake --preset debug
cmake --build build --target loopback-p2p
./build/examples/Debug/loopback-p2p     # 30-second in-process P2P smoke
```

Five-min "Hello, World" is in the README; longer-form design notes are in
`docs/`. A Wasm browser demo is on the roadmap for v0.12.0.

### What I want from HN

- Architecture critique — is the L0–L3 + PAL split the right call?
  mediasoup chose runtime composition; LiveKit chose service-isolation.
  What did I get wrong?
- Plugin interface feedback — `docs/plugin_author_guide.md` is the
  primary contract. Anything that would make it easier / harder to
  contribute a backend?
- v1.x design partners — anyone building real-time voice/video and
  willing to talk about what *they'd* want from a WebTransport/QUIC data
  plane. Email is in the README.
- GM/T cryptography collaboration — if you have ship-ready SM2/SM4 code
  and want to register a DTLS backend, the seam is open.

Repo: <https://github.com/NimRTC/nimrtc>
License: Apache-2.0
Version: v0.11.0 Beta 前哨 (tagged 2026-10-09)

---

## Reply template for "why not just use Pion / mediasoup / LiveKit?"

> Those are great projects and the right answer for most teams. NimRTC
> targets a specific niche: **C++20, embeddable at single-digit MB,
> Xinchuang/GM/T-aware, AI-Agent-friendly PCM tap.** If you're in JS/Go
> land, Pion/mediasoup/LiveKit are the right call. If you're shipping on
> aarch64 SBCs or need a swappable DTLS backend for regulatory reasons,
> NimRTC's the option.

## Reply template for "libwebrtc has feature X"

> Likely yes — libwebrtc is the most complete WebRTC implementation on
> the planet. NimRTC is intentionally narrower (media plane + Opus + H.264
> + VP8, no screen share yet, no AV1, no iOS/Android, no SCTP-data-channel
> yet). The bet is that in 2026 the wire interop is stable, the codec
> surface is mostly frozen, and a focused C++20 re-implementation can
> stay readable for a decade.

## Reply template for "benchmarks?"

> Static lib size: 5.6 MB (Release, MSVC, x86_64). Source LOC: ~50k
> excluding tests. The Chrome interop case D runs in CI on every push
> across 4 platforms; `tools/run_e2e_acceptance.py` is the entry point.
> I don't have public throughput/JIT numbers yet — that's a v0.12.0 item.

---

**Disclaimer for the actual Show HN post:** strip the boilerplate above
("Reply template", "What I want from HN" sub-bullets). HN comments reward
candor and brevity; keep the first post < 500 words and add detail only
when a specific commenter asks.
