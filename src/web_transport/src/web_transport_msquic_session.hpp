/**
 * @file src/web_transport/src/web_transport_msquic_session.hpp
 * @brief Internal header for WebTransportMsQuicSession implementation.
 *
 * NOT part of the public API. Contains private-internal types used
 * by the msquic-backed WebTransport session implementation:
 *
 *   - ApiTable    — cached msquic API function table (global singleton)
 *   - MsQuicBidiStream   — QUIC bidirectional stream wrapper
 *   - MsQuicUniStream    — QUIC unidirectional stream wrapper
 *   - Impl               — the session impl (lifetime + connection state)
 *
 * Consumers include <nimrtc/web_transport/web_transport_session_iface.hpp>
 * which is the canonical public header. Do NOT include this file from
 * outside the web_transport module.
 */

#ifndef NIMRTC_WEB_TRANSPORT_MSQUIC_SESSION_HPP
#define NIMRTC_WEB_TRANSPORT_MSQUIC_SESSION_HPP

// Must come before any msquic header so QUIC_API_TABLE is defined.
extern "C" {
#include <msquic.h>
}

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <nimrtc/plugins/base.hpp>
#include <nimrtc/web_transport/bidi_stream_iface.hpp>
#include <nimrtc/web_transport/uni_stream_iface.hpp>
#include <nimrtc/web_transport/web_transport_config.hpp>
#include <nimrtc/web_transport/web_transport_session_factory.hpp>
#include <nimrtc/web_transport/web_transport_session_iface.hpp>
#include <nimrtc/web_transport/web_transport_state.hpp>

namespace nimrtc::web_transport::detail {

// ===========================================================================
// ApiTable — process-wide cached msquic API function table
// ===========================================================================
//
// Initialised once on first use (thread-safe per C++11 static-local
// semantics).  Provides typed access to every MsQuic C API function
// so the session and stream implementations don't need to call
// MsQuicOpenVersion / MsQuicClose directly.
//
// MsQuicOpenVersion(2, &table) is cheap — it just returns a pointer to
// an already-loaded function table.  The global singleton caches the
// result so the handle is never released (MsQuicClose would release it)
// as long as any session is alive.
//
// NOTE: We intentionally do NOT call MsQuicClose at program shutdown.
// This is safe because the function table is read-only and the underlying
// library is unloaded at process exit anyway.  Calling MsQuicClose
// prematurely would invalidate the table for any still-alive session.
//
// v0.14.0-beta.1 scope: this is the ONLY process-wide msquic state.
// Future work (server mode, multiple ALPNs) may add a Registration
// per factory, but for now we use a single global table.
// ===========================================================================

struct ApiTable {
    // The QUIC_API_TABLE pointer.  Held as a non-const pointer to
    // match the msquic public API (MsQuicOpenVersion returns
    // `const QUIC_API_TABLE**`, but the function pointers inside
    // the table are themselves non-const — caching them in our
    // own non-const members is what we want).
    //
    // The table itself is logically read-only (we never mutate it)
    // but msquic's public API surface uses a non-const table
    // pointer, so we follow suit.
    QUIC_API_TABLE* api = nullptr;

    ApiTable();
    ~ApiTable();

    // Disallow copy/move (singleton).
    ApiTable(const ApiTable&) = delete;
    ApiTable& operator=(const ApiTable&) = delete;

    /** Returns true if the API table was successfully loaded. */
    [[nodiscard]] bool ok() const noexcept { return api != nullptr; }

    /** Process-wide singleton accessor. Thread-safe on first call. */
    static ApiTable& instance() noexcept;

    // ---- Convenience accessors (mirrors the QUIC_API_TABLE struct) ----
    // Each member uses the same typedef msquic uses internally
    // (e.g. `QUIC_STREAM_SEND_FN`).  This guarantees the
    // cached pointer type is exactly what the source
    // `api->StreamSend` field has, so the assignment in
    // `ApiTable::ApiTable` is a direct same-type copy with
    // no const_cast needed.
    //
    // Stream lifecycle
    QUIC_STREAM_OPEN_FN            StreamOpen;
    QUIC_STREAM_CLOSE_FN           StreamClose;
    QUIC_STREAM_START_FN           StreamStart;
    QUIC_STREAM_SHUTDOWN_FN        StreamShutdown;
    QUIC_STREAM_SEND_FN            StreamSend;
    QUIC_STREAM_RECEIVE_COMPLETE_FN StreamReceiveComplete;
    QUIC_STREAM_RECEIVE_SET_ENABLED_FN StreamReceiveSetEnabled;

    // Connection lifecycle
    QUIC_CONNECTION_OPEN_FN             ConnectionOpen;
    QUIC_CONNECTION_CLOSE_FN            ConnectionClose;
    QUIC_CONNECTION_SHUTDOWN_FN         ConnectionShutdown;
    QUIC_CONNECTION_START_FN            ConnectionStart;
    QUIC_CONNECTION_SET_CONFIGURATION_FN ConnectionSetConfiguration;

    // Configuration
    QUIC_CONFIGURATION_OPEN_FN  ConfigurationOpen;
    QUIC_CONFIGURATION_CLOSE_FN ConfigurationClose;

    // Registration
    QUIC_REGISTRATION_OPEN_FN  RegistrationOpen;
    QUIC_REGISTRATION_CLOSE_FN RegistrationClose;

    // Datagram
    QUIC_DATAGRAM_SEND_FN DatagramSend;

    // Context
    QUIC_SET_CONTEXT_FN          SetContext;
    QUIC_GET_CONTEXT_FN          GetContext;
    QUIC_SET_CALLBACK_HANDLER_FN SetCallbackHandler;
};

// Forward declaration of Impl — required because MsQuicBidiStream /
// MsQuicUniStream hold an `Impl*` back-pointer and are themselves
// declared before the full Impl class definition below.
class Impl;

// Forward declaration of the QUIC stream callback so that
// `MsQuicUniStream::set_handle` (inline-defined inside the class
// body) can take its address.  The full definition lives in the
// .cpp file; we declare it here to satisfy the inline body.
QUIC_STATUS QUIC_API quic_stream_callback(
    HQUIC Stream,
    void* Context,
    QUIC_STREAM_EVENT* Event) noexcept;

// Internal dispatch helpers used by quic_stream_callback.  Defined
// in the .cpp; declared here so the callback function can call them.
class MsQuicBidiStream;
class MsQuicUniStream;
QUIC_STATUS quic_stream_callback_bidi_impl(
    HQUIC, MsQuicBidiStream* s, QUIC_STREAM_EVENT* Event) noexcept;
QUIC_STATUS quic_stream_callback_uni_impl(
    HQUIC, MsQuicUniStream* s, QUIC_STREAM_EVENT* Event) noexcept;

// ===========================================================================
// MsQuicBidiStream — internal wrapper for QUIC bidirectional stream
// ===========================================================================
//
// Wraps a QUIC bidirectional stream (HQUIC handle) as an
// IBidirectionalStream.  The wrapper is owned by the parent
// WebTransportMsQuicSession::Impl and is invalidated (returns
// kErrNotReady from all methods) once the session closes.
//
// Thread model: NOT thread-safe.  All methods must be called from
// the engine thread, which serialises all WebTransport calls per the
// IWebTransportSession contract.
// ===========================================================================

class MsQuicBidiStream final : public IBidirectionalStream {
public:
    MsQuicBidiStream(HQUIC stream, const ApiTable* api,
                     Impl* session) noexcept;
    ~MsQuicBidiStream() override;

    // ---- IBidirectionalStream ----
    plugins::Status send(std::span<const std::uint8_t> bytes) noexcept override;
    plugins::Status recv(std::span<std::uint8_t> out) noexcept override;
    void set_on_data(OnStreamDataCb cb) noexcept override;
    void close() noexcept override;
    void reset() noexcept override;

    // ---- Internal ----
    /** Called by the QUIC stream callback when data arrives. */
    void on_receive(std::span<const std::uint8_t> bytes, bool fin) noexcept;

    /** Called by the QUIC stream callback when the stream is done. */
    void on_peer_send_shutdown() noexcept;
    void on_peer_send_aborted() noexcept;
    void on_peer_receive_aborted() noexcept;
    void on_stream_closed() noexcept;

    /** Returns true if the stream is still usable. */
    [[nodiscard]] bool valid() const noexcept { return stream_ != nullptr; }

    /** The QUIC stream handle, for use by the connection callback. */
    HQUIC handle() const noexcept { return stream_; }

    /** Context pointer stored in the msquic handle. */
    static MsQuicBidiStream* from_context(void* ctx) noexcept;

    /** Initialise msquic context (SetContext + SetCallbackHandler).
     *  Called by Impl::open_bidi_stream() after StreamOpen succeeds. */
    void init_msquic_context(HQUIC stream) noexcept;

private:
    HQUIC stream_;
    const ApiTable* api_;

    // Inbound queue (filled by QUIC callback, drained by recv()).
    std::vector<std::vector<std::uint8_t>> recv_queue_;
    std::mutex recv_mutex_;
    bool recv_fin_seen_ = false;

    OnStreamDataCb on_data_cb_;
    std::atomic<bool> closed_{false};
};

// ===========================================================================
// MsQuicUniStream — internal wrapper for QUIC unidirectional stream
// ===========================================================================
//
// Wraps a QUIC unidirectional stream (HQUIC handle) as an
// IUnidirectionalStream.  The wrapper is owned by the parent
// WebTransportMsQuicSession::Impl.
//
// Thread model: NOT thread-safe.
// ===========================================================================

class MsQuicUniStream final : public IUnidirectionalStream {
public:
    MsQuicUniStream(HQUIC stream, const ApiTable* api,
                    Impl* session) noexcept;
    ~MsQuicUniStream() override;

    // ---- IUnidirectionalStream ----
    plugins::Status send(std::span<const std::uint8_t> bytes) noexcept override;
    void close() noexcept override;
    void reset() noexcept override;

    // ---- Internal ----
    void on_receive(std::span<const std::uint8_t> bytes, bool fin) noexcept;
    void on_peer_send_aborted() noexcept;
    void on_stream_closed() noexcept;

    [[nodiscard]] bool valid() const noexcept { return stream_ != nullptr; }
    HQUIC handle() const noexcept { return stream_; }
    static MsQuicUniStream* from_context(void* ctx) noexcept;

    /** Transfer a valid stream handle to this wrapper and initialise
     *  msquic context.  Called by Impl::open_uni_stream() after
     *  StreamOpen succeeds. */
    void set_handle(HQUIC stream) noexcept {
        stream_ = stream;
        if (stream_ != nullptr) {
            api_->SetContext(stream_, this);
            api_->SetCallbackHandler(
                stream_,
                reinterpret_cast<void*>(&quic_stream_callback),
                reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(this) | 1u));
        }
    }

private:
    HQUIC stream_;
    const ApiTable* api_;
    std::atomic<bool> closed_{false};
};

// ===========================================================================
// Impl — internal session implementation
// ===========================================================================
//
// Holds the QUIC connection handle, all active streams, and the session
// state machine.  This class is referenced by the QUIC connection
// callback (which is a C function, so it receives a HQUIC handle and
// a void* context pointer).
//
// The public IWebTransportSession surface is a thin owner of a
// std::unique_ptr<Impl>.
// ===========================================================================

class Impl {
public:
    /**
     * @brief Construct a client-mode QUIC connection.
     *
     * Opens a new connection to `peer_hostname`:`peer_port`.
     * The connection is in kConnecting state initially.
     * Call `start()` after construction to begin the handshake.
     *
     * @param peer_hostname  Server hostname (SNI). Must not be empty.
     * @param peer_port      Server port (0 = pick default).
     * @param api            Pointer to the global ApiTable singleton.
     *                        Must outlive this object.
     * @param cfg            WebTransport connection configuration.
     * @throws std::runtime_error if msquic API cannot be opened.
     */
    Impl(const std::string& peer_hostname, uint16_t peer_port,
         const ApiTable* api, const WebTransportConfig& cfg);
    ~Impl();

    // ---- IWebTransportSession surface (delegated from the owner) ----
    WebTransportState state() const noexcept { return state_.load(std::memory_order_acquire); }
    IBidirectionalStream* open_bidi_stream() noexcept;
    IUnidirectionalStream* open_uni_stream() noexcept;
    plugins::Status send_datagram(std::span<const std::uint8_t> bytes) noexcept;
    void set_on_datagram(OnDatagramCb cb) noexcept { std::lock_guard<std::mutex> l{cb_mutex_}; on_datagram_cb_ = std::move(cb); }
    void close() noexcept;

    /** Called by the QUIC connection callback. */
    void on_connected() noexcept;
    void on_shutdown_initiated_by_transport(QUIC_STATUS status) noexcept;
    void on_shutdown_initiated_by_peer(uint64_t error_code) noexcept;
    void on_shutdown_complete() noexcept;
    void on_peer_stream_started(HQUIC stream, QUIC_STREAM_OPEN_FLAGS flags) noexcept;
    void on_datagram_state_changed(bool send_enabled, uint16_t max_send_length) noexcept;
    void on_datagram_received(const uint8_t* data, uint32_t len) noexcept;
    void on_connection_closed() noexcept;

    /** Returns the QUIC connection handle, for use by stream callbacks. */
    HQUIC connection_handle() const noexcept { return conn_; }

    /** Context pointer stored in the msquic connection handle. */
    static Impl* from_context(void* ctx) noexcept;

private:
    friend class MsQuicBidiStream;
    friend class MsQuicUniStream;

    // Helper: open a stream of the given type and register it.
    template<class StreamT>
    StreamT* open_stream(QUIC_STREAM_OPEN_FLAGS flags) noexcept;

    const ApiTable* api_;
    HQUIC registration_ = nullptr;
    HQUIC configuration_ = nullptr;
    HQUIC conn_ = nullptr;

    std::atomic<WebTransportState> state_{WebTransportState::kConnecting};

    // All active streams owned by this session.
    std::vector<std::unique_ptr<MsQuicBidiStream>> bidi_streams_;
    std::vector<std::unique_ptr<MsQuicUniStream>>  uni_streams_;
    std::mutex streams_mutex_;

    // Inbound datagram callback.
    OnDatagramCb on_datagram_cb_;
    std::mutex cb_mutex_;

    // Datagram send state (set by DATAGRAM_STATE_CHANGED event).
    bool datagram_send_enabled_ = false;
    uint16_t datagram_max_send_length_ = 0;
    std::mutex datagram_mutex_;

    // Flag: close() has been called — prevents new streams.
    std::atomic<bool> closing_{false};
};

// ===========================================================================
// QUIC callback — bridges msquic C events to Impl methods
// ===========================================================================

/**
 * @brief QUIC connection callback function.
 *
 * This is the single C callback that msquic invokes for all connection
 * events on a given HQUIC connection handle.  It dispatches to the
 * corresponding Impl method based on the event type.
 *
 * Registered via QUIC_CONNECTION_CALLBACK_HANDLER at connection open.
 * The Impl pointer is stored as the connection context via SetContext.
 */
QUIC_STATUS QUIC_API quic_connection_callback(
    HQUIC Connection,
    void* Context,
    QUIC_CONNECTION_EVENT* Event) noexcept;

/**
 * @brief QUIC stream callback function.
 *
 * This is the single C callback that msquic invokes for all stream
 * events on a given HQUIC stream handle.  It dispatches to the
 * appropriate stream wrapper based on the stream's context.
 *
 * Registered via QUIC_STREAM_CALLBACK_HANDLER at stream open.
 * The stream wrapper pointer (MsQuicBidiStream* or MsQuicUniStream*)
 * is stored as the stream context via SetContext.
 */
QUIC_STATUS QUIC_API quic_stream_callback(
    HQUIC Stream,
    void* Context,
    QUIC_STREAM_EVENT* Event) noexcept;

} // namespace nimrtc::web_transport::detail

#endif // NIMRTC_WEB_TRANSPORT_MSQUIC_SESSION_HPP
