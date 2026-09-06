/**
 * @file nimrtc/video_payload/vp9.hpp
 * @brief VP9 RTP payload packetizer / depacketizer (RFC 9559).
 *
 * VP9 payload descriptor is up to 3 bytes:
 *
 *   0 1 2 3 4 5 6 7
 *   +-+-+-+-+-+-+-+-+
 *   |Z|Y|F| ID  |N|D|
 *   +-+-+-+-+-+-+-+-+
 *
 *   Z = picture ID present (boolean)
 *   Y = layer ID present (boolean)
 *   F = flexible mode (no SVC layers)
 *   ID = 2 bits (if Z=0 → 0; if Z=1 → 7-bit picture ID otherwise)
 *   N = non-reference frame (1 = non-ref)
 *   D = inter-frame predicted (1 = inter, 0 = intra-key)
 *
 * If Z=1, a 1- or 2-byte picture ID follows (M bit picks 7 vs 15 bits).
 * If Y=1, a 1-byte layer info follows (TID + SID + D + U bits).
 *
 * Scalability Structure (SS) data is optional and follows.  We don't
 * generate SS data — Chrome accepts its absence for non-SVC streams.
 *
 * @note P1. Standalone module; not yet wired into the engine.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <vector>

#include <nimrtc/core/bytes.hpp>

namespace nimrtc::video_payload::vp9 {

// ---------------------------------------------------------------------------
// Descriptor
// ---------------------------------------------------------------------------

struct Descriptor {
    /** Picture ID. If `has_picture_id` is false, value is unspecified. */
    bool         has_picture_id = false;
    bool         picture_id_16bit = false;   ///< M=1 → 16-bit; M=0 → 7-bit
    std::int32_t picture_id     = 0;

    /** Layer info present (TID + SID). */
    bool         has_layer_info = false;
    std::uint8_t temporal_id    = 0;
    std::uint8_t spatial_id     = 0;
    /** Flexible mode (no SVC). */
    bool         flexible_mode  = false;

    /** Non-reference frame (N bit). */
    bool         non_reference  = false;

    /** Inter-frame predicted (D bit). If false, this is an intra key frame. */
    bool         inter_predicted = false;
};

/** Total size of the descriptor for this payload, in bytes.
 *  Returns 1, 2, or 3 depending on has_picture_id / has_layer_info. */
std::size_t descriptor_size(const Descriptor& d) noexcept;

// ---------------------------------------------------------------------------
// Parse
// ---------------------------------------------------------------------------

struct ParseResult {
    bool          parsed_ok = false;
    Descriptor    desc{};
    core::ByteSpan bitstream;
};

ParseResult parse(core::ByteSpan payload) noexcept;

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

/** Write the VP9 payload descriptor into @p output.
 *  @return Number of bytes written; 0 on overflow / unsupported config. */
std::size_t build_descriptor(const Descriptor& d,
                             core::MutableByteSpan output) noexcept;

/** Build a complete VP9 RTP payload (descriptor + bitstream) into @p output.
 *  @return Number of bytes written. */
std::size_t build(const Descriptor& d,
                  core::ByteSpan bitstream,
                  core::MutableByteSpan output) noexcept;

} // namespace nimrtc::video_payload::vp9
