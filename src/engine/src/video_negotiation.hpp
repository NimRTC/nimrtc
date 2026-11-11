/**
 * @file video_negotiation.hpp
 * @brief The negotiated video codec state, shared by the SDP layer and the
 *        media layer.
 *
 * ## Why this file exists
 *
 * Two independent concerns need the same piece of information — *what the
 * peer and we actually agreed on for video*:
 *
 *   - The **SDP layer** (`engine_signaling.cpp`) computes it while building
 *     an answer or reading a remote description.
 *   - The **media layer** (`engine_media.cpp` / `engine_timeline.cpp`)
 *     consumes it when stamping outgoing RTP headers and when demultiplexing
 *     inbound packets by payload type.
 *
 * Putting the shape in one place keeps the producer and the consumer from
 * drifting apart, and keeps `engine_impl.hpp` free of another pile of
 * loosely-related scalars.
 *
 * ## What "negotiated" means here
 *
 * RFC 3264 §6.1 constrains an answer to use only payload types the offer
 * contained.  The *negotiated* payload type is therefore the one that
 * appears in **both** descriptions:
 *
 *   - When we **answer** a remote offer, it is the payload type we picked
 *     out of the offer's video m-line (and echoed in our answer).
 *   - When we **receive** an answer to our offer, it is the payload type
 *     the peer kept in its answer's video m-line.
 *
 * A payload type that only exists in one of the two descriptions is *not*
 * negotiated, and packets carrying it must not be treated as video.
 *
 * ## Relationship to the audio side
 *
 * The audio side has its own, older, field (`Impl::negotiated_audio_pt`).
 * This structure is the video counterpart; the two are deliberately kept
 * separate because they are negotiated by different rules (audio has
 * static payload types and codec-specific fmtp, video has the RFC 6184
 * packetization-mode / profile-level-id pair).
 */

#pragma once

#include <cstdint>
#include <string>

namespace nimrtc::engine::detail {

/** The video codec and payload type the SDP exchange settled on.
 *
 *  Default-constructed state means "nothing was negotiated" — the caller
 *  must check `valid` (or the payload type) before acting on it.  This
 *  matters because a host may drive the engine without an SDP exchange
 *  at all (the test seams, the loopback harness), in which case the
 *  engine falls back to its local configuration. */
struct VideoNegotiationResult {
    /** True once a description exchange produced a usable answer.
     *
     *  False means "no negotiation happened" — either no remote
     *  description was processed, or the two sides had no video codec in
     *  common.  The two cases are told apart by `codec_unsupported`. */
    bool valid = false;

    /** True when the negotiation ran but failed because the peer offered
     *  no video codec we can decode.  `valid` is false in that case, but
     *  this flag distinguishes "we never tried" from "we tried and the
     *  answer is no". */
    bool codec_unsupported = false;

    /** The negotiated payload type; meaningful only when `valid`. */
    std::uint8_t payload_type = 0;

    /** The negotiated codec's SDP encoding name, e.g. "H264", "VP8",
     *  "VP9" (the value that appears in `a=rtpmap:`).  Empty when
     *  nothing was negotiated. */
    std::string encoding;

    /** The negotiated codec's clock rate in Hz (90000 for H.264/VP8/VP9).
     *  Zero when nothing was negotiated. */
    std::uint32_t clock_rate = 0;

    /** The negotiated `a=fmtp:` body, without the `a=fmtp:<pt> ` prefix.
     *
     *  Empty when the codec carries no format parameters, or when nothing
     *  was negotiated. */
    std::string fmtp;
};

} // namespace nimrtc::engine::detail
