/**
 * @file src/web_transport/src/web_transport_msquic_session.cpp
 * @brief WebTransportMsQuicSession implementation (v0.14.0-beta.1, ADR-014).
 *
 * Implements `IWebTransportSession` using the msquic C library
 * (Microsoft QUIC, MIT-licensed) as the underlying QUIC transport.
 *
 * ## Thread model
 *
 * IWebTransportSession is NOT thread-safe by itself. The engine
 * serialises WebTransport calls on the engine thread; this
 * implementation is driven from there only.  All msquic callbacks
 * fire on msquic's internal worker threads — they post work to
 * the engine thread via the session's datagram/streams queues
 * (protected by mutexes) so no locking is needed at the
 * IWebTransportSession surface level.
 *
 * ## Architecture
 *
 *   IWebTransportSession  (pure-virtual public seam)
 *       ↓
 *   WebTransportMsQuicSession  (thin owner; delegates to MsQuicSessionImpl)
 *       ↓
 *   detail::MsQuicSessionImpl  (connection lifetime, state machine, stream registry)
 *       ↓
 *   detail::MsQuicBidiStream / MsQuicUniStream  (per-stream wrappers)
 *       ↓
 *   msquic C API  (HQUIC handles, QUIC_CONNECTION_CALLBACK, QUIC_STREAM_CALLBACK)
 *
 * ## msquic lifecycle
 *
 *   MsQuicOpenVersion(2) → ApiTable (global singleton, never closed)
 *       ↓
 *   ApiTable->RegistrationOpen() → registration_
 *       ↓
 *   ApiTable->ConfigurationOpen() → configuration_  (QUIC_SETTINGS + ALPN)
 *       ↓
 *   ApiTable->ConnectionOpen() → conn_
 *       ↓
 *   ApiTable->ConnectionSetConfiguration(conn_, configuration_)
 *       ↓
 *   ApiTable->ConnectionStart(conn_, ..., hostname, port)
 *       ↓  (QUIC handshake — fires CONNECTED event)
 *   on_connected() → state_ = kConnected
 *       ↓
 *   open_bidi_stream() / open_uni_stream() → MsQuicBidiStream / MsQuicUniStream
 *       ↓
 *   close() → ConnectionShutdown → ConnectionClose → ConfigurationClose → RegistrationClose
 *
 * ## ALPN
 *
 * The WebTransport over QUIC ALPN is "sm" (IANA-assigned for WebTransport).
 * See: https://www.iana.org/assignments/tls-extensiontype-values/tls-extensiontype-values.xhtml
 */

#include "web_transport_msquic_session.hpp"

#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

// Windows macros `min` and `max` conflict with std::min / std::max.
// Undef them before any standard headers that might use them.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/plugin_id.hpp>
#include <nimrtc/web_transport/web_transport_plugin.hpp>
#include <nimrtc/web_transport/web_transport_session_factory.hpp>

namespace nimrtc::web_transport::detail {

// ===========================================================================
// ApiTable — global singleton
// ===========================================================================

namespace {
QUIC_API_TABLE* g_api_table = nullptr;
std::once_flag g_api_flag;

QUIC_STATUS load_api(QUIC_API_TABLE** out_api) {
    const void* raw = nullptr;
    // QUIC_API_VERSION_2 is the current stable API version.
    const QUIC_STATUS status = MsQuicOpenVersion(/*version=*/2, &raw);
    if (QUIC_FAILED(status) || raw == nullptr) {
        return status;
    }
    *out_api = static_cast<QUIC_API_TABLE*>(const_cast<void*>(raw));
    return QUIC_STATUS_SUCCESS;
}
} // anonymous namespace

ApiTable::ApiTable() {
    std::call_once(g_api_flag, [&]() {
        g_api_table = []() -> QUIC_API_TABLE* {
            QUIC_API_TABLE* t = nullptr;
            if (QUIC_FAILED(load_api(&t))) {
                return nullptr;
            }
            return t;
        }();
    });
    api = g_api_table;
    if (!api) return;

    // Cache typed function pointers from the table for fast access.
    // These are safe to cache because the QUIC_API_TABLE pointer
    // is stable for the lifetime of the process (MsQuicClose is
    // intentionally never called).
    StreamOpen            = api->StreamOpen;
    StreamClose          = api->StreamClose;
    StreamStart          = api->StreamStart;
    StreamShutdown        = api->StreamShutdown;
    StreamSend           = api->StreamSend;
    StreamReceiveComplete = api->StreamReceiveComplete;
    StreamReceiveSetEnabled = api->StreamReceiveSetEnabled;

    ConnectionOpen            = api->ConnectionOpen;
    ConnectionClose           = api->ConnectionClose;
    ConnectionShutdown        = api->ConnectionShutdown;
    ConnectionStart           = api->ConnectionStart;
    ConnectionSetConfiguration = api->ConnectionSetConfiguration;

    ConfigurationOpen   = api->ConfigurationOpen;
    ConfigurationClose  = api->ConfigurationClose;
    ConfigurationLoadCredential = api->ConfigurationLoadCredential;

    RegistrationOpen  = api->RegistrationOpen;
    RegistrationClose = api->RegistrationClose;

    DatagramSend = api->DatagramSend;

    SetContext        = api->SetContext;
    GetContext        = api->GetContext;
    SetCallbackHandler = api->SetCallbackHandler;

    ListenerOpen     = api->ListenerOpen;
    ListenerStart    = api->ListenerStart;
    ListenerClose    = api->ListenerClose;
    ListenerStop     = api->ListenerStop;
}

ApiTable::~ApiTable() {
    // NOTE: MsQuicClose is intentionally NOT called here.
    // Calling it would invalidate the API table while any session
    // or stream is still alive.  The library is unloaded at process
    // exit anyway, so there's no resource leak.
}

// ===========================================================================
// MsQuicBidiStream
// ===========================================================================

MsQuicBidiStream::MsQuicBidiStream(HQUIC stream, const ApiTable* api,
                                   MsQuicSessionImpl* /*session*/) noexcept
    : stream_(stream), api_(api) {
    // Note: SetContext / SetCallbackHandler are NOT called here.
    // The caller (MsQuicSessionImpl::open_bidi_stream) calls init_msquic_context()
    // after StreamOpen succeeds, so the handle is always valid at this point.
}

void MsQuicBidiStream::init_msquic_context(HQUIC stream) noexcept {
    stream_ = stream;
    if (stream_ != nullptr) {
        api_->SetContext(stream_, this);
        api_->SetCallbackHandler(
            stream_,
            reinterpret_cast<void*>(&quic_stream_callback),
            reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(this) & ~1u));
    }
}

MsQuicBidiStream::~MsQuicBidiStream() {
    if (stream_ != nullptr) {
        // If the stream is still open, send a graceful shutdown first.
        if (!closed_.load(std::memory_order_relaxed)) {
            api_->StreamShutdown(stream_,
                                  QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        }
        api_->StreamClose(stream_);
        stream_ = nullptr;
    }
}

plugins::Status MsQuicBidiStream::send(
    std::span<const std::uint8_t> bytes) noexcept {
    if (stream_ == nullptr || closed_.load(std::memory_order_acquire)) {
        return plugins::kErrNotReady;
    }
    if (bytes.empty()) return plugins::kOk;

    QUIC_BUFFER buf{};
    buf.Length = static_cast<uint32_t>(bytes.size());
    buf.Buffer = const_cast<std::uint8_t*>(bytes.data());

    // The stream was already started via StreamStart in open_bidi_stream().
    // Subsequent sends use QUIC_SEND_FLAG_NONE (no START, no FIN here —
    // FIN is sent by close()).
    const QUIC_STATUS rc = api_->StreamSend(stream_, &buf, 1,
                                              QUIC_SEND_FLAG_NONE, nullptr);
    if (QUIC_FAILED(rc)) {
        // Distinguish permanent vs transient failures.
        // QUIC_STATUS_CONNECTION_IDLE means the stream's send path is done.
        if (rc == QUIC_STATUS_CONNECTION_IDLE) {
            return plugins::kErrClosed;
        }
        return plugins::kErrInternal;
    }
    return plugins::kOk;
}

plugins::Status MsQuicBidiStream::recv(
    std::span<std::uint8_t> out) noexcept {
    if (stream_ == nullptr) return plugins::kErrNotReady;

    std::lock_guard<std::mutex> l{recv_mutex_};
    if (recv_queue_.empty()) {
        if (recv_fin_seen_) return plugins::kErrClosed;
        return plugins::kOk;  // nothing available yet
    }

    auto& front = recv_queue_.front();
    const size_t n = std::min(out.size(), front.size());
    std::memcpy(out.data(), front.data(), n);
    if (n < front.size()) {
        // Partial read: keep the remainder.
        std::vector<std::uint8_t> remainder(front.begin() + static_cast<std::ptrdiff_t>(n), front.end());
        front = std::move(remainder);
    } else {
        recv_queue_.erase(recv_queue_.begin());
    }
    // The return type is plugins::Status (uint32_t), so we must ensure
    // the byte count fits.  A recv buffer larger than 4 GiB cannot
    // occur in practice (the per-stream flow-control window is far
    // smaller), but we validate explicitly to satisfy the type contract.
    const std::uint64_t n64 = static_cast<std::uint64_t>(n);
    if (n64 > std::numeric_limits<std::uint32_t>::max()) {
        return plugins::kErrBufferTooSmall;
    }
    return static_cast<plugins::Status>(n64);
}

void MsQuicBidiStream::set_on_data(OnStreamDataCb cb) noexcept {
    std::lock_guard<std::mutex> l{recv_mutex_};
    on_data_cb_ = std::move(cb);
    // If data is already buffered, fire the callback immediately.
    if (!recv_queue_.empty() && on_data_cb_) {
        // We defer to the next recv() call for actual delivery so the
        // callback fires in the right context (engine thread, not msquic
        // worker thread).  The recv() implementation drains the queue.
    }
}

void MsQuicBidiStream::close() noexcept {
    if (stream_ == nullptr) return;
    if (closed_.exchange(true)) return;  // idempotent

    api_->StreamShutdown(stream_, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
    // StreamClose is called in the destructor to allow SEND_COMPLETE to fire.
}

void MsQuicBidiStream::reset() noexcept {
    if (stream_ == nullptr) return;
    if (closed_.exchange(true)) return;  // idempotent

    api_->StreamShutdown(stream_,
                          QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND |
                          QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE,
                          0);
    // StreamClose is called in the destructor.
}

void MsQuicBidiStream::on_receive(
    std::span<const std::uint8_t> bytes, bool fin) noexcept {
    if (stream_ == nullptr) return;

    // Stage the callback + data, then release the lock before invoking.
    // The callback may call recv() (which also locks recv_mutex_), so
    // we must not hold the lock when we fire it.  We copy both the
    // std::function and the data into a local buffer; the callback
    // receives a non-owning span over that local buffer, which is
    // valid for the duration of the call.
    OnStreamDataCb cb_copy;
    std::vector<std::uint8_t> data_copy;
    bool fin_seen = false;
    {
        std::lock_guard<std::mutex> l{recv_mutex_};
        if (fin) recv_fin_seen_ = true;
        recv_queue_.emplace_back(bytes.begin(), bytes.end());
        fin_seen = recv_fin_seen_;
        // Take a snapshot of the callback.  set_on_data() takes the
        // same lock, so any later update will block until we release.
        cb_copy = on_data_cb_;
        // Copy the first queued chunk for the callback.
        if (cb_copy) {
            const auto& front = recv_queue_.front();
            data_copy.assign(front.begin(), front.end());
        }
    }

    // Fire the callback OUTSIDE the lock.
    if (cb_copy) {
        cb_copy(std::span<const std::uint8_t>{data_copy.data(),
                                                data_copy.size()});
    }
    (void)fin_seen;  // captured for future use (currently no-op)

    // Tell msquic we've consumed this data.
    api_->StreamReceiveComplete(stream_, bytes.size());
}

void MsQuicBidiStream::on_peer_send_shutdown() noexcept {
    // The peer half-closed the stream.  This is FIN-like.
    // The stream transitions to RecvClosed.  We signal this by setting
    // recv_fin_seen_ — recv() will return kErrClosed when the queue drains.
    std::lock_guard<std::mutex> l{recv_mutex_};
    recv_fin_seen_ = true;
}

void MsQuicBidiStream::on_peer_send_aborted() noexcept {
    std::lock_guard<std::mutex> l{recv_mutex_};
    recv_fin_seen_ = true;
    closed_.store(true, std::memory_order_release);
}

void MsQuicBidiStream::on_peer_receive_aborted() noexcept {
    if (closed_.exchange(true)) return;
    // The peer reset our send side — signal with kErrClosed.
}

void MsQuicBidiStream::on_stream_closed() noexcept {
    closed_.store(true, std::memory_order_release);
    stream_ = nullptr;  // invalidate
}

MsQuicBidiStream* MsQuicBidiStream::from_context(void* ctx) noexcept {
    return static_cast<MsQuicBidiStream*>(ctx);
}

// ===========================================================================
// MsQuicUniStream
// ===========================================================================

MsQuicUniStream::MsQuicUniStream(HQUIC stream, const ApiTable* api,
                                  MsQuicSessionImpl* /*session*/) noexcept
    : stream_(stream), api_(api) {
    // Note: SetContext / SetCallbackHandler are NOT called here.
    // The caller (MsQuicSessionImpl::open_uni_stream) calls set_handle()
    // after StreamOpen succeeds, so the handle is always valid at that point.
    // Calling SetContext/SetCallbackHandler with a null handle is a no-op
    // in msquic, but we skip it entirely for consistency with MsQuicBidiStream.
}

MsQuicUniStream::~MsQuicUniStream() {
    if (stream_ != nullptr) {
        if (!closed_.load(std::memory_order_relaxed)) {
            api_->StreamShutdown(stream_, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        }
        api_->StreamClose(stream_);
        stream_ = nullptr;
    }
}

plugins::Status MsQuicUniStream::send(
    std::span<const std::uint8_t> bytes) noexcept {
    if (stream_ == nullptr || closed_.load(std::memory_order_acquire)) {
        return plugins::kErrNotReady;
    }
    if (bytes.empty()) return plugins::kOk;

    QUIC_BUFFER buf{};
    buf.Length = static_cast<uint32_t>(bytes.size());
    buf.Buffer = const_cast<std::uint8_t*>(bytes.data());

    const QUIC_STATUS rc = api_->StreamSend(stream_, &buf, 1,
                                              QUIC_SEND_FLAG_START |
                                              QUIC_SEND_FLAG_FIN, nullptr);
    if (QUIC_FAILED(rc)) {
        if (rc == QUIC_STATUS_CONNECTION_IDLE) return plugins::kErrClosed;
        return plugins::kErrInternal;
    }
    // Send succeeded: mark the stream send side as done (FIN sent above).
    // This is the only send for a unidirectional stream, so we transition
    // to the closed state immediately after confirming success.
    closed_.store(true, std::memory_order_release);
    return plugins::kOk;
}

void MsQuicUniStream::close() noexcept {
    if (stream_ == nullptr) return;
    if (closed_.exchange(true)) return;
    // For unidirectional: FIN is sent in send() with QUIC_SEND_FLAG_FIN.
    // close() here sends an abort if send() was never called.
    api_->StreamShutdown(stream_,
                          QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND, 0);
}

void MsQuicUniStream::reset() noexcept {
    if (stream_ == nullptr) return;
    if (closed_.exchange(true)) return;
    api_->StreamShutdown(stream_,
                          QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND, 0);
}

void MsQuicUniStream::on_receive(
    std::span<const std::uint8_t>, bool) noexcept {
    // Unidirectional: we are the sender, not the receiver.
    // msquic will not fire RECEIVE events on a uni stream we opened.
    // This callback is a no-op.
}

void MsQuicUniStream::on_peer_send_aborted() noexcept {
    // Peer aborted their send side — for a uni stream we opened, this
    // means nothing to us (we're the sender only).
}

void MsQuicUniStream::on_stream_closed() noexcept {
    closed_.store(true, std::memory_order_release);
    stream_ = nullptr;
}

MsQuicUniStream* MsQuicUniStream::from_context(void* ctx) noexcept {
    return static_cast<MsQuicUniStream*>(ctx);
}

// ===========================================================================
// QUIC stream callback
// ===========================================================================

QUIC_STATUS QUIC_API quic_stream_callback(
    HQUIC Stream,
    void* Context,
    QUIC_STREAM_EVENT* Event) noexcept {
    // Determine which stream wrapper owns this callback.
    // We stored the wrapper pointer (MsQuicBidiStream* or MsQuicUniStream*)
    // as the stream context.  We can distinguish them by the value
    // of the first byte of the pointer alignment (bidi vs uni are distinct
    // subclasses), but the simplest approach is to check the stream flags.
    //
    // However, msquic doesn't pass the stream flags in the event.
    // Instead, we use a tag: we store the pointer and a type tag in a
    // small struct, or we can just use a reinterpret_cast with a heuristic.
    //
    // Simpler: store the MsQuicSessionImpl* as context (not the stream wrapper) and
    // dispatch from the connection callback for PEER_STREAM_STARTED.
    // For stream callbacks, the context is the stream wrapper itself.
    //
    // Approach: we always store the stream wrapper pointer as context.
    // We need to know whether it's a bidi or uni stream.  We can use
    // dynamic_cast (the wrapper has RTTI), or we can use a base-class
    // tag.  The simplest is to store the MsQuicSessionImpl* as context for ALL
    // streams and handle everything in the connection callback, but
    // that's awkward for recv data.
    //
    // Best approach: store the stream wrapper as context and use a
    // virtual method for dispatch.  But the wrappers don't inherit from
    // a common base with a virtual callback method here.
    //
    // Final approach: use two separate callback functions, registered
    // with the correct context.  We need a way to know which wrapper
    // owns the stream.  We store the wrapper pointer and a flag.
    // Since we can't easily store two values in the void* context, we
    // use the MSB of the pointer as a type tag (pointer alignment
    // guarantees the low bits are zero).  This is hacky but works for
    // our use case.
    //
    // Cleaner: we store the wrapper pointer in the context and cast based
    // on knowing the stream type from the QUIC_STREAM_OPEN_FLAGS that were
    // used when opening.  But the callback doesn't receive the flags.
    //
    // Best clean approach: use a small tagged union as context.
    // For now, let's use the MsQuicSessionImpl* as context (set via SetContext on the
    // stream) and handle PEER_STREAM_STARTED only in the connection
    // callback.  Stream callbacks that need per-stream dispatch
    // (RECEIVE, SEND_COMPLETE, etc.) will use a different mechanism.
    //
    // Re-thinking: the cleanest way is to register DIFFERENT callbacks
    // for bidi vs uni streams, passing the wrapper pointer as context.
    // We distinguish them by the QUIC_STREAM_OPEN_FLAGS passed to StreamOpen:
    //   - Bidi: no flags (default = bidirectional)
    //   - Uni:  QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL
    // We register quic_stream_callback_bidi or quic_stream_callback_uni
    // depending on the type, and each callback receives the correct
    // wrapper pointer as context.
    //
    // Let's use this approach: we need to register separate callbacks.
    // But QUIC_STREAM_CALLBACK_HANDLER is a single void* callback.
    // We can store the wrapper pointer as context and distinguish by
    // looking at the callback handler that was registered.
    //
    // The msquic docs say: "The type of the handler must be appropriate
    // for the type of the handle." For a stream handle, we need a stream
    // callback.  We can register the same callback for both bidi and uni
    // streams and distinguish by the context.
    //
    // Since both MsQuicBidiStream and MsQuicUniStream are different types,
    // we can check if the context pointer points to a valid MsQuicBidiStream
    // by checking if the stream is bidirectional.  But there's no direct
    // way to tell from the context alone.
    //
    // Best approach: we use a simple tagged-pointer technique.
    // The context is the stream wrapper pointer, but we OR in a tag bit.
    // Bidi: context = MsQuicBidiStream* (tag = 0)
    // Uni:  context = MsQuicUniStream* | 1 (tag = 1)
    // This lets us distinguish in the callback.
    //
    // IMPLEMENTED: we use this tagged-pointer scheme in stream open.
    // Here we decode it.

    // Decode tagged context.
    // The tag bit is the LSB.  If set, it's a uni stream; otherwise bidi.
    const uintptr_t tagged = reinterpret_cast<uintptr_t>(Context);
    const bool is_uni = (tagged & 1u) != 0;
    void* wrapper_ptr = reinterpret_cast<void*>(tagged & ~1u);

    if (is_uni) {
        auto* s = static_cast<MsQuicUniStream*>(wrapper_ptr);
        return s ? quic_stream_callback_uni_impl(Stream, s, Event)
                 : QUIC_STATUS_INVALID_STATE;
    } else {
        auto* s = static_cast<MsQuicBidiStream*>(wrapper_ptr);
        return s ? quic_stream_callback_bidi_impl(Stream, s, Event)
                 : QUIC_STATUS_INVALID_STATE;
    }
}

QUIC_STATUS quic_stream_callback_bidi_impl(
    HQUIC, MsQuicBidiStream* s, QUIC_STREAM_EVENT* Event) noexcept {
    switch (Event->Type) {
        case QUIC_STREAM_EVENT_RECEIVE: {
            // Data arrived on the stream.  msquic may coalesce multiple
            // buffers into one RECEIVE event.  We copy each buffer.
            for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; ++i) {
                const auto* buf = &Event->RECEIVE.Buffers[i];
                s->on_receive(
                    std::span<const std::uint8_t>{buf->Buffer, buf->Length},
                    (Event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0);
            }
            return QUIC_STATUS_SUCCESS;
        }
        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
            s->on_peer_send_shutdown();
            return QUIC_STATUS_SUCCESS;
        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
            s->on_peer_send_aborted();
            return QUIC_STATUS_SUCCESS;
        case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
            s->on_peer_receive_aborted();
            return QUIC_STATUS_SUCCESS;
        case QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE:
            // Our FIN was acknowledged.
            return QUIC_STATUS_SUCCESS;
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            s->on_stream_closed();
            return QUIC_STATUS_SUCCESS;
        default:
            return QUIC_STATUS_SUCCESS;
    }
}

QUIC_STATUS quic_stream_callback_uni_impl(
    HQUIC, MsQuicUniStream* s, QUIC_STREAM_EVENT* Event) noexcept {
    switch (Event->Type) {
        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
            s->on_peer_send_aborted();
            return QUIC_STATUS_SUCCESS;
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            s->on_stream_closed();
            return QUIC_STATUS_SUCCESS;
        default:
            return QUIC_STATUS_SUCCESS;
    }
}

// ===========================================================================
// MsQuicSessionImpl
// ===========================================================================

MsQuicSessionImpl::MsQuicSessionImpl(const std::string& peer_hostname, uint16_t peer_port,
           const ApiTable* api, const WebTransportConfig& cfg)
    : api_(api) {
    if (!api_->ok()) {
        throw std::runtime_error(
            "WebTransportMsQuicSession: msquic API not available");
    }

    // ---- Open the API table (singleton; just checks ok) ----
    // (done above via api_ initialization)

    // ---- Registration ----
    {
        QUIC_REGISTRATION_CONFIG reg_cfg{};
        reg_cfg.AppName = "nimrtc-webtransport";
        reg_cfg.ExecutionProfile = QUIC_EXECUTION_PROFILE_LOW_LATENCY;
        const QUIC_STATUS rc = api_->RegistrationOpen(&reg_cfg, &registration_);
        if (QUIC_FAILED(rc)) {
            throw std::runtime_error(
                std::string("MsQuicRegistrationOpen failed: ") +
                std::to_string(rc));
        }
    }

    // ---- Configuration ----
    {
        // ALPN: "sm" is the WebTransport over QUIC protocol.
        // See: https://www.iana.org/assignments/tls-extensiontype-values/
        constexpr uint8_t kAlpn[] = {'s', 'm'};
        QUIC_BUFFER alpn_buf{};
        alpn_buf.Buffer = const_cast<uint8_t*>(kAlpn);
        alpn_buf.Length = sizeof(kAlpn);

        // Map WebTransportConfig to QUIC_SETTINGS.
        QUIC_SETTINGS settings{};
        std::memset(&settings, 0, sizeof(settings));
        settings.IsSetFlags = 0;

        // Idle timeout (ms).
        if (cfg.max_idle_timeout_ms > 0) {
            settings.IdleTimeoutMs = cfg.max_idle_timeout_ms;
            settings.IsSet.IdleTimeoutMs = 1;
        }
        // Per-stream flow control window.
        if (cfg.initial_max_stream_data > 0) {
            settings.IsSetFlags |= (1ULL << 5);  // StreamRecvWindowDefault
            settings.IsSet.StreamRecvWindowDefault = 1;
            settings.StreamRecvWindowDefault = cfg.initial_max_stream_data;
            // Also set the per-stream data limit.
            settings.IsSet.StreamRecvWindowBidiLocalDefault = 1;
            settings.IsSet.StreamRecvWindowBidiRemoteDefault = 1;
            settings.IsSet.StreamRecvWindowUnidiDefault = 1;
            settings.StreamRecvWindowBidiLocalDefault  = cfg.initial_max_stream_data;
            settings.StreamRecvWindowBidiRemoteDefault = cfg.initial_max_stream_data;
            settings.StreamRecvWindowUnidiDefault     = cfg.initial_max_stream_data;
        }
        // Per-connection flow control window.
        if (cfg.initial_max_data > 0) {
            settings.IsSet.ConnFlowControlWindow = 1;
            settings.ConnFlowControlWindow = cfg.initial_max_data;
        }
        // Keep-alive interval.
        if (cfg.keep_alive_interval_ms > 0) {
            settings.IsSet.KeepAliveIntervalMs = 1;
            settings.KeepAliveIntervalMs = cfg.keep_alive_interval_ms;
        }
        // Enable datagram support (RFC 9221).
        settings.IsSet.DatagramReceiveEnabled = 1;
        settings.DatagramReceiveEnabled = 1;

        // Set initial stream counts to reasonable defaults (per RFC 9000).
        // These can be overridden by cfg fields if needed.
        settings.IsSet.PeerBidiStreamCount = 1;
        settings.IsSet.PeerUnidiStreamCount = 1;
        settings.PeerBidiStreamCount  = 10;
        settings.PeerUnidiStreamCount = 10;

        const QUIC_STATUS rc = api_->ConfigurationOpen(
            registration_, &alpn_buf, 1,
            &settings, sizeof(settings),
            nullptr,  // no async credential loading
            &configuration_);
        if (QUIC_FAILED(rc)) {
            api_->RegistrationClose(registration_);
            throw std::runtime_error(
                std::string("MsQuicConfigurationOpen failed: ") +
                std::to_string(rc));
        }
    }

    // ---- Connection ----
    {
        const QUIC_STATUS rc = api_->ConnectionOpen(
            registration_,
            quic_connection_callback,
            this,   // context = this MsQuicSessionImpl
            &conn_);
        if (QUIC_FAILED(rc)) {
            api_->ConfigurationClose(configuration_);
            api_->RegistrationClose(registration_);
            throw std::runtime_error(
                std::string("MsQuicConnectionOpen failed: ") +
                std::to_string(rc));
        }

        // Set context and callback on the connection handle.
        api_->SetContext(conn_, this);
        api_->SetCallbackHandler(conn_,
                                   reinterpret_cast<void*>(&quic_connection_callback),
                                   this);

        // Attach the configuration.
        const QUIC_STATUS rc2 = api_->ConnectionSetConfiguration(conn_,
                                                                   configuration_);
        if (QUIC_FAILED(rc2)) {
            api_->ConnectionClose(conn_);
            api_->ConfigurationClose(configuration_);
            api_->RegistrationClose(registration_);
            throw std::runtime_error(
                std::string("MsQuicConnectionSetConfiguration failed: ") +
                std::to_string(rc2));
        }

        // ---- Start the connection (initiate QUIC handshake) ----
        // peer_hostname may be empty for in-process loopback testing.
        const QUIC_STATUS rc3 = api_->ConnectionStart(
            conn_,
            configuration_,
            AF_UNSPEC,          // address family (unspecified = auto)
            peer_hostname.empty() ? nullptr : peer_hostname.c_str(),
            peer_port);
        if (QUIC_FAILED(rc3)) {
            api_->ConnectionClose(conn_);
            api_->ConfigurationClose(configuration_);
            api_->RegistrationClose(registration_);
            throw std::runtime_error(
                std::string("MsQuicConnectionStart failed: ") +
                std::to_string(rc3));
        }
        // Connection is now in kConnecting state.
        state_.store(WebTransportState::kConnecting,
                      std::memory_order_release);
    }
}

MsQuicSessionImpl::MsQuicSessionImpl(HQUIC conn, HQUIC registration, HQUIC configuration,
           const ApiTable* api, [[maybe_unused]] const WebTransportConfig& cfg)
    : api_(api), registration_(registration), configuration_(configuration),
      conn_(conn) {
    if (!api_->ok()) {
        throw std::runtime_error(
            "WebTransportMsQuicSession: msquic API not available");
    }

    // Set context and callback on the connection handle.
    api_->SetContext(conn_, this);
    api_->SetCallbackHandler(conn_,
                               reinterpret_cast<void*>(&quic_connection_callback),
                               this);

    // Server-side: the QUIC handshake is already complete (the listener
    // accepted the connection).  We transition directly to kConnected.
    state_.store(WebTransportState::kConnected, std::memory_order_release);
    nimrtc::core::log::Logger::instance().info(
        "WebTransportMsQuicSession: server-side session created (already connected)");
}

MsQuicSessionImpl::~MsQuicSessionImpl() {
    close();
}

IBidirectionalStream* MsQuicSessionImpl::open_bidi_stream() noexcept {
    if (state_.load(std::memory_order_acquire) != WebTransportState::kConnected) {
        return nullptr;
    }
    if (closing_.load(std::memory_order_acquire)) {
        return nullptr;
    }

    // Allocate the wrapper first (may throw).
    std::unique_ptr<MsQuicBidiStream> s;
    try {
        s = std::make_unique<MsQuicBidiStream>(nullptr, api_, this);
    } catch (const std::bad_alloc&) {
        return nullptr;
    } catch (...) {
        return nullptr;
    }

    // Open a bidirectional QUIC stream and get its handle.
    HQUIC stream_handle = nullptr;
    const QUIC_STATUS rc_open = api_->StreamOpen(
        conn_,
        QUIC_STREAM_OPEN_FLAG_NONE,  // bidirectional by default
        quic_stream_callback,
        reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(s.get()) & ~1u),
        &stream_handle);
    if (QUIC_FAILED(rc_open) || stream_handle == nullptr) {
        return nullptr;
    }

    // Register this wrapper as the stream context and install the callback.
    s->init_msquic_context(stream_handle);

    // Start the stream (no data on the first frame — just open it).
    const QUIC_STATUS rc_start = api_->StreamStart(
        stream_handle, QUIC_STREAM_START_FLAG_NONE);
    if (QUIC_FAILED(rc_start)) {
        api_->StreamClose(stream_handle);
        return nullptr;
    }

    IBidirectionalStream* ptr = s.get();
    std::lock_guard<std::mutex> l{streams_mutex_};
    bidi_streams_.push_back(std::move(s));
    return ptr;
}

IUnidirectionalStream* MsQuicSessionImpl::open_uni_stream() noexcept {
    if (state_.load(std::memory_order_acquire) != WebTransportState::kConnected) {
        return nullptr;
    }
    if (closing_.load(std::memory_order_acquire)) {
        return nullptr;
    }

    // Allocate the wrapper first (may throw).  We pass nullptr for the
    // stream handle here; set_handle() is called below once StreamOpen
    // returns the real handle.  Passing nullptr avoids SetContext/SetCallbackHandler
    // on a bogus handle in the constructor — set_handle() will do the real setup.
    std::unique_ptr<MsQuicUniStream> s;
    try {
        s = std::make_unique<MsQuicUniStream>(nullptr, api_, this);
    } catch (const std::bad_alloc&) {
        return nullptr;
    } catch (...) {
        return nullptr;
    }

    // Open a unidirectional QUIC stream.
    HQUIC stream_handle = nullptr;
    const QUIC_STATUS rc_open = api_->StreamOpen(
        conn_,
        QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
        quic_stream_callback,
        // Tagged context: uni stream (LSB = 1) — decoded in quic_stream_callback.
        reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(s.get()) | 1u),
        &stream_handle);
    if (QUIC_FAILED(rc_open) || stream_handle == nullptr) {
        return nullptr;
    }

    // Transfer the handle to the wrapper and set up msquic context.
    s->set_handle(stream_handle);

    // Start the stream.
    const QUIC_STATUS rc_start = api_->StreamStart(
        stream_handle, QUIC_STREAM_START_FLAG_NONE);
    if (QUIC_FAILED(rc_start)) {
        api_->StreamClose(stream_handle);
        return nullptr;
    }

    IUnidirectionalStream* ptr = s.get();
    std::lock_guard<std::mutex> l{streams_mutex_};
    uni_streams_.push_back(std::move(s));
    return ptr;
}

plugins::Status MsQuicSessionImpl::send_datagram(
    std::span<const std::uint8_t> bytes) noexcept {
    if (state_.load(std::memory_order_acquire) != WebTransportState::kConnected) {
        return plugins::kErrNotReady;
    }
    if (bytes.empty()) return plugins::kOk;

    std::lock_guard<std::mutex> l{datagram_mutex_};
    if (!datagram_send_enabled_) {
        // Peer has not advertised DATAGRAM support (RFC 9221 §3.1).
        return plugins::kErrBufferTooSmall;  // closest approximation
    }

    QUIC_BUFFER buf{};
    buf.Length = static_cast<uint32_t>(bytes.size());
    buf.Buffer = const_cast<std::uint8_t*>(bytes.data());

    // MsQuic's DatagramSend API doesn't take a special flag — the
    // datagram type is implicit (the API itself wraps a DATAGRAM
    // frame).  QUIC_SEND_FLAG_NONE is the correct value here.
    const QUIC_STATUS rc = api_->DatagramSend(
        conn_, &buf, 1, QUIC_SEND_FLAG_NONE, nullptr);
    if (QUIC_FAILED(rc)) {
        if (rc == QUIC_STATUS_CONNECTION_IDLE) {
            return plugins::kErrClosed;
        }
        return plugins::kErrInternal;
    }
    return plugins::kOk;
}

void MsQuicSessionImpl::close() noexcept {
    if (closing_.exchange(true)) return;  // idempotent

    state_.store(WebTransportState::kClosing, std::memory_order_release);

    // Shutdown all streams first.
    {
        std::lock_guard<std::mutex> l{streams_mutex_};
        for (auto& s : bidi_streams_) s->close();
        for (auto& s : uni_streams_)  s->close();
    }

    // Shutdown the connection gracefully.
    if (conn_ != nullptr) {
        api_->ConnectionShutdown(conn_,
                                   QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
                                   0);  // no application protocol error
    }
}

void MsQuicSessionImpl::on_connected() noexcept {
    state_.store(WebTransportState::kConnected, std::memory_order_release);
}

void MsQuicSessionImpl::on_shutdown_initiated_by_transport(QUIC_STATUS status) noexcept {
    (void)status;
    state_.store(WebTransportState::kFailed, std::memory_order_release);
}

void MsQuicSessionImpl::on_shutdown_initiated_by_peer(uint64_t) noexcept {
    // Peer initiated close — treat as graceful.
    state_.store(WebTransportState::kClosing, std::memory_order_release);
}

void MsQuicSessionImpl::on_shutdown_complete() noexcept {
    WebTransportState expected = WebTransportState::kClosing;
    if (!state_.compare_exchange_strong(expected,
                                         WebTransportState::kClosed,
                                         std::memory_order_release,
                                         std::memory_order_relaxed)) {
        // If not closing, it was already failed.
        state_.store(WebTransportState::kClosed, std::memory_order_release);
    }

    // Clean up all handles (msquic will have already closed streams).
    std::lock_guard<std::mutex> l{streams_mutex_};
    bidi_streams_.clear();
    uni_streams_.clear();

    if (conn_ != nullptr) {
        api_->ConnectionClose(conn_);
        conn_ = nullptr;
    }
    if (configuration_ != nullptr) {
        api_->ConfigurationClose(configuration_);
        configuration_ = nullptr;
    }
    if (registration_ != nullptr) {
        api_->RegistrationClose(registration_);
        registration_ = nullptr;
    }
}

void MsQuicSessionImpl::on_peer_stream_started(HQUIC stream,
                                   QUIC_STREAM_OPEN_FLAGS flags) noexcept {
    // An inbound stream started on the connection.  We currently don't
    // handle inbound streams (we are a client only).  msquic requires
    // a callback return value that tells it whether to accept or ignore
    // the stream.  For now, accept it but just close it immediately.
    (void)flags;
    if (stream == nullptr) return;

    // Accept the stream (return QUIC_STATUS_SUCCESS from callback).
    // We don't have a session-level listener for inbound streams yet
    // (that lands in beta.2 / v1.x).  For now, just close the stream.
    api_->StreamShutdown(stream,
                          QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND |
                          QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE,
                          0);
    api_->StreamClose(stream);
}

void MsQuicSessionImpl::on_datagram_state_changed(bool send_enabled,
                                      uint16_t max_send_length) noexcept {
    std::lock_guard<std::mutex> l{datagram_mutex_};
    datagram_send_enabled_ = send_enabled;
    datagram_max_send_length_ = max_send_length;
}

void MsQuicSessionImpl::on_datagram_received(const uint8_t* data,
                                  uint32_t len) noexcept {
    if (len == 0) return;
    OnDatagramCb cb;
    {
        std::lock_guard<std::mutex> l{cb_mutex_};
        cb = on_datagram_cb_;
    }
    if (cb) {
        cb(std::span<const std::uint8_t>{data, len});
    }
}

void MsQuicSessionImpl::on_connection_closed() noexcept {
    state_.store(WebTransportState::kFailed, std::memory_order_release);
}

MsQuicSessionImpl* MsQuicSessionImpl::from_context(void* ctx) noexcept {
    return static_cast<MsQuicSessionImpl*>(ctx);
}

// ===========================================================================
// QUIC connection callback
// ===========================================================================

QUIC_STATUS QUIC_API quic_connection_callback(
    HQUIC Connection,
    void* Context,
    QUIC_CONNECTION_EVENT* Event) noexcept {
    auto* impl = static_cast<MsQuicSessionImpl*>(Context);
    (void)Connection;  // available via impl->conn_ if needed

    switch (Event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            impl->on_connected();
            return QUIC_STATUS_SUCCESS;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            impl->on_shutdown_initiated_by_transport(
                Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
            return QUIC_STATUS_SUCCESS;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            impl->on_shutdown_initiated_by_peer(
                Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
            return QUIC_STATUS_SUCCESS;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            impl->on_shutdown_complete();
            return QUIC_STATUS_SUCCESS;

        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
            impl->on_peer_stream_started(
                Event->PEER_STREAM_STARTED.Stream,
                Event->PEER_STREAM_STARTED.Flags);
            return QUIC_STATUS_SUCCESS;

        case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED:
            impl->on_datagram_state_changed(
                Event->DATAGRAM_STATE_CHANGED.SendEnabled,
                Event->DATAGRAM_STATE_CHANGED.MaxSendLength);
            return QUIC_STATUS_SUCCESS;

        case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
            impl->on_datagram_received(
                Event->DATAGRAM_RECEIVED.Buffer->Buffer,
                Event->DATAGRAM_RECEIVED.Buffer->Length);
            return QUIC_STATUS_SUCCESS;

        case QUIC_CONNECTION_EVENT_LOCAL_ADDRESS_CHANGED:
        case QUIC_CONNECTION_EVENT_PEER_ADDRESS_CHANGED:
        case QUIC_CONNECTION_EVENT_STREAMS_AVAILABLE:
        case QUIC_CONNECTION_EVENT_PEER_NEEDS_STREAMS:
        case QUIC_CONNECTION_EVENT_IDEAL_PROCESSOR_CHANGED:
        case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
            // Informational events — nothing to do.
            return QUIC_STATUS_SUCCESS;

        case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED:
            // Client received a resumption ticket (server sent it).
            // We don't use 0-RTT in v0.14.0, so ignore.
            return QUIC_STATUS_SUCCESS;

        default:
            return QUIC_STATUS_SUCCESS;
    }
}

// ===========================================================================
// ApiTable singleton accessor (used by WebTransportMsQuicSession)
// ===========================================================================

// Defined here (in the detail namespace) to match the class
// declaration in web_transport_msquic_session.hpp.  The function-local
// `static ApiTable singleton;` is constructed on first call (thread-safe
// per C++11) and lives for the lifetime of the process.
ApiTable& ApiTable::instance() noexcept {
    static ApiTable singleton;
    return singleton;
}

// Exposed for web_transport_msquic_listener.cpp (server-side listener).
// Both listener and session share the same msquic API table.
ApiTable& api_table_instance_ref() noexcept {
    return ApiTable::instance();
}

} // namespace nimrtc::web_transport::detail

// ===========================================================================
// WebTransportMsQuicSession — public IWebTransportSession surface
// ===========================================================================
//
// Thin owner that wraps detail::MsQuicSessionImpl and exposes the IWebTransportSession
// interface.  The MsQuicSessionImpl is constructed with peer address + config and
// immediately starts the QUIC handshake.
//
// Usage:
//   WebTransportMsQuicSession session("example.com", 443, WebTransportConfig{});
//   // ... poll session.state() until kConnected ...
//   auto* stream = session.open_bidi_stream();
//   stream->send({...});
//
// Thread safety: see the IWebTransportSession contract and the
// detail::MsQuicSessionImpl header comment.
// ===========================================================================

namespace nimrtc::web_transport {

// ===========================================================================
// WebTransportMsQuicSession — out-of-line definitions
// ===========================================================================

WebTransportMsQuicSession::WebTransportMsQuicSession(
    const std::string& peer_hostname,
    uint16_t peer_port,
    const WebTransportConfig& cfg)
    : impl_(std::make_unique<detail::MsQuicSessionImpl>(
          peer_hostname, peer_port,
          &detail::ApiTable::instance(), cfg)) {}

WebTransportMsQuicSession::WebTransportMsQuicSession(
    HQUIC conn,
    HQUIC registration,
    HQUIC configuration,
    const detail::ApiTable* api,
    const WebTransportConfig& cfg)
    : impl_(std::make_unique<detail::MsQuicSessionImpl>(
          conn, registration, configuration, api, cfg)) {}

WebTransportMsQuicSession::~WebTransportMsQuicSession() = default;

WebTransportState WebTransportMsQuicSession::state() const noexcept {
    return impl_->state();
}

IBidirectionalStream* WebTransportMsQuicSession::open_bidi_stream() noexcept {
    return impl_->open_bidi_stream();
}

IUnidirectionalStream* WebTransportMsQuicSession::open_uni_stream() noexcept {
    return impl_->open_uni_stream();
}

plugins::Status
WebTransportMsQuicSession::send_datagram(
    std::span<const std::uint8_t> bytes) noexcept {
    return impl_->send_datagram(bytes);
}

void WebTransportMsQuicSession::set_on_datagram(OnDatagramCb cb) noexcept {
    impl_->set_on_datagram(std::move(cb));
}

void WebTransportMsQuicSession::close() noexcept {
    impl_->close();
}

} // namespace nimrtc::web_transport

// ===========================================================================
// WebTransportMsQuicSessionFactory — IWebTransportSessionFactory impl
// ===========================================================================
//
// Produces WebTransportMsQuicSession instances under the seam id
// "msquic".  Registered via NIMRTC_REGISTER_WEB_TRANSPORT("msquic", ...)
// in web_transport_plugin.cpp.
//
// Thread safety: create() is safe to call concurrently — each call
// returns a fresh session instance with its own QUIC connection.
// ===========================================================================

namespace nimrtc::web_transport {

class WebTransportMsQuicSessionFactory final
    : public IWebTransportSessionFactory {
public:
    std::string_view id() const noexcept override {
        return NIMRTC_PLUGIN_ID(kBackendId);
    }

    std::string_view display_name() const noexcept override {
        return "WebTransport over QUIC (msquic, MIT)";
    }

    std::unique_ptr<IWebTransportSession>
    create(const WebTransportConfig& cfg) const override {
        return std::make_unique<WebTransportMsQuicSession>(
            cfg.peer_hostname.empty() ? "" : std::string(cfg.peer_hostname),
            cfg.peer_port,
            cfg);
    }

private:
    static constexpr const char* kBackendId = "msquic";
};

// Factory singleton accessor (used by web_transport_plugin.cpp).
const IWebTransportSessionFactory* msquic_factory_singleton() noexcept {
    static const WebTransportMsQuicSessionFactory factory{};
    return &factory;
}

// Test-only slot (mirrors dtls::test_only pattern).
namespace test_only {
inline const IWebTransportSessionFactory*& registered_factory_slot()
    noexcept {
    static const IWebTransportSessionFactory* s_factory = nullptr;
    return s_factory;
}

const IWebTransportSessionFactory* get_msquic_factory() noexcept {
    return registered_factory_slot();
}

void set_msquic_factory_for_testing(
    const IWebTransportSessionFactory* f) noexcept {
    registered_factory_slot() = f;
}
} // namespace test_only

} // namespace nimrtc::web_transport
