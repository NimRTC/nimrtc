/**
 * @file tests/test_engine_stats_snapshot.cpp
 * @brief The engine's statistics snapshot lifecycle (plan Track S / S1,
 *        design plan DS-1).
 *
 * This is the test that pins DS-1's acceptance criterion:
 *
 *   > "两次 stats() 之间的差值等于两次调用之间实际发生的增量；对
 *   >  snapshot_time_us 的断言能区分'同一时刻'与'不同时刻'"
 *
 *   ("the difference between two stats() calls equals the increment
 *    that actually happened between them; an assertion on
 *    snapshot_time_us can tell 'the same instant' from 'two different
 *    instants'")
 *
 * The engine is driven through a *fake ICE transport* registered under
 * the id "fake-ice" — the same pattern `test_engine_audio_stats.cpp`
 * uses.  The fake reports `Connected`, so the engine's periodic work
 * runs.  No DTLS handshake completes (there is no peer), which is fine:
 * the statistics lifecycle does not depend on the transport.
 *
 * What is pinned here:
 *
 *   1. **The reading's identity** — `Stats::snapshot_time_us` and
 *      `Stats::snapshot_seq` say when the counters were measured and
 *      in what order the readings were taken.
 *   2. **The delta is the increment** — the difference between two
 *      readings is exactly what happened in between, not the absolute
 *      value and not something else.
 *   3. **The timestamp distinguishes the instants** — two readings
 *      taken at different times carry different timestamps; the
 *      sequence number orders them even when the timestamps are equal.
 *   4. **A gauge is not a counter** — the delta API refuses to diff a
 *      gauge, because the difference of two instantaneous bitrates is
 *      not a bitrate.
 *   5. **The registry and the flat struct agree** — the snapshot is a
 *      projection of `Engine::Stats`, so the two can never drift.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/core/stats_registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/codec.hpp>
#include <nimrtc/plugins/ice_transport.hpp>

namespace {

namespace np = nimrtc::plugins;

using nimrtc::core::PluginRegistry;
using nimrtc::core::StatEntry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

using namespace std::chrono_literals;

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
        on_recv_ = std::move(on_recv);
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
            if (on_recv_) {
                on_recv_(np::BufferView(pkt.data(), pkt.size()));
            }
            ++n;
        }
        return n;
    }

    np::Addr local_addr() const noexcept override { return {}; }
    np::Addr remote_addr() const noexcept override { return {}; }

    // ---- IICETransport ---------------------------------------------------
    np::IceState state() const noexcept override {
        return np::IceState::Connected;
    }
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
    np::IICETransport* create_ice() const override {
        return new FakeIceTransport();
    }
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class EngineStatsSnapshot : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();

        static FakeIceFactory ice_factory;
        PluginRegistry::instance().register_ice_transport("fake-ice", &ice_factory);
    }

    EngineConfig make_config() {
        EngineConfig cfg{};
        cfg.transport_name           = "fake-ice";
        cfg.datachannel_backend      = "";
        cfg.bwe_name                 = "";
        cfg.scheduler_config.enabled = false;
        return cfg;
    }

    /** Send @p n audio frames through the engine's send path.
     *
     *  The Opus plugin may not be registered in every build; the caller
     *  checks `s.audio_frames_sent` afterwards. */
    static void send_audio_frames(NimRTCEngine& engine, int n) {
        for (int i = 0; i < n; ++i) {
            std::vector<float> pcm(960, 0.1f);
            engine.send_audio(pcm.data(), pcm.size());
        }
    }
};

// ============================================================================
// 1. The reading's identity
// ============================================================================

TEST_F(EngineStatsSnapshot, AFreshEngineCarriesAReadingIdentity) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto s = engine.stats();

    // The timestamp is on the engine's monotonic clock, so it is a
    // non-zero number well after boot.
    EXPECT_GT(s.snapshot_time_us, 0)
        << "the reading must carry the instant it was taken";
    EXPECT_GT(s.snapshot_seq, 0u)
        << "the reading must carry a sequence number";

    engine.close();
}

TEST_F(EngineStatsSnapshot, TheSequenceNumberIncreasesByOnePerReading) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto a = engine.stats();
    const auto b = engine.stats();
    const auto c = engine.stats();

    EXPECT_EQ(b.snapshot_seq, a.snapshot_seq + 1);
    EXPECT_EQ(c.snapshot_seq, b.snapshot_seq + 1);

    engine.close();
}

TEST_F(EngineStatsSnapshot, TheSnapshotSequenceNumberMatchesTheFlatOne) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto flat = engine.stats();
    const auto snap = engine.stats_snapshot();

    // Each public entry point takes exactly **one** reading, so the
    // snapshot is the reading *after* the flat struct's — never the same
    // one, and never a different instant wearing the same number.
    EXPECT_EQ(snap.seq(), flat.snapshot_seq + 1)
        << "each reading advances the sequence by exactly one";
    EXPECT_GE(snap.time_us(), flat.snapshot_time_us)
        << "the snapshot's timestamp must not precede the flat one's";

    // Two readings of the *same* instant are impossible: the flat
    // struct and the snapshot are two separate observations, so the
    // host can always tell them apart.
    EXPECT_NE(snap.seq(), flat.snapshot_seq);

    engine.close();
}

// ============================================================================
// 2. The timestamp distinguishes the instants
// ============================================================================

TEST_F(EngineStatsSnapshot, TwoReadingsTakenAtDifferentTimesCarryDifferentTimestamps) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto first = engine.stats();

    // Sleep long enough that the microsecond clock certainly moved.
    std::this_thread::sleep_for(20ms);

    const auto second = engine.stats();

    EXPECT_GT(second.snapshot_time_us, first.snapshot_time_us)
        << "two readings taken 20 ms apart must carry different "
           "timestamps -- this is what lets a host tell 'the same "
           "instant' from 'two different instants'";

    engine.close();
}

TEST_F(EngineStatsSnapshot, TheSequenceNumberOrdersTwoReadingsEvenWithEqualTimestamps) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto a = engine.stats_snapshot();
    const auto b = engine.stats_snapshot();

    // The timestamps may be equal (a coarse clock, or two readings
    // inside the same microsecond); the sequence number must order
    // them anyway, because "the interval between these two readings"
    // has to be well-defined.
    EXPECT_LT(a.seq(), b.seq());
    EXPECT_LE(a.time_us(), b.time_us());

    engine.close();
}

// ============================================================================
// 3. The delta is the increment
// ============================================================================

TEST_F(EngineStatsSnapshot, TheDeltaOfTwoReadingsIsTheIncrement) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto before = engine.stats_snapshot();

    // The counters must not move on their own, so the increment the
    // delta reports is exactly what this loop produced.
    const std::uint64_t frames_before = engine.stats().audio_frames_sent;

    send_audio_frames(engine, 5);
    engine.tick();
    std::this_thread::sleep_for(50ms);

    const std::uint64_t frames_after = engine.stats().audio_frames_sent;

    const auto d = engine.stats_delta_since(before);
    ASSERT_TRUE(d.valid())
        << "a delta against a real reading must be valid";

    std::int64_t delta_frames = 0;
    ASSERT_TRUE(d.counter_delta("", "audio_frames_sent", delta_frames));

    EXPECT_EQ(static_cast<std::uint64_t>(delta_frames),
              frames_after - frames_before)
        << "the delta must be the increment that actually happened "
           "between the two readings";

    engine.close();
}

TEST_F(EngineStatsSnapshot, TheDeltaCarriesTheIntervalDuration) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto before = engine.stats_snapshot();
    std::this_thread::sleep_for(30ms);
    const auto d = engine.stats_delta_since(before);

    ASSERT_TRUE(d.valid());
    EXPECT_GT(d.duration_us(), 0)
        << "the delta must carry the interval's length, so a host can "
           "turn an increment into a rate without keeping its own clock";
    EXPECT_GT(d.to_seq(), d.from_seq());

    engine.close();
}

TEST_F(EngineStatsSnapshot, TheDeltaOfTwoUnchangedReadingsIsZero) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto before = engine.stats_snapshot();
    std::this_thread::sleep_for(20ms);
    const auto d = engine.stats_delta_since(before);

    ASSERT_TRUE(d.valid());

    std::int64_t delta_frames = 0;
    ASSERT_TRUE(d.counter_delta("", "video_frames_sent", delta_frames));
    EXPECT_EQ(delta_frames, 0)
        << "nothing happened between the two readings, so the increment "
           "must be zero";

    engine.close();
}

TEST_F(EngineStatsSnapshot, ADefaultSnapshotHasNoUsableBaseline) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // A default-constructed snapshot means "no baseline": the delta
    // would be measured against a made-up zero, so it is refused.
    const auto d = engine.stats_delta_since(nimrtc::core::StatSnapshot{});
    EXPECT_FALSE(d.valid());

    std::int64_t frames = 0;
    EXPECT_FALSE(d.counter_delta("", "video_frames_sent", frames));

    engine.close();
}

// ============================================================================
// 4. A gauge is not a counter
// ============================================================================

TEST_F(EngineStatsSnapshot, TheDeltaRefusesToDiffAGauge) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto before = engine.stats_snapshot();
    std::this_thread::sleep_for(20ms);
    const auto d = engine.stats_delta_since(before);
    ASSERT_TRUE(d.valid());

    // `video_tx_bitrate_bps` is an instantaneous value: the difference
    // of two readings is not a bitrate, so the delta API refuses.
    std::int64_t bitrate = 0;
    EXPECT_FALSE(d.counter_delta("", "video_tx_bitrate_bps", bitrate));

    // The gauge's value at the end of the interval is the answer.
    ASSERT_TRUE(d.gauge_value("", "video_tx_bitrate_bps", bitrate));
    EXPECT_GE(bitrate, 0);

    // The mirror image: a counter is not a gauge.
    EXPECT_FALSE(d.gauge_value("", "video_frames_sent", bitrate));

    engine.close();
}

// ============================================================================
// 5. The registry and the flat struct agree
// ============================================================================

TEST_F(EngineStatsSnapshot, TheSnapshotProjectsTheFlatStruct) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto flat = engine.stats();
    const auto snap = engine.stats_snapshot();

    ASSERT_FALSE(snap.empty());

    // Every numeric field of the flat struct must be readable from the
    // snapshot with the same value — the snapshot is a *projection*,
    // so the two can never drift.
    std::int64_t v = 0;
    ASSERT_TRUE(snap.value_of("", "video_frames_sent", v));
    EXPECT_EQ(v, static_cast<std::int64_t>(flat.video_frames_sent));

    ASSERT_TRUE(snap.value_of("", "srtp_drops", v));
    EXPECT_EQ(v, static_cast<std::int64_t>(flat.srtp_drops));

    ASSERT_TRUE(snap.value_of("", "audio_frames_sent", v));
    EXPECT_EQ(v, static_cast<std::int64_t>(flat.audio_frames_sent));

    engine.close();
}

TEST_F(EngineStatsSnapshot, TheSnapshotCarriesTheSemantics) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto snap = engine.stats_snapshot();

    // A counter.
    StatEntry e;
    ASSERT_TRUE(snap.find("", "video_frames_sent", e));
    EXPECT_EQ(e.semantics, nimrtc::core::StatSemantics::kCumulative);

    // A gauge.
    ASSERT_TRUE(snap.find("", "video_tx_bitrate_bps", e));
    EXPECT_EQ(e.semantics, nimrtc::core::StatSemantics::kGauge);

    engine.close();
}

TEST_F(EngineStatsSnapshot, TheSnapshotOutlivesTheEngine) {
    nimrtc::core::StatSnapshot kept;
    {
        EngineConfig cfg = make_config();
        NimRTCEngine engine(cfg);
        ASSERT_EQ(engine.open(), 0u);

        kept = engine.stats_snapshot();
        engine.close();
    }

    // The engine is gone; the reading is still usable as a baseline.
    std::int64_t v = 0;
    EXPECT_TRUE(kept.value_of("", "video_frames_sent", v));
    EXPECT_GT(kept.seq(), 0u);
}

}  // namespace
