// modules/jb/tests/test_jb.cpp
// =============================================================================
// Jitter-buffer module tests.
//
// P0: Config/Stats/Frame defaults.
// P1: push/pop/flush/reset behaviour.
// =============================================================================

#include <gtest/gtest.h>

#include <nimrtc/jb/jitter_buffer.hpp>
#include <nimrtc/rtp/packet.hpp>

namespace {

using namespace nimrtc::jb;
using namespace nimrtc::core;
using namespace nimrtc::rtp;

// ---------------------------------------------------------------------------
// Helper — build a minimal raw RTP packet with PacketBuilder, then parse it
// back to produce a PacketView for push() testing.
//
// Returns nullopt if parsing fails. Caller guarantees the returned view's
// underlying ByteBuffer stays alive until push() completes.
// ---------------------------------------------------------------------------
std::optional<PacketView> make_view(std::uint16_t seq,
                                  std::uint32_t timestamp,
                                  std::uint8_t payload_type,
                                  std::uint32_t ssrc = 0xCAFEBABEu,
                                  bool marker = false) {
    PacketBuilder b;
    b.set_ssrc(ssrc).set_seq(seq).set_timestamp(timestamp)
        .set_payload_type(payload_type).set_marker(marker);
    std::uint8_t payload_bytes[2] = {
        static_cast<std::uint8_t>(seq >> 8),
        static_cast<std::uint8_t>(seq & 0xFF)
    };
    b.set_payload(ByteSpan{payload_bytes, sizeof(payload_bytes)});
    ByteBuffer raw = b.build();

    Parser p;
    auto r = p.parse(ByteSpan{raw.data(), raw.size()});
    if (!r.ok()) return std::nullopt;
    return std::make_optional(std::move(r).value());
}

// =============================================================================
// Config / Stats / Frame defaults
// =============================================================================

TEST(JbConfig, DefaultValues) {
    Config cfg;
    EXPECT_EQ(cfg.initial_delay, Milliseconds{50});
    EXPECT_EQ(cfg.min_delay,     Milliseconds{20});
    EXPECT_EQ(cfg.max_delay,     Milliseconds{400});
    EXPECT_EQ(cfg.max_packets,   512u);
    EXPECT_TRUE(cfg.adaptive);
}

TEST(JbStats, DefaultConstructsToZero) {
    Stats s;
    EXPECT_EQ(s.buffered_packets, 0u);
    EXPECT_EQ(s.frames_emitted,   0u);
    EXPECT_EQ(s.packets_dropped, 0u);
    EXPECT_EQ(s.current_delay,    Milliseconds{0});
    EXPECT_EQ(s.current_jitter,   Milliseconds{0});
}

TEST(JbFrame, DefaultConstructsToZero) {
    Frame f;
    EXPECT_TRUE(f.packets.empty());
    EXPECT_EQ(f.frame_timestamp,    0u);
    EXPECT_EQ(f.capture_latency,   Nanoseconds{0});
    EXPECT_FALSE(f.first_arrival.has_value());
    EXPECT_FALSE(f.ref_frame.has_value());
}

// =============================================================================
// JitterBuffer construction and basic stats
// =============================================================================

TEST(JbJitterBuffer, ConstructsWithDefaultConfig) {
    JitterBuffer jb{};   // uses default Config
    Stats s = jb.stats();
    EXPECT_EQ(s.buffered_packets, 0u);
    EXPECT_EQ(s.frames_emitted,   0u);
    EXPECT_EQ(s.packets_dropped,  0u);
    EXPECT_EQ(s.current_delay,    Milliseconds{0});
    EXPECT_EQ(s.current_jitter,   Milliseconds{0});
}

TEST(JbJitterBuffer, CustomConfig) {
    Config cfg;
    cfg.initial_delay = Milliseconds{100};
    cfg.min_delay     = Milliseconds{50};
    cfg.max_delay     = Milliseconds{200};
    cfg.max_packets   = 256;
    cfg.adaptive      = false;

    JitterBuffer jb{cfg};
    EXPECT_EQ(jb.config().initial_delay, Milliseconds{100});
    EXPECT_EQ(jb.config().min_delay,     Milliseconds{50});
    EXPECT_EQ(jb.config().max_delay,     Milliseconds{200});
    EXPECT_EQ(jb.config().max_packets,   256u);
    EXPECT_FALSE(jb.config().adaptive);
}

TEST(JbJitterBuffer, ConfigRoundTrip) {
    Config original;
    original.initial_delay = Milliseconds{75};
    original.adaptive = false;

    JitterBuffer jb{original};
    Config retrieved = jb.config();
    EXPECT_EQ(retrieved.initial_delay, original.initial_delay);
    EXPECT_EQ(retrieved.adaptive, original.adaptive);
}

// =============================================================================
// Push/pop behaviour
// =============================================================================

TEST(JbJitterBuffer, PopEmptyReturnsNullopt) {
    JitterBuffer jb{};
    auto frame = jb.pop(SteadyClock::now());
    EXPECT_FALSE(frame.has_value());
}

TEST(JbJitterBuffer, FlushEmptyReturnsEmpty) {
    JitterBuffer jb{};
    auto frames = jb.flush();
    EXPECT_TRUE(frames.empty());
}

TEST(JbJitterBuffer, ResetEmptyIsNoOp) {
    JitterBuffer jb{};
    jb.reset();   // no-throw on empty buffer
    Stats s = jb.stats();
    EXPECT_EQ(s.buffered_packets, 0u);
}

TEST(JbJitterBuffer, UpdateConfigAfterConstruction) {
    JitterBuffer jb{};
    Config new_cfg;
    new_cfg.initial_delay = Milliseconds{120};
    new_cfg.adaptive = false;

    jb.update_config(new_cfg);

    Config retrieved = jb.config();
    EXPECT_EQ(retrieved.initial_delay, Milliseconds{120});
    EXPECT_FALSE(retrieved.adaptive);
}

TEST(JbJitterBuffer, PushOnePacketPopEmitsFrameAfterDelay) {
    // Push a packet; pop immediately → nullopt (too early).
    // Pop after large delay → frame returned.
    JitterBuffer jb{};
    auto pv = make_view(/*seq*/ 1, /*ts*/ 1000, /*pt*/ 96);
    ASSERT_TRUE(pv.has_value());

    TimePoint t0 = SteadyClock::now();
    jb.push(*pv, t0, std::nullopt);

    auto frame = jb.pop(t0);  // no delay yet
    EXPECT_FALSE(frame.has_value());

    TimePoint t_late = t0 + Milliseconds{500};
    frame = jb.pop(t_late);
    ASSERT_TRUE(frame.has_value()) << "frame should be emitted after delay";
    ASSERT_EQ(frame->packets.size(), 1u);
    EXPECT_EQ(frame->frame_timestamp, 1000u);
    EXPECT_EQ(frame->packets[0].seq, 1u);
}

TEST(JbJitterBuffer, PushTwoPacketsSameTimestampOneFrame) {
    // Two packets with the same RTP timestamp belong to the same frame.
    JitterBuffer jb{};
    auto pv0 = make_view(/*seq*/ 1, /*ts*/ 2000, /*pt*/ 96);
    auto pv1 = make_view(/*seq*/ 2, /*ts*/ 2000, /*pt*/ 96);  // same ts
    ASSERT_TRUE(pv0.has_value());
    ASSERT_TRUE(pv1.has_value());

    TimePoint t0 = SteadyClock::now();
    jb.push(*pv0, t0, 100u);                       // ref_frame = 100
    jb.push(*pv1, t0 + Milliseconds{10}, std::nullopt);

    auto frame = jb.pop(t0 + Milliseconds{500});
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->packets.size(), 2u);
    EXPECT_EQ(frame->frame_timestamp, 2000u);
    EXPECT_TRUE(frame->ref_frame.has_value());
    EXPECT_EQ(frame->ref_frame.value(), 100u);
}

TEST(JbJitterBuffer, PopBeforePlayoutReturnsNullopt) {
    // pop() before playout time must not return a frame.
    JitterBuffer jb{};
    auto pv = make_view(/*seq*/ 5, /*ts*/ 3000, /*pt*/ 96);
    ASSERT_TRUE(pv.has_value());

    TimePoint t0 = SteadyClock::now();
    jb.push(*pv, t0, std::nullopt);

    // 1ms later is still too early
    auto frame = jb.pop(t0 + Milliseconds{1});
    EXPECT_FALSE(frame.has_value());

    Stats s = jb.stats();
    EXPECT_EQ(s.buffered_packets, 1u);
}

TEST(JbJitterBuffer, PushTwoFramesPopEmitsInOrder) {
    // Frames are emitted in timestamp order, not arrival order.
    JitterBuffer jb{};
    auto pv1 = make_view(/*seq*/ 10, /*ts*/ 10000, /*pt*/ 96);
    auto pv2 = make_view(/*seq*/ 20, /*ts*/ 20000, /*pt*/ 96);
    ASSERT_TRUE(pv1.has_value());
    ASSERT_TRUE(pv2.has_value());

    TimePoint t0 = SteadyClock::now();
    jb.push(*pv2, t0 + Milliseconds{10}, std::nullopt); // ts=20000 arrives first
    jb.push(*pv1, t0,              std::nullopt);         // ts=10000 arrives second

    TimePoint t_late = t0 + Milliseconds{500};

    auto frame1 = jb.pop(t_late);
    ASSERT_TRUE(frame1.has_value());
    EXPECT_EQ(frame1->frame_timestamp, 10000u) << "earlier ts emits first";

    auto frame2 = jb.pop(t_late);
    ASSERT_TRUE(frame2.has_value());
    EXPECT_EQ(frame2->frame_timestamp, 20000u);
}

TEST(JbJitterBuffer, FlushEmitsAllBufferedFrames) {
    // flush() returns every buffered frame regardless of playout time.
    JitterBuffer jb{};
    auto pv1 = make_view(/*seq*/ 1, /*ts*/ 4001, /*pt*/ 96);
    auto pv2 = make_view(/*seq*/ 2, /*ts*/ 4002, /*pt*/ 96);
    auto pv3 = make_view(/*seq*/ 3, /*ts*/ 4003, /*pt*/ 96);
    ASSERT_TRUE(pv1.has_value() && pv2.has_value() && pv3.has_value());

    TimePoint t0 = SteadyClock::now();
    jb.push(*pv1, t0, std::nullopt);
    jb.push(*pv2, t0, std::nullopt);
    jb.push(*pv3, t0, std::nullopt);

    auto frames = jb.flush();
    ASSERT_EQ(frames.size(), 3u);
    EXPECT_EQ(frames[0].frame_timestamp, 4001u);
    EXPECT_EQ(frames[1].frame_timestamp, 4002u);
    EXPECT_EQ(frames[2].frame_timestamp, 4003u);

    EXPECT_EQ(jb.stats().buffered_packets, 0u);
    EXPECT_FALSE(jb.pop(t0 + Milliseconds{500}).has_value());
}

TEST(JbJitterBuffer, ResetClearsState) {
    JitterBuffer jb{};
    auto pv = make_view(/*seq*/ 1, /*ts*/ 5000, /*pt*/ 96);
    ASSERT_TRUE(pv.has_value());

    TimePoint t0 = SteadyClock::now();
    jb.push(*pv, t0, std::nullopt);
    ASSERT_EQ(jb.stats().buffered_packets, 1u);

    jb.reset();

    Stats s = jb.stats();
    EXPECT_EQ(s.buffered_packets, 0u);
    EXPECT_EQ(s.frames_emitted, 0u);
    EXPECT_EQ(s.packets_dropped, 0u);
    EXPECT_FALSE(jb.pop(t0 + Milliseconds{500}).has_value());
}

TEST(JbJitterBuffer, BufferedPacketsTracksCorrectly) {
    JitterBuffer jb{};
    TimePoint t0 = SteadyClock::now();

    auto pv1 = make_view(/*seq*/ 1, /*ts*/ 6001, /*pt*/ 96);
    auto pv2 = make_view(/*seq*/ 2, /*ts*/ 6002, /*pt*/ 96);
    auto pv3 = make_view(/*seq*/ 3, /*ts*/ 6003, /*pt*/ 96);
    ASSERT_TRUE(pv1.has_value() && pv2.has_value() && pv3.has_value());

    jb.push(*pv1, t0, std::nullopt);
    EXPECT_EQ(jb.stats().buffered_packets, 1u);

    jb.push(*pv2, t0, std::nullopt);
    EXPECT_EQ(jb.stats().buffered_packets, 2u);

    jb.flush();
    EXPECT_EQ(jb.stats().buffered_packets, 0u);

    jb.push(*pv3, t0, std::nullopt);
    EXPECT_EQ(jb.stats().buffered_packets, 1u);
}

TEST(JbJitterBuffer, FramesEmittedStatIncrements) {
    JitterBuffer jb{};
    TimePoint t0 = SteadyClock::now();

    auto pv1 = make_view(/*seq*/ 1, /*ts*/ 7001, /*pt*/ 96);
    auto pv2 = make_view(/*seq*/ 2, /*ts*/ 7002, /*pt*/ 96);
    ASSERT_TRUE(pv1.has_value() && pv2.has_value());

    jb.push(*pv1, t0, std::nullopt);
    auto f1 = jb.pop(t0 + Milliseconds{500});
    ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(jb.stats().frames_emitted, 1u);

    jb.push(*pv2, t0 + Milliseconds{100}, std::nullopt);
    auto f2 = jb.pop(t0 + Milliseconds{600});
    ASSERT_TRUE(f2.has_value());
    EXPECT_EQ(jb.stats().frames_emitted, 2u);
}

TEST(JbJitterBuffer, MarkerBitPassedThrough) {
    // A packet with M=1 keeps its marker bit after buffering and emission.
    JitterBuffer jb{};
    auto pv = make_view(/*seq*/ 1, /*ts*/ 8000, /*pt*/ 96,
                        /*ssrc*/ 0xAABBCCDD, /*marker*/ true);
    ASSERT_TRUE(pv.has_value());
    ASSERT_TRUE(pv->marker) << "make_view must produce a marker packet";

    TimePoint t0 = SteadyClock::now();
    jb.push(*pv, t0, std::nullopt);
    auto frame = jb.pop(t0 + Milliseconds{500});
    ASSERT_TRUE(frame.has_value());
    ASSERT_FALSE(frame->packets.empty());
    EXPECT_TRUE(frame->packets[0].marker);
}

TEST(JbJitterBuffer, RefFrameIdPassedThrough) {
    // ref_frame set at push() must be preserved in the emitted frame.
    JitterBuffer jb{};
    auto pv = make_view(/*seq*/ 9, /*ts*/ 9000, /*pt*/ 96);
    ASSERT_TRUE(pv.has_value());

    TimePoint t0 = SteadyClock::now();
    jb.push(*pv, t0, 42u);

    auto frame = jb.pop(t0 + Milliseconds{500});
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(frame->ref_frame.has_value());
    EXPECT_EQ(frame->ref_frame.value(), 42u);
}

TEST(JbJitterBuffer, CaptureLatencySetOnFirstPacket) {
    // first_arrival records the monotonic arrival of the first packet in the
    // frame (not the wall-clock now at pop() time).
    JitterBuffer jb{};
    auto pv = make_view(/*seq*/ 1, /*ts*/ 10000, /*pt*/ 96);
    ASSERT_TRUE(pv.has_value());

    TimePoint t0 = SteadyClock::now();
    jb.push(*pv, t0, std::nullopt);

    auto frame = jb.pop(t0 + Milliseconds{500});
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(frame->first_arrival.has_value());
    EXPECT_EQ(*frame->first_arrival, t0);
}

}  // namespace
