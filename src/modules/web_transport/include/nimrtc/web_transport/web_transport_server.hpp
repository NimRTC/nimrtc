/**
 * @file nimrtc/web_transport/web_transport_server.hpp
 * @brief IWebTransportServer — public WebTransport server seam
 *        (Case E, v0.14.0-beta.2).
 *
 * NIMRTC_API_UNSTABLE — the server interface is in beta.2 alongside
 * the msquic listener infrastructure (Case E).
 *
 * Provides a server-mode entry point that accepts incoming WebTransport
 * connections over QUIC (RFC 9000) + HTTP/3 (RFC 9114) + WebTransport
 * (RFC 9298).
 *
 * Chrome WebTransport JS API uses the WebSocket-like `new WebTransport(url)`
 * syntax which maps to an HTTP/3 CONNECT request with `:protocol: webtransport`.
 * This interface handles the HTTP/3 SETTINGS handshake and WebTransport
 * session establishment, then delivers connected IWebTransportSession objects
 * to the caller via a callback.
 *
 * ## Architecture
 *
 *   IWebTransportServer::start(port)  →  msquic listener bound to port
 *       ↓
 *   Chrome connects via WebTransport JS API
 *       ↓
 *   HTTP/3 SETTINGS exchange (automatic via Http3Handler)
 *       ↓
 *   WebTransport HEADERS frame parsed (QPACK decoder)
 *       ↓
 *   on_session(session) callback fires
 *       ↓
 *   Caller receives IWebTransportSession* (already in kConnected state)
 *
 * ## msquic listener strategy
 *
 * The msquic C API provides `MsQuicListenerOpen` + `MsQuicListenerStart`.
 * The listener callback (`QUIC_LISTENER_CALLBACK`) fires on new connections.
 * Each accepted connection uses a server-side connection callback that:
 *   1. Sends the server's HTTP/3 SETTINGS frame.
 *   2. Parses the client's SETTINGS frame.
 *   3. Parses the client's HEADERS frame (QPACK-encoded WebTransport request).
 *   4. Sends a 200 response HEADERS frame.
 *   5. Fires the on_session callback.
 *
 * For the QPACK decoder (RFC 9204), a minimal implementation handles Chrome's
 * WebTransport request format (`:method: CONNECT`, `:path`, `:protocol: webtransport`,
 * `sec-webtransport-version`).
 *
 * @see docs/adr/ADR-009-pal-slice-1.md for the Case E architecture notes.
 */

#ifndef NIMRTC_WEB_TRANSPORT_SERVER_HPP
#define NIMRTC_WEB_TRANSPORT_SERVER_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include <nimrtc/core/api_version.hpp>   // NIMRTC_API_UNSTABLE
#include <nimrtc/web_transport/web_transport_config.hpp>
#include <nimrtc/web_transport/web_transport_session_iface.hpp>

namespace nimrtc::web_transport {

/**
 * @brief Callback type for new WebTransport sessions.
 *
 * Fired once per Chrome WebTransport connection that successfully
 * completes the HTTP/3 + WebTransport handshake.
 *
 * The session is already in kConnected state when this fires.
 * The caller receives a fully-connected IWebTransportSession* and
 * is responsible for calling session->close() when done.
 *
 * The callback is NOT invoked from the msquic worker thread — it
 * is dispatched through the Http3Handler's event loop, which
 * posts to the calling thread (engine thread) to maintain the
 * IWebTransportSession thread-safety contract.
 *
 * @param session  Owning unique_ptr to the connected session.
 *                 The callback takes ownership; callers that want
 *                 to keep the session alive must call release() on
 *                 the unique_ptr.
 */
using NIMRTC_API_UNSTABLE OnWebTransportSessionCb = std::function<void(
    std::unique_ptr<IWebTransportSession> session)>;

/**
 * @brief IWebTransportServer — pluggable WebTransport server.
 *
 * Listens on a UDP port for incoming QUIC connections and,
 * for each valid WebTransport-over-HTTP/3 handshake (RFC 9298),
 * delivers a connected IWebTransportSession to the registered callback.
 *
 * The msquic-backed implementation (beta.2) uses:
 *   - ALPN "sm" (WebTransport over QUIC)
 *   - HTTP/3 SETTINGS exchange (minimal)
 *   - QPACK decoder for HEADERS frame parsing
 *   - RFC 9298 WebTransport CONNECT request handling
 *
 * ## Usage
 *
 * @code
 *   #include <nimrtc/web_transport/web_transport_server.hpp>
 *
 *   IWebTransportServer* server = create_msquic_webtransport_server();
 *   server->set_on_session([](auto session) {
 *       // session->state() == WebTransportState::kConnected
 *       auto* stream = session->open_bidi_stream();
 *       stream->send({...});
 *   });
 *   server->start(9443);
 *   // ... run event loop ...
 *   server->stop();
 * @endcode
 *
 * @see v0.14-plan.md §2.1.3 WT-SEVER for the server-mode scope.
 */
class NIMRTC_API_UNSTABLE IWebTransportServer {
public:
    virtual ~IWebTransportServer() = default;

    /**
     * @brief Start the WebTransport server on the given UDP port.
     *
     * Binds a msquic listener to the port and starts accepting
     * incoming QUIC connections.
     *
     * @param port  UDP port number for the QUIC listener.
     *              Standard WebTransport port is 443 (requires root
     *              on Unix); for testing use an unprivileged port
     *              (e.g. 9443).
     *
     * @return true  if the listener was started successfully.
     *         false if the port could not be bound or the msquic
     *               listener could not be created.
     */
    virtual bool start(uint16_t port) = 0;

    /**
     * @brief Stop the server and close all accepted connections.
     *
     * Idempotent.  After stop(), no new sessions are delivered.
     * Already-delivered sessions continue to operate normally.
     */
    virtual void stop() noexcept = 0;

    /**
     * @brief Register the callback for new WebTransport sessions.
     *
     * The callback fires once per Chrome WebTransport JS API connection
     * that successfully completes the HTTP/3 + WebTransport handshake.
     *
     * Only one callback can be registered at a time.  Setting a new
     * callback replaces the previous one.  Pass an empty std::function
     * to unregister.
     *
     * Thread safety: can be called before start(), after stop(), or
     * concurrently with session callbacks firing (the implementation
     * uses an atomic or mutex to protect the callback storage).
     */
    virtual void set_on_session(OnWebTransportSessionCb cb) noexcept = 0;

    /**
     * @brief True if the server is currently running.
     *
     * Returns true between start() and stop() calls.
     */
    [[nodiscard]] virtual bool running() const noexcept = 0;
};

// ===========================================================================
// Factory function — msquic-backed implementation
// ===========================================================================

/**
 * @brief Create a msquic-backed IWebTransportServer.
 *
 * The returned server uses the same msquic C library as the client-side
 * WebTransportMsQuicSession (shared API table singleton).  Multiple
 * server instances on different ports are supported.
 *
 * @param cfg  WebTransport / QUIC configuration (copied into the server).
 *             The cfg.peer_hostname and cfg.peer_port fields are not
 *             used for server mode (they apply to the client-side
 *             ConnectionStart in the session implementation).
 *
 * @return A new IWebTransportServer.  Never returns nullptr.
 *         Throws std::runtime_error if msquic cannot be loaded.
 */
[[nodiscard]] NIMRTC_API_UNSTABLE
std::unique_ptr<IWebTransportServer>
create_msquic_webtransport_server(
    const WebTransportConfig& cfg = WebTransportConfig{}) noexcept(false);

} // namespace nimrtc::web_transport

#endif // NIMRTC_WEB_TRANSPORT_SERVER_HPP
