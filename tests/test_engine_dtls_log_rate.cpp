// ============================================================================
// test_engine_dtls_log_rate.cpp — the engine's DTLS trace must be a *trace*.
//
// ## What the soak found
//
// The 2026-11-20 Q5 impaired soak printed
//
//     engine[dtls→srtp]: DTLS reached Connected, calling maybe_install_srtp_keys()
//
// **169 983 times in 22½ minutes** — 15 % of every line in the log — and
// that projects to roughly 2.6 GB of log for a single 24 h window.
//
// The cause is a guard on the *state* rather than on the *transition*:
//
//     if (new_state == DtlsState::Connected) { log(...); maybe_install_srtp_keys(); }
//
// `state() == Connected` is true on **every tick** for the whole life of
// the connection, so the "handshake reached Connected" line was printed
// once per tick instead of once per handshake.
//
// This is not cosmetic.  A log that repeats a line once per tick is not a
// handshake trace; it is noise that buries the transitions it is meant to
// announce, and it is exactly the kind of noise that hides a real failure
// in a 24 h soak — the whole point of the run is to be readable after the
// fact.
//
// ## The contract this test pins
//
//   1. The line is emitted **once per handshake** — when the state
//      *transitions* into Connected — not once per tick.
//   2. `maybe_install_srtp_keys()` is still called on **every** tick while
//      Connected.  Narrowing the *call* to the transition would be a
//      regression of a different kind: the RFC 5764 §4.2 keying material
//      is only readable once the Finished message has been processed, and
//      the tick that observes `Connected` is not necessarily the tick
//      that can read it.  The retry is what makes the install eventual
//      rather than lucky.
//
// ## The fakes
//
// The engine is driven through the same two fakes the ICE-send-gate suite
// uses: a **fake ICE transport** and a **fake DTLS session** that reports
// the handshake as complete and exports no SRTP keying material.
//
// Threading: nothing concurrent; standard GTest ordering.
// ============================================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/engine_errors.hpp>
#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/ice_transport.hpp>

namespace {

namespace np   = nimrtc::plugins;
namespace nd   = nimrtc::dtls;
namespace core = nimrtc::core;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

/// The marker the engine logs when the handshake completes.
constexpr std::string_view kConnectedMarker = "DTLS reached Connected";

// ---------------------------------------------------------------------------
// A log sink that counts the lines matching a needle.
//
// The engine's log goes to the core logger, so the only way to *see* what
// the engine said is to install a sink for the duration of the test (the
// same technique `tests/test_plugin_id_contract.cpp` uses).
// ---------------------------------------------------------------------------
class CountingSink final : public core::log::Sink {
public:
    void write(core::log::Level /*level*/,
               std::string_view msg) noexcept override {
        ++total_;
        if (msg.find(kConnectedMarker) != std::string_view::npos) {
            ++connected_;
        }
    }

    /// Every line the engine logged.
    [[nodiscard]] std::uint64_t total() const noexcept { return total_; }

    /// How many times the DTLS-Connected handshake line was logged.
    [[nodiscard]] std::uint64_t connected() const noexcept { return connected_; }

private:
    std::uint64_t total_ = 0;
    std::uint64_t connected_ = 0;
};

/// RAII installer for the counting sink.
class ScopedCountingSink {
public:
    ScopedCountingSink() {
        previous_ = core::log::Logger::instance().sink();
        core::log::Logger::instance().set_sink(
            std::make_shared<CountingSink>());
    }
    ~ScopedCountingSink() {
        core::log::Logger::instance().set_sink(previous_);
    }
    ScopedCountingSink(const ScopedCountingSink&) = delete;
    ScopedCountingSink& operator=(const ScopedCountingSink&) = delete;

    [[nodiscard]] CountingSink* get() const noexcept {
        return static_cast<CountingSink*>(
            core::log::Logger::instance().sink().get());
    }

private:
    core::log::SinkPtr previous_;
};

// ---------------------------------------------------------------------------
// Fake ICE transport — the DTLS drain is gated on ICE being connected, so
// the fake has to report Connected for `drain_dtls()` to reach the branch
// under test.
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

    np::Status send(np::BufferView, np::Addr = {}) noexcept override {
        return np::kOk;
    }

    int recv() noexcept override { return 0; }

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

private:
    np::RecvCallback  on_recv_;
    np::ErrorCallback on_error_;
};

class FakeIceFactory final : public np::IICETransportFactory {
public:
    std::string_view id() const noexcept override { return "fake-ice"; }
    std::string_view display_name() const noexcept override {
        return "Fake ICE transport (test)";
    }
    np::IICETransport* create_ice() const override {
        return new FakeIceTransport();
    }
};

// ---------------------------------------------------------------------------
// Fake DTLS session — reports the handshake as complete, exports no SRTP
// keying material.
//
// The engine asks two things of the DTLS session on the drain path: "is
// the handshake done?" and "give me the SRTP keys".  The fake answers the
// first with yes and the second with "not ready", so the install stays
// pending — which is exactly the state the soak ran in.
// ---------------------------------------------------------------------------

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
    core::Result<void> open() noexcept override {
        return core::Result<void>::make_ok();
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

class EngineDtlsLogRate : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        core::register_all_default_plugins();

        static FakeIceFactory  ice_factory;
        static FakeDtlsFactory dtls_factory;
        PluginRegistry::instance().register_ice_transport("fake-ice", &ice_factory);
        PluginRegistry::instance().register_dtls_session("fake-dtls", &dtls_factory);
    }

    EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.transport_name             = "fake-ice";
        cfg.dtls_name                  = "fake-dtls";
        cfg.datachannel_backend        = "";
        cfg.bwe_name                   = "";
        cfg.scheduler_config.enabled    = false;
        return cfg;
    }
};

// ============================================================================
// 1. The defect: the handshake line was logged once per tick.
//
// The engine is driven through `drain_dtls()` — the exact entry point the
// tick loop uses — many times over, and the handshake line must appear
// **once**, not once per call.
// ============================================================================

TEST_F(EngineDtlsLogRate, TheConnectedLineIsLoggedOnceNotOncePerTick) {
    ScopedCountingSink sink;

    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    // Drive the drain loop the way the tick loop does.  50 iterations is
    // far more than a handshake needs, so a per-tick log would show up as
    // ~50 lines.
    constexpr int kTicks = 50;
    for (int i = 0; i < kTicks; ++i) {
        engine.drain_dtls();
    }

    EXPECT_EQ(sink.get()->connected(), 1u)
        << "the DTLS-Connected handshake line must be logged ONCE per "
           "handshake, not once per tick (" << kTicks << " drains produced "
        << sink.get()->connected() << " lines). A per-tick handshake line is "
           "what filled the 24 h soak log with 170 000 identical lines in "
           "22 minutes.";

    engine.close();
}

// ============================================================================
// 2. The companion property: the *install* is still retried every tick.
//
// The line is narrowed; the work is not.  If the install were also
// narrowed to the transition, the engine would give up on a handshake
// whose keying material was not readable on the transition tick — the
// install would become "lucky" instead of "eventual".
//
// The observable proxy: the engine must keep asking, and must keep
// reporting the session as *not* encrypted while the material is missing.
// A narrowed install would still report "not encrypted", so the assertion
// that distinguishes them is the **absence** of the "no keying material"
// *warning* storm: the engine warns every time it cannot install, and a
// narrowed call would warn exactly once.
// ============================================================================

TEST_F(EngineDtlsLogRate, TheInstallIsStillRetriedWhileConnected) {
    class WarningCountingSink final : public core::log::Sink {
    public:
        void write(core::log::Level level,
                   std::string_view msg) noexcept override {
            if (level == core::log::Level::Warn &&
                msg.find("no keying material") != std::string_view::npos) {
                ++no_material_;
            }
        }
        [[nodiscard]] std::uint64_t no_material() const noexcept {
            return no_material_;
        }
    private:
        std::uint64_t no_material_ = 0;
    };

    auto previous = core::log::Logger::instance().sink();
    auto sink = std::make_shared<WarningCountingSink>();
    core::log::Logger::instance().set_sink(sink);

    NimRTCEngine engine(make_config());
    ASSERT_EQ(engine.open(), 0u);

    constexpr int kTicks = 20;
    for (int i = 0; i < kTicks; ++i) {
        engine.drain_dtls();
    }

    // The install must have been attempted on *every* tick: each attempt
    // that finds no material warns, so the warning count is the number of
    // attempts.
    EXPECT_EQ(sink->no_material(), static_cast<std::uint64_t>(kTicks))
        << "the SRTP install must be retried on every tick while the "
           "session is Connected (RFC 5764 §4.2 material is only readable "
           "after the Finished message, so the tick that observes "
           "'Connected' is not necessarily the tick that can read it). "
        << kTicks << " drains produced " << sink->no_material()
        << " install attempts.";

    EXPECT_FALSE(engine.srtp_installed())
        << "the fake exports no keying material, so the install must not "
           "report success";

    engine.close();
    core::log::Logger::instance().set_sink(previous);
}

}  // namespace
