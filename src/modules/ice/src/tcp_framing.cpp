// tcp_framing.cpp — RFC 6544 §3.2 two-octet length-prefix framing.

#include "tcp_framing.hpp"

#include <cstring>

namespace nimrtc::ice {

namespace {
constexpr std::size_t kFramingHeaderSize = 2;
// A STUN message is at least its 20-byte header (RFC 8489 §6).
constexpr std::size_t kMinStunMessageSize = 20;
// The framing length is a 16-bit field.
constexpr std::size_t kMaxFramedLength = 0xFFFF;
}  // namespace

std::vector<std::uint8_t> frame_message(std::span<const std::uint8_t> stun_message) {
    if (stun_message.size() > kMaxFramedLength || stun_message.empty()) {
        return {};
    }
    std::vector<std::uint8_t> framed;
    framed.reserve(kFramingHeaderSize + stun_message.size());
    // RFC 6544 §3.2: the length is the full STUN message size (STUN
    // header + attributes), excluding the framing prefix itself.
    framed.push_back(static_cast<std::uint8_t>((stun_message.size() >> 8) & 0xFF));
    framed.push_back(static_cast<std::uint8_t>(stun_message.size() & 0xFF));
    framed.insert(framed.end(), stun_message.begin(), stun_message.end());
    return framed;
}

bool TcpUnframer::push(std::span<const std::uint8_t> bytes,
                       std::vector<std::vector<std::uint8_t>>& out) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());

    std::size_t pos = 0;
    while (buffer_.size() - pos >= kFramingHeaderSize) {
        const std::size_t length =
            (static_cast<std::size_t>(buffer_[pos]) << 8) | buffer_[pos + 1];
        // Sanity: the length must describe a plausible STUN message.  A
        // stray length here means the stream has desynchronised.
        if (length < kMinStunMessageSize) {
            return false;
        }
        const std::size_t frame_end = pos + kFramingHeaderSize + length;
        if (frame_end > buffer_.size()) {
            break;  // partial frame; wait for the rest
        }
        std::vector<std::uint8_t> message(buffer_.begin() + pos + kFramingHeaderSize,
                                          buffer_.begin() + frame_end);
        out.push_back(std::move(message));
        pos = frame_end;
    }

    // Keep only the unconsumed tail (never re-copy consumed bytes).
    if (pos > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(pos));
    }
    return true;
}

}  // namespace nimrtc::ice
