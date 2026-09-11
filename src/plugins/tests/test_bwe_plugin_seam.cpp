/**
 * @file src/plugins/tests/test_bwe_plugin_seam.cpp
 * @brief Plugin seam tests for the BWE plugin (plugins::IBwe).
 *
 * These tests verify the *seam* between the BWE module and the plugin
 * system:
 *
 *   1. The registry resolves a registered BWE factory by id (`"aimd"`).
 *   2. AimdPluginFactory exposes correct id / display_name.
 *   3. The factory's `create()` returns a `plugins::IBwe*` (concrete
 *      `nimrtc::bwe::Bwe`).
 *   4. The created instance behaves correctly through the plugin
 *      interface (open() / close() / name()).
 *   5. The `plugins::SimpleBweFactory<T>` template wrapper works
 *      correctly for out-of-tree plugins.
 *
 * These tests are distinct from `modules/bwe/tests/test_bwe.cpp`, which
 * drives the AIMD *algorithm* (loss → decrease, REMB override, reset).
 * Algorithm behaviour belongs to the module; plugin integration
 * (registry, factory wiring, interface conformance) belongs here.
 *
 * NOTE: the test still calls `nimrtc::bwe::register_default_plugins()`
 * to register the AIMD factory, so it links `nimrtc::bwe` in addition
 * to `nimrtc::plugins` and `nimrtc::core`. The seam being tested is the
 * registry + factory contract, not just the interface signatures — the
 * cheapest way to verify both at once is to round-trip through the
 * registry.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string_view>

#include <nimrtc/plugins/bwe.hpp>        // plugins::IBwe / IBweFactory / SimpleBweFactory
#include <nimrtc/bwe/bwe.hpp>            // nimrtc::bwe::Bwe, register_default_plugins()
#include <nimrtc/core/registry.hpp>      // core::PluginRegistry

namespace {

using namespace nimrtc;

// Build a feedback sample with given loss rate.
[[maybe_unused]]
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

TEST(BwePluginSeam, RegisterDefaultPluginsRegistersAimd) {
    // Call once at program init (idempotent).
    nimrtc::bwe::register_default_plugins();

    auto& reg = core::PluginRegistry::instance();
    const auto* f = reg.get_bwe("aimd");
    ASSERT_NE(f, nullptr) << "BWE factory \"aimd\" not registered";
    EXPECT_EQ(f->id(), "aimd");
}

TEST(BwePluginSeam, UnknownIdReturnsNullptr) {
    auto& reg = core::PluginRegistry::instance();
    EXPECT_EQ(reg.get_bwe("nonexistent_bwe"), nullptr);
}

TEST(BwePluginSeam, ListBwesContainsAimd) {
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

TEST(BwePluginSeam, FactoryDisplayNameNonEmpty) {
    nimrtc::bwe::register_default_plugins();
    auto& reg = core::PluginRegistry::instance();
    const auto* f = reg.get_bwe("aimd");
    ASSERT_NE(f, nullptr);
    EXPECT_FALSE(f->display_name().empty());
}

TEST(BwePluginSeam, FactoryCreatesConcreteBwe) {
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
// SimpleBweFactory<T> template (used by out-of-tree plugins)
// -----------------------------------------------------------------------------

TEST(SimpleBweFactorySeam, ReportsCorrectId) {
    plugins::SimpleBweFactory<nimrtc::bwe::Bwe> factory{
        "my_bwe", "My BWE"};
    EXPECT_EQ(factory.id(), "my_bwe");
    EXPECT_EQ(factory.display_name(), "My BWE");
}

TEST(SimpleBweFactorySeam, CreateReturnsNewInstance) {
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
