/**
 * @file src/plugins/tests/test_scheduler_plugin_seam.cpp
 * @brief Unit tests for the scheduler plugin seam (plugins::IScheduler).
 *
 * These tests live in src/plugins/tests/ because they verify the
 * HEADER-ONLY INTERFACE contract, not any concrete implementation.
 *
 * The tests are HERMETIC: they register their own inline factory via
 * `SimpleSchedulerFactory<LocalScheduler>` and exercise the registry /
 * factory seam directly. They MUST NOT depend on `nimrtc::sched`. The
 * only dependencies are `nimrtc::plugins` (for the interface) and
 * `nimrtc::core` (for `PluginRegistry`).
 *
 * Behavioural tests of the concrete Scheduler implementation
 * (`EnqueueDrainViaInterface`, `DrainBudgetViaInterface`,
 * `StatsViaInterface`) live under src/modules/sched/tests/ — they
 * exercise implementation details and therefore must stay co-located
 * with the implementation.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string_view>

#include <nimrtc/plugins/scheduler.hpp>    // plugins::IScheduler / Priority
#include <nimrtc/core/registry.hpp>        // core::PluginRegistry

namespace {

using namespace nimrtc;
using plugins::Priority;
using plugins::SchedulerConfig;

// ---------------------------------------------------------------------------
// LocalScheduler — a minimal conformant implementation used to exercise
// the seam without depending on any concrete module (in particular, we
// intentionally avoid `#include <nimrtc/sched/sched.hpp>`).
// ---------------------------------------------------------------------------
class LocalScheduler final : public plugins::IScheduler {
public:
    explicit LocalScheduler(SchedulerConfig) {}

    // IPlugin ----------------------------------------------------------
    const char*       name()  const noexcept override { return "seam::LocalScheduler"; }
    plugins::Status   open()        noexcept override { return plugins::kOk; }
    void              close()       noexcept override {}

    // IScheduler -------------------------------------------------------
    void              enqueue(Priority, core::ByteSpan, plugins::Addr) noexcept override {}
    int               drain(int)                                  noexcept override { return 0; }
    int               drain_with(int, plugins::DrainCallback)      noexcept override { return 0; }
    void              on_bwe_update(std::uint32_t)                 noexcept override {}
    void              reset()                                      noexcept override {}
    plugins::SchedulerStats  stats()  const noexcept override { return {}; }
    plugins::SchedulerConfig config() const noexcept override { return {}; }
};

// LocalSchedulerFactory — exposed factory for the inline scheduler.
class LocalSchedulerFactory final : public plugins::ISchedulerFactory {
public:
    std::string_view id()           const noexcept override { return "seam_test_scheduler"; }
    std::string_view display_name() const noexcept override {
        return "Seam-test scheduler (LocalScheduler)";
    }
    plugins::IScheduler* create(SchedulerConfig config) const override {
        return new LocalScheduler(config);
    }
};

// Register the local factory at static-init time. The seam test owns
// the factory — no `nimrtc::sched` dependency required.
const LocalSchedulerFactory& local_factory() {
    static const LocalSchedulerFactory f{};
    // Idempotent registration (overwrite is allowed by the registry).
    nimrtc::core::PluginRegistry::instance().register_scheduler(f.id(), &f);
    return f;
}

} // namespace

// -----------------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------------

TEST(SchedulerPluginSeam, RegisterLocalFactoryMakesItResolvable) {
    (void)local_factory();

    auto& reg = core::PluginRegistry::instance();
    const auto* f = reg.get_scheduler("seam_test_scheduler");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->id(), "seam_test_scheduler");
}

TEST(SchedulerPluginSeam, UnknownIdReturnsNullptr) {
    auto& reg = core::PluginRegistry::instance();
    EXPECT_EQ(reg.get_scheduler("nonexistent_scheduler"), nullptr);
}

TEST(SchedulerPluginSeam, ListSchedulersContainsLocalFactory) {
    (void)local_factory();
    auto& reg = core::PluginRegistry::instance();
    auto ids = reg.list_schedulers();
    bool found = false;
    for (auto id : ids) {
        if (id == "seam_test_scheduler") { found = true; break; }
    }
    EXPECT_TRUE(found);
}

// -----------------------------------------------------------------------------
// Factory
// -----------------------------------------------------------------------------

TEST(SchedulerPluginSeam, FactoryDisplayNameNonEmpty) {
    const auto& f = local_factory();
    EXPECT_FALSE(f.display_name().empty());
}

TEST(SchedulerPluginSeam, FactoryCreatesConcreteScheduler) {
    (void)local_factory();
    auto& reg = core::PluginRegistry::instance();
    const auto* f = reg.get_scheduler("seam_test_scheduler");
    ASSERT_NE(f, nullptr);

    SchedulerConfig cfg;
    std::unique_ptr<plugins::IScheduler> inst(f->create(cfg));
    ASSERT_NE(inst, nullptr);

    EXPECT_NE(inst->name(), nullptr);
    EXPECT_EQ(inst->open(), plugins::kOk);
    inst->close();
}

// -----------------------------------------------------------------------------
// SimpleSchedulerFactory<T>
// -----------------------------------------------------------------------------

TEST(SimpleSchedulerFactory, ReportsCorrectId) {
    plugins::SimpleSchedulerFactory<LocalScheduler> factory{
        "my_q", "My queue"};
    EXPECT_EQ(factory.id(), "my_q");
    EXPECT_EQ(factory.display_name(), "My queue");
}

TEST(SimpleSchedulerFactory, CreateReturnsNewInstance) {
    plugins::SimpleSchedulerFactory<LocalScheduler> factory{
        "my_q", "My queue"};

    SchedulerConfig cfg;
    std::unique_ptr<plugins::IScheduler> a(factory.create(cfg));
    std::unique_ptr<plugins::IScheduler> b(factory.create(cfg));
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a.get(), b.get());
}
