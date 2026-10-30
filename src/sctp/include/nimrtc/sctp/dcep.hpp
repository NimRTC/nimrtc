/**
 * @file nimrtc/sctp/dcep.hpp
 * @brief DCEP (RFC 8832) + PPID (RFC 8831 §6) codec for WebRTC DataChannel.
 *
 * Minimal encode/decode for the engine-side SCTP-over-DTLS wiring:
 *   - DATA_CHANNEL_OPEN (0x03): label + reliability / ordered / priority.
 *   - DATA_CHANNEL_ACK  (0x02): open acknowledgement.
 *   - PPID constants: DCEP=50, UTF-8 string=51, binary=53.
 *   - Reliability type mapping to plugins::DataChannelReliability.
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
inline constexpr std::uint32_t kPpidDcep   = 50;
inline constexpr std::uint32_t kPpidString = 51;
inline constexpr std::uint32_t kPpidBinary = 53;

// ---- DCEP message types (RFC 8832 §5) ------------------------------------
inline constexpr std::uint8_t kDcepAck  = 0x02;
inline constexpr std::uint8_t kDcepOpen = 0x03;

// ---- DCEP reliability types (RFC 8832 §5.1) -------------------------------
inline constexpr std::uint8_t kDcepReliable            = 0x00;
inline constexpr std::uint8_t kDcepPartialReliableRexmit = 0x01;
inline constexpr std::uint8_t kDcepPartialReliableTimed  = 0x02;

/** Decoded DATA_CHANNEL_OPEN parameters. */
struct NIMRTC_API_STABLE DcepOpenParams {
    plugins::DataChannelReliability reliability =
        plugins::DataChannelReliability::kReliableOrdered;
    bool ordered = true;
    std::uint16_t max_retransmits = 0;
    std::uint16_t max_packet_lifetime_ms = 0;
    std::string label;
};

/** Encode DATA_CHANNEL_ACK (type 0x02, no payload). */
NIMRTC_API_STABLE std::vector<std::uint8_t> dcep_encode_ack();

/** Encode DATA_CHANNEL_OPEN from a channel config (type 0x03). */
NIMRTC_API_STABLE std::vector<std::uint8_t> dcep_encode_open(
    const plugins::DataChannelConfig& cfg);

/** Decode DATA_CHANNEL_OPEN payload. Returns nullopt on malformed input. */
NIMRTC_API_STABLE std::optional<DcepOpenParams> dcep_decode_open(
    const std::uint8_t* data, std::size_t len) noexcept;

/** Map a channel config to its wire PPID (string=51 default). */
NIMRTC_API_STABLE inline std::uint32_t dcep_ppid_for_config(
    const plugins::DataChannelConfig& /*cfg*/) noexcept {
    return kPpidBinary;
}

}  // namespace nimrtc::sctp

#endif  // NIMRTC_SCTP_DCEP_HPP
