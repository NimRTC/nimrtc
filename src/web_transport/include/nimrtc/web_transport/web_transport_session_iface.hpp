/**
 * @file nimrtc/web_transport/web_transport_session_iface.hpp
 * @brief IWebTransportSession — WebTransport / QUIC session seam
 *        (v0.14.0 alpha.2, ADR-014 + v1.0-plan.md §10.4).
 *
 * WebTransport is the v1.0.0 data-plane re-design per
 * `docs/plan/v0.11-plan.md` §2.2 + §10. SCTP-over-DTLS is
 * deprecated by the Chrome team in favour of WebTransport / QUIC;
 * v0.14.0 cuts the seam here and the msquic-backed implementation
 * in v0.14.0-beta.1.
 *
 * ## Thread model
 *
 * IWebTransportSession is NOT thread-safe by itself. The engine
 * serializes WebTransport calls on the engine thread; the msquic
 * implementation is driven from there via the surface below.
 *
 * ## Method semantics
 *
 *  - `state`                       — current connection state; one
 *                                   of `WebTransportState` values
 *                                   (kConnecting / kConnected /
 *                                   kClosing / kClosed / kFailed).
 *  - `open_bidi_stream`            — open a new client-initiated
 *                                   bidirectional QUIC stream;
 *                                   returns nullptr on a state
 *                                   other than kConnected.
 *  - `open_uni_stream`             — open a new client-initiated
 *                                   unidirectional QUIC stream;
 *                                   returns nullptr on a state
 *                                   other than kConnected.
 *  - `send_datagram`               — send a single connection-level
 *                                   datagram (RFC 9221 §3); returns
 *                                   the byte count or a non-zero
 *                                   plugins::Status on failure.
 *  - `set_on_datagram`             — install the inbound datagram
 *                                   callback. The callback fires
 *                                   once per datagram; the engine
 *                                   may install a different one
 *                                   later. Empty std::function =
 *                                   unregister.
 *  - `close`                       — close the session; safe to
 *                                   call multiple times. After
 *                                   close, all streams opened
 *                                   through this session are
 *                                   invalidated (their methods
 *                                   become no-ops returning
 *                                   kErrNotReady).
 *
 * ## Deviations from v1.0-plan.md §10.4 (documented in the PR report)
 *
 *  - §10.4 lists a `cancel_stream` / `reset_stream` method. The
 *    RFC 9221 reset semantics are surfaced on the per-stream
 *    interface (`IBidirectionalStream::reset`,
 *    `IUnidirectionalStream::reset`) so the session surface stays
 *    connection-level only. Per-stream reset is documented in
 *    `bidi_stream_iface.hpp` / `uni_stream_iface.hpp`.
 *  - §10.4 lists a `set_on_state_change` callback. We expose the
 *    state via the polling accessor `state()` only — the engine
 *    polls from its tick loop at ~50 ms cadence, the same
 *    cadence it uses for DTLS `pump()`. The callback surface is
 *    reserved for a v1.0.0 final follow-up if a v1.x use case
 *    needs push semantics.
 *
 * @note P0 — interface added as part of v0.14.0 alpha.2
 *       (the v1.0.0-alpha.2 pre-release).
 */

#ifndef NIMRTC_WEB_TRANSPORT_SESSION_IFACE_HPP
#define NIMRTC_WEB_TRANSPORT_SESSION_IFACE_HPP

// Stability classification: NIMRTC_API_UNSTABLE — the seam is
// pure-virtual with no impl until v0.14.0-beta.1; the impl lands
// alongside the msquic integration. The "v1.0.0 stable" banner
// does not flip until v1.0.0 final; the seam is explicitly
// UNSTABLE through v1.0.0-beta.1.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

#include <nimrtc/core/api_version.hpp>  // NIMRTC_API_UNSTABLE
#include <nimrtc/plugins/base.hpp>       // plugins::Status

// Forward declarations — the companion types live in sibling
// headers (bidi_stream_iface.hpp / uni_stream_iface.hpp /
// web_transport_state.hpp). Including them here would force every
// consumer of the session seam to also pull in the stream headers
// even when they only use the datagram surface; the forward
// declarations keep the session header minimal.
namespace nimrtc::web_transport {

class NIMRTC_API_UNSTABLE IBidirectionalStream;
class NIMRTC_API_UNSTABLE IUnidirectionalStream;
enum class NIMRTC_API_UNSTABLE WebTransportState : std::uint8_t;

/**
 * @brief One-shot inbound datagram callback.
 *
 * Fired once per datagram received on the WebTransport session
 * (RFC 9221 §3). The bytes span is non-owning; the data is valid
 * only for the duration of the call. Consumers that need to
 * retain the bytes must copy them.
 *
 * Pass an empty std::function to unregister. Not thread-safe; call
 * only from the engine thread.
 */
using NIMRTC_API_UNSTABLE OnDatagramCb =
    std::function<void(std::span<const std::uint8_t> bytes)>;

} // namespace nimrtc::web_transport

namespace nimrtc::web_transport {

/**
 * @brief IWebTransportSession — pluggable WebTransport / QUIC session.
 *
 * One IWebTransportSession per peer connection. Created by
 * `IWebTransportSessionFactory::create()` (lands in beta.1
 * alongside the msquic integration).
 *
 * @see v1.0-plan.md §10.4 (the IWebTransportSession seam contract);
 *      v0.14-plan.md §2.1.1 WT-SEAM (the alpha.2 scope).
 */
class NIMRTC_API_UNSTABLE IWebTransportSession {
public:
    virtual ~IWebTransportSession() = default;

    // -------------------------------------------------------------------
    // Connection state
    // -------------------------------------------------------------------

    /**
     * @brief Current connection state.
     *
     * The state transitions are:
     *   kConnecting → kConnected (handshake complete)
     *   kConnecting → kFailed     (handshake failed)
     *   kConnected  → kClosing    (close() called)
     *   kClosing    → kClosed     (close finished)
     *   any state   → kFailed     (fatal error)
     *
     * The engine polls this from its tick loop at ~50 ms cadence,
     * the same cadence it uses for `IDtlsSession::pump()`.  No
     * callback is fired; the engine is responsible for the
     * state-diff loop.
     */
    virtual WebTransportState state() const noexcept = 0;

    // -------------------------------------------------------------------
    // Stream open
    // -------------------------------------------------------------------

    /**
     * @brief Open a new client-initiated bidirectional QUIC stream.
     *
     * Returns a non-null `IBidirectionalStream*` on success, or
     * `nullptr` if the session is not in `kConnected` state, or if
     * the underlying msquic configuration has reached its
     * `initial_max_bidi_streams` limit (RFC 9000 §18.2).
     *
     * The returned stream is owned by the session; consumers MUST
     * NOT delete it. Stream lifetime ends when the stream is
     * `close()`-d or `reset()`-d.
     */
    virtual IBidirectionalStream* open_bidi_stream() noexcept = 0;

    /**
     * @brief Open a new client-initiated unidirectional QUIC stream.
     *
     * Returns a non-null `IUnidirectionalStream*` on success, or
     * `nullptr` if the session is not in `kConnected` state, or if
     * the underlying msquic configuration has reached its
     * `initial_max_uni_streams` limit (RFC 9000 §18.2).
     *
     * The returned stream is owned by the session; consumers MUST
     * NOT delete it. Stream lifetime ends when the stream is
     * `close()`-d or `reset()`-d.
     */
    virtual IUnidirectionalStream* open_uni_stream() noexcept = 0;

    // -------------------------------------------------------------------
    // Datagram surface
    // -------------------------------------------------------------------

    /**
     * @brief Send a single connection-level datagram.
     *
     * RFC 9221 §3 semantics: the bytes are framed as a single
     * QUIC DATAGRAM frame and sent on the connection. The
     * underlying QUIC implementation may drop the datagram if it
     * exceeds the connection's `max_datagram_frame_size` (RFC 9221
     * §3.1) or if the peer has not advertised DATAGRAM support.
     *
     * @param bytes  The payload. Empty span = no-op returning 0.
     * @return Number of bytes queued on success (positive);
     *         `kErrNotReady` if state() != kConnected;
     *         `kErrBufferTooSmall` if bytes exceed
     *         `max_datagram_frame_size`;
     *         `kErrInternal` on a transient transport error.
     */
    virtual plugins::Status send_datagram(
        std::span<const std::uint8_t> bytes) noexcept = 0;

    /**
     * @brief Register the inbound datagram callback.
     *
     * The callback fires once per received datagram. Pass an
     * empty std::function to unregister. Not thread-safe; call
     * only from the engine thread.
     */
    virtual void set_on_datagram(OnDatagramCb cb) noexcept = 0;

    // -------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------

    /**
     * @brief Close the session and all streams opened through it.
     *
     * Idempotent. After close, state() transitions to kClosing
     * and eventually kClosed; all streams opened through this
     * session become invalid (their methods are no-ops
     * returning kErrNotReady).
     */
    virtual void close() noexcept = 0;
};

} // namespace nimrtc::web_transport

#endif // NIMRTC_WEB_TRANSPORT_SESSION_IFACE_HPP
