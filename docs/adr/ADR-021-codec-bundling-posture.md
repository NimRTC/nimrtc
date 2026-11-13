# ADR-021: Codec bundling posture — permissive vendoring, direct vendor-SDK hardware

| | |
|---|---|
| Number | 021 |
| Status | **Accepted** |
| Date | 2026-11-13 |
| Phase | M1 (remote-desktop plan, Track B) |
| Bound to | v1.3.0 |
| Amends | the video plan's OD-1 ("no bundled software video encoder / decoder") |
| Related | `docs/plan/remote-desktop-plan.md` §0, `src/third_party/`, `tools/check_vendor.py`, `AGENTS.md` L5 |

## Context

The video plan's **OD-1** says the project bundles **no** software video
encoder or decoder: the hardware backends talk to the vendor SDKs
directly, and the software path was a stub. That was the right call for a
project whose whole premise was "hardware-first, no codec weight".

Remote desktop breaks it. A screen stream has two properties that a
camera stream does not:

1. **The picture is text.** A software H.264 encode of a 1080p desktop
   at 30 fps costs roughly 1.5–2.5 CPU-seconds per wall-clock second on
   a mid-range laptop core — the remote-desktop plan's §B.6 CPU budget
   does not survive it. Only the *hardware* path fits the budget, and a
   machine without a hardware encoder (a VM, an old laptop, a CI runner)
   needs *some* software encoder or it cannot participate at all.
2. **The picture is flat.** Screen content compresses far better under
   AV1 with intra-block-copy than under H.264, and the encoders that
   expose the screen tools (`svt-av1`) are software.

Meanwhile **FFmpeg** — the obvious answer, since it wraps every codec
including the hardware ones, cross-platform — is LGPL-2.1. The project
has deliberately not signed up for LGPL's distribution obligations, and
the binary weight is unacceptable for an embedded-oriented engine.

The precise scope of the licence blocker is worth stating, because it is
narrower than "FFmpeg is LGPL": FFmpeg's own lists do **not** treat the
hardware encoders (`nvenc`, `qsv`, `vaapi`, `videotoolbox`,
`ffnvcodec`) as GPL or nonfree. An LGPL build can carry every hardware
encoder. The blocker is the **software** codec set (`libx264` is GPL)
plus the binary weight.

But bundling FFmpeg would still lose to a direct implementation, on
non-licence grounds:

- NimRTC already implements the hardware backends directly against the
  vendor SDKs. FFmpeg would replace working zero-copy paths with an
  `AVFrame` / `AVBufferRef` model that does **not** interoperate with
  `video_frame::GpuBuffer` — the D3D11 → CUDA route has no FFmpeg
  `hwmap` equivalent and would need a full CPU round-trip, which is
  exactly what **B10** forbids.
- The project already vendors mbedTLS, libsrtp, libopus, libjuice and
  nlohmann_json. Adding FFmpeg's ~40 transitive libraries changes the
  dependency, licence and NOTICE surface by an order of magnitude.

So the question this ADR answers is **not** "FFmpeg or not FFmpeg". It
is: *given that we do not bundle FFmpeg, which codecs do we bundle
directly, under what licence rules, and with what platform-coverage
obligation?*

## Decision

### 1. Two filters, both mandatory

A codec may be vendored only if it passes **both**:

| Filter | Rule | Source |
|---|---|---|
| **RD-2 — Licence** | Permissive (BSD / BSD-3-Clause / Apache-2.0 / MIT) **and** a distributable patent grant. Copyleft (GPL / LGPL) is out, with no exceptions. | remote-desktop plan RD-2 |
| **RD-10 — Platform** | One library covering Windows + macOS + Linux beats three platform implementations. A platform-specific library must justify itself in its own item's acceptance criterion. | remote-desktop plan RD-10 |

The **patent** half of RD-2 is not decorative. `dav1d` and `svt-av1`
are BSD-licensed *and* carry the AOM Patent License 1.0; `openh264`
carries the AVC patent portfolio. Those grants are distributable, so the
licence filter passes — but the grant **must be reproduced in the
distribution materials** (`NOTICE`), or the licence filter has not
actually been met.

### 2. The selection table

| Library | Does what | Licence | Platforms | Verdict |
|---|---|---|---|---|
| `libyuv` | YUV/RGB scale + convert + rotate | BSD-3-Clause | Win / macOS / Linux / Android / iOS | **Vendor — default** (Track C). One library covers every platform we ship. |
| `dav1d` | AV1 decode | BSD-2-Clause + AOM Patent License 1.0 | Win / macOS / Linux | **Vendor — default** (Track B). |
| `openh264` (source-built) | H.264 encode | BSD-2-Clause + AVC patent portfolio | Win / macOS / Linux | **Vendor — default** (Track B). Source-build avoids Cisco's *prebuilt binary* conditions; the patent attribution duty remains. |
| `libvpx` | VP8 / VP9 encode | BSD-3-Clause | Win / macOS / Linux | **Optional vendor** behind `NIMRTC_VENDORED_LIBVPX` (Track B). |
| `svt-av1` | AV1 encode + **screen-content tools** | BSD-3-Clause Clear + AOM Patent License 1.0 | Win (MSVC + NASM) / macOS / Linux | **Optional vendor** behind `NIMRTC_VENDORED_SVT_AV1` (Track B). Heavy build, opt-in. Its screen mode (`--scm`, intra block copy) is exactly the feature remote control needs. |
| `libsamplerate` | Sample-rate conversion | BSD-2-Clause | Win / macOS / Linux | **Vendor — default** (Track F, F5). Chosen over SpeexDSP (BSD-3 but last release 2022-06, unmaintained). |
| `portaudio` | Cross-platform audio I/O | MIT | Win (WASAPI / MME / DSound / ASIO) / macOS (Core Audio) / Linux (ALSA / JACK / PulseAudio) | **Vendor — default** (Track F, F1/F2). PortAudio's own SRC is naive, so NimRTC's resampling goes through libsamplerate (F5), not PortAudio. |
| `x265` | HEVC encode | GPL-2.0 | all | **Excluded** — copyleft (RD-2). |
| `libaom` | AV1 encode | BSD-2-Clause | all | **Excluded for now** — build cost and encode latency; `svt-av1` covers the same need with screen tools. |
| FFmpeg / GStreamer | everything | LGPL-2.1 | all | **Not vendored** — LGPL distribution obligation + binary weight, and the `AVBufferRef` model does not interoperate with `GpuBuffer`. |

### 3. Hardware codecs: direct vendor-SDK, never via FFmpeg

There is **no** portable abstraction for hardware video encode — every
hardware encoder is vendor SDK territory (NVENC / NVIDIA, AMF / AMD,
QSV / Intel, VA-API / Linux, VideoToolbox / Apple, MediaCodec /
Android). RD-10 therefore does not apply to the hardware row, and that
exception is written down rather than left implicit.

The hardware backends stay in `cmake/NimRTHwPlugins.cmake`, behind their
existing `NIMRTC_PLUGINS_<NAME>` options, and keep talking to the vendor
SDKs directly. **The portable part is the selection policy**
(`HwVideoBackendRegistry`), not the encoders. A host may still register
an FFmpeg- or GStreamer-backed codec / capture plug-in through
`plugins::IVideoCodecFactory` and `IVideoCaptureFactory` — the seams
exist for exactly that — but NimRTC does not ship one.

### 4. `NIMRTC_VENDORED_*` option policy

Every vendored dependency gets a `NIMRTC_VENDORED_<NAME>` CMake option:

| Option | Default | Rationale |
|---|---|---|
| `NIMRTC_VENDORED_LIBYUV` | ON | Small, builds everywhere we ship, and the scaling hot path needs it. |
| `NIMRTC_VENDORED_DAV1D` | ON | The only practical cross-platform AV1 decoder. |
| `NIMRTC_VENDORED_OPENH264` | ON | The only practical cross-platform H.264 encoder under a permissive licence. |
| `NIMRTC_VENDORED_PORTAUDIO` | ON | Cross-platform audio I/O for the three platforms. |
| `NIMRTC_VENDORED_LIBSAMPLERATE` | ON | The maintained resampler. |
| `NIMRTC_VENDORED_LIBVPX` | **OFF** | Only needed for Chrome-compatible VP9; heavy build. |
| `NIMRTC_VENDORED_SVT_AV1` | **OFF** | Heavy build (NASM), opt-in. Its screen mode is the reason it is in the table at all. |

**Default ON ⇒ must build on every CI leg the project claims.** A
dependency that does not build everywhere is default OFF and its
`NOTICE` entry states the platform matrix. No exception is granted by
default — an exception requires an ADR amendment.

### 5. Vendoring mechanics

Every vendored dependency is a **pinned submodule** under
`src/third_party/`, recorded in `src/third_party/vendor.json`, and
enforced by `tools/check_vendor.py` in CI. `NIMRTC_VENDORED` stays ON
(**AGENTS.md L5**). **No system-installed codec is ever linked** — the
`find_package` path for a codec is not used.

The release bundle's licence manifest lists every vendored codec and
reproduces every patent grant.

## Consequences

### Positive

- The OD-1 amendment is explicit and bounded: software codecs are in,
  but only under a permissive licence with a distributable patent grant,
  and only one library per cell.
- FFmpeg's exclusion now has a written reason that survives the obvious
  "but FFmpeg has everything" objection, and the reason is *not* "LGPL
  forbids the hardware codecs" (it does not).
- The hardware path is untouched by the licence policy: it was already
  direct-SDK, and it stays that way, so B10's zero-copy claim is not
  traded away for convenience.
- A distributor can strip the optional heavy codecs at configure time
  without patching the tree.

### Negative / risks

- **Patent duties travel with the distribution.** `dav1d` / `svt-av1`
  (AOM) and `openh264` (AVC) each require the grant to be reproduced.
  A distributor who ships a binary without the `NOTICE` file is in
  breach. This is a real obligation, not a formality.
- **`openh264` must be source-built** to avoid Cisco's prebuilt-binary
  conditions (the "OpenH264 Video Codec provided by Cisco Systems, Inc."
  end-user download + display). That is a build-time cost we accept.
- **Vendoring six libraries is a maintenance surface.** Each needs a
  submodule pin, a `vendor.json` entry, a `NOTICE` entry, a licence scan
  in CI, and a security-watch entry. That is the price of not bundling
  FFmpeg, and it is smaller than FFmpeg's.
- **`svt-av1` builds slowly** (NASM + C++ on every platform), which is
  why it is default OFF.
- **Libvpx / SVT-AV1 being OFF by default means a default build cannot
  offer software VP9 or screen-mode AV1.** Acceptable: the hardware
  backends cover the mainstream path, and the plan's M4 milestone is
  where the optional codecs land.

### Neutral

- The `HwVideoBackendRegistry` selection policy is portable and stays
  portable — the RD-10 exception applies to the *encoders*, not to the
  *policy*.
- `src/plugins/` stays interface-only: the FFmpeg/GStreamer escape
  hatch is a *host* registering a plug-in, never a vendored
  implementation.

## Verification

| Check | Where | Required |
|---|---|---|
| Every vendored dependency is a pinned submodule under `src/third_party/` | `tools/check_vendor.py` | pass |
| Every vendored dependency's licence is in the allowlist and its patent grant is reproduced in `NOTICE` | CI licence scan | pass |
| `cmake -LA` lists every `NIMRTC_VENDORED_<NAME>` option | CI | pass |
| A build with each optional option OFF still configures and links | CI matrix | pass |
| No system-installed codec is linked | `ldd` / `dumpbin` inspection of the built binaries | zero codec libs outside `src/third_party/` |
| The hardware backends still talk to the vendor SDKs directly | code review of `src/modules/nvcodec/`, `src/modules/hw_video_backend/` | pass |

## Out of scope

- The hardware backends' internal implementations (B5 / B5a / B6).
- The optional software AV1 / VP9 encoders themselves (**B11**).
- Audio I/O and resampling in detail (Track F, F1/F2/F5).
- Any change to `AGENTS.md` L5 or to the `src/core` / `src/plugins`
  layering rules.

## References

- `docs/plan/remote-desktop-plan.md` §0 (the RD-2 / RD-10 filters and the
  selection table), §B.6 (the CPU budget that forces the software path
  to exist at all).
- `AGENTS.md` — L5 (vendored-only), the "禁区" list.
- `src/third_party/vendor.json`, `tools/check_vendor.py`, `NOTICE`.
- `cmake/NimRTHwPlugins.cmake` — the `NIMRTC_PLUGINS_*` hardware options.
- `docs/adr/ADR-022-screen-content-as-content-type.md` — the companion
  ADR on *why* a screen is not a camera.
