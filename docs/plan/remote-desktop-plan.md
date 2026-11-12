# Remote Desktop on NimRTC — Development Plan

> **This document is the single plan for turning NimRTC into the media engine
> of a production remote-desktop product.** It covers the device layer
> (screen / camera capture, audio devices, rendering), the codec layer
> (hardware-first encoding, screen-content coding), the adaptive path, the
> control plane, multi-stream, and the signalling / session story.
>
> Scope: **work-item inventory** (Part A — every item that has to be done,
> numbered so nothing is lost) plus **milestone plan** (Part B — M1..M4, the
> four milestones that carry the product from "pipes work" to "shippable").
> Part A is deliberately exhaustive; Part B says in which release each item
> ships and what the milestone's *exit criterion* is.

| | |
|---|---|
| Plan opened | 2026-11-11 |
| Decisions closed | 2026-11-11 — the ten owner decisions (RD-1 .. RD-10) are recorded in **§0** and are binding for every work item below. RD-10 (cross-platform open source preferred) is the second selection filter, applied alongside RD-2 (licence). |
| Baseline | `main` @ post-v1.1 consolidation, plus the in-flight v1.2.0 work (`docs/plan/video-pipeline-v2-plan.md`, `docs/plan/audio-pipeline-dev-plan.md`) |
| Scope class | 设备层 + 编解码层 + 控制面 — new public plugin seams, new modules, new vendored codecs; **no transport rewrite** |
| First release | **v1.3.0** — M1 lands together with the video plan's v1.3.0 (hardware-first + capture seam) |
| Last release in this plan | **v1.6.0** (M4) |
| Owner | BDFL |
| Related | `docs/zh/architecture.md` §6, ADR-001 (plugin system), ADR-010 (profile JSON), ADR-015 (API stability), ADR-017 (cross-end timeline), ADR-018 (bandwidth signal), ADR-020 (codec plug-in architecture — **amended by ADR-021**), `docs/plan/video-pipeline-v2-plan.md`, `docs/plan/audio-pipeline-dev-plan.md`, `docs/plan/transport-strategies.md`, `docs/hw_plugin_seam.md`, `docs/plugin_author_guide.md` |

---

## 0. Decisions this plan is built on

These ten answers came from the design discussion (2026-11-11) and are
binding for every work item below. They are written here rather than
scattered through the tracks so that a reader who disagrees with one of them
knows exactly which items change.

| # | Decision | Consequence for the work items |
|---|---|---|
| **RD-1** | **Hardware-first encoding is the default, not an option.** The engine selects a hardware encoder whenever one exists and supports the requested configuration; software is the *fallback*, not the default. A configuration override (`codec_backend: auto \| hardware \| software`) forces a path, and the selected backend is reported in telemetry. | Track B — reuses the existing `plugins::HwVideoBackendRegistry` priority mechanism; adds HEVC / AV1 backends, adds the `codec_backend` override, adds capability queries so an unsupported configuration is rejected **up front** instead of at the first frame. This **extends** video-plan OD-2; it does not replace it. |
| **RD-2** | **Permissive-licensed open-source codecs may be vendored.** The video plan's OD-1 ("no bundled software video encoder/decoder") is **amended** (ADR-021): NimRTC may vendor codecs whose licence is permissive *and* whose patent grant is distributable (BSD / BSD-3-Clause / Apache-2.0 + AOM Patent License 1.0). Copyleft (GPL / LGPL) codecs stay out of the tree. | Track B — `src/third_party/` gains `libyuv`, `dav1d`, `openh264` (source-built), optionally `libvpx` and `svt-av1` behind `NIMRTC_VENDORED_*` options. Each is pinned in `src/third_party/vendor.json` and enforced by `tools/check_vendor.py`. |
| **RD-3** | **Screen content is a first-class content type, not "a webcam pointed at a monitor".** The engine must know the difference: it enables screen-content coding (intra block copy / palette / text mode), region-of-interest, dirty-rectangle incremental update, and cursor compositing. | Track A (dirty-rect + cursor), Track B (screen-content codec tuning + `content-hint` negotiation), Track C (ROI). A screen capture is *not* modelled as "a camera with a huge resolution". |
| **RD-4** | **Zero-copy is the default path; CPU staging is the fallback.** A hardware capture that yields a GPU surface must reach a hardware encoder without a CPU round-trip. The `video_frame::GpuBuffer` / `GpuHandle` / `GpuBackend` surface that already exists in the tree is the currency. | Track A + Track B — the capture plugins produce a `GpuBuffer`; the encoder plugins consume one; the CPU path exists for machines without a compatible HW pair and is *reported* as such. |
| **RD-5** | **The control plane gets its own congestion-control domain.** Key/mouse events must not sit in the same queue as a 15 Mbps video stream. The target design is a *separate transport stack* (the existing `raw-udp-arq` control stack, with its own ICE selected pair) — **not** the media connection's DataChannel. The interim path (G1) rides the DTLS record layer, which is the media connection, and is therefore explicitly a *stepping stone*, not the end state. | Track G — reuses `transport-strategies.md` and the existing `TransportSession.media_stack` / `control_stack` split rather than inventing a new transport. The control path has its own budget, so a video-heavy media path cannot starve it. |
| **RD-6** | **The engine stays a single-threaded orchestrator; device plugins own their own capture threads.** A capture plugin may own a thread that produces frames into a bounded queue; the *encoder* is still driven only from the engine's tick thread, so the codec's non-thread-safety contract is preserved. The engine today creates no `std::thread` of its own, and this plan does not change that — nor does it need to, because `docs/zh/architecture.md` §D4 already fixes the model as an **injectable Executor** with "network and media on separate threads by default, single-threaded event-driven supported". A capture plug-in owning its own thread is a *plug-in* concern, not an engine one. | Track A — the `IVideoCapture` seam is asynchronous with a bounded queue and an explicit drop policy. The engine gains an internal video pump that pops and encodes on the tick thread. No new thread is created under `src/engine/`, and the Executor seam stays the single place where thread policy lives. |
| **RD-7** | **The engine ships a signalling *seam*, not a signalling server.** Device registration, pairing, room management and TURN credential issuance are product concerns; NimRTC exposes the SDP/candidate exchange surface it already has (`create_offer` / `process_remote_sdp` / `add_turn_server` / candidate forwarding) and the reference implementation lives in `examples/`, not in `src/`. | Track I — the example carries a minimal WebSocket signalling service and the pairing flow; `docs/guides/remote-desktop.md` documents how to swap it. |
| **RD-8** | **Performance budgets are acceptance criteria, not aspirations.** Every capture / convert / scale / encode / packetize / render step gets a per-frame CPU-time budget at 1080p30, and the pipeline test measures it. A milestone that hits its feature list but blows its budget does not exit. | §B.6 — the budget table is the contract; Track J carries the benchmark harness. |
| **RD-9** | **One ladder, one timeline, one log surface.** The remote-desktop work reuses the video plan's `video_bitrate_control` ladder, the audio plan's playout / 3A model, the ADR-017 timeline and the single `core::log::Logger`. It does not grow a second rate controller, a second skew model, or a second logger. | Every track — the "reuse, do not re-invent" rule is a per-item acceptance criterion, not a general principle. |
| **RD-10** | **Cross-platform open source is preferred, and platform-specific code is the exception that must be justified.** Whenever a cross-platform open-source library can cover a platform × codec × capability cell, it is used **in preference to** writing platform code — even when the platform code would be marginally faster. "Prefer the open-source library" is a *default*, and a platform-specific implementation must justify itself in the item's acceptance criterion. The licence filter is RD-2; the platform filter is this decision. | Track B (codecs), Track C (scale / colour), Track F (audio I/O + resampling), Track A (capture, partially). Each such item names the cross-platform library it vendors; the per-platform backends in `cmake/NimRTHwPlugins.cmake` (NVENC / AMF / QSV / DXVA / VA-API) remain, because **hardware codecs have no cross-platform abstraction** — that is the documented limit of this rule. |

### RD-2 + RD-10 in detail — the open-source selection table

OD-1 exists because x265 is GPL-2.0 and incompatible with NimRTC's
Apache-2.0 distribution model. That reasoning is correct and is **not**
being reversed. RD-2 narrows OD-1 from "no bundled codec" to "no bundled
copyleft codec", because a remote-desktop product that cannot ship an
encoder is not a product. RD-10 adds the second filter: **prefer the
cross-platform library over platform code.**

#### The two filters

Every candidate must clear **both**:

| Filter | Rule | Source |
|---|---|---|
| **Licence** | Permissive (BSD / Apache-2.0 / MIT) **and** a distributable patent grant. Copyleft (GPL / LGPL) is out. | RD-2 |
| **Platform** | One library covering Windows + macOS + Linux beats three platform implementations. A platform implementation must justify itself in its item's acceptance criterion. | RD-10 |

#### The selection table

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

#### What RD-10 could *not* cover, and why

RD-10 is a default, not an absolute. Two cells have **no** cross-platform
open-source answer, and the plan says so rather than pretending otherwise:

| Cell | Why there is no cross-platform library | What the plan does |
|---|---|---|
| **Hardware video encode/decode** | Every hardware encoder is vendor SDK territory: NVENC (NVIDIA), AMF (AMD), QSV (Intel), VA-API (VA-API is Linux-only), VideoToolbox (Apple), MediaCodec (Android). There is no portable abstraction *and* no portable licence. | Keep the per-platform backends in `cmake/NimRTHwPlugins.cmake` — they are already there and already the tree's established pattern. The cross-platform part is the **selection policy** (`HwVideoBackendRegistry`), which is portable. See the inventory below for exactly which backends exist today. |
| **Screen / display capture** | The closest cross-project options are `xcap` (Rust, Apache-2.0, Win/macOS/Linux, **Wayland capture marked incomplete**) and `scap` (Rust, MIT, Win/macOS/Linux via WGC / ScreenCaptureKit / PipeWire). Both are **Rust**, and NimRTC is C++20 — introducing a Rust dependency to a C++ engine for a partial feature (xcap's Wayland path is "⛔ feature available, but not fully supported") is not a trade the project should make. GStreamer could cover it but is LGPL (excluded). | Write the three platform backends (A3/A4/A5). The **shared** layer — queue, format negotiation, dirty-rect, cursor compositing, the `IVideoCapture` seam — is cross-platform and is where the effort goes; only the leaf API call differs per platform. |

#### The hardware-codec inventory — what actually exists today

This is the precise state of the tree, verified against the source (not the
README, which the project corrected once already — see `CHANGELOG.md` §
"The README no longer overstates the hardware codec support"). It matters
because B5 / B6 are **not** new capability: the backend table, the priority
mechanism, the `available()` probe contract and the CMake
`NIMRTC_PLUGINS_*` pattern all exist and are proven.

| Backend | File | Size | State | Encode | Decode |
|---|---|---|---|---|---|
| **NVENC** (NVIDIA) | `src/modules/nvcodec/src/nvenc_encoder.cpp` | 47.3 KB | ✅ real — H.264 / H.265 / AV1 | ✅ | — |
| **VA-API** (Linux Intel/AMD) | `src/modules/h264/src/vaapi_encoder.cpp` | 46.5 KB | ✅ real | ✅ | ✅ |
| **AMF** (AMD) | `src/modules/h264/src/amf_encoder.cpp` | 26.1 KB | ✅ real | ✅ | ✅ |
| **QSV** (Intel) | `src/modules/h264/src/qsv_encoder.cpp` | 24.6 KB | ✅ real | ✅ | — |
| **DXVA / Media Foundation** (Windows) | `src/modules/h264/src/dxva_decoder.cpp` | 26.3 KB | ✅ real | — | ✅ |
| **NVDEC** (NVIDIA) | `src/modules/nvcodec/src/nvdec_decoder.cpp` | 38.2 KB | ✅ **real** (was an empty seam) — CUVID parser + decoder, NV12 output, runtime-loaded driver | — | ✅ |
| **H.265 module** | `src/modules/h265/` | 4 files | ✅ partial — real NVENC/NVDEC HEVC backends wired to `src/modules/nvcodec/`; the in-module software decoder is a **stub** | ✅ (NVIDIA) | ✅ (NVIDIA) |
| **AV1 module** | `src/modules/av1/` | 4 files | ✅ partial — same shape as H.265: real NVENC/NVDEC AV1 backends, stub software decoder | ✅ (NVIDIA) | ✅ (NVIDIA) |
| **VideoToolbox** (macOS) | — | — | ⚠️ **empty seam** — `src/modules/h264/src/hw_backends.cpp:230` reads "NOT IMPLEMENTED — create() returns nullptr" | ❌ | ❌ |
| **MediaCodec** (Android) | — | — | ⚠️ **empty seam** — same shape (`hw_backends.cpp:289`) | ❌ | ❌ |
| **OpenH264** (software) | `src/modules/h264/src/openh264_encoder.cpp` | 4.1 KB | ⚠️ **stub** — delegates to the synthetic `CodecPluginAdapter` | ❌ | ❌ |

Three properties of this inventory are load-bearing for the plan:

1. **The send-side is the strong side, and that is the side remote
   desktop needs.** Encoding latency dominates the feel of a remote
   session, and the four H.264 *encoders* cover Windows and Linux across
   every mainstream vendor (NVIDIA, AMD, Intel) plus VA-API.
2. **The empty seams are honest, not broken.** The seam id stays stable
   for API compatibility, but `available()` returns `false` and `create()`
   returns `nullptr`, so `HwVideoBackendRegistry` can never hand a caller
   a backend that cannot produce a codec. B2's `available()` contract is
   therefore **already the right shape** — the work is filling in the
   entries, not inventing a mechanism.
3. **macOS has no hardware codec path at all.** This is the sharpest gap
   for a cross-platform product: a Mac sender falls back to software
   encoding, which at 1080p30 does not fit the §B.6 CPU budget. B5's
   VideoToolbox HEVC entry and a VideoToolbox H.264 entry are therefore
   not "nice to have" — they are the price of macOS support.

This is the honest shape of the rule: **cross-platform for the shared
mechanism, platform-specific only for the leaf OS call.** A platform
backend that is more than a leaf call must justify itself in its item's
acceptance criterion.

ADR-021 records this table, the amendment to OD-1, and RD-10. The video
plan's O-3 entry is updated to point at ADR-021 instead of saying
"closed by OD-1".

### Constraint compliance (per AGENTS.md)

| Constraint | How this plan respects it |
|---|---|
| **L1** `src/core/` is the only INTERFACE library, header-only | No new core types. Value structs that both the plugins layer and the modules layer need (e.g. `VideoEncoderParams`) live in `src/plugins/` and are re-exported; `src/core/` only gains registry slots (a typed `register_<category>` / `get_<category>` pair, same shape as the existing ones). |
| **L2** `src/plugins/` holds interfaces and factory interfaces only | Every new seam (`IVideoCapture`, `IAudioSource`, `IAudioSink`) is declared there with **no implementation**. The one documented exception pattern (`hw_video_backend.cpp`) is not extended. |
| **L3** each module self-contained under `src/modules/<name>/` with its own CMakeLists + include/ + src/ + tests/ | All new modules follow the existing layout: `video_capture`, `audio_device`, `rd_input`, `video_bitrate_control` (from the video plan), `rtx` (from the video plan), `h265` / `av1` (from the video plan). |
| **L4** `src/engine/` orchestrates only, never includes concrete module headers | The engine reaches every new component through `core::PluginRegistry` + the PAL resolver (`pal::resolve_*`), the same path as `bwe` / `audio3a` / `video_source` today. The engine's public header keeps including **no** concrete module header. |
| **L5** `src/third_party/` is the only vendor location; `NIMRTC_VENDORED` stays ON | Every new dependency is a pinned submodule under `src/third_party/`, recorded in `src/third_party/vendor.json`, enforced by `tools/check_vendor.py`, with a `NIMRTC_VENDORED_<NAME>` option (default ON for `libyuv` / `dav1d` / `openh264` / `portaudio` / `libsamplerate`, default OFF for the optional heavy ones `libvpx` / `svt-av1`). Each one's platform matrix is recorded and CI-verified (K8b). No system-installed codec is ever linked. |
| **L6** no global mutable state outside `nimrtc::core::` | Every new component is a per-session instance owned by the engine. The `HwVideoBackendRegistry` singleton stays where it is (`plugins::`, mutex-guarded, registration-only) — it is the *existing* documented seam, not a new global. |
| **L7** protected domains (`policy/`, `.zelto/`, `AGENTS.md`) are not overridable | No work item touches those paths. |

---

# Part A — Complete work-item inventory

Every item has an ID, a landing site and an acceptance criterion. Part B maps
the IDs onto the four milestones. **An item is not done until its acceptance
criterion passes on the CI legs that apply to it** (Windows MSVC, Linux
x86_64, Linux aarch64, macOS arm64 — aarch64 per `docs/zh/architecture.md`
§13.1 "compiles ≠ interoperates"; hardware paths per the dedicated hardware
leg, RD-8).

Legend for landing sites: `P` = `src/plugins/`, `M` = `src/modules/<name>/`,
`E` = `src/engine/`, `T` = `tests/`, `X` = `tools/` + `examples/`,
`D` = `docs/`, `V` = `src/third_party/`.

---

## Track A — Device layer: capture

> **Why first:** nothing in the video path can show a real picture until
> something real is captured. The existing `IVideoSource` ships exactly one
> implementation — an in-memory **test pattern** generator
> (`src/modules/video_source/src/video_source_plugin.cpp:254`), and
> `IVideoSink` ships two sinks that **never present a picture**: the headless
> one says so outright ("Headless sink (drops pixels; counts frames)",
> `src/modules/video_sink/src/video_sink.cpp:156`) and the "pinned" one only
> validates `capture_ts` ordering (`video_sink.cpp:79-90`). There is no
> capture device and no display.

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **A1** | **Capture seam `IVideoCapture`** (`src/plugins/include/nimrtc/plugins/video_capture.hpp`): device enumeration, format negotiation, region selection, start/stop, asynchronous frame delivery through a bounded queue with an explicit drop policy, and a capability query (max resolution / fps / zero-copy support / dirty-rect support / cursor compositing support). This is the video plan's C1 seam, landed with the remote-desktop fields it needs. **Status: landed** (`src/plugins/include/nimrtc/plugins/video_capture.hpp`, plus the registry's own `kVideoCapture` category in `src/core/include/nimrtc/core/registry.hpp`); the seam carries no implementation code, so `src/plugins/` stays interface-only. | `P` | A third-party capture plug-in registers and is driven by the engine without touching `src/engine/` or `src/modules/`; `src/plugins/` contains no implementation code. |
| **A2** | **Capture module** `src/modules/video_capture/` — the shared implementation surface: device enumeration abstraction, format negotiation, the bounded queue, the frame-pool integration, and the shared "capture → `GpuBuffer` / CPU `VideoSourceFrame`" adapter. Platform code lives in `src/modules/video_capture/src/platform/`. **Status: landed** for the portable half — the bounded queue (`capture_queue.hpp`), the synthetic device (`synthetic_capture.hpp`) and the shared capture → frame-stream adapter (`capture_source_adapter.hpp`); the `GpuBuffer` half and the frame-pool integration land with the platform backends in M1-c, which are the only things that own a GPU surface. | `M` | Module unit tests with a synthetic capture device: queue overflow drops the **oldest** frame and increments a counter; the drop policy is configurable. |
| **A2b** | **Cross-platform capture layer first** (RD-10) — before any platform backend is written, establish what the *portable* part of capture actually is: the target enumeration model, the format-negotiation state machine, the dirty-rect model, the cursor-compositing contract, and the queue. Only the leaf OS call differs per platform. This item exists so that the three platform backends in A3/A4/A5 are thin adapters over one mechanism rather than three parallel implementations. **Status: the portable layer is landed** — the enumeration model, the negotiation contract, the dirty-rect model and the queue are in `src/modules/video_capture/`, and the **synthetic device** exercises all of them on every CI leg; the cursor-compositing contract is declared in the seam (`CaptureCapabilities::cursor_composited`) but its implementation is A6. | `M` | The portable layer is exercised by a **synthetic capture device** on all CI legs; a platform backend is a < 200-line adapter over it. A test asserts that the same synthetic device drives the pipeline identically on every leg. |
| **A3** | **Windows capture** — DXGI Desktop Duplication (display, change-driven, `GetFrameDirtyRects` / `GetFrameMoveRects`, `B8G8R8A8_UNORM` surface, no cursor) **and** Windows Graphics Capture (window / display, cursor composited by the OS, requires user consent). Both produce a `GpuBuffer` with `GpuBackend::kWindowsD3D11Texture`. Selection: WGC for windows and for cursor fidelity, DXGI for displays when the caller does not need the cursor. **Status: the DXGI half is landed** (`src/modules/video_capture/src/platform/dxgi_desktop_dup.cpp`, `include/nimrtc/video_capture/dxgi_desktop_dup.hpp`): the device enumerates the outputs, opens an `IDXGIOutputDuplication`, runs a **capture thread** that blocks in `AcquireNextFrame` and feeds the shared bounded queue, reads the frame back through a staging texture (BGRA), and reports DXGI's own dirty / move rectangles clipped against the selected region. It registers under the id `"dxgi_desktop"`. **Still open**: the WGC half (windows, cursor composited by the OS) and the zero-copy `GpuBuffer` hand-off, which is B10's job. | `M` | A test on a Windows host captures a known window and asserts the frame is non-black and that the frame's `GpuHandle` carries a D3D11 texture; the dirty-rect metadata is non-empty for a window that changed. |
| **A4** | **macOS capture** — ScreenCaptureKit (display / window, BGRA, cursor, system audio as a bonus). Produces `GpuBuffer` with `GpuBackend::kAppleCFPixelBuffer`. Screen-recording permission is requested; denial is a clean error, not a crash. | `M` | macOS leg: capture produces non-black frames; a permission denial is reported through the engine error callback with a distinct code. |
| **A5** | **Linux capture** — XDG Desktop Portal + PipeWire (Wayland) and X11 (`XGetImage`) fallback. Wayland's security model means the portal owns source selection: the seam exposes "the platform asks the user" rather than "the engine enumerates monitors". | `M` | Linux leg: the portal path negotiates a format and delivers frames; the X11 path delivers frames; the module documents that global source enumeration is a portal limitation, not an engine bug. |
| **A6** | **Cursor compositing** — a pluggable cursor layer that composites the pointer into the captured frame when the platform does not (DXGI does not). Sources: Windows `GetCursorInfo` + `DrawIconEx`, X11 `XFixesGetCursorImage`, macOS (handled by ScreenCaptureKit). A `PinnedSink`-style "cursor is always drawn at the last position" fallback exists when the platform cursor is unavailable. **Status: the pluggable layer is landed** (`src/modules/video_capture/include/nimrtc/video_capture/cursor_compositor.hpp`): `ICursorSource` reports *where* the pointer is, `ICursorCompositor` decides *whether* to draw it, and `AlphaCursorCompositor` does the integer fixed-point alpha blend of a BGRA cursor bitmap over BGRA frame pixels, clipped and rebased onto the region origin — pixels outside the pointer's bounding box stay byte-exact. `PinnedCursorSource` is the "last known position" fallback. The adapter (`CaptureSourceAdapter::set_cursor`) applies the layer before the frame is published. **Still open**: the platform *sources* (`GetCursorInfo` on Windows, `XFixesGetCursorImage` on X11). | `M` | On a Windows host the composited frame differs from the raw frame in the cursor's bounding box and is identical elsewhere (pixel-exact comparison outside the box). |
| **A7** | **Dirty-rect / incremental metadata** — the capture seam reports, per frame, the changed region(s) and a "full frame" flag. Windows DXGI supplies them natively; macOS / Linux supply a conservative bounding box from a pixel-diff, or full-frame when unavailable. | `M` | A static desktop produces frames whose dirty rect is empty (DXGI) or a no-op rect (diff-based); the encoder is told to skip the unchanged region (Track B, B9). |
| **A8** | **Region / multi-monitor selection** — select one display, a window, or a sub-region; multi-monitor is either "one display per capture instance" (M4 multi-stream) or an explicit "virtual desktop" stitch (out of scope, see Track X). **Status: landed for the portable layer and for the DXGI backend** — the synthetic device honours the requested region exactly, and the DXGI backend crops the captured frame to the requested source rectangle (and rebases DXGI's dirty rectangles onto the region's origin) before the frame is queued. | `M` | A capture configured for a 800×600 region produces frames of exactly that size; the module test uses a synthetic device. |
| **A9** | **Camera capture** — the same seam, a different device class: Windows Media Foundation / D3D11 camera, macOS AVFoundation, Linux V4L2. Delivers to the same `GpuBuffer` / `VideoSourceFrame` contract so the encoder path is identical. | `M` | The camera path registers under its own plug-in id and drives the same encoder path; the camera's own `HwBackend` (`DirectShow` / `AvFoundation` / `V4l2` / `AndroidCamera2`) is reported by `hardware_backend()`. |
| **A10** | **Capture → engine pump** — the engine gains an internal pump (RD-6) that pops from the capture queue and drives the encoder **on the tick thread**, replacing the current "the host must pull frames and call `send_video()`" contract for the automatic path. `send_video()` stays as the explicit manual path. **Status: landed** (`src/engine/src/engine_video_capture.cpp`, wired into `NimRTCEngine::tick()` in `src/engine/src/engine_transport.cpp`). `EngineConfig::video_capture_name` names the capture plug-in, `EngineConfig::video_capture_config` carries the device request, and `EngineConfig::video_capture_auto_pump` (default `true`) selects the automatic path; with it off, the host keeps driving `send_video()` by hand. The pump pops at most one frame per tick (an empty queue is a no-op, never a stall) and runs *before* the scheduler drain, so the packets the frame produces leave in the same tick. | `E` | A capture plug-in + a real encoder produce an outbound RTP stream **without the host calling `send_video()`**; the codec is only ever touched from the tick thread (asserted by a test that records the thread id at `encode()` time). |
| **A11** | **Capture statistics** — frames captured / dropped (queue overflow) / zero-copy vs CPU-staged / device lost / format changes, surfaced through the engine stats snapshot. **Status: landed** (`Engine::Stats`: `video_capture_frames`, `video_capture_frames_pumped`, `video_capture_dropped`, `video_capture_device_errors`, `video_capture_restarts`, `video_capture_format_changes`, `video_capture_bytes_copied`). The counters are read against a *baseline* the pump remembers, not as a delta around the pop: the device drops frames on its **own** thread, so a delta taken around the pop would miss every drop that happened between two ticks. A device that reports an error is reopened once and the recovery is counted. | `E` + `M` | The stats snapshot reports the counters and they move during a live session. |

---

## Track B — Codec layer: hardware-first, screen content

> **Why:** the in-tree "H.264 codec" is a **fake** — `src/modules/h264/src/codec_plugin.cpp:109-113`
> fills the payload with a **Numerical Recipes LCG** pattern, and the matching
> decoder produces **no pixels at all** (`src/modules/h264/src/decoder.cpp:51`,
> "Stub produces no pixels — buffer remains empty"). The pipeline is provably
> wired; the *picture* is not real.
>
> **But the hardware layer is largely real** — see the §0 hardware-codec
> inventory for the exact per-backend state. Four H.264 *encoders* (NVENC,
> VA-API, AMF, QSV) and three *decoders* (DXVA, VA-API, NVDEC) are
> implemented against the vendor SDKs, ~154 KB of real code, registered in the
> `hw_backends.cpp` priority table. The NVIDIA encoder / decoder now lives in
> the shared, codec-generic `src/modules/nvcodec/` module, which serves H.264,
> H.265 and AV1 from one implementation. Two entries remain **empty seams**
> whose `available()` honestly returns `false` (VideoToolbox, MediaCodec),
> and the OpenH264 binding is a stub that delegates back to the fake encoder
> (`src/modules/h264/src/openh264_encoder.cpp:12-14` — "Real implementation
> would link against libopenh264 … we delegate to CodecPluginAdapter").
>
> So Track B's work is **not** "build hardware encoding" — it is: fill the
> empty seams (B5a — **NVDEC is done**), extend the existing table to
> HEVC / AV1 (B5, B6 — **done for NVIDIA**; the non-NVIDIA HEVC/AV1
> backends and the software decoders are still stubs), make the software path
> real (B3), and add the missing **round-trip proof** (B10a — **done**, see
> `src/modules/nvcodec/tests/test_nvcodec_roundtrip.cpp`) that turns the existing
> backend code into a *verified* claim.

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **B1** | **Codec split seam** — `IVideoEncoder` / `IVideoDecoder` split out of the combined `IVideoCodec`, with a **capability query** (supported profiles, resolutions, pixel formats, fps, max bitrate, zero-copy). `IVideoCodec` stays as a deprecated compatibility shim for one MINOR cycle (ADR-015 Decision 3). This is the video plan's C2 / C3. | `P` | Old code using `IVideoCodec` still compiles (deprecated marker present); the engine rejects an unsupported resolution at `open()` with a clear error instead of failing at the first frame. |
| **B2** | **Hardware-first selection, made real** — extend the existing `HwVideoBackendRegistry` priority mechanism: register a real backend for every platform × codec pair the project claims, each with an honest `available()` probe, a priority, and a `zero_copy_supported` flag. Add the `codec_backend` config (`auto` / `hardware` / `software`). | `P` + `E` + `M` | On a machine with no hardware encoder the pipeline still runs (software path) and *says so* in the stats; on a machine with one, the hardware path is selected and reported; an explicit `software` override really does use software. |
| **B3** | **Real H.264 software codec** — `openh264`, built from source under `src/third_party/openh264` (BSD-2-Clause, cross-platform Win/macOS/Linux), registered as the `software` H.264 encoder and decoder. | `V` + `M` | An H.264 frame encoded by the software path is decoded by the software path and the pixels match the source within the codec's tolerance; the round trip is asserted in `tests/` on all three platforms. |
| **B4** | **Real AV1 decode** — `dav1d` under `src/third_party/dav1d` (BSD-2-Clause + AOM patent licence, cross-platform Win/macOS/Linux), registered as the AV1 decoder. | `V` + `M` | An AV1 frame decoded through the pipeline yields the expected frame size and non-trivial pixel content on all three platforms. |
| **B5** | **HEVC hardware encode** — NVENC HEVC (Windows / Linux), VA-API HEVC (Linux), QSV HEVC (Windows / Linux), VideoToolbox HEVC (macOS), MediaCodec HEVC (Android). The `hw_backends.cpp` table gains the HEVC entries; `VideoCodecKind` gains `kH265`. **This extends the existing H.264 backend table, it does not create a new mechanism** — each new entry reuses the proven `available()` probe + priority + `zero_copy_supported` contract and the `NIMRTC_PLUGINS_<NAME>` CMake pattern. Note that VideoToolbox is an **empty seam** today (§0 inventory), so macOS HEVC needs the VideoToolbox backend itself (B5a) to exist first. **Status: NVENC HEVC is landed** (`src/modules/h265/src/hw_backends.cpp`, priority 450, probing `nvcodec::nvenc_codec_available(kH265)`); the VA-API / QSV / VideoToolbox / MediaCodec HEVC entries are not wired yet. | `M` | Each backend encodes one HEVC frame end-to-end on its platform; unsupported combinations are **explicitly reported**, not silently ignored. |
| **B5a** | **VideoToolbox backend (macOS) — the empty seam filled** — implement `videotoolbox_encoder.cpp` / `videotoolbox_decoder.cpp` against `VTCompressionSession` / `VTDecompressionSession` and `CVPixelBuffer`, modelled directly on `nvenc_encoder.cpp` (the same probe-then-create shape, the same `available()` honesty, the same `CVPixelBuffer`-in / `CVPixelBuffer`-out zero-copy contract that B10 needs). Without this, macOS has **no** hardware codec and falls back to software encoding, which at 1080p30 does not fit the §B.6 CPU budget. | `M` | On a macOS machine the backend probes `VTCompressionSessionCreate` successfully and encodes one H.264 frame; on a non-macOS build the entry still exists as a seam with `available() == false`; `available()` is false when the OS is too old for the required `VTCompressionOutputSettings`. |
| **B6** | **AV1 hardware encode** — NVENC AV1, AMF AV1, VA-API AV1, MediaCodec AV1 — where the silicon supports it. Same extension-of-existing-table shape as B5. **Status: NVENC AV1 is landed** (`src/modules/av1/src/hw_backends.cpp`, priority 450, probing `nvcodec::nvenc_codec_available(kAv1)`); the AMF / VA-API / MediaCodec AV1 entries are not wired yet, and the module's own software decoder is still a stub. | `M` | Same acceptance shape as B5; a backend that exists in the table but not in the silicon reports `available() == false` with a reason. |
| **B7** | **Screen-content coding** — the encoder-side knobs that make screen content cheap: intra block copy, palette / text mode, higher QP on flat regions, and `content-hint` (`detail` vs `motion` / `text`) signalled in SDP (`a=content-hint`). H.264's `x-google-content-hint` / `x-google-start-bitrate` `fmtp` parameters and the AV1 / HEVC equivalents. | `M` + `E` | The negotiated `fmtp` carries the content hint; the encoder receives it and switches its tuning; a screen-content frame set encodes materially smaller (bytes-per-frame) than the same frames encoded as camera content, asserted in a test. |
| **B8** | **Screen-content codec preference** — when the peer supports it, prefer AV1 (screen tools) > HEVC > H.264 for screen content; the preference table is configuration, not a hard-coded branch. | `E` + `profiles/` | Two profiles with different codec preferences negotiate different codecs; the negotiated result is reported in the stats. |
| **B9** | **Dirty-rect incremental encode** — the encoder encodes only the changed region and signals the region to the peer (a region-invalidation / loss-region refresh mechanism), or — where the codec cannot express it — the sender emits a keyframe with the changed region only where the codec supports it. This is a **screen-content-specific optimisation** and is explicitly *not* part of the generic video path. | `M` | With a static desktop the outbound byte rate drops to near zero between changes; the mechanism is disabled when the codec does not support it and the fallback is reported. |
| **B10** | **Zero-copy capture → encode** — the `GpuBuffer` from Track A reaches the hardware encoder's input surface without a CPU round-trip (D3D11 → NVENC, `CVPixelBuffer` → VideoToolbox, `VASurface` → VA-API). The CPU staging path remains for machines without a compatible pair. | `M` | A test asserts that on the hardware path no CPU readback of the captured surface occurs (the encoder receives the native handle); the CPU path is exercised by the software backend test. |
| **B10a** | **Hardware round-trip test (B10's prerequisite)** — the existing `src/modules/h264/tests/test_hw_backends.cpp` exercises only the **selection** logic (registry population, priority ordering, preferred-name resolution, bitstream helper functions) — **no test actually encodes a frame through a hardware backend and checks the output.** Add a hardware-leg test that, for each available backend, encodes one known frame, asserts the output is a non-empty Annex-B / parameter-set-carrying bitstream, and (for the platforms with a decoder) decodes it back and compares the pixels. Without this, B10's zero-copy claim cannot be verified — the only way to prove a frame never hit the CPU is to prove the encoded output is correct while the frame was never readable by the CPU. **Status: landed** as `src/modules/nvcodec/tests/test_nvcodec_roundtrip.cpp` (NVENC → NVDEC round trip, plus a CUVID test vector in `nvdec_test_vector.hpp`); the round trip is asserted for **every codec the driver offers** (H.264, H.265, AV1) — `NvencNvdecRoundTripH265` / `NvencNvdecRoundTripAv1` cover the codec-generic leg, and a host whose silicon lacks an encoder for a codec skips honestly with that reason; the non-NVIDIA backends still have no round-trip coverage. | `M` + `X` | On a machine with an available backend the round-trip test passes and is reported in the hardware leg; on a machine without one it **skips cleanly** (not a failure) and the skip reason names the missing backend. The test is registered with ctest but tagged so the standard CI matrix does not require the hardware. |
| **B11** | **Optional software AV1 / VP9 encode** — `svt-av1` (screen-content mode + intra block copy; official CI covers Windows/MSVC, macOS and Linux) and `libvpx`, both behind `NIMRTC_VENDORED_*` options that default OFF. | `V` + `M` | With the option ON, the software encoder registers, encodes a frame, and the peer decodes it; with the option OFF the build still configures and the plug-in is absent. |
| **B12** | **Encoder parameter set, computed in one place** — one `VideoEncoderParams` struct (width, height, fps, bitrate, keyframe interval, QP / quality, content hint, ROI) computed by the engine and pushed to the encoder. The encoder is **passive**: it has no access to `IBwe`, to the rate controller, or to the feedback stream. This is the video plan's B5. | `P` + `M` | The encoder plug-in interface exposes a single `set_params()`-style entry point; a test asserts the encoder never queries the bandwidth estimator. |
| **B13** | **Encoder restart on resolution change** — a parameter-set change (SPS/PPS, VPS, AV1 sequence header) is emitted with the new frame so the peer decodes without a PLI round-trip. This is the video plan's B8 and it is a **prerequisite** for any ladder that changes resolution. | `M` | After a resolution change the receiver decodes cleanly without a keyframe request; asserted in a pipeline test. |
| **B14** | **Keyframe storm limiter** — a peer that requests a keyframe every 100 ms produces at most the rate-limited number of IDRs. The existing `FirCoalescer` is finally wired into the engine. This is the video plan's B7. | `E` | A sustained keyframe-request storm produces a bounded IDR rate; the count is in the stats. |
| **B15** | **Codec telemetry** — per stream: selected backend id, hardware / software, zero-copy or staged, configured vs actual bitrate, encode / decode errors, keyframe rate, average encode latency, queue depth. | `E` + `M` | One stats call returns a coherent snapshot; the numbers change during a live session. |

---

## Track C — Scaler, colour, and pixel-format conversion

> **Why:** `src/modules/video_frame/` has colour conversion
> (`convert_i420_to_bgra`, `convert_i420_to_nv12` —
> `src/modules/video_frame/include/nimrtc/video_frame/frame.hpp:770,788`) but
> **no scaler at all** (a search for "scale" in that header finds nothing).
> The engine hard-codes 640×480 in two places — the video source config
> (`src/engine/src/engine_media.cpp:407-408`) and the video codec config
> (`engine_media.cpp:472-473`) — so a 1080p or 4K screen cannot be encoded
> today.

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **C1** | **Vendor `libyuv`** under `src/third_party/libyuv` (BSD-3-Clause, pinned submodule). It provides SIMD-optimised `I420Scale` (point / bilinear / box), `ARGBToI420`, `ARGBToI420` / `NV12` / `BGRA` conversions, and rotation — the exact set a capture → encode path needs, already used by Chrome. Cross-platform by construction: **Windows / macOS / Linux / Android / iOS**, with SSSE3/AVX2 on x86-64 and NEON/SVE2/SME on arm64. Actively maintained by Google/Chromium. | `V` | The build links it on all CI legs (x86-64 SSSE3/AVX2, arm64 NEON); `tools/check_vendor.py` verifies the pin. |
| **C2** | **Scaler in `video_frame`** — a `VideoFrameScaler` class wrapping libyuv, with filter-mode selection per use case: `kFilterBox` for the *display* path (quality), `kFilterBilinear` for the *encode* path (speed/quality balance). Backed by a small `IVideoScaler` seam **only if** a host genuinely needs to swap it; the default implementation is in-tree. | `M` | A scale test asserts the output dimensions and that a solid-colour source survives scaling without banding; a benchmark records ns/pixel at 1080p. |
| **C3** | **Format normalisation** — the capture path may deliver BGRA / NV12 / I420; the encoder path needs one canonical format. The conversion happens once, on the platform side, and the *format actually used* is reported. | `M` | The encoder receives a single documented format; the conversion count is in the stats and is zero on the zero-copy path. |
| **C4** | **ROI / region-of-interest** — the encoder may be given a sub-rectangle to code at higher fidelity (the cursor, the region the operator is looking at). The ROI is a rectangle in capture coordinates, scaled with the frame. | `M` | A test asserts the encoder receives the expected ROI rectangle and that the ROI survives a ladder resolution change. |
| **C5** | **Colour range / matrix correctness** — screen content is typically full-range BT.709 while camera content is often limited-range BT.601. Getting this wrong produces washed-out or crushed pictures that look like "the codec is broken". The seam carries the colour description; the encoder and the renderer both honour it. | `P` + `M` | The colour description round-trips through the seam; a test asserts the renderer and the encoder agree on it. |

---

## Track D — Adaptive rate control (the remote-desktop half)

> **Why:** the video plan's Track B (`video_bitrate_control`, two ladder
> modes) is scheduled for **v1.2.0** and is **not yet implemented**. The
> remote-desktop product cannot ship without it, but it also needs
> screen-specific behaviour the generic ladder does not have.

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **D1** | **Reuse the video plan's ladder** — the remote-desktop product consumes `video_bitrate_control`; it does **not** grow a second rate controller (RD-9). This item exists to state the dependency, not to re-implement it. | `M` (from the video plan) | The engine resolves the controller through the registry; a remote-desktop profile selects `smoothness` or `quality` mode through configuration. |
| **D2** | **Screen-specific ladder entries** — the ladder's resolution steps are chosen for *text legibility* (a remote-desktop user reads text at 1080p; dropping to 480p destroys the product). The screen profile therefore uses a **resolution floor** and spends the remaining budget on quality, or on frame rate when the operator is moving a window. The floor is **configurable per machine class**: 1080p on a hardware-encoding machine, **720p** on a software-only one (§B.6's software-path rule) — a floor the machine cannot sustain would be a floor the ladder breaks anyway, so it must not be hard-coded. | `M` + `profiles/` | A remote-desktop profile has a resolution floor that the ladder never crosses, even under a synthetic bandwidth collapse; the floor is visible in the stats; a software-only machine resolves a 720p floor, not a 1080p one. |
| **D3** | **Keyframe-cost awareness** — a ladder step that changes resolution forces an IDR, which costs a burst of bandwidth at the exact moment the link is congested. The controller budgets for it (step down on a *quiet* interval, or accept the burst knowingly). | `M` | A sawtooth bandwidth trace does not produce a staircase that flips level every interval; the level-change rate is bounded (the video plan's B9). |
| **D4** | **Video bitrate reaches the encoder** — the controller's target is pushed into the encoder's `VideoEncoderParams` (B12). Today the video encoder bitrate is written once at `open()` (1 Mbps, `src/engine/src/engine_media.cpp:474`) and never updated — the audio path already has `maybe_update_audio_bitrate()`; the video path must get its equivalent. | `E` | Under a synthetic bandwidth change the *encoder's* configured bitrate moves, not only the pacer rate; asserted in an engine test. |
| **D5** | **Per-stream bandwidth shares** — with multiple streams (M4), the session estimate is divided between them by a rule that is stated once and shared with the audio path. The video plan's F8 / `bandwidth_allocation.hpp` is the home of that rule; the remote-desktop profile declares the shares. | `E` | With two video streams and one audio stream the allocation is the documented split; the rule is unit-tested in the same place as the audio split. |

---

## Track E — Rendering and the receive experience

> **Why:** the shipped sinks **discard the pixels** (`video_sink.cpp:156,165`
> — "Headless sink (drops pixels; counts frames)"). There is no path from a
> decoded frame to a screen, and the video plan's R3 (video render clock) is
> scheduled for v1.4.0.

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **E1** | **Renderer seam** — extend `IVideoSink` with a GPU-surface delivery path (`render(const GpuBuffer&)`) alongside the existing CPU path, so a decoded frame reaches the display without a CPU round-trip on the HW decode path. The CPU path stays and is the fallback. | `P` | A plug-in can implement either path; the engine picks based on the decoder's `zero_copy_supported`. |
| **E2** | **Windows renderer** — D3D11 / DXGI swap-chain presentation of the decoded surface, with the frame paced to the display refresh (no busy-wait, `Present(0)` + a presentation clock). | `M` | A Windows test presents N frames and asserts the frame rate matches the source cadence within tolerance and that CPU usage is not pegged (no spin-wait). |
| **E3** | **macOS renderer** — Metal / AVSampleBufferDisplayLayer presentation. | `M` | macOS leg: frames are presented; the render thread does not block the engine tick thread. |
| **E4** | **Linux renderer** — Wayland (`wl_shm` or `zwp_linux_dmabuf` for zero-copy) with an X11 fallback (`XCB` / `X11 present`). | `M` | Linux leg: frames are presented under both compositors; the DMABUF path is used when the compositor advertises it. |
| **E5** | **Render clock and A/V sync (video half)** — the video plan's R3: turn the observed skew (ADR-017 mapping) into an explicit align / drop / delay decision. The engine already corrects the **audio** playout against the same capture timeline; the video half must consume the same skew and must not grow a second skew model (RD-9). | `M` + `E` | A synthetic skew beyond the configured tolerance produces a documented drop or delay correction, asserted in a test. |
| **E6** | **Input echo / cursor feedback** — the operator's own cursor movement is echoed back to the viewer with a bounded latency, or not at all (configurable). This is a remote-desktop product decision, not a codec feature. | `E` | The echo path is switchable; when on, the echoed cursor reaches the viewer within the configured budget; when off, nothing is sent. |
| **E7** | **Full-screen / scale-to-fit presentation** — the viewer scales the decoded picture to its window, honouring the aspect ratio, with letterbox or crop as configured. Uses the Track C scaler. | `M` | A non-matching aspect ratio renders without stretching. |
| **E8** | **Receiver-side back-pressure** — when the renderer cannot keep up, frames are dropped (latest-wins), not queued without bound. The video plan's R4. | `M` | Memory stays bounded under a sustained decode overrun; the drop count is reported. |
| **E9** | **Frame-accurate "what the operator saw"** — the receive side records, per displayed frame, the `ref_frame` id and the capture timestamp, so a host can answer "the user clicked on what they were looking at". This is the product payoff of NimRTC's existing timeline work and it must be *exposed*, not re-implemented. | `E` | The engine exposes the displayed frame's capture timestamp and frame id at the moment of presentation; a test asserts they match the sender's `record_video_ref_frame` mapping. |

---

## Track F — Audio device layer

> **Why:** `src/plugins/` contains **no** `audio_source.hpp` / `audio_sink.hpp`
> — the gap is already logged as F-24 in `docs/plan/audio-pipeline-spec.md:131`.
> Today the host feeds PCM into `send_audio()` and receives PCM from
> `set_on_audio_frame()`. A remote-desktop product needs the device layer
> on both ends, and the AEC reference path needs the **playback** position
> to be reported to the 3A.

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **F1** | **Audio device seams** — `IAudioSource` (capture) and `IAudioSink` (playback) in `src/plugins/`, matching the shape `docs/plan/ohos-audio-hal.md` §3.1 already designed. The seams carry the device format negotiation; the engine's existing PCM contract (`send_audio` / `on_audio_frame`) stays. | `P` | A host can plug in a device without touching the engine; the seams are in `src/plugins/` with no implementation. |
| **F2** | **Audio device module** `src/modules/audio_device/` — a **PortAudio**-backed implementation (MIT, cross-platform: WASAPI / MME / DSound / ASIO on Windows, Core Audio on macOS, ALSA / JACK / PulseAudio on Linux) rather than three hand-written platform backends (RD-10). On top of PortAudio: a shared ring buffer, a device-change / hot-unplug path, and a device-enumeration surface. | `M` | Module tests with a synthetic device; a device-loss event is surfaced as a clean error, not a crash. PortAudio is the **only** platform dependency in this item — the rest of the module is portable. |
| **F3** | **Playback progress reporting** — the audio sink reports the number of samples actually played out so the 3A can align the AEC reference. The engine already has the `set_render_delivered()` path (the APM reference bug was fixed in v1.1); this item wires the *device* to it. | `M` + `E` | With a real device the AEC reference tracks the loudspeaker signal; a test asserts the reported playback counter advances monotonically and matches the device's consumed-sample count. |
| **F4** | **AEC delay measurement** — the loudspeaker→microphone loop delay is measured (or configured) and fed to the 3A as `aec_delay_headroom_ms`. A remote desktop that plays audio on the same machine as the microphone **will** have echo otherwise. | `M` + `E` | The measured delay is reported in the stats and is within a sane range of the configured headroom; an echo test shows materially less residual echo than with 3A disabled. |
| **F5** | **Resampler** — the engine deliberately does not resample (audio plan DEC-3) and hard-errors when the PCM rate ≠ the negotiated codec rate. A device whose native rate is 44.1 kHz cannot feed Opus without a conversion. The implementation is **`libsamplerate`** (BSD-2-Clause since 2016, cross-platform, CMake build, actively maintained) — chosen over SpeexDSP, whose resampler is BSD-3 but whose last release was **2022-06**. | `M` + `P` | A 44.1 kHz capture device feeds the engine and the peer receives a 48 kHz Opus stream with the expected duration and no audible pitch error; the resampling is **explicit** and visible in the stats (the engine keeps DEC-3: no silent rate change). |
| **F6** | **System-audio capture (optional)** — "share the tab's audio" / "share the system's output" is a remote-desktop feature. It is a *second* audio source, not a variant of the microphone. | `M` | A system-loopback source can be selected alongside the microphone; the two are mixed or kept separate by configuration. |

---

## Track G — Control plane: keyboard, mouse, and the input↔frame binding

> **Why:** the data plane exists and works, but (a) the default backend is
> WebTransport/msquic, which is **not** interoperable with a browser's
> `RTCDataChannel`; the `"sctp"` backend was retired from the public
> selectors in v1.2.0 and was in any case an **in-process loopback socket
> that could never reach a peer** (`README.md:10`) — its
> `UsrsctpSocket::s_packet_output` is a no-op stub
> (`src/sctp/src/usrsctp_socket.cpp:454`); (b) the media and data planes are
> two independent connections with independent congestion control, so
> "control cannot be starved by video" is **not** currently true at the
> wire level (`README.md:550`); (c) there is no input-event codec, and the
> `ref_frame` binding is internal.
>
> **The honest position on browser interop (G1b):** a browser's
> `RTCDataChannel` requires a real **DTLS-SCTP association** (RFC 8841) —
> SCTP framed over the DTLS record layer on the ICE-selected pair. That is a
> transport-layer project, not a plug-in flip: `usrsctp` is vendored and the
> DTLS session already has an `send_app_data()` / `take_app_data()` seam, but
> the SCTP association has never been carried over the DTLS connection.
> G1b is therefore written as a **separate, explicitly-scoped item** with its
> own cost, and the plan does **not** depend on it: a NimRTC↔NimRTC product
> ships without it.

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **G1** | **NimRTC-native control channel over the DTLS app-data seam** — the DTLS session already exposes `send_app_data()` / `take_app_data()` (`src/modules/dtls/include/nimrtc/dtls/dtls.hpp:204-210`), and the DTLS record layer keeps record boundaries intact. A NimRTC↔NimRTC control channel rides those records, framed by the input codec (G3). This is the **interim** control path and it costs no transport work. | `M` + `E` | Two engines exchange input events over the DTLS app-data path with no DataChannel backend configured; the path is measurably lower-latency than the DataChannel path. |
| **G1b** | **Browser-interoperable DataChannel (RFC 8841 DTLS-SCTP association)** — carry a real SCTP association over the DTLS connection on the ICE-selected pair, so a browser's `RTCDataChannel` interoperates. `usrsctp` is vendored; the DTLS record layer must now deliver the SCTP packets instead of opaque application data. **This is the single most expensive item in Track G** and it is *optional* for the product. | `M` + `E` | The Chrome interop harness exchanges a DataChannel message with NimRTC over `RTCDataChannel` (not the WebTransport JS API) and records the case as a pass. **If G1b is descoped, the plan's other items are unaffected** and the product is NimRTC-only. |
| **G2** | **Control plane on its own transport** (RD-5) — the `teleop-ultra-low-latency` strategy shape (media on one stack, control on a separate `raw-udp-arq` stack with its own `max_p99_ms` / `freq_hz` / reliability) applied to the remote-desktop profile. The transport layer already models this: `TransportSession` owns a `media_stack` **and** a `control_stack`, and `ITransportStack::raw_control()` returns the raw-UDP datagram path (`src/transport/include/nimrtc/transport/transport_stack.hpp:134`) — but the engine never uses `raw_control()` (no reference in `src/engine/`), so the control stack is built and idle. The item is to **wire the control path onto it**, giving the control plane its own ICE selected pair and therefore its own congestion-control domain. | `E` + `profiles/` | Under a saturated video path the control path's p99 stays within its declared budget; the two stacks carry independent traffic and the control stack's packets are observed on a different 5-tuple. |
| **G3** | **Input event codec** — a compact, versioned binary encoding for mouse move / button / wheel / key down-up / text, in a new module `src/modules/rd_input/`. It is a **module, not a seam**: the transport is DataChannel, and only the payload format is NimRTC's. | `M` | A round-trip test encodes and decodes every event type; the encoding is compact (a mouse-move delta is a handful of bytes). |
| **G4** | **Input batching / coalescing / rate limiting** — pointer moves are coalesced to the display rate, not the input-device rate; key events are never dropped; the input path has its own rate limit so a stuck device cannot flood the link. | `M` | A synthetic 1000 Hz pointer stream produces at most one event per display frame; key events are lossless (asserted). |
| **G5** | **Input ↔ frame binding** — the product promise: "the click happened on the frame the operator was looking at". The engine's existing `record_video_ref_frame` / `ref_frame_to_capture_ts` machinery is exposed as a public API, and the input module attaches the current `ref_frame` id to each outgoing input event. | `E` + `P` | The operator's input event carries the frame id it was generated against; the viewer resolves it to the frame it displayed. A test asserts the two match. |
| **G6** | **Dead-man / heartbeat** — an optional liveness heartbeat and a dead-man switch (the operator releases a key or a button to stop the remote side from moving). This is the `takeover` profile's feature in the architecture doc, and a remote-desktop product needs it. | `E` + `M` | When the heartbeat stops, the remote side enters the configured safe state within the declared timeout; the state transition is reported. |
| **G7** | **Input safety filter** — a configurable allow / deny list per key and per axis range, so a viewer cannot send a dangerous combination. | `M` | A denied key never reaches the remote side; the denial is logged. |

---

## Track H — Multi-stream (screen + camera, and more)

> **Why:** the engine holds exactly **one** of each video plugin —
> `video_source_ / video_sink_ / video_receiver_ / video_sender_`
> (`src/engine/include/nimrtc/engine/engine.hpp:2080-2084`) plus a single
> `video_codec_` (`engine.hpp:2125`) — and one `video_sender_tuning.ssrc`.
> A remote-desktop product typically needs a
> screen stream plus a camera (or voice-camera) stream, and often a
> low-resolution "thumbnail" of the screen alongside the full one.

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **H1** | **Multi-stream media model** — the engine's single-video-slot model becomes a **stream set** keyed by a stable stream id (SSRC + mid). The public API changes are additive: `send_video(stream_id, frame)`, `set_video_source(stream_id, …)`, and the stats surface becomes per-stream. The single-stream call sites keep working. | `E` + `P` | Two video streams flow concurrently through one engine with independent SSRCs, independent codecs and independent stats; the existing single-stream tests still pass unchanged. |
| **H2** | **Per-stream codec / sink / receiver instances** — each stream owns its own encoder, sender, receiver, decoder and sink, resolved through the registry by id. The engine holds the set, not the slots. | `E` | Two streams use different codecs (e.g. H.264 screen + VP8 camera) in the same session. |
| **H3** | **Simulcast / spatial layers** — negotiate `a=simulcast` and send multiple encodings of one source at different resolutions; the receiver picks a layer. This is the video plan's T4, deferred there to v1.5.0+; the remote-desktop product needs at least the **thumbnail** case, which can be satisfied by an independent low-resolution capture of the same source instead of true simulcast. | `M` + `E` | The thumbnail case works without simulcast (independent capture). True simulcast, if pursued, negotiates and selects a layer. |
| **H4** | **Per-stream statistics** — the stats snapshot returns a list of per-stream entries, each with its own bitrate / ladder level / keyframe rate / backend id. | `E` + `M` | One stats call returns one entry per active stream. |

---

## Track I — Signalling, session and pairing

> **Why:** the repository contains **no signalling server and no session
> manager** — only `examples/demo-p2p`, which bridges a proxy over
> stdin/stdout. RD-7 keeps the server out of `src/`, but the product
> still needs one somewhere, and the example is the honest place for it.

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **I1** | **Signalling seam documentation** — a precise description of the exchange the engine requires: offer / answer, ICE candidate trickle (both directions), ICE restart, TURN credential refresh, and the `reconnect` path. `docs/guides/remote-desktop.md` is the canonical description; the example implements it. | `D` | A reader can write a signalling service from the guide alone. |
| **I2** | **Reference signalling service** — a minimal WebSocket service in `examples/remote-desktop/` (device registration, pairing code, room join, candidate relay, TURN credential issuance with short TTL). Not in `src/` (RD-7). | `X` | Two example clients connect through it and establish a media session. |
| **I3** | **Pairing / device identity** — a device registers with a short-lived pairing code; the operator's client exchanges it for a session credential. The credential, not the address, is the identity. | `X` | A session cannot be joined without the credential; an expired credential is refused. |
| **I4** | **TURN credential issuance** — short-TTL TURN credentials (RFC 7635 style `username:timestamp` + HMAC) rather than a static account, so the credential can be scoped to the session. | `X` + `E` | The engine's `add_turn_server()` consumes the issued credential; the credential expires and the session re-issues. |
| **I5** | **Reconnect / ICE restart** — the product must survive a network change (the operator's laptop moves between Wi-Fi and Ethernet). The engine already has the ICE-restart seam; the product wires it to the signalling service and to a network-change notification. | `E` + `X` | A simulated transport change recovers the session without the operator re-pairing. |
| **I6** | **Session limits and policy** — maximum concurrent sessions, session timeouts, idle disconnect, and an administrative kill switch. | `X` | Each limit is enforced and observable. |

---

## Track J — Observability, benchmarks, quality

> **Why:** the remote-desktop product is judged on *feel* — latency,
> sharpness, responsiveness. Those numbers must be measured by the
> project, not by anecdote.

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **J1** | **Per-frame CPU-time budget instrumentation** — the pipeline records the wall time of each stage (capture → convert → scale → encode → packetize → decode → render). | `E` + `M` | A stats call returns per-stage averages and p95. |
| **J2** | **Latency breakdown** — one-way latency decomposed into capture → encode → network → jitter-buffer → decode → render, with the ADR-017 timeline as the common clock. | `E` | The breakdown is reported and sums to the measured end-to-end latency within a stated tolerance. |
| **J3** | **QoE metrics** — freeze count / freeze duration, sharpness proxy, end-to-end latency percentiles, frame-rate at the viewer, input-to-photon latency (input event → frame displayed). | `E` + `M` | The metrics move under a synthetic network impairment and are plotted by the benchmark tool. |
| **J4** | **Weak-network benchmark** — extend `tools/benchmark_weaknet.py` with a remote-desktop scenario (netem loss / jitter / rate limit) that reports the *response curve*: ladder level vs. time, delivered fps vs. time, freeze count vs. time, latency vs. time — not a throughput number. | `X` | The tool emits a JSON artifact; the README benchmark table cites it. |
| **J5** | **Capture / encode benchmark** — a repeatable micro-benchmark for the capture path (frames/s per megapixel) and the encode path (ms per frame, per backend, per resolution). This is the evidence for RD-1's "hardware-first" claim. | `X` | A committed artifact shows the hardware backend's per-frame cost against the software fallback on the same machine. |
| **J6** | **Input-to-photon measurement** — the latency that actually matters for remote control: from the operator's input event to the frame that reflects it being displayed. | `X` + `E` | The measured value is reported end-to-end, including the control transport. |

---

## Track K — Architecture governance, documentation, API

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **K1** | **ADR-021 — codec bundling posture** (amends the video plan's OD-1): the two-filter selection table of §0 ("RD-2 + RD-10 in detail"), the licence allowlist, the platform-coverage requirement, and the `NIMRTC_VENDORED_*` option policy. | `docs/adr/` | ADR accepted; the video plan's O-3 row points at it. |
| **K2** | **ADR-022 — screen content as a content type** (RD-3): what the engine knows about screen content, where the dirty-rect / ROI / content-hint information flows, and why a screen capture is not a camera. | `docs/adr/` | ADR accepted; referenced from the `IVideoCapture` header. |
| **K3** | **Profile: `remote-desktop`** — a new profile (`profiles/remote-desktop.json` + `assembly::kProfileRemoteDesktop` + a schema update) describing: screen-content codec preference, the resolution floor, the ladder mode, the transport strategy (media + a separate control stack), the timeline on, the DataChannel on, and the 3A choice. | `M` + `profiles/` | The profile round-trips through the assembly serialiser exactly like the existing profiles (the byte-for-byte JSON test covers it). |
| **K4** | **Profile schema v1.2** — a `profile-v1.2.json` schema adding the remote-desktop keys (codec preference, resolution floor, ladder mode, content type). Strictly additive on v1.1. | `profiles/schema/` | Every v1.1 profile loads unchanged under the v1.2 schema. |
| **K5** | **Plugin author guide** — sections for `IVideoCapture`, `IAudioSource` / `IAudioSink`, the encoder / decoder split, the hardware-selection contract, and how to supply a screen-content codec under the RD-2 allowlist. | `D` | A reader can write a capture plug-in and a hardware encoder plug-in from the guide alone. |
| **K6** | **API bookkeeping** — every new public symbol carries `NIMRTC_API_STABLE` / `NIMRTC_API_UNSTABLE` / `NIMRTC_API_DEPRECATED`; `tests/test_api_classification.cpp`'s header-count baseline and `scripts/api_freeze_scan.js` are updated in the same commit as the new headers. | `T` + `P` | The freeze scan passes with the new headers; the header count test is updated **in the same commit** (not later). |
| **K7** | **CMake option hygiene** — every new module gets `NIMRTC_MODULE_<NAME>` (default ON); every new vendored dependency gets `NIMRTC_VENDORED_<NAME>`; every HW backend gets its existing `NIMRTC_PLUGINS_<NAME>` option and a `nimrtc_link_<backend>()` helper in `cmake/NimRTHwPlugins.cmake`. | `CMakeLists.txt` + `cmake/` | `cmake -LA` lists the new options; a build with each module OFF still configures. |
| **K8** | **Vendor pinning** — every new dependency is a pinned submodule with an entry in `src/third_party/vendor.json`, enforced by `tools/check_vendor.py` in CI, and listed in `NOTICE`. | `V` + `X` | `tools/check_vendor.py` passes; `NOTICE` mentions every vendored library and its licence. |
| **K8b** | **Cross-platform vendoring policy** (RD-10) — a vendored dependency must build on **all** CI legs that its capability applies to (Windows MSVC, Linux x86_64, Linux aarch64, macOS arm64), or it is behind an option that defaults OFF on the platforms it does not build. The `NOTICE` entry states the licence **and** the platform coverage. A dependency that only builds on one platform needs a documented reason. | `V` + `X` | `tools/check_vendor.py` records each dependency's platform matrix; CI verifies each default-ON dependency is actually built on every leg it claims; the `NOTICE` block for each library names its platforms. |
| **K9** | **Documentation honesty** — `README.md` / `README.zh.md` feature tables and `docs/zh/architecture.md` §6 are updated to describe what actually exists. A row must never imply a bundled codec or a device backend that is not shipped (the video plan's G5 rule, applied to the remote-desktop rows). | `D` | The feature tables match the shipped state; no row over-claims. |
| **K10** | **CHANGELOG discipline** — every new public API and every behaviour change has a `CHANGELOG.md` entry under `[Unreleased]`, and every `NIMRTC_API_DEPRECATED` symbol has one (ADR-015 Decision 3, step 1). | `D` | Code review confirms the pairing. |

---

## Track X — Explicitly deferred (recorded, not in scope here)

| ID | Item | Why deferred | Target |
|---|---|---|---|
| **X-1** | Virtual-desktop stitching (multiple monitors into one canvas) | Multi-monitor is served by "one capture instance per display" (M4 multi-stream). Stitching is a separate product feature with its own scaling / edge-seam problems. | v1.7.0+ |
| **X-2** | True simulcast / SVC | The thumbnail case is served by an independent low-resolution capture (H3). True simulcast multiplies the encoder / layer / feedback variables. | v1.7.0+ |
| **X-3** | `x265` / any copyleft codec | Excluded by RD-2's licence rule. A host may register one as an out-of-tree plug-in. | n/a |
| **X-4** | FFmpeg / GStreamer integration as a *bundled* dependency | LGPL distribution obligation + binary weight. A host may still register an FFmpeg-backed codec plug-in through the existing seam. This is the single biggest loss RD-10 would have bought: GStreamer covers the screen-capture cells (`ximagesrc`, `d3d11screencapturesrc`, `pipewiresrc`, `avfdsrc`) that this plan otherwise writes by hand (A3/A4/A5). **But the hardware-codec cells are NOT a loss** — FFmpeg's `nvenc` / `qsv` / `vaapi` / `videotoolbox` encoders are LGPL-buildable, and NimRTC's own backends already cover them directly. The FFmpeg trade would be: give up `GpuBuffer` zero-copy (D3D11→CUDA has no `hwmap` route in FFmpeg), give up Apache-2.0-only vendoring, and add a large binary — in exchange for a genericity this product does not want. | n/a |
| **X-4b** | A Rust-based cross-platform capture library (`xcap` Apache-2.0 / `scap` MIT) | Both are Rust; NimRTC is C++20. `xcap`'s Wayland path is documented as "not fully supported", so it would not even close the hardest cell. Introducing a Rust toolchain into a C++ engine is a larger architectural commitment than the problem warrants. Revisit if the project ever takes a Rust component. | n/a |
| **X-5** | ULPFEC / FlexFEC / RED | The video plan's T3 — deferred there to v1.5.0+; the RTX path (T1) covers the common loss case. | v1.5.0+ |
| **X-6** | Server-side recording / session archiving | A product concern (and a privacy one). The engine exposes decoded frames and the timeline, which is what an archiver needs. | v1.7.0+ |
| **X-7** | Client-side session recording | Same reason; a file-sink plug-in on `IVideoSink` is the natural extension point. | v1.7.0+ |
| **X-8** | Hardware encode coverage on every CI leg | The CI legs do not all have a hardware encoder. The **software** path is what CI enforces; the hardware paths are validated on a dedicated hardware leg (§B.7). | ongoing |

---

# Part B — Milestone plan (M1 .. M4)

## B.1 The milestone map

| Milestone | Theme | Release | Items | Exit criterion (the sentence that says "done") |
|---|---|---|---|---|
| **M1** | **See the real screen** | **v1.3.0** | A1–A8, A2b, A10, A11, B1–B4, B7, B10, B10a, B12, B13, B15, C1–C3, C5, K1, K2, K6–K8, K8b | Two native clients see each other's **real** desktop, in real pixels, at 1080p, over the network, with the picture actually encoded by a real codec — and the per-frame CPU budget of §B.6 is met. |
| **M2** | **Talk and control** | **v1.4.0** | A9, E1–E9, F1–F6, G1, G1b (optional), G2–G7, I1–I6, C4, D5, K3, K4, K9, K10 | The session has **sound in both directions** (with working AEC), and the operator can **drive** the remote machine — mouse and keyboard — with the input bound to the frame the operator was looking at. |
| **M3** | **Survive the network** | **v1.5.0** | D1–D4, B9, B14, E8, J1–J4, plus the video plan's T1 (RTX) / T2 | Under a synthetic lossy, jittery, rate-limited link the session **degrades predictably** (ladder steps down, then recovers) instead of stalling or collapsing, and the measured quality is better than a fixed-bitrate baseline. |
| **M4** | **Multi-stream and screen-content quality** | **v1.6.0** | B5, B6, B8, B11, H1–H4, J5, J6 | The product ships the features a remote-desktop user actually pays for: **screen + camera concurrently**, **screen-content coding** (AV1/HEVC screen tools where available), a **low-bitrate fallback that stays legible**, and a measured **input-to-photon** latency. |

### Why this order

- **M1 before everything.** Nothing else can be evaluated on a fake picture.
  The codec layer is the blocker: the pipeline is already wired, but the
  encoder produces synthetic bytes and the decoder produces no pixels.
- **M2 before M3.** Audio and control are what make it a *call* rather than
  a one-way stream, and they are independent of the rate controller. They
  also carry the hard engineering (device layers, AEC alignment) that has
  the longest tail.
- **M3 after M2.** The ladder is a *quality* feature; it needs a real
  stream to act on, and its acceptance is a measured curve, not a
  feature list.
- **M4 last.** Multi-stream and screen-content coding multiply the number
  of interacting variables. Landing them on top of a stable M1–M3 is the
  difference between a feature and a rewrite.

### Relationship to the existing plans (no duplicated work)

| This plan | Existing plan | How they compose |
|---|---|---|
| B1 (encoder / decoder split) | video plan C2 / C3 | **Same item.** M1 delivers it; the video plan's v1.3.0 milestone N5 references this plan. |
| B2 (hardware-first selection) | video plan C12 / C13 / C14 | **Same item.** The registry and the selection policy already exist (`src/plugins/include/nimrtc/plugins/hw_video_backend.hpp`); this plan adds the real backends and the screen-content preference. |
| B12 / B13 / B14 | video plan B5 / B8 / B7 | **Same items.** Landed in v1.2.0 (video plan Track B) — M1 depends on them, M3 hardens B14. |
| D1 (the ladder) | video plan B2 / B3 / B4 | **Same item.** v1.2.0. M3 consumes it and adds the screen-specific entries. |
| E5 (render clock) | video plan R3 | **Same item.** v1.4.0. |
| E8 (receiver back-pressure) | video plan R4 | **Same item.** v1.4.0. |
| H3 (simulcast) | video plan T4 | Deferred in both; the thumbnail case is the cheap substitute. |
| — | audio plan WP-1..WP-15 | The audio pipeline lands in v1.2.0. **M2 depends on it** and adds only the device layer (Track F). |
| G2 (control transport) | `transport-strategies.md` | Reuses the existing strategy shape; no new transport. |
| G5 (input ↔ frame binding) | ADR-017 timeline | Exposes existing machinery; no new timeline model. |

## B.2 M1 — "See the real screen" (v1.3.0)

**Milestone order inside M1** (dependency-driven):

| Step | Items | Ends when |
|---|---|---|
| **M1-a — Real codec** | B1, B3, B4, B12, B13 | A frame encoded by `openh264` survives the full pipeline and comes back as real pixels. The fake stub encoder is no longer the default path. |
| **M1-b — Capture seam** | A1, A2, A2b, A10, A11 | A third-party capture plug-in can be registered and driven by the engine; the engine pumps frames on the tick thread; the **portable** capture layer is complete and exercised by a synthetic device on every CI leg. || **M1-c — Platform capture** | A3, A4, A5, A8, A6 | On each platform the engine captures a real screen into a `GpuBuffer`, with the cursor composited where the platform does not do it. |
| **M1-d — Hardware encoders** | B2, B5 (encode half), B10, B10a, B15 | On a machine with a GPU the capture surface reaches the encoder without a CPU round-trip; the backend id is in the stats; and the **round-trip test proves the hardware path really encodes a decodable frame** (B10a) rather than only proving the backend was *selected*. |
| **M1-e — Scaling / format** | C1, C2, C3, C5 | A 4K capture is scaled to the ladder's resolution in SIMD time; the format and colour range are consistent end to end. |
| **M1-f — Screen content** | B7, A7 | The negotiated `fmtp` carries the content hint and the encoder changes its tuning accordingly. |
| **M1-g — Governance** | K1, K2, K6, K7, K8, K8b | ADR-021 / ADR-022 accepted; the freeze scan and vendor check are green; each vendored dependency's platform matrix is recorded. |

**M1 exit measurement** (not a feature list — a measurement):

| Metric | Target |
|---|---|
| Capture rate at 1080p | ≥ 30 fps sustained on the CI hardware reference, no frame drops under a static desktop |
| CPU time per frame (capture → encode → packetize) | within §B.6's budget on the hardware path; within 2× of it on the software path |
| End-to-end picture correctness | the displayed picture matches the captured screen within the codec's tolerance (a pixel-diff test on a synthetic pattern) |
| Zero-copy | on the hardware path, the encoder receives the native handle; **zero** CPU readbacks of the captured surface |

## B.3 M2 — "Talk and control" (v1.4.0)

| Step | Items | Ends when |
|---|---|---|
| **M2-a — Audio devices** | F1, F2, F3 | A real microphone feeds the engine and a real speaker plays the peer's voice; the playback position reaches the 3A. The device layer is **PortAudio**-backed, so one implementation covers Windows / macOS / Linux (RD-10). |
| **M2-b — AEC reality** | F4, F5 | The loop delay is measured and configured; the residual echo is materially lower with 3A on than off; the 44.1 kHz device question is answered by the `libsamplerate` resampler (cross-platform, maintained). |
| **M2-c — Rendering** | E1, E2, E3, E4, E7 | A decoded frame reaches the screen at the source cadence on every platform, via the GPU path where available. |
| **M2-d — A/V sync** | E5 | A synthetic skew produces a documented, tested correction. |
| **M2-e — Control transport** | G1, G2 (and G1b if browser interop is in scope) | The control path is on its own stack with its own budget; a NimRTC↔NimRTC control channel works over the DTLS app-data seam. |
| **M2-f — Input codec** | G3, G4, G5, G7 | Every input event round-trips; pointer moves are coalesced; keys are lossless; the input carries the `ref_frame` id. |
| **M2-g — Session** | I1, I2, I3, I4, I5, I6 | The example service pairs two devices and issues short-TTL TURN credentials; a network change recovers the session. |
| **M2-h — Camera & ROI** | A9, C4, E6 | The camera stream reaches the peer; the ROI follows the region of interest; the cursor echo is switchable. |

**M2 exit measurement:**

| Metric | Target |
|---|---|
| Audio round-trip latency (mouth → ear) | ≤ 150 ms on the loopback reference, ≤ 300 ms over the simulated WAN |
| Residual echo with 3A on | materially below the 3A-off baseline in the echo test (the artifact is committed) |
| Input → displayed-frame latency (local) | ≤ 50 ms p50 |
| Control path p99 under a saturated video path | within the profile's declared `max_p99_ms` |
| Input-event loss | zero for key events; pointer moves are coalesced by design |

## B.4 M3 — "Survive the network" (v1.5.0)

| Step | Items | Ends when |
|---|---|---|
| **M3-a — Encoder follows the network** | D4 | The *encoder's* bitrate (not just the pacer rate) tracks the estimate. This is the item that is missing today and that makes every other adaptation claim false. |
| **M3-b — Screen-aware ladder** | D1, D2, D3 | The ladder respects the resolution floor and budgets for the IDR cost of a resolution change. |
| **M3-c — Recovery** | B9, B14, E8, video plan T1 / T2 | A lost packet is recovered through RTX; a keyframe-request storm is rate-limited; the receiver's memory stays bounded. |
| **M3-d — Measurement** | J1, J2, J3, J4 | The response-curve artifact exists and shows the closed loop beating a fixed-bitrate baseline (the video plan's O5 requirement). |

**M3 exit measurement:**

| Metric | Target |
|---|---|
| Delivered fps under a 500 kbps / 3 % loss link | the ladder settles at a stable level; no oscillation (level-change rate bounded) |
| Freeze count vs. the fixed-bitrate baseline | lower, with the comparison artifact committed |
| End-to-end latency under the same impairment | does not grow without bound; the jitter buffer's contribution is reported |
| Resolution floor | the screen profile's floor is never crossed |

## B.5 M4 — "Multi-stream and screen-content quality" (v1.6.0)

| Step | Items | Ends when |
|---|---|---|
| **M4-a — Multi-stream** | H1, H2, H4 | Two streams flow concurrently with independent codecs, SSRCs and stats; the single-stream API still works. |
| **M4-b — Screen-content codecs** | B5, B5a, B6, B8, B11 | Where the silicon supports it, the session negotiates HEVC / AV1 for the screen; the preference is configuration; the software AV1 path is available behind an option. |
| **M4-c — Incremental** | B9 (dirty-rect) | A static desktop produces near-zero outbound bytes between changes. |
| **M4-d — Proof** | J5, J6 | The hardware-vs-software encode cost artifact and the input-to-photon number are committed. |

**M4 exit measurement:**

| Metric | Target |
|---|---|
| Concurrent screen + camera | both streams at their configured quality, each with its own ladder level |
| Screen bitrate at the same perceived sharpness | materially lower than the camera-tuned encode of the same content (the screen-content claim, measured) |
| Static-desktop outbound bitrate | ≈ 0 between changes |
| Input-to-photon latency | measured and reported, with the decomposition |

---

## B.6 Performance budgets (RD-8)

These are **budgets**, not claims — the measurement is a milestone exit
criterion. Reference machine for the CI legs: 4-core x86-64 @ 3 GHz,
1080p30, 8 GB RAM. "HW path" = the platform's own capture + encode pair.

| Stage | HW path budget | Software path budget | Notes |
|---|---|---|---|
| Screen capture (per frame, 1080p) | ≤ 1.5 ms | ≤ 4 ms | HW = DXGI / ScreenCaptureKit / PipeWire; software = X11 `XGetImage` + a CPU copy |
| Format conversion (BGRA/NV12 → I420) | 0 (folded into the capture path) | ≤ 2 ms | libyuv `ARGBToI420` is SIMD; the cost is the copy, not the maths |
| Scale (1080p → ladder level) | ≤ 1.5 ms | ≤ 1.5 ms | libyuv `I420Scale`; bilinear in the encode path, box in the display path |
| Encode (1080p, 4 Mbps, H.264) | ≤ 4 ms | ≤ 45 ms | HW = NVENC / VA-API / QSV / VideoToolbox; software = OpenH264 at a realtime preset |
| Packetize (FU-A, ~1300 B MTU) | ≤ 0.5 ms | ≤ 0.5 ms | The existing `video_pipeline` path; this is the budget that keeps the encoder thread short |
| Total send-side CPU per frame | **≤ 8 ms** | **≤ 55 ms at 1080p** | See the software-path rule below |
| Decode (1080p) | ≤ 4 ms | ≤ 12 ms | HW = DXVA / NVDEC / VideoToolbox / MediaCodec; software = `openh264` / `dav1d` |
| Render / present | ≤ 1 ms (GPU) | ≤ 2 ms | The present call must not block the tick thread |
| Audio capture + 3A (10 ms frame) | ≤ 1.5 ms | ≤ 1.5 ms | The APM's cost is fixed; the device layer's cost is the copy |
| Audio playback | ≤ 0.5 ms | ≤ 0.5 ms | Ring-buffer write |
| Resample (44.1 k → 48 k, 10 ms stereo) | ≤ 0.3 ms | ≤ 0.3 ms | `libsamplerate`; the cost is negligible next to the 3A, which is why F5 is a *quality* item, not a performance one |

**The software-path rule (important — do not skip this).** At 30 fps a
single core offers **33 ms** per frame. The software encode alone costs
~45 ms at 1080p, so **a machine with no hardware encoder cannot run
1080p30 on one core** — and the plan does not pretend otherwise. The
consequences are baked into the design rather than discovered later:

1. The software path's *sustainable* operating point is **720p30** (≈ 12 ms
   encode, ≈ 20 ms total — comfortably inside one core). The budget column
   above is therefore split: the **≤ 55 ms** figure is the 1080p *single
   frame* cost, which is affordable only with frame-skipping across a
   2-core budget.
2. The **ladder's lowest rung for a hardware-less machine is 720p**, and the
   profile states it. A hardware-less client is told up front, not after
   the picture stutters.
3. The **frame-skip** mechanism (the video plan's P3) is what absorbs the
   overrun: the encoder drops frames rather than the network, and the
   dropped-frame count is reported.
4. This is the strongest argument for RD-1's hardware-first default: the
   software path is a **fallback that changes the resolution**, not an
   equal-quality alternative.

**Budget rules:**

1. The **encode + packetize** pair must fit in the codec's frame budget with
   headroom for the pacer; if it does not, the encoder is asked to skip
   frames (the video plan's P3), not the network to drop them.
2. A stage that exceeds its budget is **reported** in the stats; the
   pipeline does not silently get slower.
3. The tick thread must never block on a device call — device I/O happens
   on the capture thread, and the engine's tick thread only ever *pops* a
   ready frame (RD-6).

---

## B.7 Verification matrix

| Level | What it covers | Where |
|---|---|---|
| Module unit tests | every new module's own logic (queue overflow, ladder transitions, codec round-trip, event codec, device abstraction) | `src/modules/<name>/tests/` |
| Pipeline integration tests | capture → encode → RTP → JB → decode → render, in-process, with synthetic sources | `tests/` |
| Engine loopback tests | two engines, one session, real media, asserted on the stats | `tests/` |
| Weak-network / quality | the response curve under netem | `tools/benchmark_weaknet.py` |
| Interop | a browser peer for the media path; the standard DataChannel path (G1b) only if that item is in scope | `interop/` + `tests/chrome_interop_harness/` |
| CI legs | Windows MSVC, Linux x86_64, Linux aarch64, macOS arm64 — build + unit tests green | `.github/workflows/ci.yml` |
| **Cross-platform matrix check** (RD-10 / K8b) | every vendored dependency's **platform matrix** is recorded and each default-ON dependency is actually built on every leg it claims; a dependency that only builds on one platform is option-gated and its `NOTICE` entry says so | `tools/check_vendor.py` (extended) + CI |
| **Same-result check** (RD-10) | the portable layer produces **the same result** on every leg: the same synthetic capture device, the same scale/colour output, the same codec round trip. A platform backend may differ only in *how* it acquires the pixels, never in what the pipeline does with them. | `src/modules/video_capture/tests/` + `src/modules/video_frame/tests/` — one test body, run on every leg |
| Hardware leg | NVENC / QSV / AMF / VA-API / VideoToolbox / MediaCodec encode and decode, on a machine that has them. This leg runs the **B10a round-trip test** (encode one frame → assert a non-empty parameter-set-carrying bitstream → decode back → compare pixels), which is currently **missing** — the existing `test_hw_backends.cpp` only exercises selection logic. **Not** in the standard CI matrix (the legs do not all have the hardware); reported honestly per the video plan's O-6. | a dedicated job or a documented manual run |
| Budget check | the §B.6 table measured, as a committed artifact | `tools/benchmark_capture_encode.py` (new, Track J5) |
| Freeze / vendor checks | `scripts/api_freeze_scan.js`, `tools/check_vendor.py`, `tools/check_prerequisites.py` | CI |

---

## B.8 Risks and the pre-agreed response

| Risk | Impact | Response |
|---|---|---|
| **R-1** The real codecs (OpenH264 / dav1d) are slower or heavier than expected, and the software path misses its budget. | High | The hardware path is the default (RD-1), so the software path is the *fallback* for machines without a GPU. The budget table measures both; if the software path misses, the ladder's lowest level drops (M3) rather than the frame rate. |
| **R-2** A capture API is unavailable or restricted on a platform (Wayland's portal, macOS permissions, Windows secure desktop). | High | The capture seam reports the restriction as a distinct error code, the profile names the fallback backend, and the README states the platform limitation (RD-9 / K9 honesty rule). It is not silently degraded to "black frames". |
| **R-3** Multi-stream (H1) turns out to be an engine-wide refactor with a large blast radius. | High | H1 is confined to M4 — after M1–M3 are stable. The additive public API (`send_video(stream_id, …)`) keeps the single-stream call sites working, so the refactor is incremental and revertible. |
| **R-3b** G1b (RFC 8841 DTLS-SCTP association) turns out to be larger than estimated and blocks M2. | High | G1b is explicitly optional and isolated: the interim control channel (G1) needs no transport work, and the control plane (G2) rides the existing control stack. Descoping G1b changes the product's *browser* interop, not its function. The plan's exit criteria do not mention it. |
| **R-4** The screen-content coding claims do not hold against the camera-tuned baseline. | Medium | B7's acceptance is a **measured** bytes-per-frame comparison, not a claim. If the measurement does not show a material difference, the feature is reported honestly as "not a win on this hardware" rather than shipped as a headline. |
| **R-5** The AEC reference path regresses when a real speaker is used (the v1.1 fix was validated against synthetic signals). | Medium | F3 / F4 are explicitly part of M2's exit measurement (an echo test with a real output device), not a unit test. |
| **R-6** Bundling codecs breaks the distribution model (L5, the NOTICE file, the release bundle). | Medium | K1 / K8 handle it: pinned submodules, `vendor.json`, `NOTICE` updated, `check_vendor.py` in CI, and the release bundle's licence manifest lists every vendored codec. The `NIMRTC_VENDORED_*` options let a distributor strip the optional ones. |
| **R-7** The signalling example is mistaken for a production server. | Medium | RD-7 and the example's README state plainly that it is a reference implementation, and `docs/guides/remote-desktop.md` documents how to replace it. |
| **R-8** The public API surface grows past the freeze scan's tolerance in one release. | Medium | K6 makes the header-count update part of the same commit as the new headers, so the scan never sees an unclassified symbol. |
| **R-9** The plan duplicates work already scheduled in the video / audio plans and the two drift. | Medium | §B.1's "relationship to the existing plans" table names every shared item and states which plan owns it. Any new overlap is an ADR amendment, not an implicit divergence. |
| **R-10** The remote-desktop product's needs pull the engine towards a product-specific shape (a "remote desktop mode" flag inside the engine). | High | The product shape lives in `profiles/`, `examples/` and the plug-in seams. The engine gains **configuration**, not modes. A profile that needs a capability the engine lacks is answered with a new plug-in, not a branch in `src/engine/`. |
| **R-11** A vendored dependency turns out not to build on one of the CI legs (a common failure with `svt-av1`, which needs NASM on Windows). | Medium | K8b makes the platform matrix an explicit, checked property: a dependency that cannot build everywhere is behind an option that defaults OFF where it does not build. The build never breaks because of an optional dependency. |
| **R-12** RD-10 is read as "never write platform code" and the plan under-delivers on the cells where no cross-platform library exists. | Medium | The rule is stated with its **two documented exceptions** (§0 "What RD-10 could not cover") — hardware codecs and screen capture — so the boundary is explicit rather than argued case by case. A new platform-specific implementation outside those two cells needs an ADR amendment. |

---

## B.9 Definition of done (per milestone)

A milestone is done when **all** of the following hold:

1. Every item's acceptance criterion is backed by a named automated test, and that test is green on the CI legs that apply.
2. The milestone's **exit measurement** table is filled in with real numbers and the artifact is committed.
3. The public API additions carry stability markers; the freeze scan and the header-count test are green.
4. `CHANGELOG.md` has an entry for every new public symbol and every behaviour change.
5. `README.md` / `README.zh.md` / `docs/zh/architecture.md` describe what actually shipped — no row over-claims.
6. `NOTICE` lists every new vendored dependency, and `tools/check_vendor.py` is green.
7. No new global mutable state outside `nimrtc::core::`; no concrete module header in the engine's public API.

---

## B.10 Open items — status

| # | Question | Status |
|---|---|---|
| **Q-1** | Is a resampler in scope, or does the host convert? (F5) | **Decided: a resampler is in scope.** A 44.1 kHz capture device is the common case, and "the host must convert" pushes the problem to every consumer. The implementation is `libsamplerate` (BSD-2-Clause, cross-platform, maintained); the engine keeps the DEC-3 contract (no implicit rate change — the resampler is explicit and visible in the stats). |
| **Q-1b** | What is the software path's sustainable resolution? | **Decided: 720p30** (§B.6's software-path rule). The 1080p software encode costs ~45 ms/frame against a 33 ms budget at 30 fps, so a hardware-less machine runs 720p and is told so. The ladder's lowest rung for such a machine is 720p, and the frame-skip mechanism absorbs the residual overrun. |
| **Q-2** | Is the browser-interoperable DataChannel (RFC 8841) in scope, or is the product NimRTC-only? | **Default: the product is NimRTC-only for v1.4.0.** The interim control channel (G1, DTLS app-data) covers the product. G1b is written as a separate, explicitly-costed item so it can be descoped without disturbing the rest of the plan — it is the only item in the plan that would require transport-layer work of that size. |
| **Q-3** | Does the screen profile use `smoothness` or `quality` ladder mode? | **Default: `smoothness` for the screen stream** (a late-but-fresh frame is worth more than a sharp-but-stale one when the operator is dragging a window), configurable per stream. The floor in D2 is what protects legibility. |
| **Q-4** | Which screen-content codec is the default when the peer supports several? | **AV1 > HEVC > H.264**, configurable (B8). The default prefers AV1 because its screen tools (intra block copy, palette) are the strongest, and because Chrome supports AV1 — the interop case matters for a browser peer. |
| **Q-5** | Is the reference signalling service in `examples/` acceptable to ship in the release bundle? | **Decided: yes, as a clearly-labelled reference implementation** (RD-7). It is small and it is the thing that makes the product reproducible for a reader. |
| **Q-6** | Does M4's multi-stream require a public API change that breaks v1.x consumers? | **No.** The API change is additive: `send_video(stream_id, frame)` is a new overload; the existing single-stream entry point keeps its exact signature and semantics (ADR-015 Decision 2 — additive changes are a MINOR bump). |
| **Q-7** | Should the platform-specific capture backends be replaced by GStreamer or a Rust capture library? | **Decided: no.** GStreamer is LGPL (RD-2 excludes it) and a Rust library would add a Rust toolchain to a C++20 engine while still not closing the Wayland cell. The plan writes the three platform backends (A3/A4/A5) but keeps them **thin adapters** over a portable capture layer (A2b) — the portable part is where the engineering goes. |
| **Q-8** | What is the cross-platform rule for a vendored dependency that does not build everywhere? | **Decided (K8b):** a default-ON dependency must build on every CI leg it claims; otherwise it is behind a `NIMRTC_VENDORED_*` option that defaults OFF where it does not build, and its `NOTICE` entry states the platform matrix. No exception is granted by default — an exception needs an ADR amendment. |

---

## Appendix — Item index

| Track | Items | Milestone |
|---|---|---|
| A — Capture | A1, A2, A2b, A3, A4, A5, A6, A7, A8, A9, A10, A11 | M1 (A1–A8, A10, A11), M2 (A9) |
| B — Codec | B1, B2, B3, B4, B5, B5a, B6, B7, B8, B9, B10, B10a, B11, B12, B13, B14, B15 | M1 (B1–B4, B7, B10, B10a, B12, B13, B15), M3 (B9, B14), M4 (B5, B5a, B6, B8, B11) |
| C — Pixel | C1, C2, C3, C4, C5 | M1 (C1–C3, C5), M2 (C4) |
| D — Rate control | D1, D2, D3, D4, D5 | M3 (D1–D4), M2 (D5) |
| E — Rendering | E1, E2, E3, E4, E5, E6, E7, E8, E9 | M2 (E1–E7, E9), M3 (E8) |
| F — Audio devices | F1, F2, F3, F4, F5, F6 | M2 |
| G — Control | G1, G1b, G2, G3, G4, G5, G6, G7 | M2 |
| H — Multi-stream | H1, H2, H3, H4 | M4 (H3 is the thumbnail case, satisfied by H1/H2) |
| I — Session | I1, I2, I3, I4, I5, I6 | M2 |
| J — Observability | J1, J2, J3, J4, J5, J6 | M3 (J1–J4), M4 (J5, J6) |
| K — Governance | K1, K2, K3, K4, K5, K6, K7, K8, K8b, K9, K10 | M1 (K1, K2, K6, K7, K8, K8b), M2 (K3, K4, K9, K10), M5 (K5) |
| X — Deferred | X-1 .. X-8 + X-4b | — |

**Count:** 89 in-scope items (A 12 + B 17 + C 5 + D 5 + E 9 + F 6 + G 8 + H 4 + I 6 + J 6 + K 11) + 9 deferred.
