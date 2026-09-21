# Changelog

All notable changes to NimRTC are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/)
and uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.10.2] - 2026-09-17

### Status: Tech Preview

### Highlights

- **Transport PAL Slice 8 — registry hooks for DTLS / SCTP / raw_UDP /
  transport-stack** (`docs/plan/transport-selection.md` §6.5 follow-up
  gap, closed). Four new typed slots on `core::PluginRegistry`:
  - `register_dtls_session(id, factory*)` / `get_dtls_session(id)`
  - `register_sctp_socket(id, factory*)` / `get_sctp_socket(id)`
  - `register_raw_udp_datagram(id, factory*)` / `get_raw_udp_datagram(id)`
  - `register_transport_stack(id, factory*)` / `get_transport_stack(id)`
  Each module's `register_default_plugins()` now publishes its factory
  through the matching typed slot (was previously a process-local cache
  or test-only accessor). All four slots participate in the unified
  `core::register_all_default_plugins()` table (PAL Slice 2).
- **Engine DTLS lookup via registry** — `engine.cpp` resolves the DTLS
  factory through `reg.get_dtls_session("wolfssl")` (was direct
  `make_unique<DtlsSessionWolfSSL>`). `engine.hpp` is **byte-identical**
  with v0.10.1 — the lookup id is hard-coded to "wolfssl" so the public
  API surface stays frozen. Non-wolfSSL builds (defensive `else`
  branch) keep the pre-Slice-8 direct-construction path. Slice 7.5 will
  widen this to a `cfg.dtls_name` field.
- **Callback types promoted to `plugins/base.hpp`** (§6.5). The local
  typedefs in `nimrtc/sctp/sctp_socket_iface.hpp`
  (`plugins::OnSctpRecvCb`) and `nimrtc/raw_udp/raw_udp_datagram.hpp`
  (`plugins::Endpoint`, `plugins::OnDatagramCb`) are now canonical in
  `plugins/base.hpp`. The Slice 5/6 headers no longer re-declare them
  — a single source of truth. Source-compat: any pre-Slice-8 caller
  that wrote `plugins::OnSctpRecvCb` etc. continues to compile.
- **`IRawUdpFactory` interface added** (`src/raw_udp/include/nimrtc/raw_udp/raw_udp_factory_iface.hpp`).
  `ArqRawUdpFactory` now publicly inherits the interface, parallel to
  `dtls::IDtlsSessionFactory` and `sctp::ISctpSocketFactory`.
- **Shell `CapabilitySelectorStackFactory`** (id="default") registered
  through the new `register_transport_stack()` hook. The factory
  produces a `CapabilitySelectorStack` whose component accessors
  return references to null-instance singletons (no-op / kErrNotReady).
  Slice 7.5 will replace this with `WebRtcClassicStackFactory` (real
  ICE+DTLS+RTP+SCTP composition).

### Added

- **`nimrtc::transport::register_default_plugins()`** — Slice 8 entry
  point that publishes `CapabilitySelectorStackFactory` into the
  `core::PluginRegistry::register_transport_stack()` slot.
- **`NIMRTC_REGISTER_DTLS_SESSION / NIMRTC_REGISTER_SCTP_SOCKET /
  NIMRTC_REGISTER_RAW_UDP_DATAGRAM / NIMRTC_REGISTER_TRANSPORT_STACK`**
  macros — convenience `static ::nimrtc::core::detail::Registrar`
  wrappers for the four new typed slots, parallel to the existing
  audio / video / bwe / scheduler macros.
- **`IRawUdpFactory` interface** — typed factory contract for
  `IRawUdpDatagram` backends (Slice 6.5 → Slice 8 promotion).

### Changed

- **`dtls::register_default_plugins()`** now publishes
  `WolfsslDtlsFactory` through `register_dtls_session("wolfssl")`
  (Slice 8). Legacy `dtls::test_only::get_wolfssl_factory()` is
  preserved as a back-compat accessor that now reads back through
  the registry so the typed slot is the single source of truth.
- **`sctp::register_default_plugins()`** now publishes
  `SctpStubFactory` through `register_sctp_socket("stub")` (Slice 8).
  Legacy `sctp::test_only::get_stub_factory()` is preserved similarly.
- **`raw_udp::register_default_plugins()`** now publishes
  `ArqRawUdpFactory` through `register_raw_udp_datagram("arq")`.
- **`pal_default_registrars.cpp` `kDefaultRegistrars[]`** extended
  with DTLS / SCTP / raw_UDP / transport entries — every transport-
  layer seam now participates in the unified entry point.
- **Engine `init_modules_once()` DTLS construction** now goes through
  the registry (see Highlights).

### Notes

- No public API change (`nimrtc/engine/engine.hpp` is byte-identical
  with v0.10.1).
- 7 + 2 PAL Slice 1/2/3 subtests in `test_engine_plugin_loading.cpp`
  stay green; 5 new Slice 8 subtests verify the four new registry
  hooks are populated and that `register_all_default_plugins()`
  remains idempotent under repeated calls.
- `git log v0.10.1..HEAD --oneline` ≤ 30 commits (Slice 8 landed as a
  single sequence; CHANGELOG + release notes + tag cut to follow).
- 国密后端 (SM2/SM4) remains P4 / Enterprise — not in v0.10.2.
- API still **experimental / not for production** through v1.0.0.
- No binary artefacts shipped (源码为主).
- 4 new DTLS / SCTP / raw_UDP / transport-stack registry hooks are
  the §6.5 follow-up gap listed in `docs/plan/transport-selection.md`
  (was: "Slice 8 必须顺手补"). Closed.

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

### Added retroactively in v0.10.3

> **Note:** These entries describe work that landed in commit `446e8a0
> refactor(pal): Slice 2 + Slice 3` and shipped with the v0.10.1 tag,
> but the CHANGELOG `[Unreleased]` block was not promoted at release
> time (the same hygiene drift that v0.10.0 fixed for v0.9.2). The
> v0.10.3 tag brings the CHANGELOG into alignment with the code tree
> at v0.10.1; no new code, no new tests, no public API change are
> introduced by this hygiene pass.

- **PAL Slice 2 — explicit self-registration table**
  (`docs/plan/pal-architecture.md` §4 Slice 2). The engine's
  `core::register_all_default_plugins()` is now backed by an iterable
  table (`core::detail::kDefaultRegistrars[]` in
  `src/core/src/pal_default_registrars.cpp`) instead of an inline
  statement list. Layout Invariant 1 of `docs/architecture.md`
  ("`src/core/` is the only INTERFACE library") is preserved by
  compiling this single .cpp into a new `nimrtc_core_objects` OBJECT
  library that is linked INTERFACE by `nimrtc::core`. The table enables
  (a) compile-time completeness audits via a test iteration, (b)
  mechanical grep discoverability, and (c) future extension without
  editing `registry.hpp`.
- **PAL Slice 3 — `NIMRTC_PLUGIN_ID()` compile-time-unique id literal
  wrapper** (`docs/plan/pal-architecture.md` §4 Slice 3). Each plugin
  factory's `id()` callsite wraps its string literal in a
  `nimrtc::core::detail::PluginIdTag<__LINE__>` wrapper, giving every
  callsite a distinct type at compile time. Implicit conversion to
  `std::string_view` preserves all existing
  `return NIMRTC_PLUGIN_ID("webrtc_apm");` patterns. Two new
  `EnginePluginLoading` subtests
  (`pal_default_registrars_table_is_nonempty`,
  `pal_plugin_id_wrappers_preserve_string_values`) verify the table
  iteration registers all 5 unconditional categories and that all 6
  known factory ids match their v0.10.0 string values byte-for-byte.
- **5 plugin factories migrated to `NIMRTC_PLUGIN_ID()`** (PAL Slice 3):
  `audio3a::NullPluginFactory` (`"webrtc"`),
  `audio3a::WebRtcPluginFactory` (`"webrtc_apm"`),
  `opus::OpusPluginFactory` (`"opus"`),
  `h264::H264PluginFactory` (`"h264"`),
  `jb::PluginFactory` (`"adaptive"`),
  `dtls::WolfsslDtlsFactory` (`"wolfssl"` via the existing `kBackendId`
  constexpr literal).

---

## [0.10.3] - 2026-09-18

### Status: Tech Preview

### Highlights

- **PAL Slice 7.5 — `WebRtcClassicStackFactory`** (commit `7d3a92d`,
  `docs/plan/transport-selection.md` §12.5): the first real factory
  implementation registered through the v0.10.2
  `core::PluginRegistry::register_transport_stack()` hook. Composes the
  four real backend singletons (ICE / DTLS / RTP / SCTP) all resolved
  through the plugin seam — no concrete-`new` inside the factory. Id
  = `"webrtc-classic"`. Lifecycle methods (`start()` / `close()` /
  `tick()`) are wired through. CapabilitySelector rule 1
  (`needs_browser_interop == true`) now resolves to a real
  ICE+DTLS+RTP+SCTP composition instead of the Slice 8 shell
  `CapabilitySelectorStackFactory` (id `"default"`, still registered
  for backward compatibility).
- **CHANGELOG retroactive attribution for v0.10.1** (PAL Slice 2 + 3) —
  see the "Added retroactively in v0.10.3" sub-heading inside
  `[0.10.1]` above. No new code; documentation alignment.
- **Root-cleanliness sweep** — defensive `.gitignore` patterns
  (`*.lock`, `src/engine/src/*.lock`) added. Local-only stray files
  (`sslkeylog.log`, `Testing/`, `src/engine/src/engine.cpp.lock`)
  deleted from disk at tag prep time. No tracked files changed.

### Added

- **`src/transport/src/webrtc_classic_stack.cpp`** (Slice 7.5):
  `WebRtcClassicStack` + `WebRtcClassicStackFactory` with id
  `"webrtc-classic"`. Registered via
  `core::PluginRegistry::register_transport_stack()` from
  `nimrtc::transport::register_default_plugins()`.
- **2 new `EnginePluginLoading` subtests** in
  `tests/test_engine_plugin_loading.cpp`:
  `slice75_webrtc_classic_factory_registered` (factory wired through
  the Slice 8 registry hook) and
  `slice75_webrtc_classic_stack_create_and_lifecycle` (the produced
  stack exposes ICE/DTLS/RTP/SCTP refs through the seam and survives
  `start()`/`close()` in the right order).

### Changed

- **`src/transport/src/transport_plugin.cpp`**
  `do_register_default_plugins()` now publishes both
  `"default"` (Slice 8 shell, retained for back-compat with any
  v0.10.2 caller) and `"webrtc-classic"` (Slice 7.5 real) through
  `register_transport_stack()`.

### Notes

- No public API or ABI change. `git diff v0.10.2..v0.10.3 -- src/engine/include/**`
  shows zero lines.
- Engine still constructs ICE/DTLS/RTP/SCTP through its direct
  constructor path; Slice 7.5's value is the factory-side seam, not the
  engine-side ownership switch (that change is tracked as a future
  "engine accepts `ITransportStack*`" patch in `transport-selection.md`
  §12.5.4).
- `tests/test_engine_plugin_loading`: 15/15 subtests (8 base + 5 Slice 8 +
  2 Slice 7.5). Full suite: 380/387 ctest pass overall; the 7 failures
  are pre-existing WSL-localhost UDP-loopback timeouts, unrelated to
  Slice 7.5 per commit `7d3a92d`. The 2 `Audio3ATapFixture` subtests
  remain intentionally disabled (pre-existing condition, tracked
  separately).
- 国密后端 (SM2/SM4) remains P4 / Enterprise — not in v0.10.3.
- API still **experimental / not for production** through v1.0.0.

---

## [0.11.0] - 2026-09-20

### Status: Beta 前哨

> **Open 2026-09-18.** First minor to ship P2 content (DataChannel interop,
> in-process SFU relay, PCM tap on WebRTC APM, Profile library, AI Agent demo).
> Planned release, tracked in `docs/plan/v0.11-plan.md`.

- **RFC-1:** RFC 001 (PCM tap) promoted Draft → Final.

### Highlights (TAP-1 — PCM tap on WebRTC APM)

- **`IAudio3A::set_pre_process_tap()` and `set_post_process_tap()` are now
  live on the default `PluginAdapter` implementation** (RFC 001 / §6.1–6.5,
  v0.11.0 — TAP-1 DoD). The WebRTC-APM-backed adapter
  (`audio3a::PluginAdapter` — wired to `webrtc_apm` id, with `NullAudio3A`
  fallback) now fires pre and post 3A taps per `process_capture()` call,
  so AI-Agent / wake-word / streaming-ASR consumers can subscribe to the
  raw mic PCM (pre) and the cleaned PCM (post) without an extra DSP
  integration. Taps are `std::function`-based, install / uninstall via
  `nullptr`, and guarded by a dedicated `tap_mu_` mutex so setters from
  any thread stay race-free against the audio thread's `process_capture()`
  call (RFC §2.3 thread-safety contract).
- **`post_tap_i16` ASR-friendly variant** — the post-tap is now also
  delivered as `int16_t*` with the same metadata.  Conversion uses
  `int16_t(std::round(std::clamp(s, -1.0f, 1.0f) * 32767.0f))` per RFC
  §6.3 (round + clamp BEFORE multiplication so `-1.0f → -32767`, never
  `-32768`; matches Whisper / Vosk / Kaldi behaviour).  A reusable
  `int16_tap_buf_` member eliminates per-frame heap allocation on the
  audio path.
- **`timestamp_us` source** — captured ONCE per `process_capture()` call
  via `std::chrono::steady_clock::now().time_since_epoch().count() / 1000`
  (RFC §6.4 — monotonic microseconds).  The pre-tap and post-tap share the
  same `PcmFrameMetadata` so the timestamp is identical for the same
  frame (RFC §2.4 single-tick-per-call).
- **TAP-1 test suite** — `tests/plugins/test_audio3a_tap.cpp` adds 15
  GTest subtests covering pre / post / i16 / null-uninstall / metadata /
  ordering / reinstall / concurrent-install-and-process stress.  All 15
  pass.  The pre-existing `test_audio3a_plugin` (17 subtests) still
  passes — no regression.  Test binary: `tests/Debug/test_audio3a_tap.exe`.

### Highlights (TPAL-4 — DTLS seam engine-routing cleanup)

- **`EngineConfig::dtls_name` field added** — defaulted to `""` (preserves
  every v0.10.x caller source-compat).  Resolved at engine.open() time
  through `core::PluginRegistry::get_dtls_session(id)`; empty string
  falls back to the built-in `WolfsslDtlsFactory` at id `"wolfssl"`.
  Future 国密 / OpenSSL / BoringSSL / mbedTLS backends register their
  own id via `register_dtls_session("guomi_sm4", &factory)` and select
  via `cfg.dtls_name = "guomi_sm4"`.
- **`IDtlsSession` extended with the engine-facing surface** — the seam
  now declares `open()` / `set_role(DtlsRole)` /
  `set_peer_fingerprint(string, vector)` / `feed_inbound` /
  `take_outbound` / `tick` / `state` / `is_connected` /
  `local_fingerprint` / `srtp_keying_material`.  Both concrete
  implementations (`DtlsSessionWolfSSL` and the non-wolfSSL fallback
  `DtlsSession`) now publicly inherit `IDtlsSession` and `override` the
  full seam surface, so `unique_ptr<IDtlsSession>` accepts either
  without downcasting.  All types hoisted out of `dtls.hpp` into the
  shared `nimrtc/dtls/dtls_types.hpp` to break the seam↔module include
  cycle — `dtls.hpp` now includes `dtls_types.hpp` instead of
  redeclaring every constant and type (resolves the long-standing
  "kFingerprintHashLen redefined" duplicate-definition error when both
  headers are pulled into a single TU).
- **`static_cast<DtlsSessionWolfSSL*>` removed from engine.cpp** — the
  `Impl::dtls` field is now `std::unique_ptr<nimrtc::dtls::IDtlsSession>`
  and is populated directly from the factory.  Unknown ids surface as
  `kEngineInternal` via `on_error_` (deliberately do NOT silently fall
  back to `"wolfssl"` — that would mask integrator typos for the 国密
  path).
- **`#ifdef NIMRTC_USE_WOLFSSL_DTLS` block in engine.cpp deleted** —
  the wolfSSL / non-wolfSSL conditional that v0.10.2 used as a
  transitional escape hatch (hard-coded `DtlsSessionImpl` typedef +
  `static_cast`) is gone.  The engine now goes through the seam
  unconditionally; `NIMRTC_USE_WOLFSSL_DTLS` is still defined for the
  concrete module compile, but no longer drives engine.cpp control
  flow.
- **`NullDtlsSession` (transport shell-stack component) extended** —
  `src/transport/src/default_transport_stack_factory.cpp`'s stub DTLS
  component now implements the full TPAL-4 engine-facing surface
  (`open` / `set_role(DtlsRole)` / `set_peer_fingerprint(string, vec)`
  / `feed_inbound` / `take_outbound` / `tick` / `state` /
  `is_connected` / `local_fingerprint` / `srtp_keying_material`), not
  just the original Slice 4 surface.  Without this the C++ linker
  rejects the class with C2259 ("cannot instantiate abstract class").
- **TPAL-4 seam mock-factory test** — `src/dtls/tests/test_dtls_seam_mock_factory.cpp`
  adds 5 GTest subtests: registry accepts and round-trips the mock;
  mock-factory create() returns a polymorphic `IDtlsSession*` whose
  full vtable is wired (every seam method exercised); same-id
  registration overwrites per `TypedRegistry` contract;
  `list_dtls_sessions()` includes both the mock and the built-in
  wolfssl; the engine's `cfg.dtls_name` lookup resolves the mock
  pointer.  All 5 pass.
- **`test_engine_plugin_loading` regression check** — 16 of 17 subtests
  still pass; the 1 regression is
  `slice75_idempotent_register_does_not_duplicate` which asserts
  `list_sctp_sockets().size() == 1`.  That assertion was written for
  the v0.10.3 single-stub state; TPAL-5 in this same v0.11.0 cycle
  adds a second SCTP factory (`"usrsctp"`), bringing the count to 2.
  Tracked as a TPAL-5 follow-up — unrelated to TPAL-4 DTLS work.
- **`engine.hpp` public-API delta** — `EngineConfig::dtls_name` is the
  one and only public-API delta in this PR.  Layout Invariant 4 (no
  concrete module headers in `engine.hpp`) is preserved: the field is
  a plain `std::string`; the rest of `engine.hpp` is unchanged from
  v0.10.3.

### Highlights (PROFILE-1 — 5 Profile variant catalog)

- **Three new built-in `kProfile*` constants and matching JSON files**
  appended to the v1.0 profile library:
  - `kProfileSfuAgent` ⇄ `profiles/sfu-agent.json` — SFU relay + AI Agent
    hybrid (N:1).  Relay path skips L2 processing; Agent side keeps
    WebRTC APM for the PCM-tap consumer.
  - `kProfileAgentGateway` ⇄ `profiles/agent-gateway.json` — server-side
    Agent media gateway that exposes the pre/post-3A PCM tap surface
    (`IAudio3A::set_pre_process_tap` / `set_post_process_tap` per
    RFC 001 / TAP-1) for ASR / wake-word / VAD consumers.
  - `kProfileAgentLowLatency` ⇄ `profiles/agent-low-latency.json` —
    Agent SDK's tightest-latency variant: 20 ms JB initial, AGC level
    estimator off (signalled via `agent_low_latency` marker), Opus FEC
    off.
- **Two v1.0 JSON↔C++ latent-bug fixes** uncovered by the new strict
  round-trip test: `kProfileLive` now sets `scheduler.impl =
  "weighted_fair"` (was relying on the default `"strict_priority"` while
  `live.json` declared strategy `"weighted_fair"`); `profiles/sfu.json`
  now correctly sets `bwe_name = ""` (the comment said "Empty: no BWE"
  but the value was the string `"aimd"`).
- **`profiles/{sfu,teleop}.json` files exist** but no new `kProfile*`
  was added — they round-trip with the existing `kProfileSfu` /
  `kProfileTeleop` constants.  v0.10.x callers that used the C++ form
  continue to work.
- **`docs/profiles.md`** — single-page index of all 9 profiles with a
  decision matrix (jb / audio3a / codec / bwe / datachannel /
  distinguishing feature) plus "when to pick each" notes.
- **`test_assembly.cpp` coverage** extended from 28 to 32 subtests:
  3 new `Profile*DistinguishingFeature` invariant tests, plus a strict
  `JsonFileMatchesBuiltinConstant` regression test that loads every
  JSON profile file and asserts field-by-field equality with the
  matching C++ constant.  All 32 pass.

### Highlights (TPAL-5 — SCTP seam: usrsctp backend production land)

- **`UsrsctpSocketFactory` (id="usrsctp") now registered alongside the
  existing `SctpStubFactory` (id="stub")** in
  `nimrtc::sctp::register_default_plugins()`.  Vendored upstream
  usrsctp 0.9.5.0 at `src/third_party/usrsctp/`.  The Slice-5 seam
  surface (`send_datagram` / `send_stream` / `send_partial_reliable` /
  `set_on_recv`) is fully wired and callable through
  `core::PluginRegistry::get_sctp_socket("usrsctp")`, and the
  **SCTP association handshake** (`usrsctp_listen` /
  `usrsctp_connect`) **已落地** in TPAL-5 Stage 2 — two
  `UsrsctpSocket` instances on `127.0.0.1` complete the
  SCTP INIT/INIT-ACK handshake via the shared userspace UDP socket
  (`s_acquire_shared_udp()` / `s_release_shared_udp()` wrapped with
  `SCTP_REMOTE_UDP_ENCAPS_PORT` per RFC 6951), with state
  transitions (`kUnbound → kListening/kConnecting → kEstablished`)
  driven by `SCTP_ASSOC_CHANGE` notifications on the per-instance
  recv callback. End-to-end sends now work over a live association.
- **v0.10.x callers that explicitly pick `id="stub"` continue to
  work** — TPAL-5 Stage 1 is purely additive; the engine does NOT
  silently switch the default backend.  The stub stays as the
  registration-time default (`SctpStubFactory` first-wins), so any
  external reproducer that hard-codes `"stub"` is source-compat.
- **Static-trampoline ABI fix on Windows MSVC** — `s_instance_recv_cb`
  / `s_instance_send_cb` now declare the actual usrsctp types
  (`struct socket*` / `union sctp_sockstore` /
  `struct sctp_rcvinfo`) instead of `void*` reinterpret_cast.  The
  prior `void*` cast was UB on Windows because `struct sctp_rcvinfo`
  is ~20 bytes, not pointer-sized; the trampoline signature drifted
  out of the upstream type contract and surfaces as a corrupt
  `rcvinfo` read by the `OnRecv` dispatcher.  Stage 1 fixes this
  before the handshake lights up the trampoline path on receive.
- **`tests/test_sctp_usrsctp.exe`**: 4/4 PASS. Factory-registration
  (`UsrsctpSocketFactory` reachable via `get_sctp_socket("usrsctp")`)
  + stub-not-displaced regression (`get_sctp_socket("stub")` still
  returns `SctpStubFactory`) + `LoopbackDatagramRoundTrip` (in-process
  SCTP association handshake + 64-byte datagram echo through
  shared UDP socket + `SCTP_REMOTE_UDP_ENCAPS_PORT` RFC 6951
  encapsulation) + `PartialReliableTtlDrop` (PR-SCTP TTL seam
  reachable; best-effort recv assertion per known usrsctp 0.9.5.0
  Windows MSVCRT WSAELOOP limitation) all PASS.

---

## [Unreleased]

> No unreleased changes yet. The next planned release is **v0.11.0**
> (Beta 前哨), tracked in `docs/plan/v0.11-plan.md`. Items landed in
> v0.11.0 cycle include RFC-1 (PCM Tap Final), TPAL-4 (DTLS seam +
> `Config::dtls_name`), TPAL-5 (`UsrsctpSocketFactory` + Stage 2
> SCTP handshake), DC-1 (`SctpDataChannel`), DC-2 (L1 strict-priority
> scheduler), TAP-1 (PCM tap on WebRTC APM), PROFILE-1 (5-variant
> Profile library + 32/32 regression), DEMO-1 (`demo-agent-gateway`),
> DISC-1 (GitHub Discussions enabled).

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
