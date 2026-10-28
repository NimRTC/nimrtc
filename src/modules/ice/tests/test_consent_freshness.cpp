// modules/ice/tests/test_consent_freshness.cpp
// =============================================================================
// ICE consent freshness tests (RFC 7675 / RFC 8445 §10).
//
// Validates the NimRTC-layer consent freshness tracker layered on top of the
// underlying libjuice agent:
//
//   T1 — Normal traffic: peer keeps sending STUN Binding requests
//        (simulated by notify_binding_received()) → on_consent_lost()
//        must NOT fire within `consent_timeout_ms` after the last notify.
//
//   T2 — Peer stops: no notify_binding_received() for `consent_timeout_ms` →
//        on_consent_lost() fires (and consent_lost_pending() == true).
//
//   T3 — Recovery: after a loss event, calling notify_binding_received()
//        rearms the timer so a fresh timeout window applies before the
//        callback fires again.
//
//   T4 — Null callback: set_on_consent_lost({}) doesn't crash even when the
//        timer is allowed to expire.
//
//   T5 — Race: a notify_binding_received() issued while the consent-lost
//        callback is still running must clear the pending-loss flag (and
//        must not be overwritten by the tracker afterwards).
//
// The tests use `force_consent_armed_for_testing()` to skip the libjuice
// connectivity check. Without that, each case would have to wait ~5–10 s
// for the loopback ICE handshake before the consent timer becomes
// meaningful. Production code must never call that helper.
//
// Threading note: the consent freshness tracker runs on its own background
// thread inside IceTransport. Tests spin a tiny "peer" thread that pumps
// notify_binding_received() at a fixed cadence so we can verify the timer
// truly is reactive.
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nimrtc/ice/ice.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/transport.hpp>

namespace {

using namespace std::chrono_literals;
using nimrtc::ice::IceConfig;
using nimrtc::ice::IceTransport;
using nimrtc::ice::Role;

// Small timeouts so each test finishes in well under a second.  T3 is the
// exception: it needs a 1000 ms consent timeout so that its re-arm notify
// is not racing the tracker's own auto-rearm (see the comment in T3).
constexpr std::int64_t kConsentTimeoutMs  = 200;
constexpr std::int64_t kConsentIntervalMs = 100;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

struct Config {
    std::int64_t timeout_ms  = kConsentTimeoutMs;
    std::int64_t interval_ms = kConsentIntervalMs;
};

IceConfig MakeConfig(const Config& c = {}) {
    IceConfig cfg;
    cfg.role = Role::Controlling;
    cfg.bind_address = "127.0.0.1";
    cfg.consent_interval_ms = c.interval_ms;
    cfg.consent_timeout_ms  = c.timeout_ms;
    cfg.enable_consent_freshness = true;
    return cfg;
}

// Wait at most `budget` for `pred` to become true, polling every 20 ms.
// Returns true if predicate became true within the budget, false otherwise.
template <class Pred>
bool WaitFor(Pred pred, std::chrono::milliseconds budget,
             std::chrono::milliseconds step = 20ms) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(step);
    }
    return pred();
}

// -----------------------------------------------------------------------------
// T1 — normal traffic must not trigger callback
// -----------------------------------------------------------------------------

TEST(IceConsentFreshness, T1_NormalPeerTrafficNeverFires) {
    IceConfig cfg = MakeConfig();
    IceTransport t{cfg};
    ASSERT_EQ(t.open(), nimrtc::plugins::kOk);

    std::atomic<int> lost_count{0};
    t.set_on_consent_lost([&] { ++lost_count; });

    // Arm the tracker so the timer math is independent of libjuice state.
    t.force_consent_armed_for_testing();

    // Pretend the peer keeps sending STUN Binding requests: wake up every
    // 40 ms (well under the 200 ms timeout) and notify.
    std::atomic<bool> stop{false};
    std::thread peer([&] {
        while (!stop.load(std::memory_order_acquire)) {
            t.notify_binding_received();
            std::this_thread::sleep_for(40ms);
        }
    });

    // Run for 3× the timeout. The callback must not have fired.
    std::this_thread::sleep_for(3 * kConsentTimeoutMs * 1ms);
    stop.store(true, std::memory_order_release);
    peer.join();

    EXPECT_EQ(lost_count.load(), 0)
        << "consent_lost should not have fired while peer is active";
    EXPECT_FALSE(t.consent_lost_pending());

    t.close();
}

// -----------------------------------------------------------------------------
// T2 — peer goes silent → callback fires within timeout
// -----------------------------------------------------------------------------

TEST(IceConsentFreshness, T2_PeerStoppedFiresAfterTimeout) {
    IceConfig cfg = MakeConfig();
    IceTransport t{cfg};
    ASSERT_EQ(t.open(), nimrtc::plugins::kOk);

    std::atomic<int> lost_count{0};
    std::mutex mu;
    std::condition_variable cv;
    t.set_on_consent_lost([&] {
        {
            std::lock_guard<std::mutex> lk(mu);
            ++lost_count;
        }
        cv.notify_all();
    });

    t.force_consent_armed_for_testing();
    // Force one notification so the clock starts fresh from known t0; the
    // test then waits for the timer to expire.
    t.notify_binding_received();

    // The tracker checks every ~100 ms (kTick). Wait up to ~5× the timeout
    // so timing jitter on a slow CI runner doesn't flake the test.
    constexpr auto budget = 5 * kConsentTimeoutMs * 1ms + 500ms;
    const bool fired = WaitFor(
        [&] { return lost_count.load() > 0; }, budget);

    EXPECT_TRUE(fired) << "on_consent_lost never fired within " << budget.count()
                       << "ms with timeout=" << kConsentTimeoutMs << "ms";
    EXPECT_GE(lost_count.load(), 1);
    EXPECT_TRUE(t.consent_lost_pending());

    t.close();
}

// -----------------------------------------------------------------------------
// T3 — receiving a Binding request after loss rearms the timer
// -----------------------------------------------------------------------------

TEST(IceConsentFreshness, T3_BindingReceivedRearmsTimer) {
    // T3 needs a longer consent timeout than the other cases, and it has to
    // compare *timestamps* instead of counting fires across a wall-clock
    // sleep.
    //
    // Why: the tracker auto-rearms after it fires (it resets
    // last_binding_received_us_ to "now" when the callback runs), so a
    // second fire is expected one timeout after the first one unless the
    // test's re-arm notify lands first.  The previous version of this test
    // used the shared 200 ms timeout and asserted "the fire count is
    // unchanged after a 100 ms sleep" -- i.e. it left the runner only
    // 100 ms of slack.  On the macOS runner of ci run #275 the test thread
    // was descheduled for longer than that, the auto-rearm fired inside
    // the "fresh window" and the count came out as 2 instead of 1:
    //
    //   test_consent_freshness.cpp:201: Failure
    //   Expected equality of these values:
    //     lost_count.load()  Which is: 2
    //     after_first        Which is: 1
    //   callback must not refire inside a fresh timeout window
    //
    // The rewritten test states the same property in a scheduler-proof
    // way: the *first* fire after the re-arm must not happen earlier than
    // a full timeout after the re-arm.  That is exactly what the
    // implementation guarantees (it fires when
    // now - last_binding_received_us_ >= timeout), so the assertion holds
    // no matter when the test thread happens to be scheduled.  The 1000 ms
    // timeout additionally gives the test a full second to issue the
    // re-arm before the tracker's own auto-rearm can fire.
    constexpr std::int64_t kTimeoutMs  = 1000;
    constexpr std::int64_t kIntervalMs = 100;
    // The tracker stamps its re-arm instant inside
    // notify_binding_received(); rearm_at is read a hair later, so the
    // measured window is never over-estimated.  50 ms of tolerance
    // absorbs that gap (and the clock granularity) without weakening the
    // assertion.
    constexpr std::int64_t kToleranceMs = 50;

    IceConfig cfg = MakeConfig({kTimeoutMs, kIntervalMs});
    IceTransport t{cfg};
    ASSERT_EQ(t.open(), nimrtc::plugins::kOk);

    std::atomic<int> lost_count{0};
    std::mutex mu;
    std::vector<std::chrono::steady_clock::time_point> fire_times;
    t.set_on_consent_lost([&] {
        {
            std::lock_guard<std::mutex> lk(mu);
            fire_times.push_back(std::chrono::steady_clock::now());
        }
        ++lost_count;
    });

    t.force_consent_armed_for_testing();
    t.notify_binding_received();

    // 1. Wait for the first loss.  The tracker ticks every 100 ms, so the
    //    fire lands between kTimeoutMs and kTimeoutMs + 100 ms.
    ASSERT_TRUE(WaitFor([&] { return lost_count.load() >= 1; },
                        5 * kTimeoutMs * 1ms + 500ms));
    const int after_first = lost_count.load();

    // 2. Rearm by simulating a fresh STUN Binding request.
    t.notify_binding_received();
    const auto rearm_at = std::chrono::steady_clock::now();
    EXPECT_FALSE(t.consent_lost_pending())
        << "notify_binding_received must clear the pending-loss flag";

    // 3. The next fire must not happen before a full timeout has elapsed
    //    since the re-arm -- that is what "the re-arm restarted the
    //    timer" means.
    const bool second = WaitFor(
        [&] { return lost_count.load() > after_first; },
        5 * kTimeoutMs * 1ms + 500ms);
    ASSERT_TRUE(second)
        << "callback should fire again after a full rearmed timeout";

    // 4. Exactly one more fire, and it must be at least a full timeout
    //    after the re-arm.
    EXPECT_EQ(lost_count.load(), after_first + 1)
        << "exactly one fire is expected after the re-arm";

    std::chrono::steady_clock::time_point next_fire{};
    {
        std::lock_guard<std::mutex> lk(mu);
        ASSERT_GT(fire_times.size(), static_cast<std::size_t>(after_first));
        next_fire = fire_times[static_cast<std::size_t>(after_first)];
    }
    const auto since_rearm = next_fire - rearm_at;
    EXPECT_GE(since_rearm, std::chrono::milliseconds(kTimeoutMs - kToleranceMs))
        << "the first fire after the re-arm must not be sooner than "
        << (kTimeoutMs - kToleranceMs) << "ms after the re-arm (measured "
        << since_rearm.count() << "ms)";
    EXPECT_LT(since_rearm, std::chrono::milliseconds(5 * kTimeoutMs))
        << "the re-armed timer must still fire";

    t.close();
}

// -----------------------------------------------------------------------------
// T4 — null callback → no crash even when the timer expires
// -----------------------------------------------------------------------------

TEST(IceConsentFreshness, T4_NullCallbackDoesNotCrash) {
    IceConfig cfg = MakeConfig();
    IceTransport t{cfg};
    ASSERT_EQ(t.open(), nimrtc::plugins::kOk);

    // Set a null std::function — must not crash when the timer fires.
    t.set_on_consent_lost(nullptr);
    t.force_consent_armed_for_testing();
    t.notify_binding_received();

    // Wait long enough that the timer would fire if there were a callback.
    std::this_thread::sleep_for(3 * kConsentTimeoutMs * 1ms);

    // The flag is set whether or not a callback exists — it documents that
    // "consent lost" was observed, not that the callback ran.
    EXPECT_TRUE(t.consent_lost_pending());

    // Set a non-null callback AFTER expiry — must be safe; calling it now
    // would re-fire only after another full timeout (we don't wait that
    // long in the test, but verify the registration path is still alive).
    int later_count = 0;
    t.set_on_consent_lost([&] { ++later_count; });
    t.notify_binding_received();              // clear pending + rearm
    EXPECT_FALSE(t.consent_lost_pending());
    EXPECT_EQ(later_count, 0);

    t.close();
}

// -----------------------------------------------------------------------------
// T5 — a notify that races with the consent-lost callback must win
// -----------------------------------------------------------------------------

TEST(IceConsentFreshness, T5_NotifyDuringCallbackClearsPendingFlag) {
    // Regression test for the CI flake in
    // IceConsentFreshness.T3_BindingReceivedRearmsTimer.
    //
    // The tracker publishes the "consent lost" state and *then* runs the
    // user callback.  If the state were published after the callback, a
    // notify_binding_received() issued while the callback is still running
    // would be overwritten by the tracker's late store and the pending flag
    // would come back up — the callback would then fire again inside a
    // freshly armed timeout window.
    //
    // The callback blocks until the test has issued its notify, which pins
    // the ordering deterministically (no reliance on scheduler luck).
    IceConfig cfg = MakeConfig();
    IceTransport t{cfg};
    ASSERT_EQ(t.open(), nimrtc::plugins::kOk);

    std::atomic<int> lost_count{0};
    std::mutex mu;
    std::condition_variable cv;
    bool callback_entered = false;
    bool test_notified = false;

    t.set_on_consent_lost([&] {
        {
            std::lock_guard<std::mutex> lk(mu);
            ++lost_count;
            callback_entered = true;
        }
        cv.notify_all();
        // Block until the test has re-armed the timer.  This also proves
        // the tracker does not hold its internal lock while running the
        // user callback — otherwise this notify would deadlock.
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return test_notified; });
    });

    t.force_consent_armed_for_testing();
    t.notify_binding_received();

    bool entered = false;
    {
        std::unique_lock<std::mutex> lk(mu);
        entered = cv.wait_for(lk, 5 * kConsentTimeoutMs * 1ms + 500ms,
                               [&] { return callback_entered; });
    }

    // Re-arm while the callback is still running, then let the callback
    // finish.  The notify must win.
    t.notify_binding_received();
    {
        std::lock_guard<std::mutex> lk(mu);
        test_notified = true;
    }
    cv.notify_all();

    EXPECT_TRUE(entered) << "on_consent_lost never fired";

    // Give the tracker thread a moment to finish its critical section.
    std::this_thread::sleep_for(50ms);

    EXPECT_GE(lost_count.load(), 1);
    EXPECT_FALSE(t.consent_lost_pending())
        << "notify_binding_received() must clear the pending-loss flag even "
           "when it races with the consent-lost callback";

    t.close();
}

// -----------------------------------------------------------------------------
// Bonus — the ITransport base-level callback forwards correctly via dispatch
// -----------------------------------------------------------------------------

TEST(IceConsentFreshness, ITransportInterfaceForwardsConsentLost) {
    // Set the callback through the base interface to make sure the
    // virtual override dispatch lands on the IceTransport implementation.
    IceConfig cfg = MakeConfig();
    IceTransport t{cfg};
    ASSERT_EQ(t.open(), nimrtc::plugins::kOk);

    std::atomic<int> lost_count{0};
    nimrtc::plugins::ITransport* base = static_cast<nimrtc::plugins::ITransport*>(&t);
    base->set_on_consent_lost([&] { ++lost_count; });
    t.force_consent_armed_for_testing();
    t.notify_binding_received();

    const bool fired = WaitFor([&] { return lost_count.load() >= 1; },
                               5 * kConsentTimeoutMs * 1ms + 500ms);
    EXPECT_TRUE(fired);

    t.close();
}

}  // namespace
