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
 * @note P0 scaffold — interface stable, binary layout TBD P1.
 */

// base.hpp must be included BEFORE the include guard.
// This ensures base types are always available regardless of include order.
#include "nimrtc/plugins/base.hpp"

#ifndef NIMRTC_PLUGINS_TRANSPORT_HPP
#define NIMRTC_PLUGINS_TRANSPORT_HPP

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
struct TransportConfig {
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
inline std::optional<std::pair<std::string_view, uint16_t>>
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
using ErrorCallback = std::function<void(Status err, std::string_view msg)>;

/** Notifies the engine that ICE consent freshness (RFC 7675 / RFC 8445 §10)
 *  has been lost — i.e. no STUN Binding request received from the peer
 *  within the configured timeout window.
 *
 *  Fired exactly once per loss event by transports that implement consent
 *  tracking. After firing, the tracker is automatically rearmed; the
 *  callback may fire again on a subsequent loss event. */
using OnConsentLost = std::function<void()>;

// ---------------------------------------------------------------------------
// ITransport
// ---------------------------------------------------------------------------

class ITransport : public IPlugin {
public:
    /** Set callbacks before calling open(). */
    virtual void set_callbacks(RecvCallback on_recv,
                               ErrorCallback on_error) noexcept = 0;

    /** Send a raw packet. The packet may be queued and sent asynchronously.
     *  @param view  Packet payload (header+data) to send.
     *  @param addr  Destination address. Empty = last-used address.
     *  @return kOk on queuing success; error code on failure.
     *  @note Thread-safe. */
    virtual Status send(BufferView view,
                       Addr dst_addr = {}) noexcept = 0;

    /** Drain received packets. Calls `on_recv` for each packet.
     *  @return number of packets dispatched, or negative on error.
     *  @note Call from engine tick or a dedicated recv thread. */
    virtual int recv() noexcept = 0;

    /** Returns the local address this transport is bound to, or empty. */
    virtual Addr local_addr() const noexcept = 0;

    /** Returns the resolved remote address, or empty. */
    virtual Addr remote_addr() const noexcept = 0;

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
    virtual void set_on_consent_lost(OnConsentLost cb) noexcept { (void)cb; }
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

class ITransportFactory {
public:
    virtual ~ITransportFactory() = default;

    /** Unique identifier, e.g. "webrtc", "udp", "quic", "proprietary". */
    virtual std::string_view id() const noexcept = 0;

    /** Short human-readable name, e.g. "WebRTC (ICE+DTLS-SRTP)". */
    virtual std::string_view display_name() const noexcept = 0;

    /** Create a new transport instance. Ownership passes to caller. */
    virtual ITransport* create() const = 0;
};

/** Helper to create a factory that owns a transport instance. */
template<class T>
class SimpleTransportFactory : public ITransportFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleTransportFactory(std::string_view id,
                                   std::string_view name) noexcept
        : id_(id), name_(name) {}

    virtual std::string_view id()          const noexcept override { return id_; }
    virtual std::string_view display_name()const noexcept override { return name_; }
    virtual ITransport* create()           const override { return new T(); }
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_TRANSPORT_HPP
