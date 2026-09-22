# Building NimRTC for OpenHarmony (OHOS) — Tech Preview

> **Audience**: integrators targeting OpenHarmony aarch64 devices
> (or any environment running the OHOS SDK + clang/llvm
> cross-toolchain).
>
> **Status**: v0.12.0-alpha — **Tech Preview**. The toolchain and
> CMake preset are landed (Stage 0 ✅); muscle-stage 4-platform
> cross-compile passes (Stage 1 🔶); GMSSL DTLS backend landed
> (Stage 2 ✅); CI runner pool integration is best-effort (Stage 3
> 🔶). Read the [OHOS support plan](../plan/openharmony-support.md)
> for the engineering decisions behind the choices below.

---

## 1. Why OpenHarmony is in scope

Two converging motivations in the README user-profile:

1. **Xinchuang / GM/T cryptography** — domestic deploy targets need
   SM2/SM3/SM4. The OpenHarmony ecosystem is the only mobile-grade
   domestic OS with a credible native toolchain (DevEco Studio +
   native API in C/C++).
2. **Embedded Linux aarch64** — the same ARMv8 cross-compile path
   NimRTC already supports (4-platform CI green). Adding OHOS
   doesn't require a new architecture; it requires a new libc
   (musl) and a different SDK layout.

OHOS-aarch64 is the gateway platform for these users. The plan
document calls out a third motivation — the marketing story that
NimRTC is "GM/T ready" only becomes honest once OHOS actually
builds against GMSSL.

---

## 2. Prerequisites

### 2.1 Hardware

- **Build host** — any Linux x86_64 (or WSL2) box with ~16 GB RAM;
  OHOS cross-compile of NimRTC consumes ~1.5 GB and finishes in
  ~5 minutes from cache.
- **Target** — OHOS-aarch64 device, OpenHarmony 4.0+
  (API 9+). Recommended single-board test devices:
  Dayu200 / RK3568, aOSP reference boards with OHOS 4.0+ ported.

### 2.2 Software

Install on the build host:

```bash
# 1) OHOS Native SDK (DevEco Studio native toolchain)
#    Download from: https://developer.harmonyos.com/cn/develop/deveco-studio
#    Extract; this is the toolchain root (set OHOS_NATIVE_HOME).

# 2) OHOS LLVM clang (part of the Native SDK above)
$ ls $OHOS_NATIVE_HOME/llvm/bin/clang++
$OHOS_NATIVE_HOME/llvm/bin/clang++ --version
# Expect: clang version 14+ for OHOS 4.0

# 3) Perl (for the vendored wolfSSL / opus / usrsctp configure scripts)
sudo apt install -y perl python3

# 4) Ninja + CMake
sudo apt install -y ninja-build cmake

# 5) (optional) GMSSL if you also want SM cipher suite testing
#    See docs/guides/switching-dtls-backend.md §5.
```

The toolchain layout assumed by `cmake/toolchains/ohos-aarch64.cmake`:

```
$OHOS_NATIVE_HOME
├── llvm/                     # clang/llvm bin + sysroot
│   ├── bin/{clang,clang++}
│   └── lib/clang/.../include
└── sysroot/                  # OHOS C/C++ headers and libs (musl)
```

---

## 3. Quick build

### 3.1 Configure

```bash
# From repo root, on Linux x86_64:
cmake --preset debug.ohos-arm64
```

This preset:

- picks `cmake/toolchains/ohos-aarch64.cmake`;
- sets `CMAKE_SYSTEM_NAME=OHOS`, `CMAKE_SYSTEM_PROCESSOR=aarch64`;
- sets `-stdlib=libc++` and links against the OHOS C++ STL
  (`c++_shared.so`);
- targets the OHOS SDK's sysroot by default; override
  via `OHOS_NATIVE_HOME` env var.

If the preset fails at the configure step, see
[§"Troubleshooting"](#7-troubleshooting) below — most likely cause
is `OHOS_NATIVE_HOME` not exported, or an OHOS SDK older than 4.0.

### 3.2 Build

```bash
cmake --build build-ohos --target nimrtc_engine -j
cmake --build build-ohos --target loopback-p2p -j
cmake --build build-ohos --target test_dtls_gcm_aead -j
```

Expected outputs in `build-ohos/`:

```
build-ohos/
├── src/engine/libnimrtc_engine.a       # ~5.7 MB static lib
├── examples/loopback-p2p/loopback-p2p  # ~6.7 MB ELF binary
└── tests/test_dtls_gcm_aead           # gtest unit binary
```

### 3.3 Test

```bash
cmake --build build-ohos --target test_dtls_gcm_aead
$OHOS_NATIVE_HOME/llvm/bin/llvm-objdump --syms \
    build-ohos/tests/test_dtls_gcm_aead | head -30   # sanity
# Then push to an OHOS device / emulator:
hdc_std file send build-ohos/tests/test_dtls_gcm_aead /data/local/tmp/
hdc_std shell "/data/local/tmp/test_dtls_gcm_aead"
```

For the recommended test surface (which passes today):

- `test_dtls_gcm_aead` — AES-GCM round-trip, no network deps.
- `libnimrtc_engine.a` links without unresolved symbols.

End-to-end (Chrome / Firefox peer interop from OHOS) is **out of
scope for v0.12.0** (per `docs/plan/openharmony-support.md` §2.2
Risk OH-RISK-4).

---

## 4. The OHOS toolchain

### 4.1 What the preset does

`cmake/toolchains/ohos-aarch64.cmake` is a dual-mode toolchain:

1. **Mode A — real cross clang** (preferred). Detects the OHOS
   native toolchain at `$OHOS_NATIVE_HOME/llvm/bin/clang++` and
   sets `CMAKE_C_COMPILER`, `CMAKE_CXX_COMPILER` accordingly.
2. **Mode B — clang `--target` fallback**. If the OHOS toolchain
   is missing, falls back to a host clang with
   `--target=aarch64-linux-ohos`. Useful for sanity checks before
   the SDK install completes; **does NOT** produce a working OHOS
   binary.

`cmake/NimRTCOptions.cmake` adds an `if(OHOS)` branch:

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    set(NIMRTC_PLATFORM_OHOS 1)
    add_compile_options(-stdlib=libc++)
    add_link_options(-stdlib=libc++)
    add_compile_definitions(__MUSL__)
    add_compile_options(-fvisibility=hidden)
endif()
```

`__MUSL__` lets C++ headers and STL shims take the musl-aware path
(see `docs/plan/ohos-musl-compat.md` for the audit).

### 4.2 Preset variants

| Preset | Notes |
|---|---|
| `dev.ohos-arm64` | Hidden dev preset; inherits `debug.ohos-arm64`. Use for inner-loop. |
| `debug.ohos-arm64` | Debug `-g -O0`; recommended for development. |
| `release.ohos-arm64` | Release `-O3 -DNDEBUG`; for size / performance benchmarks. |
| `tests.ohos-arm64` | CTest driver for the OHOS build. |
| `asan.ohos-arm64` | AddressSanitizer build (when ASan works on OHOS — see OH-DOD-3). |
| `asan.ohos-arm64` | AddressSanitizer build (when ASan works on OHOS). |
| `release.ohos-arm64-lite` | **Reserved**, post-1.0 milestone. |

### 4.3 Cross-compile from Windows

Cross-compiling from a Windows host is supported (the preset
detects host-Mode-A fallbacks). CI doesn't gate on Windows hosts
because LLVM on OHOS requires Linux sysroot semantics — we run
`if(false)` / `continue-on-error` here, mirroring the existing
`linux-aarch64` job policy (per OH-RISK-1).

---

## 5. Building with GMSSL

The default DTLS backend on Linux-Desktop is wolfSSL; the default
on the **OHOS profile** is GMSSL. To enable it:

```bash
cmake --preset debug.ohos-arm64 \
      -DNIMRTC_ENABLE_DTLS_GMSSL=ON \
      -DOpenSSL_ROOT=$OHOS_NATIVE_HOME/openssl    # if pre-shipped
cmake --build build-ohos --target nimrtc_engine
```

If the OHOS image you're shipping doesn't pre-install OpenSSL /
GMSSL, build GMSSL first:

```bash
# Cross-compile GMSSL 4.x to OHOS-aarch64
git clone --depth 1 -b gmssl_4_x  https://github.com/guanzhi/GmSSL.git
cd GmSSL
./Configure linux-aarch64 --prefix=$OHOS_NATIVE_HOME/gmssl \
    --cross-compile-prefix=$OHOS_NATIVE_HOME/llvm/bin/
make && make install

# Then point NimRTC at it:
cmake --preset debug.ohos-arm64 \
      -DNIMRTC_ENABLE_DTLS_GMSSL=ON \
      -DOpenSSL_ROOT=$OHOS_NATIVE_HOME/gmssl
```

For browser-interop builds (NimRTC ↔ Chrome), accept the default
western cipher suite and stop there. For GM/T compliance, swap
the cipher priority table (see ADR-013 §Decision 2).

---

## 6. How NimRTC fits into an OHOS application

### 6.1 Project layout

```
MyOHOSApp/
├── entry/                   # Standard OHOS Ability slice (UI)
├── cpp/                     # Native C++ side — NimRTC lives here
│   ├── CMakeLists.txt
│   └── src/
│       ├── main.cpp         # bootstraps NimRTCEngine + ACE UI bridge
│       └── rtc_session.cpp  # the engine lifecycle
├── cpp/third_party/
│   └── nimrtc/             # NimRTC source tree (or a subtree)
└── build-profile.json5     # externalNativeBuild + arguments
```

`build-profile.json5` excerpt:

```jsonc
{
  "externalNativeOptions": {
    "path": "./cpp/CMakeLists.txt",
    "arguments": [
      "-DNIMRTC_ENABLE_DTLS_GMSSL=ON",
      "-DOpenSSL_ROOT=/path/to/gmssl-on-ohos",
      "-DCMAKE_TOOLCHAIN_FILE=$OHOS_NATIVE_HOME/build/cmake/ohos.toolchain.cmake"
    ]
  }
}
```

### 6.2 Minimal main.cpp

```cpp
#include <nimrtc/engine/engine.hpp>

extern "C" int NimRTCMain(int /*argc*/, char** /*argv*/) {
    nimrtc::engine::EngineConfig cfg;
    cfg.local_bind_address     = "0.0.0.0";   // listen on the device's IP
    cfg.local_port_range_begin = 51000;
    cfg.local_port_range_end   = 51099;
    cfg.pcm_sample_rate_hz     = 48000;
    cfg.pcm_channels           = 1;
    cfg.dtls_name              = "gmssl";      // OHOS profile default

    nimrtc::engine::NimRTCEngine engine(std::move(cfg));
    if (engine.open() != 0) return 1;

    auto on_error = [](uint32_t err, std::string_view msg) {
        // forward to the OHOS logger (HiTrace or HiLog)
        OH_LOG_ERROR(LOG_APP, "NimRTC err=0x%{public}x msg=%{public}s",
                     static_cast<unsigned>(err), std::string(msg).c_str());
    };
    engine.set_on_error(on_error);

    while (engine.ice_state_string() != std::string("connected")) {
        engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // ... drive audio / video flow ...
    return 0;
}
```

Hook this into an OHOS `Ability::onStart()` via `napi_create_async_work`
or run it as a dedicated `pthread`. The audio capture side
(volume 4 of OHOS support plan, `ohos-audio-hal.md`, separate
milestone) is not implemented in v0.12.0 — for now use a dummy PCM
source via `engine.send_audio(...)`.

---

## 7. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `cmake --preset debug.ohos-arm64` errors with "Could not find OHOS toolchain" | `OHOS_NATIVE_HOME` not exported, or OHOS SDK < 4.0 | `export OHOS_NATIVE_HOME=/opt/ohos-sdk`; install DevEco Studio native SDK ≥ 4.0 |
| Link fails with undefined references in `__cxxabi_*` symbols | STL linkage wrong; `-stdlib=libc++` missing | ensure preset's `if(OHOS)` branch applied; re-run cmake with `-DCMAKE_CXX_FLAGS="-stdlib=libc++"` |
| `libnimrtc_engine.a` builds but ELF has musl-incompatible syscall refs | one of the vendored libs (libopus / libsrtp / libjuice) picked up glibc configurations | re-vendor with `CC=$OHOS_NATIVE_HOME/llvm/bin/clang --target=aarch64-linux-ohos`; see [plan/ohos-musl-compat.md](../plan/ohos-musl-compat.md) |
| `test_dtls_gcm_aead` runs but doesn't reach Connected | OHOS firewall blocks UDP loopback at 127.0.0.1:51000 | check `hdc_std shell`; allow via `iptables -A INPUT -p udp -j ACCEPT` for local testing |
| GMSSL link fails on `SSL_export_keying_material` | OpenSSL without srtp patch | install GMSSL or apply openssl-srtp patch; see [switching-dtls-backend.md §5.2](./switching-dtls-backend.md) |
| Build is green but binary doesn't load on device | sysroot mismatch — `--target` mismatch between compiler and link step | use Mode A (real cross clang) instead of Mode B (`--target` fallback) |

### 7.1 The R1 musl audit (read this before debugging)

`docs/plan/ohos-musl-compat.md` (the audit at §4.1 of the OHOS
support plan) captures every glibc-only dependency NimRTC has
audited. Useful as a triage checklist when porting a new
submodule.

---

## 8. Known limitations (Tech Preview)

- **No browser e2e from OHOS**. v0.12.0 OHOS Tech Preview only
  guarantees toolchain + cross-compile + minimal unit test. Loop
  back to a Linux desktop NimRTC engine for verifiable interop.
- **No audio HAL**. Audio capture / render on OHOS requires
  OHAudio / OpenSL ES selection (Stage 4 in the support plan).
  Until that's done, feed PCM via `engine.send_audio(...)`.
- **CI runner is best-effort**. The `ohos-arm64` GitHub Actions
  job runs with `continue-on-error` because the open-source runner
  pool is unstable. Local verification is the authoritative gate.
- **No SM cipher suite interop testing**. No current browser
  supports SM suites. TLCP-only peers require a custom SM-only
  cipher priority table, which breaks browser interop by design.

---

## 9. Roadmap (post-1.0)

| Milestone | Goal |
|---|---|
| v0.12.x patch | OHAudio integration (Stage 4 of OHOS support plan). Demo: NimRTC on OHOS capturing mic. |
| v0.13.0 | Stable OHS runner; full ctest green; OH-DOD-5 (test_dtls_gmssl_tlcp on device) PASS. |
| v1.0+ | Release `release.ohos-arm64-lite` preset for OHOS-Lite (RTOS) devices. |

For full schedule, see `docs/plan/openharmony-support.md` §5.

---

## 10. See also

- `docs/plan/openharmony-support.md` — engineering plan,
  alternatives considered, R-Audit, DoD gates.
- `docs/plan/ohos-musl-compat.md` — musl-vs-glibc audit.
- `docs/adr/ADR-013-gmssl-dtls-backend.md` — GMSSL backend ADR.
- `docs/guides/switching-dtls-backend.md` — DTLS backend selection.
- `cmake/toolchains/ohos-aarch64.cmake` — the toolchain file.
- `CMakePresets.json` — preset schema.
- `cmake/NimRTCVendored.cmake` — `nimrtc_link_gmssl()` helper.
- `examples/loopback-p2p/` — cross-platform minimal program.

> **One-line summary:** install OHOS 4.0+ SDK, set `OHOS_NATIVE_HOME`,
> `cmake --preset debug.ohos-arm64`, build, ship.
