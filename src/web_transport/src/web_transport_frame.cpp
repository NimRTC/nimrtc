/**
 * @file src/web_transport/src/web_transport_frame.cpp
 * @brief NimRTC data-channel frame codec — implementation
 *        (v1.2.0, DataChannel capability plan WP-B / WP-C3).
 *
 * See `web_transport_frame.hpp` for the wire layout.
 */

#include "web_transport_frame.hpp"

#include <algorithm>
#include <cstring>

namespace nimrtc::web_transport::detail {

namespace {

void put_u32_be(std::uint8_t* dst, std::uint32_t v) noexcept {
    dst[0] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
    dst[1] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    dst[2] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    dst[3] = static_cast<std::uint8_t>(v & 0xFFu);
}

std::uint32_t get_u32_be(const std::uint8_t* src) noexcept {
    return (static_cast<std::uint32_t>(src[0]) << 24) |
           (static_cast<std::uint32_t>(src[1]) << 16) |
           (static_cast<std::uint32_t>(src[2]) << 8) |
           (static_cast<std::uint32_t>(src[3]));
}

} // anonymous namespace

std::vector<std::uint8_t> encode_frame(std::string_view label,
                                        std::span<const std::uint8_t> payload) {
    const std::size_t label_len = std::min<std::size_t>(label.size(),
                                                         kMaxFrameLabelLength);
    std::vector<std::uint8_t> out;
    out.reserve(kFrameHeaderSize + label_len + payload.size());
    out.resize(kFrameHeaderSize, 0);
    put_u32_be(out.data(), static_cast<std::uint32_t>(label_len));
    put_u32_be(out.data() + 4, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), label.begin(),
               label.begin() + static_cast<std::ptrdiff_t>(label_len));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

void FrameReader::reset() noexcept {
    buffer_.clear();
    label_.clear();
    payload_.clear();
    has_frame_ = false;
    failed_    = false;
    error_     = "";
}

bool FrameReader::feed(std::span<const std::uint8_t> bytes) noexcept {
    if (failed_) return false;
    if (!bytes.empty()) {
        try {
            buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
        } catch (...) {
            failed_ = true;
            error_  = "out of memory while buffering the inbound frame";
            return false;
        }
    }
    return try_complete();
}

bool FrameReader::try_complete() noexcept {
    if (has_frame_) return true;

    // ---- header ----
    if (buffer_.size() < kFrameHeaderSize) return true;

    const std::uint32_t label_len = get_u32_be(buffer_.data());
    const std::uint32_t payload_len = get_u32_be(buffer_.data() + 4);

    if (label_len > kMaxFrameLabelLength) {
        failed_ = true;
        error_  = "frame label length exceeds the protocol maximum";
        return false;
    }
    if (payload_len > max_payload_) {
        failed_ = true;
        error_  = "frame payload length exceeds the negotiated maximum";
        return false;
    }

    const std::size_t total = kFrameHeaderSize + label_len + payload_len;
    if (buffer_.size() < total) return true;  // wait for more bytes

    try {
        label_.assign(reinterpret_cast<const char*>(buffer_.data() + kFrameHeaderSize),
                      label_len);
        payload_.assign(buffer_.begin() + static_cast<std::ptrdiff_t>(
                                             kFrameHeaderSize + label_len),
                        buffer_.begin() + static_cast<std::ptrdiff_t>(total));
    } catch (...) {
        failed_ = true;
        error_  = "out of memory while reassembling the inbound frame";
        return false;
    }

    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(total));
    has_frame_ = true;
    return true;
}

void FrameReader::consume() noexcept {
    label_.clear();
    payload_.clear();
    payload_.shrink_to_fit();
    has_frame_ = false;
}

} // namespace nimrtc::web_transport::detail
