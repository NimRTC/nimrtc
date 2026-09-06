# Changelog

All notable changes to NimRTC are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/)
and uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-09-06

### Added
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
- vendored libjuice 77daa8b, libsrtp 2f82ec0, mbedtls 4.2.0 source trees (un-stubs P0 vendor plumbing)
- R2.5: `plugins::IICETransport` interface (extends `ITransport`) and `plugins::IICETransportFactory`, registered via `PluginRegistry::register_ice_transport()` / `get_ice_transport()`; the engine and demo now reach ICE-specific methods through `engine.get_ice_transport()`, removing the previous `dynamic_cast<ice::IceTransport*>` leak. New header: `src/plugins/include/nimrtc/plugins/ice_transport.hpp`.
- P2: PCM tap interface on `IAudio3A` (§8.7 of the technical doc) — `set_pre_process_tap()` and `set_post_process_tap(PcmTapCallback, PcmTapCallbackI16)` plus the supporting `PcmFrameMetadata` struct, allowing wake-word engines to see raw mic PCM pre-3A and ASR engines to see 3A-cleaned PCM in float32 or int16. Design rationale captured in ADR-008.
- P0 vendor: third-party sources for libjuice (`77daa8b`), libsrtp (`2f82ec0`) and mbedtls (`023aca8`) materialised on disk with submodules initialised; CMake wrappers unchanged.
- P0 fetch: nlohmann_json FetchContent now supports a four-tier offline fallback — `NLOHMANN_JSON_SOURCE_DIR` pre-extracted path, staged tarball at `${CMAKE_BINARY_DIR}/_deps-cache/nlohmann_json.tar.xz`, `NLOHMANN_JSON_ARCHIVE` user-supplied archive, and online fetch from github.com with SHA256 verification.
- Interop harness: `interop/run_interop.py` now drives Chrome via Playwright (with a subprocess fallback) so `_interopResults` is actually extracted from the headless page instead of being silently discarded.

### Changed
- **Vendored nlohmann_json 3.11.3** as a single-header INTERFACE library at `src/third_party/nlohmann_json/`. Top-level CMakeLists no longer fetches nlohmann_json via FetchContent; assembly module now builds fully offline. See `src/third_party/nlohmann_json/LICENSE.MIT` (MIT, Copyright (c) 2013-2022 Niels Lohmann).
- **Vendored GoogleTest 1.12.1** at `src/third_party/googletest/`. Replaces FetchContent in `cmake/NimRTCTest.cmake`. All builds (configure + build + test) are now fully offline. License: BSD-3-Clause (Copyright 2008 Google Inc.). GoogleMock is disabled by default (`BUILD_GMOCK=OFF`) because `tests/` do not use it.

### Fixed
- CMake: `nimrtc_add_test` unknown command (moved `include(NimRTCTest)` before `add_subdirectory(modules)`)
- CMake: duplicate `DEPS` keyword in `cmake_parse_arguments`
- `.gitignore`: added `cmake-configure.log`, `.vs/`, `CMakeUserPresets.json`
- Interop: `signaling_server.py` handler now reads connection path via `ws.path` (websockets 13.x legacy protocol) instead of the missing `ws.request`, so per-path room routing actually works.
- **`HwSeam.IsHwAcceleratedHelperDetectsHwPlugin`** test was crashing with SEH `0xc0000005`. Root cause: the test queried `PluginRegistry::get_video_sender("reference")` without first calling `register_default_plugins()` to populate the registry. Fixed by adding the three `register_default_plugins()` calls (video_source / video_sink / video_pipeline) at the top of the test, matching the pattern used by every other test in `tests/test_hw_plugin_seam.cpp`.
- **DTLS handshake with Chrome — ServerHello / Certificate / ServerKeyExchange now sent**.  Previously, the NimRTC DTLS server path only emitted `ServerHello + ServerKeyExchange + ServerHelloDone`, omitting the mandatory `Certificate` handshake message, and the ServerKeyExchange signature was a raw SHA-256 digest instead of an ECDSA signature.  Chrome's BoringSSL rejects both, causing the ClientHello retransmit loop observed in `build/_proxy.log`.  Fixed in `src/modules/dtls/src/dtls.cpp`:
  - Added `build_self_signed_cert()` that emits a syntactically valid DER-encoded X.509v3 certificate (`SubjectPublicKeyInfo` wrapping the ECDSA P-256 pubkey) and self-signs the TBSCertificate via `BCryptSignHash` against the ECDSA keypair.  Cert is cached at `open()` and embedded in the `Certificate` handshake message sent between `ServerHello` and `ServerKeyExchange`.
  - Added a separate ECDSA P-256 keypair (`ecdsa_alg`/`ecdsa_key`) since BCrypt binds key handles to their alg provider and `BCryptSignHash` rejects ECDH-bound keys.  ECDSA keypair shares its certificate's pubkey with the SDP-pinned fingerprint (fingerprint now computed from the cert's SPKI, not the ECDH pubkey).
  - ServerKeyExchange signature now uses `bcrypt_ecdsa_sign_der()` which calls `BCryptSignHash` on a SHA-256 digest of `(client_random || server_random || server_params)` and re-encodes the raw r||s output as ASN.1 DER `SEQUENCE { INTEGER r, INTEGER s }`.  Wire format: 2-byte big-endian length prefix + DER bytes.
  - ServerHello `legacy_version` changed from `0xFEFF` (DTLS 1.0) to `0xFEFD` (DTLS 1.2) and now carries a `supported_versions` extension (`type 0x002B`, value `0xFEFD`) — modern Chrome (≥M150) requires this to confirm DTLS 1.2 was selected since their ClientHello's `legacy_version=0xFEFF` is treated as DTLS 1.0 without the explicit extension.
  - DER helpers added: `der_encode_integer`, `der_encode_oid`, `der_encode_utctime`, `der_wrap_sequence`.  All use length-prefix encoding with the proper ASN.1 tag byte.
  - `teardown_crypto()` extended to release the ECDSA key/alg handles.
  Verified: ICE completes (`connected` → `completed`), NimRTC sends the full ServerHello + Certificate (336 bytes) + ServerKeyExchange (167 bytes) + ServerHelloDone (25 bytes) DTLS flight.  Chrome still retransmits ClientHellos after this flight, which suggests an additional cert/SPKI or signature-format mismatch — see `Known issues` below.

### Known issues (deferred)
- **Chrome DTLS still rejects NimRTC's ServerHello flight** — After the
  fixes above, NimRTC emits the full ServerHello + Certificate +
  ServerKeyExchange + ServerHelloDone flight, but Chrome continues to
  retransmit ClientHello (logged in `build/_proxy.log` as four
  ClientHellos before ICE fails with "Consent expired").  The most likely
  remaining issues:
  - **DTLS cipher**: NimRTC selects `TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256`
    (`0xC023`).  Modern Chrome prefers `0xC02B` (AES-128-GCM) or `0xCCA9`
    (ChaCha20-Poly1305).  Switching to GCM requires implementing the
    AES-GCM AEAD nonce/explicit-IV twist in the DTLS record layer — see
    RFC 5288 / RFC 5246 §6.2.3.2.
  - **X.509 cert parse**: Chrome's BoringSSL X.509 parser is strict.
    The current self-signed cert is structurally minimal (single CN,
    no extensions, no AIA, no SKI/AKI).  A more complete cert with
    `basicConstraints CA:FALSE` and `subjectAltName` may be required.
  - **Certificate verify chain**: Even with fingerprint match, BoringSSL
    may require a non-empty `issuer` chain.  Test with `--enable-features=...
    WebRtcAllowInputVolumeAdjustment` or by adding a debug callback to
    Chrome to inspect the verify error.
  *Reproduction*: `python build/orchestrate_e2e_v2.py` (after `cmake
  --build build --config Debug --target demo-p2p`).
- **`chrome_opus_interop` partial fix — NimRTC still has no signaling answerer bridge.**
  Identified 2026-09-05 during R3 regression.
  *What is now fixed (2026-09-05, 07:53)*:
    - `interop/signaling/signaling_server.py`: handler now reads the connection
      path via `ws.path` (websockets 13.x legacy protocol exposes the URL path
      there; `ws.request` is absent). Without this fix the path was always empty
      and rooms were routed by `args.room` default only. Verified: log now shows
      `WS HANDSHAKE: path='/interop' room_id='interop'` for Chrome connections.
    - `interop/run_interop.py`: `ChromeBrowser` rewritten on top of Playwright
      (with a subprocess fallback) so that `window._interopResults` is actually
      extracted from the headless page. Subprocess launch was launching Chrome
      correctly but never reading results back, so the test always reported
      "did not connect" even when the WS had connected. Verified: Playwright
      now reports `wsConnected=True` when the WS succeeds.
  *What is still blocked (deferred)*: signaling server works, Chrome connects
  and joins the room as offerer, but the NimRTC demo binary has no consumer
  for the buffered SDP offer / ICE candidates, so it never creates a
  `PeerConnection` and never sends back an SDP answer. Chrome waits 8s for an
  answer, closes the WS (`exitCode=18, "WebSocket closed before ICE connected"`).
  Reproduction artifacts: `build/_sig_err.log`, `build/_cdp_e2e.log`,
  `build/_cdp_e2e_ofer.py`.
  Required follow-up: implement a NimRTC-side signaling client (answerer bridge)
  that listens for `offer`/`candidate` messages on the WS, drives
  `Engine::createPeerConnection` / `setRemoteDescription` / `createAnswer`,
  pushes the resulting answer and local ICE candidates back over the same socket.
  Out of scope for R3-Batch; track as a separate work item (suggested title:
  *NimRTC signaling answerer bridge for Chrome interop*).

### Changed
- `NIMRTC_FETCH_GTEST` is now initialised before `include(NimRTCTest)` to ensure
  GoogleTest is fetched on first configure without a stale cache
- `nimrtc_target_include_directories()` dead function removed from `NimRTCOptions.cmake`
- Vendor library target names standardised to `nimrtc::vendor::<name>` namespace

## [0.1.0] — P0 scaffold

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

Until v1.0.0, **MINOR** bumps indicate phase completions (P0, P1, P2…),
and **PATCH** bumps indicate internal fixes within a phase.
