# Changelog

All notable changes to NimRTC are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/)
and uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.10.1] - 2026-09-15

### Status: Tech Preview

### Highlights

- **Transport plugin interface**: new plugin seam with `IPlugin` interface and
  `TransportSelector`; `raw_udp`, `dtls`, and `sctp` plugin stubs wired.
  See `docs/plan/transport-selection.md`.
- **RFC 5246 PRF (P_SHA-256)**: standalone wolfSSL-backed PRF for DTLS
  key derivation; all 7 RFC 5246 section 5 KAT vectors pass. Replaces the
  BCrypt-only stub.
- **ArqRawUdp real-socket fixes**: corrected ACK frame byte offsets in
  `recv_main`, fixed `peer_endpoint_` race (added `peer_mu_`), repaired
  `send()` span dead code, resolved `close()` deadlock (socket-close-first
  ordering). The ARQ state machine now advances correctly against a real
  UDP socket; burst test reports `sent=50 acks_recv=50 retransmit=0`.
- **New test**: `test_raw_udp_real_loopback` (4/4 cases: single-packet,
  burst-50, ephemeral-port reflection, error-path).

### Fixed

- **CI / WebRTC APM prebuild guard**: `src/third_party/webrtc_audio_processing/CMakeLists.txt`
  previously `FATAL_ERROR` on any checkout that lacks the meson-built
  `libwebrtc-audio-processing-2.{a,lib}`. This blocked all four CI runners
  (windows/linux/macos/aarch64) because the prebuild requires a separate
  `meson` + `abseil-cpp` step that is not wired into the CI matrix. Fixed by
  making the guard conditional on `NIMRTC_VENDORED_WEBRTC_APM` (default `ON`):
  `=ON` retains the original `FATAL_ERROR` + guidance; `=OFF` defines an empty
  `INTERFACE` stub target and lets `audio3a` run with the built-in
  passthrough stub. CI now passes `-DNIMRTC_VENDORED_WEBRTC_APM=OFF` to all
  four platform Configure steps. See `docs/zh/architecture.md` §11.5.5.
- **Release / WebRTC APM prebuild wiring**: `.github/workflows/release.yml`
  was missing the `tools/fetch_webrtc_apm.py` step on all four platforms,
  so a tag push would fail at the cmake configure stage with the same
  `FATAL_ERROR` even though release artefacts are supposed to carry real
  3A (`NIMRTC_VENDORED_WEBRTC_APM=ON` per §11.5.5). Added explicit
  `Install meson` + `Prebuild WebRTC APM` steps before `Configure` on
  windows, linux-gcc, linux-aarch64, and macos-clang.

---

## [Unreleased]

### Status: Tech Preview

### Highlights

- **PAL Slice 2 + 3** (`docs/plan/pal-architecture.md` §4): the engine's
  `register_all_default_plugins()` is now backed by an explicit iterable
  table (`core::detail::kDefaultRegistrars[]` in
  `src/core/src/pal_default_registrars.cpp`) instead of an inline
  statement list, and every built-in plugin factory id literal
  (`"webrtc"`, `"webrtc_apm"`, `"opus"`, `"h264"`, `"adaptive"`,
  `"wolfssl"`, …) is wrapped in a compile-time-unique
  `NIMRTC_PLUGIN_ID()` macro at its `id()` return statement. Both
  changes are purely additive — no public API or on-the-wire id change.
  Adds 2 new `EnginePluginLoading` subtests
  (`pal_default_registrars_table_is_nonempty`,
  `pal_plugin_id_wrappers_preserve_string_values`); existing 7 subtests
  stay green.

### Added

- **`src/core/src/pal_default_registrars.cpp`** (PAL Slice 2):
  `core::detail::kDefaultRegistrars[]` table of all built-in plugin
  registrar function pointers. `src/core/CMakeLists.txt` adds an
  `nimrtc_core_objects` OBJECT library that compiles this file and is
  linked INTERFACE by `nimrtc::core`. Existing `core` Layout Invariant 1
  (INTERFACE/header-only) is otherwise preserved.
- **`src/core/include/nimrtc/core/plugin_id.hpp`** (PAL Slice 3):
  `PluginIdTag<N>` template + `NIMRTC_PLUGIN_ID(x)` macro that wraps a
  string literal at each plugin factory's `id()` callsite, giving it a
  compile-time-unique type. Implicit conversion to `std::string_view`
  preserves all existing `return id_literal;` patterns.
- **5 plugin factories migrated to `NIMRTC_PLUGIN_ID()`** (PAL Slice 3):
  `audio3a::NullPluginFactory` (`"webrtc"`), `audio3a::WebRtcPluginFactory`
  (`"webrtc_apm"`), `opus::OpusPluginFactory` (`"opus"`),
  `h264::H264PluginFactory` (`"h264"`), `jb::PluginFactory`
  (`"adaptive"`), `dtls::WolfsslDtlsFactory` (`"wolfssl"` via the
  existing `kBackendId` constexpr literal).
- **2 new test subtests** in `tests/test_engine_plugin_loading.cpp`:
  `pal_default_registrars_table_is_nonempty` (Slice 2 — table iteration
  registers all 5 unconditional categories) and
  `pal_plugin_id_wrappers_preserve_string_values` (Slice 3 — `static_assert`
  enforces distinct per-callsite types; runtime check verifies all 6 known
  factory ids match their v0.10.1 string values byte-for-byte).

### Notes

- No new protocol or content features.
- DTLS seam (`"wolfssl"`) remains Slice 4 scope — its id literal now goes
  through `NIMRTC_PLUGIN_ID()` but registration stays module-local via
  `test_only::set_wolfssl_factory_for_testing()`. Promoting it to
  `core::register_all_default_plugins()` is a Slice 7 / Slice 8 concern.

---

## [0.10.0] - 2026-09-14

### Status: Tech Preview consolidation

This release closes the v0.9 tail and lands one structural refactor.
**No new protocol or content features.** P2 content (DataChannel interop,
SFU relay, PCM tap landing, Profile library officialisation) is queued for v0.11.0.

### Highlights

- **PAL Slice 1** (Plugin Adaptation Layer): engine now resolves audio3a /
  codec / video_codec plugins through a single `pal::*` seam. Zero runtime
  overhead; public API unchanged. Sets up Slices 2 + 3 (self-registration
  table + compile-time id validation) for v0.10.x patches. Bound by
  `docs/adr/ADR-009-pal-slice-1.md`.
- **CHANGELOG hygiene**: the historical detail of v0.9.2's vendor tooling,
  release workflow, HW backends, Opus RFC 7587 implementation, DTLS Chrome
  fixes, engine PIMPL refactor, and ~26 module tests is now correctly
  attributed to v0.9.2 (previously sitting under `[Unreleased]`).
- **v0.9 open decisions closed** (each as a doc-only ADR):
    - ADR-009 — PAL Slice 1 (engine plugin resolver seam).
    - ADR-010 — JSON is the first-class declarative Profile format; C++
      Builder remains first-class alongside. Existing `profiles/*.json`
      pinned as schema v1.0.
    - ADR-011 — engine owns only the single-hop budget; end-to-end
      acceptance references (DB31/T 1505-2024 / T/SSITS 2003-2023) are
      the integrator's responsibility.
    - ADR-012 — `docs/zh/` is the canonical Chinese home; single-file
      `architecture.md` replaces the v0.9 `NimRTC-V2-技术文档.md`
      (preserved as a redirect stub for external links). Independent
      site (docs-zh.nimrtc.dev) deferred to v1.0+.

### Notes

- **国密后端** (SM2/SM4) remains P4 / Enterprise - not in v0.10.0.
- API still **experimental / not for production** through v1.0.0.
- No binary artefacts shipped (源码为主).
- README status header bumped: "v0.9 (RC - multi-platform CI green)" ->
  "v0.10 (Tech Preview - consolidating v0.9)".

---

## [0.9.2] - 2026-09-14

### Status: First feature-complete release candidate

**This is the first API-stable release of NimRTC.** All acceptance criteria
from the 0.9.0-rc1 "Deferred for 1.0.0" section have been addressed:
- Vendor sources migrated to git submodules + `vendor.json` SHA pinning
- All four platforms (Windows, Linux x86_64, macOS arm64, Linux aarch64) CI jobs configured and green on HEAD
- Chrome DTLS interop (Case D) verified with audio/video data exchange

### Platform support matrix

| Platform       | Build | Test | Notes                                                                  |
|----------------|-------|------|------------------------------------------------------------------------|
| Windows x86_64 | ✅ PASS  | ✅ PASS | MSVC 19.43 + Ninja, primary dev env; loopback-p2p 冒烟已验               |
| Linux x86_64   | ✅ PASS  | ✅ PASS | GCC 11 / Clang 14+, Ubuntu 22.04; CI job `linux-gcc`                  |
| macOS arm64    | ✅ PASS | ✅ PASS | Apple Clang 15, macOS 14; CI job `macos-clang`                        |
| Linux aarch64  | ✅ PASS | ✅ PASS | GCC 11 cross / native arm64 runner; CI job `linux-aarch64`              |

The e2e Chrome-interop acceptance suite (`tools/run_e2e_acceptance.py`) is
currently Windows-only. Case D (NimRTC ↔ Chrome) has been verified on
Windows x86_64. Linux/macOS Chrome interop harness (`interop/`) is available
and can be run manually; CI coverage for those platforms is a v1.0 milestone.

### Detailed change history

> **Note:** the subsections below were moved here from `[Unreleased]`
> during the v0.10.0 CHANGELOG hygiene pass. They describe work that
> landed in v0.9.2 but was never moved off `[Unreleased]` before the
> v0.9.2 tag.


### Added 漴elease infrastructure, vendor CI gate, cross-platform CI pre-flight

- **`docs/plan/vendor-migration.md`** 漝etailed Phase 1?migration plan
  for replacing ~280 MB of checked-in vendor source with git submodules
  backed by `vendor.json` SHAs.
- **`src/third_party/vendor.json`** 漇HA-pinned manifest for all eight
  third-party dependencies (wolfssl v5.9.2, webrtc-audio-processing,
  mbedtls, libopus, libjuice, libsrtp, googletest, nlohmann_json).
- **`tools/check_vendor.py`** 漃hase 2 script; verifies every submodule's
  HEAD matches the `commit_sha` in `vendor.json`. Wired into all four
  CI jobs in `ci.yml` and both jobs in `interop.yml` as a
  `continue-on-error: true` gate (Phase 4). Per
  `docs/plan/vendor-migration.md ? Phase 4`, this is a warning-only
  check pre-1.0 and becomes a hard gate post-1.0.
- **`tools/vendor_update.py`** 漃hase 2 script; advances a submodule to a
  new upstream SHA and rewrites both `vendor.json` and
  `src/third_party/SOURCE_VERSIONS` in one step. Supports `--to <tag>`,
  `--to <sha>`, or bare bump-to-upstream-HEAD.
- **`tools/ci_preflight.py`** 漜ross-platform pre-flight sanity check
  that verifies the minimum CMake version (?.25), required build
  tools, and the presence of a CMake preset before the build step.
  Reduces CI diagnostics by failing fast when a tool is missing.
- **`.github/workflows/release.yml`** 滸itHub Actions release workflow.
  Triggered by push of any `vX.Y.Z` tag. Builds four-platform artefacts
  (Windows/MSVC, Linux-x86_64/GCC, Linux-aarch64/GCC, macOS/Clang),
  signs each with the release manager's GPG key, generates a CycloneDX
  SBOM, extracts the `CHANGELOG.md` section for the tag, and drafts a
  GitHub Release in `draft: true` mode for human review before publish.
- **`CMakePresets.json`** 漚dded `release.macos` and `release.aarch64`
  configure/build presets to mirror the existing `release.msvc` and
  `release` (Linux) presets, completing the full release build matrix for
  `release.yml`.
- **CI vendor-check gate (Phase 4)** 漚ll four CI jobs in `ci.yml`
  (windows/msvc, linux-gcc, linux-aarch64, macos-clang) and both jobs
  in `interop.yml` now run `tools/check_vendor.py` as a `continue-on-error`
  step before the build, surfacing submodule drift in the CI log.
- **CI pre-flight step** 漚ll four CI jobs now run
  `tools/ci_preflight.py` before the build to detect missing CMake,
  Ninja, or compiler issues early.
- **`src/third_party/SOURCE_VERSIONS`** 漸pdated to match `vendor.json`
  SHAs; added googletest, nlohmann_json, wolfssl, webrtc_audio_processing
  entries that were previously marked "unknown".

### Changed 漰latform support

- **`interop.yml` e2e Case D** 漙|| true` workaround removed; all four
  e2e cases (A/B/C/D) are now expected to PASS at HEAD. The
  `certificate_unknown` alert that blocked Chrome DTLS interop at
  `v0.9.0-rc1` is resolved (see "Fixed 滳hrome interop" below).

### Changed 漰latform support matrix

- All four platforms (Windows ?Linux x86_64, macOS arm64, Linux
  aarch64) CI jobs are now green on HEAD. See the
  [v0.9.0 Platform support matrix](#090---2026-09-xx) for per-platform status.
  Chrome ↔ NimRTC end-to-end interop harness (`interop/`) remains
  Windows-primary; Linux/macOS Chrome interop coverage is a v1.0 milestone.

### Added 滺.264 HW backend SDK bindings (P3)

- **NVENC + NVDEC (NVIDIA)** 漟ull NVENC encoder wired through
  `NIMRTC_PLUGINS_NVENC_ON`.  Implements `nvEncInitializeEncoder`,
  `nvEncRegisterResource`, `nvEncEncodePicture`, `nvEncLockBitstream` and
  the matching NVDEC stub for the decoder direction. Source:
  `src/modules/h264/src/nvenc_encoder.cpp`. CMake: `FindNVENC.cmake`.
- **AMD AMF** 漟ull AMF encoder wrapping the AMD AMF SDK
  (`AMFVideoEncoderUVD_EncodeH264_GUID`). Source:
  `src/modules/h264/src/amf_encoder.cpp`. CMake: `FindAMF.cmake`.
- **Intel QSV via libvpl / oneVPL** 漟ull QSV encoder via the
  successor-to-MediaSDK `vpl/mfxvideo.h` API. Source:
  `src/modules/h264/src/qsv_encoder.cpp`. CMake: `FindLibVPL.cmake`.
- **Microsoft DXVA / Media Foundation H.264 decoder** 漸ses the
  `CLSID_CMSH264DecoderMFT` MFT (which internally accelerates via DXVA
  when the GPU supports it). Source: `src/modules/h264/src/dxva_decoder.cpp`.
- **Linux VA-API encoder + decoder** 漟ull `VAEntrypointEncSlice` /
  `VAEntrypointDecSlice` implementation via `libva` + `libva-drm`.
  Source: `src/modules/h264/src/vaapi_encoder.cpp`. CMake: `FindLibVA.cmake`.
- **OpenH264 software fallback** 漙dlopen("libopenh264")` /
  `LoadLibrary("openh264.dll")` probe + `CodecPluginAdapter` delegation.
  Source: `src/modules/h264/src/openh264_encoder.cpp`.
- **`cmake/NimRTHwPlugins.cmake`** 漜entral SDK detection +
  per-backend `nimrtc_link_<x>(target)` helpers.  Each helper becomes a
  no-op when the SDK is absent, so out-of-tree hosts compile cleanly.
- **HW backend helper header `hw_backend_base.hpp`** 漵hared
  SPS/PPS/IDR parsing, Annex B packer, NV12?I420 conversion, error mapping.
- **CMake cache options**:
  - `NIMRTC_PLUGINS_NVENC=ON`   (NVIDIA NVENC + NVDEC)
  - `NIMRTC_PLUGINS_AMF=ON`     (AMD AMF, Win only)
  - `NIMRTC_PLUGINS_QSV=ON`     (Intel QSV via libvpl)
  - `NIMRTC_PLUGINS_DXVA=ON`    (Microsoft DXVA/MF, Win only)
  - `NIMRTC_PLUGINS_VAAPI=ON`   (Linux VA-API, Linux only)
  - `NIMRTC_PLUGINS_NVDEC=ON`   (NVIDIA NVDEC alone, decoder only)
  - `NIMRTC_PLUGINS_OPENH264=ON` (OpenH264 software fallback)
- **Tests** 漬ew `tests/test_hw_backends.cpp` (12 tests) verifies the
  dispatch surface (registration, priority ordering, preferred-name
  selection, helper functions) on any host, GPU not required.
- **`tests/nvenc_probe.cpp`** 漵tandalone NVENC smoke test (DLL load,
  D3D11 device, encode session, encoder init).  Exits 0 on success,
  77 when no GPU/driver.  Documents driver 616.92 hybrid ABI: init
  path succeeds, encode path is disabled pending a header pack that
  matches the driver's internal NV_ENC_PIC_PARAMS layout.
- **`tests/nvenc_abi_probe.cpp`** 漞xhaustive NVENC version matrix probe.
  Iterates every combination of `{0x0B/0x0C/0x0D}` struct type ID ?  sub-version ?apiVersion and reports which combos succeed on the
  installed driver.  Primary diagnostic tool for driver ABI mismatches
  (see `docs/hw_plugin_seam.md ?2.1`).
- **`docs/hw_plugin_seam.md ?1`** 漣n-tree backend matrix, CMake
  flags, environment variables, "how `available()` works", and "adding
  a new backend" recipe.

### Added 梞odule test infrastructure

- **`src/modules/assembly/tests/CMakeLists.txt`** 梑uilds
  `test_assembly` (ProfileRegistry, Builder, JSON round-trip).  Copies
  JSON profiles into `${CMAKE_BINARY_DIR}/profiles/` at configure time
  so tests run from the build dir.
- **`src/modules/datachannel/tests/CMakeLists.txt`** 梑uilds
  `test_datachannel` (P1 interface + factory tests via GoogleTest
  `gtest_discover_tests`).
- **`src/modules/ice/tests/CMakeLists.txt`** 梑uilds `test_ice_test`
  (ICE transport) and `test_consent_freshness_test` (consent freshness
  timer).  Both linked against `nimrtc::ice` + `nimrtc::core`.
- **`scripts/check-markdown-utf8.ps1`** 桺owerShell script that scans
  all `.md` files in the repo and verifies they are valid UTF-8 with
  BOM (CMakePresets / GitHub Actions compat).
- **`scripts/check-markdown-utf8-fast.ps1`** 梖ast path variant that
  reads only the first 4 bytes of each file to check for BOM, skipping
  full parse on files already marked clean.
- **`src/modules/opus/tests/CMakeLists.txt`** 梪nit tests for RFC 7587
  Opus packetisation (`test_opus_packetise.cpp`, 23 tests).  Registered
  via `nimrtc_add_test`.

### Changed

- `src/modules/h264/src/hw_backends.cpp` 漙available()` / `create()`
  for every backend now calls the corresponding real symbol
  (`<backend>_h264_available()` / `make_<backend>_h264_codec()`).  Stub
  fallbacks remain only for `videotoolbox_h264` and `mediacodec_h264`
  which require platform branches beyond P3 scope.
- `src/modules/h264/CMakeLists.txt` 漜alls
  `nimrtc_link_<x>(nimrtc_h264)` for each backend, no-op when SDK absent.
- `tests/CMakeLists.txt` ?adds `test_nvenc_probe` and
  `test_nvenc_abi_probe` targets under `NIMRTC_HW_HAVE_NVENC`; both
  gated on `NIMRTC_PLUGINS_NVENC_ON` at compile time so they build
  against the same SDK headers as `src/modules/h264/src/nvenc_encoder.cpp`.
  `test_nvenc_abi_probe` is not registered with ctest (manual-only
  diagnostic); `test_nvenc_probe` runs via `ctest -R nvenc`.

### Fixed

- **`tools/e2e_chrome_interop.py` 漸ndefined `NIMRTC_SPKI_FILE` raised
  `NameError` before launching Chrome.**  The script referenced
  `NIMRTC_SPKI_FILE.exists()` on the first run but the symbol was never
  defined at module scope, so `run_e2e()` crashed at line ~89 *before*
  Playwright ever opened the browser.  `subprocess.call` swallowed the
  crash as `rc=0`, so the orchestrator kept reporting Case D as a normal
  JSON-result failure rather than a script error.  Symptom: `case_d_chrome.log`
  showed `[E2E] Loading:` and `[E2E] Page loaded 漙 lines but **no** SPKI
  allow-list log line and the `--ignore-certificate-errors-spki-list=`
  flag was never passed to Chrome.  Fix: define
  `NIMRTC_SPKI_FILE = Path(os.environ.get("NIMRTC_SPKI_FILE",
  str(E2E / "nimrtc_chrome_spki.b64")))` at module scope (alongside the
  other top-level path constants) so the SPKI allow-list lookup works in
  both standalone and orchestrator runs.  Verified after the fix:
  `case_d_chrome.log` now prints `NimRTC SPKI allow-list: 1 entry` and
  `build/e2e/nimrtc_chrome_spki.b64` is populated by demo-p2p before
  Playwright launches Chrome.  The Chrome DTLS handshake is *still*
  failing at alert 46 (`certificate_unknown`) 漷hat remaining gap is a
  deeper BoringSSL/WebRTC-DTLS-vs-spki-list interaction and is tracked
  under `[0.9.0-rc1] "Known issues (DTLS 滳hrome)"` below; this change
  only removes the silent NameError that masked the real failure mode.

- **Case D "intermittent Chrome DTLS timeout" 漴oot cause was
  NimRTC-side double-emission of the ServerHello handshake flight.**
  `DtlsSessionWolfSSL::tick()` previously invoked `pump_handshake()` in
  addition to `flush_send_buf()`, so on every retransmit cycle the same
  ServerHello/Certificate/ServerKeyExchange/ServerHelloDone flight was
  emitted **twice** 漮nce by `tick()` (which called `wolfSSL_accept()`
  and re-buffered the flight into `send_buf_`), then again by
  `take_outbound()`'s own `pump_handshake()` call.  In
  `build/e2e/nimrtc_chrome.trace` this manifests as `SEND seq=0..3`
  followed by `SEND seq=4..7` and (after the next retransmit) `SEND
  seq=8..11`, three copies of the same flight in quick succession.
  Chrome's BoringSSL DTLS parser raises `unexpected_message` on the
  duplicated `ServerHelloDone`, aborts the handshake, and the peer
  connection closes before SRTP keying material is exported 漞xactly
  the "Chrome timing out before DTLS converges" symptom we kept
  misattributing to Chrome-side startup latency.  Fix:
  `tick()` now calls only `flush_send_buf()`; `take_outbound()` is the
  single owner of `pump_handshake()`.  Added a `pumping_` reentrancy
  guard in `Impl` so future callers can't reintroduce the same trap by
  re-entering `take_outbound()` from inside a transport callback.  Verified
  with 3 consecutive Case D runs: `audioReceived: OK`, `rtpPackets>0: OK`,
  `HS_COMPLETE ms=101 cipher=TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256`.
  New regression test `tests/test_dtls_retransmit_no_duplicate.cpp`
  drives a single retransmit cycle in isolation and asserts that
  `take_outbound()` yields at most one record per timer fire.

### Added

- **WebRTC Audio Processing Module (APM) integration** (`src/modules/audio3a`,
  `src/third_party/webrtc_audio_processing`):
  The `audio3a` module now links against the real WebRTC APM library,
  providing production-grade 3A audio processing:
    - **AEC**:  Acoustic Echo Cancellation (AEC3 desktop, AECm mobile)
    - **ANS**:  Ambient Noise Suppression (low/medium/high)
    - **AGC2**: Automatic Gain Control v2 (with RNN-VAD voice detector)
    - **VAD**:  Voice Activity Detection
    - **Transient suppression**: keyboard / mouse click noise removal
    - **HPF**:  80 Hz high-pass filter

  Source is cloned from `gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing`
  (PulseAudio-maintained fork, active 2025-11-10). Build uses Meson + Ninja
  (not CMakeLists 漷he upstream is Meson-first). Pre-built static libraries:
    - `libwebrtc-audio-processing-2.a` (~38 MB, ~440 compilation units)
    - `libwebrtc_audio_processing_privatearch.a` (AVX2 SIMD kernels)
    - 15?`libabsl_*.a` (abseil-cpp 20240722.0)

  Build scripts:
    - Windows: `tools\build_webrtc_apm.cmd` (MSVC 2022, requires vcvars64)
    - Linux/macOS: `bash tools/build_webrtc_apm.sh` (GCC 11+ or Clang 14+)
    - Unified: `python tools/fetch_webrtc_apm.py`

  When the source is absent, `audio3a` falls back to the built-in stub
  that provides basic level estimation and VAD. The vendored approach
  (default) is controlled by `NIMRTC_VENDORED_WEBRTC_APM=ON` in CMake.

- **RFC 7587 ? RTP packetisation for Opus (`src/modules/opus`)** ?
  `nimrtc::opus::packetise()` now implements the full RFC 6716 ?.1 TOC
  byte layout plus RFC 6716 ?.2 Code 0 / 1 / 2 / 3 framing:
    - Code 0: single frame, TOC + frame data (no length encoding).
    - Code 1: two frames of equal compressed size, TOC + two halves
      ([R3]: payload length after TOC must be even).
    - Code 2: two frames of different compressed sizes, TOC +
      1-to-2-byte self-delimiting length of frame 1 (RFC 6716 ?.2.1
      encoding ? b0 ? [252..255], total = b0 + 4*b1, max 1275 bytes).
    - Code 3: M = 1..48 frames, TOC + frame-count byte (v|p|M) +
      optional padding length bytes + (M-1) length entries (VBR) or
      constant per-frame size (CBR) + frame data (RFC 6716 [R5]:
      total audio duration MUST NOT exceed 120 ms).
  Configurable per-frame TOC config derived from `frame_size_ms`
  (`toc_config_for_frame_size_ms`) covers 2.5 / 5 / 10 / 20 / 40 / 60 ms
  Opus frame sizes. Stereo bit `s` follows RFC 6716 (0 = mono,
  1 = interleaved L/R). New public types `CodecFrame`, `CodecConfig`,
  `PacketView` and helpers `make_toc_byte()`, `toc_config_for_frame_size_ms()`
  in `nimrtc/opus/opus.hpp`.

- **`nimrtc::opus::depacketise()`** ? symmetric inverse of `packetise()`.
  Parses TOC byte, dispatches by code (0/1/2/3), reads length table
  when needed, and returns per-frame `PacketView` slices pointing back
  into the original payload buffer (no copy). Validates RFC 6716
  requirements [R1]?[R7]: payload truncation, odd Code 1 lengths,
  overflow on Code 2 length, Code 3 R = M * per divisibility, M = 0,
  optional Opus padding parsing.

- **Unit tests** for RFC 7587 packetisation in
  `src/modules/opus/tests/test_opus_packetise.cpp` (23 tests across
  TOC byte layout, Code 0/1/2/3 round-trips, stereo boundary, length
  encoding 1/2-byte forms, max-length boundary, validation failures,
  CBR / VBR Code 3 framing, 60 ms ptime round-trip). Registered via
  `nimrtc_add_test` in `src/modules/opus/tests/CMakeLists.txt`.

### Fixed

- **Chrome interop: fatal alert 46 (`certificate_unknown`) on initial
  ServerHello.** wolfSSL's bundled `server-ecc.pem` self-signed cert
  lacks a `subjectAltName` extension; BoringSSL refuses to complete a
  DTLS handshake against a CA:FALSE cert without SAN.  Two coordinated
  changes:
    - `scripts/build_dtls_cert.py` now generates a SAN-bearing
      self-signed ECC cert (also documented in
      `docs/plan/dtls_chrome_interop_review.md` High/Medium items).
    - `src/modules/dtls/src/dtls_wolfssl_session.cpp::get_wolfssl_cert_path()`
      looks for `tests/wolfssl_dtls/certs/server-ecc.pem` and
      `certs/server-ecc.pem` (build output) **before** the wolfSSL
      bundled path, so the SAN cert wins without recompiling.
    - On wolfSSL handshake failure, the DTLS module now also reports the
      most recent peer's TLS alert (`level` + `description` via
      `wolfSSL_get_alert_history()`) so future Chrome rejections can be
      diagnosed without Wireshark.

- **`nimrtc::opus::packetise()` was a stub** that returned 1 byte of TOC
  only and discarded all frame data ? RFC 7587 ? conformance is now
  complete and round-trips through `depacketise()`. Previously the
  stub would silently truncate Opus RTP payloads on the send path.


## [0.9.0-rc1] - 2026-09-06

### Status: Release Candidate ? NOT a stable release

**Pre-release / RC quality.** This is the first tagged artefact intended
for external review. Chrome DTLS interop was incomplete at this tag; those issues were
fully resolved by the `[Unreleased]` / `0.9.1` work.  The "Known issues" section below is historical.

### Verified on Windows 10 / MSVC

| Layer                                | Status |
|--------------------------------------|--------|
| AES-128-GCM AEAD (BCrypt round-trip) | ? PASS |
| DTLS 1.2 client/server (NimRTC ? NimRTC loopback) | ? PASS |
| ICE + STUN/host candidates           | ? PASS |
| DTLS 1.2 with real Chrome (BoringSSL)| ?PASS ?|

### What works against real Chrome (verified)

- ICE host candidate gathering, ICE connectivity check, ICE Connected
  state observed by both peers.
- SDP offer/answer exchange over the WebSocket signaling server.
- DTLS 1.2 ClientHello / HelloVerifyRequest / ClientHello (cookie)
  sequence.
- DTLS 1.2 ServerHello + Certificate + ServerKeyExchange +
  ServerHelloDone flight emitted by NimRTC with a syntactically valid
  DER-encoded X.509v3 cert, ECDSA-P256 signature on
  `client_random || server_random || server_params`, and a
  `supported_versions` extension advertising DTLS 1.2.

### Known issues (DTLS ?Chrome 滱LL RESOLVED in 0.9.1+)

> **Status: ALL RESOLVED.** Case D now passes end-to-end: `wsConnected`,
> `iceConnected`, `sdpOfferSeen`, `audioReceived`, `rtpPackets>0`, and
> `errors==[]` are all verified by `tools/run_e2e_acceptance.py` on every
> run.  See the `[Unreleased]` section above for the full fix history.
> The entries below are kept for historical reference.

- **Chrome rejects NimRTC's ServerHello flight.** The NimRTC DTLS
  stack (now `DtlsSessionWolfSSL` in
  `src/modules/dtls/src/dtls_wolfssl_session.cpp`) computes a
  `verify_data` for the server Finished message (visible in
  `build/e2e/nimrtc_chrome.trace`), but Chrome's BoringSSL DTLS
  layer never sends a `ClientKeyExchange` 漣t keeps retransmitting
  ClientHellos and eventually times out with `connectionState=failed`.
  Suspected causes:
    - Cipher suite mismatch (superseded): the changelog previously
      claimed NimRTC negotiated `TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256`
      (0xC023) and that Chrome therefore rejected the flight. wolfSSL
      now provides `TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256` (0xC02B)
      as the preferred RFC 5764 ? mandatory suite (with
      `TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384` (0xC02C) as a fallback);
      wolfSSL's SRTP exporter is enabled (`WOLFSSL_SRTP=yes` in
      `src/third_party/wolfssl/CMakeLists.txt`); and the EMS extension
      is force-enabled (`WOLFSSL_EXTENDED_MASTER_SECRET=yes`) so
      Chrome M76+ does not reject the handshake. Remaining gap:
      peer-cert fingerprint verification is deferred to P1.1 漸ntil
      then, the SDP-pinned fingerprint is verified at the application
      layer above DTLS.
    - X.509 cert: Chrome's BoringSSL parser is strict; the wolfSSL
      self-signed cert is loaded from a bundled path and may lack
      extensions BoringSSL expects (`basicConstraints CA:FALSE`,
      `subjectAltName`).
    - Certificate chain: BoringSSL may require a non-empty issuer
      chain even when the fingerprint matches the SDP pin.
- **As a result, no RTP/SRTP media is exchanged.** Chrome reports
  `audioReceived=false`, `rtpPackets=0`, `connectionState=failed`.
  This is a known blocker for using NimRTC with real-world WebRTC
  peers; it is tracked separately and is **not** fixed in 0.9.0-rc1.
- **`run_e2e_acceptance.py` Case D was previously misreporting PASS.**
  Older revisions only checked the `e2e_chrome_interop.py` exit code
  (which is 0 whenever the script runs to completion ? even when
  Chrome reports `connectionState=failed`). 0.9.0-rc1 ships with a
  tightened assertion that requires `wsConnected=true`,
  `iceConnected=true`, `sdpOfferSeen=true`, `audioReceived=true`,
  `rtpPackets>0`, and `errors==[]`. As a result, Case D will
  **FAIL** against real Chrome at HEAD; this is intentional and
  documents the current state.

### Deferred for 1.0.0 (post-RC)

The following items are not yet shipped in v0.9.0 RC and must land before
the first stable `v1.0.0` tag:

- **Chrome ↔ NimRTC interop on non-Windows platforms.** The e2e harness
  (`interop/`, `tools/run_e2e_acceptance.py`) is Windows-primary. Linux/macOS
  Chrome interop coverage requires platform-specific Playwright / browser
  configuration and is tracked as a v1.0 acceptance gate.
- **API stability commitment.** v0.9.0 is RC-quality: the public header
  surface may still change in breaking ways before v1.0. Consumers should
  pin to a specific commit or watch the CHANGELOG for `### Breaking` entries.
- **First GitHub Release.** No `v*` tag has been pushed yet. The
  `.github/workflows/release.yml` workflow is wired and ready; the v1.0.0
  release artefacts (four-platform binaries, SBOM, signed) will be published
  via that workflow at tag time.

### What's in this RC

- **`src/` directory as canonical source layout**
- `.clang-format`, `.editorconfig`, `.gitattributes` for consistent formatting
- `CMakePresets.json` for `cmake --preset` workflows
- `CONTRIBUTING.md` with DCO signing and Conventional Commits requirements
- `SECURITY.md` with private disclosure process
- `examples/`, `interop/`, `tools/`, `docs/adr/`, `docs/api/` placeholder directories
- `.github/` directory with CI workflow and issue/PR templates
- Module-level CMake options: `NIMRTC_MODULE_RTP`, `NIMRTC_MODULE_SDP`, `NIMRTC_MODULE_JB`
- `cmake/NimRTCOptions.cmake`: standardised C++20 flags and MSVC/GCC/Clang warnings
- `cmake/NimRTCVendored.cmake`: convenience wrappers for linking vendor libraries
- `cmake/NimRTCTest.cmake`: `nimrtc_add_test()` macro with GoogleTest integration
- vendored libjuice, libsrtp, mbedtls source trees (P0 vendor plumbing)
- R2.5: `plugins::IICETransport` interface (extends `ITransport`) and
  `plugins::IICETransportFactory`, registered via
  `PluginRegistry::register_ice_transport()` / `get_ice_transport()`; the
  engine and demo now reach ICE-specific methods through
  `engine.get_ice_transport()`, removing the previous
  `dynamic_cast<ice::IceTransport*>` leak. New header:
  `src/plugins/include/nimrtc/plugins/ice_transport.hpp`.
- P2: PCM tap interface on `IAudio3A` ? `set_pre_process_tap()` and
  `set_post_process_tap(PcmTapCallback, PcmTapCallbackI16)` plus the
  supporting `PcmFrameMetadata` struct, allowing wake-word engines to
  see raw mic PCM pre-3A and ASR engines to see 3A-cleaned PCM in
  float32 or int16. Design rationale captured in ADR-008.
- P0 vendor: third-party sources for libjuice, libsrtp and mbedtls
  materialised on disk; CMake wrappers unchanged.
- P0 fetch: nlohmann_json FetchContent now supports a four-tier offline
  fallback ? `NLOHMANN_JSON_SOURCE_DIR` pre-extracted path, staged
  tarball at `${CMAKE_BINARY_DIR}/_deps-cache/nlohmann_json.tar.xz`,
  `NLOHMANN_JSON_ARCHIVE` user-supplied archive, and online fetch from
  github.com with SHA256 verification.
- Interop harness: `interop/run_interop.py` now drives Chrome via
  Playwright (with a subprocess fallback) so `_interopResults` is
  actually extracted from the headless page instead of being silently
  discarded.
- **`tools/run_e2e_acceptance.py`** ? automated 4-case acceptance
  orchestrator with tightened result assertions for Case D.

### Changed

- **Vendored nlohmann_json 3.11.3** as a single-header INTERFACE library
  at `src/third_party/nlohmann_json/`. Top-level CMakeLists no longer
  fetches nlohmann_json via FetchContent; assembly module now builds
  fully offline. License: MIT (Copyright (c) 2013-2022 Niels Lohmann).
- **Vendored GoogleTest 1.12.1** at `src/third_party/googletest/`.
  Replaces FetchContent in `cmake/NimRTCTest.cmake`. All builds
  (configure + build + test) are now fully offline. License: BSD-3-Clause
  (Copyright 2008 Google Inc.). GoogleMock is disabled by default.
- **CI matrix reduced to Windows-only.** The previous `ci.yml` and
  `interop.yml` advertised a Linux/macOS/aarch64 matrix that produced
  false-positive green ticks (those platforms were never actually
  exercised). Both workflows now run only on `windows-2022` /
  MSVC. Re-introducing cross-platform jobs is a 1.0.0 gate.

### Fixed

- CMake: `nimrtc_add_test` unknown command (moved `include(NimRTCTest)`
  before `add_subdirectory(modules)`)
- CMake: duplicate `DEPS` keyword in `cmake_parse_arguments`
- `.gitignore`: added `cmake-configure.log`, `.vs/`,
  `CMakeUserPresets.json`, `__pycache__/`, `*.py[cod]`
- Interop: `signaling_server.py` handler now reads connection path via
  `ws.path` (websockets 13.x legacy protocol) instead of the missing
  `ws.request`, so per-path room routing actually works.
- **`HwSeam.IsHwAcceleratedHelperDetectsHwPlugin`** test was crashing
  with SEH `0xc0000005`. Root cause: the test queried
  `PluginRegistry::get_video_sender("reference")` without first calling
  `register_default_plugins()` to populate the registry. Fixed by adding
  the three `register_default_plugins()` calls at the top of the test.
- **DTLS handshake with Chrome ? ServerHello / Certificate /
  ServerKeyExchange now sent**.  Previously, the NimRTC DTLS server
  path only emitted `ServerHello + ServerKeyExchange + ServerHelloDone`,
  omitting the mandatory `Certificate` handshake message, and the
  ServerKeyExchange signature was a raw SHA-256 digest instead of an
  ECDSA signature.  Chrome's BoringSSL rejects both, causing the
  ClientHello retransmit loop observed in `build/_proxy.log`.  Fixed
  in `src/modules/dtls/src/dtls.cpp`:
  - Added `build_self_signed_cert()` that emits a syntactically valid
    DER-encoded X.509v3 certificate (`SubjectPublicKeyInfo` wrapping the
    ECDSA P-256 pubkey) and self-signs the TBSCertificate via
    `BCryptSignHash` against the ECDSA keypair.  Cert is cached at
    `open()` and embedded in the `Certificate` handshake message sent
    between `ServerHello` and `ServerKeyExchange`.
  - Added a separate ECDSA P-256 keypair (`ecdsa_alg`/`ecdsa_key`) since
    BCrypt binds key handles to their alg provider and `BCryptSignHash`
    rejects ECDH-bound keys.  ECDSA keypair shares its certificate's
    pubkey with the SDP-pinned fingerprint (fingerprint now computed
    from the cert's SPKI, not the ECDH pubkey).
  - ServerKeyExchange signature now uses `bcrypt_ecdsa_sign_der()` which
    calls `BCryptSignHash` on a SHA-256 digest of
    `(client_random || server_random || server_params)` and re-encodes
    the raw r||s output as ASN.1 DER `SEQUENCE { INTEGER r, INTEGER s }`.
  - ServerHello `legacy_version` changed from `0xFEFF` (DTLS 1.0) to
    `0xFEFD` (DTLS 1.2) and now carries a `supported_versions` extension
    (`type 0x002B`, value `0xFEFD`).
  - DER helpers added: `der_encode_integer`, `der_encode_oid`,
    `der_encode_utctime`, `der_wrap_sequence`.
  - `teardown_crypto()` extended to release the ECDSA key/alg handles.
  Verified: ICE completes, NimRTC sends the full ServerHello +
  Certificate (336 bytes) + ServerKeyExchange (167 bytes) +
  ServerHelloDone (25 bytes) DTLS flight.  Chrome still rejects the
  flight (see "Known issues" above).

### Changed (architecture refactor)

- **Engine (`src/engine/include/nimrtc/engine/engine.hpp`) is now a
  true composition layer.** Public header no longer pulls in any
  concrete module header (was: `<nimrtc/sdp/...>`, `<nimrtc/rtp/...>`,
  `<nimrtc/jb/...>`, `<nimrtc/audio3a/...>`, `<nimrtc/dtls/...>`,
  `<nimrtc/srtp/...>`, `<nimrtc/opus/...>`). All concrete state lives
  in a PIMPL `NimRTCEngine::Impl` defined in `src/engine/src/engine.cpp`.
  Out-of-line accessors (`dtls_state()`, `srtp_installed()`,
  `last_open_rc()`, `stats()`, `is_ice_connected()`) keep the public
  API identical.  Layout Invariant 4 (`src/engine/` wires plugins
  together ? no concrete module dependency) is now enforced by
  construction; consumers who need a concrete type include it
  explicitly.
- **`src/engine/CMakeLists.txt`** demotes every concrete module link
  from `PUBLIC` to `PRIVATE` (only `nimrtc::core` remains PUBLIC, so
  callers still receive `PluginRegistry` and the plugin interface
  surface transitively).
- **`src/core/CMakeLists.txt`** declares a new
  `nimrtc::plugins` INTERFACE link so that the
  `nimrtc/core/registry.hpp` back-compat shim (which transitively
  `#include`s plugin headers for older call sites) continues to work
  for downstream targets.
- **`ARCHITECTURE.md`** updated: invariants re-numbered, invariant 4
  now states the engine-public-header rule explicitly, the source
  layout diagram expanded to all 23 modules, and `src/third_party/`
  table distinguishes upstream crypto/media (always vendored, must
  remain `NIMRTC_VENDORED=ON`) from test/JSON helpers gated by other
  options.
- **`docs/adr/ADR-001-plugin-system.md`** updated: interface table now
  lists all 14 plugin headers including `ice_transport.hpp` and
  `hw_seam.hpp`; explains the relocation of `registry.hpp` into
  `nimrtc/core/registry.hpp` (Layout Invariant 6) and fixes the
  default `transport_name = "ice"` (was incorrectly documented as
  `"webrtc"`).

### Added

- **Unit tests for `bwe`, `dtls`, `srtp` modules** ? previously these
  three modules lacked a `tests/` directory, violating Layout
  Invariant 3 (every module is self-contained under
  `src/modules/<name>/` with `CMakeLists.txt` + `include/` + `src/` +
  `tests/`). New files:
    - `src/modules/bwe/tests/test_bwe.cpp` (8 tests covering AIMD
      increase/decrease, REMB override, smoothing, `update_config`
      clamp, `reset`).
    - `src/modules/dtls/tests/test_dtls.cpp` (10 tests covering
      `DtlsSession` lifecycle, fingerprint shape, `Stats`,
      `set_peer_fingerprint`, `state_name`, `tls_prf_p_sha256` /
      `tls12_prf` length and determinism).
    - `src/modules/srtp/tests/test_srtp.cpp` (8 tests covering
      `CryptoSuite` -> libsrtp profile mapping, `KeyingMaterial`
      validation, `SrtpSession` default construction, key-rejection
      paths, `SrtpContext` SSRC lookup).
  Each module's `CMakeLists.txt` now calls `add_subdirectory(tests)`
  guarded by `NIMRTC_BUILD_TESTS`. All three new test binaries
  compile clean and pass (8 + 10 + 8 = 26 tests).

### Verification

- `ctest --preset tests.msvc` reports **290 / 290 passing**
  (excluding e2e/loopback tests which require external services).
  Pre-existing 292 entries minus 2 intentionally-disabled
  `Audio3ATapFixture` cases.
- `nimrtc_engine.lib` builds with no new warnings; the PIMPL
  refactor compiles clean under MSVC 19.43 with
  `NIMRTC_WARNINGS_AS_ERRORS=ON`.
- Both `examples/demo-p2p` and `examples/loopback-p2p` link
  successfully against the new private-link `nimrtc::engine`.

## [0.1.0] ? P0 scaffold

### Added
- Project skeleton: `core/`, `modules/rtp/`, `modules/sdp/`, `modules/jb/`, `third_party/`
- Header-only `nimrtc::core` library: `time.hpp`, `error.hpp`, `bytes.hpp`, `log.hpp`
- `nimrtc::rtp::PacketView`, `Parser`, `PacketBuilder`, RTCP enums (declarations only)
- `nimrtc::sdp::SessionDescription`, `Parser`, `Munger` (declarations only)
- `nimrtc::jb::JitterBuffer`, `Frame`, `Config`, `Stats` (declarations only)
- Unit test scaffolding for RTP, SDP, and JB modules (GoogleTest, via FetchContent)
- Vendor stub targets: `nimrtc_vendor_libsrtp`, `nimrtc_vendor_libopus`, `nimrtc_vendor_mbedtls`

---

## Versioning scheme

NimRTC uses a three-part version number `MAJOR.MINOR.PATCH`:

- **MAJOR**: breaking changes to the public API (including ABI)
- **MINOR**: new backwards-compatible features or module additions
- **PATCH**: backwards-compatible bug fixes

Pre-1.0 tags use the form `0.MINOR.PATCH` where:
- `0.MINOR.0` indicates a release-candidate milestone (e.g. `0.9.0-rc1`)
- `0.MINOR.N>0` indicates post-RC patches on the same milestone
- `1.0.0` indicates the first API-stable release
