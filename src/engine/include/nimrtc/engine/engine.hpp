/**
 * @file nimrtc/engine/engine.hpp
 * @brief NimRTCEngine — top-level composable engine that wires modules.
 *
 * P1 implementation:
 *   - Uses PluginRegistry to create ICE transport + SDP + RTP + JB + Audio3A
 *     plugin instances at open() time (ADR-001 wiring).
 *   - Generates RFC 8829-conformant SDP offers and parses answers.
 *   - Drives the wire protocol on tick() / drain callbacks.
 *   - Wires DTLS handshake + SRTP encryption + Opus codec.
 *
 * ## Plugin wiring (ADR-001)
 *
 * Plugins are selected by string ID via EngineConfig fields:
 *   cfg.transport_name = "ice";
 *   cfg.rtp_name      = "webrtc";
 *   cfg.sdp_name      = "webrtc";
 *   cfg.jb_name       = "adaptive";
 *   cfg.audio3a_name  = "webrtc";
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

#include <nimrtc/core/registry.hpp>     // PluginRegistry (Layout Invariant 6)
#include <nimrtc/plugins/transport.hpp>
#include <nimrtc/plugins/audio3a.hpp>

// Concrete module headers still required (until P1.1 plugin adapters land).
#include <nimrtc/ice/ice.hpp>
#include <nimrtc/sdp/session_description.hpp>
#include <nimrtc/rtp/packet.hpp>
#include <nimrtc/jb/jitter_buffer.hpp>
#include <nimrtc/audio3a/audio3a.hpp>
#include <nimrtc/dtls/dtls.hpp>
#include <nimrtc/srtp/srtp.hpp>
#include <nimrtc/opus/opus.hpp>

namespace nimrtc::engine {

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

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

    // Callbacks
    void set_on_audio_frame(AudioFrameCallback cb) noexcept { on_audio_frame_ = std::move(cb); }
    void set_on_video_frame(VideoFrameCallback cb) noexcept { on_video_frame_ = std::move(cb); }
    void set_on_error(EngineErrorCallback cb) noexcept     { on_error_       = std::move(cb); }
    void set_on_state_change(EngineStateCallback cb) noexcept { on_state_change_ = std::move(cb); }

    // SDP — RFC 8829 compliant
    std::string create_offer() noexcept;
    std::optional<std::string> process_remote_sdp(std::string_view remote_sdp) noexcept;

    // Media I/O — Opus encode + RTP packetise + SRTP encrypt + transport.send.
    uint32_t send_audio(const float* pcm_samples, size_t num_samples) noexcept;

    /** Drive DTLS state machine: feed pending outbound DTLS records back
     *  through ICE.  Returns number of records sent. */
    int drain_dtls() noexcept;

    /** Once DTLS connected, pull SRTP keying material and install on the
     *  SrtpContext.  Idempotent. */
    void maybe_install_srtp_keys() noexcept;

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

    // Engine stats for debugging
    struct Stats {
        int srtp_drops = 0;
    };
    Stats stats() const noexcept { return {srtp_stats_drop_}; }

private:
    EngineConfig                                config_;

    // ---- Plugin instances (created via PluginRegistry in open()) ----
    std::unique_ptr<plugins::ITransport>        transport_;   // wraps IceTransport
    std::unique_ptr<plugins::IAudio3A>          audio3a_plugin_;  // R2.5: prefer over concrete

    // ---- Non-plugin concrete modules ----
    // SDP: Parser/Munger are concrete classes (session module not yet scaffolded).
    // We hold raw pointers to them via SdpImpl; the plugin interface ISDP wraps these.
    // TODO(P1.1): Replace with session module once scaffolded.
    struct SdpImpl;
    std::unique_ptr<SdpImpl>                    sdp_impl_;

    // Jitter buffers — keyed by SSRC. The concrete jb::JitterBuffer is
    // used directly (IJB interface is used only for the plugin factory path).
    // TODO(P1): Expose IJB via assembly module.
    std::map<std::uint32_t, std::unique_ptr<jb::JitterBuffer>> jitter_buffers_;

    // ---- Concrete fallback classes (when no plugin adapter is registered) ----
    std::unique_ptr<audio3a::IAudio3A>          audio3a_concrete_;

    // ---- SRTP / DTLS / Opus — not yet plugin-exposed ----
    std::unique_ptr<dtls::DtlsSession>          dtls_;
    std::unique_ptr<srtp::SrtpContext>          srtp_;
    std::unique_ptr<opus::Encoder>              opus_encoder_;
    std::unique_ptr<opus::Decoder>              opus_decoder_;

    bool                                        dtls_active_inbound_ = false;
    bool                                        srtp_installed_ = false;
    int                                         srtp_stats_drop_ = 0;

    // Local SDP (after create_offer / process_remote_sdp)
    std::optional<sdp::SessionDescription>      local_sdp_;

    AudioFrameCallback  on_audio_frame_;
    VideoFrameCallback  on_video_frame_;
    EngineErrorCallback on_error_;
    EngineStateCallback on_state_change_;

    enum class State { kConstructed, kOpen, kClosed };
    State state_ = State::kConstructed;

    void on_transport_recv(const plugins::BufferView& pkt) noexcept;
    void handle_rtp(const rtp::PacketView& pv) noexcept;

    static void default_on_error(uint32_t err, std::string_view msg) noexcept;
};

} // namespace nimrtc::engine

#endif // NIMRTC_ENGINE_ENGINE_HPP
