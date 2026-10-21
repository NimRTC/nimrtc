/**
 * @file nimrtc/plugins/transport.hpp
 * @brief ITransport — pluggable transport layer interface.
 *
 * ITransport is the only point where NimRTC touches the network.
 * Replace it to use UDP directly, ICE+DTLS-SRTP, a custom RDT
 * protocol, or a proprietary gateway protocol.
 *
 * ## Swapping the transport
 *
 * 1. Implement `ITransport` and `ITransportFactory`.
 * 2. Register: `PluginRegistry::instance().register_transport("my", factory);`
 * 3. Pass name to `NimRTCEngine::Config::transport_name`.
 *
 * ## Thread safety
 *
 * Implementations must be thread-safe for `send()` and `recv()` called
 * concurrently from different threads. `open()` / `close()` are called
 * from a single thread (the engine thread).
 *
 * ## Per-symbol stability annotations (v0.14.x PER-SYMBOL-CLASSIFY)
 *
 * The v0.13.0 sweep marked this header at the file level. The v0.14.x
 * per-symbol follow-up annotates each `ITransport` member:
 *
 *   - `set_callbacks()` / `send()` / `recv()` / `local_addr()` /
 *     `remote_addr()` — STABLE. The five-member contract is the
 *     load-bearing network seam (ADR-001 §"Swapping the transport");
 *     every custom transport implementation (ICE / raw UDP / WebSocket
 *     signaling proxy / in-process pipe) implements exactly this set.
 *   - `set_on_consent_lost()` — STABLE. The RFC 7675 / RFC 8445 §10
 *     consent-freshness hook landed in v0.10.x and is consumed by
 *     `tests/test_consent_freshness.cpp`; keeping it STABLE prevents a
 *     v1.0.x patch from silently dropping the hook.
 *   - `ITransportFactory::id()` / `display_name()` / `create()` —
 *     STABLE. Registry lookups key off `id()`; `display_name()`
 *     surfaces in log lines and `core::PluginRegistry` listings.
 *   - `SimpleTransportFactory` — STABLE. The template helper is the
 *     most common in-tree factory implementation pattern.
 *
 * @note P0 scaffold — interface stable, binary layout TBD P1.
 */

// base.hpp must be included BEFORE the include guard.
// This ensures base types are always available regardless of include order.
#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_TRANSPORT_HPP
#define NIMRTC_PLUGINS_TRANSPORT_HPP

// Stability classification: NIMRTC_API_STABLE (file-level marker from the
// v0.13.0 sweep; see ADR-015 / docs/plan/v0.13-plan.md). The v0.14.x
// per-symbol follow-up annotates every `ITransport` member below; see the
// "Per-symbol stability annotations" note in the header comment above.
//
// `nimrtc_plugins` links `nimrtc::core` PUBLIC (see src/plugins/CMakeLists.txt),
// so <nimrtc/core/api_version.hpp> is reachable from this header.
#include <nimrtc/core/api_version.hpp>   // NIMRTC_API_STABLE

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

/** Opaque key-value config passed to the transport at open(). */
struct NIMRTC_API_STABLE TransportConfig {
    std::string_view name;   // plugin name, for logging
    std::string_view local_addr;    // e.g. "0.0.0.0:5000" or ""
    std::string_view remote_addr;   // e.g. "192.168.1.10:5000" or ""
    bool            enable_ipv6 = false;
    int             socket_rcvbuf = 0;    // 0 = system default
    int             socket_sndbuf = 0;

    /** Additional protocol-specific key=value pairs.
     *  E.g. for WebRTC transport: "ice_lite=true", "dtls_role=passive" */
    std::vector<std::pair<std::string_view, std::string_view>> extra;
};

/** Parse "host:port" string into addr parts. Returns {host, port}. */
NIMRTC_API_STABLE inline std::optional<std::pair<std::string_view, uint16_t>>
parse_endpoint(std::string_view ep) {
    auto colon = ep.rfind(':');
    if (colon == std::string_view::npos) return std::nullopt;
    auto host = ep.substr(0, colon);
    auto port_str = ep.substr(colon + 1);
    if (host == "*") host = "";
    int port = 0;
    for (char c : port_str) {
        if (c < '0' || c > '9') return std::nullopt;
        port = port * 10 + (c - '0');
    }
    if (port == 0 || port > 65535) return std::nullopt;
    return {{host, static_cast<uint16_t>(port)}};
}

// ---------------------------------------------------------------------------
// Events (fire-and-forget callbacks)
// ---------------------------------------------------------------------------

/** Notifies the engine that a complete inbound packet is ready. */
using RecvCallback = std::function<void(BufferView)>;

/** Notifies the engine of an asynchronous error. */
using NIMRTC_API_STABLE ErrorCallback = std::function<void(Status err, std::string_view msg)>;

/** Notifies the engine that ICE consent freshness (RFC 7675 / RFC 8445 §10)
 *  has been lost — i.e. no STUN Binding request received from the peer
 *  within the configured timeout window.
 *
 *  Fired exactly once per loss event by transports that implement consent
 *  tracking. After firing, the tracker is automatically rearmed; the
 *  callback may fire again on a subsequent loss event. */
using NIMRTC_API_STABLE OnConsentLost = std::function<void()>;

// ---------------------------------------------------------------------------
// ITransport
// ---------------------------------------------------------------------------

/** Plugin-seam network-transport interface (ADR-001 "Swapping the
 * transport").  Every in-tree transport backend (ICE, raw UDP,
 * in-process pipe, WebSocket) implements exactly this interface.
 *
 * Thread safety:
 *   - `set_callbacks()` / `set_on_consent_lost()` are called from a single
 *     thread (the engine thread) before `open()`.
 *   - `send()` is thread-safe — implementations must queue outbound data.
 *   - `recv()` may be called concurrently from different threads.
 *     `open()` / `close()` are called from a single thread (the engine thread).
 *
 * ## Per-symbol stability annotations (v0.14.x PER-SYMBOL-CLASSIFY)
 *
 * The file-level header comment carries the full list.  In brief:
 *   - `set_callbacks()` / `send()` / `recv()` / `local_addr()` /
 *     `remote_addr()` — STABLE.  The five-member contract is the load-bearing
 *     network seam; every custom transport impl uses it.
 *   - `set_on_consent_lost()` — STABLE (RFC 7675 / RFC 8445 §10 hook).
 *   - `ITransportFactory` / `SimpleTransportFactory` — STABLE. */

class NIMRTC_API_STABLE ITransport : public IPlugin {
public:
    // STABLE: `set_callbacks()` is the only entry point for registering
    // inbound recv / error callbacks; the signature has been stable since
    // v0.9.0 and every transport impl (ICE / raw UDP / in-process pipe)
    // uses it.
    /** Set callbacks before calling open().  Pass an empty std::function
     *  for either argument to indicate no handler is needed (e.g. a test
     *  harness that drives recv() manually).
     *  @note Not thread-safe — call from the engine thread. */
    virtual NIMRTC_API_STABLE void set_callbacks(RecvCallback on_recv,
                                         ErrorCallback on_error) noexcept = 0;

    /** Send a raw packet. The packet may be queued and sent asynchronously.
     *  @param view  Packet payload (header+data) to send.
     *  @param addr  Destination address. Empty = last-used address.
     *  @return kOk on queuing success; error code on failure.
     *  @note Thread-safe. */
    virtual NIMRTC_API_STABLE Status send(BufferView view,
                                   Addr dst_addr = {}) noexcept = 0;

    /** Drain received packets. Calls `on_recv` for each packet.
     *  @return number of packets dispatched, or negative on error.
     *  @note Call from engine tick or a dedicated recv thread. */
    virtual NIMRTC_API_STABLE int recv() noexcept = 0;

    /** Returns the local address this transport is bound to, or empty. */
    virtual NIMRTC_API_STABLE Addr local_addr() const noexcept = 0;

    /** Returns the resolved remote address, or empty. */
    virtual NIMRTC_API_STABLE Addr remote_addr() const noexcept = 0;

    // STABLE: the RFC 7675 / RFC 8445 §10 consent-freshness hook landed
    // in v0.10.x and is consumed by test_consent_freshness.cpp; keeping
    // it STABLE prevents a v1.0.x patch from silently dropping it.
    /** Register a one-shot callback fired when ICE consent freshness is lost
     *  (RFC 7675 / RFC 8445 §10). After firing, the tracker is automatically
     *  rearmed and the callback may fire again on a subsequent loss event.
     *
     *  The default implementation is a no-op — transports without consent
     *  tracking (e.g. raw UDP, in-process pipes) don't have to do anything.
     *  ICE-backed transports override this.
     *
     *  Passing a null callback clears the previously-registered one.
     *  @note Thread-safe; the callback fires on the transport's background
     *        thread — implementers must be thread-safe with respect to it. */
    virtual NIMRTC_API_STABLE void set_on_consent_lost(OnConsentLost cb) noexcept { (void)cb; }
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

// STABLE: the factory pattern (id / display_name / create) is the
// engine-facing seam for backend substitution. `DefaultTransportStackFactory`
// (id `"default"`), `WebRtcClassicStackFactory` (id `"webrtc-classic"`) and
// `RawUdpArqStackFactory` (id `"raw-udp-arq"`) are the concrete impls in
// the v0.12.x cycle; all use this interface. Marking STABLE so the registry
// key identity is frozen.
class NIMRTC_API_STABLE ITransportFactory {
public:
    virtual ~ITransportFactory() = default;

    /** Unique identifier, e.g. "webrtc", "udp", "quic", "proprietary".
     *
     *  STABLE: every `core::PluginRegistry::register_transport()` /
     *  `get_transport()` lookup keys off this string; callers use it to
     *  select backends, so it is the primary identity of a transport
     *  factory. */
    virtual NIMRTC_API_STABLE std::string_view id() const noexcept = 0;

    /** Short human-readable name, e.g. "WebRTC (ICE+DTLS-SRTP)".
     *
     *  STABLE: surfaces in log lines, `core::PluginRegistry` listings,
     *  and `NimRTCEngine` diagnostics. */
    virtual NIMRTC_API_STABLE std::string_view display_name() const noexcept = 0;

    /** Create a new transport instance. Ownership passes to caller.
     *
     *  STABLE: the factory pattern is the engine-facing seam for backend
     *  substitution (ADR-001 §"Swapping the transport"). */
    virtual NIMRTC_API_STABLE ITransport* create() const = 0;
};

/** Helper to create a factory that owns a transport instance.
 *
 *  STABLE: the most common in-tree factory implementation pattern. */
template<class T>
class NIMRTC_API_STABLE SimpleTransportFactory : public ITransportFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleTransportFactory(std::string_view id,
                                    std::string_view name) noexcept
        : id_(id), name_(name) {}

    NIMRTC_API_STABLE std::string_view id()          const noexcept override { return id_; }
    NIMRTC_API_STABLE std::string_view display_name()const noexcept override { return name_; }
    NIMRTC_API_STABLE ITransport* create()           const override { return new T(); }
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_TRANSPORT_HPP
