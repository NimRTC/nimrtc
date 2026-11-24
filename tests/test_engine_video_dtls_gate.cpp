/**
 * @file tests/test_engine_video_dtls_gate.cpp
 * @brief The video path must not send media before the DTLS handshake is done.
 *
 * ## What this file is about
 *
 * The **audio** path has always gated on the DTLS session being
 * `Connected` before it lets an RTP packet leave the engine
 * (`send_audio()` in `engine_media.cpp`).  The **video** path did not,
 * and that was a real defect with two visible consequences:
 *
 *  1. **Plaintext on the wire.**  The capture pump starts encoding the
 *     moment the engine is open.  The DTLS handshake takes tens of
 *     milliseconds — long enough for several frames to be encoded,
 *     packetized and sent with no SRTP key attached.
 *
 *  2. **A corrupted sequence space at the peer.**  The plaintext packets
 *     are discarded by the peer's (still unkeyed) SRTP context, but the
 *     peer's RTP layer has already seen their sequence numbers.  When the
 *     protected stream starts, its sequence numbers continue from where
 *     the plaintext ones stopped, so the peer's jitter buffer sees a gap
 *     and reports the missing packets as lost.  The plaintext frames were
 *     not IDRs either (the IDR is armed *after* the handshake, by
 *     `maybe_install_srtp_keys()`), so the peer is left without a
 *     reference frame and its decoder refuses the first P-frames.
 *
 * The observable symptom was a session that *always* started with a
 * handful of "dropped" frames and a key-frame request round-trip, even
 * though nothing was actually lost.
 *
 * ## The two things this file proves
 *
 *  - **The gate holds.**  While the DTLS session is not `Connected`, the
 *    video path sends *nothing*.  Once it is, the video flows again.
 *
 *  - **The gate is narrow.**  A session with **no** DTLS session at all
 *    (the rule-2 `raw-udp-arq` stack and the `shell` stack) sends
 *    plaintext media *by design*, and the gate must not reject it.  That
 *    is the case the other half of this file covers, because a gate that
 *    only worked for the DTLS case would be a trap for the no-DTLS
 *    stacks.
 *
 * The engine is driven through a fake ICE transport ("fake-ice") and a
 * fake DTLS session whose `is_connected()` the test controls, so the
 * handshake's timing is entirely under the test's thumb.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/plugins/rtp.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/plugins/video_pipeline.hpp>
#include <nimrtc/sctp/sctp_socket_iface.hpp>
#include <nimrtc/transport/transport_stack.hpp>

namespace {

namespace np = nimrtc::plugins;
namespace nd = nimrtc::dtls;
namespace ns = nimrtc::sctp;
namespace nru = nimrtc::raw_udp;
using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

// ---------------------------------------------------------------------------
// The fake ICE transport
// ---------------------------------------------------------------------------

class FakeIceTransport final : public np::IICETransport {
public:
    const char* name() const noexcept override { return "fake-ice"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    void set_callbacks(np::RecvCallback on_recv,
                       np::ErrorCallback on_error) noexcept override {
        on_recv_ = std::move(on_recv);
        on_error_ = std::move(on_error);
    }

    np::Status send(np::BufferView view, np::Addr = {}) noexcept override {
        sent_.emplace_back(view.data(), view.data() + view.size());
        return np::kOk;
    }

    int recv() noexcept override { return 0; }

    np::Addr local_addr() const noexcept override { return {}; }
    np::Addr remote_addr() const noexcept override { return {}; }

    np::IceState state() const noexcept override { return np::IceState::Connected; }
    std::string local_ufrag() const noexcept override { return "fakeufrag"; }
    std::string local_password() const noexcept override { return "fakepassword"; }
    bool wait_for_gathering(int) noexcept override { return true; }
    std::vector<std::string> gathered_local_candidates() const noexcept override {
        return {};
    }
    np::Status set_remote_description(std::string_view) noexcept override {
        return np::kOk;
    }
    np::Status add_remote_candidate(std::string_view) noexcept override {
        return np::kOk;
    }
    void set_bind_address(std::string_view) noexcept override {}
    void set_stun_server(std::string_view, std::uint16_t) noexcept override {}
    void set_local_port_range(std::uint16_t, std::uint16_t) noexcept override {}
    void add_turn_server(std::string_view, std::uint16_t, std::string_view,
                         std::string_view) noexcept override {}
    void set_bwe(np::IBwe*) noexcept override {}
    void set_scheduler(np::IScheduler*) noexcept override {}

    // ---- Test helpers ----------------------------------------------------
    const std::vector<std::vector<std::uint8_t>>& sent() const noexcept {
        return sent_;
    }
    void clear_sent() { sent_.clear(); }

private:
    np::RecvCallback   on_recv_;
    np::ErrorCallback  on_error_;
    std::vector<std::vector<std::uint8_t>> sent_;
};

class FakeIceFactory final : public np::IICETransportFactory {
public:
    std::string_view id() const noexcept override { return "fake-ice"; }
    std::string_view display_name() const noexcept override {
        return "Fake ICE transport (DTLS gate test)";
    }
    np::IICETransport* create_ice() const override {
        auto* t = new FakeIceTransport();
        last_created() = t;
        return t;
    }
    static FakeIceTransport*& last_created() {
        static FakeIceTransport* instance = nullptr;
        return instance;
    }
};

// ---------------------------------------------------------------------------
// The fake DTLS session — the test drives `is_connected()`
// ---------------------------------------------------------------------------

/**
 * @brief A DTLS session whose "connected" state the test controls.
 *
 * It exports no SRTP keying material, so the RTP travels in cleartext —
 * which is exactly what lets the test count the packets on the wire.
 */
class ControllableDtlsSession final : public nd::IDtlsSession {
public:
    // ---- The seam surface ------------------------------------------------
    void set_role(nd::Role) noexcept override {}
    void set_peer_fingerprint(std::span<const std::uint8_t>) noexcept override {}
    void start() noexcept override {}
    void pump() noexcept override {}
    void on_handshake_complete(OnCompleteCb) noexcept override {}
    np::Status export_srtp_key_material(std::span<std::uint8_t, 60>) noexcept override {
        return np::kErrNotReady;
    }

    // ---- The legacy engine-facing surface --------------------------------
    nimrtc::core::Result<void> open() noexcept override {
        return nimrtc::core::Result<void>::make_ok();
    }
    void set_role(nd::DtlsRole) noexcept override {}
    void set_peer_fingerprint(std::string, std::vector<std::uint8_t>) noexcept override {}
    std::size_t feed_inbound(std::span<const std::uint8_t>,
                             const nd::DtlsAddr&) noexcept override {
        return 0;
    }
    std::vector<nd::DtlsRecord> take_outbound() noexcept override { return {}; }
    void tick() noexcept override {}

    nd::DtlsState state() const noexcept override {
        return connected_ ? nd::DtlsState::Connected : nd::DtlsState::HelloSent;
    }
    bool is_connected() const noexcept override { return connected_; }

    const nd::Fingerprint& local_fingerprint() const noexcept override {
        return fingerprint_;
    }
    std::optional<nd::SrtpKeyingMaterial>
    srtp_keying_material() const noexcept override {
        return std::nullopt;
    }

    // ---- Test control ----------------------------------------------------
    void set_connected(bool c) noexcept { connected_ = c; }

private:
    bool connected_ = false;
    nd::Fingerprint fingerprint_{"sha-256", {}, {}, ""};
};

class ControllableDtlsFactory final : public nd::IDtlsSessionFactory {
public:
    std::string_view id() const noexcept override { return "controllable-dtls"; }
    std::string_view display_name() const noexcept override {
        return "Controllable DTLS session (DTLS gate test)";
    }
    std::unique_ptr<nd::IDtlsSession> create(const nd::DtlsConfig&) const override {
        auto* s = new ControllableDtlsSession();
        last_created() = s;
        return std::unique_ptr<nd::IDtlsSession>(s);
    }
    static ControllableDtlsSession*& last_created() {
        static ControllableDtlsSession* instance = nullptr;
        return instance;
    }
};

// ---------------------------------------------------------------------------
// A transport stack with **no** DTLS session
// ---------------------------------------------------------------------------

/**
 * @brief A transport stack whose media plane has no DTLS at all.
 *
 * This is the shape of the rule-2 `raw-udp-arq` media stack and of the
 * Slice-8 "shell" stack: `dtls_weak()` returns an empty weak_ptr, so
 * the engine's `impl_->dtls.lock()` is null and the engine treats the
 * session as "no crypto, plaintext media by design".
 *
 * The gate under test has to leave those sessions alone — a gate that
 * only worked for the DTLS case would silently break every
 * plaintext-by-design stack.
 *
 * The engine only ever touches two members of the stack: `ice()` (which
 * it drives) and `dtls_weak()` (which it stores).  The other three are
 * the null-instance singletons the in-tree stacks also use.
 */
class NoDtlsStack final : public nimrtc::ITransportStack {
public:
    explicit NoDtlsStack(std::unique_ptr<np::IICETransport> ice)
        : ice_(std::move(ice)) {}

    np::IICETransport& ice() noexcept override { return *ice_; }
    nd::IDtlsSession& dtls() noexcept override { return null_dtls(); }
    np::IRTP& rtp() noexcept override { return null_rtp(); }
    ns::ISctpSocket& sctp() noexcept override { return null_sctp(); }

    nru::IRawUdpDatagram* raw_control() noexcept override { return nullptr; }

    // The whole point: **no** DTLS session, so the engine's weak_ptr
    // stays empty and the gate must not fire.
    std::weak_ptr<nd::IDtlsSession> dtls_weak() noexcept override { return {}; }

    void start() noexcept override {}
    void close() noexcept override {}
    std::string_view id() const noexcept override { return "default"; }

private:
    // ---- The null-instance singletons (the same shape the in-tree
    // stacks use — see `src/modules/transport/src/raw_udp_arq_stack.cpp`).
    class NullRtp final : public np::IRTP {
    public:
        const char* name() const noexcept override { return "null-rtp"; }
        np::Status open() noexcept override { return np::kOk; }
        void close() noexcept override {}
        void set_callbacks(np::RtpRecvCallback, np::RtcpRecvCallback,
                           np::RtpParseErrorCallback) noexcept override {}
        std::optional<np::RtpPacket>
        parse_packet(np::BufferView) const noexcept override { return std::nullopt; }
        std::size_t build_packet(const np::RtpHeader&, np::BufferView,
                                 np::OutPacket&) const noexcept override { return 0; }
        std::vector<np::RtcpPacket>
        parse_rtcp(np::BufferView) const noexcept override { return {}; }
        std::size_t build_rtcp(const std::vector<np::RtcpPacket>&,
                               np::OutPacket&) const noexcept override { return 0; }
        void record_inbound(const np::RtpPacket&, np::TimestampUs) noexcept override {}
        np::IRTP::Stats stats() const noexcept override { return {}; }
    };

    class NullDtls final : public nd::IDtlsSession {
    public:
        void set_role(nd::Role) noexcept override {}
        void set_peer_fingerprint(std::span<const std::uint8_t>) noexcept override {}
        void start() noexcept override {}
        void pump() noexcept override {}
        void on_handshake_complete(OnCompleteCb) noexcept override {}
        np::Status export_srtp_key_material(std::span<std::uint8_t, 60>) noexcept override {
            return np::kErrNotReady;
        }
        nimrtc::core::Result<void> open() noexcept override {
            return nimrtc::core::Result<void>::make_ok();
        }
        void set_role(nd::DtlsRole) noexcept override {}
        void set_peer_fingerprint(std::string, std::vector<std::uint8_t>) noexcept override {}
        std::size_t feed_inbound(std::span<const std::uint8_t>,
                                 const nd::DtlsAddr&) noexcept override { return 0; }
        std::vector<nd::DtlsRecord> take_outbound() noexcept override { return {}; }
        void tick() noexcept override {}
        nd::DtlsState state() const noexcept override { return nd::DtlsState::Closed; }
        bool is_connected() const noexcept override { return false; }
        const nd::Fingerprint& local_fingerprint() const noexcept override {
            return fingerprint_;
        }
        std::optional<nd::SrtpKeyingMaterial>
        srtp_keying_material() const noexcept override { return std::nullopt; }

    private:
        nd::Fingerprint fingerprint_{"sha-256", {}, {}, ""};
    };

    class NullSctp final : public ns::ISctpSocket {
    public:
        np::Status send_datagram(std::uint16_t, np::BufferView) noexcept override {
            return np::kErrNotReady;
        }
        np::Status send_stream(std::uint16_t, np::BufferView) noexcept override {
            return np::kErrNotReady;
        }
        np::Status send_partial_reliable(std::uint16_t, np::BufferView,
                                         std::chrono::milliseconds) noexcept override {
            return np::kErrNotReady;
        }
        void set_on_recv(np::OnSctpRecvCb) noexcept override {}
    };

    static np::IRTP& null_rtp() noexcept {
        static NullRtp n;
        return n;
    }
    static nd::IDtlsSession& null_dtls() noexcept {
        static NullDtls n;
        return n;
    }
    static ns::ISctpSocket& null_sctp() noexcept {
        static NullSctp n;
        return n;
    }

    std::unique_ptr<np::IICETransport> ice_;
};

class NoDtlsStackFactory final : public nimrtc::ITransportStackFactory {
public:
    std::string_view id() const noexcept override { return "default"; }
    std::string_view display_name() const noexcept override {
        return "Transport stack without DTLS (DTLS gate test)";
    }
    std::unique_ptr<nimrtc::ITransportStack>
    create(const nimrtc::StackConfig&) const override {
        auto* t = new FakeIceTransport();
        FakeIceFactory::last_created() = t;
        return std::make_unique<NoDtlsStack>(std::unique_ptr<np::IICETransport>(t));
    }
};

// ---------------------------------------------------------------------------
// The video codec
// ---------------------------------------------------------------------------

/**
 * @brief A codec that encodes a minimal Annex-B IDR and decodes it.
 *
 * The decode succeeds without producing pixels; the engine stops at the
 * null-buffer check, which is as far as this test needs.
 */
class GateVideoCodec final : public np::IVideoCodec {
public:
    const char* name() const noexcept override { return "gate-codec"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    bool can_encode() const noexcept override { return true; }
    bool can_decode() const noexcept override { return true; }
    np::VideoCodecKind kind() const noexcept override {
        return np::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override { return "gate"; }

    np::Status encode(const np::VideoFrame&,
                       std::uint8_t* output,
                       std::size_t output_capacity,
                       np::EncodedVideoFrame& encoded_out) noexcept override {
        static constexpr std::uint8_t kAnnexBIdr[] = {0x00, 0x00, 0x00, 0x01, 0x65};
        if (!output || output_capacity < sizeof(kAnnexBIdr)) return np::kErrBufferTooSmall;
        std::memcpy(output, kAnnexBIdr, sizeof(kAnnexBIdr));
        encoded_out.payload = nimrtc::core::ByteSpan{output, sizeof(kAnnexBIdr)};
        encoded_out.codec = np::VideoCodecKind::kH264;
        encoded_out.is_keyframe = true;
        encoded_out.nalu_format = np::NaluFormat::kAnnexBStartCode;
        encoded_out.payload_type = payload_type();
        return np::kOk;
    }

    np::Status decode(const np::EncodedVideoFrame&,
                      np::VideoFrame&,
                      std::uint8_t* const*) noexcept override {
        return np::kOk;
    }

    np::Status force_keyframe() noexcept override { return np::kOk; }

    np::VideoCodecConfig config() const noexcept override { return cfg_; }
    np::Status update_config(np::VideoCodecConfig cfg) noexcept override {
        cfg_ = cfg;
        return np::kOk;
    }
    np::VideoCodecStats stats() const noexcept override { return {}; }
    std::uint8_t payload_type() const noexcept override { return 102; }

private:
    np::VideoCodecConfig cfg_{};
};

class GateVideoCodecFactory final : public np::IVideoCodecFactory {
public:
    std::string_view id() const noexcept override { return "gate-codec"; }
    std::string_view display_name() const noexcept override {
        return "Gate video codec (DTLS gate test)";
    }
    np::IVideoCodec* create(np::VideoCodecConfig) const override {
        return new GateVideoCodec();
    }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Is this datagram an RTP packet (i.e. not RTCP)?  RFC 3550 §6.1. */
bool is_rtp_packet(const std::vector<std::uint8_t>& p) noexcept {
    if (p.size() < 2) return false;
    const std::uint8_t pt2 = p[1];
    return !(pt2 >= 192 && pt2 <= 223);
}

/** The number of RTP packets among the datagrams the transport captured. */
std::size_t count_rtp_packets(
    const std::vector<std::vector<std::uint8_t>>& sent) noexcept {
    std::size_t n = 0;
    for (const auto& d : sent) {
        if (is_rtp_packet(d)) ++n;
    }
    return n;
}

/** The engine configuration the tests share. */
EngineConfig make_config(std::string_view dtls_name,
                         std::string_view stack_name = "") {
    EngineConfig cfg{};
    cfg.transport_name = "fake-ice";
    cfg.dtls_name = std::string(dtls_name);
    // An explicit transport-stack name is the engine's user override
    // (`EngineConfig::transport_stack_name`) — it wins over the
    // CapabilitySelector's rule.  Empty = let the selector pick
    // "webrtc-classic", which is the DTLS-bearing stack.
    cfg.transport_stack_name = std::string(stack_name);
    cfg.video_receiver_name = "reference";
    cfg.video_sender_name   = "reference";
    cfg.video_codec_name    = "gate-codec";
    cfg.datachannel_backend = "";       // no DataChannel in this test
    cfg.bwe_name            = "";       // no pacing / scheduler jitter
    cfg.scheduler_config.enabled = false;
    cfg.video_width  = 640;
    cfg.video_height = 480;
    cfg.video_fps    = 30;
    return cfg;
}

/** A source frame the engine can encode (a solid grey I420 picture). */
np::VideoSourceFrame make_source_frame(std::int64_t ts_us) {
    np::VideoSourceFrame f{};
    f.width = 640;
    f.height = 480;
    f.format = np::VideoPixelFormat::kI420;
    f.capture_ts_us = ts_us;
    f.frame_seq = static_cast<std::uint32_t>(ts_us / 33'333);
    f.rtp_timestamp = static_cast<std::uint32_t>(ts_us * 90);
    // The engine stages the pixels itself; the planes may be null — the
    // stub codec does not read them.
    f.plane_y = nullptr;
    f.plane_u = nullptr;
    f.plane_v = nullptr;
    return f;
}

// ---------------------------------------------------------------------------
// The registry setup
// ---------------------------------------------------------------------------

/** Register the fakes exactly once, for the whole binary. */
void register_fakes_once() {
    static std::once_flag once;
    std::call_once(once, [] {
        // The built-in plugins have to be in the registry **before** the
        // fakes, because `NimRTCEngine::open()` calls
        // `core::register_all_default_plugins()` itself — and that call
        // re-publishes the transport module's "default" shell stack.
        //
        // The module-level registrars are function-local statics, so
        // they run **once** per process.  Draining them here (before the
        // fakes are published) means the engine's own call during
        // `open()` is a no-op, and the fake stack published below
        // survives.
        nimrtc::core::register_all_default_plugins();

        static FakeIceFactory ice_factory;
        static ControllableDtlsFactory dtls_factory;
        static GateVideoCodecFactory codec_factory;
        static NoDtlsStackFactory stack_factory;
        PluginRegistry::instance().register_ice_transport(
            "fake-ice", &ice_factory);
        PluginRegistry::instance().register_dtls_session(
            "controllable-dtls", &dtls_factory);
        PluginRegistry::instance().register_video_codec(
            "gate-codec", &codec_factory);
        // The "default" stack id is the Slice-8 "shell" stack — the one
        // the engine treats as "no DTLS by design".  Registering the
        // fake over it (the registry allows a late-binding overwrite)
        // gives this test a stack with a *driven* ICE transport and no
        // DTLS session, which is exactly the case the gate must leave
        // alone.
        PluginRegistry::instance().register_transport_stack(
            "default", &stack_factory);
    });
}

// =====================================================================
// Test 1: the gate holds — no video leaves before DTLS is Connected.
// =====================================================================

TEST(EngineVideoDtlsGate, NoVideoLeavesBeforeTheHandshakeCompletes) {
    register_fakes_once();

    NimRTCEngine engine(make_config("controllable-dtls"));
    ASSERT_EQ(engine.open(), 0u);

    auto* ice = FakeIceFactory::last_created();
    auto* dtls = ControllableDtlsFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ASSERT_NE(dtls, nullptr);

    // The handshake has NOT completed: the DTLS session is in HelloSent.
    dtls->set_connected(false);
    ice->clear_sent();

    // Feed the engine a few frames.  Every one of them would be encoded,
    // packetized and sent if there were no gate.
    for (int i = 0; i < 5; ++i) {
        (void)engine.send_video(make_source_frame(i * 33'333));
    }
    for (int i = 0; i < 10; ++i) engine.tick();

    EXPECT_EQ(count_rtp_packets(ice->sent()), 0u)
        << "the video path sent RTP packets before the DTLS handshake "
           "completed — they would have gone out in plaintext, and the "
           "peer's sequence space would have been corrupted by them";

    // ---- The handshake completes ----------------------------------------
    dtls->set_connected(true);
    ice->clear_sent();

    for (int i = 0; i < 5; ++i) {
        (void)engine.send_video(make_source_frame((100 + i) * 33'333));
    }
    for (int i = 0; i < 10; ++i) engine.tick();

    EXPECT_GT(count_rtp_packets(ice->sent()), 0u)
        << "the video path sent nothing after the DTLS handshake completed "
           "— the gate is closed too early";

    engine.close();
}

// =====================================================================
// Test 2: the gate is narrow — a session with no DTLS still sends.
// =====================================================================

TEST(EngineVideoDtlsGate, ASessionWithoutDtlsStillSendsVideo) {
    register_fakes_once();

    // The `default` stack id is the Slice-8 "shell" stack — the shape of
    // the rule-2 `raw-udp-arq` media stack and of the shell: it has
    // **no** DTLS session at all, so the engine's DTLS weak_ptr stays
    // empty and the media is plaintext *by design*.
    NimRTCEngine engine(make_config("", /*stack_name=*/"default"));
    ASSERT_EQ(engine.open(), 0u);

    auto* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();

    for (int i = 0; i < 5; ++i) {
        (void)engine.send_video(make_source_frame(i * 33'333));
    }
    for (int i = 0; i < 10; ++i) engine.tick();

    EXPECT_GT(count_rtp_packets(ice->sent()), 0u)
        << "a session without a DTLS session sent no video — the gate must "
           "only refuse media while a handshake is *in progress*, not when "
           "the stack has no DTLS at all (rule-2 raw-udp-arq / shell)";

    engine.close();
}

// =====================================================================
// Test 3: the packet count is exactly zero before the handshake.
//
// The first test proves "fewer than expected"; this one pins the exact
// number, so a future change that leaks a *single* packet through the
// gate is caught here rather than in a lab.
// =====================================================================

TEST(EngineVideoDtlsGate, NotOnePacketEscapesTheGate) {
    register_fakes_once();

    NimRTCEngine engine(make_config("controllable-dtls"));
    ASSERT_EQ(engine.open(), 0u);

    auto* ice = FakeIceFactory::last_created();
    auto* dtls = ControllableDtlsFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ASSERT_NE(dtls, nullptr);

    dtls->set_connected(false);
    ice->clear_sent();

    // Many frames, many ticks: a gate that only covered the first frame
    // would leak here.
    for (int i = 0; i < 60; ++i) {
        (void)engine.send_video(make_source_frame(i * 33'333));
        engine.tick();
    }

    const std::size_t leaked = count_rtp_packets(ice->sent());
    EXPECT_EQ(leaked, 0u)
        << leaked << " RTP packet(s) escaped the DTLS gate before the "
                    "handshake completed";

    engine.close();
}

}  // namespace
