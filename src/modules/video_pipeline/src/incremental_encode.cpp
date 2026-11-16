/**
 * @file src/modules/video_pipeline/src/incremental_encode.cpp
 * @brief Dirty-rect incremental encode — remote-desktop plan B9.
 */

#include <nimrtc/video_pipeline/incremental_encode.hpp>

#include <nimrtc/video_rtcp_feedback/video_rtcp_feedback.hpp>

namespace nimrtc::video_pipeline {

// ---------------------------------------------------------------------------
// The vocabulary
// ---------------------------------------------------------------------------

std::string_view video_content_type_name(VideoContentType type) noexcept {
    switch (type) {
        case VideoContentType::kUnspecified: return "unspecified";
        case VideoContentType::kCamera:      return "camera";
        case VideoContentType::kScreen:      return "screen";
    }
    return "unknown";
}

bool is_screen_content(VideoContentType type) noexcept {
    return type == VideoContentType::kScreen;
}

std::string_view incremental_decision_name(
        IncrementalDecision decision) noexcept {
    switch (decision) {
        case IncrementalDecision::kNoChange:          return "no-change";
        case IncrementalDecision::kFullFrame:         return "full-frame";
        case IncrementalDecision::kIncremental:       return "incremental";
        case IncrementalDecision::kFallbackKeyframe:  return "fallback-keyframe";
        case IncrementalDecision::kNotScreenContent:  return "not-screen-content";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// IncrementalEncodeController
// ---------------------------------------------------------------------------

IncrementalEncodeController::IncrementalEncodeController() noexcept = default;

void IncrementalEncodeController::set_content_type(VideoContentType type) noexcept {
    content_type_ = type;
}

void IncrementalEncodeController::set_codec_supports_incremental(
        bool supported) noexcept {
    codec_supports_incremental_ = supported;
}

void IncrementalEncodeController::set_enabled(bool enabled) noexcept {
    enabled_ = enabled;
}

bool IncrementalEncodeController::enabled() const noexcept {
    return enabled_ && is_screen_content(content_type_);
}

void IncrementalEncodeController::reset() noexcept {
    request_ = plugins::VideoIncrementalEncodeRequest{};
    regions_.clear();
    stats_ = IncrementalEncodeStats{};
}

IncrementalDecision IncrementalEncodeController::decide(
        const std::vector<plugins::VideoRegion>& dirty,
        bool full_frame) noexcept {
    ++stats_.frames_seen;

    // The request is rebuilt from scratch every time: a stale request
    // left on the encoder would make the *next* frame incremental too,
    // which is exactly the kind of silent state bug this mechanism must
    // not have.
    request_ = plugins::VideoIncrementalEncodeRequest{};
    regions_.clear();

    // ---- The screen-content gate ------------------------------------------
    //
    // The mechanism is a *screen-content* optimisation (plan B9).  A
    // camera stream has no unchanged area worth skipping, so the
    // mechanism never engages for it — even with a codec that supports
    // it.  The gate comes first, before the codec check, so the
    // "skipped because it is not screen content" counter is honest.
    if (!is_screen_content(content_type_)) {
        ++stats_.skipped_not_screen;
        last_decision_ = IncrementalDecision::kNotScreenContent;
        return IncrementalDecision::kNotScreenContent;
    }

    if (!enabled_) {
        ++stats_.skipped_not_screen;
        last_decision_ = IncrementalDecision::kNotScreenContent;
        return IncrementalDecision::kNotScreenContent;
    }

    // ---- Nothing changed ---------------------------------------------------
    //
    // The static-desktop case: the capture device reported no dirty
    // rectangle.  There is nothing to encode and nothing to signal.
    if (!full_frame && dirty.empty()) {
        ++stats_.frames_unchanged;
        last_decision_ = IncrementalDecision::kNoChange;
        return IncrementalDecision::kNoChange;
    }

    // ---- The whole frame changed -------------------------------------------
    if (full_frame || dirty.empty()) {
        ++stats_.frames_full;
        last_decision_ = IncrementalDecision::kFullFrame;
        return IncrementalDecision::kFullFrame;
    }

    // ---- The codec cannot express the region -------------------------------
    //
    // The honest fallback: the sender emits a **full key frame** for the
    // changed frame, and the fallback is counted, because plan B9's
    // acceptance criterion says the fallback must be *reported*.
    if (!codec_supports_incremental_) {
        ++stats_.fallback_keyframes;
        last_decision_ = IncrementalDecision::kFallbackKeyframe;
        return IncrementalDecision::kFallbackKeyframe;
    }

    // ---- Incremental --------------------------------------------------------
    regions_ = dirty;
    request_.enabled             = true;
    request_.full_frame          = false;
    request_.regions             = dirty;
    request_.allow_skip_unchanged = true;
    request_.signalling          = plugins::VideoRegionSignalling::kRegionRefresh;
    ++stats_.frames_incremental;
    last_decision_ = IncrementalDecision::kIncremental;
    return IncrementalDecision::kIncremental;
}

// ---------------------------------------------------------------------------
// IncrementalEncodeDriver
// ---------------------------------------------------------------------------

IncrementalEncodeDriver::IncrementalEncodeDriver() noexcept = default;

void IncrementalEncodeDriver::set_encoder(
        plugins::IVideoEncoder* encoder) noexcept {
    encoder_ = encoder;
    // The controller must know what the codec can do — the capability
    // query is the *only* thing that decides between the incremental
    // path and the fallback.
    if (encoder_) {
        controller_.set_codec_supports_incremental(
            encoder_->supports_incremental_encode());
    } else {
        controller_.set_codec_supports_incremental(false);
    }
}

void IncrementalEncodeDriver::set_region_refresh_callback(
        RegionRefreshCallback cb) noexcept {
    cb_ = std::move(cb);
}

void IncrementalEncodeDriver::set_media_ssrc(std::uint32_t ssrc) noexcept {
    media_ssrc_ = ssrc;
}

void IncrementalEncodeDriver::set_feedback_ssrc(std::uint32_t ssrc) noexcept {
    feedback_ssrc_ = ssrc;
}

IncrementalDecision IncrementalEncodeDriver::on_frame_dirty(
        const std::vector<plugins::VideoRegion>& dirty,
        bool full_frame) noexcept {
    last_region_refresh_.clear();

    const IncrementalDecision decision = controller_.decide(dirty, full_frame);
    last_decision_ = decision;

    switch (decision) {
        case IncrementalDecision::kIncremental: {
            // Push the request to the encoder.  A codec that claims
            // support but rejects the request is treated as a fallback
            // — the honest outcome, and it is counted.
            if (encoder_) {
                const plugins::Status rc = encoder_->set_incremental_request(
                    controller_.last_request());
                if (rc != plugins::kOk) {
                    controller_.note_fallback();
                    last_decision_ = IncrementalDecision::kFallbackKeyframe;
                    if (encoder_) {
                        (void)encoder_->force_keyframe();
                    }
                    return IncrementalDecision::kFallbackKeyframe;
                }
            }

            // Build the region-refresh message the peer needs.
            const auto& regions = controller_.last_regions();
            if (!regions.empty()) {
                video_rtcp_feedback::RegionRefreshPacket pkt;
                pkt.sender_ssrc = feedback_ssrc_;
                pkt.media_ssrc  = media_ssrc_;
                pkt.regions.reserve(regions.size());
                for (const auto& r : regions) {
                    video_rtcp_feedback::RegionRect rr;
                    rr.x      = r.x;
                    rr.y      = r.y;
                    rr.width  = r.width;
                    rr.height = r.height;
                    pkt.regions.push_back(rr);
                }
                const std::size_t needed =
                    video_rtcp_feedback::region_refresh_size(pkt.regions.size());
                last_region_refresh_.resize(needed);
                const std::size_t written =
                    video_rtcp_feedback::build_region_refresh(
                        core::MutableByteSpan{last_region_refresh_.data(),
                                              last_region_refresh_.size()},
                        pkt);
                if (written == 0) {
                    // The message could not be built — the peer would
                    // not know what to invalidate, so the frame must not
                    // be an incremental one.  Fall back honestly.
                    last_region_refresh_.clear();
                    controller_.note_fallback();
                    last_decision_ = IncrementalDecision::kFallbackKeyframe;
                    if (encoder_) {
                        (void)encoder_->force_keyframe();
                    }
                    return IncrementalDecision::kFallbackKeyframe;
                }
                last_region_refresh_.resize(written);
                controller_.note_region_refresh();
                if (cb_) {
                    cb_(core::ByteSpan{last_region_refresh_.data(),
                                       last_region_refresh_.size()});
                }
            }
            return IncrementalDecision::kIncremental;
        }

        case IncrementalDecision::kFallbackKeyframe:
            // The codec cannot express the region: the sender emits a
            // full key frame for the changed frame, and the fallback is
            // counted by the controller.
            if (encoder_) {
                (void)encoder_->force_keyframe();
            }
            return IncrementalDecision::kFallbackKeyframe;

        case IncrementalDecision::kNoChange:
        case IncrementalDecision::kFullFrame:
        case IncrementalDecision::kNotScreenContent:
            break;
    }

    // Any other path must leave the encoder with a *clean* request, so
    // a stale incremental request cannot leak into the next frame.
    if (encoder_) {
        plugins::VideoIncrementalEncodeRequest empty;
        (void)encoder_->set_incremental_request(empty);
    }
    return decision;
}

} // namespace nimrtc::video_pipeline
