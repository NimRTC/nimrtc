/**
 * @file src/web_transport/src/web_transport_msquic_listener.hpp
 * @brief Internal header for WebTransportMsQuicListener implementation.
 *
 * NOT part of the public API. Contains private-internal types used
 * by the msquic-backed WebTransport server implementation:
 *
 *   - QpackDecoder    — minimal QPACK decoder for parsing HTTP/3 HEADERS
 *   - Http3Handler    — minimal HTTP/3 state machine per connection
 *   - Impl  — msquic listener + session lifecycle
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

namespace nimrtc::web_transport::detail {

// Forward declarations.
class Http3Handler;
struct ApiTable;

// ===========================================================================
// QpackDecoder — minimal QPACK decoder for HTTP/3 HEADERS parsing
// ===========================================================================
//
// Decodes a QPACK-encoded header block (RFC 9204).  This is a minimal
// implementation sufficient to extract the `:protocol` pseudo-header
// that Chrome sends in WebTransport requests.
//
// QPACK structure:
//   - Header block prefix: variable-length integer (required insert count)
//     followed by delta base (encoded with sign bit in low bits)
//   - Sequence of header entries (indexed or literal)
//
// For the NimRTC test server, Chrome typically sends headers like:
//   :method: CONNECT
//   :scheme: https
//   :authority: localhost:PORT
//   :path: /webtransport
//   :protocol: webtransport
//   sec-webtransport-version: ...
//   sec-webtransport-extensions: ...
// ===========================================================================

class QpackDecoder {
public:
    explicit QpackDecoder() = default;

    /**
     * @brief Decode a QPACK header block.
     * @param data Raw QPACK-encoded header block bytes.
     * @return Map of lowercase header name → value.
     */
    [[nodiscard]] std::unordered_map<std::string, std::string>
    decode(std::span<const std::uint8_t> data) noexcept;

    /**
     * @brief Maximum dynamic table size to honour during decoding.
     *        Chrome typically sends a SETTINGS frame advertising
     *        max_table_capacity = 0, so we don't need a real table.
     */
    void set_max_table_size(std::uint32_t size) noexcept {
        max_table_size_ = size;
    }

    // Static table entry (predefined per RFC 9204).
    struct StaticEntry {
        std::string_view name;
        std::string_view value;
    };

    // Dynamic table entry (built up as headers are decoded).
    struct DynamicEntry {
        std::string name;
        std::string value;
    };

    // Static table — indexed by 1-based position.
    static const std::vector<StaticEntry>& static_table() noexcept;

    // Helper: decode a variable-length integer (RFC 9000 §16).
    static std::pair<std::uint64_t, std::span<const std::uint8_t>>
    decode_varint(std::span<const std::uint8_t> data) noexcept;

private:
    // Decode one header entry from the bitstream.
    // Returns true if more entries remain; false on EOS or error.
    bool decode_entry(
        std::span<const std::uint8_t>& rem,
        std::string& out_name,
        std::string& out_value) noexcept;

    // Helper: decode an indexed header field (S=1 or S=0).
    bool decode_indexed(std::span<const std::uint8_t>& rem,
                        bool post_base, std::string& out_name,
                        std::string& out_value) noexcept;

    // Helper: decode a literal header field with possible name indexing.
    bool decode_literal(std::span<const std::uint8_t>& rem,
                        std::string& out_name, std::string& out_value) noexcept;

    // Dynamic table for this stream.
    std::vector<DynamicEntry> dynamic_table_;
    // Delta base for QPACK (tracks position in dynamic table).
    std::int64_t delta_base_ = 0;
    // Maximum dynamic table size (from SETTINGS_QPACK_MAX_TABLE_CAPACITY).
    std::uint32_t max_table_size_ = 0;
};

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
     * @param stream    QUIC stream handle (WebTransport request stream).
     * @param headers   Parsed HTTP headers (lowercase names).
     */
    void on_webtransport_request(
        HQUIC stream,
        const std::unordered_map<std::string, std::string>& headers) noexcept;

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

    // HTTP/3 handler — shared across all connections in this listener.
    // Each connection uses the same Http3Handler instance.
    std::unique_ptr<Http3Handler> http3_handler_;
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
// Http3Handler — minimal HTTP/3 state machine per connection
// ===========================================================================
//
// Manages HTTP/3 state for a single QUIC connection:
//   - Receives HTTP/3 SETTINGS frame from peer
//   - Parses HEADERS frames (via QpackDecoder)
//   - Sends SETTINGS + response HEADERS
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
     *        Sends the server's HTTP/3 SETTINGS frame.
     */
    bool on_connect(const ApiTable* api, HQUIC conn) noexcept;

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
    // Send an HTTP/3 SETTINGS frame.
    bool send_settings(const ApiTable* api, HQUIC conn) noexcept;

    // Send a HEADERS response for a WebTransport request.
    bool send_webtransport_response(const ApiTable* api, HQUIC conn,
                                    HQUIC stream) noexcept;

    // Send a generic HTTP/3 frame.
    bool send_frame(const ApiTable* api, HQUIC conn, HQUIC stream,
                    std::uint8_t frame_type,
                    std::span<const std::uint8_t> payload) noexcept;

    // Parse a SETTINGS frame.
    bool parse_settings(std::span<const std::uint8_t> payload) noexcept;

    // Parse a HEADERS frame and check for WebTransport.
    // On success, triggers Impl::on_webtransport_request().
    bool parse_headers(HQUIC stream, std::span<const std::uint8_t> payload) noexcept;

    // QPACK decoder (shared across streams for dynamic table).
    QpackDecoder qpack_;
    std::atomic<bool> settings_received_{false};

    // Back-pointer to the owning listener (never null during lifetime).
    Impl* listener_impl_;

    // Per-stream receive state.
    struct StreamState {
        std::vector<std::vector<std::uint8_t>> buf;  // reassembly queue
        bool fin_seen = false;
        bool webtransport_sent = false;  // response sent for this stream
        std::uint64_t stream_id = 0;
    };

    // Per-stream receive state.
    std::unordered_map<HQUIC, StreamState> stream_states_;
    std::mutex stream_mutex_;
};

} // namespace nimrtc::web_transport::detail

#endif // NIMRTC_WEB_TRANSPORT_MSQUIC_LISTENER_HPP
