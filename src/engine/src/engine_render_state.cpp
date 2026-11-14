/**
 * @file src/engine/src/engine_render_state.cpp
 * @brief The engine-side wiring of the receive-side render state
 *        (remote-desktop plan E5 / E6 / E7 / E9).
 *
 * Four features live here:
 *
 *   - **E5 — the render clock.**  The engine asks the
 *     `VideoSyncController` what to do with each decoded frame, and
 *     publishes the counters.  The skew the controller consumes is the
 *     one the ADR-017 timeline already produces
 *     (`last_sender_recv_skew_us()`), so the engine does **not** grow a
 *     second skew model (RD-9).
 *   - **E6 — the input echo.**  The engine owns the *switch*, the
 *     *staleness* rule and the *rate limit*; the payload is opaque
 *     (the host decides what an input event looks like).
 *   - **E7 — the presentation.**  The engine resolves the requested
 *     target size and the aspect policy into the sink's
 *     `VideoSinkConfig`.
 *   - **E9 — the displayed-frame record.**  The engine records, per
 *     displayed frame, the capture timestamp and the `ref_frame` id.
 *
 * This file is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * @see engine_render_state.hpp — the state itself.
 * @see docs/plan/remote-desktop-plan.md §Track E.
 */
#include "nimrtc/engine/engine.hpp"

#include "engine_impl.hpp"
#include "engine_render_state.hpp"
#include "video_aspect_fit.hpp"

#include <chrono>
#include <utility>

#include <nimrtc/core/log.hpp>

namespace nimrtc::engine {

namespace {

/// The local monotonic instant, in microseconds.
std::int64_t now_us() noexcept {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            core::SteadyClock::now().time_since_epoch()).count());
}

/// Translate the engine's public aspect policy into the sink seam's.
plugins::VideoScaleMode to_scale_mode(VideoAspectPolicy policy) noexcept {
    switch (policy) {
        case VideoAspectPolicy::kLetterbox: return plugins::VideoScaleMode::kLetterbox;
        case VideoAspectPolicy::kCrop:      return plugins::VideoScaleMode::kCrop;
        case VideoAspectPolicy::kStretch:   return plugins::VideoScaleMode::kStretch;
    }
    return plugins::VideoScaleMode::kLetterbox;
}

}  // namespace

// ---------------------------------------------------------------------------
// E5 — the render clock
// ---------------------------------------------------------------------------

void NimRTCEngine::set_video_sync_tolerance(std::int32_t tolerance_ms,
                                            std::int32_t max_delay_ms) noexcept {
    if (impl_ == nullptr) return;
    detail::VideoSyncConfig cfg = impl_->render.sync.config();
    cfg.tolerance_ms = tolerance_ms < 0 ? 0 : tolerance_ms;
    cfg.max_delay_ms  = max_delay_ms  < 0 ? 0 : max_delay_ms;
    impl_->render.sync.configure(cfg);
}

VideoSyncStats NimRTCEngine::video_sync_stats() const noexcept {
    VideoSyncStats out;
    if (impl_ == nullptr) return out;
    const auto& c = impl_->render.sync.counters();
    out.frames_presented    = c.frames_presented;
    out.frames_dropped_sync = c.frames_dropped;
    out.frames_delayed      = c.frames_delayed;
    out.total_delay_us      = c.total_delay_us;
    out.last_skew_ms        = static_cast<double>(c.last_skew_us) / 1000.0;
    return out;
}

bool NimRTCEngine::apply_render_sync(std::int64_t capture_ts_us,
                                     std::int64_t* delay_us) noexcept {
    if (delay_us) *delay_us = 0;
    if (impl_ == nullptr) return true;

    // ---- The reference (plan E5) ------------------------------------------
    //
    // The comparison needs *two* things: the picture's capture instant
    // and the audio playout's capture instant.  Both live on the
    // ADR-017 timeline — the peer's clock — so the difference between
    // them is a real skew and not an artefact of two unrelated clocks.
    //
    // With no audio reference yet there is nothing to align against, so
    // every frame is presented: a session that has not received audio
    // yet must still show its picture.
    const std::int64_t audio_capture_us =
        impl_->render.audio_playout_capture_us;
    if (audio_capture_us == 0) return true;
    if (capture_ts_us == 0) return true;

    // The inter-clock skew (the difference between the peer's capture
    // clock and our own) is the one the Sender Report tracker already
    // reconstructed.  The engine does **not** derive a second one
    // (RD-9) — it consumes this one.
    //
    // A session that has not received a Sender Report yet has no skew,
    // and the picture is presented unchanged.
    const auto skew = last_sender_recv_skew_us();
    if (!skew.has_value()) return true;

    // The skew between the *picture* and the *sound*: the picture was
    // captured at `capture_ts_us` and the sound that belongs with it is
    // at the speaker now (`audio_capture_us`).  A positive value means
    // the picture is older than the sound — the picture has to wait.
    const std::int64_t picture_skew_us =
        audio_capture_us - capture_ts_us + *skew;

    const detail::VideoSyncOutcome outcome =
        impl_->render.sync.decide(picture_skew_us, capture_ts_us);

    switch (outcome.decision) {
        case detail::VideoSyncDecision::kDrop:
            // The picture is *ahead* of the sound: showing it would put
            // the image on the screen before the audio it belongs to.
            return false;

        case detail::VideoSyncDecision::kDelay:
            // The picture is *behind* the sound: hold it for the
            // difference.  The hold is bounded by the controller's cap
            // (and by the debt it has already promised), so it cannot
            // grow without bound.
            if (delay_us) *delay_us = outcome.delay_us;
            return true;

        case detail::VideoSyncDecision::kPresent:
        default:
            return true;
    }
}

// ---------------------------------------------------------------------------
// E6 — the input echo
// ---------------------------------------------------------------------------

void NimRTCEngine::set_input_echo_enabled(bool enabled) noexcept {
    if (impl_ == nullptr) return;
    impl_->render.echo.set_enabled(enabled);
}

bool NimRTCEngine::input_echo_enabled() const noexcept {
    return impl_ != nullptr && impl_->render.echo.enabled();
}

void NimRTCEngine::set_input_echo_limits(std::int64_t max_age_us,
                                         std::uint32_t max_rate_hz) noexcept {
    if (impl_ == nullptr) return;
    impl_->render.echo.set_max_age_us(max_age_us);
    impl_->render.echo.set_max_rate_hz(max_rate_hz);
}

void NimRTCEngine::set_input_echo_callback(InputEchoCallback cb) noexcept {
    on_input_echo_ = std::move(cb);
}

bool NimRTCEngine::submit_input_echo(const InputEchoEvent& event,
                                      std::int64_t now_us) noexcept {
    if (impl_ == nullptr) return false;

    const detail::InputEchoReject verdict =
        impl_->render.echo.decide(now_us, event.timestamp_us);
    if (verdict != detail::InputEchoReject::kNone) return false;

    // The accepted echo leaves the engine through the *callback seam*:
    // the engine does not encode it (the event vocabulary belongs to
    // the `rd_input` module, which the engine may not include — Layout
    // Invariant 4), and it does not put it on the control channel by
    // itself (the G-track owns that wiring).
    if (on_input_echo_) {
        on_input_echo_(event);
    }
    return true;
}

NimRTCEngine::InputEchoStats NimRTCEngine::input_echo_stats() const noexcept {
    InputEchoStats out;
    if (impl_ == nullptr) return out;
    const auto& c = impl_->render.echo.counters();
    out.echoed           = c.echoed;
    out.suppressed_stale = c.suppressed_stale;
    out.suppressed_rate  = c.suppressed_rate;
    return out;
}

// ---------------------------------------------------------------------------
// E7 — the presentation
// ---------------------------------------------------------------------------

plugins::VideoSinkConfig NimRTCEngine::resolve_sink_config() const noexcept {
    plugins::VideoSinkConfig cfg;
    if (impl_ == nullptr) return cfg;

    if (!impl_->render.presentation_configured) return cfg;

    cfg.width      = impl_->render.presentation_width;
    cfg.height     = impl_->render.presentation_height;
    cfg.scale_mode = to_scale_mode(impl_->render.presentation_policy);
    return cfg;
}

void NimRTCEngine::set_video_presentation(std::uint32_t width,
                                          std::uint32_t height,
                                          VideoAspectPolicy policy) noexcept {
    if (impl_ == nullptr) return;
    impl_->render.presentation_width  = width;
    impl_->render.presentation_height = height;
    impl_->render.presentation_policy = policy;
    impl_->render.presentation_configured = (width != 0 && height != 0);

    // A sink that is *already live* has to learn about the new
    // presentation: the viewer resized the window, and the picture must
    // follow (plan E7).  The sink seam carries the reconfigure
    // (`IVideoSink::set_config()`), so the engine does not have to
    // rebuild the whole video pipeline for a resize.
    //
    // The new configuration is the sink's *current* one with the
    // presentation fields replaced: the sink owns the other fields
    // (the native window, the offscreen flag, the presentation
    // cadence), and a wholesale replacement would silently drop them.
    if (video_sink_) {
        plugins::VideoSinkConfig cfg = video_sink_->config();
        cfg.width  = width;
        cfg.height = height;
        cfg.scale_mode = to_scale_mode(policy);

        const plugins::Status rc = video_sink_->set_config(cfg);
        if (rc != plugins::kOk) {
            core::log::Logger::instance().warn(
                "engine[render]: the video sink refused the new "
                "presentation (it keeps the previous one)");
        }
    }

    core::log::Logger::instance().info(
        "engine[render]: the presentation is " +
        std::to_string(width) + "x" + std::to_string(height) +
        " (aspect policy " +
        std::to_string(static_cast<int>(policy)) + ")");
}

VideoAspectPolicy NimRTCEngine::video_aspect_policy() const noexcept {
    if (impl_ == nullptr) return VideoAspectPolicy::kLetterbox;
    return impl_->render.presentation_policy;
}

// ---------------------------------------------------------------------------
// E9 — the displayed-frame record
// ---------------------------------------------------------------------------

std::optional<DisplayedFrameInfo>
NimRTCEngine::last_displayed_frame() const noexcept {
    if (impl_ == nullptr) return std::nullopt;
    const auto& rec = impl_->render.last_displayed;
    if (!rec.has_value()) return std::nullopt;

    DisplayedFrameInfo out;
    out.capture_ts_us  = rec->capture_ts_us;
    out.ref_frame_id   = rec->ref_frame_id;
    out.frame_seq      = rec->frame_seq;
    out.rendered_at_us = rec->rendered_at_us;
    out.has_ref_frame  = rec->has_ref_frame;
    return out;
}

std::optional<DisplayedFrameInfo>
NimRTCEngine::displayed_frame_for_ref_frame(
        std::uint64_t ref_frame_id) const noexcept {
    if (impl_ == nullptr) return std::nullopt;
    for (auto it = impl_->render.displayed_history.rbegin();
         it != impl_->render.displayed_history.rend(); ++it) {
        if (!it->has_ref_frame) continue;
        if (it->ref_frame_id != ref_frame_id) continue;
        DisplayedFrameInfo out;
        out.capture_ts_us  = it->capture_ts_us;
        out.ref_frame_id   = it->ref_frame_id;
        out.frame_seq      = it->frame_seq;
        out.rendered_at_us = it->rendered_at_us;
        out.has_ref_frame  = it->has_ref_frame;
        return out;
    }
    return std::nullopt;
}

void NimRTCEngine::note_displayed_frame(std::int64_t capture_ts_us,
                                        std::uint32_t frame_seq) noexcept {
    if (impl_ == nullptr) return;

    detail::DisplayedFrameRecord rec;
    rec.capture_ts_us  = capture_ts_us;
    rec.frame_seq      = frame_seq;
    rec.rendered_at_us = now_us();

    // The `ref_frame` id is the peer's `frame_seq` — the same id space
    // the input events carry (plan G5 / E9).  The engine recorded the
    // mapping when the frame arrived (the `record_video_ref_frame()`
    // call in the receive path), so the id is resolvable.
    //
    // The *capture timestamp* is the more precise key: it is what the
    // ADR-017 timeline reconstructs, and it is the value the reverse
    // lookup has to agree with.  So the id is resolved *from* the
    // timestamp when the table knows it — the timestamp is the
    // timeline's own identity for the frame.
    const std::uint64_t ref_frame_id = static_cast<std::uint64_t>(frame_seq);
    rec.ref_frame_id  = ref_frame_id;
    rec.has_ref_frame = ref_frame_to_capture_ts(ref_frame_id).has_value();

    impl_->render.last_displayed = rec;

    // The bounded history: the operator's click may lag the display by
    // a frame or two, so a *recent* frame must still be resolvable by
    // its id.  The ring is bounded, so it cannot grow without bound.
    impl_->render.displayed_history.push_back(rec);
    while (impl_->render.displayed_history.size() >
           detail::RenderState::kDisplayedHistory) {
        impl_->render.displayed_history.pop_front();
    }
}

}  // namespace nimrtc::engine
