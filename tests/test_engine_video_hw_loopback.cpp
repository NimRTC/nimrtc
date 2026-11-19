/**
 * @file tests/test_engine_video_hw_loopback.cpp
 * @brief The engine-level hardware-encode test (v1.3 plan Q7 / O-6).
 *
 * ## What this test proves
 *
 * `test_engine_video_e2e_loopback.cpp` proves the *software* media path:
 * pixels → OpenH264 → RTP → depacketize → OpenH264 → pixels.  This file
 * proves the same chain for the **hardware** path, and it proves it
 * where the engine's own telemetry can see it:
 *
 *   - the engine is opened with `video_codec_backend = "hardware"`, which
 *     (plan B2) **fails** rather than falling back to software when no
 *     hardware encoder exists — so a software fallback cannot make this
 *     test pass;
 *   - `Stats::video_codec_backend_preference` must read `"hardware"` (the
 *     host asked) and `Stats::video_codec_hardware_accelerated` must be
 *     true (the engine got it);
 *   - `Stats::video_codec_backend` must be a *hardware* backend id
 *     (e.g. `"nvenc"`), never the software stub (`"software-stub"`) or
 *     the OpenH264 backend (`"openh264"`) — the two ids the software
 *     path would report;
 *   - the picture the receiving engine renders must still be the picture
 *     the transmitter captured (the same luma MAE / Pearson thresholds
 *     the software e2e test uses).
 *
 * ## Honest skip
 *
 * A machine without a hardware encoder skips (`GTEST_SKIP`) with a
 * message naming the reason the backend table gave.  The dedicated
 * evidence run is `examples/hw-e2e` + `tools/run_hardware_e2e.py`
 * (`.github/workflows/hardware.yml`), where a skip is visible as a skip
 * rather than hidden inside a green suite.
 *
 * The *decoder* is probed too, and for the same reason the encoder is.
 * The engine holds **one** codec instance per session and drives both
 * directions through it, and the `"hardware"` pin (plan B2) is honoured
 * in both: a host that has a hardware encoder but no hardware decoder
 * cannot open a hardware-pinned engine at all — the codec factory
 * refuses to substitute the software decoder, and `open()` reports
 * `kVideoCodecPluginMissing` (6705).  Such a host skips here, with the
 * decoder's own reason, rather than failing a test that is really
 * asserting "this machine has no hardware H.264 decoder".
 *
 * (On macOS / Apple Silicon the hardware *decoder* used to be
 * unavailable: `VTDecompressionSessionCreate` rejects a format
 * description built from a bare codec type with `os=-8971`
 * (`codecExtensionNotFoundErr`), because such a description carries no
 * `avcC` atom.  The decoder now asks
 * `VTIsHardwareDecodeSupported()` whether the host has a hardware
 * decoder at all, and creates the session from the *stream's own*
 * parameter sets when they arrive — so the row is live there too.  The
 * encoder is unaffected either way.  The dedicated hardware job is
 * where the end-to-end claim is gated.)
 *
 * ## Why a hardware *encode* and any decode is enough
 *
 * Q7's wording is "a frame was captured, encoded by the hardware codec,
 * transmitted, decoded, and rendered, and the pipeline reported hardware
 * encoding".  The decode leg is asserted to *happen* (frames decoded and
 * rendered); its acceleration is reported (`video_codec_backend` is the
 * composite's encoder-side id) but not required, because a host whose
 * silicon lacks a decode engine still proves the encode path.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/h264/hw_backends.hpp>
#include <nimrtc/plugins/hw_video_backend.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/plugins/video_capture.hpp>
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

// 1080p, like the software e2e test: the hardware session's rate control
// and macroblock layout only get exercised at the size the product runs
// at.  (Some hardware encoders pad odd / tiny geometries internally;
// 1080p is what the criterion names.)
constexpr std::uint32_t kWidth  = 1920;
constexpr std::uint32_t kHeight = 1080;

constexpr std::uint8_t kVideoPt = 102;

// ---------------------------------------------------------------------------
// The hardware probe
// ---------------------------------------------------------------------------

/** True iff the H.264 backend table offers a usable hardware encoder. */
bool HardwareEncoderAvailable() {
    nimrtc::h264::register_default_video_backends();
    np::VideoCodecConfig probe{};
    probe.width  = kWidth;
    probe.height = kHeight;
    probe.fps    = 30;
    probe.codec  = np::VideoCodecKind::kH264;
    for (const auto* be : np::HwVideoBackendRegistry::instance()
                             .list_encoders(np::VideoCodecKind::kH264)) {
        if (be != nullptr && be->is_hw && be->available && be->available(probe)) {
            return true;
        }
    }
    return false;
}

/** The reason the table gives for the first *unavailable* hardware
 *  encoder — used to make the skip message honest. */
std::string HardwareEncoderUnavailableReason() {
    nimrtc::h264::register_default_video_backends();
    np::VideoCodecConfig probe{};
    probe.width  = kWidth;
    probe.height = kHeight;
    probe.fps    = 30;
    probe.codec  = np::VideoCodecKind::kH264;
    for (const auto* be : np::HwVideoBackendRegistry::instance()
                             .list_encoders(np::VideoCodecKind::kH264)) {
        if (be == nullptr || !be->is_hw) continue;
        if (be->available && be->available(probe)) return {};
        if (be->unavailable_reason) {
            const std::string why = be->unavailable_reason(probe);
            if (!why.empty()) return why;
        }
    }
    return "no hardware H.264 encoder row is available on this host";
}

/** True iff the H.264 backend table offers a usable hardware *decoder*.
 *
 *  The engine's `video_codec_backend = "hardware"` pin (plan B2) demands
 *  a hardware backend in *both* directions: the encoder sends, the
 *  decoder receives, and the engine holds one codec instance for the
 *  two.  A host that has a hardware encoder but no hardware decoder
 *  therefore cannot open a hardware-pinned engine at all — the codec
 *  factory refuses to fall back to software — so the test has to know
 *  about the decoder before it asserts anything.
 *
 *  The probe asks the *same* question the registry asks, so the test
 *  and the engine can never disagree about what the host can do. */
bool HardwareDecoderAvailable() {
    nimrtc::h264::register_default_video_backends();
    np::VideoCodecConfig probe{};
    probe.width  = kWidth;
    probe.height = kHeight;
    probe.fps    = 30;
    probe.codec  = np::VideoCodecKind::kH264;
    for (const auto* be : np::HwVideoBackendRegistry::instance()
                             .list_decoders(np::VideoCodecKind::kH264)) {
        if (be != nullptr && be->is_hw && be->available && be->available(probe)) {
            return true;
        }
    }
    return false;
}

/** The reason the table gives for the first *unavailable* hardware
 *  decoder — used to make the skip message honest. */
std::string HardwareDecoderUnavailableReason() {
    nimrtc::h264::register_default_video_backends();
    np::VideoCodecConfig probe{};
    probe.width  = kWidth;
    probe.height = kHeight;
    probe.fps    = 30;
    probe.codec  = np::VideoCodecKind::kH264;
    for (const auto* be : np::HwVideoBackendRegistry::instance()
                             .list_decoders(np::VideoCodecKind::kH264)) {
        if (be == nullptr || !be->is_hw) continue;
        if (be->available && be->available(probe)) return {};
        if (be->unavailable_reason) {
            const std::string why = be->unavailable_reason(probe);
            if (!why.empty()) return why;
        }
    }
    return "no hardware H.264 decoder row is available on this host";
}

// ---------------------------------------------------------------------------
// The synthetic picture (the same pattern `examples/hw-e2e` paints)
// ---------------------------------------------------------------------------
//
// A horizontal ramp plus a hard *vertical* edge at a quarter of the
// width.  The edge is what the pattern is for (a broken codec smooths it
// away), and it costs only the block columns that cross it.  The earlier
// horizontal stripe pattern put an edge in every block *row*, which the
// quantiser renders inexactly at horizontal block boundaries — at 1080p
// that pattern artefact alone pushed the luma MAE past the threshold on
// a healthy pipeline.

void paint_pattern(std::uint8_t* bgra, std::uint32_t width,
                   std::uint32_t height) {
    for (std::uint32_t row = 0; row < height; ++row) {
        std::uint8_t* p = bgra + static_cast<std::size_t>(row) * width * 4u;
        for (std::uint32_t col = 0; col < width; ++col) {
            std::int32_t value = 32 + static_cast<std::int32_t>(
                (col * 160) / static_cast<std::int32_t>(width > 1 ? width - 1 : 1));
            if (col == width / 4) value = std::clamp(value + 160, 16, 235);
            value = std::clamp(value, 16, 235);
            p[col * 4u + 0u] = static_cast<std::uint8_t>(value);
            p[col * 4u + 1u] = static_cast<std::uint8_t>(value);
            p[col * 4u + 2u] = static_cast<std::uint8_t>(value);
            p[col * 4u + 3u] = 255;
        }
    }
}

/** The reference luma plane (BT.601 limited range, the engine's own
 *  `convert_bgra_to_i420` transform). */
std::vector<std::uint8_t> reference_luma() {
    std::vector<std::uint8_t> bgra(
        static_cast<std::size_t>(kWidth) * kHeight * 4u);
    paint_pattern(bgra.data(), kWidth, kHeight);
    std::vector<std::uint8_t> y(static_cast<std::size_t>(kWidth) * kHeight);
    for (std::size_t i = 0; i < y.size(); ++i) {
        const std::int32_t b = bgra[i * 4u + 0u];
        const std::int32_t g = bgra[i * 4u + 1u];
        const std::int32_t r = bgra[i * 4u + 2u];
        const std::int32_t lum = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
        y[i] = static_cast<std::uint8_t>(lum < 0 ? 0 : (lum > 255 ? 255 : lum));
    }
    return y;
}

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
        cov += da * db;
        var_a += da * da;
        var_b += db * db;
    }
    if (var_a <= 0.0 || var_b <= 0.0) return 0.0;
    return cov / (std::sqrt(var_a) * std::sqrt(var_b));
}

// ---------------------------------------------------------------------------
// The fake wire (the same seam the software e2e test uses)
// ---------------------------------------------------------------------------

class FakeIceTransport final : public np::IICETransport {
public:
    const char* name() const noexcept override { return "hw-fake-ice"; }
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
    std::string local_ufrag() const noexcept override { return "hwe2eufrag"; }
    std::string local_password() const noexcept override { return "hwe2epassword"; }
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
    std::string_view id() const noexcept override { return "hw-fake-dtls"; }
    std::string_view display_name() const noexcept override {
        return "Fake DTLS session (hardware e2e test)";
    }
    std::unique_ptr<nd::IDtlsSession> create(const nd::DtlsConfig&) const override {
        return std::make_unique<FakeDtlsSession>();
    }
};

class HwCaptureDevice final : public np::IVideoCapture {
public:
    explicit HwCaptureDevice(np::CaptureConfig cfg) : cfg_(std::move(cfg)) {
        pixels_.resize(static_cast<std::size_t>(kWidth) * kHeight * 4u);
        paint_pattern(pixels_.data(), kWidth, kHeight);
    }
    ~HwCaptureDevice() override { close(); }

    const char* name() const noexcept override { return "HwCaptureDevice"; }
    np::Status open() noexcept override {
        open_ = true;
        return np::kOk;
    }
    void close() noexcept override { open_ = false; }

    std::vector<np::CaptureDevice> enumerate_devices() const noexcept override {
        return {};
    }
    std::vector<np::CaptureFormat> supported_formats(
        const std::string&) const noexcept override {
        return {};
    }
    np::CaptureCapabilities capabilities() const noexcept override {
        np::CaptureCapabilities c;
        c.backend    = "hw-e2e-synthetic";
        c.max_pixels = 0;
        c.max_fps    = 0;
        return c;
    }

    bool is_open() const noexcept override { return open_; }
    np::CaptureConfig config() const noexcept override { return cfg_; }

    np::Status pop_frame(np::CaptureFrame& out) noexcept override {
        if (!open_) return np::kErrNotReady;
        if (!pending_) {
            pending_ = true;
            ++produced_;
        }
        out.pixels        = pixels_.data();
        out.size          = pixels_.size();
        out.stride        = static_cast<std::int32_t>(kWidth * 4u);
        out.width         = kWidth;
        out.height        = kHeight;
        out.format        = np::VideoPixelFormat::kBGRA;
        out.capture_ts_us = 1'000 + static_cast<std::int64_t>(produced_) * 33'333;
        out.frame_seq     = static_cast<std::uint32_t>(produced_);
        return np::kOk;
    }

    void set_frame_callback(np::CaptureFrameCallback) noexcept override {}

    np::CaptureStats stats() const noexcept override {
        np::CaptureStats s;
        s.frames_captured   = produced_;
        s.frames_cpu_staged = produced_;
        return s;
    }

private:
    np::CaptureConfig cfg_{};
    std::vector<std::uint8_t> pixels_;
    bool open_ = false;
    bool pending_ = false;
    std::uint64_t produced_ = 0;
};

class HwCaptureFactory final : public np::IVideoCaptureFactory {
public:
    std::string_view id() const noexcept override { return "hw-e2e-capture"; }
    std::string_view display_name() const noexcept override {
        return "Synthetic BGRA capture device (hardware e2e test)";
    }
    np::IVideoCapture* create(np::CaptureConfig cfg) const override {
        return new HwCaptureDevice(std::move(cfg));
    }
};

/** The decoded frame the sink kept. */
struct RecordedFrame {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> luma;
};

class HwRecordingSink final : public np::IVideoSink {
public:
    using np::IVideoSink::render;

    explicit HwRecordingSink(np::VideoSinkConfig cfg) : cfg_(cfg) {}

    const char* name() const noexcept override { return "HwRecordingSink"; }
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

class HwRecordingSinkFactory final : public np::IVideoSinkFactory {
public:
    explicit HwRecordingSinkFactory(std::string_view id) : id_(id) {}

    HwRecordingSink* instance() const noexcept { return instance_; }

    std::string_view id() const noexcept override { return id_; }
    std::string_view display_name() const noexcept override {
        return "Recording sink (hardware e2e test)";
    }
    np::IVideoSink* create(np::VideoSinkConfig cfg) const override {
        auto* s = new HwRecordingSink(std::move(cfg));
        instance_ = s;
        return s;
    }

private:
    std::string_view id_;
    mutable HwRecordingSink* instance_ = nullptr;
};

// ---------------------------------------------------------------------------
// The shared plugin wiring
// ---------------------------------------------------------------------------

FakeIceFactory g_tx_ice("hw-ice-tx", "Fake ICE (hardware TX)");
FakeIceFactory g_rx_ice("hw-ice-rx", "Fake ICE (hardware RX)");
HwRecordingSinkFactory g_tx_sink("hw-recording-tx");
HwRecordingSinkFactory g_rx_sink("hw-recording-rx");

void RegisterHwE2EPlugins() {
    nimrtc::core::register_all_default_plugins();

    static FakeDtlsFactory dtls;
    static HwCaptureFactory capture;

    PluginRegistry::instance().register_ice_transport(g_tx_ice.id(), &g_tx_ice);
    PluginRegistry::instance().register_ice_transport(g_rx_ice.id(), &g_rx_ice);
    PluginRegistry::instance().register_dtls_session(dtls.id(), &dtls);
    PluginRegistry::instance().register_video_capture(capture.id(), &capture);
    PluginRegistry::instance().register_video_sink(g_tx_sink.id(), &g_tx_sink);
    PluginRegistry::instance().register_video_sink(g_rx_sink.id(), &g_rx_sink);
}

/**
 * @brief The engine configuration used by both engines.
 *
 * `video_codec_name = "h264"` selects the shipped H.264 plugin, whose
 * factory is the one that consults the backend table — the same
 * selection an operator's session makes.  `video_codec_backend =
 * "hardware"` is the B2 pin: no software fallback.
 */
EngineConfig make_config() {
    EngineConfig cfg{};
    cfg.transport_strategy_name   = "";
    cfg.transport_name            = "hw-ice-tx";
    cfg.dtls_name                 = "hw-fake-dtls";
    cfg.datachannel_backend       = "";
    cfg.bwe_name                  = "";
    cfg.scheduler_config.enabled  = false;

    cfg.video_codec_name    = "h264";
    cfg.video_codec_backend = "hardware";
    cfg.video_sender_name   = "reference";
    cfg.video_receiver_name = "reference";
    cfg.video_source_name   = "memory";
    cfg.video_sink_name     = "hw-recording-tx";
    cfg.video_capture_name  = "hw-e2e-capture";
    cfg.video_width         = kWidth;
    cfg.video_height        = kHeight;
    cfg.video_fps           = 30;
    return cfg;
}

// ---------------------------------------------------------------------------
// The loopback driver
// ---------------------------------------------------------------------------

struct LoopbackResult {
    std::size_t tx_rtp_packets = 0;
    std::size_t rx_frames      = 0;
    std::vector<std::uint8_t> luma;
    std::uint32_t width  = 0;
    std::uint32_t height = 0;

    // The receiving engine's own counters — the "decoded and rendered"
    // half of Q7's sentence, read from the stats rather than inferred
    // from the sink.
    std::uint64_t rx_frames_decoded  = 0;
    std::uint64_t rx_frames_rendered = 0;
    std::uint64_t rx_tx_frames_encoded = 0;

    // The receiver's *loss* counters.  A frame that was dropped at the
    // jitter buffer (or by the decoder) is a different failure from a
    // frame that decoded into a partly black picture — and the two have
    // to be told apart before the picture can be blamed on the codec.
    std::uint64_t rx_frames_dropped_jb      = 0;
    std::uint64_t rx_frames_dropped_decoder = 0;
    std::uint64_t rx_bytes_received         = 0;

    // The TX engine's telemetry, copied before the engines are destroyed.
    std::string codec_backend;
    std::string codec_backend_preference;
    bool        codec_hardware = false;
    std::uint64_t codec_frames_encoded = 0;
    std::uint64_t codec_keyframes      = 0;
    std::uint64_t codec_encode_errors  = 0;

    std::uint32_t open_rc_tx = 0;
    std::string   open_error_tx;
    bool          hw_setup_failed = false;
};

LoopbackResult run_loopback() {
    LoopbackResult result;

    EngineConfig rx_cfg = make_config();
    rx_cfg.transport_name = "hw-ice-rx";
    rx_cfg.video_sink_name = "hw-recording-rx";
    rx_cfg.video_capture_name.clear();
    rx_cfg.video_capture_pump = false;

    NimRTCEngine rx_engine(rx_cfg);
    const std::uint32_t rx_open = rx_engine.open();
    EXPECT_EQ(rx_open, 0u) << "the receiving engine must open";

    EngineConfig tx_cfg = make_config();
    NimRTCEngine tx_engine(tx_cfg);
    tx_engine.set_on_error([&result](std::uint32_t code, std::string_view msg) {
        if (result.open_error_tx.empty()) {
            result.open_error_tx = std::string{msg} +
                " (code " + std::to_string(code) + ")";
        }
    });
    result.open_rc_tx = tx_engine.open();
    if (result.open_rc_tx != 0) return result;

    for (int i = 0; i < 50; ++i) {
        tx_engine.tick();
        if (tx_engine.stats().video_capture_pumped_frames >= 1) break;
    }

    {
        const auto tx_stats = tx_engine.stats();
        result.codec_backend            = tx_stats.video_codec_backend;
        result.codec_backend_preference = tx_stats.video_codec_backend_preference;
        result.codec_hardware           = tx_stats.video_codec_hardware_accelerated;
        // `FramesEncoded` is only in the nested `video` snapshot on some
        // builds; the keyframe/staged counters below come from the same
        // snapshot the encode path updates.
        result.codec_frames_encoded     = tx_stats.video_codec_frames_staged +
                                          tx_stats.video_codec_frames_zero_copy;
        result.codec_keyframes          = tx_stats.video_codec_keyframes;
        result.codec_encode_errors      = tx_stats.video_codec_encode_errors;
        result.rx_tx_frames_encoded     = tx_stats.video_tx_frames_encoded;
    }

    FakeIceTransport* tx_ice = g_tx_ice.instance();
    EXPECT_NE(tx_ice, nullptr);
    if (tx_ice == nullptr) return result;

    const auto& datagrams = tx_ice->sent();
    for (const auto& d : datagrams) {
        if (d.size() < 2) continue;
        if ((d[1] & 0x7Fu) == kVideoPt) ++result.tx_rtp_packets;
    }
    for (const auto& d : datagrams) {
        if (d.size() < 2) continue;
        if ((d[1] & 0x7Fu) != kVideoPt) continue;
        rx_engine.feed_media_inbound(d.data(), d.size());
    }
    for (int i = 0; i < 20; ++i) rx_engine.tick();

    const auto rx_stats = rx_engine.stats();
    result.rx_frames_decoded  = rx_stats.video_frames_decoded;
    result.rx_frames_rendered = rx_stats.video_frames_rendered;
    result.rx_frames_dropped_jb      = rx_stats.video_frames_dropped_jb;
    result.rx_frames_dropped_decoder = rx_stats.video_frames_dropped_decoder;
    result.rx_bytes_received         = rx_stats.video_bytes_received;
    EXPECT_GE(rx_stats.video_frames_decoded, 1u)
        << "the decoder must have decoded the hardware bitstream";

    HwRecordingSink* sink = g_rx_sink.instance();
    EXPECT_NE(sink, nullptr);
    if (sink != nullptr) {
        result.rx_frames = sink->frames().size();
        if (!sink->frames().empty()) {
            const RecordedFrame& first = sink->frames().front();
            result.luma   = first.luma;
            result.width  = first.width;
            result.height = first.height;
        }
        // ---- The diagnostic ------------------------------------------------
        //
        // Every rendered frame is compared against the reference, and the
        // per-frame numbers are printed.  A picture that is merely *shifted*
        // or *re-ranged* still correlates perfectly, so a low correlation
        // means the decoded picture has a different *shape* — and the
        // per-frame numbers say whether that is one bad frame or every
        // frame.
        const std::vector<std::uint8_t> reference = reference_luma();
        std::printf("[hw-loopback] rendered frames: %zu\n", result.rx_frames);
        std::printf("[hw-loopback] rx: decoded=%llu rendered=%llu "
                    "dropped_jb=%llu dropped_decoder=%llu bytes=%llu\n",
                    static_cast<unsigned long long>(result.rx_frames_decoded),
                    static_cast<unsigned long long>(result.rx_frames_rendered),
                    static_cast<unsigned long long>(result.rx_frames_dropped_jb),
                    static_cast<unsigned long long>(
                        result.rx_frames_dropped_decoder),
                    static_cast<unsigned long long>(result.rx_bytes_received));
        for (std::size_t i = 0; i < sink->frames().size(); ++i) {
            const RecordedFrame& f = sink->frames()[i];
            if (f.luma.size() != reference.size()) {
                std::printf("[hw-loopback] frame %zu: geometry %ux%u (the "
                            "reference is %ux%u)\n",
                            i, f.width, f.height, kWidth, kHeight);
                continue;
            }
            const double mae = mean_abs_error(reference, f.luma);
            const double corr = pearson_correlation(reference, f.luma);
            std::uint64_t min_v = 255, max_v = 0, sum = 0;
            for (const std::uint8_t v : f.luma) {
                if (v < min_v) min_v = v;
                if (v > max_v) max_v = v;
                sum += v;
            }
            std::printf("[hw-loopback] frame %zu: mae=%.3f corr=%.4f "
                        "min=%llu max=%llu mean=%.2f\n",
                        i, mae, corr,
                        static_cast<unsigned long long>(min_v),
                        static_cast<unsigned long long>(max_v),
                        static_cast<double>(sum) /
                            static_cast<double>(f.luma.size()));
        }
        // The row profile of the *first* frame: a column-wise dump makes a
        // shift, a re-range and a scrambled picture distinguishable at a
        // glance.
        if (!sink->frames().empty() &&
            sink->frames().front().luma.size() == reference.size()) {
            const std::vector<std::uint8_t>& luma = sink->frames().front().luma;
            std::printf("[hw-loopback] row 0 profile (col: ref->got):");
            for (std::uint32_t col = 0; col < kWidth; col += kWidth / 16u) {
                std::printf(" %u:%u->%u", col,
                            static_cast<unsigned>(reference[col]),
                            static_cast<unsigned>(luma[col]));
            }
            std::printf("\n[hw-loopback] near the edge column:\n");
            for (std::uint32_t col = kWidth / 4u - 8u;
                 col < kWidth / 4u + 8u; ++col) {
                std::printf(" %u:%u->%u", col,
                            static_cast<unsigned>(reference[col]),
                            static_cast<unsigned>(luma[col]));
            }
            std::printf("\n");

            // ---- The exact transition ----------------------------------
            //
            // Where does the picture turn black?  The boundary tells the
            // story: a boundary on a 16-pixel boundary is a macroblock
            // edge (a lost or undecoded slice), a boundary that is not
            // aligned is a copy or stride bug, and no boundary at all
            // means the whole row is wrong.
            //
            // The scan is per *row* of a few sampled rows, because a
            // horizontal ramp means every row has the same profile.
            for (const std::uint32_t row : {0u, kHeight / 2u, kHeight - 1u}) {
                const std::uint8_t* r =
                    luma.data() + static_cast<std::size_t>(row) * kWidth;
                const std::uint8_t* ref =
                    reference.data() + static_cast<std::size_t>(row) * kWidth;
                std::size_t first_bad = kWidth;
                std::size_t bad_count  = 0;
                for (std::uint32_t col = 0; col < kWidth; ++col) {
                    const int diff = std::abs(static_cast<int>(r[col]) -
                                              static_cast<int>(ref[col]));
                    if (diff > 8) {
                        if (first_bad == kWidth) first_bad = col;
                        ++bad_count;
                    }
                }
                std::printf("[hw-loopback] row %u: first bad column %zu, "
                            "%zu bad columns\n",
                            row,
                            first_bad == kWidth ? kWidth : first_bad,
                            bad_count);
                if (first_bad < kWidth) {
                    const std::size_t lo = first_bad > 4 ? first_bad - 4 : 0;
                    const std::size_t hi =
                        (first_bad + 12 < kWidth) ? first_bad + 12 : kWidth;
                    std::printf("[hw-loopback] row %u around the boundary:",
                                row);
                    for (std::size_t col = lo; col < hi; ++col) {
                        std::printf(" %zu:%u->%u", col,
                                    static_cast<unsigned>(ref[col]),
                                    static_cast<unsigned>(r[col]));
                    }
                    std::printf("\n");
                }
            }
        }
    }

    tx_engine.close();
    rx_engine.close();
    return result;
}

class EngineVideoHwLoopback : public ::testing::Test {
protected:
    static void SetUpTestSuite() { RegisterHwE2EPlugins(); }
};

}  // namespace

// ===========================================================================
// Q7: the hardware encode path, end to end, asserted from the stats
// ===========================================================================

TEST_F(EngineVideoHwLoopback, HardwareEncodeReachesTheReceiverAsRealPixels) {
    if (!HardwareEncoderAvailable()) {
        GTEST_SKIP() << "no hardware H.264 encoder on this host — "
                     << HardwareEncoderUnavailableReason();
    }

    // The engine's codec serves *both* directions, and the "hardware"
    // pin is honoured in both.  A host with a hardware encoder but no
    // hardware decoder cannot open a hardware-pinned engine — the codec
    // factory refuses to substitute the software decoder — so the
    // end-to-end claim cannot be made here.  That is a skip with a
    // reason, not a failure: the dedicated hardware job
    // (`.github/workflows/hardware.yml`) is where the claim is gated.
    if (!HardwareDecoderAvailable()) {
        GTEST_SKIP() << "no hardware H.264 decoder on this host — "
                     << HardwareDecoderUnavailableReason();
    }

    const LoopbackResult result = run_loopback();

    // ---- The engine really pinned hardware -------------------------------
    //
    // A failure here is a *hardware-policy* failure (plan B2), not a
    // media failure: the engine must refuse to open rather than quietly
    // run software, and the error must say so.
    ASSERT_EQ(result.open_rc_tx, 0u)
        << "the engine asked for the hardware backend and did not get "
           "one — open() must succeed here because the probe found an "
           "encoder.  The engine reported: " << result.open_error_tx;

    EXPECT_EQ(result.codec_backend_preference, "hardware")
        << "Stats::video_codec_backend_preference must echo the host's pin";
    EXPECT_TRUE(result.codec_hardware)
        << "Stats::video_codec_hardware_accelerated must be true for a "
           "run pinned to hardware (backend='" << result.codec_backend << "')";
    // These two ids are what a *software* fallback would report.  The
    // `ASSERT`s make that fallback a hard failure rather than a warning:
    // the whole point of the pin is that it cannot happen silently.
    ASSERT_NE(result.codec_backend, "software-stub")
        << "the software stub answered a 'hardware' request";
    ASSERT_NE(result.codec_backend, "openh264")
        << "the OpenH264 software encoder answered a 'hardware' request";
    EXPECT_EQ(result.codec_encode_errors, 0u)
        << "the hardware encoder reported errors";

    // ---- The encoder produced pictures -----------------------------------
    //
    // "Captured, encoded by the hardware codec": the pump must have
    // handed the synthetic capture's frames to the hardware encoder, and
    // the encoder must have returned bitstream for them.
    ASSERT_GT(result.codec_frames_encoded, 0u)
        << "the capture pump delivered no frame to the encoder";
    ASSERT_GT(result.rx_tx_frames_encoded, 0u)
        << "the hardware encoder returned no encoded frame";

    // ---- The wire carried the picture ------------------------------------
    ASSERT_GT(result.tx_rtp_packets, 0u)
        << "the hardware encoder's bitstream never reached the wire";
    ASSERT_GT(result.rx_frames, 0u)
        << "the receiving engine rendered nothing";

    // ---- The receiving engine decoded *and rendered* it ------------------
    //
    // Q7's sentence names both verbs; these counters are the engine's
    // own answer, independent of what the recording sink kept.
    EXPECT_GE(result.rx_frames_decoded, 1u)
        << "the receiving engine did not decode the hardware bitstream";
    EXPECT_GE(result.rx_frames_rendered, 1u)
        << "the receiving engine did not render the decoded picture";

    // ---- The pixels are the source picture --------------------------------
    const std::vector<std::uint8_t> reference = reference_luma();
    ASSERT_EQ(result.width, kWidth);
    ASSERT_EQ(result.height, kHeight);
    ASSERT_EQ(result.luma.size(), reference.size());

    // A picture that decoded but came out blank would pass a correlation
    // *shape* check only by accident — rule out the degenerate cases
    // (an all-zero surface is the classic NVDEC "no output" symptom)
    // before measuring.
    {
        std::uint64_t min_v = 255, max_v = 0;
        for (const std::uint8_t v : result.luma) {
            if (v < min_v) min_v = v;
            if (v > max_v) max_v = v;
        }
        ASSERT_GT(max_v - min_v, 16u)
            << "the rendered luma is flat (range " << min_v << ".." << max_v
            << ") — the decoder produced no picture";
    }

    const double mae = mean_abs_error(reference, result.luma);
    EXPECT_LT(mae, 14.0)
        << "the decoded picture does not match the captured picture "
           "(mean absolute luma error " << mae << ")";
    const double corr = pearson_correlation(reference, result.luma);
    EXPECT_GT(corr, 0.93)
        << "the decoded picture does not correlate with the captured "
           "picture (Pearson r = " << corr << ")";

    RecordProperty("mae_x100", static_cast<int>(mae * 100.0));
    RecordProperty("pearson_r_x1000", static_cast<int>(corr * 1000.0));
    RecordProperty("tx_rtp_packets", static_cast<int>(result.tx_rtp_packets));
    RecordProperty("rx_frames", static_cast<int>(result.rx_frames));
}
