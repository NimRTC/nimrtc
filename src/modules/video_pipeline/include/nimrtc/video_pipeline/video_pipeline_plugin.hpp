/**
 * @file nimrtc/video_pipeline/video_pipeline_plugin.hpp
 * @brief PluginAdapter — wraps concrete `VideoReceiver` / `VideoSender` so
 *        they can be looked up by string ID through `core::PluginRegistry`.
 *
 * Implements `plugins::IVideoReceiver` and `plugins::IVideoSender` by
 * delegating to owned concrete instances produced by
 * `make_video_receiver()` / `make_video_sender()`.
 *
 * ## Adapter vs direct implementation
 *
 * The concrete pipeline types use `rtp::PacketView` (full RTP packet
 * with CSRCs and extensions) and `video_jb::Config`. The plugin API uses
 * a minimal `VideoRtpPacket` view and separate JB fields. A small
 * adapter class bridges the two without forcing concrete code to know
 * about plugin types.
 *
 * ## Registration
 *
 * `ReferenceReceiverFactory` / `ReferenceSenderFactory` register under
 * id `"reference"` (matches `EngineConfig::video_receiver_name` /
 * `video_sender_name` defaults). When HW-assisted plugins ship (NDK
 * MediaCodec, NVDEC, VideoToolbox, ...), separate factories should
 * register under their own ids.
 *
 * ## Important: explicit-registration required (MSVC quirk)
 *
 * MSVC's static linker strips anonymous-namespace `static` global
 * initializers from a .lib when no symbol from that .obj is ODR-used.
 * Callers must explicitly invoke
 * `nimrtc::video_pipeline::register_default_plugins()` once at startup.
 *
 * @note P1 (R2-Batch2).
 */

#pragma once


// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.
#include <nimrtc/core/api_version.hpp>
#include <cstdint>
#include <memory>
#include <string_view>

#include <nimrtc/core/bytes.hpp>                 // core::MutableByteSpan
#include <nimrtc/video_pipeline/keyframe_limiter.hpp>   // KeyframeLimiter
#include <nimrtc/video_pipeline/video_pipeline.hpp>   // concrete VideoReceiver / VideoSender
#include <nimrtc/plugins/video_pipeline.hpp>          // plugins::IVideoReceiver / IVideoSender

namespace nimrtc::video_pipeline {

// ---------------------------------------------------------------------------
// PluginAdapter: receiver
// ---------------------------------------------------------------------------

/**
 * @brief Wraps a concrete `VideoReceiver` so it can be exposed via the
 *        `plugins::IVideoReceiver` interface and looked up by string ID.
 *
 * Translates between:
 *   - `plugins::VideoRtpPacket` ↔ `rtp::PacketView`
 *   - `plugins::EncodedVideoFrame` (alias of `video_frame::EncodedVideoFrame`) — same struct
 *   - plugin `VideoReceiverConfig` ↔ concrete `ReceiverConfig`
 */
class NIMRTC_API_STABLE ReceiverAdapter : public plugins::IVideoReceiver {
public:
    /** Takes ownership of the concrete receiver. */
    explicit ReceiverAdapter(std::unique_ptr<VideoReceiver> concrete) noexcept;
    ~ReceiverAdapter() override;

    ReceiverAdapter(const ReceiverAdapter&)            = delete;
    ReceiverAdapter& operator=(const ReceiverAdapter&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------
    const char*       name()  const noexcept override;
    plugins::Status   open()        noexcept override;
    void              close()       noexcept override;

    // ---- plugins::IVideoReceiver ------------------------------------------
    void              set_frame_callback(plugins::VideoReceiverFrameCallback cb) noexcept override;
    void              set_nack_callback (plugins::VideoReceiverNackCallback  cb) noexcept override;
    void              set_feedback_callback(plugins::VideoReceiverFeedbackCallback cb) noexcept override;
    std::size_t       feedback_rtcp_size(
                          const plugins::VideoFeedbackReport& report) const noexcept override;
    std::size_t       build_feedback_rtcp(const plugins::VideoFeedbackReport& report,
                                          std::uint32_t sender_ssrc,
                                          std::uint32_t media_ssrc,
                                          nimrtc::core::MutableByteSpan out) const noexcept override;
    plugins::Status   push_rtp(const plugins::VideoRtpPacket& packet,
                               plugins::TimestampUs now_us) noexcept override;
    void              set_rtx_payload_type(std::uint8_t rtx_pt) noexcept override;
    plugins::Status   push_rtp_rtx(const plugins::VideoRtpPacket& packet,
                                   plugins::TimestampUs now_us) noexcept override;
    std::vector<std::uint16_t> pop_nack_entries() noexcept override;
    plugins::Status   tick(plugins::TimestampUs now_us) noexcept override;
    void              reset() noexcept override;
    plugins::VideoReceiverStats stats() const noexcept override;
    plugins::VideoReceiverConfig config() const noexcept override;

private:
    /** Install the concrete feedback callback once; it fans out to both
     *  the NACK callback and the feedback callback. */
    void install_feedback_bridge() noexcept;

    std::unique_ptr<VideoReceiver>                       concrete_;
    plugins::VideoReceiverFrameCallback                  user_frame_cb_;
    plugins::VideoReceiverNackCallback                   user_nack_cb_;
    plugins::VideoReceiverFeedbackCallback               user_feedback_cb_;
    bool                                                feedback_bridge_installed_ = false;
};

// ---------------------------------------------------------------------------
// PluginAdapter: sender
// ---------------------------------------------------------------------------

/**
 * @brief Wraps a concrete `VideoSender` so it can be exposed via the
 *        `plugins::IVideoSender` interface.
 *
 * The plugin `push_frame()` signature takes `(frame, rtp_timestamp)`
 * and reads `frame.info.{capture_ts_us, frame_seq}`; the concrete API
 * takes `(frame, capture_ts_us, rtp_timestamp, frame_seq)` as separate
 * scalars. The adapter bridges them.
 */
class NIMRTC_API_STABLE SenderAdapter : public plugins::IVideoSender {
public:
    /** Takes ownership of the concrete sender. */
    explicit SenderAdapter(std::unique_ptr<VideoSender> concrete) noexcept;
    ~SenderAdapter() override;

    SenderAdapter(const SenderAdapter&)            = delete;
    SenderAdapter& operator=(const SenderAdapter&) = delete;

    // ---- plugins::IPlugin --------------------------------------------------
    const char*       name()  const noexcept override;
    plugins::Status   open()        noexcept override;
    void              close()       noexcept override;

    // ---- plugins::IVideoSender --------------------------------------------
    void              set_packet_callback(plugins::VideoSenderPacketCallback cb) noexcept override;
    void              set_rtx_packet_callback(
                            plugins::VideoSenderRtxCallback cb) noexcept override;
    void              set_rtx_enabled(bool enabled) noexcept override;
    bool              rtx_enabled() const noexcept override;
    plugins::Status   push_frame(const plugins::EncodedVideoFrame& frame,
                                 std::uint32_t rtp_timestamp) noexcept override;
    void              force_keyframe() noexcept override;
    int               apply_feedback_rtcp(plugins::BufferView rtcp) noexcept override;
    std::uint32_t     take_keyframe_requests() noexcept override;
    void              set_ref_frame(std::int64_t capture_ts_us,
                                    std::uint32_t frame_id,
                                    std::uint32_t rtp_timestamp) noexcept override;
    plugins::VideoSenderStats stats() const noexcept override;
    std::uint16_t     next_seq() const noexcept override;
    plugins::VideoSenderConfig config() const noexcept override;

    // ---- The key-frame storm limiter (remote-desktop plan B14) -----------
    //
    // The adapter is where the PLI / FIR path becomes a key frame, so
    // this is where the limiter is configured and where its counters are
    // read.
    void set_keyframe_limiter_config(
            nimrtc::video_pipeline::KeyframeLimiterConfig cfg) noexcept;
    nimrtc::video_pipeline::KeyframeLimiterConfig
        keyframe_limiter_config() const noexcept;
    nimrtc::video_pipeline::KeyframeLimiterStats
        keyframe_limiter_stats() const noexcept;
    /** Offer a key-frame request to the limiter. */
    nimrtc::video_pipeline::KeyframeDecision request_keyframe(
            nimrtc::video_pipeline::KeyframeReason reason,
            std::int64_t now_us,
            bool carries_parameter_sets = false) noexcept;

    // ---- The seam's half of the limiter ---------------------------------
    //
    // The engine configures the policy through the seam and reads the
    // counters back from `stats()`.  The adapter translates between the
    // seam's policy type and the module's own.  The names differ from
    // the module-level ones above on purpose: C++ cannot overload on
    // the return type alone, and the two accessors answer different
    // questions (the module's type vs the seam's type).
    void set_keyframe_limiter_config(
            const plugins::VideoKeyframeLimiterConfig& cfg) noexcept override;
    plugins::VideoKeyframeLimiterConfig
        keyframe_limiter_config_seam() const noexcept override;
    bool keyframe_request_pending() const noexcept override;
    std::uint32_t retry_pending_keyframe(std::int64_t now_us) noexcept override;

    // ---- The incremental (dirty-rect) encode mechanism (plan B9) --------
    void set_content_type(plugins::VideoContentType type) noexcept override;
    plugins::VideoContentType content_type() const noexcept override;
    void set_incremental_encode_enabled(bool enabled) noexcept override;
    bool incremental_encode_enabled() const noexcept override;
    plugins::VideoIncrementalDecision offer_dirty_region(
            const std::vector<plugins::VideoRegion>& dirty,
            bool full_frame) noexcept override;
    core::ByteSpan take_region_refresh() noexcept override;
    plugins::VideoIncrementalStats incremental_stats() const noexcept override;

    /** Attach the encoder the incremental requests are pushed to.
     *
     *  The adapter does not own the encoder (the engine does), so the
     *  pointer is borrowed. */
    void set_incremental_encoder(plugins::IVideoEncoder* encoder) noexcept override;

    /** Set the SSRCs the region-refresh message names. */
    void set_region_refresh_ssrcs(std::uint32_t media_ssrc,
                                   std::uint32_t feedback_ssrc) noexcept;

private:
    std::unique_ptr<VideoSender>                       concrete_;
    plugins::VideoSenderPacketCallback                 user_pkt_cb_;
    plugins::VideoSenderRtxCallback                    user_rtx_cb_;
    /** Key-frame requests seen since the last `take_keyframe_requests()`
     *  (PLI / FIR arrivals). */
    std::uint32_t                                      keyframe_requests_ = 0;
};

// ---------------------------------------------------------------------------
// Reference factories
// ---------------------------------------------------------------------------

/** Receiver factory wrapping the concrete `VideoReceiver` reference impl. */
class NIMRTC_API_STABLE ReferenceReceiverFactory : public plugins::IVideoReceiverFactory {
public:
    std::string_view id()           const noexcept override;
    std::string_view display_name() const noexcept override;
    plugins::IVideoReceiver* create(plugins::VideoReceiverConfig cfg) const override;
};

/** Sender factory wrapping the concrete `VideoSender` reference impl. */
class NIMRTC_API_STABLE ReferenceSenderFactory : public plugins::IVideoSenderFactory {
public:
    std::string_view id()           const noexcept override;
    std::string_view display_name() const noexcept override;
    plugins::IVideoSender* create(plugins::VideoSenderConfig cfg) const override;
};

// ---------------------------------------------------------------------------
// Public registration entry point
// ---------------------------------------------------------------------------

/** Register the default video_pipeline plugin(s) with core::PluginRegistry.
 *  Must be called once at program startup before any pipeline lookup.
 *  Idempotent: subsequent invocations are no-ops. */
NIMRTC_API_STABLE void register_default_plugins() noexcept;

} // namespace nimrtc::video_pipeline
