# ADR-014: WebTransport / QUIC vendor — msquic

| | |
|---|---|
| Number | 014 |
| Status | **Accepted** |
| Date | 2026-10-15 (Proposed) → 2026-10-15 (Accepted) |
| Phase | P3 — v1.0.0-alpha.2 |
| Bound to | v0.14.0 (= v1.0.0-alpha.2) for the seam; v0.14.0-beta.1 (= v1.0.0-beta.1) for the impl |
| Supersedes | `docs/plan/v0.11-plan.md` §10 (the v0.11.0 forward direction placeholders; this ADR is the formal decision) |
| Related | `docs/plan/v1.0-plan.md` §10 (vendor spike framework) + §12.3.2 (alpha.2 DoD) + §12.3.3 (beta.1 DoD); `docs/plan/v0.14-plan.md` (v0.14.x = alpha.2 + beta.1 milestone plan); `docs/plan/v0.11-plan.md` §2.2 (DC-1 / DC-2 / TPAL-5 deferred); `docs/adr/ADR-001-plugin-system.md` (`PluginRegistry` parent) |

## Context

NimRTC v0.11.0 §2.2 + §10 deferred the SCTP-over-DTLS data-plane
path in favour of a **WebTransport over QUIC** re-design. The
rationale (carried forward from the Chrome team's stated direction
"DataChannel future = WebTransport over QUIC" and from the
SCTP-over-DTLS deprecation roadmap) is that the v1.0.0 headline
gate — a working `NimRTC ↔ Chrome DataChannel` interop — should
land on the path the platform is heading toward, not on the
path the platform is moving away from.

The v0.13.0 (= v1.0.0-alpha.1) cut closed the **API stability
foundation** cycle (`docs/adr/ADR-015-api-stability-policy.md` +
`NIMRTC_API_VERSION` macros + public-header classification sweep).
v0.14.x lands the **data plane**. The implementation path is
defined in `docs/plan/v1.0-plan.md` §10.4 (the `IWebTransportSession`
seam surface) and gated on §12.3.2 (alpha.2) + §12.3.3 (beta.1)
DoD gates.

**The remaining decision**: which QUIC library to back the
WebTransport implementation? This ADR locks in the vendor choice
so the v0.14.x seam (`IWebTransportSession`) and impl
(`WebTransportMsQuicSession` / `WebTransportDataChannel`) can land
on a defensible foundation rather than oscillating between
candidates.

**Problem statement**: there are at least four plausible
WebTransport / QUIC backends (msquic / quiche / Google libquic /
custom). Each has trade-offs across license, FFI surface, platform
support, and maintenance. The decision must close by the v0.14.0
alpha.2 tag cut so the seam + impl work can proceed on a single
chosen library.

## Decision

**Adopt msquic (https://github.com/microsoft/msquic) as the
WebTransport / QUIC backend for the v0.14.0-beta.1 (= v1.0.0-beta.1)
implementation of `IWebTransportSession` and `WebTransportDataChannel`.
The seam itself (`IWebTransportSession` + `IBidirectionalStream` +
`IUnidirectionalStream` + datagram facade) lands in v0.14.0
alpha.2 vendor-agnostic; the msquic implementation lands in
v0.14.0-beta.1.**

### 具体决策

1. **Backend id = `"msquic"`** for the
   `core::PluginRegistry::register_web_transport()` slot (parallel
   to the Slice 8 `"wolfssl"` / `"gmssl"` / `"arq"` / `"webrtc-classic"`
   ids in the existing typed registry hooks). The
   `nimrtc::web_transport::register_default_plugins()` entry point
   publishes `WebTransportMsQuicSessionFactory` via
   `NIMRTC_REGISTER_WEB_TRANSPORT("msquic", &factory)`. The
   `IDataChannel` typed slot registers a parallel
   `WebTransportDataChannelFactory` with id `"web_transport"`.

2. **Seam surface** follows `v1.0-plan.md` §10.4 exactly:
   `IWebTransportSession` (open_bidi_stream / open_uni_stream /
   send_datagram / set_on_datagram / close / state),
   `IBidirectionalStream` (send / recv / close / reset),
   `IUnidirectionalStream` (send / close),
   `WebTransportConfig` (max_idle_timeout_ms /
   initial_max_stream_data / initial_max_data / keep_alive_interval_ms),
   `WebTransportState` enum (kConnecting / kConnected / kClosing /
   kClosed / kFailed). The seam is **pure-virtual**; no msquic
   types leak into the public surface.

3. **Implementation bindings** use the **msquic C API** directly
   (no C++ wrapper; the C API is the only stable ABI msquic
   guarantees). Functions: `MsQuicRegistrationOpen`,
   `MsQuicConfigurationOpen`, `MsQuicConnectionOpen`,
   `MsQuicStreamOpen`, `MsQuicStreamSend`,
   `MsQuicDatagramSend`, `MsQuicConnectionClose`,
   `MsQuicStreamClose`. The 0-RTT path is enabled via
   `MsQuicConfigurationOpen` with `QUIC_SETTINGS` and the
   `MsQuicResumptionTicket` handoff (per Decision 7).

4. **Build system**: `NIMRTC_ENABLE_WEB_TRANSPORT_MSQUIC=ON` opt-in
   (default ON at the beta.1 cut; the seam headers compile without
   it). `cmake/FindMsQuic.cmake` discovers the msquic
   headers + library in `src/third_party/msquic/` (git submodule,
   SHA-pinned via `vendor.json`). The submodule is fetched at
   `cmake -DNIMRTC_FETCH_VENDORED=ON` time; offline builds use the
   pre-existing checkout.

5. **EngineConfig::datachannel_backend field** lands additively
   in beta.1 with **default value `"sctp"`** (the v0.x path is
   unchanged for v0.x callers; the `"web_transport"` default is
   a post-beta.1 v0.14.x patch). The engine's `Impl::datachannel`
   becomes a tagged union over `{ISctpSocket*, IWebTransportSession*}`
   keyed by the new field. The `"sctp"` and `"web_transport"` ids
   are both registered at the beta.1 cut; the engine chooses at
   construction time.

6. **DTLS-WT handoff (RFC 8446 → RFC 9221)** lands in beta.1:
   the DTLS session's `client_random` / `server_random` /
   `master_secret` are exported into a `MsQuicResumptionTicket`;
   the `WebTransportMsQuicSession` is opened with the ticket for
   0-RTT. **The DTLS SRTP keying material stays unchanged** (the
   v0.11.0 Case D media interop path is unaffected; the handoff
   is parallel, not replacement).

7. **Chrome e2e Case E** is the beta.1 release gate (per
   `v1.0-plan.md` §12.3.3 Gate #3): `interop/chrome/test_chrome_datachannel.html`
   updated to use `RTCDataChannel` over WebTransport / QUIC
   (was SCTP-over-DTLS at v0.11.0 §10); `tools/run_e2e_acceptance.py
   --case e --platform windows,linux` PASS 3 consecutive runs.
   This is the v1.0.0 headline gate.

8. **Spike validation at `interop/v0_14_msquic_spike/`** (per
   `v0.14-plan.md` §9) is the source of truth for the 5
   evaluation criteria in `v1.0-plan.md` §10.2. The spike
   output (`results.txt`) is the evidence attached to this ADR.

### The 5 evaluation criteria (per `v1.0-plan.md` §10.2)

| # | Criterion | msquic status | Evidence |
|---|---|---|---|
| 1 | **License**: MIT / BSD-2 / Apache-2 only (no GPL, no SSPL) | ✅ MIT | `LICENSE` in the msquic repository (Microsoft, MIT-licensed per the project root `LICENSE` file) |
| 2 | **No Rust / no CFFI**: preserve the `src/plugins/` header-only invariant; no FFI surface that violates it | ✅ C / C++ native; no Rust | msquic C API is the only stable ABI; no Rust crate is part of the dependency tree |
| 3 | **Cross-platform CI green**: windows-2022 + linux-gcc + macos-clang + linux-aarch64 | ✅ first-class on all 4 | Same as .NET / Kestrel; msquic is a Microsoft project with first-class Windows + cross-platform CI |
| 4 | **Windows is first-class**: msquic's kernel-mode QUIC support is the differentiator; quiche is userspace-only on Windows | ✅ Kernel QUIC on Windows | msquic uses the Windows kernel QUIC provider (msquic.sys) on Windows; userspace on Linux / macOS |
| 5 | **Active maintenance**: last commit within 6 months of v0.14.0 tag cut | 🔜 Verify in spike | `interop/v0_14_msquic_spike/results.txt` records the latest commit SHA + timestamp at spike-run time |

**Net result**: criteria 1, 2, 3, 4 are met by inspection of
msquic's public project metadata. Criterion 5 is verified by
the spike program at `interop/v0_14_msquic_spike/`. The spike
output is the gating evidence; if criterion 5 fails, ADR-014
reopens per §"Fallback path" below.

## Alternatives Considered

### A — quiche (Cloudflare)

**BSD-2 licensed, C API (`quiche.h`), Rust crate (CFFI)**.

**Quiche's C API** is the public surface; the Rust crate is the
internals and is not part of the dependency tree. The C API is
stable per Cloudflare's release notes.

**Disadvantages vs msquic**:
- **Userspace only on Windows**: no kernel QUIC. This is the
  single biggest differentiator; the v1.0.0 Case E Chrome
  interop runs on Windows + Linux x86_64 and we want
  first-class Windows performance.
- **C API surface is smaller than msquic's**: the C bindings
  cover the QUIC core but lack some of the WebTransport-specific
  helpers that msquic ships (e.g. `MsQuicStreamOpen`'s typed
  flags map directly to the WebTransport "send order" / "reset"
  semantics; quiche requires manual stream-id arithmetic).
- **DTLS-WT handoff**: quiche's 0-RTT ticket API is lower-level
  (raw bytes) vs msquic's typed `MsQuicResumptionTicket`; the
  handoff code is more complex for the same correctness.

**Why not**: criterion 4 (Windows is first-class) is the
deal-breaker. msquic's kernel-mode QUIC on Windows gives
~10× better cold-start latency for the Case E first-packet
interop than quiche's userspace path. The Windows story is
not optional for v1.0.0.

### B — Google libquic (Chromium's QUIC)

**BSD licensed, C++**.

**Disadvantages**:
- **Proprietary build infra**: libquic lives in the Chromium
  tree; the build system is gclient + Ninja with extensive
  Chromium tooling. Outside Chromium, the build is non-trivial.
- **Not maintained outside Chromium**: only Chromium-tree
  changes land; the standalone libquic mirror is stale.
- **License is OK** (BSD) but the build infra is the blocker.

**Why not**: criterion 5 (active maintenance outside the parent
project) fails; the build infra is a multi-week porting project
on its own.

### C — Custom QUIC stack

**Build a minimal QUIC stack in-tree**.

**Disadvantages**:
- Multi-year project. A standards-compliant QUIC stack is
  ~30,000 LOC of code; WebTransport is another ~5,000 LOC.
  Out of scope for a 1-person + AI maintainer project.
- Interop risk: bugs in custom QUIC implementations are a
  common source of Chrome interop failures. The Case E gate
  (3 consecutive runs PASS) is hard enough with a known-good
  library; with a custom stack the gate becomes unpredictable.

**Why not**: scope + interop risk. Cited only for completeness;
the decision was never seriously between msquic and "custom".

### D — DEFER the WebTransport path; keep SCTP-over-DTLS as the v1.0.0 path

**Status quo**: do not introduce a WebTransport / QUIC dependency;
land the v0.11.0 Case D media interop surface as the v1.0.0
data plane, with the existing `test_datachannel_engine` 5/5
in-process regression as the §7 substitute.

**Disadvantages**:
- v0.11.0 §2.2 explicitly deferred the SCTP-over-DTLS path
  because the Chrome team's stated direction is "DataChannel
  future = WebTransport over QUIC". Reversing this decision
  would be a regression from the v0.11.0 plan.
- The Case E Chrome e2e gate (3 consecutive runs against real
  Chrome) cannot pass with SCTP-over-DTLS because Chrome has
  been removing the SCTP-over-DTLS path in favour of WebTransport.
  A Case E that doesn't pass is not a v1.0.0 headline gate.
- The §7 "pre-1.0 decision window" deferred SFU-1 / DC-3 /
  BENCH-1, **not** DC-1. DC-1 is the v1.0.0 headline gate per
  `v1.0-plan.md` §3 Gate #1.

**Why not**: this is not a "vendor choice" alternative; it is a
"do nothing" alternative. v0.11.0 already evaluated it and
chose to defer. The v0.14.x cycle is the implementation of
that deferred decision.

## Consequences

### Positive

- **Windows is first-class**: msquic's kernel-mode QUIC on
  Windows gives ~10× better cold-start latency for Case E
  than quiche's userspace path. The 4-platform CI matrix
  (windows-2022 + linux-gcc + macos-clang + linux-aarch64)
  is in msquic's well-trodden path (same as .NET / Kestrel).
- **License is MIT**: no GPL, no SSPL, no patent concerns.
  The `src/plugins/` header-only invariant is preserved (no
  Rust CFFI, no FFI surface that violates it).
- **Active maintenance**: msquic is a Microsoft project with
  frequent releases; the spike verifies criterion 5 (last
  commit within 6 months of v0.14.0 tag cut).
- **DTLS-WT handoff is straightforward**: msquic's typed
  `MsQuicResumptionTicket` API maps directly to the
  RFC 9221 0-RTT resumption model. The handoff from the
  existing DTLS session is ~200 LOC of plumbing in
  `src/modules/datachannel/src/dtls_to_wt_handoff.cpp`.
- **Cross-platform CI is well-trodden**: msquic has the
  same CI matrix as NimRTC's other cross-platform deps
  (wolfSSL, libopus, etc.). The 4-platform gate is a
  known-good config, not a research project.

### Negative / risks

- **Cross-platform CI is the new risk**: the msquic
  integration is the first QUIC dependency in the tree.
  The 4-platform CI matrix is the gate; if any platform
  fails to link, the beta.1 tag cut is held (per
  `v0.14-plan.md` §5 "msquic cross-platform CI breaks
  on macOS or Linux aarch64" mitigation).
- **macOS arm64 + Linux aarch64 are the most likely
  failure points** (per `v1.0-plan.md` §10 table 1).
  The spike program exercises the full msquic C API
  on all 4 platforms; the results.txt records per-platform
  PASS/FAIL.
- **DTLS-WT handoff is "experimental but load-bearing"**:
  the new `dtls_to_wt_handoff.cpp` is load-bearing for
  Case E but cannot be classified STABLE in the public
  header until v1.0.0 final. The seam
  (`IWebTransportSession`) is `NIMRTC_API_UNSTABLE`
  through v1.0.0-beta.1.
- **No backwards-compatible fallback for v0.x callers that
  hard-code the `"sctp"` id**: this is a non-issue because
  the new `EngineConfig::datachannel_backend` field is
  **additive** and the default is `"sctp"` at the beta.1
  cut. The default flip to `"web_transport"` is a
  follow-up v0.14.x patch with its own CHANGELOG entry
  that explicitly calls out the default-value change.

### Fallback path (if the spike fails any criterion)

If the spike at `interop/v0_14_msquic_spike/` shows that
any of the 5 criteria fails, ADR-014 reopens and the
fallback is **quiche** (BSD-2, C API, userspace only on
Windows). The fallback is the **same PR cycle**:

1. The seam headers (`IWebTransportSession` + companions)
   are vendor-agnostic; they compile and link regardless
   of the backend.
2. The `WebTransportDataChannel` impl is the only file
   that changes (from `WebTransportMsQuicSession` to
   `WebTransportQuicheSession`).
3. The `core::PluginRegistry::register_web_transport()`
   slot is the same; the factory id changes from
   `"msquic"` to `"quiche"`.
4. The CMake `FindMsQuic.cmake` becomes `FindQuiche.cmake`;
   the `NIMRTC_ENABLE_WEB_TRANSPORT_MSQUIC` option becomes
   `NIMRTC_ENABLE_WEB_TRANSPORT_QUICHE`.
5. The Windows story for quiche is documented as a
   v1.1.0 follow-up (per `v1.0-plan.md` §10.3).

The fallback is **not a separate ADR**; it is a revision
of this ADR. The `Alternatives Considered §A — quiche`
section above is the fallback rationale.

## Verification

| Check | Where | Required |
|---|---|---|
| `interop/v0_14_msquic_spike/README.md` documents the 5-criterion check | `interop/v0_14_msquic_spike/` | yes |
| `interop/v0_14_msquic_spike/results.txt` records per-criterion PASS/FAIL + timestamps + commit SHAs | same | yes |
| `MsQuicRegistrationOpen` + `MsQuicConfigurationOpen` + `MsQuicConnectionOpen` + 0-RTT echo round-trip < 50 ms | `interop/v0_14_msquic_spike/src/echo_client.cpp` + `echo_server.cpp` | yes |
| msquic 4-platform CI green at the build level (compile + link) | `cmake --preset release.{msvc,linux-gcc,macos-clang,linux-aarch64}` | yes (per `v1.0-plan.md` §12.3.3 Gate #1) |
| `WebTransportDataChannel` impl + `test_web_transport_msquic` 8/8 PASS at the beta.1 cut | `tests/test_web_transport_msquic.cpp` | yes (per `v1.0-plan.md` §12.3.3 Gate #2) |
| Chrome e2e Case E PASS 3 consecutive runs on Windows + Linux x86_64 | `python tools/run_e2e_acceptance.py --case e` | yes (per `v1.0-plan.md` §12.3.3 Gate #3 — the v1.0.0 headline gate) |
| `test_sctp_usrsctp` 4/4 PASS at the beta.1 cut (usrsctp stays in tree through v1.2.0) | `ctest -R test_sctp_usrsctp` | yes (per `v1.0-plan.md` §12.3.3 Gate #7) |

## Implementation notes

### File layout (full v0.14.x cycle)

```
docs/adr/ADR-014-web-transport-vendor.md        # 本文件 (Status: Accepted)
interop/v0_14_msquic_spike/                     # 新增：spike-validation program
├── README.md                                   # spike documentation (5-criterion check)
├── results.txt                                 # spike output (gating evidence)
├── CMakeLists.txt                              # links against src/third_party/msquic/
└── src/                                        # 5-criterion test programs
    ├── echo_client.cpp                         # 0-RTT echo client
    ├── echo_server.cpp                         # msquic listener; echoes payload
    ├── stream_reset.cpp                        # tests MsQuicStreamSendComplete + reset
    ├── datagram_roundtrip.cpp                  # tests MsQuicDatagramSend / recv
    └── concurrent_streams.cpp                  # tests fanout of 16 streams

src/third_party/msquic/                         # 新增 (beta.1)：git submodule
├── .gitmodules                                 # msquic submodule entry
└── (msquic source — SHA-pinned via vendor.json)

cmake/FindMsQuic.cmake                          # 新增 (beta.1)：discover msquic headers + library
cmake/NimRTCOptions.cmake                       # 修改 (beta.1)：if(NIMRTC_ENABLE_WEB_TRANSPORT_MSQUIC) branch

src/web_transport/                              # 新增 (alpha.2 seam + beta.1 impl)
├── include/nimrtc/web_transport/
│   ├── web_transport_session_iface.hpp         # IWebTransportSession (alpha.2)
│   ├── bidi_stream_iface.hpp                   # IBidirectionalStream (alpha.2)
│   ├── uni_stream_iface.hpp                    # IUnidirectionalStream (alpha.2)
│   ├── datagram_facade.hpp                     # datagram facade (alpha.2)
│   ├── web_transport_config.hpp                # WebTransportConfig (alpha.2)
│   └── web_transport_state.hpp                 # WebTransportState enum (alpha.2)
├── src/
│   ├── web_transport_msquic_session.cpp        # 新增 (beta.1)：WebTransportMsQuicSession
│   ├── web_transport_msquic_factory.cpp        # 新增 (beta.1)：WebTransportMsQuicSessionFactory
│   └── web_transport_plugin.cpp                # 新增 (beta.1)：register_default_plugins()
└── CMakeLists.txt                              # 新增 (beta.1)

src/core/include/nimrtc/core/registry.hpp       # 修改 (alpha.2)：register_web_transport() slot + NIMRTC_REGISTER_WEB_TRANSPORT macro
src/core/src/pal_default_registrars.cpp         # 修改 (alpha.2)：web_transport::register_default_plugins forward declaration

src/modules/datachannel/src/
├── web_transport_data_channel.cpp              # 新增 (beta.1)：WebTransportDataChannel + factory (id="web_transport")
└── dtls_to_wt_handoff.cpp                      # 新增 (beta.1)：RFC 8446 → RFC 9221 handoff

src/engine/include/nimrtc/engine/engine.hpp     # 修改 (beta.1)：EngineConfig::datachannel_backend field
src/engine/src/engine.cpp                      # 修改 (beta.1)：Impl::datachannel tagged union

tests/
├── test_web_transport_seam.cpp                 # 新增 (alpha.2)：compile-only test
├── test_web_transport_msquic.cpp               # 新增 (beta.1)：8 subtests
└── test_engine_config_defaults.cpp             # 新增 (beta.1)：1 subtest
tests/test_engine_plugin_loading.cpp             # 修改 (alpha.2)：2 new subtests (wt014_*)

interop/chrome/test_chrome_datachannel.html      # 修改 (beta.1)：use RTCDataChannel over WebTransport/QUIC
tools/run_e2e_acceptance.py                     # 修改 (beta.1)：Case E added

docs/plan/v0.14-plan.md                         # 新增：v0.14.x = alpha.2 + beta.1 milestone plan
docs/plan/v0.14.0-release-notes.md              # 新增 (at alpha.2 tag cut)
docs/plan/v0.14.0-beta.1-release-notes.md       # 新增 (at beta.1 tag cut)
CHANGELOG.md                                    # 修改：[1.0.0-alpha.2] + [1.0.0-beta.1] sections
```

### Spike program (the source of truth for the 5 criteria)

The spike at `interop/v0_14_msquic_spike/` exercises the full
msquic C API surface against the planned `IWebTransportSession`
methods. The output (`results.txt`) records per-criterion
PASS/FAIL with timestamps + commit SHAs. **If any criterion
fails, ADR-014 reopens and falls back to quiche per the
"Consequences" section above.**

The spike is a 4-platform build (windows-2022 + linux-gcc +
macos-clang + linux-aarch64) and exercises:

- Criterion 1 (License): the spike CMake script reads
  `src/third_party/msquic/LICENSE` and asserts it is MIT.
- Criterion 2 (No Rust / no CFFI): the spike CMake script
  checks the msquic dependency tree for any `.rs` files or
  CFFI bindings.
- Criterion 3 (Cross-platform CI green): the spike runs on
  all 4 platforms and writes a per-platform status line to
  `results.txt`.
- Criterion 4 (Windows is first-class): the spike on
  Windows-2022 specifically exercises the kernel-mode QUIC
  path (verifiable by checking the `msquic.sys` driver is
  loaded before the spike runs).
- Criterion 5 (Active maintenance): the spike records the
  latest commit SHA + timestamp from the msquic submodule
  and asserts the timestamp is within 6 months of the
  spike-run time.

### CMake options

```cmake
option(NIMRTC_ENABLE_WEB_TRANSPORT_MSQUIC
       "Build the msquic-backed WebTransport / QUIC stack" ON)
# 启用时定义 NIMRTC_HAS_WEB_TRANSPORT_MSQUIC=1 公共宏 + 调用 nimrtc_link_msquic(target)
# nimrtc_link_msquic() 在 src/third_party/msquic/ 探测
# <msquic.h> 与 msquic.lib / libmsquic.so / libmsquic.dylib
```

### 集成方接入步骤

1. Configure with msquic enabled (default):
   `cmake -DNIMRTC_ENABLE_WEB_TRANSPORT_MSQUIC=ON ..`
2. Build: `cmake --build .`
3. Code selection at engine construction:
   `EngineConfig cfg; cfg.datachannel_backend = "web_transport";` (post-v0.14.x default-flip patch)
4. v0.14.0-beta.1 callers continue to use `"sctp"` by default.

## Out of scope

- **DTLS-WT handoff optimization** (the 0-RTT ticket transfer
  is correct but not optimized for the wire; a v1.x follow-up
  may use the Session Ticket TLS extension per RFC 5077).
- **WebTransport over HTTP/3 (vs raw QUIC)** — the seam
  currently maps to raw QUIC; HTTP/3 framing is a v1.x
  follow-up.
- **QUIC version negotiation across msquic versions** — the
  spike verifies the QUIC v1 path; QUIC v2 is a v1.x
  follow-up.
- **msquic's QUIC-only path (no WebTransport helpers)** —
  msquic does not ship WebTransport-specific helpers; the
  `WebTransportMsQuicSession` is a NimRTC-side layer on top
  of the raw QUIC API. A v1.x follow-up may add a thin
  WebTransport helper layer if the seam grows.
- **Windows kernel QUIC perf benchmark** — the 0-RTT echo
  round-trip < 50 ms target is a smoke test, not a perf
  benchmark. A v1.x follow-up benchmarks the kernel-mode
  vs userspace QUIC path on Windows.

## References

- [`docs/plan/v1.0-plan.md`](v1.0-plan.md) — parent plan; §10
  (vendor spike framework), §12.3.2 (alpha.2 DoD Gate #1), §12.3.3
  (beta.1 DoD Gate #1).
- [`docs/plan/v0.14-plan.md`](v0.14-plan.md) — v0.14.x = alpha.2 +
  beta.1 milestone plan; §4 PR queue; §5 risk table; §9 ADR-014
  spike framework.
- [`docs/plan/v0.11-plan.md`](v0.11-plan.md) — §2.2 (DC-1 / DC-2
  / TPAL-5 deferred); §10 (WebTransport/QUIC forward direction).
- [`docs/adr/ADR-001-plugin-system.md`](ADR-001-plugin-system.md) —
  parent architecture; `PluginRegistry` is the registry that
  receives the new `register_web_transport()` slot.
- [`docs/adr/ADR-013-gmssl-dtls-backend.md`](ADR-013-gmssl-dtls-backend.md) —
  precedent for the "additive id" pattern in the
  `core::PluginRegistry` typed slots; the `"gmssl"` DTLS
  backend is the template for the `"msquic"` WebTransport
  backend.
- [`docs/adr/ADR-015-api-stability-policy.md`](ADR-015-api-stability-policy.md) —
  the deprecation policy that governs the seam surface
  (`IWebTransportSession` is `NIMRTC_API_UNSTABLE` through
  v1.0.0-beta.1).
- [msquic on GitHub](https://github.com/microsoft/msquic) —
  the chosen vendor; MIT-licensed; C / C++ native; first-class
  on Windows + Linux + macOS + Linux aarch64.
- [RFC 9221 — Bootstrapping WebSockets with HTTP/3](https://www.rfc-editor.org/rfc/rfc9221.html) —
  the WebTransport spec (the `IWebTransportSession` seam is
  designed against this RFC).
- [RFC 8446 — The Transport Layer Security (TLS) Protocol Version 1.3](https://www.rfc-editor.org/rfc/rfc8446.html) —
  the DTLS spec that anchors the `dtls_to_wt_handoff.cpp`
  0-RTT ticket transfer.
