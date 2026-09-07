/**
 * @file src/modules/bwe/tests/test_bwe_plugin.cpp
 * @brief Unit tests for the BWE plugin seam (plugins::IBwe).
 *
 * Verifies:
 *   1. PluginRegistry resolves the BWE factory registered by
 *      `bwe::register_default_plugins()` under the id `"aimd"`.
 *   2. AimdPluginFactory reports correct id / display_name.
 *   3. The factory's `create()` returns a `plugins::IBwe*` (concrete `Bwe`).
 *   4. The created instance dispatches correctly through the plugin
 *      interface (loss feedback → AIMD decrease; REMB → override).
 *   5. Plugins are visible to `core::PluginRegistry::instance()` only
 *      after `register_default_plugins()` has been called (idempotency).
 *
 * @note P1 — IBwe is interface-only in P1; engine integration lands in P3.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

#include <nimrtc/bwe/bwe.hpp>            // bwe::Bwe (concrete)
#include <nimrtc/plugins/bwe.hpp>        // plugins::IBwe / IBweFactory
#include <nimrtc/core/registry.hpp>      // core::PluginRegistry

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
// Registration
// -----------------------------------------------------------------------------

TEST(BwePlugin, RegisterDefaultPluginsRegistersAimd) {
    // Call once at program init (idempotent).
    nimrtc::bwe::register_default_plugins();

    auto& reg = core::PluginRegistry::instance();
    const auto* f = reg.get_bwe("aimd");
    ASSERT_NE(f, nullptr) << "BWE factory \"aimd\" not registered";
    EXPECT_EQ(f->id(), "aimd");
}

TEST(BwePlugin, UnknownIdReturnsNullptr) {
    auto& reg = core::PluginRegistry::instance();
    EXPECT_EQ(reg.get_bwe("nonexistent_bwe"), nullptr);
}

TEST(BwePlugin, ListBwesContainsAimd) {
    auto& reg = core::PluginRegistry::instance();
    auto ids = reg.list_bwes();
    bool found = false;
    for (auto id : ids) {
        if (id == "aimd") { found = true; break; }
    }
    EXPECT_TRUE(found);
}

// -----------------------------------------------------------------------------
// Factory
// -----------------------------------------------------------------------------

TEST(BwePlugin, FactoryDisplayNameNonEmpty) {
    nimrtc::bwe::register_default_plugins();
    auto& reg = core::PluginRegistry::instance();
    const auto* f = reg.get_bwe("aimd");
    ASSERT_NE(f, nullptr);
    EXPECT_FALSE(f->display_name().empty());
}

TEST(BwePlugin, FactoryCreatesConcreteBwe) {
    nimrtc::bwe::register_default_plugins();
    auto& reg = core::PluginRegistry::instance();
    const auto* f = reg.get_bwe("aimd");
    ASSERT_NE(f, nullptr);

    plugins::BweConfig cfg;
    cfg.initial_bitrate_bps = 500'000;
    std::unique_ptr<plugins::IBwe> inst(f->create(cfg));
    ASSERT_NE(inst, nullptr);

    // Verify it speaks for the IBwe interface (name() / open() / close()).
    EXPECT_NE(inst->name(), nullptr);
    EXPECT_EQ(inst->open(), plugins::kOk);
    inst->close();
}

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

// -----------------------------------------------------------------------------
// SimpleBweFactory<T> template (used by out-of-tree plugins)
// -----------------------------------------------------------------------------

TEST(SimpleBweFactory, ReportsCorrectId) {
    plugins::SimpleBweFactory<nimrtc::bwe::Bwe> factory{
        "my_bwe", "My BWE"};
    EXPECT_EQ(factory.id(), "my_bwe");
    EXPECT_EQ(factory.display_name(), "My BWE");
}

TEST(SimpleBweFactory, CreateReturnsNewInstance) {
    plugins::SimpleBweFactory<nimrtc::bwe::Bwe> factory{
        "my_bwe", "My BWE"};

    plugins::BweConfig cfg;
    cfg.initial_bitrate_bps = 100'000;

    std::unique_ptr<plugins::IBwe> a(factory.create(cfg));
    std::unique_ptr<plugins::IBwe> b(factory.create(cfg));
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a.get(), b.get());
}
