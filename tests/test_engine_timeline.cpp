/**
 * @file tests/test_engine_timeline.cpp
 * @brief ADR-017 — the *engine* side of the cross-end frame timeline.
 *
 * `src/modules/rtp/tests/test_timeline.cpp` covers the reconstruction
 * arithmetic (`nimrtc::rtp::SenderReportTracker`) in isolation.  This
 * suite covers the wiring above it — the part that decides whether the
 * arithmetic ever runs at all:
 *
 *   1. `EngineConfig::timeline_enabled = false` emits **nothing**:
 *      no `a=extmap` in the offer, no abs-send-time on outbound RTP,
 *      no Sender Reports.  The flag must cost zero bytes on the wire
 *      when off (ADR-017 §"Decision 5").
 *
 *   2. With the flag on, `create_offer()` advertises the abs-send-time
 *      extension at `kAbsSendTimeDefaultLocalId` and the engine reports
 *      that id back through `abs_send_time_ext_id()`.
 *
 *   3. Before any inbound Sender Report is seen, every reconstruction
 *      accessor returns `std::nullopt` — the engine does NOT invent a
 *      capture timestamp from the abs-send-time field alone.
 *
 *   4. `Profile::timeline_enabled` → `EngineConfig::timeline_enabled` is
 *      the documented host hand-off; the field defaults to `false` so a
 *      host that forgets to copy it gets the off-state, not the on-state.
 *
 * The suite uses the "shell" transport strategy (the same sentinel
 * `test_datachannel_engine` uses) so `open()` reaches the open state
 * without binding ICE UDP sockets — these cases are about config
 * plumbing and SDP, not the transport.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/rtp/timeline.hpp>

namespace {

using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

class EngineTimeline : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
    }

    static EngineConfig make_test_config() {
        EngineConfig cfg;
        cfg.stun_server_host       = "";
        cfg.local_port_range_begin = 52400;
        cfg.local_port_range_end   = 52500;
        // "shell" is the Slice 8 sentinel strategy: open() succeeds
        // without touching the network.  See test_datachannel_engine.cpp
        // for why the real stack cannot be used on CI runners.
        cfg.transport_strategy_name = "shell";
        return cfg;
    }
};

// =====================================================================
// 1. Flag off ⇒ no extmap, no negotiated id.
// =====================================================================
TEST_F(EngineTimeline, DisabledEngineAdvertisesNoAbsSendTimeExtmap) {
    EngineConfig cfg = make_test_config();
    cfg.timeline_enabled = false;
    ASSERT_FALSE(cfg.timeline_enabled);

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_FALSE(engine.timeline_enabled())
        << "the accessor must mirror EngineConfig::timeline_enabled";

    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty()) << "create_offer() must succeed for a "
                                   "shell-strategy engine";

    EXPECT_EQ(offer.find(nimrtc::rtp::kAbsSendTimeUri), std::string::npos)
        << "with timeline_enabled=false the offer MUST NOT advertise the "
           "abs-send-time extension (ADR-017 Decision 5)";

    EXPECT_EQ(engine.abs_send_time_ext_id(), 0u)
        << "no extmap advertised ⇒ outbound packets carry no abs-send-time";
    EXPECT_EQ(engine.remote_abs_send_time_ext_id(), 0u);

    engine.close();
}

// =====================================================================
// 2. Flag on ⇒ extmap advertised at the well-known local id.
// =====================================================================
TEST_F(EngineTimeline, EnabledEngineAdvertisesAbsSendTimeExtmap) {
    EngineConfig cfg = make_test_config();
    cfg.timeline_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_TRUE(engine.timeline_enabled());

    const std::string offer = engine.create_offer();
    ASSERT_FALSE(offer.empty()) << "create_offer() must succeed for a "
                                   "shell-strategy engine";

    const auto pos = offer.find(nimrtc::rtp::kAbsSendTimeUri);
    ASSERT_NE(pos, std::string::npos)
        << "with timeline_enabled=true the offer MUST advertise "
           "abs-send-time so the peer knows to expect the element";

    // The line must name the id the engine actually writes outbound.
    const std::uint16_t id = engine.abs_send_time_ext_id();
    ASSERT_NE(id, 0u);
    EXPECT_EQ(id, nimrtc::rtp::kAbsSendTimeDefaultLocalId);
    EXPECT_NE(offer.find("a=extmap:" + std::to_string(id) + " " +
                         std::string(nimrtc::rtp::kAbsSendTimeUri)),
              std::string::npos)
        << "the advertised id must match abs_send_time_ext_id() — a "
           "mismatch is a silent 'extension present but wrong id' bug "
           "(RFC 5285 §6)";

    engine.close();
}

// =====================================================================
// 3. No SR seen yet ⇒ every reconstruction accessor is empty.
//
// This is the guard that the abs-send-time refinement is not allowed to
// fabricate an epoch on its own: the field is only meaningful relative
// to a Sender Report.
// =====================================================================
TEST_F(EngineTimeline, ReconstructionAccessorsAreEmptyWithoutSenderReport) {
    EngineConfig cfg = make_test_config();
    cfg.timeline_enabled = true;

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_FALSE(engine.last_capture_ts_us().has_value())
        << "no inbound SR ⇒ no capture timestamp may be synthesised";
    EXPECT_FALSE(engine.capture_ts_us_for_ssrc(0xDEADBEEF).has_value());
    EXPECT_FALSE(engine.last_one_way_delay_us().has_value())
        << "the one-way delay is a difference of two instants, both of "
           "which require an SR";

    engine.close();
}

// =====================================================================
// 4. The feature gate is opt-in: a default-constructed config is off.
// =====================================================================
TEST_F(EngineTimeline, TimelineIsOptInByDefault) {
    const EngineConfig defaults{};
    EXPECT_FALSE(defaults.timeline_enabled)
        << "ADR-017: timeline_enabled must default to false so an engine "
           "built without the seam emits no RTCP and no abs-send-time";

    // The SR cadence has a sane default even when the gate is off — it is
    // inert until the gate opens.
    EXPECT_GT(defaults.sr_period_ms, 0u);
    EXPECT_EQ(defaults.sr_period_ms, nimrtc::rtp::kSrPeriodMs)
        << "the default cadence should track nimrtc::rtp::kSrPeriodMs "
           "(the WebRTC default) so the two do not drift";
}

} // anonymous namespace