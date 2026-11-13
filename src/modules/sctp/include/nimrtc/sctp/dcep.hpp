/**
 * @file nimrtc/sctp/dcep.hpp
 * @brief DCEP (RFC 8832) + PPID (RFC 8831 §6) codec for WebRTC DataChannel.
 *
 * Minimal encode/decode for the engine-side SCTP-over-DTLS wiring:
 *   - DATA_CHANNEL_OPEN (0x03): label + reliability / ordered / priority.
 *   - DATA_CHANNEL_ACK  (0x02): open acknowledgement.
 *   - PPID constants: DCEP=50, UTF-8 string=51, binary=53.
 *   - Reliability type mapping to plugins::DataChannelReliability.
 *
 * ## Channel Type (RFC 8832 §5.1 / §8.2.2)
 *
 * The IANA "Channel Types" registry defines six values.  The high-order
 * bit (0x80) flags unordered delivery; the low bits select the
 * reliability class:
 *
 *   0x00  DATA_CHANNEL_RELIABLE                             reliable, in-order
 *   0x01  DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT              partial (rexmit), in-order
 *   0x02  DATA_CHANNEL_PARTIAL_RELIABLE_TIMED               partial (timed), in-order
 *   0x80  DATA_CHANNEL_RELIABLE_UNORDERED                   reliable, unordered
 *   0x81  DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT_UNORDERED    partial (rexmit), unordered
 *   0x82  DATA_CHANNEL_PARTIAL_RELIABLE_TIMED_UNORDERED     partial (timed), unordered
 *
 * 0x7f and 0xff are Reserved by the RFC and are never emitted.
 *
 * ## Ambiguity of 0x80
 *
 * Two NimRTC configurations map onto `DATA_CHANNEL_RELIABLE_UNORDERED`
 * (0x80): `kUnreliable` (the send path sets SCTP_UNORDERED on a still
 * reliable association) and `kReliableOrdered` with `ordered = false`.
 * The wire format cannot tell them apart, so the decoder reports
 * `kUnreliable` for 0x80 — the wider guarantee of the two.  A peer
 * that opened the channel with `kReliableOrdered` + `ordered = false`
 * therefore sees `kUnreliable` on the NimRTC side; the delivery
 * semantics (reliable transport, unordered delivery) are identical.
 */

#ifndef NIMRTC_SCTP_DCEP_HPP
#define NIMRTC_SCTP_DCEP_HPP

#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nimrtc/plugins/datachannel.hpp>

namespace nimrtc::sctp {

// ---- PPID (RFC 8831 §6) -------------------------------------------------
NIMRTC_API_STABLE inline constexpr std::uint32_t kPpidDcep   = 50;
NIMRTC_API_STABLE inline constexpr std::uint32_t kPpidString = 51;
NIMRTC_API_STABLE inline constexpr std::uint32_t kPpidBinary = 53;

// ---- DCEP message types (RFC 8832 §5) ------------------------------------
NIMRTC_API_STABLE inline constexpr std::uint8_t kDcepAck  = 0x02;
NIMRTC_API_STABLE inline constexpr std::uint8_t kDcepOpen = 0x03;

// ---- DCEP Channel Types (RFC 8832 §5.1 / §8.2.2) -------------------------
NIMRTC_API_STABLE inline constexpr std::uint8_t kDcepReliable            = 0x00;
NIMRTC_API_STABLE inline constexpr std::uint8_t kDcepPartialReliableRexmit = 0x01;
NIMRTC_API_STABLE inline constexpr std::uint8_t kDcepPartialReliableTimed  = 0x02;
NIMRTC_API_STABLE inline constexpr std::uint8_t kDcepReliableUnordered            = 0x80;
NIMRTC_API_STABLE inline constexpr std::uint8_t kDcepPartialReliableRexmitUnordered = 0x81;
NIMRTC_API_STABLE inline constexpr std::uint8_t kDcepPartialReliableTimedUnordered  = 0x82;

/** Mask of the "unordered delivery" bit in the Channel Type field
 *  (RFC 8832 §8.2.2: "the high-order bit MUST be used to indicate
 *  whether or not the message delivery is unordered"). */
NIMRTC_API_STABLE inline constexpr std::uint8_t kDcepUnorderedFlag = 0x80;

/** Decoded DATA_CHANNEL_OPEN parameters. */
struct NIMRTC_API_STABLE DcepOpenParams {
    plugins::DataChannelReliability reliability =
        plugins::DataChannelReliability::kReliableOrdered;
    bool ordered = true;
    std::uint16_t max_retransmits = 0;
    std::uint16_t max_packet_lifetime_ms = 0;
    /** Channel priority (RFC 8832 §5.1, u16 BE).  A lower value means a
     *  higher priority, matching `plugins::DataChannelConfig::priority`. */
    std::uint16_t priority = 128;
    std::string label;
};

/** Encode DATA_CHANNEL_ACK (type 0x02, no payload). */
NIMRTC_API_STABLE std::vector<std::uint8_t> dcep_encode_ack();

/** Encode DATA_CHANNEL_OPEN from a channel config (type 0x03).
 *
 *  The Channel Type carries both the reliability class and the
 *  ordered / unordered bit, so `cfg.ordered` reaches the wire:
 *  `plugins::DataChannelReliability::kUnreliable` maps to
 *  `DATA_CHANNEL_RELIABLE_UNORDERED` (0x80) — NimRTC's "unreliable"
 *  is the SCTP_UNORDERED flag on a still-reliable association, not a
 *  partial-reliability policy.
 */
NIMRTC_API_STABLE std::vector<std::uint8_t> dcep_encode_open(
    const plugins::DataChannelConfig& cfg);

/** Decode DATA_CHANNEL_OPEN payload. Returns nullopt on malformed input. */
NIMRTC_API_STABLE std::optional<DcepOpenParams> dcep_decode_open(
    const std::uint8_t* data, std::size_t len) noexcept;

/** Map a channel config to its wire PPID.
 *
 *  NimRTC's DataChannel carries opaque binary payloads, so the binary
 *  PPID (53) is used.  The UTF-8 string PPID (51) is reserved for a
 *  channel that explicitly asks for string semantics, which
 *  `plugins::DataChannelConfig` does not model. */
NIMRTC_API_STABLE inline std::uint32_t dcep_ppid_for_config(
    const plugins::DataChannelConfig& /*cfg*/) noexcept {
    return kPpidBinary;
}

} // namespace nimrtc::sctp

#endif // NIMRTC_SCTP_DCEP_HPP
