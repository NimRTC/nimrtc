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

#include <cstdint>
#include <functional>
#include <memory>
#include <nimrtc/core/api_version.hpp>
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
#include <nimrtc/plugins/datachannel.hpp>

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
    struct NIMRTC_API_STABLE VideoReceiverTuning {
        std::uint32_t ssrc                = 0xDEADBEEF;
        std::uint8_t  payload_type        = 102;
        std::uint32_t max_inflight_frames = 8;
        std::uint32_t max_jitter_buffer_ms = 200;
        bool          emit_nacks          = true;
        bool          expect_fu_a         = true;
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
    // Default = "sctp" (matches the usrsctp-backed SctpDataChannelFactory
    // id registered at static-init time).  Empty disables DataChannel
    // entirely:
    //   - create_offer() does NOT emit m=application
    //   - process_remote_sdp() silently drops any m=application from the peer
    //   - create_data_channel() returns nullptr
    //
    // See `src/plugins/include/nimrtc/plugins/datachannel.hpp` for the
    // IDataChannel / IDataChannelFactory contract.

    /** IDataChannel plugin id. Default = "sctp". Empty = no DataChannel.
     *  This is the **low-level** factory-slot selector: the engine resolves
     *  `core::PluginRegistry::get_datachannel(datachannel_name)` at
     *  `init_modules_once()` time and uses the result for
     *  `create_data_channel()`.
     *
     *  Setting this to `""` disables DataChannel entirely (in every backend
     *  family, including `"sctp"` — this is the v0.x contract).
     *
     *  Most callers should prefer `datachannel_backend` (below); `datachannel_name`
     *  is retained for power users that want to inject a custom factory
     *  into the engine (e.g. an in-tree `MockDataChannelFactory` for tests).
     *  When `datachannel_backend` is empty, `datachannel_name` is the
     *  effective selector; otherwise `datachannel_backend` wins.
     */
    std::string datachannel_name = "sctp";

    /** DataChannel backend family selector (v0.14.0-rc.1, DC-1 v1.0.0
     *  headline gate).  The engine resolves this at `init_modules_once()`
     *  time and picks the matching `IDataChannelFactory` + (when needed)
     *  `IWebTransportSessionFactory` from `core::PluginRegistry`.
     *
     *  Valid values:
     *    - `"sctp"` (default) — SCTP-over-DTLS path (legacy).  Uses the
     *      `"sctp"` DataChannel factory; `datachannel_name` may override.
     *    - `"web_transport"`  — WebTransport / QUIC path (v1.0.0 production).
     *      Uses the `"web_transport"` DataChannel factory + the
     *      `web_transport_name` (default `"msquic"`) WebTransport session
     *      factory.  `datachannel_name` is ignored in this mode.
     *    - `""`               — DataChannel disabled (same as
     *      `datachannel_name = ""`); falls back to `datachannel_name` if
     *      non-empty (backward compatibility for v0.x callers that set
     *      `datachannel_name` without `datachannel_backend`).
     *
     *  Unknown values surface as `kEngineInternal` via `on_error_` at
     *  `open()` time — the engine does **not** silently fall back to
     *  `"sctp"` (typo'd backend ids would mask integrator errors).
     *
     *  Added in v0.14.0-rc.1 per `docs/plan/v1.0-plan.md` §2.1.1 PR-3
     *  (DC-1-LAND dependency) and the v0.14.0-beta.1 release notes
     *  "What is NOT in v0.14.0-beta.1" deferred-list entry for
     *  `EngineConfig::datachannel_backend`. */
    std::string datachannel_backend = "sctp";

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

    /** SCTP port advertised in `m=application` SDP lines (RFC 8831 §6.1).
     *  Default = 5000 (matches Chrome / WebRTC interop).  Only used when
     *  `datachannel_name` is non-empty. */
    std::uint16_t sctp_port = 5000;

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
};
// ---------------------------------------------------------------------------
// Public callbacks
// ---------------------------------------------------------------------------

using NIMRTC_API_STABLE AudioFrameCallback = std::function<void(const float* pcm, size_t num_samples)>;
using NIMRTC_API_STABLE VideoFrameCallback = std::function<void(const uint8_t* nal, size_t len, bool keyframe)>;
using NIMRTC_API_STABLE EngineErrorCallback = std::function<void(uint32_t err, std::string_view msg)>;
using NIMRTC_API_STABLE EngineStateCallback = std::function<void(const char* state)>;

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
    NIMRTC_API_STABLE void set_on_data_message(plugins::DataMessageCallback cb) noexcept {
        on_data_message_ = std::move(cb);
    }
    NIMRTC_API_STABLE void set_on_data_state(plugins::DataChannelStateCallback cb) noexcept {
        on_data_state_ = std::move(cb);
    }

    // ---- DataChannel creation --------------------------------------------
    //
    // Creates a new data channel by calling
    // `IDataChannelFactory::create()` on the factory resolved via
    // `core::PluginRegistry::instance().get_datachannel(datachannel_name)`.
    //
    // Returns nullptr when:
    //   - `cfg.datachannel_name.empty()` (DataChannel disabled)
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

    // SDP — RFC 8829 compliant -----------------------------------------
    //
    // `create_offer()` / `process_remote_sdp()` are the two engine
    // entry points every P2P / interop demo exercises.  Marked STABLE;
    // the SDP serialisation is documented in `docs/plan/sdp-grammar.md`
    // and consumed by Chrome / libjuice / libwebrtc peers.
    NIMRTC_API_STABLE std::string create_offer() noexcept;
    NIMRTC_API_STABLE std::optional<std::string> process_remote_sdp(std::string_view remote_sdp) noexcept;

    // ---- Audio I/O --------------------------------------------------------
    //
    // Opus encode + RTP packetise + SRTP encrypt + transport.send.
    // STABLE: the audio I/O surface is the load-bearing P2 contract.
    NIMRTC_API_STABLE uint32_t send_audio(const float* pcm_samples, size_t num_samples) noexcept;

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
    //
    // UNSTABLE: the video API is still P1.1 / R3 scope; the surface
    // may shift in a v0.15.x follow-up before v1.0.0 final.  Per
    // ADR-015 §Decision 2, marking UNSTABLE gives the engine room to
    // refactor the FU-A depacketise path / packetiser FU-A helpers /
    // NACK path in `video_payload/` without bumping the MAJOR version.

    /** Start the video capture source (produces frames at the configured fps).
     *  Idempotent if already running.  Returns kOk on success. */
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

    /** Inbound raw bytes — alternative entry point for test injection.
     *
     *  UNSTABLE: this is a test seam, not a general-purpose API.
     *  It bypasses the normal ICE transport recv() path and directly
     *  injects into the SRTP pipeline for loopback / harness testing. */
    NIMRTC_API_UNSTABLE uint32_t feed_srtp_inbound(const std::uint8_t* srtp_packet, std::size_t len) noexcept;

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
    // The engine does NOT thread these into the RTP/DTLS/SRTP pipeline
    // yet (P1.1 future work). Callers can:
    //   - drive video_source_->start() / produce_one() to feed a sender;
    //   - push encoded frames via video_sender_->push_frame();
    //   - receive encoded frames via video_receiver_->push_rtp() and
    //     listen on set_frame_callback();
    //   - render decoded frames via video_sink_->render().
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
    };
    /** Out-of-line: depends on concrete SRTP/DTLS state stored in Impl.
     *
     *  UNSTABLE: the Stats struct fields may change before v1.0.0. */
    NIMRTC_API_UNSTABLE Stats stats() const noexcept;

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

    // ---- DataChannel callbacks ---------------------------------------------
    //
    // Stored on the engine so create_data_channel() can install them on
    // every new channel.  Empty std::function is fine — IDataChannel
    // treats an unset callback as a no-op (the plugin interface
    // explicitly allows `cb = nullptr` because it's a `std::function`).
    plugins::DataMessageCallback    on_data_message_;
    plugins::DataChannelStateCallback on_data_state_;

    // ---- DataChannel factory dispatch (v0.14.0-rc.1, DC-1 v1.0.0 headline) -
    //
    // Resolved at `init_modules_once()` time by dispatching on
    // `config_.datachannel_backend` ("sctp" / "web_transport" / "" + legacy
    // `datachannel_name` fallback); the result is the factory id the engine
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
    void handle_rtp(const rtp::PacketView& pv) noexcept;
    void init_bwe_scheduler() noexcept;
    void shutdown_bwe_scheduler() noexcept;
    void init_video_plugins() noexcept;
    void shutdown_video_plugins() noexcept;

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
     *  unrecognised value: the engine does not silently fall back to
     *  `"sctp"` (a typo'd backend id would otherwise mask an integrator
     *  error).  The error is also routed through `on_error_` before the
     *  return, matching the other `init_*` failure paths. */
    uint32_t init_datachannel_registry() noexcept;

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
