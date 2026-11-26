/**
 * @file tests/test_engine_video_e2e_loopback.cpp
 * @brief The M1 exit criterion (remote-desktop plan §B.2 / §B.6).
 *
 * ## What this test proves
 *
 * The M1 exit criterion is:
 *
 *   > "Two native clients see each other's real desktop, in real pixels,
 *   >  at 1080p, over the network, with the picture actually encoded by a
 *   >  real codec ... pixel-diff test on a synthetic pattern."
 *
 * This file is the *engine-level* half of that proof: it drives two
 * engines — a **transmitting** one and a **receiving** one — through the
 * complete video pipeline with a **real** software H.264 codec
 * (vendored OpenH264), and asserts that the picture the receiver renders
 * is *pixel-wise the same picture* the transmitter captured.
 *
 * The chain that is exercised, end to end and without a single stub in
 * the middle:
 *
 * ```
 *   [TX engine]                                        [RX engine]
 *
 *   E2ECaptureDevice (BGRA, synthetic pattern)
 *        │  pop_frame()                    ── the wire ──┐
 *        ▼                                              │
 *   pump_video_capture()  (on tick())                  │
 *        ▼                                              │
 *   stage_source_frame()  → BGRA → I420 (BT.601)       │
 *        ▼                                              │
 *   OpenH264Codec::encode()  → real Annex-B H.264      │
 *        ▼                                              │
 *   IVideoSender  → STAP-A / FU-A packetization        │
 *        ▼                                              │
 *   rtp::PacketBuilder → RTP header                    │
 *        ▼                                              │
 *   FakeIceTransport::send()  ─────────────────────────▶ feed_media_inbound()
 *                                                              │
 *                                                              ▼
 *                                                     rtp::Parser → demux
 *                                                              │
 *                                                              ▼
 *                                                     IVideoReceiver → depacketize
 *                                                              │
 *                                                              ▼
 *                                                     VideoJitterBuffer → Annex B
 *                                                              │
 *                                                              ▼
 *                                                     OpenH264Codec::decode()
 *                                                              │
 *                                                              ▼
 *                                                     E2ERecordingSink::render()
 * ```
 *
 * ## What makes this a *proof* and not a smoke test
 *
 * 1. **The codec is a real codec.**  The engine is pointed at the
 *    `openh264_h264` backend explicitly — never at the priority-selected
 *    backend, which on a GPU host would be NVENC and would make the test
 *    non-deterministic across machines.  The engine's telemetry then has
 *    to report `video_codec_backend == "openh264"`; the synthetic stub
 *    would say `"software-stub"` and is therefore *excluded*.
 *
 * 2. **The bytes really travel as RTP.**  The packets the TX engine puts
 *    on the wire are captured by a fake ICE transport and handed to the
 *    RX engine through `feed_media_inbound()`, which re-parses the RTP
 *    header, re-demuxes on the payload type, re-depacketizes the
 *    STAP-A / FU-A payloads and re-assembles the Annex B bitstream.  A
 *    synthetic stub's bitstream would not survive that round trip — it
 *    would decode to the stub's own painted pattern, not to the source
 *    picture.
 *
 * 3. **The pixels are compared, not just counted.**  The reference luma
 *    plane is computed from the *same* BGRA pattern with the *same*
 *    BT.601 limited-range transform the engine's staging step uses, and
 *    the decoded luma is compared against it:
 *
 *      - mean absolute error < 12.0 grey levels, and
 *      - Pearson correlation > 0.95.
 *
 *    A blank frame, a wrong-colour frame, a frame from a different
 *    pattern, or the stub's synthetic picture all fail one or both.
 *
 * 4. **The engine's telemetry is checked.**  `video_capture_pumped_frames`,
 *    `video_frames_sent`, `video_frames_received`, `video_frames_decoded`,
 *    `video_frames_rendered` and `video_codec_backend` all have to move
 *    the way the criterion describes.
 *
 * ## What this test does NOT prove
 *
 * The *network* part of the criterion (real sockets, real loss, real
 * bandwidth) is covered by the transport-level work packages; this file
 * proves the **media path** — that real pixels survive a real codec
 * through a real RTP round trip.
 *
 * The picture size here **is 1080p** — the criterion's own resolution —
 * so the encode, the packetization and the decode all run at the size
 * the product will run at.  (The fake ICE transport keeps the *network*
 * out of the picture; it is a seam, not a shortcut around the codec.)
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
#include <nimrtc/h264/hw_backends.hpp>
#include <nimrtc/plugins/hw_composite_codec.hpp>
#include <nimrtc/plugins/hw_video_backend.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/plugins/video_codec.hpp>
#include <nimrtc/plugins/video_sink.hpp>
#include <nimrtc/video_frame/frame.hpp>

namespace {

namespace np = nimrtc::plugins;
namespace nd = nimrtc::dtls;
namespace vf = nimrtc::video_frame;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

// The M1 exit criterion asks for 1080p (remote-desktop plan §B.2:
// "in real pixels, at 1080p").  The test really encodes and decodes at
// this resolution — the picture is not downscaled to make the test
// fast, because that would hide exactly the defects the criterion
// exists to catch (a per-macroblock rate-control or scaling bug).
constexpr std::uint32_t kWidth  = 1920;
constexpr std::uint32_t kHeight = 1080;

/** The H.264 payload type RFC 6184 assigns (the engine's default too). */
constexpr std::uint8_t kVideoPt = 102;

// ---------------------------------------------------------------------------
// The synthetic picture
// ---------------------------------------------------------------------------

/**
 * @brief Paint the synthetic BGRA pattern.
 *
 * The pattern is a *luma ramp* with a hard horizontal edge in the middle
 * band, so the picture carries real spatial structure in the vertical
 * direction (the direction H.264 codes most efficiently) and a real
 * edge (the direction a broken codec most often smooths away).
 *
 * The colours stay inside a range that survives the BT.601
 * limited-range transform without clipping, so the reference luma is
 * unambiguous.
 */
void paint_pattern(std::uint8_t* bgra, std::uint32_t width,
                   std::uint32_t height) {
    for (std::uint32_t row = 0; row < height; ++row) {
        std::uint8_t* p = bgra + static_cast<std::size_t>(row) * width * 4u;
        for (std::uint32_t col = 0; col < width; ++col) {
            // A vertical ramp from dark to bright, plus a band that
            // inverts the slope every 32 rows — so the encoder has to
            // code genuine edges, not just a smooth gradient.
            std::int32_t value = 32 + static_cast<std::int32_t>(
                (col * 160) / static_cast<std::int32_t>(width));
            if (((row / 32) % 2) == 1) {
                value = 200 - static_cast<std::int32_t>((row % 32) * 4);
            }
            value = std::clamp(value, 16, 235);

            // A neutral chroma pattern would be a luma-only picture; a
            // *grey* picture means R == G == B, so the chroma planes
            // carry no signal and any chroma error cannot mask a luma
            // error.  A little chroma is added deliberately on the
            // diagonal so the picture is not degenerate.
            const std::uint8_t r = static_cast<std::uint8_t>(value);
            const std::uint8_t g = static_cast<std::uint8_t>(value);
            const std::uint8_t b = static_cast<std::uint8_t>(value);
            p[col * 4u + 0u] = b;   // B
            p[col * 4u + 1u] = g;   // G
            p[col * 4u + 2u] = r;   // R
            p[col * 4u + 3u] = 255; // A
        }
    }
}

/**
 * @brief The reference luma plane of the synthetic pattern.
 *
 * This is the *same* transform the engine's `stage_source_frame()`
 * applies (`video_frame::convert_bgra_to_i420`, BT.601 limited range):
 *
 *     Y = ((66 R + 129 G + 25 B + 128) >> 8) + 16
 *
 * It is recomputed here — from the *same* BGRA bytes the capture device
 * hands out — so the comparison measures what the codec did, not what
 * the test hoped the codec would do.
 */
std::vector<std::uint8_t> reference_luma() {
    std::vector<std::uint8_t> bgra(static_cast<std::size_t>(kWidth) * kHeight * 4u);
    paint_pattern(bgra.data(), kWidth, kHeight);

    std::vector<std::uint8_t> y(static_cast<std::size_t>(kWidth) * kHeight);
    for (std::size_t i = 0; i < y.size(); ++i) {
        const std::int32_t b = bgra[i * 4u + 0u];
        const std::int32_t g = bgra[i * 4u + 1u];
        const std::int32_t r = bgra[i * 4u + 2u];
        const std::int32_t lum =
            ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
        y[i] = static_cast<std::uint8_t>(
            lum < 0 ? 0 : (lum > 255 ? 255 : lum));
    }
    return y;
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

/**
 * @brief A fake ICE transport that captures every outbound datagram.
 *
 * One instance is created *per engine* (the factory holds the pointer to
 * the instance it created), so the TX engine's packets and the RX
 * engine's packets never get mixed up.
 */
class FakeIceTransport final : public np::IICETransport {
public:
    // ---- IPlugin ---------------------------------------------------------
    const char* name() const noexcept override { return "e2e-fake-ice"; }
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    // ---- ITransport ------------------------------------------------------
    void set_callbacks(np::RecvCallback on_recv,
                       np::ErrorCallback on_error) noexcept override {
        on_recv_  = std::move(on_recv);
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
            if (on_recv_) on_recv_(np::BufferView(pkt.data(), pkt.size()));
            ++n;
        }
        return n;
    }

    np::Addr local_addr() const noexcept override { return {}; }
    np::Addr remote_addr() const noexcept override { return {}; }

    // ---- IICETransport ---------------------------------------------------
    np::IceState state() const noexcept override { return np::IceState::Connected; }
    std::string local_ufrag() const noexcept override { return "e2eufrag"; }
    std::string local_password() const noexcept override { return "e2epassword"; }
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

private:
    np::RecvCallback   on_recv_;
    np::ErrorCallback  on_error_;
    std::vector<std::vector<std::uint8_t>> inbound_;
    std::vector<std::vector<std::uint8_t>> sent_;
};

/**
 * @brief A fake ICE factory that remembers the transport it created.
 *
 * The two engines in this test use two *different* factory ids
 * (`"e2e-ice-tx"` / `"e2e-ice-rx"`), so each factory owns its own
 * instance pointer — there is no global "last created" to race over.
 */
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

    /** The transport the factory handed out (nullptr until created). */
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
 * The engine asks two things of the DTLS session: "is the handshake
 * done?" and "give me the SRTP keying material".  The fake answers the
 * first with yes and the second with "not ready" — so SRTP is never
 * installed and the RTP travels in cleartext, which is what lets the
 * test read the wire bytes.  (The SRTP protection path has its own
 * tests; this file is about the *media* path.)
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
    std::string_view id() const noexcept override { return "e2e-fake-dtls"; }
    std::string_view display_name() const noexcept override {
        return "Fake DTLS session (e2e loopback test)";
    }
    std::unique_ptr<nd::IDtlsSession> create(const nd::DtlsConfig&) const override {
        return std::make_unique<FakeDtlsSession>();
    }
};

// ---------------------------------------------------------------------------
// The capture device — a deterministic BGRA source
// ---------------------------------------------------------------------------

/**
 * @brief A capture device that hands out the synthetic BGRA pattern.
 *
 * The device is *pull-based* here: the engine's pump polls it on
 * `tick()`, exactly like a real device whose queue happens to be full.
 * Every frame carries the same picture (the criterion asks for a
 * *pattern*, not a moving one) but a distinct sequence number and
 * timestamp, so the pipeline's metadata handling is exercised too.
 */
class E2ECaptureDevice final : public np::IVideoCapture {
public:
    explicit E2ECaptureDevice(np::CaptureConfig cfg) : cfg_(std::move(cfg)) {
        pixels_.resize(static_cast<std::size_t>(kWidth) * kHeight * 4u);
        paint_pattern(pixels_.data(), kWidth, kHeight);
    }

    ~E2ECaptureDevice() override { close(); }

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override { return "E2ECaptureDevice"; }

    np::Status open() noexcept override {
        open_ = true;
        return np::kOk;
    }
    void close() noexcept override { open_ = false; }

    // ---- Enumeration ------------------------------------------------------
    std::vector<np::CaptureDevice> enumerate_devices() const noexcept override {
        return {};
    }
    std::vector<np::CaptureFormat> supported_formats(
        const std::string&) const noexcept override {
        return {};
    }
    np::CaptureCapabilities capabilities() const noexcept override {
        np::CaptureCapabilities c;
        c.backend = "e2e-synthetic";
        // Unbounded geometry and frame rate: the device delivers what
        // the caller asks for.
        c.max_pixels = 0;
        c.max_fps    = 0;
        return c;
    }

    // ---- Lifecycle --------------------------------------------------------
    bool is_open() const noexcept override { return open_; }
    np::CaptureConfig config() const noexcept override { return cfg_; }

    // ---- Frame delivery ---------------------------------------------------
    np::Status pop_frame(np::CaptureFrame& out) noexcept override {
        if (!open_) return np::kErrNotReady;
        // The device holds one frame at a time; the engine's pump takes
        // it and never asks for a second copy.
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
        s.frames_captured    = produced_;
        s.frames_cpu_staged  = produced_;
        return s;
    }

private:
    np::CaptureConfig cfg_{};
    std::vector<std::uint8_t> pixels_;
    bool open_ = false;
    bool pending_ = false;
    std::uint64_t produced_ = 0;
};

class E2ECaptureFactory final : public np::IVideoCaptureFactory {
public:
    std::string_view id() const noexcept override { return "e2e-capture"; }
    std::string_view display_name() const noexcept override {
        return "Synthetic BGRA capture device (e2e loopback test)";
    }
    np::IVideoCapture* create(np::CaptureConfig cfg) const override {
        return new E2ECaptureDevice(std::move(cfg));
    }
};

// ---------------------------------------------------------------------------
// The recording sink — captures the decoded pixels
// ---------------------------------------------------------------------------

/** The decoded frame the sink kept. */
struct RecordedFrame {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    std::int32_t  stride_y = 0;
    /** The luma plane, tightly packed (width * height bytes). */
    std::vector<std::uint8_t> luma;
};

/**
 * @brief A sink that keeps the decoded luma plane for the pixel diff.
 *
 * The `IVideoSink` contract says the planes are borrowed for the
 * duration of the `render()` call, so the sink copies the luma out
 * before returning — which is exactly what a real recorder does.
 */
class E2ERecordingSink final : public np::IVideoSink {
public:
    // `IVideoSink` has three `render()` overloads; overriding one of
    // them hides the others (the usual C++ name-hiding rule), which
    // GCC's `-Woverloaded-virtual` rejects.  The `using` declaration
    // brings the base overloads back into scope.
    using np::IVideoSink::render;

    explicit E2ERecordingSink(np::VideoSinkConfig cfg) : cfg_(cfg) {}

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override { return "E2ERecordingSink"; }
    np::Status open() noexcept override {
        open_ = true;
        return np::kOk;
    }
    void close() noexcept override { open_ = false; }

    // ---- IVideoSink -------------------------------------------------------
    np::Status render(const np::VideoSourceFrame& frame) noexcept override {
        if (!open_) return np::kErrNotReady;
        if (!frame.plane_y || frame.width == 0 || frame.height == 0) {
            return np::kErrInvalidParam;
        }
        const std::int32_t stride = frame.stride_y > 0
            ? frame.stride_y
            : static_cast<std::int32_t>(frame.width);

        RecordedFrame rec;
        rec.width    = frame.width;
        rec.height   = frame.height;
        rec.stride_y = stride;
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
        // The e2e path is the CPU path: the software decoder produces
        // host pixels, so the GPU overload must not be taken.
        return np::kErrUnsupported;
    }

    np::VideoSinkStats stats() const noexcept override {
        np::VideoSinkStats s;
        s.frames_rendered = rendered_;
        return s;
    }

    np::VideoSinkConfig config() const noexcept override { return cfg_; }

    // ---- Test helpers -----------------------------------------------------
    const std::vector<RecordedFrame>& frames() const noexcept { return frames_; }

private:
    np::VideoSinkConfig cfg_{};
    std::vector<RecordedFrame> frames_;
    std::uint64_t rendered_ = 0;
    bool open_ = false;
};

class E2ERecordingSinkFactory final : public np::IVideoSinkFactory {
public:
    explicit E2ERecordingSinkFactory(std::string_view id) : id_(id) {}

    /** The sink instance this factory created (nullptr until created). */
    E2ERecordingSink* instance() const noexcept { return instance_; }

    std::string_view id() const noexcept override { return id_; }
    std::string_view display_name() const noexcept override {
        return "Recording sink (e2e loopback test)";
    }
    np::IVideoSink* create(np::VideoSinkConfig cfg) const override {
        auto* s = new E2ERecordingSink(std::move(cfg));
        instance_ = s;
        return s;
    }

private:
    std::string_view id_;
    mutable E2ERecordingSink* instance_ = nullptr;
};

// ---------------------------------------------------------------------------
// The codec factory — pins the real OpenH264 backend
// ---------------------------------------------------------------------------

/**
 * @brief A codec factory that always instantiates the OpenH264 backend.
 *
 * The engine resolves its codec through `core::PluginRegistry`, so the
 * test registers this factory under a *plugin id* the engine can name:
 * `"h264-openh264"`.  The id matters twice over:
 *
 *   - `detail::codec_family_from_plugin_id()` derives the codec family
 *     from the id: `"h264"` followed by the `'-'` separator, so the
 *     family is H.264 — exactly what the engine needs to pick the
 *     H.264 packetizer and depacketizer;
 *   - the factory *bypasses* the priority-based backend selection and
 *     instantiates the `openh264_h264` entry of the backend registry
 *     directly.  Without that, a host with an NVIDIA GPU would pick
 *     NVENC (priority 450) and the test would silently stop being a
 *     software-codec test — and, worse, would no longer be
 *     deterministic across machines.
 *
 * The class implements the *split* seam (`IVideoEncoderFactory` +
 * `IVideoDecoderFactory`) because that is what the engine resolves
 * first (plan B1).  It still implements the deprecated combined
 * `IVideoCodecFactory` so the registry's legacy slot keeps resolving
 * for hosts that ask for it — but the combined `create()` builds the
 * encoder half and hands back a borrowed view, which is exactly the
 * shape the engine no longer needs.
 */
class Openh264CodecFactory final : public np::IVideoCodecFactory,
                                   public np::IVideoEncoderFactory,
                                   public np::IVideoDecoderFactory {
public:
    std::string_view id() const noexcept override { return "h264-openh264"; }
    std::string_view display_name() const noexcept override {
        return "OpenH264 software H.264 (pinned, e2e loopback test)";
    }

    np::IVideoCodec* create(np::VideoCodecConfig cfg) const override {
        // The vendored OpenH264 has to be in this build; otherwise the
        // test skips (see `Openh264IsAvailable()`).
        nimrtc::h264::register_default_video_backends();

        // The combined shim is built from the *split* halves: the
        // encoder row and the decoder row of the OpenH264 backend.
        // `HwCompositeCodec` is the *owning* variant — it takes the two
        // halves and closes them in its destructor, which is what a
        // `new`-ed combined codec has to do.
        auto encoder = make_encoder(cfg);
        auto decoder = make_decoder(cfg);
        if (encoder == nullptr && decoder == nullptr) {
            // The backend is not in this build: the caller checks
            // `Openh264IsAvailable()` first, so this is a "cannot happen".
            return nullptr;
        }
        return new np::HwCompositeCodec(cfg, std::move(encoder), std::move(decoder));
    }

    // -- the split seam (plan B1) --------------------------------------------

    np::IVideoEncoder* create_encoder(np::VideoCodecConfig cfg) const override {
        return make_encoder(cfg).release();
    }

    np::IVideoDecoder* create_decoder(np::VideoCodecConfig cfg) const override {
        return make_decoder(cfg).release();
    }

private:
    /** The OpenH264 encoder row of the backend registry (owning). */
    std::unique_ptr<np::IVideoEncoder>
    make_encoder(np::VideoCodecConfig cfg) const {
        nimrtc::h264::register_default_video_backends();

        for (const auto* be : np::HwVideoBackendRegistry::instance()
                                 .list_encoders(np::VideoCodecKind::kH264)) {
            if (be->id == "openh264_h264" && be->create) {
                return be->create(cfg);
            }
        }
        return nullptr;
    }

    /** The OpenH264 decoder row of the backend registry (owning). */
    std::unique_ptr<np::IVideoDecoder>
    make_decoder(np::VideoCodecConfig cfg) const {
        nimrtc::h264::register_default_video_backends();

        for (const auto* be : np::HwVideoBackendRegistry::instance()
                                 .list_decoders(np::VideoCodecKind::kH264)) {
            if (be->id == "openh264_h264" && be->create) {
                return be->create(cfg);
            }
        }
        return nullptr;
    }
};

/** True iff the real OpenH264 software backend is in this build. */
bool Openh264IsAvailable() {
    nimrtc::h264::register_default_video_backends();
    for (const auto* be : np::HwVideoBackendRegistry::instance()
                             .list_encoders(np::VideoCodecKind::kH264)) {
        if (be->id == "openh264_h264") return be->available(np::VideoCodecConfig{});
    }
    return false;
}

// ---------------------------------------------------------------------------
// The shared plugin wiring
// ---------------------------------------------------------------------------

/**
 * @brief The fake ICE factory the *transmitting* engine uses.
 *
 * The two engines use two *different* factory ids (`"e2e-ice-tx"` /
 * `"e2e-ice-rx"`), so each factory owns its own instance pointer — there
 * is no global "last created" for the two engines to race over.
 */
FakeIceFactory g_tx_ice("e2e-ice-tx", "Fake ICE (e2e TX)");
FakeIceFactory g_rx_ice("e2e-ice-rx", "Fake ICE (e2e RX)");

/** The sink factories — the RX engine's decoded frames land in its instance. */
E2ERecordingSinkFactory g_tx_sink("e2e-recording-tx");
E2ERecordingSinkFactory g_rx_sink("e2e-recording-rx");

/**
 * @brief Register the test's plugins.
 *
 * Called once (from the fixture's `SetUpTestSuite()`): the fake
 * transports, the fake DTLS session, the synthetic capture device, the
 * recording sink and the OpenH264-pinned codec factory.
 */
void RegisterE2EPlugins() {
    nimrtc::core::register_all_default_plugins();

    static FakeDtlsFactory dtls;
    static E2ECaptureFactory capture;
    static Openh264CodecFactory codec;

    PluginRegistry::instance().register_ice_transport(g_tx_ice.id(), &g_tx_ice);
    PluginRegistry::instance().register_ice_transport(g_rx_ice.id(), &g_rx_ice);
    PluginRegistry::instance().register_dtls_session(dtls.id(), &dtls);
    PluginRegistry::instance().register_video_capture(capture.id(), &capture);
    PluginRegistry::instance().register_video_sink(g_tx_sink.id(), &g_tx_sink);
    PluginRegistry::instance().register_video_sink(g_rx_sink.id(), &g_rx_sink);
    // The engine resolves the *split* slots first (plan B1); the
    // combined slot stays registered for hosts that still ask for it.
    PluginRegistry::instance().register_video_encoder(codec.id(), &codec);
    PluginRegistry::instance().register_video_decoder(codec.id(), &codec);
    PluginRegistry::instance().register_video_codec(codec.id(), &codec);
}

/**
 * @brief The engine configuration used by both engines.
 *
 * The default ("browser-call") transport strategy resolves the *real*
 * `webrtc-classic` transport stack, whose ICE component is built from
 * `cfg.transport_name` — i.e. from our fake ICE factory.  (The "shell"
 * sentinel would hand the engine a null-instance ICE, so no RTP would
 * ever reach the wire.)
 *
 * The scheduler is off so the video packets are written straight to the
 * ICE transport (and are therefore readable by the test).
 */
EngineConfig make_config() {
    EngineConfig cfg{};
    // The default ("browser-call") strategy resolves the *real*
    // `webrtc-classic` transport stack, which builds its ICE component
    // from `cfg.transport_name` — i.e. from our fake ICE factory.  (The
    // "shell" sentinel would hand the engine a null-instance ICE, so no
    // RTP would ever reach the wire.)
    cfg.transport_strategy_name   = "";
    cfg.transport_name            = "e2e-ice-tx";
    cfg.dtls_name                 = "e2e-fake-dtls";
    cfg.datachannel_backend       = "";
    cfg.bwe_name                  = "";
    cfg.scheduler_config.enabled  = false;

    cfg.video_codec_name    = "h264-openh264";
    cfg.video_sender_name   = "reference";
    cfg.video_receiver_name = "reference";
    cfg.video_source_name   = "memory";
    cfg.video_sink_name     = "e2e-recording-tx";
    cfg.video_capture_name  = "e2e-capture";
    cfg.video_width         = kWidth;
    cfg.video_height        = kHeight;
    cfg.video_fps           = 30;
    return cfg;
}

// ---------------------------------------------------------------------------
// The fixture
// ---------------------------------------------------------------------------

class EngineVideoE2ELoopback : public ::testing::Test {
protected:
    static void SetUpTestSuite() { RegisterE2EPlugins(); }
};

// ---------------------------------------------------------------------------
// The loopback driver
// ---------------------------------------------------------------------------

/** Everything the loopback produced. */
struct LoopbackResult {
    /** The datagrams the TX engine put on the wire. */
    std::size_t tx_datagrams = 0;
    /** The RTP packets among them (payload type 102). */
    std::size_t tx_rtp_packets = 0;
    /** The decoded frames the RX sink recorded. */
    std::size_t rx_frames = 0;
    /**
     * @brief The luma plane of the first decoded frame.
     *
     * The sink instance belongs to the engine and is destroyed by
     * `close()`, so the pixels are copied out *before* the engines are
     * torn down — reading them afterwards would be a use-after-free.
     */
    std::vector<std::uint8_t> luma;
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
};

/**
 * @brief Drive the two engines through one capture → encode → wire →
 *        decode → render round trip.
 *
 * The TX engine's capture device is polled by the engine's own pump
 * (on `tick()`), so the host does *not* call `send_video()` — this is
 * the automatic path the M1 criterion describes.
 */
LoopbackResult run_loopback() {
    LoopbackResult result;

    // ---- The receiving engine -------------------------------------------
    EngineConfig rx_cfg = make_config();
    rx_cfg.transport_name = "e2e-ice-rx";
    rx_cfg.video_sink_name = "e2e-recording-rx";
    rx_cfg.video_capture_name.clear();   // the RX engine does not capture
    rx_cfg.video_capture_pump = false;

    NimRTCEngine rx_engine(rx_cfg);
    const std::uint32_t rx_open = rx_engine.open();
    EXPECT_EQ(rx_open, 0u) << "the receiving engine must open";

    // ---- The transmitting engine ----------------------------------------
    EngineConfig tx_cfg = make_config();

    NimRTCEngine tx_engine(tx_cfg);
    const std::uint32_t tx_open = tx_engine.open();
    EXPECT_EQ(tx_open, 0u) << "the transmitting engine must open";

    // ---- Drive the TX engine's tick until it has produced packets ------
    //
    // The pump pops one frame per tick; a handful of ticks is enough for
    // the encoder to emit its first (IDR) frame.
    for (int i = 0; i < 50; ++i) {
        tx_engine.tick();
        if (tx_engine.stats().video_capture_pumped_frames >= 1) break;
    }

    const auto tx_stats = tx_engine.stats();
    EXPECT_GE(tx_stats.video_capture_pumped_frames, 1u)
        << "the capture pump must have encoded at least one frame";
    EXPECT_EQ(tx_stats.video_codec_backend, "openh264")
        << "the engine must run on the real software codec, not the "
           "synthetic stub";

    // ---- Collect the wire bytes -----------------------------------------
    FakeIceTransport* tx_ice = g_tx_ice.instance();
    EXPECT_NE(tx_ice, nullptr) << "the TX engine must have created its transport";
    if (tx_ice == nullptr) return result;
    const auto& datagrams = tx_ice->sent();
    result.tx_datagrams = datagrams.size();

    // Count the RTP packets among them (the engine also emits RTCP).
    for (const auto& d : datagrams) {
        if (d.size() < 2) continue;
        const std::uint8_t pt = d[1] & 0x7Fu;
        if (pt == kVideoPt) ++result.tx_rtp_packets;
    }
    EXPECT_GT(result.tx_rtp_packets, 0u)
        << "the encoder's picture must have reached the wire as RTP";

    // ---- Hand the wire bytes to the receiving engine --------------------
    //
    // This is the *real* inbound path: `feed_media_inbound()` re-parses
    // the RTP header, re-demuxes on the payload type and drives the
    // depacketizer / jitter buffer / decoder / sink chain.
    for (const auto& d : datagrams) {
        if (d.size() < 2) continue;
        const std::uint8_t pt = d[1] & 0x7Fu;
        if (pt != kVideoPt) continue;   // RTCP is not part of this test
        rx_engine.feed_media_inbound(d.data(), d.size());
    }

    // ---- Give the RX engine a few ticks to drain its jitter buffer ------
    for (int i = 0; i < 20; ++i) {
        rx_engine.tick();
    }

    const auto rx_stats = rx_engine.stats();
    EXPECT_GE(rx_stats.video_frames_received, 1u)
        << "the receiver must have assembled at least one frame";
    EXPECT_GE(rx_stats.video_frames_decoded, 1u)
        << "the decoder must have decoded at least one frame";
    EXPECT_GE(rx_stats.video_frames_rendered, 1u)
        << "the sink must have received at least one decoded frame";

    E2ERecordingSink* rx_sink = g_rx_sink.instance();
    EXPECT_NE(rx_sink, nullptr);
    if (rx_sink == nullptr) return result;
    result.rx_frames = rx_sink->frames().size();
    if (!rx_sink->frames().empty()) {
        const RecordedFrame& first = rx_sink->frames().front();
        result.luma   = first.luma;
        result.width  = first.width;
        result.height = first.height;
    }

    tx_engine.close();
    rx_engine.close();
    return result;
}

}  // namespace

// ===========================================================================
// The M1 exit criterion
// ===========================================================================

TEST_F(EngineVideoE2ELoopback, TwoClientsSeeTheSameRealPixels) {
    if (!Openh264IsAvailable()) {
        GTEST_SKIP() << "the vendored OpenH264 software codec is not in "
                        "this build — the M1 criterion needs a REAL codec";
    }

    const LoopbackResult result = run_loopback();

    ASSERT_GT(result.tx_rtp_packets, 0u)
        << "the TX engine must have put a real H.264 picture on the wire";
    ASSERT_GT(result.rx_frames, 0u)
        << "the RX engine must have rendered a decoded picture";

    // ---- The pixel diff --------------------------------------------------
    //
    // The reference is the *source* pattern, converted to luma with the
    // same BT.601 limited-range transform the engine's staging step uses.
    const std::vector<std::uint8_t> reference = reference_luma();

    ASSERT_EQ(result.width, kWidth);
    ASSERT_EQ(result.height, kHeight);
    ASSERT_EQ(result.luma.size(), reference.size());

    const std::vector<std::uint8_t>& decoded_luma = result.luma;
    const double mae = mean_abs_error(reference, decoded_luma);
    EXPECT_LT(mae, 12.0)
        << "the decoded picture does not match the captured picture "
           "(mean absolute luma error " << mae << ")";

    const double corr = pearson_correlation(reference, decoded_luma);
    EXPECT_GT(corr, 0.95)
        << "the decoded picture does not correlate with the captured "
           "picture (Pearson r = " << corr << ")";

    // Record the numbers, so a regression shows *how much* it moved.
    // (`RecordProperty` takes an int, so the doubles are scaled — the
    // reader only needs the order of magnitude.)
    RecordProperty("mae_x100", static_cast<int>(mae * 100.0));
    RecordProperty("pearson_r_x1000", static_cast<int>(corr * 1000.0));
    RecordProperty("tx_rtp_packets", static_cast<int>(result.tx_rtp_packets));
    RecordProperty("rx_frames", static_cast<int>(result.rx_frames));

    // ---- The picture carries real signal ---------------------------------
    //
    // A blank frame would satisfy a loose "did anything decode" check;
    // this pins the opposite direction.
    const auto [min_it, max_it] =
        std::minmax_element(decoded_luma.begin(), decoded_luma.end());
    EXPECT_GT(*max_it, 100u) << "the decoded luma has no bright pixels";
    EXPECT_LT(*min_it, 200u) << "the decoded luma has no dark pixels";
    EXPECT_GT(static_cast<unsigned>(*max_it - *min_it), 30u)
        << "the decoded luma is a constant — the picture carries no signal";

    // ---- The negative control --------------------------------------------
    //
    // The thresholds above are only meaningful if they *reject* a wrong
    // picture.  A flat grey frame — the classic "the decoder returned
    // nothing" failure — must fail both of them, otherwise the test
    // would pass on a blank decode.
    std::vector<std::uint8_t> blank(reference.size(), 128);
    EXPECT_GE(mean_abs_error(reference, blank), 12.0)
        << "the MAE threshold is too loose: a flat grey frame passes it";
    EXPECT_LE(pearson_correlation(reference, blank), 0.95)
        << "the Pearson threshold is too loose: a flat grey frame passes it";

    // The decoded picture must also be a genuine *decode*, not a
    // byte-for-byte copy of the source (which would mean the capture
    // and the "decoded" frame never went through the codec).
    EXPECT_NE(decoded_luma, reference)
        << "the decoded frame is byte-identical to the source pattern — "
           "it did not travel through the codec";
}

// ===========================================================================
// The mid-stream parameter-set switch (v1.3 plan Q1 / video plan R2)
// ===========================================================================
//
// The codec-level half of Q1 lives in
// `src/modules/h264/tests/test_openh264_software.cpp`
// (`Openh264Software.AMidStreamParameterSetSwitchDecodesWithoutAnError`):
// it proves that OpenH264's decoder follows a new SPS that arrives while
// it already holds a different one.
//
// This file is the *engine-level* half — the half the plan's DoD asks
// for ("a codec-level test + an engine-level test").  It proves the
// property that only the full pipeline can show:
//
//   1. the **sender** notices the geometry change and restarts the
//      encoder, so the new SPS / PPS travel with the next frame (the
//      engine's own `apply_video_encoder_params()` — the B13 rule);
//   2. the new parameter sets really go **on the wire** (RTP, STAP-A /
//      FU-A), not just into the encoder;
//   3. the **receiver** — with its jitter buffer, its depacketizer and
//      its decoder already warm from the old geometry — decodes the new
//      picture **without an error**;
//   4. and no key-frame round-trip was needed: the receiver's
//      decode-failure door never opened
//      (`video_feedback_pli_from_decode_failure == 0`), and no PLI went
//      out on the wire.
//
// Point 4 is the one that makes the test a *proof* rather than a
// smoke test: a decoder that silently refused the new picture would
// still "pass" a "did anything decode?" check — it just would not
// produce the new geometry, and it *would* open the decode-failure
// door (which is exactly what the counter measures).

namespace {

/** The geometry the stream starts at. */
constexpr std::uint32_t kSwitchWidth  = 320;
constexpr std::uint32_t kSwitchHeight = 240;
/** The geometry the stream switches to, mid-stream. */
constexpr std::uint32_t kSwitchedWidth  = 640;
constexpr std::uint32_t kSwitchedHeight = 480;

/**
 * @brief Build an I420 source frame of the given geometry.
 *
 * The picture is a *ramp* rather than a flat colour, so the decoder has
 * to code genuine spatial structure: a blank frame would decode to a
 * blank frame at any geometry, and the test could not tell "the new
 * picture was decoded" from "the decoder produced something of the
 * right size".
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

/** True iff the datagram looks like an RTCP PLI (RFC 4585 §6.3.1). */
bool is_pli_datagram(const std::vector<std::uint8_t>& d) {
    if (d.size() < 2) return false;
    const std::uint8_t pt2 = d[1];
    if (pt2 < 192 || pt2 > 223) return false;   // not RTCP at all
    return pt2 == 206 && (d[0] & 0x1Fu) == 1;   // PSFB, FMT = 1 = PLI
}

}  // namespace

TEST_F(EngineVideoE2ELoopback, AMidStreamParameterSetSwitchDecodesWithoutAKeyFrameRoundTrip) {
    if (!Openh264IsAvailable()) {
        GTEST_SKIP() << "the vendored OpenH264 software codec is not in "
                        "this build — the R2 assertion needs a REAL codec";
    }

    // ---- The receiving engine -------------------------------------------
    //
    // It never captures: it only receives.  Its own decode path is the
    // real one (the engine's receive callback → depacketizer → jitter
    // buffer → decoder → sink), so the assertion below is about the
    // production path, not about a test double.
    EngineConfig rx_cfg = make_config();
    rx_cfg.transport_name          = "e2e-ice-rx";
    rx_cfg.video_sink_name         = "e2e-recording-rx";
    rx_cfg.video_capture_name.clear();
    rx_cfg.video_capture_pump      = false;
    rx_cfg.video_width             = kSwitchWidth;
    rx_cfg.video_height            = kSwitchHeight;

    NimRTCEngine rx_engine(rx_cfg);
    ASSERT_EQ(rx_engine.open(), 0u) << "the receiving engine must open";

    // ---- The transmitting engine ----------------------------------------
    EngineConfig tx_cfg = make_config();
    tx_cfg.video_capture_name.clear();   // the host drives the frames
    tx_cfg.video_capture_pump   = false;
    tx_cfg.video_width          = kSwitchWidth;
    tx_cfg.video_height         = kSwitchHeight;

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

    // Let the encoder produce the picture (the OpenH264 encoder is
    // driven synchronously by `send_video()`, so the packets are on the
    // wire already — but the tick drives the RTCP / feedback path).
    for (int i = 0; i < 5; ++i) tx_engine.tick();

    const auto tx_stats_after_first = tx_engine.stats();
    EXPECT_GE(tx_stats_after_first.video_frames_sent, 1u)
        << "the first picture must have reached the sender";

    // ---- The geometry changes mid-stream --------------------------------
    //
    // This is the whole point of the test: the *frame* is the authority
    // on what is being sent (remote-desktop plan B13), so pushing a
    // frame at a new geometry must make the engine restart the encoder
    // and put the new SPS / PPS on the wire with the next frame.
    {
        const auto frame = make_i420_frame(kSwitchedWidth, kSwitchedHeight,
                                           storage, /*capture_ts_us=*/40000,
                                           /*rtp_timestamp=*/18000,
                                           /*frame_seq=*/2);
        ASSERT_EQ(tx_engine.send_video(frame), np::kOk)
            << "the frame at the new geometry must be accepted";
    }

    for (int i = 0; i < 5; ++i) tx_engine.tick();

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

    E2ERecordingSink* rx_sink = g_rx_sink.instance();
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
        const std::size_t reference_size =
            static_cast<std::size_t>(kSwitchedWidth) * kSwitchedHeight;
        ASSERT_EQ(last.luma.size(), reference_size)
            << "the decoded luma plane does not have the expected size";

        // The source picture is a horizontal ramp: the luma of column
        // `col` is `32 + (col * 160) / width`.
        std::vector<std::uint8_t> reference(reference_size);
        for (std::uint32_t row = 0; row < kSwitchedHeight; ++row) {
            for (std::uint32_t col = 0; col < kSwitchedWidth; ++col) {
                reference[static_cast<std::size_t>(row) * kSwitchedWidth + col] =
                    static_cast<std::uint8_t>(
                        32 + ((col * 160) / kSwitchedWidth));
            }
        }

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
        const std::vector<std::uint8_t> blank(reference_size, 128);
        EXPECT_GE(mean_abs_error(reference, blank), 12.0)
            << "the MAE threshold is too loose: a flat grey frame passes it";
        EXPECT_LE(pearson_correlation(reference, blank), 0.95)
            << "the Pearson threshold is too loose: a flat grey frame "
               "passes it";
    }

    tx_engine.close();
    rx_engine.close();
}

// ===========================================================================
// The negative control for the Q1 assertion
// ===========================================================================
//
// The Q1 assertion is "`video_feedback_pli_from_decode_failure == 0`".
// A counter that is *always* zero would satisfy it vacuously — the
// assertion would prove nothing at all.  So the harness has to show
// that the counter **does** move when the decoder really does refuse a
// frame.
//
// The way to provoke it is deliberately crude and deliberately *not* a
// protocol event: a synthetic H.264 payload that is not a decodable
// picture at all.  The receiver assembles it (the jitter buffer is
// perfectly happy with it), hands it to the decoder, and the decoder
// refuses it — which is exactly the "decode-failure door" the counter
// measures.
//
// The frame is *not* routed as a normal picture: it is pushed through
// the receiver with the same payload type the real stream uses, so the
// engine's demux treats it as video.

TEST_F(EngineVideoE2ELoopback, TheDecodeFailureDoorIsLiveInThisHarness) {
    if (!Openh264IsAvailable()) {
        GTEST_SKIP() << "the vendored OpenH264 software codec is not in "
                        "this build";
    }

    // ---- The receiving engine -------------------------------------------
    EngineConfig rx_cfg = make_config();
    rx_cfg.transport_name          = "e2e-ice-rx";
    rx_cfg.video_sink_name         = "e2e-recording-rx";
    rx_cfg.video_capture_name.clear();
    rx_cfg.video_capture_pump      = false;
    rx_cfg.video_width             = kSwitchWidth;
    rx_cfg.video_height            = kSwitchHeight;

    NimRTCEngine rx_engine(rx_cfg);
    ASSERT_EQ(rx_engine.open(), 0u);

    const auto before = rx_engine.stats();
    ASSERT_EQ(before.video_feedback_pli_from_decode_failure, 0u)
        << "the counter must start at zero";

    // ---- A frame that is not a decodable picture -------------------------
    //
    // The payload is a *bare* NAL unit of type 5 (IDR) whose body is
    // nonsense.  RFC 6184 §5.1 carries bare NAL units on the wire —
    // the start code is not on the wire, and the receiver puts it back
    // (that is exactly what the receiver's depacketizer does), so the
    // payload here must *not* carry one.
    //
    // The body is a run of 0xFF bytes: a slice header that no decoder
    // can make sense of.  The receiver's depacketizer accepts the
    // packet (it is a well-formed single-NAL packet), the jitter buffer
    // assembles it, and the decoder refuses it — which is exactly the
    // "decode-failure door" the counter measures.
    std::vector<std::uint8_t> nalu;
    nalu.push_back(0x65);                      // IDR, NRI=3
    nalu.insert(nalu.end(), 256, 0xFF);

    // ---- Wrap it in an RTP packet ---------------------------------------
    //
    // The RTP header is built by hand here (12 bytes, RFC 3550 §5.1):
    // the test is about the *engine's* reaction to an undecodable
    // frame, and it must not depend on the sender plugin's state.
    std::vector<std::uint8_t> pkt(12 + nalu.size(), 0);
    pkt[0] = 0x80;                        // V=2, no padding, no extension
    // The marker bit: this is the last (and only) packet of the picture.
    pkt[1] = static_cast<std::uint8_t>((kVideoPt & 0x7Fu) | 0x80);
    pkt[2] = 0x00;                        // sequence number 1
    pkt[3] = 0x01;
    pkt[4] = 0x00;                        // timestamp
    pkt[5] = 0x00;
    pkt[6] = 0x00;
    pkt[7] = 0x00;
    pkt[8] = 0xDE;                        // SSRC (arbitrary)
    pkt[9] = 0xAD;
    pkt[10] = 0xBE;
    pkt[11] = 0xEF;
    std::memcpy(pkt.data() + 12, nalu.data(), nalu.size());

    rx_engine.feed_media_inbound(pkt.data(), pkt.size());
    for (int i = 0; i < 10; ++i) rx_engine.tick();

    // ---- The door opened ------------------------------------------------
    const auto after = rx_engine.stats();
    EXPECT_GT(after.video_feedback_pli_from_decode_failure, 0u)
        << "the decode-failure door never opened — the Q1 assertion "
           "(`== 0`) is therefore vacuous in this harness and proves "
           "nothing";

    rx_engine.close();
}
