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
 * v1.2.0 (DataChannel capability plan, WP-B / WP-C / WP-D / WP-E):
 * the channel gained the five capabilities the v1.1.0 gap table
 * listed as missing:
 *
 *   - **WP-B — message boundary.**  In `kFramed` mode every payload
 *     is wrapped in a length-prefixed frame, so one `send()` is one
 *     inbound message.  `kRaw` keeps the byte-stream behaviour (what
 *     Chrome's `RTCDataChannel`-over-WebTransport speaks).
 *   - **WP-C — multi-channel isolation.**  The channel no longer
 *     *replaces* the session's single inbound callback slot: it
 *     subscribes to the session's multicast inbound routes and removes
 *     only its own subscription on close.  In framed mode the frame's
 *     channel label decides which channel a payload belongs to.
 *   - **WP-D — partial reliability.**  The TTL and the retransmit
 *     count are *per message*, not per channel.  An expired message is
 *     dropped; the channel stays open.
 *   - **WP-E — priority.**  The channel registers an outbound priority
 *     source with the session, so the data plane orders its own
 *     pending sends (lower `priority` value first).
 *
 * ## DataChannel → WebTransport semantic mapping
 *
 * | DataChannelReliability      | WebTransport primitive            |
 * |----------------------------|-----------------------------------|
 * | kReliableOrdered           | `open_bidi_stream()` (reliable, ordered) |
 * | kUnreliable                | `send_datagram()` (unreliable, unordered) |
 * | kPartialReliableTTL        | bidi stream + per-message TTL     |
 * | kPartialReliableCount      | bidi stream + per-message retry budget |
 *
 * ## Thread model
 *
 * Same as `SctpDataChannel` / `plugins::IDataChannel`:
 *   - `set_on_message` / `set_on_state` are called from the engine
 *     thread before `open()`.
 *   - `send()` is thread-safe: it appends to the outbound queue; the
 *     queue is drained by the session's priority-ordered flush.
 *   - msquic callbacks fire on msquic's internal worker threads; we
 *     dispatch to the engine thread via the session's recv queue so
 *     no additional locking is needed at the IDataChannel surface level.
 *
 * ## Lifecycle
 *
 * A channel is created by `WebTransportDataChannelFactory::create()`
 * and lives until `close()`.  The channel owns exactly one outbound
 * bidirectional stream for the `kReliableOrdered` and partial-reliability
 * modes; the stream is opened lazily on first `send()` or explicitly on
 * `open()`.
 *
 * For `kUnreliable` (datagram), no stream is needed — `send_datagram()`
 * on the session is used directly.
 *
 * @note P0 — added as part of v0.14.0-beta.1 (DC-1 land).
 */

#ifndef NIMRTC_WEB_TRANSPORT_WEB_TRANSPORT_DATA_CHANNEL_HPP
#define NIMRTC_WEB_TRANSPORT_WEB_TRANSPORT_DATA_CHANNEL_HPP

// Stability classification: NIMRTC_API_UNSTABLE — the
// `WebTransportDataChannel` factory lands with v0.14.0-beta.1
// (DC-1 / ADR-014); the IDataChannel-on-WebTransport mapping is
// not stable until the v0.14.0 release line freezes. See
// `web_transport_session_iface.hpp` for the broader WebTransport
// seam stability rationale.
#include <nimrtc/core/api_version.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/datachannel.hpp>
#include <nimrtc/web_transport/bidi_stream_iface.hpp>
#include <nimrtc/web_transport/web_transport_config.hpp>
#include <nimrtc/web_transport/web_transport_session_iface.hpp>

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
 *   - inbound stream data → `set_on_message` / `set_on_message_ex`
 *
 * The session is a non-owning pointer passed at construction time
 * (injected by `WebTransportDataChannelFactory`).
 */
class NIMRTC_API_UNSTABLE WebTransportDataChannel final : public plugins::IDataChannel {
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

    /** Close the stream, drop the queued messages and remove this
     *  channel's session subscriptions.  Idempotent. */
    void close() noexcept override;

    // ---- IDataChannel ----------------------------------------------------

    /** Cache the channel configuration (label, reliability, framing,
     *  priority) and (re)wire the session routes for the configured
     *  framing mode. */
    void open(const plugins::DataChannelConfig& cfg) override;

    /** Queue `data` for transmission.  The queue is drained by the
     *  session's priority-ordered flush, so the send order across the
     *  channels of one session follows their `priority`. */
    void send(plugins::BufferView data) override;

    /** Replace the message callback. */
    void set_on_message(plugins::DataMessageCallback cb) noexcept override;

    /** Replace the identity-carrying message callback (v1.2.0, WP-C1). */
    void set_on_message_ex(plugins::DataChannelMessageCallback cb) noexcept override;

    /** Replace the state callback. */
    void set_on_state(plugins::DataChannelStateCallback cb) noexcept override;

    /** Wire this channel's inbound routes on the session.  Safe to
     *  call before `open(cfg)` — the wiring is (re)done by `open(cfg)`
     *  once the framing mode is known.  `close()` removes it. */
    void attach_session_callbacks() noexcept;

    // ---- Test-only -------------------------------------------------------

    /** Returns true if the channel has an open stream or has ever sent
     *  a datagram. */
    bool is_open() const noexcept { return opened_.load(std::memory_order_acquire); }

    /** Returns the cached config (read-only). Useful in tests. */
    [[nodiscard]] const plugins::DataChannelConfig& config() const noexcept {
        return cfg_;
    }

    /** Number of messages handed to the transport (v1.2.0). */
    [[nodiscard]] std::uint64_t sent_messages() const noexcept;

    /** Number of messages dropped by a partial-reliability policy
     *  (v1.2.0, WP-D). */
    [[nodiscard]] std::uint64_t dropped_messages() const noexcept;

    /** Number of messages still waiting in the outbound queue. */
    [[nodiscard]] std::size_t pending_sends() const noexcept;

private:
    /** Opaque per-channel state (outbound queue, inbound frame readers).
     *  Defined in the .cpp so the public header does not have to expose
     *  the module-internal frame codec. */
    struct State;

    /** Which inbound surface the channel currently uses. */
    enum class Routes : std::uint8_t {
        kNone,    // not wired
        kLegacy,  // raw mode: the session's single callback slots
        kMulticast// framed mode: the session's multicast subscriptions
    };

    /** (Re)wire the session routes for the current configuration. */
    void apply_session_routes() noexcept;

    /** Remove this channel's session routes (WP-C4). */
    void remove_session_routes() noexcept;

    /** Open a bidirectional stream (lazy, on first send for
     *  reliable/partial modes). */
    bool ensure_stream_open() noexcept;

    /** Offer one wire buffer to the transport.  Returns false when the
     *  transport refused it (the message stays queued). */
    bool offer_to_transport(const std::vector<std::uint8_t>& bytes) noexcept;

    /** Drain the outbound queue (called by the session's priority
     *  flush, and directly when the session has no priority surface). */
    void flush_outbound() noexcept;

    /** Inbound data path for the adopted peer stream: strips the
     *  WebTransport stream-type header, then (framed mode) decodes the
     *  frames and delivers the ones addressed to this channel. */
    void on_peer_stream_data(IBidirectionalStream* stream,
                             plugins::BufferView data) noexcept;

    /** Callback invoked for a peer-initiated bidi stream. */
    void on_peer_stream(IBidirectionalStream* stream) noexcept;

    /** Callback invoked for an inbound session datagram. */
    void on_session_datagram(std::span<const std::uint8_t> bytes) noexcept;

    /** Deliver one inbound payload to the user callbacks. */
    void deliver(std::string_view label,
                 std::span<const std::uint8_t> payload) noexcept;

    /** Report a protocol error: the channel transitions to `error`. */
    void fail_protocol(const char* reason) noexcept;

    IWebTransportSession*               session_;
    plugins::DataChannelConfig         cfg_;
    plugins::DataMessageCallback       on_msg_;
    plugins::DataChannelMessageCallback on_msg_ex_;
    plugins::DataChannelStateCallback  on_state_;
    // Non-owning raw pointer to the outbound bidi stream — the session
    // (or the test harness) owns the stream object.  The engine
    // owns the session, so lifetime is safe.
    IBidirectionalStream*              stream_raw_ = nullptr;
    // Non-owning pointer to the peer-initiated stream adopted in raw
    // mode (the legacy single-slot path).
    IBidirectionalStream*              peer_stream_raw_ = nullptr;
    // Multicast subscription tokens (v1.2.0, WP-C2).
    SubscriptionId                     dgram_sub_ = 0;
    SubscriptionId                     peer_sub_  = 0;
    SubscriptionId                     prio_sub_  = 0;
    Routes                             routes_    = Routes::kNone;
    std::atomic<bool>                  opened_{false};
    // Latched by `close()`.  The inbound path is guarded by this flag
    // (not by `opened_`) because the session routes are wired by
    // `open(cfg)`, which the engine calls *before* `open()`: a peer
    // stream or a datagram that arrives in that window must be
    // adopted, not dropped.
    std::atomic<bool>                  closed_{false};
    std::atomic<bool>                  stream_ready_{false};
    // Guards the outbound queue and the counters (send() is
    // thread-safe, the flush runs on the engine thread).
    mutable std::mutex                 tx_mutex_;
    std::unique_ptr<State>             state_;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/**
 * @brief Factory producing `WebTransportDataChannel` instances.
 *
 * Registered under id `"web_transport"` (matching
 * `EngineConfig::datachannel_backend = "web_transport"`).
 *
 * Two creation paths:
 *   - `create(IWebTransportSession*)` — production path.  The engine
 *     owns the session and binds it at construction time.
 *   - `create()` (no session) — negotiation probe.  The engine calls
 *     this once at `init_datachannel_registry()` time to verify the
 *     factory is registered; the returned channel stays in
 *     `kErrNotReady` until bound to a live session.  Not for data use.
 */
class NIMRTC_API_UNSTABLE WebTransportDataChannelFactory final
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

    /** Create an unbound negotiation probe (no session).
     *  Returns a channel in kErrNotReady state; bind a live session via
     *  `create(IWebTransportSession*)` for production use. */
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

#ifdef _MSC_VER
#pragma pop_macro("open")
#pragma pop_macro("close")
#endif

#endif // NIMRTC_WEB_TRANSPORT_WEB_TRANSPORT_DATA_CHANNEL_HPP
