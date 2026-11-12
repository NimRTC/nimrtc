/**
 * @file tests/test_engine_audio_bitrate.cpp
 * @brief The BWE-linked audio bitrate adaptation.
 *
 * The engine's audio encoder is the one outbound stream that has to
 * survive a bandwidth collapse: when the bandwidth estimate drops, the
 * encoder's bitrate follows it down (never below the configured floor),
 * and it climbs back towards the configured ceiling once the link
 * recovers.
 *
 * The contract this file pins down:
 *
 *   1. The encoder starts at the configured ceiling (32 kbps for the
 *      default mono configuration).
 *   2. A collapse of the estimate lowers the bitrate — fast (at most
 *      -50 % per step) — but never below the floor.
 *   3. A recovery raises it again — slowly (at most +10 % per step) —
 *      and never above the ceiling.
 *   4. A deviation below 10 % is ignored (hysteresis): a flickering
 *      estimate does not pump the encoder.
 *   5. `audio_bitrate_adaptive = false` pins the encoder.
 *
 * The engine is driven through fakes registered as plugins: a fake ICE
 * transport, a fake DTLS session, a fake BWE whose estimate the test
 * controls, and a fake codec that records every bitrate the engine
 * pushes into it.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/bwe.hpp>
#include <nimrtc/plugins/codec.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/rtp/rtcp_tmmbr.hpp>

namespace {

namespace np = nimrtc::plugins;
namespace nd = nimrtc::dtls;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

// ---------------------------------------------------------------------------
// Fake ICE transport
// ---------------------------------------------------------------------------

class FakeIceTransport final : public np::IICETransport {
public:
    const char* name() const noexcept override { return "fake-ice"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

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
    np::IICETransport* create_ice() const override { return new FakeIceTransport(); }
};

// ---------------------------------------------------------------------------
// Fake DTLS session
// ---------------------------------------------------------------------------

class FakeDtlsSession final : public nd::IDtlsSession {
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
// Fake BWE — the test controls the estimate
// ---------------------------------------------------------------------------

class FakeBwe final : public np::IBwe {
public:
    FakeBwe(np::BweConfig cfg, std::uint32_t initial_target)
        : cfg_(cfg), target_(initial_target) {}

    const char* name() const noexcept override { return "fake-bwe"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    void on_feedback(np::BweFeedback) noexcept override {}

    np::BweEstimate estimate() const noexcept override {
        np::BweEstimate e;
        e.target_bitrate_bps = target_;
        e.pacing_bitrate_bps = target_;
        return e;
    }

    void update_config(np::BweConfig config) noexcept override { cfg_ = config; }
    np::BweConfig config() const noexcept override { return cfg_; }
    void reset() noexcept override {}

    // ---- Test seam -------------------------------------------------------
    void set_target(std::uint32_t bps) noexcept { target_ = bps; }

private:
    np::BweConfig    cfg_;
    std::uint32_t    target_ = 0;
};

class FakeBweFactory final : public np::IBweFactory {
public:
    std::string_view id() const noexcept override { return "fake-bwe"; }
    std::string_view display_name() const noexcept override {
        return "Fake BWE (test)";
    }
    np::IBwe* create(np::BweConfig config) const override {
        auto* bwe = new FakeBwe(config, initial_target_);
        last_created() = bwe;
        return bwe;
    }

    static FakeBwe*& last_created() {
        static FakeBwe* instance = nullptr;
        return instance;
    }

    void set_initial_target(std::uint32_t bps) noexcept { initial_target_ = bps; }

private:
    std::uint32_t initial_target_ = 1'000'000;
};

// ---------------------------------------------------------------------------
// Fake codec — records every bitrate the engine pushes into it
// ---------------------------------------------------------------------------

class FakeCodec final : public np::ICodec {
public:
    explicit FakeCodec(np::CodecConfig cfg) : cfg_(cfg) {}

    const char* name() const noexcept override { return "fake-codec"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    std::size_t encode(const float*, std::size_t, std::uint8_t* output,
                       std::size_t output_capacity) noexcept override {
        if (output_capacity == 0) return 0;
        output[0] = 0;
        return 1;
    }

    std::size_t decode(const std::uint8_t*, std::size_t, float* output,
                       std::size_t output_capacity) noexcept override {
        if (output_capacity == 0) return 0;
        output[0] = 0.0f;
        return 1;
    }

    std::size_t decode_plc(float* output, std::size_t output_capacity) noexcept override {
        if (output_capacity == 0) return 0;
        output[0] = 0.0f;
        return 1;
    }

    np::CodecConfig config() const noexcept override { return cfg_; }

    np::Status update_config(np::CodecConfig cfg) noexcept override {
        cfg_ = cfg;
        bitrates_.push_back(cfg.bitrate_bps);
        return np::kOk;
    }

    np::CodecStats stats() const noexcept override { return {}; }
    std::string_view codec_name() const noexcept override { return "fake"; }
    std::uint8_t payload_type() const noexcept override { return 111; }

    // ---- Test seam -------------------------------------------------------
    const std::vector<int>& bitrates() const noexcept { return bitrates_; }

private:
    np::CodecConfig   cfg_;
    std::vector<int>  bitrates_;
};

class FakeCodecFactory final : public np::ICodecFactory {
public:
    std::string_view id() const noexcept override { return "fake-codec"; }
    std::string_view display_name() const noexcept override {
        return "Fake codec (test)";
    }
    np::ICodec* create(np::CodecConfig cfg) const override {
        auto* codec = new FakeCodec(cfg);
        last_created() = codec;
        return codec;
    }

    static FakeCodec*& last_created() {
        static FakeCodec* instance = nullptr;
        return instance;
    }
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class EngineAudioBitrate : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();

        static FakeIceFactory ice_factory;
        static FakeDtlsFactory dtls_factory;
        static FakeBweFactory bwe_factory;
        static FakeCodecFactory codec_factory;
        PluginRegistry::instance().register_ice_transport("fake-ice", &ice_factory);
        PluginRegistry::instance().register_dtls_session("fake-dtls", &dtls_factory);
        PluginRegistry::instance().register_bwe("fake-bwe", &bwe_factory);
        PluginRegistry::instance().register_codec("fake-codec", &codec_factory);
    }

    EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.transport_name          = "fake-ice";
        cfg.dtls_name               = "fake-dtls";
        cfg.datachannel_backend     = "";
        cfg.codec_name              = "fake-codec";
        cfg.bwe_name                = "fake-bwe";
        cfg.scheduler_config.enabled = false;
        // The adaptation controller is rate-limited; the test drives it
        // with a 1 ms interval so the steps are observable without
        // sleeping for seconds.
        cfg.audio_bitrate_update_interval_ms = 1;
        // A reproducible SSRC: the inbound TMMBR names *our* stream
        // (RFC 5104 §4.2.1.2), so the test needs to know the value the
        // engine sends under.
        cfg.audio_ssrc = 0x1234ABCDu;
        return cfg;
    }

    /// The peer asks us to send at most `bps` — the TMMBR path
    /// (RFC 5104 §4.2.1.2), delivered as a real RTPFB packet so the
    /// test exercises the same code the wire does.
    static void send_peer_tmmbr(NimRTCEngine& engine, std::uint32_t bps) {
        nimrtc::rtp::TmmbrEntry entry;
        entry.ssrc        = engine.config().audio_ssrc;
        entry.bitrate_bps = bps;
        const auto pkt = nimrtc::rtp::build_tmmbr(0xAABBCCDDu, {entry});
        ASSERT_FALSE(pkt.empty());
        engine.feed_media_inbound(pkt.data(), pkt.size());
    }

    /** Run `steps` adaptation steps, sleeping past the interval between
     *  them so the controller is allowed to fire again. */
    static void pump(NimRTCEngine& engine, int steps) {
        for (int i = 0; i < steps; ++i) {
            engine.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
};

// ============================================================================
// The contract
// ============================================================================

TEST_F(EngineAudioBitrate, TheEncoderStartsAtTheConfiguredCeiling) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // Mono (the default `pcm_channels`) → 32 kbps.
    EXPECT_EQ(engine.stats().audio_encoder_bitrate_bps, 32000u);
}

TEST_F(EngineAudioBitrate, ACollapsingEstimateLowersTheBitrate) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_NE(FakeBweFactory::last_created(), nullptr);

    // The link collapses: 100 kbps left, 10 % of which is the audio's
    // share → a 10 kbps budget.
    FakeBweFactory::last_created()->set_target(100'000);

    pump(engine, 1);
    const auto after_first = engine.stats().audio_encoder_bitrate_bps;
    EXPECT_LT(after_first, 32000u) << "the bitrate must follow the estimate";
    // A drop is at most 50 % per step.
    EXPECT_GE(after_first, 16000u);

    pump(engine, 3);
    const auto settled = engine.stats().audio_encoder_bitrate_bps;
    EXPECT_LE(settled, 10000u) << "the bitrate converges to the budget";
    EXPECT_GE(settled, 8000u) << "never below the configured floor";

    // The codec plugin saw the very same value.
    ASSERT_NE(FakeCodecFactory::last_created(), nullptr);
    const auto& seen = FakeCodecFactory::last_created()->bitrates();
    ASSERT_FALSE(seen.empty());
    EXPECT_EQ(static_cast<std::uint32_t>(seen.back()), settled);
}

TEST_F(EngineAudioBitrate, TheFloorIsNeverCrossed) {
    EngineConfig cfg = make_config();
    cfg.audio_min_bitrate_bps = 8000;
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_NE(FakeBweFactory::last_created(), nullptr);

    // A hopeless link: 1 kbps of estimate → a 100 bps budget, far below
    // the floor.
    FakeBweFactory::last_created()->set_target(1'000);

    pump(engine, 8);
    EXPECT_GE(engine.stats().audio_encoder_bitrate_bps, 8000u)
        << "the floor protects the audio stream from starvation";
    EXPECT_LE(engine.stats().audio_encoder_bitrate_bps, 32000u);
}

TEST_F(EngineAudioBitrate, ARisingEstimateRaisesTheBitrateSlowly) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_NE(FakeBweFactory::last_created(), nullptr);

    // Collapse first, so there is room to climb back.
    FakeBweFactory::last_created()->set_target(100'000);
    pump(engine, 4);
    const auto low = engine.stats().audio_encoder_bitrate_bps;
    ASSERT_LT(low, 32000u);

    // The link recovers.
    FakeBweFactory::last_created()->set_target(1'000'000);
    pump(engine, 1);
    const auto after_one_step = engine.stats().audio_encoder_bitrate_bps;
    EXPECT_GT(after_one_step, low) << "the bitrate climbs back";
    // A rise is at most +10 % per step.
    EXPECT_LE(after_one_step, low + low / 10 + 1);

    // It never climbs above the ceiling.
    pump(engine, 40);
    EXPECT_LE(engine.stats().audio_encoder_bitrate_bps, 32000u);
}

TEST_F(EngineAudioBitrate, ADeviationBelowTenPercentIsIgnored) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_NE(FakeBweFactory::last_created(), nullptr);

    // 300 kbps → a 30 kbps budget, i.e. -6 % from the 32 kbps ceiling:
    // below the hysteresis band, so nothing moves.
    FakeBweFactory::last_created()->set_target(300'000);

    pump(engine, 3);
    EXPECT_EQ(engine.stats().audio_encoder_bitrate_bps, 32000u);
}

TEST_F(EngineAudioBitrate, TheAdaptationCanBeDisabled) {
    EngineConfig cfg = make_config();
    cfg.audio_bitrate_adaptive = false;
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_NE(FakeBweFactory::last_created(), nullptr);

    FakeBweFactory::last_created()->set_target(10'000);
    pump(engine, 4);

    EXPECT_EQ(engine.stats().audio_encoder_bitrate_bps, 32000u)
        << "a pinned encoder ignores the estimate";
    ASSERT_NE(FakeCodecFactory::last_created(), nullptr);
    EXPECT_TRUE(FakeCodecFactory::last_created()->bitrates().empty())
        << "the codec plugin is not touched at all";
}

TEST_F(EngineAudioBitrate, AnExplicitCeilingIsHonoured) {
    EngineConfig cfg = make_config();
    cfg.audio_bitrate_bps = 24000;
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_EQ(engine.stats().audio_encoder_bitrate_bps, 24000u);

    // And it is the ceiling of the adaptation as well.
    ASSERT_NE(FakeBweFactory::last_created(), nullptr);
    FakeBweFactory::last_created()->set_target(1'000'000);
    pump(engine, 30);
    EXPECT_LE(engine.stats().audio_encoder_bitrate_bps, 24000u);
}

// ===========================================================================
// The peer's TMMBR limit (v1.2.0, session plan X5 / OD-E decision 3)
// ===========================================================================

TEST_F(EngineAudioBitrate, AnInboundTmmbrCapsTheEncoderBitrate) {
    // RFC 5104 §4.2.1.2: a TMMBR "indicates the maximum bitrate the
    // sender ... is willing to receive".  The engine answers it with a
    // TMMBN (RFC 5104 §4.2.2.2), and that acknowledgement is a
    // *promise* that the bound is in force.
    //
    // The scheduler budget and the pacer were clamped from the start,
    // but the *encoder* decides how many bits go on the wire — so the
    // limit has to reach the encoder as well.  Otherwise the engine
    // encodes above the ceiling it just promised.
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_NE(FakeBweFactory::last_created(), nullptr);

    // The link is wide open — the peer is the only constraint.
    FakeBweFactory::last_created()->set_target(1'000'000);

    EXPECT_EQ(engine.stats().audio_encoder_bitrate_bps, 32000u);

    // The peer asks for at most 8 kbps.
    send_peer_tmmbr(engine, 8'000);
    EXPECT_EQ(engine.peer_tmmbr_limit_bps(), 8'000u);

    pump(engine, 4);

    EXPECT_LE(engine.stats().audio_encoder_bitrate_bps, 8'000u)
        << "the peer's TMMBR limit caps the encoder — the engine must not "
           "encode above the bound it acknowledged in its TMMBN";

    // The codec plugin saw the capped value, too.
    ASSERT_NE(FakeCodecFactory::last_created(), nullptr);
    const auto& seen = FakeCodecFactory::last_created()->bitrates();
    ASSERT_FALSE(seen.empty());
    EXPECT_LE(static_cast<std::uint32_t>(seen.back()), 8'000u);
}

TEST_F(EngineAudioBitrate, ThePeerLimitWinsOverTheLocalFloor) {
    // OD-E decision 3 ("peer-wins"): the local floor
    // (`Config::audio_min_bitrate_bps`) exists so a congestion collapse
    // cannot starve the audio.  The peer's TMMBR limit is a different
    // kind of constraint — a promise the engine makes on the wire — so
    // when the two conflict, the peer's limit wins and the encoder is
    // allowed to go *below* the floor.
    EngineConfig cfg = make_config();
    cfg.audio_min_bitrate_bps = 8000;
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_NE(FakeBweFactory::last_created(), nullptr);

    FakeBweFactory::last_created()->set_target(1'000'000);

    // The peer asks for 5 kbps — below the 8 kbps floor.
    send_peer_tmmbr(engine, 5'000);
    ASSERT_EQ(engine.peer_tmmbr_limit_bps(), 5'000u);

    pump(engine, 4);

    EXPECT_LE(engine.stats().audio_encoder_bitrate_bps, 5'000u)
        << "the peer's limit wins over the local floor";
    EXPECT_LT(engine.stats().audio_encoder_bitrate_bps, 8'000u)
        << "the encoder is allowed to go below the local floor — a TMMBN "
           "is a commitment, and a commitment to send less is not "
           "something the sender may quietly exceed";
}

TEST_F(EngineAudioBitrate, ThePeerLimitBeatsThePinnedEncoder) {
    // `audio_bitrate_adaptive = false` says "do not move the encoder
    // because of the *estimate*".  It is not a licence to break a
    // promise the engine already made on the wire: the TMMBN that
    // acknowledged the TMMBR told the peer "this bound is in force".
    EngineConfig cfg = make_config();
    cfg.audio_bitrate_adaptive = false;
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_EQ(engine.stats().audio_encoder_bitrate_bps, 32000u);

    send_peer_tmmbr(engine, 12'000);
    pump(engine, 4);

    EXPECT_LE(engine.stats().audio_encoder_bitrate_bps, 12'000u)
        << "the acknowledged TMMBR limit is honoured even when the "
           "estimate-driven adaptation is off";
}

TEST_F(EngineAudioBitrate, TheTightestLimitWins) {
    // RFC 5104 §3.5.4.2: with several receivers the bounding set is the
    // tightest constraint, so a later, *looser* TMMBR must not raise
    // the limit back up.
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    send_peer_tmmbr(engine, 16'000);
    ASSERT_EQ(engine.peer_tmmbr_limit_bps(), 16'000u);

    send_peer_tmmbr(engine, 24'000);
    EXPECT_EQ(engine.peer_tmmbr_limit_bps(), 16'000u)
        << "RFC 5104 §3.5.4.2: the tightest constraint is the one that "
           "applies";

    pump(engine, 4);
    EXPECT_LE(engine.stats().audio_encoder_bitrate_bps, 16'000u);
}

}  // namespace
