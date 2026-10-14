#pragma once


// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nimrtc::core {

// -----------------------------------------------------------------------------
// Zero-copy view of bytes (no ownership).
// -----------------------------------------------------------------------------
using ByteSpan       = std::span<const std::uint8_t>;
using MutableByteSpan = std::span<std::uint8_t>;

// -----------------------------------------------------------------------------
// Owned byte buffer.
// -----------------------------------------------------------------------------
using ByteBuffer = std::vector<std::uint8_t>;

// -----------------------------------------------------------------------------
// Helpers — string ↔ bytes (no encoding conversion; ASCII / opaque).
// -----------------------------------------------------------------------------
inline ByteBuffer to_byte_buffer(std::string_view s) {
    ByteBuffer buf;
    buf.reserve(s.size());
    for (char c : s) {
        buf.push_back(static_cast<std::uint8_t>(c));
    }
    return buf;
}

inline std::string_view as_string_view(ByteSpan b) noexcept {
    return std::string_view(reinterpret_cast<const char*>(b.data()), b.size());
}

// Empty span sentinel — frequent in RTP/RTCP zero-payload cases.
inline ByteSpan empty_bytes() noexcept { return {}; }

} // namespace nimrtc::core