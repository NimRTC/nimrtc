/**
 * @file nimrtc/plugins/video_pipeline.hpp
 * @brief IVideoReceiver + IVideoSender — pluggable RTP-packetized video
 *        pipeline seam.
 *
 * Per ARCHITECTURE.md §6 (video pipeline pieces) and §8.4 (unified timeline),
 * the receive and send paths are the most commonly replaced layers in
 * commercial / embedded stacks:
 *
 *   - **Receiver**: HW-assisted depacketization (NVIDIA NVDEC, Intel VAAPI,
 *     Android MediaCodec via NDK, iOS VideoToolbox), proprietary gateway
 *     protocols that pre-empt RTP, etc.
 *   - **Sender**:   HW-assisted packetization (MediaCodec, VideoToolbox,
 *     NVDEC, custom FEC prependers, ...).
 *
 * Both directions sit on top of the `IRTP` plugin (packet framing) and
 * `IVideoCodec` plugin (encode / decode), so this interface intentionally
 * only describes the **pipeline glue** — not the codec or the RTP parser.
 *
 * ## Replacing the pipeline
 *
 * @code
 * class MyReceiver : public plugins::IVideoReceiver { ... };
 * class MySender   : public plugins::IVideoSender   { ... };
 * static nimrtc::plugins::SimpleVideoReceiverFactory<MyReceiver>
 *     rx_factory{"my_hw_rx", "My HW receiver"};
 * static nimrtc::plugins::SimpleVideoSenderFactory<MySender>
 *     tx_factory{"my_hw_tx", "My HW sender"};
 * NIMRTC_REGISTER_VIDEO_RECEIVER(my_hw_rx, &rx_factory);
 * NIMRTC_REGISTER_VIDEO_SENDER  (my_hw_tx, &tx_factory);
 * @endcode
 *
 * ## Pipeline contracts
 *
 * **Receiver** (`IVideoReceiver`):
 *   - Input: RTP packets (header + payload). The receiver extracts seq,
 *     timestamp, SSRC, marker bit internally.
 *   - Output: assembled `EncodedVideoFrame` via frame callback (one per
 *     fully-received frame).
 *   - Timing: pipeline is synchronous and single-threaded — `push_rtp`
 *     may fire the callback inline.
 *   - NACK surface: `pop_nack_entries()` returns missing seq numbers.
 *
 * **Sender** (`IVideoSender`):
 *   - Input: `EncodedVideoFrame` per access unit.
 *   - Output: per-packet RTP payload (no RTP header) via packet callback;
 *     the engine prepends the 12-byte RTP fixed header + extensions.
 *   - Single-threaded; sequence numbers advance monotonically.
 *
 * @note P1 (R2). Interface is stable; binary layout TBD P3.
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include "nimrtc/plugins/base.hpp"
// Reuse VideoCodecKind / VideoPacketizationMode enum from video_codec.hpp.
// EncodedVideoFrame is also defined there; we reuse it so the receiver→
// codec and sender←codec paths don't need a translation layer.
#include "nimrtc/plugins/video_codec.hpp"

#ifndef NIMRTC_PLUGINS_VIDEO_PIPELINE_HPP
#define NIMRTC_PLUGINS_VIDEO_PIPELINE_HPP

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string_view>
#include <vector>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// H.264 packetization mode (RFC 6184 §6 / §8.2).  We re-declare this here
// (instead of forwarding from video_sdp_attributes) so consumers of
// `video_pipeline.hpp` don't need to pull SDP-format helpers.
// ---------------------------------------------------------------------------

enum class VideoPacketizationMode : std::uint8_t {
    kSingleNal      = 0,   ///< only single-NALU packets allowed
    kNonInterleaved = 1,   ///< Single NAL + STAP-A + FU-A (RFC 6184 default)
    kInterleaved    = 2,   ///< STAP-B + FU-B; not implemented (RFC reserved)
};

// ---------------------------------------------------------------------------
// RTP packet view (interface-level — decoupled from `rtp::PacketView`)
// ---------------------------------------------------------------------------

/** Minimal RTP packet view used by `IVideoReceiver::push_rtp()`. The
 *  pipeline needs only the fields relevant to depacketization and reorder
 *  detection; it does NOT need the CSRC array or extension data. */
struct VideoRtpPacket {
    std::uint32_t ssrc          = 0;
    std::uint16_t seq           = 0;
    std::uint32_t rtp_timestamp = 0;
    std::uint8_t  payload_type  = 0;
    bool          marker        = false;
    TimestampUs   recv_ts_us    = 0;   ///< monotonic-clock arrival time

    /** Compressed payload (RTP payload field, header already stripped). */
    BufferView    payload;
};

// ===========================================================================
//                                RECEIVER
// ===========================================================================

/** Receiver configuration. Mirrors the concrete `video_pipeline::ReceiverConfig`
 *  but uses plugin types (`VideoCodecKind`, `TimestampUs`). */
struct VideoReceiverConfig {
    /** Codec of the inbound stream. Determines which depacketizer path
     *  the receiver uses (H.264 FU-A / STAP-A dispatch; VP8 partition
     *  detection; ...). */
    VideoCodecKind codec         = VideoCodecKind::kH264;

    /** Expected RTP SSRC of the inbound stream (used for JB bucket
     *  pre-population). If a packet's SSRC differs, it is still accepted
     *  (some gateways rewrite SSRC). */
    std::uint32_t  ssrc          = 0;

    /** RTP payload type (e.g. 102 for H.264 in WebRTC). */
    std::uint8_t   payload_type  = 0;

    /** Forwarded to the underlying jitter buffer (implementation-specific). */
    std::uint32_t  initial_jitter_buffer_ms = 40;
    std::uint32_t  max_jitter_buffer_ms     = 200;
    std::uint32_t  max_wait_ms              = 200;
    std::uint32_t  max_inflight_frames      = 8;
    bool           emit_nacks               = true;

    /** If true (recommended for H.264), the receiver runs FU-A reassembly
     *  itself. Otherwise the codec layer is expected to. */
    bool           expect_fu_a   = true;
};

/** Receiver statistics. */
struct VideoReceiverStats {
    std::uint64_t packets_pushed       = 0;
    std::uint64_t frames_emitted       = 0;
    std::uint64_t frames_incomplete    = 0;
    std::uint64_t fua_fragments_seen   = 0;
    std::uint64_t stap_a_packets_seen  = 0;
    std::uint64_t nack_entries_emitted = 0;
    std::uint64_t sequence_gaps_seen   = 0;
};

/** Frame callback: invoked on the receiver thread for every assembled frame.
 *  The frame's `payload` is owned by the receiver and is only valid for
 *  the duration of the callback; copy out if you need to keep it. */
using VideoReceiverFrameCallback =
    std::function<void(const EncodedVideoFrame& frame, TimestampUs now_us)>;

/** Callback fired when the receiver decides a packet should be requested
 *  via NACK.  Defaults: implementers populate a seq list and the engine
 *  sends a NACK RTCP FB for each entry. */
using VideoReceiverNackCallback =
    std::function<void(std::vector<std::uint16_t> missing_seqs)>;

/**
 * @brief Receiver pipeline: RTP packets → assembled encoded frames.
 *
 * Pipeline per push:
 *   parse RTP payload -> (H.264) FU-A / Single / STAP-A dispatch
 *                  -> build InboundPacket -> internal JB
 *                  -> drain JB pop_frame -> fire user callback
 *
 * Sequence-number tracking and NACK list generation happen inside the
 * receiver.  `pop_nack_entries()` is a synchronous accessor; some impls
 * may prefer a push-style `set_nack_callback()` for low-latency NACK
 * emission (P3).
 *
 * Single-threaded.
 */
class IVideoReceiver : public IPlugin {
public:
    /** Register the frame callback (must be set before push_rtp). */
    virtual void set_frame_callback(VideoReceiverFrameCallback cb) noexcept = 0;

    /** Register an optional push-style NACK callback. Default = no-op. */
    virtual void set_nack_callback(VideoReceiverNackCallback cb) noexcept {
        (void)cb;   // default: implementations that don't push NACKs ignore this
    }

    /** Feed a parsed RTP packet. The receiver extracts seq, marker, ssrc,
     *  timestamp, and payload, then runs the receive pipeline. Any
     *  assembled frames fire the user callback synchronously.
     *  @param packet  RTP packet (header already parsed by IRTP plugin).
     *  @param now_us  Monotonic-clock arrival time (microseconds).
     *  @return kOk on success; kErrInvalidParam on malformed input. */
    virtual Status push_rtp(const VideoRtpPacket& packet,
                            TimestampUs now_us) noexcept = 0;

    /** Pop the accumulated NACK list (missing packet sequence numbers).
     *  Default: pull-style; returns empty if push-style callback is set. */
    virtual std::vector<std::uint16_t> pop_nack_entries() noexcept = 0;

    /** Drive internal timers (drop frames past max_wait_ms). */
    virtual Status tick(TimestampUs now_us) noexcept = 0;

    /** Clear FU-A reassembly state + reset the JB. */
    virtual void reset() noexcept = 0;

    /** Snapshot of receiver counters. */
    virtual VideoReceiverStats stats() const noexcept = 0;

    /** Current receiver configuration (for diagnostics / logs). */
    virtual VideoReceiverConfig config() const noexcept = 0;

    // ---- HW capability flag (R3-Batch) -----------------------------------
    // Default = false. HW decoder plugins (NDK MediaCodec output format,
    // VideoToolbox, NVDEC, VAAPI, VAAPI-DRM, DirectX Video Acceleration,
    // Intel Quick Sync) override to true.
    virtual bool is_hardware_accelerated() const noexcept { return false; }

    // ---- HW backend name (R3-Batch) --------------------------------------
    // Returns a short identifier (e.g. "mediacodec", "videotoolbox",
    // "nvdec", "vaapi", "dxva", "qsv", "software"). Used for
    // diagnostics + capabilities introspection. Default = "software".
    virtual std::string_view hardware_backend() const noexcept {
        return "software";
    }
};

// ---------------------------------------------------------------------------
// Receiver factory
// ---------------------------------------------------------------------------

/** Factory for receiver pipeline plugin instances. */
class IVideoReceiverFactory {
public:
    virtual ~IVideoReceiverFactory() = default;

    /** Unique identifier, e.g. "reference", "media_codec_ndk", "nvidia_nvdec". */
    virtual std::string_view id() const noexcept = 0;

    /** Short human-readable name, e.g. "Reference H.264 depacketizer + JB". */
    virtual std::string_view display_name() const noexcept = 0;

    /** Create a new receiver instance with the given configuration.
     *  The returned IVideoReceiver is in kConstructed state; caller must
     *  call `open()` and register callbacks before push_rtp(). */
    virtual IVideoReceiver* create(VideoReceiverConfig cfg) const = 0;
};

template<class T>
class SimpleVideoReceiverFactory : public IVideoReceiverFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleVideoReceiverFactory(std::string_view id,
                                        std::string_view name) noexcept
        : id_(id), name_(name) {}

    std::string_view id()           const noexcept override { return id_; }
    std::string_view display_name() const noexcept override { return name_; }

    IVideoReceiver* create(VideoReceiverConfig cfg) const override {
        return new T(std::move(cfg));
    }
};

// ===========================================================================
//                                  SENDER
// ===========================================================================

/** Sender configuration. */
struct VideoSenderConfig {
    VideoCodecKind        codec              = VideoCodecKind::kH264;
    std::uint32_t         ssrc               = 0;
    std::uint8_t          payload_type       = 0;

    /** Maximum RTP payload size in bytes (default 1200 — well below the
     *  typical path MTU of ~1500 with safety margin for IP/UDP headers). */
    std::uint32_t         mtu                = 1200;

    /** H.264 packetization mode. Only used when codec == H264. */
    VideoPacketizationMode packetization_mode = VideoPacketizationMode::kNonInterleaved;

    /** Initial RTP sequence number (randomised by the caller in production;
     *  the pipeline just increments from this base). */
    std::uint16_t         initial_seq        = 0;
};

/** Sender statistics. */
struct VideoSenderStats {
    std::uint64_t frames_pushed                = 0;
    std::uint64_t packets_emitted              = 0;
    std::uint64_t fua_frames_packetized        = 0;
    std::uint64_t stapa_frames_packetized      = 0;
    std::uint64_t single_nal_frames_packetized = 0;
    std::uint64_t force_keyframe_calls         = 0;
    std::uint64_t frames_marked_as_keyframe    = 0;
};

/** Per-packet callback: invoked for each emitted RTP packet payload.
 *  - `rtp_packet_payload` is the bytes that go AFTER the 12-byte RTP
 *    fixed header (i.e. the RTP payload field). The receiver-side caller
 *    is expected to construct the full RTP packet by prepending its own
 *    RTP header.
 *  - `rtp_timestamp` is the 32-bit RTP media clock timestamp.
 *  - `seq` is the per-stream 16-bit RTP sequence number.
 *  - `marker` is the RTP marker bit (true on the last packet of an AU).
 *
 *  The span is only valid for the duration of the callback; the sender
 *  reuses an internal buffer for subsequent packets. */
using VideoSenderPacketCallback =
    std::function<void(BufferView rtp_packet_payload,
                       std::uint32_t rtp_timestamp,
                       std::uint16_t seq,
                       bool marker)>;

/**
 * @brief Sender pipeline: encoded frames → RTP packet payloads.
 *
 * Pipeline per push:
 *   split bitstream into NALUs / VP8 partitions
 *                  -> (H.264) decide FU-A vs STAP-A vs Single
 *                  -> emit RTP payload via packet_callback
 *
 * Sequence numbers and the seq_base advance monotonically; the RTP
 * timestamp is supplied by the caller per push_frame() call.
 *
 * Single-threaded.
 */
class IVideoSender : public IPlugin {
public:
    /** Register the per-packet callback (must be set before push_frame). */
    virtual void set_packet_callback(VideoSenderPacketCallback cb) noexcept = 0;

    /** Feed one encoded (compressed) frame.  The pipeline packetizes it
     *  and fires the callback for each emitted RTP packet payload.
     *  @param frame             Compressed access unit (codec-specific).
     *  @param rtp_timestamp     32-bit RTP timestamp (e.g. 90000/fps).
     *  @return kOk on success; kErrUnsupported if the codec + mode combo
     *          is not implemented; kErrInvalidParam on bad input. */
    virtual Status push_frame(const EncodedVideoFrame& frame,
                              std::uint32_t rtp_timestamp) noexcept = 0;

    /** Mark the next frame as a keyframe response (PLI / FIR was received).
     *  The next push_frame() call will record this in stats; the caller
     *  is responsible for actually emitting a keyframe (i.e. for H.264 an
     *  IDR slice) at that point. */
    virtual void force_keyframe() noexcept = 0;

    /** Snapshot of sender counters. */
    virtual VideoSenderStats stats() const noexcept = 0;

    /** Currently allocated next sequence number (peek for tests). */
    virtual std::uint16_t next_seq() const noexcept = 0;

    /** Current sender configuration (for diagnostics / logs). */
    virtual VideoSenderConfig config() const noexcept = 0;

    // ---- HW capability flag (R3-Batch) -----------------------------------
    // Default = false. HW encoder plugins (NDK MediaCodec, VideoToolbox,
    // NVENC, VAAPI Encode, Intel Quick Sync, AMD VCE/VCN) override to true.
    virtual bool is_hardware_accelerated() const noexcept { return false; }

    // ---- HW backend name (R3-Batch) --------------------------------------
    // Returns a short identifier (e.g. "mediacodec", "videotoolbox",
    // "nvenc", "vaapi-encode", "qsv", "amf", "software"). Used for
    // diagnostics + capabilities introspection. Default = "software".
    virtual std::string_view hardware_backend() const noexcept {
        return "software";
    }
};

// ---------------------------------------------------------------------------
// Sender factory
// ---------------------------------------------------------------------------

/** Factory for sender pipeline plugin instances. */
class IVideoSenderFactory {
public:
    virtual ~IVideoSenderFactory() = default;

    /** Unique identifier, e.g. "reference", "media_codec_ndk", "video_toolbox". */
    virtual std::string_view id() const noexcept = 0;

    /** Short human-readable name, e.g. "Reference H.264 FU-A / STAP-A packetizer". */
    virtual std::string_view display_name() const noexcept = 0;

    /** Create a new sender instance with the given configuration.
     *  The returned IVideoSender is in kConstructed state; caller must
     *  call `open()` and register the packet callback before push_frame(). */
    virtual IVideoSender* create(VideoSenderConfig cfg) const = 0;
};

template<class T>
class SimpleVideoSenderFactory : public IVideoSenderFactory {
    std::string_view id_;
    std::string_view name_;
public:
    explicit SimpleVideoSenderFactory(std::string_view id,
                                      std::string_view name) noexcept
        : id_(id), name_(name) {}

    std::string_view id()           const noexcept override { return id_; }
    std::string_view display_name() const noexcept override { return name_; }

    IVideoSender* create(VideoSenderConfig cfg) const override {
        return new T(std::move(cfg));
    }
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_VIDEO_PIPELINE_HPP
