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

#include <cstdint>
#include <memory>
#include <string_view>

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
class ReceiverAdapter : public plugins::IVideoReceiver {
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
    plugins::Status   push_rtp(const plugins::VideoRtpPacket& packet,
                               plugins::TimestampUs now_us) noexcept override;
    std::vector<std::uint16_t> pop_nack_entries() noexcept override;
    plugins::Status   tick(plugins::TimestampUs now_us) noexcept override;
    void              reset() noexcept override;
    plugins::VideoReceiverStats stats() const noexcept override;
    plugins::VideoReceiverConfig config() const noexcept override;

private:
    std::unique_ptr<VideoReceiver>                       concrete_;
    plugins::VideoReceiverFrameCallback                  user_frame_cb_;
    plugins::VideoReceiverNackCallback                   user_nack_cb_;
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
class SenderAdapter : public plugins::IVideoSender {
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
    plugins::Status   push_frame(const plugins::EncodedVideoFrame& frame,
                                 std::uint32_t rtp_timestamp) noexcept override;
    void              force_keyframe() noexcept override;
    plugins::VideoSenderStats stats() const noexcept override;
    std::uint16_t     next_seq() const noexcept override;
    plugins::VideoSenderConfig config() const noexcept override;

private:
    std::unique_ptr<VideoSender>                       concrete_;
    plugins::VideoSenderPacketCallback                 user_pkt_cb_;
};

// ---------------------------------------------------------------------------
// Reference factories
// ---------------------------------------------------------------------------

/** Receiver factory wrapping the concrete `VideoReceiver` reference impl. */
class ReferenceReceiverFactory : public plugins::IVideoReceiverFactory {
public:
    std::string_view id()           const noexcept override;
    std::string_view display_name() const noexcept override;
    plugins::IVideoReceiver* create(plugins::VideoReceiverConfig cfg) const override;
};

/** Sender factory wrapping the concrete `VideoSender` reference impl. */
class ReferenceSenderFactory : public plugins::IVideoSenderFactory {
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
void register_default_plugins() noexcept;

} // namespace nimrtc::video_pipeline
