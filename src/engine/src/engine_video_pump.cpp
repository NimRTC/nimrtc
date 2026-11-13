/**
 * @file src/engine/src/engine_video_pump.cpp
 * @brief The capture → engine pump (remote-desktop plan A10).
 *
 * ## What this file is
 *
 * The engine's *automatic* video path: it pops one frame from the
 * capture device's bounded queue and drives it through the very same
 * encode → packetize → enqueue chain that `send_video()` uses.  With
 * this in place a capture plug-in plus an encoder produce an outbound
 * RTP stream **without the host ever calling `send_video()`** — which
 * is the acceptance criterion of plan item A10.
 *
 * ## Why the pump lives here, and not in the capture module
 *
 * RD-6: the engine is a single-threaded orchestrator.  The *capture
 * device* owns its own thread and fills its bounded queue (that is the
 * device's business); the *engine* only ever **polls** that queue, and
 * it does so from `tick()`.  So the codec is touched from exactly one
 * thread — the tick thread — no matter what the capture backend does
 * internally.
 *
 * That is also why the pump does *not* install a capture frame
 * callback.  A callback would run on the capture device's thread, and
 * the codec seam is explicitly not thread-safe; encoding there would
 * race with the tick thread's own encode calls.  Polling keeps the
 * "the codec is only ever touched from the tick thread" invariant
 * true, and the test asserts it.
 *
 * ## The frame's lifetime
 *
 * `IVideoCapture::pop_frame()` documents that the pixels stay valid
 * **until the next call to `pop_frame()`**.  The pump therefore does
 * everything with one frame — the staging copy (or the zero-copy
 * encode) — before it pops the next one.  Nothing is retained across
 * the call.
 */
#include "engine_impl.hpp"

namespace nimrtc::engine {

namespace {

/** The video media clock rate, in Hz (RFC 6184 §5.6 for H.264).
 *
 *  The pump stamps the outbound RTP timestamps itself, because the
 *  capture seam reports a *capture* timestamp and not a media clock:
 *  the mapping from one to the other is a property of the session, and
 *  the session is the engine's business. */
constexpr std::uint32_t kVideoClockRateHz = 90000;

}  // anonymous namespace


bool NimRTCEngine::pump_video_capture() noexcept {
    if (!is_open()) return false;
    if (!config_.video_capture_pump) return false;
    if (!video_capture_) return false;
    if (!video_codec_ || !video_sender_) return false;

    // ---- Pop one frame (non-blocking) ----------------------------------
    //
    // The device's queue is empty most of the time: the capture cadence
    // (30 fps) is slower than the tick rate (30–100 Hz).  `pop_frame()`
    // never blocks, so an empty queue is the normal case and costs
    // nothing.
    plugins::CaptureFrame captured{};
    if (video_capture_->pop_frame(captured) != plugins::kOk) {
        return false;
    }

    // ---- The media clock ------------------------------------------------
    //
    // The capture seam reports a *capture* timestamp (microseconds).  The
    // RTP layer needs a *media* clock (90 kHz).  The engine owns the
    // mapping: the first pumped frame establishes the origin, and every
    // later frame is placed relative to it.  The origin is monotonic, so
    // the media clock never jumps backwards even if the device's
    // timestamp does.
    auto& vstate = impl_->video_state;
    if (!vstate.capture_rtp_base_valid) {
        vstate.capture_rtp_base_valid = true;
        vstate.capture_rtp_base_us     = captured.capture_ts_us;
        vstate.capture_rtp_base_ts     = impl_->video_state.video_rtp_timestamp;
    }

    std::int64_t delta_us = captured.capture_ts_us - vstate.capture_rtp_base_us;
    if (delta_us < 0) delta_us = 0;
    const std::uint32_t rtp_timestamp =
        vstate.capture_rtp_base_ts +
        static_cast<std::uint32_t>((delta_us * kVideoClockRateHz) / 1'000'000);

    // ---- Translate the capture frame into a source frame ----------------
    //
    // The capture seam and the source seam are deliberately different
    // shapes (a *device* vs a *frame stream*), so the translation is
    // the pump's job.  The pixels stay where they are: the engine stages
    // its own copy further down the chain (`stage_source_frame`), so
    // the borrowed pointer is only read during this call.
    plugins::VideoSourceFrame frame{};
    frame.width          = captured.width;
    frame.height         = captured.height;
    frame.format         = captured.format;
    frame.capture_ts_us  = captured.capture_ts_us;
    frame.frame_seq      = captured.frame_seq;
    frame.rtp_timestamp  = rtp_timestamp;
    // The colour description travels with the picture (plan C5): the
    // device knows whether it produced full-range (desktop) or
    // limited-range (camera) pixels, and the encoder must code it that
    // way.
    frame.color          = captured.color;

    if (captured.has_gpu_handle && captured.gpu_handle != nullptr) {
        // The zero-copy path: the device produced a native surface and
        // the codec may consume it directly.  The handle is *borrowed* —
        // it stays valid until the next `pop_frame()`, and the encode
        // happens inside this call, so the contract holds.
        frame.gpu_backend      = captured.gpu_backend;
        frame.gpu_handle       = captured.gpu_handle;
        frame.gpu_pitch_bytes  = captured.gpu_pitch_bytes;
        // A surface has no CPU mirror unless the device also filled one.
        if (captured.pixels != nullptr) {
            frame.plane_y   = captured.pixels;
            frame.stride_y  = captured.stride;
            frame.buffer_size = captured.size;
        }
    } else if (captured.pixels != nullptr && captured.width != 0 &&
               captured.height != 0) {
        // The CPU path.  The capture device hands over *one* buffer with
        // *one* stride, so the plane layout is derived from the declared
        // format:
        //
        //   - BGRA: one packed buffer (the desktop-compositor format).
        //   - NV12: the luma plane, then the interleaved UV plane.
        //   - I420 / unknown: the luma plane, then U, then V — each
        //     half-height, half-width.
        //
        // The staging step (`stage_source_frame`) performs the colour
        // conversion when the source is packed, so the engine owns the
        // BGRA → I420 transform instead of asking the device to do it.
        const std::int32_t stride = captured.stride > 0
            ? captured.stride
            : static_cast<std::int32_t>(captured.width * 4u);

        if (captured.format == plugins::VideoPixelFormat::kNV12) {
            frame.plane_y = captured.pixels;
            frame.plane_u = captured.pixels +
                static_cast<std::size_t>(stride) * captured.height;
            frame.stride_y = stride;
            frame.stride_u = stride;
        } else if (captured.format == plugins::VideoPixelFormat::kI420 ||
                   captured.format == plugins::VideoPixelFormat::kYV12) {
            const std::size_t y_size =
                static_cast<std::size_t>(stride) * captured.height;
            const std::size_t c_stride = static_cast<std::size_t>(stride) / 2u;
            const std::size_t c_size = c_stride * (captured.height / 2u);
            frame.plane_y = captured.pixels;
            frame.plane_u = captured.pixels + y_size;
            frame.plane_v = captured.pixels + y_size + c_size;
            frame.stride_y = stride;
            frame.stride_u = static_cast<std::int32_t>(c_stride);
            frame.stride_v = static_cast<std::int32_t>(c_stride);
        } else {
            // BGRA (and anything else packed): one buffer, one stride.
            // `stage_source_frame()` converts it to the codec-facing
            // planar format.
            frame.plane_y = captured.pixels;
            frame.stride_y = stride;
        }
        frame.buffer_size = captured.size;
    } else {
        // A metadata-only frame (a dirty-rect notification with no
        // pixels).  There is nothing to encode — but the frame still
        // counts as "seen", so the counters move.
        ++vstate.video_capture_pumped_metadata;
        return false;
    }

    // ---- Encode and push ------------------------------------------------
    //
    // The *same* code path `send_video()` uses, so the manual and the
    // automatic paths cannot drift apart.  The codec is touched here, on
    // the tick thread — never on the capture device's thread.
    const plugins::Status rc = encode_and_send_video_frame(frame);
    if (rc == plugins::kOk) {
        ++vstate.video_capture_pumped_frames;
    } else {
        ++vstate.video_capture_pump_errors;
    }
    return rc == plugins::kOk;
}

}  // namespace nimrtc::engine
