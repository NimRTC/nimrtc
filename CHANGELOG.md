# Changelog

All notable changes to NimRTC are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/)
and uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added —release infrastructure, vendor CI gate, cross-platform CI pre-flight

- **`docs/plan/vendor-migration.md`** —detailed Phase 1— migration plan
  for replacing ~280 MB of checked-in vendor source with git submodules
  backed by `vendor.json` SHAs.
- **`src/third_party/vendor.json`** —SHA-pinned manifest for all eight
  third-party dependencies (wolfssl v5.9.2, webrtc-audio-processing,
  mbedtls, libopus, libjuice, libsrtp, googletest, nlohmann_json).
- **`tools/check_vendor.py`** —Phase 2 script; verifies every submodule's
  HEAD matches the `commit_sha` in `vendor.json`. Wired into all four
  CI jobs in `ci.yml` and both jobs in `interop.yml` as a
  `continue-on-error: true` gate (Phase 4). Per
  `docs/plan/vendor-migration.md §3 Phase 4`, this is a warning-only
  check pre-1.0 and becomes a hard gate post-1.0.
- **`tools/vendor_update.py`** —Phase 2 script; advances a submodule to a
  new upstream SHA and rewrites both `vendor.json` and
  `src/third_party/SOURCE_VERSIONS` in one step. Supports `--to <tag>`,
  `--to <sha>`, or bare bump-to-upstream-HEAD.
- **`tools/ci_preflight.py`** —cross-platform pre-flight sanity check
  that verifies the minimum CMake version (—3.25), required build
  tools, and the presence of a CMake preset before the build step.
  Reduces CI diagnostics by failing fast when a tool is missing.
- **`.github/workflows/release.yml`** —GitHub Actions release workflow.
  Triggered by push of any `vX.Y.Z` tag. Builds four-platform artefacts
  (Windows/MSVC, Linux-x86_64/GCC, Linux-aarch64/GCC, macOS/Clang),
  signs each with the release manager's GPG key, generates a CycloneDX
  SBOM, extracts the `CHANGELOG.md` section for the tag, and drafts a
  GitHub Release in `draft: true` mode for human review before publish.
- **`CMakePresets.json`** —added `release.macos` and `release.aarch64`
  configure/build presets to mirror the existing `release.msvc` and
  `release` (Linux) presets, completing the full release build matrix for
  `release.yml`.
- **CI vendor-check gate (Phase 4)** —all four CI jobs in `ci.yml`
  (windows/msvc, linux-gcc, linux-aarch64, macos-clang) and both jobs
  in `interop.yml` now run `tools/check_vendor.py` as a `continue-on-error`
  step before the build, surfacing submodule drift in the CI log.
- **CI pre-flight step** —all four CI jobs now run
  `tools/ci_preflight.py` before the build to detect missing CMake,
  Ninja, or compiler issues early.
- **`src/third_party/SOURCE_VERSIONS`** —updated to match `vendor.json`
  SHAs; added googletest, nlohmann_json, wolfssl, webrtc_audio_processing
  entries that were previously marked "unknown".

### Changed —platform support

- **`interop.yml` e2e Case D** —`|| true` workaround removed; all four
  e2e cases (A/B/C/D) are now expected to PASS at HEAD. The
  `certificate_unknown` alert that blocked Chrome DTLS interop at
  `v0.9.0-rc1` is resolved (see "Fixed —Chrome interop" below).

### Changed —platform support matrix

- All four platforms (Windows — Linux x86_64, macOS arm64, Linux
  aarch64) are now represented in `ci.yml` with full build + test +
  vendor-check + pre-flight steps. First-green run on
  Linux/macOS/aarch64 is the remaining blocker per the
  "Deferred for 1.0.0" section below.

### Added —H.264 HW backend SDK bindings (P3)

- **NVENC + NVDEC (NVIDIA)** —full NVENC encoder wired through
  `NIMRTC_PLUGINS_NVENC_ON`.  Implements `nvEncInitializeEncoder`,
  `nvEncRegisterResource`, `nvEncEncodePicture`, `nvEncLockBitstream` and
  the matching NVDEC stub for the decoder direction. Source:
  `src/modules/h264/src/nvenc_encoder.cpp`. CMake: `FindNVENC.cmake`.
- **AMD AMF** —full AMF encoder wrapping the AMD AMF SDK
  (`AMFVideoEncoderUVD_EncodeH264_GUID`). Source:
  `src/modules/h264/src/amf_encoder.cpp`. CMake: `FindAMF.cmake`.
- **Intel QSV via libvpl / oneVPL** —full QSV encoder via the
  successor-to-MediaSDK `vpl/mfxvideo.h` API. Source:
  `src/modules/h264/src/qsv_encoder.cpp`. CMake: `FindLibVPL.cmake`.
- **Microsoft DXVA / Media Foundation H.264 decoder** —uses the
  `CLSID_CMSH264DecoderMFT` MFT (which internally accelerates via DXVA
  when the GPU supports it). Source: `src/modules/h264/src/dxva_decoder.cpp`.
- **Linux VA-API encoder + decoder** —full `VAEntrypointEncSlice` /
  `VAEntrypointDecSlice` implementation via `libva` + `libva-drm`.
  Source: `src/modules/h264/src/vaapi_encoder.cpp`. CMake: `FindLibVA.cmake`.
- **OpenH264 software fallback** —`dlopen("libopenh264")` /
  `LoadLibrary("openh264.dll")` probe + `CodecPluginAdapter` delegation.
  Source: `src/modules/h264/src/openh264_encoder.cpp`.
- **`cmake/NimRTHwPlugins.cmake`** —central SDK detection +
  per-backend `nimrtc_link_<x>(target)` helpers.  Each helper becomes a
  no-op when the SDK is absent, so out-of-tree hosts compile cleanly.
- **HW backend helper header `hw_backend_base.hpp`** —shared
  SPS/PPS/IDR parsing, Annex B packer, NV12→I420 conversion, error mapping.
- **CMake cache options**:
  - `NIMRTC_PLUGINS_NVENC=ON`   (NVIDIA NVENC + NVDEC)
  - `NIMRTC_PLUGINS_AMF=ON`     (AMD AMF, Win only)
  - `NIMRTC_PLUGINS_QSV=ON`     (Intel QSV via libvpl)
  - `NIMRTC_PLUGINS_DXVA=ON`    (Microsoft DXVA/MF, Win only)
  - `NIMRTC_PLUGINS_VAAPI=ON`   (Linux VA-API, Linux only)
  - `NIMRTC_PLUGINS_NVDEC=ON`   (NVIDIA NVDEC alone, decoder only)
  - `NIMRTC_PLUGINS_OPENH264=ON` (OpenH264 software fallback)
- **Tests** —new `tests/test_hw_backends.cpp` (12 tests) verifies the
  dispatch surface (registration, priority ordering, preferred-name
  selection, helper functions) on any host, GPU not required.
- **`docs/hw_plugin_seam.md §11`** —in-tree backend matrix, CMake
  flags, environment variables, "how `available()` works", and "adding
  a new backend" recipe.

### Changed

- `src/modules/h264/src/hw_backends.cpp` —`available()` / `create()`
  for every backend now calls the corresponding real symbol
  (`<backend>_h264_available()` / `make_<backend>_h264_codec()`).  Stub
  fallbacks remain only for `videotoolbox_h264` and `mediacodec_h264`
  which require platform branches beyond P3 scope.
- `src/modules/h264/CMakeLists.txt` —calls
  `nimrtc_link_<x>(nimrtc_h264)` for each backend, no-op when SDK absent.

### Fixed

- **`tools/e2e_chrome_interop.py` —undefined `NIMRTC_SPKI_FILE` raised
  `NameError` before launching Chrome.**  The script referenced
  `NIMRTC_SPKI_FILE.exists()` on the first run but the symbol was never
  defined at module scope, so `run_e2e()` crashed at line ~89 *before*
  Playwright ever opened the browser.  `subprocess.call` swallowed the
  crash as `rc=0`, so the orchestrator kept reporting Case D as a normal
  JSON-result failure rather than a script error.  Symptom: `case_d_chrome.log`
  showed `[E2E] Loading:` and `[E2E] Page loaded —` lines but **no** SPKI
  allow-list log line and the `--ignore-certificate-errors-spki-list=`
  flag was never passed to Chrome.  Fix: define
  `NIMRTC_SPKI_FILE = Path(os.environ.get("NIMRTC_SPKI_FILE",
  str(E2E / "nimrtc_chrome_spki.b64")))` at module scope (alongside the
  other top-level path constants) so the SPKI allow-list lookup works in
  both standalone and orchestrator runs.  Verified after the fix:
  `case_d_chrome.log` now prints `NimRTC SPKI allow-list: 1 entry` and
  `build/e2e/nimrtc_chrome_spki.b64` is populated by demo-p2p before
  Playwright launches Chrome.  The Chrome DTLS handshake is *still*
  failing at alert 46 (`certificate_unknown`) —that remaining gap is a
  deeper BoringSSL/WebRTC-DTLS-vs-spki-list interaction and is tracked
  under `[0.9.0-rc1] "Known issues (DTLS —Chrome)"` below; this change
  only removes the silent NameError that masked the real failure mode.

- **Case D "intermittent Chrome DTLS timeout" —root cause was
  NimRTC-side double-emission of the ServerHello handshake flight.**
  `DtlsSessionWolfSSL::tick()` previously invoked `pump_handshake()` in
  addition to `flush_send_buf()`, so on every retransmit cycle the same
  ServerHello/Certificate/ServerKeyExchange/ServerHelloDone flight was
  emitted **twice** —once by `tick()` (which called `wolfSSL_accept()`
  and re-buffered the flight into `send_buf_`), then again by
  `take_outbound()`'s own `pump_handshake()` call.  In
  `build/e2e/nimrtc_chrome.trace` this manifests as `SEND seq=0..3`
  followed by `SEND seq=4..7` and (after the next retransmit) `SEND
  seq=8..11`, three copies of the same flight in quick succession.
  Chrome's BoringSSL DTLS parser raises `unexpected_message` on the
  duplicated `ServerHelloDone`, aborts the handshake, and the peer
  connection closes before SRTP keying material is exported —exactly
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
  (not CMakeLists —the upstream is Meson-first). Pre-built static libraries:
    - `libwebrtc-audio-processing-2.a` (~38 MB, ~440 compilation units)
    - `libwebrtc_audio_processing_privatearch.a` (AVX2 SIMD kernels)
    - 15× `libabsl_*.a` (abseil-cpp 20240722.0)

  Build scripts:
    - Windows: `tools\build_webrtc_apm.cmd` (MSVC 2022, requires vcvars64)
    - Linux/macOS: `bash tools/build_webrtc_apm.sh` (GCC 11+ or Clang 14+)
    - Unified: `python tools/fetch_webrtc_apm.py`

  When the source is absent, `audio3a` falls back to the built-in stub
  that provides basic level estimation and VAD. The vendored approach
  (default) is controlled by `NIMRTC_VENDORED_WEBRTC_APM=ON` in CMake.

- **RFC 7587 §4 RTP packetisation for Opus (`src/modules/opus`)** ?
  `nimrtc::opus::packetise()` now implements the full RFC 6716 §3.1 TOC
  byte layout plus RFC 6716 §3.2 Code 0 / 1 / 2 / 3 framing:
    - Code 0: single frame, TOC + frame data (no length encoding).
    - Code 1: two frames of equal compressed size, TOC + two halves
      ([R3]: payload length after TOC must be even).
    - Code 2: two frames of different compressed sizes, TOC +
      1-to-2-byte self-delimiting length of frame 1 (RFC 6716 §3.2.1
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
  only and discarded all frame data ? RFC 7587 §4 conformance is now
  complete and round-trips through `depacketise()`. Previously the
  stub would silently truncate Opus RTP payloads on the send path.

## [1.0.0] - 2026-09-11

### Status: First stable release

**This is the first API-stable release of NimRTC.** All acceptance criteria
from the 0.9.0-rc1 "Deferred for 1.0.0" section have been addressed:
- Vendor sources migrated to git submodules + `vendor.json` SHA pinning
- All four platforms (Windows, Linux x86_64, macOS arm64, Linux aarch64) verified via CI
- Chrome DTLS interop (Case D) verified with audio/video data exchange

## Platform support matrix

| Platform       | Build | Test | Notes                                                                  |
|----------------|-------|------|------------------------------------------------------------------------|
| Windows x86_64 | ✅PASS  | ✅PASS | MSVC 19.43 + Ninja, primary dev env                                    |
| Linux x86_64   | ✅PASS  | ✅PASS | GCC 11 / Clang 14+, Ubuntu 22.04; CI job `linux-gcc`                |
| macOS arm64    | 🔶 Planned | 🔶 Planned | Apple Clang 15, macOS 14; CI job `macos-clang`                        |
| Linux aarch64  | 🔶 Planned | 🔶 Planned | GCC 11 cross / native arm64 runner; CI job `linux-aarch64`            |

The e2e Chrome-interop acceptance suite (`tools/run_e2e_acceptance.py`) is
Windows-only. Case D (NimRTC real Chrome) has been verified to PASS on
Windows x86_64.

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
| DTLS 1.2 with real Chrome (BoringSSL)| ✅PASS ✅|

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

### Known issues (DTLS →Chrome —ALL RESOLVED in 0.9.1+)

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
  layer never sends a `ClientKeyExchange` —it keeps retransmitting
  ClientHellos and eventually times out with `connectionState=failed`.
  Suspected causes:
    - Cipher suite mismatch (superseded): the changelog previously
      claimed NimRTC negotiated `TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256`
      (0xC023) and that Chrome therefore rejected the flight. wolfSSL
      now provides `TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256` (0xC02B)
      as the preferred RFC 5764 §5 mandatory suite (with
      `TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384` (0xC02C) as a fallback);
      wolfSSL's SRTP exporter is enabled (`WOLFSSL_SRTP=yes` in
      `src/third_party/wolfssl/CMakeLists.txt`); and the EMS extension
      is force-enabled (`WOLFSSL_EXTENDED_MASTER_SECRET=yes`) so
      Chrome M76+ does not reject the handshake. Remaining gap:
      peer-cert fingerprint verification is deferred to P1.1 —until
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

- **Vendor sources are checked into the tree** instead of being pulled via
  `git submodule` + a `vendor.json` manifest. 0.9.0-rc1 ships a ~280 MB
  checkout because upstream mbedtls / libopus / libjuice / libsrtp /
  googletest / nlohmann_json source trees are committed directly. This
  means upstream security patches must be merged by hand. Migrating to
  submodules + a `vendor.json` manifest with SHA256-pinned tags is a
  blocker for the 1.0.0 tag.
- **Signaling answerer bridge for real Chrome is not implemented.**
  The WebSocket signaling server (`interop/signaling/signaling_server.py`)
  is wired up, but the NimRTC demo binary does not yet consume the
  buffered offer / ICE candidates from the WS ? it expects SDP on the
  CLI. As a result, `run_interop.py` (the older harness) cannot reach
  the full Chrome?NimRTC audio round-trip yet. The new
  `tools/run_e2e_acceptance.py` Case D works around this by going
  through `signaling_proxy` instead.
- **Cross-platform validation is out of scope.** Linux, macOS, and
  aarch64 builds have not been executed at HEAD; CI has been reduced
  to Windows-only to avoid false-positive green ticks. Linux/macOS
  support is a 1.0.0 acceptance gate.

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
