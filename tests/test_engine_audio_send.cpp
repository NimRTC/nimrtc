/**
 * @file tests/test_engine_audio_send.cpp
 * @brief v1.2.0 (audio pipeline, WP-11) — the send path's two promises.
 *
 * The receive path has its own suite (`test_engine_audio_state.cpp`,
 * `test_engine_audio_stats.cpp`, `test_engine_capture_timestamp.cpp`).
 * This file covers the two things the *send* path promises, and they
 * are the two a host notices first when something is wrong:
 *
 *   1. **The framing is the engine's business, not the host's**
 *      (spec B-6, V-1 "sender framing").  `send_audio()` accepts *any*
 *      block size; the engine accumulates the PCM and emits exactly one
 *      RTP packet per 20 ms frame, with a continuous media timestamp.
 *      A host that chunks its capture into 7-sample pieces and a host
 *      that hands over one 20 ms frame at a time must put the *same*
 *      packets on the wire.  This is what lets a host use whatever
 *      capture granularity its audio backend prefers.
 *
 *   2. **No silent loss** (spec A-8, V-1 "encoder error reporting").
 *      An empty buffer, an invalid frame size or an encoder error must
 *      never disappear: each occurrence increments
 *      `Stats::audio_encode_errors` and fires `on_error`.  A host whose
 *      PCM never reaches the wire has to be able to see *why* — silence
 *      with no explanation is the failure mode this rule exists for.
 *
 * The engine is driven through two fakes, both registered as plugins:
 *
 *   * a **fake ICE transport** (`"fake-ice"`) that reports `Connected`
 *     and captures every outbound datagram, and
 *   * a **fake DTLS session** (`"fake-dtls"`) that reports `Connected`
 *     but exports no SRTP keying material.
 *
 * The second one is what makes the send path reachable at all:
 * `send_audio()` refuses to put RTP on the wire before the DTLS
 * handshake completed (otherwise the peer would receive plaintext RTP
 * that its SRTP context rejects).  The fake reports the handshake as
 * done from the start, so the send path is exercised without a peer.
 * Because the fake exports no keying material, SRTP is never installed
 * and the captured datagrams are plain RTP — which is exactly what
 * makes the framing assertable here.  The SRTP protection path is
 * covered by the SRTP module's own tests.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/engine_errors.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/rtp/packet.hpp>

namespace {

namespace np = nimrtc::plugins;
namespace nd = nimrtc::dtls;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

/** The payload type RFC 7587 assigns to Opus. */
constexpr std::uint8_t kOpusPt = 111;

/** The engine's default PCM rate (48 kHz) and frame duration (20 ms):
 *  one frame is 960 samples.
 *
 *  The two operands are *referenced* by the initializer below, and that
 *  is what keeps clang's `-Wunused-const-variable` (an error under
 *  `-Werror` on AppleClang) quiet.  The warning fires on a constant
 *  that nothing mentions again -- not on one that is merely not
 *  odr-used: a constant read in a constant expression counts as a
 *  reference. */
constexpr std::size_t kSampleRate = 48000;
constexpr std::size_t kFrameMs = 20;
constexpr std::size_t kFrameSamples = kSampleRate * kFrameMs / 1000;

// ---------------------------------------------------------------------------
// Fake ICE transport
// ---------------------------------------------------------------------------

class FakeIceTransport final : public np::IICETransport {
public:
    // ---- IPlugin ---------------------------------------------------------
    const char* name() const noexcept override { return "fake-ice"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    // ---- ITransport ------------------------------------------------------
    void set_callbacks(np::RecvCallback on_recv,
                       np::ErrorCallback on_error) noexcept override {
        on_recv_  = std::move(on_recv);
        on_error_ = std::move(on_error);
    }

    np::Status send(np::BufferView view, np::Addr = {}) noexcept override {
        sent_.emplace_back(view.data(), view.data() + view.size());
        return np::kOk;
    }

    int recv() noexcept override {
        int n = 0;
        while (!inbound_.empty()) {
            std::vector<std::uint8_t> pkt = std::move(inbound_.front());
            inbound_.erase(inbound_.begin());
            if (on_recv_) on_recv_(np::BufferView(pkt.data(), pkt.size()));
            ++n;
        }
        return n;
    }

    np::Addr local_addr() const noexcept override { return {}; }
    np::Addr remote_addr() const noexcept override { return {}; }

    // ---- IICETransport ---------------------------------------------------
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

private:
    np::RecvCallback   on_recv_;
    np::ErrorCallback  on_error_;
    std::vector<std::vector<std::uint8_t>> inbound_;
    std::vector<std::vector<std::uint8_t>> sent_;
};

class FakeIceFactory final : public np::IICETransportFactory {
public:
    std::string_view id() const noexcept override { return "fake-ice"; }
    std::string_view display_name() const noexcept override {
        return "Fake ICE transport (test)";
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
// Fake DTLS session
// ---------------------------------------------------------------------------

/** A DTLS session that reports the handshake as complete and exports no
 *  SRTP keying material.
 *
 *  The engine only asks two things of the DTLS session on the send
 *  path: "is the handshake done?" (the gate in `send_audio()`) and
 *  "give me the SRTP keys" (to install the SRTP context).  The fake
 *  answers the first with yes and the second with "not ready", so the
 *  RTP travels in cleartext and the test can read it. */
class FakeDtlsSession final : public nd::IDtlsSession {
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
    nd::DtlsState state() const noexcept override { return nd::DtlsState::Connected; }
    bool is_connected() const noexcept override { return true; }
    const nd::Fingerprint& local_fingerprint() const noexcept override {
        return fingerprint_;
    }
    std::optional<nd::SrtpKeyingMaterial>
    srtp_keying_material() const noexcept override {
        return std::nullopt;
    }

private:
    nd::Fingerprint fingerprint_{"sha-256", {}, {}, ""};
};

class FakeDtlsFactory final : public nd::IDtlsSessionFactory {
public:
    std::string_view id() const noexcept override { return "fake-dtls"; }
    std::string_view display_name() const noexcept override {
        return "Fake DTLS session (test)";
    }
    std::unique_ptr<nd::IDtlsSession> create(const nd::DtlsConfig&) const override {
        return std::make_unique<FakeDtlsSession>();
    }
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class EngineAudioSend : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();

        static FakeIceFactory ice_factory;
        static FakeDtlsFactory dtls_factory;
        PluginRegistry::instance().register_ice_transport("fake-ice", &ice_factory);
        PluginRegistry::instance().register_dtls_session("fake-dtls", &dtls_factory);
    }

    EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.transport_name          = "fake-ice";
        cfg.dtls_name               = "fake-dtls";
        cfg.datachannel_backend     = "";
        cfg.bwe_name                = "";
        cfg.scheduler_config.enabled = false;
        return cfg;
    }

    /** The RTP packets the engine put on the wire.
     *
     *  The captured datagrams are filtered by "is this an RTP packet
     *  for the Opus payload type" — the same engine also emits RTCP
     *  (sender reports), and those are not RTP. */
    static std::vector<nimrtc::rtp::PacketView> rtp_packets(
        const std::vector<std::vector<std::uint8_t>>& datagrams) {
        std::vector<nimrtc::rtp::PacketView> out;
        nimrtc::rtp::Parser parser;
        for (const auto& d : datagrams) {
            auto parsed = parser.parse(nimrtc::core::ByteSpan(d.data(), d.size()));
            if (!parsed.ok()) continue;
            const nimrtc::rtp::PacketView& view = parsed.value();
            if (view.payload_type != kOpusPt) continue;
            out.push_back(view);
        }
        return out;
    }
};

// ============================================================================
// 1. The framing (spec B-6, V-1 "sender framing")
// ============================================================================

TEST_F(EngineAudioSend, ABlockThatIsNotFrameSizedStillYieldsOnePacketPerFrame) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // 1000 samples at 48 kHz is 20.8 ms — one whole 20 ms frame plus a
    // 40-sample residual.  The residual must stay in the accumulator;
    // a partial frame is never encoded.
    std::vector<float> pcm(1000, 0.25f);
    ASSERT_EQ(engine.send_audio(pcm.data(), pcm.size()), 0u);

    auto packets = rtp_packets(FakeIceFactory::last_created()->sent());
    ASSERT_EQ(packets.size(), 1u)
        << "1000 samples carry exactly one whole 20 ms frame";

    const auto s = engine.stats();
    EXPECT_EQ(s.audio_frames_sent, 1u);
    EXPECT_EQ(s.audio_samples_in, 1000u);
    EXPECT_EQ(s.audio_samples_pending, 40u)
        << "the 40-sample residual stays in the accumulator until the "
           "next block completes it";
}

TEST_F(EngineAudioSend, TheMediaTimestampIsContinuousAcrossPackets) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // Three whole frames, handed over one call at a time.
    std::vector<float> pcm(kFrameSamples, 0.25f);
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(engine.send_audio(pcm.data(), pcm.size()), 0u);
    }

    auto packets = rtp_packets(FakeIceFactory::last_created()->sent());
    ASSERT_EQ(packets.size(), 3u);

    // The media clock advances by exactly one frame per packet: the
    // timestamp is the *sampling* instant of the first sample, so a
    // frame of 960 samples at 48 kHz advances it by 960 (RFC 3550
    // §4.1).
    for (std::size_t i = 1; i < packets.size(); ++i) {
        EXPECT_EQ(packets[i].timestamp,
                  packets[i - 1].timestamp + kFrameSamples)
            << "the media timestamp must advance by exactly one frame "
               "per packet";
        EXPECT_EQ(packets[i].seq, static_cast<std::uint16_t>(packets[i - 1].seq + 1))
            << "the sequence number advances by one per packet";
    }
}

TEST_F(EngineAudioSend, TheHostMayChunkItsCaptureAnyWayItLikes) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // 960 samples in 7-sample pieces — 138 pieces (966 samples), so the
    // last piece completes the frame and leaves a 6-sample residual.
    std::vector<float> piece(7, 0.25f);
    for (int i = 0; i < 138; ++i) {
        ASSERT_EQ(engine.send_audio(piece.data(), piece.size()), 0u);
    }

    auto packets = rtp_packets(FakeIceFactory::last_created()->sent());
    ASSERT_EQ(packets.size(), 1u)
        << "the host's chunking must not change how many frames go on "
           "the wire — only the frame boundaries are the engine's "
           "business";

    const auto s = engine.stats();
    EXPECT_EQ(s.audio_frames_sent, 1u);
    EXPECT_EQ(s.audio_samples_pending, 6u);
}

// ============================================================================
// 2. No silent loss (spec A-8, V-1 "encoder error reporting")
// ============================================================================

TEST_F(EngineAudioSend, AnEmptyPcmBufferIsReportedAndCounted) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    int errors = 0;
    std::uint32_t last_code = 0;
    engine.set_on_error([&](std::uint32_t code, std::string_view) {
        ++errors;
        last_code = code;
    });

    std::vector<float> pcm(64, 0.0f);
    EXPECT_EQ(engine.send_audio(pcm.data(), 0), nimrtc::core::kEngineInvalidParam);

    const auto s = engine.stats();
    EXPECT_EQ(s.audio_encode_errors, 1u)
        << "an empty buffer must not disappear silently (A-8)";
    EXPECT_EQ(errors, 1)
        << "the host must be told, otherwise it only hears silence";
    EXPECT_EQ(last_code, nimrtc::core::kAudioEncodeFailed);

    // Nothing reached the wire, and the accumulator did not grow.
    EXPECT_EQ(s.audio_frames_sent, 0u);
    EXPECT_EQ(s.audio_samples_pending, 0u);
}

TEST_F(EngineAudioSend, ANullPcmBufferIsReportedAndCounted) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    int errors = 0;
    engine.set_on_error([&](std::uint32_t, std::string_view) { ++errors; });

    EXPECT_EQ(engine.send_audio(nullptr, 128), nimrtc::core::kEngineInvalidParam);

    EXPECT_EQ(engine.stats().audio_encode_errors, 1u);
    EXPECT_EQ(errors, 1);
}

TEST_F(EngineAudioSend, TheErrorCallbackIsThrottled) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    int errors = 0;
    engine.set_on_error([&](std::uint32_t, std::string_view) { ++errors; });

    // Twenty empty buffers in a row: the *counter* grows with every
    // occurrence (the host can see how often it happened), but the
    // callback is throttled so a host that feeds the engine garbage
    // does not get twenty callbacks per second (A-8).
    for (int i = 0; i < 20; ++i) {
        engine.send_audio(nullptr, 128);
    }

    EXPECT_EQ(engine.stats().audio_encode_errors, 20u)
        << "every occurrence counts, even the ones that are not reported";
    EXPECT_LT(errors, 20)
        << "the callback must be throttled, not fired once per occurrence";
    EXPECT_GE(errors, 1);
}

}  // namespace
