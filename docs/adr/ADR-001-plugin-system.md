# ADR-001: Pluggable Module Architecture

**Status:** Accepted
**Date:** 2026-09-03
**Phase:** P0 (scaffold)

---

## Context

NimRTC targets two distinct use cases:
1. **Standard WebRTC interoperability** — applications that need to communicate with browsers via the standard WebRTC protocol stack (ICE/DTLS-SRTP/JSEP).
2. **Proprietary/embedded scenarios** — AI agents, embedded devices, or private cloud deployments that can use a custom protocol and do not need browser compatibility.

腾讯 (TRTC) and 声网 (Agora) both use proprietary protocols with a cloud gateway that translates to WebRTC. NimRTC's architecture must support both paths without forcing a single design.

A monolithic WebRTC stack makes it impossible to swap the transport (e.g. replace ICE+DTLS-SRTP with a proprietary gateway protocol) without rewriting the entire engine.

---

## Decision

Adopt a **plugin interface architecture** where every major module (transport, RTP, SDP, jitter buffer, audio 3A) is an abstract C++ interface. Users can replace any layer by implementing the interface and registering a factory.

```
Application
    └─> NimRTCEngine        (composition layer)
            ├─> ITransport   ──> [WebRTC transport]  or  [Proprietary gateway]
            ├─> IRTP        ──> [Standard RTP]       or  [Custom framing / RDT]
            ├─> ISDP        ──> [WebRTC SDP]         or  [Proprietary signaling]
            ├─> IJB         ──> [Adaptive JB]        or  [Fixed-size JB]
            └─> IAudio3A    ──> [WebRTC APM]         or  [SpeexDSP / custom DSP]
```

### Interface files (header-only, `src/plugins/`)

The `nimrtc::plugins` interface layer covers both WebRTC and proprietary
use cases. Concrete implementations live in their respective modules
under `src/modules/`. The full surface as of the current phase:

| File | Defines |
|------|---------|
| `plugins/base.hpp` | `Status`, `BufferView`, `MediaSample`, `OutPacket`, `Addr`, `IPlugin`, `IPluginFactory` |
| `plugins/transport.hpp` | `ITransport`, `ITransportFactory`, `TransportConfig` |
| `plugins/ice_transport.hpp` | `IICETransport`, `IICETransportFactory` (ICE-aware surface; superset of ITransport) |
| `plugins/rtp.hpp` | `IRTP`, `IRTPFactory`, `RtpHeader`, `RtpPacket`, `RtcpPacket` |
| `plugins/sdp.hpp` | `ISDP`, `ISDPFactory`, `SdpSession`, `SdpMedia`, `MungOptions` |
| `plugins/jb.hpp` | `IJB`, `IJBFactory`, `JBConfig` |
| `plugins/audio3a.hpp` | `IAudio3A`, `IAudio3AFactory`, `Audio3AConfig` |
| `plugins/codec.hpp` | `ICodec`, `ICodecFactory` (audio codec plugin surface) |
| `plugins/video_codec.hpp` | `IVideoCodec`, `IVideoCodecFactory` |
| `plugins/video_source.hpp` | `IVideoSource`, `IVideoSourceFactory` |
| `plugins/video_sink.hpp` | `IVideoSink`, `IVideoSinkFactory` |
| `plugins/video_pipeline.hpp` | `IVideoReceiver`, `IVideoSender` + factories |
| `plugins/hw_seam.hpp` | Optional hardware-acceleration hooks used by the video pipeline; carries the hardware codec / decoder / encoder factory surface (Windows MF, VideoToolbox, VA-API, etc.) |
| `plugins/datachannel.hpp` | `IDataChannel`, `IDataChannelFactory` (P1: interface only) |

> **Note on `registry.hpp`**: ADR-001 originally listed `plugins/registry.hpp`
> as a separate header. That header was merged into
> [`src/core/include/nimrtc/core/registry.hpp`](../../src/core/include/nimrtc/core/registry.hpp)
> because `PluginRegistry` is a global singleton and therefore must live in
> `nimrtc::core::` (Layout Invariant 6). Consumers that still write
> `#include <nimrtc/plugins/registry.hpp>` continue to work via a thin
> back-compat shim that re-exports `core::PluginRegistry`.

### Include-order rule

Every plugin header includes `base.hpp` **before** its include guard:

```cpp
// plugins/transport.hpp
#include "nimrtc/plugins/base.hpp"  // ← before guard

#ifndef NIMRTC_PLUGINS_TRANSPORT_HPP
#define NIMRTC_PLUGINS_TRANSPORT_HPP
// ... ITransport, TransportConfig ...
#endif
```

This ensures the header works correctly both when included standalone and when included via another plugin header (where `base.hpp` would otherwise be skipped by its include guard).

### Factory and Registry

Each interface has a corresponding factory:

```cpp
class ITransportFactory {
public:
    virtual std::string_view id()          const noexcept = 0;
    virtual std::string_view display_name()const noexcept = 0;
    virtual ITransport* create()           const = 0;
};
```

`PluginRegistry::instance()` holds all registered factories by ID string. Plugins register themselves at static-init time using macros:

```cpp
// In the plugin .cpp file:
static const SimpleTransportFactory<MyTransport> factory{"my_gateway", "My gateway"};
NIMRTC_REGISTER_TRANSPORT(my_gateway, &factory);
```

### Engine wiring

`NimRTCEngine::Config` references plugins by string ID:

```cpp
struct EngineConfig {
    // Default transport_name = "ice": the default NimRTCEngine resolves
    // an IICETransportFactory by id and instantiates the ICE-aware
    // surface.  A user can register a custom ITransportFactory under
    // the same id (the engine falls back to a dynamic_cast safety net)
    // or supply a different name entirely.
    std::string_view transport_name = "ice";
    std::string_view rtp_name       = "standard";
    std::string_view sdp_name       = "webrtc";
    std::string_view jb_name        = "adaptive";
    std::string_view audio3a_name   = "webrtc";
    std::string_view codec_name     = "opus";

    TransportConfig transport_config;
    std::vector<JBConfig> jb_configs;
    Audio3AConfig audio3a_config;
};
```

At `open()` the engine looks up each factory in the registry and calls `create()` to instantiate the plugin.

---

## Supplements (added v1.2.0)

The sections above record the decision as it was taken in P0. Three
things have changed since, and each is recorded by its own ADR; this
section states what changed and points at the detail, so that ADR-001
read on its own does not mislead.

### Supplement A — the plugin ID naming contract

The original decision specified that plugins are "registered by ID
string" and that the engine "references plugins by string ID", but it
did not specify **how an id is spelled**, **who validates it**, or
**what the compatibility promise for an id is**.

That gap was closed by
[ADR-019](ADR-019-plugin-id-naming-contract.md), which fixes:

- **The grammar.** A plugin id matches `^[a-z][a-z0-9_-]*$` — a
  lowercase ASCII token, with underscores and hyphens allowed, that
  is also a valid shell / JSON key. The hyphen is permitted because
  the shipped transport-stack ids (`"webrtc-classic"`,
  `"raw-udp-arq"`) use it.
- **The prefix rule.** An id naming an in-tree implementation is
  prefixed `nimrtc_`; an id naming a third-party implementation
  carries the vendor's name and no `nimrtc_` prefix; an id naming a
  conceptual role is bare.
- **Third-party namespacing.** A third-party id is
  `<namespace>_<implementation>`.
- **Validation at registration.** A malformed id is refused where the
  mistake was made, not later at `open()`.
- **The deprecation cycle.** Renaming an id registers both the old
  and the new id for at least one minor version.

The registration macros also converge on **one spelling**: the id is a
bare token that the macro stringifies. Passing a quoted literal is a
compile error. (The two macro families previously disagreed, and the
quoted-literal-in-a-stringifying-macro form registered an id that
included the quote characters — a silent failure.)

### Supplement B — which families are actually replaceable

The original decision said "Users can replace any layer by
implementing the interface and registering a factory." That is true
of the *seam*, but two of the seams are **not** live, and the original
text did not say so.

- **RTP and SDP are not pluggable.** The interfaces
  (`plugins/rtp.hpp`, `plugins/sdp.hpp`) still exist and are still
  registered, but the engine uses the concrete `rtp::` / `sdp::`
  implementations directly, and there is deliberately **no
  `EngineConfig` knob** for them. Registering an RTP or SDP plugin
  changes nothing. The rationale — the RTP builder sits on the
  zero-allocation hot path, and the SDP layer is the contract every
  other component is written against — is in
  [ADR-021](ADR-021-sdp-rtp-not-pluggable.md).
- **The `EngineConfig::rtp_name` and `EngineConfig::sdp_name` fields
  described in the original snippet no longer exist.** They were
  removed as part of closing those two fake knobs.

The full per-family matrix — which families are fully replaceable,
which have an in-tree fallback, which degrade silently, and how
loudly each failure is reported — is in
[`docs/plugin_author_guide.md` §4.4](../plugin_author_guide.md).

### Supplement C — `EngineConfig` fields are `std::string`, not `std::string_view`

The snippet above shows `std::string_view` fields and a `Config`
type name. Both are historical:

- The type is `nimrtc::engine::EngineConfig`, declared in
  `src/engine/include/nimrtc/engine/engine.hpp`.
- Every string field is a **`std::string`**. The `std::string_view`
  shape was migrated away because it created a lifetime hazard at
  every assignment site; the engine takes the config **by value** and
  moves it, so it owns a copy of every string. This is an
  **ABI-breaking** change in v1.2.0 (host recompilation required) and
  is specified in
  [ADR-020](ADR-020-engineconfig-string-ownership.md).

---

## Alternatives Considered

### 1. Single monolithic transport

Use ICE+DTLS-SRTP directly and require all traffic to pass through it. Rejected: forces proprietary deployments to route through an unnecessary translation layer.

### 2. D-Bus / COM-style plugin system

Use `dlopen`/`LoadLibrary` for binary plugin loading. Rejected: adds deployment complexity (plugin paths, ABI versioning) and is not needed until P4.

### 3. Template-based policy injection (CRTP)

Use template parameters to inject policy classes. Rejected: requires recompilation for every plugin combination; does not support runtime plugin swapping.

### 4. Protocol-agnostic abstraction at a higher level

Abstract only at the "session" level (e.g. `ISession`). Rejected: does not allow swapping individual layers (e.g. keep WebRTC SDP but replace transport).

---

## Consequences

**Positive:**
- Any module can be replaced without touching the engine or other modules.
- Proprietary deployments can use custom transports, codecs, and signaling without forks.
- Standard deployments use the same engine with standard plugins.
- The interface contracts make testing straightforward (mock any plugin).

**Negative:**
- Every interface method call is virtual (indirect call overhead). For the media pipeline (called per-packet), this is negligible compared to network and codec overhead.
- Plugin authors must implement the full interface. A partial stub helper (`PluginStub<Interface>`) can be added in P2 to reduce boilerplate.

**Neutral:**
- Interface stability commitment: once an interface is marked stable, breaking changes require a major version bump.
- Binary-safe plugin loading (dlopen/LoadLibrary) is deferred to P4.

---

## Implementation notes

- All plugin interfaces live in `src/plugins/` (header-only INTERFACE library `nimrtc_plugins`).
- Concrete implementations live in their respective `src/modules/<name>/`.
- `src/engine/` contains `NimRTCEngine` (composition layer), linking all modules.
- `src/core/` provides only stable types; no module dependencies.

See `src/plugins/` and `src/engine/` for the actual code.
