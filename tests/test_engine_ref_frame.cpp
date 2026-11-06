/**
 * @file tests/test_engine_ref_frame.cpp
 * @brief v1.1 T1/T2 — engine-side ref_frame <-> capture-ts map.
 *
 * Covers the layer *below* the ADR-017 cross-end timeline: OUR OWN frames,
 * not the peer's clock. send_video() records (frame_seq -> capture_ts_us)
 * automatically; record_video_ref_frame() allows explicit tagging; the
 * lookups resolve both directions; data_message_frame_age_us() answers the
 * T2 "which capture instant does this control message refer to?" query.
 *
 * Uses the "shell" transport strategy so open() succeeds without ICE/DTLS.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/core/time.hpp>
#include <nimrtc/engine/engine.hpp>

namespace {

using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
namespace np = nimrtc::plugins;

class EngineRefFrame : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
    }

    static EngineConfig make_test_config() {
        EngineConfig cfg;
        cfg.stun_server_host       = "";
        cfg.local_port_range_begin = 52600;
        cfg.local_port_range_end   = 52700;
        cfg.transport_strategy_name = "shell";
        cfg.video_codec_name    = "h264";
        cfg.video_sender_name   = "reference";
        cfg.video_receiver_name = "reference";
        cfg.video_source_name   = "memory";
        cfg.video_sink_name     = "headless";
        return cfg;
    }

    static std::int64_t steady_now_us() {
        return static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                nimrtc::core::SteadyClock::now().time_since_epoch())
                .count());
    }
};

TEST_F(EngineRefFrame, miss_returns_nullopt_before_any_record) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);
    EXPECT_FALSE(engine.ref_frame_to_capture_ts(1).has_value());
    EXPECT_FALSE(engine.capture_ts_to_ref_frame(1000).has_value());
    EXPECT_FALSE(engine.data_message_frame_age_us(1).has_value());
    engine.close();
}

TEST_F(EngineRefFrame, record_and_lookup_both_directions) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    engine.record_video_ref_frame(42u, 1234567);
    auto ts = engine.ref_frame_to_capture_ts(42u);
    ASSERT_TRUE(ts.has_value());
    EXPECT_EQ(*ts, 1234567);

    auto id = engine.capture_ts_to_ref_frame(1234567);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, 42u);

    // Re-record = last-writer-wins.
    engine.record_video_ref_frame(42u, 7654321);
    EXPECT_EQ(engine.ref_frame_to_capture_ts(42u).value(), 7654321);
    engine.close();
}

TEST_F(EngineRefFrame, send_video_auto_records_mapping) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::uint32_t w = 64, h = 48;
    const std::int32_t stride_y = 64, stride_u = 32, stride_v = 32;
    const std::size_t y_size  = static_cast<std::size_t>(stride_y) * h;
    const std::size_t uv_size = static_cast<std::size_t>(stride_u) * (h / 2);
    std::vector<std::uint8_t> buf(y_size + uv_size + uv_size, 0x80);

    np::VideoSourceFrame vf{};
    vf.width         = w;
    vf.height        = h;
    vf.format        = np::VideoPixelFormat::kI420;
    vf.stride_y      = stride_y;
    vf.stride_u      = stride_u;
    vf.stride_v      = stride_v;
    vf.plane_y       = buf.data();
    vf.plane_u       = buf.data() + y_size;
    vf.plane_v       = buf.data() + y_size + uv_size;
    vf.buffer_size   = buf.size();
    vf.capture_ts_us = 2000000;
    vf.frame_seq     = 7;
    vf.rtp_timestamp  = 180000;

    ASSERT_EQ(engine.send_video(vf), np::kOk);
    auto ts = engine.ref_frame_to_capture_ts(7u);
    ASSERT_TRUE(ts.has_value()) << "send_video() must auto-record ref_frame";
    EXPECT_EQ(*ts, 2000000);
    engine.close();
}

TEST_F(EngineRefFrame, data_message_age_tracks_capture_ts) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const std::int64_t now = steady_now_us();
    engine.record_video_ref_frame(99u, now - 5000);
    auto age = engine.data_message_frame_age_us(99u);
    ASSERT_TRUE(age.has_value());
    // Allow scheduling slack: age must be ~5000us, non-negative, < 5s.
    EXPECT_GE(*age, 4000);
    EXPECT_LT(*age, 5000000);
    engine.close();
}

TEST_F(EngineRefFrame, ring_eviction_bounds_memory) {
    EngineConfig cfg = make_test_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // Capacity is 1024; push 1030 and the first 6 must be gone.
    for (std::uint64_t i = 1; i <= 1030; ++i) {
        engine.record_video_ref_frame(i, static_cast<std::int64_t>(i * 1000));
    }
    EXPECT_FALSE(engine.ref_frame_to_capture_ts(1).has_value());
    EXPECT_FALSE(engine.ref_frame_to_capture_ts(6).has_value());
    EXPECT_TRUE(engine.ref_frame_to_capture_ts(7).has_value());
    EXPECT_TRUE(engine.ref_frame_to_capture_ts(1030).has_value());
    EXPECT_EQ(engine.ref_frame_to_capture_ts(1030).value(), 1030000);
    engine.close();
}

}  // namespace
