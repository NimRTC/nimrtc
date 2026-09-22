# NimRTC Plugin Author Guide

> **Audience:** plugin implementors — anyone writing a new transport,
> codec, audio-3A, video pipeline, BWE, scheduler, or DTLS backend for
> NimRTC.
>
> **Reading time:** ~25 min. After reading this you can implement and
> register a plugin that runs in `nimrtc::engine::NimRTCEngine` without
> forking the engine.
>
> **Pre-requisites:** familiarity with `docs/hw_plugin_seam.md` (HW
> video backends) and `docs/zh/architecture.md` §2.3 + §6.

---

## 1. Why this document exists

NimRTC's plugin interfaces are the architectural seam — they make
fork-free swaps possible (OpenSSL ↔ wolfSSL ↔ GMSSL DTLS;
in-tree ↔ HW 3A; libjuice ↔ custom ICE transport). Without a guide
for plugin authors, the seam is unusable: every contributor ends up
duplicating engine internals or pitching `#ifdef` branches upstream.

If you are reading this you probably want to:

- ship your own **DTLS backend** (e.g. BoringSSL, OpenSSL with
  SRTP patch, mbedTLS, a smartcard-backed provider);
- swap the **default ICE transport** for QUIC or a TURN-over-TLS
  variant;
- replace the **3A pipeline** with a HW DSP backend;
- bring a **codec** your organisation owns;
- or implement a **BWE / Scheduler** strategy that doesn't fit AIMD
  or `strict_priority`.

This guide covers the registration protocol (the part that is uniform
across every plugin type), then dives into one worked example per
family.

---

## 2. The Plugin Adaptation Layer in 30 seconds

PAL is split into two layers:

1. **`src/plugins/include/nimrtc/plugins/*.hpp`** — header-only
   abstract interfaces. Pure-virtual contracts; no concrete module
   dependencies leak.
2. **`src/core/include/nimrtc/core/registry.hpp`** — `PluginRegistry`,
   a type-typed registry with one slot per plugin family:
   - `register_audio3a(id, factory*)` / `get_audio3a(id)`
   - `register_codec(id, factory*)` / `get_codec(id)`
   - `register_video_source(id, factory*)` / …sink/receiver/sender/codec
   - `register_dtls_session(id, factory*)` / `get_dtls_session(id)`
   - `register_sctp_socket(id, factory*)` / `get_sctp_socket(id)`
   - `register_raw_udp_datagram(id, factory*)` / `get_raw_udp_datagram(id)`
   - `register_transport_stack(id, factory*)` / `get_transport_stack(id)`
   - `register_bwe(id, factory*)` / `get_bwe(id)`
   - `register_scheduler(id, factory*)` / `get_scheduler(id)`

   Every backend registers under a **stable string id** (e.g.
   `"wolfssl"`, `"gmssl"`, `"aimd"`, `"webrtc_apm"`).

3. The engine resolves every capability through `PluginRegistry::get_*`
   at `open()` time. After resolution, plugin calls are **direct
   virtual dispatch** — zero runtime indirection on the hot path
   (PAL Slice 1; see `docs/adr/ADR-009-pal-slice-1.md`).

The takeaway for the plugin author: write a class that implements the
abstract base interface; ship a `SimpleXFactory<T>` that produces it;
call `NIMRTC_REGISTER_*` once at static-init time. The engine does
the rest.

---

## 3. The four-step recipe

Every plugin in NimRTC follows the same recipe. Memorise these four
steps and you are 80% done:

### Step 1 — Implement the interface

```cpp
// my_plugin.hpp
#include <nimrtc/plugins/<family>.hpp>

class MyBackend final : public nimrtc::plugins::I<Family> {
public:
    // ... override every pure-virtual method of the interface ...
};
```

The interfaces are header-only and sit in
`src/plugins/include/nimrtc/plugins/`. They declare:

- a virtual destructor,
- pure-virtual methods that take `std::span<const std::uint8_t>`,
  `BufferView`, or `std::string_view` (NimRTC prefers span-style view
  types over raw pointers, but raw pointers are allowed where the size
  must be inferred from the sample format),
- a `const`-qualified factory hook named `open()` / `close()` /
  `tick()` / etc.

### Step 2 — Provide a factory

```cpp
// my_plugin_factory.hpp
#include <nimrtc/plugins/<family>_factory.hpp>     // I<Family>Factory contract
#include <nimrtc/core/registry.hpp>

class MyBackendFactory final : public nimrtc::plugins::I<Family>Factory {
public:
    explicit MyBackendFactory(std::string_view id,
                              std::string_view display_name)
        : id_(id), display_name_(display_name) {}

    std::string_view id() const noexcept override { return id_; }
    std::string_view display_name() const noexcept override { return display_name_; }

    std::unique_ptr<nimrtc::plugins::I<Family>>
    create(const nimrtc::plugins::<Family>Config& cfg) const override {
        return std::make_unique<MyBackend>(cfg);
    }

private:
    std::string_view id_;
    std::string_view display_name_;
};
```

Most plugin families already have a CRTP helper
`SimpleXFactory<Impl>` that fills in the boilerplate
(`SimpleAudio3AFactory<T>`, `SimpleCodecFactory<T>`, …). Prefer it
unless you need to mutate `cfg` or hold per-process state in the
factory.

### Step 3 — Register

```cpp
// my_plugin_init.cpp
#include <nimrtc/core/registry.hpp>

static const MyBackendFactory g_factory{"myid", "My Backend"};
NIMRTC_REGISTER_<FAMILY>(myid, &g_factory);   // macro expands to
                                              // core::PluginRegistry::
                                              // register_<family>(...)
```

The registration is **static-init**. The macro consumes an identifier
that becomes the `id` string and a pointer to the factory. As long as
`my_plugin_init.cpp` is linked into the final binary, the registry
will see your factory on process start.

There is also `core::register_all_default_plugins()` that registers
the built-in plugins (WolfSSL DTLS, WebRTC APM audio3a, Opus codec,
libjuice ICE, …). Call it once at startup **before** the first
`NimRTCEngine::open()`.

### Step 4 — Select via `EngineConfig`

```cpp
nimrtc::engine::EngineConfig cfg;
cfg.<family>_name = "myid";   // <-- matches what you registered under
```

If the id is not registered by the time the engine resolves it,
`open()` returns a non-zero error and `on_error()` fires with the
resolution failure. There is **no silent fallback** — typos in
factory ids surface immediately, which is the entire point of the
seam.

---

## 4. Interface catalogue

| Family       | Interface header                                 | Engine config knob          | Factory macro                  | Notes |
|--------------|--------------------------------------------------|-----------------------------|--------------------------------|-------|
| ICE transport | `plugins/ice_transport.hpp`                      | `transport_name`            | `NIMRTC_REGISTER_ICE_TRANSPORT` | Default = `"ice"` (libjuice). |
| RTP          | `plugins/rtp.hpp`                                | `rtp_name`                  | `NIMRTC_REGISTER_RTP`          | — |
| SDP          | `plugins/sdp.hpp`                                | `sdp_name`                  | `NIMRTC_REGISTER_SDP`          | — |
| Jitter buffer | `plugins/jb.hpp`                                 | `jb_name`                   | `NIMRTC_REGISTER_JB`           | `adaptive` is the v0.11.0 default. |
| Audio 3A     | `plugins/audio3a.hpp`                            | `audio3a_name`              | `NIMRTC_REGISTER_AUDIO3A`      | `webrtc_apm`, plus `webrtc` (Null stub). See §6.2. |
| Codec (audio)| `plugins/codec.hpp`                              | `codec_name`                | `NIMRTC_REGISTER_CODEC`        | `opus`, `pcmu` (fallback). |
| Video codec  | `plugins/video_codec.hpp`                        | `video_codec_name`          | `NIMRTC_REGISTER_VIDEO_CODEC`  | `h264` (P1 reference). |
| Video source | `plugins/video_source.hpp`                       | `video_source_name`         | `NIMRTC_REGISTER_VIDEO_SOURCE` | `memory` default. HW guide: §6.3. |
| Video sink   | `plugins/video_sink.hpp`                         | `video_sink_name`           | `NIMRTC_REGISTER_VIDEO_SINK`   | `headless`, `pinned`. |
| Video rx     | `plugins/video_pipeline.hpp` (`IVideoReceiver`)  | `video_receiver_name`       | `NIMRTC_REGISTER_VIDEO_RX`     | — |
| Video tx     | `plugins/video_pipeline.hpp` (`IVideoSender`)    | `video_sender_name`         | `NIMRTC_REGISTER_VIDEO_TX`     | — |
| DTLS         | `dtls/dtls_session_iface.hpp`                    | `dtls_name`                 | `register_dtls_session` (PAL Slice 4) | `wolfssl`, `gmssl`. |
| SCTP socket  | `sctp/sctp_socket_iface.hpp`                     | `sctp_socket_name` (via factory lookup) | `register_sctp_socket` | `stub`, `usrsctp`. |
| Raw UDP      | `raw_udp/raw_udp_datagram.hpp`                   | `raw_udp_name`              | `register_raw_udp_datagram`    | `arq` (Loopback Case D). |
| Transport stack | `transport/transport_stack.hpp`                | `transport_stack_name`      | `register_transport_stack`     | `webrtc-classic`. |
| BWE          | `plugins/bwe.hpp`                                | `bwe_name`                  | `NIMRTC_REGISTER_BWE`          | `aimd`. |
| Scheduler    | `plugins/scheduler.hpp`                          | `scheduler_name`            | `NIMRTC_REGISTER_SCHEDULER`    | `strict_priority`. |
| DataChannel  | `plugins/datachannel.hpp`                        | `datachannel_name`          | `NIMRTC_REGISTER_DATACHANNEL`  | `sctp`. |

---

## 5. The lifecycle contract

Almost every plugin interface shares the same four-phase lifecycle:

```
construct → open() → tick() / process_*() → close() → destroy
```

Specifics per phase:

- **`construct`** — your factory does the allocation. Keep it cheap
  (the engine calls factory.create() once per `NimRTCEngine`, but
  tests can build many engines; a heavy constructor slows startup).
- **`open()`** — acquire hardware / network resources / load crypto
  material. Return `kOk` on success, `kErrResourceExhausted` on
  failure (matching the `plugins::Status` namespace).
- **`tick()`** — called by the engine at ~50 ms cadence while a
  session is alive (drives DTLS retransmits, BWE feedback, scheduler
  drains, etc.). Implementations MUST be re-entrant on the same
  thread and MUST NOT block (no synchronous `connect()`, no disk I/O).
- **`close()`** — release resources. Idempotent. Called by
  `NimRTCEngine::close()` and from the destructor.

Engine threads: the engine holds every plugin on a single thread
(the thread that called `open()`). Plugins do not need to be
thread-safe unless the engine documentation for that family says so
(notably `IBwe::report_feedback()` may be called from the RTP recv
path; check the per-family docs).

---

## 6. Worked examples per family

### 6.1 DTLS backend (the most impactful seam)

A new DTLS backend implements `dtls::IDtlsSession` and provides an
`IDtlsSessionFactory`. The interface lives at:

```
src/dtls/include/nimrtc/dtls/dtls_session_iface.hpp
src/dtls/include/nimrtc/dtls/dtls_session_factory.hpp
```

A working reference is `DtlsSessionGmSSL` in `src/dtls/src/`.

```cpp
// file: my_dtls_session.hpp
#pragma once
#include <nimrtc/dtls/dtls_session_iface.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>

class MyBackendSession final : public nimrtc::dtls::IDtlsSession {
public:
    // IDtlsSession — implement every method.
    void set_role(nimrtc::dtls::Role r) noexcept override;
    void set_peer_fingerprint(std::span<const std::uint8_t> raw_sha256) noexcept override;
    void start() noexcept override;
    void pump() noexcept override;
    void on_handshake_complete(OnCompleteCb cb) noexcept override;
    nimrtc::plugins::Status export_srtp_key_material(
        std::span<std::uint8_t, 60> out) noexcept override;
    nimrtc::core::Result<void> open() noexcept override;
    void set_role(nimrtc::dtls::DtlsRole r) noexcept override;
    void set_peer_fingerprint(std::string algo,
                              std::vector<std::uint8_t> value) noexcept override;
    std::size_t feed_inbound(std::span<const std::uint8_t> bytes,
                             const nimrtc::dtls::DtlsAddr& from) noexcept override;
    std::vector<nimrtc::dtls::DtlsRecord> take_outbound() noexcept override;
    void tick() noexcept override;
    nimrtc::dtls::DtlsState state() const noexcept override;
    bool is_connected() const noexcept override;
    const nimrtc::dtls::Fingerprint& local_fingerprint() const noexcept override;
    std::optional<nimrtc::dtls::SrtpKeyingMaterial>
    srtp_keying_material() const noexcept override;

private:
    // ... your backend state ...
};

// file: my_dtls_factory.hpp
#pragma once
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include "my_dtls_session.hpp"

class MyBackendFactory final : public nimrtc::dtls::IDtlsSessionFactory {
public:
    std::string_view id() const noexcept override { return "mybackend"; }
    std::string_view display_name() const noexcept override {
        return "My DTLS Backend";
    }
    std::unique_ptr<nimrtc::dtls::IDtlsSession>
    create(const nimrtc::dtls::DtlsConfig& cfg) const override {
        return std::make_unique<MyBackendSession>(cfg);
    }
};

// file: my_dtls_init.cpp
#include <nimrtc/core/registry.hpp>
#include "my_dtls_factory.hpp"

static const MyBackendFactory g_factory{};
NIMRTC_REGISTER_DTLS_SESSION(mybackend, &g_factory);
```

Then in user code:

```cpp
#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>

int main() {
    nimrtc::core::register_all_default_plugins();   // wolfSSL, opus, libjuice, ...
    // The mybackend factory is statically registered when my_dtls_init.cpp
    // is linked into the binary. No additional call needed.

    nimrtc::engine::EngineConfig cfg;
    cfg.dtls_name = "mybackend";
    nimrtc::engine::NimRTCEngine engine(cfg);
    return engine.open() == 0 ? 0 : 1;
}
```

**Pitfalls — read before publishing:**

- The default cipher suite is `ECDHE-ECDSA-AES128-GCM-SHA256` /
  `AES256-GCM-SHA384`. If your backend supports only a subset, the
  loopback Case D interop will fail. Configure your backend to match
  before testing.
- `set_peer_fingerprint` accepts the **raw 32-byte SHA-256** digest
  from the SDP `a=fingerprint` attribute (the WebRTC interop
  convention; not the SPKI hash from RFC 7250).
- DTLS-SRTP key export uses `SSL_export_keying_material` (or your
  backend's equivalent). For OpenSSL you need the upstream
  `openssl-srtp` patch. Without it, the export silently returns 0
  bytes and SRTP will fail to install — symptom is "DTLS Connected
  but no inbound audio".
- `tick()` is called by the engine at ~50 ms cadence from
  `NimRTCEngine::tick()`. Without it, retransmits hang and so does
  the handshake.

Reference: `docs/guides/switching-dtls-backend.md` and ADR-013.

### 6.2 Audio 3A backend

```cpp
#include <nimrtc/plugins/audio3a.hpp>

class NullA3A final : public nimrtc::plugins::IAudio3A { /* … */ };

class NullA3AFactory final : public nimrtc::plugins::IAudio3AFactory {
public:
    std::string_view id() const noexcept override { return "null"; }
    std::string_view display_name() const noexcept override { return "Null"; }
    std::unique_ptr<nimrtc::plugins::IAudio3A> create() const override {
        return std::make_unique<NullA3A>();
    }
};

// Register:
NIMRTC_REGISTER_AUDIO3A(null, &g_null_factory);
```

HW DSP integration (turnkey AEC + NS on a host MCU): inherit from
`IAudio3A` and treat the PCM in/out as `float*` buffers of length
`m.num_samples` at `m.sample_rate_hz`. The PCM tap surface (RFC 001)
is reachable via `set_pre_process_tap` and `set_post_process_tap`
(found on every IAudio3A impl); tap callbacks receive metadata
including `timestamp_us` and `num_channels` so the Agent pipeline
can ingest the stream verbatim.

See `examples/demo-agent-gateway/` for a working example that taps
into WebRTC APM and feeds the post-3A PCM into a mock ASR loop.

### 6.3 Video pipeline backends

See `docs/hw_plugin_seam.md` for the full guide. Quick notes:

- `IVideoSource` / `IVideoSink` / `IVideoReceiver` / `IVideoSender`
  all expose two extra virtual methods (`is_hardware_accelerated()`
  + `hardware_backend()`); override them even on software
  implementations (return `false` / `"software"`).
- HW video backends should additionally inherit from
  `IHwVideoEncoder` / `IHwVideoDecoder` / `IHwVideoCapture` to
  expose surface handles.
- The `video_*_name` config knobs are looked up at engine `open()`
  time. Unknown ids cause `engine.open()` to return
  `kEngineVideoSourceNotFound` (and `on_error()` fires).

### 6.4 BWE and Scheduler

These are the smallest interfaces (a handful of methods each) and
are entirely in-tree. Useful for experimenting with non-AIMD BWE or
priority schedulers without forking the engine.

```cpp
#include <nimrtc/plugins/bwe.hpp>

class TokenBucketBwe final : public nimrtc::plugins::IBwe { /* … */ };
class TokenBucketBweFactory final : public nimrtc::plugins::IBweFactory { /* … */ };

NIMRTC_REGISTER_BWE(token_bucket, &g_tb_factory);
```

`cfg.bwe_config` (an instance of `plugins::BweConfig`) carries
init-time parameters (initial bitrate, min/max caps, gain values).
Read it in your factory's `create()` and forward to the instance.

---

## 7. Testing your plugin

NimRTC ships gtest tests organised by module family. For a plugin of
your own, the conventional tests are:

- **Unit** — instantiate your plugin, drive each interface method
  individually, check status / invariants. Examples:
  `tests/test_dtpf_gmssl_tlcp.cpp`, `tests/test_hw_backends.cpp`,
  `tests/test_audio3a_plugins.cpp`.
- **Integration** — wire your plugin through `EngineConfig` and run
  the loopback-p2p smoke test
  (`examples/loopback-p2p/loopback-p2p.cpp`). Success criterion is
  two engines reach ICE Connected + DTLS Connected + SRTP key
  installation within ~5 seconds.
- **E2E** — bring up a Chrome browser against one NimRTC engine
  with your plugin. Run via `tools/run_e2e_acceptance.py`.

For per-module test patterns, see `tests/README.md` (to be written).

---

## 8. Versioning and ABI

- **Plugin id string** — once published, stable for the lifetime of
  the engine. Adding a new id is non-breaking; renaming requires a
  deprecation cycle (run both ids under a `register_<family>` shim
  for at least one minor version).
- **Interface methods** — pure-virtual methods are not ABI-stable;
  adding a method is a breaking change for any plugin author. We
  bump the v0.x version that introduces the change and update the
  interface header's `@note P1 — interface added in v0.x.y` tag.
- **Config struct fields** — additive only. New fields are typed
  with defaults; existing plugins that ignored them keep working.
  Removing a field is a breaking change and goes through a
  deprecation cycle (mark `[[deprecated]]`, keep the field).

For pre-1.0, we do **not** promise ABI stability. The plugin author
must support against a pinned engine version (see
`docs/zh/architecture.md` §12 "before 1.0 everything is
experimental").

---

## 9. Common pitfalls

- **Forgetting `register_all_default_plugins()`** — engine silently
  fails to find your plugin because the WolfSSL/Opus/libjuice
  registrations haven't been merged with yours. Always call
  `core::register_all_default_plugins()` once at startup.
- **Wrong id type** — every registration is type-typed
  (`register_dtls_session` vs `register_audio3a`); cross-typed
  registration is a compile error, not a runtime warning.
- **Non-`const` factory methods** — the registry holds
  `const I*Factory*`; factory methods must be `const`. The CRTP
  helpers (`Simple*Factory<T>`) get this right; if you hand-write,
  double-check.
- **Heavy constructors** — factory.create() is called once per
  engine; heavy work belongs in `open()`, not the constructor.
- **Non-idempotent `open()`** — the engine may probe `open()` twice
  during configuration changes. Return `kOk` on the second call
  without re-doing work.
- **Silent fallback** — the registry does NOT silently fall back
  to "wolfssl" / "opus" if your id is unknown. Use `on_error()` to
  surface failures to the user.

---

## 10. Where to look next

- `src/plugins/include/nimrtc/plugins/` — every interface header.
- `src/core/include/nimrtc/core/registry.hpp` — registry hooks.
- `src/dtls/include/nimrtc/dtls/` — DTLS seam specifically.
- `docs/hw_plugin_seam.md` — HW video backend guide.
- `docs/zh/architecture.md` §2.3, §6 — design rationale.
- `docs/adr/ADR-001-plugin-system.md` —
  string-id seam decision.
- `docs/adr/ADR-009-pal-slice-1.md` — typed registry hooks decision.
- `tests/test_*` — gtest patterns to copy.
- `examples/` — end-to-end wiring examples.

> Plugin author contract is stable as of v0.12.0. New plugin
> families added during 0.x are documented in their respective
> header comments. The registry itself is API-frozen at
> Slice 8 / Slice 8.5 (v0.10.2 / v0.10.3).
