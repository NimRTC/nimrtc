// modules/jb/tests/test_jb.cpp
// =============================================================================
// Jitter-buffer module tests.
//
// P0: Config/Stats/Frame defaults.
// P1: push/pop/flush/reset behaviour.
// Q2: the memory bound (video plan R4 / v1.3 plan Q2).
// =============================================================================

#include <gtest/gtest.h>

#include <nimrtc/jb/jitter_buffer.hpp>
#include <nimrtc/rtp/packet.hpp>

#include <cstdint>
#include <optional>
#include <vector>

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

// =============================================================================
// The memory bound (v1.3 plan Q2 / video plan R4)
//
// The bound is what makes the jitter buffer's memory *bounded*: a peer that
// sends faster than the receiver drains — or a stream whose frames never
// become playable — must not be able to grow the buffer without limit.
//
// The tests below are the ones that would have failed before the bound was
// enforced: `Config::max_packets` used to be a declaration that nothing read.
// =============================================================================

// ---------------------------------------------------------------------------
// Helper — build a packet with a payload of `payload_bytes` bytes.
//
// The default make_view() payload is 2 bytes, which is too small to make a
// byte bound bite; the byte-bound tests need packets whose *size* is the
// thing being measured.
// ---------------------------------------------------------------------------
std::optional<PacketView> make_sized_view(std::uint16_t seq,
                                          std::uint32_t timestamp,
                                          std::size_t payload_bytes) {
    PacketBuilder b;
    b.set_ssrc(0xCAFEBABEu).set_seq(seq).set_timestamp(timestamp)
        .set_payload_type(96).set_marker(false);
    std::vector<std::uint8_t> payload(payload_bytes, 0x5A);
    b.set_payload(ByteSpan{payload.data(), payload.size()});
    ByteBuffer raw = b.build();

    Parser p;
    auto r = p.parse(ByteSpan{raw.data(), raw.size()});
    if (!r.ok()) return std::nullopt;
    return std::make_optional(std::move(r).value());
}

TEST(JbMemoryBound, PacketCountNeverExceedsTheCap) {
    // A sustained overrun: push far more packets than the cap, without
    // popping.  The buffered count must never exceed the cap.
    Config cfg;
    cfg.max_packets = 16;
    cfg.max_bytes   = 0;            // the packet count is the bound under test
    JitterBuffer jb{cfg};

    TimePoint t0 = SteadyClock::now();
    for (std::uint16_t i = 0; i < 200; ++i) {
        auto pv = make_view(i, /*ts*/ 1000u + i, /*pt*/ 96);
        ASSERT_TRUE(pv.has_value());
        jb.push(*pv, t0, std::nullopt);

        EXPECT_LE(jb.stats().buffered_packets, cfg.max_packets)
            << "the packet count must never exceed the cap (pushed "
            << static_cast<int>(i) + 1 << " packets)";
    }

    EXPECT_EQ(jb.stats().buffered_packets, cfg.max_packets);
}

TEST(JbMemoryBound, SustainedOverrunReportsTheDrops) {
    // The trim must be *reported*: a host that sees the counter move knows
    // the receiver could not keep up with the sender.
    Config cfg;
    cfg.max_packets = 8;
    cfg.max_bytes   = 0;
    JitterBuffer jb{cfg};

    TimePoint t0 = SteadyClock::now();
    for (std::uint16_t i = 0; i < 100; ++i) {
        auto pv = make_view(i, /*ts*/ 2000u + i, /*pt*/ 96);
        ASSERT_TRUE(pv.has_value());
        jb.push(*pv, t0, std::nullopt);
    }

    const Stats s = jb.stats();
    EXPECT_EQ(s.buffered_packets, 8u);
    EXPECT_EQ(s.packets_dropped, 92u)
        << "100 packets pushed, 8 kept → 92 dropped";
}

TEST(JbMemoryBound, TheNewestPacketIsNeverTheVictim) {
    // The trim runs *before* the insert, so the packet that has just
    // arrived is never the one the bound evicts.  Without that rule the
    // newest audio — the one the peer just sent — would always be the
    // first to be dropped.
    Config cfg;
    cfg.max_packets = 4;
    cfg.max_bytes   = 0;
    JitterBuffer jb{cfg};

    TimePoint t0 = SteadyClock::now();
    for (std::uint16_t i = 0; i < 4; ++i) {
        auto pv = make_view(i, /*ts*/ 3000u + i, /*pt*/ 96);
        ASSERT_TRUE(pv.has_value());
        jb.push(*pv, t0, std::nullopt);
    }

    // The fifth packet evicts the *oldest* frame, not the new one.
    auto newest = make_view(4, /*ts*/ 3004, /*pt*/ 96);
    ASSERT_TRUE(newest.has_value());
    jb.push(*newest, t0, std::nullopt);

    auto frames = jb.flush();
    ASSERT_EQ(frames.size(), 4u);
    std::vector<std::uint32_t> timestamps;
    for (const auto& f : frames) timestamps.push_back(f.frame_timestamp);
    EXPECT_EQ(timestamps, (std::vector<std::uint32_t>{3001u, 3002u, 3003u, 3004u}))
        << "the frame that was just pushed must survive the trim, and the "
           "oldest one must be the victim";
}

TEST(JbMemoryBound, APacketThatJoinsABufferedFrameStillRespectsTheCap) {
    // A frame may carry more than one packet (redundant audio paths, and
    // the receiver's own reassembly).  The cap counts *packets*, so such a
    // packet is charged against it like any other — the buffer stays
    // bounded either way.
    Config cfg;
    cfg.max_packets = 4;
    cfg.max_bytes   = 0;
    JitterBuffer jb{cfg};

    TimePoint t0 = SteadyClock::now();
    for (std::uint16_t i = 0; i < 4; ++i) {
        auto pv = make_view(i, /*ts*/ 9000u + i, /*pt*/ 96);
        ASSERT_TRUE(pv.has_value());
        jb.push(*pv, t0, std::nullopt);
    }
    ASSERT_EQ(jb.stats().buffered_packets, 4u);

    // The fifth packet joins the *first* frame (same RTP timestamp).
    auto joiner = make_view(99, /*ts*/ 9000, /*pt*/ 96);
    ASSERT_TRUE(joiner.has_value());
    jb.push(*joiner, t0, std::nullopt);

    EXPECT_LE(jb.stats().buffered_packets, cfg.max_packets);
    EXPECT_EQ(jb.stats().packets_dropped, 1u)
        << "the oldest frame is trimmed to make room for the joiner";
}

TEST(JbMemoryBound, TheOldestFrameIsTrimmedFirst) {
    // The trim is FIFO from the oldest (the map is keyed by RTP timestamp).
    Config cfg;
    cfg.max_packets = 3;
    cfg.max_bytes   = 0;
    JitterBuffer jb{cfg};

    TimePoint t0 = SteadyClock::now();
    // Push out of order on purpose: the *timestamp* is what decides which
    // frame is oldest, not the arrival order.
    for (std::uint32_t ts : {100u, 300u, 200u, 400u}) {
        auto pv = make_view(static_cast<std::uint16_t>(ts), ts, /*pt*/ 96);
        ASSERT_TRUE(pv.has_value());
        jb.push(*pv, t0, std::nullopt);
    }

    // ts=100 is the oldest → it is the one that was trimmed.
    auto frames = jb.flush();
    ASSERT_EQ(frames.size(), 3u);
    std::vector<std::uint32_t> timestamps;
    for (const auto& f : frames) timestamps.push_back(f.frame_timestamp);
    EXPECT_EQ(timestamps, (std::vector<std::uint32_t>{200u, 300u, 400u}));
}

TEST(JbMemoryBound, ByteCountNeverExceedsTheCap) {
    // A packet-count cap alone does not bound memory: the packets of one
    // stream can have wildly different sizes.  The byte bound is the one
    // that caps what the receiver actually holds.
    Config cfg;
    cfg.max_packets = 0;            // the byte count is the bound under test
    cfg.max_bytes   = 4096;
    JitterBuffer jb{cfg};

    TimePoint t0 = SteadyClock::now();
    for (std::uint16_t i = 0; i < 50; ++i) {
        auto pv = make_sized_view(i, /*ts*/ 4000u + i, /*payload*/ 1024);
        ASSERT_TRUE(pv.has_value());
        jb.push(*pv, t0, std::nullopt);

        EXPECT_LE(jb.stats().buffered_bytes, cfg.max_bytes)
            << "the byte count must never exceed the cap (pushed "
            << static_cast<int>(i) + 1 << " packets of 1 KiB)";
    }

    EXPECT_EQ(jb.stats().buffered_bytes, 4096u);
    EXPECT_GT(jb.stats().packets_dropped, 0u);
}

TEST(JbMemoryBound, ByteCountBoundsMemoryWithVariedPacketSizes) {
    // The same bound, with packets whose sizes differ by two orders of
    // magnitude: a count-only cap would let this through.
    Config cfg;
    cfg.max_packets = 0;
    cfg.max_bytes   = 8192;
    JitterBuffer jb{cfg};

    TimePoint t0 = SteadyClock::now();
    for (std::uint16_t i = 0; i < 40; ++i) {
        const std::size_t size = (i % 4 == 0) ? 4000 : 100;
        auto pv = make_sized_view(i, /*ts*/ 5000u + i, size);
        ASSERT_TRUE(pv.has_value());
        jb.push(*pv, t0, std::nullopt);

        EXPECT_LE(jb.stats().buffered_bytes, cfg.max_bytes);
    }
    EXPECT_GT(jb.stats().packets_dropped, 0u);
}

TEST(JbMemoryBound, TighteningTheCapTakesEffectImmediately) {
    // A host that tightens the bound at runtime expects the buffer to be
    // inside it *now* — not at the next push().
    Config cfg;
    cfg.max_packets = 64;
    cfg.max_bytes   = 0;
    JitterBuffer jb{cfg};

    TimePoint t0 = SteadyClock::now();
    for (std::uint16_t i = 0; i < 32; ++i) {
        auto pv = make_view(i, /*ts*/ 6000u + i, /*pt*/ 96);
        ASSERT_TRUE(pv.has_value());
        jb.push(*pv, t0, std::nullopt);
    }
    ASSERT_EQ(jb.stats().buffered_packets, 32u);

    Config tighter = cfg;
    tighter.max_packets = 8;
    jb.update_config(tighter);

    EXPECT_EQ(jb.stats().buffered_packets, 8u);
    EXPECT_EQ(jb.stats().packets_dropped, 24u);
}

TEST(JbMemoryBound, AnUnboundedBufferKeepsEverything) {
    // 0 disables the cap on both halves: the historical behaviour stays
    // available for a caller that wants it.
    Config cfg;
    cfg.max_packets = 0;
    cfg.max_bytes   = 0;
    JitterBuffer jb{cfg};

    TimePoint t0 = SteadyClock::now();
    for (std::uint16_t i = 0; i < 100; ++i) {
        auto pv = make_view(i, /*ts*/ 7000u + i, /*pt*/ 96);
        ASSERT_TRUE(pv.has_value());
        jb.push(*pv, t0, std::nullopt);
    }

    const Stats s = jb.stats();
    EXPECT_EQ(s.buffered_packets, 100u);
    EXPECT_EQ(s.packets_dropped, 0u);
}

TEST(JbMemoryBound, DroppedPacketsAreNotCountedAsEmitted) {
    // The drop counter and the emit counter are two different things: a
    // trimmed frame is neither emitted nor played out.
    Config cfg;
    cfg.max_packets = 4;
    cfg.max_bytes   = 0;
    JitterBuffer jb{cfg};

    TimePoint t0 = SteadyClock::now();
    for (std::uint16_t i = 0; i < 40; ++i) {
        auto pv = make_view(i, /*ts*/ 8000u + i, /*pt*/ 96);
        ASSERT_TRUE(pv.has_value());
        jb.push(*pv, t0, std::nullopt);
    }

    EXPECT_EQ(jb.stats().frames_emitted, 0u);

    auto frames = jb.flush();
    EXPECT_EQ(frames.size(), 4u);
    EXPECT_EQ(jb.stats().frames_emitted, 0u)
        << "flush() is a drain, not a playout";
    EXPECT_EQ(jb.stats().buffered_bytes, 0u);
}

}  // namespace
