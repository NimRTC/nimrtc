/**
 * @file tests/bench/bench_saturation.cpp
 * @brief The saturated-link benchmark (v1.2 plan item **E6**).
 *
 * ## What this measures
 *
 * The plan's evidence table says:
 *
 *   | Adaptive bitrate survives congestion | ABR + pacing unit-tested |
 *   | No measurement on a **saturated** link (G2 benchmark open) |
 *
 * Unit tests prove that the bandwidth estimator, the token-bucket pacer
 * and the resolution ladder *each* behave.  They cannot prove that the
 * three of them, wired together the way the engine wires them, keep a
 * stream alive on a link that is at capacity.  That is what this
 * benchmark measures.
 *
 * ## What is real, and what is modelled
 *
 * The distinction matters, so it is stated plainly:
 *
 *   **Real** — every component below is the in-tree implementation, the
 *   one the engine uses:
 *
 *     - `bwe::Bwe` (AIMD) or `bwe::GoogCcBwe` — the shipped estimator;
 *     - `video_bitrate_control::BitrateController` — the shipped rate
 *       controller, with the shipped ladder, the resolution floor and
 *       the keyframe-cost budget;
 *     - `pacing::TokenBucket` — the shipped pacer;
 *     - a **real UDP socket pair** carrying **real datagrams** across
 *       the loopback interface.
 *
 *   **Real** — the congestion itself.  The receiving side of the link
 *   enforces a **token-bucket capacity**: datagrams that arrive when the
 *   bucket is empty are **dropped**, exactly as a bottleneck router's
 *   queue overflows.  The loss is therefore a real loss of real
 *   packets, measured by the real receiver, and it is what the real
 *   estimator's loss path reacts to.
 *
 *   **Modelled** — the *link* is a fixed-capacity, loss-on-overflow
 *   bottleneck, and the *peer* is modelled as a receiver that reports
 *   what it saw (a Receiver Report's worth of loss and RTT, plus an
 *   optional REMB) once per reporting interval.  There is no ICE, no
 *   DTLS, no SRTP and no codec in the path: the benchmark measures the
 *   **rate-control closed loop**, not the transport stack.
 *
 * ## The loop being measured
 *
 * @verbatim
 *   encoder target (the ladder rung's bitrate)
 *        |
 *        v
 *   pacing::TokenBucket  ->  UDP socket  ->  bottleneck link
 *   (the shipped pacer)      (real wire)    (real drops on overflow)
 *        ^                                       |
 *        |                                       v
 *        |                            receiver-side loss + RTT sample
 *        |                                       |
 *        |                                       v
 *        +-- bwe::Bwe::on_feedback()  <-  the feedback report
 *                   |
 *                   v
 *         the estimate (bps)
 *                   |
 *                   v
 *   BitrateController::on_bandwidth_estimate()
 *                   |
 *                   v
 *         the ladder level (the encoder target)
 * @endverbatim
 *
 * Every arrow is a call the engine makes.  The benchmark adds exactly
 * one thing the engine does not have: the **bottleneck link**, which is
 * the thing under test.
 *
 * ## The scenario, and why its numbers are what they are
 *
 * The defaults describe a **screen-content** stream (the remote-desktop
 * product) on a **congested** link:
 *
 *   - the top rung is 1080p30 at 1.6 Mbps — a realistic screen-content
 *     rate, and the ladder's smoothness-first shape puts the next rung
 *     at 810p / 0.9 Mbps;
 *   - the link's capacity is 600 kbps, so the *lowest* rung the
 *     product's 720p legibility floor allows is **twice** the
 *     capacity — the picture cannot be made small enough to fit, so
 *     the ladder must hold the floor and let the **bitrate** be
 *     throttled instead.  That is the interesting case: it is the one
 *     the floor (plan D2) and the effective-target mechanism exist for;
 *   - the link then recovers to 8 Mbps and the ladder must climb back.
 *
 * The last point is bounded by the estimator, not by the controller:
 * AIMD probes upwards at `increase_bps` (50 kbps/s by default), and
 * the controller's keyframe-cost budget (plan D3) needs the estimate to
 * reach `rung + keyframe_cost_bps` before it will pay for the IDR a
 * geometry change costs.  So a full recovery takes **tens of seconds**,
 * which is why the recovery phase is long.  A benchmark that gave the
 * controller five seconds would be measuring its own impatience, not
 * the shipped behaviour.
 *
 * ## Output
 *
 * A JSON document on stdout, in the same shape as the other bench
 * binaries (`bench_capture_encode`, `bench_engine_hotpath`).  The
 * Python wrapper `tools/benchmark_saturation.py` captures it into
 * `build/` and writes the committed artifact.
 *
 * This binary is deliberately NOT a ctest case: every number in it is
 * machine-dependent (the loopback's real RTT and the host's real
 * scheduling jitter), so a threshold assertion would be a flaky test
 * rather than a measurement.  The *invariants* the run must satisfy
 * are asserted by the wrapper instead — see `tools/benchmark_saturation.py`.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include <nimrtc/bwe/bwe.hpp>
#include <nimrtc/core/time.hpp>
#include <nimrtc/pacing/pacing.hpp>
#include <nimrtc/video_bitrate_control/controller.hpp>
#include <nimrtc/video_bitrate_control/ladder.hpp>

namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// A tiny JSON writer — enough for this document, no dependency.
//
// (Same shape as the one in `tests/bench/bench_capture_encode.cpp`; the
// bench binaries are separate executables, so the helper is duplicated
// rather than moved into a shared header.)
// ---------------------------------------------------------------------------
class Json {
public:
    explicit Json(std::string& out) : out_(out) {}

    void begin_object() { separate(); out_ += '{'; stack_.push_back(true); }
    void end_object()   { out_ += '}'; close(); }
    void begin_array()  { separate(); out_ += '['; stack_.push_back(true); }
    void end_array()    { out_ += ']'; close(); }

    void key(const std::string& k) {
        separate();
        out_ += '"';
        escape(k);
        out_ += "\":";
        expect_value_ = true;
    }

    void value(double v) {
        separate();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6f", v);
        out_ += buf;
    }
    void value(long long v) {
        separate();
        out_ += std::to_string(v);
    }
    void value(unsigned long long v) {
        separate();
        out_ += std::to_string(v);
    }
    void value(const std::string& v) {
        separate();
        out_ += '"';
        escape(v);
        out_ += '"';
    }
    void value(bool v) {
        separate();
        out_ += v ? "true" : "false";
    }

    void field(const std::string& k, double v)             { key(k); value(v); }
    void field(const std::string& k, long long v)          { key(k); value(v); }
    void field(const std::string& k, unsigned long long v) { key(k); value(v); }
    void field(const std::string& k, const std::string& v) { key(k); value(v); }
    void field(const std::string& k, bool v)               { key(k); value(v); }

private:
    void separate() {
        if (expect_value_) { expect_value_ = false; return; }
        if (stack_.empty()) return;
        if (stack_.back()) stack_.back() = false;
        else out_ += ',';
    }

    void close() {
        if (!stack_.empty()) stack_.pop_back();
        if (!stack_.empty()) stack_.back() = false;
    }

    void escape(const std::string& v) {
        for (char c : v) {
            if (c == '"' || c == '\\') { out_ += '\\'; out_ += c; }
            else if (c == '\n') out_ += "\\n";
            else out_ += c;
        }
    }

    std::string& out_;
    std::vector<bool> stack_;
    bool expect_value_ = false;
};

// ---------------------------------------------------------------------------
// The socket layer — a real UDP socket pair on the loopback interface.
//
// The benchmark needs real datagrams because the thing under test is
// the *loss* the bottleneck inflicts: a modelled drop would let the
// estimator's loss path fire without a single packet ever being
// dropped, and the measurement would prove nothing about the shipped
// code.
// ---------------------------------------------------------------------------

#if defined(_WIN32)
using socket_t = std::uintptr_t;
constexpr socket_t kInvalidSocket = static_cast<socket_t>(~0ull);
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

/** Start / stop Winsock.  A no-op everywhere but Windows. */
struct WinsockScope {
#if defined(_WIN32)
    bool ok = false;
    WinsockScope() {
        WSADATA wsa{};
        ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }
    ~WinsockScope() {
        if (ok) WSACleanup();
    }
#endif
};

void close_socket(socket_t s) noexcept {
    if (s == kInvalidSocket) return;
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
}

/**
 * @brief A UDP socket bound to the loopback interface.
 *
 * The receive end is **non-blocking** and the reader drains it in a
 * tight loop.  That is not an optimisation, it is a correctness
 * requirement: a receiver that takes one datagram per blocking
 * `recvfrom()` drains at the rate of its timeout, the kernel's receive
 * buffer overflows, and the *sender's* `sendto()` starts failing —
 * which would look exactly like bottleneck loss and would corrupt the
 * measurement.  The reader must outrun the sender.
 */
class LoopbackSocket {
public:
    LoopbackSocket() = default;
    ~LoopbackSocket() { close_socket(sock_); }

    LoopbackSocket(const LoopbackSocket&)            = delete;
    LoopbackSocket& operator=(const LoopbackSocket&) = delete;

    /** Bind to 127.0.0.1 on an ephemeral port. */
    bool open() noexcept {
        sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ == kInvalidSocket) return false;

        // A generous receive buffer, so the kernel queue never becomes
        // the bottleneck (see the class comment).
        int rcvbuf = 4 * 1024 * 1024;
        ::setsockopt(sock_, SOL_SOCKET, SO_RCVBUF,
                     reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close_socket(sock_);
            sock_ = kInvalidSocket;
            return false;
        }

        // The receive end is non-blocking: the reader drains it in a
        // tight loop (see the class comment).
#if defined(_WIN32)
        u_long nonblocking = 1;
        ::ioctlsocket(sock_, FIONBIO, &nonblocking);
#else
        const int flags = ::fcntl(sock_, F_GETFL, 0);
        if (flags >= 0) ::fcntl(sock_, F_SETFL, flags | O_NONBLOCK);
#endif
        return true;
    }

    /** The port the OS assigned. */
    std::uint16_t port() const noexcept {
        if (sock_ == kInvalidSocket) return 0;
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        if (::getsockname(sock_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            return 0;
        }
        return ::ntohs(addr.sin_port);
    }

    /** The address to send to. */
    static bool peer(std::uint16_t peer_port, sockaddr_in& out) noexcept {
        if (peer_port == 0) return false;
        out.sin_family = AF_INET;
        out.sin_port = ::htons(peer_port);
        out.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        return true;
    }

    /** Send a datagram.  Returns false on error. */
    bool send_to(const sockaddr_in& dst, const void* data, std::size_t len) noexcept {
        if (sock_ == kInvalidSocket) return false;
        const auto n = ::sendto(sock_, static_cast<const char*>(data),
                                static_cast<int>(len), 0,
                                reinterpret_cast<const sockaddr*>(&dst),
                                sizeof(dst));
        return n == static_cast<int>(len);
    }

    /**
     * @brief Receive one datagram, without blocking.
     *
     * @return The number of bytes read, 0 when nothing is pending.
     */
    int recv_nowait(void* buf, std::size_t buf_size) noexcept {
        if (sock_ == kInvalidSocket) return 0;
        const auto n = ::recvfrom(sock_, static_cast<char*>(buf),
                                  static_cast<int>(buf_size), 0, nullptr, nullptr);
        if (n < 0) return 0;   // nothing pending
        return static_cast<int>(n);
    }

private:
    socket_t sock_ = kInvalidSocket;
};

// ---------------------------------------------------------------------------
// The bottleneck link.
//
// A token-bucket capacity in front of the receiver: a datagram that
// arrives while the bucket is empty is **dropped**.  That is the whole
// mechanism by which saturation becomes loss, and it is the same
// mechanism a real bottleneck router uses (queue overflow -> drop).
//
// The bucket refills at the link's capacity, so the link is *exactly*
// capacity-bounded: a sender below capacity loses nothing, a sender
// above capacity loses the excess.
// ---------------------------------------------------------------------------
class BottleneckLink {
public:
    void configure(std::uint32_t capacity_bps) noexcept {
        capacity_bps_ = capacity_bps;
        tokens_ = 0.0;
        last_refill_ = Clock::now();
    }

    /** Change the capacity mid-run (the recovery phase). */
    void set_capacity_bps(std::uint32_t capacity_bps) noexcept {
        capacity_bps_ = capacity_bps;
        tokens_ = 0.0;
        last_refill_ = Clock::now();
    }

    /**
     * @brief Admit one datagram of @p bytes, or drop it.
     *
     * @return true when the datagram was admitted (the bucket had the
     *         credit), false when it was dropped (the bucket was empty).
     */
    bool admit(std::uint32_t bytes) noexcept {
        refill();
        const std::uint64_t cost = static_cast<std::uint64_t>(bytes) * 8ull;
        if (tokens_ < static_cast<double>(cost)) {
            ++dropped_;
            return false;
        }
        tokens_ -= static_cast<double>(cost);
        ++admitted_;
        return true;
    }

    std::uint64_t dropped() const noexcept { return dropped_; }
    std::uint64_t admitted() const noexcept { return admitted_; }

private:
    void refill() noexcept {
        const auto now = Clock::now();
        const double elapsed_s =
            std::chrono::duration<double>(now - last_refill_).count();
        last_refill_ = now;
        if (capacity_bps_ == 0) return;
        // The bucket holds at most one second of capacity, so an idle
        // link does not accumulate an unbounded burst allowance.
        const double capacity_bits = static_cast<double>(capacity_bps_);
        tokens_ = std::min(capacity_bits, tokens_ + elapsed_s * capacity_bits);
    }

    std::uint32_t capacity_bps_ = 600'000;
    double tokens_ = 0.0;
    Clock::time_point last_refill_{};
    std::uint64_t dropped_ = 0;
    std::uint64_t admitted_ = 0;
};

// ---------------------------------------------------------------------------
// The scenario configuration
// ---------------------------------------------------------------------------

struct Options {
    /** The link's capacity during the saturation phase, in bps. */
    std::uint32_t capacity_bps = 600'000;

    /** The link's capacity during the recovery phase, in bps. */
    std::uint32_t recovery_capacity_bps = 8'000'000;

    /** The link's one-way latency, in milliseconds. */
    std::uint32_t latency_ms = 20;

    /** The source's frame rate (the offered load's shape). */
    std::uint32_t source_fps = 30;

    /** The source's capture geometry. */
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;

    /** The top rung's bitrate (what the stream asks for at full quality). */
    std::uint32_t source_bitrate_bps = 1'600'000;

    /** The datagram size on the wire. */
    std::uint32_t packet_bytes = 1200;

    /** The saturation phase's length, in seconds. */
    double saturation_s = 20.0;

    /** The recovery phase's length, in seconds. */
    double recovery_s = 45.0;

    /** Which estimator to drive: "aimd" or "goog-cc". */
    std::string bwe = "aimd";

    /** The ladder mode: "quality" or "smoothness". */
    std::string mode = "smoothness";

    /** The resolution floor, in pixels of picture height (0 = no floor). */
    std::uint32_t min_height = 720;

    /** The feedback reporting interval, in milliseconds. */
    std::uint32_t report_interval_ms = 100;

    /** Whether the peer also reports REMB (its own receive-rate estimate). */
    bool remb = true;
};

/** One sample of the loop's state. */
struct Sample {
    double t_s = 0.0;
    std::uint32_t level_index = 0;
    std::uint32_t target_bps = 0;
    std::uint32_t estimate_bps = 0;
    std::uint32_t delivered_bps = 0;
    double loss_rate = 0.0;
    /** The cumulative bytes the receiver admitted, up to this sample. */
    std::uint64_t delivered_bytes = 0;
    /** The sender's own outbound rate over the last interval, in bps.
     *  This is the "measured bitrate" the controller is fed, and the
     *  number that shows the offered load against the link's capacity. */
    std::uint32_t sent_bps = 0;
    /** The cumulative bytes the sender put on the wire, up to this
     *  sample.  The windowed offered rate is differenced over this
     *  counter, exactly as the delivered rate is. */
    std::uint64_t sent_bytes = 0;
};

/** The result of one run. */
struct RunResult {
    std::vector<Sample> samples;

    // ---- The summary numbers ------------------------------------------
    std::uint32_t initial_level = 0;
    std::uint32_t level_after_saturation = 0;
    std::uint32_t target_after_saturation = 0;
    std::uint32_t estimate_after_saturation = 0;

    std::uint32_t final_level = 0;
    std::uint32_t final_target_bps = 0;
    std::uint32_t final_estimate_bps = 0;

    /** The picture height of the final rung, in pixels. */
    std::uint32_t final_height = 0;

    /** The delivered rate over the last second of the saturation phase. */
    std::uint32_t saturated_delivered_bps = 0;

    /** The delivered rate over the last second of the recovery phase. */
    std::uint32_t recovered_delivered_bps = 0;

    /** The **offered** rate (what the sender put on the wire) over the
     *  last second of the saturation phase.  The gap between this and
     *  `saturated_delivered_bps` is the load the link had to drop. */
    std::uint32_t saturated_sent_bps = 0;

    /** The offered rate over the last second of the recovery phase. */
    std::uint32_t recovered_sent_bps = 0;

    /** How many times the ladder moved, and how many of those were down. */
    std::uint64_t level_changes = 0;
    std::uint64_t level_drops = 0;
    std::uint64_t level_rises = 0;

    /** The controller's own counters. */
    std::uint64_t floor_blocks = 0;
    std::uint64_t keyframe_cost_blocks = 0;
    std::uint64_t level_changes_suppressed = 0;

    /** The wire counters. */
    std::uint64_t packets_sent = 0;
    std::uint64_t packets_received = 0;
    std::uint64_t packets_dropped = 0;

    /** The final measured loss rate (over the last report window). */
    double final_loss_rate = 0.0;

    /** The measured RTT, in milliseconds (the modelled link's). */
    double measured_rtt_ms = 0.0;

    /** True when the run completed without an internal error. */
    bool ok = false;
};

/**
 * @brief The delivered rate, in bps, over the window ending at @p end_s.
 *
 * The cumulative admitted byte count is differenced over the window, so
 * the answer does not depend on how many samples the window holds.
 */
std::uint32_t delivered_over_window(const std::vector<Sample>& samples,
                                     double start_s, double end_s) {
    std::uint64_t first_bytes = 0;
    std::uint64_t last_bytes = 0;
    bool have_first = false;

    for (const auto& s : samples) {
        if (s.t_s < start_s || s.t_s > end_s) continue;
        if (!have_first) {
            first_bytes = s.delivered_bytes;
            have_first = true;
        }
        last_bytes = s.delivered_bytes;
    }
    if (!have_first) return 0;

    const double window_s = end_s - start_s;
    if (window_s <= 0.0) return 0;
    return static_cast<std::uint32_t>(
        static_cast<double>(last_bytes - first_bytes) * 8.0 / window_s);
}

/**
 * @brief The **offered** rate, in bps, over the window ending at @p end_s.
 *
 * The offered rate is what the sender put on the wire — the load the
 * link was asked to carry.  The cumulative byte count is differenced
 * over the window, exactly as the delivered rate is, so the answer does
 * not depend on how many samples the window holds and does not inherit
 * the startup spike of the first tick.
 */
std::uint32_t sent_over_window(const std::vector<Sample>& samples,
                                double start_s, double end_s) {
    std::uint64_t first_bytes = 0;
    std::uint64_t last_bytes = 0;
    bool have_first = false;

    for (const auto& s : samples) {
        if (s.t_s < start_s || s.t_s > end_s) continue;
        if (!have_first) {
            first_bytes = s.sent_bytes;
            have_first = true;
        }
        last_bytes = s.sent_bytes;
    }
    if (!have_first) return 0;

    const double window_s = end_s - start_s;
    if (window_s <= 0.0) return 0;
    return static_cast<std::uint32_t>(
        static_cast<double>(last_bytes - first_bytes) * 8.0 / window_s);
}

/**
 * @brief Run one saturation scenario.
 *
 * The run has two phases:
 *
 *   1. **Saturation** — the link's capacity is `capacity_bps`, which is
 *      far below what the top rung asks for.  The offered load is
 *      permanently above the capacity, so the queue overflows
 *      permanently and the loss is real.
 *   2. **Recovery** — the link's capacity is raised to
 *      `recovery_capacity_bps`.  The ladder must climb back.
 */
RunResult run_scenario(const Options& opt) {
    RunResult result;

    // ---- The components under test -------------------------------------
    //
    // These are the shipped implementations.  Nothing here is a model
    // of the shipped behaviour; it *is* the shipped behaviour.

    nimrtc::plugins::BweConfig bwe_cfg{};
    bwe_cfg.initial_bitrate_bps = opt.source_bitrate_bps;
    // The estimator's ceiling is deliberately **above** the top rung's
    // bitrate.  The controller's keyframe-cost budget (plan D3) needs
    // the estimate to reach `rung + keyframe_cost_bps` before it will
    // step *up* into a geometry change, so an estimator capped exactly
    // at the top rung's bitrate can never prove that a step up is
    // affordable — the ladder would come up to the top and then be
    // stuck there for the rest of the session.  A real session's
    // estimator ceiling is independent of the encoder's top rung; the
    // shipped profiles use 10–20 Mbps against a 2–8 Mbps initial rate.
    bwe_cfg.max_bitrate_bps =
        std::max(10'000'000u, opt.source_bitrate_bps * 2);
    bwe_cfg.min_bitrate_bps = 150'000;
    bwe_cfg.increase_bps = 50'000;
    bwe_cfg.decrease_factor = 0.5;

    std::unique_ptr<nimrtc::plugins::IBwe> estimator;
    if (opt.bwe == "goog-cc") {
        estimator = std::make_unique<nimrtc::bwe::GoogCcBwe>(bwe_cfg);
    } else {
        estimator = std::make_unique<nimrtc::bwe::Bwe>(bwe_cfg);
    }
    estimator->open();

    nimrtc::plugins::VideoBitrateControllerConfig ctrl_cfg{};
    ctrl_cfg.mode = (opt.mode == "quality")
                        ? nimrtc::plugins::VideoLadderMode::kQuality
                        : nimrtc::plugins::VideoLadderMode::kSmoothness;
    ctrl_cfg.source_width = opt.width;
    ctrl_cfg.source_height = opt.height;
    ctrl_cfg.source_fps = opt.source_fps;
    ctrl_cfg.source_bitrate_bps = opt.source_bitrate_bps;
    ctrl_cfg.min_height = opt.min_height;
    ctrl_cfg.min_bitrate_bps = 150'000;
    ctrl_cfg.max_bitrate_bps = opt.source_bitrate_bps;

    nimrtc::video_bitrate_control::BitrateController controller(ctrl_cfg);
    controller.open();
    result.initial_level = controller.stats().level_index;

    // ---- The link -------------------------------------------------------
    LoopbackSocket rx;
    LoopbackSocket tx;
    if (!rx.open() || !tx.open()) {
        std::fprintf(stderr,
                     "bench_saturation: could not open the loopback sockets\n");
        return result;
    }

    BottleneckLink link;
    link.configure(opt.capacity_bps);

    nimrtc::pacing::PacerConfig pacer_cfg{};
    pacer_cfg.rate_bps = opt.source_bitrate_bps;
    nimrtc::pacing::TokenBucket pacer(pacer_cfg);

    // ---- The receiver thread --------------------------------------------
    //
    // The receiver drains the socket, asks the bottleneck whether each
    // datagram would have survived, and counts what got through.  The
    // *count* is what the feedback report is built from, exactly as a
    // Receiver Report is built from the peer's own counters.
    struct RxCounters {
        std::atomic<std::uint64_t> received{0};
        std::atomic<std::uint64_t> dropped{0};
        std::atomic<std::uint64_t> bytes{0};
        std::atomic<bool> stop{false};
    } rx_counters;

    std::thread rx_thread([&] {
        std::vector<char> buf(opt.packet_bytes + 64);
        while (!rx_counters.stop.load(std::memory_order_relaxed)) {
            // Drain everything that is pending before going back to
            // sleep: one datagram per wake-up would let the kernel's
            // receive queue overflow, and the overflow would show up
            // as *sender-side* loss — the very thing the benchmark is
            // trying to attribute to the bottleneck.
            bool idle = true;
            for (;;) {
                const int n = rx.recv_nowait(buf.data(), buf.size());
                if (n <= 0) break;
                idle = false;
                rx_counters.received.fetch_add(1, std::memory_order_relaxed);
                if (link.admit(static_cast<std::uint32_t>(n))) {
                    rx_counters.bytes.fetch_add(static_cast<std::uint64_t>(n),
                                                std::memory_order_relaxed);
                } else {
                    rx_counters.dropped.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (idle) {
                // Nothing pending: yield the CPU rather than spin at
                // 100 % on a machine with several cores.
                std::this_thread::yield();
            }
        }
    });

    // ---- The main loop ---------------------------------------------------
    //
    // One iteration per "frame tick": the source offers the current
    // rung's worth of bits, the pacer shapes them into datagrams, and
    // the link decides how many survive.  The feedback report is
    // emitted once per reporting interval, exactly as the engine's
    // RTCP path does it.
    const double total_s = opt.saturation_s + opt.recovery_s;
    const auto start = Clock::now();

    std::uint64_t last_report_sent = 0;
    std::uint64_t last_report_dropped = 0;
    std::uint64_t last_report_received = 0;
    std::uint64_t last_report_bytes = 0;
    auto last_report_at = start;

    // The delivered-rate accounting: the cumulative admitted bytes and
    // the time they were measured at.
    std::uint64_t last_rate_bytes = 0;
    auto last_rate_at = start;

    // The **sent**-rate accounting: the cumulative bytes the sender put
    // on the wire, and the time they were measured at.  This is what the
    // controller's `on_measured_bitrate()` is fed — the encoder's own
    // output, not the receiver's throughput.
    std::uint64_t tx_bytes = 0;
    std::uint64_t last_sent_bytes = 0;
    auto last_sent_at = start;

    // The **measured outbound bitrate** is a *windowed* rate, not a
    // per-tick one.
    //
    // The seam says what it wants ("the bitrate actually measured on the
    // wire"), and the engine's own telemetry computes the same figure
    // as an average over a window (`VideoCodecStats::actual_bitrate_bps`
    // is documented as "averaged over the session"), not as the
    // instantaneous rate of the last frame.
    //
    // The distinction is not cosmetic.  A per-tick rate is spiky by
    // construction — one frame's worth of bytes over one 33 ms tick —
    // and the controller's congestion check (`is_congested()`) compares
    // that measurement against the estimate *directly*.  A spike would
    // therefore read as "the encoder is overrunning", bypass the
    // hysteresis band and force the ladder down, even on a link that
    // had plenty of headroom.  The result is a ladder that oscillates
    // on measurement noise rather than on link state.
    //
    // The window below matches the reporting interval, so the figure
    // the controller sees and the figure the feedback report describes
    // cover the same span.
    const auto measured_window = std::chrono::milliseconds(opt.report_interval_ms);
    std::uint64_t measured_window_bytes = 0;
    auto measured_window_at = start;
    std::uint32_t measured_bps = 0;

    const std::size_t packet_bytes = opt.packet_bytes;
    std::vector<char> datagram(packet_bytes, 0);

    bool saturation_snapshot_taken = false;
    // The link's capacity is changed **once**, at the phase boundary.
    // Calling `set_capacity_bps()` every tick would reset the
    // bottleneck's token bucket every tick, so the link would never
    // accumulate any credit and would drop *everything* — the recovery
    // phase would measure a dead link, not a fast one.
    bool recovery_applied = false;

    // The frame tick's schedule.  `next_tick` is an **absolute** time
    // that advances by one frame interval per iteration; recomputing it
    // from `start` every time would make every iteration sleep until
    // the *same* instant, and the loop would spin at full speed once
    // that instant had passed.
    const auto frame_interval = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / opt.source_fps));
    auto next_tick = start + frame_interval;

    while (true) {
        const auto now = Clock::now();
        const double t_s = std::chrono::duration<double>(now - start).count();
        if (t_s >= total_s) break;

        // ---- The phase -------------------------------------------------
        const bool recovering = t_s >= opt.saturation_s;
        if (recovering && !recovery_applied) {
            recovery_applied = true;
            link.set_capacity_bps(opt.recovery_capacity_bps);
        }

        // ---- The encoder's target (the ladder rung) --------------------
        const auto level = controller.current_level();
        const std::uint32_t target_bps = level.bitrate_bps;

        // ---- The pacer -------------------------------------------------
        //
        // The pacer's rate follows the controller's target, which is
        // what the scheduler does.  The offered load is the *rung's*
        // bitrate: the encoder produces what the ladder says, and the
        // pacer shapes it onto the wire.
        pacer.set_rate_bps(target_bps);

        // ---- Offer one frame's worth of bits ----------------------------
        //
        // The source runs at `source_fps`; the ladder's rung may ask
        // for fewer frames per second, in which case the extra frames
        // are skipped (the plan's §B.6 frame-skip rule).
        const std::uint32_t rung_fps = level.fps > 0 ? level.fps : opt.source_fps;
        const std::uint64_t frame_bits =
            static_cast<std::uint64_t>(target_bps) / std::max(1u, rung_fps);
        const std::size_t frame_bytes =
            static_cast<std::size_t>((frame_bits + 7) / 8);

        // Split the frame into datagrams and push them through the pacer.
        const std::size_t datagrams = (frame_bytes + packet_bytes - 1) / packet_bytes;
        for (std::size_t i = 0; i < datagrams; ++i) {
            const std::size_t chunk =
                std::min(packet_bytes, frame_bytes - i * packet_bytes);
            if (chunk == 0) break;

            // The pacer decides *when* the datagram goes out.  A
            // refused packet waits for the next tick — the pacer is
            // what keeps the offered load from bursting past the
            // target, which is the whole point of the measurement.
            //
            // The refusal is **not** fatal: the pacer's burst ceiling
            // is 100 ms of traffic, so a frame that is larger than the
            // burst is shaped across several ticks.  That is the
            // pacer doing its job, and the offered rate still ends up
            // at the target over time.
            if (!pacer.try_consume(chunk, Clock::now())) continue;

            sockaddr_in dst{};
            if (!LoopbackSocket::peer(rx.port(), dst)) break;
            if (!tx.send_to(dst, datagram.data(), chunk)) break;
            ++result.packets_sent;
            tx_bytes += chunk;
        }

        // ---- The feedback report ----------------------------------------
        //
        // Once per reporting interval, the receiver's counters are
        // turned into a feedback sample and handed to the estimator —
        // the same shape as the engine's `feed_bwe_from_net_feedback()`.
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now - last_report_at).count();
        if (elapsed_ms >= static_cast<long long>(opt.report_interval_ms)) {
            const std::uint64_t received = rx_counters.received.load();
            const std::uint64_t dropped = rx_counters.dropped.load();
            const std::uint64_t bytes = rx_counters.bytes.load();
            const std::uint64_t sent = result.packets_sent;

            const std::uint64_t delta_received = received - last_report_received;
            const std::uint64_t delta_dropped = dropped - last_report_dropped;
            const std::uint64_t delta_bytes = bytes - last_report_bytes;
            const std::uint64_t delta_sent = sent - last_report_sent;

            // The loss the *peer* observed: the datagrams it never
            // saw, out of the ones that reached it.
            const std::uint64_t observed_total = delta_received + delta_dropped;
            double loss_rate = 0.0;
            if (observed_total > 0) {
                loss_rate = static_cast<double>(delta_dropped) /
                            static_cast<double>(observed_total);
            }

            // The peer's own receive-rate estimate, for REMB.
            double remb_bps = 0.0;
            if (elapsed_ms > 0) {
                remb_bps = static_cast<double>(delta_bytes) * 8.0 *
                           (1000.0 / static_cast<double>(elapsed_ms));
            }

            nimrtc::plugins::BweFeedback fb{};
            fb.timestamp = now;
            fb.signal_source = nimrtc::plugins::NetFeedbackSource::kLossRtt;
            fb.loss_rate = loss_rate;
            fb.rtt = nimrtc::core::Milliseconds{
                static_cast<std::int64_t>(opt.latency_ms) * 2};
            fb.packets_received = static_cast<std::uint32_t>(delta_received);
            fb.packets_lost = static_cast<std::uint32_t>(delta_dropped);
            fb.ack_base_seq = static_cast<std::uint16_t>(last_report_sent & 0xFFFF);
            fb.ack_seq_span = static_cast<std::uint16_t>(delta_sent & 0xFFFF);
            if (opt.remb && remb_bps > 0.0) {
                fb.remb_bps = static_cast<std::uint32_t>(remb_bps);
            }

            estimator->on_feedback(fb);

            last_report_at = now;
            last_report_sent = sent;
            last_report_received = received;
            last_report_dropped = dropped;
            last_report_bytes = bytes;

            result.final_loss_rate = loss_rate;
            result.measured_rtt_ms = static_cast<double>(fb.rtt.count());
        }

        // ---- The estimate -> the ladder ----------------------------------
        //
        // The engine's tick does exactly this: the estimate is handed to
        // the controller, and the controller's rung becomes the
        // encoder's target.
        const auto estimate = estimator->estimate();

        // The delivered rate, over the interval since the last tick.
        //
        // This is the **receiver's** throughput — what actually got
        // through the bottleneck.  It is the number the artifact reports
        // ("did the link carry what it was asked to carry?"), and it is
        // NOT what the controller is fed: see `on_measured_bitrate`
        // below.
        const std::uint64_t now_bytes = rx_counters.bytes.load();
        const double rate_elapsed_s =
            std::chrono::duration<double>(now - last_rate_at).count();
        std::uint32_t delivered_bps = 0;
        if (rate_elapsed_s >= 0.001) {
            delivered_bps = static_cast<std::uint32_t>(
                static_cast<double>(now_bytes - last_rate_bytes) * 8.0 /
                rate_elapsed_s);
        }
        last_rate_bytes = now_bytes;
        last_rate_at = now;

        // The **instantaneous** offered rate, for the artifact's curve.
        // The controller is fed the *windowed* figure below; this one is
        // what the curve shows, because it is the honest per-tick view
        // of the offered load.
        const double sent_elapsed_s =
            std::chrono::duration<double>(now - last_sent_at).count();
        std::uint32_t instant_sent_bps = 0;
        // The first tick's elapsed time is the tick's own duration, which
        // is fine — but on the very first iteration the elapsed time can
        // be a fraction of a millisecond, and dividing a whole frame's
        // bytes by it produces a meaningless spike.  A window shorter
        // than a millisecond is not a measurement.
        if (sent_elapsed_s >= 0.001) {
            instant_sent_bps = static_cast<std::uint32_t>(
                static_cast<double>(tx_bytes - last_sent_bytes) * 8.0 /
                sent_elapsed_s);
        }
        last_sent_bytes = tx_bytes;
        last_sent_at = now;

        // The **measured outbound bitrate**: what the encoder put on the
        // wire, measured on the *sender's* side over the reporting
        // window.
        //
        // This is what `on_measured_bitrate()` means (the seam's own
        // words: "the bitrate actually measured on the wire ... what the
        // encoder is producing").  The controller compares it against the
        // estimate to notice that the encoder is overrunning its target.
        //
        // Feeding it the *receiver's* throughput instead would be a
        // category error: the receiver's number is already throttled by
        // the bottleneck, so it would always look *below* the estimate,
        // the congestion check would never fire, and the controller's
        // "the encoder is overrunning" path would be dead code in the
        // measurement.
        {
            const auto window_elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - measured_window_at);
            if (window_elapsed >= measured_window) {
                const double window_s =
                    std::chrono::duration<double>(now - measured_window_at).count();
                if (window_s > 0.0) {
                    measured_bps = static_cast<std::uint32_t>(
                        static_cast<double>(tx_bytes - measured_window_bytes) * 8.0 /
                        window_s);
                }
                measured_window_bytes = tx_bytes;
                measured_window_at = now;
            }
        }
        const std::uint32_t sent_bps = measured_bps;

        controller.on_bandwidth_estimate(estimate.target_bitrate_bps);
        controller.on_measured_bitrate(sent_bps);

        // ---- The sample --------------------------------------------------
        Sample s;
        s.t_s = t_s;
        s.level_index = controller.stats().level_index;
        s.target_bps = target_bps;
        s.estimate_bps = estimate.target_bitrate_bps;
        s.delivered_bps = delivered_bps;
        s.sent_bps = instant_sent_bps;
        s.sent_bytes = tx_bytes;
        s.loss_rate = result.final_loss_rate;
        s.delivered_bytes = now_bytes;
        result.samples.push_back(s);

        // ---- The phase boundary -----------------------------------------
        if (!saturation_snapshot_taken && t_s >= opt.saturation_s - 0.5) {
            // Snapshot the state at the end of the saturation phase.
            saturation_snapshot_taken = true;
            result.level_after_saturation = controller.stats().level_index;
            result.target_after_saturation = target_bps;
            result.estimate_after_saturation = estimate.target_bitrate_bps;
        }

        // ---- The tick -----------------------------------------------------
        //
        // The source runs at `source_fps`; the loop's own pace is that
        // frame rate.  The pacer absorbs the jitter of the loop's
        // timing, which is what a pacer is for.
        next_tick += frame_interval;
        std::this_thread::sleep_until(next_tick);
    }

    // ---- The end state ---------------------------------------------------
    const auto stats = controller.stats();
    result.final_level = stats.level_index;
    result.final_target_bps = stats.target_bitrate_bps;
    result.final_estimate_bps = estimator->estimate().target_bitrate_bps;
    result.final_height = controller.current_level().height;
    result.level_changes = stats.level_changes;
    result.level_drops = stats.level_drops;
    result.level_rises = stats.level_rises;
    result.floor_blocks = stats.floor_blocks;
    result.keyframe_cost_blocks = stats.keyframe_cost_blocks;
    result.level_changes_suppressed = stats.level_changes_suppressed;

    result.packets_received = rx_counters.received.load();
    result.packets_dropped = rx_counters.dropped.load();

    // The delivered rate over the last second of each phase.
    result.saturated_delivered_bps = delivered_over_window(
        result.samples, opt.saturation_s - 1.0, opt.saturation_s);
    result.recovered_delivered_bps = delivered_over_window(
        result.samples, total_s - 1.0, total_s);

    // The offered rate over the last second of each phase.
    result.saturated_sent_bps = sent_over_window(
        result.samples, opt.saturation_s - 1.0, opt.saturation_s);
    result.recovered_sent_bps = sent_over_window(
        result.samples, total_s - 1.0, total_s);

    rx_counters.stop.store(true);
    if (rx_thread.joinable()) rx_thread.join();

    estimator->close();
    result.ok = true;
    return result;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](auto& dst) {
            if (i + 1 < argc) {
                if constexpr (std::is_same_v<std::decay_t<decltype(dst)>, std::string>) {
                    dst = argv[++i];
                } else {
                    dst = static_cast<std::decay_t<decltype(dst)>>(
                        std::atoll(argv[++i]));
                }
            }
        };
        if (a == "--capacity-bps")        next(opt.capacity_bps);
        else if (a == "--recovery-bps")   next(opt.recovery_capacity_bps);
        else if (a == "--latency-ms")     next(opt.latency_ms);
        else if (a == "--source-fps")     next(opt.source_fps);
        else if (a == "--source-bitrate") next(opt.source_bitrate_bps);
        else if (a == "--packet-bytes")   next(opt.packet_bytes);
        else if (a == "--saturation-s")   next(opt.saturation_s);
        else if (a == "--recovery-s")     next(opt.recovery_s);
        else if (a == "--bwe")            next(opt.bwe);
        else if (a == "--mode")           next(opt.mode);
        else if (a == "--min-height")     next(opt.min_height);
        else if (a == "--report-ms")      next(opt.report_interval_ms);
        else if (a == "--no-remb")        opt.remb = false;
        else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: bench_saturation [options]\n"
                "\n"
                "Drives the shipped bandwidth estimator, token-bucket pacer and\n"
                "resolution ladder against a real UDP link whose capacity is\n"
                "enforced by a token bucket, so the congestion is a real loss of\n"
                "real packets.  Prints a JSON document on stdout.\n"
                "\n"
                "  --capacity-bps N    the link's capacity while saturated (default 600000)\n"
                "  --recovery-bps N   the link's capacity after the saturation phase\n"
                "                      (default 8000000)\n"
                "  --latency-ms N     the link's one-way latency (default 20)\n"
                "  --source-fps N     the source frame rate (default 30)\n"
                "  --source-bitrate N the top rung's bitrate (default 1600000)\n"
                "  --packet-bytes N   the datagram size (default 1200)\n"
                "  --saturation-s F   the saturation phase's length (default 20)\n"
                "  --recovery-s F     the recovery phase's length (default 45)\n"
                "  --bwe NAME         \"aimd\" or \"goog-cc\" (default aimd)\n"
                "  --mode NAME        \"quality\" or \"smoothness\" (default smoothness)\n"
                "  --min-height N     the resolution floor in pixels (default 720, 0 = none)\n"
                "  --report-ms N      the feedback reporting interval (default 100)\n"
                "  --no-remb          do not report REMB from the peer\n");
            return 0;
        }
    }

    WinsockScope winsock;

    const RunResult r = run_scenario(opt);

    std::string out;
    Json j(out);
    j.begin_object();

    j.key("benchmark");
    j.begin_object();
    j.field("name", std::string("saturation"));
    j.field("schema", 1LL);
    j.field("bwe", opt.bwe);
    j.field("ladder_mode", opt.mode);
    j.field("capacity_bps", static_cast<long long>(opt.capacity_bps));
    j.field("recovery_capacity_bps", static_cast<long long>(opt.recovery_capacity_bps));
    j.field("latency_ms", static_cast<long long>(opt.latency_ms));
    j.field("source_fps", static_cast<long long>(opt.source_fps));
    j.field("source_width", static_cast<long long>(opt.width));
    j.field("source_height", static_cast<long long>(opt.height));
    j.field("source_bitrate_bps", static_cast<long long>(opt.source_bitrate_bps));
    j.field("packet_bytes", static_cast<long long>(opt.packet_bytes));
    j.field("saturation_s", opt.saturation_s);
    j.field("recovery_s", opt.recovery_s);
    j.field("min_height", static_cast<long long>(opt.min_height));
    j.field("report_interval_ms", static_cast<long long>(opt.report_interval_ms));
    j.field("remb", opt.remb);
    j.end_object();

    j.key("summary");
    j.begin_object();
    j.field("initial_level", static_cast<long long>(r.initial_level));
    j.field("level_after_saturation", static_cast<long long>(r.level_after_saturation));
    j.field("target_after_saturation_bps", static_cast<long long>(r.target_after_saturation));
    j.field("estimate_after_saturation_bps", static_cast<long long>(r.estimate_after_saturation));
    j.field("final_level", static_cast<long long>(r.final_level));
    j.field("final_height", static_cast<long long>(r.final_height));
    j.field("final_target_bps", static_cast<long long>(r.final_target_bps));
    j.field("final_estimate_bps", static_cast<long long>(r.final_estimate_bps));
    j.field("saturated_delivered_bps", static_cast<long long>(r.saturated_delivered_bps));
    j.field("recovered_delivered_bps", static_cast<long long>(r.recovered_delivered_bps));
    j.field("saturated_sent_bps", static_cast<long long>(r.saturated_sent_bps));
    j.field("recovered_sent_bps", static_cast<long long>(r.recovered_sent_bps));
    j.field("level_changes", static_cast<unsigned long long>(r.level_changes));
    j.field("level_drops", static_cast<unsigned long long>(r.level_drops));
    j.field("level_rises", static_cast<unsigned long long>(r.level_rises));
    j.field("floor_blocks", static_cast<unsigned long long>(r.floor_blocks));
    j.field("keyframe_cost_blocks", static_cast<unsigned long long>(r.keyframe_cost_blocks));
    j.field("level_changes_suppressed", static_cast<unsigned long long>(r.level_changes_suppressed));
    j.field("packets_sent", static_cast<unsigned long long>(r.packets_sent));
    j.field("packets_received", static_cast<unsigned long long>(r.packets_received));
    j.field("packets_dropped", static_cast<unsigned long long>(r.packets_dropped));
    j.field("final_loss_rate", r.final_loss_rate);
    j.field("measured_rtt_ms", r.measured_rtt_ms);
    j.end_object();

    // The curve: the response the plan asks for.  One sample every
    // 500 ms, so the artifact is readable.
    j.key("curve");
    j.begin_array();
    double next_sample_s = 0.0;
    for (const auto& s : r.samples) {
        if (s.t_s < next_sample_s) continue;
        next_sample_s += 0.5;
        j.begin_object();
        j.field("t_s", s.t_s);
        j.field("level", static_cast<long long>(s.level_index));
        j.field("target_bps", static_cast<long long>(s.target_bps));
        j.field("estimate_bps", static_cast<long long>(s.estimate_bps));
        j.field("delivered_bps", static_cast<long long>(s.delivered_bps));
        j.field("sent_bps", static_cast<long long>(s.sent_bps));
        j.field("loss_rate", s.loss_rate);
        j.end_object();
    }
    j.end_array();

    j.end_object();
    out += '\n';
    std::fputs(out.c_str(), stdout);
    return r.ok ? 0 : 1;
}
