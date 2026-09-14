# Repository Layout

This document describes the directory layout of NimRTC. For high-level
project context, see [`README.md`](README.md) and
[`docs/zh/NimRTC-V2-技术文档.md`](docs/zh/NimRTC-V2-技术文档.md).

> **Reminder:** P0 = scaffold only (no implementations yet). Implementation
> lands P1..P4 per §13 of the technical document.

---

## Top-level files

| File                 | Purpose                                                               |
|----------------------|-----------------------------------------------------------------------|
| `LICENSE`            | Apache-2.0 full text — do not edit                                   |
| `NOTICE`            | Third-party attributions (every vendor release appended)               |
| `README.md`         | Project introduction, status, contribution entry points                 |
| `CHANGELOG.md`      | Semantic version + change log                                         |
| `CONTRIBUTING.md`   | DCO signing, PR process, commit message format                        |
| `SECURITY.md`       | Private vulnerability disclosure process                               |
| `CODEOWNERS`        | Per-module maintainer assignments                                     |
| `.clang-format`     | C++20 code formatter (LLVM style, 100-col limit)                      |
| `.editorconfig`     | Editor settings (UTF-8, 4-space indent, LF)                          |
| `.gitattributes`    | Line-ending normalisation (LF for source files)                        |
| `.gitignore`        | Standard C++/CMake/IDE exclusions                                   |
| `CMakeLists.txt`    | Root build entry — declares options, includes cmake/, traverses subdirs |
| `CMakePresets.json` | `cmake --preset` shortcuts (debug, release, asan, ubsan, coverage)    |
| `ARCHITECTURE.md`   | This file (directory layout, dependency rules)                       |

## Top-level directories

| Dir          | Contents                                                              |
|--------------|-----------------------------------------------------------------------|
| `cmake/`     | Cross-module CMake helpers (compiler flags, vendor linking, tests)       |
| `src/`       | All source code and vendored third-party libraries                     |
| `tests/`     | Cross-module / integration tests                                       |
| `examples/`  | Sample programs (gated by `NIMRTC_BUILD_EXAMPLES=ON`)                 |
| `interop/`   | Chrome/Firefox interop test fixtures and runners                      |
| `tools/`     | Development and release utility scripts                               |
| `docs/`      | Doxygen API reference, ADRs, and Chinese technical document            |

### Source layout (`src/`)

```
src/
├── core/         ← Header-only foundation types (time / error / buffer /
│                   PluginRegistry singleton — Layout Invariant 6)
├── plugins/      ← Header-only plugin interfaces (ITransport, IRTP, ISDP, IJB,
│                   IAudio3A, ICodec, IVideo*, IICETransport, hw_seam, …)
├── modules/      ← Concrete implementations (each = cmake + include + src + tests)
│   ├── rtp/
│   ├── sdp/
│   ├── jb/
│   ├── bwe/
│   ├── dtls/
│   ├── srtp/
│   ├── ice/
│   ├── opus/
│   ├── audio3a/
│   ├── video_frame/
│   ├── video_payload/
│   ├── video_jb/
│   ├── video_sink/
│   ├── video_source/
│   ├── video_pipeline/
│   ├── … (see src/modules/CMakeLists.txt for the full list)
├── engine/       ← NimRTCEngine (wires plugins together; composition layer
│                   only — no concrete module headers in its public API,
│                   see Layout Invariant 4)
├── log/          ← Logger / Sink implementation (nimrtc_log STATIC — the only
│                   compiled module in src/ besides the engine itself)
└── third_party/ ← Vendored upstream: mbedTLS, libsrtp, libopus, libjuice
                    + special-purpose helpers (googletest, nlohmann_json)
```

### `src/third_party/` layout

The third-party tree mixes upstream C libraries (cryptography, media) and
two helper categories. Each is a leaf CMake subdirectory:

| Subdir | Type | Wired by | Notes |
|--------|------|----------|-------|
| `mbedtls/`, `libsrtp/`, `libopus/`, `libjuice/` | Upstream crypto/media libraries | `NIMRTC_VENDORED=ON` (default) | Compiled into NimRTC; `NIMRTC_VENDORED` must remain `ON` in our releases (Layout Invariant 5) |
| `googletest/` | Test framework | `NIMRTC_BUILD_TESTS=ON` | Vendored to keep CI offline; see `cmake/NimRTCTest.cmake` |
| `nlohmann_json/` | JSON helper (single-header) | `NIMRTC_MODULE_ASSEMBLY=ON` | Vendored as an INTERFACE library so the assembly module's profile-loading can run fully offline |

### Docs layout (`docs/`)

```
docs/
├── api/   ← Doxygen output (gated by NIMRTC_BUILD_DOCS=ON)
├── adr/   ← Architectural Decision Records (ADR-001-plugin-system.md)
└── zh/    ← Chinese technical document (always present)
```

---

## Plugin Architecture (ADR-001)

Every major module is an abstract C++ interface in `src/plugins/`. Users
replace any layer by implementing the interface and registering a factory:

```cpp
// Register a proprietary transport plugin:
static const SimpleTransportFactory<MyGateway> factory{"my_gateway", "My gateway"};
NIMRTC_REGISTER_TRANSPORT(my_gateway, &factory);

// Use it:
NimRTCEngine::Config cfg;
cfg.transport_name = "my_gateway";   // look up by string ID
NimRTCEngine engine(cfg);
engine.open();
```

See [`docs/adr/ADR-001-plugin-system.md`](docs/adr/ADR-001-plugin-system.md) for the full design.

---

## Layout invariants (must always hold)

1. **`src/core/` is the only INTERFACE library** and exposes **only stable types**.
   No `src/core/src/` files — when log sinks get implementations in P1, a new
   `nimrtc_log` target is added; `src/core/` stays header-only and stable.

2. **`src/plugins/` is the only plugin interface layer** (header-only INTERFACE library `nimrtc_plugins`). It contains no implementations — only abstract classes and factory interfaces. All concrete implementations live in `src/modules/`.

3. **Every module is self-contained under `src/modules/<name>/`** with
   `CMakeLists.txt` + `include/` + `src/` + `tests/`. Modules may
   declare cross-module PUBLIC deps only via documented headers in another
   module's `include/`. No reaching into another module's `src/`.

4. **`src/engine/` wires plugins together.** Applications link `nimrtc::engine`,
   not individual modules. The engine's public header (`engine.hpp`)
   deliberately does **not** include any concrete module header — all
   concrete state lives in a PIMPL `Impl` defined inside `engine.cpp`.
   Consumers who need concrete types (e.g. `nimrtc::dtls::DtlsSession`)
   include those headers themselves; they aren't dragged in transitively.

5. **`src/third_party/` is the only place vendor code lives.** Nothing may
   link system-installed crypto / DTLS / SRTP libraries — `NIMRTC_VENDORED`
   must always remain `ON` in our releases.

6. **`docs/api/`, `docs/adr/`, and `examples/` are gated by CMake options**, so
   a vanilla `cmake -B build -DNIMRTC_BUILD_TESTS=OFF` produces a clean
   core/modules build with no optional dependencies.

7. **`cmake/` is a tool layer, not part of `src/`**. It lives at the project
   root alongside `CMakeLists.txt` because it is required to configure the build
   before any `src/` content is compiled.

8. **No header in any module exports a global mutable state.** Singletons
   (Logger, PluginRegistry, config registries) live under `nimrtc::core::` only.
   A back-compat shim in `plugins/registry.hpp` re-exports
   `core::PluginRegistry` so older call sites continue to work.

---

## CMake target name conventions

| Form                        | Meaning                                                      |
|-----------------------------|--------------------------------------------------------------|
| `nimrtc_<module>`           | CMake library target — `nimrtc_core`, `nimrtc_rtp`, etc.     |
| `nimrtc::<module>`          | Namespaced alias for consumers (`nimrtc::rtp`, etc.)          |
| `nimrtc_vendor_<name>`      | Vendored library target (e.g. `nimrtc_vendor_libsrtp`)      |
| `nimrtc::vendor::<name>`    | Alias for vendored targets (`nimrtc::vendor::libsrtp`)       |

Examples:

```cmake
target_link_libraries(my_app  PRIVATE  nimrtc::rtp  nimrtc::jb)
target_link_libraries(my_app  PRIVATE  nimrtc_vendor_libsrtp)
```

Vendored linking is wrapped in helpers from `NimRTCVendored.cmake`
(`nimrtc_link_libsrtp(my_app)` etc.) so consume sites stay short.

---

## Header convention

Each public type lives under its module's `include/`:

```
src/core/include/nimrtc/core/{time,error,bytes,log}.hpp
src/modules/rtp/include/nimrtc/rtp/packet.hpp
src/modules/sdp/include/nimrtc/sdp/session_description.hpp
src/modules/jb/include/nimrtc/jb/jitter_buffer.hpp
```

Path ↔ namespace mapping is always
`<module_dir>/include/nimrtc/<module>/<name>.hpp`
↔ namespace `nimrtc::<module>`. This is enforced by the CMake and CMakeLint
configs (added in P1).

---

## Build presets

Use `cmake --preset <name>` to configure without specifying generator options:

| Preset    | Compiler | Build type | Notes                           |
|-----------|---------|-----------|---------------------------------|
| `debug`   | GCC     | Debug     | ASan/UBSan OFF                  |
| `release` | GCC     | Release   | Optimised, warnings→errors     |
| `asan`    | GCC     | Debug     | AddressSanitizer ON             |
| `ubsan`   | GCC     | Debug     | UndefinedBehaviourSanitizer ON  |
| `coverage`| GCC     | Debug     | Coverage instrumentation        |
| `debug.msvc`, `release.msvc`, `asan.msvc` | MSVC | — | Visual Studio toolchain |

Run: `cmake --build --preset <name>` and `ctest --preset tests`.
