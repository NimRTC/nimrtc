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

#include <nimrtc/ice/ice.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/transport.hpp>

namespace {

using namespace std::chrono_literals;
using nimrtc::ice::IceConfig;
using nimrtc::ice::IceTransport;
using nimrtc::ice::Role;

// Small timeouts so each test finishes in well under a second.
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
    IceConfig cfg = MakeConfig();
    IceTransport t{cfg};
    ASSERT_EQ(t.open(), nimrtc::plugins::kOk);

    std::atomic<int> lost_count{0};
    t.set_on_consent_lost([&] { ++lost_count; });

    t.force_consent_armed_for_testing();
    t.notify_binding_received();

    // 1. Wait for first loss (timeout ≈ 200ms).
    ASSERT_TRUE(WaitFor([&] { return lost_count.load() >= 1; },
                        3 * kConsentTimeoutMs * 1ms));
    EXPECT_GE(lost_count.load(), 1);
    const int after_first = lost_count.load();

    // 2. Rearm by simulating a fresh STUN Binding request.
    t.notify_binding_received();
    EXPECT_FALSE(t.consent_lost_pending())
        << "notify_binding_received must clear the pending-loss flag";

    // 3. Within the timeout window, no second fire should occur.
    std::this_thread::sleep_for(kConsentTimeoutMs * 1ms / 2);
    EXPECT_EQ(lost_count.load(), after_first)
        << "callback must not refire inside a fresh timeout window";

    // 4. Past the rearmed window, it should fire again.
    const bool second = WaitFor(
        [&] { return lost_count.load() > after_first; },
        4 * kConsentTimeoutMs * 1ms);
    EXPECT_TRUE(second)
        << "callback should fire again after a full rearmed timeout";

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
                               3 * kConsentTimeoutMs * 1ms);
    EXPECT_TRUE(fired);

    t.close();
}

}  // namespace
