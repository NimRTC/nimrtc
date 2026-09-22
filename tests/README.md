# NimRTC Test Suite

> Where the test files live, how to invoke them, and how to add your
> own. For the framework conventions see [`src/third_party/googletest/`](../../src/third_party/googletest/).
>
> **Pre-requisites**: `cmake --preset debug` already configured.

---

## 1. Layout

```
tests/
├── README.md                  ← this file
├── CMakeLists.txt             ← top-level test driver
│
├── test_<subsystem>_*.cpp     ← gtest unit / cross-module tests
├── test_<integration>*.cpp   ← E2E over real or in-process I/O
├── nvenc_probe.cpp            ← ad-hoc driver binaries (delete after commit)
│
├── plugins/                   ← plugin-impl-specific tests
│   └── test_audio3a_tap.cpp
│
├── wolfssl_dtls/              ← standalone DTLS tests (WolfSSL only)
│   ├── dtls_client.cpp
│   ├── dtls_server.cpp
│   └── bridge/wolfssl_responder.cpp
│
├── certs/                     ← generated test certs (NOT committed)
│   ├── server-ecc.pem
│   └── ecc-key.pem
│
└── CMakeFiles/...             ← build output (in build/ tree)
```

Tests are organised in three layers:

1. **Pure unit** — single module, no I/O. Mocks / stubs ok. Built as
   gtest binaries.
2. **Integration** — multiple modules, real or in-process I/O.
   Mostly gtest; occasionally standalone binaries with their own
   `main()`. Both flavours go through `ctest`.
3. **E2E** — process-boundary tests that talk to Chrome / Firefox
   via the harness in `tools/run_e2e_acceptance.py`.

There is **no mock framework** in NimRTC; plugin tests use real
implementations wherever the dependency is small (e.g. the JB test
plumbs a real `AdaptiveJB` instance), and stub plugins
(`NullAudio3A`, etc.) for heavy interfaces.

---

## 2. Build & run

### 2.1 Configure with tests

```bash
cmake --preset debug                       # NIMRTC_BUILD_TESTS=ON by default
cmake --build build --config Debug -j       # build everything incl. tests
```

For release builds:

```bash
cmake --preset release -DNIMRTC_BUILD_TESTS=ON
cmake --build build --config Release -j
```

### 2.2 Run all tests

```bash
ctest --preset tests --output-on-failure
```

The `tests` preset wraps `ctest --test-dir build` with consistent
output formatting and parallel-by-default settings.

For a single test:

```bash
ctest --preset tests -R test_dtls_gcm_aead --output-on-failure
# Equivalent:
cd build && ctest -R test_dtls_gcm_aead --output-on-failure
```

For one test directly (without ctest), useful when attaching a
debugger:

```bash
./build/tests/Debug/test_dtls_gcm_aead       # MSVC multi-config
./build/tests/test_dtls_gcm_aead             # single-config
```

### 2.3 Common test patterns

| Test | What it validates | Where |
|---|---|---|
| `test_dtls_gcm_aead` | AES-GCM AEAD crypto primitives | `tests/` |
| `test_dtls_inproc`   | Two in-process DtlsSession objects complete a wolfSSL handshake | `tests/` |
| `test_dtls_handshake_e2e` | Identical to `test_dtls_inproc` but asserts the post-handshake SPKI fingerprint pin matches SDP | `tests/` |
| `test_dtls_gmssl_tlcp` | Identical handshake pattern with `DtlsSessionGmSSL`; requires `NIMRTC_ENABLE_DTLS_GMSSL=ON` | `tests/` |
| `test_srtp` | libsrtp2 round-trip + replay + auth | `tests/` |
| `test_dtls_over_arq_udp` | DTLS over ARQ raw UDP over loopback | `tests/` |
| `test_srtp_over_dtls_over_arq_udp` | Full protocol stack E2E | `tests/` |
| `test_rtp_srtp_arq_e2e` | RTP / SRTP / ARQ together | `tests/` |
| `test_dtls_seam` | `IDtlsSession` / `IDtlsSessionFactory` factory lookup, default-plugins registration | `tests/` |
| `test_engine_plugin_loading` | Every engine `*_name` factory lookup resolves on a fresh process | `tests/` |
| `test_datachannel_engine` | DataChannel creation through the engine (DC-1 Gate #1 §7 fallback) | `tests/` |
| `test_sctp_usrsctp` | usrsctp backend (TPAL-5) | `tests/` |
| `test_plugins/test_audio3a_tap` | PCM tap surface — pre/post 3A int16 + float variants | `tests/plugins/` |
| `test_video_plugin_interfaces` | Polymorphism / capability flag invariants for video plugins | `tests/` |
| `test_engine_video_plugins` | Engine resolves all 4 video plugins | `tests/` |
| `test_hw_plugin_seam` | HW backend registration + capability flag (NVENC etc., no GPU required) | `tests/` |
| `test_dtls_*_kat` | Known-answer tests against DTLS PRF | `tests/` |
| `test_dtls_x25519` | X.25519 / Curve25519 path (when wolfSSL is built with `--enable-curve25519`) | `tests/` |

The full list is in `tests/CMakeLists.txt` and is rendered when you
run `ctest -N` (list mode).

---

## 3. Test naming conventions

| Filename pattern | Style |
|---|---|
| `test_<module>_*.cpp` | gtest binary, single-module unit test. |
| `test_<integration>*.cpp` | gtest binary, multi-module integration. |
| `test_<a>_<b>_e2e.cpp` | gtest binary, full-stack E2E with real UDP loopback. |
| `*_probe.cpp` / `*_probe_v2.cpp` | Ad-hoc diagnostic binaries (see `docs/hw_plugin_seam.md` §12.2). **Don't commit these unless they're reusable.** |
| `wolfssl_dtls/<role>.cpp` | Standalone DTLS client / server / bridge binary; not gtest, has its own `main()`. |

---

## 4. Authoring a new test

### 4.1 Decide the layer

- **Pure unit** of a module that doesn't touch the network or another
  module: put under `tests/` (cross-module) or `src/<module>/tests/`
  (intra-module) — convention is the former for cross-module tests.
- **Multi-module integration** with real or in-process I/O: `tests/`,
  wired through the cross-module `CMakeLists.txt`.
- **Process-boundary E2E against Chrome / Firefox**: harness lives in
  `tools/run_e2e_acceptance.py`; the test binary itself is in `tests/`,
  but you run it through the harness. See
  `tools/run_e2e_acceptance.py --help`.

### 4.2 Use the right glue function

`tests/CMakeLists.txt` exposes three wrappers:

```cmake
add_e2e_test(name source)                 # standalone main()
add_srtp_test(name source)                # needs libsrtp
add_dtls_handshake_test(name source)      # needs wolfssl / gmssl
```

For gtest binaries, the standard `add_executable` + `target_link_libraries(nimrtc::engine)`
pattern works. Add the test to `CMakeLists.txt` so it appears in
`ctest`.

### 4.3 Run on multiple platforms

The CI matrix (`linux-x86_64`, `windows-x86_64`, `macos-arm64`,
`linux-aarch64` `if: false`) runs `ctest --preset tests` on each.
For local verification:

```bash
# Multi-config (MSVC):
cmake --build build --config Debug --target <test-name>

# Single-config (Ninja):
cmake --build build --target <test-name>
```

### 4.4 Don't commit secrets / generated files

Tests that need certs write to `tests/certs/` (gitignored). Use
the helper at `tools/gen_test_certs.sh` (or its `.ps1` equivalent)
to regenerate deterministically.

---

## 5. What does each layer's CI look like?

| Layer | CI runs | Configuration |
|---|---|---|
| Unit (`src/<module>/tests/`) | On every push to main | All platforms |
| Cross-module (`tests/`) | On every push to main | All platforms |
| E2E against Chrome/Firefox (`tests/` via `tools/run_e2e_acceptance.py`) | Nightly | Windows only (until v1.0); Linux planned for v1.0 |
| E2E against OHOS device | N/A (Tech Preview) | OH-DOD-5 + OH-DOD-6 — best-effort runner |

If a new test fails on Linux but passes on Windows (or vice versa),
file an issue with `[interop]` prefix; it's almost certainly a
platform-specific bug worth a separate fix.

---

## 6. Pre-commit checks

Before pushing changes to a test file:

```bash
# 1) The test should compile clean under -Wall -Wextra -Werror
cmake --build build --config Debug -j

# 2) Run the full suite once
ctest --preset tests --output-on-failure -j

# 3) If you added a new test, list it
ctest --preset tests -N | grep test_<yours>

# 4) Profile it: if your new test takes > 1 s of wall time, document
#    why in the test's file header so others don't accidentally
#    lengthen it further.
```

---

## 7. Where to put weird tests

- **Hardware-dependent tests** (real GPU/SOC). These are not in
  `tests/` — they live in the SDK vendor's `tests/hw/` directory.
  See `docs/hw_plugin_seam.md` §11.4 for the dispatch story.
- **Fuzzers**. `tools/fuzz_*` and the `fuzzit/` directory.
- **Performance benchmarks**. `tools/benchmark_size.py` and
  Perfsuite-style script (in development).

---

## 8. See also

- `tools/run_e2e_acceptance.py` — Chrome / Firefox acceptance harness.
- `docs/zh/architecture.md` §10 — testing strategy.
- `docs/plugin_author_guide.md` §7 — testing your plugin.
- `examples/` — `loopback-p2p` is structured as a self-test; the
  smoke test pattern is in its `README.md`.
- `CONTRIBUTING.md` — DCO requirement for commits.
- `src/third_party/googletest/` — vendored gtest 1.12.1.
- `tests/CMakeLists.txt` — master test list (`grep "add_test"`).
