/**
 * @file examples/remote-desktop/rd_verify.cpp
 * @brief The **product-level sign-off program** for the remote-desktop
 *        plan's M1 exit criterion (plan §B.2 / §B.6).
 *
 * ## What this program is for
 *
 * Everything the tree can prove about the zero-copy capture seam has
 * been proved by a test:
 *
 *   - `tests/test_engine_capture_zero_copy.cpp` proves the **wiring** —
 *     that the engine hands the capture's device to the codec, that the
 *     surface reaches the encoder without a host copy, and that the
 *     codec is only ever touched from the tick thread;
 *   - `src/modules/nvcodec/tests/test_nvcodec_roundtrip.cpp` proves the
 *     **driver facts** — that NVENC consumes a registered DirectX
 *     surface and that NVDEC decodes what it produced;
 *   - `tests/bench/bench_zc_seam.cpp` measures the seam on real
 *     hardware: the same-device surface is accepted, the cross-device
 *     one is refused with status 9.
 *
 * What none of those proves is that the three hold **together, on one
 * real desktop, in one process**.  That is the last mile, and it is a
 * *human* activity: you run this program, you move your mouse, and you
 * look at the window.
 *
 * ## The chain this program drives
 *
 * ```
 *   [TX engine]                                     [RX engine]
 *
 *   WindowsCapture (DXGI / WGC)
 *        │  real desktop, its own thread
 *        ▼
 *   the engine's GPU mailbox            (remote-desktop plan B10)
 *        ▼
 *   NVENC  ── encode on the *capture's* D3D11 device ──
 *        ▼
 *   IVideoSender → STAP-A / FU-A → RTP
 *        ▼
 *   the in-memory wire  ─────────────────────────▶  RTP → depacketize
 *                                                         ▼
 *                                                    NVDEC ── decode
 *                                                         ▼
 *                                                    D3D11Renderer ── present
 * ```
 *
 * The "in-memory wire" is a queue between the two engines' ICE
 * transports.  It replaces the network and nothing else: the bytes that
 * cross it are the *same* RTP the engine produced, and they enter the
 * receiving engine through the *same* `feed_media_inbound()` seam a
 * real connection uses, so the demux → parse → depacketize → jitter
 * buffer → decode → render chain is the production one.
 *
 * ## The verdict
 *
 * The program's output is a table, and the table has one line that
 * matters more than all the others:
 *
 * ```
 *   cpu bytes copied ......... 0
 * ```
 *
 * Zero means the captured pixels never entered host memory on the way
 * to the encoder.  Everything else in the table says whether the chain
 * ran at all, and how fast.
 *
 * ## Usage
 *
 * ```
 *   rd_verify [--capture <id>] [--codec <id>] [--backend <class>]
 *             [--width <px>] [--height <px>] [--duration <sec>]
 *             [--window] [--no-zero-copy]
 * ```
 *
 * The defaults are the ones the criterion asks for: the DXGI desktop
 * duplication, the hardware H.264 path, 1920x1080, and the zero-copy
 * seam **on**.  `--no-zero-copy` turns the seam off so the CPU
 * fallback can be measured on the same machine — the comparison is the
 * point.
 */
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nimrtc/core/registry.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/ice_transport.hpp>
#include <nimrtc/plugins/video_sink.hpp>

namespace {

namespace np = nimrtc::plugins;
namespace nd = nimrtc::dtls;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct Options {
    std::string capture_name  = "windows-dxgi";
    std::string codec_name    = "h264";
    std::string codec_backend = "auto";
    std::uint32_t width       = 1920;
    std::uint32_t height      = 1080;
    std::uint32_t fps         = 30;
    int duration_sec          = 6;
    /** Present into a real window instead of the offscreen target. */
    bool window = false;
    /** Turn the zero-copy seam off (to measure the CPU fallback). */
    bool no_zero_copy = false;
};

void print_usage(const char* prog) {
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "  --capture <id>       Capture backend: windows-dxgi | windows-wgc |\n"
        "                       macos-screencapturekit | linux-x11 | synthetic\n"
        "                       (default: %s)\n"
        "  --codec <id>         Video codec (default: %s)\n"
        "  --backend <class>    Codec backend: auto | hardware | software\n"
        "  --width <px>         Capture width (default: %u)\n"
        "  --height <px>        Capture height (default: %u)\n"
        "  --fps <n>            Frame rate (default: %u)\n"
        "  --duration <sec>     How long to run (default: %d)\n"
        "  --window             Present into a real window instead of offscreen\n"
        "  --no-zero-copy       Turn the zero-copy seam OFF (CPU fallback)\n",
        prog, "windows-dxgi", "h264", 1920u, 1080u, 30u, 6);
}

// ---------------------------------------------------------------------------
// The in-memory wire
// ---------------------------------------------------------------------------

/**
 * @brief The queue the two engines share.
 *
 * The transmitting engine's ICE transport pushes into it; the main loop
 * drains it into the receiving engine.  The mutex is the only
 * synchronisation, and it is held for the duration of a pointer swap —
 * never across an encode and never across a callback into the host.
 */
class Wire {
public:
    void push(std::vector<std::uint8_t>&& datagram) {
        std::lock_guard<std::mutex> lock(mutex_);
        // A bounded queue: a slow receiver must not turn into unbounded
        // memory growth.  The oldest datagram is dropped — the newest
        // picture is the truth, which is the same policy the GPU
        // mailbox uses.
        if (queue_.size() >= kMaxDatagrams) {
            queue_.pop_front();
            ++dropped_;
        }
        queue_.push_back(std::move(datagram));
    }

    bool pop(std::vector<std::uint8_t>* out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        *out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    [[nodiscard]] std::uint64_t dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

private:
    static constexpr std::size_t kMaxDatagrams = 512;
    mutable std::mutex mutex_;
    std::deque<std::vector<std::uint8_t>> queue_;
    std::uint64_t dropped_ = 0;
};

/** The wire the two engines share. */
Wire g_wire;

// ---------------------------------------------------------------------------
// The fake ICE transport
// ---------------------------------------------------------------------------

/**
 * @brief An ICE transport that puts the engine's datagrams on the wire.
 *
 * It replaces the network and nothing else: the bytes it carries are
 * the *same* bytes the engine's transport stack produced (RTP, already
 * packetized, already STAP-A / FU-A fragmented).
 */
class WireIceTransport final : public np::IICETransport {
public:
    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    const char* name() const noexcept override { return "rd-verify-wire"; }

    void set_callbacks(np::RecvCallback on_recv,
                       np::ErrorCallback on_error) noexcept override {
        on_recv_  = std::move(on_recv);
        on_error_ = std::move(on_error);
    }

    np::Status send(np::BufferView view, np::Addr = {}) noexcept override {
        g_wire.push(std::vector<std::uint8_t>(view.data(),
                                              view.data() + view.size()));
        return np::kOk;
    }

    int recv() noexcept override { return 0; }

    np::Addr local_addr() const noexcept override { return {}; }
    np::Addr remote_addr() const noexcept override { return {}; }

    np::IceState state() const noexcept override {
        return np::IceState::Connected;
    }
    std::string local_ufrag() const noexcept override { return "rdverify"; }
    std::string local_password() const noexcept override { return "rdverifypw"; }
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
    void set_bind_address(std::string_view host) noexcept override {
        (void)host;
    }
    void set_stun_server(std::string_view host, std::uint16_t port) noexcept override {
        (void)host;
        (void)port;
    }
    void set_local_port_range(std::uint16_t begin, std::uint16_t end) noexcept override {
        (void)begin;
        (void)end;
    }
    void add_turn_server(std::string_view host,
                        std::uint16_t port,
                        std::string_view username,
                        std::string_view password) noexcept override {
        (void)host;
        (void)port;
        (void)username;
        (void)password;
    }
    void set_bwe(np::IBwe* bwe) noexcept override { (void)bwe; }
    void set_scheduler(np::IScheduler* sched) noexcept override { (void)sched; }

private:
    np::RecvCallback  on_recv_;
    np::ErrorCallback on_error_;
};

/** The factory for the wire transport. */
class WireIceFactory final : public np::IICETransportFactory {
public:
    std::string_view id() const noexcept override { return "rd-verify-wire"; }
    std::string_view display_name() const noexcept override {
        return "In-memory wire (the remote-desktop verification program)";
    }
    np::IICETransport* create_ice() const override {
        return new WireIceTransport();
    }
};

// ---------------------------------------------------------------------------
// The fake DTLS session
// ---------------------------------------------------------------------------

/**
 * @brief A DTLS session that reports "connected" but exports no keys.
 *
 * The engine asks two things of the DTLS session: "is the handshake
 * done?" and "give me the SRTP keying material".  This one answers the
 * first with yes and the second with "not ready" — so SRTP is never
 * installed and the RTP travels in cleartext, which is what lets the
 * program hand it to the receiving engine's inbound seam.
 *
 * (The SRTP path has its own tests.  This program is about the *media*
 * path, and the media path is what the criterion describes.)
 */
class WireDtlsSession final : public nd::IDtlsSession {
public:
    void set_role(nd::Role) noexcept override {}
    void set_peer_fingerprint(std::span<const std::uint8_t>) noexcept override {}
    void start() noexcept override {}
    void pump() noexcept override {}
    void on_handshake_complete(OnCompleteCb) noexcept override {}

    np::Status export_srtp_key_material(
            std::span<std::uint8_t, 60>) noexcept override {
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
    nd::DtlsState state() const noexcept override {
        return nd::DtlsState::Connected;
    }
    bool is_connected() const noexcept override { return true; }
    const nd::Fingerprint& local_fingerprint() const noexcept override {
        return fingerprint_;
    }
    std::optional<nd::SrtpKeyingMaterial> srtp_keying_material() const noexcept override {
        return std::nullopt;
    }

private:
    nd::Fingerprint fingerprint_{"sha-256", {}, {}, ""};
};

class WireDtlsFactory final : public nd::IDtlsSessionFactory {
public:
    std::string_view id() const noexcept override { return "rd-verify-dtls"; }
    std::string_view display_name() const noexcept override {
        return "Fake DTLS session (the remote-desktop verification program)";
    }
    std::unique_ptr<nd::IDtlsSession> create(const nd::DtlsConfig&) const override {
        return std::make_unique<WireDtlsSession>();
    }
};

// ---------------------------------------------------------------------------
// A sink that counts what it was given
// ---------------------------------------------------------------------------

/**
 * @brief A sink that counts the frames it received and samples one.
 *
 * The D3D11 renderer does the presenting; this one is the *measurement*
 * side, so the program can say "the decoder produced N frames and the
 * sink received M" without depending on the renderer's internals.
 */
class CountingSink final : public np::IVideoSink {
public:
    // `IVideoSink` has three `render()` overloads; overriding one of
    // them hides the others (the usual C++ name-hiding rule), which
    // GCC's `-Woverloaded-virtual` rejects.  The `using` declaration
    // brings the base overloads back into scope.
    using np::IVideoSink::render;

    explicit CountingSink(np::VideoSinkConfig cfg) : cfg_(std::move(cfg)) {}

    np::Status open() noexcept override { return np::kOk; }
    void close() noexcept override {}

    const char* name() const noexcept override { return "rd-verify-counter"; }

    np::Status render(const np::VideoSourceFrame& frame) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++frames_;
        if (first_width_ == 0 && frame.width != 0) {
            first_width_  = frame.width;
            first_height_ = frame.height;
            first_format_ = frame.format;
        }
        return np::kOk;
    }

    np::Status render(const np::GpuBuffer&) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++frames_;
        return np::kOk;
    }

    bool supports_gpu_render() const noexcept override { return true; }
    bool supports_cpu_render() const noexcept override { return true; }

    np::VideoSinkStats stats() const noexcept override {
        np::VideoSinkStats s;
        s.frames_rendered = frames_;
        return s;
    }

    np::VideoSinkConfig config() const noexcept override { return cfg_; }
    np::Status set_config(np::VideoSinkConfig cfg) noexcept override {
        cfg_ = std::move(cfg);
        return np::kOk;
    }

    [[nodiscard]] std::uint64_t frames() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_;
    }
    [[nodiscard]] std::uint32_t width() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return first_width_;
    }
    [[nodiscard]] std::uint32_t height() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return first_height_;
    }
    [[nodiscard]] np::VideoPixelFormat format() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return first_format_;
    }

private:
    mutable std::mutex mutex_;
    np::VideoSinkConfig cfg_;
    std::uint64_t frames_ = 0;
    std::uint32_t first_width_  = 0;
    std::uint32_t first_height_ = 0;
    np::VideoPixelFormat first_format_ = np::VideoPixelFormat::kUnknown;
};

class CountingSinkFactory final : public np::IVideoSinkFactory {
public:
    std::string_view id() const noexcept override { return "rd-verify-counter"; }
    std::string_view display_name() const noexcept override {
        return "Frame counter (the remote-desktop verification program)";
    }
    np::IVideoSink* create(np::VideoSinkConfig cfg) const override {
        instance_ = new CountingSink(std::move(cfg));
        return instance_;
    }
    [[nodiscard]] CountingSink* instance() const noexcept { return instance_; }

private:
    mutable CountingSink* instance_ = nullptr;
};

CountingSinkFactory g_counter_sink;

// ---------------------------------------------------------------------------
// The engine configuration
// ---------------------------------------------------------------------------

/**
 * @brief The configuration both engines share.
 *
 * The transport strategy is the *real* one (the "browser-call" default
 * resolves the `webrtc-classic` stack), because that stack is what
 * builds the ICE and DTLS components the program overrides.  The
 * scheduler is off so the video packets are written straight to the ICE
 * transport instead of waiting for a pacing window.
 */
EngineConfig make_config(const Options& opts, bool transmitting) {
    EngineConfig cfg;

    cfg.transport_strategy_name  = "";
    cfg.transport_name           = "rd-verify-wire";
    cfg.dtls_name                = "rd-verify-dtls";
    cfg.datachannel_backend      = "";
    cfg.bwe_name                 = "";
    cfg.scheduler_config.enabled = false;
    cfg.stun_server_host         = "";

    cfg.video_codec_name    = opts.codec_name;
    cfg.video_codec_backend = opts.codec_backend;
    cfg.video_sender_name   = "reference";
    cfg.video_receiver_name = "reference";
    cfg.video_source_name   = "memory";
    cfg.video_width         = opts.width;
    cfg.video_height        = opts.height;
    cfg.video_fps           = opts.fps;
    // Screen content, not camera content: the encoder should spend its
    // budget on sharp edges and text (remote-desktop plan B7).
    cfg.video_content_type  = np::VideoContentType::kScreen;
    cfg.video_content_hint  = np::VideoEncoderParams::ContentHint::kDetail;

    // The zero-copy seam.  The flag asks the *send* path to avoid a
    // host copy; the capture side is the device's own business.
    cfg.video_zero_copy = !opts.no_zero_copy;

    if (transmitting) {
        cfg.video_capture_name = opts.capture_name;
        cfg.video_capture_pump  = true;
        cfg.video_capture_config.want_cursor = true;
        cfg.video_sink_name = "headless";
    } else {
        // The receiving engine does not capture.
        cfg.video_capture_name.clear();
        cfg.video_capture_pump = false;
        cfg.video_sink_name    = "rd-verify-counter";
    }

    return cfg;
}

// ---------------------------------------------------------------------------
// The desktop activator
// ---------------------------------------------------------------------------

#if defined(_WIN32)
#include <windows.h>
#endif

/**
 * @brief Makes the desktop *change* during the measurement window.
 *
 * DXGI Desktop Duplication and Windows Graphics Capture deliver a frame
 * **only when the desktop changes**.  On an idle machine nothing changes,
 * so the backends deliver nothing — which is the whole point of a
 * dirty-rect-based capture path, but it does mean a measurement has
 * nothing to measure.
 *
 * The activator jiggles the mouse pointer across the desktop.  It is
 * deliberately the cheapest possible change: the compositor only has to
 * re-blit the cursor, so the measured rate is the *backend's* delivery
 * rate and not the compositor's redraw rate.
 *
 * The pointer's original position is restored when the program ends, so
 * the machine is left as it was found.
 *
 * The user is welcome to move the mouse instead — this only guarantees
 * that *something* changes.
 */
class DesktopActivator {
public:
    DesktopActivator() {
#if defined(_WIN32)
        if (::GetCursorPos(&saved_)) valid_ = true;
#endif
    }

    ~DesktopActivator() {
#if defined(_WIN32)
        if (valid_) ::SetCursorPos(saved_.x, saved_.y);
#endif
    }

    DesktopActivator(const DesktopActivator&)            = delete;
    DesktopActivator& operator=(const DesktopActivator&) = delete;

    /** Move the pointer one step.  Called from the measurement loop. */
    void step() const {
#if defined(_WIN32)
        if (!valid_) return;
        static LONG x = 0;
        x += 24;
        if (x > 240) x = 0;
        ::SetCursorPos(saved_.x + x,
                      saved_.y + ((x / 24) % 2 ? 8 : -8));
#endif
    }

private:
#if defined(_WIN32)
    POINT saved_{};
    bool  valid_ = false;
#endif
};

// ---------------------------------------------------------------------------
// The report
// ---------------------------------------------------------------------------

void row(const char* label, const char* value) {
    std::printf("  %-28s %s\n", label, value);
}

void row_u64(const char* label, std::uint64_t value) {
    std::printf("  %-28s %" PRIu64 "\n", label, value);
}

void row_str(const char* label, const std::string& value) {
    std::printf("  %-28s %s\n", label, value.c_str());
}

}  // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    // The report is the program's output — it must not be lost in a
    // buffer when the process is killed, and a reader watching the
    // console must see the phases as they happen.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    Options opts;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_next = (i + 1 < argc);
        if (arg == "--capture" && has_next) {
            opts.capture_name = argv[++i];
        } else if (arg == "--codec" && has_next) {
            opts.codec_name = argv[++i];
        } else if (arg == "--backend" && has_next) {
            opts.codec_backend = argv[++i];
        } else if (arg == "--width" && has_next) {
            opts.width = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--height" && has_next) {
            opts.height = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--fps" && has_next) {
            opts.fps = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--duration" && has_next) {
            opts.duration_sec = std::atoi(argv[++i]);
        } else if (arg == "--window") {
            opts.window = true;
        } else if (arg == "--no-zero-copy") {
            opts.no_zero_copy = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "rd_verify: unknown option '%.*s'\n",
                         static_cast<int>(arg.size()), arg.data());
            print_usage(argv[0]);
            return 2;
        }
    }

    std::printf("NimRTC remote-desktop verification (M1 sign-off)\n");
    std::printf("=====================================================\n\n");

    // ---- The plugins ------------------------------------------------------
    nimrtc::core::register_all_default_plugins();

    static WireIceFactory ice;
    static WireDtlsFactory dtls;
    PluginRegistry::instance().register_ice_transport(ice.id(), &ice);
    PluginRegistry::instance().register_dtls_session(dtls.id(), &dtls);
    PluginRegistry::instance().register_video_sink(g_counter_sink.id(),
                                                   &g_counter_sink);

    // ---- The receiving engine --------------------------------------------
    //
    // It is opened *first* so its receive pipeline (the jitter buffer in
    // particular) is already running when the first RTP packet arrives.
    NimRTCEngine rx_engine(make_config(opts, /*transmitting=*/false));
    const std::uint32_t rx_open = rx_engine.open();
    if (rx_open != 0u) {
        std::fprintf(stderr,
                     "rd_verify: the receiving engine did not open (0x%04X)\n",
                     rx_open);
        return 1;
    }

    // ---- The transmitting engine -----------------------------------------
    NimRTCEngine tx_engine(make_config(opts, /*transmitting=*/true));
    const std::uint32_t tx_open = tx_engine.open();
    if (tx_open != 0u) {
        std::fprintf(stderr,
                     "rd_verify: the transmitting engine did not open (0x%04X)\n",
                     tx_open);
        rx_engine.close();
        return 1;
    }

    std::printf("running for %d second(s) — the program moves the mouse so "
                "the desktop changes\n\n", opts.duration_sec);
    std::fflush(stdout);

    // The activator makes the desktop change, so the measurement does not
    // depend on the user doing something.
    const DesktopActivator activator;

    // ---- Drive the loop ----------------------------------------------------
    //
    // The transmitting engine's tick runs the capture pump (which drains
    // the GPU mailbox and encodes on this thread — RD-6); the datagrams
    // it produces land on the wire; the main loop moves them into the
    // receiving engine and ticks it, which drains the jitter buffer,
    // decodes and renders.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(opts.duration_sec);

    std::uint64_t wire_datagrams = 0;

    // ---- The measurement window -------------------------------------------
    //
    // The capture device is opened *before* the codec exists — the codec
    // has to adopt the capture's D3D11 device, and that device only
    // exists once the capture device is open.  The handful of frames
    // the device produced in that window had no GPU callback to go
    // through, so they were staged to host memory.  Those bytes were
    // really copied, and the device counts them honestly.
    //
    // The criterion is about the *seam*, so the measurement starts when
    // the seam goes live: the counters are sampled once the first GPU
    // frame has been encoded, and everything after that is the steady
    // state.  The startup transient is reported separately, so nothing
    // is hidden — it is simply not part of what is being measured.
    //
    // The window opens on the **first GPU frame**, not on the engine
    // being opened: the capture device is opened *before* the codec
    // exists (the codec has to adopt the capture's D3D11 device, and
    // that device only exists once the capture device is open), so the
    // handful of frames the device produced in that window went through
    // the CPU staging path — the GPU callback did not exist yet.  Those
    // bytes were really copied and the device counts them honestly, so
    // they belong to the transient, not to the steady state.
    //
    // A run in which the seam never goes live (a software encoder, or a
    // capture device with no GPU support) has **no** steady-state
    // window: the counters below then describe the whole run, which is
    // the honest reading — the seam was never live, so there was never
    // a steady state to measure.
    bool measuring = false;
    std::uint64_t base_bytes_copied = 0;
    std::uint64_t base_cpu_staged = 0;
    std::uint64_t base_gpu_frames = 0;
    std::uint64_t base_captured = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        activator.step();
        tx_engine.tick();

        std::vector<std::uint8_t> datagram;
        while (g_wire.pop(&datagram)) {
            ++wire_datagrams;
            rx_engine.feed_media_inbound(datagram.data(), datagram.size());
        }

        rx_engine.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

        // The measurement window opens when the seam goes live.
        if (!measuring) {
            const auto s = tx_engine.stats();
            if (s.video_capture_gpu_frames > 0) {
                measuring = true;
                base_bytes_copied = s.video_capture_device_bytes_copied;
                base_cpu_staged   = s.video_capture_device_frames_cpu_staged;
                base_gpu_frames   = s.video_capture_gpu_frames;
                base_captured     = s.video_capture_device_frames_captured;
                std::printf("  (the seam is live — the measurement window "
                            "starts now)\n");
                std::fflush(stdout);
            }
        }
    }
    std::printf("  (the loop is over — draining the wire)\n");
    std::fflush(stdout);

    // One last drain, so the frames that were in flight are counted.
    std::vector<std::uint8_t> datagram;
    while (g_wire.pop(&datagram)) {
        ++wire_datagrams;
        rx_engine.feed_media_inbound(datagram.data(), datagram.size());
    }
    for (int i = 0; i < 20; ++i) rx_engine.tick();

    std::printf("  (reading the stats)\n");
    std::fflush(stdout);
    const auto tx_stats = tx_engine.stats();
    const auto rx_stats = rx_engine.stats();

    // The steady state: the counters as they stood when the seam went
    // live, subtracted from the counters now.
    const std::uint64_t steady_bytes_copied =
        tx_stats.video_capture_device_bytes_copied - base_bytes_copied;
    const std::uint64_t steady_cpu_staged =
        tx_stats.video_capture_device_frames_cpu_staged - base_cpu_staged;
    const std::uint64_t steady_gpu_frames =
        tx_stats.video_capture_gpu_frames - base_gpu_frames;
    const std::uint64_t steady_captured =
        tx_stats.video_capture_device_frames_captured - base_captured;

    CountingSink* counter = g_counter_sink.instance();
    const std::uint64_t sink_frames = counter ? counter->frames() : 0;

    // ---- The report --------------------------------------------------------
    std::printf("\n");
    std::printf("======================= RESULT =======================\n\n");

    row_str("capture device", opts.capture_name);
    row_str("codec", opts.codec_name);
    row_str("codec backend", opts.codec_backend);
    row_str("encoder backend", tx_stats.video_codec_backend);
    row_str("decoder backend", rx_stats.video_codec_backend);
    row("encoder is hardware",
        tx_stats.video_codec_hardware_accelerated ? "yes" : "no");

    if (measuring) {
        std::printf("\n  the zero-copy seam (steady state)\n");
    } else {
        // The seam never went live, so there is no steady state to
        // report — the numbers below are the whole run.  Saying so is
        // the honest reading: a report that called them "steady state"
        // would attribute a startup transient to the seam.
        std::printf("\n  the zero-copy seam (the whole run — the seam "
                    "never went live)\n");
    }
    row("seam wired", tx_stats.video_capture_gpu_wired ? "yes" : "NO");
    row_u64("gpu frames encoded", steady_gpu_frames);
    row_u64("gpu errors", tx_stats.video_capture_gpu_errors);
    row_u64("cpu bytes copied", steady_bytes_copied);
    row_u64("cpu frames staged", steady_cpu_staged);
    row_u64("frames captured", steady_captured);

    if (measuring) {
        std::printf("\n  the startup transient (before the seam was wired)\n");
        row_u64("cpu bytes copied", base_bytes_copied);
        row_u64("cpu frames staged", base_cpu_staged);
        row_u64("gpu frames encoded", base_gpu_frames);
    }

    std::printf("\n  the capture device (whole run)\n");
    row_u64("frames captured", tx_stats.video_capture_device_frames_captured);
    row_u64("frames dropped", tx_stats.video_capture_device_frames_dropped);
    row_u64("frames zero-copy", tx_stats.video_capture_device_frames_zero_copy);
    row_u64("device errors", tx_stats.video_capture_device_error_count);

    std::printf("\n  the pipeline\n");
    row_u64("frames pumped", tx_stats.video_capture_pumped_frames);
    row_u64("frames sent", tx_stats.video_frames_sent);
    row_u64("wire datagrams", wire_datagrams);
    row_u64("frames received", rx_stats.video_frames_received);
    row_u64("frames decoded", rx_stats.video_frames_decoded);
    row_u64("frames rendered", rx_stats.video_frames_rendered);
    row_u64("sink frames", sink_frames);
    if (counter && counter->width() != 0) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%ux%u",
                      counter->width(), counter->height());
        row("decoded picture", buf);
    }

    // ---- The verdict -------------------------------------------------------
    //
    // The criterion is a *measurement*, so the verdict is a measurement
    // too — not "it ran" but "the number that must be zero is zero".
    //
    // The numbers are the *steady state*: the ones measured after the
    // seam went live.  The startup transient is reported on its own
    // lines above; it is a real cost, but it is not what the criterion
    // is about, and folding it in would hide the steady state behind a
    // number that is not the seam's.
    const bool seam_live   = steady_gpu_frames > 0;
    const bool no_cpu_copy = steady_bytes_copied == 0;
    const bool no_errors   = tx_stats.video_capture_gpu_errors == 0;
    const bool got_picture = rx_stats.video_frames_decoded > 0 &&
                             rx_stats.video_frames_rendered > 0;
    const bool hw_encoder  = tx_stats.video_codec_hardware_accelerated;

    std::printf("\n  verdict\n");
    if (opts.no_zero_copy) {
        // The CPU fallback run: the criterion is *not* expected to be
        // met, and saying so is the honest outcome.
        row("mode", "CPU fallback (--no-zero-copy)");
        row_u64("cpu bytes copied", tx_stats.video_capture_device_bytes_copied);
        row("zero-copy criterion", "not applicable for this mode");
        row("picture arrived", got_picture ? "yes" : "NO");
    } else {
        row("gpu frames encoded", seam_live ? "yes" : "NO");
        row("cpu bytes copied == 0", no_cpu_copy ? "yes" : "NO");
        row("gpu errors == 0", no_errors ? "yes" : "NO");
        row("picture decoded + rendered", got_picture ? "yes" : "NO");
        row("hardware encoder", hw_encoder ? "yes" : "NO");

        const bool pass = seam_live && no_cpu_copy && no_errors &&
                          got_picture && hw_encoder;
        std::printf("\n  %s\n", pass ? "PASS" : "FAIL");
        if (!pass) {
            if (!seam_live) {
                std::printf("  the zero-copy seam never ran — the capture "
                            "device did not deliver a surface, or the codec "
                            "could not consume one\n");
            }
            if (!no_cpu_copy) {
                std::printf("  the captured pixels were copied to host "
                            "memory — the seam is not zero-copy\n");
            }
            if (!got_picture) {
                std::printf("  no picture reached the renderer\n");
            }
            if (!hw_encoder) {
                std::printf("  the encoder was not a hardware one\n");
            }
        }
    }

    std::printf("\n=======================================================\n");
    std::fflush(stdout);

    std::printf("closing the transmitting engine...\n");
    std::fflush(stdout);
    tx_engine.close();
    std::printf("closing the receiving engine...\n");
    std::fflush(stdout);
    rx_engine.close();
    std::printf("(the engines are closed)\n");
    std::fflush(stdout);
    return 0;
}
