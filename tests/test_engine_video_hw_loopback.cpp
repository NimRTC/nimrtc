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
#include <nimrtc/h265/hw_backends.hpp>
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

// The H.265 payload type: RFC 7798 §4.1 recommends 116 for H.265 and
// Chrome's WebRTC stack uses it too — it is also the engine's own
// default for the H.265 family (see `video_codec_config.hpp`).
constexpr std::uint8_t kH265VideoPt = 116;

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
// The H.265 hardware probe (v1.3 plan item Q1b)
// ---------------------------------------------------------------------------
//
// The H.265 backend table is a *separate* table from the H.264 one (the
// two codec families have their own rows, their own priorities and their
// own availability probes), so the probe has to ask the H.265 table the
// same question the H.264 probe above asks its table.  The answer is
// still the driver's: a GPU with an H.264 encoder but no H.265 encoder
// reports "not available" here, and the skip message says so.

// The geometry the Q1b switch runs at: small enough for a hardware
// encoder to re-initialise quickly, large enough to be a real picture.
constexpr std::uint32_t kSwitchWidth      = 320;
constexpr std::uint32_t kSwitchHeight     = 240;
constexpr std::uint32_t kSwitchedWidth    = 640;
constexpr std::uint32_t kSwitchedHeight   = 480;

/** True iff the H.265 backend table offers a usable hardware encoder. */
bool H265HardwareEncoderAvailable() {
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

/** The reason the table gives for the first *unavailable* hardware H.265
 *  encoder — used to make the skip message honest. */
std::string H265HardwareEncoderUnavailableReason() {
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

/** True iff the H.265 backend table offers a usable hardware *decoder*. */
bool H265HardwareDecoderAvailable() {
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

/** The reason the table gives for the first *unavailable* hardware H.265
 *  decoder — used to make the skip message honest. */
std::string H265HardwareDecoderUnavailableReason() {
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
// The source picture (the Q1b switch's input)
// ---------------------------------------------------------------------------

/**
 * @brief An I420 frame with a horizontal luma ramp and neutral chroma.
 *
 * The picture is *grey* (the chroma planes carry no signal), so a chroma
 * error cannot mask a luma error, and the luma of column `col` is
 * `32 + (col * 160) / width` — a ramp the decoder's output can be
 * compared against directly.
 *
 * The frame is a *view* into @p storage, which the caller keeps alive
 * for as long as the frame is in use.
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

/** The luma plane the source picture *should* decode to. */
std::vector<std::uint8_t> ramp_reference(std::uint32_t width,
                                          std::uint32_t height) {
    std::vector<std::uint8_t> y(static_cast<std::size_t>(width) * height);
    for (std::uint32_t row = 0; row < height; ++row) {
        for (std::uint32_t col = 0; col < width; ++col) {
            y[static_cast<std::size_t>(row) * width + col] =
                static_cast<std::uint8_t>(32 + ((col * 160) / width));
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

/**
 * @brief The engine configuration of the Q1b (H.265) case.
 *
 * The same wiring as `make_config()`, with two differences:
 *
 *   - the codec is the shipped **H.265** plug-in (`"h265"`), whose
 *     factory pairs NVENC and NVDEC into one composite codec — the
 *     engine holds a single codec per session and drives both
 *     directions through it;
 *   - the capture device is *not* used: the host pushes the frames
 *     itself through `send_video()`, because the switch is about the
 *     geometry of the frame the host hands over.
 */
EngineConfig make_h265_config() {
    EngineConfig cfg = make_config();
    cfg.video_codec_name    = "h265";
    cfg.video_codec_backend = "hardware";
    cfg.video_capture_name.clear();
    cfg.video_capture_pump = false;
    cfg.video_width         = kSwitchWidth;
    cfg.video_height        = kSwitchHeight;
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

// ===========================================================================
// Q1b: the H.265 half of Q1 — a mid-stream parameter-set switch on the
//       hardware path (v1.3 plan item Q1b)
// ===========================================================================
//
// Q1 proved the H.264 mechanism on the *software* decoder: a mid-stream
// SPS / PPS change decodes on the warm decoder, with no key-frame round
// trip.  Q1b is the H.265 counterpart, and it is asserted where H.265
// actually runs in this tree — on **NVENC / NVDEC**.
//
// The chain under test is the *whole* engine path, with no stub in the
// middle:
//
//   send_video(320x240)  -> NVENC -> RFC 7798 packetization (AP for the
//   parameter sets, FU for the slices) -> RTP on the wire -> the
//   receiving engine's depacketizer -> jitter buffer -> NVDEC -> the
//   recording sink
//
// and then the same thing again at 640x480.  The switch is a *parameter
// set* switch (a new VPS / SPS / PPS travels in-band with the next
// IRAP), so the receiver's decoder must follow it on its own.
//
// The acceptance is Q1b's own wording:
//
//   - the first decode after the change succeeds (no `kErrNotReady`);
//   - the decoded geometry is the *new* one;
//   - **no PLI goes on the wire** — neither as the engine's internal
//     decode-failure counter nor as an RTCP datagram on the receiver's
//     transport.
//
// The last point needs two independent checks: the counter is an
// internal fact, and the datagram is what actually left the machine.  A
// PLI that went out and came back would show up in the second check
// even if the first one were blind.

TEST_F(EngineVideoHwLoopback, AnH265MidStreamParameterSetSwitchDecodesWithoutAKeyFrameRoundTrip) {
    // ---- The hardware has to be there ------------------------------------
    //
    // The skip is the honest result on a host without an H.265 encoder
    // (or decoder): the dedicated hardware job
    // (`.github/workflows/hardware.yml`, the `gpu-nvenc` leg) is where
    // the claim is gated.
    if (!H265HardwareEncoderAvailable()) {
        GTEST_SKIP() << "no hardware H.265 encoder on this host — "
                     << H265HardwareEncoderUnavailableReason();
    }
    if (!H265HardwareDecoderAvailable()) {
        GTEST_SKIP() << "no hardware H.265 decoder on this host — "
                     << H265HardwareDecoderUnavailableReason();
    }

    // ---- The receiving engine -------------------------------------------
    //
    // It never captures: it only receives.  Its decode path is the real
    // one (depacketizer -> jitter buffer -> NVDEC -> sink), so the
    // assertions below are about the production path.
    EngineConfig rx_cfg = make_h265_config();
    rx_cfg.transport_name  = "hw-ice-rx";
    rx_cfg.video_sink_name = "hw-recording-rx";

    NimRTCEngine rx_engine(rx_cfg);
    ASSERT_EQ(rx_engine.open(), 0u) << "the receiving engine must open";

    // ---- The transmitting engine ----------------------------------------
    EngineConfig tx_cfg = make_h265_config();

    NimRTCEngine tx_engine(tx_cfg);
    ASSERT_EQ(tx_engine.open(), 0u) << "the transmitting engine must open";

    FakeIceTransport* tx_ice = g_tx_ice.instance();
    ASSERT_NE(tx_ice, nullptr);
    FakeIceTransport* rx_ice = g_rx_ice.instance();
    ASSERT_NE(rx_ice, nullptr);

    // ---- The first geometry ----------------------------------------------
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

    // The wire state before the switch: the second picture has to add
    // packets of its own (a new picture, with a new parameter set).
    const std::size_t datagrams_before_switch = tx_ice->sent().size();

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

    // The engine restarts the encoder for the new geometry (plan B13):
    // that is what puts the new parameter sets on the wire with the next
    // frame, and it is what the "no PLI" assertion below depends on.
    EXPECT_GT(tx_ice->sent().size(), datagrams_before_switch)
        << "the frame at the new geometry produced no packets — the "
           "encoder did not restart and the new parameter sets never "
           "reached the peer";

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
            if (pt == kH265VideoPt) ++rtp_packets;
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
    //     independent check: the counter above is an *internal* fact,
    //     and this one is what actually went on the wire.
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

    HwRecordingSink* rx_sink = g_rx_sink.instance();
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
    //     parameter sets and the second one with the *new* ones: both
    //     pictures went through the same warm decoder.
    ASSERT_GE(frames.size(), 2u)
        << "both pictures must have been decoded — the switch test is "
           "only meaningful on a decoder that was already working";
    EXPECT_EQ(frames.front().width, kSwitchWidth);
    EXPECT_EQ(frames.front().height, kSwitchHeight);

    // (g) The picture is the *right* picture — a decoder that kept the
    //     old parameter sets would render the *old* picture at the new
    //     size, and only a pixel comparison catches that.
    {
        const std::vector<std::uint8_t> reference =
            ramp_reference(kSwitchedWidth, kSwitchedHeight);
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

        RecordProperty("mae_x100", static_cast<int>(mae * 100.0));
        RecordProperty("pearson_r_x1000", static_cast<int>(corr * 1000.0));
    }

    RecordProperty("rtp_packets", static_cast<int>(rtp_packets));
    RecordProperty("rx_frames", static_cast<int>(frames.size()));

    tx_engine.close();
    rx_engine.close();
}
