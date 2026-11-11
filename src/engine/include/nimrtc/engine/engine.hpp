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
 *   cfg.audio3a_name    = "webrtc_apm";// nimrtc::audio3a::register_default_plugins()
 *   cfg.codec_name      = "opus";      // nimrtc::opus::register_default_plugins() (if enabled)
 *   cfg.bwe_name        = "aimd";      // nimrtc::bwe::register_default_plugins()
 *   cfg.scheduler_name   = "strict_priority"; // nimrtc::sched::register_default_plugins()
 *   cfg.video_codec_name = "h264";     // nimrtc::h264::register_default_plugins()
 *
 * Open() calls `core::register_all_default_plugins()` first, so callers
 * normally do not need to do anything to populate the registry.
 *
 * ## Per-symbol stability annotations (v0.14.x PER-SYMBOL-CLASSIFY)
 *
 * The v0.13.0 sweep marked this header at the file level
 * (`NIMRTC_API_STABLE` once on `EngineConfig` / `AudioCodecConfig` /
 * `NimRTCEngine`). The v0.14.x follow-up annotates the
 * most-instantiated public methods of `NimRTCEngine` individually
 * so callers can reason about per-symbol stability:
 *
 *   - Lifecycle (`open()` / `pre_open()` / `close()` / `is_open()`) —
 *     STABLE. The four fundamental engine lifecycle entry points; every
 *     caller invokes at least one of them.
 *   - Core payload (`send_audio()` / `create_offer()` /
 *     `process_remote_sdp()` / `create_data_channel()`) — STABLE.
 *     The core SDP / data-channel / audio emission contract.
 *   - Video (`start_video()` / `stop_video()` / `send_video()`) —
 *     UNSTABLE. The video API is still P1.1 / R3 scope; the surface
 *     may shift in a v0.15.x follow-up before v1.0.0 final.
 *   - Accessors (`set_on_*()` / ICE diagnostics / state inspect) —
 *     STABLE. Simple std::function setters and diagnostic readbacks.
 *
 * The macro expands to nothing in default builds; it becomes load-bearing
 * when the host app opts into `NIMRTC_ENABLE_API_STABILITY_ANNOTATIONS=1`,
 * at which point `NIMRTC_API_UNSTABLE` emits the cross-compiler
 * `[[deprecated]]` attribute. Per ADR-015 §Decision 3: v0.x → v1.0.0
 * may mark symbols DEPRECATED but does not REMOVE any.
 *
 * @note P1 — DTLS-SRTP keying material is now derived after the DTLS
 *       handshake completes; SRTP protects RTP in both directions.
 */

#ifndef NIMRTC_ENGINE_ENGINE_HPP
#define NIMRTC_ENGINE_ENGINE_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <nimrtc/core/api_version.hpp>
#include <nimrtc/core/bytes.hpp>          // core::ByteSpan (the DTMF receive seam)
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
#include <nimrtc/plugins/audio_decoder.hpp>
#include <nimrtc/plugins/audio_receiver.hpp>
#include <nimrtc/plugins/codec.hpp>
#include <nimrtc/plugins/dtmf.hpp>          // v1.2.0 (session plan D1/D2) — RFC 4733 seam
#include <nimrtc/plugins/net_feedback.hpp>   // v1.2.0 M1 (F2) — feedback collector seam
#include <nimrtc/plugins/session.hpp>         // v1.2.0 (session plan N1/T1) — session seam
#include <nimrtc/plugins/video_source.hpp>
#include <nimrtc/plugins/video_sink.hpp>
#include <nimrtc/plugins/video_pipeline.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/plugins/datachannel.hpp>
#include <nimrtc/plugins/xr_report.hpp>        // v1.2.0 (session plan X1) — RTCP XR seam

// Forward declarations of concrete module types — full definitions live
// in module headers (e.g. <nimrtc/dtls/dtls.hpp>) and are pulled in by
// src/engine/src/engine.cpp.  This keeps engine.hpp free of concrete
// module dependency surface (Layout Invariant 4).
namespace nimrtc::sdp {
struct NIMRTC_API_STABLE SessionDescription;
} // namespace nimrtc::sdp

namespace nimrtc::rtp    { struct NIMRTC_API_STABLE PacketView; }
namespace nimrtc::jb     { class NIMRTC_API_STABLE JitterBuffer; }
namespace nimrtc::audio3a { class NIMRTC_API_STABLE IAudio3A; }
namespace nimrtc::dtls   {
enum class NIMRTC_API_STABLE DtlsState : std::uint8_t;
class NIMRTC_API_STABLE DtlsSession;
class NIMRTC_API_STABLE DtlsSessionWolfSSL;
}
namespace nimrtc::srtp   { class NIMRTC_API_STABLE SrtpContext; }
#ifdef NIMRTC_HAS_OPUS
namespace nimrtc::opus   { class NIMRTC_API_STABLE Encoder; class Decoder; }
#endif
namespace nimrtc::plugins {
class NIMRTC_API_STABLE IVideoCodec;
}

namespace nimrtc::engine {

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

/** Parameters for the audio codec advertised in the SDP and used for
 *  encoding / decoding.  Defaults match RFC 7587 Opus.
 *  When NIMRTC_HAS_OPUS is not defined, the engine ignores codec_name and
 *  falls back to the first statically-linked codec plugin (e.g. PCMU). */
struct NIMRTC_API_STABLE AudioCodecConfig {
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

struct NIMRTC_API_STABLE EngineConfig {
    // ---- Plugin selection (ADR-001: look up by string ID in PluginRegistry) ----

    /** ITransport plugin name. Default = "ice". */
    std::string_view transport_name = "ice";

    /** IRTP plugin name. Default = "webrtc". */
    std::string_view rtp_name = "webrtc";

    /** ISDP plugin name. Default = "webrtc". */
    std::string_view sdp_name = "webrtc";

    /** IJB plugin name. Default = "adaptive". */
    std::string_view jb_name = "adaptive";

    /** IAudio3A plugin name.
     *  Default = "webrtc_apm" (real WebRTC Audio Processing Module —
     *  AEC/ANS/AGC2/VAD/HPF via webrtc_audio_processing).
     *  When NIMRTC_VENDORED_WEBRTC_APM is OFF or the prebuilt `.a` is
     *  missing, WebRtcPluginFactory transparently falls back to
     *  NullAudio3A so the engine still works.  Set to "" to disable 3A
     *  entirely (raw passthrough). */
    std::string_view audio3a_name = "webrtc_apm";

    /** ICodec plugin name. Default = "opus". */
    std::string_view codec_name = "opus";

    /** Audio codec parameters for the SDP offer and send_audio().
     *  Defaults to RFC 7587 Opus (payload type 111, 48 kHz, stereo).
     *  When opus is not available (NIMRTC_HAS_OPUS undefined), set to
     *  { .payload_type=0, .encoding="PCMU", .clock_rate=8000, .channels=1 }
     *  for WebRTC-mandatory narrowband fallback. */
    AudioCodecConfig audio_codec;

    // ---- DTLS backend selector (Transport PAL Slice 4 / TPAL-4, v0.11.0) ----
    //
    // Resolved at engine.open() time through the typed
    // `core::PluginRegistry::get_dtls_session(id)` slot populated by
    // `nimrtc::dtls::register_default_plugins()` (and any user-registered
    // `IDtlsSessionFactory`). When empty, the engine falls back to the
    // built-in wolfSSL factory at id="wolfssl", so every v0.10.x caller
    // that left this field default-valued stays source-compatible.
    //
    // To plug in a 国密 / OpenSSL / BoringSSL / mbedTLS / custom backend:
    //   1. Provide an IDtlsSessionFactory (matching the contract in
    //      <nimrtc/dtls/dtls_session_factory.hpp>) that hands out
    //      unique_ptr<IDtlsSession> instances.
    //   2. Register it once at program startup:
    //        core::PluginRegistry::instance().register_dtls_session(
    //            "guomi_sm4", &my_factory);
    //   3. Select it per-engine via the field below — empty means
    //      the default ("wolfssl") wins:
    //        EngineConfig cfg;
    //        cfg.dtls_name = "guomi_sm4";   // selects the 国密 backend
    //
    // Unknown ids surface as `kEngineInternal` via `on_error_` and
    // `open()` returns non-zero — the engine does NOT silently fall
    // back to "wolfssl" (that would mask integrator typos).
    std::string dtls_name;

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

    /** Duration of one outbound audio RTP packet, in milliseconds.
     *
     *  One packet carries exactly one Opus frame of this duration
     *  (RFC 7587 code 0 framing), so the value must be one of the Opus
     *  frame sizes (RFC 6716 §4): 10, 20, 40 or 60 ms.  Default 20 ms
     *  — the value every browser uses.
     *
     *  The RTP timestamp advances by this duration per packet, whether or
     *  not the packet was actually emitted, so a dropped frame leaves a
     *  gap in the sequence number but never a gap in the media clock.
     */
    std::uint16_t audio_ptime_ms = 20;

    /** Explicit SSRC for the outbound audio stream.
     *
     *  0 (the default) means "choose one": the engine then draws a
     *  per-engine SSRC from a per-process seed, so two engines in one
     *  process never share an SSRC.  Set an explicit value to make the
     *  stream reproducible (tests, or a host that manages SSRCs itself).
     *
     *  The value the engine uses is the one it advertises in the SDP
     *  (`a=ssrc:`) and the one it stamps into every outbound RTP header
     *  — the two cannot drift apart.
     */
    std::uint32_t audio_ssrc = 0;

    // ---- JitterBuffer config ----

    /** JitterBuffer fixed delay (ms). */
    int jb_initial_delay_ms = 40;
    int jb_min_delay_ms     = 10;
    int jb_max_delay_ms     = 200;

    // ---- Video plugin selection (R3-Batch: lookup by string ID) ----
    //
    // The engine DOES thread the video send and receive paths into the
    // RTP/DTLS/SRTP pipeline:
    //   send:    send_video() -> video_codec_ (encode) -> video_sender_
    //             (FU-A packetize) -> RTP header -> SRTP protect ->
    //             scheduler_ -> ICE.
    //   receive: ICE recv -> handle_rtp (PT demux) -> video_receiver_->push_rtp()
    //             (FU-A depacketize + jitter buffer) -> video_codec_ (decode)
    //             -> video_sink_ (render).
    //
    // What is deliberately NOT auto-wired is the video SOURCE: the source
    // runs its own cadence thread and IVideoCodec / IVideoSender are not
    // thread-safe, so the engine starts/stops the source (start_video() /
    // stop_video()) but the host is responsible for pulling frames and
    // pushing them through send_video().  See the "Video plugin accessors"
    // section further down for the full contract.
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
    struct NIMRTC_API_STABLE VideoReceiverTuning {
        std::uint32_t ssrc                = 0xDEADBEEF;
        std::uint8_t  payload_type        = 102;
        std::uint32_t max_inflight_frames = 8;
        std::uint32_t max_jitter_buffer_ms = 200;
        std::uint32_t initial_jitter_buffer_ms = 40;
        bool          emit_nacks          = true;
        bool          expect_fu_a         = true;
        /** Enable the adaptive playout delay of the video jitter buffer
         *  (the target delay then follows the interarrival jitter
         *  estimate instead of being fixed). */
        bool          adaptive_playout    = false;
    } video_receiver_tuning;

    /** Defaults forwarded to plugins::VideoSenderConfig when the engine
     *  instantiates the video sender plugin. */
    struct NIMRTC_API_STABLE VideoSenderTuning {
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

    // ---- Net feedback plugin selection (v1.2.0 M1, F2) --------------------
    //
    // The receiver-side network-feedback collector sits *between* the
    // wire and the bandwidth estimator: the engine demultiplexes the
    // inbound RTCP compound datagram and routes the receiver-side
    // network signals (Receiver Report, REMB, transport-cc) to the
    // collector, the collector aggregates them into one snapshot, and
    // the engine hands that snapshot to `IBwe::on_feedback()`.
    //
    // Without this plugin the estimator is never fed, so the bandwidth
    // target stays pinned at `bwe_config.initial_bitrate_bps` forever
    // and every rate-control feature built on top of it is decorative.
    //
    // The engine resolves the factory at open() via
    // `core::PluginRegistry::instance().get_net_feedback(name)`.
    // Default = "nimrtc_net_feedback" (the built-in collector).
    // Empty string = no collector (the pre-v1.2.0 behaviour: the
    // estimator is never fed).
    std::string net_feedback_name = "nimrtc_net_feedback";

    // ---- Signal-selection policy (v1.2.0 M2, F5 / ADR-018 Decision 2) ---
    //
    // Which receiver-side signal drives the bandwidth estimate:
    //
    //   "auto"                (default) transport-cc when the peer
    //                         negotiated it and its feedback is fresh,
    //                         loss/RTT otherwise;
    //   "loss_rtt"            always the loss/RTT path (REMB stays an
    //                         upper-bound hint);
    //   "require_transport_cc" transport-cc must drive; the engine
    //                         reports the path as unavailable instead
    //                         of falling back silently.
    //
    // An unknown value is treated as "auto" — the policy must never be
    // the reason a session fails.
    std::string net_feedback_signal = "auto";

    // How long transport-cc feedback may stay silent before the policy
    // treats it as stale and falls back to the loss/RTT path, in
    // milliseconds.  Must be comfortably larger than the peer's
    // feedback interval (Chrome: 50-100 ms).
    std::uint32_t net_feedback_transport_cc_timeout_ms = 3000;

    // ---- RTCP Extended Report consumer (v1.2.0, session plan X1) -------
    //
    // The engine demultiplexes every inbound RTCP compound datagram
    // and routes the XR sub-packets (payload type 207) to this
    // consumer, which decodes the RFC 3611 report blocks (the Receiver
    // Reference Time, the DLRR and the Statistics Summary reports) so
    // the host can read the peer's QoE / network statistics.
    //
    // The engine resolves the factory at open() via
    // `core::PluginRegistry::instance().get_xr_report(name)`.
    // Default = "nimrtc_xr" (the built-in consumer).
    // Empty string = no consumer (the pre-v1.2.0 behaviour: the XR
    // sub-packet is recognised and dropped).
    std::string xr_report_name = "nimrtc_xr";

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

    // ---- DataChannel selection (ADR-001: resolved via PluginRegistry) ----
    //
    // The engine resolves the data-channel factory at open() (via init_modules_once()).
    // Channels are NOT auto-created — the caller invokes create_data_channel(label)
    // to allocate one after open() succeeds.  This matches the WebRTC JS API
    // (peer.createDataChannel()) and lets callers attach distinct on_message /
    // on_state callbacks per channel.
    //
    // Default = "web_transport" (since v0.15.0; WebTransport/QUIC path).
    // `"sctp"` was retired in v1.2.0: it was a loopback *test* backend
    // (in-process usrsctp socket on 127.0.0.1, no SCTP-over-DTLS wiring),
    // so it could never reach a remote peer — passing it now fails the
    // engine's `open()` with `kEngineInternal`.
    // Empty disables DataChannel entirely:
    //   - create_offer() does NOT emit m=application
    //   - process_remote_sdp() silently drops any m=application from the peer
    //   - create_data_channel() returns nullptr
    //
    // See `src/plugins/include/nimrtc/plugins/datachannel.hpp` for the
    // IDataChannel / IDataChannelFactory contract.

    /** IDataChannel plugin id. Default = "" (unset).
     *
     *  This is the **low-level** factory-slot selector: the engine resolves
     *  `core::PluginRegistry::get_datachannel(datachannel_name)` at
     *  `init_modules_once()` time and uses the result for
     *  `create_data_channel()`.
     *
     *  v1.2.0: the default changed from `"sctp"` to `""` because the SCTP
     *  backend was retired from the public selectors (see
     *  `datachannel_backend` below and
     *  `docs/plan/datachannel-capability-plan.md` WP-A / DEC-4 = A1).
     *  `"sctp"` is now rejected: the SCTP factory opens an in-process
     *  usrsctp loopback socket and the engine never wires SCTP-over-DTLS,
     *  so it cannot reach a peer.
     *
     *  Setting this to `""` disables DataChannel entirely **in the legacy
     *  path** (this is the v0.x contract).  In the default
     *  `"web_transport"` family this field is ignored — clear
     *  `datachannel_backend` instead to disable the data plane.
     *
     *  Most callers should prefer `datachannel_backend` (below); `datachannel_name`
     *  is retained for power users that want to inject a custom factory
     *  into the engine (e.g. an in-tree `MockDataChannelFactory` for tests).
     *  When `datachannel_backend` is empty, `datachannel_name` is the
     *  effective selector; otherwise `datachannel_backend` wins.
     */
    std::string datachannel_name = "";

    /** DataChannel backend family selector (v0.14.0-rc.1, DC-1 v1.0.0
     *  headline gate).  The engine resolves this at `init_modules_once()`
     *  time and picks the matching `IDataChannelFactory` + (when needed)
     *  `IWebTransportSessionFactory` from `core::PluginRegistry`.
     *
     *  Valid values:
     *    - `"web_transport"` (default since v0.15.0) — WebTransport / QUIC
     *      path (v1.0.0 production). Uses the `"web_transport"` DataChannel
     *      factory + the `web_transport_name` (default `"msquic"`)
     *      WebTransport session factory. `datachannel_name` is ignored
     *      in this mode.
     *    - `"sctp"` — **retired** (v1.2.0, `docs/plan/datachannel-capability-plan.md`
     *      WP-A / DEC-4 = A1).  The SCTP backend was a loopback *test*
     *      backend: its factory opens an in-process usrsctp socket bound
     *      to `127.0.0.1` and the engine never wires SCTP-over-DTLS, so
     *      it could never reach a remote peer.  Passing `"sctp"` now
     *      surfaces `kEngineInternal` via `on_error_` at `open()` time
     *      instead of silently resolving to a data plane that cannot work.
     *    - `""`               — DataChannel disabled (same as
     *      `datachannel_name = ""`); falls back to `datachannel_name` if
     *      non-empty (backward compatibility for v0.x callers that set
     *      `datachannel_name` without `datachannel_backend`).
     *
     *  Unknown values — and the retired `"sctp"` — surface as
     *  `kEngineInternal` via `on_error_` at `open()` time; the engine
     *  does **not** silently fall back to another backend (typo'd backend
     *  ids would mask integrator errors).
     *
     *  Added in v0.14.0-rc.1 per `docs/plan/v1.0-plan.md` §2.1.1 PR-3
     *  (DC-1-LAND dependency) and the v0.14.0-beta.1 release notes
     *  "What is NOT in v0.14.0-beta.1" deferred-list entry for
     *  `EngineConfig::datachannel_backend`.
     *
     *  v0.15.0: default flipped from `"sctp"` to `"web_transport"`
     *  (WebTransport / QUIC product route). */
    std::string datachannel_backend = "web_transport";

    /** Data-channel framing mode (v1.2.0, DataChannel capability plan
     *  WP-B / WP-C).
     *
     *  - `plugins::DataChannelFraming::kRaw` (default) — the payload
     *    goes to the transport verbatim.  A QUIC stream is a byte
     *    stream, so two sends may surface as one message and one send
     *    may surface as several; and a byte stream carries no channel
     *    identity, so several channels on one connection cannot be
     *    told apart.  This is the mode Chrome's
     *    `RTCDataChannel`-over-WebTransport speaks.
     *  - `plugins::DataChannelFraming::kFramed` — every payload is
     *    wrapped in a length-prefixed frame that also carries the
     *    channel label.  The frame restores the message boundary (one
     *    `send()` == one inbound message) and it carries the channel
     *    identity, which is what makes several channels on one
     *    connection isolable.
     *
     *  Both ends must agree: a framed peer cannot read a raw stream and
     *  vice versa.  The setting applies to every channel the engine
     *  creates. */
    plugins::DataChannelFraming datachannel_framing =
        plugins::DataChannelFraming::kRaw;

    /** Scheduling priority of the channels the engine creates
     *  (v1.2.0, WP-E1).  Lower value = higher priority.  The value
     *  reaches the backend and orders the data plane's own outbound
     *  queue (a control channel is not stuck behind a bulk transfer
     *  channel on the same QUIC connection).  It does NOT arbitrate
     *  against the media plane — the data channel rides its own
     *  connection.  Default = 128 (the historical hard-coded value). */
    std::uint8_t datachannel_priority = 128;

    /** Maximum retransmission attempts of the partial-reliability
     *  count mode (v1.2.0, WP-D4).  The value reaches the backend and
     *  bounds the application-level retransmission protocol: a message
     *  that is not acknowledged within the budget is dropped, and the
     *  channel stays open.
     *
     *  0 = unlimited (the documented meaning in
     *  `plugins::DataChannelConfig`).  The setting applies to every
     *  channel the engine creates. */
    std::uint16_t datachannel_max_retransmits = 0;

    /** Packet lifetime of the partial-reliability TTL mode (v1.2.0,
     *  WP-D4), in milliseconds.  The value reaches the backend and
     *  bounds the lifetime of *one message*: a message that did not
     *  leave the sender within the lifetime is dropped, and the
     *  channel stays open.
     *
     *  0 = unlimited.  The setting applies to every channel the
     *  engine creates. */
    std::uint16_t datachannel_max_packet_lifetime_ms = 0;

    /** IWebTransportSession factory id (v0.14.0-rc.1, DC-1 v1.0.0
     *  headline gate).  Consumed only when `datachannel_backend ==
     *  "web_transport"`; the engine resolves
     *  `core::PluginRegistry::get_web_transport(web_transport_name)` at
     *  `init_modules_once()` time (readiness verification only — the
     *  actual session creation PR follows in v0.14.0-rc.1 PR after this
     *  one and wires the session through the DTLS/ICE pipeline).
     *
     *  Default = `"msquic"` (the v0.14.0-beta.1 msquic-backed factory;
     *  matches `list_web_transports()` at the beta.1 cut).
     *
     *  Empty string = disable WebTransport resolution (the engine logs a
     *  warning and `datachannel_backend = "web_transport"` then fails at
     *  `open()` with a clear error). */
    std::string web_transport_name = "msquic";

    /** WebTransport peer host (client mode, Case E).  Consumed only when
     *  `datachannel_backend == "web_transport"`; the engine copies it into
     *  `WebTransportConfig::peer_hostname` when creating the client-side
     *  `IWebTransportSession`.  Empty = in-process loopback (unit tests). */
    std::string web_transport_peer_host;

    /** WebTransport peer QUIC port (client mode, Case E).  Default = 443
     *  (standard WebTransport port; demo-wtserver uses 9443). */
    std::uint16_t web_transport_peer_port = 443;

    /** SCTP port advertised in `m=application` SDP lines (RFC 8831 §6.1).
     *  Default = 5000 (matches Chrome / WebRTC interop).
     *
     *  v1.2.0: the SCTP backend was retired from the public backend
     *  selector (WP-A / DEC-4 = A1), so this field is now only reached by
     *  the legacy power-user factory path (a custom `datachannel_name`
     *  factory).  It no longer configures any supported data plane. */
    std::uint16_t sctp_port = 5000;

    // ---- Cross-end timeline (ADR-017) -------------------------------------
    //
    // When enabled, the engine turns the timeline seam on:
    //
    //   1. SDP: the offer advertises `a=extmap:<id>` for the WebRTC
    //      abs-send-time extension (RFC 5285 §6 negotiation), and the
    //      answer echoes it.  The extension id actually used on the wire
    //      is the one the *peer* advertised for its own sending
    //      direction.
    //   2. Outbound RTP packets carry the abs-send-time element, which
    //      encodes the sender's wall-clock send instant (24 bits at
    //      65536 Hz, 256 s wrap).
    //   3. Outbound RTCP Sender Reports are sent every `sr_period_ms`,
    //      giving the peer the (NTP wall-clock ↔ RTP media clock) anchor
    //      it needs to reconstruct our capture instants.
    //   4. Inbound RTCP Sender Reports + inbound abs-send-time elements
    //      are fed to `nimrtc::rtp::SenderReportTracker`, which
    //      reconstructs the *peer's* capture instant for every inbound
    //      frame.  The result is exposed through `last_capture_ts_us()` /
    //      `capture_ts_us_for_ssrc()`.
    //
    // While this flag is false the engine MUST NOT emit abs-send-time
    // and MUST NOT schedule Sender Reports (ADR-017 §"Decision 5").
    //
    // Host applications that drive the engine from an `assembly::Profile`
    // copy `Profile::timeline_enabled` into this field.
    bool timeline_enabled = false;

    /** Sender Report cadence in milliseconds.  Default = 1000 ms
     *  (matches `nimrtc::rtp::kSrPeriodMs` and the WebRTC default).
     *  0 disables the periodic Sender Report while keeping the
     *  abs-send-time extension active. */
    std::uint32_t sr_period_ms = 1000;

    // ---- Transport strategy (v0.12.0 Slice 7.6 PR3) -------------------------
    //
    // Named use-case strategy (one of the 5 from
    // `docs/plan/transport-strategies.md` §2):
    //   - "browser-call"              (default — equivalent to empty)
    //   - "teleop-ultra-low-latency"
    //   - "agent-gateway"
    //   - "sfu-relay"
    //   - "cross-network-peer"
    //   - "shell"                     (SENTINEL — see below)
    //
    // Empty string (the default) means "use the engine compile-time
    // default", which is currently `browser-call`. The engine validates
    // the value at open() time via
    // `nimrtc::transport::strategy_by_name()` — an unknown name fails
    // the open() with a clear `std::invalid_argument` rethrown as a
    // non-zero engine error code. The strategy + Selector combination
    // is then recorded on the engine as a `TransportSession`
    // (accessible via `transport_strategy_session()` for diagnostics
    // and integration tests).
    //
    // The `cfg.transport_name` field above (default "ice") selects the
    // transport implementation (ICE, etc.); this new field selects the
    // USE-CASE shape (which media_stack + control_stack pair to compose).
    // The two are orthogonal: a "webrtc-classic"-shaped media stack can
    // be governed by any of the 5 strategies, depending on whether the
    // control side needs raw-udp-arq, etc.
    //
    // Sentinel "shell": a well-known reserved name that opts out of
    // the real ICE / DTLS / SRTP media pipeline entirely. The engine
    // resolves this to the Slice 8 shell stack factory
    // (id="default"; `CapabilitySelectorStack`), whose component
    // accessors return references to null-instance singletons whose
    // methods are no-ops or return `kErrNotReady`. Specifically:
    //   - `ice_t_->open()` returns `kOk` (NullIceTransport::open())
    //     and never attempts to bind UDP sockets;
    //   - DTLS/SRTP are inactive by design (no real session);
    //   - `cfg.local_port_range_begin/end` are never consulted.
    //
    // Use cases:
    //   - In-process unit tests that exercise engine wiring
    //     (`create_data_channel`, `create_offer`, plugin dispatch)
    //     without a network. On Windows GitHub Actions runners, the
    //     real `ice_t_->open()` of `webrtc-classic` fails with
    //     WSAEACCES (errno 10013) on `[52000, 52100]` because of
    //     restricted Winsock access in the sandbox, which breaks any
    //     test that needs to fully bring up an engine. The "shell"
    //     sentinel bypasses that bind entirely.
    //   - SDK / demo binaries that intentionally disable transport
    //     (render-only / source-only demos that exercise the encoder
    //     and codec paths but never connect to a peer).
    //
    // The sentinel is documented as a stable public API; downstream
    // tests and integration shims can rely on it as the canonical way
    // to construct a network-free engine instance.
    std::string_view transport_strategy_name = "";

    // ---- Audio pipeline plugin selection (v1.2.0, audio pipeline WP-7) ---
    //
    // The receive side of the audio path is a *plugin* (spec B-15 /
    // B-16): the engine owns no jitter buffer and no decoder, it only
    // drives `plugins::IAudioReceiver`.  The two ids below select the
    // plugins, and they are resolved through the registry's typed slots
    // (`core::PluginRegistry::get_audio_decoder()` /
    // `get_audio_receiver()`).
    //
    // There is deliberately **no fallback**: a missing decoder or a
    // missing receiver is a hard error (`kAudioDecoderMissing` /
    // `kAudioReceiverMissing`) that fires `on_error` exactly once and
    // disables the audio track.  A "built-in minimal receiver" would
    // decode nothing and hand the bit-stream over as if it were audio
    // (defect F-2).

    /** Audio-decoder plugin id (the registry's audio-decoder slot).
     *  Default = "opus" — the id the Opus module registers its decoder
     *  under (`nimrtc::opus::register_default_plugins()`).
     *
     *  An empty string disables the audio track explicitly (no error is
     *  reported: the host asked for it). */
    std::string_view audio_decoder_name = "opus";

    /** Audio receive-pipeline plugin id (the registry's audio-receiver
     *  slot).  Default = "nimrtc_audio_rx" — the id the audio_rx module
     *  registers its pipeline under.
     *
     *  An empty string disables the audio track explicitly (no error is
     *  reported: the host asked for it). */
    std::string_view audio_receiver_name = "nimrtc_audio_rx";

    // ---- Audio pipeline behaviour (v1.2.0, audio pipeline WP-10) -------
    //
    // The five fields below are appended at the **end** of the struct, so
    // an existing host that aggregate-initialises `EngineConfig` keeps
    // compiling (spec E-6).

    /** Enable Opus in-band FEC on the receive path.
     *
     *  Must match what the SDP signals: the offer carries
     *  `useinbandfec=1` when this is true, so a host that turns it off
     *  must not accept a peer that relies on it.  The decoder plugin
     *  receives the flag through `AudioReceiverConfig::enable_fec_recovery`
     *  (spec B-8).  Default true. */
    bool audio_use_inband_fec = true;

    /** Enable Opus DTX (discontinuous transmission).
     *
     *  Only honoured when the peer signals `usedtx=1` in its `a=fmtp:`
     *  line — a DTX stream from a peer that did not negotiate it is not
     *  something the engine may invent.  Default false. */
    bool audio_use_dtx = false;

    /** Target jitter-buffer depth for the inbound audio stream, in ms.
     *
     *  This is the *audio* target; the video path keeps its own tuning
     *  (`video_receiver_tuning`).  It is handed to the receive pipeline
     *  as `AudioReceiverConfig::target_delay_ms`.  Default 40. */
    int audio_jitter_target_delay_ms = 40;

    /** Maximum number of consecutive concealed frames the receive
     *  pipeline may emit before it gives up on the stream.
     *
     *  Handed to the pipeline as `AudioReceiverConfig::max_concealed_frames`
     *  (spec B-7).  Default 10. */
    int audio_max_concealed_frames = 10;

    /** Downmix the decoded inbound stream to `pcm_channels`.
     *
     *  When false, the pipeline keeps the decoder's own channel count
     *  and the host receives a stereo stream on a mono configuration
     *  (spec B-9).  Default true. */
    bool audio_downmix_to_pcm_channels = true;

    /** In-band redundancy the audio encoder embeds, in percent (0..100).
     *
     *  The value is the "negotiated redundancy" of spec B-8: it is
     *  handed to the encoder (`OPUS_SET_PACKET_LOSS_PERC`) so that every
     *  frame carries a low-bitrate copy of the previous one, and to the
     *  decoder, which uses it as its own redundancy hint.  A receiver
     *  that lost a frame reconstructs it from the redundancy the *next*
     *  packet carries.
     *
     *  0 disables the redundancy (the encoder then carries no copy at
     *  all and the FEC recovery path cannot work).  Default 5 %, the
     *  value the specification names. */
    int audio_redundancy_percent = 5;

    // ---- Adaptive audio bitrate (BWE-linked) --------------------------

    /** Target bitrate of the outbound audio encoder, in bits per second.
     *
     *  0 (the default) means "codec default": 32 kbps for a mono
     *  configuration (`pcm_channels == 1`) and 64 kbps for a stereo one.
     *  The value is also the *ceiling* of the adaptive bitrate — the
     *  encoder never runs above it. */
    int audio_bitrate_bps = 0;

    /** Floor of the adaptive audio bitrate, in bits per second.
     *
     *  The engine never lowers the encoder below this value, so a
     *  bandwidth collapse cannot starve the audio stream entirely.
     *  Default 8 kbps (Opus stays intelligible down to ~6 kbps). */
    int audio_min_bitrate_bps = 8000;

    /** Enable the BWE-linked audio bitrate adaptation.
     *
     *  When true (the default) the engine lowers / raises the audio
     *  encoder's bitrate from the bandwidth estimate, so a session on a
     *  very narrow link degrades the audio instead of losing it.  Set
     *  it to false to pin the encoder at `audio_bitrate_bps`.
     *
     *  The adaptation runs from `tick()` and reconfigures the codec
     *  plugin, so `tick()` and `send_audio()` have to be called from the
     *  same thread — the same rule the rest of the engine already
     *  follows. */
    bool audio_bitrate_adaptive = true;

    /** Share of the bandwidth estimate the audio encoder may use.
     *
     *  The audio budget is `estimate.target_bitrate_bps * share`, so
     *  the video stream keeps the rest.  Default 0.1 (10 %). */
    double audio_bwe_share = 0.1;

    /** Minimum interval between two bitrate adaptation steps, in ms.
     *
     *  The adaptation runs from `tick()`, so the effective granularity
     *  is this value.  Default 1000 ms. */
    int audio_bitrate_update_interval_ms = 1000;

    // ---- Session model selection (v1.2.0, session capability plan N1/T1) --
    //
    // The engine drives renegotiation (add / remove a track, change a
    // direction) through a *pluggable* session model.  The model is
    // resolved at `open()` time through the registry's session slot:
    //
    //     core::PluginRegistry::instance().get_session(session_name)
    //
    // The built-in implementation lives in `src/modules/session/` and
    // registers under the id "session"; a host that wants a different
    // mid-allocation or BUNDLE policy implements `plugins::ISession`
    // and registers it under its own id.
    //
    // An empty string disables the session model entirely: the engine
    // then keeps the pre-v1.2.0 behaviour (a fixed two-m-line offer with
    // no track API).  The field is appended at the **end** of the struct
    // so an existing host that aggregate-initialises `EngineConfig`
    // keeps compiling.
    std::string_view session_name = "session";

    // ---- DTMF (v1.2.0, session capability plan D1-D4) ------------------
    //
    // RFC 4733 carries DTMF digits as in-band `telephone-event`
    // payloads on the audio m-line.  Decision OD-B of the plan: DTMF is
    // **off by default** -- a host has to ask for it explicitly, so the
    // default offer carries no telephone-event declaration and the
    // pre-existing drop path for an unknown audio payload type stays
    // untouched.
    //
    // With `dtmf_enabled == true` the engine
    //
    //   - declares `a=rtpmap:<pt> telephone-event/8000` and
    //     `a=fmtp:<pt> 0-16` on the audio m-line (RFC 4733 §2.3.2),
    //   - decodes a received telephone-event payload into key events and
    //     reports them through `set_on_dtmf()`,
    //   - encodes the events a host passes to `send_dtmf()`.
    bool dtmf_enabled = false;

    /** The dynamic payload type used for telephone-event.
     *
     *  RFC 4733 §2.3.2 negotiates it in the SDP.  The default is 126,
     *  the value Chrome uses. */
    std::uint8_t dtmf_payload_type = 126;

    /** IDtmfCodec plugin name.  Default = "dtmf" (the built-in
     *  RFC 4733 codec in `src/modules/dtmf/`).  An empty string
     *  disables the DTMF path even when `dtmf_enabled` is true. */
    std::string_view dtmf_codec_name = "dtmf";

    // ---- RTCP XR / TMMBR (v1.2.0, session capability plan X2-X5) ------
    //
    /** Send RFC 3611 Extended Reports.
     *
     *  The reports carry the blocks NimRTC implements (RFC 3611 §4.4 /
     *  §4.5 / §4.6): the **Receiver Reference Time** report (BT = 4),
     *  the **DLRR** report (BT = 5) and the **Statistics Summary**
     *  report (BT = 6).  The SDP carries the matching `a=rtcp-xr:`
     *  attribute (RFC 3611 §5.1).
     *
     *  Default = false: an XR report is a QoE / network-statistics
     *  report, and a host that does not consume those metrics should
     *  not pay for the packets. */
    bool rtcp_xr_enabled = false;

    /** Send TMMBR feedback (RFC 5104 §4.2.1).
     *
     *  A TMMBR tells the peer "you may not send me more than this".
     *  The engine reports the bit rate its own bandwidth estimator is
     *  currently *using*, so the peer and the engine agree on the
     *  number: a TMMBR that reports a limit the sender does not obey
     *  is worse than no TMMBR at all.
     *
     *  Default = false: the feature is opt-in, because a TMMBR changes
     *  how the peer sends, and a host that never asked for that should
     *  not get it. */
    bool tmmbr_enabled = false;

    /** Do not send TMMBR feedback (RFC 5104).
     *
     *  The `disable_tmmbr` knob of `plugins::SdpCodecConfig` used to be
     *  a dead field: nothing in the engine read it.  This field is the
     *  engine-side switch -- when it is set, the engine sends no
     *  TMMBR, and the SDP carries no `a=rtcp-fb` entry for it either.
     *
     *  It overrides `tmmbr_enabled`: a host that sets both gets no
     *  TMMBR.
     *
     *  Default = false (TMMBR is governed by `tmmbr_enabled`). */
    bool disable_tmmbr = false;

    // ---- Media pacing (v1.2.0 M3, video plan P2) ----------------------
    //
    // When true (the default) the engine drives the scheduler's
    // **per-class token buckets** from the bandwidth estimate: the
    // session's estimate is split between the streams (the audio takes
    // its reserved share first, the video takes what is left), and each
    // class is paced at its own share.  A saturated video stream can
    // therefore no longer eat the audio's budget, and the outbound
    // traffic leaves the machine at a *rate* instead of in bursts.
    //
    // The control plane (RTCP feedback, NACK retransmissions) is never
    // paced: delaying feedback costs more than the bytes it occupies.
    //
    // Set it to false to restore the pre-v1.2.0 behaviour, where the
    // scheduler only had the aggregate session budget.
    bool media_pacing_enabled = true;
};
// ---------------------------------------------------------------------------
// Public callbacks
// ---------------------------------------------------------------------------

using NIMRTC_API_STABLE AudioFrameCallback = std::function<void(const float* pcm, size_t num_samples)>;
using NIMRTC_API_STABLE VideoFrameCallback = std::function<void(const uint8_t* nal, size_t len, bool keyframe)>;
using NIMRTC_API_STABLE EngineErrorCallback = std::function<void(uint32_t err, std::string_view msg)>;
using NIMRTC_API_STABLE EngineStateCallback = std::function<void(const char* state)>;

/** Called for every DTMF event the engine decodes.
 *
 *  v1.2.0 (session capability plan D1): the callback fires once per
 *  *completed* RFC 4733 event -- that is, when the end-of-event packet
 *  arrives, with the accumulated duration of the whole key press.  The
 *  intermediate packets of the repetition sequence (RFC 4733 §2.5) do
 *  not fire it, so a host that counts events does not have to
 *  de-duplicate. */
using NIMRTC_API_STABLE DtmfEventCallback =
    std::function<void(const nimrtc::plugins::DtmfEvent& event)>;

// ---------------------------------------------------------------------------
// Audio frame details (v1.2.0, audio pipeline WP-10, spec E-1)
// ---------------------------------------------------------------------------

/** Everything the host needs to know about one delivered audio frame.
 *
 *  This is the *detailed* counterpart of the legacy
 *  `AudioFrameCallback`: the legacy callback carries the PCM and
 *  nothing else, this one carries the PCM together with the metadata a
 *  host needs to place the frame on a timeline (spec E-1). */
struct NIMRTC_API_STABLE AudioFrameInfo {
    /** Interleaved PCM, float, [-1.0, 1.0].
     *  Length = num_samples_per_channel * channels.
     *  Valid only for the duration of the callback. */
    const float*  pcm                 = nullptr;

    /** Samples per channel in `pcm`. */
    std::size_t   num_samples_per_channel = 0;

    /** Sample rate of `pcm` — the decoder's native rate (48000 for Opus).
     *  The host resamples if its device rate differs. */
    std::uint32_t sample_rate_hz      = 48000;

    /** Channel count of `pcm` — after downmix to the configured output
     *  channel count. */
    std::uint8_t  channels            = 1;

    /** RTP timestamp of the first sample in this frame. */
    std::uint32_t rtp_timestamp       = 0;

    /** Engine-thread instant at which the frame was handed to the host.
     *  On the engine's monotonic origin (same epoch as `tick()`'s `now_us`),
     *  NOT the Unix wall clock. */
    std::int64_t  playout_time_us     = 0;

    /** Peer's capture instant of this frame, in Unix microseconds.
     *  0 when the cross-end timeline seam has not yet reconstructed it
     *  (no usable Sender Report / abs-send-time yet — ADR-017).
     *  This is the audio side of the A/V-sync contract (C-4): the host
     *  compares it against the video frame's `capture_ts_us`. */
    std::int64_t  capture_time_us     = 0;

    /** True when this frame is concealment (PLC) audio. */
    bool          concealed           = false;

    /** True when this frame was reconstructed from in-band FEC. */
    bool          fec_recovered       = false;
};

/** The negotiated format of the inbound audio stream.
 *  Filled in by `remote_audio_format()` once the remote SDP has been
 *  processed. */
struct NIMRTC_API_STABLE AudioFormat {
    /** Decoder output rate — the codec's native rate (48000 for Opus). */
    std::uint32_t sample_rate_hz      = 48000;

    /** Channel count after the engine's downmix — what the host receives. */
    std::uint8_t  channels            = 1;

    /** The negotiated payload type (RFC 7587 Opus = 111). */
    std::uint8_t  payload_type        = 111;

    /** Negotiated frame duration in ms (10 / 20 / 40 / 60). */
    std::uint16_t frame_duration_ms   = 20;
};

using NIMRTC_API_STABLE AudioFrameInfoCallback =
    std::function<void(const AudioFrameInfo& info)>;

// ---------------------------------------------------------------------------
// NimRTCEngine
// ---------------------------------------------------------------------------

/** Main entry point for the NimRTC stack.
 *
 * ## Per-symbol stability annotations (v0.14.x PER-SYMBOL-CLASSIFY)
 *
 * The file-level header comment carries the full per-method list.
 * In brief:
 *   - Lifecycle (`open()` / `pre_open()` / `close()` / `tick()`) — STABLE.
 *   - Core payload (`send_audio()` / `create_offer()` /
 *     `process_remote_sdp()` / `create_data_channel()`) — STABLE.
 *   - Video (`start_video()` / `stop_video()` / `send_video()`) — UNSTABLE.
 *   - Accessors (`set_on_*()` / ICE diagnostics / plugin handles) — STABLE.
 *
 * The macro expands to nothing in default builds; it becomes load-bearing
 * when `NIMRTC_ENABLE_API_STABILITY_ANNOTATIONS=1` is set, at which point
 * `NIMRTC_API_UNSTABLE` emits the cross-compiler `[[deprecated]]` attribute.
 * Per ADR-015 §Decision 3: v0.x → v1.0.0 may mark DEPRECATED
 * but does not REMOVE any symbol. */

class NIMRTC_API_STABLE NimRTCEngine {
public:
    // STABLE: the four fundamental engine lifecycle entry points; every
    // caller invokes at least one of them.
    /** Construct from a config. The caller may destroy / mutate `config`
     *  after this returns. */
    explicit NIMRTC_API_STABLE NimRTCEngine(EngineConfig config);
    NIMRTC_API_STABLE ~NimRTCEngine();

    NIMRTC_API_STABLE NimRTCEngine(const NimRTCEngine&)            = delete;
    NIMRTC_API_STABLE NimRTCEngine& operator=(const NimRTCEngine&) = delete;

    // Lifecycle -----------------------------------------------------------
    //
    // `open()` / `pre_open()` / `close()` / `tick()` are the four
    // fundamental engine lifecycle entry points.  Marked STABLE.

    /** Bring the engine up: build the ICE / DTLS / SRTP pipeline and
     *  start ICE candidate gathering.  Idempotent only in the
     *  kConstructed -> kOpen transition; subsequent calls return
     *  immediately with kOk (or the prior open() error code). */
    NIMRTC_API_STABLE uint32_t open() noexcept;

    /** Tear the engine down: close all plugins, drop ICE sockets, drain
     *  pending DTLS / SRTP records.  Safe to call multiple times. */
    NIMRTC_API_STABLE void     close() noexcept;

    /** True iff the engine is in the kOpen state. Cheap inline check;
     *  usable from any thread (reads an atomic state_). */
    NIMRTC_API_STABLE bool     is_open() const noexcept { return state_ == State::kOpen; }

    /** Optional early setup: creates the ICE transport (and all other modules)
     *  WITHOUT starting ICE candidate gathering.  Callers can then call
     *  set_remote_ice() to inject the peer's ICE credentials before open().
     *  This allows the loopback test to ensure the answerer knows the offerer's
     *  ICE ufrag/pwd BEFORE gather_candidates() starts, so libjuice sets the
     *  answerer to CONTROLLED instead of CONTROLLING (avoids ICE role conflict).
     *
     *  After pre_open(), call open() normally — gather_candidates() will fire.
     *  Can be called at most once. Returns same error codes as open(). */
    NIMRTC_API_STABLE uint32_t pre_open() noexcept;

    // Callbacks -----------------------------------------------------------
    //
    // `set_on_*()` install std::function callbacks the engine invokes during
    // tick().  Marked STABLE — the setter signature has been stable since
    // v0.10.0 and is consumed by every demo / test.

    /** Install the audio-frame callback.  Fires on the engine thread at
     *  the configured PCM rate.  Pass an empty std::function to clear. */
    NIMRTC_API_STABLE void set_on_audio_frame(AudioFrameCallback cb) noexcept { on_audio_frame_ = std::move(cb); }

    /** Install the detailed audio-frame callback.
     *
     *  Fires on the engine thread, after decoding, after downmix, and
     *  after the 3A render stage.  When it is set, it fires *after* the
     *  legacy callback; both receive the same buffer (spec E-2).
     *
     *  The legacy callback stays the recommended surface for a host that
     *  only needs PCM — this one exists for the host that has to place
     *  the frame on a timeline (A/V sync, capture-time bookkeeping).
     *  Pass an empty std::function to clear. */
    NIMRTC_API_STABLE void set_on_audio_frame_info(AudioFrameInfoCallback cb) noexcept {
        on_audio_frame_info_ = std::move(cb);
    }

    /** Install the video-frame callback.  Fires on the engine thread
     *  when the video pipeline decodes a complete frame (raw NAL).
     *  Pass an empty std::function to clear. */
    NIMRTC_API_STABLE void set_on_video_frame(VideoFrameCallback cb) noexcept { on_video_frame_ = std::move(cb); }

    /** Install the error callback.  Fires on the engine thread on
     *  non-fatal errors (DTLS retry exhausted, ICE consent loss, etc).
     *  Pass an empty std::function to clear. */
    NIMRTC_API_STABLE void set_on_error(EngineErrorCallback cb) noexcept     { on_error_       = std::move(cb); }

    /** Install the state-change callback.  Fires on the engine thread
     *  when the ICE / DTLS / connection state transitions.  The string
     *  argument is one of the canonical ICE state names ("new",
     *  "checking", "connected", "completed", "disconnected", "failed",
     *  "closed").  Pass an empty std::function to clear. */
    NIMRTC_API_STABLE void set_on_state_change(EngineStateCallback cb) noexcept { on_state_change_ = std::move(cb); }

    // ---- DataChannel callbacks (forwarded to every channel create_data_channel() returns) -
    //
    // Set once at engine construction / pre_open() time.  Every channel
    // created via create_data_channel() after this point receives these
    // callbacks through `IDataChannel::set_on_message` /
    // `IDataChannel::set_on_state` before its open() call.  The callbacks
    // are stored as `plugins::DataMessageCallback` /
    // `plugins::DataChannelStateCallback` (declared in
    // `src/plugins/include/nimrtc/plugins/datachannel.hpp`).
    //
    // No-op when the engine has `cfg.datachannel_name.empty()` — the
    // callbacks are still stored so callers don't have to gate by config.
    // (v1.2.0: `datachannel_name` defaults to `""`; in the default
    // `"web_transport"` backend family the field is ignored altogether
    // and the callbacks are still installed on the created channels.)
    NIMRTC_API_STABLE void set_on_data_message(plugins::DataMessageCallback cb) noexcept {
        on_data_message_ = std::move(cb);
    }
    NIMRTC_API_STABLE void set_on_data_state(plugins::DataChannelStateCallback cb) noexcept {
        on_data_state_ = std::move(cb);
    }

    /**
     * @brief Install the identity-carrying data-message callback
     *        (v1.2.0, WP-C1).
     *
     * The additive counterpart of `set_on_data_message()`: the callback
     * receives the same payloads, plus the identity of the channel the
     * message arrived on (`plugins::DataChannelMessage::label` and
     * `::channel_id`).
     *
     * A host that opens two channels can therefore tell which one a
     * message belongs to — the plain callback cannot express that.
     * The callback is installed on every channel the engine creates;
     * `channel_id` is assigned by the engine (monotonically increasing
     * per engine, starting at 1) and `label` is the channel's label.
     */
    NIMRTC_API_STABLE void set_on_data_message_ex(
        plugins::DataChannelMessageCallback cb) noexcept {
        on_data_message_ex_ = std::move(cb);
    }

    // ---- DataChannel creation --------------------------------------------
    //
    // Creates a new data channel by calling
    // `IDataChannelFactory::create()` on the factory resolved via
    // `core::PluginRegistry::instance().get_datachannel(datachannel_name)`.
    //
    // Returns nullptr when:
    //   - The data plane is disabled (the resolved factory id is empty —
    //     `datachannel_backend == ""`, see `init_datachannel_registry()`)
    //   - The engine is not open()ed yet
    //   - The factory id is not registered (the engine is open but the
    //     caller hasn't called `core::register_all_default_plugins()` or
    //     registered the requested backend themselves).
    //   - The factory's create() returned null (catastrophic OOM / impl bug).
    //
    // On success, the returned channel is open()ed with a default config
    // (label = @p label, reliability = kReliableOrdered, priority = 128,
    // ordered = true).  The engine installs `on_data_message_` /
    // `on_data_state_` on the channel before its open() call so the
    // callback set up via set_on_data_message() / set_on_data_state()
    // takes effect immediately.
    //
    // Note (v0.11.x follow-up): the returned channel is NOT yet wired
    // to the DTLS transport — see the comment in init_modules_once()
    // and create_data_channel().  Physical SCTP-over-DTLS plumbing is
    // deferred to the v0.11.x engine-integration follow-up PR.  v0.11.0
    // satisfies DC-1 Gate #1 via the §7 fallback (in-process
    // `test_datachannel_engine` 5/5 PASS).
    //
    // Must be called after open() succeeds — pre_open() / open() build
    // the ICE / DTLS / SRTP pipeline the channel will eventually sit on.
    // Calling before open() returns nullptr because the registry slot
    // isn't probed until init_modules_once() runs.
    //
    // STABLE: the factory-resolve + callback-install contract has been
    // stable since v0.11.0; the WebTransport / msquic backend that
    // lands in v0.14.x is a backend swap (no API change at this
    // signature).
    NIMRTC_API_STABLE
    [[nodiscard]]
    std::unique_ptr<plugins::IDataChannel>
    create_data_channel(std::string_view label) noexcept;

    /** Create a data channel with explicit reliability.
     *
     *  v0.15.0 (WebTransport product route): exposes the reliability
     *  selector that the label-only overload pins to kReliableOrdered.
     *  `kUnreliable` routes to the WebTransport datagram plane
     *  (`send_datagram`); `kReliableOrdered` routes to a bidirectional
     *  QUIC stream.  SCTP backend keeps its existing mapping.
     *
     *  The label-only overload delegates here with kReliableOrdered. */
    NIMRTC_API_STABLE
    [[nodiscard]]
    std::unique_ptr<plugins::IDataChannel>
    create_data_channel(std::string_view label,
                        plugins::DataChannelReliability reliability) noexcept;

    // SDP — RFC 8829 compliant -----------------------------------------
    //
    // `create_offer()` / `process_remote_sdp()` are the two engine
    // entry points every P2P / interop demo exercises.  Marked STABLE;
    // the SDP serialisation is documented in `docs/plan/sdp-grammar.md`
    // and consumed by Chrome / libjuice / libwebrtc peers.
    NIMRTC_API_STABLE std::string create_offer() noexcept;
    NIMRTC_API_STABLE std::optional<std::string> process_remote_sdp(std::string_view remote_sdp) noexcept;

    // ---- Session / tracks / transceivers (v1.2.0, session capability plan)
    //
    // N1 / N2 / N3 / N4 / N5 and T1 / T2 / T3 / T4 of
    // `docs/plan/session-capability-plan.md`.
    //
    // ## The model
    //
    // The engine keeps a *session* — a set of tracks and a set of
    // transceivers (one transceiver per SDP m-line).  The session model
    // itself is a plugin (`plugins::ISession`), resolved through
    // `EngineConfig::session_name`; the built-in one registers under
    // the id "session".
    //
    // The host mutates the session through the calls below and then
    // asks for a new offer.  `needs_negotiation()` tells the host
    // whether the session changed since the last offer / answer, so a
    // signalling loop does not have to track the changes itself.
    //
    // ## Direction mapping (N4)
    //
    // The m-line's direction attribute is the direction of the
    // transceiver that owns the m-line:
    //
    //   | transceiver / track state            | m-line attribute |
    //   |--------------------------------------|------------------|
    //   | send + receive                       | `a=sendrecv`     |
    //   | send only                            | `a=sendonly`     |
    //   | receive only                         | `a=recvonly`     |
    //   | nothing attached                     | `a=inactive`     |
    //
    // ## Removed tracks and stopped transceivers (OD-A / N5 / T4)
    //
    // A removed track does **not** make its m-line disappear: the m-line
    // stays in the offer / answer with the direction `inactive`, and its
    // mid is never recycled.  The same holds for a stopped transceiver.
    // Keeping the mid stable is what stops the peer from mistaking a
    // removed m-line for a newly negotiated one.
    //
    // @note The media pipeline carries **one** audio stream and **one**
    //       video stream.  A transceiver the host adds beyond those is
    //       advertised in the SDP (so the negotiation is complete and
    //       the peer sees the m-line), but it does not open a second
    //       media pipeline.

    /** Add a track to the session.
     *
     *  The track is bound to a transceiver of the same kind — an
     *  existing one with a free send slot, or a new transceiver, in
     *  which case the offer grows by one m-line.
     *
     *  @return false when the id is already taken or the session model
     *          is disabled. */
    NIMRTC_API_STABLE bool add_track(std::string_view id,
                                     plugins::MediaKind kind,
                                     plugins::Direction dir) noexcept;

    /** Remove a track.
     *
     *  The track leaves the transceiver's slot; the m-line stays and
     *  turns `inactive` (OD-A).  The mid is not recycled.
     *
     *  @return false when no track with that id existed. */
    NIMRTC_API_STABLE bool remove_track(std::string_view id) noexcept;

    /** Change a track's direction.
     *
     *  The m-line that carries the track follows: `kSendOnly` becomes
     *  `a=sendonly`, `kRecvOnly` becomes `a=recvonly`, `kInactive`
     *  becomes `a=inactive`.
     *
     *  @return false when no track with that id existed. */
    NIMRTC_API_STABLE bool set_track_direction(std::string_view id,
                                               plugins::Direction dir) noexcept;

    /** Snapshot of every track in the session. */
    NIMRTC_API_STABLE std::vector<plugins::TrackInfo> tracks() const noexcept;

    /** Create a transceiver — one new m-line.
     *
     *  @return The allocated mid, or an empty string on failure.  The
     *          mid is unique and never reused. */
    NIMRTC_API_STABLE std::string add_transceiver(plugins::MediaKind kind,
                                                  plugins::Direction dir) noexcept;

    /** Change a transceiver's direction (the m-line's direction
     *  attribute). */
    NIMRTC_API_STABLE bool set_transceiver_direction(std::string_view mid,
                                                     plugins::Direction dir) noexcept;

    /** Stop a transceiver.
     *
     *  The m-line stays in the SDP with the direction `inactive` and the
     *  mid is not recycled (OD-A / T4).
     *
     *  @return false when no transceiver with that mid existed. */
    NIMRTC_API_STABLE bool stop_transceiver(std::string_view mid) noexcept;

    /** Restart a stopped transceiver (its previous direction is
     *  restored). */
    NIMRTC_API_STABLE bool restart_transceiver(std::string_view mid) noexcept;

    /** Snapshot of every transceiver in the session. */
    NIMRTC_API_STABLE std::vector<plugins::TransceiverInfo> transceivers() const noexcept;

    /** True when the session changed since the last offer / answer.
     *
     *  A host that drives renegotiation polls this (or watches the
     *  return value of the mutating calls) to decide whether it has to
     *  produce and send a new offer.  The flag is cleared by
     *  `create_offer()` and by `process_remote_sdp()`. */
    NIMRTC_API_STABLE bool needs_negotiation() const noexcept;

    // ---- DTMF (v1.2.0, session capability plan D1 / D2) ----------------
    //
    // RFC 4733 in-band DTMF over the audio m-line.  Both calls need
    // `EngineConfig::dtmf_enabled == true`; without it the engine
    // behaves exactly as it did before DTMF existed (no telephone-event
    // declaration in the SDP, a received telephone-event payload counts
    // as an unknown payload type).
    //
    // @see `docs/plan/session-capability-plan.md` §2 (Track D) and the
    //      decision OD-B.

    /** Install the callback that receives decoded DTMF events.
     *
     *  The callback fires once per completed event (RFC 4733 §2.5 end
     *  packet), with the accumulated duration of the key press. */
    NIMRTC_API_STABLE void set_on_dtmf(DtmfEventCallback cb) noexcept;

    /** Send a DTMF event.
     *
     *  The engine encodes the event into RFC 4733 telephone-event
     *  payloads and sends them on the audio m-line, following the
     *  repetition and end-of-event timing of RFC 4733 §2.5.
     *
     *  @param event_code   The RFC 4733 event code (0..16).
     *  @param duration_ms  How long the key was held, in milliseconds.
     *  @return false when DTMF is disabled, when the event code is out
     *          of range, or when the engine is not open. */
    NIMRTC_API_STABLE bool send_dtmf(std::uint8_t event_code,
                                     std::uint16_t duration_ms) noexcept;

    /** The negotiated telephone-event payload type (RFC 4733 §2.3.2).
     *
     *  Zero when no telephone-event payload type was negotiated -- which
     *  is the case when DTMF is disabled, when the peer offered no
     *  telephone-event payload type, or when the peer's answer dropped
     *  the one the offer carried.  A host that wants to know whether a
     *  key press can actually reach the peer asks here: `send_dtmf()`
     *  fails in exactly that state.
     *
     *  The number is the *peer's* number: RFC 3264 §6.1 binds both ends
     *  of the session to the payload type the negotiated description
     *  names, and the answerer must use the offerer's number. */
    NIMRTC_API_STABLE std::uint8_t negotiated_dtmf_payload_type() const noexcept;

    // ---- Audio I/O --------------------------------------------------------
    //
    // Opus encode + RTP packetise + SRTP encrypt + transport.send.
    // STABLE: the audio I/O surface is the load-bearing P2 contract.
    NIMRTC_API_STABLE uint32_t send_audio(const float* pcm_samples, size_t num_samples) noexcept;

    // ---- Video I/O --------------------------------------------------------
    //
    // Outbound: drive the full video pipeline:
    //   send_video(frame) → video_codec_ (encode H.264)
    //   → video_sender_ (FU-A packetize) → RTP header → SRTP
    //   → scheduler_ (priority queue, tick drains to ICE).
    // The engine tick() must be called frequently (≥30 Hz) to drain the
    // scheduler and send packets.  start_video() / stop_video() control the
    // video SOURCE's cadence — but the source is NOT auto-wired into the
    // encoder (IVideoCodec / IVideoSender are not thread-safe), so the host
    // pulls frames from video_source() and pushes them via send_video().
    //
    // Inbound: ICE recv → on_transport_recv → handle_rtp (PT demux)
    //   → video_receiver_ (FU-A depacketize) → video_codec_ (decode)
    //   → video_sink_ (render).  The on_video_frame_ callback fires on
    //   the raw NAL level (before decode) for users who want to do their
    //   own rendering.
    //
    // Both paths require `open()` to have been called first.
    //
    // UNSTABLE: the video API is still P1.1 / R3 scope; the surface
    // may shift in a v0.15.x follow-up before v1.0.0 final.  Per
    // ADR-015 §Decision 2, marking UNSTABLE gives the engine room to
    // refactor the FU-A depacketise path / packetiser FU-A helpers /
    // NACK path in `video_payload/` without bumping the MAJOR version.

    /** Start the video capture source (produces frames at the configured fps).
     *  Idempotent if already running.  Returns kOk on success.
     *
     *  Note: the source's frames are NOT auto-encoded or sent — the host
     *  must pull them (via `video_source()`) and push them through
     *  `send_video()`.  See the "Video I/O" note above. */
    NIMRTC_API_UNSTABLE plugins::Status start_video() noexcept;

    /** Stop the video capture source. Idempotent if already stopped. */
    NIMRTC_API_UNSTABLE void stop_video() noexcept;

    /** Manually push one raw frame through the video pipeline.
     *  Bypasses the video_source capture cadence; useful for feeding
     *  camera frames directly (caller owns the cadence loop).
     *  The frame is encoded, packetized, and enqueued to the scheduler
     *  exactly like the auto-capture path.
     *  @param frame  Raw frame (must remain valid for the call).
     *
     *  UNSTABLE: part of the video API surface (see start_video()). */
    NIMRTC_API_UNSTABLE
    plugins::Status send_video(const plugins::VideoSourceFrame& frame) noexcept;

    // ---- Ref-frame timeline map (v1.1 T1/T2, UNSTABLE) -------------------
    //
    // Local frame-id <-> capture-timestamp registry. Unlike the ADR-017
    // cross-end timeline (peer's clock reconstructed from Sender Reports),
    // this map tracks OUR OWN frames: send_video() records
    // (frame_seq -> capture_ts_us) automatically, and the inbound
    // receiver callback records the peer's (frame_seq -> capture_ts_us)
    // the same way. Hosts driving AI-agent / teleop loops use it to
    // answer "which capture instant does control message X refer to?"
    // without touching the SR tracker.
    //
    //   - ref_frame id == plugins::VideoSourceFrame::frame_seq
    //     (per-SSRC, strictly increasing). Reusing frame_seq avoids any
    //     ABI break on the STABLE VideoSourceFrame struct.
    //   - record_video_ref_frame() is idempotent: re-recording an id
    //     updates its timestamp (last-writer-wins).
    //   - Bounded to the most recent 1024 entries (ring eviction); older
    //     ids return nullopt from the lookups.
    //   - data_message_frame_age_us() returns now_us - capture_ts for a
    //     known id (T2 command-to-frame association), or nullopt when
    //     the id is unknown.
    //
    // Example (T2 command-frame association):
    // @code
    //   // Sender: tag outgoing control messages with the frame_seq you
    //   // just sent via send_video().
    //   engine.send_video(frame);  // frame.frame_seq = 42
    //   dc->send(control_bytes_for_frame_42);
    //   // Receiver/host side: resolve the capture instant + age.
    //   if (auto ts = engine.ref_frame_to_capture_ts(42)) {
    //       auto age = engine.data_message_frame_age_us(42);
    //   }
    // @endcode
    //
    // UNSTABLE: registry capacity + eviction policy may be tuned.

    /** Record (or refresh) one ref_frame mapping. Idempotent. */
    NIMRTC_API_UNSTABLE
    void record_video_ref_frame(std::uint64_t ref_frame_id,
                                std::int64_t capture_ts_us) noexcept;

    /** Capture timestamp for a known ref_frame id, or nullopt. */
    NIMRTC_API_UNSTABLE
    std::optional<std::int64_t> ref_frame_to_capture_ts(
        std::uint64_t ref_frame_id) const noexcept;

    /** Ref_frame id for a known capture timestamp, or nullopt. */
    NIMRTC_API_UNSTABLE
    std::optional<std::uint64_t> capture_ts_to_ref_frame(
        std::int64_t capture_ts_us) const noexcept;

    /** Age (now_us - capture_ts) for a known ref_frame id, or nullopt. */
    NIMRTC_API_UNSTABLE
    std::optional<std::int64_t> data_message_frame_age_us(
        std::uint64_t ref_frame_id) const noexcept;

    // --- DTLS/SRTP internal pipeline -----------------------------------
    //
    // These two methods drive the DTLS and SRTP handshake steps inside
    // the engine tick.  Marked STABLE so the tick() contract is frozen:
    // callers (tests / demo harness / msquic handoff) can rely on being
    // able to drain DTLS records or install SRTP keys without these
    // signatures changing.

    /** Drive DTLS state machine: feed pending outbound DTLS records back
     *  through ICE.  Returns number of records sent. */
    NIMRTC_API_STABLE int drain_dtls() noexcept;

    /** Once DTLS connected, pull SRTP keying material and install on the
     *  SrtpContext.  Idempotent. */
    NIMRTC_API_STABLE void maybe_install_srtp_keys() noexcept;

    // --- Inspection helpers (for tests / debugging) ---

    /** True iff SRTP keys have been installed (post-DTLS-Connected).
     *  Out-of-line to keep the public header free of <nimrtc/srtp/srtp.hpp>. */
    NIMRTC_API_STABLE bool srtp_installed() const noexcept;

    /** Current DTLS state, or Closed if DTLS not initialised.
     *  Out-of-line so the public header doesn't need <nimrtc/dtls/dtls.hpp>. */
    NIMRTC_API_STABLE dtls::DtlsState dtls_state() const noexcept;

    /** True iff DTLS has reached Connected state. */
    NIMRTC_API_STABLE bool dtls_connected() const noexcept;

    /** True iff the ICE agent has selected at least one candidate pair
     *  (Connected or Completed).  Used to gate outbound DTLS sends.
     *  Implementation now goes through the plugin seam
     *  (`plugins::IICETransport::state()`), not a dynamic_cast to the
     *  concrete module class. */
    NIMRTC_API_STABLE bool is_ice_connected() const noexcept;

    /** Last error code from open()/pre_open() — useful for diagnostics
     *  when the bool result is non-OK but we want to know which subsystem
     *  failed (e.g. 0x1FFF = ICE, 0x2000 = DTLS, 0x1A00 = audio3a).
     *  Out-of-line because last_open_rc_ lives in Impl. */
    NIMRTC_API_STABLE uint32_t last_open_rc() const noexcept;

    // --- ICE configuration (pre-open) -----------------------------------
    //
    // These configure the ICE transport before candidate gathering begins.
    // Marked STABLE so the loopback test and signaling proxy use-cases
    // (set_remote_ice / add_turn_server) have a stable contract.

    /** Set the remote ICE description (ice-ufrag / ice-pwd / candidates) BEFORE
     *  open() is called.  This is required for the loopback test: the answerer
     *  (controlled) must know the offerer's ICE credentials before its ICE
     *  transport opens, so libjuice sets the agent to CONTROLLED instead of
     *  CONTROLLING (avoiding the ICE role conflict).  The ice_block should be
     *  the raw SDP media section containing a=ice-ufrag, a=ice-pwd, and
     *  optionally a=candidate lines.  Can also be called after open() (applied
     *  immediately). */
    NIMRTC_API_STABLE bool set_remote_ice(std::string_view ice_block) noexcept;

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
    NIMRTC_API_STABLE int add_turn_server(std::string_view host, std::uint16_t port,
                        std::string_view username,
                        std::string_view password) noexcept;

    // --- ICE restart (RFC 8445 §9.1.1.1) ---------------------------------
    //
    // A restart re-runs the whole ICE session with a fresh credential pair
    // and re-gathers the candidates, which is what makes the session
    // survive a network change (WiFi -> 4G, VPN up/down, DHCP renewal).

    /** Perform an ICE restart (RFC 8445 §9.1.1.1).
     *
     *  The local ICE credentials are regenerated and the candidates are
     *  gathered again.  The peer can only observe the restart through the
     *  new credentials, so the host has to publish a fresh offer
     *  afterwards — `create_offer()` picks the new credentials up
     *  automatically:
     *
     *      engine.restart_ice();
     *      send_to_peer(engine.create_offer());
     *
     *  @return true if the restart was performed; false if no ICE transport
     *          exists yet (call pre_open() first) or if the restart failed.
     *
     *  Thread-safety: not thread-safe; call from the same thread that
     *  drives the signaling. */
    NIMRTC_API_STABLE bool restart_ice() noexcept;

    /** Number of ICE restarts performed on this engine's transport. */
    NIMRTC_API_STABLE std::uint32_t ice_restart_count() const noexcept;

    /** Install the callback fired when the *peer* restarts ICE.
     *
     *  The peer published new ICE credentials, so the engine regenerated its
     *  own (RFC 8445 §9.1.1.1 requires both sides to change).  The host has
     *  to answer with the new credentials — that is what this callback is
     *  for: call `process_remote_sdp()`-equivalent answer generation and
     *  send it to the peer.
     *
     *  Pass a null callback to clear. */
    NIMRTC_API_STABLE void set_on_ice_restart(plugins::OnIceRestart cb) noexcept;

    /** Inbound raw bytes — alternative entry point for test injection.
     *
     *  UNSTABLE: this is a test seam, not a general-purpose API.
     *  It bypasses the normal ICE transport recv() path and directly
     *  injects into the SRTP pipeline for loopback / harness testing. */
    NIMRTC_API_UNSTABLE uint32_t feed_srtp_inbound(const std::uint8_t* srtp_packet, std::size_t len) noexcept;

    /** Inbound *plaintext* RTP / RTCP bytes — test seam for the cross-end
     *  timeline (ADR-017).
     *
     *  Feeds `pkt` through the exact RTCP-demux → RTP-parse →
     *  `handle_rtcp()` / `handle_rtp()` tail that `on_transport_recv()`
     *  uses once the DTLS and SRTP layers have been peeled off. There is
     *  deliberately no separate implementation here: a test must exercise
     *  the production dispatch, not a parallel copy of it that can drift.
     *
     *  This is the only way to drive `handle_rtcp()` / `observe_timeline()`
     *  from a test. `feed_srtp_inbound()` cannot: it needs a completed DTLS
     *  handshake to derive the SRTP keys, and a shell-strategy engine has
     *  no peer to complete one against.
     *
     *  UNSTABLE: this is a test seam, not a general-purpose API. Feeding it
     *  cleartext on a real connection bypasses DTLS confidentiality. */
    NIMRTC_API_UNSTABLE void feed_media_inbound(const std::uint8_t* pkt, std::size_t len) noexcept;

    /** Drain packets from the ICE socket and dispatch them.  Returns the
     *  number of packets dispatched (RTP / DTLS / STUN). */
    NIMRTC_API_STABLE int tick() noexcept;

    // --- ICE state and credential inspection -----------------------------
    //
    // These expose ICE / DTLS diagnostics used by tests, demos, and the
    // signaling proxy.  Marked STABLE so the diagnostic surface is frozen.

    /** ICE state as a human-readable string. */
    NIMRTC_API_STABLE const char* ice_state_string() const noexcept;

    /** Local ICE ufrag (user fragment).  Embed in SDP. */
    NIMRTC_API_STABLE std::string local_ufrag()    const noexcept;
    /** Local ICE password.  Embed in SDP. */
    NIMRTC_API_STABLE std::string local_password() const noexcept;

    /** Local DTLS fingerprint advertised in our SDP, as a base64-encoded
     *  SHA-256 SPKI hash (the format Chrome's
     *  --ignore-certificate-errors-spki-list expects).  Returns an empty
     *  string if the DTLS session is not yet open or self-signed certs
     *  are disabled.  Computed lazily from the underlying DtlsSession. */
    NIMRTC_API_STABLE std::string local_dtls_fingerprint_sha256_base64() const noexcept;

    // --- Transport accessors (diagnostic / integration testing) ---------
    //
    // These expose the live transport instances to callers that need direct
    // access (signaling proxy, ICE candidate forwarding, test harness).
    // Marked STABLE so the non-owning handle identity is frozen.

    /** Underlying ITransport (ICE by default).  Useful for tests and demos that
     *  need direct access to the transport — e.g. for ICE candidate forwarding
     *  in the WebSocket signaling proxy.  Returns nullptr if not yet open().
     *
     *  The pointer is non-owning: the engine retains the only owner
     *  (the `media_stack` inside `Impl::session`). Callers must not
     *  delete it. */
    NIMRTC_API_STABLE plugins::ITransport* get_transport() noexcept { return ice_t_; }
    NIMRTC_API_STABLE const plugins::ITransport* get_transport() const noexcept { return ice_t_; }

    /** ICE-aware view of the transport.  Returns nullptr if no
     *  IICETransport plugin has been resolved (which only happens if the
     *  transport plugin chosen by `cfg.transport_name` does not implement
     *  the ICE-aware interface — the engine only treats ICE transports as
     *  ICE transports).
     *
     *  Non-owning; same lifetime as the engine. */
    NIMRTC_API_STABLE plugins::IICETransport* get_ice_transport() noexcept { return ice_t_; }
    NIMRTC_API_STABLE const plugins::IICETransport* get_ice_transport() const noexcept {
        return ice_t_;
    }

    /** Read-only view of the engine's config.  Useful for tools that need
     *  to know the PCM rate / channels (e.g. demo-p2p's audio generator)
     *  without making those fields public. */
    NIMRTC_API_STABLE const EngineConfig& config() const noexcept { return config_; }

    // ---- Cross-end timeline diagnostics (ADR-017) --------------------------
    //
    // These expose what the timeline seam reconstructed from the peer's
    // RTCP Sender Reports (+ abs-send-time refinement).  They are read-only
    // and safe to call from any thread after the engine is open.

    /** True when the cross-end timeline seam is enabled for this engine
     *  (mirrors `EngineConfig::timeline_enabled`). */
    NIMRTC_API_STABLE bool timeline_enabled() const noexcept {
        return config_.timeline_enabled;
    }

    /** The RTP header-extension id used for *outbound* abs-send-time
     *  elements.  0 means the extension was never negotiated (the peer
     *  never advertised it), in which case no abs-send-time is emitted. */
    NIMRTC_API_STABLE std::uint16_t abs_send_time_ext_id() const noexcept;

    /** The RTP header-extension id the peer advertised for its own
     *  abs-send-time elements — the id inbound packets are parsed with.
     *  0 = the peer never advertised the extension. */
    NIMRTC_API_STABLE std::uint16_t remote_abs_send_time_ext_id() const noexcept;

    /** Sender-wall-clock capture instant (microseconds since the Unix
     *  epoch) of the most recently received inbound frame, reconstructed
     *  from the peer's Sender Report.
     *
     *  Empty until the first usable Sender Report has been ingested.
     *  The value lives on the *peer's* wall clock, so it is directly
     *  comparable with a locally computed Unix timestamp (and with the
     *  peer's own timeline) — `last_one_way_delay_us()` is that
     *  comparison already made. */
    NIMRTC_API_STABLE std::optional<std::int64_t> last_capture_ts_us() const noexcept;

    /** Same as `last_capture_ts_us()` but for a specific inbound SSRC
     *  (audio and video streams have their own SSRCs). */
    NIMRTC_API_STABLE std::optional<std::int64_t>
        capture_ts_us_for_ssrc(std::uint32_t ssrc) const noexcept;

    /** One-way delay (microseconds) of the most recently received inbound
     *  frame: the local arrival instant **minus** the peer's capture
     *  instant, both on the Unix timeline, i.e. how long the packet took
     *  to get here.
     *
     *  Positive in the normal case, zero at the theoretical floor, and
     *  negative only when the two ends' clocks disagree — which is itself
     *  worth surfacing rather than clamping.
     *
     *  Empty until the first Sender Report has been ingested. */
    NIMRTC_API_STABLE std::optional<std::int64_t> last_one_way_delay_us() const noexcept;

    /** Inter-clock skew (microseconds) of the most recently received
     *  inbound frame — the *change*, since the peer's last Sender Report,
     *  of the offset between our clock and the peer's.
     *
     *  This is the quantity RFC 3550 §8.1 calls inter-clock skew: a
     *  *rate* difference between the two ends' clocks, which is what
     *  makes a long session's capture timestamps drift.  A peer whose
     *  wall clock runs fast shows up as a negative value.
     *
     *  The constant phase difference between the two clocks cancels out;
     *  what remains is the accumulated rate difference over the
     *  Sender-Report → packet interval, plus the difference in transit
     *  delay between the two packets.  A single sample is therefore
     *  noisy at the millisecond level — the *trend* over many packets is
     *  the clock skew.
     *
     *  Empty until the first Sender Report has been ingested.  Requires
     *  the engine's monotonic origin, which is established at `open()`,
     *  so the value is available as soon as a reconstruction exists. */
    NIMRTC_API_STABLE std::optional<std::int64_t>
        last_sender_recv_skew_us() const noexcept;

    /** Same as `last_sender_recv_skew_us()` but for a specific inbound
     *  SSRC (each stream carries its own clock pair). */
    NIMRTC_API_STABLE std::optional<std::int64_t>
        sender_recv_skew_us_for_ssrc(std::uint32_t ssrc) const noexcept;

    // ---- Transport strategy accessors (v0.12.0 Slice 7.6 PR3) -----------
    //
    // These expose the resolved (validated + Selector-applied) state of
    // `cfg.transport_strategy_name`.  Both are diagnostic / integration
    // testing surface; the engine does NOT use them to drive the actual
    // ICE / DTLS / SRTP pipeline (that continues to go through the
    // `cfg.transport_name` plugin id and the existing ICE transport).
    //
    // The PR3 step deliberately **records** the (media_factory_id,
    // control_factory_id) choice without changing the engine's media
    // stack instantiation path.  Slice 7.x 2nd-half work
    // (v0.13+; see `docs/plan/transport-selection.md` §6.5) will
    // promote these factory ids to drive the engine's `media_stack` /
    // `control_stack` selection, at which point the same accessor
    // pair will return the live factory ids actually in use.
    //
    // Returns empty strings before open() succeeds (the strategy is
    // resolved inside `init_modules_once()`).

    /** Strategy name that the engine resolved from `cfg.transport_strategy_name`
     *  (empty string when open() was never called or failed). */
    NIMRTC_API_STABLE std::string_view transport_strategy_name() const noexcept;

    /** Resolved (media_factory_id, control_factory_id) pair from the
     *  CapabilitySelector.  `control_factory_id` is empty when the
     *  strategy does not enable a side-stack (e.g. `browser-call`).
     *  Both fields are empty before open() succeeds. */
    struct NIMRTC_API_STABLE TransportSessionView {
        std::string_view media_factory_id;
        std::string_view control_factory_id;
    };
    NIMRTC_API_STABLE TransportSessionView transport_strategy_session() const noexcept;

    // ---- Video plugin accessors (R3-Batch) ----
    //
    // The engine resolves all 4 video plugins from core::PluginRegistry
    // at open() time, using the *_name fields in EngineConfig. The plugins
    // themselves live for the engine's lifetime (or until replaced via
    // set_video_sink / set_video_source).
    //
    // The engine DOES thread the send and receive video paths into the
    // RTP/DTLS/SRTP pipeline:
    //   send:    send_video() → video_codec_ (encode) → video_sender_
    //             (FU-A packetize) → RTP header → SRTP protect →
    //             scheduler_ → ICE.
    //   receive: ICE recv → handle_rtp (PT demux) → video_receiver_->push_rtp()
    //             (FU-A depacketize + jitter buffer) → video_codec_ (decode)
    //             → video_sink_ (render).
    // The set_frame_callback() on the receiver fires on the raw NAL level
    // (before decode).
    //
    // The video SOURCE is deliberately NOT auto-wired into the encode path:
    // IVideoCodec / IVideoSender are not thread-safe, and the source's
    // cadence thread would race with any concurrent caller.  The engine
    // therefore starts/stops the source (start_video() / stop_video()) but
    // the host is responsible for pulling frames and pushing them through
    // send_video().  A host that wants the source to drive the encoder must
    // wire video_source()->set_callback() itself and serialise the calls.
    //
    // Returns nullptr if the configured plugin id was not found at open().
    //
    // UNSTABLE: these are plugin-seam handles (ADR-001 §"Plugin seam");
    // the underlying plugin ABI is still evolving as video_pipeline/
    // video_payload/ move toward v1.0.0.

    NIMRTC_API_UNSTABLE plugins::IVideoSource*   video_source()   noexcept { return video_source_.get(); }
    NIMRTC_API_UNSTABLE plugins::IVideoSink*     video_sink()     noexcept { return video_sink_.get(); }
    NIMRTC_API_UNSTABLE plugins::IVideoReceiver* video_receiver() noexcept { return video_receiver_.get(); }
    NIMRTC_API_UNSTABLE plugins::IVideoSender*   video_sender()   noexcept { return video_sender_.get(); }
    NIMRTC_API_UNSTABLE const plugins::IVideoSource*   video_source()   const noexcept { return video_source_.get(); }
    NIMRTC_API_UNSTABLE const plugins::IVideoSink*     video_sink()     const noexcept { return video_sink_.get(); }
    NIMRTC_API_UNSTABLE const plugins::IVideoReceiver* video_receiver() const noexcept { return video_receiver_.get(); }
    NIMRTC_API_UNSTABLE const plugins::IVideoSender*   video_sender()   const noexcept { return video_sender_.get(); }

    /** Replace the video sink at runtime (e.g. swap "headless" for an SDL
     *  renderer). Closes the previous sink and transfers ownership of @p sink.
     *  Safe to call before or after open(); if called before open(), the
     *  sink configured via video_sink_name is replaced and the new one is
     *  used instead. Pass nullptr to detach (next render() calls are no-ops). */
    NIMRTC_API_STABLE void set_video_sink(std::unique_ptr<plugins::IVideoSink> sink) noexcept;

    /** Replace the video source at runtime. Same semantics as set_video_sink. */
    NIMRTC_API_STABLE void set_video_source(std::unique_ptr<plugins::IVideoSource> source) noexcept;

    // ---- BWE / Scheduler accessors (ADR-001: resolved via PluginRegistry) ----
    //
    // These plugins are created at open() time via
    // `core::PluginRegistry::instance().get_bwe()` and `get_scheduler()`.
    // The engine injects them into `ice_t_` via `set_bwe()` / `set_scheduler()`.
    // Callers can use the accessors to drive the plugins manually
    // (e.g. inject custom BWE feedback, inspect scheduler stats).
    //
    // Returns nullptr if the configured plugin id was not found at open().
    //
    // UNSTABLE: the BWE / scheduler / video-codec plugin ABI is still
    // evolving; these handles expose the live plugin state.

    NIMRTC_API_UNSTABLE plugins::IBwe*        bwe()        noexcept { return bwe_.get(); }
    NIMRTC_API_UNSTABLE const plugins::IBwe*  bwe()  const noexcept { return bwe_.get(); }

    NIMRTC_API_UNSTABLE plugins::IScheduler*  scheduler()  noexcept { return scheduler_.get(); }
    NIMRTC_API_UNSTABLE const plugins::IScheduler* scheduler() const noexcept { return scheduler_.get(); }

    // ---- Net feedback collector accessor (v1.2.0 M1, F2) -------------
    /** The `INetFeedback` collector instance (or nullptr when the
     *  collector was not created).  Call after open().  A host that
     *  wants the raw aggregated signals can read `snapshot()` here. */
    NIMRTC_API_UNSTABLE plugins::INetFeedback* net_feedback() noexcept { return net_feedback_.get(); }
    NIMRTC_API_UNSTABLE const plugins::INetFeedback* net_feedback() const noexcept { return net_feedback_.get(); }

    // ---- XR report consumer accessor (v1.2.0, session plan X1) ---------
    /** The `IXrReport` consumer instance (or nullptr when the consumer
     *  was not created).  Call after open().  A host that wants the
     *  decoded RFC 3611 report blocks reads `snapshot()` here. */
    NIMRTC_API_UNSTABLE plugins::IXrReport* xr_report() noexcept { return xr_report_.get(); }
    NIMRTC_API_UNSTABLE const plugins::IXrReport* xr_report() const noexcept { return xr_report_.get(); }

    // ---- RTCP XR / TMMBR observability (v1.2.0, session plan X2–X5) --
    //
    // The four accessors below are the public face of the outbound XR
    // and TMMBR planes.  They are counters and a limit, not a
    // configuration surface: the host switches the features on and off
    // through `EngineConfig`, and reads back what happened through
    // these.

    /** The tightest bit-rate limit any peer's TMMBR has asked the
     *  engine to honour, in bits per second.
     *
     *  RFC 5104 §4.2.1.2: a TMMBR is a receiver's statement of the
     *  maximum rate it supports, and the sender must obey it.  The
     *  engine clamps its scheduler budget and its pacing rates to this
     *  value, so the number here *is* the effective egress ceiling.
     *
     *  0 = no limit received (the peer's estimate governs). */
    NIMRTC_API_UNSTABLE std::uint32_t peer_tmmbr_limit_bps() const noexcept;

    /** The bit rate the last outbound TMMBR reported, in bits per
     *  second.
     *
     *  This is the bandwidth estimate the engine was *using* when it
     *  built the message (X4: the TMMBR must reflect the live BWE
     *  target, not a stale number).  0 = no TMMBR was sent yet. */
    NIMRTC_API_UNSTABLE std::uint32_t last_tmmbr_bps() const noexcept;

    /** The number of outbound TMMBR / TMMBN messages the engine sent. */
    NIMRTC_API_UNSTABLE std::uint64_t tmmbr_sent_count() const noexcept;

    /** The number of inbound TMMBR messages the engine honoured. */
    NIMRTC_API_UNSTABLE std::uint64_t tmmbr_received_count() const noexcept;

    /** The number of outbound RFC 3611 extended reports the engine
     *  sent (one per report, whatever the block count). */
    NIMRTC_API_UNSTABLE std::uint64_t xr_reports_sent_count() const noexcept;

    NIMRTC_API_UNSTABLE plugins::IVideoCodec* video_codec() noexcept { return video_codec_.get(); }
    NIMRTC_API_UNSTABLE const plugins::IVideoCodec* video_codec() const noexcept { return video_codec_.get(); }

    // ---- Audio3A plugin accessor (for PCM tap setup in demos/tests) ----
    /** Returns the IAudio3A plugin instance (or nullptr if not created).
     *  Call after open().  Used by demos to set up pre/post process taps. */
    NIMRTC_API_STABLE plugins::IAudio3A* audio3a() noexcept { return audio3a_plugin_.get(); }
    NIMRTC_API_STABLE const plugins::IAudio3A* audio3a() const noexcept { return audio3a_plugin_.get(); }

    // Engine stats for debugging
    struct NIMRTC_API_STABLE Stats {
        int srtp_drops = 0;
        // ---- Video counters (v1.1 V1, additive) -------------------------
        // All cumulative since open().  Wired to the already-existing
        // video pipeline hooks; no new threads, no new I/O.
        std::uint64_t video_frames_sent        = 0;  // send_video() frames
        std::uint64_t video_packets_sent       = 0;  // RTP packets emitted
        std::uint64_t video_bytes_sent         = 0;  // RTP bytes (hdr+payload)
        std::uint64_t video_frames_received    = 0;  // receiver frames
        std::uint64_t video_nack_seqs_queued   = 0;  // NACK seqs queued (out)
        std::uint64_t video_keyframes_q        = 0;  // keyframe reqs queued
        std::uint64_t video_feedback_applied   = 0;  // inbound RTCP actions
        std::uint64_t video_keyframes_applied  = 0;  // inbound keyframe reqs
        // ---- Audio counters (WP-5, additive) -------------------------
        // Cumulative since open().  `audio_encode_errors` is the
        // "never silently dropped" counter: every PCM block that could
        // not be encoded is counted here and reported through
        // on_error, so a host that feeds the engine at the wrong rate
        // sees an error instead of hearing silence.
        std::uint64_t audio_frames_sent      = 0;  // RTP packets emitted
        std::uint64_t audio_bytes_sent       = 0;  // RTP bytes (hdr+payload)
        std::uint64_t audio_encode_errors    = 0;  // frames that failed to encode
        std::uint64_t audio_samples_in       = 0;  // PCM samples handed to send_audio()
        std::uint64_t audio_samples_pending  = 0;  // samples still in the accumulator
        // ---- Audio receive counters (WP-8, spec B-13, additive) -------
        // All cumulative since open().  The send-side counters above
        // count what the engine put on the wire; these count what came
        // *in* and what the host was handed.  The values come from the
        // receive pipeline's own statistics (the pipeline owns the
        // detailed bookkeeping) plus the engine's inbound sequence
        // tracking, so a host sees the same numbers Chrome reports in
        // `inbound-rtp`.
        std::uint64_t audio_packets_sent        = 0;  // RTP packets emitted
        std::uint64_t audio_frames_received     = 0;  // decoded frames delivered
        std::uint64_t audio_samples_received    = 0;  // samples per channel delivered
        std::uint64_t audio_concealed_frames    = 0;  // PLC frames delivered
        std::uint64_t audio_fec_recovered_frames= 0;  // frames rebuilt from in-band FEC
        std::uint64_t audio_packets_lost        = 0;  // inbound packets inferred lost
        std::uint64_t audio_decode_errors       = 0;  // decoder errors
        std::uint32_t audio_jitter_ms           = 0;  // current interarrival jitter
        std::uint32_t audio_playout_delay_ms    = 0;  // current playout delay
        // ---- Adaptive audio bitrate (additive) -----------------------
        // The bitrate the outbound audio encoder currently runs at.
        // It equals `Config::audio_bitrate_bps` until the BWE-linked
        // adaptation lowers it (or raises it back towards that ceiling).
        std::uint32_t audio_encoder_bitrate_bps  = 0;
        // ---- Network feedback / BWE loop (v1.2.0 M1, additive) ---------
        // The counters that make the feedback loop observable: how many
        // inbound RTCP blocks were routed to the collector, how many
        // aggregated samples were handed to the bandwidth estimator, and
        // what the estimator currently says.  A host (or a test) that
        // sees `bwe_feedback_samples` stuck at zero knows the loop is
        // not closed.
        std::uint64_t net_feedback_blocks    = 0;  // RTCP blocks routed to the collector
        std::uint64_t bwe_feedback_samples  = 0;  // samples handed to IBwe::on_feedback()
        std::uint32_t bandwidth_estimate_bps = 0; // current BWE target, in bit/s
        // ---- DTMF counters (v1.2.0 session plan D1–D4, additive) ------
        // The telephone-event plane has its own counters because a
        // telephone-event payload is *not* audio: it never reaches the
        // audio pipeline, so the audio counters above say nothing about
        // it.  `dtmf_payload_type` is the negotiated payload type (RFC
        // 4733 §2.3.2); zero means the peer never negotiated one, which
        // is also the state in which a received telephone-event payload
        // counts as an unknown audio payload type (decision D4).
        std::uint64_t dtmf_packets_sent      = 0;  // outbound event packets
        std::uint64_t dtmf_packets_received  = 0;  // inbound event packets decoded
        std::uint64_t dtmf_packets_malformed = 0;  // inbound packets that failed to decode
        std::uint64_t dtmf_events_delivered  = 0;  // completed events reported to the host
        std::uint8_t  dtmf_payload_type      = 0;  // the negotiated telephone-event PT
        // ---- RTCP XR counters (v1.2.0 session plan X1, additive) --------
        // The number of inbound RTCP XR sub-packets (payload type 207)
        // that were routed to the XR consumer.  A peer that sends
        // extended reports and a counter stuck at zero means the routing
        // is not wired.
        std::uint64_t xr_report_blocks      = 0;  // XR blocks routed to the consumer
    };
    /** Out-of-line: depends on concrete SRTP/DTLS state stored in Impl.
     *
     *  UNSTABLE: the Stats struct fields may change before v1.0.0. */
    NIMRTC_API_UNSTABLE Stats stats() const noexcept;

    /** The negotiated remote audio format, once the remote SDP has been
     *  processed.
     *
     *  Returns false when no audio track is negotiated yet (or when the
     *  peer's audio m-line was rejected), in which case `out` is left
     *  untouched (spec E-4). */
    NIMRTC_API_STABLE bool remote_audio_format(AudioFormat* out) const noexcept;

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
    // `ice_t_` is a NON-OWNING handle to the ICE-aware transport plugin
    // living inside the media `ITransportStack` (Slice 7.6 PR3 / v0.12.0
    // refactor). The actual ownership moved into `Impl::session`
    // (a `TransportSession` with `media_stack` and `control_stack`),
    // which is constructed by `make_transport_instance()` in
    // `init_modules_once()` and torn down in `~NimRTCEngine()`.
    //
    // Why a raw pointer (not unique_ptr)?
    //   - The session owns the ITransportStack; the stack owns ICE /
    //     DTLS / RTP / SCTP. Two unique_ptrs (one on `ice_t_`, one
    //     inside the stack) would double-free on destruction.
    //   - The session is destroyed in `delete impl_` (which runs in
    //     `~NimRTCEngine` BEFORE the engine's own members are
    //     destroyed). After that point, `ice_t_` becomes dangling but
    //     no code dereferences it (close() is the last user and
    //     runs before `delete impl_`).
    //   - Storing the raw pointer is therefore safe AND keeps the
    //     call sites (`ice_t_->send(bv, {})`, `ice_t_->recv()`,
    //     `ice_t_->local_ufrag()`) unchanged.
    //
    // For pre-v0.12.0 readers: the previous `unique_ptr<IICETransport>`
    // was the engine's pre-PR3 storage. The refactor moves ownership
    // into the session but preserves the call-site contract.
    plugins::IICETransport*                     ice_t_ = nullptr;
    std::unique_ptr<plugins::IAudio3A>          audio3a_plugin_;
    std::unique_ptr<plugins::ICodec>            codec_plugin_;

    // ---- Audio receive pipeline (v1.2.0, audio pipeline WP-7) ----
    //
    // The engine drives the receive side of the audio path through this
    // one seam (spec B-11 / B-16): the concrete pipeline lives in
    // `src/modules/audio_rx/` and is reached through the registry's
    // audio-receiver slot, so the engine includes no concrete module
    // header (constraint L4).
    //
    // `nullptr` means "no audio receive pipeline" — either the host
    // disabled the audio track (`audio_receiver_name == ""`) or the
    // plugin could not be resolved (a hard error that was already
    // reported through `on_error_`).
    std::unique_ptr<plugins::IAudioReceiver>     audio_receiver_;

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

    // ---- Net feedback collector (v1.2.0 M1, F2) -------------------------
    //
    // Created in init_bwe_scheduler() (called from pre_open/open) from
    // the registry's net-feedback slot.  It owns the aggregation of the
    // receiver-side network signals; the engine owns the routing and
    // the hand-off to the estimator.
    std::unique_ptr<plugins::INetFeedback> net_feedback_;

    // ---- XR report consumer (v1.2.0, session plan X1) ------------------
    //
    // Created in init_bwe_scheduler() (called from pre_open/open) from
    // the registry's XR slot.  It owns the decoding of the peer's
    // RFC 3611 extended reports; the engine owns the routing of the
    // inbound RTCP XR sub-packets into it.
    std::unique_ptr<plugins::IXrReport> xr_report_;

    // ---- Adaptive audio bitrate (BWE-linked) ---------------------------
    //
    // The engine lowers the audio encoder's bitrate when the bandwidth
    // estimate collapses, and raises it back towards the configured
    // ceiling once the link recovers.  These three fields carry the
    // state of that controller:
    //
    //   - `audio_bitrate_current_bps_` — the bitrate the encoder runs
    //     at right now (the value handed to the codec plugin).
    //   - `audio_bitrate_ceiling_bps_` — the configured ceiling; the
    //     adaptation never goes above it.
    //   - `audio_bitrate_last_step_` — when the last adaptation step
    //     ran, so the controller rate-limits itself.
    int audio_bitrate_current_bps_ = 0;
    int audio_bitrate_ceiling_bps_ = 0;
    std::chrono::steady_clock::time_point audio_bitrate_last_step_{};

    // ---- Video codec plugin (ADR-001: resolved via PluginRegistry) ----
    std::unique_ptr<plugins::IVideoCodec> video_codec_;

    // ---- Public callbacks (no concrete deps) ----
    AudioFrameCallback  on_audio_frame_;
    AudioFrameInfoCallback on_audio_frame_info_;
    VideoFrameCallback  on_video_frame_;
    EngineErrorCallback on_error_;
    EngineStateCallback on_state_change_;

    /** Fired when the peer restarts ICE (RFC 8445 §9.1.1.1).
     *  Installed on the ICE transport when the transport is created; the
     *  transport calls it when the peer's description carries new ICE
     *  credentials.  The host has to answer with the new credentials. */
    plugins::OnIceRestart on_ice_restart_;

    // ---- DTMF (v1.2.0, session capability plan D1 / D2) ----------------
    //
    // The RFC 4733 codec is resolved from the registry's DTMF slot when
    // `config_.dtmf_enabled` is true; it is null when DTMF is off, when
    // the module is not built, or when the resolution failed.
    std::unique_ptr<plugins::IDtmfCodec> dtmf_codec_;

    /** The callback that receives decoded DTMF events. */
    DtmfEventCallback on_dtmf_;

    /** The telephone-event payload type the *peer* negotiated (RFC 4733
     *  §2.3.2).  Zero when the peer did not negotiate one -- which is
     *  the case when DTMF is off, and the case in which a
     *  telephone-event payload must still count as an unknown audio
     *  payload type (decision D4). */
    std::uint8_t negotiated_dtmf_pt_ = 0;

    /** The negotiated `a=fmtp:` event-code list of the telephone-event
     *  payload type (RFC 4733 §2.3.2) — the codes both ends support.
     *  Empty when no telephone-event payload type was negotiated. */
    std::string negotiated_dtmf_fmtp_;

    // ---- DataChannel callbacks ---------------------------------------------
    //
    // Stored on the engine so create_data_channel() can install them on
    // every new channel.  Empty std::function is fine — IDataChannel
    // treats an unset callback as a no-op (the plugin interface
    // explicitly allows `cb = nullptr` because it's a `std::function`).
    plugins::DataMessageCallback    on_data_message_;
    plugins::DataChannelStateCallback on_data_state_;
    // The identity-carrying data-message callback (v1.2.0, WP-C1).
    plugins::DataChannelMessageCallback on_data_message_ex_;
    // The next channel id the engine hands out (WP-C1): monotonically
    // increasing per engine, starting at 1.
    std::uint32_t next_data_channel_id_ = 1;

    // ---- DataChannel factory dispatch (v0.14.0-rc.1, DC-1 v1.0.0 headline) -
    //
    // Resolved at `init_modules_once()` time by dispatching on
    // `config_.datachannel_backend` ("web_transport" / "" + legacy
    // `datachannel_name` fallback; the "sctp" family was retired in
    // v1.2.0 and is rejected); the result is the factory id the engine
    // hands to `core::PluginRegistry::get_datachannel(id)` in
    // `create_data_channel()`.  When empty, DataChannel is disabled
    // (mirrors `cfg.datachannel_backend.empty() && cfg.datachannel_name.empty()`).
    //
    // Lives on the engine (not in `Impl`) because `create_data_channel()` is
    // a public API method that needs to read it directly, and `Impl` is a
    // pimpl hidden behind `engine.cpp`.
    std::string                        resolved_dc_factory_id_;

    enum class State { kConstructed, kOpen, kClosed };
    State state_ = State::kConstructed;

    // Out-of-line helpers whose definitions live in engine.cpp and pull in
    // the concrete module headers.  Public API keeps the header free of
    // those headers (Layout Invariant 4).
    void on_transport_recv(const plugins::BufferView& pkt) noexcept;
    /** The RTCP-demux → RTP-parse → dispatch tail shared by
     *  `on_transport_recv()` and the `feed_media_inbound()` test seam.
     *  `tried_srtp` only controls the "probably STUN" debug line. */
    void dispatch_media_inbound(plugins::BufferView view, bool tried_srtp) noexcept;
    void handle_rtp(const rtp::PacketView& pv) noexcept;
    /** The audio receive pipeline hands a decoded frame over (v1.2.0,
     *  audio pipeline WP-7).  The engine applies 3A render to it and
     *  forwards it to the legacy PCM tap. */
    void on_audio_frame_decoded(const plugins::AudioOutputFrame& frame) noexcept;
    /** The peer's capture instant of an inbound audio frame, in Unix
     *  microseconds (v1.2.0, audio pipeline WP-9, spec B-17).
     *
     *  Returns 0 when the cross-end timeline seam has not reconstructed
     *  the instant yet — no usable Sender Report / abs-send-time for
     *  this SSRC.  The value is **never** faked from the local playout
     *  clock: that would silently mix two different clocks (spec O-9).
     *
     *  @param ssrc          The inbound audio source.
     *  @param rtp_timestamp The RTP timestamp of the frame. */
    std::int64_t audio_capture_time_us(std::uint32_t ssrc,
                                       std::uint32_t rtp_timestamp) const noexcept;
    /** An inbound audio RTP packet arrived (v1.2.0, audio pipeline WP-8,
     *  spec B-12).
     *
     *  The RTCP receiver report needs the *sequence* bookkeeping — the
     *  extended highest sequence number and the cumulative packet loss
     *  (RFC 3550 §6.4.2) — and the receiver plugin does not track the
     *  sequence numbers, only the packet count.
     *
     *  @param arrival_us  The arrival instant on the engine's monotonic
     *                     clock; it feeds the interarrival-jitter
     *                     estimate (RFC 3550 §6.4.1). */
    void note_inbound_audio_packet(std::uint32_t ssrc,
                                   std::uint16_t seq,
                                   std::uint32_t rtp_timestamp,
                                   std::int64_t arrival_us) noexcept;
    /** Send the RTCP receiver report for the inbound audio stream
     *  (v1.2.0, audio pipeline WP-8, spec B-12).
     *
     *  Driven from `tick()`: a report goes out at least once per second
     *  while audio is inbound (RFC 3550 §6.4.2 recommends 5–10 s, but
     *  WebRTC sends far more often because the browser's `inbound-rtp`
     *  statistics depend on it). */
    void maybe_send_receiver_report() noexcept;
    /** Hand the decoded frame to the detailed callback (v1.2.0, audio
     *  pipeline WP-10, spec E-2).
     *
     *  Called by `on_audio_frame_decoded()` *after* the legacy PCM tap
     *  has fired, so a host that installed both callbacks sees the
     *  legacy one first. */
    void deliver_audio_frame_info(const plugins::AudioOutputFrame& frame) noexcept;
    void handle_rtcp(plugins::BufferView view) noexcept;
    /** Merge a receiver feedback report into the pending one.  The
     *  reports are put on the wire by `flush_video_feedback()` from
     *  `tick()` — the receiver raises them from inside the ICE receive
     *  path, and sending from there would re-enter the transport. */
    void queue_video_feedback(const plugins::VideoFeedbackReport& report) noexcept;
    /** Encode the pending receiver feedback as RTCP and put it on the wire
     *  (v1.1 Q3).  The bytes are produced by the receiver plugin — the
     *  engine only protects them (SRTCP) and sends them. */
    void flush_video_feedback() noexcept;
    void observe_timeline(const rtp::PacketView& pv) noexcept;
    void maybe_send_sender_report() noexcept;
    // ---- RTCP XR / TMMBR (v1.2.0, session capability plan X2–X5) ------
    //
    // The declarations use *primitive* types only (no `rtp::` structs):
    // `engine.hpp` must not include a concrete module header (Layout
    // Invariant 4).  The definitions live in `engine_rtcp_xr.cpp`, which
    // is the one engine TU that links the RFC 3611 / RFC 5104 parsers.
    /** Send the RFC 3611 extended report — the Receiver Reference Time
     *  block (BT = 4) plus, when the peer's own reference time report
     *  has been received, the DLRR block (BT = 5) that answers it.
     *
     *  Driven from `tick()`; the report rides the receiver-report
     *  cadence (RFC 3611 §5.2). */
    void maybe_send_extended_report() noexcept;
    /** Send the RFC 5104 TMMBR — the "you may not send me more than
     *  this" request, carrying the live bandwidth estimate.
     *
     *  Gated by `EngineConfig::tmmbr_enabled` and vetoed by
     *  `EngineConfig::disable_tmmbr`. */
    void maybe_send_tmmbr() noexcept;
    /** Send the RFC 5104 TMMBN — the answer to an inbound TMMBR.
     *
     *  Exactly one TMMBN is sent per batch of inbound TMMBRs
     *  (RFC 5104 §4.2.2.2), and it is sent even when the peer's
     *  tuple was *not* accepted into the bounding set — that is what
     *  lets a participant that missed the last TMMBN recover. */
    void flush_tmmbn() noexcept;
    /** Handle an inbound TMMBR (RFC 5104 §4.2.1.2): clamp the outbound
     *  rate to the peer's limit and queue the TMMBN answer. */
    void on_tmmbr_received(std::uint32_t sender_ssrc,
                           std::uint32_t media_ssrc,
                           std::uint32_t bitrate_bps) noexcept;
    /** Parse an inbound TMMBR sub-packet and hand its entries to
     *  `on_tmmbr_received()`.  A malformed packet is counted and
     *  dropped — never an error on the read path. */
    void handle_inbound_tmmbr(core::ByteSpan packet) noexcept;
    /** Remember the peer's Receiver Reference Time report (RFC 3611
     *  §4.4) so the engine's next DLRR can answer it (§4.5). */
    void note_peer_reference_time(core::ByteSpan packet) noexcept;
    /** Put one RTCP packet on the wire.
     *
     *  RTCP leaves the engine as SRTCP (RFC 3711 §3.4) once SRTP is
     *  keyed — a *different* crypto context from SRTP, so
     *  `protect_rtp()` would produce a packet the peer's
     *  `unprotect_rtcp()` rejects.
     *
     *  @param ssrc  The SSRC the packet is sent under: the engine's own
     *               RTCP SSRC for the reports it makes (RFC 3550
     *               §6.1), the SSRC of the feedback sender for a
     *               feedback message (RFC 4585 §6.1).
     *  @param bytes The cleartext RTCP packet. */
    bool send_rtcp_packet(std::uint32_t ssrc,
                          const core::ByteBuffer& bytes) noexcept;
    /** The SSRC the engine's RTCP reports are sent under.
     *
     *  RFC 3550 §6.1: the reports identify their *originator*, which is
     *  the party making the statement — not the media SSRC of the
     *  stream the report is about.  The value is drawn once and then
     *  reused, so the receiver report, the extended report and the
     *  feedback messages all come from the same reporter. */
    std::uint32_t rtcp_reporter_ssrc() noexcept;
    void poll_ice_flap_for_rehandshake() noexcept;
    void init_bwe_scheduler() noexcept;
    void shutdown_bwe_scheduler() noexcept;
    /** Hand the aggregated network-feedback snapshot to the bandwidth
     *  estimator (v1.2.0 M1, F6).  Called from `tick()`; it feeds the
     *  estimator only when the collector has a *new* snapshot, so the
     *  estimator sees one sample per feedback report rather than one per
     *  engine tick. */
    void feed_bwe_from_net_feedback() noexcept;
    /** True when the host configured a transport-cc-capable collector and
     *  did not pin the signal policy to the loss/RTT path — i.e. when the
     *  offer should advertise `a=rtcp-fb:<pt> transport-cc`
     *  (ADR-018 Decision 6). */
    bool wants_transport_cc() const noexcept;
    /** BWE-linked audio bitrate adaptation — see
     *  `Config::audio_bitrate_adaptive`.  Called from `tick()`. */
    void maybe_update_audio_bitrate() noexcept;
    /** Drive the scheduler's per-class pacing buckets from the bandwidth
     *  estimate (v1.2.0 M3, video plan P2).  Called from `tick()`.
     *
     *  The estimate is split between the streams with the *same* rule the
     *  audio bitrate adaptation uses (`bandwidth_allocation.hpp`, F8):
     *  the audio is the reserved stream, the video is the elastic one.
     *  Each class is then paced at its own share, so a saturated video
     *  stream cannot starve the audio. */
    void update_pacing_rates() noexcept;
    /** The configured audio encoder ceiling, in bits per second:
     *  `Config::audio_bitrate_bps` when set, otherwise the codec
     *  default for the channel count (32 kbps mono / 64 kbps stereo). */
    int resolve_audio_bitrate_ceiling() const noexcept;
    void init_video_plugins() noexcept;
    void shutdown_video_plugins() noexcept;

    // ---- Session model helpers (v1.2.0, session capability plan) --------
    //
    // `resolve_session_model()` resolves the pluggable session model
    // from the registry's session slot (`EngineConfig::session_name`).
    // `ensure_base_session_state()` creates the base transceivers and
    // the tracks that ride on them — it is idempotent and only ever
    // adds what is missing, so it can run from `create_offer()` and
    // again after `open()` resolved the video / datachannel plugins.
    bool resolve_session_model() noexcept;
    void ensure_base_session_state() noexcept;
    /** Recompute the direction of a transceiver from the tracks that
     *  ride on it (N4).  A transceiver with no attached track is
     *  `inactive` — that is how a removed track turns its m-line
     *  `inactive` (OD-A). */
    void sync_transceiver_direction(std::string_view mid) noexcept;
    /** Mirror the peer's m-lines into the session model (T2 / T3).
     *
     *  Called when the remote description is an *offer*: every media
     *  section the peer sent gets a transceiver with the same mid, so
     *  the two ends agree on the mid ↔ m-line mapping and a later
     *  renegotiation keeps the mids stable. */
    void mirror_remote_media(const sdp::SessionDescription& remote) noexcept;
    /** Bind a track to a transceiver: an existing transceiver of the
     *  same kind with a free send slot, or a fresh one.  Returns the
     *  mid of the transceiver the track was bound to (empty on
     *  failure). */
    std::string bind_track_to_transceiver(std::string_view track_id,
                                          plugins::MediaKind kind);

    // ---- init_modules_once() phase helpers (P0 refactor) -----------------
    //
    // `init_modules_once()` used to be a single ~460-line procedure that
    // mixed transport-strategy validation, TransportSession construction,
    // ICE pre-open configuration, BWE/Scheduler injection, Audio3A
    // plugin creation, DataChannel factory verification, Codec plugin
    // creation, SRTP context creation, DTLS handshake, and ICE
    // callback wiring.  The function grew organically as Slice 7.6 PR3
    // (v0.12.0) replaced the manual ICE+DTLS construction path with the
    // composition-root flow; reading it required 4 scroll-up cycles to
    // remember which phase you were in.
    //
    // The 5 phase helpers below give each step a single named entry
    // point so:
    //   - failures land in one place (a helper returns an engine error
    //     code; the orchestrator forwards it to `init_modules_once()`'s
    //     caller);
    //   - the orchestrator itself is a 6-line top-down read;
    //   - future per-phase instrumentation (timing logs, e2e hooks,
    //     per-strategy validators) can be added in one focused spot.
    //
    // Phase ordering is preserved bit-for-bit with the pre-refactor
    // function — the orchestrator's call sequence matches the
    // pre-refactor inline order exactly.  Behaviour is unchanged.
    //
    // @see init_modules_once() for the orchestration contract.

    /** Phase 1 — validate `cfg.transport_strategy_name` and apply the
     *  CapabilitySelector.  Stores the resolved strategy name + the
     *  Selector-derived (media_factory_id, control_factory_id) pair on
     *  `impl_`.  Returns `kEngineInvalidParam` (0x1001) for unknown
     *  strategy names, or `kEngineInternal` for defensive failures. */
    uint32_t init_transport_strategy() noexcept;

    /** Phase 2 — build the `TransportSession` from the Selector-resolved
     *  factory ids (media + optional control), apply ICE pre-open config
     *  (bind address / STUN / local port range), and extract the
     *  non-owning `ice_t_` / `impl_->dtls` handles.  Returns
     *  `kEngineInternal` when a required factory is missing, returns
     *  null, or when DTLS-required-but-missing contract fails. */
    uint32_t init_transport_session() noexcept;

    /** Phase 4 — create the Audio3A plugin (with `NullAudio3A` fallback)
     *  and the Codec plugin (with static Opus fallback when
     *  `NIMRTC_HAS_OPUS`).  Both go through `pal::make_plugin()` with
     *  their pre-refactor failure modes.  No return value — failures
     *  surface through `on_error_` and the `unique_ptr` slot is reset
     *  to `nullptr` so the fallback path runs. */
    void init_audio_modules() noexcept;

    /** Phase 5 — resolve the DataChannel factory id from
     *  `cfg.datachannel_backend` (+ the legacy `cfg.datachannel_name`
     *  fallback) and verify the factory resolves via
     *  `core::PluginRegistry`.
     *  No construction here — `create_data_channel()` is the
     *  on-demand API.  A missing factory only logs a warning and
     *  `create_data_channel()` will return `nullptr` on the caller
     *  side, so those paths return 0.
     *
     *  Returns `kEngineInternal` when `cfg.datachannel_backend` holds an
     *  unrecognised value — or the retired `"sctp"` value (v1.2.0, WP-A):
     *  the engine does not silently fall back to another backend (a typo'd
     *  backend id would otherwise mask an integrator error, and the SCTP
     *  backend is a test-only loopback socket that cannot reach a peer).
     *  The error is also routed through `on_error_` before the
     *  return, matching the other `init_*` failure paths. */
    uint32_t init_datachannel_registry() noexcept;

    // ---- DTMF (v1.2.0, session capability plan D1–D4) ----------------
    //
    // The two private helpers of the DTMF plane.  They live in
    // `src/engine/src/engine_dtmf.cpp`; they are declared here so the
    // other engine TUs (the SDP builder and the inbound RTP demux)
    // can reach them.

    /** Resolve the RFC 4733 codec from the registry's DTMF slot.
     *
     *  A no-op when `config_.dtmf_enabled` is false (DTMF is off by
     *  default, decision OD-B) or when the plugin is not registered —
     *  a missing codec degrades the DTMF path, it does not fail the
     *  engine. */
    void resolve_dtmf_codec() noexcept;

    /** Handle one inbound telephone-event payload.
     *
     *  Decodes the payload, accumulates the event duration across the
     *  repeats RFC 4733 §2.5.1.2 describes, and reports the completed
     *  event through `on_dtmf_`.
     *
     *  @param ssrc     The SSRC of the stream the payload arrived on
     *                  (the accumulator is per-SSRC).
     *  @param payload  The four-byte telephone-event header. */
    void handle_dtmf_payload(std::uint32_t ssrc,
                             core::ByteSpan payload) noexcept;

    /** Emit the queued DTMF events (RFC 4733 §2.5).
     *
     *  The tick loop calls this; it emits the repeats of the event in
     *  progress and closes it with the end-of-event bit.  A no-op when
     *  nothing is queued. */
    void pump_dtmf_tx() noexcept;

    /** Build, protect and enqueue one telephone-event RTP packet.
     *
     *  @param code        The RFC 4733 event code.
     *  @param duration_ms  The duration the packet reports.
     *  @param end          True for the packet that closes the event.
     *  @param marker       True for the first packet of the event
     *                      (RFC 4733 §2.2.2).
     *  @return false when the packet could not be built or sent. */
    bool emit_dtmf_packet(std::uint8_t code, std::uint16_t duration_ms,
                          bool end, bool marker) noexcept;

    /** Phase 6 — create the SRTP context, open the DTLS session
     *  (skipping Rule 2 raw-UDP-ARQ strategies where `impl_->dtls`
     *  is null by design), and wire the ICE recv / error callbacks.
     *  Returns `kDtlsOpenFailed` when `dtls_->open()` reports a
     *  failure, or `kEngineInternal` when the media stack requires
     *  DTLS (webrtc-classic) but the user-supplied `cfg.dtls_name`
     *  did not resolve to a registered factory. */
    uint32_t init_dtls_handshake() noexcept;

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
