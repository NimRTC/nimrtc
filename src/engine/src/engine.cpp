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

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <random>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/time.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/ice/ice.hpp>
#include <nimrtc/plugins/transport.hpp>
#include <nimrtc/plugins/audio3a.hpp>

// Concrete module headers — required by NimRTCEngine::Impl which lives in
// this translation unit.  The public header (engine.hpp) does NOT include
// any of these; consumers only need the plugin interfaces.
#include <nimrtc/sdp/session_description.hpp>
#include <nimrtc/rtp/packet.hpp>
#include <nimrtc/jb/jitter_buffer.hpp>
#include <nimrtc/audio3a/audio3a.hpp>
#include <nimrtc/dtls/dtls.hpp>
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
    std::unique_ptr<dtls::DtlsSession> dtls;
    std::unique_ptr<srtp::SrtpContext> srtp;
#ifdef NIMRTC_HAS_OPUS
    std::unique_ptr<opus::Encoder>     opus_encoder;
    std::unique_ptr<opus::Decoder>     opus_decoder;
#endif

    bool            dtls_active_inbound = false;
    bool            srtp_installed      = false;
    int             srtp_stats_drop     = 0;
    dtls::DtlsState last_dtls_state     = dtls::DtlsState::Closed;

    // Local SDP (after create_offer / process_remote_sdp).
    std::optional<sdp::SessionDescription> local_sdp;

    uint32_t        last_open_rc = 0;
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

NimRTCEngine::NimRTCEngine(EngineConfig config)
    : config_(std::move(config)), impl_(new Impl()) {}

NimRTCEngine::~NimRTCEngine() {
    if (state_ == State::kOpen) {
        close();
    }
    delete impl_;
    impl_ = nullptr;
}

    uint32_t NimRTCEngine::pre_open() noexcept {
    if (state_ != State::kConstructed) return 0x1002;
    core::log::Logger::instance().debug("pre_open: starting");

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
        if (!tfactory) { impl_->last_open_rc = 0x1FFF; if (on_error_) on_error_(0x1FFF, "transport plugin not found"); return 0x1FFF; }
        plugins::ITransport* raw = tfactory->create();
        if (!raw) { impl_->last_open_rc = 0x1FFF; if (on_error_) on_error_(0x1FFF, "transport factory returned null"); return 0x1FFF; }
        // Last-resort safety net: a generic ITransport that also happens
        // to implement IICETransport.  This is the only place the engine
        // still does a cross-class dynamic_cast, and it's intentional —
        // it lets out-of-tree ICE transports (or a test double) work
        // without registering the ICE-specific factory.
        ice_t_.reset(dynamic_cast<plugins::IICETransport*>(raw));
        if (!ice_t_) {
            impl_->last_open_rc = 0x1FFF;
            if (on_error_) on_error_(0x1FFF, "transport plugin does not implement IICETransport");
            delete raw;
            return 0x1FFF;
        }
    }

    // Apply pre-open configuration through the plugin interface — no
    // dynamic_cast to `ice::IceTransport` needed.
    ice_t_->set_bind_address(config_.local_bind_address);
    ice_t_->set_stun_server(config_.stun_server_host, config_.stun_server_port);
    ice_t_->set_local_port_range(config_.local_port_range_begin,
                                  config_.local_port_range_end);

    // ---- Remaining modules (same as open()) -----------------------------
    // Audio3A, Codec, SRTP, DTLS, SDP
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
                if (on_error_) on_error_(0x1A00, "audio3a plugin open failed; falling back to concrete");
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

    {
        plugins::CodecConfig codec_cfg{};
        codec_cfg.sample_rate_hz = config_.pcm_sample_rate_hz;
        codec_cfg.channels       = config_.pcm_channels;
        codec_cfg.bitrate_bps   = 64000;
        codec_cfg.complexity    = 10;
        codec_cfg.fec_enabled  = true;
        codec_cfg.dtx_enabled  = false;
        codec_cfg.vad_enabled  = false;
        codec_cfg.payload_type = config_.audio_codec.payload_type;
        codec_cfg.name         = config_.codec_name;
        const plugins::ICodecFactory* cf = reg.get_codec(config_.codec_name);
        if (cf) {
            codec_plugin_.reset(cf->create(codec_cfg));
            if (codec_plugin_ && codec_plugin_->open() == plugins::kOk) {
                // ok
            } else {
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

    impl_->srtp = std::make_unique<srtp::SrtpContext>();
    core::log::Logger::instance().debug("engine: srtp_ created, about to create dtls_");

    dtls::Config dcfg;
    dcfg.role = dtls::DtlsRole::Server;
    dcfg.srtp_profile = dtls::SrtpProfile::Aes128CmSha1_80;
    impl_->dtls = std::make_unique<dtls::DtlsSession>(dcfg);
    core::log::Logger::instance().debug("engine: dtls_ created, calling dtls_->open()");
    if (!impl_->dtls->open()) { impl_->last_open_rc = 0x2000; core::log::Logger::instance().error("pre_open: dtls_->open() returned false"); if (on_error_) on_error_(0x2000, "DTLS open failed"); return 0x2000; }
    core::log::Logger::instance().debug("pre_open: dtls_->open() returned ok");

    ice_t_->set_callbacks(
        [this](plugins::BufferView bv) { on_transport_recv(bv); },
        [this](plugins::Status err, std::string_view msg) {
            if (on_error_) on_error_(static_cast<std::uint32_t>(err), msg);
        });

    // State stays kConstructed — open() will call ice_t_->open()
    // and set state to kOpen when invoked.
    return 0;
}

uint32_t NimRTCEngine::open() noexcept {
    if (state_ != State::kConstructed) return 0x1002;

    // If pre_open() already created the modules, skip recreation and go straight
    // to opening the transport (which triggers gather_candidates with the
    // correct role set by set_remote_ice()).
    if (!ice_t_) {
        // Normal path: modules not yet created — run the full init.
        core::register_all_default_plugins();
        auto& reg = core::PluginRegistry::instance();

        // Preferred path: ICE-aware factory (see pre_open() for the
        // longer rationale on why we look up IICETransportFactory first
        // and only fall back to a generic ITransportFactory as a safety
        // net for out-of-tree transports).
        const plugins::IICETransportFactory* ifactory =
            reg.get_ice_transport(config_.transport_name);
        if (ifactory) {
            ice_t_.reset(ifactory->create_ice());
        } else {
            const plugins::ITransportFactory* tfactory =
                reg.get_transport(config_.transport_name);
            if (!tfactory) { if (on_error_) on_error_(0x1FFF, "transport plugin not found"); return 0x1FFF; }
            plugins::ITransport* raw = tfactory->create();
            if (!raw) { if (on_error_) on_error_(0x1FFF, "transport factory returned null"); return 0x1FFF; }
            ice_t_.reset(dynamic_cast<plugins::IICETransport*>(raw));
            if (!ice_t_) {
                if (on_error_) on_error_(0x1FFF, "transport plugin does not implement IICETransport");
                delete raw;
                return 0x1FFF;
            }
        }

        // Apply pre-open configuration through the plugin interface.
        ice_t_->set_bind_address(config_.local_bind_address);
        ice_t_->set_stun_server(config_.stun_server_host, config_.stun_server_port);
        ice_t_->set_local_port_range(config_.local_port_range_begin,
                                      config_.local_port_range_end);

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
                    if (on_error_) on_error_(0x1A00, "audio3a plugin open failed; falling back to concrete");
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

        {
            plugins::CodecConfig codec_cfg{};
            codec_cfg.sample_rate_hz = config_.pcm_sample_rate_hz;
            codec_cfg.channels       = config_.pcm_channels;
            codec_cfg.bitrate_bps   = 64000;
            codec_cfg.complexity    = 10;
            codec_cfg.fec_enabled  = true;
            codec_cfg.dtx_enabled  = false;
            codec_cfg.vad_enabled  = false;
            codec_cfg.payload_type = config_.audio_codec.payload_type;
            codec_cfg.name         = config_.codec_name;
            const plugins::ICodecFactory* cf = reg.get_codec(config_.codec_name);
            if (cf) {
                codec_plugin_.reset(cf->create(codec_cfg));
                if (codec_plugin_ && codec_plugin_->open() == plugins::kOk) {
                    // ok
                } else {
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

        impl_->srtp = std::make_unique<srtp::SrtpContext>();
        core::log::Logger::instance().debug("engine.open: srtp_ created, about to create dtls_");

        dtls::Config dcfg;
        dcfg.role = dtls::DtlsRole::Server;
        dcfg.srtp_profile = dtls::SrtpProfile::Aes128CmSha1_80;
        impl_->dtls = std::make_unique<dtls::DtlsSession>(dcfg);
        core::log::Logger::instance().debug("engine.open: dtls_ created, calling dtls_->open()");
        if (!impl_->dtls->open()) { impl_->last_open_rc = 0x2000; core::log::Logger::instance().error("engine.open: dtls_->open() returned false"); if (on_error_) on_error_(0x2000, "DTLS open failed"); return 0x2000; }
        core::log::Logger::instance().debug("engine.open: dtls_->open() returned ok");

        ice_t_->set_callbacks(
            [this](plugins::BufferView bv) { on_transport_recv(bv); },
            [this](plugins::Status err, std::string_view msg) {
                if (on_error_) on_error_(static_cast<std::uint32_t>(err), msg);
            });
    } else {
        // pre_open() was called — modules already created.  Open the
        // transport to trigger ICE gathering (transport is already configured).
        core::log::Logger::instance().debug("op7b transport pre_open→open");
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
        impl_->last_open_rc = 0x1FFF;
        core::log::Logger::instance().error(std::string(err));
        if (on_error_) on_error_(0x1FFF, err);
        ice_t_.reset();
        return 0x1FFF;
    }
    core::log::Logger::instance().debug("op7 transport open ok");

    // ---- Video plugins (R3-Batch) ---------------------------------------
    // Resolves all four video_* plugins from core::PluginRegistry and
    // opens them. Failures are non-fatal at engine level (R3 scope: just
    // expose the accessors); warnings go through on_error_.
    init_video_plugins();

    // ---- Wire callbacks (if not already done by pre_open) ----------------
    ice_t_->set_callbacks(
        [this](plugins::BufferView bv) { on_transport_recv(bv); },
        [this](plugins::Status err, std::string_view msg) {
            if (on_error_) on_error_(static_cast<std::uint32_t>(err), msg);
        });

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
        plugins::VideoSinkConfig cfg{};
        if (video_sink_->open() != plugins::kOk) {
            if (on_error_) on_error_(0x1A20, "video_sink open failed");
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
            if (on_error_) on_error_(0x1A21, "video_source open failed");
            video_source_.reset();
        }
    }
}

void NimRTCEngine::init_video_plugins() noexcept {
    auto& reg = core::PluginRegistry::instance();

    // ---- Video Sink (independent of others; safest to open first) --------
    if (!video_sink_) {
        const auto* f = reg.get_video_sink(config_.video_sink_name);
        if (f) {
            plugins::VideoSinkConfig cfg{};
            video_sink_.reset(f->create(cfg));
            if (video_sink_) {
                if (video_sink_->open() != plugins::kOk) {
                    if (on_error_) on_error_(0x1A20,
                        "video_sink plugin open failed");
                    video_sink_.reset();
                }
            }
        } else if (on_error_) {
            on_error_(0x1A22,
                std::string("video_sink plugin not found: ")
                    .append(config_.video_sink_name));
        }
    }

    // ---- Video Source ----------------------------------------------------
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
                    if (on_error_) on_error_(0x1A21,
                        "video_source plugin open failed");
                    video_source_.reset();
                }
            }
        } else if (on_error_) {
            on_error_(0x1A23,
                std::string("video_source plugin not found: ")
                    .append(config_.video_source_name));
        }
    }

    // ---- Video Receiver --------------------------------------------------
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
                    if (on_error_) on_error_(0x1A24,
                        "video_receiver plugin open failed");
                    video_receiver_.reset();
                } else {
                    // Bridge receiver frame callback → engine's
                    // on_video_frame_ (H.264 NAL delivery) so existing
                    // engine clients see the same callback shape.
                    // Decoded rendering is up to the user via video_sink().
                    video_receiver_->set_frame_callback(
                        [this](const plugins::EncodedVideoFrame& f,
                               plugins::TimestampUs /*now_us*/) {
                            if (on_video_frame_) {
                                on_video_frame_(f.payload.data(),
                                                f.payload.size(),
                                                f.is_keyframe);
                            }
                        });
                }
            }
        } else if (on_error_) {
            on_error_(0x1A25,
                std::string("video_receiver plugin not found: ")
                    .append(config_.video_receiver_name));
        }
    }

    // ---- Video Sender ----------------------------------------------------
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
                    if (on_error_) on_error_(0x1A26,
                        "video_sender plugin open failed");
                    video_sender_.reset();
                }
            }
        } else if (on_error_) {
            on_error_(0x1A27,
                std::string("video_sender plugin not found: ")
                    .append(config_.video_sender_name));
        }
    }
}

void NimRTCEngine::shutdown_video_plugins() noexcept {
    auto close_all = [](auto& p) {
        if (p) p->close();
        p.reset();
    };
    close_all(video_source_);
    close_all(video_sink_);
    close_all(video_receiver_);
    close_all(video_sender_);
}

void NimRTCEngine::close() noexcept {
    if (state_ != State::kOpen) return;
    if (ice_t_) ice_t_->close();
    if (audio3a_plugin_) audio3a_plugin_->close();
    if (codec_plugin_) codec_plugin_->close();
    shutdown_video_plugins();
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

    if (ice_t_) {
        auto gathered = ice_t_->gathered_local_candidates();
        for (auto& c : gathered) {
            // libjuice emits candidates with the "a=candidate:" prefix.
            // The munger adds its own "a=candidate:" so strip the prefix.
            constexpr std::string_view kFullPrefix = "a=candidate:";
            constexpr std::string_view kHalfPrefix = "candidate:";
            if (c.size() >= kFullPrefix.size() &&
                c.substr(0, kFullPrefix.size()) == kFullPrefix) {
                audio.candidates.push_back(std::string(c.substr(kFullPrefix.size())));
            } else if (c.size() >= kHalfPrefix.size() &&
                       c.substr(0, kHalfPrefix.size()) == kHalfPrefix) {
                audio.candidates.push_back(std::string(c.substr(kHalfPrefix.size())));
            } else {
                audio.candidates.push_back(c);
            }
        }
    }

    sdp.media.push_back(std::move(audio));

    auto sdp_str = impl_->sdp_munger->to_sdp(sdp);
    if (!sdp_str) {
        if (on_error_) on_error_(0x1004, "SDP munger failed");
        return {};
    }
    impl_->local_sdp = std::move(sdp);
    return sdp_str.value();
}

std::optional<std::string>
NimRTCEngine::process_remote_sdp(std::string_view remote_sdp) noexcept {
    auto parsed = impl_->sdp_parser->parse(remote_sdp);
    if (!parsed) {
        if (on_error_) on_error_(0x1004, "SDP parse failed");
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
            + " candidates=" + std::to_string(m.candidates.size()));
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

    // Configure DTLS role + peer fingerprint from the first audio m-line.
    // IMPORTANT: do NOT recreate the DtlsSession here.  The local certificate
    // (and therefore the advertised local fingerprint) is generated once at
    // pre_open(); recreating it would invalidate the fingerprint already
    // advertised in our SDP and break the handshake.  We only update the
    // role + peer fingerprint, and set_role() handles driving the state
    // machine when the role changes (e.g., answerer promoting Server -> Client).
    for (const auto& rm : remote.media) {
        if (rm.type != sdp::MediaType::Audio) continue;
        if (impl_->dtls) {
            dtls::DtlsRole desired_role = dtls::DtlsRole::Server;
            if      (rm.dtls_setup == "active")  desired_role = dtls::DtlsRole::Server;
            else if (rm.dtls_setup == "passive") desired_role = dtls::DtlsRole::Client;
            else                                 desired_role = dtls::DtlsRole::Client;
            impl_->dtls->set_role(desired_role);

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
        if (rm.type != sdp::MediaType::Audio) continue;
        sdp::MediaDescription am;
        am.type            = sdp::MediaType::Audio;
        am.port            = 9;
        am.protocol        = rm.protocol.empty() ? "UDP/TLS/RTP/SAVPF" : rm.protocol;
        am.formats         = rm.formats;
        am.direction       = sdp::Direction::SendRecv;
        am.mid             = rm.mid;
        am.rtcp_mux_value  = "rtcp-mux";
        am.ice_ufrag       = local_ufrag();
        am.ice_pwd         = local_password();
        am.dtls_setup      = (rm.dtls_setup == "active") ? "passive" : "active";
        if (impl_->dtls) {
            am.dtls_fingerprint_algo  = "sha-256";
            am.dtls_fingerprint_value = impl_->dtls->local_fingerprint().hex_colon;
        }
        am.rtpmap = rm.rtpmap;
        am.fmtp   = rm.fmtp;

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

    auto ans_str = impl_->sdp_munger->to_sdp(ans);
    if (!ans_str) {
        if (on_error_) on_error_(0x1004, "SDP munger failed");
        return std::nullopt;
    }
    return ans_str.value();
}

uint32_t NimRTCEngine::send_audio(const float* pcm_samples, std::size_t num_samples) noexcept {
    if (!is_open()) return 0x1002;
    if (!pcm_samples || num_samples == 0) return 0x1001;

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

    static thread_local std::vector<std::uint8_t> codec_pkt;
    codec_pkt.resize(num_samples * sizeof(float) + 64);
    // Codec — prefer plugin adapter (R2-Batch1); fall back to concrete opus::Encoder.
    std::size_t codec_len = 0;
    if (codec_plugin_) {
        codec_len = codec_plugin_->encode(pcm_samples, num_samples,
                                         codec_pkt.data(), codec_pkt.size());
#ifdef NIMRTC_HAS_OPUS
    } else if (impl_->opus_encoder) {
        codec_len = impl_->opus_encoder->encode(pcm_samples, num_samples,
                                         codec_pkt.data(), codec_pkt.size());
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
      .set_payload(core::ByteSpan(codec_pkt.data(), codec_len));
    auto pkt_buf = pb.build();
    if (pkt_buf.empty()) return 0;

    std::vector<std::uint8_t> send_buf;
    core::ByteSpan out_payload(pkt_buf.data(), pkt_buf.size());
    if (impl_->srtp_installed && impl_->srtp) {
        auto* sess = impl_->srtp->get_session(curr_ssrc);
        if (sess) {
            auto enc = sess->protect_rtp(out_payload, curr_ssrc, cur_ts);
            if (enc) {
                send_buf.assign(enc.value().data(),
                                enc.value().data() + enc.value().size());
            } else {
                return 0;
            }
        }
    } else {
        send_buf.assign(out_payload.data(),
                        out_payload.data() + out_payload.size());
    }

    plugins::BufferView bv{send_buf.data(), send_buf.size()};
    return ice_t_->send(bv, {});
}

int NimRTCEngine::tick() noexcept {
    if (!is_open()) return 0;
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

void NimRTCEngine::on_transport_recv(const plugins::BufferView& pkt) noexcept {
    if (pkt.empty() || pkt.size() < 1) return;
    std::uint8_t b0 = pkt.data()[0];
    bool looks_like_dtls = (b0 == 22 || b0 == 23 || b0 == 21);
    if (looks_like_dtls && impl_->dtls) {
        core::log::Logger::instance().info(
            std::string("engine[dtls]: rx dtls ct=") + std::to_string(b0) +
            " len=" + std::to_string(pkt.size()) +
            " state=" + std::string(dtls::DtlsSession::state_name(impl_->dtls->state())));
        std::span<const std::uint8_t> bytes(pkt.data(), pkt.size());
        dtls::DtlsAddr from;
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
    if (b0 == dtls::kDtlsChangeCipherSpec && impl_->dtls) {
        std::span<const std::uint8_t> bytes(pkt.data(), pkt.size());
        dtls::DtlsAddr from;
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
        auto* sess = impl_->srtp->get_session(0);
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
            std::string(dtls::DtlsSession::state_name(impl_->last_dtls_state)) + " → " +
            std::string(dtls::DtlsSession::state_name(new_state)));
        impl_->last_dtls_state = new_state;
    }

    if (new_state == dtls::DtlsState::Connected) {
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
    if (dtls_state != dtls::DtlsState::Connected) {
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

    // Install on the SrtpContext (used for inbound unprotect).
    impl_->srtp->derive_keys_for_remote(ck, cs, srtp::CryptoSuite::Aes128CmSha1_80);

    // Mark installed; subsequent calls will short-circuit.
    impl_->srtp_installed = true;

    log.info("engine[dtls→srtp]: SRTP keys INSTALLED ✓ "
             "(state=" + std::string(state_ == State::kOpen ? "Open" : "Other") +
             ") — RTP from peer will now be unprotectable");
}

uint32_t NimRTCEngine::feed_srtp_inbound(const std::uint8_t* srtp_packet, std::size_t len) noexcept {
    if (!srtp_packet || len == 0 || !impl_->srtp) return 0x1001;
    auto sess = impl_->srtp->get_session(0);
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

void NimRTCEngine::handle_rtp(const rtp::PacketView& pv) noexcept {
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
        std::format("ERROR 0x{:04X}: {}", err, msg));
}

// ---------------------------------------------------------------------------
// Out-of-line inspection accessors
//
// These used to be `inline` in the public header.  They reference concrete
// module state (DTLS, SRTP, ICE plugin) and therefore pull in concrete
// module headers — keeping them out-of-line lets engine.hpp stay free of
// those headers (Layout Invariant 4).
// ---------------------------------------------------------------------------

dtls::DtlsState NimRTCEngine::dtls_state() const noexcept {
    return impl_->dtls ? impl_->dtls->state() : dtls::DtlsState::Closed;
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
