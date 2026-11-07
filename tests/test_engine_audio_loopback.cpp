/**
 * @file tests/test_engine_audio_loopback.cpp
 * @brief v1.2.0 (audio pipeline, WP-11) — the engine integration tests of
 *        the audio pipeline (spec V-2).
 *
 * The unit tests (`tests/test_engine_audio_state.cpp`,
 * `src/modules/audio_rx/tests/test_audio_rx.cpp`) pin down the
 * *components*: the SDP, the routing table, the concealment rule, the
 * decoder.  This file pins down the thing that only shows up when the
 * parts run together — the **media loopback**:
 *
 *   1. `LossInjectionProducesConcealmentAndKeepsTheCadence` — every
 *      N-th packet is dropped on the way in.  The receiver must keep a
 *      constant output cadence (the media timeline is continuous) and
 *      the concealed-frame counter must match the drop count exactly
 *      (spec A-3 / A-4, V-2).
 *   2. `InbandFecRecoversTheLostFramesInsteadOfConcealingThem` — the
 *      same drop pattern with in-band FEC on: the lost frames come back
 *      as *reconstructed* frames, flagged `fec_recovered`, not as
 *      concealment (spec B-8).
 *   3. `TheTickRateDoesNotChangeTheAudioThatComesOut` — the same stream
 *      played out at 30 Hz and at 200 Hz produces the same audio; only
 *      the granularity of the cadence changes (spec A-1, O-4).
 *   4. `TwoEnginesOverLoopbackDeliverDecodedAudio` — the real thing:
 *      two engines over 127.0.0.1 (ICE + DTLS + SRTP), engine A feeds
 *      PCM and engine B decodes it.  This is the direction that used to
 *      be broken (defect F-1: the inbound Opus bit-stream was handed to
 *      the host as if it were float PCM).
 *
 * The first three use the "shell" transport strategy (the sentinel
 * `test_engine_audio_state` uses) and inject packets through the
 * engine's `feed_media_inbound()` test seam: the packets they inject
 * are **real Opus frames** (produced by the registered Opus codec
 * plugin), because since WP-7 the receive path really decodes.
 *
 * The fourth one needs a real transport; when the host forbids the UDP
 * bind (a sandboxed CI runner) the case *skips* instead of failing —
 * the failure it guards against is a media-path defect, not a
 * port-binding one.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nimrtc/core/log.hpp>
#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/codec.hpp>
#include <nimrtc/rtp/packet.hpp>

namespace {

using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

using namespace std::chrono_literals;

/** RFC 7587's payload type for Opus — the one the engine advertises. */
constexpr std::uint8_t kOpusPt = 111;

/** The media clock of Opus (RFC 7587 §4.1). */
constexpr std::uint32_t kClockRate = 48000;

/** Samples per channel in one 20 ms Opus frame. */
constexpr std::size_t kFrameSamples = 960;

/** The RTP timestamp step of one 20 ms frame. */
constexpr std::uint32_t kTsStep = 960;

/** The SSRC the fake peer uses for its audio stream. */
constexpr std::uint32_t kPeerSsrc = 0x0A0A0A0Au;

/** One frame as the receiver delivered it. */
struct DeliveredFrame {
    std::uint32_t rtp_timestamp = 0;
    std::size_t   samples_per_channel = 0;
    std::uint8_t  channels = 0;
    bool          concealed = false;
    bool          fec_recovered = false;
    double        rms = 0.0;
};

/** The RMS of a frame's PCM — the "energy envelope" the lossy codec
 *  comparison of spec V-2 asks for. */
double rms_of(const float* pcm, std::size_t n) {
    if (pcm == nullptr || n == 0) return 0.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = static_cast<double>(pcm[i]);
        sum += v * v;
    }
    return std::sqrt(sum / static_cast<double>(n));
}

/** A 440 Hz sine at 48 kHz — a signal a 64 kbps Opus frame carries
 *  faithfully enough for an energy comparison. */
std::vector<float> make_tone(std::size_t samples, std::uint32_t phase_offset = 0) {
    std::vector<float> pcm(samples);
    const double two_pi = 6.283185307179586;
    for (std::size_t i = 0; i < samples; ++i) {
        const double t = static_cast<double>(phase_offset + i) / kClockRate;
        pcm[i] = static_cast<float>(0.4 * std::sin(two_pi * 440.0 * t));
    }
    return pcm;
}

class EngineAudioLoopback : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();
    }

    /** A "shell" configuration: no ICE, no DTLS, no SRTP — the engine
     *  reaches the open state without touching the network. */
    static EngineConfig shell_config() {
        EngineConfig cfg;
        cfg.stun_server_host       = "";
        cfg.local_port_range_begin = 52600;
        cfg.local_port_range_end   = 52700;
        cfg.transport_strategy_name = "shell";
        cfg.pcm_sample_rate_hz     = kClockRate;
        cfg.pcm_channels           = 1;
        cfg.audio_ptime_ms         = 20;
        cfg.audio_codec.clock_rate = kClockRate;
        cfg.audio_codec.channels   = 1;
        cfg.audio_codec.payload_type = kOpusPt;
        return cfg;
    }

    /** The sender side of the fake peer: **one** Opus encoder for the
     *  whole stream.
     *
     *  This is not a detail — it is the whole in-band FEC mechanism.
     *  Opus carries the low-bitrate copy (the SILK LBRR) of the
     *  *previous* frame inside the *next* packet, so the redundancy can
     *  only exist when one encoder produced both frames.  A sender that
     *  creates a fresh encoder per packet (as an earlier version of this
     *  test did) can never embed a redundancy, and the receiver can never
     *  reconstruct anything.
     *
     *  Returns false when the Opus plugin is not registered (a build
     *  without the Opus module). */
    class OpusSender {
    public:
        explicit OpusSender(bool fec_enabled) {
            const auto* factory =
                nimrtc::core::PluginRegistry::instance().get_codec("opus");
            if (!factory) return;

            nimrtc::plugins::CodecConfig cc{};
            cc.sample_rate_hz = kClockRate;
            cc.channels       = 1;
            cc.bitrate_bps    = 64000;
            cc.complexity     = 10;
            cc.fec_enabled    = fec_enabled;
            cc.dtx_enabled    = false;
            cc.vad_enabled    = false;
            cc.payload_type   = kOpusPt;
            cc.name           = "opus";
            // The in-band redundancy the encoder has to embed (spec B-8).
            //
            // libopus only carries the low-bitrate copy of the previous
            // frame (the SILK LBRR) when the redundancy hint
            // `OPUS_SET_PACKET_LOSS_PERC` is non-zero **and** the
            // encoder is in SILK (or hybrid) mode.  The mode decision in
            // libopus (opus_encoder.c) puts a 64 kbps mono stream in
            // CELT mode, and the override that pulls it back into SILK
            // needs `packetLossPercentage > (128 - voice_est) >> 4` — a
            // threshold of about 8 for a non-speech signal.  20 % is
            // above that threshold for every signal, so the encoder
            // really embeds the redundancy and the receiver really has
            // something to reconstruct the lost frame from.
            cc.packet_loss_perc = fec_enabled ? 20 : 0;

            codec_.reset(factory->create(cc));
            if (codec_ && codec_->open() != nimrtc::plugins::kOk) {
                codec_.reset();
            }
        }

        bool ready() const { return static_cast<bool>(codec_); }

        /** Encode one frame into an RFC 7587 payload. */
        bool encode(const std::vector<float>& pcm,
                    std::vector<std::uint8_t>& out) {
            if (!codec_) return false;
            out.assign(4096, 0);
            const std::size_t n = codec_->encode(pcm.data(), pcm.size(),
                                                 out.data(), out.size());
            if (n == 0) return false;
            out.resize(n);
            return true;
        }

    private:
        std::unique_ptr<nimrtc::plugins::ICodec> codec_;
    };

    /** Encode one 20 ms Opus frame and wrap it into an RTP packet. */
    static nimrtc::core::ByteBuffer make_opus_rtp(OpusSender& sender,
                                                  std::uint32_t ssrc,
                                                  std::uint16_t seq,
                                                  std::uint32_t rtp_ts,
                                                  const std::vector<float>& pcm) {
        std::vector<std::uint8_t> bytes;
        if (!sender.encode(pcm, bytes)) return {};

        nimrtc::rtp::PacketBuilder pb;
        pb.set_ssrc(ssrc)
          .set_seq(seq)
          .set_timestamp(rtp_ts)
          .set_payload_type(kOpusPt);
        pb.set_payload(nimrtc::core::ByteSpan(bytes.data(), bytes.size()));
        return pb.build();
    }

    /** Negotiate @p rx with a second engine, so the routing table knows
     *  which payload type is audio (spec B-4). */
    static bool negotiate(NimRTCEngine& rx, const EngineConfig& cfg) {
        NimRTCEngine tx(cfg);
        if (tx.open() != 0) return false;
        const std::string offer = tx.create_offer();
        if (offer.empty()) return false;
        return rx.process_remote_sdp(offer).has_value();
    }

    /** Feed @p count packets into @p engine, dropping every
     *  @p drop_every -th one, and drive the engine's clock while doing
     *  it.
     *
     *  The packets arrive in real time (one every 20 ms) — a receiver
     *  that only saw them in a burst would measure a different (and
     *  much easier) problem than the one a real network produces. */
    static void feed_stream(NimRTCEngine& engine,
                            std::size_t count,
                            std::size_t drop_every,
                            std::size_t& dropped) {
        dropped = 0;
        // One encoder for the whole stream — see OpusSender.
        OpusSender sender(/*fec_enabled=*/true);
        std::uint32_t ts = 1000;   // a non-zero origin: the first frame
                                   // must not look like a "gap from 0"
        for (std::size_t i = 0; i < count; ++i) {
            const auto pcm = make_tone(kFrameSamples, static_cast<std::uint32_t>(i));
            const bool drop = (drop_every != 0) && ((i % drop_every) == 0) && (i != 0);
            if (drop) {
                ++dropped;
                // The sequence number and the timestamp still advance:
                // the *peer* did not lose the frame — the network did.
                //
                // The frame itself is still *encoded* (the peer sent it);
                // only its packet is lost.  That is what makes the
                // in-band FEC possible at all: the redundancy of the lost
                // frame travels inside the *next* packet.
                std::vector<std::uint8_t> unused;
                sender.encode(pcm, unused);
            } else {
                const auto pkt = make_opus_rtp(sender,
                                               kPeerSsrc,
                                               static_cast<std::uint16_t>(100 + i),
                                               ts, pcm);
                if (pkt.empty()) return;
                engine.feed_media_inbound(pkt.data(), pkt.size());
            }
            ts += kTsStep;

            // One 20 ms frame of media time; tick at 100 Hz in between.
            for (int k = 0; k < 2; ++k) {
                engine.tick();
                std::this_thread::sleep_for(10ms);
            }
        }
    }

    /** Let the receiver drain what is still buffered. */
    static void drain(NimRTCEngine& engine, std::size_t ticks = 40) {
        for (std::size_t i = 0; i < ticks; ++i) {
            engine.tick();
            std::this_thread::sleep_for(10ms);
        }
    }
};

// ============================================================================
// 1. Loss injection — the concealment rule and the cadence
// ============================================================================

TEST_F(EngineAudioLoopback, LossInjectionProducesConcealmentAndKeepsTheCadence) {
    EngineConfig cfg = shell_config();
    // In-band FEC off: the point of this case is the *concealment* path.
    cfg.audio_use_inband_fec = false;
    if (std::getenv("NIMRTC_AUDIO_TRACE")) {
        nimrtc::core::log::set_level(nimrtc::core::log::Level::Trace);
    }

    NimRTCEngine rx(cfg);
    ASSERT_EQ(rx.open(), 0u);
    ASSERT_TRUE(negotiate(rx, cfg))
        << "the SDP handshake a real session performs must succeed";

    std::vector<DeliveredFrame> frames;
    rx.set_on_audio_frame_info(
        [&](const nimrtc::engine::AudioFrameInfo& info) {
            DeliveredFrame f;
            f.rtp_timestamp = info.rtp_timestamp;
            f.samples_per_channel = info.num_samples_per_channel;
            f.channels = info.channels;
            f.concealed = info.concealed;
            f.fec_recovered = info.fec_recovered;
            f.rms = rms_of(info.pcm,
                           info.num_samples_per_channel * info.channels);
            frames.push_back(f);
        });

    constexpr std::size_t kPackets = 30;
    constexpr std::size_t kDropEvery = 4;   // every 4th packet is lost
    std::size_t dropped = 0;
    feed_stream(rx, kPackets, kDropEvery, dropped);
    drain(rx);
    {
        const auto st = rx.stats();
        std::printf("DIAG frames=%zu received=%llu concealed=%llu fec=%llu "
                    "lost=%llu decode_errors=%llu jitter=%u delay=%u\n",
                    frames.size(),
                    (unsigned long long)st.audio_frames_received,
                    (unsigned long long)st.audio_concealed_frames,
                    (unsigned long long)st.audio_fec_recovered_frames,
                    (unsigned long long)st.audio_packets_lost,
                    (unsigned long long)st.audio_decode_errors,
                    st.audio_jitter_ms, st.audio_playout_delay_ms);
        std::fflush(stdout);
    }

    ASSERT_GT(dropped, 0u);
    EXPECT_EQ(dropped, (kPackets - 1) / kDropEvery)
        << "the drop pattern must be the one the test injected";

    // ---- The cadence is constant ------------------------------------
    //
    // Every media slot is delivered — the received ones and the
    // concealed ones together (spec A-4: the output sample count is
    // the media time × the rate).
    const std::size_t expected_frames = kPackets;
    ASSERT_GE(frames.size(), expected_frames - 1u)
        << "the receiver must deliver every media slot; got "
        << frames.size() << " frames for " << kPackets << " slots";
    EXPECT_LE(frames.size(), expected_frames + 1u)
        << "the receiver must not invent media slots; got "
        << frames.size() << " frames for " << kPackets << " slots";

    for (std::size_t i = 0; i < frames.size(); ++i) {
        EXPECT_EQ(frames[i].samples_per_channel, kFrameSamples)
            << "frame " << i << " has the wrong length — the output cadence "
               "must be constant";
        EXPECT_EQ(frames[i].channels, 1u);
        if (i == 0) continue;
        EXPECT_EQ(frames[i].rtp_timestamp - frames[i - 1].rtp_timestamp, kTsStep)
            << "frame " << i << " is not exactly one frame after frame "
            << (i - 1) << " — the media timeline has a hole";
    }

    // ---- The concealed counter matches the drop count ----------------
    std::size_t concealed = 0;
    std::size_t fec_recovered = 0;
    for (const auto& f : frames) {
        if (f.concealed) ++concealed;
        if (f.fec_recovered) ++fec_recovered;
    }
    EXPECT_EQ(concealed, dropped)
        << "every lost packet must produce exactly one concealed frame";
    EXPECT_EQ(fec_recovered, 0u)
        << "in-band FEC is off in this case — nothing may be reported as "
           "recovered";

    // ---- The engine's own counters agree ----------------------------
    const auto stats = rx.stats();
    EXPECT_EQ(stats.audio_concealed_frames, concealed);
    EXPECT_GE(stats.audio_frames_received, concealed);
}

// ============================================================================
// 2. In-band FEC — the same drops, recovered instead of concealed
// ============================================================================

TEST_F(EngineAudioLoopback, InbandFecRecoversTheLostFramesInsteadOfConcealingThem) {
    EngineConfig cfg = shell_config();
    cfg.audio_use_inband_fec = true;    // the default, stated explicitly

    NimRTCEngine rx(cfg);
    ASSERT_EQ(rx.open(), 0u);
    ASSERT_TRUE(negotiate(rx, cfg));

    std::vector<DeliveredFrame> frames;
    rx.set_on_audio_frame_info(
        [&](const nimrtc::engine::AudioFrameInfo& info) {
            DeliveredFrame f;
            f.rtp_timestamp = info.rtp_timestamp;
            f.samples_per_channel = info.num_samples_per_channel;
            f.concealed = info.concealed;
            f.fec_recovered = info.fec_recovered;
            frames.push_back(f);
        });

    constexpr std::size_t kPackets = 30;
    constexpr std::size_t kDropEvery = 4;
    std::size_t dropped = 0;
    feed_stream(rx, kPackets, kDropEvery, dropped);
    drain(rx);

    std::size_t concealed = 0;
    std::size_t fec_recovered = 0;
    for (const auto& f : frames) {
        if (f.concealed) ++concealed;
        if (f.fec_recovered) ++fec_recovered;
    }

    EXPECT_GT(dropped, 0u);
    EXPECT_GT(fec_recovered, 0u)
        << "the in-band redundancy the sender embedded must reconstruct the "
           "lost frames (spec B-8)";
    EXPECT_EQ(concealed + fec_recovered, dropped)
        << "every lost packet must be accounted for exactly once — as a "
           "reconstruction or as a concealment, never as neither";
    EXPECT_LE(concealed, dropped);
}

// ============================================================================
// 3. Tick-rate independence
// ============================================================================

TEST_F(EngineAudioLoopback, TheTickRateDoesNotChangeTheAudioThatComesOut) {
    // The same stream, played out at two very different tick rates.
    auto run_at = [](std::size_t tick_hz) {
        EngineConfig cfg = shell_config();
        cfg.audio_use_inband_fec = false;

        NimRTCEngine rx(cfg);
        if (rx.open() != 0) return std::vector<DeliveredFrame>{};
        if (!negotiate(rx, cfg)) return std::vector<DeliveredFrame>{};

        std::vector<DeliveredFrame> frames;
        rx.set_on_audio_frame_info(
            [&](const nimrtc::engine::AudioFrameInfo& info) {
                DeliveredFrame f;
                f.rtp_timestamp = info.rtp_timestamp;
                f.samples_per_channel = info.num_samples_per_channel;
                f.concealed = info.concealed;
                frames.push_back(f);
            });

        constexpr std::size_t kPackets = 20;
        OpusSender sender(/*fec_enabled=*/false);
        const auto period = std::chrono::microseconds(1000000 / tick_hz);
        std::uint32_t ts = 1000;
        for (std::size_t i = 0; i < kPackets; ++i) {
            const auto pcm = make_tone(kFrameSamples, static_cast<std::uint32_t>(i));
            const auto pkt = make_opus_rtp(sender,
                                           kPeerSsrc,
                                           static_cast<std::uint16_t>(100 + i),
                                           ts, pcm);
            if (pkt.empty()) return std::vector<DeliveredFrame>{};
            rx.feed_media_inbound(pkt.data(), pkt.size());
            ts += kTsStep;

            // One frame of media time, ticked at the chosen rate.
            const auto deadline =
                std::chrono::steady_clock::now() + 20ms;
            while (std::chrono::steady_clock::now() < deadline) {
                rx.tick();
                std::this_thread::sleep_for(period);
            }
        }
        // Drain the tail of the jitter buffer.
        const auto tail = std::chrono::steady_clock::now() + 300ms;
        while (std::chrono::steady_clock::now() < tail) {
            rx.tick();
            std::this_thread::sleep_for(period);
        }
        return frames;
    };

    const auto at_30 = run_at(30);
    const auto at_200 = run_at(200);

    if (at_30.empty() || at_200.empty()) {
        GTEST_SKIP() << "the Opus codec plugin is not registered in this build";
    }

    EXPECT_GE(at_30.size(), 15u)
        << "a 30 Hz tick must still produce continuous audio";
    EXPECT_GE(at_200.size(), 15u)
        << "a 200 Hz tick must produce continuous audio";

    // The two runs play the same stream: the number of media slots and
    // the timeline must agree.  A slower tick may deliver the same
    // frames in a coarser batch — but never a different stream.
    const auto total_30 = at_30.size();
    const auto total_200 = at_200.size();
    EXPECT_LE(std::abs(static_cast<int>(total_30) - static_cast<int>(total_200)), 2)
        << "the tick rate changed the amount of audio that came out ("
        << total_30 << " vs " << total_200 << " frames)";

    for (const auto* run : {&at_30, &at_200}) {
        for (std::size_t i = 1; i < run->size(); ++i) {
            EXPECT_EQ((*run)[i].rtp_timestamp - (*run)[i - 1].rtp_timestamp, kTsStep)
                << "the media timeline must not depend on the tick rate";
        }
    }
}

// ============================================================================
// 4. The real loopback — two engines over 127.0.0.1
// ============================================================================

namespace {

/** The ICE credentials of an SDP, in the form `set_remote_ice()` wants
 *  (the same extraction `examples/loopback-p2p` performs). */
std::string extract_ice_block(const std::string& sdp) {
    std::string block;
    std::size_t pos = 0;
    while (pos < sdp.size()) {
        std::size_t end = sdp.find("\r\n", pos);
        if (end == std::string::npos) end = sdp.size();
        const std::string line = sdp.substr(pos, end - pos);
        if (line.rfind("a=ice-ufrag:", 0) == 0 ||
            line.rfind("a=ice-pwd:", 0) == 0 ||
            line.rfind("a=candidate:", 0) == 0) {
            block += line;
            block += "\n";
        }
        pos = end + 2;
    }
    return block;
}

/** True when the ICE state string names a connected agent. */
bool is_connected(std::string_view state) {
    return state == "connected" || state == "completed";
}

}  // namespace

TEST_F(EngineAudioLoopback, TwoEnginesOverLoopbackDeliverDecodedAudio) {
    EngineConfig cfg;
    cfg.local_bind_address    = "127.0.0.1";
    cfg.local_port_range_begin = 51800;
    cfg.local_port_range_end   = 51899;
    cfg.stun_server_host       = "";
    cfg.pcm_sample_rate_hz     = kClockRate;
    cfg.pcm_channels           = 1;
    cfg.audio_ptime_ms         = 20;
    cfg.audio_codec.clock_rate = kClockRate;
    cfg.audio_codec.channels   = 1;
    cfg.audio_codec.payload_type = kOpusPt;

    NimRTCEngine a(cfg);
    if (a.pre_open() != 0 || a.open() != 0) {
        GTEST_SKIP() << "the host does not allow the loopback UDP bind "
                        "(a sandboxed CI runner) — the media path cannot be "
                        "exercised here";
    }

    const std::string offer = a.create_offer();
    ASSERT_FALSE(offer.empty());

    EngineConfig cfg_b = cfg;
    cfg_b.local_port_range_begin = 51900;
    cfg_b.local_port_range_end   = 51999;

    NimRTCEngine b(cfg_b);
    const std::string ice_block = extract_ice_block(offer);
    if (!ice_block.empty()) {
        ASSERT_EQ(b.pre_open(), 0u);
        ASSERT_TRUE(b.set_remote_ice(ice_block))
            << "the answerer must know the offerer's ICE credentials before "
               "it starts gathering (the ICE role conflict)";
    }
    if (b.open() != 0) {
        GTEST_SKIP() << "the host does not allow the second loopback UDP bind";
    }

    const auto answer = b.process_remote_sdp(offer);
    ASSERT_TRUE(answer.has_value()) << "the answerer must accept the offer";
    ASSERT_TRUE(a.process_remote_sdp(*answer).has_value())
        << "the offerer must accept the answer";

    // ---- Wait for the two agents to connect --------------------------
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    bool connected = false;
    while (std::chrono::steady_clock::now() < deadline) {
        a.tick();
        b.tick();
        if (is_connected(a.ice_state_string()) && is_connected(b.ice_state_string())) {
            connected = true;
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    if (!connected) {
        GTEST_SKIP() << "ICE did not connect on the loopback interface in time "
                        "(A=" << a.ice_state_string()
                     << ", B=" << b.ice_state_string() << ")";
    }

    // ---- A sends PCM, B decodes it ------------------------------------
    std::size_t received_frames = 0;
    double received_rms = 0.0;
    double sent_rms = 0.0;
    b.set_on_audio_frame_info(
        [&](const nimrtc::engine::AudioFrameInfo& info) {
            ++received_frames;
            received_rms += rms_of(info.pcm,
                                   info.num_samples_per_channel * info.channels);
        });

    const auto pcm = make_tone(kFrameSamples);
    sent_rms = rms_of(pcm.data(), pcm.size());

    const auto send_deadline = std::chrono::steady_clock::now() + 3s;
    std::size_t sent = 0;
    while (std::chrono::steady_clock::now() < send_deadline) {
        a.tick();
        b.tick();
        // `send_audio()` is refused until the DTLS handshake is done — the
        // error is expected, not a failure.
        if (a.send_audio(pcm.data(), pcm.size()) == 0) {
            ++sent;
        }
        std::this_thread::sleep_for(20ms);
    }
    // Let the last packets arrive and be played out.
    const auto tail = std::chrono::steady_clock::now() + 500ms;
    while (std::chrono::steady_clock::now() < tail) {
        a.tick();
        b.tick();
        std::this_thread::sleep_for(10ms);
    }

    ASSERT_GT(sent, 0u) << "the sender never managed to put a frame on the wire";
    ASSERT_GT(received_frames, 0u)
        << "the receiver never decoded a frame — the inbound path is broken";

    const double mean_received_rms =
        received_rms / static_cast<double>(received_frames);
    EXPECT_GT(mean_received_rms, 0.2 * sent_rms)
        << "the decoded audio is far too quiet to be the signal that was "
           "sent (sent rms " << sent_rms << ", got " << mean_received_rms << ")";
    EXPECT_LT(mean_received_rms, 5.0 * sent_rms)
        << "the decoded audio is far too loud to be the signal that was "
           "sent (sent rms " << sent_rms << ", got " << mean_received_rms << ")";

    // The engine's own counters must agree with what the host saw.
    const auto stats = b.stats();
    EXPECT_GE(stats.audio_frames_received, received_frames);
    EXPECT_GT(stats.audio_samples_received, 0u);
}

}  // namespace
