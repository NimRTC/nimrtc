/**
 * @file src/web_transport/src/web_transport_msquic_listener.hpp
 * @brief Internal header for WebTransportMsQuicListener implementation.
 *
 * NOT part of the public API. Contains private-internal types used
 * by the msquic-backed WebTransport server implementation:
 *
 *   - Http3Handler    — HTTP/3 state machine (one per QUIC connection)
 *   - Impl  — msquic listener + session lifecycle
 *
 * The QPACK codec used to parse HTTP/3 header blocks lives in
 * web_transport_qpack.hpp (RFC 9204).
 *
 * Consumers should include the public header
 * <nimrtc/web_transport/web_transport_server.hpp> instead.
 */

#ifndef NIMRTC_WEB_TRANSPORT_MSQUIC_LISTENER_HPP
#define NIMRTC_WEB_TRANSPORT_MSQUIC_LISTENER_HPP

// Must come before any msquic header so QUIC_API_TABLE is defined.
extern "C" {
#include <msquic.h>
}

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nimrtc/plugins/base.hpp>
#include <nimrtc/web_transport/bidi_stream_iface.hpp>
#include <nimrtc/web_transport/web_transport_config.hpp>
#include <nimrtc/web_transport/web_transport_server.hpp>
#include <nimrtc/web_transport/web_transport_session_iface.hpp>
#include <nimrtc/web_transport/web_transport_state.hpp>

// Must come after nimrtc headers (so QUIC_API_TABLE is defined).
#include "web_transport_msquic_session.hpp"
#include "web_transport_qpack.hpp"

namespace nimrtc::web_transport::detail {

// Forward declarations.
class Http3Handler;
struct ApiTable;

// ===========================================================================
// QUIC callbacks — server-side
// ===========================================================================

/**
 * @brief QUIC listener callback function.
 *
 * Registered on the listener handle via SetCallbackHandler.
 * Dispatches QUIC_LISTENER_EVENT_NEW_CONNECTION to the Impl.
 */
QUIC_STATUS QUIC_API quic_listener_callback(
    HQUIC Listener,
    void* Context,
    QUIC_LISTENER_EVENT* Event) noexcept;

/**
 * @brief QUIC connection callback for listener-accepted connections.
 *
 * Registered on each accepted connection handle.  Bridges to
 * Http3Handler for HTTP/3 parsing + WebTransport session creation.
 */
QUIC_STATUS QUIC_API quic_server_conn_callback(
    HQUIC Connection,
    void* Context,
    QUIC_CONNECTION_EVENT* Event) noexcept;

/**
 * @brief QUIC stream callback for server-side streams.
 *
 * Registered on inbound streams.  The Http3Handler owns the stream
 * receive state; this callback forwards data to it.
 */
QUIC_STATUS QUIC_API quic_server_stream_callback(
    HQUIC Stream,
    void* Context,
    QUIC_STREAM_EVENT* Event) noexcept;

// ===========================================================================
// Impl — msquic listener implementation
// ===========================================================================

class Impl {
public:
    /**
     * @brief Construct and start a WebTransport server listener.
     *
     * @param port  TCP/UDP port to listen on (QUIC runs over UDP).
     * @param api   Pointer to the global ApiTable singleton.
     *              Must outlive this object.
     * @param cfg   WebTransport / QUIC configuration.
     * @param cb    Callback fired when a new WebTransport session starts.
     *              The callback receives a connected IWebTransportSession*.
     *              The session is owned by the caller (caller must call
     *              session->close() when done).
     *
     * @throws std::runtime_error if msquic API calls fail.
     */
    Impl(uint16_t port,
         const ApiTable* api,
         const WebTransportConfig& cfg,
         std::function<void(std::unique_ptr<IWebTransportSession>)> cb);
    ~Impl();

    // Non-copyable, non-movable.
    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;

    /** @brief Stop the listener and close all accepted connections. */
    void stop() noexcept;

    /**
     * @brief Called by Http3Handler when a WebTransport request is detected.
     *
     * @param conn     QUIC connection the request arrived on.
     * @param stream   QUIC stream handle (WebTransport request stream).
     * @param headers  Parsed HTTP headers (lowercase names).
     * @param datagram_send_enabled     Peer supports QUIC datagrams.
     * @param datagram_max_send_length  Maximum datagram payload size.
     * @param datagram_state_known      True once msquic reported the
     *                                 peer's datagram state.
     */
    void on_webtransport_request(
        HQUIC conn,
        HQUIC stream,
        const std::unordered_map<std::string, std::string>& headers,
        bool datagram_send_enabled,
        std::uint16_t datagram_max_send_length,
        bool datagram_state_known) noexcept;

    /** @brief True if the listener is running. */
    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    // Accessors for use by the connection callback.
    const ApiTable* api() const noexcept { return api_; }
    HQUIC registration() const noexcept { return registration_; }
    HQUIC configuration() const noexcept { return configuration_; }
    const WebTransportConfig& config() const noexcept { return cfg_; }

private:
    friend class Http3Handler;  // for http3_handler_ access
    // Grant the QUIC callbacks access so they can manage connections_.
    friend QUIC_STATUS QUIC_API quic_listener_callback(
        HQUIC, void*, QUIC_LISTENER_EVENT*) noexcept;
    friend QUIC_STATUS QUIC_API quic_server_conn_callback(
        HQUIC, void*, QUIC_CONNECTION_EVENT*) noexcept;

    const ApiTable* api_;
    HQUIC registration_ = nullptr;
    HQUIC configuration_ = nullptr;
    HQUIC listener_ = nullptr;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopped_{false};

    // Configuration (saved for accepted connections).
    WebTransportConfig cfg_;

    // Callback for new WebTransport sessions.
    std::function<void(std::unique_ptr<IWebTransportSession>)> on_session_cb_;

    // All accepted connections (kept alive while listener is running).
    std::vector<HQUIC> connections_;
    std::mutex conn_mutex_;
};

// ===========================================================================
// Listener — public RAII handle
// ===========================================================================

class Listener {
public:

    /**
     * @brief Start a WebTransport server on the given port.
     *
     * @param port  UDP port for QUIC listener.
     * @param cfg   WebTransport configuration (copied).
     * @param cb    Called with each new IWebTransportSession*.
     *              The session is in kConnected state when the callback fires.
     *
     * @throws std::runtime_error if the msquic listener cannot be started.
     */
    Listener(uint16_t port,
             const WebTransportConfig& cfg,
             std::function<void(std::unique_ptr<IWebTransportSession>)> cb);

    ~Listener();

    // Move-only.
    Listener(Listener&&) noexcept;
    Listener& operator=(Listener&&) noexcept;

    [[nodiscard]] bool running() const noexcept {
        return impl_ && impl_->running();
    }

    void stop() noexcept {
        if (impl_) impl_->stop();
    }

private:
    std::unique_ptr<Impl> impl_;
};

// ===========================================================================
// Http3Handler — HTTP/3 state machine (one instance per connection)
// ===========================================================================
//
// Manages HTTP/3 state for a single QUIC connection:
//   - Opens the server control stream (RFC 9114 §6.2.1) and sends SETTINGS
//   - Parses the peer's control stream and QPACK encoder stream
//   - Parses HEADERS frames on request streams (via the QPACK codec)
//   - Answers a WebTransport CONNECT request with ":status: 200"
//   - Calls Impl::on_webtransport_request() when a valid
//     WebTransport session-start request is detected
//
// HTTP/3 frames (RFC 9114):
//   0x00 = DATA
//   0x01 = HEADERS
//   0x04 = SETTINGS
//
// The handler stores a back-pointer to Impl so it can
// trigger session creation when a WebTransport request is detected.
// ===========================================================================

class Http3Handler {
public:
    /**
     * @brief Construct a HTTP/3 handler for a server-side connection.
     *
     * @param listener_impl  Back-pointer to the owning Impl.
     *                       Used to call on_webtransport_request() when
     *                       a valid WebTransport HEADERS frame is received.
     */
    explicit Http3Handler(Impl* listener_impl) noexcept;
    ~Http3Handler() = default;

    Http3Handler(const Http3Handler&)            = delete;
    Http3Handler& operator=(const Http3Handler&) = delete;

    /**
     * @brief Called when the QUIC connection is established.
     *        Opens the control stream and sends the server's SETTINGS.
     */
    bool on_connect(const ApiTable* api, HQUIC conn) noexcept;

    /**
     * @brief Called when the peer opens a stream.
     *
     * @param stream         QUIC stream handle.
     * @param unidirectional True for a peer-initiated unidirectional
     *                        stream (control / QPACK encoder stream).
     */
    void on_peer_stream_started(HQUIC stream, bool unidirectional) noexcept;

    /**
     * @brief Called when the peer's QUIC datagram support is known.
     *
     * The state is learned while the QUIC handshake runs, i.e. before the
     * WebTransport session object exists, so it is remembered here and
     * handed to the session when it is created.
     */
    void on_datagram_state_changed(bool send_enabled,
                                   std::uint16_t max_send_length) noexcept;

    /** @brief True once msquic reported the peer's datagram state. */
    [[nodiscard]] bool datagram_state_known() const noexcept {
        return datagram_state_known_;
    }

    /**
     * @brief Called when data arrives on a stream.
     * @param stream  QUIC stream handle.
     * @param data    Raw bytes received.
     * @param fin     True if this is the FIN (end of stream).
     * @return True if the data was consumed (processed or queued).
     */
    bool on_stream_data(HQUIC stream,
                         std::span<const std::uint8_t> data,
                         bool fin) noexcept;

    /**
     * @brief Called when a stream is closed.
     */
    void on_stream_closed(HQUIC stream) noexcept;

    /** @brief True if SETTINGS frame has been received from the peer. */
    [[nodiscard]] bool settings_received() const noexcept {
        return settings_received_.load(std::memory_order_acquire);
    }

private:
    /**
     * @brief Role of a peer-initiated stream inside HTTP/3.
     */
    enum class StreamRole : std::uint8_t {
        /** @brief Unidirectional stream whose type byte is not read yet. */
        kUnknown,
        /** @brief Client-initiated bidirectional request stream. */
        kRequest,
        /** @brief Client control stream (RFC 9114 §6.2.1). */
        kControl,
        /** @brief Client QPACK encoder stream (RFC 9204 §4.3). */
        kEncoder,
        /** @brief Client push stream — not allowed (RFC 9114 §6.2.2). */
        kPush,
        /** @brief Client QPACK decoder stream — not allowed. */
        kDecoder,
    };

    // Per-stream receive state.
    struct StreamState {
        StreamRole role = StreamRole::kUnknown;
        std::vector<std::uint8_t> buf;  // reassembly queue
        bool fin_seen = false;
        bool webtransport_sent = false;  // response sent for this stream
    };

    // Send an HTTP/3 SETTINGS frame on the server control stream.
    bool send_settings(const ApiTable* api, HQUIC conn) noexcept;

    // Send a HEADERS response for a WebTransport request.
    bool send_webtransport_response(const ApiTable* api, HQUIC conn,
                                    HQUIC stream) noexcept;

    // Send a generic HTTP/3 frame.
    bool send_frame(const ApiTable* api, HQUIC conn, HQUIC stream,
                    std::uint8_t frame_type,
                    std::span<const std::uint8_t> payload,
                    bool fin) noexcept;

    // Parse a SETTINGS frame.
    bool parse_settings(std::span<const std::uint8_t> payload) noexcept;

    // Parse a HEADERS frame and check for WebTransport.
    // On success, triggers Impl::on_webtransport_request().
    bool parse_headers(HQUIC stream,
                       std::span<const std::uint8_t> payload) noexcept;

    // Consume as many complete HTTP/3 frames as the buffer holds.
    bool process_request_frames(HQUIC stream,
                                std::vector<std::uint8_t>& buf) noexcept;

    // Consume as many complete HTTP/3 frames as the buffer holds.
    bool process_control_frames(std::vector<std::uint8_t>& buf) noexcept;

    // QPACK decoder (per connection: the dynamic table is connection-wide).
    QpackDecoder qpack_;
    std::atomic<bool> settings_received_{false};

    // Peer QUIC datagram support, learned during the QUIC handshake.
    // datagram_state_known_ is false until msquic reports the peer's
    // datagram state via QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED.
    bool datagram_state_known_ = false;
    bool datagram_send_enabled_ = false;
    std::uint16_t datagram_max_send_length_ = 0;

    // Back-pointer to the owning listener (never null during lifetime).
    Impl* listener_impl_;

    // The connection this handler belongs to (set in on_connect()).
    HQUIC conn_ = nullptr;

    // Per-stream receive state.
    std::unordered_map<HQUIC, StreamState> stream_states_;
    std::mutex stream_mutex_;
};

}  // namespace nimrtc::web_transport::detail

#endif  // NIMRTC_WEB_TRANSPORT_MSQUIC_LISTENER_HPP
