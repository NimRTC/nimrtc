/**
 * @file tests/test_engine_inbound_rtp_stats.cpp
 * @brief v1.2.0 (plan Track S / S2) — the per-source inbound RTP
 *        statistics entries.
 *
 * ## What this pins down
 *
 * `Engine::Stats` has a *flat* set of inbound counters
 * (`video_frames_received`, `audio_frames_received`,
 * `video_bytes_received`, …) that describe **every** inbound stream
 * mixed together.  That is the right shape for one peer stream and
 * the wrong one for anything else:
 *
 *   * a peer that sends **simulcast** (RFC 5956 — several encodings of
 *     the same picture on different SSRCs) puts three different
 *     qualities into one counter, so the host cannot tell which
 *     encoding it is actually receiving;
 *   * a session that receives from more than one source has the same
 *     problem.
 *
 * S2 adds the missing receive-side counterpart of `VideoStreamStats`:
 * `Stats::inbound_rtp_streams`, one entry per **inbound SSRC**.
 *
 * ## The properties under test
 *
 *   1. **A fresh engine has no entries.**  An empty list is the
 *      correct "nothing arrived yet" answer — a list with a zeroed
 *      entry would be a lie about a source that does not exist.
 *
 *   2. **A packet is counted against the SSRC it arrived on**, and the
 *      entries are ordered by SSRC so a host that diffs two readings
 *      never sees them swap places.
 *
 *   3. **Two sources on the same payload type stay separate.**  This
 *      is the simulcast case (RFC 5956): the whole point of the
 *      per-SSRC shape is that two SSRCs do not share a counter.
 *
 *   4. **The packet and byte counts match what went in.**  The byte
 *      figure is the *whole* packet on the wire (header + payload),
 *      because that is what the host's bandwidth arithmetic needs.
 *
 *   5. **A decoded frame is counted against its source.**  The frame
 *      count is the one that says "how much media did I actually
 *      receive on this source", so it must not be lumped in with the
 *      other sources.
 *
 *   6. **A packet the engine drops is still counted.**  A packet on an
 *      unknown payload type is dropped *before* it reaches a pipeline,
 *      but the peer did send it: a host investigating a loss must see
 *      it.
 *
 * The engine is driven through `feed_media_inbound()` — a documented
 * test seam.  No ICE / DTLS handshake is needed, and the packets
 * travel in cleartext, which is exactly what makes the wire format
 * assertable.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/rtp/packet.hpp>

namespace {

namespace np = nimrtc::plugins;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::InboundRtpStats;
using nimrtc::engine::NimRTCEngine;

using namespace std::chrono_literals;

/** The payload type RFC 7587 assigns to Opus — the engine's own
 *  default inbound audio payload type. */
constexpr std::uint8_t kOpusPt = 111;

/** The SSRC the fake peer uses for its audio stream. */
constexpr std::uint32_t kPeerAudioSsrc = 0x0A0A0A0Au;

/** A second audio SSRC — the simulcast case (RFC 5956): the same
 *  payload type, two sources. */
constexpr std::uint32_t kPeerAudioSsrc2 = 0x0B0B0B0Bu;

/** The SSRC the fake peer uses for its video stream. */
constexpr std::uint32_t kPeerVideoSsrc = 0x11223344u;

/** The engine's default video payload type (`VideoReceiverTuning`). */
constexpr std::uint8_t kVideoPt = 102;

// ---------------------------------------------------------------------------
// Packet helpers
// ---------------------------------------------------------------------------

/** Build a minimal RTP packet (12-byte header + payload).
 *
 *  The header is written by hand rather than through the `rtp::`
 *  builder so the test does not depend on the RTP module's own
 *  encoding choices — the point here is the *engine's* accounting,
 *  not the wire format. */
std::vector<std::uint8_t> make_rtp(std::uint32_t ssrc,
                                    std::uint16_t seq,
                                    std::uint32_t timestamp,
                                    std::uint8_t pt,
                                    std::size_t payload_bytes,
                                    bool marker = true) {
    std::vector<std::uint8_t> out;
    out.reserve(12 + payload_bytes);
    out.push_back(0x80);
    out.push_back(static_cast<std::uint8_t>((pt & 0x7F) | (marker ? 0x80 : 0x00)));
    out.push_back(static_cast<std::uint8_t>((seq >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(seq & 0xFF));
    out.push_back(static_cast<std::uint8_t>((timestamp >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((timestamp >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((timestamp >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(timestamp & 0xFF));
    out.push_back(static_cast<std::uint8_t>((ssrc >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((ssrc >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((ssrc >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(ssrc & 0xFF));
    out.insert(out.end(), payload_bytes, 0x00);
    return out;
}

/** Find the entry for @p ssrc in a published list. */
const InboundRtpStats* find_entry(const std::vector<InboundRtpStats>& list,
                                   std::uint32_t ssrc) {
    for (const auto& e : list) {
        if (e.ssrc == ssrc) return &e;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class EngineInboundRtpStats : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
    }

    EngineConfig make_config() {
        EngineConfig cfg{};
        // The default plugins are enough: the "reference" video
        // receiver / sender and the built-in Opus codec drive the whole
        // pipeline.
        cfg.video_receiver_name = "reference";
        cfg.video_sender_name   = "reference";
        cfg.video_sink_name     = "headless";
        // No DataChannel / BWE / scheduler noise in this test.
        cfg.datachannel_backend   = "";
        cfg.bwe_name              = "";
        cfg.scheduler_config.enabled = false;
        return cfg;
    }

    /** Encode one 20 ms Opus frame and wrap it into a real RTP packet.
     *
     *  Returns an empty buffer when the Opus codec plugin is not
     *  registered (a build without the Opus module). */
    static nimrtc::core::ByteBuffer make_opus_rtp(std::uint32_t ssrc,
                                                   std::uint8_t pt,
                                                   std::uint16_t seq,
                                                   std::uint32_t rtp_ts) {
        const auto* factory =
            nimrtc::core::PluginRegistry::instance().get_codec("opus");
        if (!factory) return {};

        nimrtc::plugins::CodecConfig cc{};
        cc.sample_rate_hz = 48000;
        cc.channels       = 1;
        cc.bitrate_bps    = 64000;
        cc.complexity     = 10;
        cc.fec_enabled    = true;
        cc.dtx_enabled    = false;
        cc.vad_enabled    = false;
        cc.payload_type   = pt;
        cc.name           = "opus";

        std::unique_ptr<nimrtc::plugins::ICodec> enc(factory->create(cc));
        if (!enc) return {};
        if (enc->open() != nimrtc::plugins::kOk) return {};

        std::vector<float> pcm(960, 0.25f);
        std::vector<std::uint8_t> bytes(4096);
        const std::size_t n = enc->encode(pcm.data(), 960,
                                          bytes.data(), bytes.size());
        if (n == 0) return {};

        nimrtc::rtp::PacketBuilder pb;
        pb.set_ssrc(ssrc)
          .set_seq(seq)
          .set_timestamp(rtp_ts)
          .set_payload_type(pt);
        pb.set_payload(nimrtc::core::ByteSpan(bytes.data(), n));
        return pb.build();
    }

    /** Feed one packet into the engine's inbound path. */
    static void feed(NimRTCEngine& engine,
                     const std::vector<std::uint8_t>& pkt) {
        engine.feed_media_inbound(pkt.data(), pkt.size());
    }

    /** Let the audio receive pipeline settle.
     *
     *  The jitter buffer plays a frame out `target_delay` after its
     *  packet arrived, and the playout only happens on a tick.  A
     *  frame that is already buffered is therefore delivered *after*
     *  the last packet was fed. */
    static void settle_audio(NimRTCEngine& engine,
                             std::uint32_t quiet_rounds = 4) {
        std::uint64_t last = engine.stats().audio_frames_received;
        std::uint32_t quiet = 0;
        for (std::uint32_t i = 0; i < 100 && quiet < quiet_rounds; ++i) {
            engine.tick();
            std::this_thread::sleep_for(20ms);
            const std::uint64_t now = engine.stats().audio_frames_received;
            if (now == last) {
                ++quiet;
            } else {
                quiet = 0;
                last  = now;
            }
        }
    }
};

// ============================================================================
// 1. The baseline
// ============================================================================

TEST_F(EngineInboundRtpStats, AFreshEngineReportsNoInboundSources) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto s = engine.stats();
    EXPECT_TRUE(s.inbound_rtp_streams.empty())
        << "a fresh engine has received nothing, so there is no source to "
           "report — an entry with zeroed counters would be a lie about a "
           "source that does not exist";

    engine.close();
}

// ============================================================================
// 2. One source, one entry
// ============================================================================

TEST_F(EngineInboundRtpStats, AnInboundPacketCreatesAnEntryForItsSsrc) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The Opus payload type is the engine's own default inbound audio
    // payload type, so the packet is routed to the audio pipeline
    // without an SDP exchange.
    const auto pkt = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 1, 0);
    if (pkt.empty()) {
        GTEST_SKIP() << "the Opus codec plugin is not registered in this build";
    }
    const std::size_t wire_bytes = pkt.size();
    feed(engine, std::vector<std::uint8_t>(pkt.begin(), pkt.end()));

    const auto s = engine.stats();
    ASSERT_EQ(s.inbound_rtp_streams.size(), 1u)
        << "one source sent packets, so there is exactly one entry";

    const InboundRtpStats& e = s.inbound_rtp_streams.front();
    EXPECT_EQ(e.ssrc, kPeerAudioSsrc);
    EXPECT_EQ(e.payload_type, kOpusPt)
        << "the entry must report the payload type the source actually "
           "used on the wire";
    EXPECT_EQ(e.kind, "audio")
        << "the engine routes by payload type, so it knows which plane the "
           "source belongs to";
    EXPECT_EQ(e.packets_received, 1u);
    EXPECT_EQ(e.bytes_received, wire_bytes)
        << "the byte figure is the whole packet on the wire (header + "
           "payload) — that is what the host's bandwidth arithmetic needs";

    engine.close();
}

// ============================================================================
// 3. Two sources on the same payload type (simulcast, RFC 5956)
// ============================================================================

TEST_F(EngineInboundRtpStats, TwoSourcesOnTheSamePayloadTypeStaySeparate) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto first = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 1, 0);
    if (first.empty()) {
        GTEST_SKIP() << "the Opus codec plugin is not registered in this build";
    }
    const auto second = make_opus_rtp(kPeerAudioSsrc2, kOpusPt, 1, 0);
    ASSERT_FALSE(second.empty());

    feed(engine, std::vector<std::uint8_t>(first.begin(), first.end()));
    feed(engine, std::vector<std::uint8_t>(second.begin(), second.end()));

    const auto s = engine.stats();
    ASSERT_EQ(s.inbound_rtp_streams.size(), 2u)
        << "two SSRCs on the same payload type are two sources, not one";

    const InboundRtpStats* a = find_entry(s.inbound_rtp_streams, kPeerAudioSsrc);
    const InboundRtpStats* b = find_entry(s.inbound_rtp_streams, kPeerAudioSsrc2);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->packets_received, 1u);
    EXPECT_EQ(b->packets_received, 1u);
    EXPECT_EQ(a->bytes_received, first.size());
    EXPECT_EQ(b->bytes_received, second.size());

    // The list is ordered by SSRC, so a host that diffs two readings
    // never sees the entries swap places.
    EXPECT_LT(s.inbound_rtp_streams[0].ssrc, s.inbound_rtp_streams[1].ssrc)
        << "the published list must be ordered by SSRC (a host that diffs "
           "two readings must not see the entries move)";

    engine.close();
}

// ============================================================================
// 4. The counts accumulate
// ============================================================================

TEST_F(EngineInboundRtpStats, TheCountersAccumulateOverTheSourceLifetime) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    std::uint64_t expected_bytes = 0;
    for (std::uint16_t seq = 1; seq <= 5; ++seq) {
        const auto pkt = make_opus_rtp(kPeerAudioSsrc, kOpusPt, seq,
                                       static_cast<std::uint32_t>(seq - 1) * 960);
        if (pkt.empty()) {
            GTEST_SKIP() << "the Opus codec plugin is not registered in this build";
        }
        expected_bytes += pkt.size();
        feed(engine, std::vector<std::uint8_t>(pkt.begin(), pkt.end()));
    }

    const auto s = engine.stats();
    ASSERT_EQ(s.inbound_rtp_streams.size(), 1u);
    const InboundRtpStats& e = s.inbound_rtp_streams.front();
    EXPECT_EQ(e.packets_received, 5u);
    EXPECT_EQ(e.bytes_received, expected_bytes);

    engine.close();
}

// ============================================================================
// 5. The frame count
// ============================================================================

TEST_F(EngineInboundRtpStats, ADecodedFrameIsCountedAgainstItsSource) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto first = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 1, 0);
    if (first.empty()) {
        GTEST_SKIP() << "the Opus codec plugin is not registered in this build";
    }
    feed(engine, std::vector<std::uint8_t>(first.begin(), first.end()));

    std::this_thread::sleep_for(120ms);

    const auto second = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 2, 960);
    ASSERT_FALSE(second.empty());
    feed(engine, std::vector<std::uint8_t>(second.begin(), second.end()));

    settle_audio(engine);

    const auto s = engine.stats();
    ASSERT_EQ(s.inbound_rtp_streams.size(), 1u);
    const InboundRtpStats& e = s.inbound_rtp_streams.front();

    EXPECT_GE(e.frames_received, 1u)
        << "a decoded frame must be counted against the source it came "
           "from — this is the figure that says how much media actually "
           "arrived";
    EXPECT_LE(e.frames_received, e.packets_received)
        << "a source cannot have produced more frames than it received "
           "packets";

    engine.close();
}

// ============================================================================
// 6. A dropped packet is still counted
// ============================================================================

TEST_F(EngineInboundRtpStats, APacketOnAnUnknownPayloadTypeIsStillCounted) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // Payload type 99 is in no routing table: the engine drops the
    // packet — but the peer did send it, so a host investigating a
    // loss must see it in the source's counters.
    constexpr std::uint8_t kUnknownPt = 99;
    const auto pkt = make_rtp(kPeerAudioSsrc, 1, 0, kUnknownPt, 40);
    feed(engine, pkt);

    const auto s = engine.stats();
    ASSERT_EQ(s.inbound_rtp_streams.size(), 1u)
        << "the packet was dropped, but it arrived on a source, so the "
           "source must appear in the list";
    EXPECT_EQ(s.inbound_rtp_streams.front().packets_received, 1u);
    EXPECT_GT(s.inbound_rtp_streams.front().bytes_received, 0u);

    engine.close();
}

// ============================================================================
// 7. A video source is told apart from an audio source
// ============================================================================

TEST_F(EngineInboundRtpStats, AVideoSourceIsReportedAsVideo) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    // The engine's default inbound video payload type.  A packet on
    // it is routed to the *video* pipeline, so the entry must say so:
    // the kind is what lets a host tell the two media planes apart
    // without consulting the SSRC table itself.
    const auto pkt = make_rtp(kPeerVideoSsrc, 1, 0, kVideoPt, 40);
    feed(engine, pkt);

    const auto s = engine.stats();
    ASSERT_EQ(s.inbound_rtp_streams.size(), 1u);

    const InboundRtpStats& e = s.inbound_rtp_streams.front();
    EXPECT_EQ(e.ssrc, kPeerVideoSsrc);
    EXPECT_EQ(e.payload_type, kVideoPt);
    EXPECT_EQ(e.kind, "video")
        << "the engine routes by payload type, so it knows which plane the "
           "source belongs to";
    EXPECT_EQ(e.packets_received, 1u);

    engine.close();
}

// ============================================================================
// 8. The list survives the source going away
// ============================================================================

TEST_F(EngineInboundRtpStats, TheEntryKeepsItsTotalsAfterTheSourceStops) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto pkt = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 1, 0);
    if (pkt.empty()) {
        GTEST_SKIP() << "the Opus codec plugin is not registered in this build";
    }
    feed(engine, std::vector<std::uint8_t>(pkt.begin(), pkt.end()));

    const auto before = engine.stats();
    ASSERT_EQ(before.inbound_rtp_streams.size(), 1u);

    // No more packets arrive: the source is gone, but its totals must
    // still be readable — a host that reads the list after the peer
    // stopped still sees what that source produced.
    const auto after = engine.stats();
    ASSERT_EQ(after.inbound_rtp_streams.size(), 1u);
    EXPECT_EQ(after.inbound_rtp_streams.front().packets_received,
              before.inbound_rtp_streams.front().packets_received);
    EXPECT_EQ(after.inbound_rtp_streams.front().bytes_received,
              before.inbound_rtp_streams.front().bytes_received);

    engine.close();
}

} // namespace
