/**
 * @file nimrtc/video_payload/vp8.hpp
 * @brief VP8 RTP payload packetizer / depacketizer (RFC 7741).
 *
 * VP8 is a relatively simple bitstream. The RTP payload descriptor is a
 * single byte:
 *
 *   0 1 2 3 4 5 6 7
 *   +-+-+-+-+-+-+-+-+
 *   |X|R|N|S|R| PID |
 *   +-+-+-+-+-+-+-+-+
 *
 *   X = extended control bits present (a second descriptor byte follows)
 *   R = reserved
 *   N = non-reference frame (1 = non-ref)
 *   S = start of partition (1 = first fragment of frame)
 *   R = reserved (WebRTC uses as "has layer info")
 *   PID = partition index (0 = first partition; 7-bit values allowed)
 *
 * If X=1, a second descriptor byte follows:
 *
 *   0 1 2 3 4 5 6 7
 *   +-+-+-+-+-+-+-+-+
 *   |I|L|T|K| RSV   |
 *   +-+-+-+-+-+-+-+-+
 *
 *   I = picture ID present (1 = next 16 bits are picture ID, M-bit picks M or 16-bit)
 *   L = TL0PICIDX present
 *   T = TID present (temporal-layer index, 3 bits)
 *   K = KEYIDX present (1 = next 5 bits)
 *
 * We support only the mandatory descriptor (X=0) and the minimal extension
 * (I=1, picture ID only) — sufficient for Chrome interop.
 *
 * @note P1. Standalone module; not yet wired into the engine.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <vector>

#include <nimrtc/core/bytes.hpp>

namespace nimrtc::video_payload::vp8 {

// ---------------------------------------------------------------------------
// Descriptor fields
// ---------------------------------------------------------------------------

struct Descriptor {
    /** Partition index (0..7). */
    std::uint8_t partition_index = 0;
    /** True iff this is the start of a partition (S bit). */
    bool         start_of_partition = false;
    /** Non-reference frame (N bit). */
    bool         non_reference = false;
    /** Picture ID. If `has_picture_id` is false, value is unspecified. */
    bool         has_picture_id = false;
    std::int32_t picture_id     = 0;     ///< signed (M=1) or unsigned 16-bit
    /** True iff picture_id is a 16-bit (M=1) value; false = 7-bit (M=0). */
    bool         picture_id_16bit = false;
};

/** Total size of the descriptor for this payload, in bytes.
 *  Returns 1 if X=0, 2 if only I is set, 3-4 if more bits are set. */
std::size_t descriptor_size(const Descriptor& d) noexcept;

// ---------------------------------------------------------------------------
// Parse
// ---------------------------------------------------------------------------

struct ParseResult {
    bool          parsed_ok = false;
    Descriptor    desc{};
    core::ByteSpan bitstream;       ///< payload bytes AFTER the descriptor
};

ParseResult parse(core::ByteSpan payload) noexcept;

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

/** Write the VP8 payload descriptor into @p output.
 *  @return Number of bytes written; 0 on overflow / unsupported config. */
std::size_t build_descriptor(const Descriptor& d,
                             core::MutableByteSpan output) noexcept;

/** Build a complete VP8 RTP payload (descriptor + bitstream) into @p output.
 *  @return Number of bytes written. */
std::size_t build(const Descriptor& d,
                  core::ByteSpan bitstream,
                  core::MutableByteSpan output) noexcept;

// ---------------------------------------------------------------------------
// Convenience — derive keyframe / partition 0 marker
// ---------------------------------------------------------------------------

/** True if the VP8 bitstream's first byte indicates a key frame.
 *  VP8 bitstream: byte 0 = frame tag.  Bits 0 = key frame flag (1=keyframe). */
inline bool bitstream_is_keyframe(core::ByteSpan bitstream) noexcept {
    return bitstream.size() >= 1 && (bitstream[0] & 0x01) != 0;
}

} // namespace nimrtc::video_payload::vp8
