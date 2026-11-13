# ADR-021: Codec bundling posture — permissive vendoring, direct vendor-SDK hardware

| | |
|---|---|
| Number | 021 |
| Status | **Accepted** |
| Date | 2026-11-14 |
| Phase | M1 (remote-desktop plan, Track B) |
| Bound to | v1.3.0 |
| Amends | the video plan's OD-1 ("no bundled software video encoder / decoder") |
| Related | `docs/plan/remote-desktop-plan.md` §0 (RD-2, RD-10) and Track K (K1, K8, K8b); `docs/plan/video-pipeline-v2-plan.md` §B.7.1 (OD-1) and Track X (O-3); `docs/adr/ADR-015-api-stability-policy.md`; `docs/adr/ADR-025-sdp-rtp-not-pluggable.md`; `src/third_party/vendor.json`; `tools/check_vendor.py`; `NOTICE`; `AGENTS.md` L5 |

## Context

The video plan's **OD-1** says the project bundles **no** software video
encoder or decoder:

> **OD-1 — No bundled software video encoders.** NimRTC ships no bundled
> software video encoder or decoder for H.265 or AV1. The seams, payload
> layers, SDP layers and the negotiation are complete and tested; the codec
> implementation is supplied by the host application or by an out-of-tree
> plug-in.
>
> — `docs/plan/video-pipeline-v2-plan.md` §B.7.1

The reasoning behind it is sound and is **not** being reversed. x265 is
GPL-2.0, which is incompatible with NimRTC's permissively licensed
distribution model (`AGENTS.md` 禁区: no system-installed codec libraries
bypassing the vendor policy). A library whose size is a published selling
point (`tools/benchmark_size.py`) does not want a software codec in it by
default.

Remote desktop breaks it, for two reasons that a camera stream does not
have:

1. **The picture is text.** A software H.264 encode of a 1080p desktop at
   30 fps costs roughly the whole of one CPU core on a mid-range laptop,
   and the remote-desktop plan's §B.6 per-frame CPU budget does not survive
   it. Only the *hardware* path fits the budget — and a machine without a
   hardware encoder (a VM, an old laptop, a CI runner) needs *some*
   software encoder or it cannot participate at all.
2. **The picture is flat.** Screen content compresses far better under
   AV1 with intra block copy than under H.264, and the encoders that expose
   the screen tools (`svt-av1`) are software.

Meanwhile **FFmpeg** — the obvious answer, since it wraps every codec
including the hardware ones, cross-platform — is LGPL-2.1. The project has
deliberately not signed up for LGPL's distribution obligations, and the
binary weight is unacceptable for an embedded-oriented engine.

The precise scope of the licence blocker is worth stating, because it is
narrower than "FFmpeg is LGPL": FFmpeg's own lists do **not** treat the
hardware encoders (`nvenc`, `qsv`, `vaapi`, `videotoolbox`, `ffnvcodec`)
as GPL or nonfree. An LGPL build can carry every hardware encoder. The
blocker is the **software** codec set (`libx264` is GPL) plus the binary
weight.

But bundling FFmpeg would still lose to a direct implementation, on
non-licence grounds:

- NimRTC already implements the hardware backends directly against the
  vendor SDKs (`src/modules/h264/src/hw_backends.cpp`, `src/modules/nvcodec/`).
  FFmpeg would replace working zero-copy paths with an
  `AVFrame` / `AVBufferRef` model that does **not** interoperate with
  `video_frame::GpuBuffer` — the D3D11 → CUDA route has no FFmpeg `hwmap`
  equivalent and would need a full CPU round-trip, which is exactly what
  the remote-desktop plan's **B10** forbids.
- The project already vendors mbedTLS, libsrtp, libopus, libjuice and
  nlohmann_json. Adding FFmpeg's transitive library set changes the
  dependency, licence and NOTICE surface by an order of magnitude.

So the question this ADR answers is **not** "FFmpeg or not FFmpeg". It is:
*given that we do not bundle FFmpeg, which codecs do we bundle directly,
under what licence rules, and with what platform-coverage obligation?*

## Decision

### Decision 1 — OD-1 is amended: no bundled **copyleft** codec

**OD-1 is narrowed, not reversed.** The prohibition becomes:

> NimRTC may vendor a codec whose licence is **permissive** *and* whose
> **patent grant is distributable** (BSD / BSD-3-Clause / Apache-2.0, plus
> the AOM Patent License 1.0 or an equivalent AVC patent portfolio).
> **Copyleft (GPL / LGPL) codecs stay out of the tree.** No system-installed
> codec is ever linked (AGENTS.md L5).

The reasoning that produced OD-1 is preserved verbatim: a copyleft codec
in the tree is incompatible with the distribution model, and that is a
property of the *licence*, not of the codec's usefulness. A permissive
codec with a distributable patent grant has no such property.

What changes is the *scope* of the prohibition. Before this ADR, "no
bundled codec" also meant "no bundled *permissive* codec", and the
justification for that part was binary weight. Binary weight is now
handled by an **option**, not by a prohibition: a heavy codec is behind a
`NIMRTC_VENDORED_<NAME>` switch that defaults OFF, and a light one that
every session needs defaults ON. The licence filter is absolute; the
weight filter is a default.

### Decision 2 — The two filters, both mandatory

A codec may be vendored only if it clears **both**:

| Filter | Rule | Source |
|---|---|---|
| **Licence** | Permissive (BSD / BSD-3-Clause / Apache-2.0 / MIT) **and** a distributable patent grant. Copyleft (GPL / LGPL) is out, with no exceptions. | remote-desktop plan RD-2 |
| **Platform** | One library covering Windows + macOS + Linux beats three platform implementations. A platform-specific implementation must justify itself in the item's acceptance criterion. | remote-desktop plan RD-10 |

The **patent** half of the licence filter is not decorative. `dav1d` and
`svt-av1` are BSD-licensed *and* carry the AOM Patent License 1.0;
`openh264` carries the AVC patent portfolio. Those grants are
distributable, so the licence filter passes — but the grant **must be
reproduced in the distribution materials** (`NOTICE`), or the licence
filter has not actually been met. A licence that passes without its
attribution duty discharged is not a licence the project has met.

The **platform** half is a *default*, not an absolute. Two cells have no
cross-platform open-source answer, and the plan says so rather than
pretending otherwise:

| Cell | Why there is no cross-platform library | What the plan does |
|---|---|---|
| **Hardware video encode / decode** | Every hardware encoder is vendor SDK territory: NVENC (NVIDIA), AMF (AMD), QSV (Intel), VA-API (Linux-only), VideoToolbox (Apple), MediaCodec (Android). There is no portable abstraction *and* no portable licence. | Keep the per-platform backends in `cmake/NimRTHwPlugins.cmake`. The cross-platform part is the **selection policy** (`HwVideoBackendRegistry`), which is portable. |
| **Screen / display capture** | The closest cross-project options are `xcap` (Rust, Apache-2.0, Win/macOS/Linux, Wayland capture marked incomplete) and `scap` (Rust, MIT, Win/macOS/Linux via WGC / ScreenCaptureKit / PipeWire). Both are **Rust**, and NimRTC is C++20. GStreamer could cover it but is LGPL (excluded). | Write the three platform backends (A3/A4/A5) as thin adapters over one portable capture layer. |

A platform-specific implementation outside those two cells needs an ADR
amendment; it does not get a case-by-case argument.

### Decision 3 — The selection table

This is the canonical table. A candidate codec that does not appear here,
or appears here with a verdict other than "vendor", has not been
evaluated under Decision 2.

| Library | Does what | Licence | Platforms | Verdict |
|---|---|---|---|---|
| **`libyuv`** | YUV/RGB scale + convert + rotate | BSD-3-Clause | **Win / macOS / Linux / Android / iOS** | **Vendor — default** (Track C). Actively maintained by Google/Chromium (commits on 2026-11-11), consumed by WebRTC and WebKit (WebKit resynced 2026-08). One library covers every platform we ship. |
| **`dav1d`** | AV1 decode | BSD-2-Clause + AOM Patent License 1.0 | **Win / macOS / Linux** | **Vendor — default** (Track B). The patent licence must be reproduced in the distribution materials. |
| **`openh264`** (source-built) | H.264 encode | BSD-2-Clause + AVC patent portfolio | **Win / macOS / Linux** | **Vendor — default** (Track B). Source-build avoids Cisco's *prebuilt binary* conditions (end-user download + "OpenH264 Video Codec provided by Cisco Systems, Inc." display); the patent attribution duty remains. |
| **`libvpx`** | VP8 / VP9 encode | BSD-3-Clause | **Win / macOS / Linux** | **Optional vendor** behind `NIMRTC_VENDORED_LIBVPX` (Track B) — the only cross-platform way to offer Chrome-compatible VP9. |
| **`svt-av1`** | AV1 encode + **screen-content tools** | BSD-3-Clause Clear + AOM Patent License 1.0 | **Win (MSVC + NASM) / macOS / Linux** — official CI covers all three | **Optional vendor** behind `NIMRTC_VENDORED_SVT_AV1` (Track B). Heavy build, opt-in. Its screen mode (`--scm`, intra block copy) is exactly the feature remote control needs, and it is the *only* cross-platform software AV1 encoder that has it. |
| **`libsamplerate`** (Secret Rabbit Code) | Sample-rate conversion | BSD-2-Clause (since 2016) | **Win / macOS / Linux** | **Vendor — default** (Track F, F5). CMake build system, actively built on GitHub Actions. Chosen over SpeexDSP: SpeexDSP's resampler is BSD-3 but its last release is **2022-06** (unmaintained since), while libsamplerate is maintained. |
| **`portaudio`** | Cross-platform audio I/O (capture + playback) | MIT | **Win (WASAPI / MME / DSound / ASIO) / macOS (Core Audio) / Linux (ALSA / JACK / PulseAudio)** | **Vendor — default** (Track F, F1/F2). One library replaces the WASAPI + CoreAudio + ALSA/PulseAudio triple. *Caveat:* PortAudio's own sample-rate conversion is naive, so NimRTC's resampling goes through libsamplerate (F5) rather than PortAudio's. |
| **`x265`** | HEVC encode | GPL-2.0 | all | **Excluded** — copyleft (RD-2). |
| **`libaom`** | AV1 encode | BSD-2-Clause | all | **Excluded for now** — build cost and encode latency; SVT-AV1 covers the same need with screen tools. |
| **FFmpeg / GStreamer** | everything | LGPL-2.1 | all | **Not vendored** — LGPL is a distribution obligation the project has not signed up for, and the binary weight is unacceptable for an embedded-oriented engine. **Precise scope of the blocker:** the LGPL problem is *not* the hardware codecs — `nvenc`, `qsv`, `vaapi`, `videotoolbox` and `ffnvcodec` are **not** on FFmpeg's GPL or nonfree lists, so an LGPL build can carry every hardware encoder. The blocker is the **software** codec set (`libx264` is GPL) plus the binary weight. **But bundling FFmpeg still loses on this project:** NimRTC already implements the hardware backends directly against the vendor SDKs (§0 inventory), so FFmpeg would replace working zero-copy paths with a `AVFrame`/`AVBufferRef` model that does not interoperate with `video_frame::GpuBuffer` — D3D11→CUDA has no `hwmap` route in FFmpeg and needs a full CPU round-trip, which is exactly what B10 forbids. A *host* may still register an FFmpeg- or GStreamer-backed codec / capture plug-in through `plugins::IVideoCodecFactory` and `IVideoCaptureFactory`; the seams exist for exactly that. |
| **`usrsctp`** (already vendored) | SCTP | BSD-3-Clause | all | **Re-activated, but not as a "flip a switch"** — the vendored `UsrsctpSocket` delivers in-process over loopback (`src/sctp/src/usrsctp_socket.cpp:15-39`) and its output path is a no-op stub. Carrying a real association over DTLS (RFC 8841) is **G1b**, a transport-layer project of its own. |

Two rows of this table are worth reading twice, because they are the ones
that will be re-argued:

- **`libaom` is excluded, not banned.** It passes the licence filter
  (BSD-2-Clause). It fails on build cost and encode latency, and
  `svt-av1` covers the same need *with* screen tools. If `svt-av1` ever
  fails to build on a required leg, `libaom` is the first candidate to be
  re-opened — it is a default change, not an ADR amendment.
- **FFmpeg / GStreamer being "not vendored" is not "not supported".** The
  `plugins::IVideoCodecFactory` / `IVideoCaptureFactory` seams exist so a
  host can register an FFmpeg- or GStreamer-backed implementation without
  the project taking on the licence or the binary.

### Decision 4 — The licence allowlist

A vendored dependency's licence must be one of:

| Licence | Notes |
|---|---|
| **BSD-2-Clause** | Accepted. `dav1d`, `openh264`, `libsamplerate`. |
| **BSD-3-Clause** | Accepted. `libyuv`, `libvpx`, `libsrtp`, `libopus`, `usrsctp`, `googletest`. The `BSD-3-Clause Clear` variant is accepted on the same terms (no endorsement clause) — that is `svt-av1`'s grant. |
| **Apache-2.0** | Accepted. `mbedtls`, `gmssl`. The Apache-2.0 patent grant is part of the licence. |
| **MIT** | Accepted. `portaudio`, `libjuice`, `msquic`, `nlohmann_json`, `nv_codec_headers`. |

**Not on the allowlist**, and therefore not vendorable under this ADR:

- **GPL-2.0 / GPL-3.0** (e.g. `x265`) — copyleft.
- **LGPL-2.1 / LGPL-3.0** (e.g. FFmpeg, GStreamer) — weak copyleft, and
  the distribution obligation is one the project has not signed up for.
- **AGPL** — copyleft plus a network-use clause.
- **MPL-2.0** (weak copyleft, file-level) — see the note below.

Two notes, so the allowlist is not misread as a retroactive judgement on
the tree:

1. **Patent grants are part of the licence.** An AV1 codec additionally
   needs the **AOM Patent License 1.0** (`dav1d`, `svt-av1`, `libaom`); an
   H.264 codec needs an **AVC patent portfolio** grant (`openh264`). The
   grant must be reproduced in `NOTICE`.
2. **Pre-existing exceptions.** Two dependencies already in the tree sit
   outside this allowlist and are **not** covered by it:
   `libjuice` (MPL-2.0) and `wolfssl` (dual GPL-2.0-or-commercial). They
   were vetted when they were adopted, under the licence policy that
   applied then. This ADR governs **new** vendoring and does not reopen
   them; doing so would be a separate, explicit decision.

### Decision 5 — The `NIMRTC_VENDORED_*` option policy

**Every** vendored dependency gets a `NIMRTC_VENDORED_<NAME>` CMake option.
The switch means "use the vendored source build of `<NAME>`". There is
**no system fallback**: `NIMRTC_VENDORED` (the umbrella switch, `AGENTS.md`
L5) stays ON, and turning an individual option OFF means the dependency
is simply not in the build — never "link the system copy".

| Dependency | Option | Default | Why |
|---|---|---|---|
| `libyuv` | `NIMRTC_VENDORED_LIBYUV` | **ON** | Small, SIMD, and the capture→encode path needs it on every leg; a default-OFF would mean the default build has no scaler. |
| `dav1d` | `NIMRTC_VENDORED_DAV1D` | **ON** | The only software AV1 decoder in the tree; the software path cannot decode AV1 without it. |
| `openh264` | `NIMRTC_VENDORED_OPENH264` | **ON** | The software H.264 baseline. Already shipped under this switch. |
| `libsamplerate` | `NIMRTC_VENDORED_LIBSAMPLERATE` | **ON** | The engine does not resample implicitly (audio plan DEC-3), so the device path *needs* an explicit resampler. |
| `portaudio` | `NIMRTC_VENDORED_PORTAUDIO` | **ON** | One cross-platform audio I/O implementation replaces three platform backends. |
| `libvpx` | `NIMRTC_VENDORED_LIBVPX` | **OFF** | Optional; needed only for VP8/VP9 interoperability. |
| `svt-av1` | `NIMRTC_VENDORED_SVT_AV1` | **OFF** | Heavy build (and NASM on Windows); needed only for software AV1 *encode* with screen tools. |

The **default-ON vs default-OFF** test is a single question:

> **Does a session that cannot use this dependency still work?**

If **no** — the dependency is on the critical path of the default build
(the scaler, the software decoder, the resampler) — the option defaults
**ON**, and a distributor who wants it gone pays the feature. If **yes**
— the dependency serves an optional capability (VP9, software AV1 encode)
— the option defaults **OFF**, and a user who wants the capability turns
it on. The weight argument alone never decides the default; it decides
*OFF for optional, ON for critical*.

An option's default may differ per platform (Decision 6). It may not
differ without the reason being written down.

### Decision 6 — The platform-coverage requirement (K8b)

**A vendored dependency must build on every CI leg its capability applies
to** (Windows MSVC, Linux x86_64, Linux aarch64, macOS arm64), **or it
is behind an option that defaults OFF on the platforms it does not
build.**

The CI legs are the project's definition of "a platform we ship". A
dependency that only builds on one of them is a dependency whose
capability exists on one platform and not on the others — a claim the
README must then make honestly, and a build that breaks the moment
somebody enables it on the wrong machine.

Concretely:

- `tools/check_vendor.py` records each dependency's **platform matrix**,
  and CI verifies that each **default-ON** dependency is actually built
  on every leg it claims.
- The `NOTICE` entry for each library states the licence **and** the
  platform coverage.
- A dependency that builds on only one platform needs a **documented
  reason**, and the option defaults OFF on the platforms it does not
  build.

**No exception is granted by default.** An exception needs an ADR
amendment. This is the rule that keeps a single-platform dependency from
becoming a silent regression in the default build — the failure mode the
remote-desktop plan records as risk **R-11** (a vendored dependency that
turns out not to build on one of the CI legs, `svt-av1`'s NASM
requirement being the standing example).

### Decision 7 — What this ADR does not change

- **The hardware-first default is untouched.** Hardware encoding stays
  the default and software stays the fallback (the video plan's OD-2, the
  remote-desktop plan's RD-1). Vendoring a software codec does not make
  it the default; it makes the fallback *real*.
- **The codec seam is untouched.** `plugins::IVideoCodec` /
  `IVideoEncoder` / `IVideoDecoder` remain the way a codec is reached
  (ADR-025's rule that the engine reaches concrete modules through
  `plugins::I*` seams). A vendored codec is registered as a plug-in, not
  wired into the engine.
- **`src/third_party/` remains the only vendor location** (AGENTS.md L5),
  every dependency is pinned in `src/third_party/vendor.json`, verified by
  `tools/check_vendor.py`, and listed in `NOTICE` with its licence and
  platform coverage.
- **The public API is unaffected.** This ADR adds no public symbol, so
  ADR-015's stability policy has nothing to say about it beyond "no
  breaking change". The `NIMRTC_VENDORED_*` options are build-time, not
  API.

## Consequences

### What NimRTC gains

- **A real software fallback.** A machine with no hardware encoder — a
  CI runner, a VM, an old laptop — can produce real pixels, which is what
  makes the remote-desktop bring-up loop provable at all.
- **Screen-content coding where it exists.** `svt-av1`'s screen mode
  (intra block copy) is the cross-platform software encoder feature that
  remote control actually needs, and it is available behind one option.
- **One implementation per capability, not three.** `libyuv` and
  `portaudio` each replace a set of platform backends; the platform code
  that remains is the leaf OS call, and the effort goes into the portable
  mechanism (RD-10).
- **A licence filter that is checkable.** The allowlist is short, the
  patent-attribution duty is explicit, and `NOTICE` records both — so the
  distribution question has an answer that does not depend on who is
  asking.

### What NimRTC gives up

- **The simplicity of "no bundled codec".** OD-1's clarity was worth
  something; from now on a reader of the tree has to ask which codec is
  in it and under which option.
- **Build weight, on the default-ON set.** `libyuv`, `dav1d`, `openh264`,
  `libsamplerate` and `portaudio` are in the default build. The heavy
  optional ones are not.
- **The binary-weight argument no longer decides anything.** OD-1 used it
  to rule out permissive codecs entirely. It now decides only the
  *default* of an option, which is the right place for it.

### What NimRTC must keep doing manually

- **Reproducing every patent grant in `NOTICE`** for every vendored
  codec. The licence filter is only met when the attribution duty is
  discharged, and that is a manual, per-dependency obligation.
- **Keeping the platform matrix honest.** `tools/check_vendor.py` and CI
  verify that a default-ON dependency builds where it claims; keeping the
  matrix in `vendor.json` and `NOTICE` current is manual work every time
  a dependency is added or a leg is added.
- **Reviewing every new candidate against both filters.** The table in
  Decision 3 is the record of that review, and a new candidate does not
  enter the tree by being merely permissive — it enters by being
  permissive *and* cross-platform *and* worth the weight.
- **Honest README rows.** A capability that exists only where a default-OFF
  option is ON must be documented as such, under the video plan's G5 rule
  and the remote-desktop plan's K9.

## Alternatives considered

### A — Keep OD-1 exactly as it is

Rejected. OD-1 says "no bundled software video encoder or decoder", and
the tree now ships exactly that: `src/modules/h264/src/openh264_encoder.cpp`
and `src/modules/av1/src/dav1d_decoder.cpp` are real codecs, built from
vendored source. Keeping OD-1 unchanged would leave the plan's stated
posture contradicting the tree, which is the documentation-honesty failure
the video plan's G5 and the remote-desktop plan's K9 exist to prevent.

Beyond that, the substantive objection stands on its own: a
remote-desktop product that cannot ship an encoder is not a product, and
the prohibition's own stated reason (x265 is GPL) does not apply to
`openh264` (BSD-2-Clause) or `dav1d` (BSD-2-Clause + AOM Patent License
1.0). **The amendment narrows OD-1 to the reason OD-1 actually had.**

### B — Vendor FFmpeg

Rejected, on three independent grounds, any one of which would be enough:

1. **Licence.** LGPL-2.1 carries a distribution obligation (relinkability
   for a modified library) that the project has not signed up for. The
   software codec set it would bring is GPL (`libx264`), and the
   copyleft filter has no exceptions.
2. **Binary weight.** An embedded-oriented engine whose size is a
   published selling point does not carry FFmpeg's library set.
3. **Architecture.** FFmpeg's `AVFrame` / `AVBufferRef` model does not
   interoperate with `video_frame::GpuBuffer`, and FFmpeg's
   `AVHWFrameContext` / `hwmap` has **no D3D11 → CUDA route**, so the
   path would force a full CPU round-trip — exactly what the
   remote-desktop plan's **B10** forbids. Bundling FFmpeg would replace
   working zero-copy hardware paths with a slower genericity the product
   does not want.

The hardware-codec half of FFmpeg is *not* the objection: `nvenc`, `qsv`,
`vaapi`, `videotoolbox` and `ffnvcodec` are LGPL-buildable, and NimRTC's
own backends already cover them directly against the vendor SDKs. A host
that wants FFmpeg can register an FFmpeg-backed plug-in through the
existing seams.

### C — Vendor `x265` despite GPL

Rejected. This is the one candidate the licence filter exists for. x265 is
GPL-2.0, and a GPL-2.0 library linked into the NimRTC distribution makes
the combined work GPL-2.0 — incompatible with the project's permissive
distribution model. There is no configuration in which this is a
trade-off rather than a prohibition.

A host that wants HEVC software encoding may still register `x265` as an
out-of-tree plug-in (the remote-desktop plan's X-3 records this). The
project does not ship it, and does not take on its licence.

### D — Hardware-only

Rejected. Hardware-first is the *default* (OD-2 / RD-1), not the only
path. A hardware-only engine cannot run on a machine with no hardware
encoder: a CI runner, a VM, an old laptop, a headless server. That means
the software path cannot be exercised in CI at all, which makes the
zero-copy claim, the round-trip proof and the remote-desktop bring-up
loop all unprovable on those machines. It also makes the video plan's
O-6 (hardware coverage is not on every CI leg) a permanent hole rather
than a temporary one.

Hardware-first with a *real* software fallback is strictly better: the
hardware path is what production uses, and the software path is what
proves the pipeline.

### E — A Rust cross-platform capture library (`xcap` / `scap`)

Rejected, and recorded here because it is the closest thing to a
cross-platform capture answer that exists. Both are Rust, and NimRTC is
C++20; introducing a Rust toolchain into a C++ engine is a larger
architectural commitment than the problem warrants. `xcap`'s Wayland path
is documented as "not fully supported", so it would not close the hardest
cell anyway. See the remote-desktop plan's X-4b.

## References

- `docs/plan/remote-desktop-plan.md` — the source plan: §0 (RD-1 … RD-10,
  the "RD-2 + RD-10 in detail" section with the selection table), Track B
  (B3, B4, B11), Track K (K1, K8, K8b), §B.7 (the verification matrix),
  §B.10 (Q-8, the cross-platform rule).
- `docs/plan/video-pipeline-v2-plan.md` — §B.7.1 (OD-1, the decision this
  ADR amends), Track X (O-3), Track G (G3 / ADR-020, the codec plug-in
  architecture and the `IVideoCodec` deprecation path).
- `docs/adr/ADR-015-api-stability-policy.md` — the stability policy; this
  ADR adds no public symbol, so it makes no API change.
- `docs/adr/ADR-025-sdp-rtp-not-pluggable.md` — the engine reaches codecs
  through the `plugins::I*` seams; a vendored codec is a plug-in, not an
  engine dependency.
- `src/third_party/vendor.json`, `tools/check_vendor.py`, `NOTICE` — the
  pinning, the platform matrix and the attribution that Decision 4 and
  Decision 6 require.
