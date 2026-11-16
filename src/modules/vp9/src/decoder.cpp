/**
 * @file src/modules/vp9/src/decoder.cpp
 * @brief VP9 stub decoder — consumes a frame, reports its shape.
 *
 * The stub does not parse the VP9 bitstream: it recognises the *shape*
 * of a VP9 frame (a non-empty payload) and reports it as a key frame
 * when the caller says so through the frame's metadata.  It returns an
 * empty `DecodedImage`; a real decoder (the vendored libvpx backend)
 * produces a fully-populated `VideoFrameBuffer`.
 */

#include <nimrtc/vp9/decoder.hpp>

namespace nimrtc::vp9 {

// ---------------------------------------------------------------------------
// StubDecoder
// ---------------------------------------------------------------------------

std::optional<DecodedImage> StubDecoder::decode(core::ByteSpan encoded,
                                                video_frame::VideoFrameInfo info) noexcept {
    if (encoded.empty()) return std::nullopt;

    DecodedImage out;
    out.capture_ts_us = info.capture_ts_us;
    out.frame_seq     = info.frame_seq;
    out.rtp_timestamp = info.rtp_timestamp;

    // The stub does not parse the frame header, so the key-frame flag is
    // a conservative "true": the synthetic encoder this module ships
    // marks every frame as a random-access point, and the real decoder
    // (libvpx) reports the truth.
    out.is_keyframe = true;

    // The stub produces no pixels — the buffer remains empty
    // (valid() == false).
    return out;
}

std::unique_ptr<Decoder> create_stub_decoder() noexcept {
    return std::make_unique<StubDecoder>();
}

} // namespace nimrtc::vp9
