/**
 * @file src/engine/src/engine_video_params.hpp
 * @brief The ONE place the engine computes the encoder's parameter set
 *        (remote-desktop plan B12).
 *
 * ## Why this file exists
 *
 * The encoder is a **passive** component.  It has no access to the
 * bandwidth estimator, to the rate controller, or to the feedback
 * stream: everything it needs to know about the stream it is coding
 * arrives through `plugins::IVideoCodec::set_params()`.
 *
 * That contract is only enforceable if the parameters are computed in
 * **one** place.  When they were scattered — some in `EngineConfig`,
 * some in the codec's own `open()` config, some implicit in whatever
 * the capture device happened to produce — two of them could disagree,
 * and the disagreement only showed up as a picture that was subtly
 * wrong.
 *
 * This file is that one place.  `compute_video_encoder_params()` reads
 * the engine's configuration, the negotiated codec, and (optionally)
 * the geometry the source is actually producing, and returns the
 * complete parameter set.  Everything else in the engine calls it.
 *
 * ## The geometry rule (B13)
 *
 * A resolution change is a **parameter-set change**.  The peer's
 * decoder cannot make sense of a frame whose size differs from the one
 * its parameter sets describe, so the new SPS / PPS (or VPS, or the
 * AV1 sequence header) must travel with the new frame — no PLI
 * round-trip, no waiting for the peer to ask.
 *
 * The engine therefore watches the geometry of the frames it actually
 * sends and re-computes the parameter set when it changes.  The
 * `set_params()` call is what tells the codec to restart, and the
 * encoder is required to emit the parameter sets with the next frame.
 */

#pragma once

#include <cstdint>
#include <optional>

#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/video_frame/roi.hpp>

namespace nimrtc::engine::detail {

/** The inputs the parameter computation needs.
 *
 *  A struct rather than a long parameter list, so that adding an input
 *  later cannot silently reorder the arguments. */
struct VideoEncoderParamInputs {
    /** The configured (negotiated) width in pixels. */
    std::uint32_t width = 0;
    /** The configured (negotiated) height in pixels. */
    std::uint32_t height = 0;
    /** The configured frame rate. */
    std::uint32_t fps = 30;
    /** The configured target bitrate in bits per second. */
    std::uint32_t bitrate_bps = 0;
    /** The configured keyframe interval (frames between IDRs). */
    std::uint32_t keyframe_interval = 60;
    /** The codec's own pixel format (I420 for every backend today). */
    plugins::VideoPixelFormat pixel_format = plugins::VideoPixelFormat::kI420;
    /** The content hint for this stream.
     *
     *  `kUnspecified` for a camera stream; `kDetail` / `kText` for a
     *  screen stream (remote-desktop plan B7 / RD-3). */
    plugins::VideoEncoderParams::ContentHint content_hint =
        plugins::VideoEncoderParams::ContentHint::kUnspecified;

    /** The colour description of the stream (remote-desktop plan C5).
     *
     *  The engine takes it from the *frame* it is about to encode, so
     *  the description always belongs to the pixels actually being
     *  coded. */
    plugins::VideoColorDescription color;

    /** The region of higher fidelity (remote-desktop plan C4), already
     *  scaled into the **output** resolution.
     *
     *  The engine keeps the ROI in capture coordinates and rescales it
     *  with the ladder (see `nimrtc::video_pipeline::RoiScaler`); this
     *  field carries the *scaled* rectangle, because the encoder never
     *  needs to know the capture resolution. */
    std::optional<plugins::VideoRegion> roi;

    /** The capture resolution the ROI's source rectangle is expressed
     *  in.  The engine scales the ROI from this resolution into the
     *  output resolution. */
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;

    /** The smallest ROI (in output pixels) worth asking for.  Zero
     *  disables the floor. */
    std::uint32_t roi_min_size = 0;
};

/** Compute the encoder's parameter set.
 *
 *  The single authority.  Every call site that wants to tell the
 *  encoder something about the stream goes through here, so the engine
 *  and the encoder can never disagree about what was asked for.
 *
 *  @param in  The inputs (configuration + negotiated codec facts).
 *  @return The complete parameter set to push through
 *          `IVideoCodec::set_params()`. */
inline plugins::VideoEncoderParams compute_video_encoder_params(
        const VideoEncoderParamInputs& in) noexcept {
    plugins::VideoEncoderParams p;
    p.width             = in.width;
    p.height            = in.height;
    p.fps               = in.fps ? in.fps : 30u;
    p.bitrate_bps       = in.bitrate_bps;
    p.keyframe_interval = in.keyframe_interval ? in.keyframe_interval : 60u;
    p.pixel_format      = in.pixel_format;
    p.content_hint      = in.content_hint;
    p.color             = in.color;

    // The ROI (plan C4): the source rectangle is in capture pixels, and
    // the encoder wants output pixels.  The scaling rule itself lives in
    // `nimrtc::video_frame` (the exempt module), so the engine and the
    // encoder-side policy (`video_pipeline::RoiScaler`) cannot disagree
    // about it.
    if (in.roi.has_value() && !in.roi->empty()
        && in.capture_width != 0 && in.capture_height != 0) {
        const auto scaled = video_frame::scale_region(in.roi->x,
                                                      in.roi->y,
                                                      in.roi->width,
                                                      in.roi->height,
                                                      in.capture_width,
                                                      in.capture_height,
                                                      p.width,
                                                      p.height,
                                                      in.roi_min_size);
        if (scaled.has_value()) {
            plugins::VideoRegion out;
            out.x = scaled->x;
            out.y = scaled->y;
            out.width = scaled->width;
            out.height = scaled->height;
            p.roi = out;
        }
    }
    // The QP stays unset (255): the engine does not impose a quality
    // target of its own.  The bitrate is the constraint, and the codec's
    // rate control decides the quantiser.  A host that wants a fixed QP
    // can push one through `set_params()` directly.
    p.qp = 255;
    return p;
}

/** True iff two parameter sets differ in a way that requires the encoder
 *  to restart (remote-desktop plan B13).
 *
 *  Only the *geometry* forces a restart: a bitrate or keyframe-interval
 *  change is applied live, and a content-hint or QP change is a tuning
 *  change the codec handles without re-initialising.
 *
 *  @param a  The parameters currently in force.
 *  @param b  The parameters the engine wants to apply.
 *  @return true iff the encoder must be restarted (and must emit the
 *          new parameter set with its next frame). */
inline bool video_params_need_restart(const plugins::VideoEncoderParams& a,
                                      const plugins::VideoEncoderParams& b) noexcept {
    if (!b.has_geometry()) return false;
    return a.width != b.width || a.height != b.height;
}

}  // namespace nimrtc::engine::detail
