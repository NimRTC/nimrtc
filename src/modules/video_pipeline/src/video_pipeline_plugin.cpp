/**
 * @file src/modules/video_pipeline/src/video_pipeline_plugin.cpp
 * @brief PluginAdapter (receiver + sender) + Reference factory implementations.
 *
 * Per ADR-001: concrete implementations live in their respective modules.
 * This file registers `ReferenceReceiverFactory` and `ReferenceSenderFactory`
 * with core::PluginRegistry under the id "reference" so the engine can
 * resolve `EngineConfig::video_receiver_name = "reference"` /
 * `video_sender_name = "reference"` to a PluginAdapter wrapping the
 * concrete `VideoReceiver` / `VideoSender` reference impls.
 *
 * @note P1 (R2-Batch2).
 */

#include <nimrtc/video_pipeline/video_pipeline_plugin.hpp>

#include <cstdint>
#include <utility>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/rtp/packet.hpp>
#include <nimrtc/video_frame/frame.hpp>

namespace nimrtc::video_pipeline {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/** Translate plugin-level `VideoCodecKind` → concrete `video_frame::CodecKind`. */
video_frame::CodecKind to_concrete_codec(plugins::VideoCodecKind k) noexcept {
    switch (k) {
        case plugins::VideoCodecKind::kH264: return video_frame::CodecKind::kH264;
        case plugins::VideoCodecKind::kH265: return video_frame::CodecKind::kH265;
        case plugins::VideoCodecKind::kVP8:  return video_frame::CodecKind::kVP8;
        case plugins::VideoCodecKind::kVP9:  return video_frame::CodecKind::kVP9;
        case plugins::VideoCodecKind::kAV1:  return video_frame::CodecKind::kAV1;
        default:                            return video_frame::CodecKind::kUnknown;
    }
}

/** Translate concrete `video_frame::CodecKind` → plugin-level. */
plugins::VideoCodecKind to_plugin_codec_kind(video_frame::CodecKind k) noexcept {
    switch (k) {
        case video_frame::CodecKind::kH264: return plugins::VideoCodecKind::kH264;
        case video_frame::CodecKind::kH265: return plugins::VideoCodecKind::kH265;
        case video_frame::CodecKind::kVP8:  return plugins::VideoCodecKind::kVP8;
        case video_frame::CodecKind::kVP9:  return plugins::VideoCodecKind::kVP9;
        case video_frame::CodecKind::kAV1:  return plugins::VideoCodecKind::kAV1;
        default:                            return plugins::VideoCodecKind::kUnknown;
    }
}

/** Translate concrete NaluFormat → plugin NaluFormat. */
plugins::EncodedVideoFrame::NaluFormat
to_plugin_nalu_format(video_frame::NaluFormat f) noexcept {
    using ConcreteF = video_frame::NaluFormat;
    using PluginF   = plugins::EncodedVideoFrame::NaluFormat;
    switch (f) {
        case ConcreteF::kAnnexBStartCode: return PluginF::kAnnexB;
        case ConcreteF::kLengthPrefixed:  return PluginF::kLengthPrefixed;
        default:                          return PluginF::kUnknown;
    }
}

/** Translate plugin-level EncodedVideoFrame → concrete video_frame::EncodedVideoFrame. */
video_frame::EncodedVideoFrame
to_concrete_frame(const plugins::EncodedVideoFrame& p) noexcept {
    using PluginF = plugins::EncodedVideoFrame::NaluFormat;
    video_frame::EncodedVideoFrame c{};
    c.codec         = to_concrete_codec(p.codec);
    c.payload       = p.payload;
    c.payload_type  = p.payload_type;
    c.is_keyframe   = p.is_keyframe;
    c.info.capture_ts_us = p.info.capture_ts_us;
    c.info.frame_seq     = p.info.frame_seq;
    c.info.rtp_timestamp = p.info.rtp_timestamp;
    switch (p.nalu_format) {
        case PluginF::kAnnexB:
            c.nalu_format = video_frame::NaluFormat::kAnnexBStartCode; break;
        case PluginF::kLengthPrefixed:
            c.nalu_format = video_frame::NaluFormat::kLengthPrefixed; break;
        default:
            c.nalu_format = video_frame::NaluFormat::kUnknown; break;
    }
    return c;
}

/** Translate concrete frame → plugin-level frame. */
plugins::EncodedVideoFrame
to_plugin_frame(const video_frame::EncodedVideoFrame& c) noexcept {
    plugins::EncodedVideoFrame p{};
    p.codec        = to_plugin_codec_kind(c.codec);
    p.payload      = c.payload;
    p.payload_type = c.payload_type;
    p.is_keyframe  = c.is_keyframe;
    p.nalu_format  = to_plugin_nalu_format(c.nalu_format);
    p.info.capture_ts_us = c.info.capture_ts_us;
    p.info.frame_seq     = c.info.frame_seq;
    p.info.rtp_timestamp = c.info.rtp_timestamp;
    return p;
}

/** Translate plugin-level packetisation mode → concrete `PacketizationMode`. */
PacketizationMode to_concrete_pmode(plugins::VideoPacketizationMode m) noexcept {
    switch (m) {
        case plugins::VideoPacketizationMode::kSingleNal:
            return PacketizationMode::kSingleNal;
        case plugins::VideoPacketizationMode::kNonInterleaved:
            return PacketizationMode::kNonInterleaved;
        case plugins::VideoPacketizationMode::kInterleaved:
            return PacketizationMode::kInterleaved;
        default:
            return PacketizationMode::kNonInterleaved;
    }
}

plugins::VideoPacketizationMode
to_plugin_pmode(PacketizationMode m) noexcept {
    switch (m) {
        case PacketizationMode::kSingleNal:
            return plugins::VideoPacketizationMode::kSingleNal;
        case PacketizationMode::kNonInterleaved:
            return plugins::VideoPacketizationMode::kNonInterleaved;
        case PacketizationMode::kInterleaved:
            return plugins::VideoPacketizationMode::kInterleaved;
        default:
            return plugins::VideoPacketizationMode::kNonInterleaved;
    }
}

/** Translate plugin `VideoRtpPacket` → concrete `rtp::PacketView`. */
rtp::PacketView to_concrete_rtp(const plugins::VideoRtpPacket& p) noexcept {
    rtp::PacketView v{};
    v.ssrc          = p.ssrc;
    v.seq           = p.seq;
    v.timestamp     = p.rtp_timestamp;
    v.payload_type  = p.payload_type;
    v.marker        = p.marker;
    v.payload       = p.payload;
    v.raw           = p.payload;   // safe; concrete only inspects payload
    return v;
}

/** Translate plugin `VideoReceiverConfig` → concrete `ReceiverConfig`. */
ReceiverConfig to_concrete_receiver_cfg(const plugins::VideoReceiverConfig& p) noexcept {
    ReceiverConfig c{};
    c.codec         = to_concrete_codec(p.codec);
    c.ssrc          = p.ssrc;
    c.payload_type  = p.payload_type;
    c.expect_fu_a   = p.expect_fu_a;

    video_jb::Config jb{};
    jb.mode = video_jb::Mode::kLowLatency;
    jb.max_inflight_frames = p.max_inflight_frames;
    jb.max_wait_ms         = p.max_jitter_buffer_ms;
    jb.emit_nacks          = p.emit_nacks;
    c.jitter_buffer_config = jb;
    return c;
}

plugins::VideoReceiverConfig
to_plugin_receiver_cfg(const ReceiverConfig& c) noexcept {
    plugins::VideoReceiverConfig p{};
    p.codec        = to_plugin_codec_kind(c.codec);
    p.ssrc         = c.ssrc;
    p.payload_type = c.payload_type;
    p.expect_fu_a  = c.expect_fu_a;
    p.max_inflight_frames = c.jitter_buffer_config.max_inflight_frames;
    p.max_jitter_buffer_ms = c.jitter_buffer_config.max_wait_ms;
    p.emit_nacks          = c.jitter_buffer_config.emit_nacks;
    return p;
}

/** Translate plugin `VideoSenderConfig` → concrete `SenderConfig`. */
SenderConfig to_concrete_sender_cfg(const plugins::VideoSenderConfig& p) noexcept {
    SenderConfig c{};
    c.codec         = to_concrete_codec(p.codec);
    c.ssrc          = p.ssrc;
    c.payload_type  = p.payload_type;
    c.mtu           = p.mtu;
    c.packetization_mode = to_concrete_pmode(p.packetization_mode);
    c.initial_seq   = p.initial_seq;
    return c;
}

plugins::VideoSenderConfig
to_plugin_sender_cfg(const SenderConfig& c) noexcept {
    plugins::VideoSenderConfig p{};
    p.codec        = to_plugin_codec_kind(c.codec);
    p.ssrc         = c.ssrc;
    p.payload_type = c.payload_type;
    p.mtu          = c.mtu;
    p.packetization_mode = to_plugin_pmode(c.packetization_mode);
    p.initial_seq  = c.initial_seq;
    return p;
}

/** Translate concrete stats → plugin stats. */
plugins::VideoReceiverStats
to_plugin_receiver_stats(const ReceiverStats& s) noexcept {
    plugins::VideoReceiverStats out{};
    out.packets_pushed       = s.packets_pushed;
    out.frames_emitted       = s.frames_emitted;
    out.frames_incomplete    = s.frames_incomplete;
    out.fua_fragments_seen   = s.fua_fragments_seen;
    out.stap_a_packets_seen  = s.stap_a_packets_seen;
    out.nack_entries_emitted = s.nack_entries_emitted;
    out.sequence_gaps_seen   = s.sequence_gaps_seen;
    return out;
}

plugins::VideoSenderStats
to_plugin_sender_stats(const SenderStats& s) noexcept {
    plugins::VideoSenderStats out{};
    out.frames_pushed                = s.frames_pushed;
    out.packets_emitted              = s.packets_emitted;
    out.fua_frames_packetized        = s.fua_frames_packetized;
    out.stapa_frames_packetized      = s.stapa_frames_packetized;
    out.single_nal_frames_packetized = s.single_nal_frames_packetized;
    out.force_keyframe_calls         = s.force_keyframe_calls;
    out.frames_marked_as_keyframe    = s.frames_marked_as_keyframe;
    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// ReceiverAdapter
// ---------------------------------------------------------------------------

ReceiverAdapter::ReceiverAdapter(std::unique_ptr<VideoReceiver> concrete) noexcept
    : concrete_(std::move(concrete)) {}

ReceiverAdapter::~ReceiverAdapter() = default;

const char* ReceiverAdapter::name() const noexcept {
    return "nimrtc::video_pipeline::ReceiverAdapter (wraps concrete VideoReceiver)";
}

plugins::Status ReceiverAdapter::open() noexcept {
    if (!concrete_) return plugins::kErrNotReady;
    return plugins::kOk;   // concrete VideoReceiver has no open/close lifecycle
}

void ReceiverAdapter::close() noexcept {
    if (concrete_) concrete_.reset();
    user_frame_cb_ = {};
    user_nack_cb_  = {};
}

void ReceiverAdapter::set_frame_callback(plugins::VideoReceiverFrameCallback cb) noexcept {
    user_frame_cb_ = std::move(cb);
    if (concrete_) {
        // Translate plugin callback → concrete ReceiverFrameCallback.
        concrete_->set_frame_callback(
            [this](video_frame::EncodedVideoFrame frame, std::int64_t now_us) {
                if (user_frame_cb_) {
                    user_frame_cb_(to_plugin_frame(frame),
                                   static_cast<plugins::TimestampUs>(now_us));
                }
            });
    }
}

void ReceiverAdapter::set_nack_callback(plugins::VideoReceiverNackCallback cb) noexcept {
    // Concrete VideoReceiver exposes pull-style pop_nack_entries() only;
    // a push callback isn't yet supported in the concrete API. Store it
    // so future revisions can wire it up without an API break.
    user_nack_cb_ = std::move(cb);
}

plugins::Status ReceiverAdapter::push_rtp(const plugins::VideoRtpPacket& packet,
                                          plugins::TimestampUs now_us) noexcept {
    if (!concrete_) return plugins::kErrNotReady;
    const auto v = to_concrete_rtp(packet);
    const bool ok = concrete_->push_rtp(v, static_cast<std::int64_t>(now_us));
    return ok ? plugins::kOk : plugins::kErrInvalidParam;
}

std::vector<std::uint16_t> ReceiverAdapter::pop_nack_entries() noexcept {
    if (!concrete_) return {};
    return concrete_->pop_nack_entries();
}

plugins::Status ReceiverAdapter::tick(plugins::TimestampUs now_us) noexcept {
    if (!concrete_) return plugins::kErrNotReady;
    concrete_->tick(static_cast<std::int64_t>(now_us));
    return plugins::kOk;
}

void ReceiverAdapter::reset() noexcept {
    if (concrete_) concrete_->reset();
}

plugins::VideoReceiverStats ReceiverAdapter::stats() const noexcept {
    if (!concrete_) return {};
    return to_plugin_receiver_stats(concrete_->stats());
}

plugins::VideoReceiverConfig ReceiverAdapter::config() const noexcept {
    if (!concrete_) return {};
    return to_plugin_receiver_cfg(/* default ctor concrete config */ ReceiverConfig{});
}

// ---------------------------------------------------------------------------
// SenderAdapter
// ---------------------------------------------------------------------------

SenderAdapter::SenderAdapter(std::unique_ptr<VideoSender> concrete) noexcept
    : concrete_(std::move(concrete)) {}

SenderAdapter::~SenderAdapter() = default;

const char* SenderAdapter::name() const noexcept {
    return "nimrtc::video_pipeline::SenderAdapter (wraps concrete VideoSender)";
}

plugins::Status SenderAdapter::open() noexcept {
    if (!concrete_) return plugins::kErrNotReady;
    return plugins::kOk;
}

void SenderAdapter::close() noexcept {
    if (concrete_) concrete_.reset();
    user_pkt_cb_ = {};
}

void SenderAdapter::set_packet_callback(plugins::VideoSenderPacketCallback cb) noexcept {
    user_pkt_cb_ = std::move(cb);
    if (concrete_) {
        concrete_->set_packet_callback(
            [this](core::ByteSpan payload, std::uint32_t rtp_ts,
                   std::uint16_t seq, bool marker) {
                if (user_pkt_cb_) {
                    user_pkt_cb_(payload, rtp_ts, seq, marker);
                }
            });
    }
}

plugins::Status SenderAdapter::push_frame(const plugins::EncodedVideoFrame& frame,
                                          std::uint32_t rtp_timestamp) noexcept {
    if (!concrete_) return plugins::kErrNotReady;
    auto concrete_frame = to_concrete_frame(frame);
    auto rc = concrete_->push_frame(concrete_frame,
                                    static_cast<std::int64_t>(frame.info.capture_ts_us),
                                    rtp_timestamp,
                                    frame.info.frame_seq);
    return rc.ok() ? plugins::kOk : plugins::kErrInternal;
}

void SenderAdapter::force_keyframe() noexcept {
    if (concrete_) concrete_->force_keyframe();
}

plugins::VideoSenderStats SenderAdapter::stats() const noexcept {
    if (!concrete_) return {};
    return to_plugin_sender_stats(concrete_->stats());
}

std::uint16_t SenderAdapter::next_seq() const noexcept {
    if (!concrete_) return 0;
    return concrete_->next_seq();
}

plugins::VideoSenderConfig SenderAdapter::config() const noexcept {
    if (!concrete_) return {};
    return to_plugin_sender_cfg(/* default concrete config */ SenderConfig{});
}

// ---------------------------------------------------------------------------
// ReferenceReceiverFactory
// ---------------------------------------------------------------------------

std::string_view ReferenceReceiverFactory::id() const noexcept {
    return "reference";
}

std::string_view ReferenceReceiverFactory::display_name() const noexcept {
    return "Reference H.264 / VP8 / VP9 depacketizer + jitter buffer";
}

plugins::IVideoReceiver*
ReferenceReceiverFactory::create(plugins::VideoReceiverConfig cfg) const {
    auto concrete = std::make_unique<VideoReceiver>(to_concrete_receiver_cfg(cfg));
    return new ReceiverAdapter(std::move(concrete));
}

// ---------------------------------------------------------------------------
// ReferenceSenderFactory
// ---------------------------------------------------------------------------

std::string_view ReferenceSenderFactory::id() const noexcept {
    return "reference";
}

std::string_view ReferenceSenderFactory::display_name() const noexcept {
    return "Reference H.264 FU-A / STAP-A packetizer";
}

plugins::IVideoSender*
ReferenceSenderFactory::create(plugins::VideoSenderConfig cfg) const {
    auto concrete = std::make_unique<VideoSender>(to_concrete_sender_cfg(cfg));
    return new SenderAdapter(std::move(concrete));
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

namespace detail {

void do_register_default_plugins() noexcept {
    static const struct Registrar {
        Registrar() {
            static ReferenceReceiverFactory s_rx{};
            static ReferenceSenderFactory   s_tx{};
            nimrtc::core::PluginRegistry::instance().register_video_receiver(
                s_rx.id(), &s_rx);
            nimrtc::core::PluginRegistry::instance().register_video_sender(
                s_tx.id(), &s_tx);
        }
    } s_registrar;
    (void)s_registrar;
}

} // namespace detail

void register_default_plugins() noexcept {
    static const int once = []() {
        detail::do_register_default_plugins();
        return 1;
    }();
    (void)once;
}

} // namespace nimrtc::video_pipeline
