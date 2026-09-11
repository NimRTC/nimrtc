/**
 * @file nimrtc/engine/engine.hpp
 * @brief NimRTCEngine — top-level composable engine that wires modules.
 *
 * P1.1 implementation:
 *   - Uses PluginRegistry to create ICE transport + SDP + RTP + JB + Audio3A
 *     plugin instances at open() time (ADR-001 wiring).
 *   - BWE (AIMD) and Scheduler (strict-priority) plugins are created via
 *     PluginRegistry and injected into the ICE transport.
 *   - H.264 video codec plugin is resolved via PluginRegistry at open().
 *   - Generates RFC 8829-conformant SDP offers and parses answers.
 *   - Drives the wire protocol on tick() / drain callbacks.
 *   - Wires DTLS handshake + SRTP encryption + Opus codec.
 *
 * ## Plugin wiring (ADR-001)
 *
 * Plugins are selected by string ID via EngineConfig fields. The
 * default IDs match what the built-in plugins register under in their
 * `register_default_plugins()` entry points:
 *
 *   cfg.transport_name    = "ice";       // nimrtc::ice::register_default_plugins()
 *   cfg.rtp_name        = "webrtc";    // nimrtc::rtp::register_default_plugins()
 *   cfg.sdp_name        = "webrtc";    // nimrtc::sdp::register_default_plugins()
 *   cfg.jb_name         = "adaptive";   // nimrtc::jb::register_default_plugins()
 *   cfg.audio3a_name    = "webrtc";    // nimrtc::audio3a::register_default_plugins()
 *   cfg.codec_name      = "opus";      // nimrtc::opus::register_default_plugins() (if enabled)
 *   cfg.bwe_name        = "aimd";      // nimrtc::bwe::register_default_plugins()
 *   cfg.scheduler_name   = "strict_priority"; // nimrtc::sched::register_default_plugins()
 *   cfg.video_codec_name = "h264";     // nimrtc::h264::register_default_plugins()
 *
 * Open() calls `core::register_all_default_plugins()` first, so callers
 * normally do not need to do anything to populate the registry.
 * normally do not need to do anything to populate the registry.
 *
 * @note P1 — DTLS-SRTP keying material is now derived after the DTLS
 *       handshake completes; SRTP protects RTP in both directions.
 */

#ifndef NIMRTC_ENGINE_ENGINE_HPP
#define NIMRTC_ENGINE_ENGINE_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Plugin-seam headers only — engine is a composition layer and must NOT
// pull in concrete module headers (Layout Invariant 4 + ADR-001).
// Applications get the full set transitively via nimrtc::engine's CMake
// PUBLIC deps on the concrete modules, so callers that want concrete
// types can still include them themselves.
#include <nimrtc/core/registry.hpp>     // PluginRegistry (Layout Invariant 6)
#include <nimrtc/plugins/transport.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/plugins/audio3a.hpp>
#include <nimrtc/plugins/codec.hpp>
#include <nimrtc/plugins/video_source.hpp>
#include <nimrtc/plugins/video_sink.hpp>
#include <nimrtc/plugins/video_pipeline.hpp>
#include <nimrtc/plugins/video_codec.hpp>

// Forward declarations of concrete module types — full definitions live
// in module headers (e.g. <nimrtc/dtls/dtls.hpp>) and are pulled in by
// src/engine/src/engine.cpp.  This keeps engine.hpp free of concrete
// module dependency surface (Layout Invariant 4).
namespace nimrtc::sdp {
struct SessionDescription;
} // namespace nimrtc::sdp

namespace nimrtc::rtp    { struct PacketView; }
namespace nimrtc::jb     { class JitterBuffer; }
namespace nimrtc::audio3a { class IAudio3A; }
namespace nimrtc::dtls   {
enum class DtlsState : std::uint8_t;
class DtlsSession;
class DtlsSessionWolfSSL;
}
namespace nimrtc::srtp   { class SrtpContext; }
#ifdef NIMRTC_HAS_OPUS
namespace nimrtc::opus   { class Encoder; class Decoder; }
#endif
namespace nimrtc::plugins {
class IVideoCodec;
}

namespace nimrtc::engine {

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

/** Parameters for the audio codec advertised in the SDP and used for
 *  encoding / decoding.  Defaults match RFC 7587 Opus.
 *  When NIMRTC_HAS_OPUS is not defined, the engine ignores codec_name and
 *  falls back to the first statically-linked codec plugin (e.g. PCMU). */
struct AudioCodecConfig {
    /** RTP payload type number.  RFC 7587 uses 111 for Opus.
     *  WebRTC mandatory: 0 (PCMU) and 8 (PCMA). */
    std::uint8_t  payload_type  = 111;

    /** Encoding name per RFC 4855 / IANA registry, e.g. "opus", "PCMU", "PCMA". */
    std::string   encoding      = "opus";

    /** Audio sampling rate in Hz. Opus = 48000; PCMU/PCMA = 8000. */
    std::uint32_t clock_rate    = 48000;

    /** Number of audio channels. Opus stereo = 2; narrowband codecs = 1. */
    std::uint8_t  channels      = 2;

    /** fmtp (format parameters) string, e.g. "minptime=10;useinbandfec=1".
     *  Empty string means no a=fmtp line is emitted. */
    std::string   fmtp;
};

struct EngineConfig {
    // ---- Plugin selection (ADR-001: look up by string ID in PluginRegistry) ----

    /** ITransport plugin name. Default = "ice". */
    std::string_view transport_name = "ice";

    /** IRTP plugin name. Default = "webrtc". */
    std::string_view rtp_name = "webrtc";

    /** ISDP plugin name. Default = "webrtc". */
    std::string_view sdp_name = "webrtc";

    /** IJB plugin name. Default = "adaptive". */
    std::string_view jb_name = "adaptive";

    /** IAudio3A plugin name. Default = "webrtc". */
    std::string_view audio3a_name = "webrtc";

    /** ICodec plugin name. Default = "opus". */
    std::string_view codec_name = "opus";

    /** Audio codec parameters for the SDP offer and send_audio().
     *  Defaults to RFC 7587 Opus (payload type 111, 48 kHz, stereo).
     *  When opus is not available (NIMRTC_HAS_OPUS undefined), set to
     *  { .payload_type=0, .encoding="PCMU", .clock_rate=8000, .channels=1 }
     *  for WebRTC-mandatory narrowband fallback. */
    AudioCodecConfig audio_codec;

    // ---- Transport config (forwarded to ITransport::open) ----

    /** ICE binding address. Empty = "0.0.0.0" (any IPv4). */
    std::string local_bind_address;

    /** STUN server, e.g. "stun.l.google.com" — empty = skip STUN gathering. */
    std::string stun_server_host;
    std::uint16_t stun_server_port = 3478;

    /** Local UDP port range for ICE. 0 = OS picks. MUST be unique across
     *  multiple NimRTCEngine instances on the same host or both will bind
     *  to the same port and process each other's STUN packets. */
    std::uint16_t local_port_range_begin = 0;
    std::uint16_t local_port_range_end   = 0;

    // ---- Audio config ----

    /** PCM sample rate / channels. */
    std::uint32_t pcm_sample_rate_hz = 48000;
    std::uint8_t  pcm_channels = 1;

    // ---- JitterBuffer config ----

    /** JitterBuffer fixed delay (ms). */
    int jb_initial_delay_ms = 40;
    int jb_min_delay_ms     = 10;
    int jb_max_delay_ms     = 200;

    // ---- Video plugin selection (R3-Batch: lookup by string ID) ----
    //
    // Video integration is in P1.1/R3 scope; the engine currently does not
    // pipe video RTP/RTCP, so these plugins are resolved but not yet
    // threaded into the ICE/DTLS/SRTP pipeline. Callers can drive them
    // manually via the `video_source() / video_sink() / video_receiver()
    // / video_sender()` accessors.
    //
    // Defaults match the IDs registered by `video_source::register_default_plugins()`,
    // `video_sink::register_default_plugins()`, and
    // `video_pipeline::register_default_plugins()`. Override here to
    // select alternate implementations (HW plugins register under
    // different ids; see `plugins::hw::*`).

    /** IVideoSource plugin id (e.g. "memory", or HW plugin id). */
    std::string_view video_source_name = "memory";

    /** IVideoSink plugin id (e.g. "headless", "pinned", or HW renderer id). */
    std::string_view video_sink_name   = "headless";

    /** IVideoReceiver plugin id (e.g. "reference"). */
    std::string_view video_receiver_name = "reference";

    /** IVideoSender plugin id (e.g. "reference"). */
    std::string_view video_sender_name   = "reference";

    /** Defaults forwarded to plugins::VideoReceiverConfig when the engine
     *  instantiates the video receiver plugin. */
    struct VideoReceiverTuning {
        std::uint32_t ssrc                = 0xDEADBEEF;
        std::uint8_t  payload_type        = 102;
        std::uint32_t max_inflight_frames = 8;
        std::uint32_t max_jitter_buffer_ms = 200;
        bool          emit_nacks          = true;
        bool          expect_fu_a         = true;
    } video_receiver_tuning;

    /** Defaults forwarded to plugins::VideoSenderConfig when the engine
     *  instantiates the video sender plugin. */
    struct VideoSenderTuning {
        std::uint32_t ssrc         = 0xCAFEBABE;
        std::uint8_t  payload_type = 102;
        std::uint16_t mtu          = 1200;
        std::uint16_t initial_seq  = 0;
    } video_sender_tuning;

    // ---- BWE plugin selection (ADR-001: resolved via PluginRegistry) --------
    //
    // BWE controls outbound pacing and adapts to network congestion.
    // Default = "aimd" (AIMD BWE, P1 default).  Empty string = no BWE
    // (passthrough, used by the "transport" profile).
    //
    // The engine resolves the factory at open() via
    // `core::PluginRegistry::instance().get_bwe(bwe_name)`.

    /** IBwe plugin id. Default = "aimd". */
    std::string bwe_name = "aimd";

    /** Initial BWE parameters handed to `IBweFactory::create()`. */
    plugins::BweConfig bwe_config;

    // ---- Scheduler plugin selection (ADR-001: resolved via PluginRegistry) ----
    //
    // The scheduler prioritises outbound packets (audio / video / control).
    // Default = "strict_priority" (5-queue strict priority, P1 default).
    // Empty `scheduler_name` (and scheduler_config.enabled == false) = no
    // scheduler (direct ICE send, used by the "transport" profile).
    //
    // The engine resolves the factory at open() via
    // `core::PluginRegistry::instance().get_scheduler(scheduler_name)`.
    // When `scheduler_config.enabled == false`, no scheduler is created and
    // `send_audio()` sends directly through ICE.

    /** IScheduler plugin id. Default = "strict_priority". */
    std::string scheduler_name = "strict_priority";

    /** Scheduler parameters handed to `ISchedulerFactory::create()`. */
    plugins::SchedulerConfig scheduler_config;

    // ---- Video codec selection (ADR-001: resolved via PluginRegistry) ----
    //
    // The engine resolves the video codec factory at open() (via init_video_plugins()).
    // This field is used when the video pipeline is active; it does NOT gate
    // whether the video pipeline is created — that is controlled by whether
    // `video_sender_name` / `video_receiver_name` resolve to non-null factories.
    //
    // Default = "h264" (H.264 stub decoder/encoder).  Empty string = no video codec.

    /** IVideoCodec plugin id. Default = "h264". */
    std::string video_codec_name = "h264";
};

// ---------------------------------------------------------------------------
// Public callbacks
// ---------------------------------------------------------------------------

using AudioFrameCallback = std::function<void(const float* pcm, size_t num_samples)>;
using VideoFrameCallback = std::function<void(const uint8_t* nal, size_t len, bool keyframe)>;
using EngineErrorCallback = std::function<void(uint32_t err, std::string_view msg)>;
using EngineStateCallback = std::function<void(const char* state)>;

// ---------------------------------------------------------------------------
// NimRTCEngine
// ---------------------------------------------------------------------------

class NimRTCEngine {
public:
    explicit NimRTCEngine(EngineConfig config);
    ~NimRTCEngine();

    NimRTCEngine(const NimRTCEngine&)            = delete;
    NimRTCEngine& operator=(const NimRTCEngine&) = delete;

    // Lifecycle
    uint32_t open() noexcept;
    void     close() noexcept;
    bool     is_open() const noexcept { return state_ == State::kOpen; }

    /** Optional early setup: creates the ICE transport (and all other modules)
     *  WITHOUT starting ICE candidate gathering.  Callers can then call
     *  set_remote_ice() to inject the peer's ICE credentials before open().
     *  This allows the loopback test to ensure the answerer knows the offerer's
     *  ICE ufrag/pwd BEFORE gather_candidates() starts, so libjuice sets the
     *  answerer to CONTROLLED instead of CONTROLLING (avoids ICE role conflict).
     *
     *  After pre_open(), call open() normally — gather_candidates() will fire.
     *  Can be called at most once. Returns same error codes as open(). */
    uint32_t pre_open() noexcept;

    // Callbacks
    void set_on_audio_frame(AudioFrameCallback cb) noexcept { on_audio_frame_ = std::move(cb); }
    void set_on_video_frame(VideoFrameCallback cb) noexcept { on_video_frame_ = std::move(cb); }
    void set_on_error(EngineErrorCallback cb) noexcept     { on_error_       = std::move(cb); }
    void set_on_state_change(EngineStateCallback cb) noexcept { on_state_change_ = std::move(cb); }

    // SDP — RFC 8829 compliant
    std::string create_offer() noexcept;
    std::optional<std::string> process_remote_sdp(std::string_view remote_sdp) noexcept;

    // ---- Audio I/O --------------------------------------------------------
    // Opus encode + RTP packetise + SRTP encrypt + transport.send.
    uint32_t send_audio(const float* pcm_samples, size_t num_samples) noexcept;

    // ---- Video I/O --------------------------------------------------------
    //
    // Outbound: drive the full video pipeline:
    //   video_source_ (frame capture) → video_codec_ (encode H.264)
    //   → video_sender_ (FU-A packetize) → RTP header → SRTP
    //   → scheduler_ (priority queue, tick drains to ICE).
    // The engine tick() must be called frequently (≥30 Hz) to drain the
    // scheduler and send packets.  start_video() / stop_video() control the
    // source cadence.
    //
    // Inbound: ICE recv → on_transport_recv → handle_rtp (PT demux)
    //   → video_receiver_ (FU-A depacketize) → video_codec_ (decode)
    //   → video_sink_ (render).  The on_video_frame_ callback fires on
    //   the raw NAL level (before decode) for users who want to do their
    //   own rendering.
    //
    // Both paths require `open()` to have been called first.

    /** Start the video capture source (produces frames at the configured fps).
     *  Idempotent if already running.  Returns kOk on success. */
    plugins::Status start_video() noexcept;

    /** Stop the video capture source. Idempotent if already stopped. */
    void stop_video() noexcept;

    /** Manually push one raw frame through the video pipeline.
     *  Bypasses the video_source capture cadence; useful for feeding
     *  camera frames directly (caller owns the cadence loop).
     *  The frame is encoded, packetized, and enqueued to the scheduler
     *  exactly like the auto-capture path.
     *  @param frame  Raw frame (must remain valid for the call). */
    plugins::Status send_video(const plugins::VideoSourceFrame& frame) noexcept;

    /** Drive DTLS state machine: feed pending outbound DTLS records back
     *  through ICE.  Returns number of records sent. */
    int drain_dtls() noexcept;

    /** Once DTLS connected, pull SRTP keying material and install on the
     *  SrtpContext.  Idempotent. */
    void maybe_install_srtp_keys() noexcept;

    // --- Inspection helpers (for tests / debugging) ---

    /** True iff SRTP keys have been installed (post-DTLS-Connected).
     *  Out-of-line to keep the public header free of <nimrtc/srtp/srtp.hpp>. */
    bool srtp_installed() const noexcept;

    /** Current DTLS state, or Closed if DTLS not initialised.
     *  Out-of-line so the public header doesn't need <nimrtc/dtls/dtls.hpp>. */
    dtls::DtlsState dtls_state() const noexcept;

    /** True iff DTLS has reached Connected state. */
    bool dtls_connected() const noexcept;

    /** True iff the ICE agent has selected at least one candidate pair
     *  (Connected or Completed).  Used to gate outbound DTLS sends.
     *  Implementation now goes through the plugin seam
     *  (`plugins::IICETransport::state()`), not a dynamic_cast to the
     *  concrete module class. */
    bool is_ice_connected() const noexcept;

    /** Last error code from open()/pre_open() — useful for diagnostics
     *  when the bool result is non-OK but we want to know which subsystem
     *  failed (e.g. 0x1FFF = ICE, 0x2000 = DTLS, 0x1A00 = audio3a).
     *  Out-of-line because last_open_rc_ lives in Impl. */
    uint32_t last_open_rc() const noexcept;

    /** Set the remote ICE description (ice-ufrag / ice-pwd / candidates) BEFORE
     *  open() is called.  This is required for the loopback test: the answerer
     *  (controlled) must know the offerer's ICE credentials before its ICE
     *  transport opens, so libjuice sets the agent to CONTROLLED instead of
     *  CONTROLLING (avoiding the ICE role conflict).  The ice_block should be
     *  the raw SDP media section containing a=ice-ufrag, a=ice-pwd, and
     *  optionally a=candidate lines.  Can also be called after open() (applied
     *  immediately). */
    bool set_remote_ice(std::string_view ice_block) noexcept;

    /** Add a TURN server for relay candidate gathering.
     *
     *  Must be called BEFORE open() — after pre_open() creates the ICE transport
     *  but before open() triggers candidate gathering.  The engine forwards the
     *  call to `ice_t_->add_turn_server()`.
     *
     *  Thread-safety: not thread-safe; call from the same thread that will
     *  call open().
     *
     *  @return  0 on success; non-zero if the ICE transport is not yet created
     *            (call pre_open() first).
     */
    int add_turn_server(std::string_view host, std::uint16_t port,
                        std::string_view username,
                        std::string_view password) noexcept;

    /** Inbound raw bytes — alternative entry point for test injection. */
    uint32_t feed_srtp_inbound(const std::uint8_t* srtp_packet, std::size_t len) noexcept;

    /** Drain packets from the ICE socket and dispatch them.  Returns the
     *  number of packets dispatched (RTP / DTLS / STUN). */
    int tick() noexcept;

    // ICE state inspection
    const char* ice_state_string() const noexcept;

    // Local ICE credentials — to embed in SDP
    std::string local_ufrag()    const noexcept;
    std::string local_password() const noexcept;

    /** Local DTLS fingerprint advertised in our SDP, as a base64-encoded
     *  SHA-256 SPKI hash (the format Chrome's
     *  --ignore-certificate-errors-spki-list expects).  Returns an empty
     *  string if the DTLS session is not yet open or self-signed certs
     *  are disabled.  Computed lazily from the underlying DtlsSession. */
    std::string local_dtls_fingerprint_sha256_base64() const noexcept;

    /** Underlying ITransport (ICE by default).  Useful for tests and demos that
     *  need direct access to the transport — e.g. for ICE candidate forwarding
     *  in the WebSocket signaling proxy.  Returns nullptr if not yet open().
     *
     *  The pointer is non-owning: the engine retains the only owner
     *  (`ice_t_`). Callers must not delete it. */
    plugins::ITransport* get_transport() noexcept { return ice_t_.get(); }
    const plugins::ITransport* get_transport() const noexcept { return ice_t_.get(); }

    /** ICE-aware view of the transport.  Returns nullptr if no
     *  IICETransport plugin has been resolved (which only happens if the
     *  transport plugin chosen by `cfg.transport_name` does not implement
     *  the ICE-aware interface — the engine only treats ICE transports as
     *  ICE transports).
     *
     *  Non-owning; same lifetime as the engine. */
    plugins::IICETransport* get_ice_transport() noexcept { return ice_t_.get(); }
    const plugins::IICETransport* get_ice_transport() const noexcept {
        return ice_t_.get();
    }

    /** Read-only view of the engine's config.  Useful for tools that need
     *  to know the PCM rate / channels (e.g. demo-p2p's audio generator)
     *  without making those fields public. */
    const EngineConfig& config() const noexcept { return config_; }

    // ---- Video plugin accessors (R3-Batch) ----
    //
    // The engine resolves all 4 video plugins from core::PluginRegistry
    // at open() time, using the *_name fields in EngineConfig. The plugins
    // themselves live for the engine's lifetime (or until replaced via
    // set_video_sink / set_video_source).
    //
    // The engine does NOT thread these into the RTP/DTLS/SRTP pipeline
    // yet (P1.1 future work). Callers can:
    //   - drive video_source_->start() / produce_one() to feed a sender;
    //   - push encoded frames via video_sender_->push_frame();
    //   - receive encoded frames via video_receiver_->push_rtp() and
    //     listen on set_frame_callback();
    //   - render decoded frames via video_sink_->render().
    //
    // Returns nullptr if the configured plugin id was not found at open().

    plugins::IVideoSource*   video_source()   noexcept { return video_source_.get(); }
    plugins::IVideoSink*     video_sink()     noexcept { return video_sink_.get(); }
    plugins::IVideoReceiver* video_receiver() noexcept { return video_receiver_.get(); }
    plugins::IVideoSender*   video_sender()   noexcept { return video_sender_.get(); }
    const plugins::IVideoSource*   video_source()   const noexcept { return video_source_.get(); }
    const plugins::IVideoSink*     video_sink()     const noexcept { return video_sink_.get(); }
    const plugins::IVideoReceiver* video_receiver() const noexcept { return video_receiver_.get(); }
    const plugins::IVideoSender*   video_sender()   const noexcept { return video_sender_.get(); }

    /** Replace the video sink at runtime (e.g. swap "headless" for an SDL
     *  renderer). Closes the previous sink and transfers ownership of @p sink.
     *  Safe to call before or after open(); if called before open(), the
     *  sink configured via video_sink_name is replaced and the new one is
     *  used instead. Pass nullptr to detach (next render() calls are no-ops). */
    void set_video_sink(std::unique_ptr<plugins::IVideoSink> sink) noexcept;

    /** Replace the video source at runtime. Same semantics as set_video_sink. */
    void set_video_source(std::unique_ptr<plugins::IVideoSource> source) noexcept;

    // ---- BWE / Scheduler accessors (ADR-001: resolved via PluginRegistry) ----
    //
    // These plugins are created at open() time via
    // `core::PluginRegistry::instance().get_bwe()` and `get_scheduler()`.
    // The engine injects them into `ice_t_` via `set_bwe()` / `set_scheduler()`.
    // Callers can use the accessors to drive the plugins manually
    // (e.g. inject custom BWE feedback, inspect scheduler stats).
    //
    // Returns nullptr if the configured plugin id was not found at open().

    plugins::IBwe*        bwe()        noexcept { return bwe_.get(); }
    const plugins::IBwe*  bwe()  const noexcept { return bwe_.get(); }

    plugins::IScheduler*  scheduler()  noexcept { return scheduler_.get(); }
    const plugins::IScheduler* scheduler() const noexcept { return scheduler_.get(); }

    plugins::IVideoCodec* video_codec() noexcept { return video_codec_.get(); }
    const plugins::IVideoCodec* video_codec() const noexcept { return video_codec_.get(); }

    // Engine stats for debugging
    struct Stats {
        int srtp_drops = 0;
    };
    /** Out-of-line: depends on concrete SRTP/DTLS state stored in Impl. */
    Stats stats() const noexcept;

private:
    // ---- Engine state ----
    // Plugin instances live as std::unique_ptr<plugins::I*Interface> in the
    // header because their headers (under nimrtc/plugins/) ARE plugin-seam
    // headers — no concrete module dependency is leaked here.
    //
    // All concrete-module state (JitterBuffer, Audio3A fallback, DTLS, SRTP,
    // Opus, parsed SDP) lives inside the opaque `Impl` defined in engine.cpp.
    // That keeps `nimrtc/engine/engine.hpp` free of concrete module headers
    // (Layout Invariant 4).

    struct Impl;
    Impl* impl_ = nullptr;   // pimpl — see src/engine.cpp

    EngineConfig                                config_;

    // ---- Plugin instances (created via PluginRegistry in open()) ----
    //
    // `ice_t_` is the owning handle to the ICE-aware transport plugin
    // (concretely `nimrtc::ice::IceTransport` for the default "ice"
    // plugin, but resolvable through the plugin seam — see
    // `plugins::IICETransport`). Because IICETransport IS-A ITransport,
    // callers can downcast to ITransport* for generic send/recv; we keep
    // the IICETransport* alias around because every ICE-specific call
    // (state, credentials, gathering, remote SDP, pre-open config) goes
    // through it.  Storing the IICETransport pointer eliminates all
    // **`dynamic_cast<ice::IceTransport*>`** calls from this class — the
    // plugin-seam objective achieved by this refactor.
    std::unique_ptr<plugins::IICETransport>      ice_t_;
    std::unique_ptr<plugins::IAudio3A>          audio3a_plugin_;
    std::unique_ptr<plugins::ICodec>            codec_plugin_;

    // ---- Video plugins (resolved from core::PluginRegistry) ----
    std::unique_ptr<plugins::IVideoSource>      video_source_;
    std::unique_ptr<plugins::IVideoSink>        video_sink_;
    std::unique_ptr<plugins::IVideoReceiver>    video_receiver_;
    std::unique_ptr<plugins::IVideoSender>      video_sender_;

    // ---- BWE / Scheduler plugins (ADR-001: resolved via PluginRegistry) ----
    // Created in init_bwe_scheduler() (called from pre_open/open).
    // Injected into ice_t_ via set_bwe() / set_scheduler().
    std::unique_ptr<plugins::IBwe>        bwe_;
    std::unique_ptr<plugins::IScheduler>  scheduler_;

    // ---- Video codec plugin (ADR-001: resolved via PluginRegistry) ----
    std::unique_ptr<plugins::IVideoCodec> video_codec_;

    // ---- Public callbacks (no concrete deps) ----
    AudioFrameCallback  on_audio_frame_;
    VideoFrameCallback  on_video_frame_;
    EngineErrorCallback on_error_;
    EngineStateCallback on_state_change_;

    enum class State { kConstructed, kOpen, kClosed };
    State state_ = State::kConstructed;

    // Out-of-line helpers whose definitions live in engine.cpp and pull in
    // the concrete module headers.  Public API keeps the header free of
    // those headers (Layout Invariant 4).
    void on_transport_recv(const plugins::BufferView& pkt) noexcept;
    void handle_rtp(const rtp::PacketView& pv) noexcept;
    void init_bwe_scheduler() noexcept;
    void shutdown_bwe_scheduler() noexcept;
    void init_video_plugins() noexcept;
    void shutdown_video_plugins() noexcept;

    static void default_on_error(uint32_t err, std::string_view msg) noexcept;

    /** One-time module initialisation shared by pre_open() and the
     *  "modules not yet created" branch of open().
     *
     *  Builds the ICE transport + BWE + Scheduler + Audio3A + Codec +
     *  SRTP + DTLS, applies pre-open config through the plugin seam, and
     *  wires ICE recv callbacks.  Does NOT call ice_t_->open() — callers
     *  decide when to start ICE gathering (pre_open leaves it for a later
     *  open(); open() calls it immediately after).
     *
     *  @return 0 on success, or a non-zero plugins::Status-derived code on
     *          failure (0x1FFF = transport, 0x2000 = DTLS, 0x1A00 = audio3a,
     *          etc.).  On failure, partial state is rolled back enough to
     *          leave the engine re-callable. */
    uint32_t init_modules_once() noexcept;
};

} // namespace nimrtc::engine

#endif // NIMRTC_ENGINE_ENGINE_HPP
