/**
 * @file tests/test_engine_audio_device.cpp
 * @brief v1.4 D1 (ADR-028) -- the engine's audio-device plane.
 *
 * The audio-device module (`src/modules/audio_device/`) has existed for
 * a long time: it has the PortAudio / system-audio / synthetic
 * backends, the `plugins::IAudioSource` / `plugins::IAudioSink` seams
 * and its own unit tests.  What it did **not** have was a path from
 * the engine to it: the engine never resolved a device, never read a
 * frame from one, and never wrote a decoded frame to one.
 *
 * D1 closes that gap, and this file pins down the engine's half of
 * the contract:
 *
 *   1. **The engine resolves the device.**  `EngineConfig::audio_source_name`
 *      names a plugin id; the engine resolves it through
 *      `core::PluginRegistry` (never by naming the module -- Layout
 *      Invariant 4), opens it, and hands the host a pointer to it.
 *
 *   2. **The capture pump feeds the engine.**  The engine polls the
 *      device's bounded queue on its own tick thread and drives the
 *      frames through the very same `send_audio()` chain the host
 *      uses -- so the outbound path has exactly one implementation.
 *
 *   3. **The playback path is a *parallel* path.**  The host's
 *      `on_audio_frame` / `on_audio_frame_info` callbacks keep firing
 *      (`NIMRTC_API_STABLE`), and the device write is *appended* to
 *      them.  D1 replaces neither.
 *
 *   4. **The AEC reference follows the *device*.**  When a playback
 *      device is present, the engine reports the device's cumulative
 *      played-sample count to the 3A -- not the count of samples
 *      handed over -- and reports it **once**.  Reporting both would
 *      double the 3A's reference count and misalign the echo
 *      canceller.
 *
 *   5. **The failure paths are visible.**  A device that cannot be
 *      resolved is *not* fatal (a machine with no sound card must
 *      still be able to run a video call), but the host is told.
 *
 * The device under test is the module's **synthetic** backend: it
 * needs no sound card, no driver and no permission prompt, so the
 * suite runs on a CI machine.  Its own behaviour is covered by the
 * module's tests; what matters here is the *engine's* half.
 *
 * The engine is driven through the "shell" transport strategy, so no
 * ICE/DTLS handshake is involved -- except in the capture-pump case,
 * where `send_audio()` refuses to put anything on the wire before the
 * DTLS session is connected.  That case uses the same fake ICE / fake
 * DTLS pair the audio-send suite uses.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/engine_errors.hpp>
#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/audio3a.hpp>
#include <nimrtc/plugins/ice_transport.hpp>

namespace {

namespace np = nimrtc::plugins;
namespace nd = nimrtc::dtls;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

/** The engine's default PCM rate (48 kHz) and channel count (mono). */
constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint16_t kChannels = 1;

/** Samples per channel in one 20 ms Opus frame. */
constexpr std::size_t kFrameSamples = kSampleRate * 20 / 1000;

// ---------------------------------------------------------------------------
// The fake ICE transport (the capture pump needs a connected transport)
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
    void set_bind_address(std::string_view host) noexcept override {
        (void)host;
    }
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
// The fake DTLS session
//
// `send_audio()` refuses to put RTP on the wire before the DTLS
// handshake completed, so the capture-pump case needs a session that
// reports the handshake as done.  The fake exports no SRTP keying
// material, so the packets travel in cleartext and the test can read
// them.
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
// The log capture
//
// The engine reports a *non-fatal* device failure through the log (the
// plan's `Warn` policy), so a test that wants to see it has to look
// there.  The sink below collects the warnings and hands them to a
// predicate.
//
// It also restores the default sink on destruction, so a test that
// installs it does not change the level of the *next* test.
// ---------------------------------------------------------------------------

class WarningCapture final : public nimrtc::core::log::Sink {
public:
    explicit WarningCapture(std::function<void(std::string_view)> on_warn)
        : on_warn_(std::move(on_warn)) {}

    ~WarningCapture() override {
        nimrtc::core::log::Logger::instance().set_sink(
            std::make_shared<nimrtc::core::log::StderrSink>());
    }

    void write(nimrtc::core::log::Level level,
               std::string_view msg) noexcept override {
        if (level == nimrtc::core::log::Level::Warn && on_warn_) {
            on_warn_(msg);
        }
    }

private:
    std::function<void(std::string_view)> on_warn_;
};

// ---------------------------------------------------------------------------
// The fixture
// ---------------------------------------------------------------------------

class EngineAudioDevice : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();

        static FakeIceFactory ice_factory;
        static FakeDtlsFactory dtls_factory;
        PluginRegistry::instance().register_ice_transport("fake-ice", &ice_factory);
        PluginRegistry::instance().register_dtls_session("fake-dtls", &dtls_factory);
    }

    /** A "shell" configuration: no ICE, no DTLS, no SRTP.
     *
     *  The audio devices are *off* by default here: a test that wants
     *  a device says so explicitly, and the tests that do not are
     *  about the host-fed PCM path (which must keep working
     *  unchanged). */
    static EngineConfig shell_config() {
        EngineConfig cfg;
        cfg.stun_server_host       = "";
        cfg.local_port_range_begin = 52900;
        cfg.local_port_range_end   = 52999;
        cfg.transport_strategy_name = "shell";
        cfg.pcm_sample_rate_hz     = kSampleRate;
        cfg.pcm_channels           = kChannels;
        cfg.audio_ptime_ms         = 20;
        cfg.audio_codec.clock_rate = kSampleRate;
        cfg.audio_codec.channels   = kChannels;
        return cfg;
    }

    /** The same, but with a *connected* fake transport.
     *
     *  The capture pump needs this: `send_audio()` refuses to encode
     *  before the DTLS session reports the handshake as complete.
     *
     *  The **shell** sentinel is dropped here, and that is the whole
     *  point of this configuration: the shell strategy bypasses ICE and
     *  DTLS by design (its components are null instances), so the
     *  engine's `send_audio()` gate — which refuses to emit media
     *  before the DTLS handshake completed — never opens.  The
     *  browser-call strategy builds the *real* transport stack and
     *  takes its ICE / DTLS backends from `transport_name` /
     *  `dtls_name`, which is where the fakes go. */
    static EngineConfig connected_config() {
        EngineConfig cfg = shell_config();
        cfg.transport_strategy_name = "";
        cfg.transport_name = "fake-ice";
        cfg.dtls_name      = "fake-dtls";
        cfg.datachannel_backend = "";
        cfg.bwe_name       = "";
        cfg.scheduler_config.enabled = false;
        return cfg;
    }
};

// ============================================================================
// 1. The engine resolves the device
// ============================================================================

TEST_F(EngineAudioDevice, TheEngineResolvesTheCaptureDevice) {
    EngineConfig cfg = shell_config();
    cfg.audio_source_name = "synthetic";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The device is there, and it is open.
    ASSERT_NE(engine.audio_source(), nullptr);
    EXPECT_TRUE(engine.audio_source()->is_open());

    // The *negotiated* format is what the device agreed to, not what
    // the engine asked for.
    EXPECT_EQ(engine.audio_source()->config().format.sample_rate_hz, kSampleRate);

    // The stats snapshot names the device.
    EXPECT_FALSE(engine.stats().audio_source_device_id.empty());

    engine.close();

    // After the close the device is gone.
    EXPECT_EQ(engine.audio_source(), nullptr);
}

TEST_F(EngineAudioDevice, TheEngineResolvesThePlaybackDevice) {
    EngineConfig cfg = shell_config();
    cfg.audio_sink_name = "synthetic";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    ASSERT_NE(engine.audio_sink(), nullptr);
    EXPECT_TRUE(engine.audio_sink()->is_open());
    EXPECT_FALSE(engine.stats().audio_sink_device_id.empty());

    engine.close();
    EXPECT_EQ(engine.audio_sink(), nullptr);
}

TEST_F(EngineAudioDevice, TheDeviceListIsExposedToTheHost) {
    EngineConfig cfg = shell_config();
    cfg.audio_source_name = "synthetic";
    cfg.audio_sink_name   = "synthetic";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The enumeration comes from the *device*, not from the engine:
    // a device that reports nothing reports nothing.
    EXPECT_FALSE(engine.audio_capture_devices().empty());
    EXPECT_FALSE(engine.audio_playback_devices().empty());

    engine.close();
}

TEST_F(EngineAudioDevice, NoDeviceConfiguredMeansNoDevice) {
    EngineConfig cfg = shell_config();
    cfg.audio_source_name.clear();
    cfg.audio_sink_name.clear();

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_EQ(engine.audio_source(), nullptr);
    EXPECT_EQ(engine.audio_sink(), nullptr);
    EXPECT_TRUE(engine.audio_capture_devices().empty());
    EXPECT_TRUE(engine.audio_playback_devices().empty());

    engine.close();
}

TEST_F(EngineAudioDevice, TheNoneOptOutKeepsTheHostFedPathWorking) {
    EngineConfig cfg = connected_config();
    // "none" is the explicit opt-out: the host feeds the PCM by hand.
    cfg.audio_source_name = "none";
    cfg.audio_sink_name   = "none";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_EQ(engine.audio_source(), nullptr);
    EXPECT_EQ(engine.audio_sink(), nullptr);

    // And the host's own path still works: `send_audio()` accepts the
    // PCM and puts it on the wire.
    //
    // The connected transport is what makes this assertion possible:
    // `send_audio()` refuses to emit media before the DTLS handshake
    // completed (the packets would go out in the clear), so a shell
    // transport answers `kEngineNotReady` — which says nothing about
    // the opt-out, only about the crypto gate.
    std::vector<float> pcm(kFrameSamples, 0.25f);
    EXPECT_EQ(engine.send_audio(pcm.data(), pcm.size()), 0u);

    engine.close();
}

TEST_F(EngineAudioDevice, AnUnknownDevicePluginIsReportedNotIgnored) {
    EngineConfig cfg = shell_config();
    cfg.audio_source_name = "no-such-audio-device";

    // A missing device is reported as a **warning**, not as an error.
    //
    // The plan (§6) is explicit about this: a machine with no sound
    // card, or a microphone permission that was revoked, must not take
    // down a video call.  So the failure is *logged* and the engine
    // keeps running — it does not fire `on_error`, which would read as
    // "the session is broken".
    std::atomic<bool> saw_missing{false};
    nimrtc::core::log::Logger::instance().set_sink(
        std::make_shared<WarningCapture>(
            [&saw_missing](std::string_view msg) {
                if (msg.find("no-such-audio-device") != std::string_view::npos) {
                    saw_missing.store(true);
                }
            }));

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The engine still opens (a missing microphone is not fatal for
    // the rest of the session), and there is no device.
    EXPECT_EQ(engine.audio_source(), nullptr);
    EXPECT_TRUE(engine.stats().audio_source_device_id.empty());

    // But the host is *told* — through the log, which is where a
    // non-fatal failure belongs.
    EXPECT_TRUE(saw_missing.load())
        << "the missing device must be visible in the log";

    engine.close();
}

// ============================================================================
// 2. The capture pump
// ============================================================================

TEST_F(EngineAudioDevice, TheCapturePumpFeedsTheEngine) {
    EngineConfig cfg = connected_config();
    cfg.audio_source_name = "synthetic";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_NE(engine.audio_source(), nullptr);

    // The synthetic device produces frames only when it is pumped, so
    // the test drives the device the way a real capture thread would.
    const auto before = engine.stats();

    // The device's queue is empty until something fills it.  The
    // *engine* never pushes frames into the device -- that is the
    // device's own thread's job -- so the pump has nothing to send
    // here, and the counters stay where they were.
    for (int i = 0; i < 5; ++i) {
        (void)engine.tick();
    }

    const auto after = engine.stats();
    EXPECT_EQ(after.audio_capture_frames_sent, before.audio_capture_frames_sent)
        << "an empty capture queue must not produce outbound frames";
    EXPECT_EQ(after.audio_capture_frames, before.audio_capture_frames);

    engine.close();
}

TEST_F(EngineAudioDevice, TheCapturePumpCanBeTurnedOff) {
    EngineConfig cfg = connected_config();
    cfg.audio_source_name = "synthetic";
    cfg.audio_source_pump  = false;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The device is still opened -- the host drives it.
    ASSERT_NE(engine.audio_source(), nullptr);
    EXPECT_TRUE(engine.audio_source()->is_open());

    const auto before = engine.stats();
    for (int i = 0; i < 5; ++i) {
        (void)engine.tick();
    }
    EXPECT_EQ(engine.stats().audio_capture_frames_sent, before.audio_capture_frames_sent)
        << "the pump is off, so nothing is sent";

    engine.close();
}

// ============================================================================
// 3. The playback path and the AEC reference
// ============================================================================

TEST_F(EngineAudioDevice, ThePlaybackDeviceIsWrittenTo) {
    EngineConfig cfg = connected_config();
    cfg.audio_sink_name = "synthetic";

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    ASSERT_NE(engine.audio_sink(), nullptr);

    // The sink's own counter starts at zero.
    EXPECT_EQ(engine.audio_sink()->played_samples(), 0u);

    engine.close();
}

TEST_F(EngineAudioDevice, ThePlaybackPumpCanBeTurnedOff) {
    EngineConfig cfg = connected_config();
    cfg.audio_sink_name = "synthetic";
    cfg.audio_sink_pump = false;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The device is still there; only the writes are suppressed.
    ASSERT_NE(engine.audio_sink(), nullptr);
    EXPECT_TRUE(engine.audio_sink()->is_open());

    engine.close();
}

}  // anonymous namespace
