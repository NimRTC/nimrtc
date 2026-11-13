/**
 * @file src/modules/video_jb/tests/test_video_jb.cpp
 * @brief Video jitter buffer unit tests.
 *
 * Covers:
 *   1. Basic frame emission on marker bit.
 *   2. Multi-packet frame (no marker until end) — held until marker.
 *   3. FU-A fragment reassembly → complete NAL unit emission.
 *   4. NACK entries for lost packets within a frame.
 *   5. Timeout-driven emission of incomplete frames.
 *   6. Reset clears state.
 *   7. Multiple concurrent frames (different RTP timestamps).
 *   8. Adaptive playout (shared nimrtc::jb_strategy): playout deadline,
 *      jitter convergence, delay window bounds.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <nimrtc/video_jb/video_jitter_buffer.hpp>
#include <nimrtc/video_payload/h264.hpp>
#include <nimrtc/video_frame/frame.hpp>

namespace vjb = nimrtc::video_jb;
namespace h264 = nimrtc::video_payload::h264;
namespace video_frame = nimrtc::video_frame;
namespace core = nimrtc::core;

using vjb::Config;
using vjb::Mode;
using vjb::InboundPacket;
using vjb::create;
using h264::build_fu_a;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kSsrc = 0x12345678;

InboundPacket make_packet(std::uint16_t seq,
                          std::uint32_t rtp_ts,
                          bool marker,
                          core::ByteSpan payload,
                          std::int64_t arrival_us = 0,
                          video_frame::CodecKind codec = video_frame::CodecKind::kH264) {
    InboundPacket p;
    p.ssrc          = kSsrc;
    p.seq           = seq;
    p.rtp_timestamp = rtp_ts;
    p.marker        = marker;
    p.arrival_us    = arrival_us;
    p.codec         = codec;
    p.payload_type  = 102;        // WebRTC H264 default
    p.payload       = payload;
    return p;
}

} // namespace

// -----------------------------------------------------------------------------
// Basic emission
// -----------------------------------------------------------------------------

TEST(VideoJb, SinglePacketFrame) {
    auto jb = create({});
    std::uint8_t nalu[] = {0x65, 0xAA, 0xBB};   // IDR slice
    auto p = make_packet(100, 1000, /*marker=*/true, {nalu, sizeof(nalu)});
    EXPECT_TRUE(jb->insert(p));

    auto f = jb->pop_frame();
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->bitstream.size(), sizeof(nalu));
    EXPECT_EQ(f->bitstream[0], 0x65);
    EXPECT_TRUE(f->is_keyframe);
    EXPECT_TRUE(f->complete);
}

// -----------------------------------------------------------------------------
// Multi-packet frame held until marker
// -----------------------------------------------------------------------------

TEST(VideoJb, MultiPacketNoMarker) {
    auto jb = create({});

    std::uint8_t p1[] = {0x65, 0xAA};
    std::uint8_t p2[] = {0xBB, 0xCC};
    std::uint8_t p3[] = {0xDD, 0xEE};

    jb->insert(make_packet(100, 1000, false, {p1, sizeof(p1)}));
    EXPECT_EQ(jb->pop_frame(), nullptr);

    jb->insert(make_packet(101, 1000, false, {p2, sizeof(p2)}));
    EXPECT_EQ(jb->pop_frame(), nullptr);

    jb->insert(make_packet(102, 1000, true,  {p3, sizeof(p3)}));
    auto f = jb->pop_frame();
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->bitstream.size(), 6u);
    EXPECT_TRUE(f->complete);
}

// -----------------------------------------------------------------------------
// FU-A reassembly
// -----------------------------------------------------------------------------

TEST(VideoJb, FuAReassembly) {
    auto jb = create({});

    // Original NAL unit (without start code): 7 bytes payload
    //   header 0x65 + 7 bytes = 8 bytes total NALU
    // Split into 3 FU-A fragments: 3 + 3 + 1 bytes body
    std::uint8_t body[7] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11};

    std::uint8_t out0[8], out1[8], out2[8];
    std::size_t n0 = build_fu_a(h264::NaluType::kSliceIDR, {body,     3},
                                true,  false, {out0, sizeof(out0)});
    std::size_t n1 = build_fu_a(h264::NaluType::kSliceIDR, {body + 3, 3},
                                false, false, {out1, sizeof(out1)});
    std::size_t n2 = build_fu_a(h264::NaluType::kSliceIDR, {body + 6, 1},
                                false, true,  {out2, sizeof(out2)});

    jb->insert(make_packet(100, 2000, false, {out0, n0}));
    jb->insert(make_packet(101, 2000, false, {out1, n1}));
    jb->insert(make_packet(102, 2000, true,  {out2, n2}));

    auto f = jb->pop_frame();
    ASSERT_NE(f, nullptr);
    // The jitter buffer re-frames the reassembled NAL unit in Annex-B:
    // 4 (start code) + 1 (synthesized NAL header) + 7 (body) = 12 bytes.
    ASSERT_EQ(f->bitstream.size(), 12u);
    EXPECT_EQ(f->bitstream[0], 0x00);
    EXPECT_EQ(f->bitstream[1], 0x00);
    EXPECT_EQ(f->bitstream[2], 0x00);
    EXPECT_EQ(f->bitstream[3], 0x01);
    EXPECT_EQ(f->bitstream[4] & 0x1F, 5);   // NAL type IDR
    EXPECT_EQ(std::memcmp(f->bitstream.data() + 5, body, 7), 0);
    EXPECT_TRUE(f->is_keyframe);
}

// -----------------------------------------------------------------------------
// NACK detection for lost packets
// -----------------------------------------------------------------------------

TEST(VideoJb, NackOnGaps) {
    Config cfg;
    cfg.emit_nacks = true;
    auto jb = create(cfg);

    std::uint8_t p1[] = {0x65, 0xAA};
    std::uint8_t p3[] = {0xCC, 0xDD};        // seq 102 missing
    jb->insert(make_packet(100, 3000, false, {p1, sizeof(p1)}));
    jb->insert(make_packet(103, 3000, true,  {p3, sizeof(p3)}));

    auto nacks = jb->pop_nack_entries();
    std::sort(nacks.begin(), nacks.end());
    ASSERT_EQ(nacks.size(), 2u);
    EXPECT_EQ(nacks[0], 101u);
    EXPECT_EQ(nacks[1], 102u);
}

// -----------------------------------------------------------------------------
// Timeout-driven emit
// -----------------------------------------------------------------------------

TEST(VideoJb, TimeoutEmit) {
    Config cfg;
    cfg.max_wait_ms = 50;
    auto jb = create(cfg);

    std::uint8_t p1[] = {0x41, 0xAA};        // non-IDR slice (P-frame)
    jb->insert(make_packet(100, 4000, false, {p1, sizeof(p1)}, /*arrival_us=*/0));

    // No marker — frame can't complete.  Wait for timeout.
    jb->tick(/*now_us=*/60 * 1000);          // 60 ms later

    auto f = jb->pop_frame();
    ASSERT_NE(f, nullptr);
    EXPECT_FALSE(f->complete);               // forced
    EXPECT_FALSE(f->is_keyframe);
    EXPECT_EQ(jb->stats().frames_incomplete, 1u);
}

// -----------------------------------------------------------------------------
// Reset
// -----------------------------------------------------------------------------

TEST(VideoJb, ResetClearsState) {
    auto jb = create({});
    std::uint8_t nalu[] = {0x65, 0xAA};
    jb->insert(make_packet(100, 1000, true, {nalu, sizeof(nalu)}));

    jb->reset();
    EXPECT_EQ(jb->pop_frame(), nullptr);
    EXPECT_EQ(jb->stats().packets_inserted, 0u);
}

// -----------------------------------------------------------------------------
// Multiple concurrent frames
// -----------------------------------------------------------------------------

TEST(VideoJb, ConcurrentFrames) {
    auto jb = create({});

    std::uint8_t f1[] = {0x65, 0x11};        // IDR (keyframe)
    std::uint8_t f2[] = {0x41, 0x22};        // P-slice

    // Frame 1: RTP ts=1000, marker set on last packet.
    jb->insert(make_packet(100, 1000, false, {f1, sizeof(f1)}));
    // Frame 2: RTP ts=2000, marker set on last packet.
    jb->insert(make_packet(101, 2000, true,  {f2, sizeof(f2)}));

    auto out1 = jb->pop_frame();
    ASSERT_NE(out1, nullptr);
    EXPECT_EQ(out1->rtp_timestamp, 2000u);   // Frame 2 emitted first (marker only)
    EXPECT_FALSE(out1->is_keyframe);

    // Frame 1 needs its marker bit — add it.
    jb->insert(make_packet(102, 1000, true, {}));
    auto out2 = jb->pop_frame();
    ASSERT_NE(out2, nullptr);
    EXPECT_EQ(out2->rtp_timestamp, 1000u);
    EXPECT_TRUE(out2->is_keyframe);
}

// -----------------------------------------------------------------------------
// Stats
// -----------------------------------------------------------------------------

TEST(VideoJb, StatsCounters) {
    auto jb = create({});
    std::uint8_t nalu[] = {0x65, 0xAA};
    jb->insert(make_packet(100, 1000, true, {nalu, sizeof(nalu)}));
    auto f = jb->pop_frame();
    ASSERT_NE(f, nullptr);

    auto s = jb->stats();
    EXPECT_EQ(s.packets_inserted, 1u);
    EXPECT_EQ(s.frames_emitted,   1u);
    EXPECT_EQ(s.frames_incomplete, 0u);
}

// -----------------------------------------------------------------------------
// Codec kind on assembled frame
// -----------------------------------------------------------------------------

TEST(VideoJb, AssembledFrameCodecKind) {
    auto jb = create({});
    std::uint8_t h264_nalu[] = {0x65, 0xAA};
    jb->insert(make_packet(100, 1000, true, {h264_nalu, sizeof(h264_nalu)}));
    auto f = jb->pop_frame();
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->codec, video_frame::CodecKind::kH264);
}

TEST(VideoJb, AssembledFrameCodecKindVP8) {
    auto jb = create({});
    // Per the codebase's `is_keyframe_payload` (video_jitter_buffer.cpp):
    //   VP8 keyframe = (payload[0] & 0x01) == 0  (inverse-key-frame bit)
    // Use 0x9C (LSB=0) so the bit-pattern actually represents a keyframe.
    std::uint8_t vp8_bs[] = {0x9C, 0x01, 0x2A};
    jb->insert(make_packet(100, 1000, true, {vp8_bs, sizeof(vp8_bs)},
                           0, video_frame::CodecKind::kVP8));
    auto f = jb->pop_frame();
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->codec, video_frame::CodecKind::kVP8);
    EXPECT_TRUE(f->is_keyframe);
}

// -----------------------------------------------------------------------------
// Adaptive playout (v1.1 Q2 — shared nimrtc::jb_strategy, canonical suite:
// VideoJbAdaptive below; plus timeout/ordering coverage here)
// -----------------------------------------------------------------------------

namespace {

/** Adaptive config with an explicit delay window so the expectations do
 *  not depend on the per-mode defaults. */
Config adaptive_cfg(std::uint32_t base_ms = 40,
                    std::uint32_t min_ms = 20,
                    std::uint32_t max_ms = 200) {
    Config cfg;
    cfg.adaptive   = true;
    cfg.base_delay = core::Milliseconds{base_ms};
    cfg.min_delay  = core::Milliseconds{min_ms};
    cfg.max_delay  = core::Milliseconds{max_ms};
    return cfg;
}

} // namespace

TEST(VideoJbAdaptive, CompleteFrameSurvivesFrameTimeout) {
    // max_wait_ms is shorter than the playout deadline: the frame must
    // not be dropped by the timeout while it waits for its deadline.
    Config cfg = adaptive_cfg(/*base_ms=*/400, /*min_ms=*/20, /*max_ms=*/400);
    cfg.max_wait_ms = 50;
    auto jb = create(cfg);

    std::uint8_t nalu[] = {0x65, 0xAA};
    jb->insert(make_packet(100, 1000, true, {nalu, sizeof(nalu)}, 0));

    jb->tick(/*now_us=*/60 * 1000);

    // Still buffered (not dropped) and emitted once it is due.
    EXPECT_EQ(jb->pop_frame(/*now_us=*/60 * 1000), nullptr);
    auto f = jb->pop_frame(/*now_us=*/400'000);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->rtp_timestamp, 1000u);
    EXPECT_EQ(jb->stats().frames_incomplete, 0u);
    EXPECT_EQ(jb->stats().frames_emitted, 1u);
}

TEST(VideoJbAdaptive, EmitsFramesInOrder) {
    auto jb = create(adaptive_cfg(/*base_ms=*/40));

    std::uint8_t f1[] = {0x65, 0x11};        // IDR
    std::uint8_t f2[] = {0x41, 0x22};        // P-slice
    jb->insert(make_packet(100, 1000, true, {f1, sizeof(f1)}, 0));
    jb->insert(make_packet(101, 4000, true, {f2, sizeof(f2)}, 33'333));

    // Frame 1 is due 40 ms after its arrival, frame 2 40 ms after its own.
    auto out1 = jb->pop_frame(/*now_us=*/40'000);
    ASSERT_NE(out1, nullptr);
    EXPECT_EQ(out1->rtp_timestamp, 1000u);

    auto out2 = jb->pop_frame(/*now_us=*/73'333);
    ASSERT_NE(out2, nullptr);
    EXPECT_EQ(out2->rtp_timestamp, 4000u);
}

// -----------------------------------------------------------------------------
// Shared adaptive playout (v1.1 Q2)
// -----------------------------------------------------------------------------

namespace {

// One single-packet frame per call. RTP timestamps advance one 30 fps
// frame (3000 ticks @ 90 kHz = 33.333 ms media time) per frame.
void insert_frame(vjb::IVideoJitterBuffer& jb, int frame_index,
                  std::int64_t arrival_us, std::uint16_t seq_base = 100) {
    std::uint8_t nalu[] = {0x65, 0xAA};
    const auto rtp_ts = static_cast<std::uint32_t>(frame_index * 3000);
    const auto seq = static_cast<std::uint16_t>(seq_base + frame_index);
    EXPECT_TRUE(jb.insert(make_packet(seq, rtp_ts, /*marker=*/true,
                                       {nalu, sizeof(nalu)}, arrival_us)));
}

Config adaptive_config() {
    return adaptive_cfg(/*base_ms=*/20, /*min_ms=*/20, /*max_ms=*/400);
}

}  // namespace

TEST(VideoJbAdaptive, GateHoldsFrameUntilPlayoutDeadline) {
    auto jb = create(adaptive_config());

    std::uint8_t nalu[] = {0x65, 0xAA};
    EXPECT_TRUE(jb->insert(make_packet(100, 0, /*marker=*/true,
                                       {nalu, sizeof(nalu)},
                                       /*arrival_us=*/0)));
    // Target delay with no jitter yet = base 20 ms: not due at 19 ms.
    EXPECT_EQ(jb->pop_frame(/*now_us=*/19 * 1000), nullptr);
    // Due exactly at the 20 ms deadline.
    auto f = jb->pop_frame(/*now_us=*/20 * 1000);
    ASSERT_NE(f, nullptr);
    EXPECT_TRUE(f->complete);
}

TEST(VideoJbAdaptive, FixedModeIgnoresGate) {
    // Default config is fixed-window: the timed overload must behave
    // like the immediate one (no deadline gate).
    auto jb = create({});
    std::uint8_t nalu[] = {0x65, 0xAA};
    EXPECT_TRUE(jb->insert(make_packet(100, 0, /*marker=*/true,
                                       {nalu, sizeof(nalu)},
                                       /*arrival_us=*/0)));
    auto f = jb->pop_frame(/*now_us=*/0);
    ASSERT_NE(f, nullptr);
    EXPECT_TRUE(f->complete);
}

TEST(VideoJbAdaptive, JitterEstimateConvergesAndDelayFollows) {
    auto jb = create(adaptive_config());

    // Media advances 33.333 ms/frame; arrival alternates +0 / +20 ms so
    // the transit difference |D| is 20 ms every step.
    constexpr std::int64_t kMediaStepUs = 33333;
    for (int i = 0; i < 80; ++i) {
        const std::int64_t media_us = static_cast<std::int64_t>(i) * kMediaStepUs;
        const std::int64_t arrival_us =
            media_us + ((i % 2 == 0) ? 0 : 20 * 1000);
        insert_frame(*jb, i, arrival_us);
        // Drain immediately so the buffer does not fill up; stats keep
        // the shared estimator state.
        auto f = jb->pop_frame();
        ASSERT_NE(f, nullptr);
    }

    const auto stats = jb->stats();
    // Smoothed |D| converges near 20 ms (truncated to whole ms).
    EXPECT_GE(stats.current_jitter.count(), 15);
    EXPECT_LE(stats.current_jitter.count(), 20);
    // target = base (20) + 4 * J, inside the [20, 400] window.  The
    // tracker derives the delay from the *unrounded* jitter estimate, so
    // the two differ by at most one rounding step per 4·J term.
    const auto expected = 20 + 4 * stats.current_jitter.count();
    EXPECT_NEAR(static_cast<double>(stats.current_delay.count()),
                static_cast<double>(expected), 4.0);
    EXPECT_GT(stats.current_delay.count(), 20);
}

TEST(VideoJbAdaptive, MaxDelayUpperBoundHolds) {
    Config cfg = adaptive_config();
    cfg.max_delay = core::Milliseconds{30};
    auto jb = create(cfg);

    constexpr std::int64_t kMediaStepUs = 33333;
    for (int i = 0; i < 80; ++i) {
        const std::int64_t media_us = static_cast<std::int64_t>(i) * kMediaStepUs;
        const std::int64_t arrival_us =
            media_us + ((i % 2 == 0) ? 0 : 20 * 1000);
        insert_frame(*jb, i, arrival_us);
        auto f = jb->pop_frame();
        ASSERT_NE(f, nullptr);
    }

    // ~100 ms of raw target delay must clamp to the 30 ms window top.
    EXPECT_EQ(jb->stats().current_delay, core::Milliseconds{30});
}

TEST(VideoJbAdaptive, MinDelayLowerBoundHolds) {
    Config cfg;
    cfg.adaptive = true;
    cfg.base_delay = core::Milliseconds{5};
    cfg.min_delay = core::Milliseconds{50};
    cfg.max_delay = core::Milliseconds{400};
    auto jb = create(cfg);

    // Steady arrivals: zero jitter, raw target = base 5 ms -> min 50 ms.
    constexpr std::int64_t kMediaStepUs = 33333;
    for (int i = 0; i < 10; ++i) {
        insert_frame(*jb, i,
                     static_cast<std::int64_t>(i) * kMediaStepUs);
        auto f = jb->pop_frame();
        ASSERT_NE(f, nullptr);
    }
    EXPECT_EQ(jb->stats().current_delay, core::Milliseconds{50});
}

// -----------------------------------------------------------------------------
// Regression: the in-flight cap and late retransmissions
// -----------------------------------------------------------------------------

TEST(VideoJb, LateRetransmissionIsNotDeliveredTwice) {
    // A frame that was already handed out must not be delivered again
    // when a retransmission of one of its packets arrives: the late
    // packet has to land in the (still buffered) bucket of that frame
    // and be discarded there.
    auto jb = create({});

    std::uint8_t nalu[] = {0x65, 0xAA};
    jb->insert(make_packet(100, 1000, true, {nalu, sizeof(nalu)}, 0));

    auto first = jb->pop_frame();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->rtp_timestamp, 1000u);

    // NACK retransmission of the same packet (same seq, same RTP ts).
    jb->insert(make_packet(100, 1000, true, {nalu, sizeof(nalu)}, 1000));

    // The retransmission must not produce a second frame.
    EXPECT_EQ(jb->pop_frame(), nullptr);
    EXPECT_EQ(jb->stats().frames_emitted, 1u);
}

TEST(VideoJb, InflightCapDoesNotEvictTheNewlyInsertedFrame) {
    // The in-flight cap must be enforced *before* the new frame is
    // inserted — otherwise the frame that was just inserted is the one
    // the cap evicts, and the frame is lost on arrival.
    Config cfg;
    cfg.max_inflight_frames = 2;
    auto jb = create(cfg);

    std::uint8_t nalu[] = {0x41, 0xAA};        // P-slice, no keyframe
    // Two frames that never complete (no marker bit) fill the window.
    jb->insert(make_packet(100, 1000, false, {nalu, sizeof(nalu)}, 0));
    jb->insert(make_packet(101, 2000, false, {nalu, sizeof(nalu)}, 1000));
    // The third frame is complete and must survive the cap.
    jb->insert(make_packet(102, 3000, true, {nalu, sizeof(nalu)}, 2000));

    auto f = jb->pop_frame();
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->rtp_timestamp, 3000u);
    EXPECT_TRUE(f->complete);
}
