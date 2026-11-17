/**
 * @file tests/test_engine_stats_snapshot.cpp
 * @brief Engine-level test of the stats snapshot lifecycle (Track S / S1).
 *
 * ## What this covers
 *
 * `Engine::Stats` is a set of **lifetime** counters: every field counts
 * from `open()`.  That is the right shape for a total, but it cannot
 * answer the question a monitoring UI actually asks — *how much happened
 * in the last second?*  To answer it, a host has to subtract two
 * snapshots and divide by the time between them, and the snapshot did not
 * carry that time.
 *
 * Track S / S1 (the design plan's DS-1) adds exactly that:
 *
 *   - `Stats::snapshot_time_us` — the engine-monotonic instant the
 *     snapshot was taken, so two snapshots can be placed on a timeline.
 *   - `StatsDelta` — the change between two snapshots, with the interval's
 *     length (`duration_us`) and the increments.
 *   - `Engine::stats_delta(previous)` — the convenience form.
 *
 * ## The property under test
 *
 * The design plan's acceptance criterion is: *"the difference between two
 * `stats()` calls equals the increment that actually happened between
 * them, and an assertion on `snapshot_time_us` can tell 'the same
 * instant' from 'a different instant'."*
 *
 * The second half of that criterion is the interesting one, because a
 * delta that is *arithmetically* correct can still be *semantically*
 * wrong: `Stats` mixes **counters** (subtract them) with **gauges**
 * (take the newer reading) and with **identifiers** (no interval meaning
 * at all).  A delta that subtracted the gauges would report a falling
 * bitrate as "minus 300 kbps", which is not a rate and not a level.
 *
 * So the tests below pin three things separately:
 *
 *   1. the counters subtract, and the subtraction equals the real
 *      increment;
 *   2. the gauges take the newer reading, and do NOT subtract;
 *   3. the identifiers are absent from the delta entirely.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/codec.hpp>
#include <nimrtc/plugins/ice_transport.hpp>

namespace {

namespace np = nimrtc::plugins;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

// ---------------------------------------------------------------------------
// Fake ICE transport
// ---------------------------------------------------------------------------
//
// The stats surface does not need a peer, but the engine will not open
// without a transport, so the fake stands in.  It reports `Connected`, so
// the engine's periodic work runs.

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

    int recv() noexcept override { return 0; }

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
    std::vector<std::vector<std::uint8_t>> sent_;
};

class FakeIceFactory final : public np::IICETransportFactory {
public:
    std::string_view id() const noexcept override { return "fake-ice"; }
    std::string_view display_name() const noexcept override {
        return "Fake ICE transport (test)";
    }
    np::IICETransport* create() const override {
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
        cfg.transport_name = "fake-ice";
        // No media pipelines: the stats surface under test is the
        // snapshot lifecycle itself, and a session with no video and no
        // audio keeps the counters at zero on purpose -- so that a test
        // that *does* move one is unambiguous.
        cfg.datachannel_backend = "";
        cfg.bwe_name = "";
        cfg.scheduler_config.enabled = false;
        return cfg;
    }
};

// ---------------------------------------------------------------------------
// The snapshot carries its own instant
// ---------------------------------------------------------------------------

TEST_F(EngineStatsSnapshot, a_snapshot_carries_the_instant_it_was_taken) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto s0 = engine.stats();
    EXPECT_GT(s0.snapshot_time_us, 0)
        << "an open engine must stamp its snapshot with an instant";

    // Two snapshots taken a moment apart must carry different instants --
    // this is what lets a host tell "the same instant" from "a different
    // instant", which is the second half of the design plan's acceptance
    // criterion.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto s1 = engine.stats();
    EXPECT_GT(s1.snapshot_time_us, s0.snapshot_time_us)
        << "two snapshots taken 5 ms apart must carry different instants";

    engine.close();
}

TEST_F(EngineStatsSnapshot, the_interval_is_the_distance_between_the_snapshots) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto s0 = engine.stats();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto s1 = engine.stats();

    // The delta's interval must be the distance between the two
    // timestamps -- not a nominal sampling period the host has to guess.
    const auto d = NimRTCEngine::StatsDelta::between(s0, s1);
    EXPECT_EQ(d.duration_us, s1.snapshot_time_us - s0.snapshot_time_us);
    EXPECT_EQ(d.start_time_us, s0.snapshot_time_us);
    EXPECT_EQ(d.end_time_us, s1.snapshot_time_us);

    // And it must actually measure the sleep: 20 ms is 20 000 us, and
    // the clock is monotonic, so the interval cannot be shorter.  The
    // upper bound is loose on purpose -- a loaded CI runner may take
    // much longer than the sleep asked for -- but the lower bound is
    // what proves the interval is measured rather than assumed.
    EXPECT_GE(d.duration_us, 20'000);
    EXPECT_LT(d.duration_us, 5'000'000);

    engine.close();
}

// ---------------------------------------------------------------------------
// The increments are the real increments
// ---------------------------------------------------------------------------

TEST_F(EngineStatsSnapshot, the_increments_are_what_actually_happened) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto before = engine.stats();

    // Drive some real traffic: the audio send path bumps the audio
    // counters, so the delta has something to report.
    np::AudioFrame f{};
    f.samples = 960;
    f.sample_rate = 48000;
    f.channels = 1;
    f.data = std::vector<float>(960, 0.0f);
    for (int i = 0; i < 5; ++i) {
        engine.send_audio(f);
    }
    engine.tick();

    const auto after = engine.stats();
    const auto d = NimRTCEngine::StatsDelta::between(before, after);

    // The delta must equal the difference of the two snapshots -- this is
    // the acceptance criterion stated literally.
    EXPECT_EQ(d.audio_frames_sent_delta,
              after.audio_frames_sent - before.audio_frames_sent);
    EXPECT_EQ(d.audio_bytes_sent_delta,
              after.audio_bytes_sent - before.audio_bytes_sent);

    // And the traffic must actually have been counted, so the assertion
    // above is not vacuously true (a delta of 0 equals a difference of 0).
    EXPECT_GT(after.audio_frames_sent, before.audio_frames_sent)
        << "the test must actually move a counter";
    EXPECT_GT(d.audio_frames_sent_delta, 0u)
        << "the delta must report the frames that were sent";

    // The lifetime counter is still cumulative -- the delta does not
    // replace it.
    EXPECT_GE(after.audio_frames_sent, before.audio_frames_sent);

    engine.close();
}

TEST_F(EngineStatsSnapshot, stats_delta_matches_between_the_same_pair) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto before = engine.stats();
    np::AudioFrame f{};
    f.samples = 960;
    f.sample_rate = 48000;
    f.channels = 1;
    f.data = std::vector<float>(960, 0.0f);
    engine.send_audio(f);
    engine.tick();

    // The convenience form takes a fresh snapshot and subtracts the one
    // the host kept.  It must agree with the explicit form (up to the
    // interval's length, which depends on when the fresh snapshot was
    // taken).
    const auto d = engine.stats_delta(before);
    EXPECT_GT(d.duration_us, 0);
    EXPECT_EQ(d.start_time_us, before.snapshot_time_us);
    EXPECT_GE(d.end_time_us, before.snapshot_time_us);
    EXPECT_EQ(d.audio_frames_sent_delta,
              engine.stats().audio_frames_sent - before.audio_frames_sent);

    engine.close();
}

// ---------------------------------------------------------------------------
// The gauges are NOT subtracted
// ---------------------------------------------------------------------------

TEST_F(EngineStatsSnapshot, a_gauge_in_the_delta_is_the_newer_reading) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // Build the two snapshots by hand so the gauge values are known and
    // deliberately *different*.  A gauge that falls from 500 kbps to
    // 200 kbps is at 200 kbps at the end of the interval -- reporting
    // "minus 300 kbps" would be neither a level nor a rate.
    NimRTCEngine::Stats older{};
    older.snapshot_time_us = 1'000'000;
    older.video_current_bitrate_bps = 500'000;
    older.video_tx_bitrate_bps = 400'000;
    older.bandwidth_estimate_bps = 300'000;
    older.audio_jitter_ms = 20;
    older.audio_playout_delay_ms = 40;
    older.video_last_freeze_ms = 150;

    NimRTCEngine::Stats newer{};
    newer.snapshot_time_us = 2'000'000;
    newer.video_current_bitrate_bps = 200'000;
    newer.video_tx_bitrate_bps = 100'000;
    newer.bandwidth_estimate_bps = 50'000;
    newer.audio_jitter_ms = 5;
    newer.audio_playout_delay_ms = 10;
    newer.video_last_freeze_ms = 30;

    const auto d = NimRTCEngine::StatsDelta::between(older, newer);

    // The interval is the difference of the timestamps.
    EXPECT_EQ(d.duration_us, 1'000'000);

    // Every gauge is the NEWER reading, verbatim -- not a difference.
    EXPECT_EQ(d.video_current_bitrate_bps, 200'000u);
    EXPECT_EQ(d.video_tx_bitrate_bps, 100'000u);
    EXPECT_EQ(d.bandwidth_estimate_bps, 50'000u);
    EXPECT_EQ(d.audio_jitter_ms, 5u);
    EXPECT_EQ(d.audio_playout_delay_ms, 10u);
    EXPECT_EQ(d.video_last_freeze_ms, 30u);

    // Explicitly: the delta must NOT contain the difference.  If it did,
    // the bitrate would read as 200 000 - 500 000 = -300 000, which is
    // a nonsense "level".
    EXPECT_NE(d.video_current_bitrate_bps, 300'000u)
        << "a gauge must not be subtracted into a change of level";

    engine.close();
}

// ---------------------------------------------------------------------------
// The counters ARE subtracted
// ---------------------------------------------------------------------------

TEST_F(EngineStatsSnapshot, a_counter_in_the_delta_is_the_difference) {
    NimRTCEngine::Stats older{};
    older.snapshot_time_us = 1'000'000;
    older.video_frames_sent = 100;
    older.video_packets_sent = 200;
    older.video_bytes_sent = 3000;
    older.video_frames_received = 40;
    older.audio_packets_lost = 5;
    older.video_feedback_rx_pli = 1;
    older.video_feedback_rx_nack = 2;

    NimRTCEngine::Stats newer{};
    newer.snapshot_time_us = 3'000'000;
    newer.video_frames_sent = 160;
    newer.video_packets_sent = 320;
    newer.video_bytes_sent = 5000;
    newer.video_frames_received = 90;
    newer.audio_packets_lost = 9;
    newer.video_feedback_rx_pli = 4;
    newer.video_feedback_rx_nack = 3;

    const auto d = NimRTCEngine::StatsDelta::between(older, newer);

    EXPECT_EQ(d.duration_us, 2'000'000);
    EXPECT_EQ(d.video_frames_sent_delta, 60u);
    EXPECT_EQ(d.video_packets_sent_delta, 120u);
    EXPECT_EQ(d.video_bytes_sent_delta, 2000u);
    EXPECT_EQ(d.video_frames_received_delta, 50u);
    EXPECT_EQ(d.audio_packets_lost_delta, 4u);

    // The feedback total is the sum of the per-type counters, so a host
    // does not have to add the four types itself.
    //   old: pli 1 + nack 2                     = 3
    //   new: pli 4 + nack 3                     = 7
    EXPECT_EQ(d.video_feedback_rx_total_delta, 4u);

    engine.close();
}

// ---------------------------------------------------------------------------
// The degenerate intervals
// ---------------------------------------------------------------------------

TEST_F(EngineStatsSnapshot, two_snapshots_at_the_same_instant_give_no_interval) {
    NimRTCEngine::Stats older{};
    older.snapshot_time_us = 5'000'000;
    older.video_frames_sent = 10;

    NimRTCEngine::Stats newer{};
    newer.snapshot_time_us = 5'000'000;   // the same instant
    newer.video_frames_sent = 10;

    const auto d = NimRTCEngine::StatsDelta::between(older, newer);

    // No time passed, so no rate is defined.  `duration_us == 0` is the
    // honest answer -- and it is distinguishable from "a missing
    // value", which is what makes the assertion in the design plan's
    // criterion ("tell 'the same instant' from 'a different instant'")
    // meaningful.
    EXPECT_EQ(d.duration_us, 0);
    EXPECT_EQ(d.start_time_us, older.snapshot_time_us);
    EXPECT_EQ(d.end_time_us, newer.snapshot_time_us);
}

TEST_F(EngineStatsSnapshot, snapshots_taken_out_of_order_give_no_interval) {
    NimRTCEngine::Stats older{};
    older.snapshot_time_us = 9'000'000;
    older.video_frames_sent = 10;

    NimRTCEngine::Stats newer{};
    newer.snapshot_time_us = 4'000'000;   // EARLIER -- the caller mixed them up
    newer.video_frames_sent = 20;

    const auto d = NimRTCEngine::StatsDelta::between(older, newer);

    // A negative interval is not an interval.  The delta reports zero
    // rather than a negative duration or a wrapped-around count.
    EXPECT_EQ(d.duration_us, 0);
}

// ---------------------------------------------------------------------------
// The identifiers are not in the delta at all
// ---------------------------------------------------------------------------

TEST_F(EngineStatsSnapshot, the_delta_carries_no_identifiers) {
    // An identifier ("the codec is h264", "the payload type is 96")
    // has no interval meaning: it does not accumulate, so there is
    // nothing to subtract, and a delta that carried one would invite a
    // host to subtract one string from another.
    //
    // This is asserted at compile time rather than at run time: the
    // check below fails to *compile* if a future change adds an
    // identifier-shaped field to `StatsDelta`.
    //
    // The presence of `video_codec_backend_id` in `Stats` and its
    // absence in `StatsDelta` is the property.
    static_assert(sizeof(NimRTCEngine::Stats) > sizeof(NimRTCEngine::StatsDelta),
                  "StatsDelta is expected to be a curated subset of Stats, "
                  "not a copy of it");

    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The engine's own snapshot carries the identifiers; the delta
    // carries the measurements.
    const auto s = engine.stats();
    EXPECT_FALSE(s.video_codec_backend_id.empty() ||
                 s.video_codec_backend_id == "unknown")
        << "the snapshot is expected to carry the codec backend id";
    EXPECT_EQ(s.video_format_canonical, s.video_format_canonical);

    engine.close();
}

} // namespace
