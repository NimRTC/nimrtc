/**
 * @file nimrtc/web_transport/web_transport_data_channel.hpp
 * @brief WebTransportDataChannel — IDataChannel backed by IWebTransportSession.
 *
 * v0.14.0-beta.1 (ADR-014 / DC-1): implements `plugins::IDataChannel`
 * on top of `IWebTransportSession` (QUIC / msquic).  This replaces
 * the SCTP-over-DTLS `SctpDataChannel` as the production DataChannel
 * backend for Chrome interop (Case E — RTCDataChannel exchange over
 * WebTransport / QUIC).
 *
 * ## DataChannel → WebTransport semantic mapping
 *
 * | DataChannelReliability      | WebTransport primitive            |
 * |----------------------------|-----------------------------------|
 * | kReliableOrdered           | `open_bidi_stream()` (reliable, ordered) |
 * | kUnreliable                | `send_datagram()` (unreliable, unordered) |
 * | kPartialReliableTTL        | bidi stream + app-level TTL reset  |
 * | kPartialReliableCount      | bidi stream + app-level count reset |
 *
 * ## Thread model
 *
 * Same as `SctpDataChannel` / `plugins::IDataChannel`:
 *   - `set_on_message` / `set_on_state` are called from the engine
 *     thread before `open()`.
 *   - `send()` is thread-safe: queues into a lock-free outbound queue;
 *     the stream send callback drains it.
 *   - msquic callbacks fire on msquic's internal worker threads; we
 *     dispatch to the engine thread via the session's recv queue so
 *     no additional locking is needed at the IDataChannel surface level.
 *
 * ## Lifecycle
 *
 * A channel is created by `WebTransportDataChannelFactory::create()`
 * and lives until `close()`.  The channel owns exactly one bidirectional
 * stream for the `kReliableOrdered` and partial-reliability modes; the
 * stream is opened lazily on first `send()` or explicitly on `open()`.
 *
 * For `kUnreliable` (datagram), no stream is needed — `send_datagram()`
 * on the session is used directly.
 *
 * @note P0 — added as part of v0.14.0-beta.1 (DC-1 land).
 */

#ifndef NIMRTC_WEB_TRANSPORT_WEB_TRANSPORT_DATA_CHANNEL_HPP
#define NIMRTC_WEB_TRANSPORT_WEB_TRANSPORT_DATA_CHANNEL_HPP

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/datachannel.hpp>
#include <nimrtc/web_transport/web_transport_config.hpp>
#include <nimrtc/web_transport/web_transport_session_iface.hpp>
#include <nimrtc/web_transport/bidi_stream_iface.hpp>

namespace nimrtc::web_transport {

// Bring Status constants from plugins/base.hpp into scope so the
// unqualified `Status` / `kOk` / `kErrNotReady` / `kErrInternal`
// declarations in this header parse correctly.
using plugins::Status;
using plugins::kOk;
using plugins::kErrNotReady;
using plugins::kErrInternal;

// ---------------------------------------------------------------------------
// Guard against POSIX / Windows SDK `open` macro pollution.
// ---------------------------------------------------------------------------
#ifdef _MSC_VER
#pragma push_macro("open")
#pragma push_macro("close")
#undef open
#undef close
#endif

/**
 * @brief WebTransport-backed implementation of `plugins::IDataChannel`.
 *
 * Wraps an `IWebTransportSession` and dispatches:
 *   - `send()` → `send_datagram()` (kUnreliable) or
 *     bidirectional stream send (kReliableOrdered / PR modes)
 *   - inbound stream data → `set_on_message` callback
 *
 * The session is a non-owning pointer passed at construction time
 * (injected by `WebTransportDataChannelFactory`).
 */
class WebTransportDataChannel final : public plugins::IDataChannel {
public:
    /**
     * @brief Construct a channel backed by an existing WebTransport session.
     *
     * @param session  Non-owning pointer to the session.  Must remain
     *                valid for the lifetime of this channel.  The caller
     *                is responsible for ensuring the session is connected
     *                before `send()` is called.
     */
    explicit WebTransportDataChannel(IWebTransportSession* session) noexcept;
    ~WebTransportDataChannel() override;

    // Non-copyable, non-movable.
    WebTransportDataChannel(const WebTransportDataChannel&)            = delete;
    WebTransportDataChannel& operator=(const WebTransportDataChannel&) = delete;
    WebTransportDataChannel(WebTransportDataChannel&&)                 = delete;
    WebTransportDataChannel& operator=(WebTransportDataChannel&&)        = delete;

    // ---- IPlugin ---------------------------------------------------------

    /** Returns `"nimrtc::web_transport::WebTransportDataChannel"`. */
    const char* name() const noexcept override;

    /** No-op: the session transport handshake is handled externally
     *  (by the engine's WebTransport session owner).  Returns kOk. */
    Status open() noexcept override;

    /** Close the stream and clear all callbacks.  Idempotent. */
    void close() noexcept override;

    // ---- IDataChannel ----------------------------------------------------

    /** Cache the channel configuration.  For kUnreliable, store the
     *  label (not yet used for WebTransport but reserved for future
     *  ALPN / stream-type routing).  For reliable/partial modes,
     *  the stream will be opened lazily on first `send()` or explicitly
     *  when the session is in kConnected state. */
    void open(const plugins::DataChannelConfig& cfg) override;

    /** Send `data` over the appropriate WebTransport primitive based
     *  on `cfg_.reliability`. */
    void send(plugins::BufferView data) override;

    /** Replace the message callback. */
    void set_on_message(plugins::DataMessageCallback cb) noexcept override;

    /** Replace the state callback. */
    void set_on_state(plugins::DataChannelStateCallback cb) noexcept override;

    // ---- Test-only -------------------------------------------------------

    /** Returns true if the channel has an open stream or has ever sent
     *  a datagram. */
    bool is_open() const noexcept { return opened_; }

    /** Returns the cached config (read-only). Useful in tests. */
    [[nodiscard]] const plugins::DataChannelConfig& config() const noexcept {
        return cfg_;
    }

private:
    // Forward declaration of the stream recv helper.
    class StreamWrapper;

    /** Open a bidirectional stream (lazy, on first send for
     *  reliable/partial modes). */
    bool ensure_stream_open() noexcept;

    /** Callback invoked by the bidirectional stream when data arrives. */
    void on_stream_data(plugins::BufferView data) noexcept;

    /** Callback invoked when the stream is closed or reset. */
    void on_stream_closed() noexcept;

    /** Map DataChannelReliability to a human-readable state string. */
    static const char* state_from_reliability(plugins::DataChannelReliability r,
                                               bool connected) noexcept;

    IWebTransportSession*               session_;
    plugins::DataChannelConfig         cfg_;
    plugins::DataMessageCallback       on_msg_;
    plugins::DataChannelStateCallback  on_state_;
    // Non-owning raw pointer to the bidi stream — the session
    // (or the test harness) owns the stream object.  The engine
    // owns the session, so lifetime is safe.
    IBidirectionalStream*              stream_raw_ = nullptr;
    std::atomic<bool>                  opened_{false};
    std::atomic<bool>                  stream_ready_{false};
    std::chrono::steady_clock::time_point pr_ttl_deadline_{};
    std::uint16_t                      pr_count_remaining_{0};
};

// ---------------------------------------------------------------------------
// StreamWrapper — inline recv handler for the bidi stream.
// ---------------------------------------------------------------------------

/**
 * @brief Inline stream recv handler.
 *
 * Lives inside the translation unit so it can capture the
 * `WebTransportDataChannel*` without exposing it in the header.
 * Registered as the stream's `set_on_data` callback on stream open.
 */
class WebTransportDataChannel::StreamWrapper {
public:
    explicit StreamWrapper(WebTransportDataChannel* chan) noexcept
        : chan_(chan) {}

    void operator()(plugins::BufferView data) noexcept {
        if (chan_) chan_->on_stream_data(data);
    }

    void on_closed() noexcept {
        if (chan_) chan_->on_stream_closed();
    }

private:
    WebTransportDataChannel* chan_;
};

#ifdef _MSC_VER
#pragma pop_macro("open")
#pragma pop_macro("close")
#endif

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/**
 * @brief Factory producing `WebTransportDataChannel` instances.
 *
 * Registered under id `"web_transport"` (matching
 * `EngineConfig::datachannel_backend = "web_transport"`).
 *
 * The factory itself is a thin shell — it requires a session to
 * be injected at channel construction time.  The engine owns the
 * session and creates channels via `create(IWebTransportSession*)`.
 */
class WebTransportDataChannelFactory final
    : public plugins::IDataChannelFactory {
public:
    WebTransportDataChannelFactory() = default;
    ~WebTransportDataChannelFactory() override = default;

    WebTransportDataChannelFactory(const WebTransportDataChannelFactory&)            = delete;
    WebTransportDataChannelFactory& operator=(const WebTransportDataChannelFactory&) = delete;

    /** Registration id: `"web_transport"`. */
    std::string_view id() const noexcept override { return "web_transport"; }

    /** Short human-readable name. */
    std::string_view display_name() const noexcept override {
        return "WebTransport-backed DataChannel (QUIC / msquic, RFC 9221)";
    }

    /** Create a channel without a session (stub — use `create(IWebTransportSession*)`
     *  for production).  Returns a channel in kErrNotReady state. */
    plugins::IDataChannel* create() const override;

    /** Create a channel backed by an existing session. */
    WebTransportDataChannel* create(IWebTransportSession* session) const noexcept {
        return new WebTransportDataChannel(session);
    }
};

// ---------------------------------------------------------------------------
// Public registration entry point
// ---------------------------------------------------------------------------

/**
 * @brief Register both WebTransport factories with `core::PluginRegistry`.
 *
 * Idempotent — first call registers; subsequent calls are no-ops.
 * Thread-safe per C++11 static-local semantics.
 *
 * Registers:
 *   - WebTransportMsQuicSessionFactory (id="msquic") under the
 *     `register_web_transport` slot.
 *   - WebTransportDataChannelFactory (id="web_transport") under the
 *     `register_datachannel` slot.
 *
 * Implemented in `web_transport_plugin.cpp`.
 */

} // namespace nimrtc::web_transport

#endif // NIMRTC_WEB_TRANSPORT_WEB_TRANSPORT_DATA_CHANNEL_HPP
