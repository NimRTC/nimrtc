/**
 * @file tests/test_engine_video_feedback.cpp
 * @brief Engine-level test of the v1.3 Q3 RTCP feedback loop.
 *
 * The module-level loop is covered by `tests/test_video_feedback_loop.cpp`
 * and the plugin adapters by `tests/test_video_plugin_adapters.cpp`.  What
 * this file adds is the *engine* half — the wiring the module tests cannot
 * reach:
 *
 *   outbound  receiver detects a loss -> engine queues the report ->
 *             engine asks the receiver plugin for the RTCP bytes ->
 *             engine protects them (SRTCP) and sends them over ICE.
 *   inbound   peer RTCP (NACK / PLI) -> engine hands the bytes to the
 *             sender plugin -> retransmission, and a key-frame request
 *             forwarded to the encoder.
 *
 * The engine is driven through a *fake ICE transport* registered under the
 * id "fake-ice" and selected with `EngineConfig::transport_name`.  The fake
 * reports `Connected` (so `flush_video_feedback()` does not short-circuit on
 * `is_ice_connected()`), captures every outbound datagram, and can queue
 * inbound datagrams that the engine drains in `tick()`.
 *
 * No DTLS handshake completes (there is no peer), so SRTP is never keyed and
 * the feedback travels in cleartext RTCP — which is exactly what makes the
 * wire format assertable here.  The SRTCP protection path is covered by the
 * SRTP module's own tests.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/plugins/video_pipeline.hpp>

namespace {

namespace np = nimrtc::plugins;
using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::uint16_t be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

std::uint32_t be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

/** Is this datagram an RTCP packet? (RFC 3550 §6.1 demux rule.) */
bool is_rtcp(const std::vector<std::uint8_t>& p) noexcept {
    return p.size() >= 2 && p[1] >= 192 && p[1] <= 223;
}

/** Is this datagram an RTP packet with the given payload type? */
bool is_rtp_with_pt(const std::vector<std::uint8_t>& p, std::uint8_t pt) noexcept {
    return p.size() >= 12 && (p[0] >> 6) == 2 && (p[1] & 0x7F) == pt;
}

/** Build a minimal RTP packet (12-byte header + payload). */
std::vector<std::uint8_t> make_rtp(std::uint32_t ssrc,
                                    std::uint16_t seq,
                                    std::uint32_t timestamp,
                                    std::uint8_t pt,
                                    std::vector<std::uint8_t> payload) {
    std::vector<std::uint8_t> out;
    out.reserve(12 + payload.size());
    out.push_back(0x80);
    out.push_back(pt & 0x7F);
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
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/** Generic NACK (RFC 4585 §6.2.1) with a single FCI entry (PID + BLP). */
std::vector<std::uint8_t> make_nack(std::uint32_t sender_ssrc,
                                    std::uint32_t media_ssrc,
                                    std::uint16_t pid,
                                    std::uint16_t blp) {
    // header(4) + sender SSRC(4) + media SSRC(4) + FCI(4) = 16 bytes
    std::vector<std::uint8_t> out;
    out.reserve(16);
    out.push_back(0x81);          // V=2, P=0, FMT=1 (Generic NACK)
    out.push_back(205);           // RTPFB
    out.push_back(0x00);          // length = 16/4 - 1 = 3
    out.push_back(0x03);
    const std::uint32_t ssrcs[2] = {sender_ssrc, media_ssrc};
    for (std::uint32_t s : ssrcs) {
        out.push_back(static_cast<std::uint8_t>((s >> 24) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((s >> 16) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((s >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>(s & 0xFF));
    }
    out.push_back(static_cast<std::uint8_t>((pid >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(pid & 0xFF));
    out.push_back(static_cast<std::uint8_t>((blp >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(blp & 0xFF));
    return out;
}

/** PLI (RFC 4585 §6.3.1) — 12 bytes, no FCI. */
std::vector<std::uint8_t> make_pli(std::uint32_t sender_ssrc,
                                   std::uint32_t media_ssrc) {
    std::vector<std::uint8_t> out;
    out.reserve(12);
    out.push_back(0x81);          // V=2, P=0, FMT=1 (PLI)
    out.push_back(206);           // PSFB
    out.push_back(0x00);          // length = 12/4 - 1 = 2
    out.push_back(0x02);
    const std::uint32_t ssrcs[2] = {sender_ssrc, media_ssrc};
    for (std::uint32_t s : ssrcs) {
        out.push_back(static_cast<std::uint8_t>((s >> 24) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((s >> 16) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((s >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>(s & 0xFF));
    }
    return out;
}

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
    np::IceState state() const noexcept override { return np::IceState::Connected; }
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
    void add_turn_server(std::string_view,
                         std::uint16_t,
                         std::string_view,
                         std::string_view) noexcept override {}
    void set_bwe(np::IBwe*) noexcept override {}
    void set_scheduler(np::IScheduler*) noexcept override {}

    // ---- Test helpers ----------------------------------------------------
    void queue_inbound(std::vector<std::uint8_t> pkt) {
        inbound_.push_back(std::move(pkt));
    }
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
// Fake video codec — counts force_keyframe() calls
// ---------------------------------------------------------------------------

class CountingVideoCodec final : public np::IVideoCodec {
public:
    const char* name() const noexcept override { return "counting-codec"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    bool can_encode() const noexcept override { return true; }
    bool can_decode() const noexcept override { return false; }
    np::VideoCodecKind kind() const noexcept override {
        return np::VideoCodecKind::kH264;
    }
    std::string_view codec_name() const noexcept override { return "counting"; }

    np::Status encode(const np::VideoFrame&,
                      std::uint8_t* output,
                      std::size_t output_capacity,
                      np::EncodedVideoFrame& encoded_out) noexcept override {
        // Emit a minimal valid Annex-B frame (start code + one IDR NAL) so
        // the reference sender's NALU parser accepts it.  A bare 0x65 byte
        // has no start code and push_frame() would reject it.
        static constexpr std::uint8_t kAnnexBIdr[] = {0x00, 0x00, 0x00, 0x01, 0x65};
        if (!output || output_capacity < sizeof(kAnnexBIdr)) return np::kErrBufferTooSmall;
        std::memcpy(output, kAnnexBIdr, sizeof(kAnnexBIdr));
        encoded_out.payload = nimrtc::core::ByteSpan{output, sizeof(kAnnexBIdr)};
        encoded_out.codec = np::VideoCodecKind::kH264;
        encoded_out.is_keyframe = true;
        encoded_out.nalu_format = np::NaluFormat::kAnnexBStartCode;
        encoded_out.payload_type = payload_type();
        return np::kOk;
    }

    np::Status decode(const np::EncodedVideoFrame&,
                      np::VideoFrame&,
                      std::uint8_t* const*) noexcept override {
        return np::kErrUnsupported;
    }

    np::Status force_keyframe() noexcept override {
        force_keyframe_calls_.fetch_add(1, std::memory_order_relaxed);
        return np::kOk;
    }

    np::VideoCodecConfig config() const noexcept override { return cfg_; }
    np::Status update_config(np::VideoCodecConfig cfg) noexcept override {
        cfg_ = cfg;
        return np::kOk;
    }
    np::VideoCodecStats stats() const noexcept override { return {}; }
    std::uint8_t payload_type() const noexcept override { return 102; }

    int force_keyframe_calls() const noexcept {
        return force_keyframe_calls_.load(std::memory_order_relaxed);
    }

private:
    np::VideoCodecConfig        cfg_{};
    std::atomic<int>            force_keyframe_calls_{0};
};

class CountingVideoCodecFactory final : public np::IVideoCodecFactory {
public:
    std::string_view id() const noexcept override { return "counting-codec"; }
    std::string_view display_name() const noexcept override {
        return "Counting video codec (test)";
    }
    np::IVideoCodec* create(np::VideoCodecConfig) const override {
        auto* c = new CountingVideoCodec();
        last_created() = c;
        return c;
    }
    static CountingVideoCodec*& last_created() {
        static CountingVideoCodec* instance = nullptr;
        return instance;
    }
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

constexpr std::uint32_t kPeerVideoSsrc = 0x11223344u;

class EngineVideoFeedback : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();

        static FakeIceFactory ice_factory;
        PluginRegistry::instance().register_ice_transport("fake-ice", &ice_factory);

        static CountingVideoCodecFactory codec_factory;
        PluginRegistry::instance().register_video_codec("counting-codec", &codec_factory);
    }

    EngineConfig make_config() {
        EngineConfig cfg{};
        // The fake ICE stands in for the real transport: it reports
        // Connected, captures outbound datagrams and can inject inbound
        // ones.  The DTLS handshake never completes (there is no peer),
        // so SRTP is never keyed and the feedback stays in cleartext.
        cfg.transport_name = "fake-ice";
        cfg.video_receiver_name = "reference";
        cfg.video_sender_name   = "reference";
        cfg.video_codec_name    = "counting-codec";
        cfg.datachannel_backend = "";       // no DataChannel in this test
        cfg.bwe_name            = "";       // no pacing / scheduler jitter
        cfg.scheduler_config.enabled = false;
        return cfg;
    }

    /** Find the first captured datagram matching `pred`. */
    static const std::vector<std::uint8_t>* find_sent(
        const std::vector<std::vector<std::uint8_t>>& sent,
        const std::function<bool(const std::vector<std::uint8_t>&)>& pred) {
        for (const auto& p : sent) {
            if (pred(p)) return &p;
        }
        return nullptr;
    }

    static bool is_nack(const std::vector<std::uint8_t>& p) {
        return is_rtcp(p) && p[1] == 205 && (p[0] & 0x1F) == 1;
    }
    static bool is_pli(const std::vector<std::uint8_t>& p) {
        return is_rtcp(p) && p[1] == 206 && (p[0] & 0x1F) == 1;
    }
};

// ---------------------------------------------------------------------------
// Outbound half: loss -> engine -> RTCP NACK on the wire
// ---------------------------------------------------------------------------

TEST_F(EngineVideoFeedback, inbound_loss_produces_rtcp_nack_on_the_wire) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();

    // Two video RTP packets with a hole in between (101 never arrives).
    // The engine routes them by payload type into the video receiver.
    const std::uint8_t pt = cfg.video_receiver_tuning.payload_type;
    ice->queue_inbound(make_rtp(kPeerVideoSsrc, 100, 90000, pt, {0x65, 0x88}));
    ice->queue_inbound(make_rtp(kPeerVideoSsrc, 102, 93000, pt, {0x41, 0x9A}));

    // tick() drains the transport (the receiver detects the gap and the
    // engine queues the report); the *next* tick() flushes the queued
    // report onto the wire.
    engine.tick();
    engine.tick();

    const auto* nack = find_sent(ice->sent(), is_nack);
    ASSERT_NE(nack, nullptr)
        << "the engine did not put an RTCP NACK on the wire";

    // RTCP layout: header(4) | sender SSRC(4) | media SSRC(4) | FCI(4)
    ASSERT_GE(nack->size(), 16u);
    EXPECT_EQ(be32(nack->data() + 8), kPeerVideoSsrc)
        << "the NACK must name the peer's video SSRC as the media source";
    EXPECT_EQ(be16(nack->data() + 12), 101)
        << "the NACK must ask for the missing sequence number";
    EXPECT_EQ(be16(nack->data() + 14), 0)
        << "no further packet is missing (BLP must be empty)";

    engine.close();
}

// ---------------------------------------------------------------------------
// Inbound half: peer NACK -> retransmission
// ---------------------------------------------------------------------------

TEST_F(EngineVideoFeedback, inbound_nack_triggers_retransmission) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();

    // Push one encoded frame through the sender plugin so that it lands
    // in the retransmission cache and goes out on the wire.
    np::EncodedVideoFrame frame{};
    frame.codec        = np::VideoCodecKind::kH264;
    frame.payload_type = cfg.video_sender_tuning.payload_type;
    frame.is_keyframe  = true;
    frame.nalu_format  = np::NaluFormat::kAnnexBStartCode;
    static const std::uint8_t kIdrSlice[] = {
        0x00, 0x00, 0x00, 0x01,
        0x65,
        0x88, 0x84, 0x00, 0x33,
    };
    frame.payload = nimrtc::core::ByteSpan{kIdrSlice, sizeof(kIdrSlice)};
    frame.info.capture_ts_us = 5000;
    frame.info.frame_seq     = 1;
    frame.info.rtp_timestamp = 90000;

    ASSERT_EQ(engine.video_sender()->push_frame(frame, 90000), np::kOk);

    const std::uint8_t pt = cfg.video_sender_tuning.payload_type;
    const auto* original = find_sent(ice->sent(), [pt](const std::vector<std::uint8_t>& p) {
        return is_rtp_with_pt(p, pt);
    });
    ASSERT_NE(original, nullptr) << "the sender did not emit an RTP packet";
    const std::uint16_t seq = be16(original->data() + 2);

    const std::size_t before = ice->sent().size();

    // The peer asks for that packet back.
    const auto nack = make_nack(/*sender_ssrc=*/0x0BADF00D,
                                 /*media_ssrc=*/cfg.video_sender_tuning.ssrc,
                                 /*pid=*/seq,
                                 /*blp=*/0);
    engine.feed_media_inbound(nack.data(), nack.size());

    EXPECT_EQ(engine.video_sender()->stats().packets_retransmitted, 1u)
        << "the sender did not retransmit the NACKed packet";

    // The retransmission went out with the ORIGINAL sequence number.
    std::size_t same_seq = 0;
    for (std::size_t i = 0; i < ice->sent().size(); ++i) {
        const auto& p = ice->sent()[i];
        if (is_rtp_with_pt(p, pt) && be16(p.data() + 2) == seq) ++same_seq;
    }
    EXPECT_EQ(same_seq, 2u)
        << "expected the original + the retransmission on the wire "
        << "(sent " << (ice->sent().size() - before) << " new datagrams)";

    engine.close();
}

// ---------------------------------------------------------------------------
// Inbound half: peer PLI -> forced IDR at the encoder
// ---------------------------------------------------------------------------

TEST_F(EngineVideoFeedback, inbound_pli_forces_encoder_keyframe) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    CountingVideoCodec* codec = CountingVideoCodecFactory::last_created();
    ASSERT_NE(codec, nullptr);
    const int before = codec->force_keyframe_calls();

    const auto pli = make_pli(/*sender_ssrc=*/0x0BADF00D,
                              /*media_ssrc=*/cfg.video_sender_tuning.ssrc);
    engine.feed_media_inbound(pli.data(), pli.size());

    EXPECT_EQ(engine.video_sender()->stats().force_keyframe_calls, 1u)
        << "the sender did not arm its key-frame flag";
    EXPECT_EQ(codec->force_keyframe_calls(), before + 1)
        << "the engine did not forward the key-frame request to the encoder";

    engine.close();
}

// ---------------------------------------------------------------------------
// The engine must not answer feedback it cannot decode
// ---------------------------------------------------------------------------

TEST_F(EngineVideoFeedback, unrelated_rtcp_is_ignored) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    CountingVideoCodec* codec = CountingVideoCodecFactory::last_created();
    ASSERT_NE(codec, nullptr);
    const int before = codec->force_keyframe_calls();

    // A Receiver Report (PT 201) — the engine must not treat it as a
    // key-frame request.
    std::vector<std::uint8_t> rr = {0x80, 201, 0x00, 0x01,
                                     0x00, 0x00, 0x00, 0x01,
                                     0x00, 0x00, 0x00, 0x02};
    engine.feed_media_inbound(rr.data(), rr.size());

    EXPECT_EQ(engine.video_sender()->stats().force_keyframe_calls, 0u);
    EXPECT_EQ(codec->force_keyframe_calls(), before);

    engine.close();
}

// ---------------------------------------------------------------------------
// v1.3 V1: engine stats() counters track video traffic
// ---------------------------------------------------------------------------

TEST_F(EngineVideoFeedback, video_stats_counters_track_traffic) {
    EngineConfig cfg = make_config();
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    FakeIceTransport* ice = FakeIceFactory::last_created();
    ASSERT_NE(ice, nullptr);
    ice->clear_sent();

    // All video counters start at zero on a fresh engine.
    const auto s0 = engine.stats();
    EXPECT_EQ(s0.video_frames_sent, 0u);
    EXPECT_EQ(s0.video_packets_sent, 0u);
    EXPECT_EQ(s0.video_bytes_sent, 0u);
    EXPECT_EQ(s0.video_frames_received, 0u);
    EXPECT_EQ(s0.video_nack_seqs_queued, 0u);
    EXPECT_EQ(s0.video_keyframes_q, 0u);
    EXPECT_EQ(s0.video_feedback_applied, 0u);
    EXPECT_EQ(s0.video_keyframes_applied, 0u);

    // ---- Outbound: send_video() drives frames/packets/bytes ---------------
    // The counting codec ignores pixel input, so null planes are fine.
    np::VideoSourceFrame vf{};
    vf.width = 320;
    vf.height = 240;
    vf.format = np::VideoPixelFormat::kI420;
    vf.capture_ts_us = 1000;
    vf.frame_seq = 1;
    vf.rtp_timestamp = 90000;
    ASSERT_EQ(engine.send_video(vf), np::kOk);

    const auto s1 = engine.stats();
    EXPECT_EQ(s1.video_frames_sent, 1u)
        << "send_video() must increment video_frames_sent";
    EXPECT_GT(s1.video_packets_sent, 0u)
        << "the sender packet callback must count emitted RTP packets";
    EXPECT_GT(s1.video_bytes_sent, 0u)
        << "the sender packet callback must count emitted RTP bytes";

    // ---- Outbound feedback: inbound loss queues a NACK --------------------
    const std::uint8_t rx_pt = cfg.video_receiver_tuning.payload_type;
    ice->queue_inbound(make_rtp(kPeerVideoSsrc, 100, 90000, rx_pt, {0x65, 0x88}));
    ice->queue_inbound(make_rtp(kPeerVideoSsrc, 102, 93000, rx_pt, {0x41, 0x9A}));
    engine.tick();
    engine.tick();

    const auto s2 = engine.stats();
    EXPECT_GT(s2.video_nack_seqs_queued, s1.video_nack_seqs_queued)
        << "queued NACK seqs must grow when the receiver reports a gap";
    EXPECT_GE(s2.video_frames_received, s1.video_frames_received);

    // ---- Inbound feedback: peer NACK applies + retransmits ----------------
    // send_video() already cached an encoded frame in the sender, so ask
    // the peer for one of the RTP packets it just emitted.
    const std::uint8_t tx_pt = cfg.video_sender_tuning.payload_type;
    const auto* original = find_sent(ice->sent(), [tx_pt](const std::vector<std::uint8_t>& p) {
        return is_rtp_with_pt(p, tx_pt);
    });
    ASSERT_NE(original, nullptr) << "send_video() emitted no RTP packet";
    const std::uint16_t seq = be16(original->data() + 2);
    const auto nack = make_nack(/*sender_ssrc=*/0x0BADF00D,
                                 /*media_ssrc=*/cfg.video_sender_tuning.ssrc,
                                 /*pid=*/seq,
                                 /*blp=*/0);
    engine.feed_media_inbound(nack.data(), nack.size());
    const auto s3 = engine.stats();
    EXPECT_GT(s3.video_feedback_applied, s2.video_feedback_applied)
        << "inbound NACK must increment video_feedback_applied";

    // ---- Inbound feedback: peer PLI requests a keyframe -------------------
    const auto pli = make_pli(/*sender_ssrc=*/0x0BADF00D,
                              /*media_ssrc=*/cfg.video_sender_tuning.ssrc);
    engine.feed_media_inbound(pli.data(), pli.size());
    const auto s4 = engine.stats();
    EXPECT_GT(s4.video_keyframes_applied, s3.video_keyframes_applied)
        << "inbound PLI must increment video_keyframes_applied";

    engine.close();
}

} // namespace
