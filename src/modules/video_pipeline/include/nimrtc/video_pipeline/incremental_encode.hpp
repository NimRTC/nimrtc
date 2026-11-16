/**
 * @file nimrtc/video_pipeline/incremental_encode.hpp
 * @brief Dirty-rect incremental encode — remote-desktop plan B9.
 *
 * ## What this is
 *
 * Plan B9: "the encoder encodes only the changed region and signals the
 * region to the peer (a region-invalidation / loss-region refresh
 * mechanism), or — where the codec cannot express it — the sender emits a
 * keyframe with the changed region only where the codec supports it.
 * This is a **screen-content-specific optimisation** and is explicitly
 * *not* part of the generic video path."
 *
 * The mechanism has three parts:
 *
 *  1. the **capture** reports which rectangles changed (plan A7 — the
 *     `CaptureDirtyRects` metadata; it exists end to end inside the
 *     capture layer),
 *  2. the **encoder** is asked to code only those rectangles (the
 *     `plugins::IVideoEncoder::set_incremental_request()` seam, and the
 *     `supports_incremental_encode()` capability query),
 *  3. the **peer** is told which rectangles changed (the
 *     `nimrtc::video_rtcp_feedback` region-refresh message — an RTCP APP
 *     packet, because neither the PLI nor the FIR can carry a rectangle).
 *
 * This class is the **arbiter** of that mechanism: it decides whether
 * the mechanism engages at all, and it reports honestly when it does not.
 *
 * ## The screen-content gate
 *
 * The mechanism only engages for **screen** content.  A camera stream
 * has no meaningful "unchanged region" — every pixel moves every frame,
 * so a dirty rectangle computed for it is noise, and skipping the
 * "unchanged" area would corrupt the picture.  The gate is therefore on
 * the *content type*, not on the codec: a camera stream never engages,
 * even with a codec that supports incremental encode.
 *
 * ## The fallback, and its honest reporting
 *
 * When the codec cannot express the region (`supports_incremental_encode()
 * == false`), the sender falls back to a **full key frame** for the
 * changed frame — and the fallback is *counted*, because plan B9's
 * acceptance criterion says: "the mechanism is disabled when the codec
 * does not support it and **the fallback is reported**".
 *
 * ## Threading
 *
 * Not thread-safe.  One controller per outbound video stream.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v1.13.x follow-up.
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::video_pipeline {

// ---------------------------------------------------------------------------
// The vocabulary
// ---------------------------------------------------------------------------

/**
 * @brief The content type of a stream.
 *
 * This is the *stream-level* content type, not the capture device's
 * kind: ADR-022 Decision 5 is explicit that the engine must not infer
 * the content type from the device — a document camera is camera-shaped
 * but screen-like, and a screen playing video is screen-shaped but
 * motion-like.  The host declares it.
 *
 * The mapping onto the wire hint (`plugins::VideoEncoderParams::
 * ContentHint`) is the one ADR-022 §Decision 3 describes:
 * screen -> `kDetail`, camera -> `kMotion`.
 */
enum class NIMRTC_API_STABLE VideoContentType : std::uint8_t {
    /** Nobody said.  The camera default — the safe choice. */
    kUnspecified = 0,
    /** Camera content.  The incremental mechanism never engages. */
    kCamera = 1,
    /** Screen content.  The incremental mechanism may engage. */
    kScreen = 2,
};

/** Human-readable name of a content type. */
NIMRTC_API_STABLE std::string_view video_content_type_name(
    VideoContentType type) noexcept;

/** True iff the content type is screen content. */
NIMRTC_API_STABLE bool is_screen_content(VideoContentType type) noexcept;

// ---------------------------------------------------------------------------
// The decision
// ---------------------------------------------------------------------------

/** What the controller decided to do with one frame's dirty rectangles. */
enum class NIMRTC_API_STABLE IncrementalDecision : std::uint8_t {
    /** Nothing changed — the frame carries no region, and the encoder
     *  should not be asked to do anything. */
    kNoChange = 0,
    /** The whole frame changed — an ordinary full encode. */
    kFullFrame = 1,
    /** The encoder was asked to code only the changed region. */
    kIncremental = 2,
    /** The codec cannot express the region — the sender falls back to a
     *  full key frame and the fallback is counted. */
    kFallbackKeyframe = 3,
    /** The mechanism did not engage: the content is not screen content. */
    kNotScreenContent = 4,
};

NIMRTC_API_STABLE std::string_view incremental_decision_name(
    IncrementalDecision decision) noexcept;

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

/** The controller's counters. */
struct NIMRTC_API_STABLE IncrementalEncodeStats {
    /** Frames offered to the controller. */
    std::uint64_t frames_seen = 0;
    /** Frames that carried no change at all (the static-desktop case). */
    std::uint64_t frames_unchanged = 0;
    /** Frames encoded incrementally (the mechanism engaged). */
    std::uint64_t frames_incremental = 0;
    /** Frames that needed a full encode because the whole frame changed. */
    std::uint64_t frames_full = 0;
    /** The **fallback** counter: frames that fell back to a full key
     *  frame because the codec cannot express the region. */
    std::uint64_t fallback_keyframes = 0;
    /** Frames where the mechanism did not engage because the content is
     *  not screen content. */
    std::uint64_t skipped_not_screen = 0;
    /** Region-refresh messages emitted for the peer. */
    std::uint64_t region_refreshes_sent = 0;
};

// ---------------------------------------------------------------------------
// IncrementalEncodeController
// ---------------------------------------------------------------------------

/**
 * @brief The arbiter of the dirty-rect incremental-encode mechanism.
 *
 * The controller is deliberately **codec-agnostic**: it is told whether
 * the codec supports incremental encode (`set_codec_supports_incremental()`)
 * rather than holding a codec pointer, so the module does not depend on
 * the codec implementations and a host can drive it directly.
 */
class NIMRTC_API_STABLE IncrementalEncodeController {
public:
    IncrementalEncodeController() noexcept;

    /** The content type of the stream.  Only screen content engages the
     *  mechanism. */
    void set_content_type(VideoContentType type) noexcept;
    VideoContentType content_type() const noexcept { return content_type_; }

    /** Tell the controller whether the codec can express a region. */
    void set_codec_supports_incremental(bool supported) noexcept;
    bool codec_supports_incremental() const noexcept {
        return codec_supports_incremental_;
    }

    /** True iff the mechanism is enabled at all (the host asked for it
     *  *and* the content is screen content). */
    bool enabled() const noexcept;

    /** Enable / disable the mechanism.  When disabled, every frame takes
     *  the `kFullFrame` path and nothing is counted as incremental. */
    void set_enabled(bool enabled) noexcept;

    /** Offer one frame's changed region to the controller.
     *
     *  @param dirty     The changed rectangles (source pixels).  An empty
     *                   list means "nothing changed" — the static-desktop
     *                   case that plan B9 cares about.
     *  @param full_frame True iff the whole frame changed.
     *  @return The decision. */
    IncrementalDecision decide(const std::vector<plugins::VideoRegion>& dirty,
                               bool full_frame) noexcept;

    /** The incremental request the controller would push to the encoder
     *  for the last decision.  It is inert (`enabled == false`) unless
     *  the last decision was `kIncremental`. */
    const plugins::VideoIncrementalEncodeRequest& last_request() const noexcept {
        return request_;
    }

    /** The decision of the most recent `decide()`. */
    IncrementalDecision last_decision() const noexcept { return last_decision_; }

    /** The region-refresh message the controller wants sent to the peer
     *  for the last decision.  Empty when nothing has to be signalled. */
    const std::vector<plugins::VideoRegion>& last_regions() const noexcept {
        return regions_;
    }

    /** Snapshot of the counters. */
    IncrementalEncodeStats stats() const noexcept { return stats_; }

    /** Record a fallback that the controller itself did not decide (the
     *  driver found out that the codec rejected the request, or that the
     *  region-refresh message could not be built).  The fallback counter
     *  is the one plan B9's acceptance criterion names, so it must be
     *  incrementable from outside the decision path. */
    void note_fallback() noexcept { ++stats_.fallback_keyframes; }

    /** Record a region-refresh message that was handed to the peer. */
    void note_region_refresh() noexcept { ++stats_.region_refreshes_sent; }

    /** Reset the counters. */
    void reset() noexcept;

private:
    VideoContentType content_type_ = VideoContentType::kUnspecified;
    bool codec_supports_incremental_ = false;
    bool enabled_ = true;

    plugins::VideoIncrementalEncodeRequest request_{};
    std::vector<plugins::VideoRegion> regions_{};
    IncrementalEncodeStats stats_{};
    IncrementalDecision last_decision_ = IncrementalDecision::kNotScreenContent;
};

// ---------------------------------------------------------------------------
// IncrementalEncodeDriver
// ---------------------------------------------------------------------------

/**
 * @brief Drives one outbound stream's incremental-encode mechanism.
 *
 * The driver is the piece a host actually holds.  It owns the
 * controller, and it does the three things that have to happen together
 * for the mechanism to be worth anything:
 *
 *  1. ask the controller what to do with the frame's dirty rectangles,
 *  2. push the resulting request to the **encoder** (through the
 *     `plugins::IVideoEncoder` seam — the driver does not know which
 *     codec it is talking to),
 *  3. build the **region-refresh** message the peer needs.
 *
 * The peer-facing bytes are produced by the driver but *sent* by
 * whoever owns the RTCP transport — the driver hands them over through
 * the `region_refresh` callback.
 *
 * ## The fallback
 *
 * When the codec cannot express the region, the driver asks for a **full
 * key frame** instead of an incremental encode, and the fallback is
 * counted by the controller.  The driver reports that through
 * `last_decision()`, so a host can see the fallback happened.
 */
class NIMRTC_API_STABLE IncrementalEncodeDriver {
public:
    /** The callback that carries the region-refresh bytes to the peer.
     *  Fired only when the mechanism actually engaged. */
    using RegionRefreshCallback =
        std::function<void(core::ByteSpan rtcp_bytes)>;

    IncrementalEncodeDriver() noexcept;

    /** The controller, for configuration and counters. */
    IncrementalEncodeController& controller() noexcept { return controller_; }
    const IncrementalEncodeController& controller() const noexcept {
        return controller_;
    }

    /** The encoder the requests are pushed to.  May be null (the driver
     *  then only makes the decisions and counts them). */
    void set_encoder(plugins::IVideoEncoder* encoder) noexcept;

    /** The callback that carries the region-refresh bytes to the peer. */
    void set_region_refresh_callback(RegionRefreshCallback cb) noexcept;

    /** The SSRC of the outbound media stream (the region-refresh message
     *  names it). */
    void set_media_ssrc(std::uint32_t ssrc) noexcept;
    std::uint32_t media_ssrc() const noexcept { return media_ssrc_; }

    /** The SSRC used as the *sender* of the region-refresh message. */
    void set_feedback_ssrc(std::uint32_t ssrc) noexcept;

    /**
     * @brief Handle one frame's changed region.
     *
     *  @param dirty     The changed rectangles (source pixels).
     *  @param full_frame True iff the whole frame changed.
     *  @return The decision. */
    IncrementalDecision on_frame_dirty(
        const std::vector<plugins::VideoRegion>& dirty,
        bool full_frame) noexcept;

    /** The decision of the last `on_frame_dirty()`. */
    IncrementalDecision last_decision() const noexcept {
        return last_decision_;
    }

    /** The region-refresh bytes for the last frame, empty when nothing
     *  had to be signalled. */
    const core::ByteBuffer& last_region_refresh() const noexcept {
        return last_region_refresh_;
    }

    /** Snapshot of the counters. */
    IncrementalEncodeStats stats() const noexcept {
        return controller_.stats();
    }

private:
    IncrementalEncodeController controller_;
    plugins::IVideoEncoder*       encoder_ = nullptr;
    RegionRefreshCallback         cb_;
    std::uint32_t                 media_ssrc_ = 0;
    std::uint32_t                 feedback_ssrc_ = 0;
    IncrementalDecision           last_decision_ =
        IncrementalDecision::kNotScreenContent;
    core::ByteBuffer              last_region_refresh_;
};

} // namespace nimrtc::video_pipeline
