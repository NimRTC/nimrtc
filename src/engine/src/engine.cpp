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
 * gathered_local_candidates()) are accessed via dynamic_cast<IceTransport*>
 * since the plugin interface doesn't expose them.
 */

#include "nimrtc/engine/engine.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <random>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/time.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/ice/ice.hpp>
#include <nimrtc/plugins/transport.hpp>
#include <nimrtc/plugins/audio3a.hpp>

namespace nimrtc::engine {

namespace {

void trace(const char* fmt, ...) noexcept {
#if defined(_DEBUG) || defined(NIMRTC_TRACE)
    (void)fmt;
    std::fprintf(stderr, "[NimRTCEngine] ");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n");
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

} // anonymous namespace

struct NimRTCEngine::SdpImpl {
    std::unique_ptr<sdp::Parser> parser = std::make_unique<sdp::Parser>();
    std::unique_ptr<sdp::Munger> munger = std::make_unique<sdp::Munger>();
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

NimRTCEngine::NimRTCEngine(EngineConfig config)
    : config_(std::move(config)) {}

NimRTCEngine::~NimRTCEngine() {
    if (state_ == State::kOpen) {
        close();
    }
}

uint32_t NimRTCEngine::open() noexcept {
    if (state_ != State::kConstructed) return 0x1002;

    auto& reg = core::PluginRegistry::instance();

    // ---- Plugin: Transport --------------------------------------------
    const plugins::ITransportFactory* tfactory =
        reg.get_transport(config_.transport_name);
    if (!tfactory) {
        if (on_error_) on_error_(0x1FFF, "transport plugin not found");
        return 0x1FFF;
    }
    transport_.reset(tfactory->create());
    if (!transport_) {
        if (on_error_) on_error_(0x1FFF, "transport factory returned null");
        return 0x1FFF;
    }
    std::fprintf(stderr, "[debug] op5 transport created\n");
    // Cast back to concrete IceTransport for ICE-specific methods.
    (void)dynamic_cast<ice::IceTransport*>(transport_.get());
    std::fprintf(stderr, "[debug] op6 dynamic_cast ok\n");

    // Forward port-range config to the concrete IceTransport so that two
    // engines on the same host bind to disjoint UDP ports (otherwise both
    // bind the same OS-allocated port and process each other's STUN).
    if (auto* ice = dynamic_cast<ice::IceTransport*>(transport_.get())) {
        ice->set_bind_address(config_.local_bind_address);
        ice->set_stun_server(config_.stun_server_host,
                             config_.stun_server_port);
        ice->set_local_port_range(config_.local_port_range_begin,
                                  config_.local_port_range_end);
    }

    if (transport_->open() != plugins::kOk) {
        if (on_error_) on_error_(0x1FFF, "transport open failed");
        transport_.reset();
        return 0x1FFF;
    }
    std::fprintf(stderr, "[debug] op7 transport open ok\n");
    audio3a_concrete_ = std::make_unique<audio3a::NullAudio3A>();
    audio3a::Config a3a;
    a3a.sample_rate_hz   = config_.pcm_sample_rate_hz;
    a3a.capture_channels = config_.pcm_channels;
    a3a.render_channels  = config_.pcm_channels;
    audio3a_concrete_->init(a3a);

    std::fprintf(stderr, "[debug] op9 audio3a ok\n");
    // ---- Opus codec (stub) --------------------------------------------
    opus::EncoderConfig ec;
    ec.sample_rate_hz = config_.pcm_sample_rate_hz;
    ec.channels       = config_.pcm_channels;
    opus_encoder_ = std::make_unique<opus::Encoder>(ec);

    opus::DecoderConfig dc;
    dc.sample_rate_hz = config_.pcm_sample_rate_hz;
    dc.channels       = config_.pcm_channels;
    opus_decoder_ = std::make_unique<opus::Decoder>(dc);

    std::fprintf(stderr, "[debug] op10 opus ok\n");
    // ---- SRTP / DTLS --------------------------------------------------
    srtp_ = std::make_unique<srtp::SrtpContext>();

    dtls::Config dcfg;
    dcfg.role = dtls::DtlsRole::Server;  // flipped when remote SDP arrives
    dcfg.srtp_profile = dtls::SrtpProfile::Aes128CmSha1_80;
    dtls_ = std::make_unique<dtls::DtlsSession>(dcfg);
    if (!dtls_->open()) {
        if (on_error_) on_error_(0x2000, "DTLS open failed");
        return 0x2000;
    }
    std::fprintf(stderr, "[debug] op11 dtls ok\n");

    // ---- SDP impl ------------------------------------------------------
    sdp_impl_ = std::make_unique<SdpImpl>();

    // ---- Wire transport callbacks --------------------------------------
    transport_->set_callbacks(
        [this](plugins::BufferView bv) { on_transport_recv(bv); },
        [this](plugins::Status err, std::string_view msg) {
            if (on_error_) on_error_(static_cast<std::uint32_t>(err), msg);
        });

    state_ = State::kOpen;
    if (on_state_change_) on_state_change_("open");
    trace("NimRTCEngine opened (DTLS, SRTP, Opus initialised)");
    return 0;
}

void NimRTCEngine::close() noexcept {
    if (state_ != State::kOpen) return;
    if (transport_) transport_->close();
    transport_.reset();
    jitter_buffers_.clear();
    sdp_impl_.reset();
    audio3a_concrete_.reset();
    opus_encoder_.reset();
    opus_decoder_.reset();
    dtls_.reset();
    srtp_.reset();
    state_ = State::kClosed;
    if (on_state_change_) on_state_change_("closed");
}

std::string NimRTCEngine::create_offer() noexcept {
    ice::IceTransport* ice = dynamic_cast<ice::IceTransport*>(transport_.get());
    if (ice) ice->wait_for_gathering(1500);

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
    audio.formats         = {"111"};
    audio.direction       = sdp::Direction::SendRecv;
    audio.mid             = "0";
    audio.rtcp_mux_value  = "rtcp-mux";
    audio.ice_ufrag       = ice ? ice->local_ufrag() : "";
    audio.ice_pwd         = ice ? ice->local_password() : "";
    audio.ice_options     = "trickle";
    audio.dtls_setup      = "actpass";

    if (dtls_) {
        audio.dtls_fingerprint_algo  = "sha-256";
        audio.dtls_fingerprint_value = dtls_->local_fingerprint().base64;
    }

    sdp::MediaDescription::RtpMap opus_map;
    opus_map.encoding   = "opus";
    opus_map.clock_rate = 48000;
    opus_map.channels   = 2;
    audio.rtpmap["111"] = opus_map;
    audio.fmtp["111"]   = "minptime=10;useinbandfec=1;stereo=0;sprop-stereo=0";

    if (ice) {
        auto gathered = ice->gathered_local_candidates();
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

    auto sdp_str = sdp_impl_->munger->to_sdp(sdp);
    if (!sdp_str) {
        if (on_error_) on_error_(0x1004, "SDP munger failed");
        return {};
    }
    local_sdp_ = std::move(sdp);
    return sdp_str.value();
}

std::optional<std::string>
NimRTCEngine::process_remote_sdp(std::string_view remote_sdp) noexcept {
    auto parsed = sdp_impl_->parser->parse(remote_sdp);
    if (!parsed) {
        if (on_error_) on_error_(0x1004, "SDP parse failed");
        return std::nullopt;
    }

    const auto& remote = parsed.value();
    ice::IceTransport* ice = dynamic_cast<ice::IceTransport*>(transport_.get());

    // Apply ICE credentials + candidates.
    std::string ice_block;
    for (const auto& m : remote.media) {
        if (m.ice_ufrag.empty() || m.ice_pwd.empty()) continue;
        ice_block += "a=ice-ufrag:" + m.ice_ufrag + "\n";
        ice_block += "a=ice-pwd:"   + m.ice_pwd   + "\n";
        for (const auto& c : m.candidates) {
            ice_block += "a=candidate:" + c + "\n";
        }
        break;
    }
    if (!ice_block.empty() && ice) {
        ice->set_remote_description(ice_block);
    }

    // Configure DTLS role + peer fingerprint from the first audio m-line.
    for (const auto& rm : remote.media) {
        if (rm.type != sdp::MediaType::Audio) continue;
        if (dtls_) {
            dtls_->close();
            dtls::Config dcfg;
            dcfg.srtp_profile = dtls::SrtpProfile::Aes128CmSha1_80;
            if      (rm.dtls_setup == "active")   dcfg.role = dtls::DtlsRole::Server;
            else if (rm.dtls_setup == "passive")  dcfg.role = dtls::DtlsRole::Client;
            else                                 dcfg.role = dtls::DtlsRole::Client;
            if (!rm.dtls_fingerprint_algo.empty() &&
                !rm.dtls_fingerprint_value.empty()) {
                dcfg.peer_fingerprint_value = base64_decode(rm.dtls_fingerprint_value);
                dcfg.peer_fingerprint_algo = rm.dtls_fingerprint_algo;
            }
            dtls_ = std::make_unique<dtls::DtlsSession>(dcfg);
            if (!dtls_->open() && on_error_) on_error_(0x2000, "DTLS re-open failed");
        }
        break;
    }

    if (ice) ice->wait_for_gathering(1500);

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
        if (dtls_) {
            am.dtls_fingerprint_algo  = "sha-256";
            am.dtls_fingerprint_value = dtls_->local_fingerprint().base64;
        }
        am.rtpmap = rm.rtpmap;
        am.fmtp   = rm.fmtp;

        if (ice) {
            auto gathered = ice->gathered_local_candidates();
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

    auto ans_str = sdp_impl_->munger->to_sdp(ans);
    if (!ans_str) {
        if (on_error_) on_error_(0x1004, "SDP munger failed");
        return std::nullopt;
    }
    return ans_str.value();
}

uint32_t NimRTCEngine::send_audio(const float* pcm_samples, std::size_t num_samples) noexcept {
    if (!is_open()) return 0x1002;
    if (!pcm_samples || num_samples == 0) return 0x1001;

    if (audio3a_concrete_) {
        audio3a::Frame frame;
        frame.samples        = const_cast<float*>(pcm_samples);
        frame.num_samples    = num_samples;
        frame.num_channels   = config_.pcm_channels;
        frame.sample_rate_hz = config_.pcm_sample_rate_hz;
        audio3a_concrete_->process_capture(frame);
    }

    static thread_local std::vector<std::uint8_t> opus_pkt;
    opus_pkt.resize(num_samples * sizeof(float) + 64);
    std::size_t opus_len = opus_encoder_->encode(pcm_samples, num_samples,
                                                 opus_pkt.data(), opus_pkt.size());
    if (opus_len == 0) return 0;

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
      .set_payload_type(111)
      .set_marker(false)
      .set_payload(core::ByteSpan(opus_pkt.data(), opus_len));
    auto pkt_buf = pb.build();
    if (pkt_buf.empty()) return 0;

    std::vector<std::uint8_t> send_buf;
    core::ByteSpan out_payload(pkt_buf.data(), pkt_buf.size());
    if (srtp_installed_ && srtp_) {
        auto* sess = srtp_->get_session(curr_ssrc);
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
    return transport_->send(bv, {});
}

int NimRTCEngine::tick() noexcept {
    if (!is_open()) return 0;
    drain_dtls();
    return transport_->recv();
}

const char* NimRTCEngine::ice_state_string() const noexcept {
    ice::IceTransport* ice = dynamic_cast<ice::IceTransport*>(transport_.get());
    if (!ice) return "closed";
    switch (ice->state()) {
        case ice::IceState::Disconnected: return "disconnected";
        case ice::IceState::Gathering:    return "gathering";
        case ice::IceState::Connecting:   return "connecting";
        case ice::IceState::Connected:    return "connected";
        case ice::IceState::Completed:    return "completed";
        case ice::IceState::Failed:       return "failed";
    }
    return "unknown";
}

std::string NimRTCEngine::local_ufrag() const noexcept {
    ice::IceTransport* ice = dynamic_cast<ice::IceTransport*>(transport_.get());
    return ice ? ice->local_ufrag() : std::string{};
}

std::string NimRTCEngine::local_password() const noexcept {
    ice::IceTransport* ice = dynamic_cast<ice::IceTransport*>(transport_.get());
    return ice ? ice->local_password() : std::string{};
}

void NimRTCEngine::on_transport_recv(const plugins::BufferView& pkt) noexcept {
    if (pkt.empty() || pkt.size() < 1) return;
    std::uint8_t b0 = pkt.data()[0];
    bool looks_like_dtls = (b0 == 22 || b0 == 23 || b0 == 21);
    if (looks_like_dtls && dtls_) {
        std::span<const std::uint8_t> bytes(pkt.data(), pkt.size());
        dtls::DtlsAddr from;
        dtls_->feed_inbound(bytes, from);
        drain_dtls();
        return;
    }
    std::vector<std::uint8_t> plaintext;
    core::ByteSpan view(pkt.data(), pkt.size());
    bool tried_srtp = false;
    if (srtp_installed_ && srtp_) {
        auto* sess = srtp_->get_session(0);
        if (sess) {
            auto r = sess->unprotect_rtp(view, nullptr, nullptr);
            if (r) {
                view = r.value();
                tried_srtp = true;
            } else {
                ++srtp_stats_drop_;
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
    if (!dtls_) return 0;
    auto recs = dtls_->take_outbound();
    int sent = 0;
    for (auto& rec : recs) {
        plugins::BufferView bv{rec.bytes.data(), rec.bytes.size()};
        if (transport_->send(bv, {}) == plugins::kOk) ++sent;
    }
    if (dtls_->state() == dtls::DtlsState::Connected) {
        maybe_install_srtp_keys();
    }
    return sent;
}

void NimRTCEngine::maybe_install_srtp_keys() noexcept {
    if (srtp_installed_) return;
    if (!dtls_ || !srtp_) return;
    auto km = dtls_->srtp_keying_material();
    if (!km) return;
    std::vector<std::uint8_t> ck(km->client_master_key.begin(),
                                 km->client_master_key.end());
    std::vector<std::uint8_t> cs(km->client_master_salt.begin(),
                                 km->client_master_salt.end());
    srtp_->derive_keys_for_remote(ck, cs, srtp::CryptoSuite::Aes128CmSha1_80);
    srtp_installed_ = true;
    core::log::Logger::instance().info(
        "engine: SRTP keys installed (DTLS complete)");
}

uint32_t NimRTCEngine::feed_srtp_inbound(const std::uint8_t* srtp_packet, std::size_t len) noexcept {
    if (!srtp_packet || len == 0 || !srtp_) return 0x1001;
    auto sess = srtp_->get_session(0);
    if (!sess) return 0;
    core::ByteSpan span(srtp_packet, len);
    auto un = sess->unprotect_rtp(span, nullptr, nullptr);
    return un ? 0 : 0xDEAD;
}

void NimRTCEngine::handle_rtp(const rtp::PacketView& pv) noexcept {
    auto& jb = jitter_buffers_[pv.ssrc];
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
    std::fprintf(stderr, "[NimRTCEngine] ERROR 0x%04X: %.*s\n",
                 err,
                 static_cast<int>(msg.size()), msg.data());
}

} // namespace nimrtc::engine
