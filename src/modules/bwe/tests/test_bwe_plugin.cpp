/**
 * @file src/modules/bwe/tests/test_bwe_plugin.cpp
 * @brief AIMD instance-behaviour tests through the plugins::IBwe seam.
 *
 * These tests verify that the concrete `nimrtc::bwe::Bwe` (AIMD)
 * implementation behaves correctly when driven *through* the plugin
 * interface — i.e. via `plugins::IBwe*` rather than the concrete
 * helper methods on `Bwe` directly.
 *
 * Why these belong here, not in `plugins/tests/`:
 *   - They hardcode the AIMD algorithm's response to loss / REMB /
 *     reset (specific decrease_factor=0.5, REMB trust=0.8, etc.).
 *   - The seam-only tests in `plugins/tests/test_bwe_plugin_seam.cpp`
 *     verify that the registry + factory wiring works; they don't
 *     pin any algorithm behaviour.
 *
 * @note P1 — IBwe is interface-only in P1; engine integration lands in P3.
 */

#include <gtest/gtest.h>

#include <cstdint>

#include <nimrtc/bwe/bwe.hpp>            // bwe::Bwe (concrete)
#include <nimrtc/plugins/bwe.hpp>        // plugins::IBwe

namespace {

using namespace nimrtc;

// Build a feedback sample with given loss rate.
plugins::BweFeedback make_feedback(double loss_rate,
                                   core::TimePoint ts,
                                   std::uint32_t packets_lost = 5,
                                   std::uint32_t packets_received = 100) {
    plugins::BweFeedback f;
    f.loss_rate = loss_rate;
    f.rtt = core::Milliseconds{50};
    f.timestamp = ts;
    f.packets_lost = packets_lost;
    f.packets_received = packets_received;
    return f;
}

} // namespace

// -----------------------------------------------------------------------------
// Dispatch through the plugin interface
// -----------------------------------------------------------------------------

TEST(BwePlugin, LossViaInterfaceCausesDecrease) {
    auto* ibwe = new nimrtc::bwe::Bwe();
    std::unique_ptr<plugins::IBwe> inst(ibwe);
    ASSERT_EQ(inst->open(), plugins::kOk);

    plugins::BweConfig cfg;
    cfg.initial_bitrate_bps = 1'000'000;
    cfg.decrease_factor = 0.5;
    cfg.smoothing_alpha = 1.0;
    inst->update_config(cfg);

    auto t0 = core::SteadyClock::now();
    inst->on_feedback(make_feedback(0.10, t0));

    auto e = inst->estimate();
    EXPECT_LE(e.target_bitrate_bps, 500'000u);
    EXPECT_EQ(e.reason, plugins::BweEstimate::Reason::AIMDDecrease);
}

TEST(BwePlugin, RembOverrideViaInterface) {
    auto* ibwe = new nimrtc::bwe::Bwe();
    std::unique_ptr<plugins::IBwe> inst(ibwe);
    ASSERT_EQ(inst->open(), plugins::kOk);

    plugins::BweConfig cfg;
    cfg.initial_bitrate_bps = 500'000;
    cfg.max_bitrate_bps = 5'000'000;
    cfg.smoothing_alpha = 1.0;
    inst->update_config(cfg);

    plugins::BweFeedback f = make_feedback(0.0, core::SteadyClock::now());
    f.remb_bps = 3'000'000;
    inst->on_feedback(f);

    auto e = inst->estimate();
    EXPECT_NEAR(static_cast<double>(e.target_bitrate_bps),
                2'400'000.0, 1.0);
    EXPECT_EQ(e.reason, plugins::BweEstimate::Reason::REMBOverride);
}

TEST(BwePlugin, ResetViaInterfaceRestoresInitial) {
    auto* ibwe = new nimrtc::bwe::Bwe();
    std::unique_ptr<plugins::IBwe> inst(ibwe);
    ASSERT_EQ(inst->open(), plugins::kOk);

    plugins::BweConfig cfg;
    cfg.initial_bitrate_bps = 800'000;
    inst->update_config(cfg);

    inst->on_feedback(make_feedback(1.0, core::SteadyClock::now()));
    inst->reset();

    auto e = inst->estimate();
    EXPECT_NEAR(static_cast<double>(e.target_bitrate_bps),
                800'000.0, 40'000.0);
    EXPECT_EQ(e.reason, plugins::BweEstimate::Reason::Initial);
}
