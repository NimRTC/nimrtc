/**
 * @file src/engine/src/rtp_packetizer.hpp
 * @brief PRIVATE engine collaborator: the RTP packet builder.
 *
 * Architecture remediation plan §4.3 (D1 — engine split) and ADR-025
 * Decision 4: the `rtp::` usage is *concentrated* into this one place,
 * so the engine's exemption from Layering Invariant 4 has a visible
 * boundary instead of being spread over a dozen files.
 *
 * ## Why this exists
 *
 * ADR-025 Decision 1 exempts the `rtp` module from the "the engine only
 * talks to plugins" rule, and Decision 4 says the exemption is only
 * honest if the concrete use is *concentrated*: a reader must be able to
 * look at one file and see every place the engine touches the RTP wire
 * format.  Before this, the engine's *outbound* packet building named a
 * concrete `rtp::` type in four files.
 *
 * The header below deliberately contains **no** `rtp::` type.  The
 * collaborator speaks in plain scalars and byte spans, so the header can
 * be included by the engine's own internal headers without dragging the
 * concrete RTP module in with it.  The concrete `rtp::PacketBuilder`
 * lives in `rtp_packetizer.cpp` and nowhere else.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

#include <nimrtc/core/bytes.hpp>

namespace nimrtc::engine::detail {

/** The description of one outbound RTP packet.
 *
 *  Plain scalars and a payload span — no `rtp::` type appears here, so
 *  the header stays independent of the concrete RTP module. */
struct RtpPacketSpec {
    /** The sender's SSRC (RFC 3550 §5.1). */
    std::uint32_t ssrc = 0;

    /** The 16-bit sequence number. */
    std::uint16_t seq = 0;

    /** The media-clock timestamp. */
    std::uint32_t timestamp = 0;

    /** The payload type on the wire. */
    std::uint8_t payload_type = 0;

    /** The marker bit (set on the last packet of a frame). */
    bool marker = false;

    /** The payload bytes. */
    core::ByteSpan payload{};

    /** The abs-send-time extension id to emit the extension under, or 0
     *  to emit no abs-send-time extension at all.
     *
     *  RFC 5285 §6: the extension id is negotiated per direction, so it
     *  is the *caller's* number, not a constant. */
    std::uint16_t abs_send_time_ext_id = 0;

    /** The 24-bit abs-send-time value (RFC 8285 §4.2), already encoded
     *  on the caller's clock basis.  Ignored when
     *  `abs_send_time_ext_id == 0`. */
    std::uint32_t abs_send_time_24mhz = 0;
};

/** Builds outbound RTP packets.
 *
 *  The concrete `rtp::PacketBuilder` lives behind this collaborator, so
 *  the engine's internal headers never see the concrete RTP module and
 *  the concentration has exactly one home.
 *
 *  ## Threading
 *
 *  Stateless, and therefore thread-safe: every call builds its own
 *  builder.  The engine's send paths are serialised by the engine's own
 *  contract anyway (the codec / sender seams are explicitly not
 *  thread-safe), but a stateless collaborator removes the question
 *  entirely. */
class RtpPacketizer {
public:
    /** The serialised size of the packet described by `spec`.
     *
     *  Returns 0 when the spec is not representable. */
    static std::size_t compute_size(const RtpPacketSpec& spec) noexcept;

    /** Serialise the packet described by `spec` into `out`.
     *
     *  @param out     Destination buffer (may be null only when
     *                 @p capacity is 0, in which case nothing is written).
     *  @param capacity Bytes available in @p out.
     *  @return The number of bytes written, or 0 when the buffer is too
     *          small (or null).  A 0-byte packet is not representable,
     *          so 0 unambiguously means "nothing was written". */
    static std::size_t build_into(const RtpPacketSpec& spec,
                                  std::uint8_t* out,
                                  std::size_t capacity) noexcept;

    /** Encode a send instant into the 24-bit abs-send-time wire value
     *  (RFC 8285 §4.2).
     *
     *  @param monotonic_us   The send instant on the *monotonic* clock.
     *  @param origin_unix_us The wall clock (µs since the Unix epoch)
     *                        that corresponds to a monotonic reading of
     *                        zero — the engine's timeline origin.
     *  @return The 24-bit value (the encoder already applies the 24-bit
     *          wrap, so the caller masks nothing). */
    static std::uint32_t encode_abs_send_time_24(
        std::chrono::microseconds monotonic_us,
        std::int64_t origin_unix_us) noexcept;
};

}  // namespace nimrtc::engine::detail
