/**
 * @file src/web_transport/src/web_transport_frame.hpp
 * @brief NimRTC data-channel frame codec (v1.2.0, DataChannel capability
 *        plan WP-B / WP-C3).
 *
 * NOT part of the public API — an internal header of the
 * `web_transport` module.  Consumers include
 * <nimrtc/web_transport/web_transport_session_iface.hpp> and
 * <nimrtc/web_transport/web_transport_data_channel.hpp>.
 *
 * ## Why a frame
 *
 * A QUIC stream is a *byte* stream: `send()` boundaries are not
 * preserved, and two sends may surface as one chunk or vice versa.
 * The WebTransport data plane therefore carries an explicit frame:
 *
 * ```text
 *   0            4            8                        8+label_len
 *   +------------+------------+------------------------+-------------+
 *   | u32 BE     | u32 BE     | label bytes (UTF-8)    | payload     |
 *   | label_len  | payload_len|                        | bytes       |
 *   +------------+------------+------------------------+-------------+
 * ```
 *
 * The length prefixes make the message boundary explicit (one frame
 * == one `IDataChannel::send()`), and the label is the channel
 * identity that makes several channels on one connection isolable
 * (WP-C3).  The label matches the DCEP `DATA_CHANNEL_OPEN` semantics
 * in RFC 8832: the receiver routes an incoming channel by its label.
 *
 * The framing is a **NimRTC-internal convention** (DEC-2 = (b)): a
 * peer that speaks raw bytes (Chrome's `RTCDataChannel`-over-
 * WebTransport) must be configured for `DataChannelFraming::kRaw`,
 * in which case no frame is written and no frame is expected.
 */

#ifndef NIMRTC_WEB_TRANSPORT_FRAME_HPP
#define NIMRTC_WEB_TRANSPORT_FRAME_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nimrtc::web_transport::detail {

/** Size of the fixed frame header (label_len + payload_len). */
inline constexpr std::size_t kFrameHeaderSize = 8;

/** Maximum label length a frame may carry (bytes). */
inline constexpr std::uint32_t kMaxFrameLabelLength = 255;

/**
 * @brief Default maximum payload length of one frame.
 *
 * The engine advertises `a=max-message-size:6556` in its SDP
 * (see `src/engine/src/engine_signaling.cpp`), so a frame that claims
 * more than this is a protocol error rather than an allocation
 * request.  The value is deliberately larger than the advertised one
 * so that a peer which negotiated a bigger limit is not rejected.
 */
inline constexpr std::uint32_t kDefaultMaxFramePayload = 65536;

/**
 * @brief Encode one frame.
 *
 * @param label    Channel label (the identity).  Must not exceed
 *                 `kMaxFrameLabelLength`; longer labels are truncated
 *                 by the caller, not here.
 * @param payload  The message payload.
 * @return The framed bytes, ready to be handed to the transport.
 */
std::vector<std::uint8_t> encode_frame(std::string_view label,
                                        std::span<const std::uint8_t> payload);

/**
 * @brief Reassembles a byte stream into frames.
 *
 * Feed the transport's chunks in order; the reader buffers whatever
 * does not yet form a complete frame and hands out one frame at a
 * time.  A frame whose length prefix exceeds the configured maximum is
 * a protocol error: the reader latches the error state and reports it
 * once, so the owner can transition the channel to `error` instead of
 * allocating an unbounded buffer.
 */
class FrameReader {
public:
    explicit FrameReader(std::uint32_t max_payload = kDefaultMaxFramePayload) noexcept
        : max_payload_(max_payload) {}

    /** Reset the reader (drops any partial frame and the error latch). */
    void reset() noexcept;

    /**
     * @brief Append bytes and try to complete a frame.
     *
     * @return false when the input is a protocol error (the reader
     *         latches `failed()`); true otherwise.
     */
    bool feed(std::span<const std::uint8_t> bytes) noexcept;

    /** True when a complete frame is available. */
    [[nodiscard]] bool has_frame() const noexcept { return has_frame_; }

    /** The current frame's channel label (valid while `has_frame()`). */
    [[nodiscard]] std::string_view label() const noexcept { return label_; }

    /** The current frame's payload (valid while `has_frame()`). */
    [[nodiscard]] std::span<const std::uint8_t> payload() const noexcept {
        return std::span<const std::uint8_t>{payload_.data(), payload_.size()};
    }

    /** Drop the current frame; the reader is ready for the next one. */
    void consume() noexcept;

    /** True when a protocol error was latched. */
    [[nodiscard]] bool failed() const noexcept { return failed_; }

    /** Human-readable reason for the latched error ("" when none). */
    [[nodiscard]] const char* error() const noexcept { return error_; }

private:
    bool try_complete() noexcept;

    std::vector<std::uint8_t> buffer_;
    std::string                 label_;
    std::vector<std::uint8_t>   payload_;
    std::uint32_t               max_payload_;
    bool                        has_frame_ = false;
    bool                        failed_    = false;
    const char*                 error_     = "";
};

} // namespace nimrtc::web_transport::detail

#endif // NIMRTC_WEB_TRANSPORT_FRAME_HPP
