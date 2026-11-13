/**
 * @file nimrtc/web_transport/uni_stream_iface.hpp
 * @brief IUnidirectionalStream — QUIC unidirectional stream seam
 *        (v0.14.0 alpha.2, ADR-014).
 *
 * One `IUnidirectionalStream` is produced by
 * `IWebTransportSession::open_uni_stream()`. The stream is
 * owned by the session; consumers MUST NOT delete it.
 *
 * Unidirectional streams are send-only from the initiator's
 * perspective (RFC 9000 §19.1). The receiver side is on the
 * peer; this interface does not implement a listener API
 * (inbound unidirectional streams are surfaced through a
 * separate listener callback that lands in beta.1 alongside
 * the `IWebTransportListener` extension).
 *
 * ## Method semantics
 *
 *  - `send`  — queue a chunk of bytes for transmission on
 *              the stream. Returns the number of bytes
 *              queued (may be less than the input length if
 *              the per-stream flow-control window is
 *              partially consumed); returns `kErrNotReady`
 *              if the stream is not in the SendReady state,
 *              or `kErrClosed` if the peer has reset the
 *              stream.
 *  - `close` — half-close the stream (send FIN). The stream
 *              is then SendClosed. Idempotent.
 *  - `reset` — abruptly terminate the stream (RESET_STREAM,
 *              RFC 9000 §19.4). The stream transitions to
 *              Closed; the peer observes a STREAM_RESET
 *              error. Idempotent. Safe to call after close().
 *
 * @note P0 — interface added as part of v0.14.0 alpha.2.
 */

#ifndef NIMRTC_WEB_TRANSPORT_UNI_STREAM_IFACE_HPP
#define NIMRTC_WEB_TRANSPORT_UNI_STREAM_IFACE_HPP

// Stability classification: NIMRTC_API_UNSTABLE — see
// web_transport_session_iface.hpp for the rationale.
#include <cstdint>
#include <span>

#include <nimrtc/core/api_version.hpp>  // NIMRTC_API_UNSTABLE
#include <nimrtc/plugins/base.hpp>       // plugins::Status

namespace nimrtc::web_transport {

/**
 * @brief IUnidirectionalStream — pluggable QUIC unidirectional
 *        stream.
 *
 * Returned by `IWebTransportSession::open_uni_stream()`. Stream
 * is owned by the session; do not delete.
 */
class NIMRTC_API_UNSTABLE IUnidirectionalStream {
public:
    virtual ~IUnidirectionalStream() = default;

    /**
     * @brief Queue a chunk of bytes for transmission.
     *
     * @return Number of bytes queued on success (0 or positive);
     *         `kErrNotReady` if the stream is not in the
     *         SendReady state;
     *         `kErrClosed` if the peer has reset the stream.
     */
    virtual plugins::Status send(
        std::span<const std::uint8_t> bytes) noexcept = 0;

    /**
     * @brief Half-close the stream (send FIN).
     *
     * The stream is then SendClosed. Idempotent.
     */
    virtual void close() noexcept = 0;

    /**
     * @brief Abruptly terminate the stream
     *        (RESET_STREAM, RFC 9000 §19.4).
     *
     * The stream transitions to Closed; the peer observes a
     * STREAM_RESET error. Idempotent. Safe to call after
     * close().
     */
    virtual void reset() noexcept = 0;
};

} // namespace nimrtc::web_transport

#endif // NIMRTC_WEB_TRANSPORT_UNI_STREAM_IFACE_HPP
