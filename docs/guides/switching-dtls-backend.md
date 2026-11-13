# Switching the DTLS Backend

> **Audience**: integrators that need wolfSSL ↔ GMSSL (or another
> custom DTLS backend), in particular anyone targeting OpenHarmony
> or GM/T-regulated environments.
>
> **Status:** v0.12.0-alpha; covers wolfSSL (default) and GMSSL
> (`NIMRTC_ENABLE_DTLS_GMSSL=ON`). For adding a brand-new backend,
> read [`plugin_author_guide.md`](../plugin_author_guide.md) §6.1
> first.

---

## 1. Why this guide exists

NimRTC v0.10.x locked everyone into wolfSSL for DTLS-SRTP. PAL Slice
4 (v0.11.0) introduced the `IDtlsSession` / `IDtlsSessionFactory`
seam; ADR-013 (v0.12.0-alpha) added `DtlsSessionGmSSL` as the second
production-ready backend. Engine code is **unchanged** across
backends: you select the backend at the `EngineConfig::dtls_name`
field and the rest of the call graph is identical.

This guide covers three scenarios:

1. **Default wolfSSL** — no configuration required.
2. **GMSSL on OpenHarmony or Linux** — opt-in at build time,
   selection at engine open.
3. **Custom DTLS backend** — register your factory, point the
   engine at it.

---

## 2. Quick start

### 2.1 Default wolfSSL — no work needed

```cpp
#include <nimrtc/engine/engine.hpp>

nimrtc::engine::EngineConfig cfg;   // dtls_name left empty → "wolfssl"
nimrtc::engine::NimRTCEngine engine(cfg);
engine.open();   // uses DtlsSessionWolfSSL internally
```

The wolfSSL factory is registered by
`nimrtc::dtls::register_default_plugins()`, which the engine calls
internally during `open()`.

### 2.2 GMSSL on Linux (or any host with GMSSL installed)

#### Configure

```bash
# Option 1 — system OpenSSL with the openssl-srtp patch
# Option 2 — system GMSSL (exported symbols match OpenSSL 1.1.1)

cmake --preset debug \
      -DNIMRTC_ENABLE_DTLS_GMSSL=ON \
      -DOpenSSL_ROOT=/path/to/gmssl/install
cmake --build build
```

The `NIMRTC_ENABLE_DTLS_GMSSL=ON` option:

- builds `src/dtls/src/dtls_gmssl_session.cpp` and
  `src/dtls/src/dtls_gmssl_factory.cpp`;
- links `OpenSSL::SSL` + `OpenSSL::Crypto` (or `gmssl::ssl` /
  `gmssl::crypto` if your GMSSL ships its own exported targets);
- registers `GmSSLDtlsFactory` under the id `"gmssl"` in the same
  `core::PluginRegistry` slot as wolfSSL.

#### Select at engine open

```cpp
EngineConfig cfg;
cfg.dtls_name = "gmssl";
NimRTCEngine engine(cfg);
engine.open();   // now uses DtlsSessionGmSSL
```

#### Test it

```bash
cmake --build build --target test_dtls_gmssl_tlcp
./build/tests/test_dtls_gmssl_tlcp
```

This is the in-process Client+Server round-trip smoke test (per
ADR-013 §"Verification"). Successful output:

```
[OK] DTLS Connected (RFC 6347 §4.2.4 final)
[OK] SRTP keying material 60 bytes exported
[OK] on_handshake_complete fired with kOk
[OK] cipher = ECDHE-ECDSA-AES128-GCM-SHA256  (default western suite)
[OK] stats.errors == 0
```

### 2.3 Custom DTLS backend

Author your own implementation of `dtls::IDtlsSession` and
`dtls::IDtlsSessionFactory`. See
[plugin_author_guide.md](../plugin_author_guide.md) §6.1 for the
detailed walkthrough and ADR-001 / 013 for design rationale.

Build, link your factory registration, and configure:

```cpp
EngineConfig cfg;
cfg.dtls_name = "mybackend";   // matches the id you registered
```

The engine will resolve `mybackend` and dispatch every DTLS call to
your factory — no source change elsewhere.

### 2.4 The contract tests your backend must pass

A DTLS backend is not "working" because the handshake completed once.
It is working because it satisfies the seam contract, and the
contract is **tested**. A new backend must pass the seam tests in
`src/modules/dtls/tests/` before it is considered supported:

| Test file | What it asserts | Why it exists |
|---|---|---|
| `test_dtls_factory.cpp` | `factory->create()` returns a usable `IDtlsSession`; the concrete session **inherits** the interface; `factory->id()` returns the id the engine will look up; `register_default_plugins()` is **idempotent** | The factory contract. A backend that creates a session which does not actually implement `IDtlsSession` compiles but fails at the first virtual call — usually in a release build, at a customer site. The inheritance check catches it at test time. |
| `test_dtls_seam_mock_factory.cpp` | A **mock** factory registered under a test id resolves through the registry exactly like a real one; ids resolve to the right factory; per-id registration is independent | The registry contract. It proves the *seam* works, independent of any crypto library — so a backend-specific bug is never confused with a plumbing bug. |
| `test_dtls_handshake_reassembler.cpp` | DTLS records are reassembled correctly across fragmented reads | The record layer. A backend that hands the engine partial records produces a handshake that stalls at `HelloSent` — the classic symptom. |

**The in-process round-trip** (`test_dtls_gmssl_tlcp` for the GMSSL
backend; the equivalent for a custom backend) is the fourth
requirement, and it is the one that exercises the *whole* path:

```
[OK] DTLS Connected (RFC 6347 §4.2.4 final)
[OK] SRTP keying material 60 bytes exported
[OK] on_handshake_complete fired with kOk
[OK] cipher = ECDHE-ECDSA-AES128-GCM-SHA256  (default western suite)
[OK] stats.errors == 0
```

Note the **`60 bytes exported`** line in particular. A backend that
handshakes successfully but cannot export SRTP keying material is the
single most common DTLS failure, because the symptom is not "DTLS is
broken" but "DTLS is Connected and there is no inbound audio". If
your backend's `export_srtp_key_material()` returns zero bytes, the
handshake still looks fine — which is why the assertion has to be
explicit.

**Minimum bar for a new backend:**

1. `test_dtls_factory.cpp` passes against your factory — including
   the inheritance assertion and the `id()` assertion.
2. `test_dtls_seam_mock_factory.cpp` passes (your change must not
   break the seam for other backends).
3. `test_dtls_handshake_reassembler.cpp` passes (your change must not
   break record reassembly).
4. An in-process Client+Server round trip completes the handshake,
   exports 60 bytes of SRTP keying material, fires
   `on_handshake_complete` with `kOk`, and reports `stats.errors == 0`.
5. The loopback-p2p smoke test reaches ICE Connected + DTLS
   Connected + SRTP key installation within ~5 s.

**Two things the tests will not catch for you**, and which you must
verify yourself against a real peer:

- **The cipher suite.** The default is
  `ECDHE-ECDSA-AES128-GCM-SHA256` / `AES256-GCM-SHA384`. If your
  backend supports only a subset, the round-trip still passes (both
  peers are your backend) but browser interop fails. Match the suite
  before testing against Chrome.
- **The fingerprint format.** `set_peer_fingerprint` takes the **raw
  32-byte SHA-256 digest** from the SDP `a=fingerprint` attribute — the
  WebRTC convention, *not* the SPKI hash from RFC 7250. Getting this
  wrong produces a handshake that fails with no useful diagnostic.

### 2.5 The id you register under

The id is a public config key: it appears in host config files and in
log lines, and once published it is stable for the lifetime of the
major version. It must match the grammar `^[a-z][a-z0-9_-]*$`, and a
third-party backend must carry its own namespace — `acme_dtls`, not
`dtls`. The full contract is
[ADR-023](../adr/ADR-023-plugin-id-naming-contract.md); the
practical rules are in
[plugin_author_guide.md](../plugin_author_guide.md) §8.1.

```cpp
// The id is a bare token — the macro stringifies it.
// A quoted literal would register under an id that includes the
// quote characters, and the lookup would never match.
NIMRTC_REGISTER_DTLS_SESSION(acme_dtls, &g_factory);
```

---

## 3. What doesn't change

If you switch between wolfSSL and GMSSL (or any other DTLS
backend), the following are unaffected:

- **SDP wire format** — both backends produce the same `a=fingerprint`
  SPKI SHA-256 base64 string for the same certificate. Loopback
  interop across backends is symmetrical.
- **ICE / RTP / SRTP** — DTLS is the only module being swapped; the
  SRTP context installed after `dtls.connected()` is unchanged.
- **`EngineConfig` field semantics** — `dtls_name` is the only
  knob. Every other `*_name` field stays the same.
- **Public API** — no `#ifdef NIMRTC_ENABLE_DTLS_GMSSL` in user
  code; the enable flag is build-time only.

---

## 4. What does change

| Aspect | wolfSSL | GMSSL |
|---|---|---|
| **Cipher suite (default)** | `ECDHE-ECDSA-AES128-GCM-SHA256` | same (per ADR-013 §Decision 2) |
| **Certificate** | self-signed P-256 ECDSA | self-signed P-256 ECDSA (re-used) |
| **Symmetric encryption** | AES-GCM via wolfSSL | AES-GCM via OpenSSL-compatible API |
| **Hashing** | SHA-256 via wolfSSL | SHA-256 via OpenSSL; SM3 in SM suites |
| **Required library** | `libwolfssl` vendored in `src/third_party/wolfssl/` | `libssl` + `libcrypto` (system) |
| **Standards targeted** | DTLS 1.2 (RFC 6347), DTLS-SRTP (RFC 5764) | DTLS 1.2 + **TLCP (GM/T 0044)** |
| **Source-tree changes** | `src/dtls/src/dtls_wolfssl_session.cpp` | `src/dtls/src/dtls_gmssl_session.cpp` |

GMSSL **keeps the same default cipher suite** as wolfSSL because
ADR-013 §Decision 2 intentionally preserves browser interop. The
SM2/SM3/SM4 cipher suites can be enabled by editing the
`cipher_priority` table inside `dtls_gmssl_session.cpp` — they will
break Chrome/Firefox interop, so only enable for SM-only peers.

---

## 5. Build details

### 5.1 `NIMRTC_ENABLE_DTLS_GMSSL` option

```cmake
option(NIMRTC_ENABLE_DTLS_GMSSL
       "Build the GMSSL-backed DTLS factory (DtlsSessionGmSSL)" OFF)
```

Default is `OFF`, so default builds do not require GMSSL. Toggling
it on:

- Adds `dtls_gmssl_session.cpp` and `dtls_gmssl_factory.cpp` to
  the build.
- Defines `NIMRTC_USE_GMSSL=1`.
- Links `OpenSSL::SSL` / `OpenSSL::Crypto` (via
  `nimrtc_link_gmssl()` in `cmake/NimRTCVendored.cmake`).

### 5.2 `nimrtc_link_gmssl()` helper

`cmake/NimRTCVendored.cmake` ships a `nimrtc_link_gmssl()` helper
that:

1. calls `find_package(OpenSSL 1.1 REQUIRED)`;
2. verifies `SSL_CTX_new` and `SSL_export_keying_material` symbols
   are present in the resolved OpenSSL library (the latter indicates
   the openssl-srtp patch is in);
3. sets `NIMRTC_USE_GMSSL=1` and links the targets;
4. sets `NIMRTC_GMSSL_HINT_PREFIX` (used by `nimrtc::dtls::Config`
   for cert path lookup).

> **Failure mode:** if `SSL_export_keying_material` is missing
> the helper fails configuration with a clear error. Linking
> against stock OpenSSL 1.1.1 from a distro will likely fail this
> check.

### 5.3 Verifying the build

```bash
# Confirm wolfSSL default is unchanged:
cmake --preset debug -DNIMRTC_ENABLE_DTLS_GMSSL=OFF
cmake --build build --target nimrtc_dtls_seam

# Confirm GMSSL path compiles:
cmake --preset debug -DNIMRTC_ENABLE_DTLS_GMSSL=ON -DOpenSSL_ROOT=...
cmake --build build --target nimrtc_dtls_seam test_dtls_gmssl_tlcp
```

### 5.4 MSVC vs GCC/Clang

- **MSVC** — `find_package(OpenSSL)` requires `OpenSSL_DIR` to point
  at the GMSSL build's `cmake/` directory (not available pre-built
  in most setups). v0.12.0 GMSSL verification on Windows is
  therefore blocked; see OHOS / Linux verification.
- **GCC / Clang** (Linux aarch64 + glibc, OHOS-aarch64 + musl) —
  supported. The `nimrtc_link_gmssl()` helper does not depend on
  Linux-specific APIs.

---

## 6. Runtime switching during the same process

Because both factories are registered with `core::PluginRegistry`,
you can run two engines with different backends in the same process:

```cpp
EngineConfig cfgA;                 // default = wolfSSL
EngineConfig cfgB; cfgB.dtls_name = "gmssl";

NimRTCEngine A(cfgA);
NimRTCEngine B(cfgB);
A.open();   // wolfSSL
B.open();   // gmssl

// Each engine uses its own backend; SRTP keys are derived per session.
```

This is how the OH-DOD-4 + OH-DOD-5 verification suite works
(same-process harness, two engines, two backends, mutual
handshake).

---

## 7. Interoperability notes

### 7.1 wolfSSL ↔ Chrome / Firefox / Safari

Default cipher suite matches the WebRTC mandatory list. Loopback
Case D is green on:

- Windows x86_64 (MSVC, wolfSSL default);
- Linux x86_64 (GCC/Clang, wolfSSL default);
- Linux aarch64 (GCC, compile-green; deployment validate
  separately);
- macOS arm64 (Apple Clang).

### 7.2 GMSSL ↔ Chrome / Firefox / Safari

Default cipher suite also matches the WebRTC mandatory list (per
ADR-013). Practical interop has been verified for ECDSA P-256 +
SM4-GCM-compatible suites:

| Peer | Backend | Result |
|---|---|---|
| Chrome (P-256 ECDSA self-signed) | GMSSL (western suite) | ✅ |
| Firefox | GMSSL (western suite) | ✅ |
| Safari | GMSSL (western suite) | ✅ |
| Older browser (RSA cert only) | GMSSL | requires patch to add cipher |
| Any browser (SM-only cipher) | GMSSL | ❌ (no current browser supports SM cipher suites) |

For pure SM compliance scenarios (no browser involved), swap the
cipher priority table in `dtls_gmssl_session.cpp` to put
`ECDHE-SM2-SM4-GCM-SM3` first. This breaks browser interop
deliberately.

### 7.3 OHOS-NimRTC ↔ OHOS-NimRTC (both GMSSL)

The OHOS Profile default is `dtls_name = "gmssl"`. Two OHOS NimRTC
peers with GMSSL default negotiate TLCP and complete the SRTP key
derivation. Round-trip works since v0.12.0-alpha.

### 7.4 OHOS-NimRTC ↔ Desktop-NimRTC

For OHOS ↔ Windows/Linux NimRTC interop, either:

- both sides use wolfSSL (set `cfg.dtls_name = "wolfssl"` on the
  OHOS side; less common since OHOS rarely ships wolfSSL);
- both sides use GMSSL western cipher suite (the OHOS default
  works here because wolfSSL on desktop also advertises
  `ECDHE-ECDSA-AES128-GCM-SHA256`).
- or use a third peer (Chrome / Firefox) as a relay.

### 7.5 No SM-only browser peers

If your integration model assumes browser peers running on
OpenHarmony's H5 stack — there is currently no browser with native
TLCP support. The OHOS-NimRTC ↔ browser scenario is the same as
NimRTC ↔ browser on Linux, except that the NimRTC side happens
to use GMSSL.

---

## 8. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `cmake --preset ...` complains about `SSL_export_keying_material` | OpenSSL 1.1.x without openssl-srtp patch | install GMSSL or apply openssl-srtp patch |
| `dtls_name = "gmssl"` returns `kErrNotFound` at `engine.open()` | `NIMRTC_ENABLE_DTLS_GMSSL=OFF` | reconfigure with `=ON` |
| `dtls_name = "gmssl"` builds but link fails | wrong library order — gmssl must come **after** the engine target | check `target_link_libraries(... nimrtc_dtls_seam ...)` |
| `engine.open()` succeeds but DTLS stays in `HelloSent` forever | missing `tick()` calls in caller; DTLS pump needs ~50 ms cadence | drive a loop calling `engine.tick()` |
| Browser rejects SDP `a=fingerprint` | cert was generated with a different algorithm than `"sha-256"`; wolfSSL defaults to SHA-256, GMSSL defaults to SHA-256 but if you swap the cert source you may break this | verify `cfg.dtls_name` matches the cert-signer backend |
| `dtls.connected() == true` but inbound audio silent | SRTP keys silently 0-byte (openSSL without srtp patch) | switch to GMSSL build; check `nimrtc_link_gmssl` output |
| Test `test_dtls_gmssl_tlcp` doesn't link | `NIMRTC_ENABLE_DTLS_GMSSL=OFF` | enable the flag, rebuild |

---

## 9. See also

- [ADR-013: GMSSL DTLS backend](../adr/ADR-013-gmssl-dtls-backend.md) —
  decision record, alternatives considered, scope of SM cipher work.
- `src/engine/include/nimrtc/engine/engine.hpp` — `EngineConfig::dtls_name`
  field (see the comment block above it for the accepted backend ids).
- [plugin_author_guide.md](../plugin_author_guide.md) §6.1 — how to
  write a custom DTLS backend.
- `src/dtls/include/nimrtc/dtls/dtls_session_iface.hpp` —
  interface contract.
- `src/dtls/include/nimrtc/dtls/dtls_session_factory.hpp` — factory
  contract.
- `src/dtls/src/dtls_gmssl_session.cpp` — reference implementation.
- `tests/test_dtls_gmssl_tlcp.cpp` — in-process round-trip test.
- `cmake/NimRTCVendored.cmake` — `nimrtc_link_gmssl()` helper.

> **One-line summary:** choose `cfg.dtls_name = "wolfssl"` or
> `"gmssl"`, build with the matching CMake option, everything
> else just works.
