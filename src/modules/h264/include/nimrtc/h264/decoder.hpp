/**
 * @file nimrtc/h264/decoder.hpp
 * @brief H.264 decoder interface + stub implementation.
 *
 * Per ARCHITECTURE.md §1.4 + §11.3: NimRTC does NOT bundle H.264 decoder
 * (H.264 has patent pool fees, see §11.3).  This module ships:
 *   - The Decoder interface (what a host plugin must implement)
 *   - A no-op StubDecoder that consumes H.264 NAL units and produces an
 *     empty VideoFrameBuffer (useful for testing the RTP / payload /
 *     jitter pipeline without a real decoder)
 *
 * Host applications register their own decoder (e.g. OpenH264 / FFmpeg /
 * HW-accelerated decoder) via:
 *
 * @code
 * class MyDecoder : public nimrtc::h264::Decoder { ... };
 * static nimrtc::plugins::SimpleVideoCodecFactory<MyCodec> factory{
 *     "h264_hw", "My HW H.264 codec"};
 * NIMRTC_REGISTER_VIDEO_CODEC("h264_hw", &factory);
 * @endcode
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/video_frame/frame.hpp>

namespace nimrtc::h264 {

// ---------------------------------------------------------------------------
// SPS-derived configuration (best-effort extraction)
// ---------------------------------------------------------------------------

struct SpsSummary {
    std::uint32_t width      = 0;     ///< pic_width_in_mbs_minus1 * 16
    std::uint32_t height     = 0;     ///< pic_height_in_map_units_minus1 * 16
    std::uint8_t  profile_idc = 0;    ///< e.g. 66 (Baseline), 100 (High)
    std::uint8_t  level_idc   = 0;
    bool          valid = false;
};

/** Extract high-level SPS fields (width, height, profile). Used by SDP
 *  answer generation.  Full H.264 RBSP parsing is intentionally NOT done
 *  here (it's several hundred lines of bit-fiddling) — we extract only
 *  the fields needed for SDP `a=fmtp` lines.
 *
 *  For full SPS parsing, integrate libopenh264 or ffmpeg via a host plugin.
 */
SpsSummary parse_sps(core::ByteSpan sps_nalu) noexcept;

// ---------------------------------------------------------------------------
// Decoder interface
// ---------------------------------------------------------------------------

/** Output of one decode call. */
struct DecodedImage {
    video_frame::VideoFrameBuffer buffer;   ///< refcounted pixel storage
    std::uint32_t                 width    = 0;
    std::uint32_t                 height   = 0;
    std::int64_t                  capture_ts_us = 0;
    std::uint32_t                 frame_seq     = 0;
    std::uint32_t                 rtp_timestamp = 0;
    bool                          is_keyframe   = false;
};

/** Abstract H.264 decoder — implemented by host plugins (libopenh264 / ffmpeg / HW). */
class Decoder {
public:
    virtual ~Decoder() = default;

    /** Initialise the decoder with a configuration.
     *  Must be called once before decode(). */
    virtual plugins::Status open() noexcept = 0;

    /** Decode one access unit (typically one IDR or one P-frame).
     *  @param encoded  H.264 Annex B bitstream (SPS/PPS may precede slices).
     *  @param info     Per-frame metadata (capture_ts, frame_seq, rtp_timestamp).
     *  @return DecodedImage on success (may have empty buffer for stub).
     */
    virtual std::optional<DecodedImage> decode(core::ByteSpan encoded,
                                               video_frame::VideoFrameInfo info) noexcept = 0;

    /** Tear down. */
    virtual void close() noexcept = 0;

    /** Whether this is a no-op decoder (consumes input, produces empty output). */
    virtual bool is_stub() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// Stub decoder
// ---------------------------------------------------------------------------

/** A no-op decoder used when no real H.264 implementation is linked.
 *  Useful for the pipeline test (RTP → payload → JB → decoder → empty
 *  VideoFrame) without dragging in libopenh264 / ffmpeg. */
class StubDecoder final : public Decoder {
public:
    StubDecoder() = default;
    ~StubDecoder() override = default;

    plugins::Status open() noexcept override { return plugins::kOk; }

    std::optional<DecodedImage> decode(core::ByteSpan encoded,
                                       video_frame::VideoFrameInfo info) noexcept override;

    void close() noexcept override {}

    bool is_stub() const noexcept override { return true; }
};

/** Factory for the stub decoder. */
std::unique_ptr<Decoder> create_stub_decoder() noexcept;

} // namespace nimrtc::h264
