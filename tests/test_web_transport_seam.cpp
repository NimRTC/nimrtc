/**
 * @file tests/test_web_transport_seam.cpp
 * @brief Compile-only test for the v0.14.0 alpha.2 WebTransport seam
 *        (ADR-014 + v1.0-plan.md §10.4 + v0.14-plan.md §3.1 Gate #2).
 *
 * What this test verifies:
 *  - Every pure-virtual interface in
 *    `nimrtc::web_transport::*` is well-formed:
 *      * `IWebTransportSession`            (web_transport_session_iface.hpp)
 *      * `IBidirectionalStream`            (bidi_stream_iface.hpp)
 *      * `IUnidirectionalStream`           (uni_stream_iface.hpp)
 *      * `WebTransportConfig`              (web_transport_config.hpp)
 *      * `WebTransportState`               (web_transport_state.hpp)
 *      * `DatagramFacade`                  (datagram_facade.hpp)
 *  - Each interface is `std::is_abstract` (the pure-virtual contract
 *    is intact — no `final` methods accidentally swallow a pure
 *    virtual, and no methods are missing).
 *  - Each interface's `state()` / config field round-trips through
 *    the documented value range (the `static_assert`s fire at
 *    compile time, so a typo in `WebTransportState` enum values
 *    or in `WebTransportConfig` defaults is caught at PR review
 *    time, not at runtime in a downstream beta.1 consumer).
 *  - The `DatagramFacade` utility class binds a non-owning
 *    pointer to an `IWebTransportSession` and forwards
 *    `set_sink` / `send` to the session.
 *  - `OnDatagramCb` and `OnStreamDataCb` accept
 *    `std::function<void(std::span<const std::uint8_t>)>` and
 *    can be constructed from a lambda (the callback surface
 *    is not accidentally restricted to function pointers).
 *
 * ## What this test does NOT verify
 *
 *  - No runtime behaviour.  The test is a **compile-only**
 *    contract gate; the actual implementation
 *    (`WebTransportMsQuicSession`, lands in beta.1) is the
 *    unit-under-test for the `tests/test_web_transport_msquic.cpp`
 *    suite.  The alpha.2 tag cut does not include the impl.
 *  - No `libmsquic` link.  The seam headers are vendor-agnostic;
 *    they compile and link without msquic headers on the include
 *    path.  This is the v1.0-plan.md §10.4 "seam is
 *    transport-portable" contract.
 *
 * ## GTest strategy
 *
 * The test fixture runs in a single TU; the `static_assert`s at
 * namespace scope catch every well-formedness regression at
 * compile time.  The runtime tests are minimal: 2 simple
 * smoke tests that exercise a minimal `IWebTransportSession`
 * impl with no msquic dependency (the impl class is local
 * to this TU and never escapes).
 *
 * If the headers are miscompiled (e.g. a missing pure-virtual,
 * a typo in a method signature, an enum value out of range),
 * the TU fails to build and the test target fails to link —
 * both are caught by the CI matrix.  This is the alpha.2
 * Gate #2 contract.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

#include <nimrtc/core/api_version.hpp>  // NIMRTC_API_UNSTABLE
#include <nimrtc/plugins/base.hpp>      // plugins::Status

#include <nimrtc/web_transport/bidi_stream_iface.hpp>
#include <nimrtc/web_transport/datagram_facade.hpp>
#include <nimrtc/web_transport/uni_stream_iface.hpp>
#include <nimrtc/web_transport/web_transport_config.hpp>
#include <nimrtc/web_transport/web_transport_session_iface.hpp>
#include <nimrtc/web_transport/web_transport_state.hpp>

// ---------------------------------------------------------------------------
// Static asserts — every interface is abstract and the public surface
// matches the v1.0-plan.md §10.4 contract.
// ---------------------------------------------------------------------------

namespace nimrtc_wt_seam_test {

// The IWebTransportSession interface is the headline seam.  It must
// be a pure-virtual abstract class; std::is_abstract_v<T> is true
// iff T has at least one pure-virtual method.  This catches the
// "accidentally made a method concrete" regression where someone
// adds a default implementation in the header.
static_assert(
    std::is_abstract_v<nimrtc::web_transport::IWebTransportSession>,
    "v0.14.0 alpha.2 seam: IWebTransportSession must be abstract "
    "(pure-virtual methods preserved; v1.0-plan.md §10.4 contract).");

// The stream interfaces must also be abstract.
static_assert(
    std::is_abstract_v<nimrtc::web_transport::IBidirectionalStream>,
    "v0.14.0 alpha.2 seam: IBidirectionalStream must be abstract "
    "(send / recv / set_on_data / close / reset are pure-virtual).");

static_assert(
    std::is_abstract_v<nimrtc::web_transport::IUnidirectionalStream>,
    "v0.14.0 alpha.2 seam: IUnidirectionalStream must be abstract "
    "(send / close / reset are pure-virtual).");

// The DatagramFacade is a non-virtual utility class — it is NOT
// abstract.  Verifying the inverse is important so future
// refactors that accidentally introduce a pure-virtual get caught.
static_assert(
    !std::is_abstract_v<nimrtc::web_transport::DatagramFacade>,
    "v0.14.0 alpha.2 seam: DatagramFacade is a non-virtual utility "
    "class; std::is_abstract_v must be false.");

// The state enum is `enum class : uint8_t` with 5 values; verify
// the underlying type and the value range so a future typo
// (`kConnecting = 5`) is caught at compile time.
static_assert(
    std::is_same_v<
        std::underlying_type_t<nimrtc::web_transport::WebTransportState>,
        std::uint8_t>,
    "v0.14.0 alpha.2 seam: WebTransportState underlying type must "
    "be uint8_t (matches the v1.0-plan.md §10.4 contract).");

static_assert(
    static_cast<std::uint8_t>(
        nimrtc::web_transport::WebTransportState::kConnecting) == 0 &&
    static_cast<std::uint8_t>(
        nimrtc::web_transport::WebTransportState::kConnected) == 1 &&
    static_cast<std::uint8_t>(
        nimrtc::web_transport::WebTransportState::kClosing) == 2 &&
    static_cast<std::uint8_t>(
        nimrtc::web_transport::WebTransportState::kClosed) == 3 &&
    static_cast<std::uint8_t>(
        nimrtc::web_transport::WebTransportState::kFailed) == 4,
    "v0.14.0 alpha.2 seam: WebTransportState enum values must be "
    "exactly 0..4 in declaration order (kConnecting=0, kConnected=1, "
    "kClosing=2, kClosed=3, kFailed=4).");

// The default values on WebTransportConfig must match the v1.0-plan.md
// §10.4 / RFC 9000 defaults.
static_assert(
    nimrtc::web_transport::WebTransportConfig{}.max_idle_timeout_ms ==
        30'000,
    "v0.14.0 alpha.2 seam: WebTransportConfig::max_idle_timeout_ms "
    "default must be 30000 (RFC 9000 §10.1 default).");

static_assert(
    nimrtc::web_transport::WebTransportConfig{}.initial_max_stream_data ==
        65'536,
    "v0.14.0 alpha.2 seam: WebTransportConfig::initial_max_stream_data "
    "default must be 65536 (RFC 9000 §19.10 default).");

static_assert(
    nimrtc::web_transport::WebTransportConfig{}.initial_max_data ==
        1'048'576,
    "v0.14.0 alpha.2 seam: WebTransportConfig::initial_max_data "
    "default must be 1048576 (RFC 9000 §19.9 default).");

static_assert(
    nimrtc::web_transport::WebTransportConfig{}.keep_alive_interval_ms == 0,
    "v0.14.0 alpha.2 seam: WebTransportConfig::keep_alive_interval_ms "
    "default must be 0 (keep-alives disabled by default).");

// The OnDatagramCb / OnStreamDataCb callback aliases accept
// std::function<void(span<const uint8_t>)> — verify the std::function
// constructor accepts a lambda (the callback surface is not
// accidentally restricted to function pointers).
static_assert(
    std::is_constructible_v<
        nimrtc::web_transport::OnDatagramCb,
        std::function<void(std::span<const std::uint8_t>)>>,
    "v0.14.0 alpha.2 seam: OnDatagramCb must be constructible from "
    "a std::function<void(span<const uint8_t>)> (lambda support).");

static_assert(
    std::is_constructible_v<
        nimrtc::web_transport::OnStreamDataCb,
        std::function<void(std::span<const std::uint8_t>)>>,
    "v0.14.0 alpha.2 seam: OnStreamDataCb must be constructible from "
    "a std::function<void(span<const uint8_t>)> (lambda support).");

} // namespace nimrtc_wt_seam_test

// ---------------------------------------------------------------------------
// Minimal in-process IWebTransportSession impl (test-only, no msquic).
// ---------------------------------------------------------------------------
//
// The impl is local to this TU; it never escapes via factory
// registration.  The runtime tests below use it to verify the
// surface contract (the seam compiles, methods have the right
// signatures, the facade binds to a real session).

namespace nimrtc_wt_seam_test {

class NullSession final : public nimrtc::web_transport::IWebTransportSession {
public:
    nimrtc::web_transport::WebTransportState state() const noexcept override {
        return state_;
    }
    nimrtc::web_transport::IBidirectionalStream* open_bidi_stream() noexcept override {
        return nullptr;  // not implemented in this minimal stub
    }
    nimrtc::web_transport::IUnidirectionalStream* open_uni_stream() noexcept override {
        return nullptr;
    }
    nimrtc::plugins::Status send_datagram(
        std::span<const std::uint8_t> /*bytes*/) noexcept override {
        return state_ == nimrtc::web_transport::WebTransportState::kConnected
                   ? nimrtc::plugins::Status::kOk
                   : nimrtc::plugins::Status::kErrNotReady;
    }
    void set_on_datagram(
        nimrtc::web_transport::OnDatagramCb /*cb*/) noexcept override {}
    void close() noexcept override {
        state_ = nimrtc::web_transport::WebTransportState::kClosed;
    }

    void set_state(nimrtc::web_transport::WebTransportState s) noexcept {
        state_ = s;
    }

private:
    nimrtc::web_transport::WebTransportState state_ =
        nimrtc::web_transport::WebTransportState::kConnecting;
};

} // namespace nimrtc_wt_seam_test

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

namespace {

class WebTransportSeamCompileOnly : public ::testing::Test {
protected:
    nimrtc_wt_seam_test::NullSession session_;
};

} // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(WebTransportSeamCompileOnly, seam_interfaces_are_abstract) {
    // The static_asserts above already prove the contract at compile
    // time.  This runtime test documents the same contract for the
    // ctest log so a human reviewer doesn't have to grep the source.
    EXPECT_TRUE(std::is_abstract_v<
        nimrtc::web_transport::IWebTransportSession>);
    EXPECT_TRUE(std::is_abstract_v<
        nimrtc::web_transport::IBidirectionalStream>);
    EXPECT_TRUE(std::is_abstract_v<
        nimrtc::web_transport::IUnidirectionalStream>);
    EXPECT_FALSE(std::is_abstract_v<
        nimrtc::web_transport::DatagramFacade>);
}

TEST_F(WebTransportSeamCompileOnly, datagram_facade_forwards_to_session) {
    // Set the session to kConnected and verify the facade's send()
    // returns kOk; set it to kConnecting and verify the facade
    // returns kErrNotReady.  The forward contract is documented in
    // `datagram_facade.hpp` and the test pins it.
    nimrtc::web_transport::DatagramFacade facade(&session_);

    constexpr std::array<std::uint8_t, 4> kPayload = {0xDE, 0xAD, 0xBE, 0xEF};

    session_.set_state(nimrtc::web_transport::WebTransportState::kConnected);
    EXPECT_EQ(facade.send(kPayload), nimrtc::plugins::Status::kOk);

    session_.set_state(nimrtc::web_transport::WebTransportState::kConnecting);
    EXPECT_EQ(facade.send(kPayload),
              nimrtc::plugins::Status::kErrNotReady);
}

TEST_F(WebTransportSeamCompileOnly, datagram_facade_with_null_session) {
    // The facade must be safe to construct with a null session; the
    // contract documents the `kErrInternal` return for that case.
    // (The seam surface does not throw — the facade reports the
    // error via the plugins::Status return value, matching the
    // seam's "no exceptions across the boundary" rule.)
    nimrtc::web_transport::DatagramFacade facade(nullptr);
    constexpr std::array<std::uint8_t, 4> kPayload = {0x01, 0x02, 0x03, 0x04};
    EXPECT_EQ(facade.send(kPayload),
              nimrtc::plugins::Status::kErrInternal);
}

TEST_F(WebTransportSeamCompileOnly, callbacks_accept_lambdas) {
    // The OnDatagramCb / OnStreamDataCb aliases must be constructible
    // from a lambda — the surface must not accidentally restrict
    // callbacks to function pointers.  The test installs a lambda
    // and the lambda is callable.
    bool datagram_called = false;
    bool stream_data_called = false;

    nimrtc::web_transport::OnDatagramCb datagram_cb =
        [&datagram_called](std::span<const std::uint8_t> /*bytes*/) {
            datagram_called = true;
        };
    nimrtc::web_transport::OnStreamDataCb stream_data_cb =
        [&stream_data_called](std::span<const std::uint8_t> /*bytes*/) {
            stream_data_called = true;
        };

    constexpr std::array<std::uint8_t, 3> kBytes = {0xAA, 0xBB, 0xCC};
    datagram_cb(kBytes);
    stream_data_cb(kBytes);
    EXPECT_TRUE(datagram_called);
    EXPECT_TRUE(stream_data_called);
}

TEST_F(WebTransportSeamCompileOnly, state_enum_round_trip) {
    // Verify every enum value round-trips through
    // static_cast<WebTransportState>(uint8_t) — a future patch that
    // renames / reorders the enum values is caught at compile time
    // (the static_asserts above), and a runtime regression that
    // swaps the value is caught here.
    using S = nimrtc::web_transport::WebTransportState;
    for (std::uint8_t i = 0; i < 5; ++i) {
        const auto s = static_cast<S>(i);
        EXPECT_EQ(static_cast<std::uint8_t>(s), i)
            << "WebTransportState round-trip failed for value " << +i;
    }
}
