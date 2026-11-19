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
#include <nimrtc/core/api_version.hpp>
#include "nimrtc/plugins/base.hpp"
// Reuse VideoCodecKind / VideoPacketizationMode enum from video_codec.hpp.
// EncodedVideoFrame is also defined there; we reuse it so the receiver→
// codec and sender←codec paths don't need a translation layer.
#include "nimrtc/plugins/video_codec.hpp"

#ifndef NIMRTC_PLUGINS_VIDEO_PIPELINE_HPP
#define NIMRTC_PLUGINS_VIDEO_PIPELINE_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v0.13.x follow-up.

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

enum class NIMRTC_API_STABLE VideoPacketizationMode : std::uint8_t {
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
struct NIMRTC_API_STABLE VideoRtpPacket {
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
struct NIMRTC_API_STABLE VideoReceiverConfig {
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

    /** The RFC 4588 retransmission payload type, or 0 when RTX was not
     *  negotiated.
     *
     *  RFC 4588 §4.1: a retransmission travels on a *different* payload
     *  type than the original, so the receiver can tell the two apart
     *  and restore the original sequence number from the two-octet OSN
     *  header.  0 means "no retransmission stream" — the receiver then
     *  treats every inbound packet as a media packet.
     *
     *  The engine sets this from the negotiated `a=fmtp:<pt> apt=` line
     *  (see `nimrtc::rtx::parse_apt`), so the value is the one the peer
     *  actually agreed to. */
    std::uint8_t   rtx_payload_type = 0;

    /** Forwarded to the underlying jitter buffer (implementation-specific). */
    std::uint32_t  initial_jitter_buffer_ms = 40;
    std::uint32_t  max_jitter_buffer_ms     = 200;
    std::uint32_t  max_wait_ms              = 200;
    std::uint32_t  max_inflight_frames      = 8;
    bool           emit_nacks               = true;

    /** Enable the adaptive playout delay of the video jitter buffer.
     *
     *  false (default) — a complete frame is delivered as soon as it is
     *    assembled (no playout delay).
     *  true — the frame is held until its playout deadline has passed;
     *    the deadline follows the smoothed interarrival jitter estimate
     *    (base delay + 4·J, clamped to [initial, max]). */
    bool           adaptive_playout         = false;

    /** If true (recommended for H.264), the receiver runs FU-A reassembly
     *  itself. Otherwise the codec layer is expected to. */
    bool           expect_fu_a   = true;
};

/** Receiver statistics. */
struct NIMRTC_API_STABLE VideoReceiverStats {
    std::uint64_t packets_pushed       = 0;
    std::uint64_t frames_emitted       = 0;
    std::uint64_t frames_incomplete    = 0;
    std::uint64_t fua_fragments_seen   = 0;
    std::uint64_t stap_a_packets_seen  = 0;
    std::uint64_t nack_entries_emitted = 0;
    std::uint64_t sequence_gaps_seen   = 0;
    /** Key-frame requests raised because picture data was lost (v1.1 Q3). */
    std::uint64_t keyframe_requests_emitted = 0;
    /** Retransmission packets received and unwrapped (RFC 4588). */
    std::uint64_t rtx_packets_received = 0;
    /** Retransmission packets that could not be unwrapped (a payload too
     *  short to carry the two-octet OSN header). */
    std::uint64_t rtx_packets_malformed = 0;
};

/** Frame callback: invoked on the receiver thread for every assembled frame.
 *  The frame's `payload` is owned by the receiver and is only valid for
 *  the duration of the callback; copy out if you need to keep it. */
using NIMRTC_API_STABLE VideoReceiverFrameCallback =
    std::function<void(const EncodedVideoFrame& frame, TimestampUs now_us)>;

/** Decoded frame callback: invoked once a frame has been fully decoded
 *  into a VideoFrame (CPU pixels or GPU surface — see VideoFrame::has_gpu).
 *
 *  Zero-copy contract:
 *   - The receiver holds a ref on the underlying GpuBuffer / VideoFrameBuffer.
 *   - The callback may do `out_frame.buffer->acquire_ref()` to keep it alive
 *     past the call (release_ref() returns it to the pool).
 *   - When the callback returns, the receiver drops its ref; if no one
 *     else holds a ref, the buffer is recycled to the backend's pool.
 *
 *  HW backends will surface the decoded picture as a GPU handle; software
 *  backends surface it as a VideoFrameBuffer (CPU pixels). The callback
 *  SHOULD branch on `frame.has_gpu()` to pick the appropriate consumer
 *  (e.g. DirectXTexture path vs BGRA → window blit path).
 *
 *  Default receivers don't decode — they emit the encoded callback only.
 *  Set `set_decoded_callback` to opt into decoding + zero-copy display.
 */
using NIMRTC_API_STABLE VideoReceiverDecodedCallback =
    std::function<void(const VideoFrame& frame, TimestampUs now_us)>;

/** Callback fired when the receiver decides a packet should be requested
 *  via NACK.  Defaults: implementers populate a seq list and the engine
 *  sends a NACK RTCP FB for each entry. */
using NIMRTC_API_STABLE VideoReceiverNackCallback =
    std::function<void(std::vector<std::uint16_t> missing_seqs)>;

/**
 * @brief Feedback report produced by a receiver (v1.1 Q3).
 *
 *  `missing_seqs` holds the RTP sequence numbers the receiver detected
 *  as lost — the NACK candidates of RFC 4585 §6.2.1.  `want_keyframe`
 *  asks the peer for a new key frame — RFC 4585 §6.3.1 (PLI) or
 *  RFC 5104 §4.3.1 (FIR).
 *
 *  The report is plain data: the RTCP wire format is produced by the
 *  receiver plugin itself (`build_feedback_rtcp()`), so the engine
 *  never has to know how a NACK or a PLI is encoded.
 */
struct NIMRTC_API_STABLE VideoFeedbackReport {
    /** Lost RTP sequence numbers (NACK candidates). */
    std::vector<std::uint16_t> missing_seqs;

    /** True when the receiver needs a new key frame (PLI / FIR). */
    bool                       want_keyframe = false;
};

/** Push-style feedback callback (loss report and/or key-frame request).
 *  Fired synchronously on the thread that fed the receiver. */
using NIMRTC_API_STABLE VideoReceiverFeedbackCallback =
    std::function<void(const VideoFeedbackReport& report)>;

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
class NIMRTC_API_STABLE IVideoReceiver : public IPlugin {
public:
    /** Register the frame callback (must be set before push_rtp). */
    virtual void set_frame_callback(VideoReceiverFrameCallback cb) noexcept = 0;

    /** Register an optional push-style NACK callback. Default = no-op. */
    virtual void set_nack_callback(VideoReceiverNackCallback cb) noexcept {
        (void)cb;   // default: implementations that don't push NACKs ignore this
    }

    // ---- Feedback bridge (v1.1 Q3) ---------------------------------------
    //
    // The receiver detects the loss and says *what* is wrong; the engine
    // owns the RTCP transport and therefore needs the *bytes*.  These
    // three methods close that gap without leaking the RTCP wire format
    // into the engine (the engine only sees an opaque byte buffer).

    /** Register the push-style feedback callback (loss + key-frame
     *  request).  Default = no-op; receivers that cannot report feedback
     *  leave this as the default.
     *
     *  A receiver may implement either this callback or the older
     *  `set_nack_callback()` — the engine registers both, so an
     *  implementation should support whichever it prefers. */
    virtual void set_feedback_callback(VideoReceiverFeedbackCallback cb) noexcept {
        (void)cb;
    }

    /** Number of bytes `build_feedback_rtcp()` needs for `report`.
     *  0 when the report carries nothing to send (no loss, no key-frame
     *  request). */
    virtual std::size_t feedback_rtcp_size(
        const VideoFeedbackReport& report) const noexcept {
        (void)report;
        return 0;
    }

    /** Encode `report` as RTCP feedback bytes (NACK and/or PLI).
     *
     *  @param report      The feedback to encode.
     *  @param sender_ssrc SSRC of the feedback sender (this side).
     *  @param media_ssrc  SSRC of the media source the feedback is about
     *                     (the peer's stream).
     *  @param out         Destination buffer, sized with
     *                     `feedback_rtcp_size()`.
     *  @return The number of bytes written; 0 when the report is empty or
     *          `out` is too small. */
    virtual std::size_t build_feedback_rtcp(const VideoFeedbackReport& report,
                                            std::uint32_t sender_ssrc,
                                            std::uint32_t media_ssrc,
                                            core::MutableByteSpan out) const noexcept {
        (void)report; (void)sender_ssrc; (void)media_ssrc; (void)out;
        return 0;
    }

    /** Register the decoded-frame callback for zero-copy display path.
     *  Default = no-op (decoded frames are dropped). Receivers that
     *  don't decode at all (e.g. depacketizer-only) leave this as the
     *  default; receivers with a HW decoder override and fire the
     *  callback once per decoded picture.
     *
     *  When the receiver supports zero-copy, the callback receives a
     *  VideoFrame whose `has_gpu()` is true and whose `gpu_buffer()`
     *  points at a backend-specific surface (CVPixelBuffer, ID3D11Texture,
     *  VASurface, …). When the receiver is CPU-only (no HW decoder),
     *  `has_gpu()` is false and `cpu_buffer()` points at an I420 frame.
     */
    virtual void set_decoded_callback(VideoReceiverDecodedCallback cb) noexcept {
        (void)cb;
    }

    /** Feed a parsed RTP packet. The receiver extracts seq, marker, ssrc,
     *  timestamp, and payload, then runs the receive pipeline. Any
     *  assembled frames fire the user callback synchronously.
     *  @param packet  RTP packet (header already parsed by IRTP plugin).
     *  @param now_us  Monotonic-clock arrival time (microseconds).
     *  @return kOk on success; kErrInvalidParam on malformed input. */
    virtual Status push_rtp(const VideoRtpPacket& packet,
                            TimestampUs now_us) noexcept = 0;

    // ---- RFC 4588 retransmission (RTX) — the receive side ---------------
    //
    // A retransmission arrives on the *retransmission* payload type, with
    // its own sequence number, and carries the original packet's payload
    // behind a two-octet OSN header (RFC 4588 §4.1).  The receiver has
    // to unwrap it and put the payload back under the *original* sequence
    // number — otherwise the jitter buffer sees a packet of a brand-new
    // frame and the original frame stays incomplete.
    //
    // The seam carries the two halves separately:
    //
    //   * `set_rtx_payload_type()` says *which* payload type is the
    //     retransmission stream (the engine reads it out of the
    //     negotiated `apt=` parameter);
    //   * `push_rtp_rtx()` hands one retransmission packet over.
    //
    // The default implementations ignore the RTX stream (a receiver that
    // does not implement RFC 4588 keeps working exactly as before), and
    // the default `push_rtp_rtx()` drops the packet.

    /** Tell the receiver which payload type carries retransmissions.
     *
     *  @param rtx_pt The retransmission payload type, or 0 to disable the
     *                RTX path (the default). */
    virtual void set_rtx_payload_type(std::uint8_t rtx_pt) noexcept {
        (void)rtx_pt;
    }

    /** Feed one RFC 4588 retransmission packet.
     *
     *  @param packet  The retransmission packet.  Its `seq` is the
     *                 *retransmission stream's* sequence number (its own
     *                 space, RFC 4588 §4); the original's is inside the
     *                 payload's OSN header.  Its `rtp_timestamp` and
     *                 `marker` are the *original's* (RFC 4588 §4.1).
     *  @param now_us  Monotonic-clock arrival time (microseconds).
     *  @return kOk when the packet was unwrapped and restored; kErrInvalidParam
     *          when the payload was too short to carry the OSN header;
     *          kErrNotReady when no RTX payload type was configured. */
    virtual Status push_rtp_rtx(const VideoRtpPacket& packet,
                                TimestampUs now_us) noexcept {
        (void)packet; (void)now_us;
        return kErrNotReady;
    }

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
class NIMRTC_API_STABLE IVideoReceiverFactory {
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
class NIMRTC_API_STABLE SimpleVideoReceiverFactory : public IVideoReceiverFactory {
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
struct NIMRTC_API_STABLE VideoSenderConfig {
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

    /** The RFC 4588 retransmission payload type, or 0 when RTX was not
     *  negotiated.
     *
     *  RFC 4588 §4.1: a retransmission SHALL be sent on a different
     *  payload type than the original, so the receiver can distinguish
     *  the two and restore the original sequence number from the
     *  two-octet OSN header.  0 means "answer a NACK by re-sending the
     *  original packet" — the pre-RTX behaviour, which RFC 4588 §4.1
     *  explicitly is *not* RTX.
     *
     *  The engine sets this from the negotiated `a=fmtp:<pt> apt=` line,
     *  so the value is the one the peer actually agreed to. */
    std::uint8_t          rtx_payload_type   = 0;
};

/** Sender statistics. */
struct NIMRTC_API_STABLE VideoSenderStats {
    std::uint64_t frames_pushed                = 0;
    std::uint64_t packets_emitted              = 0;
    std::uint64_t fua_frames_packetized        = 0;
    std::uint64_t stapa_frames_packetized      = 0;
    std::uint64_t single_nal_frames_packetized = 0;
    std::uint64_t force_keyframe_calls         = 0;
    std::uint64_t frames_marked_as_keyframe    = 0;
    /** Packets re-sent in response to a NACK (v1.1 Q3). */
    std::uint64_t packets_retransmitted        = 0;
    /** NACKs that could not be answered (the packet had already left the
     *  retransmission cache) (v1.1 Q3). */
    std::uint64_t packets_retransmit_missed    = 0;
    /** Packets re-sent as RFC 4588 retransmissions (on the RTX payload
     *  type, with the OSN header). */
    std::uint64_t rtx_packets_emitted           = 0;
    // ---- The key-frame storm limiter (remote-desktop plan B14) -----------
    /** Key-frame requests the limiter was offered. */
    std::uint64_t keyframe_requests             = 0;
    /** Key-frame requests the limiter granted (a key frame was produced). */
    std::uint64_t keyframes_granted             = 0;
    /** Key-frame requests the limiter deferred (held back, not dropped). */
    std::uint64_t keyframes_deferred            = 0;
    /** Key-frame requests the limiter rejected (the budget was spent). */
    std::uint64_t keyframes_rejected            = 0;
    /** Grants that bypassed the limiter because the request carried the
     *  codec's parameter sets (a resolution change / a codec switch).
     *  Those are never deferred: deferring them breaks the decode. */
    std::uint64_t keyframes_forced              = 0;
};

/**
 * @brief The key-frame storm limiter's policy (remote-desktop plan B14).
 *
 * The limiter sits between "a key frame was asked for" and "the encoder
 * is armed".  A peer that asks for a key frame every 100 ms would
 * otherwise produce an IDR storm: an IDR costs roughly an order of
 * magnitude more bytes than an inter frame, so the storm saturates the
 * link and makes the picture *worse*.
 *
 * This is the **configuration** half of the limiter; the counters live
 * in `VideoSenderStats`.  The engine owns the policy (the host does not
 * configure the sender's internals), and the seam is where the
 * configuration crosses.
 */
struct NIMRTC_API_STABLE VideoKeyframeLimiterConfig {
    /** The minimum interval between two granted key frames, in
     *  microseconds.  0 disables the interval gate.
     *
     *  The default is 250 ms — the same interval the FIR coalescer
     *  (RFC 5104 §4.3.1.1) applies, because the two are the *same
     *  policy* seen from two ends: the coalescer limits how often a
     *  key-frame *request* may go out, the limiter how often a key
     *  *frame* may be produced. */
    std::int64_t min_interval_us = 250'000;

    /** The key-frame **cost budget** per window, expressed in "inter
     *  frame equivalents".  A key frame costs `keyframe_cost_ratio`
     *  times an inter frame, so the limiter refuses to spend more than
     *  the budget inside one window.  0 disables the cost gate. */
    std::uint32_t keyframe_budget = 32;

    /** How many times more expensive a key frame is than an inter
     *  frame.  Used to convert the budget into a "key frames per
     *  window" bound.  Must be at least 1. */
    std::uint32_t keyframe_cost_ratio = 8;

    /** The hard cap on granted key frames inside one window, whatever
     *  the cost budget says.  0 disables the cap. */
    std::uint32_t max_keyframes_per_window = 4;

    /** The length of the accounting window, in microseconds. */
    std::int64_t window_us = 1'000'000;

    /** True iff the limiter does nothing (every request is granted).
     *  The default is false — the limiter is *on*. */
    bool disabled = false;
};

// ---------------------------------------------------------------------------
// Incremental (dirty-rect) encode — remote-desktop plan B9
// ---------------------------------------------------------------------------

/**
 * @brief The stream-level content type of a video stream.
 *
 * ADR-022 Decision 5: the content type is carried **explicitly** and
 * is *never* inferred from the capture device's kind.  A document
 * camera is camera-shaped but screen-like, and a screen playing video
 * is screen-shaped but motion-like — the host decides.
 *
 * The gate matters for the incremental-encode mechanism: it only
 * engages for screen content, because a camera stream has no unchanged
 * area worth skipping.
 */
enum class NIMRTC_API_STABLE VideoContentType : std::uint8_t {
    /** Nobody said.  The camera default — the safe choice. */
    kUnspecified = 0,
    /** Camera content.  The incremental mechanism never engages. */
    kCamera = 1,
    /** Screen content.  The incremental mechanism may engage. */
    kScreen = 2,
};

/**
 * @brief What the incremental-encode arbiter decided for one frame.
 *
 * The decision is *reported* rather than hidden: plan B9's acceptance
 * criterion says "the mechanism is disabled when the codec does not
 * support it and **the fallback is reported**".
 */
enum class NIMRTC_API_STABLE VideoIncrementalDecision : std::uint8_t {
    /** Nothing changed — the frame carries no region. */
    kNoChange = 0,
    /** The whole frame changed — an ordinary full encode. */
    kFullFrame = 1,
    /** The encoder was asked to code only the changed region. */
    kIncremental = 2,
    /** The codec cannot express the region — the sender falls back to a
     *  full key frame, and the fallback is counted. */
    kFallbackKeyframe = 3,
    /** The mechanism did not engage: the content is not screen content. */
    kNotScreenContent = 4,
};

/**
 * @brief The incremental-encode mechanism's counters.
 *
 * They make the mechanism observable from the outside: a host that
 * expects a static desktop to cost nothing can see `frames_unchanged`
 * move, and a host whose codec cannot express a region can see the
 * `fallback_keyframes` counter rather than wondering why the byte rate
 * did not drop.
 */
struct NIMRTC_API_STABLE VideoIncrementalStats {
    /** Frames offered to the arbiter. */
    std::uint64_t frames_seen            = 0;
    /** Frames that carried no change at all (the static-desktop case). */
    std::uint64_t frames_unchanged        = 0;
    /** Frames encoded incrementally (the mechanism engaged). */
    std::uint64_t frames_incremental      = 0;
    /** Frames that needed a full encode because the whole frame changed. */
    std::uint64_t frames_full             = 0;
    /** The **fallback** counter: frames that fell back to a full key
     *  frame because the codec cannot express the region. */
    std::uint64_t fallback_keyframes      = 0;
    /** Frames where the mechanism did not engage because the content is
     *  not screen content (or the mechanism was switched off). */
    std::uint64_t skipped_not_screen      = 0;
    /** Region-refresh messages emitted for the peer. */
    std::uint64_t region_refreshes_sent   = 0;
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
using NIMRTC_API_STABLE VideoSenderPacketCallback =
    std::function<void(BufferView rtp_packet_payload,
                       std::uint32_t rtp_timestamp,
                       std::uint16_t seq,
                       bool marker)>;

// ---------------------------------------------------------------------------
// The RFC 4588 retransmission seam (the sender side)
// ---------------------------------------------------------------------------

/**
 * @brief One retransmission packet the sender wants put on the wire.
 *
 *  RFC 4588 §4.1 shapes the packet in three ways, and all three are
 *  visible here:
 *
 *   * the **payload** is the two-octet OSN header followed by the
 *     original packet's payload — the original's *payload*, not the
 *     original's RTP header;
 *   * the **timestamp** and the **marker bit** are the *original*
 *     packet's (RFC 4588 §4.1: "the RTP header of the retransmission
 *     packet is identical to the RTP header of the original packet,
 *     except for the sequence number");
 *   * the **sequence number** is *not* here — the retransmission stream
 *     has its own sequence space (RFC 4588 §4), and the SSRC and the
 *     payload type are the engine's (they are per-session values that
 *     come out of the SDP negotiation).
 */
struct NIMRTC_API_STABLE VideoSenderRtxPacket {
    /** The RFC 4588 §4.1 retransmission payload: the two-octet OSN
     *  header followed by the original packet's payload. */
    BufferView    payload;

    /** The **original** packet's RTP timestamp. */
    std::uint32_t rtp_timestamp = 0;

    /** The **original** packet's marker bit. */
    bool          marker = false;

    /** The **original** packet's sequence number (the OSN the payload's
     *  header carries).  The engine needs it to log and to know which
     *  media sequence space the packet belongs to. */
    std::uint16_t original_seq = 0;

    /** The retransmission stream's **own** sequence number.
     *
     *  RFC 4588 §4 gives the retransmission stream its own sequence
     *  number space, so a retransmission does NOT reuse the original's
     *  number on the wire — the original's number is inside the OSN
     *  header. */
    std::uint16_t rtx_seq = 0;
};

/** Callback fired once per retransmission packet.
 *
 *  Fired synchronously on the thread that fed the RTCP feedback to the
 *  sender.  The span is only valid for the duration of the callback. */
using NIMRTC_API_STABLE VideoSenderRtxCallback =
    std::function<void(const VideoSenderRtxPacket& packet)>;

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
class NIMRTC_API_STABLE IVideoSender : public IPlugin {
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

    // ---- Feedback bridge (v1.1 Q3) ---------------------------------------
    //
    // The mirror image of the receiver side: the engine owns the RTCP
    // transport and hands the received feedback bytes to the sender,
    // which knows how to answer them.

    /** Apply an inbound RTCP feedback packet (a single feedback packet
     *  or a compound packet — every sub-packet is processed).
     *
     *  The sender answers:
     *   - NACK (RFC 4585 §6.2.1) → the listed packets are retransmitted
     *     through the packet callback (same sequence number, timestamp
     *     and marker as the original).
     *   - PLI  (RFC 4585 §6.3.1) → a key frame is requested.
     *   - FIR  (RFC 5104 §4.3.1) → a key frame is requested.
     *
     *  @param rtcp  The received (plaintext) RTCP bytes.
     *  @return The number of actions performed (retransmissions plus
     *          key-frame requests).  Default = 0 (no feedback support). */
    virtual int apply_feedback_rtcp(BufferView rtcp) noexcept {
        (void)rtcp;
        return 0;
    }

    /** Number of key-frame requests (PLI / FIR) recorded since the last
     *  call; the counter is cleared by this call.
     *
     *  The engine polls it after `apply_feedback_rtcp()` so it can force
     *  an IDR at the *encoder* — the packetizer alone cannot produce
     *  one.  Default = 0. */
    virtual std::uint32_t take_keyframe_requests() noexcept {
        return 0;
    }

    // ---- The key-frame storm limiter (remote-desktop plan B14) -----------
    //
    // The limiter is what turns a key-frame-request *storm* into a
    // bounded IDR rate.  It lives behind the seam because it is the
    // sender's business: the sender knows when it is about to produce a
    // key frame, and only it can arbitrate.
    //
    // The engine configures the policy once (at open() time) and then
    // reads the counters back.  Both halves are optional: a sender that
    // does not implement the limiter keeps the default behaviour, and
    // the default configuration is the limiter's own default (on).

    /** Configure the key-frame storm limiter.
     *
     *  @param cfg The limiter's policy.  The engine calls this once,
     *             after the sender was created.  Default = no-op. */
    virtual void set_keyframe_limiter_config(
            const VideoKeyframeLimiterConfig& cfg) noexcept {
        (void)cfg;
    }

    /** The limiter's current configuration.  Default = the defaults. */
    virtual VideoKeyframeLimiterConfig keyframe_limiter_config_seam() const noexcept {
        return {};
    }

    /** True iff a key-frame request is still waiting to be re-offered.
     *
     *  A deferred request is **held**, not dropped: the limiter says
     *  "not yet", and the caller re-offers it once the minimum interval
     *  has elapsed, so the picture still recovers even when the peer
     *  only asked once.  Default = false (nothing pending). */
    virtual bool keyframe_request_pending() const noexcept {
        return false;
    }

    /** Re-offer the held key-frame request.
     *
     *  The engine calls this from `tick()`: the limiter arbitrates, and
     *  the answer is published through `take_keyframe_requests()` — the
     *  same path an inbound PLI takes, so the engine has *one* place
     *  that arms the encoder.
     *
     *  @param now_us Monotonic timestamp in microseconds.
     *  @return The number of key frames granted (0 or 1).  Default = 0. */
    virtual std::uint32_t retry_pending_keyframe(std::int64_t /*now_us*/) noexcept {
        return 0;
    }

    // ---- The incremental (dirty-rect) encode mechanism (plan B9) --------
    //
    // The mechanism has three parts and the seam carries all three:
    //
    //   1. the *capture* reports the changed rectangles (the engine
    //      hands them over here),
    //   2. the *sender* arbitrates whether the region can be coded
    //      incrementally (the codec's capability decides), and
    //   3. the *peer* is told which region changed (the region-refresh
    //      message comes back through the callback).
    //
    // The arbiter lives behind the seam because it needs the codec's
    // capability (`IVideoEncoder::supports_incremental_encode()`), and
    // only the sender is in a position to hold both.

    /** The stream's content type.
     *
     *  ADR-022 Decision 5: the content type is carried explicitly and
     *  is never inferred from the capture device's kind.  The default is
     *  `kUnspecified` (the camera default — the behaviour that is
     *  correct for a camera and merely *safe* for a screen). */
    virtual void set_content_type(VideoContentType type) noexcept {
        (void)type;
    }
    virtual VideoContentType content_type() const noexcept {
        return VideoContentType::kUnspecified;
    }

    /** Enable / disable the incremental-encode mechanism.
     *
     *  Disabled (the default) means every frame takes the ordinary full
     *  encode path.  The mechanism is additionally gated on the content
     *  type: it only engages for screen content. */
    virtual void set_incremental_encode_enabled(bool enabled) noexcept {
        (void)enabled;
    }
    virtual bool incremental_encode_enabled() const noexcept {
        return false;
    }

    /** Offer one frame's changed region to the incremental arbiter.
     *
     *  @param dirty     The changed rectangles, in source pixels.  An
     *                   empty list means "nothing changed" — the
     *                   static-desktop case.
     *  @param full_frame True iff the whole frame changed.
     *  @return The decision.  Default = `kFullFrame` (no mechanism). */
    virtual VideoIncrementalDecision offer_dirty_region(
            const std::vector<VideoRegion>& dirty, bool full_frame) noexcept {
        (void)dirty; (void)full_frame;
        return VideoIncrementalDecision::kFullFrame;
    }

    /** The region-refresh message the sender wants sent to the peer.
     *
     *  The message is produced by the sender (it knows the media SSRC
     *  and the regions it just coded) but *sent* by whoever owns the
     *  RTCP transport — the engine.  The bytes are only valid until the
     *  next call.  Default = empty (nothing to signal). */
    virtual core::ByteSpan take_region_refresh() noexcept {
        return {};
    }

    /** The incremental mechanism's counters.  Default = zeros. */
    virtual VideoIncrementalStats incremental_stats() const noexcept {
        return {};
    }

    /** Attach the encoder the incremental requests are pushed to.
     *
     *  The sender does not own the encoder (the engine does), so the
     *  pointer is borrowed and must outlive the sender's use of it.
     *  The engine wires it once, right after both plugins exist.
     *
     *  Default = no-op (a sender that does not implement the mechanism
     *  ignores it). */
    virtual void set_incremental_encoder(plugins::IVideoEncoder* encoder) noexcept {
        (void)encoder;
    }

    /** P3 — interface stable; implementation lands in 1.0.
     *  Sets the timeline reference frame metadata for the next encode call.
     *  capture_ts_us : monotonic capture timestamp in microseconds.
     *  frame_id      : monotonically-increasing per-stream frame counter.
     *  rtp_timestamp: 90kHz RTP clock value pinned to capture_ts_us.
     *
     *  Compatibility note (ADR-017, v1.0.0): the active timeline carrier
     *  is WebRTC abs-send-time + RTCP-SR-based NTP↔RTP-TS mapping
     *  (see `nimrtc/rtp/timeline.hpp`). set_ref_frame() is preserved as
     *  an ABI-stable no-op for plugin implementers; the engine reads
     *  capture_ts/frame_seq directly from the EncodedVideoFrame and the
     *  abs-send-time 24-bit unsigned timestamp bound to the wire.
     *
     *  Default impl is the no-op stub for binary compatibility with
     *  plugin adapters that pre-date v1.0.0. */
    virtual void set_ref_frame(std::int64_t /*capture_ts_us*/,
                               std::uint32_t /*frame_id*/,
                               std::uint32_t /*rtp_timestamp*/) noexcept {
        // no-op: timeline propagation lives in the engine + abs-send-time
        // extension; see ADR-017 for the long-term plan.
    }

    /** Snapshot of sender counters. */
    virtual VideoSenderStats stats() const noexcept = 0;

    // ---- RFC 4588 retransmission (RTX) — the sender side ---------------
    //
    // A NACK asks the sender to repeat one packet.  With RTX negotiated
    // the repeat travels on the *retransmission* payload type, wrapped
    // in the two-octet OSN header (RFC 4588 §4.1), so the receiver can
    // tell it apart from the original and put it back where it belongs.
    //
    // The seam splits the work:
    //
    //   * the *sender plugin* owns the retransmission cache and knows
    //     which original packet a NACK refers to;
    //   * the *engine* owns the RTP header, the SSRC, the payload type
    //     and the transport — so it needs the retransmission's bytes
    //     and the original's timestamp / marker, which is exactly what
    //     `VideoSenderRtxPacket` carries.
    //
    // A sender that does not implement RTX keeps the default behaviour
    // (re-send the original packet through the ordinary packet
    // callback), which is what the pre-RTX path did.

    /** Register the callback that carries the retransmission packets.
     *
     *  The engine registers it when it learns that RTX was negotiated;
     *  when no callback is registered the sender falls back to
     *  re-sending the original packet. */
    virtual void set_rtx_packet_callback(VideoSenderRtxCallback cb) noexcept {
        (void)cb;
    }

    /** Enable or disable the RFC 4588 retransmission path.
     *
     *  @param enabled True when RTX was negotiated (the engine sets this
     *                 from the negotiated `apt=` parameter). */
    virtual void set_rtx_enabled(bool enabled) noexcept {
        (void)enabled;
    }

    /** True iff the RFC 4588 retransmission path is on. */
    virtual bool rtx_enabled() const noexcept { return false; }

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
class NIMRTC_API_STABLE IVideoSenderFactory {
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
class NIMRTC_API_STABLE SimpleVideoSenderFactory : public IVideoSenderFactory {
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
