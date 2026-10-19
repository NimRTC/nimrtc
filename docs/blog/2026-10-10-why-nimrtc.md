---
title: "Why a from-scratch WebRTC engine in 2026?"
date: 2026-10-10
author: NimRTC maintainers
tags: [webrtc, cpp20, open-source, design-notes]
status: opinion-piece
---

# Why a from-scratch WebRTC engine in 2026?

> Companion piece to the [v0.11.0 Beta 前哨 release notes](docs/releases/v0.11.0-beta.md).
> This is the design-rationale long-form; the release notes are the
> what's-new. If you only have 5 minutes, the [README](../README.md) and
> the [Show HN draft](docs/marketing/show-hn-post.md) are tighter.

---

## The libwebrtc question

Every time I open a public discussion about NimRTC, the first comment
is the same: *why not just use libwebrtc?*

It's a fair question. libwebrtc is the most complete WebRTC
implementation on the planet, and on a good day I would just link it.
The "good day" is, roughly, the case where:

- You are building a web app.
- Your backend runs x86_64 Linux.
- You have a few hundred MB to spare in your binary.
- Your crypto path is whatever libwebrtc ships, and you don't need an
  audit trail.
- Your 3A pipeline is whatever libwebrtc ships, and you don't need to
  peek inside it.

If all five apply, the answer is *use libwebrtc*. Nothing in this
project competes with that.

What I'm optimising for is the case where **one or more of those five
is false.** Specifically:

- **Embedded Linux aarch64.** Robotics, industrial SBCs, Raspberry
  Pi-class boards. libwebrtc at 150–250 MB is not a small binary in
  this context. NimRTC is ~5.6 MB.
- **Xinchuang / GM/T-regulated deployments.** The audit trail is the
  point. Forking libwebrtc to swap the DTLS backend is a months-long
  project and produces a fork you have to maintain. NimRTC's
  `EngineConfig::dtls_name` lets you register a new backend in a few
  hundred lines of header + .cpp.
- **AI Agent / teleop / cloud gaming.** I want a clean pre/post-3A PCM
  tap so a wake-word model or a streaming ASR can listen to either the
  raw mic or the cleaned signal. In NimRTC that's a 200-line file
  (`src/audio/3a/pcm_tap.cpp`); in libwebrtc it's a four-week
  integration that touches five BoringOwned modules.
- **Readability.** I want a real-time media engine that a single
  engineer can hold in their head. ~50k LOC of C++20 with a strict
  L0–L3 layer split and header-only plugin interfaces is, I claim, that.

I'm not going to pretend NimRTC is "better than libwebrtc" at WebRTC.
libwebrtc is more complete, has more codecs, more platforms, more
contributors, more users, more tests, more security audits. What I
claim is that **for the specific niches above, the from-scratch
implementation is the smaller, more auditable, more adaptable choice.**

---

## The architectural bet: L0–L3 + PAL

The hardest question in any medium-sized C++ project is: *where do I
put the seams?* Plugins, callbacks, virtuals, generic interfaces,
messaging channels — there are too many choices and each one is
load-bearing for the next ten years.

NimRTC's bet is a four-layer compile-time split:

```
L3  Engine façade (NimRTCEngine)          ← the only thing app code sees
L2  Codecs + 3A                          ← skipped by `relay` profile
L1  Wire / state (RTP, RTCP, SDP, ICE,   ← the bit that's WebRTC
     DTLS, SRTP, JB, BWE)                   wire-compatible
L0  Primitives (bytes, log, time, status)
PAL Plugin Adaptation Layer               ← resolved at build time
Plugin interfaces (header-only)           ← header in src/plugins/
```

Each layer is a CMake INTERFACE / STATIC library. The **profile** is
also a compile-time selection: `transport` (P2P client) / `relay`
(forwarding only, no L2) / `agent-gateway` (PCM dual-tap) / `cloudgame`
(high-bitrate + aligned input) / `relay-agent` (combo).

The reason this is interesting: the same source tree ships both a P2P
client *and* a relay gateway. LiveKit and mediasoup are server-only;
libwebrtc is monolithic. The "P2P client and relay in one source tree" is
a thing nobody else in the OSS WebRTC ecosystem has, as far as I can
tell, and it's a load-bearing feature for the AI-Agent use case (the
gateway side often needs both directions: a thin client to the user's
browser and a forwarding path to a downstream worker).

The PAL (Plugin Adaptation Layer) is the second bet: every capability
is resolved at build time through `core::PluginRegistry`. Zero runtime
indirection, public API unchanged. This is *not* a service-locator
pattern at runtime — it's a typed slot table that the linker sees
during PAL Slice 2's `register_all_default_plugins()`. The point is
that you can swap DTLS from wolfSSL to GMSSL by linking a different
translation unit and the engine doesn't notice.

ADR-009 has the full design record; the key invariants are:

- `src/core/` is the only INTERFACE library.
- `engine.hpp` does not include any concrete module header.
- Singletons live in `nimrtc::core::`.

Those three rules keep the seam surface small and stable.

---

## The crypto bet: register, don't fork

The DTLS seam is, I think, the most underrated feature of the
project. Today you can register a wolfSSL, OpenSSL, mbedTLS, or
BoringSSL backend; the planned next step is GMSSL for Xinchuang
deployments.

The code path looks like this in `engine.cpp`:

```cpp
auto factory = reg.get_dtls_session(cfg.dtls_name);  // or "wolfssl" if empty
std::unique_ptr<IDtlsSession> dtls = factory->create();
```

That's it. The engine never names wolfSSL directly. The seam is small
(`IDtlsSession` has ~12 methods), and the `DtlsSessionWolfSSL` is a
single `~800 LOC` adapter that translates to wolfSSL's DTLS 1.3 API.

If you want to register GMSSL tomorrow:

```cpp
// in your_gmssl_backend.cpp
class GmsslDtlsFactory : public dtls::IDtlsSessionFactory { /* ... */ };
NIMRTC_REGISTER_DTLS_SESSION("gmssl", &GmsslDtlsFactory::singleton());
```

```cpp
// in your app
EngineConfig cfg;
cfg.dtls_name = "gmssl";
NimRTCEngine engine(cfg);
```

No fork, no patching, no `--build-flag`. This is the kind of thing
that makes a regulatory audit possible: the audit can review the
GMSSL adapter in isolation, sign off on it, and the rest of the
engine doesn't change.

The trade-off: the seam surface is conservative, and you can only
swap what fits through `IDtlsSession`. That's intentional — a bigger
seam would mean a less stable public API. The cost is that some
TLS-level features (e.g. client-certificate chains outside the WebRTC
identity model) won't fit through the seam. If you need those, the
fork is still on the table, but at least for the WebRTC
identity-model crypto path, the seam is enough.

---

## The data-plane deferral: WebTransport over QUIC

v0.11.0 does **not** ship Chrome ↔ NimRTC DataChannel interop. This is
the headline item that didn't make the cut, and it's worth a paragraph
explaining why.

The original plan was: wire `IDtlsSession::srtp_keying_material()` to
`ISctpSocket::set_dtls_keys()`, and bridge DTLS transport to SCTP
socket for both inbound and outbound datagrams. The physical wiring
never landed on `main`; Chrome-headless e2e stays 0/4; v0.11.0 ships
the IDataChannel seam against an in-process `test_datachannel_engine`
5/5 substitute.

There are two reasons I decided not to push the SCTP-over-DTLS path
harder:

1. **The data plane is moving.** Chrome's stated direction is
   "DataChannel future = WebTransport over QUIC." Investing
   engineering in SCTP-over-DTLS in late 2026 means investing in a
   path that the dominant browser is actively deprecating. I'd rather
   follow Chrome's signal.
2. **The seam is already there.** `IDataChannel` is a stable interface
   in v0.11.0. The transport under it can be swapped from "in-process
   stub" (today) to "SCTP-over-DTLS" (legacy interop) to
   "WebTransport-over-QUIC" (v1.x) without changing the public API.

The v1.x design will land on **msquic** as the QUIC stack
(MIT-licensed, Windows-first, cross-platform CI). The IDataChannel
seam stays the same; the transport changes underneath. This is exactly
the kind of swap the L0–L3 split was designed to enable.

---

## What I want from the community

A from-scratch WebRTC engine is, in 2026, a weird thing to spend
your time on. The legitimate reasons to do it are the ones I listed
above; the rest of the work is just making it real.

What I'd most want from someone reading this:

- **Architecture critique.** Is the L0–L3 + PAL split the right
  call? mediasoup chose runtime composition; LiveKit chose
  service-isolation; Pion chose language-ergonomics. What did I get
  wrong?
- **Plugin interface feedback.** `docs/plugin_author_guide.md` is
  the primary contract. Anything that would make it easier / harder
  to contribute a backend?
- **v1.x design partners.** If you're building real-time voice /
  video and willing to talk about what *you'd* want from a
  WebTransport / QUIC data plane, my email is in the README.
- **GM/T cryptography collaboration.** If you have ship-ready
  SM2 / SM4 code and want to register a DTLS backend, the seam is
  open. The full P4 work is still ahead of us, but the contract is
  stable.
- **Star / share if you've ever wanted a smaller, auditable WebRTC
  in C++.** The single most useful thing a casual reader can do for
  the project is the GitHub star — it determines whether the next
  search for "webrtc alternative" finds us.

---

## Appendix: numbers

- **Source LOC** (L0–L3, excluding tests / examples / docs): ~50k.
- **Static lib size** (Release, MSVC, x86_64): 5.6 MB.
- **CI matrix:** Windows / Linux x86_64 / macOS arm64 / Linux aarch64
  — all four pass build + unit tests on every push.
- **Wire interop:** Chrome, Firefox, libwebrtc (verified by
  `tools/run_e2e_acceptance.py --case d`).
- **Plugin interfaces:** 12 (ITransport, IICETransport, IRTP, ISDP,
  IJB, IAudio3A, ICodec, IVideoSource, IVideoSink, IDataChannel,
  IHwCodec, IHw3A).

These are the only numbers I cite publicly; if you spot one that's
stale, the release notes are the source of truth and the README is
what to update first.

---

*Thanks for reading. If you got this far, you might enjoy the
[Show HN draft](docs/marketing/show-hn-post.md) — it's the same
project, same author, different register.*
