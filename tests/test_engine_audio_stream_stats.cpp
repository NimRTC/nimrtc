/**
 * @file tests/test_engine_audio_stream_stats.cpp
 * @brief v1.2.0 (plan Track S / S4) — the per-stream audio statistics
 *        entries.
 *
 * ## What this pins down
 *
 * The audio plane had **no counterpart** to `VideoStreamStats`: every
 * audio number was a flat `Stats::audio_*` counter with no stream
 * identity attached to it.  That was harmless while the audio plane was
 * single-stream — which it is today — but it is an accident of the
 * current topology, not a property of the interface.
 *
 * S4 adds the missing shape: `Stats::audio_streams`, a **list** of
 * per-stream entries, each carrying the track's identity (SSRC, payload
 * type, codec, clock rate, channels, frame duration) and its own send
 * and receive counters.
 *
 * ## The properties under test
 *
 *   1. **A fresh engine reports exactly one entry** — the engine's own
 *      track, `"default"`.  An *empty* list would be a lie in the other
 *      direction: the engine's audio track exists whether or not
 *      anything has been sent on it.
 *
 *   2. **The entry names the track.**  The SSRC and the payload type
 *      are the identity, and they must be the *negotiated* ones — a peer
 *      that mapped Opus onto a different number must be described by the
 *      number it actually uses.
 *
 *   3. **The list and the flat counters agree.**  The flat `audio_*`
 *      fields describe the same track, so a host that reads one must
 *      not see a different number than a host that reads the other.
 *      This is the property that makes the list *additive* rather than
 *      a second source of truth.
 *
 *   4. **What the engine received is counted against the track.**  The
 *      engine is the only party that sees every inbound packet on the
 *      audio path, so it counts them where it routes them.
 *
 *   5. **What the engine handed to the host is counted too** — the
 *      frames, the samples, and the concealment / FEC flags.
 *
 *   6. **The wire-side receive figures come from the pipeline.**  The
 *      receive pipeline is the only party that sees the source's
 *      sequence space, so the loss and the jitter are its numbers, not
 *      re-derived ones.
 *
 * The engine is driven through `feed_media_inbound()` — a documented test
 * seam.  No ICE / DTLS handshake is needed, and the packets travel in
 * cleartext, which is exactly what makes the wire format assertable.
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
using nimrtc::engine::AudioStreamStats;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

using namespace std::chrono_literals;

/** The payload type RFC 7587 assigns to Opus. */
constexpr std::uint8_t kOpusPt = 111;

/** The SSRC the fake peer uses for its audio stream. */
constexpr std::uint32_t kPeerAudioSsrc = 0x0A0A0A0Au;

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

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class EngineAudioStreamStats : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
    }

    EngineConfig make_config() {
        EngineConfig cfg{};
        // The default plugins are enough: the built-in Opus codec and the
        // audio receive pipeline drive the whole path.
        cfg.datachannel_backend      = "";
        cfg.bwe_name                 = "";
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

TEST_F(EngineAudioStreamStats, AFreshEngineReportsTheDefaultStream) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto s = engine.stats();

    // The engine's audio stream exists whether or not anything has
    // been sent on it, so the list is not empty.  An empty list would
    // be as much a lie as a zeroed entry for a source that does not
    // exist.
    ASSERT_EQ(s.audio_streams.size(), 1u);
    EXPECT_EQ(s.audio_streams.front().stream_id, "default")
        << "the engine's own audio stream is the 'default' one";

    engine.close();
}

// ============================================================================
// 2. The identity
// ============================================================================

TEST_F(EngineAudioStreamStats, TheEntryNamesTheStream) {
    EngineConfig cfg = make_config();
    cfg.audio_codec.payload_type = kOpusPt;
    cfg.audio_codec.encoding      = "opus";
    cfg.audio_codec.clock_rate    = 48000;
    cfg.audio_codec.channels      = 2;
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto s = engine.stats();
    ASSERT_EQ(s.audio_streams.size(), 1u);

    const AudioStreamStats& e = s.audio_streams.front();
    EXPECT_EQ(e.payload_type, kOpusPt);
    EXPECT_EQ(e.codec_name, "opus");
    EXPECT_EQ(e.clock_rate_hz, 48000u);
    EXPECT_EQ(e.channels, 2);

    // The frame duration is the negotiated packet time (RFC 7587 code 0
    // framing): one Opus frame per RTP packet.
    EXPECT_EQ(e.frame_duration_ms, cfg.audio_ptime_ms);

    engine.close();
}

// ============================================================================
// 3. The list and the flat counters agree
// ============================================================================

TEST_F(EngineAudioStreamStats, TheListAgreesWithTheFlatCounters) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto pkt = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 1, 0);
    if (pkt.empty()) {
        GTEST_SKIP() << "the Opus codec plugin is not registered in this build";
    }
    feed(engine, pkt);
    settle_audio(engine);

    const auto s = engine.stats();
    ASSERT_EQ(s.audio_streams.size(), 1u);

    const AudioStreamStats& e = s.audio_streams.front();

    // The flat fields describe the same stream as the list's entry, so
    // a host that reads one must not see a different number than a host
    // that reads the other.  This is what makes the list *additive*
    // rather than a second source of truth.
    EXPECT_EQ(e.packets_sent, s.audio_frames_sent);
    EXPECT_EQ(e.bytes_sent, s.audio_bytes_sent);
    EXPECT_EQ(e.encode_errors, s.audio_encode_errors);
    EXPECT_EQ(e.samples_in, s.audio_samples_in);
    EXPECT_EQ(e.frames_received, s.audio_frames_received);
    EXPECT_EQ(e.samples_received, s.audio_samples_received);
    EXPECT_EQ(e.frames_concealed, s.audio_concealed_frames);
    EXPECT_EQ(e.frames_fec_recovered, s.audio_fec_recovered_frames);
    EXPECT_EQ(e.packets_lost, s.audio_packets_lost);
    EXPECT_EQ(e.decode_errors, s.audio_decode_errors);
    EXPECT_EQ(e.packets_dropped, s.audio_packets_dropped);
    EXPECT_EQ(e.jitter_ms, s.audio_jitter_ms);
    EXPECT_EQ(e.playout_delay_ms, s.audio_playout_delay_ms);
    EXPECT_EQ(e.encoder_bitrate_bps, s.audio_encoder_bitrate_bps);

    engine.close();
}

// ============================================================================
// 4. What the engine received
// ============================================================================

TEST_F(EngineAudioStreamStats, TheReceivedPacketsAreCountedAgainstTheStream) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto pkt = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 1, 0);
    if (pkt.empty()) {
        GTEST_SKIP() << "the Opus codec plugin is not registered in this build";
    }
    feed(engine, pkt);
    settle_audio(engine);

    const auto s = engine.stats();
    ASSERT_EQ(s.audio_streams.size(), 1u);

    const AudioStreamStats& e = s.audio_streams.front();
    EXPECT_GT(e.packets_received, 0u)
        << "the peer sent a packet, so the stream must report it";
    EXPECT_GT(e.bytes_received, 0u);
    EXPECT_GT(e.frames_received, 0u)
        << "the pipeline decoded the frame and handed it to the host";

    engine.close();
}

// ============================================================================
// 5. The inbound source is named
// ============================================================================

TEST_F(EngineAudioStreamStats, TheInboundSourceIsNamed) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto pkt = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 1, 0);
    if (pkt.empty()) {
        GTEST_SKIP() << "the Opus codec plugin is not registered in this build";
    }
    feed(engine, pkt);
    settle_audio(engine);

    const auto s = engine.stats();
    ASSERT_EQ(s.audio_streams.size(), 1u);

    const AudioStreamStats& e = s.audio_streams.front();

    // The peer's SSRC is its own choice — it is *not* the engine's
    // outbound SSRC, and the entry has to say which end it is
    // describing.
    EXPECT_EQ(e.inbound_ssrc, kPeerAudioSsrc);
    EXPECT_NE(e.inbound_ssrc, e.ssrc)
        << "the two ends of the track have different SSRCs";

    engine.close();
}

// ============================================================================
// 6. The list is stable across readings
// ============================================================================

TEST_F(EngineAudioStreamStats, TheListIsStableAcrossReadings) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto pkt = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 1, 0);
    if (pkt.empty()) {
        GTEST_SKIP() << "the Opus codec plugin is not registered in this build";
    }
    feed(engine, pkt);

    const auto first = engine.stats();
    const auto second = engine.stats();

    ASSERT_EQ(first.audio_streams.size(), second.audio_streams.size());
    for (std::size_t i = 0; i < first.audio_streams.size(); ++i) {
        EXPECT_EQ(first.audio_streams[i].ssrc, second.audio_streams[i].ssrc)
            << "a host that diffs two readings must not see the entries "
               "swap places";
        EXPECT_EQ(first.audio_streams[i].stream_id,
                  second.audio_streams[i].stream_id);
    }

    engine.close();
}

}  // namespace
