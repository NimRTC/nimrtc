/**
 * @file nimrtc/vp9/decoder.hpp
 * @brief VP9 decoder interface + stub implementation.
 *
 * ## Why a stub
 *
 * The remote-desktop plan's B11 vendors `libvpx` — the reference VP9
 * codec — behind `NIMRTC_VENDORED_LIBVPX`, which **defaults OFF**.
 * With the option off there is no VP9 decoder in the tree, so what this
 * module ships:
 *
 *   - The `Decoder` interface — what a host plugin (or the vendored
 *     libvpx backend) must implement.
 *   - A no-op `StubDecoder` that consumes a VP9 frame and reports its
 *     key-frame-ness without producing pixels.
 *
 * The stub exists so the full pipeline (RTP → payload → JB → decoder →
 * callback) can be exercised for VP9 without linking a codec, exactly
 * like the H.264 / H.265 / AV1 modules' stubs.
 *
 * ## Frame framing
 *
 * The decoder consumes a **raw VP9 frame** — the bitstream libvpx's
 * `vpx_codec_encode()` produces.  Unlike H.264 / HEVC there is no
 * start-code framing: VP9 is a single frame per access unit, and the
 * random-access point is the *frame header*'s `frame_type == KEY_FRAME`
 * bit plus the presence of the key-frame index.
 *
 * NimRTC carries VP9 in the RFC 9559 payload format, whose descriptor
 * carries the `D` (inter-frame predicted) bit — so the key-frame-ness
 * survives the RTP round trip in the descriptor, not in the bitstream.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/video_frame/frame.hpp>

namespace nimrtc::vp9 {

// ---------------------------------------------------------------------------
// Decoder interface
// ---------------------------------------------------------------------------

/** Output of one decode call. */
struct NIMRTC_API_STABLE DecodedImage {
    video_frame::VideoFrameBuffer buffer;   ///< refcounted pixel storage
    std::uint32_t                 width    = 0;
    std::uint32_t                 height   = 0;
    std::int64_t                  capture_ts_us = 0;
    std::uint32_t                 frame_seq     = 0;
    std::uint32_t                 rtp_timestamp = 0;
    bool                          is_keyframe   = false;
};

/** Abstract VP9 decoder — implemented by the vendored libvpx backend or
 *  by a host plugin. */
class NIMRTC_API_STABLE Decoder {
public:
    virtual ~Decoder() = default;

    /** Initialise the decoder with a configuration.
     *  Must be called once before decode(). */
    virtual plugins::Status open() noexcept = 0;

    /** Decode one frame.
     *  @param encoded  A VP9 frame (the bitstream libvpx emits).
     *  @param info     Per-frame metadata (capture_ts, frame_seq,
     *                  rtp_timestamp).
     *  @return DecodedImage on success (may have an empty buffer for the
     *          stub). */
    virtual std::optional<DecodedImage> decode(core::ByteSpan encoded,
                                               video_frame::VideoFrameInfo info) noexcept = 0;

    /** Tear down. */
    virtual void close() noexcept = 0;

    /** Whether this is a no-op decoder (consumes input, produces empty
     *  output). */
    virtual bool is_stub() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// Stub decoder
// ---------------------------------------------------------------------------

/** A no-op decoder used when no real VP9 implementation is linked. */
class NIMRTC_API_STABLE StubDecoder final : public Decoder {
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
NIMRTC_API_STABLE std::unique_ptr<Decoder> create_stub_decoder() noexcept;

} // namespace nimrtc::vp9
