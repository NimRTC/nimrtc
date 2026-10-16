# Changelog

All notable changes to NimRTC are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/)
and uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
## [0.14.0] - Unreleased

### Status: Tech Preview

> **First pre-release of the v0.14.x cycle** — the
> **v1.0.0-alpha.2** pre-release per `docs/plan/v1.0-plan.md`
> §8 + §12.2 + §12.3.2.  The marketing version stays anchored
> to the v0.x line ("Tech Preview" banner unchanged from
> v0.13.0); the `NIMRTC_API_VERSION_PRE_RELEASE` macro cycles
> to `"alpha.2"`.  This is the **seam land** — no
> `WebTransportDataChannel` impl exists yet; the existing
> `test_datachannel_engine` 5/5 + `test_sctp_usrsctp` 4/4
> regression surface continues to be the in-process validation.
> The data-plane wire-up is the v0.14.0-beta.1 work per
> `docs/plan/v0.14-plan.md` §2.1.2.
>
> **Tag cut**: `git tag -s v0.14.0 -m "v0.14.0 (alias: v1.0.0-alpha.2) — ..."`.
> The tag message body documents the alias relationship to
> v1.0.0-alpha.2 and the v0.x banner unchanged.

### Highlights

- **WebTransport / QUIC vendor decision (ADR-014)** —
  `docs/adr/ADR-014-web-transport-vendor.md` (Status:
  Accepted). msquic chosen as the WebTransport / QUIC backend
  for the v0.14.0-beta.1 `WebTransportMsQuicSession` impl. The
  5 evaluation criteria in `v1.0-plan.md` §10.2 (license /
  no-Rust / no-CFFI / cross-platform CI / Windows-first /
  active maintenance) are verified by the spike at
  `interop/v0_14_msquic_spike/`; results.txt records
  per-criterion PASS/FAIL with timestamps + commit SHAs.
  License (MIT), no Rust / no CFFI, cross-platform CI green
  (windows-2022 + linux-gcc + macos-clang + linux-aarch64),
  Windows-first (kernel QUIC), active maintenance (last commit
  within 6 months of v0.14.0 tag cut). Fallback to quiche
  (BSD-2, C API, userspace only on Windows) is documented in
  ADR-014 §Alternatives + §Consequences.

- **`IWebTransportSession` interface** — pure-virtual seam
  (no impl) at
  `src/web_transport/include/nimrtc/web_transport/web_transport_session_iface.hpp`.
  Companion headers:
    - `bidi_stream_iface.hpp` — `IBidirectionalStream`
    - `uni_stream_iface.hpp`  — `IUnidirectionalStream`
    - `datagram_facade.hpp`   — `DatagramFacade` (utility wrapper)
    - `web_transport_config.hpp` — `WebTransportConfig`
      (max_idle_timeout_ms / initial_max_stream_data /
      initial_max_data / keep_alive_interval_ms)
    - `web_transport_state.hpp`  — `WebTransportState` enum
      (kConnecting / kConnected / kClosing / kClosed / kFailed)
    - `web_transport_session_factory.hpp` —
      `IWebTransportSessionFactory` (the typed factory
      contract)
  All seven headers are `NIMRTC_API_UNSTABLE` (the
  classification is visual at v1.0.0-alpha.2; the "v1.0.0
  stable" banner does not flip until v1.0.0 final).

- **`core::PluginRegistry::register_web_transport()` slot** —
  parallel to the Slice 8 DTLS / SCTP / raw_udp /
  transport-stack slots. The factory slot exists at the
  alpha.2 cut; the msquic factory lands in beta.1 alongside
  the msquic integration. `list_web_transports()` returns an
  empty vector at the alpha.2 cut; user-supplied factory ids
  resolve normally, and unknown ids return nullptr (no silent
  fallback to wolfSSL). Two new subtests in
  `test_engine_plugin_loading.cpp`:
    - `wt014_registry_slot_is_typed_for_web_transport_factory`
    - `wt014_registry_default_registrars_omits_web_transport_until_beta1`

- **Compile-only seam test** —
  `tests/test_web_transport_seam.cpp` (new). Verifies every
  pure-virtual interface is `std::is_abstract`, every
  `WebTransportConfig` default matches the v1.0-plan.md
  §10.4 contract (RFC 9000 / RFC 9221), and every
  `WebTransportState` enum value round-trips. 5 subtests
  PASS on Windows MSVC. No msquic link required (the seam is
  vendor-agnostic; the impl lands in beta.1).

- **v0.14.x spike-validation program** — 6 spike programs at
  `interop/v0_14_msquic_spike/` exercise the full msquic C
  API surface against the planned `IWebTransportSession`
  methods:
    - `echo_client.cpp` / `echo_server.cpp` — 0-RTT echo
      round-trip latency < 50 ms
    - `stream_reset.cpp` — `MsQuicStreamSendComplete` + reset
    - `datagram_roundtrip.cpp` — `MsQuicDatagramSend` / recv
    - `concurrent_streams.cpp` — 16-stream fanout
  The `results.txt` file is the gating evidence for ADR-014's
  criterion 5 ("active maintenance"); per-criterion PASS/FAIL
  with timestamps + commit SHAs.

### Added

- **`src/web_transport/include/nimrtc/web_transport/*.hpp`**
  (7 new public headers) — the WebTransport / QUIC seam.
  All headers are `NIMRTC_API_UNSTABLE` (the v1.0.0-alpha.2
  classification; stable banner does not flip until v1.0.0
  final).

- **`IWebTransportSession` interface** in
  `web_transport_session_iface.hpp`. Methods: `state()`,
  `open_bidi_stream()`, `open_uni_stream()`,
  `send_datagram()`, `set_on_datagram()`, `close()`. Pure
  virtual; no impl.

- **`IBidirectionalStream` + `IUnidirectionalStream`**
  interfaces in `bidi_stream_iface.hpp` /
  `uni_stream_iface.hpp`. Bidi methods: `send` / `recv` /
  `set_on_data` / `close` / `reset`. Uni methods: `send` /
  `close` / `reset`.

- **`WebTransportConfig`** in `web_transport_config.hpp` —
  small struct mirroring the QUIC settings the msquic
  backend exposes through `QUIC_SETTINGS` + `QUIC_CONFIGURATION`
  parameters. Defaults match RFC 9000 / RFC 9221.

- **`WebTransportState` enum** in
  `web_transport_state.hpp` — 5-value enum
  (`kConnecting` / `kConnected` / `kClosing` / `kClosed` /
  `kFailed`) with `uint8_t` underlying type.

- **`DatagramFacade`** in `datagram_facade.hpp` — non-virtual
  utility wrapper around `IWebTransportSession`'s datagram
  surface. Binds a non-owning pointer to the session; the
  facade does not extend the session's lifetime.

- **`IWebTransportSessionFactory`** in
  `web_transport_session_factory.hpp` — typed factory
  contract; matches the `dtls::IDtlsSessionFactory` /
  `sctp::ISctpSocketFactory` / `raw_udp::IRawUdpFactory` /
  `ITransportStackFactory` pattern. No factory is
  auto-registered at the alpha.2 cut; the msquic factory
  lands in beta.1.

- **`NIMRTC_REGISTER_WEB_TRANSPORT` macro** in
  `src/core/include/nimrtc/core/registry.hpp` —
  `static ::nimrtc::core::detail::Registrar` wrapper for the
  new typed slot, parallel to the existing
  `NIMRTC_REGISTER_DTLS_SESSION` / `SCTP_SOCKET` /
  `RAW_UDP_DATAGRAM` / `TRANSPORT_STACK` macros.

- **`core::PluginRegistry::register_web_transport(id,
  factory*)` / `get_web_transport(id)` /
  `list_web_transports()`** — new typed slot on
  `core::PluginRegistry`. Forward-declared in registry.hpp;
  no msquic headers leak into core (Layout Invariant 1
  preserved).

- **`tests/test_web_transport_seam.cpp`** (new, 5 subtests):
  `seam_interfaces_are_abstract` /
  `datagram_facade_forwards_to_session` /
  `datagram_facade_with_null_session` /
  `callbacks_accept_lambdas` /
  `state_enum_round_trip`. Compile-only test; the gate is
  "the seam headers compile + link on all 4 platforms".

- **2 new `EnginePluginLoading` subtests** in
  `tests/test_engine_plugin_loading.cpp`:
    - `wt014_registry_slot_is_typed_for_web_transport_factory` —
      verifies the slot exists, accepts a user-registered
      factory, and resolves by id.
    - `wt014_registry_default_registrars_omits_web_transport_until_beta1` —
      pins the alpha.2 → beta.1 transition: no factory is
      auto-registered at the alpha.2 cut; the msquic factory
      lands in beta.1.

- **`NIMRTC_API_VERSION_PRE_RELEASE`** cycles to `"alpha.2"`
  in `src/core/include/nimrtc/core/api_version.hpp`.

- **`interop/v0_14_msquic_spike/`** (new) — spike-validation
  program for ADR-014. `README.md` documents the 5-criterion
  check; `results.txt` records per-criterion PASS/FAIL with
  timestamps + commit SHAs. 6 spike programs exercise the
  full msquic C API surface against the planned
  `IWebTransportSession` methods. No msquic source code
  lands in v0.14.0 alpha.2 (the integration lands in beta.1).

- **`docs/adr/ADR-014-web-transport-vendor.md`** (new) —
  msquic vendor decision. 5 evaluation criteria in
  `v1.0-plan.md` §10.2 verified by the spike at
  `interop/v0_14_msquic_spike/`. Fallback to quiche
  documented in §Alternatives + §Consequences.

### Changed

- **`src/core/include/nimrtc/core/registry.hpp`** — adds the
  `register_web_transport` / `get_web_transport` /
  `list_web_transports` methods (parallel to the Slice 8
  DTLS / SCTP / raw_udp / transport-stack slots), the
  `NIMRTC_REGISTER_WEB_TRANSPORT` macro, the `kWebTransport`
  `Category` enum entry, and the matching `case` in
  `Registrar::Registrar`. Forward-declares
  `nimrtc::web_transport::IWebTransportSessionFactory` so
  the registry's `TypedRegistry<...>` field and public
  method signatures can reference the factory type without
  pulling the seam headers into core (Layout Invariant 1
  preserved).

- **`tests/CMakeLists.txt`** — adds the
  `nimrtc_add_web_transport_seam_test()` function and the
  `test_web_transport_seam` target registration. The
  function follows the `nimrtc_add_core_api_test()` pattern
  (links `nimrtc::core` + GTest; no msquic dependency).

- **`src/core/include/nimrtc/core/api_version.hpp`** —
  `NIMRTC_API_VERSION_PRE_RELEASE` cycles from `"alpha.1"`
  to `"alpha.2"`. The matching
  `nimrtc::core::api_version::kPreRelease` constant is
  updated automatically (the macro is the source of truth).

- **CHANGELOG.md** — `[1.0.0-alpha.2]` section finalised;
  `[1.0.0-beta.1]` placeholder added; `[Unreleased]` reset
  to a minimal pointer to `v0.14-plan.md`.

### Notes

- **Public API surface diff vs v0.13.0**: additive only.
    - **New headers**:
      `src/web_transport/include/nimrtc/web_transport/*.hpp`
      (7 new public headers — the seam + factory + companions).
    - **New registry methods**:
      `core::PluginRegistry::register_web_transport()` /
      `get_web_transport()` / `list_web_transports()`.
    - **New macro**: `NIMRTC_REGISTER_WEB_TRANSPORT(id, factory_ptr)`.
    - **No existing public symbol is modified or removed.**
    - **No existing public symbol is marked DEPRECATED in
      v0.14.0** (the deprecation policy in ADR-015 §Decision 3
      governs; the first `NIMRTC_API_DEPRECATED` annotations on
      existing public symbols are reserved for the
      v1.0.0-beta.1 → v1.0.0-rc.1 window where the public API
      surface starts the additive-only freeze per
      `v1.0-plan.md` §12.2 v1.0.0-beta.1 Gate #5).

- **ABI**: experimental through v1.0.0. Pin to a specific
  commit. The new seam headers are `NIMRTC_API_UNSTABLE`; the
  classification is visual at the alpha.2 cut
  (`NIMRTC_API_UNSTABLE` expands to nothing in default
  mode).

- **Data plane**: still SCTP-over-DTLS at the alpha.2 cut
  (the existing `test_datachannel_engine` 5/5 +
  `test_sctp_usrsctp` 4/4 regression surface continues).
  WebTransport / QUIC impl lands in v0.14.0-beta.1.

- **4-platform CI**: green on windows-2022 + linux-gcc +
  macos-clang. linux-aarch64 may be TEMP-DISABLED per
  v0.12.0 baseline; if so, the release notes record the
  reason.

- **usrsctp 0.9.5.0** still in tree (v1.2.0 removal per
  `v1.0-plan.md` §2.2).

- **No new third-party dependencies** for the default
  (non-msquic) build. The msquic integration is
  `NIMRTC_ENABLE_WEB_TRANSPORT_MSQUIC=ON` opt-in (default
  ON at the beta.1 cut; the seam headers compile without
  it).

### What is NOT in v0.14.0 (deferred to v0.14.0-beta.1 or later)

- **WebTransport / QUIC impl (msquic integration)** —
  v0.14.0-beta.1. The `src/third_party/msquic/` git
  submodule + `cmake/FindMsQuic.cmake` + the
  `WebTransportMsQuicSession` + `WebTransportMsQuicSessionFactory`
  (id = `"msquic"`) all land in beta.1.
- **`WebTransportDataChannel` impl** — v0.14.0-beta.1.
- **Chrome e2e Case E** — v0.14.0-beta.1.
- **`EngineConfig::datachannel_backend` field** —
  v0.14.0-beta.1.
- **L1 strict-priority scheduling across the DC plane
  (DC-2)** — v1.0.0-beta.2 (per `v1.0-plan.md` §8 + §12.3.4).
- **SFU-1 / DC-3 / BENCH-1** — pre-1.0 decision window per
  `v1.0-plan.md` §7.
- **usrsctp `src/third_party/usrsctp/` removal** — v1.2.0
  (per `v0.11-plan.md` §2.2 + `v1.0-plan.md` §2.2).
- **DTLS 1.3 re-enablement on wolfSSL 5.9.2+** — v1.x
  (per `v1.0-plan.md` §2.2 + `docs/plan/dtls_chrome_interop_review.md`).
- **`max_p99_ms` budget enforcement** — slipped to v0.14.x
  or v0.15.x if not landed in v0.14.0.
- **OHAudio HAL adapter implementation** — slipped to
  v0.14.x or v0.15.x if not landed in v0.14.0.
- **`cross-network-peer` naming review** — slipped to
  v0.14.x if not landed in v0.14.0.
- **§5.3 rule 2 media-plane production hardening (FEC +
  codec + ARQ reliability)** — v0.14.x slip-or-land or
  later.
- **Composite `sfu-agent` Profile resolution** — pre-1.0
  decision window.
- **Adaptive JitterBuffer + Goog-CC full mainline BWE** —
  v1.x.
- **GM/T 国密 SRTP backend** — v1.x.
- **AddressSanitizer CI matrix** — v1.x.
- **Per-symbol inline stability annotation** — v0.14.x
  slip-or-land (the v0.13.0 sweep was at the file level;
  the per-symbol pass is a v0.14.x follow-up).
## [1.0.0-alpha.1] - 2026-10-14

### Status: Tech Preview

> **First pre-release of the v1.0.0 cycle.** Banner remains
> "Tech Preview" — the public API is **not yet frozen**. The
> "experimental through v1.0.0" banner is dropped at the
> v1.0.0 final tag, not at pre-releases.
>
> **Marketing alias:** the v1.0.0-alpha.1 Git tag is published as
> `v0.13.0` so the public version label stays anchored to the
> v0.x "Tech Preview" line (per `docs/plan/v1.0-plan.md` §8 + §12.2
> — pre-release split). The pre-release identifier carried by
> `NIMRTC_API_VERSION_PRE_RELEASE` is the string `"alpha.1"` and
> the corresponding header `[1.0.0-alpha.1]` heading documents
> the alpha.1 marker.

### Highlights

- **API stability framework** — `NIMRTC_API_VERSION_MAJOR=1 /
  MINOR=0 / PATCH=0 / PRE_RELEASE="alpha.1"` plus the
  `NIMRTC_API_STABLE` / `NIMRTC_API_UNSTABLE` /
  `NIMRTC_API_DEPRECATED(msg)` classification markers and
  `nimrtc::core::api_version::` namespace mirror
  (`docs/adr/ADR-015-api-stability-policy.md`). Every public
  header under `nimrtc/*` now carries a stability classification;
  `tests/test_api_classification` 2/2 PASS.
- **P1 UAF-DTLS type-system hardening** —
  `ITransportStack::dtls_weak()` interface lands in all three
  media-stack implementations (`WebRtcClassicStack`,
  `RawUdpArqStack`, `DefaultTransportStackFactory`); the engine
  migrates `Impl::dtls` + `Impl::datachannel_transport_cookie`
  from raw `IDtlsSession*` to `std::weak_ptr<IDtlsSession>`;
  14 use sites use `if (auto d = impl_->dtls.lock()) { ... }`
  that auto-expires on `close()`. The v0.12.0 P0 `is_open()`
  guards stay as defence-in-depth. New regression test
  `v013_dtls_weak_expires_after_close` in
  `tests/test_engine_plugin_loading.cpp`.
  P1 UAF-on-DTLS class is closed at the type-system level.
- **PAL Slice 1.5** — `pal::MakePluginOpts` named-arguments
  struct + C++20 `auto Resolver` template parameter overload on
  `pal::make_plugin` + 6 new `pal::resolve_*` forwarders
  (`resolve_bwe`, `resolve_scheduler`, `resolve_video_sink`,
  `resolve_video_source`, `resolve_video_receiver`,
  `resolve_video_sender`) + 9 `engine.cpp` plugin-load call
  sites migrated (`docs/adr/ADR-009-pal-slice-1.md` § "Slice
  1.5 follow-up"). Per-call-site arg count drops from 9-10
  positional to 5 positional + 1 struct. Behaviour-preserving:
  27/27 `test_engine_plugin_loading`, 15/15 `test_audio3a_tap`,
  13/13 `test_video_source` PASS on Windows MSVC.

### Added

- **`NIMRTC_API_VERSION` macros** (v1.0.0-alpha.1 / `docs/plan/v1.0-plan.md`
  §2.1 P0 item API-VERSION): `NIMRTC_API_VERSION_MAJOR/MINOR/PATCH`,
  `NIMRTC_API_VERSION_PRE_RELEASE`, `NIMRTC_API_VERSION_STRING` (combined
  "1.0.0"). The matching `nimrtc::core::api_version::kMajor/kMinor/kPatch/
  String/PreRelease` constants are exposed for runtime inspection.
  No public API change to any existing symbol.

- **`NIMRTC_API_STABLE` / `NIMRTC_API_UNSTABLE` markers**: visual
  documentation macros applied to public symbols. Empty expansion
  by default; no compile-time effect. The public header classification
  sweep (alpha.1 PR #3 → `15a47f1`) applies them without changing
  compilation semantics.

- **`NIMRTC_API_DEPRECATED(msg)` macro**: cross-compiler deprecation
  attribute. Expands to `[[deprecated(msg)]]` (C++14+),
  `__attribute__((deprecated(msg)))` (GCC/Clang), or
  `__declspec(deprecated(msg))` (MSVC). Emits the standard
  compiler warning on caller use.

- **`tests/test_api_version.cpp`** (5/5 subtests): verifies the
  macros + namespace constants are well-formed. Compile-time
  smoke for `NIMRTC_API_STABLE/UNSTABLE/DEPRECATED` expansion.

- **`tests/test_api_classification.cpp`** (2/2 subtests; landed as
  test-first design in `6cea40f` with intentional FAIL on
  `every_header_has_at_least_one_marker`; the FAIL flips to PASS
  after the bulk sweep `15a47f1`). The `count_markers()` walker
  runtime-checks every header under `src/<mod>/include/nimrtc/<mod>/`
  (excluding `src/third_party/`) for ≥1 `NIMRTC_API_STABLE` /
  `NIMRTC_API_UNSTABLE` occurrence.

- **`ITransportStack::dtls_weak()`** — new
  `std::weak_ptr<dtls::IDtlsSession>` accessor in
  `src/transport/include/nimrtc/transport/transport_stack.hpp`
  (9f4956b). The existing raw `dtls()` reference getter stays
  with a `[[deprecated]]` comment. `WebRtcClassicStack::dtls_`
  migrates from `std::unique_ptr` to `std::shared_ptr` so the
  stack can hand out a `weak_ptr` outliving the stack's own
  reference; `RawUdpArqStack::dtls_weak()` returns an empty
  `weak_ptr` (no DTLS in the control plane); the default factory
  records the stack for later weak_ptr resolution.

- **Regression test `v013_dtls_weak_expires_after_close`** in
  `tests/test_engine_plugin_loading.cpp` (`d0665aa`). Asserts
  the core v0.13.0 invariant: after `stack->close()`, the
  previously-non-expired `weak_ptr` is expired; `.lock()`
  returns `nullptr`. Guards against future regressions where
  `Engine::Impl::dtls weak_ptr` could silently hold a
  "stuck non-null" weak pointer that `.lock()`s into freed
  memory.

- **`pal::MakePluginOpts` named-arguments struct** in
  `src/engine/include/nimrtc/engine/pal/make_plugin.hpp`
  (`5eb65fa`). Replaces 4 positional error/mode args + trailing
  `on_error` in `pal::make_plugin`. Designated initializers
  make per-category failure policy (Error / Warn / Silent)
  visible at a glance:
  ```cpp
  pal::make_plugin<&pal::resolve_bwe>(
      config_.bwe_name, bwe_, config_.bwe_config,
      {.err_open_failed = core::kEngineInternal,
       .err_missing = core::kEngineInternal,
       .open_fail_mode  = pal::PluginFailureMode::Warn,
       .missing_mode    = pal::PluginFailureMode::Warn,
       .on_error        = on_error_});
  ```

- **C++20 `auto Resolver` template overload on
  `pal::make_plugin`** — `make_plugin<auto Resolver, IPlugin,
  Config>(id, slot, cfg, opts, post_create)` deduces the factory
  type from `std::invoke_result_t<Resolver, std::string_view>`.
  The factory lookup (was: `reg.get_X(id)` at each call site)
  moves inside the helper.

- **6 `pal::resolve_*` forwarders** in
  `src/engine/include/nimrtc/engine/pal/engine_plugin_resolver.hpp`:
  `resolve_audio3a`, `resolve_codec`, `resolve_bwe`,
  `resolve_scheduler`, `resolve_video_codec`,
  `resolve_video_sink`, `resolve_video_source`,
  `resolve_video_receiver`, `resolve_video_sender`,
  `resolve_datachannel`. The header is now symmetric across
  every plugin category the engine uses.

- **`tests/test_engine_plugin_loading.cpp` Slice 1.5 verification
  table** — 27/27 + 15/15 + 13/13 subtests PASS on Windows MSVC
  (`test_engine_plugin_loading`, `test_audio3a_tap`,
  `test_video_source` respectively).

- **ADR-009 Slice 1.5 section** in
  `docs/adr/ADR-009-pal-slice-1.md`. Documents the
  MakePluginOpts + Resolver + 6 forwarders design and the
 9-call-site migration plan.

- **`scripts/clean_build.ps1`** — MSVC build-artifact cleaner
  invoked at tag prep (`4c45a41`). Removes stale
  `vcxproj.user` + `.cache` files from the build tree.

- **`docs/plan/v0.13-plan.md`** — milestone plan for the
  v0.13.0 = v1.0.0-alpha.1 cut. Item-by-item dependency
  graph, PR sequencing, DoD gates, and risk table.

### Changed

- **`nimrtc::engine::pal::make_plugin<IPlugin, Config, Factory>`**
  template at `src/engine/include/nimrtc/engine/pal/make_plugin.hpp`
  (`5eb65fa`). Consolidates the 5-step "factory lookup → create →
  null-check → open → report" dance that was copy-pasted across
  9 plugin categories in `engine.cpp` (5 video plugins + 2
  BWE/Scheduler + audio3a + codec). Per-block size dropped from
  ~20 lines to ~7 lines with no behaviour change.
    - `PluginFailureMode` enum (Error / Warn / Silent) preserves
      each category's failure-reporting policy (video →
      on_error_, BWE → log-warn, audio3a → silent fallback,
      codec → silent fallback).
    - `post_create` hook is invoked between `create()` and
      `open()` to cover the audio3a case where `set_callbacks()`
      must precede `open()`.
    - `if constexpr` branch handles the no-config case
      (`IAudio3AFactory::create()` takes no arguments; the
      caller passes `nullptr` as the config argument).
    - `init_video_plugins()` lost the `auto& reg = ...` capture
      and the `reg.get_video_*` calls; the registry lookups now
      happen inline via `pal::make_plugin<&pal::resolve_*>(...)`.

- **All 9 `engine.cpp` plugin-load call sites** use the new
  template overload
  `pal::make_plugin<&pal::resolve_*>(id, slot, cfg, opts)`:
    - BWE              (line 408, `<&pal::resolve_bwe>`)
    - Scheduler        (line 423, `<&pal::resolve_scheduler>`)
    - Audio3A          (line 758, `<&pal::resolve_audio3a>`,
                          `nullptr` config + lambda post_create)
    - Codec            (line 796, `<&pal::resolve_codec>`,
                          `std::move(codec_cfg)` + Silent policy)
    - Video Sink       (line 1095, `<&pal::resolve_video_sink>`)
    - Video Source     (line 1113, `<&pal::resolve_video_source>`)
    - Video Receiver   (line 1136, `<&pal::resolve_video_receiver>`)
    - Video Sender     (line 1157, `<&pal::resolve_video_sender>`)
    - Video Codec      (line 1180, `<&pal::resolve_video_codec>`)
  Each call site is `(void)pal::make_plugin<...>(...)` to
  silence `-Werror=unused-result` (`[[nodiscard]]` return).
  The 2 `reg.get_transport_stack` calls in
  `init_transport_session()` are intentionally left as
  `reg.get_*()` (not migrated) because they belong to the
  v0.12.0 Slice 7.6 PR3 transport-strategy wiring, not to
  the per-plugin factory loading pattern PAL Slice 1 covers.

- **`ITransportStack::dtls()` raw reference getter** carries a
  `[[deprecated]]` migration hint comment ("use `dtls_weak()`
  for the v0.13.0 weak_ptr surface"). The getter itself remains
  functional for callers that already hold a non-owning raw
  pointer.

- **`NimRTCEngine::Impl::dtls`** + **`Impl::datachannel_transport_cookie`**
  retyped from `nimrtc::dtls::IDtlsSession*` to
  `std::weak_ptr<nimrtc::dtls::IDtlsSession>` in
  `src/engine/src/engine.cpp` (`313b40c`). The capture site
  (`init_modules_once()`) reads
  `impl_->dtls = impl_->session.media_stack->dtls_weak();`.
  All 14 engine use sites use
  `if (auto d = impl_->dtls.lock()) { d->X(); }` (P1 UAF
  Mitigation #4 per `docs/plan/p1-uaf-dtls-investigation.md`).
  The Medium-rated `Impl::datachannel_transport_cookie`
  risk is now closed (was listed as deferred-to-v1.x in
  v0.11-plan.md §2.2 follow-up).

- **Public header classification sweep** (`15a47f1`,
  `feat(public-headers): bulk API-CLASSIFY sweep`).
  85 of 89 public headers annotated with
  `NIMRTC_API_STABLE` (header-level marker; per-symbol
  inline is a v0.13.x follow-up). 4 headers were already
  marked in the alpha.1 pilot (`6cea40f`):
  `engine.hpp`, `engine/pal/make_plugin.hpp`,
  `transport_plugin.hpp`, `core/api_version.hpp`. The sweep
  adds header-level markers to the remaining 85 (transport /
  raw_udp / sctp / dtls / datachannel / modules / core —
  89 total). `tests/test_api_classification` 2/2 PASS
  post-sweep (`headers=89 stable=107 unstable=6
  deprecated=8 missing_markers=0`).

- **`docs/plan/p1-uaf-dtls-investigation.md`** §0 resolution
  status table updated to record the v0.13.0 weak_ptr
  mitigation as completed (P1 closed); §8 7/7 open
  questions answered (`e79a1eb`).

- **`CHANGELOG.md`** `[1.0.0-alpha.1]` section finalised at
  tag cut. **[Unreleased]** reset to a minimal pointer to
  `docs/plan/v0.13-plan.md`.

### Notes

- No public API change. All existing v0.12.0 callers continue
  to compile and link without modification. `git diff
  v0.12.0..HEAD -- src/engine/include/**` shows zero lines;
  the engine surface is byte-identical with v0.12.0.
- v1.0.0 pre-release sequence per `docs/plan/v1.0-plan.md` §12.
  Next pre-release: v1.0.0-alpha.2 (WebTransport seam,
  msquic IWebTransportSession).
- Banner remains "Tech Preview" through v1.0.0-beta.x. The
  "experimental through v1.0.0" line is dropped only at the
  v1.0.0 final tag.
- The `v0.13.0` Git tag and the `v1.0.0-alpha.1` pre-release
  marker are the same commit. The semver version (`v0.13.0`)
  is the canonical Git tag; the pre-release identifier is in
  the tag message body and the `NIMRTC_API_VERSION_PRE_RELEASE`
  macro.

---
## [0.12.0] - 2026-10-13

### Status: Tech Preview

> **Tagged 2026-10-13.** Platform expansion + GMSSL TLCP DTLS
> backend (ADR-013) + OpenHarmony aarch64 toolchain (Stage 0) +
> T-PAL Slice 7 / 7.6 / 8 named use-case transport strategies +
> Profile schema v1.1 (additive `transport` section) +
> PAL Slice 1.5 (`pal::make_plugin` template + 9 call sites) +
> DTLS Handshake reassembly shim (Chrome Case D) +
> UAF guards on `on_transport_recv` / `process_remote_sdp`.
> **API still experimental / not for production through v1.0.0.**
>
> 37 commits since the v0.11.0 cut (`3ae98f3` on 2026-10-09).
> See `docs/plan/v0.12.0-release-notes.md` for the full
> commit split (22 v0.12.0-delta + 14 v0.11.x patch train + 1
> merge).
>
> **Reconciled 2026-10-16:** the duplicate `[0.12.0] - Unreleased`
> section that previously appeared above (and the orphan
> `[0.13.0] - Unreleased` block, plus the empty `[Unreleased]`
> pointer) have been merged into this section per Keep a
> Changelog 1.0.0 — see `Fixed` (UAF guards + reassembler shim)
> and `Changed` (PAL Slice 1.5 + datachannel cookie) for the
> content that was lifted from the duplicate.

### Highlights

- **Transport Slice 7.6 PR3 — end-to-end wiring of the 5 transport
  strategies** (engine-side composition root in
  `NimRTCEngine::init_modules_once()`; pre-PR3 manual ICE+DTLS
  stitching removed). `NimRTCEngine::ice_t_` and `Impl::dtls`
  migrate to raw non-owning pointers into
  `Impl::session.media_stack` (lifetime safe because
  `~NimRTCEngine` runs `close()` before `delete impl_`). Rule 2
  (`teleop-ultra-low-latency`) takes a dedicated
  no-DTLS-by-design branch: `Impl::dtls` is null, the engine
  skips `dtls->open()`, and the audio pipeline falls back to
  null-encoder. The 4 browser-interop strategies
  (`browser-call`, `agent-gateway`, `sfu-relay`,
  `cross-network-peer`) use `webrtc-classic` for the media stack;
  missing DTLS factory hard-fails with `kEngineInternal`.
- **T-PAL Slice 7 / 7.6 / 8 — Five named use-case transport
  strategies** (`docs/plan/transport-strategies.md`, commits
  `75b4f79` + `744d9d1` + `5f18e1a`): `browser-call`,
  `teleop-ultra-low-latency`, `agent-gateway`, `sfu-relay`,
  `cross-network-peer`. Each strategy is a small `TransportRequirements`
  constant in `src/transport/include/nimrtc/transport/transport_strategy.hpp`
  plus a `strategy_by_name()` lookup that the Profile loader
  consumes. Strategies 1–3 map to one of the four `transport-selection.md`
  §5.3 Selector rules; `cross-network-peer` is a new combination
  (rule 1 with a tighter `max_p99_ms = 150`) that §5.3 does not
  enumerate but is consistent with it.
- **Profile schema v1.1 (additive `transport` section)**
  (commit `ac4fad9`): `profiles/schema/profile-v1.1.json` adds
  a top-level `transport` key with `{strategy, media{...},
  control{...}}` sub-blocks. v1.0 profiles continue to load;
  missing `transport` defaults to `null` (= engine's
  compile-time default = `browser-call`). All nine existing
  Profile JSONs are retro-fitted with a default
  `transport.strategy` value (see transport-strategies.md §3).
- **Engine integration + Profile bridge** (commit `5f18e1a`,
  PR3): `EngineConfig::transport_strategy_name` (string, optional)
  added alongside `EngineConfig::transport_name`. The engine
  now (a) resolves the strategy name to a `TransportRequirements`
  via `strategy_by_name()`, (b) calls
  `CapabilitySelector::select()` to get a `TransportSession`
  with both `media_factory_id` and `control_factory_id`, and
  (c) records the chosen session in the engine's `open()` result
  for diagnostic + e2e-test inspection. **The factory is selected
  but not yet instantiated** — the existing
  `cfg.transport_name`-driven media-stack construction path
  remains in effect. Real instantiation of the strategy's
  chosen factory id is a v0.13+ change.
- **`raw-udp-arq` control stack registered** (commit `744d9d1`,
  PR2): the first non-`webrtc-classic` control-plane factory
  under `core::PluginRegistry::register_transport_stack()`,
  id `"raw-udp-arq"`. Used by `teleop-ultra-low-latency` and
  `agent-gateway` to carry the high-frequency control channel
  independent of the media plane.
- **GMSSL TLCP DTLS backend (ADR-013)** (commits
  `8d60e23` + `b2af1c0` + `e9faa7b` + `660610d`):
  `DtlsSessionGmSSL` wraps GMSSL v3.x's GM/T 0044 (TLCP)
  handshake + SM2/SM3/SM4 certificates + SM4-GCM encryption.
  Registered as id `"gmssl"` in
  `core::PluginRegistry::register_dtls_session()`. Engine
  default remains wolfSSL (zero regression); OHOS profile
  defaults to GMSSL. ~900 LOC in `dtls_gmssl_session.cpp` +
  `dtls_gmssl_factory.cpp`. `NIMRTC_ENABLE_DTLS_GMSSL=OFF` by
  default.
- **OpenHarmony aarch64 CMake toolchain (Stage 0)** (commit
  `bfeffd5`): dual-mode `cmake/toolchains/ohos-aarch64.cmake`
  (OHOS SDK clang cross-compile + `--target=aarch64-linux-ohos`
  fallback), six OHOS presets in `CMakePresets.json`, and a
  `cmake/NimRTCOptions.cmake` `if(OHOS)` musl-compat branch.
  Stages 1–4 (toolchain validation + e2e audio capture + CI
  green + OHAudio HAL implementation) remain in-progress.
- **OHAudio HAL decision** (`docs/plan/ohos-audio-hal.md`):
  OHAudio selected as the audio HAL for OHOS; OpenSL ES
  rejected (deprecated). Implementation is a v0.12.x patch
  follow-up.

### Added

- **`src/transport/include/nimrtc/transport/transport_strategy.hpp`**
  — five `kStrategy*` constants + `strategy_by_name()`
  lookup. Round-trip contract with `profile-v1.1.json`
  asserted by
  `test_transport_strategy.StrategyNamesMatchProfileSchemaEnum`.
- **`src/transport/src/raw_udp_arq_stack.cpp`** + matching
  `RawUdpArqStackFactory` (id `"raw-udp-arq"`).
- **`profiles/schema/profile-v1.1.json`** — additive
  `transport` section.
- **`src/dtls/src/dtls_gmssl_session.cpp`** +
  **`src/dtls/src/dtls_gmssl_factory.cpp`** + matching
  `NIMRTC_ENABLE_DTLS_GMSSL` CMake option (default OFF).
- **`cmake/toolchains/ohos-aarch64.cmake`** + 6 OHOS presets
  in `CMakePresets.json`.
- **2 new `EnginePluginLoading` subtests** in
  `tests/test_engine_plugin_loading.cpp`: Slice 7.6 PR3
  (engine reads `cfg.transport_strategy_name` + resolves to
  `TransportSession` with both factory ids) + Slice 7.6 PR2
  (`raw-udp-arq` factory registered + lifecycle).
- **New tests**: `tests/test_transport_strategy.cpp` (11/11
  PASS) covering all 5 strategy constants + the
  `strategy_by_name()` lookup + the Profile v1.1 round-trip.
- **`nimrtc::transport::make_transport_instance(media_factory_id,
  control_factory_id, cfg)`** — canonical engine-facing entry
  point for the v0.12.0 transport-strategy layer. Translates the
  Selector's resolved factory ids into a fully-constructed
  `TransportSession`. Lives in `transport_plugin.cpp` for future
  consumers (test harnesses, webrtc-quic stack factory); the
  engine inlines the equivalent lookup so its existing
  `nimrtc::transport` PRIVATE link is sufficient.
- **6 new subtests in `test_engine_plugin_loading.cpp`** for the
  5 strategies + unknown-strategy failure mode:
  `slice76_strategy_default_is_browser_call`,
  `slice76_strategy_agent_gateway`
  (webrtc-classic + raw-udp-arq control),
  `slice76_strategy_teleop_ultra_low_latency`
  (raw-udp-arq media + raw-udp-arq control),
  `slice76_strategy_sfu_relay` (webrtc-classic, no control),
  `slice76_strategy_cross_network_peer` (webrtc-classic, no control),
  `slice76_unknown_strategy_fails_open` (`kEngineInternal` guard),
  and `slice76_accessors_empty_before_open` (diagnostic surface
  contract).
- **New documentation**: `docs/guides/building-on-openharmony.md`,
  `docs/guides/switching-dtls-backend.md`,
  `docs/api/engine_api.md`, `docs/api/engine_config.md`,
  `docs/plugin_author_guide.md`, `docs/troubleshooting.md`,
  `examples/demo-agent-gateway/README.md`,
  `docs/plan/transport-strategies.md`,
  `docs/plan/ohos-audio-hal.md`.

### Changed

- **`NimRTCEngine::Impl::dtls`** changed from
  `unique_ptr<nimrtc::dtls::IDtlsSession>` to a raw non-owning
  pointer. The DTLS session is now owned by
  `WebRtcClassicStackFactory::create()` inside the media stack.
  See `Impl::dtls` field comment for lifetime rationale.
- **`NimRTCEngine::ice_t_`** changed from
  `unique_ptr<plugins::IICETransport>` to a raw non-owning
  pointer. The ICE transport is now owned by the media stack.
- **`NimRTCEngine::close()`** now calls `Impl::session.close()`
  (which tears down the media and control stacks in order)
  before nulling `ice_t_` and `Impl::dtls`.
- **`init_modules_once()` transport section** removed the
  pre-PR3 manual ICE + DTLS construction path; the section now
  resolves the §5.3 factory pair from the Selector, calls
  `get_transport_stack()` on each id, and builds the session.
- **`src/engine/src/engine.cpp`** — `init_modules_once()`
  now recognises `cfg.transport_strategy_name` (optional,
  default `""` → `"browser-call"`) and resolves it to a
  `TransportSession` via `strategy_by_name()` +
  `CapabilitySelector::select()`. The chosen `media_factory_id`
  and `control_factory_id` are recorded in the engine's
  `open()` result; instantiation continues to use the
  pre-PR3 `cfg.transport_name`-driven direct-constructor path.
- **`src/modules/assembly/profiles.cpp`** — Profile loader
  now reads the v1.1 `transport` section and bridges the
  declared strategy id + `media` / `control` sub-blocks to
  a `TransportRequirements`. v1.0 profiles (no `transport`
  key) default to `transport=null` → compile-time default.
- **`src/transport/src/transport_plugin.cpp`** —
  `do_register_default_plugins()` now publishes
  `"raw-udp-arq"` (PR2) in addition to the existing
  `"default"` and `"webrtc-classic"` registrations.
- **`dtls::register_default_plugins()`** — now publishes
  `GmSSLDtlsFactory` through `register_dtls_session("gmssl")`
  when `NIMRTC_ENABLE_DTLS_GMSSL=ON`; default `OFF` keeps
  wolfSSL-only behaviour.
- **`cmake/NimRTCOptions.cmake`** — added `if(OHOS)`
  musl-compat branch + `nimrtc_link_gmssl()` helper.
- **PAL Slice 1.5 — `nimrtc::engine::pal::make_plugin<IPlugin,
  Config, Factory>` template** added at
  `src/engine/include/nimrtc/engine/pal/make_plugin.hpp`. The
  helper consolidates the 5-step "factory lookup → create →
  null-check → open → report" dance that was copy-pasted across
  9 plugin categories in `engine.cpp` (5 video plugins + 2 BWE /
  Scheduler + audio3a + codec). Per-block size dropped from
  ~20 lines to ~7 lines with no behaviour change.
  - The helper takes a `PluginFailureMode` enum (Error / Warn /
    Silent) so each plugin category keeps its original
    failure-reporting policy (e.g. video → on_error_, BWE →
    log-warn, audio3a → silent fallback, codec → silent
    fallback).
  - A `post_create` hook is invoked between `create()` and
    `open()` to cover the audio3a case where `set_callbacks()`
    must precede `open()`.
  - A compile-time branch (`if constexpr`) handles the
    no-config case (`IAudio3AFactory::create()` takes no
    arguments) — the caller passes `nullptr` as the config
    argument.
  - `init_video_plugins()` lost the `auto& reg = ...` capture
    and the `reg.get_video_*` calls; the registry lookups now
    happen inline at each `pal::make_plugin` call site.
- **PAL Slice 1.5 upgrade — `pal::make_plugin` now has a C++20
  `auto Resolver` overload and a `MakePluginOpts` named-arguments
  struct**. The 4 positional `err_open_failed` / `err_missing` /
  `open_fail_mode` / `missing_mode` arguments (and the trailing
  `on_error`) are folded into a single `MakePluginOpts` struct
  consumed by designated initializers at the call site. The new
  template overload
  `make_plugin<auto Resolver, IPlugin, Config>(id, slot, cfg, opts, post_create)`
  takes a resolver function pointer (e.g. `&pal::resolve_bwe`)
  and deduces the factory type from
  `std::invoke_result_t<Resolver, string_view>`; the
  `reg.get_X(id)` lookup now happens inside the helper. All 9
  `engine.cpp` plugin-load call sites (BWE / Scheduler / Audio3A
  / Codec / Video Sink / Source / Receiver / Sender / Codec)
  migrated — per-call-site arg count drops from 9-10 positional
  to 5 positional + 1 struct. 6 new `pal::resolve_*` forwarders
  added to `engine_plugin_resolver.hpp` (bwe, scheduler,
  video_sink, video_source, video_receiver, video_sender) so the
  resolver header is now symmetric across all 9 plugin
  categories used by the engine. Behaviour-preserving: 27/27
  `test_engine_plugin_loading`, 15/15 `test_audio3a_tap`, 13/13
  `test_video_source` PASS on Windows MSVC. The
  `reg.get_transport_stack` calls in `init_transport_session()`
  are intentionally left as `reg.get_*()` (not migrated) because
  they are part of the v0.12.0 Slice 7.6 PR3 transport-strategy
  wiring, not the per-plugin factory loading pattern that PAL
  Slice 1 covers.
- **`NimRTCEngine::Impl::datachannel_transport_cookie`**
  retyped from `void*` to `nimrtc::dtls::IDtlsSession*` (non-
  owning). The v1.x WebTransport redesign will replace the
  field with a tagged union over `{IDtlsSession*,
  IWebTransportSession*}`; until then, the typed pointer
  removes the unsafe `static_cast<void*>` round-trip.
- **Meta-comments removed** from `core/registry.hpp` and three
  production headers (`engine.hpp`, `engine.cpp`,
  `raw_udp_datagram.hpp`). Production code no longer mentions
  per-subagent ownership boundaries (those live in PR
  descriptions / changelogs).

### Fixed

- **DTLS 1.2 forced for Case D Chrome interop** (commit `36ee838`,
  cherry-picked from `fix/windows-dll-boundary`): `dtls_wolfssl_session.cpp`
  no longer negotiates DTLS 1.3 by default. wolfSSL 5.9.2's
  DTLS 1.3 server has a broken short-circuit that (a) requires a
  `key_share` extension up front on a 1.2-style ClientHello from a
  1.3-capable client and emits `missing_extension` (this is what
  82ca051 disables DTLS 1.3 to avoid in the in-tree unit tests).
  v0.12.0 forces DTLS 1.2 on both client and server paths so the
  in-tree unit tests stop depending on the broken 1.3 short-circuit.
  Tracked under `docs/plan/dtls_chrome_interop_review.md`.  **Note**:
  this commit does *not* restore the NimRTC ↔ Chrome e2e (Case D)
  flow — see Known Issues below.
- **Use-after-free guard in `NimRTCEngine::on_transport_recv()`**
  (`src/engine/src/engine.cpp`, around line 2286). Added
  `if (!is_open()) return;` at function entry. The ICE recv
  callback runs on the transport thread and can race with
  `close()`: a buffered packet may arrive after `close()` has
  torn down `Impl::dtls` (a raw non-owning pointer into
  `session.media_stack`), causing a use-after-free in
  `impl_->dtls->state()` / `feed_inbound()`. Mirrors the existing
  `send_audio()` pattern (line 2119). **Severity: Critical**
  (per `docs/plan/p1-uaf-dtls-investigation.md`).
- **Use-after-free guard in `NimRTCEngine::process_remote_sdp()`**
  (`src/engine/src/engine.cpp`, around line 1699). Added
  `if (state_ != State::kOpen) return std::nullopt;` at function
  entry. Same race window: a delayed signaling message after
  `close()` would deref `impl_->dtls->set_role()` /
  `set_peer_fingerprint()`. Mirrors the existing DataChannel
  guard pattern (line 1341). **Severity: High**.
- **Note:** `NimRTCEngine::tick()` (line 2188) already had the
  `is_open()` guard and required no change. The
  `Impl::datachannel_transport_cookie` lifetime (Medium risk)
  is deferred to the v1.x wiring PR (per P1 investigation).
- **Follow-up:** `Impl::dtls` migration from raw pointer to
  `std::weak_ptr<IDtlsSession>` is **P1 for v0.13.0** (not
  v0.12.0). The two `is_open()` / `state_` guards above close
  the known Critical and High paths; the `weak_ptr` migration
  eliminates the entire UAF class.
- **DTLS Handshake-layer fragment reassembly shim**
  (`src/modules/dtls/include/nimrtc/dtls/dtls_handshake_reassembler.hpp`,
  `src/modules/dtls/src/dtls_wolfssl_session.cpp`). Closes
  the NimRTC ↔ Chrome (BoringSSL) Case D interop failure that
  was tracked under the v0.12.0 "Known Issues" bullet below.
  wolfSSL 5.9.2's DTLS 1.2 server path does not reassemble
  fragmented Handshake records (the `WOLFSSL_DTLS_CH_FRAG`
  switch is gated on `WOLFSSL_DTLS13 && WOLFSSL_HAVE_MLKEM`,
  and we pin DTLS 1.2). Chrome 117+ always splits its first
  ClientHello flight into two records (~1100 B + ~300 B), so
  wolfSSL silently dropped the second fragment and never
  produced a HelloVerifyRequest / ServerHello back. The shim
  intercepts each inbound Handshake record via
  `DtlsHandshakeReassembler::try_reassemble()`, buffers
  fragments keyed by `msg_seq`, and synthesises a fresh
  unfragmented DTLS record once the full body has been
  received. Partial buffers are GC'd after 5 s
  (`set_gc_ttl()`, well inside the RFC 6347 §4.2.4 60-s
  retransmit envelope) so a peer that crashes mid-flight
  cannot leak. `nimrtc_chrome.keylog` now populates and SRTP
  keys derive normally. New tests:
  - `tests/test_dtls_handshake_reassembler.cpp` (unit
    coverage: unfragmented pass-through, two-fragment
    reassembly, three-fragment reassembly with reverse
    arrival order, mid-stream msg_seq change, malformed
    header, GC TTL eviction, defensive upper bounds).
  - `tests/test_dtls_replay_chrome_hello.cpp` (end-to-end
    replay of a captured Chrome 117+ ClientHello flight
    against the wolfSSL server session; asserts the
    ServerHello path now completes).
  The DTLS 1.3 short-circuit (commit `36ee838`) and the
  `NIMRTC_SKIP_CASE_D=1` escape hatch remain in place as
  belt-and-braces — the env override is no longer required
  for this PR. **Severity: Critical** (Case D was the last
  hard gate blocking Chrome interop on `main`).

### What's NOT in v0.12.0 (explicitly deferred)

- **§5.3 rule 2 actual media-stack instantiation** —
  `teleop-ultra-low-latency` is **selected** (PR3 records
  `media=raw-udp-arq, control=raw-udp-arq` on the
  `TransportSession`) but the engine still constructs the
  media stack through the pre-PR3 `cfg.transport_name`-driven
  direct-constructor path. Wiring the strategy's chosen
  factory id into a real `MakeTransportInstance()` call is
  a v0.13+ change. Tracked under
  `docs/plan/transport-strategies.md` §6 Open questions.
- **`max_p99_ms` budget enforcement** — Selector **records**
  the budget on the `TransportSession` for e2e-test /
  monitoring consumption, but does not enforce it at engine
  `open()` time. A `StrategyValidator` is v0.13+ scope.
- **`cross-network-peer` naming review** — strategy id is
  descriptive but long; alternatives (`robust-peer`,
  `lossy-link`, `mobile-friendly`) are tracked under
  `docs/plan/transport-strategies.md` §6 #1.
- **Composite `sfu-agent` resolution** — the `sfu-agent`
  Profile JSON declares both SFU and Agent strategies on the
  same engine; current loader cannot produce two
  `TransportSession`s for one engine. Tracked under
  `docs/plan/transport-strategies.md` §6 #2, deferred to
  the pre-1.0 decision window.
- **OHOS-aarch64 end-to-end audio capture** — OHAudio
  adapter implementation is a v0.12.x patch.
- **OHOS ↔ Chrome / Firefox interop** — TLCP is not
  supported by any browser; OHOS ↔ NimRTC-Desktop uses
  wolfSSL path.
- **GMSSL SRTP backend** — GM/T 0044 SRTP profile is P4;
  SRTP continues to use wolfSSL.
- **usrsctp on OHOS** — deferred to v0.12.x patch;
  OHOS uses `SctpStubFactory` (id `"stub"`) for now.
- **T-PAL Slice 6** (Raw UDP bypass) — v0.11.x patch.
- **SFU-1 / DC-3 / BENCH-1** — pre-1.0 decision window per
  `docs/plan/v0.11-plan.md` §2.2.

### Known Issues (v0.12.0)

- **NimRTC ↔ Chrome DTLS handshake is broken on wolfSSL 5.9.2**
  (Case D in `tools/run_e2e_acceptance.py`). ~~The DTLS 1.2
  force-pin landed in 36ee838 prevents the wolfSSL DTLS 1.3
  server's broken short-circuit from engaging, but the **DTLS
  1.2 server path has the same fragment-reassembly bug** in
  5.9.2~~ **Resolved in this release** by the
  `DtlsHandshakeReassembler` shim (see "Fixed" above):
  Chrome 117+'s two-record ClientHello flight is now
  reassembled before wolfSSL sees it, the wolfSSL server
  produces a HelloVerifyRequest / ServerHello back, and
  `nimrtc_chrome.keylog` populates normally so SRTP keys
  derive. The DTLS 1.3 short-circuit (commit `36ee838`) and
  the `NIMRTC_SKIP_CASE_D=1` escape hatch remain in place as
  belt-and-braces; the env override is **no longer required**
  to merge. CI artifact from run 38201352429 is preserved as
  the historical "before" trace; the new
  `test_dtls_replay_chrome_hello` covers the "after" path.
  Tracked under `docs/plan/dtls_chrome_interop_review.md`;
  the longer-term DTLS backend swap (mbedTLS or BoringSSL)
  remains v0.12.x work but is no longer on the Chrome-interop
  critical path.
- **macOS arm64 `video_source_tests::MemorySource.CadenceProducesApproxFps`**
  is timing-sensitive (asserts on `std::chrono` deltas inside a
  1-second wall-clock window).  Push CI run 38201349751 saw it
  fail once; main has not regressed.  Tracked under
  `docs/plan/transport-selection.md` §6.5 follow-up (test
  hardener).  Re-run usually clears it.

### Notes

- **Public API**: byte-identical with v0.11.0 except two
  additive fields on `EngineConfig`: `dtls_name` (string,
  default `""` → `"wolfssl"`) and `transport_strategy_name`
  (string, default `""` → `"browser-call"`).
- **ABI**: Not committed; experimental through v1.0.0.
- **Profile schema**: v1.1 is purely additive (no field
  removal / rename). v1.0 profiles continue to load.
- **Strategy C++ surface**: five `kStrategy*` constants +
  `strategy_by_name()` lookup. The constants are `inline
  const` (not `constexpr`) because `TransportRequirements`
  contains `std::optional<std::string>`, which is not a
  literal type in C++17/20.
- **Commit count**: `git log v0.11.0..HEAD --oneline` is
  currently **24 commits** as of the CI escape-hatch
  cherry-pick (`4e91942`). The **10 v0.12.0-delta
  commits** are: `75b4f79` (transport-strategies.md),
  `ac4fad9` (Profile v1.1 schema), `744d9d1` (Slice 7.6
  PR2), `5f18e1a` (Slice 7.6 PR3), `4aaf74c` (alignment),
  `36ee838` (force DTLS 1.2 cherry-pick), `4bbebec`
  (record cherry-pick), `8225451` (correct commit count →
  21), `0797393` (correct commit count → 22), and
  `4e91942` (cherry-pick `NIMRTC_SKIP_CASE_D` CI escape
  hatch from `fix/windows-dll-boundary`). The remaining
  14 commits are v0.11.x stabilization fixes (DTLS Chrome
  interop + macOS/Linux CI 30s timeouts) that landed
  between the v0.11.0 cut and HEAD. Note: `bfeffd5
  feat(v0.12.0): stage-3 — GMSSL TLCP backend, OHOS
  stage-3 docs, OHAudio HAL decision` is **NOT** a
  v0.12.0 delta — it landed on main on 2026-09-22, 17
  days before the v0.11.0 cut (`3ae98f3` on
  2026-10-09), and is part of the v0.11.0 baseline. After
  `4e91942`, two follow-ups land on main: `407002d`
  (PowerShell 5.1 backtick-parsing fix in the Step
  Summary) and `822cf75` (this commit-count note
  update). Release manager decides the v0.11.x patch
  train vs v0.12.0 split for the 14 stabilization fixes
  at tag time.
- **4-platform CI**: windows + linux-gcc + macos-clang ✅
  (linux-aarch64 TEMP-DISABLED). v0.12.0 release manager
  verifies the 5 new `test_transport_strategy` subtests +
  5 new `test_engine_plugin_loading` Slice 7.6 subtests
  pass on all three active platforms.
- **GMSSL builds**: `NIMRTC_ENABLE_DTLS_GMSSL=ON` requires
  GMSSL v3.x headers + library on the build host. Linux
  x86_64 and OHOS-aarch64 are the supported hosts.
- **国密后端** (SM2/SM4) — GMSSL is the v0.12.0 deliverable
  for the GM/T 0024 / 0044 path; 国密 SRTP remains P4.
- **No binary artefacts shipped** (源码为主).
- **No new third-party dependencies** for the default
  (wolfSSL) build.

---
## [0.11.0] - 2026-10-09

### Status: Beta 前哨

> **Tag cut 2026-10-09.** First minor to ship P2 content. P1
> closed at v0.10.3. **API still experimental / not for production
> through v1.0.0.**
>
> **The headline data-plane item (DataChannel Chrome ↔ NimRTC
> interop, DC-1) is NOT in v0.11.0.** The SCTP-over-DTLS physical
> wiring (set_dtls_keys() + DTLS↔SCTP inbound/outbound hookup)
> never landed on `main`; Chrome-headless e2e stays 0/4. v0.11.0
> ships the **Chrome media interop** surface (audio + video Case D,
> all four platforms) and the L2 / L3 P2 scaffolding (TAP-1,
> PROFILE-1, DEMO-1, TPAL-4 cleanup), with the data plane
> explicitly deferred to the **v1.x WebTransport / QUIC**
> re-design. See `docs/plan/v0.11-plan.md` §2.2 (DC-1 / DC-2 /
> TPAL-5 deferred) and §10 (forward direction).

### Highlights

- **Chrome media interop (audio + video Case D)** — NimRTC ↔ Chrome
  audio Opus + video H.264 end-to-end via ICE + DTLS 1.3 + SRTP +
  RTP. `python tools/run_e2e_acceptance.py --case d` PASS on
  Windows + Linux x86_64. Verified across the four-platform CI
  matrix (windows-2022 + linux-gcc + macos-clang + linux-aarch64).
  This is the v0.11.0 release contract for media interop.
- **TAP-1 — PCM tap on WebRTC APM** —
  `IAudio3A::set_pre_process_tap()` and
  `set_post_process_tap()` (RFC 001 §6.1–6.5) are now live on the
  default `PluginAdapter` implementation. The WebRTC-APM-backed
  adapter fires pre and post 3A taps per `process_capture()` call,
  so AI-Agent / wake-word / streaming-ASR consumers can subscribe
  to the raw mic PCM (pre) and the cleaned PCM (post) without an
  extra DSP integration. Taps are `std::function`-based, install
  / uninstall via `nullptr`, and guarded by a dedicated `tap_mu_`
  mutex so setters from any thread stay race-free against the
  audio thread's `process_capture()` call (RFC §2.3 thread-safety
  contract). The post-tap is also delivered as `int16_t*` with the
  same metadata (`tests/test_pcm_tap.cpp` 15/15 PASS).
- **PROFILE-1 — Assembly Profile library officialised** — five
  profile variants are first-class and loadable at runtime:
  `sfu` / `transport` / `agent` / `agent-gateway` / `sfu-agent`.
  PAL Slice 1/2/3 (ADR-009) make all five backend-agnostic.
  `tests/test_assembly.cpp` 32/32 PASS including a strict
  `JsonFileMatchesBuiltinConstant` regression test that loads
  every JSON profile file and asserts field-by-field equality with
  the matching C++ constant. Two latent JSON↔C++ bugs uncovered by
  the new strict round-trip test are fixed: `kProfileLive` now sets
  `scheduler.impl = "weighted_fair"` (was relying on the default
  while `live.json` declared `"weighted_fair"`); `profiles/sfu.json`
  now correctly sets `bwe_name = ""` (the comment said "Empty: no
  BWE" but the value was the string `"aimd"`).
- **TPAL-4 — DTLS seam cleanup** — `EngineConfig::dtls_name` field
  added (defaulted to `""` to preserve every v0.10.x caller
  source-compat); engine resolves the DTLS factory through
  `core::PluginRegistry::get_dtls_session(id)` at open() time
  instead of direct `new DtlsSessionWolfSSL`. The `static_cast`
  escape hatch from v0.10.2 is gone. The `IDtlsSession` seam
  surface is tightened (every concrete method on `DtlsSessionWolfSSL`
  is now `override` against the seam). Future 国密 / OpenSSL /
  BoringSSL / mbedTLS backends register their own id via
  `register_dtls_session(id, &factory)` and select via
  `cfg.dtls_name = "..."`. `test_engine_plugin_loading` 17/17 PASS
  (8 base + 5 Slice 8 + 2 Slice 7.5 + 5 new TPAL-4 subtests).
- **DEMO-1 — AI Agent 接入 demo** — `demo-agent-gateway`
  demonstrates the `agent-gateway` Profile end-to-end with PCM
  tap → streaming ASR → LLM mock → response.
- **DISC-1 — GitHub Discussions enabled** — community now has a
  Discussions space alongside issues; issue templates updated.
- **RFC-1 — RFC 001 (PCM tap) promoted Draft → Final** — first
  formal design record under `docs/rfcs/`.

### What is **not** in v0.11.0 (data plane deferred to v1.x)

The following P2 headline items are explicitly cut from v0.11.0 and
moved to the v1.x WebTransport / QUIC re-design window — see
`docs/plan/v0.11-plan.md` §2.2 / §10 for the full rationale:

- **Chrome ↔ NimRTC DataChannel bidirectional interop (DC-1)** —
  the SCTP-over-DTLS physical wiring
  (`IDtlsSession::srtp_keying_material()` →
  `ISctpSocket::set_dtls_keys()` + DTLS transport →
  `ISctpSocket::feed_inbound_datagram()` + SCTP socket → DTLS
  transport outbound) was never landed on `main`; Chrome-headless
  e2e stays 0/4. v0.11.0 falls back to the in-process
  `test_datachannel_engine` 5/5 PASS as the §7 substitute, made
  permanent. The Chrome team's stated direction is "DataChannel
  future = WebTransport over QUIC"; the SCTP-over-DTLS path is
  on Google's deprecation roadmap. v1.x will redesign the data
  plane on top of **WebTransport over QUIC** (likely msquic,
  MIT-licensed, Windows-first, cross-platform CI) — see
  `docs/plan/v0.11-plan.md` §10.
- **L1 strict-priority scheduling across the DC plane (DC-2)** —
  only meaningful once DC-1 lands. The RTP-only strict-priority
  path is already covered by `test_sched_priority.cpp` from
  v0.10.x.
- **usrsctp production backend (TPAL-5)** — the `UsrsctpSocket`
  class is `@deprecated` and still compiles and links for opt-in
  callers (the in-process `test_sctp_usrsctp` 4/4 PASS regression
  test continues to run), but `sctp::register_default_plugins()`
  no longer publishes id="usrsctp" with `core::PluginRegistry`.
  The `UsrsctpSocket*` headers carry `@deprecated` markers
  pointing at the v1.x plan. Expected removal: v1.2.0. The
  `tools/fix_usrsctp{1,2,3,4}.py` history is deleted.

### Deferred to pre-1.0 decision window (unchanged from v0.11-preview)

The following P2 headline items are intentionally **not** in v0.11.0:

- **In-process SFU relay (`nimrtc_sfu`)** — SFU forwarding has open
  architectural questions on BWE/JB interaction (§13 P3 "SFU 转发跳过
  L2" 是否引入 BWE 接口依赖). Decision deferred to v1.0.0 commit.
  If community asks for it during v0.11.0 cycle, it lands in v0.11.x
  patch train (same track as T-PAL Slice 6 Raw UDP bypass).
- **DC forwarding policy (DC-3)** — dependent on SFU-1 AND v1.x
  DC-1 WebTransport land; inherits both deferrals.
- **`sfu-relay` benchmark (BENCH-1)** — dependent on SFU-1.

### Notes

- No public API change relative to v0.10.3 except the single
  `EngineConfig::dtls_name` field. `engine.hpp` adds one new
  field; everything else in the public API surface is unchanged.
- Layout Invariant 4 (no concrete module headers in `engine.hpp`)
  preserved. Layout Invariant 1 (`src/core/` is the only
  INTERFACE library) preserved. Layout Invariant 6 (singletons
  in `nimrtc::core::`) preserved.
- The build is expected to remain green on all four CI platforms
  (Windows / Linux x86_64 / macOS / Linux aarch64). The usrsctp
  sources still compile and link, the `test_sctp_usrsctp` 4/4
  regression test continues to pass, and the `test_datachannel_engine`
  5/5 covers the IDataChannel seam against the in-process stub.
- 国密后端 (SM2/SM4) remains P4 / Enterprise — not in v0.11.0.
- API still **experimental / not for production** through v1.0.0.
- No binary artefacts shipped (源码为主).
- The 4-platform CI matrix stays green; e2e Case D (audio + video)
  is the new Gate #1 (replacing the original "Chrome DC
  bidirectional interop" gate, which is deferred with DC-1 to v1.x).

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
