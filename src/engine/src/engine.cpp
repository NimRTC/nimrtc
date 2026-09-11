/**
 * @file src/engine/src/engine.cpp
 * @brief NimRTCEngine — wires ICE + SDP + RTP + JB + Audio3A + DTLS + SRTP + Opus.
 *
 * P1.1 implementation:
 *   - open(): create ICE transport via PluginRegistry (id="ice" → IceTransport),
 *     SDP parser/munger, Audio3A, Opus codec, DTLS session, SRTP context.
 *   - create_offer: emit RFC 8829-compliant audio-only offer with Opus payload,
 *     BUNDLE, rtcp-mux, ICE credentials, candidates, DTLS fingerprint.
 *   - process_remote_sdp: parse remote SDP, apply ICE credentials + candidates,
 *     configure DTLS role + peer fingerprint, emit answer.
 *   - tick: drain transport.recv() + drain_dtls(); demux by first byte.
 *   - send_audio: 3A → Opus encode → RTP packetise → SRTP encrypt → ICE.send.
 *
 * ICE-specific methods (state(), local_ufrag(), wait_for_gathering(),
 * gathered_local_candidates(), set_remote_description(), etc.) are accessed
 * through `plugins::IICETransport*` — the engine stores the resolved
 * IICETransport as `ice_t_` and calls through that virtual interface; no
 * `dynamic_cast<ice::IceTransport*>` happens at the engine layer (the only
 * remaining cast is a one-shot safety-net inside open()/pre_open() that
 * lets a generic ITransport plugin still satisfy the ICE-aware surface if
 * the user chose not to register an IICETransportFactory).
 */

#include "nimrtc/engine/engine.hpp"

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/time.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/core/engine_errors.hpp>   // P1#9 — module error-code constants
#include <nimrtc/ice/ice.hpp>
#include <nimrtc/plugins/transport.hpp>
#include <nimrtc/plugins/audio3a.hpp>
#include <nimrtc/plugins/bwe.hpp>
#include <nimrtc/plugins/scheduler.hpp>
#ifdef NIMRTC_HAS_H264
#include <nimrtc/h264/codec_plugin.hpp>
#endif

// Concrete module headers — required by NimRTCEngine::Impl which lives in
// this translation unit.  The public header (engine.hpp) does NOT include
// any of these; consumers only need the plugin interfaces.
#include <nimrtc/sdp/session_description.hpp>
#include <nimrtc/rtp/packet.hpp>
#include <nimrtc/jb/jitter_buffer.hpp>
#include <nimrtc/audio3a/audio3a.hpp>

// Choose DTLS implementation.
// Set NIMRTC_USE_WOLFSSL_DTLS to 1 in CMakeLists.txt to switch from the
// hand-written dtls.cpp to the wolfSSL-backed version.
#ifdef NIMRTC_USE_WOLFSSL_DTLS
#include <nimrtc/dtls/dtls_wolfssl_session.hpp>
using DtlsSessionImpl = nimrtc::dtls::DtlsSessionWolfSSL;
#else
#include <nimrtc/dtls/dtls.hpp>
using DtlsSessionImpl = nimrtc::dtls::DtlsSession;
#endif
#include <nimrtc/srtp/srtp.hpp>
#ifdef NIMRTC_HAS_OPUS
#include <nimrtc/opus/opus.hpp>
#endif

namespace nimrtc::engine {

namespace {

void trace(const char* fmt, ...) noexcept {
#if defined(_DEBUG) || defined(NIMRTC_TRACE)
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    core::log::Logger::instance().trace(std::string_view{buf});
#else
    (void)fmt;
#endif
}

std::string make_session_id() {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    std::random_device rd;
    std::mt19937_64 mt(rd());
    return std::to_string(now) + std::to_string(mt() & 0xFFFF);
}

std::vector<std::uint8_t> base64_decode(std::string_view sv) {
    static const auto kIdx = []{
        std::array<int8_t, 256> t{};
        t.fill(-1);
        const char* a =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) t[static_cast<uint8_t>(a[i])] = static_cast<int8_t>(i);
        return t;
    }();
    std::vector<std::uint8_t> raw;
    raw.reserve((sv.size() * 3) / 4);
    int val = 0, bits = 0;
    for (char c : sv) {
        if (c == '=') break;
        if (c < 0 || kIdx[static_cast<uint8_t>(c)] < 0) continue;
        val = (val << 6) | kIdx[static_cast<uint8_t>(c)];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            raw.push_back(static_cast<std::uint8_t>((val >> bits) & 0xff));
        }
    }
    return raw;
}

// Decode RFC 8122 DTLS fingerprint value = uppercase hex bytes separated by ':'.
// Example: "AA:BB:CC:..." -> {0xAA, 0xBB, 0xCC, ...}.  Tolerant of whitespace.
// Returns empty vector on malformed input.
std::vector<std::uint8_t> hex_colon_decode(std::string_view sv) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        return -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve(sv.size() / 3 + 1);
    int hi = -1;
    for (char c : sv) {
        if (c == ':' || c == ' ' || c == '\t') {
            if (hi >= 0) return {};     // dangling half-byte
            continue;
        }
        int v = hex(c);
        if (v < 0) return {};
        if (hi < 0) hi = v;
        else {
            out.push_back(static_cast<std::uint8_t>((hi << 4) | v));
            hi = -1;
        }
    }
    if (hi >= 0) return {};            // dangling half-byte at end
    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// NimRTCEngine::Impl — all concrete-module state lives here.
//
// Layout Invariant 4 forbids NimRTCEngine from depending on concrete
// module headers in its public API.  engine.hpp forward-declares the
// concrete types it exposes in the public surface (e.g. dtls::DtlsState)
// and the accessors that need them are out-of-line; the rest of the
// concrete state (JitterBuffers, Audio3A fallback, SRTP, Opus, DTLS, SDP)
// lives in this Impl struct compiled together with engine.cpp.
// ---------------------------------------------------------------------------
struct NimRTCEngine::Impl {
    // SDP: Parser/Munger are concrete classes.
    std::unique_ptr<sdp::Parser>    sdp_parser = std::make_unique<sdp::Parser>();
    std::unique_ptr<sdp::Munger>    sdp_munger = std::make_unique<sdp::Munger>();

    // Jitter buffers — keyed by SSRC.
    std::map<std::uint32_t, std::unique_ptr<jb::JitterBuffer>> jitter_buffers;

    // Concrete fallback classes (when no plugin adapter is registered).
    std::unique_ptr<audio3a::IAudio3A> audio3a_concrete;

    // SRTP / DTLS / Opus — not yet plugin-exposed.
    // DtlsSessionImpl is a global-scope typedef (defined in the
    // NIMRTC_USE_WOLFSSL_DTLS conditional block at the top of this file).
    // Aliased locally so unique_ptr<DtlsSessionImpl> compiles inside this
    // struct (without polluting the global namespace lookup chain).
    using DtlsSessionImpl_T = ::DtlsSessionImpl;
    std::unique_ptr<DtlsSessionImpl_T> dtls;
    std::unique_ptr<srtp::SrtpContext> srtp;
#ifdef NIMRTC_HAS_OPUS
    std::unique_ptr<opus::Encoder>     opus_encoder;
    std::unique_ptr<opus::Decoder>     opus_decoder;
#endif

    bool            dtls_active_inbound = false;
    bool            srtp_installed      = false;
    int             srtp_stats_drop     = 0;
    nimrtc::dtls::DtlsState last_dtls_state     = nimrtc::dtls::DtlsState::Closed;

    // Local SDP (after create_offer / process_remote_sdp).
    std::optional<sdp::SessionDescription> local_sdp;

    // ---- Video receive pipeline decode buffers (ping-pong I420) ------------
    // Allocated lazily on first decoded frame; reused across frames.
    // We keep two buffers so we can receive a new frame while the previous
    // one is still being rendered (the sink takes a view, not ownership).
    static constexpr std::size_t kDecodeBufferCount = 2;
    std::array<std::vector<std::uint8_t>, kDecodeBufferCount> video_decode_bufs;
    std::atomic<std::size_t> video_decode_buf_idx_{0};
    std::uint32_t video_decode_width_  = 0;
    std::uint32_t video_decode_height_ = 0;

    // ---- Video send pipeline state ------------------------------------------
    // Tracks whether the next encoded frame is a keyframe (forced by
    // force_keyframe() or signalled by the codec).  Consumed by the sender's
    // packet callback to set kVideoKeyframe priority on the marker packet.
    std::atomic<bool> video_keyframe_pending_{false};

    // ---- DTLS role negotiation ---------------------------------------------
    // Resolved in process_remote_sdp() from the peer's a=setup attribute.
    // Used to emit a matching a=setup on the answer SDP.  Empty until
    // process_remote_sdp() runs; defaults to "active" if unset.
    std::string dtls_local_setup;

    // ---- send_audio scratch buffers (P0#4) --------------------------------
    // Reusable per-engine buffers for the audio capture→encode→RTP→SRTP
    // path.  Capacity grows monotonically across calls, avoiding the heap
    // allocations that a fresh `std::vector` per call would incur.
    //
    // `audio_codec_pkt` holds the raw codec (Opus) output for the in-flight
    // frame.  `audio_send_buf` is the SRTP-protected RTP packet that goes
    // out the wire (or into the scheduler queue).  Both are members rather
    // than `static thread_local` so the engine owns the lifetime and so a
    // future move to per-stream send queues stays trivial.
    std::vector<std::uint8_t> audio_codec_pkt;
    std::vector<std::uint8_t> audio_send_buf;

    // ---- send_video scratch buffer (P0#4) ---------------------------------
    // Reusable buffer for the encoded H.264 frame.  Default reserve size
    // matches the worst-case 64 KiB IDR; resize grows monotonically.
    std::vector<std::uint8_t> video_enc_buf;

    uint32_t        last_open_rc = 0;
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

NimRTCEngine::NimRTCEngine(EngineConfig config)
    : impl_(new Impl()), config_(std::move(config)) {}

NimRTCEngine::~NimRTCEngine() {
    if (state_ == State::kOpen) {
        close();
    }
    delete impl_;
    impl_ = nullptr;
}

void NimRTCEngine::init_bwe_scheduler() noexcept {
    auto& reg = core::PluginRegistry::instance();

    // ---- BWE plugin -------------------------------------------------------
    // Created and opened here; injected into ice_t_ via set_bwe().
    // If bwe_name is empty, BWE is skipped (passthrough — "transport" profile).
    if (!config_.bwe_name.empty() && !bwe_) {
        const auto* f = reg.get_bwe(config_.bwe_name);
        if (f) {
            bwe_.reset(f->create(config_.bwe_config));
            if (bwe_) {
                if (bwe_->open() != plugins::kOk) {
                    core::log::Logger::instance().warn(
                        "engine: bwe plugin open failed, skipping BWE");
                    bwe_.reset();
                }
            }
        } else {
            core::log::Logger::instance().warn(
                std::string("engine: bwe plugin not found: ")
                    .append(config_.bwe_name));
        }
    }

    // ---- Scheduler plugin ------------------------------------------------
    // Created and opened here; injected into ice_t_ via set_scheduler().
    // If scheduler_config.enabled is false, scheduler is skipped (direct ICE send).
    if (config_.scheduler_config.enabled && !scheduler_) {
        const auto* f = reg.get_scheduler(config_.scheduler_name);
        if (f) {
            scheduler_.reset(f->create(config_.scheduler_config));
            if (scheduler_) {
                if (scheduler_->open() != plugins::kOk) {
                    core::log::Logger::instance().warn(
                        "engine: scheduler plugin open failed, falling back to direct send");
                    scheduler_.reset();
                }
            }
        } else {
            core::log::Logger::instance().warn(
                std::string("engine: scheduler plugin not found: ")
                    .append(config_.scheduler_name));
        }
    }

    // ---- Inject into ICE transport ---------------------------------------
    // ice_t_ is set by the time init_bwe_scheduler() is called
    // (pre_open / open both create it first).
    if (ice_t_) {
        ice_t_->set_bwe(bwe_.get());
        ice_t_->set_scheduler(scheduler_.get());
    }
}

void NimRTCEngine::shutdown_bwe_scheduler() noexcept {
    auto close_plugin = [](auto& p) {
        if (p) p->close();
        p.reset();
    };
    close_plugin(bwe_);
    close_plugin(scheduler_);
    if (ice_t_) {
        ice_t_->set_bwe(nullptr);
        ice_t_->set_scheduler(nullptr);
    }
}

    uint32_t NimRTCEngine::pre_open() noexcept {
    if (state_ != State::kConstructed) return core::kEngineNotReady;
    core::log::Logger::instance().debug("pre_open: starting");

    // Defer ice_t_->open() until a later open() call so the caller can
    // call set_remote_ice() between pre_open() and open() (this is what
    // the loopback test relies on to fix the ICE role conflict).
    const uint32_t rc = init_modules_once();
    if (rc != 0) return rc;

    // State stays kConstructed — open() will call ice_t_->open()
    // and set state to kOpen when invoked.
    return 0;
}

uint32_t NimRTCEngine::init_modules_once() noexcept {
    core::register_all_default_plugins();
    auto& reg = core::PluginRegistry::instance();

    // ---- Plugin: Transport (created but NOT opened yet) -------------------
    //
    // Preferred path: look up the ICE-aware factory by name. This is the
    // only place the engine ever casts across the plugin seam — once we
    // hold an `IICETransport*`, all ICE-specific calls go through the
    // virtual interface (no dynamic_cast to `ice::IceTransport`).
    //
    // We still keep a one-shot dynamic_cast fallback so that if a user
    // registers a generic ITransportFactory (not an IICETransportFactory)
    // under the same name, the engine can still detect it and run — the
    // ICE-specific code paths then just skip silently.
    const plugins::IICETransportFactory* ifactory =
        reg.get_ice_transport(config_.transport_name);
    if (ifactory) {
        ice_t_.reset(ifactory->create_ice());
    } else {
        const plugins::ITransportFactory* tfactory =
            reg.get_transport(config_.transport_name);
        if (!tfactory) {
            impl_->last_open_rc = core::kEngineInternal;
            if (on_error_) on_error_(core::kEngineInternal, "transport plugin not found");
            return core::kEngineInternal;
        }
        plugins::ITransport* raw = tfactory->create();
        if (!raw) {
            impl_->last_open_rc = core::kEngineInternal;
            if (on_error_) on_error_(core::kEngineInternal, "transport factory returned null");
            return core::kEngineInternal;
        }
        // Last-resort safety net: a generic ITransport that also happens
        // to implement IICETransport.  This is the only place the engine
        // still does a cross-class dynamic_cast, and it's intentional —
        // it lets out-of-tree ICE transports (or a test double) work
        // without registering the ICE-specific factory.
        ice_t_.reset(dynamic_cast<plugins::IICETransport*>(raw));
        if (!ice_t_) {
            impl_->last_open_rc = core::kEngineInternal;
            if (on_error_) on_error_(core::kEngineInternal, "transport plugin does not implement IICETransport");
            delete raw;
            return core::kEngineInternal;
        }
    }

    // Apply pre-open configuration through the plugin interface — no
    // dynamic_cast to `ice::IceTransport` needed.
    ice_t_->set_bind_address(config_.local_bind_address);
    ice_t_->set_stun_server(config_.stun_server_host, config_.stun_server_port);
    ice_t_->set_local_port_range(config_.local_port_range_begin,
                                  config_.local_port_range_end);
    // TURN servers are added via NimRTCEngine::add_turn_server() *before*
    // open() — they're applied to ice_t_ at that time, so no loop here.

    // ---- BWE + Scheduler (injected into ice_t_) -------------------------
    init_bwe_scheduler();

    // ---- Audio3A --------------------------------------------------------
    const plugins::IAudio3AFactory* a3a_factory = reg.get_audio3a(config_.audio3a_name);
    if (a3a_factory) {
        audio3a_plugin_.reset(a3a_factory->create());
        if (audio3a_plugin_) {
            audio3a_plugin_->set_callbacks(
                [](bool) {}, [](float) {},
                [this](std::uint32_t err, std::string_view msg) {
                    if (on_error_) on_error_(err, msg);
                });
            if (audio3a_plugin_->open() != plugins::kOk) {
                if (on_error_) on_error_(core::kAudio3APluginOpenFailed, "audio3a plugin open failed; falling back to concrete");
                audio3a_plugin_.reset();
            }
        }
    }
    if (!audio3a_plugin_) {
        impl_->audio3a_concrete = std::make_unique<audio3a::NullAudio3A>();
        audio3a::Config a3a;
        a3a.sample_rate_hz    = config_.pcm_sample_rate_hz;
        a3a.capture_channels = config_.pcm_channels;
        a3a.render_channels   = config_.pcm_channels;
        impl_->audio3a_concrete->init(a3a);
    }

    // ---- Codec ---------------------------------------------------------
    {
        plugins::CodecConfig codec_cfg{};
        codec_cfg.sample_rate_hz = config_.pcm_sample_rate_hz;
        codec_cfg.channels       = config_.pcm_channels;
        codec_cfg.bitrate_bps    = 64000;
        codec_cfg.complexity     = 10;
        codec_cfg.fec_enabled    = true;
        codec_cfg.dtx_enabled    = false;
        codec_cfg.vad_enabled    = false;
        codec_cfg.payload_type   = config_.audio_codec.payload_type;
        codec_cfg.name           = config_.codec_name;
        const plugins::ICodecFactory* cf = reg.get_codec(config_.codec_name);
        if (cf) {
            codec_plugin_.reset(cf->create(codec_cfg));
            if (codec_plugin_ && codec_plugin_->open() != plugins::kOk) {
                codec_plugin_.reset();
            }
        }
        if (!codec_plugin_) {
#ifdef NIMRTC_HAS_OPUS
            opus::EncoderConfig ec;
            ec.sample_rate_hz = config_.pcm_sample_rate_hz;
            ec.channels       = config_.pcm_channels;
            impl_->opus_encoder = std::make_unique<opus::Encoder>(ec);
            opus::DecoderConfig dc;
            dc.sample_rate_hz = config_.pcm_sample_rate_hz;
            dc.channels       = config_.pcm_channels;
            impl_->opus_decoder = std::make_unique<opus::Decoder>(dc);
#endif
        }
    }

    // ---- SRTP ----------------------------------------------------------
    impl_->srtp = std::make_unique<srtp::SrtpContext>();
    core::log::Logger::instance().debug(
        "engine: srtp_ created, about to create dtls_");

    // ---- DTLS ----------------------------------------------------------
    // Note: the initial DTLS role is set to Server as a placeholder —
    // process_remote_sdp() resolves the actual role from the peer's
    // a=setup attribute (RFC 5763 §5) and calls dtls->set_role() before
    // the handshake starts.  See P0#1 (DTLS role negotiation).
    nimrtc::dtls::Config dcfg;
    dcfg.role          = nimrtc::dtls::DtlsRole::Server;
    dcfg.srtp_profile  = nimrtc::dtls::SrtpProfile::Aes128CmSha1_80;
    impl_->dtls = std::make_unique<Impl::DtlsSessionImpl_T>(dcfg);
    core::log::Logger::instance().debug(
        "engine: dtls_ created, calling dtls_->open()");
    if (!impl_->dtls->open()) {
        impl_->last_open_rc = core::kDtlsOpenFailed;
        core::log::Logger::instance().error("engine: dtls_->open() returned false");
        if (on_error_) on_error_(core::kDtlsOpenFailed, "DTLS open failed");
        return core::kDtlsOpenFailed;
    }
    core::log::Logger::instance().debug("engine: dtls_->open() returned ok");

    // ---- ICE recv / error callbacks ------------------------------------
    ice_t_->set_callbacks(
        [this](plugins::BufferView bv) { on_transport_recv(bv); },
        [this](plugins::Status err, std::string_view msg) {
            if (on_error_) on_error_(static_cast<std::uint32_t>(err), msg);
        });

    return 0;
}

uint32_t NimRTCEngine::open() noexcept {
    if (state_ != State::kConstructed) return core::kEngineNotReady;

    // If pre_open() already created the modules, skip recreation and go straight
    // to opening the transport (which triggers gather_candidates with the
    // correct role set by set_remote_ice()).
    if (!ice_t_) {
        // Normal path: modules not yet created — run the full init via the
        // shared helper. pre_open() takes the same path.
        const uint32_t rc = init_modules_once();
        if (rc != 0) return rc;
    } else {
        core::log::Logger::instance().debug(
            "op7b transport pre_open→open");
    }

    // ---- ice_t_->open() triggers ICE gather_candidates() ------------------
    // At this point, if set_remote_ice() was called before open(), the ICE
    // transport knows the peer's credentials and will be CONTROLLED.
    core::log::Logger::instance().debug("op8 calling ice_t_->open()");
    const auto ice_open_rc = ice_t_->open();
    if (ice_open_rc != plugins::kOk) {
        char err[64];
        std::snprintf(err, sizeof(err), "transport open failed rc=%d",
                      static_cast<int>(ice_open_rc));
        impl_->last_open_rc = core::kEngineInternal;
        core::log::Logger::instance().error(std::string(err));
        if (on_error_) on_error_(core::kEngineInternal, err);
        ice_t_.reset();
        return core::kEngineInternal;
    }
    core::log::Logger::instance().debug("op7 transport open ok");

    // ---- Video plugins (R3-Batch) ---------------------------------------
    // Resolves all four video_* plugins from core::PluginRegistry and
    // opens them. Failures are non-fatal at engine level (R3 scope: just
    // expose the accessors); warnings go through on_error_.
    init_video_plugins();

    state_ = State::kOpen;
    if (on_state_change_) on_state_change_("open");
    trace("NimRTCEngine opened (DTLS, SRTP, Opus initialised)");
    return 0;
}

void NimRTCEngine::set_video_sink(std::unique_ptr<plugins::IVideoSink> sink) noexcept {
    if (video_sink_) {
        video_sink_->close();
        video_sink_.reset();
    }
    video_sink_ = std::move(sink);
    if (video_sink_) {
        if (video_sink_->open() != plugins::kOk) {
            if (on_error_) on_error_(core::kVideoSinkOpenFailed, "video_sink open failed");
            video_sink_.reset();
        }
    }
}

void NimRTCEngine::set_video_source(
    std::unique_ptr<plugins::IVideoSource> source) noexcept {
    if (video_source_) {
        video_source_->close();
        video_source_.reset();
    }
    video_source_ = std::move(source);
    if (video_source_) {
        if (video_source_->open() != plugins::kOk) {
            if (on_error_) on_error_(core::kVideoSourceOpenFailed, "video_source open failed");
            video_source_.reset();
        }
    }
}

void NimRTCEngine::init_video_plugins() noexcept {
    auto& reg = core::PluginRegistry::instance();

    // ========================================================================
    // PHASE 1 — create all plugins (order: sink, source, receiver, sender, codec)
    // ========================================================================

    // ---- Video Sink --------------------------------------------------------
    if (!video_sink_) {
        const auto* f = reg.get_video_sink(config_.video_sink_name);
        if (f) {
            plugins::VideoSinkConfig cfg{};
            video_sink_.reset(f->create(cfg));
            if (video_sink_) {
                if (video_sink_->open() != plugins::kOk) {
                    if (on_error_) on_error_(core::kVideoSinkOpenFailed,
                        "video_sink plugin open failed");
                    video_sink_.reset();
                }
            }
        } else if (on_error_) {
            on_error_(core::kVideoSinkPluginMissing,
                std::string("video_sink plugin not found: ")
                    .append(config_.video_sink_name));
        }
    }

    // ---- Video Source ------------------------------------------------------
    if (!video_source_) {
        const auto* f = reg.get_video_source(config_.video_source_name);
        if (f) {
            plugins::VideoSourceConfig cfg{};
            cfg.width  = 640;
            cfg.height = 480;
            cfg.fps    = 30;
            cfg.ssrc   = config_.video_sender_tuning.ssrc;
            video_source_.reset(f->create(cfg));
            if (video_source_) {
                if (video_source_->open() != plugins::kOk) {
                    if (on_error_) on_error_(core::kVideoSourceOpenFailed,
                        "video_source plugin open failed");
                    video_source_.reset();
                }
            }
        } else if (on_error_) {
            on_error_(core::kVideoSourcePluginMissing,
                std::string("video_source plugin not found: ")
                    .append(config_.video_source_name));
        }
    }

    // ---- Video Receiver ----------------------------------------------------
    if (!video_receiver_) {
        const auto* f = reg.get_video_receiver(config_.video_receiver_name);
        if (f) {
            plugins::VideoReceiverConfig cfg{};
            cfg.codec        = plugins::VideoCodecKind::kH264;
            cfg.ssrc         = config_.video_receiver_tuning.ssrc;
            cfg.payload_type = config_.video_receiver_tuning.payload_type;
            cfg.expect_fu_a  = config_.video_receiver_tuning.expect_fu_a;
            cfg.max_inflight_frames =
                config_.video_receiver_tuning.max_inflight_frames;
            cfg.max_jitter_buffer_ms =
                config_.video_receiver_tuning.max_jitter_buffer_ms;
            cfg.emit_nacks  = config_.video_receiver_tuning.emit_nacks;
            video_receiver_.reset(f->create(cfg));
            if (video_receiver_) {
                if (video_receiver_->open() != plugins::kOk) {
                    if (on_error_) on_error_(core::kVideoReceiverOpenFailed,
                        "video_receiver plugin open failed");
                    video_receiver_.reset();
                }
            }
        } else if (on_error_) {
            on_error_(core::kVideoReceiverPluginMissing,
                std::string("video_receiver plugin not found: ")
                    .append(config_.video_receiver_name));
        }
    }

    // ---- Video Sender ------------------------------------------------------
    if (!video_sender_) {
        const auto* f = reg.get_video_sender(config_.video_sender_name);
        if (f) {
            plugins::VideoSenderConfig cfg{};
            cfg.codec        = plugins::VideoCodecKind::kH264;
            cfg.ssrc         = config_.video_sender_tuning.ssrc;
            cfg.payload_type = config_.video_sender_tuning.payload_type;
            cfg.mtu          = config_.video_sender_tuning.mtu;
            cfg.initial_seq  = config_.video_sender_tuning.initial_seq;
            cfg.packetization_mode =
                plugins::VideoPacketizationMode::kNonInterleaved;
            video_sender_.reset(f->create(cfg));
            if (video_sender_) {
                if (video_sender_->open() != plugins::kOk) {
                    if (on_error_) on_error_(core::kVideoSenderOpenFailed,
                        "video_sender plugin open failed");
                    video_sender_.reset();
                }
            }
        } else if (on_error_) {
            on_error_(core::kVideoSenderPluginMissing,
                std::string("video_sender plugin not found: ")
                    .append(config_.video_sender_name));
        }
    }

    // ---- Video Codec (H.264) -----------------------------------------------
    if (!config_.video_codec_name.empty() && !video_codec_) {
        const auto* f = reg.get_video_codec(config_.video_codec_name);
        if (f) {
            plugins::VideoCodecConfig cfg{};
            cfg.width = 640;
            cfg.height = 480;
            cfg.fps = 30;
            cfg.bitrate_bps = 1'000'000;
            cfg.keyframe_interval = 60;
            cfg.pixel_format = plugins::VideoPixelFormat::kI420;
            cfg.codec = plugins::VideoCodecKind::kH264;
            cfg.payload_type = config_.video_receiver_tuning.payload_type;
            cfg.name = config_.video_codec_name;
            video_codec_.reset(f->create(cfg));
            if (video_codec_) {
                if (video_codec_->open() != plugins::kOk) {
                    if (on_error_) on_error_(core::kVideoCodecOpenFailed,
                        "video_codec plugin open failed");
                    video_codec_.reset();
                }
            }
        } else if (on_error_) {
            on_error_(core::kVideoCodecPluginMissing,
                std::string("video_codec plugin not found: ")
                    .append(config_.video_codec_name));
        }
    }

    // ========================================================================
    // PHASE 2 — wire all pipeline callbacks (all plugins now exist)
    // ========================================================================

    // ---- Video send pipeline: source → codec → sender → RTP+SRTP → scheduler
    //
    // Tracking state: the codec's force_keyframe() causes the NEXT encoded
    // frame to be a keyframe. We track it in impl_->video_keyframe_pending_
    // (atomic bool) so it survives across send_video() calls.
    //
    // The video_source frame callback is NOT wired to the encode path
    // here (IVideoCodec / IVideoSender are NOT thread-safe; the source
    // cadence thread would race with any concurrent user thread).
    // Users drive video through send_video() — see engine.hpp.

    if (video_sender_) {
        // video_sender packet callback: build RTP header → SRTP protect → enqueue
        video_sender_->set_packet_callback(
            [this](plugins::BufferView payload,
                                          std::uint32_t rtp_ts,
                                          std::uint16_t seq,
                                          bool marker) {
                // ---- Build 12-byte RTP header ---------------------------------
                // byte 0: V=2 P=0 X=0 CC=0 → 0x80
                // byte 1: M=X(1) PT(7)
                std::uint8_t pt = config_.video_sender_tuning.payload_type;
                std::uint8_t hdr[12] = {
                    0x80,                             // version, no extensions
                    static_cast<std::uint8_t>((marker ? 0x80 : 0x00) | (pt & 0x7F)),
                    static_cast<std::uint8_t>((seq >> 8) & 0xFF),
                    static_cast<std::uint8_t>(seq & 0xFF),
                    static_cast<std::uint8_t>((rtp_ts >> 24) & 0xFF),
                    static_cast<std::uint8_t>((rtp_ts >> 16) & 0xFF),
                    static_cast<std::uint8_t>((rtp_ts >>  8) & 0xFF),
                    static_cast<std::uint8_t>(rtp_ts & 0xFF),
                    static_cast<std::uint8_t>((config_.video_sender_tuning.ssrc >> 24) & 0xFF),
                    static_cast<std::uint8_t>((config_.video_sender_tuning.ssrc >> 16) & 0xFF),
                    static_cast<std::uint8_t>((config_.video_sender_tuning.ssrc >>  8) & 0xFF),
                    static_cast<std::uint8_t>(config_.video_sender_tuning.ssrc & 0xFF),
                };

                // ---- Assemble full RTP packet ----------------------------------
                std::vector<std::uint8_t> rtp_pkt;
                rtp_pkt.reserve(12 + payload.size());
                rtp_pkt.insert(rtp_pkt.end(), hdr, hdr + 12);
                rtp_pkt.insert(rtp_pkt.end(),
                               payload.data(), payload.data() + payload.size());

                // ---- SRTP protect (if DTLS is connected) ----------------------
                if (impl_->srtp_installed && impl_->srtp) {
                    auto* sess = impl_->srtp->get_session(
                        config_.video_sender_tuning.ssrc, /*outgoing=*/true);
                    if (!sess) {
                        // SRTP not ready — drop this packet silently.
                        return;
                    }
                    core::ByteSpan span(rtp_pkt.data(), rtp_pkt.size());
                    auto enc = sess->protect_rtp(span,
                        config_.video_sender_tuning.ssrc, rtp_ts);
                    if (!enc) return;
                    rtp_pkt.assign(enc.value().data(),
                                   enc.value().data() + enc.value().size());
                }

                // ---- Enqueue to scheduler (move, P0#3) or send directly -------
                plugins::Priority prio = plugins::Priority::kVideo;
                if (marker && impl_->video_keyframe_pending_.load(std::memory_order_acquire)) {
                    prio = plugins::Priority::kVideoKeyframe;
                    impl_->video_keyframe_pending_.store(false, std::memory_order_release);
                }

                if (scheduler_) {
                    scheduler_->enqueue_owned(prio,
                                              std::move(rtp_pkt),
                                              plugins::Addr{});
                } else if (ice_t_) {
                    plugins::BufferView bv{rtp_pkt.data(), rtp_pkt.size()};
                    ice_t_->send(bv, {});
                }
            });
    }

    // ---- Video receive pipeline: receiver → codec → sink
    //
    // On each assembled frame from the receiver:
    //   1. Decode via video_codec_
    //   2. Render via video_sink_
    //   3. Fire on_video_frame_ (NAL delivery, for existing users)
    // -------------------------------------------------------------------------
    if (video_receiver_) {
        video_receiver_->set_frame_callback(
            [this](const plugins::EncodedVideoFrame& enc,
                   plugins::TimestampUs /*now_us*/) {
                // Fire the NAL-level callback (existing behaviour).
                if (on_video_frame_) {
                    on_video_frame_(enc.payload.data(),
                                   enc.payload.size(),
                                   enc.is_keyframe);
                }

                // Decode + render if the codec and sink are available.
                if (!video_codec_ || !video_sink_) return;

                const std::uint32_t w = enc.payload.size() > 0 ? 640 : 0;
                const std::uint32_t h = 480;

                // Lazily allocate / resize decode buffers to the frame size.
                // Stride is aligned to 16 bytes.
                auto align16 = [](std::uint32_t v) -> std::uint32_t {
                    return (v + 15u) & ~15u;
                };
                const std::uint32_t stride_y = align16(w);
                const std::uint32_t stride_uv = align16(w / 2);
                const std::size_t y_size = static_cast<std::size_t>(stride_y) * h;
                const std::size_t uv_size = static_cast<std::size_t>(stride_uv) * (h / 2);
                const std::size_t total = y_size + uv_size + uv_size;

                // Use a ping-pong buffer: alternate between two allocations.
                std::size_t buf_idx = impl_->video_decode_buf_idx_.fetch_add(1)
                                      % 2;
                auto& buf = impl_->video_decode_bufs[buf_idx];
                if (buf.size() < total) buf.resize(total);

                // Set plane pointers into the flat buffer.
                std::uint8_t* planes[3] = {
                    buf.data(),
                    buf.data() + y_size,
                    buf.data() + y_size + uv_size,
                };

                // Decode.
                plugins::VideoFrame raw_out{};
                raw_out.info.capture_ts_us = enc.info.capture_ts_us;
                raw_out.info.frame_seq     = enc.info.frame_seq;
                raw_out.info.rtp_timestamp = enc.info.rtp_timestamp;

                if (video_codec_->decode(enc, raw_out, planes) != plugins::kOk) {
                    return;
                }

                // Render to sink.
                plugins::VideoSourceFrame sink_frame{};
                sink_frame.width   = w;
                sink_frame.height  = h;
                sink_frame.format  = plugins::VideoPixelFormat::kI420;
                sink_frame.stride_y = static_cast<std::int32_t>(stride_y);
                sink_frame.stride_u = static_cast<std::int32_t>(stride_uv);
                sink_frame.stride_v = static_cast<std::int32_t>(stride_uv);
                sink_frame.plane_y  = planes[0];
                sink_frame.plane_u  = planes[1];
                sink_frame.plane_v  = planes[2];
                sink_frame.buffer_size = total;
                sink_frame.capture_ts_us = enc.info.capture_ts_us;
                sink_frame.frame_seq     = enc.info.frame_seq;
                sink_frame.rtp_timestamp = enc.info.rtp_timestamp;

                video_sink_->render(sink_frame);
            });
    }
}

void NimRTCEngine::shutdown_video_plugins() noexcept {
    auto close_all = [](auto& p) {
        if (p) p->close();
        p.reset();
    };
    stop_video();   // stop video source if running
    close_all(video_source_);
    close_all(video_sink_);
    close_all(video_receiver_);
    close_all(video_sender_);
    close_all(video_codec_);
}

void NimRTCEngine::close() noexcept {
    if (state_ != State::kOpen) return;
    if (ice_t_) ice_t_->close();
    if (audio3a_plugin_) audio3a_plugin_->close();
    if (codec_plugin_) codec_plugin_->close();
    shutdown_video_plugins();
    shutdown_bwe_scheduler();
    ice_t_.reset();
    impl_->jitter_buffers.clear();
    audio3a_plugin_.reset();
    impl_->audio3a_concrete.reset();
    codec_plugin_.reset();
#ifdef NIMRTC_HAS_OPUS
    impl_->opus_encoder.reset();
    impl_->opus_decoder.reset();
#endif
    impl_->dtls.reset();
    impl_->srtp.reset();
    state_ = State::kClosed;
    if (on_state_change_) on_state_change_("closed");
}

std::string NimRTCEngine::create_offer() noexcept {
    if (ice_t_) ice_t_->wait_for_gathering(1500);

    sdp::SessionDescription sdp;
    sdp.version = 0;
    sdp.origin_username         = "-";
    sdp.origin_session_id      = make_session_id();
    sdp.origin_session_version = "1";
    sdp.origin_address          = "0.0.0.0";
    sdp.session_name            = "-";
    sdp.bundle_mids = {"0"};
    sdp.msid_semantic_token = "WMS";

    sdp::MediaDescription audio;
    audio.type            = sdp::MediaType::Audio;
    audio.port            = 9;
    audio.protocol        = "UDP/TLS/RTP/SAVPF";
    // Build the format string key from the configurable payload type.
    const std::string pt = std::to_string(config_.audio_codec.payload_type);
    audio.formats         = {pt};
    audio.direction       = sdp::Direction::SendRecv;
    audio.mid             = "0";
    audio.rtcp_mux_value  = "rtcp-mux";
    audio.ice_ufrag       = ice_t_ ? ice_t_->local_ufrag() : "";
    audio.ice_pwd         = ice_t_ ? ice_t_->local_password() : "";
    audio.ice_options     = "trickle";
    audio.dtls_setup      = "actpass";

    if (impl_->dtls) {
        audio.dtls_fingerprint_algo  = "sha-256";
        audio.dtls_fingerprint_value = impl_->dtls->local_fingerprint().hex_colon;
    }

    sdp::MediaDescription::RtpMap codec_map;
    codec_map.encoding   = config_.audio_codec.encoding;
    codec_map.clock_rate = config_.audio_codec.clock_rate;
    codec_map.channels   = config_.audio_codec.channels;
    audio.rtpmap[pt]     = codec_map;
    if (!config_.audio_codec.fmtp.empty())
        audio.fmtp[pt] = config_.audio_codec.fmtp;

    // Helper: append ICE candidates stripped of any prefix the libjuice
    // collector added (the munger re-emits "a=candidate:" itself).
    auto push_candidates_into = [&](sdp::MediaDescription& m) {
        if (!ice_t_) return;
        auto gathered = ice_t_->gathered_local_candidates();
        constexpr std::string_view kFullPrefix = "a=candidate:";
        constexpr std::string_view kHalfPrefix = "candidate:";
        for (auto& c : gathered) {
            if (c.size() >= kFullPrefix.size() &&
                c.substr(0, kFullPrefix.size()) == kFullPrefix) {
                m.candidates.push_back(std::string(c.substr(kFullPrefix.size())));
            } else if (c.size() >= kHalfPrefix.size() &&
                       c.substr(0, kHalfPrefix.size()) == kHalfPrefix) {
                m.candidates.push_back(std::string(c.substr(kHalfPrefix.size())));
            } else {
                m.candidates.push_back(c);
            }
        }
    };
    push_candidates_into(audio);

    sdp.media.push_back(std::move(audio));

    // ---- Video m-line (RFC 8829 §5, H.264 via RFC 6184) -----------------
    // Emit a video m-line only when at least one video plugin was resolved
    // (video_receiver_ / video_sender_ / video_codec_).  The default H.264
    // codec plugin is decoder-only — for encode we'd need a real codec
    // plugin — but the SDP/SDP-answer plumbing is correct regardless so
    // that browsers see the negotiation succeed even on a one-way demo.
    if (video_receiver_ || video_sender_) {
        sdp::MediaDescription video;
        video.type     = sdp::MediaType::Video;
        video.port     = 9;
        video.protocol = "UDP/TLS/RTP/SAVPF";
        const std::string vpt =
            std::to_string(config_.video_receiver_tuning.payload_type);
        video.formats         = {vpt};
        video.direction       = sdp::Direction::SendRecv;
        video.mid             = "1";           // second BUNDLE mid
        video.rtcp_mux_value  = "rtcp-mux";
        video.ice_ufrag       = ice_t_ ? ice_t_->local_ufrag() : "";
        video.ice_pwd         = ice_t_ ? ice_t_->local_password() : "";
        video.ice_options     = "trickle";
        video.dtls_setup      = "actpass";

        if (impl_->dtls) {
            video.dtls_fingerprint_algo  = "sha-256";
            video.dtls_fingerprint_value = impl_->dtls->local_fingerprint().hex_colon;
        }

        // a=rtpmap:<vpt> H264/90000  (RFC 6184 §5.6)
        sdp::MediaDescription::RtpMap video_map;
        video_map.encoding   = "H264";
        video_map.clock_rate = 90000;
        video_map.channels   = 1;             // H.264 clock is 90 kHz, no audio channels
        video.rtpmap[vpt]    = video_map;

        // a=fmtp:<vpt> profile-level-id=42E01F;packetization-mode=1;
        //                  sprop-parameter-sets=Z0LAHtkA,aM4G4g==
        // Baseline profile, level 3.1 — matches the synthetic SPS/PPS our
        // H.264 stub encoder emits (see h264::StubEncoder::encode).  When
        // a real codec plugin replaces the stub, the codec should override
        // these via a getter or config (out of scope for this round).
        if (video_codec_) {
            video.fmtp[vpt] =
                "profile-level-id=42E01F;"
                "packetization-mode=1;"
                "level-asymmetry-allowed=1;"
                "sprop-parameter-sets=Z0LAHtkA,aM4G4g==";
        } else {
            video.fmtp[vpt] =
                "profile-level-id=42E01F;packetization-mode=1";
        }

        // a=rtcp-fb:<vpt> nack pli  (RFC 4585 §6.2.1 + WebRTC §5.2)
        // NACK + Picture Loss Indication are the two feedbacks Chrome's
        // SFU expects on a recvonly/sendrecv video track.  We add both
        // unconditionally so the answer side mirrors them verbatim.
        video.rtcp_fb.push_back(vpt + " nack");
        video.rtcp_fb.push_back(vpt + " nack pli");
        video.rtcp_fb.push_back(vpt + " goog-remb");

        // a=ssrc:<video_ssrc> cname:nimrtc-video  (RFC 5576 §4.1)
        // WebRTC browsers count inbound-rtp only when the answer SDP
        // carries an a=ssrc line that matches the wire SSRC.  Emit it
        // here on the offer side as well so we have it for renegotiation.
        char ssrc_line[96];
        std::snprintf(ssrc_line, sizeof(ssrc_line),
                      "%u cname:nimrtc-video",
                      static_cast<unsigned>(config_.video_sender_tuning.ssrc));
        video.extra_attrs.emplace_back("ssrc", ssrc_line);

        push_candidates_into(video);
        sdp.media.push_back(std::move(video));
        // Make sure both mids are in the BUNDLE group.
        sdp.bundle_mids = {"0", "1"};
    }

    auto sdp_str = impl_->sdp_munger->to_sdp(sdp);
    if (!sdp_str) {
        if (on_error_) on_error_(core::kEngineSdpCorrupt, "SDP munger failed");
        return {};
    }
    impl_->local_sdp = std::move(sdp);
    return sdp_str.value();
}

std::optional<std::string>
NimRTCEngine::process_remote_sdp(std::string_view remote_sdp) noexcept {
    auto parsed = impl_->sdp_parser->parse(remote_sdp);
    if (!parsed) {
        if (on_error_) on_error_(core::kEngineSdpCorrupt, "SDP parse failed");
        return std::nullopt;
    }

    const auto& remote = parsed.value();
    core::log::Logger::instance().info(
        std::string("process_remote_sdp: parsed ok; media.size=") + std::to_string(remote.media.size())
        + " extra_attrs.size=" + std::to_string(remote.extra_attrs.size()));

    // Apply ICE credentials + candidates through the plugin seam.
    //
    // RFC 8829 / WebRTC BUNDLE: when the SDP uses BUNDLE, ice-ufrag/ice-pwd
    // often live at the *session* level (since there is effectively one
    // transport), not at the media level.  Try session-level first, then
    // fall back to media-level — whichever has credentials wins.
    std::string ice_block;

    // 1. Session-level fallback (Chrome BUNDLE answer).  The concrete
    //    sdp::SessionDescription stores session-level a= attributes in
    //    extra_attrs (when cur_media was nullopt at parse time).
    std::string sess_ufrag, sess_pwd;
    for (const auto& [k, v] : remote.extra_attrs) {
        if      (k == "ice-ufrag" && !v.empty()) sess_ufrag = v;
        else if (k == "ice-pwd"   && !v.empty()) sess_pwd   = v;
    }
    auto flush_ufrag_pwd = [&](const std::string& ufrag,
                               const std::string& pwd) {
        if (ufrag.empty() || pwd.empty()) return false;
        ice_block += "a=ice-ufrag:" + ufrag + "\n";
        ice_block += "a=ice-pwd:"   + pwd   + "\n";
        return true;
    };

    bool wrote_creds = false;
    if (!sess_ufrag.empty() || !sess_pwd.empty()) {
        wrote_creds = flush_ufrag_pwd(sess_ufrag, sess_pwd);
    }
    core::log::Logger::instance().info(
        std::string("process_remote_sdp: sess_ufrag='") + sess_ufrag
        + "' sess_pwd.len=" + std::to_string(sess_pwd.size())
        + " wrote_creds=" + (wrote_creds ? "1" : "0"));

    // 2. Media-level candidates (always appended).  Use the *first* media
    //    block that has credentials; otherwise use session credentials.
    //
    // Note: `sdp::SessionDescription::media[i]` is the concrete MediaDescription
    // (NOT the plugins::SdpMedia wrapper), which exposes dedicated ice_ufrag,
    // ice_pwd and candidates fields.  This is the engine's internal parser
    // (impl_->sdp_parser), not the ISDP plugin.
    for (const auto& m : remote.media) {
        core::log::Logger::instance().info(
            std::string("process_remote_sdp: media ice_ufrag='") + m.ice_ufrag
            + "' ice_pwd.len=" + std::to_string(m.ice_pwd.size())
            + " candidates=" + std::to_string(m.candidates.size())
            + " dtls_setup='" + m.dtls_setup + "'");
        if (!wrote_creds) {
            wrote_creds = flush_ufrag_pwd(m.ice_ufrag, m.ice_pwd);
        }
        for (const auto& c : m.candidates) {
            ice_block += "a=candidate:" + c + "\n";
        }
        // Don't break: subsequent media blocks may carry more candidates.
    }

    core::log::Logger::instance().info(
        std::string("process_remote_sdp: ice_block.size=") + std::to_string(ice_block.size())
        + " ice_t_=" + (ice_t_ ? "yes" : "no")
        + " wrote_creds=" + (wrote_creds ? "1" : "0"));

    if (wrote_creds && !ice_block.empty() && ice_t_) {
        ice_t_->set_remote_description(ice_block);
    }

    // Configure DTLS role + peer fingerprint from the FIRST m-line we
    // recognise (audio / video / application).  The DTLS session is
    // BUNDLE-shared, so all m= lines use the same DTLS role + fingerprint
    // — we only need to read the offer's setup once.
    for (const auto& rm : remote.media) {
        if (rm.type != sdp::MediaType::Audio &&
            rm.type != sdp::MediaType::Video &&
            rm.type != sdp::MediaType::Application) {
            continue;
        }
        if (impl_->dtls) {
        // DTLS role is determined by the remote peer's `a=setup` attribute
        // (RFC 5763 §5):
        //
        //   remote "actpass" → the peer did not commit to a role; we are the
        //                      answerer and we decide.  WebRTC convention
        //                      (libwebrtc / Chrome): answerer is the DTLS
        //                      Client (sends ClientHello).  We therefore
        //                      become Client and advertise "active".
        //   remote "active"  → the peer will send ClientHello; we are Server
        //                      (passive), advertise "passive".
        //   remote "passive" → the peer is waiting for our ClientHello;
        //                      we are Client (active), advertise "active".
        //   remote ""        → missing setup attribute; treat as "actpass"
        //                      (we answerer, become Client, advertise "active").
        //
        // The resulting local setup attribute is also captured for the
        // answer SDP below.
        std::string peer_setup = rm.dtls_setup;
        if (peer_setup.empty()) peer_setup = "actpass";

        // DEBUG-TEMP: force NimRTC to be the DTLS SERVER (passive) for
        // Chrome interop.  Recent Chrome 124+ BoringSSL rejects every
        // variant of anonymous client response to CertificateRequest
        // (empty list → handshake_failure, omitted → unexpected_message,
        // self-signed cert → certificate_unknown) — but as the SERVER we
        // never receive a CertReq so the question doesn't arise.  This
        // is purely a workaround for the Chrome-side rejection of RFC
        // 5246 §7.4.6 anonymous clients; restore the original logic
        // once Chrome's BoringSSL relaxes this.
        nimrtc::dtls::DtlsRole desired_role  = nimrtc::dtls::DtlsRole::Server;
        std::string    local_setup_attr = "passive";
        (void)peer_setup;
        impl_->dtls->set_role(desired_role);
        impl_->dtls_local_setup = local_setup_attr;
        core::log::Logger::instance().info(
            std::string("process_remote_sdp: dtls role resolved peer_setup='")
            + peer_setup + "' local_setup='" + local_setup_attr
            + "' role=" + (desired_role == nimrtc::dtls::DtlsRole::Client ? "Client" : "Server"));

            if (!rm.dtls_fingerprint_algo.empty() &&
                !rm.dtls_fingerprint_value.empty()) {
                // RFC 8122: peer fingerprint value is colon-separated UPPER hex,
                // NOT base64.  Use hex_colon_decode; fall back to base64 if the
                // value clearly isn't hex (older peers / future deprecation).
                auto bytes = hex_colon_decode(rm.dtls_fingerprint_value);
                if (bytes.empty()) {
                    bytes = base64_decode(rm.dtls_fingerprint_value);
                }
                impl_->dtls->set_peer_fingerprint(rm.dtls_fingerprint_algo, std::move(bytes));
            }
        }
        break;
    }

    if (ice_t_) ice_t_->wait_for_gathering(1500);

    sdp::SessionDescription ans;
    ans.version = 0;
    ans.origin_username         = "-";
    ans.origin_session_id      = make_session_id();
    ans.origin_session_version = "1";
    ans.origin_address          = "0.0.0.0";
    ans.session_name            = "-";
    ans.bundle_mids             = remote.bundle_mids;
    ans.msid_semantic_token     = "WMS";

    for (const auto& rm : remote.media) {
        // Support audio (RTP), video (RTP/H.264), and application (data
        // channel, DTLS/SCTP).  Skipping other types avoids leaking
        // unknown m= sections into the answer.
        if (rm.type != sdp::MediaType::Audio &&
            rm.type != sdp::MediaType::Video &&
            rm.type != sdp::MediaType::Application) {
            continue;
        }
        sdp::MediaDescription am;
        am.type = rm.type;
        am.port = 9;
        am.protocol = rm.protocol.empty() ? "UDP/TLS/RTP/SAVPF" : rm.protocol;
        am.formats  = rm.formats;
        am.direction = sdp::Direction::SendRecv;
        // Preserve the remote mid when present (BUNDLE); fall back to a
        // positional assignment by index for the legacy single-m-line case.
        am.mid      = rm.mid.empty() ? std::to_string(remote.media.size()) : rm.mid;
        am.rtcp_mux_value = "rtcp-mux";
        am.ice_ufrag = local_ufrag();
        am.ice_pwd   = local_password();
        am.dtls_setup = impl_->dtls_local_setup.empty()
                            ? "active"
                            : impl_->dtls_local_setup;
        core::log::Logger::instance().info(
            std::string("process_remote_sdp: answer SDP setup='") + am.dtls_setup +
            "' (peer wanted '" + rm.dtls_setup + "')");
        if (impl_->dtls) {
            am.dtls_fingerprint_algo  = "sha-256";
            am.dtls_fingerprint_value = impl_->dtls->local_fingerprint().hex_colon;
        }

        // For audio: carry rtpmap/fmtp from offer.
        if (rm.type == sdp::MediaType::Audio) {
            am.rtpmap = rm.rtpmap;
            am.fmtp   = rm.fmtp;
        }

        // For video: carry rtpmap/fmtp/rtcp-fb from offer verbatim.  The
        // remote codec params (profile-level-id, packetization-mode, etc.)
        // are what the remote decoder expects to see in our answer; the
        // engine doesn't renegotiate them.  We also append the a=ssrc
        // line so Chrome's inbound-rtp counter advances on the wire SSRC
        // we emit.
        if (rm.type == sdp::MediaType::Video) {
            am.rtpmap   = rm.rtpmap;
            am.fmtp     = rm.fmtp;
            am.rtcp_fb  = rm.rtcp_fb;
            char ssrc_line[96];
            std::snprintf(ssrc_line, sizeof(ssrc_line),
                          "%u cname:nimrtc-video",
                          static_cast<unsigned>(config_.video_sender_tuning.ssrc));
            am.extra_attrs.emplace_back("ssrc", ssrc_line);
        }

        // For data channel (application): carry sctp-port from offer's fmtp if present.
        if (rm.type == sdp::MediaType::Application) {
            for (auto& [fmt, params] : rm.fmtp) {
                // params looks like "max-message-size=1073741823"
                am.fmtp[fmt] = params;
            }
        }

        if (ice_t_) {
            auto gathered = ice_t_->gathered_local_candidates();
            for (auto& c : gathered) {
                // libjuice emits candidates with the "a=candidate:" prefix.
                // The munger adds its own "a=candidate:" so strip the prefix.
                constexpr std::string_view kFullPrefix = "a=candidate:";
                constexpr std::string_view kHalfPrefix = "candidate:";
                if (c.size() >= kFullPrefix.size() &&
                    c.substr(0, kFullPrefix.size()) == kFullPrefix) {
                    am.candidates.push_back(std::string(c.substr(kFullPrefix.size())));
                } else if (c.size() >= kHalfPrefix.size() &&
                           c.substr(0, kHalfPrefix.size()) == kHalfPrefix) {
                    am.candidates.push_back(std::string(c.substr(kHalfPrefix.size())));
                } else {
                    am.candidates.push_back(c);
                }
            }
        }

        ans.media.push_back(std::move(am));
    }

    // Rebuild BUNDLE mids from the answer's media sections so a remote
    // multi-m-line BUNDLE offer produces a matching multi-m-line answer.
    if (!ans.media.empty()) {
        ans.bundle_mids.clear();
        for (const auto& m : ans.media) {
            if (!m.mid.empty()) ans.bundle_mids.push_back(m.mid);
        }
        if (ans.bundle_mids.empty()) {
            ans.bundle_mids = {"0"};
        }
    }

    auto ans_str = impl_->sdp_munger->to_sdp(ans);
    if (!ans_str) {
        if (on_error_) on_error_(core::kEngineSdpCorrupt, "SDP munger failed");
        return std::nullopt;
    }
    return ans_str.value();
}

uint32_t NimRTCEngine::send_audio(const float* pcm_samples, std::size_t num_samples) noexcept {
    if (!is_open()) return core::kEngineNotReady;
    if (!pcm_samples || num_samples == 0) return core::kEngineInvalidParam;
    // Gate on DTLS being Connected — otherwise RTP packets would reach the
    // peer over plaintext UDP (no SRTP key), or get sent into a half-open
    // session that the peer has already torn down (e.g. after a fatal
    // DTLS alert).  Without this check, demo-p2p would continue logging
    // "audio frames sent=N rc=0x0" after the handshake has transitioned
    // to Failed.
    if (!impl_->dtls || !impl_->dtls->is_connected()) {
        return core::kEngineNotReady;
    }

    // 3A — prefer plugin adapter; fall back to concrete NullAudio3A.
    if (audio3a_plugin_) {
        // Plugin API takes raw (ptr, samples, channels) — same shape we have.
        audio3a_plugin_->process_capture(
            const_cast<float*>(pcm_samples),
            num_samples,
            config_.pcm_channels);
    } else if (impl_->audio3a_concrete) {
        audio3a::Frame frame;
        frame.samples        = const_cast<float*>(pcm_samples);
        frame.num_samples    = num_samples;
        frame.num_channels   = config_.pcm_channels;
        frame.sample_rate_hz = config_.pcm_sample_rate_hz;
        impl_->audio3a_concrete->process_capture(frame);
    }

    // Codec — prefer plugin adapter; fall back to concrete opus::Encoder.
    // Reuse Impl::audio_codec_pkt (P0#4) — capacity grows monotonically.
    const std::size_t needed = num_samples * sizeof(float) + 64;
    if (impl_->audio_codec_pkt.size() < needed) {
        impl_->audio_codec_pkt.resize(needed);
    }
    std::size_t codec_len = 0;
    if (codec_plugin_) {
        codec_len = codec_plugin_->encode(pcm_samples, num_samples,
                                         impl_->audio_codec_pkt.data(),
                                         impl_->audio_codec_pkt.size());
#ifdef NIMRTC_HAS_OPUS
    } else if (impl_->opus_encoder) {
        codec_len = impl_->opus_encoder->encode(pcm_samples, num_samples,
                                         impl_->audio_codec_pkt.data(),
                                         impl_->audio_codec_pkt.size());
#endif
    }
    if (codec_len == 0) return 0;

    static std::atomic<std::uint32_t> seq{0};
    static std::atomic<std::uint32_t> ssrc_val{0xDEADBEEF};
    static std::atomic<std::uint32_t> ts_val{0};
    rtp::PacketBuilder pb;
    std::uint32_t curr_ssrc = ssrc_val.load();
    std::uint16_t seq_num = static_cast<std::uint16_t>(seq.fetch_add(1));
    std::uint32_t cur_ts  = ts_val.fetch_add(static_cast<std::uint32_t>(num_samples));
    pb.set_ssrc(curr_ssrc)
      .set_seq(seq_num)
      .set_timestamp(cur_ts)
      .set_payload_type(config_.audio_codec.payload_type)
      .set_marker(false)
      .set_payload(core::ByteSpan(impl_->audio_codec_pkt.data(), codec_len));
    auto pkt_buf = pb.build();
    if (pkt_buf.empty()) return 0;

    // SRTP protect (or pass-through when not yet keyed).
    // Reuse Impl::audio_send_buf (P0#4) — one allocation ever, then in-place.
    core::ByteSpan out_payload(pkt_buf.data(), pkt_buf.size());
    if (impl_->srtp_installed && impl_->srtp) {
        auto* sess = impl_->srtp->get_session(curr_ssrc, /*outgoing=*/true);
        if (!sess) {
            // SRTP installed but no session for this SSRC — drop.
            return 0;
        }
        auto enc = sess->protect_rtp(out_payload, curr_ssrc, cur_ts);
        if (!enc) return 0;
        impl_->audio_send_buf.assign(enc.value().data(),
                                     enc.value().data() + enc.value().size());
    } else {
        impl_->audio_send_buf.assign(out_payload.data(),
                                     out_payload.data() + out_payload.size());
    }

    // ---- Scheduler: enqueue for priority-drain (move, P0#3) --------------
    // The scheduler takes ownership of audio_send_buf via enqueue_owned.
    // After this call, audio_send_buf is in MOVED-FROM state; we must not
    // touch it.  The scheduler will release it on drain.
    if (scheduler_) {
        scheduler_->enqueue_owned(plugins::Priority::kAudio,
                                  std::move(impl_->audio_send_buf),
                                  plugins::Addr{});
        return 0;
    }

    // Fallback: no scheduler — send directly through ICE (transport profile).
    plugins::BufferView bv{impl_->audio_send_buf.data(),
                           impl_->audio_send_buf.size()};
    const auto rc = ice_t_->send(bv, {});
    // Clear so the next call starts fresh.
    impl_->audio_send_buf.clear();
    impl_->audio_send_buf.shrink_to_fit();   // release backing if needed
    return rc;
}

plugins::Status NimRTCEngine::start_video() noexcept {
    if (!is_open()) return plugins::kErrNotReady;
    if (video_source_) {
        video_source_->start();
        return plugins::kOk;
    }
    return plugins::kErrNotReady;
}

void NimRTCEngine::stop_video() noexcept {
    if (video_source_) {
        video_source_->stop();
    }
}

plugins::Status NimRTCEngine::send_video(
    const plugins::VideoSourceFrame& frame) noexcept {
    if (!is_open()) return plugins::kErrNotReady;
    if (!video_codec_ || !video_sender_) return plugins::kErrNotReady;

    // Translate plugins::VideoSourceFrame → plugins::VideoFrame for the codec.
    // P0: only the metadata is propagated; the actual pixel storage is
    // owned by `frame.plane_y/u/v` and must outlive the encode call.
    plugins::VideoFrame raw{};
    raw.info.capture_ts_us = frame.capture_ts_us;
    raw.info.frame_seq     = frame.frame_seq;
    raw.info.rtp_timestamp = frame.rtp_timestamp;

    // Encode — reuse Impl::video_enc_buf (P0#4).  Reserve a typical IDR
    // size on first use; subsequent calls within the same capacity reuse
    // the storage and only pay the per-byte conversion cost inside the
    // codec.  H.264 frames can spike above 64 KiB (high-bitrate 1080p60)
    // — when that happens we resize and pay one reallocation.
    constexpr std::size_t kInitialVideoEncCap = 64 * 1024;
    if (impl_->video_enc_buf.capacity() < kInitialVideoEncCap) {
        impl_->video_enc_buf.reserve(kInitialVideoEncCap);
    }
    plugins::EncodedVideoFrame encoded{};
    const auto enc_rc = video_codec_->encode(raw,
                                             impl_->video_enc_buf.data(),
                                             impl_->video_enc_buf.capacity(),
                                             encoded);
    if (enc_rc != plugins::kOk) {
        // Codec may need a larger buffer than reserved (e.g. very high
        // bitrate IDR).  Resize to the codec's expected capacity and retry
        // exactly once to avoid an unbounded loop on a misbehaving codec.
        const std::size_t needed = encoded.payload.size();
        if (needed == 0 || needed > impl_->video_enc_buf.capacity() * 4) {
            return plugins::kErrInternal;  // give up on pathological sizes
        }
        impl_->video_enc_buf.reserve(needed);
        if (video_codec_->encode(raw,
                                 impl_->video_enc_buf.data(),
                                 impl_->video_enc_buf.capacity(),
                                 encoded) != plugins::kOk) {
            return plugins::kErrInternal;
        }
    }

    // Mark keyframe so the sender packet callback gives it kVideoKeyframe
    // priority (consumed on the marker=true packet).
    if (encoded.is_keyframe) {
        impl_->video_keyframe_pending_.store(true, std::memory_order_release);
    }

    // Packetize (the sender's packet callback handles RTP header assembly
    // + SRTP protect + scheduler enqueue).
    return video_sender_->push_frame(encoded, frame.rtp_timestamp);
}

int NimRTCEngine::tick() noexcept {
    if (!is_open()) return 0;

    // ---- Push BWE estimate into the scheduler (P2/P3 wiring) -------------
    // estimate() is cheap (reads an atomic snapshot in the AIMD impl) and
    // tick is on the engine thread, so no extra locks.  We skip the push
    // when target_bps == 0 so the scheduler keeps its previous budget
    // rather than reading "0 = total sender pause" on a transient BWE
    // startup glitch.  Both pointers may be null in the "transport" profile.
    if (bwe_ && scheduler_) {
        const std::uint32_t target_bps = bwe_->estimate().target_bitrate_bps;
        if (target_bps > 0) {
            scheduler_->on_bwe_update(target_bps);
        }
    }

    // ---- Drain the scheduler (if enabled) ---------------------------------
    // The scheduler holds outbound packets (audio / video / control) and
    // dispatches them in priority order.  drain_with() invokes our callback
    // for each packet; we send it through ICE.
    if (scheduler_) {
        scheduler_->drain_with(64, [this](plugins::Priority,
                                          core::ByteSpan data) noexcept -> bool {
            if (!ice_t_) return false;
            plugins::BufferView bv{data.data(), data.size()};
            ice_t_->send(bv, {});
            return true;
        });
    }

    // Only drain DTLS outbound once ICE has selected a candidate pair;
    // otherwise libjuice drops the UDP datagram ("Send while ICE is not
    // connected") and the handshake never starts.
    if (is_ice_connected()) {
        drain_dtls();
    }
    return ice_t_->recv();
}

const char* NimRTCEngine::ice_state_string() const noexcept {
    if (!ice_t_) return "closed";
    switch (ice_t_->state()) {
        case plugins::IceState::Disconnected: return "disconnected";
        case plugins::IceState::Gathering:    return "gathering";
        case plugins::IceState::Connecting:   return "connecting";
        case plugins::IceState::Connected:    return "connected";
        case plugins::IceState::Completed:    return "completed";
        case plugins::IceState::Failed:       return "failed";
    }
    return "unknown";
}

std::string NimRTCEngine::local_ufrag() const noexcept {
    return ice_t_ ? ice_t_->local_ufrag() : std::string{};
}

std::string NimRTCEngine::local_password() const noexcept {
    return ice_t_ ? ice_t_->local_password() : std::string{};
}

std::string NimRTCEngine::local_dtls_fingerprint_sha256_base64() const noexcept {
    // Pointer stability: dtls_ is owned by the engine and never replaced
    // after open(), so reading it from a const method is safe.
    auto* d = impl_ ? impl_->dtls.get() : nullptr;
    if (!d) return {};
    const auto& fp = d->local_fingerprint();
    if (fp.bytes.empty()) return {};
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((fp.bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= fp.bytes.size()) {
        std::uint32_t v = (std::uint32_t(fp.bytes[i]) << 16)
                        | (std::uint32_t(fp.bytes[i + 1]) <<  8)
                        |  std::uint32_t(fp.bytes[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 0x3f]);
        out.push_back(kAlphabet[(v >> 12) & 0x3f]);
        out.push_back(kAlphabet[(v >>  6) & 0x3f]);
        out.push_back(kAlphabet[ v        & 0x3f]);
        i += 3;
    }
    if (i < fp.bytes.size()) {
        std::uint32_t v = std::uint32_t(fp.bytes[i]) << 16;
        if (i + 1 < fp.bytes.size()) v |= std::uint32_t(fp.bytes[i + 1]) << 8;
        out.push_back(kAlphabet[(v >> 18) & 0x3f]);
        out.push_back(kAlphabet[(v >> 12) & 0x3f]);
        if (i + 1 < fp.bytes.size()) {
            out.push_back(kAlphabet[(v >> 6) & 0x3f]);
        } else {
            out.push_back('=');
        }
        out.push_back('=');
    }
    return out;
}

void NimRTCEngine::on_transport_recv(const plugins::BufferView& pkt) noexcept {
    if (pkt.empty() || pkt.size() < 1) return;
    std::uint8_t b0 = pkt.data()[0];
    bool looks_like_dtls = (b0 == 22 || b0 == 23 || b0 == 21);
    if (looks_like_dtls && impl_->dtls) {
        core::log::Logger::instance().info(
            std::string("engine[dtls]: rx dtls ct=") + std::to_string(b0) +
            " len=" + std::to_string(pkt.size()) +
            " state=" + std::string(nimrtc::dtls::DtlsSession::state_name(impl_->dtls->state())));
        std::span<const std::uint8_t> bytes(pkt.data(), pkt.size());
        // The ITransport::RecvCallback interface carries only the payload, no
        // source address.  For DTLS over ICE there is only one peer at a time
        // (the selected ICE candidate pair), so we derive the peer address from
        // the ICE transport's current selected remote endpoint.  This is correct
        // for both the initial ClientHello and all subsequent handshake records.
        //
        // NOTE: `ice_t_->remote_addr()` returns a text-encoded "host:port"
        // string packed into a plugins::Addr structure.  The DtlsAddr uses
        // plain "host" + port fields — the parsing below extracts those fields.
        nimrtc::dtls::DtlsAddr from;
        if (ice_t_) {
            plugins::Addr raw = ice_t_->remote_addr();
            // Text encoding: "host\0port\0" packed in raw.data[0..len-1].
            std::string_view sv(reinterpret_cast<const char*>(raw.data), raw.len);
            std::size_t colon = sv.find(':');
            if (colon != std::string_view::npos) {
                from.host.assign(sv.data(), colon);
                auto port_str = sv.substr(colon + 1);
                if (!port_str.empty()) {
                    // NOLINTNEXTLINE(cert-env33-c): port is always numeric here
                    int p = std::atoi(std::string(port_str).c_str());
                    from.port = static_cast<std::uint16_t>(p);
                }
            }
        }
        impl_->dtls->feed_inbound(bytes, from);
        // Only flush outbound DTLS records once ICE has selected a pair,
        // mirroring the gate in tick().  Without this, the first inbound
        // ClientHello (which arrives as soon as the peer's ICE connectivity
        // check reaches us) enqueues a HelloVerifyRequest that libjuice
        // silently drops with "Send while ICE is not connected"; the peer
        // never sees our cookie and keeps retransmitting its cookie-less
        // ClientHello forever.
        if (is_ice_connected()) {
            drain_dtls();
        }
        return;
    }
    // CCS (content type 20) isn't recognised as DTLS above; process it so
    // the receiving side can complete its state machine.
    if (b0 == nimrtc::dtls::kDtlsChangeCipherSpec && impl_->dtls) {
        std::span<const std::uint8_t> bytes(pkt.data(), pkt.size());
        // Same peer-address derivation as the DTLS path above.
        nimrtc::dtls::DtlsAddr from;
        if (ice_t_) {
            plugins::Addr raw = ice_t_->remote_addr();
            std::string_view sv(reinterpret_cast<const char*>(raw.data), raw.len);
            std::size_t colon = sv.find(':');
            if (colon != std::string_view::npos) {
                from.host.assign(sv.data(), colon);
                auto port_str = sv.substr(colon + 1);
                if (!port_str.empty()) {
                    int p = std::atoi(std::string(port_str).c_str()); // NOLINT(cert-env33-c)
                    from.port = static_cast<std::uint16_t>(p);
                }
            }
        }
        impl_->dtls->feed_inbound(bytes, from);
        if (is_ice_connected()) {
            drain_dtls();
        }
        return;
    }
    std::vector<std::uint8_t> plaintext;
    core::ByteSpan view(pkt.data(), pkt.size());
    bool tried_srtp = false;
    if (impl_->srtp_installed && impl_->srtp) {
        auto* sess = impl_->srtp->get_session(0, /*outgoing=*/false);
        if (sess) {
            auto r = sess->unprotect_rtp(view, nullptr, nullptr);
            if (r) {
                view = r.value();
                tried_srtp = true;
            } else {
                ++impl_->srtp_stats_drop;
                return;
            }
        }
    }
    rtp::Parser rp;
    auto parsed = rp.parse(view);
    if (parsed) {
        handle_rtp(parsed.value());
        return;
    }
    if (!tried_srtp) {
        core::log::Logger::instance().debug(
            "engine: non-RTP and non-DTLS packet (likely STUN handled by libjuice)");
    }
}

int NimRTCEngine::drain_dtls() noexcept {
    if (!impl_->dtls) return 0;

    // Drive the wolfSSL retransmit timer (RFC 6347 §4.2.4).  Without
    // this pump, a stalled handshake (e.g. lost HelloVerifyRequest
    // cookie exchange) hangs forever.  tick() is a no-op once the
    // session reaches Connected/Failed/Closed.
    impl_->dtls->tick();

    auto recs = impl_->dtls->take_outbound();
    int sent = 0;
    for (auto& rec : recs) {
        plugins::BufferView bv{rec.bytes.data(), rec.bytes.size()};
        if (ice_t_->send(bv, {}) == plugins::kOk) ++sent;
    }

    // Trace DTLS state transitions for debuggability.  Uses a per-engine
    // member (not a static local) so two engines in the same process
    // (e.g. the loopback test) log their transitions independently.
    const auto new_state = impl_->dtls->state();
    if (new_state != impl_->last_dtls_state) {
        core::log::Logger::instance().info(
            "engine[dtls]: state transition " +
            std::string(nimrtc::dtls::DtlsSession::state_name(impl_->last_dtls_state)) + " → " +
            std::string(nimrtc::dtls::DtlsSession::state_name(new_state)));
        impl_->last_dtls_state = new_state;
    }

    if (new_state == nimrtc::dtls::DtlsState::Connected) {
        // SRTP key installation happens here — this is the critical
        // handshake boundary the P1 interop tests verify.
        core::log::Logger::instance().info(
            "engine[dtls→srtp]: DTLS reached Connected, "
            "calling maybe_install_srtp_keys()");
        maybe_install_srtp_keys();
    }
    return sent;
}

void NimRTCEngine::maybe_install_srtp_keys() noexcept {
    // Idempotency: only install once per DTLS connection.
    if (impl_->srtp_installed) {
        core::log::Logger::instance().debug(
            "engine[dtls→srtp]: already installed, skipping");
        return;
    }
    if (!impl_->dtls) {
        core::log::Logger::instance().debug(
            "engine[dtls→srtp]: dtls_ is null, skipping");
        return;
    }
    if (!impl_->srtp) {
        core::log::Logger::instance().debug(
            "engine[dtls→srtp]: srtp_ is null, skipping");
        return;
    }

    // Pre-condition: DTLS MUST be Connected (RFC 5764 §4.2 — keying material
    // is only available after the Finished message).  Re-check inside this
    // function to avoid races between drain_dtls()'s state read and our own.
    const auto dtls_state = impl_->dtls->state();
    if (dtls_state != nimrtc::dtls::DtlsState::Connected) {
        core::log::Logger::instance().debug(
            "engine[dtls→srtp]: dtls_state=" +
            std::to_string(static_cast<int>(dtls_state)) +
            " (not Connected, skipping key install)");
        return;
    }

    auto km = impl_->dtls->srtp_keying_material();
    if (!km) {
        core::log::Logger::instance().warn(
            "engine[dtls→srtp]: dtls reports Connected but no keying material");
        return;
    }

    // RFC 5764 §4.2 — DTLS-SRTP key derivation produces:
    //   client_master_key || client_master_salt || server_master_key || server_master_salt
    // Each side uses its OWN direction's keys for outbound, peer's for inbound.
    // NimRTC uses client_master_key for inbound (we are the controlled/answerer).
    std::vector<std::uint8_t> ck(km->client_master_key.begin(),
                                 km->client_master_key.end());
    std::vector<std::uint8_t> cs(km->client_master_salt.begin(),
                                 km->client_master_salt.end());

    auto& log = core::log::Logger::instance();
    log.info("engine[dtls→srtp]: derived keying material:");
    log.info("  - profile        = 0x" + std::to_string(static_cast<int>(km->profile)));
    log.info("  - key_len        = " + std::to_string(ck.size()));
    log.info("  - salt_len       = " + std::to_string(cs.size()));
    log.info("  - lifetime       = " + std::to_string(km->lifetime));
    if (!ck.empty()) {
        log.info("  - master_key[0..7] = " + [&]{
            std::string s;
            for (std::size_t i = 0; i < std::min<std::size_t>(8, ck.size()); ++i) {
                char buf[8]; std::snprintf(buf, sizeof(buf), "%02x", ck[i]);
                s += buf;
                if (i + 1 < std::min<std::size_t>(8, ck.size())) s += ":";
            }
            return s;
        }());
        log.info("  - master_salt[0..7] = " + [&]{
            std::string s;
            for (std::size_t i = 0; i < std::min<std::size_t>(8, cs.size()); ++i) {
                char buf[8]; std::snprintf(buf, sizeof(buf), "%02x", cs[i]);
                s += buf;
                if (i + 1 < std::min<std::size_t>(8, cs.size())) s += ":";
            }
            return s;
        }());
    }

    // Install the PEER's keys for INBOUND (decrypting packets from Chrome).
    impl_->srtp->derive_keys_for_remote(ck, cs, srtp::CryptoSuite::Aes128CmSha1_80);

    // Install NimRTC's OWN (server) keys for OUTBOUND (encrypting packets to Chrome).
    // RFC 5764 §4.2: each side uses its OWN direction's keys for outbound.
    std::vector<std::uint8_t> sk(km->server_master_key.begin(),
                                 km->server_master_key.end());
    std::vector<std::uint8_t> ss(km->server_master_salt.begin(),
                                 km->server_master_salt.end());
    if (!sk.empty() && !ss.empty()) {
        impl_->srtp->derive_keys_for_local(sk, ss, srtp::CryptoSuite::Aes128CmSha1_80);
    } else {
        core::log::Logger::instance().warn(
            "engine[dtls→srtp]: server_master_key/salt missing — outbound SRTP will fail");
    }

    // Mark installed; subsequent calls will short-circuit.
    impl_->srtp_installed = true;

    log.info("engine[dtls→srtp]: SRTP keys INSTALLED ✓ "
             "(state=" + std::string(state_ == State::kOpen ? "Open" : "Other") +
             ") — RTP from peer will now be unprotectable");
}

uint32_t NimRTCEngine::feed_srtp_inbound(const std::uint8_t* srtp_packet, std::size_t len) noexcept {
    if (!srtp_packet || len == 0 || !impl_->srtp) return core::kEngineInvalidParam;
    auto sess = impl_->srtp->get_session(0, /*outgoing=*/false);
    if (!sess) return 0;
    core::ByteSpan span(srtp_packet, len);
    auto un = sess->unprotect_rtp(span, nullptr, nullptr);
    return un ? 0 : 0xDEAD;
}

bool NimRTCEngine::set_remote_ice(std::string_view ice_block) noexcept {
    // If the transport already exists, apply immediately through the plugin seam.
    if (ice_t_) {
        return ice_t_->set_remote_description(ice_block) == plugins::kOk;
    }
    return false;
}

int NimRTCEngine::add_turn_server(std::string_view host, std::uint16_t port,
                                  std::string_view username,
                                  std::string_view password) noexcept {
    if (!ice_t_) {
        core::log::Logger::instance().error(
            "add_turn_server: ICE transport not yet created — call pre_open() first");
        return -1;
    }
    ice_t_->add_turn_server(host, port, username, password);
    return 0;
}

void NimRTCEngine::handle_rtp(const rtp::PacketView& pv) noexcept {
    // ---- Demux by RTP payload type --------------------------------------
    // The audio_codec PT is configured in EngineConfig; the video codec PT
    // lives in the video_receiver/video_sender tuning. We first check
    // whether this packet matches the video PT (and a video_receiver
    // plugin is wired up) — if so, push it through the video receiver
    // pipeline; otherwise treat it as audio.
    if (video_receiver_ &&
        pv.payload_type == config_.video_receiver_tuning.payload_type) {
        plugins::VideoRtpPacket vp;
        vp.ssrc          = pv.ssrc;
        vp.seq           = pv.seq;
        vp.rtp_timestamp = pv.timestamp;
        vp.payload_type  = pv.payload_type;
        vp.marker        = pv.marker;
        vp.recv_ts_us    = plugins::TimestampUs{
            static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    core::SteadyClock::now().time_since_epoch()).count())};
        // BufferView = core::ByteSpan = std::span<const uint8_t>
        vp.payload       = pv.payload;
        video_receiver_->push_rtp(vp, vp.recv_ts_us);
        video_receiver_->tick(vp.recv_ts_us);
        return;
    }

    // ---- Audio path (existing behaviour) --------------------------------
    auto& jb = impl_->jitter_buffers[pv.ssrc];
    if (!jb) {
        jb::Config jc;
        jc.initial_delay = core::Milliseconds{config_.jb_initial_delay_ms};
        jc.min_delay     = core::Milliseconds{config_.jb_min_delay_ms};
        jc.max_delay     = core::Milliseconds{config_.jb_max_delay_ms};
        jb = std::make_unique<jb::JitterBuffer>(jc);
    }
    jb->push(pv, core::SteadyClock::now(), std::nullopt);
    if (auto frame = jb->pop(core::SteadyClock::now())) {
        if (on_audio_frame_ && !frame->packets.empty()) {
            const auto& p = frame->packets.front();
            if (p.payload.data() != nullptr && p.payload.size() > 0) {
                on_audio_frame_(reinterpret_cast<const float*>(p.payload.data()),
                                p.payload.size() / sizeof(float));
            }
        }
    }
}

void NimRTCEngine::default_on_error(uint32_t err, std::string_view msg) noexcept {
    core::log::Logger::instance().error(
        "ERROR 0x" + ([err, msg] {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "%04X", static_cast<unsigned>(err));
            return std::string(hex) + ": " + std::string(msg);
        })());
}

// ---------------------------------------------------------------------------
// Out-of-line inspection accessors
//
// These used to be `inline` in the public header.  They reference concrete
// module state (DTLS, SRTP, ICE plugin) and therefore pull in concrete
// module headers — keeping them out-of-line lets engine.hpp stay free of
// those headers (Layout Invariant 4).
// ---------------------------------------------------------------------------

nimrtc::dtls::DtlsState NimRTCEngine::dtls_state() const noexcept {
    return impl_->dtls ? impl_->dtls->state() : nimrtc::dtls::DtlsState::Closed;
}

bool NimRTCEngine::dtls_connected() const noexcept {
    return impl_->dtls && impl_->dtls->is_connected();
}

bool NimRTCEngine::is_ice_connected() const noexcept {
    if (!ice_t_) return false;
    const auto s = ice_t_->state();
    return s == plugins::IceState::Connected ||
           s == plugins::IceState::Completed;
}

bool NimRTCEngine::srtp_installed() const noexcept {
    return impl_->srtp_installed;
}

uint32_t NimRTCEngine::last_open_rc() const noexcept {
    return impl_->last_open_rc;
}

NimRTCEngine::Stats NimRTCEngine::stats() const noexcept {
    return Stats{impl_->srtp_stats_drop};
}

} // namespace nimrtc::engine
