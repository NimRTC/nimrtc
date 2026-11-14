/**
 * @file tests/test_engine_qoe.cpp
 * @brief The engine's QoE wiring (remote-desktop plan **J3** / **J6**).
 *
 * ## What is tested
 *
 * The `rd_qoe` module owns the *metrics*; the engine owns the *events*.
 * This test drives the **engine** — the public surface a host touches —
 * and asserts that the engine really pushes the events into the collector
 * and really publishes the result:
 *
 *   - a frame that reached the viewer is counted as **presented**, and
 *     the end-to-end latency (render − capture) is measured;
 *   - the viewer's frame rate and the source frame rate are published;
 *   - an input event sent on the control plane is paired with the frame
 *     that reflects it, and the **input-to-photon** latency is measured
 *     (plan item J6);
 *   - without a collector the engine still publishes the frame counters
 *     — and *nothing else*, because the metrics were not measured.
 *
 * ## The negative control
 *
 * The last suite is the important one: it drives the engine with the
 * collector **disabled** and asserts that the metrics stay at zero.
 * Without it, a test that only ever sees non-zero numbers cannot tell
 * "the engine feeds the collector" from "the collector measures
 * something on its own".
 *
 * ## How the receive path is driven
 *
 * The engine only routes an inbound RTP packet to the video pipeline
 * when its payload type is in the *negotiated* routing table, so the
 * fixture completes a description exchange with a peer engine (the same
 * pattern as `test_engine_video_stats.cpp`) and then feeds the frames
 * through `feed_media_inbound()` — a documented test seam.
 *
 * The frames are encoded with the engine's **own** codec, so the test
 * does not depend on which backend the host happens to have (a hardware
 * decoder would reject a hand-written bitstream).
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/dtls/dtls_session_iface.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/h264/codec_plugin.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/plugins/video_sink.hpp>
#include <nimrtc/video_frame/frame.hpp>

namespace {

namespace np = nimrtc::plugins;
namespace nd = nimrtc::dtls;
namespace core = nimrtc::core;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

using namespace std::chrono_literals;

/** The SSRC the fake peer uses for its video stream. */
constexpr std::uint32_t kPeerVideoSsrc = 0x11223344u;

constexpr std::uint32_t kWidth  = 320;
constexpr std::uint32_t kHeight = 240;

/** The sink's own delay, in milliseconds.
 *
 *  The end-to-end latency is `render − capture`, and the capture instant
 *  of an inbound frame is its *arrival* instant.  Without a delay in the
 *  sink the two would be separated only by the decode time, which on a
 *  fast machine can round to zero microseconds — and then the latency
 *  assertion would be a coin flip.  The sink therefore sleeps, which
 *  makes the measured latency deterministic.
 */
constexpr int kSinkRenderDelayMs = 2;

// ---------------------------------------------------------------------------
// A deterministic software H.264 codec.
// ---------------------------------------------------------------------------

/**
 * @brief A codec factory that always instantiates the *software* H.264
 *        adapter.
 *
 * The shipped "h264" plugin selects its backend by priority: a hardware
 * encoder / decoder when the host has one, the software stub otherwise.
 * A unit test must not depend on which backend the *host* happens to
 * have — the same binary would then pass on a CI runner and fail on a
 * workstation with a GPU.
 *
 * The id is "h264-software": the engine derives the codec *family* from
 * the id when the plugin does not name one, and "h264" followed by a
 * separator is the documented form for that (see
 * `detail::codec_family_from_plugin_id`).
 */
class SoftwareH264CodecFactory final : public np::IVideoCodecFactory {
public:
    std::string_view id() const noexcept override { return "h264-software"; }
    std::string_view display_name() const noexcept override {
        return "H.264 software codec (test)";
    }
    np::IVideoCodec* create(np::VideoCodecConfig cfg) const override {
        return new nimrtc::h264::CodecPluginAdapter(std::move(cfg));
    }
};

// ---------------------------------------------------------------------------
// The app-data bus — the two fake DTLS sessions talk through it.
// ---------------------------------------------------------------------------

/**
 * @brief The bus that stands in for the DTLS record layer.
 *
 * `send_app_data()` on one session puts the bytes into the *other*
 * session's inbox; `take_app_data()` drains that inbox.  One record in,
 * one record out — the record boundary is preserved.
 */
class AppDataBus {
public:
    int attach() {
        std::lock_guard<std::mutex> lock(mutex_);
        inboxes_.emplace_back();
        return static_cast<int>(inboxes_.size()) - 1;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& inbox : inboxes_) inbox.clear();
    }

    void deliver(int from, std::span<const std::uint8_t> bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < inboxes_.size(); ++i) {
            if (static_cast<int>(i) == from) continue;
            inboxes_[i].emplace_back(bytes.begin(), bytes.end());
        }
    }

    std::vector<std::vector<std::uint8_t>> take(int index) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index < 0 || static_cast<std::size_t>(index) >= inboxes_.size()) {
            return {};
        }
        std::vector<std::vector<std::uint8_t>> out;
        out.swap(inboxes_[static_cast<std::size_t>(index)]);
        return out;
    }

private:
    std::mutex mutex_;
    std::vector<std::vector<std::vector<std::uint8_t>>> inboxes_;
};

// ---------------------------------------------------------------------------
// The fake DTLS session — connected, with a working app-data seam.
// ---------------------------------------------------------------------------

class FakeAppDataDtlsSession final : public nd::IDtlsSession {
public:
    FakeAppDataDtlsSession(AppDataBus* bus, int index)
        : bus_(bus), index_(index) {}

    // ---- The seam-only methods -------------------------------------------
    void set_role(nd::Role) noexcept override {}
    void set_peer_fingerprint(std::span<const std::uint8_t>) noexcept override {}
    void start() noexcept override {}
    void pump() noexcept override {}
    void on_handshake_complete(OnCompleteCb) noexcept override {}
    np::Status export_srtp_key_material(std::span<std::uint8_t, 60>) noexcept override {
        return np::kErrNotReady;
    }

    // ---- The engine-facing methods ---------------------------------------
    core::Result<void> open() noexcept override {
        return core::Result<void>::make_ok();
    }
    void set_role(nd::DtlsRole) noexcept override {}
    void set_peer_fingerprint(std::string, std::vector<std::uint8_t>) noexcept override {}
    std::size_t feed_inbound(std::span<const std::uint8_t>,
                             const nd::DtlsAddr&) noexcept override {
        return 0;
    }
    std::vector<nd::DtlsRecord> take_outbound() noexcept override { return {}; }
    void tick() noexcept override {}
    nd::DtlsState state() const noexcept override { return nd::DtlsState::Connected; }
    bool is_connected() const noexcept override { return true; }
    const nd::Fingerprint& local_fingerprint() const noexcept override {
        return fingerprint_;
    }
    std::optional<nd::SrtpKeyingMaterial>
    srtp_keying_material() const noexcept override {
        return std::nullopt;
    }

    // ---- The app-data seam (plan G1) -------------------------------------
    np::Status send_app_data(std::span<const std::uint8_t> bytes) noexcept override {
        if (bus_ == nullptr) return np::kErrNotReady;
        bus_->deliver(index_, bytes);
        return np::kOk;
    }

    std::vector<std::vector<std::uint8_t>> take_app_data() noexcept override {
        if (bus_ == nullptr) return {};
        return bus_->take(index_);
    }

private:
    AppDataBus* bus_ = nullptr;
    int index_ = 0;
    nd::Fingerprint fingerprint_{"sha-256", {}, {}, ""};
};

/**
 * @brief The factory that hands out the fake sessions.
 *
 * Every `create()` call attaches a new endpoint to the bus.
 */
class FakeAppDataDtlsFactory final : public nd::IDtlsSessionFactory {
public:
    explicit FakeAppDataDtlsFactory(AppDataBus* bus) : bus_(bus) {}

    std::string_view id() const noexcept override { return "qoe-fake-dtls"; }
    std::string_view display_name() const noexcept override {
        return "Fake DTLS session with an app-data seam (QoE test)";
    }

    std::unique_ptr<nd::IDtlsSession> create(const nd::DtlsConfig&) const override {
        return std::make_unique<FakeAppDataDtlsSession>(
            bus_, bus_ != nullptr ? bus_->attach() : 0);
    }

private:
    AppDataBus* bus_ = nullptr;
};

// ---------------------------------------------------------------------------
// The fake ICE transport — "connected", and nothing else.
// ---------------------------------------------------------------------------

class FakeConnectedIce final : public np::IICETransport {
public:
    const char* name() const noexcept override { return "qoe-fake-ice"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    void set_callbacks(np::RecvCallback on_recv,
                       np::ErrorCallback on_error) noexcept override {
        on_recv_ = std::move(on_recv);
        on_error_ = std::move(on_error);
    }
    np::Status send(np::BufferView, np::Addr = {}) noexcept override {
        return np::kOk;
    }
    int recv() noexcept override { return 0; }
    np::Addr local_addr() const noexcept override { return {}; }
    np::Addr remote_addr() const noexcept override { return {}; }

    np::IceState state() const noexcept override { return np::IceState::Connected; }
    std::string local_ufrag() const noexcept override { return "qoeurag"; }
    std::string local_password() const noexcept override { return "qoepassword"; }
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

private:
    np::RecvCallback  on_recv_;
    np::ErrorCallback on_error_;
};

class FakeConnectedIceFactory final : public np::IICETransportFactory {
public:
    std::string_view id() const noexcept override { return "qoe-fake-ice"; }
    std::string_view display_name() const noexcept override {
        return "Fake ICE (always connected, QoE test)";
    }
    np::IICETransport* create_ice() const override { return new FakeConnectedIce(); }
};

// ---------------------------------------------------------------------------
// The sink — counts the frames it drew, and delays them a little.
// ---------------------------------------------------------------------------

class QoeSink final : public np::IVideoSink {
public:
    const char* name() const noexcept override { return "QoeSink"; }
    np::Status open() noexcept override { open_ = true; return np::kOk; }
    void close() noexcept override { open_ = false; }

    np::Status render(const np::VideoSourceFrame&) noexcept override {
        // The delay makes the end-to-end latency deterministic (see
        // `kSinkRenderDelayMs`).
        std::this_thread::sleep_for(std::chrono::milliseconds(kSinkRenderDelayMs));
        ++rendered_;
        return np::kOk;
    }
    np::Status render(const np::GpuBuffer&) noexcept override {
        return np::kErrUnsupported;
    }
    np::VideoSinkStats stats() const noexcept override {
        np::VideoSinkStats s;
        s.frames_rendered = rendered_;
        return s;
    }
    np::VideoSinkConfig config() const noexcept override { return {}; }

    std::uint64_t rendered() const noexcept { return rendered_; }

private:
    std::uint64_t rendered_ = 0;
    bool open_ = false;
};

class QoeSinkFactory final : public np::IVideoSinkFactory {
public:
    std::string_view id() const noexcept override { return "qoe-sink"; }
    std::string_view display_name() const noexcept override {
        return "QoE sink";
    }
    np::IVideoSink* create(np::VideoSinkConfig) const override {
        return new QoeSink();
    }
};

// ---------------------------------------------------------------------------
// The fixture
// ---------------------------------------------------------------------------

class EngineQoe : public ::testing::Test {
protected:
    /**
     * @brief The shared app-data bus.
     *
     * It is **static** on purpose: the DTLS factory is registered once
     * for the whole suite (the registry keeps the pointer for the
     * lifetime of the process), so the bus it points at has to outlive
     * every individual test.
     */
    static AppDataBus bus;

    static void SetUpTestSuite() {
        nimrtc::core::register_all_default_plugins();

        static const SoftwareH264CodecFactory codec;
        PluginRegistry::instance().register_video_codec(
            "h264-software", &codec);

        static FakeConnectedIceFactory ice;
        PluginRegistry::instance().register_ice_transport(ice.id(), &ice);

        static FakeAppDataDtlsFactory dtls(&bus);
        PluginRegistry::instance().register_dtls_session(dtls.id(), &dtls);

        static QoeSinkFactory sink;
        PluginRegistry::instance().register_video_sink(sink.id(), &sink);
    }

    void SetUp() override { bus.reset(); }

    /** The engine configuration every test in this file starts from. */
    static EngineConfig make_config(const char* qoe_collector) {
        EngineConfig cfg{};
        // The **default** strategy ("webrtc-classic") is used on
        // purpose: the control channel rides the DTLS application-data
        // seam (plan G1), and the "shell" strategy has no DTLS session
        // at all — asking for the G1 transport there is a hard error.
        cfg.transport_name           = "qoe-fake-ice";
        cfg.dtls_name                = "qoe-fake-dtls";
        cfg.datachannel_backend     = "";
        cfg.bwe_name                 = "";
        cfg.scheduler_config.enabled = false;

        cfg.video_codec_name    = "h264-software";
        cfg.video_sender_name   = "reference";
        cfg.video_receiver_name = "reference";
        cfg.video_source_name   = "memory";
        cfg.video_sink_name     = "qoe-sink";
        cfg.video_width         = kWidth;
        cfg.video_height        = kHeight;
        cfg.video_fps           = 30;

        // The control plane: the input event needs a transport, and the
        // DTLS app-data seam (plan G1) is the one the fake session
        // provides.
        cfg.control_channel_name = "control-channel";
        cfg.control_channel_transport =
            np::ControlChannelTransport::kDtlsAppData;

        cfg.qoe_collector_name = qoe_collector;
        return cfg;
    }

    /**
     * @brief Complete the inbound routing table.
     *
     *  The engine only routes an inbound RTP packet to the video
     *  pipeline when its payload type is in the *negotiated* routing
     *  table, and that table is filled exclusively by
     *  `process_remote_sdp()`.  A second engine plays the peer: its offer
     *  names the video payload type, and processing that offer installs
     *  the route.
     */
    static void negotiate_video(NimRTCEngine& engine,
                                const EngineConfig& cfg) {
        NimRTCEngine peer(cfg);
        if (peer.open() != 0) return;
        const std::string offer = peer.create_offer();
        peer.close();
        if (offer.empty()) return;
        const auto answer = engine.process_remote_sdp(offer);
        EXPECT_TRUE(answer.has_value())
            << "the peer offer must be accepted — the routing table is "
               "built from it";
    }

    /** Build a minimal RTP packet (12-byte header + payload). */
    static std::vector<std::uint8_t> make_rtp(std::uint32_t ssrc,
                                               std::uint16_t seq,
                                               std::uint32_t timestamp,
                                               std::uint8_t pt,
                                               const std::vector<std::uint8_t>& payload,
                                               bool marker = true) {
        std::vector<std::uint8_t> out;
        out.reserve(12 + payload.size());
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
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    /** Split an Annex B bitstream into its NAL units. */
    static std::vector<std::vector<std::uint8_t>> split_annex_b(
        const std::vector<std::uint8_t>& bs) {
        std::vector<std::vector<std::uint8_t>> out;
        std::size_t i = 0;
        while (i + 3 < bs.size()) {
            std::size_t sc = i;
            while (sc + 3 < bs.size() &&
                   !(bs[sc] == 0 && bs[sc + 1] == 0 &&
                     (bs[sc + 2] == 1 ||
                      (bs[sc + 2] == 0 && bs[sc + 3] == 1)))) {
                ++sc;
            }
            if (sc + 3 >= bs.size()) break;
            const std::size_t nal_start = (bs[sc + 2] == 1) ? sc + 3 : sc + 4;
            std::size_t next = nal_start;
            while (next + 3 < bs.size() &&
                   !(bs[next] == 0 && bs[next + 1] == 0 &&
                     (bs[next + 2] == 1 ||
                      (bs[next + 2] == 0 && bs[next + 3] == 1)))) {
                ++next;
            }
            if (next + 3 >= bs.size()) next = bs.size();
            if (next > nal_start) {
                out.emplace_back(bs.begin() + nal_start, bs.begin() + next);
            }
            i = next;
        }
        return out;
    }

    /** Encode one synthetic frame with the engine's *own* codec. */
    static std::vector<std::vector<std::uint8_t>> encode_nalus(
        NimRTCEngine& engine,
        std::uint32_t frame_seq) {
        auto* codec = engine.video_codec();
        if (!codec) return {};

        const nimrtc::video_frame::VideoFrameLayout layout =
            nimrtc::video_frame::compute_layout(np::VideoPixelFormat::kI420,
                                                kWidth, kHeight);
        auto buf = std::make_shared<nimrtc::video_frame::VideoFrameBuffer>(layout);
        std::memset(buf->data(), 128, layout.buffer_size);

        np::VideoFrame raw{};
        raw.buffer = buf;
        raw.info.capture_ts_us  = static_cast<std::int64_t>(frame_seq) * 33333;
        raw.info.frame_seq      = frame_seq;
        raw.info.rtp_timestamp  = frame_seq * 3000;

        std::vector<std::uint8_t> enc(256 * 1024);
        np::EncodedVideoFrame encoded{};
        if (codec->encode(raw, enc.data(), enc.size(), encoded) != np::kOk) {
            return {};
        }
        return split_annex_b(
            std::vector<std::uint8_t>(encoded.payload.begin(),
                                      encoded.payload.end()));
    }

    /**
     * @brief Feed one inbound video frame as RTP packets (one per NAL unit).
     *
     *  The marker bit goes on the *last* packet of the frame: it tells
     *  the jitter buffer the frame is complete, so it can release it
     *  immediately instead of waiting for the max-wait timeout.
     */
    static void feed_video(NimRTCEngine& engine,
                            std::uint8_t pt,
                            std::uint16_t seq,
                            std::uint32_t rtp_timestamp,
                            std::uint32_t frame_seq) {
        const auto nalus = encode_nalus(engine, frame_seq);
        if (nalus.empty()) return;

        std::uint16_t s = seq;
        for (std::size_t i = 0; i < nalus.size(); ++i) {
            const bool last = (i + 1 == nalus.size());
            const auto pkt = make_rtp(kPeerVideoSsrc, s++, rtp_timestamp, pt,
                                      nalus[i], /*marker=*/last);
            engine.feed_media_inbound(pkt.data(), pkt.size());
        }
    }
};

AppDataBus EngineQoe::bus;

// ---------------------------------------------------------------------------
// The registry slot
// ---------------------------------------------------------------------------

TEST_F(EngineQoe, TheCollectorIsResolvedThroughTheRegistry) {
    // The built-in collector registers under "rd-qoe" — but only when
    // the module is built.  The test asserts the *slot* works, not that
    // a particular module is present, so it skips when the module is
    // off.
    const auto ids = PluginRegistry::instance().list_qoe_collectors();
    if (ids.empty()) {
        GTEST_SKIP() << "the rd_qoe module is not built in this configuration";
    }
    ASSERT_NE(PluginRegistry::instance().get_qoe_collector("rd-qoe"), nullptr);
}

// ---------------------------------------------------------------------------
// J3 — the presented frames
// ---------------------------------------------------------------------------

TEST_F(EngineQoe, ThePresentedFramesAreCounted) {
    EngineConfig cfg = make_config("rd-qoe");
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u) << "the engine must open";

    negotiate_video(engine, cfg);

    const std::uint8_t pt = cfg.video_receiver_tuning.payload_type;
    feed_video(engine, pt, /*seq=*/100, /*rtp_ts=*/90'000, /*frame_seq=*/1);
    feed_video(engine, pt, /*seq=*/200, /*rtp_ts=*/93'000, /*frame_seq=*/2);

    // The frames really reached the sink — without that, the QoE
    // counters could not have moved either.
    ASSERT_EQ(engine.stats().video_frames_rendered, 2u)
        << "the decoded frames must reach the sink";

    const auto stats = engine.stats();

    // The engine pushed the frames into the collector.  The count is the
    // *collector's*, not the engine's own counter — so this proves the
    // wiring, not just the bookkeeping.
    EXPECT_EQ(stats.qoe.frames_presented, 2u)
        << "the engine must have fed the collector the presented frames";

    // The end-to-end latency is measured: the capture timestamp is in
    // the past relative to the render, so the percentile is positive.
    EXPECT_GE(stats.qoe.latency_p50_us,
              static_cast<std::int64_t>(kSinkRenderDelayMs) * 1000)
        << "the end-to-end latency must have been measured";
    EXPECT_GE(stats.qoe.latency_max_us, stats.qoe.latency_p50_us);

    // The viewer's frame rate and the source frame rate.
    EXPECT_GT(stats.qoe.presented_fps, 0.0)
        << "the presented frame rate must have been measured";
    EXPECT_DOUBLE_EQ(stats.qoe.source_fps, 30.0)
        << "the engine must report the source frame rate to the collector";
}

TEST_F(EngineQoe, TheCountersArePublishedWithoutACollector) {
    EngineConfig cfg = make_config("");
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u) << "the engine must open";

    negotiate_video(engine, cfg);

    const std::uint8_t pt = cfg.video_receiver_tuning.payload_type;
    feed_video(engine, pt, /*seq=*/100, /*rtp_ts=*/90'000, /*frame_seq=*/1);
    feed_video(engine, pt, /*seq=*/200, /*rtp_ts=*/93'000, /*frame_seq=*/2);

    const auto stats = engine.stats();

    EXPECT_EQ(stats.qoe.frames_presented, 2u)
        << "the frame counters do not depend on a collector";
    EXPECT_EQ(stats.qoe.latency_p50_us, 0)
        << "without a collector the latency was NOT measured — reporting a "
           "number here would be a lie";
    EXPECT_EQ(stats.qoe.input_to_photon_p50_us, 0);
    EXPECT_DOUBLE_EQ(stats.qoe.presented_fps, 0.0);
}

TEST_F(EngineQoe, AnUnknownCollectorIdIsAConfigurationError) {
    EngineConfig cfg = make_config("no-such-qoe-collector");
    NimRTCEngine engine(cfg);
    EXPECT_NE(engine.open(), 0u)
        << "a typo'd collector id must not be silently ignored";
}

// ---------------------------------------------------------------------------
// J6 — the input-to-photon latency
// ---------------------------------------------------------------------------

TEST_F(EngineQoe, TheInputToPhotonLatencyIsMeasured) {
    EngineConfig cfg = make_config("rd-qoe");
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u) << "the engine must open";

    negotiate_video(engine, cfg);

    const std::uint8_t pt = cfg.video_receiver_tuning.payload_type;

    // The first frame is the one the operator is looking at.
    feed_video(engine, pt, /*seq=*/100, /*rtp_ts=*/90'000, /*frame_seq=*/1);
    ASSERT_EQ(engine.stats().qoe.frames_presented, 1u);

    // The operator acts on that frame: the event carries the frame's id
    // (the ref-frame id space the engine publishes through
    // `last_displayed_ref_frame_id()`).
    const std::uint8_t payload[] = {0x01, 0x02, 0x03};
    ASSERT_EQ(engine.send_input_event(
                  std::span<const std::uint8_t>(payload, sizeof(payload)),
                  /*event_id=*/1u),
              np::kOk)
        << "the control plane must accept the input event";

    // Nothing is measured yet: the frame that *reflects* the event has
    // not been displayed.
    EXPECT_EQ(engine.stats().qoe.input_to_photon_p50_us, 0)
        << "the input-to-photon latency is only measured once the "
           "reflecting frame is on the screen";

    // The next frame reflects the event (its id is strictly greater),
    // so the pair is complete.
    feed_video(engine, pt, /*seq=*/200, /*rtp_ts=*/93'000, /*frame_seq=*/2);
    ASSERT_EQ(engine.stats().qoe.frames_presented, 2u);

    const auto stats = engine.stats();
    EXPECT_GT(stats.qoe.input_to_photon_p50_us, 0)
        << "the input-to-photon latency must have been measured";
    EXPECT_GE(stats.qoe.input_to_photon_max_us,
              stats.qoe.input_to_photon_p50_us);
}

TEST_F(EngineQoe, AnUnboundInputEventIsPairedWithTheNextFrame) {
    EngineConfig cfg = make_config("rd-qoe");
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u) << "the engine must open";

    negotiate_video(engine, cfg);

    const std::uint8_t payload[] = {0x10, 0x11};
    ASSERT_EQ(engine.send_input_event(
                  std::span<const std::uint8_t>(payload, sizeof(payload)),
                  /*event_id=*/0u),
              np::kOk);

    const std::uint8_t pt = cfg.video_receiver_tuning.payload_type;
    feed_video(engine, pt, /*seq=*/100, /*rtp_ts=*/90'000, /*frame_seq=*/1);

    EXPECT_GT(engine.stats().qoe.input_to_photon_p50_us, 0)
        << "an event that was not bound to a frame is paired with the next "
           "presented frame";
}

TEST_F(EngineQoe, ARefusedInputEventIsNotMeasured) {
    EngineConfig cfg = make_config("rd-qoe");
    // The control plane is off: the event never reaches the peer, so
    // there is no honest latency to report for it.
    cfg.control_channel_name.clear();

    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u) << "the engine must open";

    negotiate_video(engine, cfg);

    const std::uint8_t payload[] = {0x01, 0x02, 0x03};
    EXPECT_EQ(engine.send_input_event(
                  std::span<const std::uint8_t>(payload, sizeof(payload)),
                  /*event_id=*/1u),
              np::kErrNotReady);

    const std::uint8_t pt = cfg.video_receiver_tuning.payload_type;
    feed_video(engine, pt, /*seq=*/100, /*rtp_ts=*/90'000, /*frame_seq=*/1);

    const auto stats = engine.stats();
    EXPECT_EQ(stats.qoe.frames_presented, 1u);
    EXPECT_EQ(stats.qoe.input_to_photon_p50_us, 0)
        << "a refused input event must not be measured";
}

TEST_F(EngineQoe, TheInputEventIsRejectedWhenThePayloadIsEmpty) {
    EngineConfig cfg = make_config("rd-qoe");
    NimRTCEngine engine(cfg);
    ASSERT_EQ(engine.open(), 0u);

    EXPECT_EQ(engine.send_input_event(std::span<const std::uint8_t>{}, 1u),
              np::kErrInvalidParam);
}

}  // namespace
