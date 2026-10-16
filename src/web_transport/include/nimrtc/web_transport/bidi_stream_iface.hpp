/**
 * @file nimrtc/web_transport/bidi_stream_iface.hpp
 * @brief IBidirectionalStream — QUIC bidirectional stream seam
 *        (v0.14.0 alpha.2, ADR-014).
 *
 * One `IBidirectionalStream` is produced by
 * `IWebTransportSession::open_bidi_stream()`. The stream is
 * owned by the session; consumers MUST NOT delete it.
 *
 * ## Thread model
 *
 * Streams are not thread-safe by themselves. The engine
 * serializes send / recv / close / reset on the engine thread
 * via the WebTransport plugin adapter layer.
 *
 * ## Method semantics
 *
 *  - `send`         — queue a chunk of bytes for transmission
 *                     on the stream. Returns the number of
 *                     bytes queued (may be less than the input
 *                     length if the per-stream flow-control
 *                     window is partially consumed); returns
 *                     `kErrNotReady` if the stream is not in
 *                     the SendReady state, or `kErrClosed` if
 *                     the peer has reset the stream.
 *  - `recv`         — pull the next received chunk off the
 *                     stream's inbound queue. Returns the
 *                     number of bytes copied into `out`
 *                     (possibly 0 if no data is available);
 *                     returns `kErrNotReady` if the stream is
 *                     not in the RecvReady state, or
 *                     `kErrClosed` on FIN.
 *  - `set_on_data`  — install the inbound-data callback. The
 *                     callback fires when new received data
 *                     is available. Empty std::function =
 *                     unregister. Not thread-safe; call only
 *                     from the engine thread.
 *  - `close`        — half-close the stream (send FIN). The
 *                     stream is then SendClosed; recv may
 *                     continue until the peer sends FIN.
 *                     Idempotent.
 *  - `reset`        — abruptly terminate the stream (RFC 9000
 *                     §19.4 — STOP_SENDING + RESET_STREAM).
 *                     The stream transitions to Closed; the
 *                     peer observes a STREAM_RESET error.
 *                     Idempotent. Safe to call after close().
 *
 * @note P0 — interface added as part of v0.14.0 alpha.2.
 */

#ifndef NIMRTC_WEB_TRANSPORT_BIDI_STREAM_IFACE_HPP
#define NIMRTC_WEB_TRANSPORT_BIDI_STREAM_IFACE_HPP

// Stability classification: NIMRTC_API_UNSTABLE — see
// web_transport_session_iface.hpp for the rationale.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

#include <nimrtc/core/api_version.hpp>  // NIMRTC_API_UNSTABLE
#include <nimrtc/plugins/base.hpp>       // plugins::Status

namespace nimrtc::web_transport {

/**
 * @brief One-shot inbound data callback.
 *
 * Fired when new data is available on the stream's inbound
 * side. The bytes span is non-owning; the data is valid only
 * for the duration of the call. Consumers that need to retain
 * the bytes must copy them.
 *
 * Pass an empty std::function to unregister. Not thread-safe;
 * call only from the engine thread.
 */
using OnStreamDataCb =
    std::function<void(std::span<const std::uint8_t> bytes)>;

} // namespace nimrtc::web_transport

namespace nimrtc::web_transport {

/**
 * @brief IBidirectionalStream — pluggable QUIC bidirectional
 *        stream.
 *
 * Returned by `IWebTransportSession::open_bidi_stream()`. Stream
 * is owned by the session; do not delete.
 */
class NIMRTC_API_UNSTABLE IBidirectionalStream {
public:
    virtual ~IBidirectionalStream() = default;

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
     * @brief Pull the next received chunk off the inbound queue.
     *
     * Non-blocking. Returns 0 if no data is available; copies up
     * to `out.size()` bytes into `out`.
     *
     * @return Number of bytes copied on success;
     *         `kErrNotReady` if the stream is not in the
     *         RecvReady state;
     *         `kErrClosed` on FIN or peer reset.
     */
    virtual plugins::Status recv(
        std::span<std::uint8_t> out) noexcept = 0;

    /**
     * @brief Register the inbound-data callback.
     *
     * The callback fires when new received data is available.
     * Pass an empty std::function to unregister. Not thread-safe;
     * call only from the engine thread.
     */
    virtual void set_on_data(OnStreamDataCb cb) noexcept = 0;

    /**
     * @brief Half-close the stream (send FIN).
     *
     * The stream is then SendClosed; recv may continue until the
     * peer sends FIN. Idempotent.
     */
    virtual void close() noexcept = 0;

    /**
     * @brief Abruptly terminate the stream
     *        (STOP_SENDING + RESET_STREAM, RFC 9000 §19.4).
     *
     * The stream transitions to Closed; the peer observes a
     * STREAM_RESET error. Idempotent. Safe to call after
     * close().
     */
    virtual void reset() noexcept = 0;
};

} // namespace nimrtc::web_transport

#endif // NIMRTC_WEB_TRANSPORT_BIDI_STREAM_IFACE_HPP
