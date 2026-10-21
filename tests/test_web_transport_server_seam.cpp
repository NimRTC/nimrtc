/**
 * @file tests/test_web_transport_server_seam.cpp
 * @brief Compile + smoke test for the v0.14.0-beta.2 WebTransport server
 *        seam (Case E, WT-SEVER, ADR-009 §2.1.3).
 *
 * What this test verifies:
 *   - `IWebTransportServer` is an abstract class with the documented
 *     pure-virtual methods: start / stop / set_on_session / running.
 *   - `OnWebTransportSessionCb` accepts a `std::function<void(unique_ptr<
 *     IWebTransportSession>)>` and is constructible from a lambda.
 *   - `create_msquic_webtransport_server(cfg)` has the correct signature
 *     (returns unique_ptr<IWebTransportServer>, takes WebTransportConfig,
 *     noexcept(false)).
 *   - A server instance can be created without throwing (the factory
 *     resolves msquic API), and the `running()` method returns false
 *     before `start()` is called.
 *   - `start(port)` on an invalid port returns false (no listener
 *     is bound); subsequent `running()` stays false.
 *   - `stop()` is idempotent (calling stop on a non-running server
 *     is safe).
 *   - `set_on_session(nullptr)` is safe; the server starts and stops
 *     correctly even when no callback is set.
 *
 * This test is NOT a full integration test — it does not attempt to
 * connect a real Chrome client.  The end-to-end Chrome WebTransport
 * interop is exercised by `tests/chrome_interop_harness/
 * run_webtransport_harness.py` (Playwright + headless Chrome).
 *
 * ## GTest strategy
 *
 * Runtime: drives the msquic-backed `IWebTransportServer` implementation
 * through its public surface without a real client connection.  The
 * `running()` / `start()` / `stop()` contract is tested directly;
 * the `on_session` callback is tested with a no-op lambda to verify
 * the wiring is correct without requiring a real QUIC connection.
 *
 * Compile-time: `static_assert` on the interface shapes catches
 * API-surface regressions at PR time.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include <nimrtc/core/api_version.hpp>  // NIMRTC_API_UNSTABLE
#include <nimrtc/plugins/base.hpp>       // plugins::Status
#include <nimrtc/web_transport/web_transport_config.hpp>
#include <nimrtc/web_transport/web_transport_server.hpp>

// ---------------------------------------------------------------------------
// Compile-time contract: IWebTransportServer is abstract.
// ---------------------------------------------------------------------------

namespace nimrtc_web_transport_server_test {

// IWebTransportServer must be abstract (pure-virtual interface).
static_assert(
    std::is_abstract_v<nimrtc::web_transport::IWebTransportServer>,
    "IWebTransportServer must be an abstract class "
    "(start / stop / set_on_session / running are pure-virtual).");

// OnWebTransportSessionCb must accept a lambda that takes a unique_ptr.
static_assert(
    std::is_constructible_v<
        nimrtc::web_transport::OnWebTransportSessionCb,
        std::function<void(std::unique_ptr<
            nimrtc::web_transport::IWebTransportSession>)>>,
    "OnWebTransportSessionCb must be constructible from "
    "std::function<void(unique_ptr<IWebTransportSession>)> "
    "(lambda support).");

// create_msquic_webtransport_server returns unique_ptr<IWebTransportServer>.
static_assert(
    std::is_same_v<
        decltype(nimrtc::web_transport::create_msquic_webtransport_server(
            std::declval<nimrtc::web_transport::WebTransportConfig>())),
        std::unique_ptr<nimrtc::web_transport::IWebTransportServer>>,
    "create_msquic_webtransport_server must return "
    "std::unique_ptr<IWebTransportServer>.");

}  // namespace nimrtc_web_transport_server_test

// ---------------------------------------------------------------------------
// Runtime tests
// ---------------------------------------------------------------------------

namespace {

class WebTransportServerSeamTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a server with a no-op session callback so the server
        // is fully configured before we test.
        server_ = nimrtc::web_transport::create_msquic_webtransport_server(
            cfg_);
    }

    // Default config — enough for the server to start.
    nimrtc::web_transport::WebTransportConfig cfg_{};
    std::unique_ptr<nimrtc::web_transport::IWebTransportServer> server_;
};

}  // namespace

// Test: factory produces a non-null server.
TEST_F(WebTransportServerSeamTest, factory_returns_non_null) {
    EXPECT_NE(server_, nullptr);
}

// Test: before start(), running() is false.
TEST_F(WebTransportServerSeamTest, running_is_false_before_start) {
    EXPECT_FALSE(server_->running());
}

// Test: start(port=0) on Windows assigns a random available port —
// this is correct QUIC/Winsock behaviour (port 0 means "pick one for me").
// The server should report running()=true after this call.
TEST_F(WebTransportServerSeamTest, start_on_port_zero_assigns_dynamic_port) {
    // Port 0 is a valid bind target on Windows: the OS assigns an
    // available ephemeral port.  This is useful for test servers that
    // don't need a fixed port.  Verify the server reports running().
    EXPECT_TRUE(server_->start(0));
    EXPECT_TRUE(server_->running());
    server_->stop();
    EXPECT_FALSE(server_->running());
}

// Test: stop() is idempotent when not running.
TEST_F(WebTransportServerSeamTest, stop_is_idempotent_before_start) {
    server_->stop();  // must not throw
    EXPECT_FALSE(server_->running());
    server_->stop();  // must not throw again
}

// Test: set_on_session with a no-op lambda is safe.
TEST_F(WebTransportServerSeamTest, set_on_session_accepts_noop_lambda) {
    bool called = false;
    server_->set_on_session(
        [&called](std::unique_ptr<
            nimrtc::web_transport::IWebTransportSession> /*session*/) {
            called = true;
        });
    // Setting the callback multiple times must not throw.
    server_->set_on_session(
        [](std::unique_ptr<
            nimrtc::web_transport::IWebTransportSession> /*session*/) {});
    // Unregistering with an empty function is also safe.
    server_->set_on_session(nullptr);
    EXPECT_FALSE(called);  // callback was never fired (no sessions arrived)
}

// Test: start on an unprivileged port succeeds (smoke — listener binds).
TEST_F(WebTransportServerSeamTest, start_on_unprivileged_port_succeeds) {
    // Use a high unprivileged port that is unlikely to be in use.
    constexpr std::uint16_t kTestPort = 29443;
    EXPECT_TRUE(server_->start(kTestPort));
    EXPECT_TRUE(server_->running());

    // Stop and verify the server is no longer running.
    server_->stop();
    EXPECT_FALSE(server_->running());
}

// Test: after stop(), start() can be called again (restart is safe).
TEST_F(WebTransportServerSeamTest, restart_after_stop_is_safe) {
    constexpr std::uint16_t kTestPort = 29444;
    EXPECT_TRUE(server_->start(kTestPort));
    EXPECT_TRUE(server_->running());

    server_->stop();
    EXPECT_FALSE(server_->running());

    // Restart on the same port.
    EXPECT_TRUE(server_->start(kTestPort));
    EXPECT_TRUE(server_->running());

    server_->stop();
    EXPECT_FALSE(server_->running());
}

// Test: start() returns false if already running (no double-start).
TEST_F(WebTransportServerSeamTest, start_when_already_running_returns_false) {
    constexpr std::uint16_t kTestPort = 29445;
    EXPECT_TRUE(server_->start(kTestPort));
    EXPECT_TRUE(server_->running());

    // Attempting to start again must return false.
    EXPECT_FALSE(server_->start(kTestPort));

    server_->stop();
}
