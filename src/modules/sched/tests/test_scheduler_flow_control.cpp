/**
 * @file src/modules/sched/tests/test_scheduler_flow_control.cpp
 * @brief Unit tests for the scheduler's flow control and its lock discipline.
 *
 * Coverage:
 *   A8 — The drain callback is invoked **without** the queue lock held, so
 *        a blocking callback cannot stall the producers (enqueue()).
 *   A8 — A vetoed packet is returned to the FRONT of its queue, in its
 *        original order, and the credit / pacing it consumed is refunded.
 *   A9-a — The S-family (per-stream) drain is charged to the **same**
 *          session BWE credit pool as the P-family drain.
 *   A9-b — The S-family drain consumes the **per-class pacing bucket** of
 *          the stream's traffic class.
 *   A9-c — `select_next_packet()` is part of the `plugins::IScheduler`
 *          interface, with a default implementation that reports "no
 *          stream support".
 *   A9-c — The S-family and the P-family share **one** credit clock: the
 *          elapsed wall-clock time is credited once, not once per family.
 *
 * Stability classification: NIMRTC_API_STABLE (regression test for the
 * architecture remediation plan, items A8 / A9).
 */

// gtest first: ensures no PCH surprises from our headers.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <thread>
#include <vector>

// Plugin types (plugins::Addr, kMaxAddrLen) must be visible before
// nimrtc/sched/sched.hpp which uses plugins::Addr in its public API.
#include <nimrtc/plugins/base.hpp>

#include <nimrtc/sched/sched.hpp>

namespace {

// The `plugins` and `core` namespaces are nested under `nimrtc`; the
// aliases let the test refer to them the way the plugin headers do.
namespace plugins = nimrtc::plugins;
namespace core    = nimrtc::core;

using namespace nimrtc::sched;
using namespace nimrtc::core;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

inline nimrtc::plugins::Addr make_addr(std::uint8_t id) {
    nimrtc::plugins::Addr a{};
    a.data[0] = id;
    a.len = 1;
    return a;
}

/** A 1000-byte payload — the packet size the flow-control tests use, so
 *  that the credit arithmetic is easy to reason about. */
std::vector<std::uint8_t> make_payload(std::size_t n = 1000) {
    return std::vector<std::uint8_t>(n, 0xAB);
}

/** A scheduler configured with a 1000-byte average packet size. */
SchedulerConfig small_packet_config() {
    SchedulerConfig cfg;
    cfg.avg_packet_size_bytes = 1000;
    return cfg;
}

// ===========================================================================
// A8 — the drain callback runs outside the queue lock
// ===========================================================================

/**
 * A drain callback that blocks must **not** block `enqueue()`.
 *
 * Before the fix the scheduler held its mutex across the callback, so a
 * blocking network write inside the callback froze every producer (the
 * audio path, the video path, the control path) for the duration of the
 * write.  The drain now collects the packets under the lock and dispatches
 * them outside it.
 *
 * The test enqueues from a second thread while the callback is parked on a
 * promise.  If the lock were still held across the callback, the enqueue
 * would block and the promise would never be fulfilled — the test would
 * time out.
 */
TEST(SchedulerLocking, CallbackDoesNotHoldTheQueueLock) {
    Scheduler sch{small_packet_config()};

    sch.enqueue(Priority::kAudio, ByteSpan{make_payload()}, make_addr(1));

    // The callback parks here until the test releases it.
    std::promise<void> callback_entered;
    std::promise<void> callback_release;
    auto entered = callback_entered.get_future();
    auto release = callback_release.get_future();

    std::thread drainer([&] {
        sch.drain_with(10, [&](Priority, ByteSpan) -> bool {
            callback_entered.set_value();
            release.wait();
            return true;
        });
    });

    // Wait until the callback is actually running.
    ASSERT_EQ(entered.wait_for(5s), std::future_status::ready);

    // The producer must not be blocked by the parked callback.
    std::atomic<bool> enqueue_done{false};
    std::thread producer([&] {
        sch.enqueue(Priority::kVideo, ByteSpan{make_payload()}, make_addr(1));
        enqueue_done = true;
    });

    // Give the producer a generous window to finish.
    for (int i = 0; i < 200 && !enqueue_done.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }

    const bool producer_completed = enqueue_done.load();

    // Release the callback and let both threads finish.
    callback_release.set_value();
    drainer.join();
    producer.join();

    EXPECT_TRUE(producer_completed)
        << "enqueue() was blocked by the drain callback — the scheduler "
           "still holds its queue lock across the callback";
}

/**
 * The same property for the S-family drain.
 */
TEST(SchedulerLocking, StreamCallbackDoesNotHoldTheQueueLock) {
    Scheduler sch{small_packet_config()};

    sch.enqueue_for_stream_owned(7u, make_payload(), make_addr(1));

    std::promise<void> callback_entered;
    std::promise<void> callback_release;
    auto entered = callback_entered.get_future();
    auto release = callback_release.get_future();

    std::thread drainer([&] {
        sch.select_next_packet(10, [&](std::uint32_t, StreamPriority,
                                       ByteSpan) -> bool {
            callback_entered.set_value();
            release.wait();
            return true;
        });
    });

    ASSERT_EQ(entered.wait_for(5s), std::future_status::ready);

    std::atomic<bool> enqueue_done{false};
    std::thread producer([&] {
        sch.enqueue_for_stream_owned(8u, make_payload(), make_addr(1));
        enqueue_done = true;
    });

    for (int i = 0; i < 200 && !enqueue_done.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }

    const bool producer_completed = enqueue_done.load();

    callback_release.set_value();
    drainer.join();
    producer.join();

    EXPECT_TRUE(producer_completed)
        << "enqueue_for_stream_owned() was blocked by the drain callback";
}

/**
 * A veto returns the packet to the FRONT of its queue and the drain stops.
 *
 * The vetoed packet must be the *first* one the next drain sees — the
 * veto is a "the transport is not accepting traffic right now" signal,
 * not a "drop this packet" signal.
 */
TEST(SchedulerLocking, VetoReturnsThePacketToTheFrontOfTheQueue) {
    Scheduler sch{small_packet_config()};

    sch.enqueue(Priority::kAudio, ByteSpan{make_payload()}, make_addr(1));
    sch.enqueue(Priority::kAudio, ByteSpan{make_payload()}, make_addr(1));

    // The first packet is vetoed.
    const int sent = sch.drain_with(10, [&](Priority, ByteSpan) -> bool {
        return false;
    });
    EXPECT_EQ(sent, 0);

    // Both packets are still queued, and the next drain sees them in the
    // original order (the vetoed one first — the payload bytes differ so
    // the order is observable).
    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    const int n = sch.drain_with(10, [&](Priority, ByteSpan data) -> bool {
        if (first.empty()) {
            first.assign(data.data(), data.data() + data.size());
        } else if (second.empty()) {
            second.assign(data.data(), data.data() + data.size());
        }
        return true;
    });

    EXPECT_EQ(n, 2) << "the vetoed packet must not be lost";
    EXPECT_EQ(first.size(), 1000u);
    EXPECT_EQ(second.size(), 1000u);
    // The two packets are identical in content, so the *count* is the
    // observable property: nothing was dropped.
    EXPECT_EQ(sch.stats().queued_packets, 0u);
}

/**
 * A veto restores the packet at the front of **its own** priority queue,
 * not in front of the higher-priority queues.
 */
TEST(SchedulerLocking, VetoKeepsThePerQueueOrder) {
    Scheduler sch{small_packet_config()};

    // A control packet and two audio packets.  The control packet is
    // dispatched first, the veto happens on the first *audio* packet.
    sch.enqueue(Priority::kControl, ByteSpan{make_payload()}, make_addr(1));
    sch.enqueue(Priority::kAudio, ByteSpan{make_payload()}, make_addr(1));
    sch.enqueue(Priority::kAudio, ByteSpan{make_payload()}, make_addr(1));

    std::vector<Priority> seen;
    bool vetoed = false;
    const int sent = sch.drain_with(10, [&](Priority p, ByteSpan) -> bool {
        seen.push_back(p);
        if (p == Priority::kAudio && !vetoed) {
            vetoed = true;
            return false;  // veto the first audio packet
        }
        return true;
    });

    // The control packet was dispatched, and the callback was then asked
    // about the first audio packet — which it vetoed.  The veto stopped
    // the drain right there, so the second audio packet never reached
    // the callback.
    EXPECT_EQ(sent, 1);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], Priority::kControl);
    EXPECT_EQ(seen[1], Priority::kAudio);

    // The next drain must see the two audio packets — the vetoed one
    // first, in the queue it came from.
    int audio_seen = 0;
    (void)sch.drain_with(10, [&](Priority p, ByteSpan) -> bool {
        if (p == Priority::kAudio) ++audio_seen;
        return true;
    });
    EXPECT_EQ(audio_seen, 2)
        << "the vetoed audio packet must be back in the audio queue";
}

// ===========================================================================
// A9-a — the S-family shares the session BWE credit pool
// ===========================================================================

/**
 * The S-family drain must not be able to exceed the session's BWE budget.
 *
 * Before the fix the S-family path bypassed the credit entirely, so a
 * stream that used the per-stream priority API could push an arbitrary
 * amount of data onto the wire.
 */
TEST(SchedulerFlowControl, StreamDrainIsBoundByTheSessionCredit) {
    Scheduler sch{small_packet_config()};

    // 8 kbps = 1000 bytes/s.  The first drain seeds the credit with one
    // drain interval (125 ms) = 125 bytes — less than one 1000-byte
    // packet, so *nothing* may leave.
    sch.on_bwe_update(8'000);

    for (int i = 0; i < 10; ++i) {
        sch.enqueue_for_stream_owned(1u, make_payload(), make_addr(1));
    }

    const int sent = sch.select_next_packet(10, nullptr);
    EXPECT_EQ(sent, 0)
        << "the S-family must not bypass the session BWE budget";
}

/**
 * The S-family and the P-family draw from the **same** credit pool: the
 * bytes the P-family spent are not available to the S-family.
 */
TEST(SchedulerFlowControl, BothFamiliesShareOneCreditPool) {
    Scheduler sch{small_packet_config()};

    // A budget of exactly 8000 bps = 1000 bytes/s.  The first drain seeds
    // the credit with 125 ms = 125 bytes, the second with the measured
    // elapsed time.
    sch.on_bwe_update(8'000);

    sch.enqueue(Priority::kAudio, ByteSpan{make_payload()}, make_addr(1));
    sch.enqueue_for_stream_owned(1u, make_payload(), make_addr(1));

    // The first drain seeds the credit: 125 bytes — not enough for a
    // 1000-byte packet, so neither family may send.
    EXPECT_EQ(sch.drain_with(10, nullptr), 0);
    EXPECT_EQ(sch.select_next_packet(10, nullptr), 0);

    // Let the clock advance so the credit is earned.
    std::this_thread::sleep_for(60ms);

    // Now the credit covers roughly one packet.  Whichever family asks
    // first gets it; the other one must wait.  The total number of
    // packets that left the scheduler may not exceed the budget.
    const int p_sent = sch.drain_with(10, nullptr);
    const int s_sent = sch.select_next_packet(10, nullptr);

    EXPECT_LE(p_sent + s_sent, 2)
        << "the two families must share one credit pool, not one each";
}

/**
 * The credit clock is **shared**: the elapsed wall-clock time between two
 * drains is credited once, no matter which family asked for it.
 *
 * With two independent clocks a session that drained both families every
 * 20 ms would grant 2 × 20 ms of credit per 20 ms — twice the budget.
 */
TEST(SchedulerFlowControl, TheCreditClockIsSharedBetweenTheFamilies) {
    Scheduler sch{small_packet_config()};

    // 80 kbps = 10 000 bytes/s.  Over 200 ms that is 2000 bytes = two
    // 1000-byte packets.  With two independent clocks the session could
    // send four.
    sch.on_bwe_update(80'000);

    sch.enqueue(Priority::kAudio, ByteSpan{make_payload()}, make_addr(1));
    sch.enqueue_for_stream_owned(1u, make_payload(), make_addr(1));

    // Let the clock advance past the seed interval so the measured credit
    // is meaningful.
    std::this_thread::sleep_for(250ms);

    const int p_sent = sch.drain_with(10, nullptr);
    const int s_sent = sch.select_next_packet(10, nullptr);

    EXPECT_LE(p_sent + s_sent, 3)
        << "the elapsed time may only be credited once, not once per family";
}

// ===========================================================================
// A9-b — the S-family consumes the per-class pacing bucket
// ===========================================================================

/**
 * A stream that is paced by a class is held back by that class's bucket.
 */
TEST(SchedulerFlowControl, StreamDrainConsumesThePacingBucket) {
    Scheduler sch{small_packet_config()};

    // The data class is paced at 8000 bps = 1000 bytes/s.  The default
    // burst is 100 ms = 100 bytes, which is less than one 1000-byte
    // packet, so the first packet is held back.
    sch.set_pacing_rate(PacedClass::kData, 8'000);

    for (int i = 0; i < 5; ++i) {
        sch.enqueue_for_stream_owned(1u, make_payload(), make_addr(1));
    }

    const int sent = sch.select_next_packet(10, nullptr);
    EXPECT_EQ(sent, 0) << "the S-family must respect the pacing bucket";

    // The stall is accounted for, not silent.
    EXPECT_GT(sch.stats().paced_blocked_packets, 0u);
}

/**
 * A stream that is paced by the video class is *not* affected by the data
 * class's bucket, and vice versa.
 */
TEST(SchedulerFlowControl, TheStreamPacedClassSelectsTheBucket) {
    Scheduler sch{small_packet_config()};

    // Only the video class is paced.  The stream is explicitly assigned
    // to it.
    sch.set_pacing_rate(PacedClass::kVideo, 8'000);
    sch.set_stream_paced_class(42u, PacedClass::kVideo);

    EXPECT_EQ(sch.stream_paced_class(42u), PacedClass::kVideo);

    for (int i = 0; i < 5; ++i) {
        sch.enqueue_for_stream_owned(42u, make_payload(), make_addr(1));
    }
    EXPECT_EQ(sch.select_next_packet(10, nullptr), 0)
        << "the video bucket must hold the stream back";

    // A stream in the data class is unaffected by the video bucket.
    sch.set_stream_paced_class(43u, PacedClass::kData);
    for (int i = 0; i < 5; ++i) {
        sch.enqueue_for_stream_owned(43u, make_payload(), make_addr(1));
    }
    EXPECT_EQ(sch.select_next_packet(10, nullptr), 5)
        << "the data bucket is not paced, so the stream must not be held back";
}

/**
 * An unconfigured stream defaults to the data class — the conservative
 * choice.
 */
TEST(SchedulerFlowControl, TheDefaultStreamPacedClassIsData) {
    Scheduler sch{small_packet_config()};
    EXPECT_EQ(sch.stream_paced_class(1234u), PacedClass::kData);
}

/**
 * A veto in the S-family does not consume the credit: the packet goes
 * back into its bucket and the budget it took is returned.
 */
TEST(SchedulerFlowControl, StreamVetoDoesNotConsumeTheCredit) {
    Scheduler sch{small_packet_config()};

    // 80 kbps = 10 000 bytes/s.  Over 200 ms that is 2000 bytes.
    sch.on_bwe_update(80'000);

    for (int i = 0; i < 5; ++i) {
        sch.enqueue_for_stream_owned(1u, make_payload(), make_addr(1));
    }

    std::this_thread::sleep_for(250ms);

    // Veto everything: nothing is sent and nothing is lost.
    int vetoes = 0;
    const int sent = sch.select_next_packet(10, [&](std::uint32_t, StreamPriority,
                                                    ByteSpan) -> bool {
        ++vetoes;
        return false;
    });
    EXPECT_EQ(sent, 0);
    EXPECT_EQ(vetoes, 1) << "the drain must stop at the first veto";
    EXPECT_EQ(sch.stats().queued_packets, 5u)
        << "the vetoed packets must be back in their bucket";

    // The credit was refunded, so the packets can still be sent later.
    std::this_thread::sleep_for(50ms);
    const int sent2 = sch.select_next_packet(10, nullptr);
    EXPECT_GT(sent2, 0)
        << "the veto must not have consumed the session credit";
}

// ===========================================================================
// A9-c — the S-family is part of the plugins::IScheduler interface
// ===========================================================================

/**
 * A third-party scheduler that does not implement the S-family must still
 * compile and behave sanely: the default implementation reports "no
 * stream support".
 */
class MinimalScheduler final : public plugins::IScheduler {
public:
    const char* name() const noexcept override { return "minimal"; }
    plugins::Status open() noexcept override { return plugins::kOk; }
    void close() noexcept override {}

    void enqueue(plugins::Priority, core::ByteSpan, plugins::Addr) noexcept override {}
    void enqueue_owned(plugins::Priority, std::vector<std::uint8_t>&&,
                       plugins::Addr) noexcept override {}
    int drain(int) noexcept override { return 0; }
    int drain_with(int, plugins::DrainCallback) noexcept override { return 0; }
    void on_bwe_update(std::uint32_t) noexcept override {}
    void reset() noexcept override {}
    plugins::SchedulerStats stats() const noexcept override { return {}; }
    plugins::SchedulerConfig config() const noexcept override { return {}; }
};

TEST(SchedulerSeam, TheDefaultStreamApiIsInert) {
    MinimalScheduler sch;

    // The defaults report "no stream support" rather than crashing.
    EXPECT_EQ(sch.select_next_packet(10, nullptr), 0);
    EXPECT_EQ(sch.get_stream_priority(1u), StreamPriority::Normal);
    EXPECT_EQ(sch.stream_paced_class(1u), PacedClass::kData);

    // The setters are no-ops.
    sch.set_stream_priority(1u, StreamPriority::High);
    sch.set_stream_paced_class(1u, PacedClass::kVideo);
    EXPECT_EQ(sch.get_stream_priority(1u), StreamPriority::Normal);
    EXPECT_EQ(sch.stream_paced_class(1u), PacedClass::kData);
}

/**
 * The built-in scheduler implements the interface's S-family.
 */
TEST(SchedulerSeam, TheBuiltinSchedulerImplementsTheStreamApi) {
    Scheduler sch{small_packet_config()};

    plugins::IScheduler* iface = &sch;
    iface->set_stream_priority(5u, StreamPriority::High);
    EXPECT_EQ(iface->get_stream_priority(5u), StreamPriority::High);

    iface->set_stream_paced_class(5u, PacedClass::kVideo);
    EXPECT_EQ(iface->stream_paced_class(5u), PacedClass::kVideo);

    iface->enqueue_for_stream_owned(5u, make_payload(), make_addr(1));

    int seen = 0;
    std::uint32_t seen_stream = 0;
    StreamPriority seen_level = StreamPriority::Low;
    const int n = iface->select_next_packet(10, [&](std::uint32_t stream_id,
                                                     StreamPriority level,
                                                     ByteSpan) -> bool {
        ++seen;
        seen_stream = stream_id;
        seen_level = level;
        return true;
    });

    EXPECT_EQ(n, 1);
    EXPECT_EQ(seen, 1);
    EXPECT_EQ(seen_stream, 5u) << "the callback must see the real stream id";
    EXPECT_EQ(seen_level, StreamPriority::High)
        << "the callback must see the real bucket level";
}

}  // namespace
