/**
 * @file src/modules/sched/tests/test_scheduler_plugin.cpp
 * @brief Unit tests for the scheduler plugin seam (plugins::IScheduler).
 *
 * Verifies:
 *   1. PluginRegistry resolves the scheduler factory registered by
 *      `sched::register_default_plugins()` under id `"strict_priority"`.
 *   2. StrictPriorityPluginFactory reports correct id / display_name.
 *   3. The factory's `create()` returns a `plugins::IScheduler*`.
 *   4. The created instance dispatches correctly through the plugin
 *      interface (enqueue → drain_with → priority order).
 *
 * @note P1 — IScheduler is interface-only in P1; engine integration
 *       (send_audio → scheduler enqueue) lands in P2.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <string_view>
#include <vector>

#include <nimrtc/sched/sched.hpp>          // sched::Scheduler (concrete)
#include <nimrtc/plugins/scheduler.hpp>    // plugins::IScheduler / Priority
#include <nimrtc/core/registry.hpp>        // core::PluginRegistry

namespace {

using namespace nimrtc;
using sched::Priority;

// Helper: enqueue a single 4-byte packet (priority + counter in payload).
// The buffer is owned by `storage` so it stays alive until `storage` is
// destructed (the scheduler holds a non-owning ByteSpan view into it).
//
// We thread `storage` through each enqueue_one() call so the caller can
// keep the buffers alive across multiple enqueues.
void enqueue_one(plugins::IScheduler& s, std::vector<std::uint8_t>& storage,
                 Priority p, std::uint8_t marker, plugins::Addr dst = {}) {
    const std::uint8_t p_byte = static_cast<std::uint8_t>(p);
    storage.push_back(p_byte);
    storage.push_back(marker);
    storage.push_back(0);
    storage.push_back(0);
    const std::uint8_t* base = &storage[storage.size() - 4];
    s.enqueue(p, core::ByteSpan(base, 4), dst);
}

// Helper: drain and collect markers in order.
std::vector<std::uint8_t> drain_all(plugins::IScheduler& s, int budget) {
    std::vector<std::uint8_t> out;
    s.drain_with(budget, [&](Priority p, core::ByteSpan b) -> bool {
        if (b.size() >= 2) out.push_back(b.data()[1]);
        (void)p;
        return true;
    });
    return out;
}

} // namespace

// -----------------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------------

TEST(SchedulerPlugin, RegisterDefaultPluginsRegistersStrictPriority) {
    nimrtc::sched::register_default_plugins();

    auto& reg = core::PluginRegistry::instance();
    const auto* f = reg.get_scheduler("strict_priority");
    ASSERT_NE(f, nullptr) << "Scheduler factory \"strict_priority\" not registered";
    EXPECT_EQ(f->id(), "strict_priority");
}

TEST(SchedulerPlugin, UnknownIdReturnsNullptr) {
    auto& reg = core::PluginRegistry::instance();
    EXPECT_EQ(reg.get_scheduler("nonexistent_scheduler"), nullptr);
}

TEST(SchedulerPlugin, ListSchedulersContainsStrictPriority) {
    nimrtc::sched::register_default_plugins();
    auto& reg = core::PluginRegistry::instance();
    auto ids = reg.list_schedulers();
    bool found = false;
    for (auto id : ids) {
        if (id == "strict_priority") { found = true; break; }
    }
    EXPECT_TRUE(found);
}

// -----------------------------------------------------------------------------
// Factory
// -----------------------------------------------------------------------------

TEST(SchedulerPlugin, FactoryDisplayNameNonEmpty) {
    nimrtc::sched::register_default_plugins();
    auto& reg = core::PluginRegistry::instance();
    const auto* f = reg.get_scheduler("strict_priority");
    ASSERT_NE(f, nullptr);
    EXPECT_FALSE(f->display_name().empty());
}

TEST(SchedulerPlugin, FactoryCreatesConcreteScheduler) {
    nimrtc::sched::register_default_plugins();
    auto& reg = core::PluginRegistry::instance();
    const auto* f = reg.get_scheduler("strict_priority");
    ASSERT_NE(f, nullptr);

    plugins::SchedulerConfig cfg;
    std::unique_ptr<plugins::IScheduler> inst(f->create(cfg));
    ASSERT_NE(inst, nullptr);

    EXPECT_NE(inst->name(), nullptr);
    EXPECT_EQ(inst->open(), plugins::kOk);
    inst->close();
}

// -----------------------------------------------------------------------------
// Dispatch through the plugin interface
// -----------------------------------------------------------------------------

TEST(SchedulerPlugin, EnqueueDrainViaInterface) {
    std::unique_ptr<plugins::IScheduler> inst(
        new nimrtc::sched::Scheduler());
    ASSERT_EQ(inst->open(), plugins::kOk);

    std::vector<std::uint8_t> storage;

    // Enqueue in mixed order; verify drain yields them in priority order.
    enqueue_one(*inst, storage, Priority::kVideo,        0xA1);
    enqueue_one(*inst, storage, Priority::kControl,      0xC1);
    enqueue_one(*inst, storage, Priority::kAudio,        0xB1);
    enqueue_one(*inst, storage, Priority::kBestEffort,   0xD1);
    enqueue_one(*inst, storage, Priority::kVideoKeyframe, 0xA2);

    auto markers = drain_all(*inst, 100);
    ASSERT_EQ(markers.size(), 5u);

    // Expected order: Control, Audio, VideoKeyframe, Video, BestEffort.
    EXPECT_EQ(markers[0], 0xC1);
    EXPECT_EQ(markers[1], 0xB1);
    EXPECT_EQ(markers[2], 0xA2);
    EXPECT_EQ(markers[3], 0xA1);
    EXPECT_EQ(markers[4], 0xD1);
}

TEST(SchedulerPlugin, DrainBudgetViaInterface) {
    std::unique_ptr<plugins::IScheduler> inst(
        new nimrtc::sched::Scheduler());
    ASSERT_EQ(inst->open(), plugins::kOk);

    std::vector<std::uint8_t> storage;
    for (int i = 0; i < 10; ++i) {
        enqueue_one(*inst, storage, Priority::kAudio,
                    static_cast<std::uint8_t>(i));
    }

    auto first = drain_all(*inst, 3);
    EXPECT_EQ(first.size(), 3u);

    auto second = drain_all(*inst, 100);
    EXPECT_EQ(second.size(), 7u);
}

TEST(SchedulerPlugin, StatsViaInterface) {
    std::unique_ptr<plugins::IScheduler> inst(
        new nimrtc::sched::Scheduler());
    ASSERT_EQ(inst->open(), plugins::kOk);

    std::vector<std::uint8_t> storage;
    enqueue_one(*inst, storage, Priority::kAudio, 1);
    enqueue_one(*inst, storage, Priority::kAudio, 2);
    drain_all(*inst, 10);

    auto s = inst->stats();
    EXPECT_EQ(s.sent_packets, 2u);
    EXPECT_EQ(s.queued_packets, 0u);
}

// -----------------------------------------------------------------------------
// SimpleSchedulerFactory<T>
// -----------------------------------------------------------------------------

TEST(SimpleSchedulerFactory, ReportsCorrectId) {
    plugins::SimpleSchedulerFactory<nimrtc::sched::Scheduler> factory{
        "my_q", "My queue"};
    EXPECT_EQ(factory.id(), "my_q");
    EXPECT_EQ(factory.display_name(), "My queue");
}

TEST(SimpleSchedulerFactory, CreateReturnsNewInstance) {
    plugins::SimpleSchedulerFactory<nimrtc::sched::Scheduler> factory{
        "my_q", "My queue"};

    plugins::SchedulerConfig cfg;
    std::unique_ptr<plugins::IScheduler> a(factory.create(cfg));
    std::unique_ptr<plugins::IScheduler> b(factory.create(cfg));
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a.get(), b.get());
}
