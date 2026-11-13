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

**The id is a bare token, not a quoted literal.** Every
`NIMRTC_REGISTER_*` macro takes the id as a bare identifier token and
stringifies it for you, so `NIMRTC_REGISTER_<FAMILY>(myid, &f)`
registers under `"myid"`. Writing `NIMRTC_REGISTER_<FAMILY>("myid", &f)`
registers under an id that *includes the quote characters* — the
registration succeeds and the lookup never matches. A quoted literal
is rejected at compile time; so is a token that violates the id
grammar. See §8.1 for the full naming contract, including the
`nimrtc_` prefix rule and the namespacing requirement
for third-party plugins.

There is also `core::register_all_default_plugins()` that registers
the built-in plugins (WolfSSL DTLS, WebRTC APM audio3a, Opus codec,
libjuice ICE, …). Call it once at startup **before** the first
`NimRTCEngine::open()`.

### Step 4 — Select via `EngineConfig`

```cpp
nimrtc::engine::EngineConfig cfg;
cfg.<family>_name = "myid";   // <-- matches what you registered under
```

#### 4.1 What "no silent fallback" actually means

Not every family fails the same way when the id is unknown. The
table below is the contract; the §4 catalogue lists which family is
in which row.

| Behaviour | Families | What the host sees |
|---|---|---|
| **Hard failure** — `open()` returns non-zero, `on_error()` fires with the resolution failure | ICE transport, DTLS, SCTP socket, transport stack, SRTP backend, DataChannel backend, transport strategy | `kEngineInvalidParam` (or `kEngineInternal` for a factory-side fault). The engine does **not** substitute a built-in implementation. |
| **Reported, non-fatal** — the session opens without the component, but `on_error()` fires with the family's error code | audio / video codecs, video source / sink / receiver / sender, video capture, audio decoder, audio receiver, jitter buffer | The affected feature is inert and its accessor returns `nullptr`, but the host is told. A typo does not turn into a feature that silently never works. |
| **Logged only** — the engine logs a warning and continues without that component | BWE, scheduler, network-feedback collector, XR report consumer, WebTransport session factory, session model, DTMF codec | The session still opens; the affected feature is inert. `on_error()` is **not** fired (the engine uses the `Warn` failure mode for these). |
| **Disabled by configuration** — the knob is empty, so the family is never resolved | DataChannel (`datachannel_backend == ""`), raw-UDP bypass (`raw_udp_name` empty), capture (`video_capture_name` empty) | The corresponding SDP section is omitted, or the accessor returns `nullptr`. |

Four consequences worth internalising:

- A typo in a **hard-failure** family's id is loud, and the session
  does not open at all. That is the point of the seam.
- A typo in a **reported, non-fatal** family's id is also loud — the
  session opens, but `on_error()` tells you which piece is missing.
- A typo in a **logged-only** family's id is quiet (a log line
  only). If you are debugging a missing feature, read the engine's
  warning log before suspecting your plugin.

#### 4.2 The families that are *not* replaceable

The RTP and SDP layers are **not** pluggable. The interfaces
(`plugins/rtp.hpp`, `plugins/sdp.hpp`) exist and are registered, but
the engine uses the concrete `rtp::` / `sdp::` implementations
directly — there is no `EngineConfig` knob for them, and adding one
would be misleading. See `docs/plan/architecture-remediation-plan.md`
§2.1 (decision: keep the concrete implementations, mark the
interfaces deprecated).

If you need to change RTP packetisation or SDP generation today, you
must fork the module; the plugin system does not cover it.

---

## 4. Interface catalogue

The **Failure** column uses the §4.1 vocabulary: `hard` = `open()`
returns non-zero, `reported` = the session opens but `on_error()` fires,
`logged` = the engine degrades with a log line only, `off` = disabled
by an empty config knob.

| Family       | Interface header                                 | Engine config knob          | Factory macro                  | Failure | Notes |
|--------------|--------------------------------------------------|-----------------------------|--------------------------------|---------|-------|
| ICE transport | `plugins/ice_transport.hpp`                      | `transport_name`            | `NIMRTC_REGISTER_ICE_TRANSPORT` | hard | Default = `"ice"` (libjuice). Also selects the ICE backend inside the transport stack. |
| Transport stack | `transport/transport_stack.hpp`               | `transport_stack_name`      | `NIMRTC_REGISTER_TRANSPORT_STACK` | hard | Empty = the `CapabilitySelector` decides (`webrtc-classic` / `raw-udp-arq`). Non-empty = explicit override of the media stack. |
| DTLS         | `dtls/dtls_session_iface.hpp`                    | `dtls_name`                 | `NIMRTC_REGISTER_DTLS_SESSION` | hard | `wolfssl`, `gmssl`. Empty = `"wolfssl"`. |
| SCTP socket  | `sctp/sctp_socket_iface.hpp`                     | `sctp_socket_name`          | `NIMRTC_REGISTER_SCTP_SOCKET`  | hard | Default = `"stub"` (in-tree loopback reference backend). |
| Raw UDP      | `raw_udp/raw_udp_datagram.hpp`                   | `raw_udp_name`              | `NIMRTC_REGISTER_RAW_UDP_DATAGRAM` | off | Empty = the raw-UDP bypass is disabled. The ARQ control plane falls back to the in-tree `"arq"` id. |
| SRTP backend | `plugins/srtp_backend.hpp`                       | `srtp_backend_name`         | `NIMRTC_REGISTER_SRTP_BACKEND` | hard | Default = `"libsrtp"`. A third-party crypto backend registers under its own id. |
| DataChannel  | `plugins/datachannel.hpp`                        | `datachannel_backend`       | `NIMRTC_REGISTER_DATACHANNEL`  | hard | `"web_transport"` (default) or `""` (disabled). The `"sctp"` family is retired — it was a test-only in-process socket. |
| WebTransport | `web_transport/web_transport_session_factory.hpp`| `web_transport_name`        | `NIMRTC_REGISTER_WEB_TRANSPORT` | logged | `"msquic"`. The QUIC session factory behind the `web_transport` DataChannel backend. |
| Audio decoder| `plugins/audio_decoder.hpp`                      | `audio_decoder_name`        | `NIMRTC_REGISTER_AUDIO_DECODER` | reported | `opus`. The codec-side seam of the audio receive pipeline. |
| Audio receiver| `plugins/audio_receiver.hpp`                    | `audio_receiver_name`       | `NIMRTC_REGISTER_AUDIO_RECEIVER` | reported | `nimrtc_audio_rx`. The pipeline-side seam of the audio receive path. |
| Session model| `plugins/session.hpp`                            | `session_name`              | `NIMRTC_REGISTER_SESSION`     | logged | `session`. The renegotiation model (transceivers / tracks). |
| DTMF codec   | `plugins/dtmf.hpp`                               | `dtmf_codec_name`           | `NIMRTC_REGISTER_DTMF`        | logged | `dtmf`. The RFC 4733 telephone-event codec. |
| Jitter buffer | `plugins/jb.hpp`                                 | `jb_name`                   | `NIMRTC_REGISTER_JB`           | reported | `adaptive` is the v0.11.0 default. Resolved inside the audio-receiver module; a missing plugin makes the receiver report `kAudioReceiverMissing`. |
| Audio 3A     | `plugins/audio3a.hpp`                            | `audio3a_name`              | `NIMRTC_REGISTER_AUDIO3A`      | logged | `webrtc_apm`, plus `webrtc` (Null stub). See §6.2. A missing plugin falls back to the concrete `NullAudio3A`; an *open* failure fires `on_error`. |
| Codec (audio)| `plugins/codec.hpp`                              | `codec_name`                | `NIMRTC_REGISTER_CODEC`        | logged | `opus`, `pcmu` (fallback). A missing plugin makes `send_audio()` report the frame as lost (throttled) rather than substituting a codec. |
| Video codec  | `plugins/video_codec.hpp`                        | `video_codec_name`          | `NIMRTC_REGISTER_VIDEO_CODEC`  | reported | `h264` (P1 reference). |
| Video source | `plugins/video_source.hpp`                       | `video_source_name`         | `NIMRTC_REGISTER_VIDEO_SOURCE` | reported | `memory` default. HW guide: §6.3. |
| Video capture| `plugins/video_capture.hpp`                      | `video_capture_name`        | `NIMRTC_REGISTER_VIDEO_CAPTURE`| reported | Empty = disabled. The capture *device* seam; a source is derived from it by an adapter. |
| Video sink   | `plugins/video_sink.hpp`                         | `video_sink_name`           | `NIMRTC_REGISTER_VIDEO_SINK`   | reported | `headless`, `pinned`. |
| Video rx     | `plugins/video_pipeline.hpp` (`IVideoReceiver`)  | `video_receiver_name`       | `NIMRTC_REGISTER_VIDEO_RECEIVER` | reported | — |
| Video tx     | `plugins/video_pipeline.hpp` (`IVideoSender`)    | `video_sender_name`         | `NIMRTC_REGISTER_VIDEO_SENDER` | reported | — |
| BWE          | `plugins/bwe.hpp`                                | `bwe_name`                  | `NIMRTC_REGISTER_BWE`          | logged | `aimd`. |
| Scheduler    | `plugins/scheduler.hpp`                          | `scheduler_name`            | `NIMRTC_REGISTER_SCHEDULER`    | logged | `strict_priority`. |
| Net feedback | `plugins/net_feedback.hpp`                       | `net_feedback_name`         | `NIMRTC_REGISTER_NET_FEEDBACK` | logged | The receiver-side collector that feeds the BWE estimator. |
| XR report    | `plugins/xr_report.hpp`                          | `xr_report_name`            | `NIMRTC_REGISTER_XR_REPORT`    | logged | The RFC 3611 extended-report consumer. |

**Not pluggable today** (see §4.2): the RTP layer
(`plugins/rtp.hpp`) and the SDP layer (`plugins/sdp.hpp`). The
interfaces exist and are registered, but the engine uses the concrete
`rtp::` / `sdp::` implementations — there is deliberately no
`EngineConfig` knob for them.

### 4.3 Registered, but the engine does not resolve them

These families have an interface, a registry slot, and a registration
macro, but **no `EngineConfig` knob and no engine-side lookup**. They
are listed here so the catalogue is complete — a plugin author who
finds `NIMRTC_REGISTER_RTP` in the registry header deserves to learn
that registering it changes nothing.

| Family | Interface header | Engine config knob | Factory macro | Failure | Notes |
|---|---|---|---|---|---|
| Generic transport | `plugins/transport.hpp` | — (none) | `NIMRTC_REGISTER_TRANSPORT` | n/a | The engine resolves the **ICE-typed** slot (`get_ice_transport`) through the transport stack, not the generic `ITransport` slot. The generic slot exists so a consumer can pick the weakest typed view. |
| RTP | `plugins/rtp.hpp` | — (none) | `NIMRTC_REGISTER_RTP` | n/a | **Deprecated.** The engine uses the concrete `rtp::` implementation. See §4.2. |
| SDP | `plugins/sdp.hpp` | — (none) | `NIMRTC_REGISTER_SDP` | n/a | **Deprecated.** The engine uses the concrete `sdp::` implementation. See §4.2. |

### 4.4 The replaceability matrix — what can you actually swap?

The catalogue above lists *every* family that has a seam. This table
answers the narrower question a plugin author actually has: **if I
implement this interface, does the engine use my implementation, or
does it keep using the in-tree one?**

There are exactly three answers, and they are worth distinguishing
before you invest the work.

| Answer | What it means | Families | What you must do |
|---|---|---|---|
| **Fully replaceable** | The engine resolves your plugin and calls *only* your implementation. Your plugin is the whole feature. | ICE transport, transport stack, DTLS, SCTP socket, SRTP backend, DataChannel backend, WebTransport session factory | Implement the interface, register, set the config field. Nothing else. |
| **Replaceable, but with an in-tree default fallback** | Your plugin is used when it resolves; when it does not, the engine substitutes the in-tree implementation. | Audio 3A (falls back to `NullAudio3A`) | Implement the interface, register, set the config field — and be aware that the engine will *not* tell you loudly when your plugin was skipped. |
| **Not replaceable** | The interface exists, but the engine uses the concrete in-tree implementation directly. Registering a plugin changes nothing. | **RTP**, **SDP** | Fork the module. There is no config knob, by decision (ADR-025): the RTP builder is on the zero-allocation hot path and the SDP layer is the contract every other component is written against. |

Two families sit between the first two columns and are worth calling
out, because they are the ones most often misread:

- **The "reported, non-fatal" families** (audio/video codecs, video
  source/sink/receiver/sender, video capture, audio decoder, audio
  receiver, jitter buffer) *do* use your implementation when it
  resolves — but a failure to resolve is **not** fatal, and the
  feature is left inert. "The engine ignored my plugin" and "the
  engine used my plugin and it failed" have the *same* symptom from
  the outside, so check `on_error()` before assuming your plugin was
  skipped.
- **The "logged only" families** (BWE, scheduler, net-feedback, XR
  report, session model, DTMF, audio codec, WebTransport session
  factory) also use your implementation when it resolves. The
  difference is that a failure to resolve is *logged and nothing
  else* — no `on_error()`, no return code. If your plugin appears to
  be ignored entirely, **read the engine's warning log**: that log
  line is the only signal you will get.

The practical test for any family: **read the `Failure` column of
the catalogue in §4.** `hard` means you cannot miss it. `reported`
means you will be told once. `logged` means you have to go looking.

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

### 7.1 How to report your plugin ID

Your plugin's id is a **public string** — it appears in host config
files, in log lines, and in Profile JSON files (ADR-010). Once
published it is part of the engine's public surface, so the way you
report it matters to the people who will configure it.

**In your README**, state the id in a way that can be copied
verbatim, together with the family it belongs to and the config field
that selects it:

```markdown
## `acme_h264` — AV1-capable H.264 encoder

| | |
|---|---|
| **Plugin family** | Video codec (`plugins/video_codec.hpp`) |
| **Config field** | `EngineConfig::video_codec_name` |
| **Registered id** | `acme_h264` |
| **Interface** | `nimrtc::plugins::IVideoCodec` |
| **Registration** | `NIMRTC_REGISTER_VIDEO_CODEC(acme_h264, &g_factory)` |

Select it with:

```cpp
cfg.video_codec_name = "acme_h264";
```
```

**The four things a reader needs**, and without which the id cannot
be used:

1. **The id string itself**, exactly as registered — including the
   namespace prefix. `acme_h264`, not `AcmeH264` and not `h264`.
2. **The family**, so the reader knows which registry slot it is in
   and which interface to implement. Two families can legitimately
   share an id string; the family disambiguates.
3. **The config field** that selects it. The id alone is not enough —
   the reader has to know where to put it.
4. **The registration line**, so the reader can see that the id is
   tokenised (not quoted) and can reproduce the call.

**Also report the id's spelling rules**, if your plugin is one of a
set that users are expected to choose between. A host that ships
several plugins of the same family needs to know the ordering and
constraints — for example, that exactly one plugin may claim a given
role, and what happens when two are configured.

**Do not** describe the id with a placeholder that is not the real
string. `cfg.video_codec_name = "<your-id>"` teaches the reader
nothing they can use, and `<your-id>` is not a legal id under the
grammar in §8.1 — so a reader who copies it gets a registration
failure rather than a working config.

**Do not** document the id in only one place. If the plugin has a
README, a doc page under `docs/`, and an example, all three must
carry the same string. The id is a config key; a doc that disagrees
with the code is a bug that only shows up on someone else's machine.

**When you rename**, the rename is a breaking change to the public
config surface. Register both the old and the new id for at least one
minor version, log the old id at `Warn` when it is selected, and say
so in the README — including the version in which the old id will be
removed. See §8.1.

---

## 8. Versioning and ABI

- **Plugin id string** — once published, stable for the lifetime of
  the engine. Adding a new id is non-breaking; renaming requires a
  deprecation cycle (run both ids under a `register_<family>` shim
  for at least one minor version). The full naming contract — the
  legal grammar, the `nimrtc_` prefix rule, third-party namespacing,
  and the deprecation cycle — is
  [ADR-023](adr/ADR-023-plugin-id-naming-contract.md); the summary is
  in §8.1 below.
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

### 8.1 The plugin ID naming contract

**The grammar.** A plugin id must match:

```
^[a-z][a-z0-9_-]*$
```

Starts with a lowercase ASCII letter; continues with lowercase
letters, digits, underscores and hyphens; nothing else. (The hyphen
is permitted because the in-tree transport-stack ids —
`"webrtc-classic"`, `"raw-udp-arq"` — already use it, and those are
published config values.)

Because the grammar accepts exactly the set of valid C++ identifiers
**plus the hyphen**, the id you pass to a registration macro is
usually a **bare token** and the macro stringifies it:

```cpp
NIMRTC_REGISTER_DTLS_SESSION(mybackend, &g_factory);         // -> "mybackend"
NIMRTC_REGISTER_AUDIO_DECODER(opus, &g_opus_decoder_factory);  // -> "opus"
```

Every `NIMRTC_REGISTER_*` macro takes the token and stringifies it —
there is one spelling, not two. (Before v1.2.0 the audio-seam macros
took a string literal while the rest took a token; the inconsistency
was closed by ADR-023 Decision 5.)

Passing a **quoted literal** to a registration macro is rejected at
compile time. This is not pedantry: a quoted literal passed to a
stringifying macro registers under an id that *includes the quote
characters* — the registration succeeds, and the lookup never
matches. That failure is silent, and it is the single most common
way a plugin author ends up debugging "my plugin registers but the
engine never picks it up".

A **malformed bare token** is rejected at compile time too: the macro
checks the token against the grammar above, so `NIMRTC_REGISTER_<FAMILY>(Opus, &f)`
does not compile. (The grammar permits the hyphen, but a hyphenated
id cannot be spelled as a bare token — see below.)

**Hyphenated ids** are the one case the tokenising form cannot
express, because `#my-id` does not lex as a single token. Those go
through the registry directly:

```cpp
core::PluginRegistry::instance().register_transport_stack("webrtc-classic", &g_factory);
```

That is exactly what the in-tree transport module does — its
registrars call `register_transport_stack(f->id(), &f)` with the
hyphenated ids `"webrtc-classic"` and `"raw-udp-arq"`.

The full naming contract — the legal grammar, the `nimrtc_` prefix
rule, third-party namespacing, and the deprecation cycle — is
[ADR-023](adr/ADR-023-plugin-id-naming-contract.md); the summary is
in §8.1 below.

**The prefix rule.** The prefix is a claim about *ownership*, and it
tells a reader of a config file who provided the component:

| Id looks like | Means | Example |
|---|---|---|
| `nimrtc_…` | An **in-tree** implementation | `nimrtc_audio_rx`, `nimrtc_net_feedback`, `nimrtc_xr` |
| A **vendor / library name** | A third-party or third-party-shaped implementation | `wolfssl`, `gmssl`, `libsrtp`, `msquic` |
| A **conceptual role** | An algorithm or a role, not an implementation | `opus`, `aimd`, `adaptive`, `strict_priority`, `memory`, `headless`, `web_transport`, `ice` |

**A third-party plugin must not use the `nimrtc_` prefix** — that name
belongs to the project, and claiming it is a false statement about
who maintains the component.

**Third-party namespacing.** A third-party id must be qualified by
your own namespace, so two vendors shipping an "opus" codec cannot
collide in the same registry slot:

```
<your_namespace>_<implementation>
```

for example `acme_h264`, `contoso_3a`, `vendorx_capture`.

**Validation.** Ids are validated against the grammar at
**registration** time, so a malformed id is refused where the mistake
was made — not later at `open()`, where the host is being told
something else went wrong. A rejected id does not disturb the other
entries in the same slot.

The **recommended** way to get a *compile-time* error instead of a
runtime one is to wrap your factory's id in `NIMRTC_PLUGIN_ID`:

```cpp
#include <nimrtc/core/plugin_id.hpp>

std::string_view MyBackendFactory::id() const noexcept {
    return NIMRTC_PLUGIN_ID("acme_h264");   // checked at compile time
}
```

Note the **quotes**: `NIMRTC_PLUGIN_ID` wraps a *string literal*, not
a bare token — its argument is handed to a `const char*` parameter.
(The registration macros are the opposite: they take a bare token
and stringify it. See §8.1.)

`NIMRTC_PLUGIN_ID` also makes the id greppable — `git grep
'NIMRTC_PLUGIN_ID('` lists every id in the tree — and gives each
callsite a distinct type, so two plugins that accidentally pick the
same id are visible in review rather than silently overwriting each
other at runtime. The macro is in
`nimrtc/core/plugin_id.hpp`; the in-tree factories all use it.

**Changing an id.** Adding an id is additive. Renaming or removing
one is a breaking change to the public config surface and requires a
deprecation cycle: register **both** the old and the new id for at
least one minor version, and log the old id at `Warn` when it is
selected. A deprecation cycle is observable — "both ids resolve to
the same factory" is an assertion a test can make.

Full rationale and alternatives: [ADR-023](adr/ADR-023-plugin-id-naming-contract.md).

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
