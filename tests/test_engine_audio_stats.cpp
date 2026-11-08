/**
 * @file tests/test_engine_audio_stats.cpp
 * @brief v1.2.0 (audio pipeline, WP-8) — the RTCP receiver report for the
 *        inbound audio stream and the audio statistics counters.
 *
 * Two things are pinned down here, and they are the two things WP-8
 * changed in the engine:
 *
 *   1. **The receiver report** (spec B-12, RFC 3550 §6.4.2).  The
 *      browser's `inbound-rtp` statistics — the loss and the jitter a
 *      host sees in `chrome://webrtc-internals` — are derived from the
 *      RTCP Receiver Reports the *receiver* sends.  Without them the
 *      peer sees a stream that never reports anything: no loss figure,
 *      no jitter figure.  The report carries one report block per
 *      inbound audio SSRC: the fraction lost, the cumulative packets
 *      lost, the extended highest sequence number, the interarrival
 *      jitter and the last Sender Report (spec B-12).
 *
 *   2. **The audio statistics** (spec B-13).  The engine publishes the
 *      counters a host needs to see what the receive path is doing:
 *      the frames and samples that were delivered, the concealed
 *      frames, the FEC-recovered frames, the packets lost, the decode
 *      errors, the jitter and the playout delay.
 *
 * The engine is driven through a *fake ICE transport* registered under
 * the id "fake-ice" (the same pattern `test_engine_video_feedback.cpp`
 * uses): the fake reports `Connected`, so the engine's periodic work
 * runs, and it captures every outbound datagram — which is how the
 * receiver report becomes observable here.  No DTLS handshake completes
 * (there is no peer), so SRTP is never keyed and the report travels in
 * cleartext RTCP, which is exactly what makes the wire format
 * assertable.  The SRTCP protection path is covered by the SRTP
 * module's own tests.
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
#include <nimrtc/plugins/codec.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/rtp/packet.hpp>
#include <nimrtc/rtp/timeline.hpp>

namespace {

namespace np = nimrtc::plugins;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

using namespace std::chrono_literals;

/** The payload type RFC 7587 assigns to Opus, and the one the engine
 *  advertises by default. */
constexpr std::uint8_t kOpusPt = 111;

/** The SSRC the fake peer uses for its audio stream. */
constexpr std::uint32_t kPeerAudioSsrc = 0x0A0A0A0Au;

// ---------------------------------------------------------------------------
// Fake ICE transport
// ---------------------------------------------------------------------------

class FakeIceTransport final : public np::IICETransport {
public:
    // ---- IPlugin ---------------------------------------------------------
    const char* name() const noexcept override { return "fake-ice"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    // ---- ITransport ------------------------------------------------------
    void set_callbacks(np::RecvCallback on_recv,
                       np::ErrorCallback on_error) noexcept override {
        on_recv_ = std::move(on_recv);
        on_error_ = std::move(on_error);
    }

    np::Status send(np::BufferView view, np::Addr = {}) noexcept override {
        sent_.emplace_back(view.data(), view.data() + view.size());
        return np::kOk;
    }

    int recv() noexcept override {
        int n = 0;
        while (!inbound_.empty()) {
            std::vector<std::uint8_t> pkt = std::move(inbound_.front());
            inbound_.erase(inbound_.begin());
            if (on_recv_) {
                on_recv_(np::BufferView(pkt.data(), pkt.size()));
            }
            ++n;
        }
        return n;
    }

    np::Addr local_addr() const noexcept override { return {}; }
    np::Addr remote_addr() const noexcept override { return {}; }

    // ---- IICETransport ---------------------------------------------------
    np::IceState state() const noexcept override {
        return np::IceState::Connected;
    }
    std::string local_ufrag() const noexcept override { return "fakeufrag"; }
    std::string local_password() const noexcept override { return "fakepassword"; }
    bool wait_for_gathering(int) noexcept override { return true; }
    std::vector<std::string> gathered_local_candidates() const noexcept override {
        return {};
    }
    np::Status set_remote_description(std::string_view) noexcept override {
        return np::kOk;
    }
    np::Status add_remote_candidate(std::string_view) noexcept override {
        return np::kOk;
    }
    void set_bind_address(std::string_view) noexcept override {}
    void set_stun_server(std::string_view, std::uint16_t) noexcept override {}
    void set_local_port_range(std::uint16_t, std::uint16_t) noexcept override {}
    void add_turn_server(std::string_view, std::uint16_t, std::string_view,
                         std::string_view) noexcept override {}
    void set_bwe(np::IBwe*) noexcept override {}
    void set_scheduler(np::IScheduler*) noexcept override {}

    // ---- Test helpers ----------------------------------------------------
    const std::vector<std::vector<std::uint8_t>>& sent() const noexcept {
        return sent_;
    }
    void clear_sent() { sent_.clear(); }

private:
    np::RecvCallback   on_recv_;
    np::ErrorCallback  on_error_;
    std::vector<std::vector<std::uint8_t>> inbound_;
    std::vector<std::vector<std::uint8_t>> sent_;
};

class FakeIceFactory final : public np::IICETransportFactory {
public:
    std::string_view id() const noexcept override { return "fake-ice"; }
    std::string_view display_name() const noexcept override {
        return "Fake ICE transport (test)";
    }
    np::IICETransport* create_ice() const override {
        auto* t = new FakeIceTransport();
        last_created() = t;
        return t;
    }
    /** The instance handed to the engine — the test inspects it. */
    static FakeIceTransport*& last_created() {
        static FakeIceTransport* instance = nullptr;
        return instance;
    }
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class EngineAudioStats : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();

        static FakeIceFactory ice_factory;
        PluginRegistry::instance().register_ice_transport("fake-ice", &ice_factory);
    }

    EngineConfig make_config() {
        EngineConfig cfg{};
        // The fake ICE stands in for the real transport: it reports
        // Connected (so the engine's periodic work runs) and captures
        // every outbound datagram.
        cfg.transport_name        = "fake-ice";
        cfg.datachannel_backend   = "";
        cfg.bwe_name              = "";
        cfg.scheduler_config.enabled = false;
        return cfg;
    }

    /** Encode one 20 ms frame with the registered Opus codec plugin and
     *  wrap it into a real RTP packet.
     *
     *  Since WP-7 the receive path *decodes*: the RTP payload is an
     *  Opus frame, not raw PCM.  Returns an empty buffer when the Opus
     *  plugin is not registered (a build without the Opus module). */
    static nimrtc::core::ByteBuffer make_opus_rtp(std::uint32_t ssrc,
                                                   std::uint8_t pt,
                                                   std::uint32_t seq,
                                                   std::uint32_t rtp_ts,
                                                   std::size_t samples_per_channel) {
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

        std::vector<float> pcm(samples_per_channel, 0.25f);
        std::vector<std::uint8_t> bytes(4096);
        const std::size_t n = enc->encode(pcm.data(), samples_per_channel,
                                          bytes.data(), bytes.size());
        if (n == 0) return {};

        nimrtc::rtp::PacketBuilder pb;
        pb.set_ssrc(ssrc)
          .set_seq(static_cast<std::uint16_t>(seq))
          .set_timestamp(rtp_ts)
          .set_payload_type(pt);
        pb.set_payload(nimrtc::core::ByteSpan(bytes.data(), n));
        return pb.build();
    }

    /** Let the audio receive pipeline settle.
     *
     *  The jitter buffer plays a frame out `target_delay` after its
     *  packet arrived (the engine's default is 40 ms), and the playout
     *  only happens on a tick.  A frame that is already buffered is
     *  therefore delivered *after* the last packet was fed — so the
     *  counters keep moving for a while after the input went quiet.
     *
     *  A test that compares the counters before and after a quiet
     *  period has to wait for that to happen, otherwise it measures the
     *  playout delay instead of the counters.
     *
     *  @return The frame count once the pipeline stopped moving. */
    static std::uint64_t settle_audio(NimRTCEngine& engine,
                                      std::uint32_t quiet_rounds = 5) {
        std::uint64_t last = engine.stats().audio_frames_received;
        std::uint32_t quiet = 0;
        for (std::uint32_t i = 0; i < 200 && quiet < quiet_rounds; ++i) {
            engine.tick();
            std::this_thread::sleep_for(20ms);
            const std::uint64_t now = engine.stats().audio_frames_received;
            if (now == last) {
                ++quiet;
            } else {
                quiet  = 0;
                last   = now;
            }
        }
        return last;
    }

    /** Build a Sender Report for @p ssrc.
     *
     *  The engine parses the peer's Sender Report both for the
     *  cross-end timeline (ADR-017) and for the LSR field of the
     *  receiver report (RFC 3550 §6.4.1). */
    static nimrtc::core::ByteBuffer make_sender_report(std::uint32_t ssrc,
                                                       std::uint64_t ntp_timestamp,
                                                       std::uint32_t rtp_timestamp) {
        nimrtc::rtp::SrBuilder sb;
        sb.set_ssrc(ssrc)
          .set_ntp_timestamp(ntp_timestamp)
          .set_rtp_timestamp(rtp_timestamp)
          .set_sender_packet_count(10)
          .set_sender_octet_count(1000);
        return sb.build();
    }

    /** Find the first captured datagram that is an RTCP Receiver Report
     *  (RFC 3550 §6.4.2: payload type 201). */
    static const std::vector<std::uint8_t>* find_rr(
        const std::vector<std::vector<std::uint8_t>>& sent) {
        for (const auto& p : sent) {
            if (p.size() >= 4 && p[1] == 201) return &p;
        }
        return nullptr;
    }

    /** Is this datagram an RTCP Receiver Report? */
    static bool is_rr(const std::vector<std::uint8_t>& p) noexcept {
        return p.size() >= 4 && p[1] == 201;
    }
};

// ============================================================================
// 1. The statistics (spec B-13)
// ============================================================================

TEST_F(EngineAudioStats, FreshEngineReportsZeroAudioCounters) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto s = engine.stats();

    // The send side (WP-5).
    EXPECT_EQ(s.audio_frames_sent, 0u);
    EXPECT_EQ(s.audio_bytes_sent, 0u);
    EXPECT_EQ(s.audio_encode_errors, 0u);
    EXPECT_EQ(s.audio_samples_in, 0u);
    EXPECT_EQ(s.audio_samples_pending, 0u);

    // The receive side (WP-8 / B-13).  A fresh engine has received
    // nothing: every counter must start at zero, otherwise a host
    // cannot tell "nothing happened" from "the counter is broken".
    EXPECT_EQ(s.audio_packets_sent, 0u);
    EXPECT_EQ(s.audio_frames_received, 0u);
    EXPECT_EQ(s.audio_samples_received, 0u);
    EXPECT_EQ(s.audio_concealed_frames, 0u);
    EXPECT_EQ(s.audio_fec_recovered_frames, 0u);
    EXPECT_EQ(s.audio_packets_lost, 0u);
    EXPECT_EQ(s.audio_decode_errors, 0u);
    EXPECT_EQ(s.audio_jitter_ms, 0u);
    EXPECT_EQ(s.audio_playout_delay_ms, 0u);

    engine.close();
}

TEST_F(EngineAudioStats, InboundAudioMovesTheReceiveCounters) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    int legacy_frames = 0;
    engine.set_on_audio_frame([&](const float*, std::size_t) { ++legacy_frames; });

    // Two real Opus packets.  The jitter buffer holds the first one
    // for its initial delay; the second one pops it.
    const auto first = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 1, 0, 960);
    if (first.empty()) {
        GTEST_SKIP() << "the Opus codec plugin is not registered in this build";
    }
    engine.feed_media_inbound(first.data(), first.size());

    std::this_thread::sleep_for(120ms);

    const auto second = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 2, 960, 960);
    ASSERT_FALSE(second.empty());
    engine.feed_media_inbound(second.data(), second.size());

    // The pipeline is clock-driven: the playout happens on tick().
    for (int i = 0; i < 5; ++i) {
        engine.tick();
        std::this_thread::sleep_for(20ms);
    }

    const auto s = engine.stats();

    EXPECT_GT(legacy_frames, 0)
        << "a decoded Opus packet must reach the host as PCM";
    EXPECT_GT(s.audio_frames_received, 0u)
        << "the engine must count the frames it delivered to the host";
    EXPECT_GT(s.audio_samples_received, 0u)
        << "the engine must count the samples it delivered to the host";
    EXPECT_EQ(s.audio_decode_errors, 0u)
        << "a well-formed Opus frame must not be counted as a decode error";
    EXPECT_EQ(s.audio_concealed_frames, 0u)
        << "no packet was lost, so nothing may be concealed";

    engine.close();
}

TEST_F(EngineAudioStats, TheReceiveCountersAreMonotonic) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    const auto first = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 1, 0, 960);
    if (first.empty()) {
        GTEST_SKIP() << "the Opus codec plugin is not registered in this build";
    }
    engine.feed_media_inbound(first.data(), first.size());

    std::this_thread::sleep_for(120ms);
    const auto second = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 2, 960, 960);
    ASSERT_FALSE(second.empty());
    engine.feed_media_inbound(second.data(), second.size());

    // The jitter buffer still holds the second frame: it is played out
    // `target_delay` after it arrived.  Let the pipeline drain before
    // the first snapshot — otherwise the second frame lands *between*
    // the two snapshots and the "no new packet, no new frame"
    // assertion measures the playout delay instead of the counter.
    settle_audio(engine);

    const auto before = engine.stats();

    // The two Opus packets carry two 20 ms frames, so the settled
    // pipeline must have delivered both of them.  Without this the
    // "the count must not grow" assertion below would also pass on an
    // empty pipeline.
    EXPECT_EQ(before.audio_frames_received, 2u)
        << "both Opus frames must have been played out before the first "
           "snapshot";
    EXPECT_EQ(before.audio_samples_received, 1920u)
        << "two 20 ms frames at 48 kHz carry 1920 samples per channel";

    for (int i = 0; i < 3; ++i) {
        engine.tick();
        std::this_thread::sleep_for(20ms);
    }
    const auto after = engine.stats();

    EXPECT_GE(after.audio_frames_received, before.audio_frames_received)
        << "the frame counter must never go backwards";
    EXPECT_GE(after.audio_samples_received, before.audio_samples_received)
        << "the sample counter must never go backwards";
    EXPECT_EQ(after.audio_frames_received, before.audio_frames_received)
        << "no new packet arrived, so the frame count must not grow";

    engine.close();
}

// ============================================================================
// 2. The receiver report (spec B-12, RFC 3550 §6.4.2)
// ============================================================================

TEST_F(EngineAudioStats, NoReceiverReportWithoutInboundAudio) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();

    // No inbound audio at all: there is nothing to report about, so the
    // engine must stay quiet.  A report with an empty body would only
    // confuse the peer's statistics.
    for (int i = 0; i < 3; ++i) {
        engine.tick();
        std::this_thread::sleep_for(20ms);
    }

    for (const auto& p : ice->sent()) {
        EXPECT_FALSE(is_rr(p))
            << "the engine must not send a receiver report before any "
               "audio came in";
    }

    engine.close();
}

TEST_F(EngineAudioStats, ReceiverReportIsSentForTheInboundAudioStream) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();

    // Three packets with a hole: 1, 2 and 5.  The gap of two is what
    // the loss accounting has to report.
    const std::uint32_t seqs[] = {1, 2, 5};
    for (std::uint32_t seq : seqs) {
        const auto packet =
            make_opus_rtp(kPeerAudioSsrc, kOpusPt, seq, (seq - 1) * 960, 960);
        if (packet.empty()) {
            GTEST_SKIP() << "the Opus codec plugin is not registered in this build";
        }
        engine.feed_media_inbound(packet.data(), packet.size());
    }

    // The report goes out from tick() — the first tick after the first
    // inbound packet already carries a full report interval.
    engine.tick();

    const auto* rr_bytes = find_rr(ice->sent());
    ASSERT_NE(rr_bytes, nullptr)
        << "the engine did not put an RTCP receiver report on the wire; "
           "without it the peer reports no inbound-rtp statistics at all";

    // RTCP layout: header(4) | reporter SSRC(4) | report block(24).
    ASSERT_GE(rr_bytes->size(), 32u);
    EXPECT_EQ((*rr_bytes)[1], 201) << "the payload type must be RR";
    EXPECT_EQ((*rr_bytes)[0] & 0x1F, 1)
        << "the report must carry exactly one report block (the audio "
           "source)";

    nimrtc::rtp::Parser parser;
    const auto rr = parser.parse_rr(nimrtc::core::ByteSpan(rr_bytes->data(),
                                                           rr_bytes->size()));
    ASSERT_TRUE(rr.ok())
        << "the bytes on the wire must be a parsable receiver report";
    ASSERT_EQ(rr.value().report_blocks.size(), 1u);

    const auto& block = rr.value().report_blocks[0];
    EXPECT_EQ(block.ssrc, kPeerAudioSsrc)
        << "the report block must name the peer's audio source";
    EXPECT_EQ(block.extended_highest_seq, 5u)
        << "the extended highest sequence number must be the highest "
           "sequence number that arrived";
    EXPECT_EQ(block.cumulative_packets_lost, 2)
        << "RFC 3550 §6.4.2: the cumulative loss is the expected packet "
           "count (extended highest sequence number minus the base, plus "
           "one) minus the number of packets received — five expected, "
           "three received, two lost";
    EXPECT_GE(block.fraction_lost, 0u);
    EXPECT_LE(block.fraction_lost, 255u);

    engine.close();
}

TEST_F(EngineAudioStats, ReceiverReportCarriesTheLastSenderReport) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();

    const auto packet = make_opus_rtp(kPeerAudioSsrc, kOpusPt, 1, 0, 960);
    if (packet.empty()) {
        GTEST_SKIP() << "the Opus codec plugin is not registered in this build";
    }
    engine.feed_media_inbound(packet.data(), packet.size());

    // The peer's Sender Report is the anchor of our receiver report:
    // its middle 32 bits are the LSR field (RFC 3550 §6.4.1).
    constexpr std::uint64_t kNtp = 3900000000ull << 32;  // 2023-08-17, say
    const auto sr = make_sender_report(kPeerAudioSsrc, kNtp, 0);
    ASSERT_FALSE(sr.empty());
    engine.feed_media_inbound(sr.data(), sr.size());

    // The DLSR field (the delay since the last Sender Report) counts in
    // units of 1/65536 s — about 15.3 µs — and the engine truncates the
    // elapsed time into that unit (RFC 3550 §6.4.1).  A delay shorter
    // than one unit therefore *is* zero on the wire, so the receiver
    // report has to be built at least one unit later for the field to
    // be observable at all.
    std::this_thread::sleep_for(1ms);

    engine.tick();

    const auto* rr_bytes = find_rr(ice->sent());
    ASSERT_NE(rr_bytes, nullptr)
        << "the engine did not put an RTCP receiver report on the wire";

    nimrtc::rtp::Parser parser;
    const auto rr = parser.parse_rr(nimrtc::core::ByteSpan(rr_bytes->data(),
                                                           rr_bytes->size()));
    ASSERT_TRUE(rr.ok());
    ASSERT_EQ(rr.value().report_blocks.size(), 1u);

    const nimrtc::rtp::Ntp64 ntp{kNtp};
    EXPECT_EQ(rr.value().report_blocks[0].last_sr_ntp, ntp.mid_32())
        << "the report must carry the middle 32 bits of the peer's last "
           "Sender Report (the LSR field)";
    EXPECT_GT(rr.value().report_blocks[0].delay_since_last_sr, 0u)
        << "the delay since the last Sender Report must be filled in";

    engine.close();
}

TEST_F(EngineAudioStats, ReceiverReportIsNotSentForAStreamThatNeverArrived) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();

    // A *video* packet: the audio stream stayed empty, so there is no
    // audio source to report about.
    std::vector<std::uint8_t> video(12 + 2, 0);
    video[0] = 0x80;
    video[1] = 102;  // the engine's own video payload type
    engine.feed_media_inbound(video.data(), video.size());

    engine.tick();

    for (const auto& p : ice->sent()) {
        EXPECT_FALSE(is_rr(p))
            << "the engine must not report about an audio source that "
               "never sent anything";
    }

    engine.close();
}

}  // namespace
