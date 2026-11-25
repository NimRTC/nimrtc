/**
 * @file tests/test_engine_video_hw_h265.cpp
 * @brief The engine-level H.265 parameter-set switch (v1.3 plan Q1b / Track R).
 *
 * ## What this test proves
 *
 * `src/modules/nvcodec/tests/test_hevc_pts.cpp` proves the *codec-level*
 * half of Q1b: one NVDEC instance, created for the first geometry, decodes
 * the picture that follows an NVENC re-init at a new geometry — no flush,
 * no re-open, no key-frame request.
 *
 * This file is the *engine-level* half — the half the plan's row asks for
 * ("a codec-level case in `src/modules/nvcodec/tests/` and an engine-level
 * case gated on the Q7 hardware leg (`gpu-nvenc`)").  It proves the
 * property that only the full pipeline can show:
 *
 *   1. the **sender** notices the geometry change and restarts the
 *      encoder, so the new VPS / SPS / PPS travel with the next frame
 *      (the engine's own `apply_video_encoder_params()` — the B13 rule);
 *   2. the **wire** carries them (the AP aggregation packet and the FU
 *      fragments of the IDR slice — RFC 7798 §4.3);
 *   3. the **receiver** assembles one complete access unit out of them
 *      and decodes it — the *first* decode after the switch returns
 *      `kOk`, with the *new* geometry;
 *   4. **no key-frame round-trip** happened: the receiver's
 *      decode-failure door never opened
 *      (`video_feedback_pli_from_decode_failure == 0`) and no PLI went
 *      out on the wire.
 *
 * Point 4 is what makes this a *proof* rather than a smoke test: a
 * decoder that silently refused the new picture would still "pass" a
 * "did anything decode?" check — it just would not produce the new
 * geometry, and it *would* open the decode-failure door (which is what
 * the counter measures).
 *
 * ## Why this file exists separately from the H.264 e2e test
 *
 * `tests/test_engine_video_e2e_loopback.cpp` has the same shape, but it
 * pins the *software* OpenH264 codec.  This file pins the *hardware* one
 * (`video_codec_backend = "hardware"`), so:
 *
 *   - the H.265 parameter sets really come from NVENC (there is no
 *     software H.265 encoder in the tree to fake them with), and
 *   - the decoder really is NVDEC (there is no software H.265 decoder in
 *     the tree either — the stub only paints a synthetic pattern).
 *
 * ## The honest skip
 *
 * A host without an NVENC / NVDEC pair for HEVC skips (`GTEST_SKIP`)
 * with a message naming the reason.  The dedicated evidence run is
 * `.github/workflows/hardware.yml` on the `gpu-nvenc` runner, where a
 * skip is visible as a skip rather than hidden inside a green suite.
 * The *software-decoder* half of Q1b stays deferred: there is still no
 * software H.265 decoder to assert on.
 *
 * ## The defect this test found
 *
 * The H.265 packetizer set the RTP marker bit unconditionally on the AP
 * aggregation packet and on the last fragment of every fragmented NAL
 * unit.  The NVENC HEVC output for an IDR frame is
 * `VPS | SPS | PPS | <oversized IDR slice>`, so the AP packet carrying
 * the three parameter sets was marked as "the last packet of the frame".
 * The receiver's jitter buffer therefore assembled the frame as
 * "VPS + SPS + PPS", emitted it, and dropped the IDR slice that followed
 * (it landed in an already-played-out frame).  NVDEC was handed a
 * bitstream with no picture data and never produced a frame.
 *
 * The fix is in `src/modules/video_pipeline/src/video_pipeline.cpp`
 * (`emit_h265()`); see PR #144.  This test is the *engine-level* proof
 * that the fix holds on the real hardware path.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/h265/hw_backends.hpp>
#include <nimrtc/plugins/hw_video_backend.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/plugins/video_sink.hpp>
#include <nimrtc/video_frame/frame.hpp>

namespace {

namespace np = nimrtc::plugins;
namespace nd = nimrtc::dtls;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

/** The geometry the stream starts at. */
constexpr std::uint32_t kSwitchWidth  = 320;
constexpr std::uint32_t kSwitchHeight = 240;
/** The geometry the stream switches to, mid-stream. */
constexpr std::uint32_t kSwitchedWidth  = 640;
constexpr std::uint32_t kSwitchedHeight = 480;

/** The H.265 payload type RFC 7798 §4.1 recommends (and Chrome uses). */
constexpr std::uint8_t kVideoPt = 116;

// ---------------------------------------------------------------------------
// The hardware probe
// ---------------------------------------------------------------------------

/** True iff the HEVC backend table offers a usable hardware *encoder*.
 *
 *  The probe asks the *same* question the registry asks, so the test and
 *  the engine can never disagree about what the host can do. */
bool HardwareEncoderAvailable() {
    nimrtc::h265::register_default_video_backends();
    np::VideoCodecConfig probe{};
    probe.width  = kSwitchWidth;
    probe.height = kSwitchHeight;
    probe.fps    = 30;
    probe.codec  = np::VideoCodecKind::kH265;
    for (const auto* be : np::HwVideoBackendRegistry::instance()
                             .list_encoders(np::VideoCodecKind::kH265)) {
        if (be != nullptr && be->is_hw && be->available && be->available(probe)) {
            return true;
        }
    }
    return false;
}

/** True iff the HEVC backend table offers a usable hardware *decoder*.
 *
 *  The engine holds **one** codec instance per session and drives both
 *  directions through it, and the `"hardware"` pin (plan B2) is honoured
 *  in both: a host that has a hardware encoder but no hardware decoder
 *  cannot open a hardware-pinned engine at all — the codec factory
 *  refuses to substitute the software decoder. */
bool HardwareDecoderAvailable() {
    nimrtc::h265::register_default_video_backends();
    np::VideoCodecConfig probe{};
    probe.width  = kSwitchWidth;
    probe.height = kSwitchHeight;
    probe.fps    = 30;
    probe.codec  = np::VideoCodecKind::kH265;
    for (const auto* be : np::HwVideoBackendRegistry::instance()
                             .list_decoders(np::VideoCodecKind::kH265)) {
        if (be != nullptr && be->is_hw && be->available && be->available(probe)) {
            return true;
        }
    }
    return false;
}

/** The reason the table gives for the first *unavailable* hardware
 *  encoder — used to make the skip message honest. */
std::string HardwareEncoderUnavailableReason() {
    nimrtc::h265::register_default_video_backends();
    np::VideoCodecConfig probe{};
    probe.width  = kSwitchWidth;
    probe.height = kSwitchHeight;
    probe.fps    = 30;
    probe.codec  = np::VideoCodecKind::kH265;
    for (const auto* be : np::HwVideoBackendRegistry::instance()
                             .list_encoders(np::VideoCodecKind::kH265)) {
        if (be == nullptr || !be->is_hw) continue;
        if (be->available && be->available(probe)) return {};
        if (be->unavailable_reason) {
            const std::string why = be->unavailable_reason(probe);
            if (!why.empty()) return why;
        }
    }
    return "no hardware H.265 encoder row is available on this host";
}

/** The reason the table gives for the first *unavailable* hardware
 *  decoder — used to make the skip message honest. */
std::string HardwareDecoderUnavailableReason() {
    nimrtc::h265::register_default_video_backends();
    np::VideoCodecConfig probe{};
    probe.width  = kSwitchWidth;
    probe.height = kSwitchHeight;
    probe.fps    = 30;
    probe.codec  = np::VideoCodecKind::kH265;
    for (const auto* be : np::HwVideoBackendRegistry::instance()
                             .list_decoders(np::VideoCodecKind::kH265)) {
        if (be == nullptr || !be->is_hw) continue;
        if (be->available && be->available(probe)) return {};
        if (be->unavailable_reason) {
            const std::string why = be->unavailable_reason(probe);
            if (!why.empty()) return why;
        }
    }
    return "no hardware H.265 decoder row is available on this host";
}

// ---------------------------------------------------------------------------
// Pixel-diff helpers
// ---------------------------------------------------------------------------

/** Mean absolute error between two equally sized luma planes. */
double mean_abs_error(const std::vector<std::uint8_t>& a,
                      const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 255.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const int diff = static_cast<int>(a[i]) - static_cast<int>(b[i]);
        sum += (diff < 0 ? -diff : diff);
    }
    return sum / static_cast<double>(a.size());
}

/** Pearson correlation of two equally sized luma planes. */
double pearson_correlation(const std::vector<std::uint8_t>& a,
                           const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0;
    double sum_a = 0.0, sum_b = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        sum_a += static_cast<double>(a[i]);
        sum_b += static_cast<double>(b[i]);
    }
    const double n = static_cast<double>(a.size());
    const double mean_a = sum_a / n;
    const double mean_b = sum_b / n;
    double cov = 0.0, var_a = 0.0, var_b = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double da = static_cast<double>(a[i]) - mean_a;
        const double db = static_cast<double>(b[i]) - mean_b;
        cov  += da * db;
        var_a += da * da;
        var_b += db * db;
    }
    if (var_a <= 0.0 || var_b <= 0.0) return 0.0;
    return cov / (std::sqrt(var_a) * std::sqrt(var_b));
}

// ---------------------------------------------------------------------------
// Fake ICE transport — one instance per engine
// ---------------------------------------------------------------------------

class FakeIceTransport final : public np::IICETransport {
public:
    const char* name() const noexcept override { return "h265-fake-ice"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    void set_callbacks(np::RecvCallback on_recv,
                       np::ErrorCallback on_error) noexcept override {
        on_recv_  = std::move(on_recv);
        on_error_ = std::move(on_error);
    }

    np::Status send(np::BufferView view, np::Addr = {}) noexcept override {
        sent_.emplace_back(view.data(), view.data() + view.size());
        return np::kOk;
    }

    int recv() noexcept override { return 0; }

    np::Addr local_addr() const noexcept override { return {}; }
    np::Addr remote_addr() const noexcept override { return {}; }

    np::IceState state() const noexcept override { return np::IceState::Connected; }
    std::string local_ufrag() const noexcept override { return "h265ufrag"; }
    std::string local_password() const noexcept override { return "h265password"; }
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

    const std::vector<std::vector<std::uint8_t>>& sent() const noexcept {
        return sent_;
    }

private:
    np::RecvCallback   on_recv_;
    np::ErrorCallback  on_error_;
    std::vector<std::vector<std::uint8_t>> sent_;
};

class FakeIceFactory final : public np::IICETransportFactory {
public:
    FakeIceFactory(std::string_view id, std::string_view name)
        : id_(id), name_(name) {}

    std::string_view id() const noexcept override { return id_; }
    std::string_view display_name() const noexcept override { return name_; }

    np::IICETransport* create_ice() const override {
        auto* t = new FakeIceTransport();
        instance_ = t;
        return t;
    }

    FakeIceTransport* instance() const noexcept { return instance_; }

private:
    std::string_view id_;
    std::string_view name_;
    mutable FakeIceTransport* instance_ = nullptr;
};

// ---------------------------------------------------------------------------
// Fake DTLS session — connected, but no SRTP keys
// ---------------------------------------------------------------------------

/**
 * @brief A DTLS session that reports "connected" but exports no SRTP keys.
 *
 * The RTP therefore travels in cleartext, which is what lets the test read
 * the wire bytes.  (The SRTP protection path has its own tests; this file
 * is about the *media* path.)
 */
class FakeDtlsSession final : public nd::IDtlsSession {
public:
    void set_role(nd::Role) noexcept override {}
    void set_peer_fingerprint(std::span<const std::uint8_t>) noexcept override {}
    void start() noexcept override {}
    void pump() noexcept override {}
    void on_handshake_complete(OnCompleteCb) noexcept override {}
    np::Status export_srtp_key_material(std::span<std::uint8_t, 60>) noexcept override {
        return np::kErrNotReady;
    }
    nimrtc::core::Result<void> open() noexcept override {
        return nimrtc::core::Result<void>::make_ok();
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

private:
    nd::Fingerprint fingerprint_{"sha-256", {}, {}, ""};
};

class FakeDtlsFactory final : public nd::IDtlsSessionFactory {
public:
    std::string_view id() const noexcept override { return "h265-fake-dtls"; }
    std::string_view display_name() const noexcept override {
        return "Fake DTLS session (H.265 engine-level test)";
    }
    std::unique_ptr<nd::IDtlsSession> create(const nd::DtlsConfig&) const override {
        return std::make_unique<FakeDtlsSession>();
    }
};

// ---------------------------------------------------------------------------
// The recording sink
// ---------------------------------------------------------------------------

/** The decoded frame the sink kept. */
struct RecordedFrame {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> luma;
};

class H265RecordingSink final : public np::IVideoSink {
public:
    using np::IVideoSink::render;

    explicit H265RecordingSink(np::VideoSinkConfig cfg) : cfg_(std::move(cfg)) {}

    const char* name() const noexcept override { return "H265RecordingSink"; }
    np::Status open() noexcept override {
        open_ = true;
        return np::kOk;
    }
    void close() noexcept override { open_ = false; }

    np::Status render(const np::VideoSourceFrame& frame) noexcept override {
        if (!open_) return np::kErrNotReady;
        if (!frame.plane_y || frame.width == 0 || frame.height == 0) {
            return np::kErrInvalidParam;
        }
        const std::int32_t stride = frame.stride_y > 0
            ? frame.stride_y
            : static_cast<std::int32_t>(frame.width);
        RecordedFrame rec;
        rec.width  = frame.width;
        rec.height = frame.height;
        rec.luma.resize(static_cast<std::size_t>(frame.width) * frame.height);
        for (std::uint32_t row = 0; row < frame.height; ++row) {
            std::memcpy(rec.luma.data() + static_cast<std::size_t>(row) * frame.width,
                        frame.plane_y + static_cast<std::size_t>(row) * stride,
                        frame.width);
        }
        frames_.push_back(std::move(rec));
        ++rendered_;
        return np::kOk;
    }

    np::Status render(const np::GpuBuffer&) noexcept override {
        // The hardware decode path delivers host pixels through the CPU
        // overload here; a GPU surface render is the zero-copy path's
        // business and has its own tests.
        return np::kErrUnsupported;
    }

    np::VideoSinkStats stats() const noexcept override {
        np::VideoSinkStats s;
        s.frames_rendered = rendered_;
        return s;
    }

    np::VideoSinkConfig config() const noexcept override { return cfg_; }

    const std::vector<RecordedFrame>& frames() const noexcept { return frames_; }

private:
    np::VideoSinkConfig cfg_{};
    std::vector<RecordedFrame> frames_;
    std::uint64_t rendered_ = 0;
    bool open_ = false;
};

class H265RecordingSinkFactory final : public np::IVideoSinkFactory {
public:
    explicit H265RecordingSinkFactory(std::string_view id) : id_(id) {}

    H265RecordingSink* instance() const noexcept { return instance_; }

    std::string_view id() const noexcept override { return id_; }
    std::string_view display_name() const noexcept override {
        return "Recording sink (H.265 engine-level test)";
    }
    np::IVideoSink* create(np::VideoSinkConfig cfg) const override {
        auto* s = new H265RecordingSink(std::move(cfg));
        instance_ = s;
        return s;
    }

private:
    std::string_view id_;
    mutable H265RecordingSink* instance_ = nullptr;
};

// ---------------------------------------------------------------------------
// The shared plugin wiring
// ---------------------------------------------------------------------------

FakeIceFactory g_tx_ice("h265-ice-tx", "Fake ICE (H.265 TX)");
FakeIceFactory g_rx_ice("h265-ice-rx", "Fake ICE (H.265 RX)");
H265RecordingSinkFactory g_tx_sink("h265-recording-tx");
H265RecordingSinkFactory g_rx_sink("h265-recording-rx");

void RegisterH265Plugins() {
    nimrtc::core::register_all_default_plugins();

    static FakeDtlsFactory dtls;

    PluginRegistry::instance().register_ice_transport(g_tx_ice.id(), &g_tx_ice);
    PluginRegistry::instance().register_ice_transport(g_rx_ice.id(), &g_rx_ice);
    PluginRegistry::instance().register_dtls_session(dtls.id(), &dtls);
    PluginRegistry::instance().register_video_sink(g_tx_sink.id(), &g_tx_sink);
    PluginRegistry::instance().register_video_sink(g_rx_sink.id(), &g_rx_sink);
}

/**
 * @brief The engine configuration used by both engines.
 *
 * `video_codec_name = "h265"` selects the shipped H.265 plugin, whose
 * factory pairs the NVENC encoder with the NVDEC decoder into a
 * `HwCompositeCodec` (the two hardware backends are one-directional, and
 * the engine holds a single codec instance for both directions).
 *
 * `video_codec_backend = "hardware"` is the B2 pin: no software fallback.
 */
EngineConfig make_config() {
    EngineConfig cfg{};
    cfg.transport_strategy_name   = "";
    cfg.transport_name            = "h265-ice-tx";
    cfg.dtls_name                 = "h265-fake-dtls";
    cfg.datachannel_backend       = "";
    cfg.bwe_name                  = "";
    cfg.scheduler_config.enabled  = false;

    cfg.video_codec_name    = "h265";
    cfg.video_codec_backend = "hardware";
    cfg.video_sender_name   = "reference";
    cfg.video_receiver_name = "reference";
    cfg.video_source_name   = "memory";
    cfg.video_sink_name     = "h265-recording-tx";
    cfg.video_capture_name.clear();   // the host drives the frames
    cfg.video_capture_pump  = false;
    cfg.video_width         = kSwitchWidth;
    cfg.video_height        = kSwitchHeight;
    cfg.video_fps           = 30;
    return cfg;
}

// ---------------------------------------------------------------------------
// The source picture
// ---------------------------------------------------------------------------

/**
 * @brief Build an I420 source frame of the given geometry.
 *
 * The picture is a *ramp* rather than a flat colour, so the encoder has to
 * code genuine spatial structure: a blank frame would decode to a blank
 * frame at any geometry, and the test could not tell "the new picture was
 * decoded" from "the decoder produced something of the right size".
 */
np::VideoSourceFrame make_i420_frame(std::uint32_t width,
                                     std::uint32_t height,
                                     std::vector<std::uint8_t>& storage,
                                     std::int64_t capture_ts_us,
                                     std::uint32_t rtp_timestamp,
                                     std::uint32_t frame_seq) {
    const std::size_t luma_size = static_cast<std::size_t>(width) * height;
    const std::size_t chroma_size =
        static_cast<std::size_t>((width + 1u) / 2u) * ((height + 1u) / 2u);
    storage.assign(luma_size + 2u * chroma_size, 0);

    for (std::uint32_t row = 0; row < height; ++row) {
        std::uint8_t* p = storage.data() + static_cast<std::size_t>(row) * width;
        for (std::uint32_t col = 0; col < width; ++col) {
            p[col] = static_cast<std::uint8_t>(
                32 + ((col * 160) / (width == 0 ? 1u : width)));
        }
    }
    // A neutral chroma: the picture is grey, so the chroma planes carry
    // no signal and a chroma error cannot mask a luma error.
    std::memset(storage.data() + luma_size, 128, 2u * chroma_size);

    np::VideoSourceFrame frame{};
    frame.width    = width;
    frame.height   = height;
    frame.format   = np::VideoPixelFormat::kI420;
    frame.stride_y = static_cast<std::int32_t>(width);
    frame.stride_u = static_cast<std::int32_t>((width + 1u) / 2u);
    frame.stride_v = static_cast<std::int32_t>((width + 1u) / 2u);
    frame.plane_y  = storage.data();
    frame.plane_u  = storage.data() + luma_size;
    frame.plane_v  = storage.data() + luma_size + chroma_size;
    frame.buffer_size = storage.size();
    frame.capture_ts_us = capture_ts_us;
    frame.rtp_timestamp = rtp_timestamp;
    frame.frame_seq     = frame_seq;
    return frame;
}

/** The reference luma plane of the ramp picture at @p width x @p height. */
std::vector<std::uint8_t> reference_luma(std::uint32_t width,
                                          std::uint32_t height) {
    std::vector<std::uint8_t> y(static_cast<std::size_t>(width) * height);
    for (std::uint32_t row = 0; row < height; ++row) {
        for (std::uint32_t col = 0; col < width; ++col) {
            y[static_cast<std::size_t>(row) * width + col] =
                static_cast<std::uint8_t>(
                    32 + ((col * 160) / (width == 0 ? 1u : width)));
        }
    }
    return y;
}

/** True iff the datagram looks like an RTCP PLI (RFC 4585 §6.3.1). */
bool is_pli_datagram(const std::vector<std::uint8_t>& d) {
    if (d.size() < 2) return false;
    const std::uint8_t pt2 = d[1];
    if (pt2 < 192 || pt2 > 223) return false;   // not RTCP at all
    return pt2 == 206 && (d[0] & 0x1Fu) == 1;   // PSFB, FMT = 1 = PLI
}

class EngineVideoHwH265 : public ::testing::Test {
protected:
    static void SetUpTestSuite() { RegisterH265Plugins(); }
};

}  // namespace

// ===========================================================================
// Q1b: the engine-level H.265 parameter-set switch
// ===========================================================================

TEST_F(EngineVideoHwH265, AMidStreamParameterSetSwitchDecodesWithoutAKeyFrameRoundTrip) {
    if (!HardwareEncoderAvailable()) {
        GTEST_SKIP() << "no hardware H.265 encoder on this host — "
                     << HardwareEncoderUnavailableReason();
    }
    if (!HardwareDecoderAvailable()) {
        GTEST_SKIP() << "no hardware H.265 decoder on this host — "
                     << HardwareDecoderUnavailableReason();
    }

    // ---- The receiving engine -------------------------------------------
    //
    // It never captures: it only receives.  Its own decode path is the
    // real one (the engine's receive callback → depacketizer → jitter
    // buffer → NVDEC → sink), so the assertion below is about the
    // production path, not about a test double.
    EngineConfig rx_cfg = make_config();
    rx_cfg.transport_name  = "h265-ice-rx";
    rx_cfg.video_sink_name = "h265-recording-rx";
    rx_cfg.video_width     = kSwitchWidth;
    rx_cfg.video_height    = kSwitchHeight;

    NimRTCEngine rx_engine(rx_cfg);
    ASSERT_EQ(rx_engine.open(), 0u) << "the receiving engine must open";

    // ---- The transmitting engine ----------------------------------------
    EngineConfig tx_cfg = make_config();
    tx_cfg.video_width  = kSwitchWidth;
    tx_cfg.video_height = kSwitchHeight;

    NimRTCEngine tx_engine(tx_cfg);
    ASSERT_EQ(tx_engine.open(), 0u) << "the transmitting engine must open";

    FakeIceTransport* tx_ice = g_tx_ice.instance();
    ASSERT_NE(tx_ice, nullptr);
    FakeIceTransport* rx_ice = g_rx_ice.instance();
    ASSERT_NE(rx_ice, nullptr);

    // ---- The first geometry ---------------------------------------------
    //
    // The host pushes the frames itself (`send_video()`), so the RTP
    // timestamp is the host's own media clock: two distinct timestamps
    // per geometry, so the jitter buffer sees two *separate* frames
    // rather than two packets of one picture.
    std::vector<std::uint8_t> storage;
    {
        const auto frame = make_i420_frame(kSwitchWidth, kSwitchHeight,
                                           storage, /*capture_ts_us=*/1000,
                                           /*rtp_timestamp=*/9000,
                                           /*frame_seq=*/1);
        ASSERT_EQ(tx_engine.send_video(frame), np::kOk)
            << "the first frame must be accepted";
    }

    for (int i = 0; i < 5; ++i) tx_engine.tick();

    const auto tx_stats_after_first = tx_engine.stats();
    EXPECT_GE(tx_stats_after_first.video_frames_sent, 1u)
        << "the first picture must have reached the sender";

    // ---- The geometry changes mid-stream --------------------------------
    //
    // This is the whole point of the test: the *frame* is the authority
    // on what is being sent (remote-desktop plan B13), so pushing a
    // frame at a new geometry must make the engine restart the encoder
    // and put the new VPS / SPS / PPS on the wire with the next frame.
    {
        const auto frame = make_i420_frame(kSwitchedWidth, kSwitchedHeight,
                                           storage, /*capture_ts_us=*/40000,
                                           /*rtp_timestamp=*/18000,
                                           /*frame_seq=*/2);
        ASSERT_EQ(tx_engine.send_video(frame), np::kOk)
            << "the frame at the new geometry must be accepted";
    }

    for (int i = 0; i < 5; ++i) tx_engine.tick();

    // The encoder must have produced bitstream for *both* pictures.  The
    // second one is the interesting one: it is the picture that follows
    // the geometry change, so it is the one that carries the new
    // parameter sets.
    EXPECT_GE(tx_engine.stats().video_tx_frames_encoded, 2u)
        << "the encoder did not produce both pictures — the switched "
           "picture never reached the wire";

    // ---- Hand the wire bytes to the receiving engine --------------------
    //
    // The *whole* datagram list is handed over, RTCP included: the
    // receiver's own feedback has to be able to come back to the sender
    // (that is what makes the "no key-frame round-trip" assertion
    // meaningful — a PLI that went out and came back would show up).
    const auto& datagrams = tx_ice->sent();
    std::size_t rtp_packets = 0;
    for (const auto& d : datagrams) {
        if (d.size() >= 2) {
            const std::uint8_t pt = d[1] & 0x7Fu;
            if (pt == kVideoPt) ++rtp_packets;
        }
        rx_engine.feed_media_inbound(d.data(), d.size());
    }
    ASSERT_GT(rtp_packets, 0u)
        << "the encoder's pictures must have reached the wire as RTP";

    // ---- Give the RX engine a few ticks to drain its jitter buffer ------
    for (int i = 0; i < 20; ++i) rx_engine.tick();

    const auto rx_stats = rx_engine.stats();

    // ---- The assertion ---------------------------------------------------
    //
    // (a) The decode-failure door never opened.  This is the "no
    //     key-frame request round-trip" half: the receiver's decoder
    //     accepted the new picture, so it never asked for a key frame.
    EXPECT_EQ(rx_stats.video_feedback_pli_from_decode_failure, 0u)
        << "the receiver asked for a key frame after the parameter-set "
           "switch — the switch must decode on its own, with no PLI "
           "round-trip";

    // (b) Nothing went out on the wire as a PLI either.  This is the
    //     independent check: the counter above is an *internal* fact, and
    //     this one is what actually went on the wire.
    {
        std::size_t plis = 0;
        for (const auto& d : rx_ice->sent()) {
            if (is_pli_datagram(d)) ++plis;
        }
        EXPECT_EQ(plis, 0u)
            << "a PLI went out on the wire after the parameter-set switch";
    }

    // (c) The decoder dropped nothing.
    EXPECT_EQ(rx_stats.video_frames_dropped_decoder, 0u)
        << "the decoder refused a frame during the parameter-set switch";

    // (d) The picture really decoded — at the *new* geometry.
    ASSERT_GE(rx_stats.video_frames_decoded, 1u)
        << "the receiver must have decoded the switched picture";
    EXPECT_GE(rx_stats.video_frames_rendered, 1u)
        << "the decoded picture must have reached the sink";

    H265RecordingSink* rx_sink = g_rx_sink.instance();
    ASSERT_NE(rx_sink, nullptr);
    const auto& frames = rx_sink->frames();
    ASSERT_FALSE(frames.empty())
        << "the sink must have received the decoded picture";

    // The last decoded frame carries the *new* geometry: a decoder that
    // kept the old parameter sets would report the old size.
    const RecordedFrame& last = frames.back();
    EXPECT_EQ(last.width, kSwitchedWidth)
        << "the decoded picture does not carry the new geometry — the "
           "decoder did not follow the new parameter sets";
    EXPECT_EQ(last.height, kSwitchedHeight)
        << "the decoded picture does not carry the new geometry — the "
           "decoder did not follow the new parameter sets";

    // (e) The new picture carries real signal — a correctly-sized but
    //     blank frame would pass a geometry-only assertion.
    {
        const auto [min_it, max_it] =
            std::minmax_element(last.luma.begin(), last.luma.end());
        EXPECT_GT(static_cast<unsigned>(*max_it - *min_it), 30u)
            << "the decoded picture after the switch is a constant — it "
               "carries no signal";
    }

    // (f) The *old* geometry was decoded too — the switch did not break
    //     the stream that came before it.  The first frame was pushed
    //     before the switch, so the receiver decoded it with the *old*
    //     parameter sets, and the second one with the *new* ones: both
    //     pictures went through the same warm decoder.
    ASSERT_GE(frames.size(), 2u)
        << "both pictures must have been decoded — the switch test is "
           "only meaningful on a decoder that was already working";
    EXPECT_EQ(frames.front().width, kSwitchWidth);
    EXPECT_EQ(frames.front().height, kSwitchHeight);

    // (g) The picture is the *right* picture — a decoder that kept the
    //     old parameter sets would render the *old* picture at the new
    //     size, and only a pixel comparison catches that.
    //
    //     The reference is the source frame the host pushed at the new
    //     geometry, so the comparison is against what was *captured*,
    //     not against another decoded frame.
    {
        const auto reference = reference_luma(kSwitchedWidth, kSwitchedHeight);
        ASSERT_EQ(last.luma.size(), reference.size())
            << "the decoded luma plane does not have the expected size";

        const double mae = mean_abs_error(reference, last.luma);
        EXPECT_LT(mae, 12.0)
            << "the decoded picture after the switch is not the captured "
               "picture (mean absolute luma error " << mae << ")";

        const double corr = pearson_correlation(reference, last.luma);
        EXPECT_GT(corr, 0.95)
            << "the decoded picture after the switch does not correlate "
               "with the captured picture (Pearson r = " << corr << ")";

        // The negative control for *those* thresholds: a flat grey frame
        // must fail both, or they prove nothing.
        const std::vector<std::uint8_t> blank(reference.size(), 128);
        EXPECT_GE(mean_abs_error(reference, blank), 12.0)
            << "the MAE threshold is too loose: a flat grey frame passes it";
        EXPECT_LE(pearson_correlation(reference, blank), 0.95)
            << "the Pearson threshold is too loose: a flat grey frame "
               "passes it";
    }

    tx_engine.close();
    rx_engine.close();
}
