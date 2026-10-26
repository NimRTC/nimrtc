/**
 * @file src/web_transport/src/web_transport_msquic_send.hpp
 * @brief Lifetime helper for outbound QUIC payloads (msquic seam).
 *
 * NOT part of the public API.
 *
 * ## Why this exists
 *
 * msquic does **not** copy the application buffer when the send API is
 * called.  `MsQuicStreamSend()` only records the `QUIC_BUFFER` list; the
 * bytes are read from it later, when the data is flushed into a packet
 * (see `QuicStreamSendBufferRequest()` in msquic's core/stream_send.c,
 * which memcpy's the application buffer into msquic's own send buffer).
 *
 * msquic's own documentation is explicit about the scope of that
 * requirement (docs/api/StreamSend.md):
 *
 *   "The buffers (both the `QUIC_BUFFER`s and the memory they reference)
 *    are 'owned' by MsQuic (and must not be modified by the app) until
 *    MsQuic indicates the `QUIC_STREAM_EVENT_SEND_COMPLETE` event for
 *    the send."
 *
 * So **two** things have to outlive the send call:
 *
 *   1. the `QUIC_BUFFER` descriptor array itself — msquic keeps the
 *      pointer the app passed in (`SendRequest->Buffers = Buffers`),
 *      so a descriptor that lives on the stack is read from freed
 *      stack memory; and
 *   2. the memory the descriptors point at.
 *
 * Passing a pointer to a local `QUIC_BUFFER` (the natural way to write
 * this code) therefore crashes inside msquic's worker thread, typically
 * as a `memcpy` inside `QuicStreamSendBufferRequest`.
 *
 * The same applies to `MsQuicDatagramSend()`: the datagram payload is
 * encoded into a packet later, so the buffer must stay alive until the
 * `QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED` event reports a
 * *final* state for that datagram (acknowledged / discarded as lost /
 * cancelled).  Note that msquic reports intermediate states as well
 * (`QUIC_DATAGRAM_SEND_SENT`, `QUIC_DATAGRAM_SEND_LOST_SUSPECT`) with
 * the very same client context — releasing the payload on those would
 * free it twice.
 *
 * ## How it is used
 *
 * `OutboundPayload` owns both the bytes *and* the `QUIC_BUFFER`
 * descriptor that points at them.  The object is heap allocated and
 * handed to msquic as the `ClientSendContext` of the send request;
 * msquic hands the very same pointer back in the completion event,
 * where it is released.
 *
 *   auto* payload = new OutboundPayload(bytes);
 *   QUIC_STATUS rc = api->StreamSend(stream, payload->buffers(), 1,
 *                                    flags, payload);
 *   if (QUIC_FAILED(rc)) { release_outbound_payload(payload); }
 *
 *   // ... later, in the stream callback:
 *   case QUIC_STREAM_EVENT_SEND_COMPLETE:
 *       release_outbound_payload(Event->SEND_COMPLETE.ClientContext);
 *
 *   // ... later, in the connection callback:
 *   case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
 *       if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(
 *               Event->DATAGRAM_SEND_STATE_CHANGED.State)) {
 *           release_outbound_payload(
 *               Event->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
 *       }
 */

#ifndef NIMRTC_WEB_TRANSPORT_MSQUIC_SEND_HPP
#define NIMRTC_WEB_TRANSPORT_MSQUIC_SEND_HPP

// Must come before any msquic header so QUIC_API_TABLE is defined.
extern "C" {
#include <msquic.h>
}

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nimrtc::web_transport::detail {

/**
 * @brief Heap-owned copy of an outbound QUIC payload.
 *
 * Owns both the bytes and the `QUIC_BUFFER` descriptor that points at
 * them, because msquic keeps the descriptor array pointer for as long
 * as the send is outstanding (see the file comment).
 */
class OutboundPayload {
public:
    /** Copy the given bytes into the payload. */
    explicit OutboundPayload(std::span<const std::uint8_t> bytes) {
        data_.assign(bytes.begin(), bytes.end());
        init_buffer();
    }

    /** Copy the given raw bytes into the payload. */
    OutboundPayload(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        data_.assign(p, p + size);
        init_buffer();
    }

    OutboundPayload(const OutboundPayload&)            = delete;
    OutboundPayload& operator=(const OutboundPayload&) = delete;

    /**
     * @brief The `QUIC_BUFFER` descriptor array to hand to msquic.
     *
     * The returned pointer stays valid for as long as this object is
     * alive, i.e. until the matching completion event releases it.
     * It must be passed to the send API as-is — msquic retains it.
     */
    [[nodiscard]] const QUIC_BUFFER* buffers() const noexcept {
        return &buffer_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

private:
    void init_buffer() noexcept {
        buffer_.Buffer = data_.data();
        buffer_.Length = static_cast<std::uint32_t>(data_.size());
    }

    std::vector<std::uint8_t> data_;
    QUIC_BUFFER buffer_{};
};

/**
 * @brief Release an OutboundPayload reported as sent (or cancelled).
 *
 * Safe to call with a null context (msquic delivers a null client
 * context when the send request had none).
 */
inline void release_outbound_payload(void* client_context) noexcept {
    delete static_cast<OutboundPayload*>(client_context);
}

}  // namespace nimrtc::web_transport::detail

#endif  // NIMRTC_WEB_TRANSPORT_MSQUIC_SEND_HPP
