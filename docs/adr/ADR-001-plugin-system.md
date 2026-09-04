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

| File | Defines |
|------|---------|
| `plugins/base.hpp` | `Status`, `BufferView`, `MediaSample`, `OutPacket`, `Addr`, `IPlugin` |
| `plugins/transport.hpp` | `ITransport`, `ITransportFactory`, `TransportConfig` |
| `plugins/rtp.hpp` | `IRTP`, `IRTPFactory`, `RtpHeader`, `RtpPacket`, `RtcpPacket` |
| `plugins/sdp.hpp` | `ISDP`, `ISDPFactory`, `SdpSession`, `SdpMedia`, `MungOptions` |
| `plugins/jb.hpp` | `IJB`, `IJBFactory`, `JBConfig` |
| `plugins/audio3a.hpp` | `IAudio3A`, `IAudio3AFactory`, `Audio3AConfig` |
| `plugins/registry.hpp` | `PluginRegistry`, registration macros |

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
    std::string_view transport_name = "webrtc";  // look up in registry
    std::string_view rtp_name       = "standard";
    std::string_view sdp_name       = "webrtc";
    std::string_view jb_name        = "adaptive";
    std::string_view audio3a_name   = "webrtc";

    TransportConfig transport_config;
    std::vector<JBConfig> jb_configs;
    Audio3AConfig audio3a_config;
};
```

At `open()` the engine looks up each factory in the registry and calls `create()` to instantiate the plugin.

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
