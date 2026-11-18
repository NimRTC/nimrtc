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

    auto& vstate = impl_->video_state;

    // ---- The device's own bookkeeping ---------------------------------
    //
    // Two things are checked before the frame is popped, because both
    // are about the *device* rather than about a frame:
    //
    //   1. **Did the device lose itself?**  A display that was
    //      unplugged, a portal that was revoked, a compositor that
    //      restarted: the producer side is where the failure happens,
    //      so the queue would simply stay empty and the pump would
    //      look like a device that never delivers anything.  The
    //      device's own error counter is what tells the two apart.
    //
    //   2. **Did the device renegotiate?**  A resolution change or a
    //      format switch is legal, and it is *visible* in the stats
    //      instead of being invisible.
    recover_capture_device();
    publish_capture_device_counters();

    // ---- The zero-copy path (remote-desktop plan B10) -------------------
    //
    // The GPU mailbox is checked **first**: when the capture device is
    // delivering surfaces, that is the freshest picture, and the CPU
    // queue (if the device also fills one) holds an older copy of the
    // same desktop.  Taking the GPU frame and the CPU frame in the
    // same tick would send the desktop twice.
    //
    // The mailbox is drained here, on the **tick thread** — so the
    // codec is touched from exactly one thread, whatever the capture
    // backend does internally (RD-6).
    if (vstate.capture_gpu_wired) {
        if (pump_video_capture_gpu()) {
            return true;
        }
    }

    // ---- Pop one frame (non-blocking) ----------------------------------
    //
    // The device's queue is empty most of the time: the capture cadence
    // (30 fps) is slower than the tick rate (30–100 Hz).  `pop_frame()`
    // never blocks, so an empty queue is the normal case and costs
    // nothing.
    plugins::CaptureFrame captured{};
    // Plan J1: the capture stage — the cost of popping the frame from
    // the device's bounded queue.  On a device with its own producer
    // thread this is the queue handoff, not the desktop duplication
    // itself; the device's own cost is in the capture plugin's stats.
    const auto capture_start = std::chrono::steady_clock::now();
    const auto pop_rc = video_capture_->pop_frame(captured);
    const auto capture_end = std::chrono::steady_clock::now();
    impl_->video_state.stage_timings.record(
        Stage::kCapture,
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                capture_end - capture_start).count()));
    if (pop_rc != plugins::kOk) {
        return false;
    }

    // Plan S7: the capture cadence, from the device's own timestamps.
    // The capture stage's *cost* is measured above; the *rate* it
    // delivers frames at needs the timestamps, which only the device
    // has.  Recorded here so a frames-dropped-before-encode figure can
    // be derived from it later.
    vstate.note_captured_frame(captured.capture_ts_us);

    // ---- The media clock ------------------------------------------------
    //
    // The capture seam reports a *capture* timestamp (microseconds).  The
    // RTP layer needs a *media* clock (90 kHz).  The engine owns the
    // mapping: the first pumped frame establishes the origin, and every
    // later frame is placed relative to it.  The origin is monotonic, so
    // the media clock never jumps backwards even if the device's
    // timestamp does.
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
        //
        // The frame is a **capture contract**: the device negotiated a
        // zero-copy surface, so the picture is in GPU memory and there
        // is no CPU copy to fall back to.  The engine therefore hands
        // the surface to the encoder unconditionally.
        frame.set_gpu_surface(captured.gpu_surface());
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

    // ---- The dirty rectangles (remote-desktop plan B9) ------------------
    //
    // The capture device reports which rectangles changed.  The
    // information is computed by the device, copied once into the
    // capture queue, and would otherwise be *discarded here* — and the
    // last twenty metres are exactly where it matters: "a static
    // desktop costs a full frame every tick" vs. "a static desktop
    // costs nothing".
    //
    // The rectangles are handed to the sender's arbiter, which decides
    // whether the frame can be coded incrementally (it needs the
    // codec's capability, and it is the one that knows the content
    // type).  The decision must be made **before** the encode, because
    // the incremental request is what the encoder acts on.
    offer_dirty_rects_to_sender(captured);

    // ---- Encode and push ------------------------------------------------
    //
    // The *same* code path `send_video()` uses, so the manual and the
    // automatic paths cannot drift apart.  The codec is touched here, on
    // the tick thread — never on the capture device's thread.
    const plugins::Status rc = encode_and_send_video_frame(frame);
    if (rc == plugins::kOk) {
        ++vstate.video_capture_pumped_frames;
        ++vstate.video_capture_frames_pumped;
    } else {
        ++vstate.video_capture_pump_errors;
    }
    return rc == plugins::kOk;
}

// ---------------------------------------------------------------------------
// The device's health (remote-desktop plan A11)
// ---------------------------------------------------------------------------

void NimRTCEngine::recover_capture_device() noexcept {
    if (!video_capture_) return;

    auto& vstate = impl_->video_state;

    // The device's error counter only ever grows, so a plain comparison
    // would fire on every tick.  The baseline is what makes the
    // comparison meaningful.
    const plugins::CaptureStats stats = video_capture_->stats();
    if (stats.device_errors <= vstate.capture_seen_device_errors) return;

    vstate.capture_seen_device_errors = stats.device_errors;
    ++vstate.video_capture_device_errors;

    core::log::Logger::instance().warn(
        "engine: the video capture device reported an error; reopening it");

    // A device that is reopened restarts its counters from zero, so the
    // baselines have to follow -- otherwise the next delta would be a
    // huge underflowed number.
    //
    // The device is **closed first**.  A lost device is torn down and
    // built up again, not "re-opened" in place: `open()` on an already
    // open device is a no-op for most backends (and would leave the
    // stale device behind), so the reopen would silently do nothing.
    video_capture_->close();
    const plugins::Status rc = video_capture_->open();
    if (rc != plugins::kOk) {
        core::log::Logger::instance().error(
            "engine: the video capture device could not be reopened; "
            "the capture pump is stopped");
        if (on_error_) {
            on_error_(core::kVideoCaptureOpenFailed,
                      "engine: the video capture device could not be "
                      "reopened; the capture pump is stopped");
        }
        return;
    }

    ++vstate.video_capture_restarts;

    const plugins::CaptureStats fresh = video_capture_->stats();
    vstate.capture_seen_frames_captured = fresh.frames_captured;
    vstate.capture_seen_frames_dropped  = fresh.frames_dropped;
    vstate.capture_seen_bytes_copied    = fresh.bytes_copied;
    vstate.capture_seen_device_errors  = fresh.device_errors;

    // The device may have renegotiated while it was away.
    const plugins::CaptureConfig again = video_capture_->config();
    vstate.capture_negotiated_width  = again.format.width;
    vstate.capture_negotiated_height = again.format.height;
    vstate.capture_negotiated_tag    = again.format.tag;
}

void NimRTCEngine::publish_capture_device_counters() noexcept {
    if (!video_capture_) return;

    auto& vstate = impl_->video_state;

    // The counters are read *after* the pop and compared against the
    // baseline the pump recorded last time.  Reading them *before* the
    // pop (and taking a delta) would miss everything that happened
    // between the previous tick and this one -- most importantly the
    // frames the device dropped while the queue was full, because those
    // drops are counted by the *producer* thread and never by the pop.
    const plugins::CaptureStats stats = video_capture_->stats();

    if (stats.frames_captured >= vstate.capture_seen_frames_captured) {
        vstate.video_capture_frames +=
            stats.frames_captured - vstate.capture_seen_frames_captured;
        vstate.capture_seen_frames_captured = stats.frames_captured;
    } else {
        // The device was reopened, so its counters restarted.  Take the
        // new value as the baseline rather than counting a huge
        // (underflowed) delta.
        vstate.capture_seen_frames_captured = stats.frames_captured;
    }

    if (stats.frames_dropped >= vstate.capture_seen_frames_dropped) {
        vstate.video_capture_dropped +=
            stats.frames_dropped - vstate.capture_seen_frames_dropped;
        vstate.capture_seen_frames_dropped = stats.frames_dropped;
    } else {
        vstate.capture_seen_frames_dropped = stats.frames_dropped;
    }

    if (stats.bytes_copied >= vstate.capture_seen_bytes_copied) {
        vstate.video_capture_bytes_copied +=
            stats.bytes_copied - vstate.capture_seen_bytes_copied;
        vstate.capture_seen_bytes_copied = stats.bytes_copied;
    } else {
        vstate.capture_seen_bytes_copied = stats.bytes_copied;
    }

    // ---- Did the device renegotiate? ---------------------------------
    const plugins::CaptureConfig now = video_capture_->config();
    if (now.format.width != vstate.capture_negotiated_width
        || now.format.height != vstate.capture_negotiated_height
        || now.format.tag != vstate.capture_negotiated_tag) {
        const bool first = vstate.capture_negotiated_width == 0
                        && vstate.capture_negotiated_height == 0
                        && vstate.capture_negotiated_tag.empty();
        vstate.capture_negotiated_width  = now.format.width;
        vstate.capture_negotiated_height = now.format.height;
        vstate.capture_negotiated_tag    = now.format.tag;
        // The *first* observation is the device's initial negotiation,
        // not a change: it is what the device agreed to at open() time.
        if (!first) {
            ++vstate.video_capture_format_changes;
            core::log::Logger::instance().warn(
                "engine: the video capture device renegotiated its format "
                "(now " + std::to_string(now.format.width) + "x"
                + std::to_string(now.format.height)
                + " tag=" + now.format.tag + ")");
        }
    }
}

// ---------------------------------------------------------------------------
// The zero-copy pump (remote-desktop plan B10)
// ---------------------------------------------------------------------------

void NimRTCEngine::drain_stale_cpu_frames() noexcept {
    if (!video_capture_) return;

    // The bound is a safety net, not a policy: the queue is small (the
    // capture seam's default depth is a handful of frames), and the
    // drain is a one-shot at seam-wiring time.
    constexpr int kMaxDrain = 64;

    plugins::CaptureFrame stale{};
    int drained = 0;
    while (drained < kMaxDrain &&
           video_capture_->pop_frame(stale) == plugins::kOk) {
        ++drained;
    }
    if (drained > 0) {
        core::log::Logger::instance().info(
            "engine: discarded " + std::to_string(drained) +
            " capture frame(s) that were staged to the host before the "
            "zero-copy seam was wired");
    }
}

void NimRTCEngine::wire_capture_gpu_callback() noexcept {
    if (!video_capture_ || !video_codec_) return;

    // ---- Did the host ask for the zero-copy path at all? ------------------
    //
    // `EngineConfig::video_zero_copy` is the host's switch.  When it is
    // off, the CPU staging path is the one that must run — the seam is
    // not wired, the captured pixels go to the host, and the statistics
    // say so.  (This is what makes the CPU fallback *measurable*: the
    // verification program turns the flag off to compare the two.)
    if (!config_.video_zero_copy) return;

    // ---- Can the capture device deliver a surface? ------------------------
    //
    // `supports_gpu_frames()` is the device's own statement: it knows
    // whether its backend has a native surface at all (a DXGI texture, a
    // `CVPixelBuffer`) and whether it is allowed to use it.
    if (!video_capture_->supports_gpu_frames()) return;

    // ---- Can the codec consume one? ---------------------------------------
    //
    // This is the half that decides whether the seam is *usable*.  The
    // device hands over the surface and does **not** keep a CPU copy, so
    // a codec that cannot take a surface would turn every frame into a
    // dropped frame — with no fallback, because there is no mirror.
    //
    // The capability is queried on the *open* codec: an unopened codec
    // has no encode session, and NVENC's answer depends on the session
    // being there.
    if (!video_codec_->supports_zero_copy_encode()) {
        core::log::Logger::instance().info(
            "engine: the capture device can deliver GPU surfaces but the "
            "video codec cannot consume one — the frames take the CPU "
            "staging path instead");
        return;
    }

    // ---- Attach ------------------------------------------------------------
    //
    // The callback runs on the **capture device's own thread**.  It does
    // one thing: hand the surface to the mailbox.  It never touches the
    // codec, never blocks, and never calls back into the host.
    //
    // The mailbox drops the stale frame when a newer one arrives (see
    // `engine_gpu_mailbox.hpp`): the newest picture is the truth, and a
    // screen that changes at 30 fps must not cost a backlog of 30
    // encodes.
    video_capture_->set_gpu_frame_callback(
        [this](plugins::GpuCaptureFrame&& frame) {
            impl_->video_state.gpu_mailbox.push(std::move(frame));
        });

    // The flag makes the seam observable: a host that wants to know
    // whether the zero-copy path is *live* (not merely requested) reads
    // it together with `video_capture_gpu_frames`.
    impl_->video_state.capture_gpu_wired = true;

    // ---- Discard the frames that predate the seam --------------------
    //
    // The capture device was opened *before* the codec existed — the
    // codec has to adopt the capture's device, so the device has to be
    // open first.  The frames the device produced in that window went
    // through the CPU staging path, because the GPU callback did not
    // exist yet; it was the only path available.
    //
    // They are stale: the mailbox delivers the *current* picture within
    // one capture interval.  Dropping them here keeps them out of the
    // encoder — a frame that was staged to the host before the seam was
    // wired has no business being encoded through the CPU path now that
    // the GPU path is live.
    //
    // It does **not** undo the device's own `bytes_copied` accounting:
    // those bytes really were copied to host memory when the frame was
    // staged.  The number the session reports therefore carries a small
    // startup transient, and a *measurement* has to account for it
    // explicitly (the verification program reports the steady state
    // separately).
    //
    // Draining is safe: a device whose GPU staging fails falls back to
    // the CPU path and keeps filling its queue.
    drain_stale_cpu_frames();
}

bool NimRTCEngine::pump_video_capture_gpu() noexcept {
    if (!is_open()) return false;
    if (!video_capture_ || !video_codec_ || !video_sender_) return false;

    auto& vstate = impl_->video_state;

    // ---- Take the pending frame (tick thread) ---------------------------
    //
    // The mailbox holds at most one frame.  If a newer one arrived
    // while this tick was busy, the older one was already dropped (the
    // newest frame is the truth).
    auto pending = vstate.gpu_mailbox.pop();
    if (!pending) return false;

    const plugins::GpuCaptureFrame& frame = *pending;
    if (!frame.gpu_buffer) {
        // A frame without a surface is metadata-only.  It still counts
        // as "seen", so the counters move.
        ++vstate.video_capture_pumped_metadata;
        return false;
    }

    // Plan S7: the GPU path delivers frames too, so it feeds the same
    // cadence record as the CPU path.
    vstate.note_captured_frame(frame.capture_ts_us);

    // ---- The media clock ------------------------------------------------
    //
    // The same rule as the CPU path: the first GPU frame establishes
    // the origin, and every later frame is placed relative to it.  The
    // origin is shared with the CPU path, so a session that switches
    // between the two does not jump.
    if (!vstate.capture_rtp_base_valid) {
        vstate.capture_rtp_base_valid = true;
        vstate.capture_rtp_base_us     = frame.capture_ts_us;
        vstate.capture_rtp_base_ts     = vstate.video_rtp_timestamp;
    }

    std::int64_t delta_us = frame.capture_ts_us - vstate.capture_rtp_base_us;
    if (delta_us < 0) delta_us = 0;
    const std::uint32_t rtp_timestamp =
        vstate.capture_rtp_base_ts +
        static_cast<std::uint32_t>((delta_us * kVideoClockRateHz) / 1'000'000);

    // ---- The dirty rectangles (remote-desktop plan B9) -----------------
    //
    // The GPU path reports the same metadata as the CPU path, so the
    // incremental-encode machinery is identical.
    {
        plugins::CaptureFrame as_capture;
        as_capture.dirty = frame.dirty;
        offer_dirty_rects_to_sender(as_capture);
    }

    // ---- Encode the surface straight into the codec --------------------
    //
    // The surface is handed to the encoder *as is* — no staging copy, no
    // colour conversion, no host round-trip.  The `GpuBuffer` owns the
    // texture and is alive for the whole call, which is the lifetime
    // `encode_zero_copy()` documents.
    if (!video_codec_->supports_zero_copy_encode()) {
        // The codec cannot take a surface.  The frame is lost — there
        // is no CPU mirror to fall back to, and manufacturing one would
        // defeat the path the host asked for.
        ++vstate.video_capture_gpu_errors;
        core::log::Logger::instance().warn(
            "engine: the capture device delivered a GPU surface but the "
            "video codec cannot consume one — the frame was dropped");
        return false;
    }

    plugins::EncodedVideoFrame encoded{};
    const plugins::Status rc = video_codec_->encode_zero_copy(
        *frame.gpu_buffer, encoded);
    if (rc != plugins::kOk) {
        ++vstate.video_capture_gpu_errors;
        core::log::Logger::instance().warn(
            "engine: the zero-copy encode of a captured surface failed "
            "(status=" + std::to_string(static_cast<int>(rc)) + ")");
        return false;
    }

    ++vstate.video_capture_gpu_frames;
    ++vstate.video_format_zero_copy;

    // ---- Packetize and push --------------------------------------------
    //
    // The *same* push path the CPU path uses, so the two cannot drift
    // apart.  The sender owns the RTP stamping and the priority
    // handling.
    const plugins::Status push_rc = push_encoded_video_frame(
        encoded, rtp_timestamp, frame.frame_seq, frame.capture_ts_us);
    if (push_rc == plugins::kOk) {
        ++vstate.video_capture_pumped_frames;
    } else {
        ++vstate.video_capture_pump_errors;
    }
    return push_rc == plugins::kOk;
}

// ---------------------------------------------------------------------------
// The dirty-rect incremental encode (remote-desktop plan B9)
// ---------------------------------------------------------------------------

void NimRTCEngine::offer_dirty_rects_to_sender(
        const plugins::CaptureFrame& captured) noexcept {
    if (!video_sender_) return;

    // The mechanism is opt-in and additionally gated on the content type
    // (ADR-022 Decision 5): it only makes sense for screen content.  The
    // sender's arbiter owns the second half of that gate, but the engine
    // checks the flag first so a disabled mechanism costs nothing at
    // all.
    if (!config_.video_incremental_encode) return;
    if (config_.video_content_type != plugins::VideoContentType::kScreen) {
        return;
    }

    auto& vstate = impl_->video_state;

    // ---- The SSRCs the region-refresh message names ----------------------
    //
    // The media SSRC is the outbound video stream's SSRC; the feedback
    // SSRC is the engine's own RTCP reporter SSRC (RFC 3550 §6.1 — the
    // SSRC identifies the *originator* of the message).  They are
    // handed to the sender once, when the sender exists.
    if (!vstate.video_region_ssrcs_set) {
        vstate.video_media_ssrc = config_.video_sender_tuning.ssrc;
        vstate.video_region_feedback_ssrc = rtcp_reporter_ssrc();
        vstate.video_region_ssrcs_set = true;
    }

    // ---- Translate the rectangles ----------------------------------------
    //
    // `CaptureRegion` and `VideoRegion` carry the same four numbers (a
    // rectangle in source pixels), but they are distinct types on
    // distinct seams, so the translation is explicit rather than a
    // reinterpret_cast.  The vector is reused across frames (it lives in
    // the video state), so the steady state allocates nothing.
    vstate.dirty_rects.clear();
    vstate.dirty_rects.reserve(captured.dirty.rects.size());
    for (const auto& r : captured.dirty.rects) {
        plugins::VideoRegion vr{};
        vr.x      = r.x;
        vr.y      = r.y;
        vr.width  = r.width;
        vr.height = r.height;
        vstate.dirty_rects.push_back(vr);
    }

    // ---- The decision ----------------------------------------------------
    //
    // The sender's arbiter owns the decision: it knows the content type,
    // the codec's capability, and whether the mechanism is enabled.  The
    // engine's job is to hand over the facts and to publish the result.
    const plugins::VideoIncrementalDecision decision =
        video_sender_->offer_dirty_region(vstate.dirty_rects,
                                          captured.dirty.full_frame);

    // ---- The region-refresh message --------------------------------------
    //
    // The peer has to be told which region changed — otherwise it cannot
    // invalidate it, and the incremental encode would leave the peer
    // showing a stale area.  The sender builds the message (it knows the
    // media SSRC and the regions it just coded); the engine sends it,
    // because the engine owns the RTCP transport.
    if (decision == plugins::VideoIncrementalDecision::kIncremental) {
        const core::ByteSpan refresh = video_sender_->take_region_refresh();
        if (!refresh.empty()) {
            // The message goes out through the same path as every other
            // RTCP the engine emits (protected, then handed to ICE).
            core::ByteBuffer packet(refresh.begin(), refresh.end());
            if (send_rtcp_packet(vstate.video_region_feedback_ssrc, packet)) {
                ++vstate.video_region_refreshes_sent;
            }
        }
    }
}


}  // namespace nimrtc::engine
