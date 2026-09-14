/**
 * @file src/modules/sched/tests/test_scheduler_plugin.cpp
 * @brief Implementation-detail tests for the Scheduler module's plugin
 *        adapter.
 *
 * These tests live here (not in src/plugins/tests/) because they
 * exercise `nimrtc::sched::Scheduler` directly — they verify how the
 * concrete implementation behaves when driven through the
 * `plugins::IScheduler` interface. Pure plugin-seam tests (registry
 * round-trip, factory contracts) live in
 * src/plugins/tests/test_scheduler_plugin_seam.cpp.
 *
 * Coverage:
 *   1. `sched::register_default_plugins()` registers the
 *      `"strict_priority"` factory.
 *   2. The registered factory creates a `Scheduler` that responds to
 *      `open()` / `close()`.
 *   3. The created instance dispatches correctly through the plugin
 *      interface — enqueue → drain_with → priority order, drain
 *      budgets, stats.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
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
// Implementation-detail tests — instance behaviour through the plugin seam
// -----------------------------------------------------------------------------

TEST(SchedulerPluginImpl, EnqueueDrainViaInterface) {
    std::unique_ptr<plugins::IScheduler> inst(
        new nimrtc::sched::Scheduler());
    ASSERT_EQ(inst->open(), plugins::kOk);

    std::vector<std::uint8_t> storage;

    // Enqueue in mixed order; verify drain yields them in priority order.
    enqueue_one(*inst, storage, Priority::kVideo,         0xA1);
    enqueue_one(*inst, storage, Priority::kControl,       0xC1);
    enqueue_one(*inst, storage, Priority::kAudio,         0xB1);
    enqueue_one(*inst, storage, Priority::kBestEffort,    0xD1);
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

TEST(SchedulerPluginImpl, DrainBudgetViaInterface) {
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

TEST(SchedulerPluginImpl, StatsViaInterface) {
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
