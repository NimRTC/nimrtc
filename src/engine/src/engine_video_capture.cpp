/**
 * @file src/engine/src/engine_video_capture.cpp
 * @brief The capture -> engine pump (remote-desktop plan A10).
 *
 * Before this file the engine's video path had exactly one way in: the
 * host pulled a frame and pushed it through `send_video()`.  That is a
 * fine contract for a host that owns the camera, but it makes the
 * *capture device* seam unusable: a device is asynchronous (it fills a
 * bounded queue from its own thread), and a host that had to poll it
 * would have to know the device's cadence, its drop policy and its
 * frame lifetime.
 *
 * So the engine grows a pump:
 *
 * @code
 *   IVideoCapture  --device-->  bounded queue  --pop-->  pump  --encode-->
 *   (platform)                                        (tick thread)
 * @endcode
 *
 * The pump runs on the **engine's tick thread**, never on a thread of
 * its own (RD-6).  That is the whole point: `IVideoCodec` and
 * `IVideoSender` are not thread-safe, and the only thread allowed to
 * touch them is the one that also owns the RTP/SRTP scheduler.  A
 * capture plug-in may own a thread (DXGI's duplication event,
 * ScreenCaptureKit's stream, PipeWire's buffer) — that is the plug-in's
 * business; the engine only ever *pops* a frame that is already there,
 * and `pop_frame()` never blocks.
 *
 * ## The conversion
 *
 * The capture seam delivers a device-native pixel layout (BGRA on most
 * desktop compositors).  The encoder wants I420.  That conversion is
 * the portable half of the capture layer, so it lives in the capture
 * module's shared adapter (`nimrtc::video_capture::CaptureSourceAdapter`)
 * — one mechanism that every platform backend shares, not three
 * parallel implementations of the same maths (the plan's RD-10).
 *
 * The engine reaches the adapter through the *source* seam: it holds it
 * as an `IVideoSource` and drives it with `produce_one()`.  From the
 * engine's point of view a capture device and a test-pattern source are
 * the same thing — a source of frames.  The adapter's callback hands
 * the engine a plain `VideoSourceFrame`, which is exactly what
 * `send_video()` takes, so the manual path and the automatic path share
 * **one** encode implementation and cannot drift apart.
 *
 * ## What the pump does NOT do
 *
 *   - It does not create a thread.  The engine is still a
 *     single-threaded orchestrator.
 *   - It does not renegotiate the codec's resolution.  The device
 *     negotiates its own format and the encoder is configured
 *     separately; a mismatch is reported, not papered over (the
 *     scaler is plan item C1).
 *   - It does not replace `send_video()`.  The manual path stays
 *     exactly as it was; the pump is the *automatic* path.
 */

#include "engine_impl.hpp"

#ifdef NIMRTC_HAS_VIDEO_CAPTURE
#include <nimrtc/video_capture/capture_source_adapter.hpp>
#endif

#include <utility>

namespace nimrtc::engine {

namespace {

/** The pixel format the capture adapter converts into.
 *
 *  I420 is what every encoder in the tree accepts, and it is the
 *  format the engine's own `stage_source_frame()` understands. */
constexpr plugins::VideoPixelFormat kCaptureOutputFormat =
    plugins::VideoPixelFormat::kI420;

/** The RTP media clock rate (RFC 3550 §4.1). */
constexpr std::uint32_t kRtpClockRateHz = 90000u;

} // anonymous namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void NimRTCEngine::init_video_capture() noexcept {
    if (video_capture_) return;
    if (config_.video_capture_name.empty()) return;

#ifdef NIMRTC_HAS_VIDEO_CAPTURE
    // ---- Resolve the factory ------------------------------------------
    //
    // A missing factory is a *configuration* error: the host named a
    // plug-in that does not exist.  It is reported as such (and not
    // silently ignored), because a remote-desktop host that believes
    // it is capturing a screen when it is not is worse than one that
    // gets an error.
    const plugins::IVideoCaptureFactory* factory =
        pal::resolve_video_capture(config_.video_capture_name);
    if (factory == nullptr) {
        const std::string msg =
            std::string{"engine: video capture plugin '"}
                + config_.video_capture_name
                + "' not registered (remote-desktop plan A10: the capture "
                  "device is what produces the outbound video frames)";
        core::log::Logger::instance().error(msg);
        if (on_error_) on_error_(core::kVideoCapturePluginMissing, msg);
        return;
    }

    plugins::IVideoCapture* device =
        factory->create(config_.video_capture_config);
    if (device == nullptr) {
        const std::string msg =
            std::string{"engine: video capture plugin '"}
                + config_.video_capture_name
                + "' returned no device";
        core::log::Logger::instance().error(msg);
        if (on_error_) on_error_(core::kVideoCapturePluginMissing, msg);
        return;
    }
    video_capture_.reset(device);

    // ---- Wrap the device in the shared adapter ------------------------
    //
    // The adapter does not own the device: the engine keeps the device
    // alive across a reopen, which an owning adapter could not survive.
    // The engine holds the adapter through the *source* seam, so the
    // engine's own members never mention a concrete capture type.
    plugins::VideoSourceConfig src_cfg{};
    src_cfg.format = kCaptureOutputFormat;
    src_cfg.ssrc   = config_.video_sender_tuning.ssrc;

    auto* adapter = new video_capture::CaptureSourceAdapter(
        video_capture_.get(), src_cfg);
    video_capture_source_.reset(adapter);

    // The adapter's callback is the pump's delivery path.  It fires
    // synchronously inside `produce_one()`, which the pump calls from
    // the tick thread — so the codec is only ever touched from there.
    video_capture_source_->set_callback(
        [this](const plugins::VideoSourceFrame& frame) {
            pump_deliver_frame(frame);
        });

    // ---- Open the device ----------------------------------------------
    //
    // Opening is the *negotiation*: the device reports back the format
    // it actually agreed to, and the engine has to read it before it
    // can log a useful diagnosis (and before it can tell a caller what
    // resolution to expect).
    //
    // A device that refuses to open is a hard error with its own code
    // (a permission denial on macOS, a display that went away on
    // Windows, a missing portal on Linux) — the plan's A4 asks for
    // exactly that distinction.
    const plugins::Status open_rc = video_capture_source_->open();
    if (open_rc != plugins::kOk) {
        // The device refused to open.  Two *different* things can
        // cause that, and the host must be able to tell them apart:
        //
        //   - a **permission denial** (the macOS Screen Recording
        //     prompt was answered with "no"; a portal's picker was
        //     dismissed) — only the *user* can fix this, and the host
        //     should say so rather than reporting a broken device;
        //   - anything else (a display that went away, a missing
        //     portal) — the host's or the platform's problem.
        const std::string msg =
            std::string{"engine: video capture device '"}
                + config_.video_capture_name
                + "' failed to open";
        core::log::Logger::instance().error(msg);
        if (on_error_) {
            if (open_rc == plugins::kErrPermissionDenied) {
                on_error_(core::kVideoCapturePermissionDenied,
                          msg + " (the screen-recording permission was not "
                                "granted)");
            } else {
                on_error_(core::kVideoCaptureOpenFailed, msg);
            }
        }
        shutdown_video_capture();
        return;
    }

    // ---- Remember what the device negotiated --------------------------
    //
    // The device is the authority on its own geometry.  The engine
    // records it so a later renegotiation is visible in the stats
    // instead of being invisible.
    const plugins::CaptureConfig negotiated = video_capture_->config();
    capture_pump_.width  = negotiated.format.width;
    capture_pump_.height = negotiated.format.height;
    capture_pump_.tag    = negotiated.format.tag;
    capture_pump_.fps    = negotiated.format.fps != 0
                         ? negotiated.format.fps : 30u;
    capture_pump_.running = true;

    // The device's counters are the pump's baseline.  Whatever the
    // device has counted *before* the engine took it over belongs to
    // the device, not to the engine's session.
    const plugins::CaptureStats baseline = video_capture_->stats();
    capture_pump_.seen_frames_captured = baseline.frames_captured;
    capture_pump_.seen_frames_dropped  = baseline.frames_dropped;
    capture_pump_.seen_bytes_copied    = baseline.bytes_copied;

    core::log::Logger::instance().info(
        std::string{"engine: video capture device '"}
            + config_.video_capture_name
            + "' opened at " + std::to_string(negotiated.format.width)
            + "x" + std::to_string(negotiated.format.height)
            + " (tag=" + negotiated.format.tag + ")");
#else
    // The capture module is not built into this configuration.  The
    // registry slot exists, but the portable capture layer — the
    // adapter that turns a device's frames into the frame stream the
    // engine consumes — does not, so there is nothing to pump.
    const std::string msg =
        std::string{"engine: video capture plugin '"}
            + config_.video_capture_name
            + "' cannot be used: this build has no video-capture module "
              "(NIMRTC_MODULE_VIDEO_CAPTURE=OFF)";
    core::log::Logger::instance().error(msg);
    if (on_error_) on_error_(core::kVideoCapturePluginMissing, msg);
#endif
}

void NimRTCEngine::shutdown_video_capture() noexcept {
    capture_pump_ = CapturePumpState{};
    if (video_capture_source_) {
        video_capture_source_->close();
        video_capture_source_.reset();
    }
    if (!video_capture_) return;
    video_capture_->close();
    video_capture_.reset();
}

// ---------------------------------------------------------------------------
// The pump
// ---------------------------------------------------------------------------

void NimRTCEngine::pump_deliver_frame(
        const plugins::VideoSourceFrame& frame) noexcept {
    // The pump's delivery path.  It runs on the tick thread (the
    // adapter's callback fires synchronously inside `produce_one()`).
    //
    // The RTP timestamp is the engine's job here: the capture seam
    // reports a *capture* timestamp and a frame sequence number, but the
    // media clock belongs to the RTP layer.  The engine advances it by
    // one frame interval per delivered frame, so a dropped capture
    // frame leaves a gap in the media clock rather than a duplicated
    // timestamp.
    plugins::VideoSourceFrame staged = frame;
    staged.rtp_timestamp = capture_pump_.rtp_timestamp;
    capture_pump_.rtp_timestamp +=
        kRtpClockRateHz / (capture_pump_.fps != 0 ? capture_pump_.fps : 30u);

    const plugins::Status rc = send_video(staged);
    if (rc != plugins::kOk) {
        ++impl_->video_capture_dropped;
        return;
    }
    ++capture_pump_.pumped;
    ++impl_->video_capture_frames_pumped;
}

void NimRTCEngine::pump_video_capture() noexcept {
    if (!video_capture_ || !video_capture_source_) return;
    if (!config_.video_capture_auto_pump) return;
    if (!capture_pump_.running) return;

    // The codec and the sender are the sinks of this pump.  Without
    // them there is nothing to do — and touching them would be a null
    // dereference.
    if (!video_codec_ || !video_sender_) return;

    // ---- Has the device reported an error? ----------------------------
    //
    // A device that is *lost* (a display unplugged, a portal revoked,
    // a compositor restarted) reports it through its own counters — the
    // producer side is where the failure happens, so the pump would
    // otherwise only ever see an empty queue.
    //
    // The device is reopened once, so a transient loss does not end the
    // session; a device that cannot be reopened is reported and the
    // pump stops rather than spinning on a broken device.
    const plugins::CaptureStats dev_stats = video_capture_->stats();
    if (dev_stats.device_errors > capture_pump_.seen_device_errors) {
        capture_pump_.seen_device_errors = dev_stats.device_errors;
        ++impl_->video_capture_device_errors;
        core::log::Logger::instance().warn(
            "engine: the video capture device reported an error; "
            "reopening it");

        video_capture_source_->stop();
        if (video_capture_source_->open() != plugins::kOk) {
            capture_pump_.running = false;
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
        ++capture_pump_.restarts;
        ++impl_->video_capture_restarts;

        // The device was reopened, so its counters restarted from zero.
        // The pump's baselines must follow, or the next delta would be
        // a huge underflowed number.
        const plugins::CaptureStats fresh = video_capture_->stats();
        capture_pump_.seen_frames_captured = fresh.frames_captured;
        capture_pump_.seen_frames_dropped  = fresh.frames_dropped;
        capture_pump_.seen_bytes_copied    = fresh.bytes_copied;

        // The device may have renegotiated while it was away.
        const plugins::CaptureConfig again = video_capture_->config();
        capture_pump_.width  = again.format.width;
        capture_pump_.height = again.format.height;
        capture_pump_.tag    = again.format.tag;
        if (again.format.fps != 0) capture_pump_.fps = again.format.fps;
    }

    // ---- Pop one frame ------------------------------------------------
    //
    // `produce_one()` pops at most one frame from the device's queue
    // and publishes it through the adapter's callback.  It never
    // blocks: an empty queue is simply "nothing to do this tick", and a
    // quiet device must not park the engine's tick loop.
    //
    // The frame count moves with the device's own counter, so the
    // stats snapshot reports what the device *captured*, not what the
    // pump happened to consume.
    //
    // The counters are read *after* the pop and compared against the
    // baseline the pump recorded last time.  Reading them *before*
    // the pop (and taking a delta) would miss everything that
    // happened between the previous tick and this one — most
    // importantly the frames the device dropped while the queue was
    // full, because those drops are counted by the *producer* thread
    // and never by the pop.
    video_capture_source_->produce_one();

    const plugins::CaptureStats after = video_capture_->stats();
    if (after.frames_captured >= capture_pump_.seen_frames_captured) {
        impl_->video_capture_frames +=
            after.frames_captured - capture_pump_.seen_frames_captured;
        capture_pump_.seen_frames_captured = after.frames_captured;
    } else {
        // The device was reopened, so its counters restarted.  Take
        // the new value as the baseline rather than counting a huge
        // (underflowed) delta.
        capture_pump_.seen_frames_captured = after.frames_captured;
    }
    if (after.frames_dropped >= capture_pump_.seen_frames_dropped) {
        impl_->video_capture_dropped +=
            after.frames_dropped - capture_pump_.seen_frames_dropped;
        capture_pump_.seen_frames_dropped = after.frames_dropped;
    } else {
        capture_pump_.seen_frames_dropped = after.frames_dropped;
    }
    if (after.bytes_copied >= capture_pump_.seen_bytes_copied) {
        impl_->video_capture_bytes_copied +=
            after.bytes_copied - capture_pump_.seen_bytes_copied;
        capture_pump_.seen_bytes_copied = after.bytes_copied;
    } else {
        capture_pump_.seen_bytes_copied = after.bytes_copied;
    }

    // ---- Has the device renegotiated? ---------------------------------
    //
    // A device may change its format mid-stream (a resolution change,
    // a switch to a different surface).  The change is counted and
    // reported; the encoder's own resolution is the host's business.
    const plugins::CaptureConfig now = video_capture_->config();
    if (now.format.width != capture_pump_.width
        || now.format.height != capture_pump_.height
        || now.format.tag != capture_pump_.tag) {
        capture_pump_.width  = now.format.width;
        capture_pump_.height = now.format.height;
        capture_pump_.tag    = now.format.tag;
        ++impl_->video_capture_format_changes;
        core::log::Logger::instance().warn(
            "engine: the video capture device renegotiated its format "
            "(now " + std::to_string(now.format.width) + "x"
            + std::to_string(now.format.height) + " tag=" + now.format.tag
            + ")");
    }
}

} // namespace nimrtc::engine
