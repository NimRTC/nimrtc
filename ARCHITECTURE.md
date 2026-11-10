# Repository Layout

This document describes the directory layout of NimRTC. For high-level
project context, see [`README.md`](README.md) and
[`docs/zh/architecture.md`](docs/zh/architecture.md).

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
├── dtls/         ← DTLS **seam** family (see "The seam-family directories"
│                   below): the IDtlsSession / IDtlsSessionFactory interfaces,
│                   the GmSSL backend, and the wolfSSL *factory*
├── raw_udp/      ← Raw-UDP seam family: ISocketFactory + the ARQ transport
├── sctp/         ← SCTP seam family: ISctpSocketFactory + the usrsctp socket
├── transport/    ← Transport-stack seam family: ITransportStackFactory
├── web_transport/← WebTransport seam family: the msquic-backed data plane
└── third_party/ ← Vendored upstream: mbedTLS, libsrtp, libopus, libjuice
                    + special-purpose helpers (googletest, nlohmann_json)
```

#### The seam-family directories (`src/dtls/`, `src/raw_udp/`, `src/sctp/`, `src/transport/`, `src/web_transport/`)

Five subsystems keep their code at the top level of `src/` instead of under
`src/modules/<name>/`.  This is the historical PAL Slice layout, and it is
**not** a violation of Layout Invariant 3: each of those directories is still
self-contained (own `CMakeLists.txt` + `include/` + `src/` + `tests/`), it just
does not use the `modules/` path prefix.

Each directory holds **the seam for one optional subsystem**: the abstract
interface, the factory interface, and (where the subsystem needs a default
implementation) that implementation.  The `src/modules/<name>/` tree holds the
modules the engine always composes.

**DTLS has code in two places — this is the one place where "which file is
authoritative?" has a real answer:**

| Path                          | Role                                                                     |
|-------------------------------|--------------------------------------------------------------------------|
| `src/dtls/`                   | The **seam**: `dtls_session_iface.hpp` (`IDtlsSession`), `dtls_session_factory.hpp` (`IDtlsSessionFactory`), `dtls_types.hpp`, the **GmSSL** backend, and the **wolfSSL factory** (`dtls_wolfssl_factory.cpp`).  This directory owns *how a backend is chosen and what the interface looks like*. |
| `src/modules/dtls/`           | The **wolfSSL session implementation**: `dtls_wolfssl_session.cpp` (the handshake, the record layer, the PRF) and `dtls_prf.cpp`.  This directory owns *how one backend behaves*. |

So: **`src/modules/dtls/` holds the authoritative wolfSSL session
implementation**, and **`src/dtls/` holds the authoritative seam plus the
GmSSL alternative**.  A change to the DTLS *protocol behaviour* belongs in
`src/modules/dtls/`; a change to the *interface* or to the *backend selection*
belongs in `src/dtls/`.  Both trees are built and tested independently
(`src/dtls/tests/` covers the factory/selection, `src/modules/dtls/tests/`
covers the handshake reassembly).

Two facts that keep the two trees from looking like duplicated work:

* **wolfSSL is the only DTLS provider in the shipped build.**  The original
  hand-written DTLS state machine is gone; `src/modules/dtls/src/dtls_session.cpp`
  is now a thin PIMPL forwarder that delegates every call to
  `DtlsSessionWolfSSL` (defined in
  `src/modules/dtls/src/dtls_wolfssl_session.cpp`).  The GmSSL backend under
  `src/dtls/` is an **alternative provider behind the same seam**, not a
  second copy of the protocol.
* **`src/dtls/src/dtls_wolfssl_session_iface.hpp` is a re-export forwarder**,
  not a second implementation: it pulls the concrete class in from
  `src/modules/dtls/` so consumers of the seam can name it without knowing
  which subtree it lives under.

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

## Threading model

NimRTC has **no single event loop**.  There are four places that own a thread,
and the host owns a fifth.  This section says which is which, because "is this
call thread-safe?" is the question a host has to answer before it can embed the
engine.

### The threads that exist

| Owner | Thread(s) | Source | What it does |
|-------|-----------|--------|--------------|
| **The host** | 1 (the "engine thread") | host code | Calls `open()` / `tick()` / `send_audio()` / `send_video()` / `close()`.  The engine **never starts a thread of its own** — there is no `std::thread` anywhere under `src/engine/`.  All engine state is therefore owned by whichever thread calls `tick()`. |
| **ICE** | 1 consent thread per transport | `src/modules/ice/src/ice.cpp` (`consent_thread_`) | Periodically checks consent freshness and, when consent lapses, tears the selected pair down.  It touches only the ICE transport's own state, which it guards with its own mutex. |
| **video_source** | 1 cadence thread per source | `src/modules/video_source/src/video_source.cpp` (`thread_`) | Generates frames at the configured cadence when the source is a synthetic/test pattern.  The engine deliberately does **not** wire this thread into the encoder (the video codec is not thread-safe) — the host pulls frames from the source and pushes them via `send_video()`. |
| **raw_udp (ARQ transport)** | 2 — a recv thread and a retransmit thread | `src/raw_udp/include/nimrtc/raw_udp/arq_raw_udp.hpp` | Only the ARQ raw-UDP transport has these; the plain UDP socket path does not.  The recv thread fills a queue; the retransmit thread re-sends unacknowledged frames. |
| **usrsctp / SCTP** | 0 | `src/sctp/src/usrsctp_socket.cpp` | The `g_shared_udp_thread` global is **declared but never started** — usrsctp runs its own internal UDP listener.  Do not read that symbol as "a thread NimRTC starts". |

The WebTransport data plane is different again: it is handed to **msquic**,
which runs its own thread pool.  NimRTC does not own or schedule those threads;
it only hands the transport over and receives callbacks.

### The rules that follow

1. **One thread drives the engine.**  `tick()` and every other call that
   touches pipeline state (`send_audio()`, `send_video()`, `create_offer()`,
   `process_remote_sdp()`, `close()`) must be made from the *same* thread.
   The engine header states this in the same words for the audio bitrate
   adaptation: "the same rule the rest of the engine already follows".
2. **Read-only diagnostics are the exception.**  Accessors that only read
   already-published state (the timeline diagnostics, the ICE state string,
   `config()`) are documented as safe to call from any thread after `open()`.
   Anything that *mutates* a plugin is not.
3. **The scheduler is mutex-guarded, and its drain callback runs under the
   lock.**  `Scheduler::drain_with()` takes `impl_->mu_` for the whole drain,
   and invokes the per-packet callback while still holding it.  A callback must
   therefore **not** re-enter the scheduler (`enqueue()`, `drain()`,
   `on_bwe_update()`); doing so deadlocks.
4. **Plugin interfaces carry their own thread-safety contract.**  The
   `IDtmfCodec` seam, for example, states that the engine drives it from its own
   thread only and that the interface does not promise internal locking.
   Read the header of the plugin you are replacing before calling it from
   anywhere else.
5. **There is no `IIoLoop` abstraction.**  This is a known gap, not a design
   choice: the four thread families above are independent, and the engine
   deliberately does not impose an event loop on the host so that it can be
   embedded in someone else's process (an in-process SFU, a game engine).

---

## Layout invariants (must always hold)

1. **`src/core/` is the only INTERFACE library** and exposes **only stable types**.
   Documented exception (PAL Slice 2 / Windows DLL boundary fix, v0.11.0):
   `src/core/src/plugin_registry.cpp` holds the `PluginRegistry::instance()`
   singleton body in a STATIC `nimrtc_registry` lib (singleton unified across
   consumers), and `src/core/src/pal_default_registrars.cpp` holds the
   `kDefaultRegistrars[]` table in OBJECT `nimrtc_core_objects` (link-time
   cycle avoidance). Both propagate INTERFACEly via `nimrtc::core`. See
   `src/core/CMakeLists.txt` header comment.

2. **`src/plugins/` is the only plugin interface layer** (INTERFACE library `nimrtc_plugins`). It contains only abstract classes and factory interfaces, plus one documented exception: `src/plugins/src/hw_video_backend.cpp`, the `HwVideoBackendRegistry` comparator/auto-load implementation (mutex-guarded, no global mutable state), compiled as STATIC and noted in `src/plugins/CMakeLists.txt`. All other concrete implementations live in `src/modules/`.

3. **Every module is self-contained under `src/modules/<name>/`** with
   `CMakeLists.txt` + `include/` + `src/` + `tests/`. Modules may
   declare cross-module PUBLIC deps only via documented headers in another
   module's `include/`. No reaching into another module's `src/`.

4. **`src/engine/` wires plugins together.** Applications link `nimrtc::engine`,
   not individual modules. The engine's public header (`engine.hpp`)
   deliberately does **not** include any concrete module header — all
   concrete state lives in a PIMPL `Impl` defined inside `engine.cpp`.
   `engine.cpp` itself includes concrete module headers (ice/rtp/dtls/srtp/…)
   and links the module libs PRIVATE — that is the composition layer doing
   its job, not a leak into consumers. Consumers who need concrete types
   (e.g. `nimrtc::dtls::DtlsSession`) include those headers themselves;
   they aren't dragged in transitively.

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
