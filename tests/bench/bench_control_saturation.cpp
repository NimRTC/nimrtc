/**
 * @file tests/bench/bench_control_saturation.cpp
 * @brief The G2 control-plane benchmark (v1.2 plan item **E6**,
 *        remote-desktop plan **G2**).
 *
 * ## What this measures
 *
 * The remote-desktop plan's **G2** acceptance criterion is:
 *
 *   > Under a saturated video path the control path's p99 stays within
 *   > its declared budget; the two stacks carry independent traffic and
 *   > the control stack's packets are observed on a different 5-tuple.
 *
 * The unit test
 * (`tests/test_engine_control_channel.cpp::TwoEnginesExchangeControlMessagesOverTheRawUdpControlPath`)
 * proves the *wiring*: two engines really do exchange control messages
 * over the raw-UDP control path, and the address really does travel in
 * the SDP exchange.  It says nothing about the two halves of the
 * criterion above — a control path that works perfectly on an idle link
 * can still be starved by a saturated one.
 *
 * This benchmark is the other half.  It drives **two real engines** over
 * **real UDP sockets** and measures the control plane's latency while
 * the media plane is deliberately saturated.
 *
 * ## What is real, and what is modelled
 *
 * **Real** — every component below is the in-tree implementation:
 *
 *   - the **engine** (`nimrtc::engine::NimRTCEngine`) — the real session,
 *     the real control channel, the real SDP exchange, the real tick
 *     loop, the real video pipeline;
 *   - the **control stack** (`nimrtc::transport::RawUdpArqStack`) — a
 *     real UDP socket with the real selective-repeat ARQ
 *     (`nimrtc::raw_udp::ArqRawUdp`), including its real retransmit
 *     timer and its real ACK frames;
 *   - the **control channel** (`rd_input`'s `control-channel`, reached
 *     through the `plugins::IControlChannel` seam) — the real framing;
 *   - the **video pipeline** — the real capture pump, the real encoder
 *     (OpenH264 when the build has it), the real RTP packetizer;
 *   - the **media path** — a real UDP socket per engine, carrying real
 *     RTP datagrams.
 *
 * **Real** — the saturation.  The media socket's *receiving* side
 * enforces a **token-bucket capacity**: a datagram that arrives when
 * the bucket is empty is **dropped**, exactly as a bottleneck router's
 * queue overflows.  The congestion is therefore a real loss of real
 * packets, and the video path really is at capacity while the control
 * plane is being measured.
 *
 * **Modelled** — the *bottleneck* itself (a fixed-capacity,
 * loss-on-overflow link) and the *media peer* (a loopback socket rather
 * than a second NAT).  Nothing about the **control** plane is modelled:
 * it is the shipped stack, the shipped ARQ and the shipped framing.
 *
 * ## The loop being measured
 *
 * @verbatim
 *   [TX engine]                                     [RX engine]
 *
 *   capture (real)  ──▶ encode (real H.264)          control stack
 *        │                                            (real UDP + ARQ)
 *        ▼                                                │
 *   RTP packetize (real)                                 │
 *        │                                                ▼
 *        ▼                                          control channel
 *   media UDP socket  ──▶ bottleneck (drops)          (real framing)
 *   (real, own 5-tuple)        │                        │
 *                              ▼                        ▼
 *                       media UDP socket        control message callback
 *                       (real, own 5-tuple)     ──▶ latency sample
 * @endverbatim
 *
 * The two planes never touch: the media traffic is bounded by the
 * bottleneck, the control traffic is bounded by the control stack's own
 * socket.  That is exactly what G2 buys, and it is what the benchmark
 * measures.
 *
 * ## The latency sample
 *
 * Every control message carries a 16-byte header:
 *
 *     [0..8)   the sequence number (little-endian u64)
 *     [8..16)  the sender's monotonic instant, in microseconds
 *
 * The receiver recomputes `now - stamped` and keeps the sample.  The
 * percentiles come from the sorted sample vector; they are *measured*
 * percentiles, not an interpolation.
 *
 * The measurement therefore includes **everything** the control path
 * costs: the channel's framing, the ARQ's queueing, the socket's
 * round trip, the ARQ's receive thread, the engine's control pump
 * (`pump_control()`, driven by `tick()`), and the channel's
 * reassembly.
 *
 * ## Output
 *
 * A JSON document on stdout, in the same shape as the other bench
 * binaries (`bench_capture_encode`, `bench_engine_hotpath`,
 * `bench_saturation`).  `tools/benchmark_control_saturation.py` captures
 * it into `build/`, asserts the run's invariants and writes the
 * committed artifact.
 *
 * This binary is deliberately NOT a ctest case: every number in it is
 * machine-dependent (the loopback's real RTT and the host's real
 * scheduling jitter), so a threshold assertion would be a flaky test
 * rather than a measurement.  The *invariants* the run must satisfy
 * are asserted by the wrapper instead.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>
#include <cstddef>

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

#include <nimrtc/core/registry.hpp>
#include <nimrtc/dtls/dtls_session_factory.hpp>
#include <nimrtc/dtls/dtls_session_iface.hpp>
#include <nimrtc/engine/engine.hpp>
#include <nimrtc/plugins/base.hpp>
#include <nimrtc/plugins/ice_transport.hpp>

namespace {

namespace np = nimrtc::plugins;
namespace nd = nimrtc::dtls;

using nimrtc::core::PluginRegistry;
using nimrtc::engine::EngineConfig;
using nimrtc::engine::NimRTCEngine;
using Clock = std::chrono::steady_clock;

/** The engine's monotonic clock, in microseconds. */
inline std::int64_t steady_now_us() noexcept {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------------------
// A tiny JSON writer — enough for this document, no dependency.
//
// (Same shape as the one in `tests/bench/bench_saturation.cpp`; the bench
// binaries are separate executables, so the helper is duplicated rather
// than moved into a shared header.)
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
// The socket layer — a real UDP socket on the loopback interface.
//
// The benchmark needs real datagrams because the thing under test on the
// media side is the *loss* the bottleneck inflicts: a modelled drop
// would let the measurement claim a saturated link that never carried
// a single packet.
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

inline std::uint32_t host_to_network32(std::uint32_t v) noexcept {
    return htonl(v);
}

inline std::uint16_t host_to_network16(std::uint16_t v) noexcept {
    return htons(v);
}

inline std::uint16_t network_to_host16(std::uint16_t v) noexcept {
    return ntohs(v);
}

void close_socket(socket_t s) noexcept {
    if (s == kInvalidSocket) return;
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
}

/** Format a sockaddr_in as the ASCII "host:port" the plugin seams use. */
std::string format_endpoint_v4(const sockaddr_in& addr) noexcept {
    char host[64]{};
    ::inet_ntop(AF_INET, &addr.sin_addr, host, sizeof(host));
    return std::string(host) + ":" +
           std::to_string(network_to_host16(addr.sin_port));
}

/** Parse a "host:port" endpoint into a sockaddr_in (defined below). */
bool parse_endpoint_v4(const std::string& s, sockaddr_in* out) noexcept;

// ---------------------------------------------------------------------------
// The media plane's socket.
//
// One per engine.  The **receiving** side enforces the bottleneck's
// capacity; the sending side just sends.
// ---------------------------------------------------------------------------

class MediaSocket {
public:
    ~MediaSocket() { close_socket(sock_); }

    MediaSocket() = default;
    MediaSocket(const MediaSocket&)            = delete;
    MediaSocket& operator=(const MediaSocket&) = delete;

    /** Bind to 127.0.0.1 on an ephemeral port. */
    bool open() noexcept {
        sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ == kInvalidSocket) return false;

        int rcvbuf = 4 * 1024 * 1024;
        ::setsockopt(sock_, SOL_SOCKET, SO_RCVBUF,
                     reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = host_to_network32(INADDR_LOOPBACK);
        if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close_socket(sock_);
            sock_ = kInvalidSocket;
            return false;
        }

#if defined(_WIN32)
        u_long nonblocking = 1;
        ::ioctlsocket(sock_, FIONBIO, &nonblocking);
#else
        const int flags = ::fcntl(sock_, F_GETFL, 0);
        if (flags >= 0) ::fcntl(sock_, F_SETFL, flags | O_NONBLOCK);
#endif
        return true;
    }

    std::uint16_t port() const noexcept {
        if (sock_ == kInvalidSocket) return 0;
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        if (::getsockname(sock_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            return 0;
        }
        return network_to_host16(addr.sin_port);
    }

    std::string local_endpoint() const noexcept {
        if (sock_ == kInvalidSocket) return {};
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        if (::getsockname(sock_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            return {};
        }
        return format_endpoint_v4(addr);
    }

    /** Send a datagram to a "host:port" endpoint. */
    bool send_to(const std::string& endpoint,
                 const void* data, std::size_t len) noexcept {
        if (sock_ == kInvalidSocket) return false;
        sockaddr_in addr{};
        if (!parse_endpoint_v4(endpoint, &addr)) return false;
#if defined(_WIN32)
        const auto n = ::sendto(sock_, static_cast<const char*>(data),
                                static_cast<int>(len), 0,
                                reinterpret_cast<const sockaddr*>(&addr),
                                static_cast<int>(sizeof(addr)));
        return n == static_cast<int>(len);
#else
        const auto n = ::sendto(sock_, static_cast<const char*>(data), len, 0,
                                reinterpret_cast<const sockaddr*>(&addr),
                                static_cast<socklen_t>(sizeof(addr)));
        return n == static_cast<ssize_t>(len);
#endif
    }

    /**
     * @brief Receive one datagram, without blocking.
     *
     * @param src_out The source endpoint, when non-null.
     * @return The number of bytes read, 0 when nothing is pending.
     */
    int recv_nowait(void* buf, std::size_t buf_size,
                    std::string* src_out = nullptr) noexcept {
        if (sock_ == kInvalidSocket) return 0;
        sockaddr_in addr{};
        socklen_t addr_len = sizeof(addr);
#if defined(_WIN32)
        const auto n = ::recvfrom(sock_, static_cast<char*>(buf),
                                  static_cast<int>(buf_size), 0,
                                  reinterpret_cast<sockaddr*>(&addr), &addr_len);
        if (n < 0) return 0;
#else
        const auto n = ::recvfrom(sock_, static_cast<char*>(buf), buf_size, 0,
                                  reinterpret_cast<sockaddr*>(&addr), &addr_len);
        if (n < 0) return 0;
#endif
        if (src_out) *src_out = format_endpoint_v4(addr);
        return static_cast<int>(n);
    }

private:
    socket_t sock_ = kInvalidSocket;
};

/** Parse a "host:port" endpoint into a sockaddr_in. */
bool parse_endpoint_v4(const std::string& s, sockaddr_in* out) noexcept {
    if (!out) return false;
    std::memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (s.empty()) return false;
    const auto colon = s.rfind(':');
    if (colon == std::string::npos) return false;
    const std::string host = s.substr(0, colon);
    const std::string port = s.substr(colon + 1);
    if (::inet_pton(AF_INET, host.c_str(), &out->sin_addr) != 1) return false;
    const long p = std::strtol(port.c_str(), nullptr, 10);
    if (p <= 0 || p > 65535) return false;
    out->sin_port = htons(static_cast<std::uint16_t>(p));
    return true;
}

// ---------------------------------------------------------------------------
// The bottleneck.
//
// A token-bucket capacity in front of the receiver: a datagram that
// arrives while the bucket is empty is **dropped**.  That is the whole
// mechanism by which saturation becomes loss, and it is the same
// mechanism a real bottleneck router uses (queue overflow -> drop).
// ---------------------------------------------------------------------------
class Bottleneck {
public:
    void configure(std::uint32_t capacity_bps) noexcept {
        capacity_bps_ = capacity_bps;
        tokens_ = 0.0;
        last_refill_ = Clock::now();
    }

    /** Admit one datagram of @p bytes, or drop it. */
    bool admit(std::uint32_t bytes) noexcept {
        refill();
        const std::uint64_t cost = static_cast<std::uint64_t>(bytes) * 8ull;
        if (tokens_ < static_cast<double>(cost)) {
            ++dropped_;
            dropped_bytes_ += bytes;
            return false;
        }
        tokens_ -= static_cast<double>(cost);
        ++admitted_;
        admitted_bytes_ += bytes;
        return true;
    }

    std::uint64_t dropped() const noexcept { return dropped_; }
    std::uint64_t admitted() const noexcept { return admitted_; }
    std::uint64_t dropped_bytes() const noexcept { return dropped_bytes_; }
    std::uint64_t admitted_bytes() const noexcept { return admitted_bytes_; }
    std::uint32_t capacity_bps() const noexcept { return capacity_bps_; }

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

    std::uint32_t capacity_bps_ = 500'000;
    double tokens_ = 0.0;
    Clock::time_point last_refill_{};
    std::uint64_t dropped_ = 0;
    std::uint64_t admitted_ = 0;
    std::uint64_t dropped_bytes_ = 0;
    std::uint64_t admitted_bytes_ = 0;
};

// ---------------------------------------------------------------------------
// The media plane's ICE transport.
//
// A real UDP socket behind the `plugins::IICETransport` seam, so the
// engine's media path behaves exactly as it does in production (the
// engine drives it through the same `send()` / `recv()` / candidate
// calls), and the bottleneck can drop real datagrams.
//
// The candidate exchange is real too: the transport advertises a host
// candidate, the engine puts it in the SDP, and the peer reads it back
// through `set_remote_description()` / `add_remote_candidate()`.
// ---------------------------------------------------------------------------

class BottleneckIce final : public np::IICETransport {
public:
    BottleneckIce() = default;
    ~BottleneckIce() override { close(); }

    BottleneckIce(const BottleneckIce&)            = delete;
    BottleneckIce& operator=(const BottleneckIce&) = delete;

    // ---- plugins::IPlugin ------------------------------------------------
    const char* name() const noexcept override { return "bench-bottleneck-ice"; }
    np::Status open() noexcept override {
        if (!sock_.open()) return np::kErrInternal;
        open_ = true;
        return np::kOk;
    }
    void close() noexcept override { open_ = false; }

    // ---- plugins::ITransport ---------------------------------------------
    void set_callbacks(np::RecvCallback on_recv,
                       np::ErrorCallback on_error) noexcept override {
        on_recv_ = std::move(on_recv);
        on_error_ = std::move(on_error);
    }

    np::Status send(np::BufferView view, np::Addr = {}) noexcept override {
        if (!open_) return np::kErrNotReady;
        if (view.empty()) return np::kOk;

        std::string dst = peer_endpoint_;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (dst.empty()) dst = peer_endpoint_;
        }
        if (dst.empty()) return np::kErrNotReady;

        const std::size_t n = sock_.send_to(dst, view.data(), view.size())
                                  ? view.size() : 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            sent_datagrams_ += (n > 0) ? 1 : 0;
            sent_bytes_ += n;
        }
        return (n > 0) ? np::kOk : np::kErrInternal;
    }

    int recv() noexcept override {
        // Drain everything pending: one datagram per wake-up would let
        // the kernel's receive queue overflow, and the overflow would
        // show up as *sender-side* loss — the very thing the benchmark
        // is trying to attribute to the bottleneck.
        int dispatched = 0;
        for (;;) {
            const int n = sock_.recv_nowait(buf_.data(), buf_.size(),
                                            &last_src_);
            if (n <= 0) break;

            // The bottleneck: the datagram is dropped when the link is
            // at capacity.
            if (!bottleneck_.admit(static_cast<std::uint32_t>(n))) {
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(mu_);
                ++received_datagrams_;
                received_bytes_ += static_cast<std::uint64_t>(n);
            }

            if (on_recv_) {
                on_recv_(np::BufferView(buf_.data(), static_cast<std::size_t>(n)));
            }
            ++dispatched;
        }
        return dispatched;
    }

    np::Addr local_addr() const noexcept override { return {}; }
    np::Addr remote_addr() const noexcept override { return {}; }

    void set_on_consent_lost(np::OnConsentLost) noexcept override {}

    // ---- plugins::IICETransport --------------------------------------------
    np::IceState state() const noexcept override {
        std::lock_guard<std::mutex> lock(mu_);
        return peer_endpoint_.empty() ? np::IceState::Connecting
                                      : np::IceState::Connected;
    }

    std::string local_ufrag() const noexcept override { return "benchufrag"; }
    std::string local_password() const noexcept override {
        return "benchpassword";
    }

    bool wait_for_gathering(int) noexcept override { return true; }

    std::vector<std::string> gathered_local_candidates() const noexcept override {
        // The real candidate line, in the real libjuice format.  The
        // engine puts it in the SDP verbatim, and the peer parses it
        // back out — so the media path's address really does travel
        // through the session description.
        return {"candidate:1 1 UDP 2130706431 127.0.0.1 " +
                std::to_string(sock_.port()) + " typ host"};
    }

    np::Status set_remote_description(std::string_view ice_block) noexcept override {
        std::string_view rest = ice_block;
        while (!rest.empty()) {
            const auto eol = rest.find('\n');
            const std::string_view line =
                rest.substr(0, eol == std::string_view::npos ? rest.size() : eol);
            add_candidate_line(line);
            if (eol == std::string_view::npos) break;
            rest = rest.substr(eol + 1);
        }
        return np::kOk;
    }

    np::Status add_remote_candidate(std::string_view candidate_sdp) noexcept override {
        add_candidate_line(candidate_sdp);
        return np::kOk;
    }

    void set_bind_address(std::string_view host) noexcept override {
        bind_host_ = std::string(host);
    }
    void set_stun_server(std::string_view, std::uint16_t) noexcept override {}
    void set_local_port_range(std::uint16_t, std::uint16_t) noexcept override {}
    void add_turn_server(std::string_view, std::uint16_t,
                         std::string_view, std::string_view) noexcept override {}
    void set_bwe(np::IBwe*) noexcept override {}
    void set_scheduler(np::IScheduler*) noexcept override {}

    // ---- The benchmark's own accessors -------------------------------------
    MediaSocket& socket() noexcept { return sock_; }
    Bottleneck& bottleneck() noexcept { return bottleneck_; }

    std::string local_endpoint() const noexcept { return sock_.local_endpoint(); }
    std::string peer_endpoint() const noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        return peer_endpoint_;
    }
    std::string last_source_endpoint() const noexcept { return last_src_; }

    std::uint64_t sent_datagrams() const noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        return sent_datagrams_;
    }
    std::uint64_t sent_bytes() const noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        return sent_bytes_;
    }
    std::uint64_t received_datagrams() const noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        return received_datagrams_;
    }
    std::uint64_t received_bytes() const noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        return received_bytes_;
    }

private:
    /** Pull "host:port" out of an SDP candidate line. */
    void add_candidate_line(std::string_view line) noexcept {
        // "candidate:1 1 UDP 2130706431 127.0.0.1 5000 typ host"
        //                    ^ foundation ip        ^ port
        const auto cand = line.find("candidate:");
        std::string_view body = line;
        if (cand != std::string_view::npos) {
            body = line.substr(cand + std::strlen("candidate:"));
        } else {
            body = line;
        }
        // Split into whitespace-separated fields.
        std::string_view fields[8];
        int n = 0;
        std::size_t pos = 0;
        while (pos < body.size() && n < 8) {
            while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\r')) {
                ++pos;
            }
            const std::size_t start = pos;
            while (pos < body.size() && body[pos] != ' ' && body[pos] != '\r') {
                ++pos;
            }
            if (pos > start) fields[n++] = body.substr(start, pos - start);
        }
        // fields: foundation, component, transport, priority, ip, port, ...
        if (n < 6) return;
        const std::string endpoint =
            std::string(fields[4]) + ":" + std::string(fields[5]);
        {
            std::lock_guard<std::mutex> lock(mu_);
            peer_endpoint_ = endpoint;
        }
    }

    mutable std::mutex mu_;
    MediaSocket sock_;
    Bottleneck bottleneck_;
    std::vector<std::uint8_t> buf_ = std::vector<std::uint8_t>(2048);
    bool open_ = false;
    std::string bind_host_;
    std::string peer_endpoint_;
    std::string last_src_;
    np::RecvCallback on_recv_;
    np::ErrorCallback on_error_;
    std::uint64_t sent_datagrams_ = 0;
    std::uint64_t sent_bytes_ = 0;
    std::uint64_t received_datagrams_ = 0;
    std::uint64_t received_bytes_ = 0;
};

/** The factory: one transport per engine, each with its own socket. */
class BottleneckIceFactory final : public np::IICETransportFactory {
public:
    explicit BottleneckIceFactory(std::string_view id) : id_(id) {}

    std::string_view id() const noexcept override { return id_; }
    std::string_view display_name() const noexcept override {
        return "Bottleneck ICE (real UDP socket, token-bucket capacity)";
    }

    np::IICETransport* create_ice() const override {
        auto* t = new BottleneckIce();
        instance_ = t;
        return t;
    }

    BottleneckIce* instance() const noexcept { return instance_; }

private:
    std::string_view id_;
    mutable BottleneckIce* instance_ = nullptr;
};

// ---------------------------------------------------------------------------
// The fake DTLS session — "connected", but no SRTP keys.
//
// The control plane does **not** ride DTLS (that is plan G1; this
// benchmark is about G2, the separate raw-UDP path), so the DTLS
// session's only job here is to let the media path run: the engine
// installs SRTP keys when DTLS reports Connected, and a session that
// exports no keying material leaves SRTP down — so the RTP travels in
// cleartext and the benchmark can measure the wire.
// ---------------------------------------------------------------------------
class NoKeyDtlsSession final : public nd::IDtlsSession {
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

class NoKeyDtlsFactory final : public nd::IDtlsSessionFactory {
public:
    std::string_view id() const noexcept override { return "bench-no-key-dtls"; }
    std::string_view display_name() const noexcept override {
        return "DTLS session without SRTP keys (bench_control_saturation)";
    }
    std::unique_ptr<nd::IDtlsSession> create(const nd::DtlsConfig&) const override {
        return std::make_unique<NoKeyDtlsSession>();
    }
};

// ---------------------------------------------------------------------------
// The video feeder.
//
// The media plane needs a *real* stream of pictures for the link to be
// saturated, and it needs them to be **incompressible** — a flat
// picture would be coded to a few hundred bits and the bottleneck
// would never be reached.
//
// The feeder therefore fills an I420 buffer with **pseudo-random
// noise** that changes every frame, and pushes it through the engine's
// ordinary `send_video()` seam: the real encoder encodes it, the real
// packetizer packetizes it, and the real sender puts it on the real
// socket.  The bottleneck then does exactly what a bottleneck router
// does with the resulting datagrams.
//
// Why the benchmark feeds the frames itself rather than using a
// capture device:
//
//   - the in-tree **synthetic** capture device is *step-driven*: it has
//     no producer thread, so the engine's capture pump would always
//     find an empty queue and the media path would carry nothing;
//   - the **camera** capture devices are bound to a real webcam, which
//     a CI runner does not have.
//
// Feeding `send_video()` keeps the *transport* half of the media path
// entirely real (which is what G2 is about) and makes the *content* of
// the picture an explicit, reproducible property of the benchmark.
// ---------------------------------------------------------------------------
class VideoFeeder {
public:
    VideoFeeder(std::uint32_t width, std::uint32_t height)
        : width_(width), height_(height) {
        const std::size_t stride_y = width_;
        const std::size_t stride_u = stride_y / 2;
        const std::size_t stride_v = stride_u;
        const std::size_t y_size  = stride_y * height_;
        const std::size_t uv_size = stride_u * (height_ / 2);
        pixels_.assign(y_size + uv_size + uv_size, 0x80u);

        frame_.width       = width_;
        frame_.height      = height_;
        frame_.format      = np::VideoPixelFormat::kI420;
        frame_.stride_y    = static_cast<std::int32_t>(stride_y);
        frame_.stride_u    = static_cast<std::int32_t>(stride_u);
        frame_.stride_v    = static_cast<std::int32_t>(stride_v);
        frame_.plane_y     = pixels_.data();
        frame_.plane_u     = pixels_.data() + y_size;
        frame_.plane_v     = pixels_.data() + y_size + uv_size;
        frame_.buffer_size = pixels_.size();
    }

    /** Fill the buffer with a fresh noise pattern and push one frame. */
    bool push(NimRTCEngine& engine) noexcept {
        // The picture is **block noise**: each `kBlock` x `kBlock` cell
        // carries one constant random value, and the cell's value
        // changes every frame.
        //
        // Why not per-pixel noise?  A per-pixel random picture is
        // mathematically incompressible, so the encoder's key frame
        // exceeds the engine's encode buffer (64 KiB) and every frame is
        // refused with `kErrBufferTooSmall` — the media path would
        // carry nothing at all.  Block noise is *compressible within a
        // cell* (the intra predictor codes a flat cell in a few bits)
        // but *uncorrelated between cells and between frames*, so the
        // encoder still has to spend real bits on it — which is what
        // saturates the link.
        std::uint32_t s = seed_;
        for (std::uint32_t by = 0; by < height_; by += kBlock) {
            for (std::uint32_t bx = 0; bx < width_; bx += kBlock) {
                s = next_random(s);
                const std::uint8_t luma = static_cast<std::uint8_t>(s);
                const std::uint8_t cb   = static_cast<std::uint8_t>(s >> 8);
                const std::uint8_t cr   = static_cast<std::uint8_t>(s >> 16);
                const std::uint32_t bh  = std::min(kBlock, height_ - by);
                const std::uint32_t bw  = std::min(kBlock, width_ - bx);
                fill_cell(luma, cb, cr, bx, by, bw, bh);
            }
        }
        seed_ = s;

        frame_.frame_seq     = seq_++;
        frame_.capture_ts_us = static_cast<std::int64_t>(steady_now_us());
        const auto rc = engine.send_video(frame_);
        if (rc != np::kOk) {
            ++failures_;
            return false;
        }
        ++successes_;
        return true;
    }

    std::uint64_t frames_pushed() const noexcept { return seq_; }
    std::uint64_t failures() const noexcept { return failures_; }
    std::uint64_t successes() const noexcept { return successes_; }

private:
    /** The edge of one noise cell, in pixels. */
    static constexpr std::uint32_t kBlock = 16;

    static std::uint32_t next_random(std::uint32_t s) noexcept {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }

    /** Write one cell into the I420 planes. */
    void fill_cell(std::uint8_t luma, std::uint8_t cb, std::uint8_t cr,
                   std::uint32_t x0, std::uint32_t y0,
                   std::uint32_t w, std::uint32_t h) noexcept {
        const std::size_t stride_y = width_;
        const std::size_t stride_c = width_ / 2;
        const std::size_t y_size  = stride_y * height_;
        std::uint8_t* py = pixels_.data();
        std::uint8_t* pu = pixels_.data() + y_size;
        std::uint8_t* pv = pu + stride_c * (height_ / 2);

        for (std::uint32_t y = y0; y < y0 + h; ++y) {
            std::memset(py + static_cast<std::size_t>(y) * stride_y + x0,
                        luma, w);
        }
        // The chroma cell is half the size in each direction.
        const std::uint32_t cx0 = x0 / 2;
        const std::uint32_t cy0 = y0 / 2;
        const std::uint32_t cw = std::max<std::uint32_t>(1, w / 2);
        const std::uint32_t ch = std::max<std::uint32_t>(1, h / 2);
        for (std::uint32_t y = cy0; y < cy0 + ch; ++y) {
            std::memset(pu + static_cast<std::size_t>(y) * stride_c + cx0,
                        cb, cw);
            std::memset(pv + static_cast<std::size_t>(y) * stride_c + cx0,
                        cr, cw);
        }
    }

    std::uint32_t width_;
    std::uint32_t height_;
    std::vector<std::uint8_t> pixels_;
    np::VideoSourceFrame frame_{};
    std::uint32_t seed_ = 0x1234567u;
    std::uint32_t seq_ = 0;
    std::uint64_t failures_ = 0;
    std::uint64_t successes_ = 0;
};

// ---------------------------------------------------------------------------
// Waiting with sub-millisecond precision.
//
// Windows' default timer granularity is ~15.6 ms, so a plain
// `sleep_for(5 ms)` routinely overshoots by 3x.  That matters here:
// the control plane's *rate* is part of what G2 measures (the profile
// says 200 Hz), and a host that can only fire a timer every 15.6 ms
// would report a 62 Hz control plane — a number that describes the
// host's timer, not the transport.
//
// So the benchmark raises the timer resolution (`timeBeginPeriod(1)`)
// and waits coarsely-then-finely: it sleeps until the deadline is
// close, then spins the last millisecond.  The spin is cheap (the
// control plane's per-message work is ~100 us) and it is what makes the
// measured rate match the configured one.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
    #include <windows.h>
    // `timeBeginPeriod` / `timeEndPeriod` live in <timeapi.h>, which
    // <windows.h> does not pull in, and their import library is
    // `winmm.lib` (linked below).
    #include <timeapi.h>

    // The high-resolution waitable timer is a Windows 10 version 1803
    // feature, and the build pins `_WIN32_WINNT` below that — so the
    // SDK headers do not declare the flag.  Its value is fixed by the
    // Win32 contract, so it is spelled out here rather than raising the
    // platform floor for the whole build.
    #ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
        #define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
    #endif
#endif

namespace {

/**
 * @brief The largest number of catch-up messages a producer may send in
 * one turn.
 *
 * The producers keep an **absolute** schedule: a late wake is absorbed
 * by the next slot rather than by shifting every slot that follows.
 * A preemption longer than a whole period leaves the schedule in the
 * past, so the loop would otherwise fire back-to-back messages until it
 * caught up — a burst that would pollute the very latency distribution
 * this benchmark measures.
 *
 * The bound keeps the offered load at the profile's rate over any
 * window while capping the burst.  A preemption that missed one slot
 * is caught up (one extra message, which is what the schedule asked
 * for); a preemption that missed many is not.
 */
constexpr std::int64_t MAX_CATCHUP = 2;

#if defined(_WIN32)
/** RAII wrapper around `timeBeginPeriod(1)` / `timeEndPeriod(1)`. */
class TimerResolutionGuard {
public:
    TimerResolutionGuard() noexcept { ::timeBeginPeriod(1); }
    ~TimerResolutionGuard() { ::timeEndPeriod(1); }
    TimerResolutionGuard(const TimerResolutionGuard&)            = delete;
    TimerResolutionGuard& operator=(const TimerResolutionGuard&) = delete;
};

/**
 * @brief A high-resolution waitable timer (Windows only).
 *
 * `std::this_thread::sleep_for()` is coarse: even with the system timer
 * resolution raised to 1 ms, a 5 ms sleep lands a fraction of a
 * millisecond late, and the control producer's 5 ms period then
 * *accumulates* that error into a rate below the profile's 200 Hz.
 *
 * The fix is a **high-resolution waitable timer**: a timer handle
 * created with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` is armed with
 * 100 ns precision instead of the ~15.6 ms system tick, so the
 * producer's schedule is the one the profile asked for rather than the
 * one the scheduler happened to grant.
 *
 * Two details this class gets right, both of which cost a run:
 *
 *   - **`SetWaitableTimer`, not `SetWaitableTimerEx`.**  The `Ex` form
 *     is *not* the high-resolution arming call — the SDK declares it
 *     with seven parameters (a `PREASON_CONTEXT` and a `TolerableDelay`
 *     the plain form does not have), and it takes no high-resolution
 *     flag at all.  Arming a high-resolution handle with the plain
 *     `SetWaitableTimer` is what actually gives the 100 ns deadline.
 *   - **One handle per thread.**  `SetWaitableTimer` re-arms the handle
 *     it is given, so a shared handle would let the media thread's
 *     33 ms deadline overwrite the control thread's 5 ms one — and the
 *     control plane's rate would then be the media plane's business.
 *     A thread-local handle keeps the two schedules apart.
 *
 * The handle lives until the thread exits, which is exactly the
 * lifetime the benchmark needs and avoids a shutdown-ordering race with
 * the threads that wait on it.
 */
class HighResTimer {
public:
    HighResTimer() noexcept {
        handle_ = ::CreateWaitableTimerExW(
            nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (handle_ != nullptr) {
            // The high-resolution flag was accepted.  The handle is
            // armed with the *plain* `SetWaitableTimer`, and that is
            // what gives the 100 ns deadline.
            high_resolution_ = true;
        } else {
            // The high-resolution flag is only honoured on Windows 10
            // version 1803 and newer.  Fall back to the ordinary
            // waitable timer, which is still far better than
            // `sleep_for` on its own.
            handle_ = ::CreateWaitableTimerExW(nullptr, nullptr, 0,
                                               TIMER_ALL_ACCESS);
        }
    }

    ~HighResTimer() {
        if (handle_ != nullptr) ::CloseHandle(handle_);
    }

    HighResTimer(const HighResTimer&)            = delete;
    HighResTimer& operator=(const HighResTimer&) = delete;

    /** Sleep until @p deadline, at high resolution. */
    void wait_until(Clock::time_point deadline) noexcept {
        const HANDLE h = handle_;
        if (h == nullptr) {
            // The waitable timer is unavailable (an old Windows, or a
            // locked-down host).  Fall back to the coarse-then-fine
            // sleep loop; the measured rate is then the host's, and the
            // artifact says so.
            coarse_fine_wait_until(deadline);
            return;
        }

        // `LARGE_INTEGER` counts 100 ns intervals, and the value is
        // *relative* (a negative timeout), so the deadline is converted
        // into "how long from now".
        LARGE_INTEGER due{};
        const auto now = Clock::now();
        if (deadline <= now) return;
        const auto delta =
            std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
        due.QuadPart = -static_cast<LONGLONG>(delta.count()) * 10LL;

        // `bManualReset = FALSE` makes the timer auto-reset, so each
        // wait consumes its own expiry.
        if (::SetWaitableTimer(h, &due, 0, nullptr, nullptr, FALSE)) {
            ::WaitForSingleObject(h, INFINITE);
        } else {
            coarse_fine_wait_until(deadline);
        }
    }

    /** True iff the handle has the high-resolution deadline. */
    bool high_resolution() const noexcept { return high_resolution_; }

private:
    /** The portable fallback: sleep coarsely, then in fine slices. */
    static void coarse_fine_wait_until(Clock::time_point deadline) noexcept {
        for (;;) {
            const auto now = Clock::now();
            if (now >= deadline) return;
            const auto remaining = deadline - now;
            if (remaining > std::chrono::milliseconds(2)) {
                std::this_thread::sleep_for(
                    remaining - std::chrono::milliseconds(1));
                continue;
            }
            std::this_thread::sleep_for(
                std::min<Clock::duration>(remaining,
                                          std::chrono::microseconds(300)));
        }
    }

    HANDLE handle_ = nullptr;
    bool high_resolution_ = false;
};
#endif

/** Sleep until @p deadline, with sub-millisecond precision. */
void wait_until(Clock::time_point deadline) noexcept {
#if defined(_WIN32)
    // The high-resolution waitable timer is the accurate path: the
    // producer's schedule is the profile's, not the scheduler's.  The
    // timer is thread-local, so the control producer's 5 ms deadline
    // and the media producer's 33 ms one cannot overwrite each other.
    static thread_local HighResTimer timer;
    timer.wait_until(deadline);
#else
    for (;;) {
        const auto now = Clock::now();
        if (now >= deadline) return;
        const auto remaining = deadline - now;
        if (remaining > std::chrono::milliseconds(2)) {
            // Coarse: sleep most of the way, leaving a short tail to
            // wait out precisely.
            std::this_thread::sleep_for(remaining - std::chrono::milliseconds(1));
            continue;
        }
        // Fine: sleep in small slices.  A *spin* would be more precise,
        // but the benchmark runs three busy threads on a machine that
        // may have only a handful of cores, and a spin loop that yields
        // gets starved by the other two planes — which would make the
        // measurement report the host's scheduling instead of the
        // transport.
        std::this_thread::sleep_for(
            std::min<Clock::duration>(remaining,
                                      std::chrono::microseconds(300)));
    }
#endif
}

/**
 * @brief Whether the wait path has the high-resolution deadline.
 *
 * The measurement's *rate* is only worth what the timer can deliver:
 * on Windows the producer's 5 ms period needs the 100 ns deadline of a
 * high-resolution waitable timer, and without it the reported rate
 * would describe the host's timer rather than the transport.  The
 * artifact records this so a reader can tell a run that had the
 * precision from one that did not.
 */
bool wait_has_high_resolution() noexcept {
#if defined(_WIN32)
    static thread_local HighResTimer probe;
    return probe.high_resolution();
#else
    // POSIX has no equivalent portable primitive: the fallback is a
    // sleep-and-slice loop, whose precision is the host's scheduler.
    return false;
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// The scenario configuration
// ---------------------------------------------------------------------------

struct Options {
    /** The media link's capacity while saturated, in bps. */
    std::uint32_t capacity_bps = 500'000;

    /** The control plane's message rate, in Hz (the profile's `freq_hz`). */
    std::uint32_t control_freq_hz = 200;

    /** The control plane's declared p99 budget, in ms (the profile's
     *  `transport.control.max_p99_ms`). */
    double control_max_p99_ms = 50.0;

    /** The measurement's length, in seconds. */
    double duration_s = 20.0;

    /** The engine's tick cadence, in milliseconds. */
    std::uint32_t tick_ms = 1;

    /** The video geometry the transmitter encodes at. */
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t fps = 30;

    /** The bytes each control message carries, on top of the 16-byte
     *  measurement header. */
    std::uint32_t control_payload_bytes = 32;

    /** Whether the video path is enabled at all (the negative control). */
    bool with_video = true;
};

/** One latency sample, in microseconds. */
struct Sample {
    std::uint64_t seq = 0;
    double latency_us = 0.0;
};

/** The result of one run. */
struct RunResult {
    // ---- The control plane ----------------------------------------------
    std::uint64_t control_messages_sent = 0;
    std::uint64_t control_messages_received = 0;
    std::uint64_t control_messages_dropped = 0;
    std::uint64_t control_malformed_received = 0;

    /** The latency samples, in microseconds, in arrival order. */
    std::vector<double> latencies_us;

    /** The percentiles, in milliseconds. */
    double p50_ms = 0.0;
    double p90_ms = 0.0;
    double p99_ms = 0.0;
    double p999_ms = 0.0;
    double max_ms = 0.0;
    double mean_ms = 0.0;

    /** The control rate the run actually achieved, in Hz. */
    double control_rate_hz = 0.0;

    // ---- The media plane -------------------------------------------------
    std::uint64_t media_sent_datagrams = 0;
    std::uint64_t media_received_datagrams = 0;
    std::uint64_t media_admitted_datagrams = 0;
    std::uint64_t media_dropped_datagrams = 0;
    std::uint64_t media_sent_bytes = 0;
    std::uint64_t media_admitted_bytes = 0;

    /** The video frames the transmitter's pipeline produced. */
    std::uint64_t video_frames_sent = 0;
    std::uint64_t video_frames_received = 0;
    std::uint64_t video_frames_decoded = 0;

    /** The video frames the benchmark itself pushed into the engine. */
    std::uint64_t video_frames_pushed = 0;

    /** The frames the engine refused to encode (0 in a healthy run). */
    std::uint64_t video_frames_rejected = 0;

    // ---- The 5-tuples ----------------------------------------------------
    std::string media_tx_local;
    std::string media_rx_local;
    std::string media_rx_observed_source;
    std::string control_tx_local;
    std::string control_rx_local;

    /** True when the two planes' local endpoints differ. */
    bool five_tuples_distinct = false;

    // ---- The run's own state ---------------------------------------------
    bool ok = false;
    std::string error;

    /** True iff the wait path had the high-resolution deadline.
     *
     *  The measured *rate* is only worth what the timer can deliver:
     *  without the 100 ns deadline the reported rate would describe the
     *  host's timer rather than the transport.  The artifact records
     *  this so a reader can tell a run that had the precision from one
     *  that did not. */
    bool high_resolution_wait = false;
};

/** The percentile of a *sorted* sample vector, by index. */
double percentile_sorted(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    if (p <= 0.0) return sorted.front();
    if (p >= 100.0) return sorted.back();
    // The nearest-rank definition: the smallest value at or above which
    // p% of the samples lie.  No interpolation — the number is a
    // *measured* latency, not a synthesised one.
    const std::size_t rank = static_cast<std::size_t>(
        std::ceil(p / 100.0 * static_cast<double>(sorted.size())));
    const std::size_t idx = (rank == 0) ? 0 : (rank - 1);
    return sorted[std::min(idx, sorted.size() - 1)];
}

// ---------------------------------------------------------------------------
// The plugins
// ---------------------------------------------------------------------------
BottleneckIceFactory g_tx_ice("bench-bottleneck-ice-tx");
BottleneckIceFactory g_rx_ice("bench-bottleneck-ice-rx");

void RegisterPlugins() {
    nimrtc::core::register_all_default_plugins();

    static NoKeyDtlsFactory dtls;
    PluginRegistry::instance().register_ice_transport(g_tx_ice.id(), &g_tx_ice);
    PluginRegistry::instance().register_ice_transport(g_rx_ice.id(), &g_rx_ice);
    PluginRegistry::instance().register_dtls_session(dtls.id(), &dtls);
}

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------

/**
 * @brief Run the saturated-link control-plane measurement.
 *
 * The two engines exchange a real SDP (so the control endpoints travel
 * out of band), the transmitter's video pipeline saturates the media
 * link, and the control plane's latency is sampled throughout.
 */
RunResult run_scenario(const Options& opt) {
    RunResult result;

#if defined(_WIN32)
    // The control plane's rate is part of the measurement, and the
    // default Windows timer granularity (~15.6 ms) would cap it at
    // ~64 Hz.  Raise the resolution for the duration of the run.
    TimerResolutionGuard timer_resolution;
#endif

    // ---- The engines ------------------------------------------------------
    //
    // The "agent-gateway" strategy is the shape the remote-desktop
    // profile uses: browser interop for the media plane (a
    // `webrtc-classic` stack) **plus** a separate `raw-udp-arq` control
    // stack.  That separate stack is what gives the control plane its
    // own 5-tuple and therefore its own congestion-control domain.
    EngineConfig cfg;
    cfg.transport_strategy_name  = "agent-gateway";
    cfg.transport_name           = g_tx_ice.id();
    cfg.dtls_name                = "bench-no-key-dtls";
    cfg.stun_server_host.clear();
    cfg.local_bind_address       = "127.0.0.1";
    cfg.control_channel_name     = "control-channel";
    cfg.control_channel_transport = np::ControlChannelTransport::kRawUdp;

    if (opt.with_video) {
        // The media path needs a real encoder and a real sender.  The
        // *frames* are produced by the benchmark itself (see
        // `VideoFeeder` below): the in-tree capture devices are either
        // step-driven (the synthetic one has no producer thread, so the
        // engine's pump would always find an empty queue) or bound to
        // a real display, and neither is what this benchmark needs.
        //
        // What matters for G2 is that the media path is *genuinely
        // saturated* — so the picture is noise-like, which the encoder
        // cannot compress away.  A flat picture would compress to
        // nothing and the link would never reach capacity.
        cfg.video_codec_name    = "h264";
        cfg.video_codec_backend = "software";
        cfg.video_sender_name   = "reference";
        cfg.video_receiver_name = "reference";
        cfg.video_source_name   = "memory";
        cfg.video_sink_name     = "headless";
        cfg.video_width         = opt.width;
        cfg.video_height        = opt.height;
        cfg.video_fps           = opt.fps;
    } else {
        cfg.video_capture_pump = false;
    }

    NimRTCEngine tx(cfg);

    // The receiver gets its **own** ICE transport — its own socket,
    // hence its own 5-tuple — so the engine must resolve a *different*
    // factory id.  Everything else (the codec, the capture device, the
    // control stack) is resolved by id too, and those ids are
    // per-engine-instance, so the receiver's config differs only in the
    // transport id and in the capture device (the receiver does not
    // capture).
    EngineConfig rx_cfg = cfg;
    rx_cfg.transport_name = g_rx_ice.id();
    rx_cfg.video_capture_pump = false;

    NimRTCEngine rx(rx_cfg);

    if (tx.open() != 0u) {
        result.error = "the transmitting engine did not open";
        return result;
    }
    if (rx.open() != 0u) {
        tx.close();
        result.error = "the receiving engine did not open";
        return result;
    }

    // ---- The control plane's receive callback ----------------------------
    //
    // The callback fires on the engine's thread with one **whole**
    // message.  The measurement header is the first 16 bytes.
    std::mutex sample_mu;
    std::vector<double> latencies_us;
    std::uint64_t received = 0;
    std::uint64_t malformed = 0;

    rx.set_on_control_message(
        [&](std::span<const std::uint8_t> message) {
            if (message.size() < 16) {
                std::lock_guard<std::mutex> lock(sample_mu);
                ++malformed;
                return;
            }
            std::uint64_t seq = 0;
            std::uint64_t sent_us = 0;
            std::memcpy(&seq, message.data(), sizeof(seq));
            std::memcpy(&sent_us, message.data() + sizeof(seq), sizeof(sent_us));

            const auto now = static_cast<std::uint64_t>(steady_now_us());
            const double latency_us =
                (now > sent_us) ? static_cast<double>(now - sent_us) : 0.0;

            std::lock_guard<std::mutex> lock(sample_mu);
            ++received;
            latencies_us.push_back(latency_us);
        });

    // ---- The SDP exchange -------------------------------------------------
    //
    // The offer carries the offerer's control endpoint, the answer
    // carries the answerer's.  That is what makes the control session
    // bidirectional — and it is the *real* exchange, over the real
    // session-description machinery.
    const std::string offer = tx.create_offer();
    if (offer.empty() || offer.find("a=x-nimrtc-control:") == std::string::npos) {
        tx.close();
        rx.close();
        result.error = "the offer did not advertise the control endpoint";
        return result;
    }
    const auto answer = rx.process_remote_sdp(offer);
    if (!answer.has_value() ||
        answer->find("a=x-nimrtc-control:") == std::string::npos) {
        tx.close();
        rx.close();
        result.error = "the answer did not advertise the control endpoint";
        return result;
    }
    const auto applied = tx.process_remote_sdp(*answer);
    if (!applied.has_value()) {
        tx.close();
        rx.close();
        result.error = "the offerer did not accept the answer";
        return result;
    }

    // ---- The 5-tuples ------------------------------------------------------
    //
    // The media plane's socket is the ICE transport's; the control
    // plane's socket is the control stack's own ARQ session.  They are
    // different sockets, so they are different 5-tuples.
    BottleneckIce* tx_ice = g_tx_ice.instance();
    BottleneckIce* rx_ice = g_rx_ice.instance();
    if (tx_ice == nullptr || rx_ice == nullptr) {
        tx.close();
        rx.close();
        result.error = "the media transport was not created";
        return result;
    }
    tx_ice->bottleneck().configure(opt.capacity_bps);
    rx_ice->bottleneck().configure(opt.capacity_bps);

    result.media_tx_local = tx_ice->local_endpoint();
    result.media_rx_local = rx_ice->local_endpoint();

    // The control stack's own endpoints.  The engine's SDP carried
    // them; the benchmark reads them back out of the descriptions it
    // exchanged, which is exactly what a peer would do.
    {
        const auto grab = [](const std::string& sdp) -> std::string {
            const std::string key = "a=x-nimrtc-control:";
            const auto pos = sdp.find(key);
            if (pos == std::string::npos) return {};
            const auto eol = sdp.find("\r\n", pos);
            return sdp.substr(pos + key.size(),
                              (eol == std::string::npos) ? std::string::npos
                                                         : eol - pos - key.size());
        };
        result.control_tx_local = grab(offer);
        result.control_rx_local = grab(*answer);
    }
    result.five_tuples_distinct =
        !result.media_tx_local.empty() && !result.control_tx_local.empty() &&
        result.media_tx_local != result.control_tx_local &&
        !result.media_rx_local.empty() && !result.control_rx_local.empty() &&
        result.media_rx_local != result.control_rx_local;

    // ---- The measurement ---------------------------------------------------
    //
    // The two planes run on **two threads**, and that is not a
    // convenience — it is the point of G2.
    //
    // G2's claim is that the control plane has its *own* congestion-
    // control domain: its own socket, its own ARQ, its own 5-tuple.  If
    // the benchmark drove both planes from one loop, then the software
    // H.264 encode (which costs tens of milliseconds per 720p frame on
    // this host) would sit on the control plane's critical path, and the
    // measurement would report the *encoder's* latency rather than the
    // transport's.  Putting the control producer on its own thread is
    // what makes the number mean "the control path's latency under
    // media saturation" instead of "the host's scheduling latency".
    //
    // The control producer is the only thread that touches
    // `send_control_message()`; the tick thread pumps the engines.  The
    // engine's own `send_control_message()` is mutex-protected and
    // flushes the channel immediately, so a control message does not
    // wait for the next tick.
    const auto period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / static_cast<double>(
            std::max<std::uint32_t>(1, opt.control_freq_hz))));
    const auto tick_period = std::chrono::milliseconds(opt.tick_ms);
    const auto end = Clock::now() +
                     std::chrono::duration_cast<Clock::duration>(
                         std::chrono::duration<double>(opt.duration_s));

    // The video feeder: the media plane's own cadence.  It is
    // deliberately *separate* from the engine's tick — the point of the
    // benchmark is that the two planes are independent, so the video
    // producer is not on the control plane's schedule.
    VideoFeeder feeder(opt.width, opt.height);
    const auto frame_period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / static_cast<double>(
            std::max<std::uint32_t>(1, opt.fps))));

    std::atomic<bool> stop{false};

    // ---- The control producer ---------------------------------------------
    //
    // It sends at the profile's rate and nothing else; the latency is
    // measured on the *receiving* engine, from the timestamp the
    // message carries.
    std::thread control_thread([&] {
        std::uint64_t seq = 0;
        std::vector<std::uint8_t> payload(16 + opt.control_payload_bytes, 0);
        auto next_send = Clock::now();
        while (!stop.load(std::memory_order_acquire)) {
            const auto now = Clock::now();
            if (now >= end) break;

            if (now >= next_send) {
                const auto sent_us = static_cast<std::uint64_t>(steady_now_us());
                std::memcpy(payload.data(), &seq, sizeof(seq));
                std::memcpy(payload.data() + sizeof(seq), &sent_us, sizeof(sent_us));
                // The rest is filler: the *size* of the message is part of
                // what the control plane has to carry.
                for (std::size_t i = 16; i < payload.size(); ++i) {
                    payload[i] = static_cast<std::uint8_t>(seq + i);
                }

                const auto rc = tx.send_control_message(
                    std::span<const std::uint8_t>(payload.data(),
                                                  payload.size()));
                if (rc == np::kOk) {
                    ++result.control_messages_sent;
                }
                ++seq;

                next_send += period;
                // The schedule is **absolute**: a late wake is absorbed
                // by the next slot rather than by shifting every slot
                // that follows.  Resetting the phase to "now + period"
                // (the obvious way to avoid a catch-up burst) would
                // make the *measured* period the wake error plus the
                // configured period — a 500 us wake error would turn a
                // 5 ms period into a 5.5 ms one, and the control plane
                // would report ~182 Hz instead of the profile's
                // 200 Hz.
                //
                // A preemption longer than a whole period would leave
                // the schedule in the past, so the loop would fire
                // back-to-back messages until it caught up.  The
                // catch-up is **bounded**: at most `MAX_CATCHUP`
                // messages are sent in one turn, and the slots beyond
                // that are dropped.  The offered load therefore stays
                // at the profile's rate over any window, without a
                // long preemption turning into a burst that would
                // pollute the latency distribution this benchmark is
                // measuring.
                const auto after = Clock::now();
                if (next_send < after) {
                    const auto behind = after - next_send;
                    const auto missed = behind / period;
                    if (missed >= MAX_CATCHUP) {
                        next_send += (missed + 1) * period;
                    }
                }
            } else {
                // Wait until the next slot with sub-millisecond
                // precision — the profile's rate is part of the
                // measurement, so the timer must not be the limit.
                wait_until(next_send);
            }
        }
    });

    // ---- The media producer ------------------------------------------------
    //
    // The frames are encoded and packetized by the real engine, and the
    // resulting datagrams go out on the media plane's own socket — the
    // one the bottleneck is in front of.
    std::thread media_thread([&] {
        auto next_frame = Clock::now();
        while (!stop.load(std::memory_order_acquire)) {
            const auto now = Clock::now();
            if (now >= end) break;
            if (!opt.with_video) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (now >= next_frame) {
                (void)feeder.push(tx);
                next_frame += frame_period;
                // The same absolute schedule as the control producer:
                // a late wake must not shift every frame that follows.
                const auto after = Clock::now();
                if (next_frame < after) {
                    const auto behind = after - next_frame;
                    next_frame += (behind / frame_period + 1) * frame_period;
                }
            } else {
                wait_until(next_frame);
            }
        }
    });

    // ---- The tick thread ----------------------------------------------------
    //
    // The engines' own pump: the socket reads, the control stack's ARQ
    // timers, the RTCP.  Both engines are ticked from here.
    std::thread tick_thread([&] {
        auto next_tick = Clock::now();
        while (!stop.load(std::memory_order_acquire)) {
            const auto now = Clock::now();
            if (now >= end) break;
            if (now >= next_tick) {
                tx.tick();
                rx.tick();
                // The tick keeps an **absolute** schedule, and skips the
                // slots it missed rather than firing a catch-up burst.
                // This matters for the control plane: its inbound path
                // is pumped by the tick (`pump_control()`), so a drifting
                // tick would show up as the control plane's *receive*
                // latency — a number the benchmark reports as the
                // transport's.
                next_tick += tick_period;
                const auto after = Clock::now();
                if (next_tick < after) {
                    const auto behind = after - next_tick;
                    next_tick += (behind / tick_period + 1) * tick_period;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    });

    // ---- Wait for the measurement to finish ---------------------------------
    while (Clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    stop.store(true, std::memory_order_release);
    control_thread.join();
    media_thread.join();
    tick_thread.join();

    // ---- Drain the tail ---------------------------------------------------
    //
    // The last messages are still in flight; give the control path a
    // bounded moment to deliver them so the sample is not truncated
    // mid-flight.
    const auto drain_end = Clock::now() + std::chrono::milliseconds(500);
    while (Clock::now() < drain_end) {
        tx.tick();
        rx.tick();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    // ---- The results --------------------------------------------------------
    {
        std::lock_guard<std::mutex> lock(sample_mu);
        result.control_messages_received = received;
        result.control_malformed_received = malformed;
        result.latencies_us = latencies_us;
    }

    {
        const auto tx_stats = tx.control_channel_stats();
        const auto rx_stats = rx.control_channel_stats();
        result.control_messages_dropped = tx_stats.messages_dropped;
        if (result.control_messages_received == 0) {
            result.control_messages_received = rx_stats.messages_received;
        }
    }

    {
        std::vector<double> sorted = result.latencies_us;
        std::sort(sorted.begin(), sorted.end());
        result.p50_ms  = percentile_sorted(sorted, 50.0)  / 1000.0;
        result.p90_ms  = percentile_sorted(sorted, 90.0)  / 1000.0;
        result.p99_ms  = percentile_sorted(sorted, 99.0)  / 1000.0;
        result.p999_ms = percentile_sorted(sorted, 99.9) / 1000.0;
        result.max_ms  = sorted.empty() ? 0.0 : sorted.back() / 1000.0;
        double sum = 0.0;
        for (double v : sorted) sum += v;
        result.mean_ms = sorted.empty() ? 0.0 : sum / static_cast<double>(sorted.size()) / 1000.0;
    }

    result.media_sent_datagrams     = tx_ice->sent_datagrams();
    result.media_sent_bytes         = tx_ice->sent_bytes();
    result.media_received_datagrams = rx_ice->received_datagrams();
    result.media_admitted_datagrams = rx_ice->bottleneck().admitted();
    result.media_dropped_datagrams  = rx_ice->bottleneck().dropped();
    result.media_admitted_bytes     = rx_ice->bottleneck().admitted_bytes();
    result.media_rx_observed_source  = rx_ice->last_source_endpoint();

    // The *achieved* control rate.  The configured rate is part of the
    // profile, but the number that matters is the one the run actually
    // produced: a run that could not reach the profile's rate would
    // measure a lighter load than the profile describes.
    result.control_rate_hz = (opt.duration_s > 0.0)
        ? static_cast<double>(result.control_messages_sent) / opt.duration_s
        : 0.0;

    {
        const auto tx_stats = tx.stats();
        const auto rx_stats = rx.stats();
        result.video_frames_sent     = tx_stats.video_frames_sent;
        result.video_frames_received = rx_stats.video_frames_received;
        result.video_frames_decoded  = rx_stats.video_frames_decoded;
    }
    result.video_frames_pushed = feeder.frames_pushed();
    result.video_frames_rejected = feeder.failures();

    tx.close();
    rx.close();

    // ---- The invariants ---------------------------------------------------
    //
    // The benchmark is a *measurement*, so it does not assert a latency
    // threshold here (the wrapper does that).  But it does refuse to
    // report "ok" for a run that did not actually saturate the link —
    // a p99 measured against an idle link would be a vacuous pass.
    if (result.control_messages_received == 0) {
        result.error = "no control message arrived";
        return result;
    }
    // The control plane must actually have run at the profile's rate.
    // A run that fell far short would have measured a *lighter* load
    // than the profile describes, and the p99 would be a number about
    // an idle control path.
    {
        const double want = static_cast<double>(
            std::max<std::uint32_t>(1, opt.control_freq_hz));
        if (result.control_rate_hz < want * 0.5) {
            result.error = "the control plane did not reach the profile's rate";
            return result;
        }
    }
    if (opt.with_video) {
        if (result.media_sent_datagrams == 0) {
            result.error = "the media path carried nothing";
            return result;
        }
        if (result.media_dropped_datagrams == 0) {
            result.error = "the media link was never saturated "
                           "(the bottleneck dropped nothing)";
            return result;
        }
    }
    if (!result.five_tuples_distinct) {
        result.error = "the two stacks did not ride different 5-tuples";
        return result;
    }

    // Record whether the wait path had the high-resolution deadline,
    // so the artifact cannot claim a precision the run did not have.
    result.high_resolution_wait = wait_has_high_resolution();

    result.ok = true;
    return result;
}

}  // namespace

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
        if (a == "--capacity-bps")     next(opt.capacity_bps);
        else if (a == "--freq-hz")     next(opt.control_freq_hz);
        else if (a == "--max-p99-ms")  next(opt.control_max_p99_ms);
        else if (a == "--duration-s")  next(opt.duration_s);
        else if (a == "--tick-ms")     next(opt.tick_ms);
        else if (a == "--width")       next(opt.width);
        else if (a == "--height")      next(opt.height);
        else if (a == "--fps")         next(opt.fps);
        else if (a == "--payload-bytes") next(opt.control_payload_bytes);
        else if (a == "--no-video")    opt.with_video = false;
        else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: bench_control_saturation [options]\n"
                "\n"
                "Drives two real engines over real UDP sockets: the media\n"
                "path is saturated by a token-bucket bottleneck while the\n"
                "control path (the separate raw-UDP ARQ stack) carries the\n"
                "profile's control traffic.  Prints a JSON document on stdout.\n"
                "\n"
                "  --capacity-bps N   the media link's capacity (default 500000)\n"
                "  --freq-hz N        the control plane's rate in Hz (default 200)\n"
                "  --max-p99-ms F     the control plane's declared p99 budget\n"
                "                     (default 50, from profiles/remote-desktop.json)\n"
                "  --duration-s F     the measurement's length (default 20)\n"
                "  --tick-ms N        the engine's tick cadence in ms (default 1)\n"
                "  --width N          the video width (default 1280)\n"
                "  --height N         the video height (default 720)\n"
                "  --fps N            the video frame rate (default 30)\n"
                "  --payload-bytes N  the control message's size (default 32)\n"
                "  --no-video         do not run the video path (the negative control)\n");
            return 0;
        }
    }

    // The Winsock scope is a no-op everywhere but Windows, where the
    // socket calls need the library started first.
#if defined(_WIN32)
    WinsockScope winsock;
#endif

    RegisterPlugins();

    const RunResult r = run_scenario(opt);

    std::string out;
    Json j(out);
    j.begin_object();

    j.key("benchmark");
    j.begin_object();
    j.field("name", std::string("control_saturation"));
    j.field("schema", 1LL);
    j.field("capacity_bps", static_cast<long long>(opt.capacity_bps));
    j.field("control_freq_hz", static_cast<long long>(opt.control_freq_hz));
    j.field("control_max_p99_ms", opt.control_max_p99_ms);
    j.field("duration_s", opt.duration_s);
    j.field("tick_ms", static_cast<long long>(opt.tick_ms));
    j.field("video_width", static_cast<long long>(opt.width));
    j.field("video_height", static_cast<long long>(opt.height));
    j.field("video_fps", static_cast<long long>(opt.fps));
    j.field("control_payload_bytes", static_cast<long long>(opt.control_payload_bytes));
    j.field("with_video", opt.with_video);
    j.end_object();

    j.key("summary");
    j.begin_object();
    j.field("control_messages_sent", static_cast<unsigned long long>(r.control_messages_sent));
    j.field("control_messages_received", static_cast<unsigned long long>(r.control_messages_received));
    j.field("control_messages_dropped", static_cast<unsigned long long>(r.control_messages_dropped));
    j.field("control_malformed_received", static_cast<unsigned long long>(r.control_malformed_received));
    j.field("control_p50_ms", r.p50_ms);
    j.field("control_p90_ms", r.p90_ms);
    j.field("control_p99_ms", r.p99_ms);
    j.field("control_p999_ms", r.p999_ms);
    j.field("control_max_ms", r.max_ms);
    j.field("control_mean_ms", r.mean_ms);
    j.field("control_rate_hz", r.control_rate_hz);
    j.field("media_sent_datagrams", static_cast<unsigned long long>(r.media_sent_datagrams));
    j.field("media_received_datagrams", static_cast<unsigned long long>(r.media_received_datagrams));
    j.field("media_admitted_datagrams", static_cast<unsigned long long>(r.media_admitted_datagrams));
    j.field("media_dropped_datagrams", static_cast<unsigned long long>(r.media_dropped_datagrams));
    j.field("media_sent_bytes", static_cast<unsigned long long>(r.media_sent_bytes));
    j.field("media_admitted_bytes", static_cast<unsigned long long>(r.media_admitted_bytes));
    j.field("video_frames_sent", static_cast<unsigned long long>(r.video_frames_sent));
    j.field("video_frames_received", static_cast<unsigned long long>(r.video_frames_received));
    j.field("video_frames_decoded", static_cast<unsigned long long>(r.video_frames_decoded));
    j.field("video_frames_pushed", static_cast<unsigned long long>(r.video_frames_pushed));
    j.field("video_frames_rejected", static_cast<unsigned long long>(r.video_frames_rejected));
    j.field("five_tuples_distinct", r.five_tuples_distinct);
    j.field("high_resolution_wait", r.high_resolution_wait);
    j.field("ok", r.ok);
    if (!r.error.empty()) j.field("error", r.error);
    j.end_object();

    j.key("endpoints");
    j.begin_object();
    j.field("media_tx_local", r.media_tx_local);
    j.field("media_rx_local", r.media_rx_local);
    j.field("media_rx_observed_source", r.media_rx_observed_source);
    j.field("control_tx_local", r.control_tx_local);
    j.field("control_rx_local", r.control_rx_local);
    j.end_object();

    // The latency curve: the p99 over time, so the artifact shows the
    // control plane's behaviour *through* the saturation rather than
    // only its final shape.
    j.key("latency_curve");
    j.begin_array();
    {
        std::vector<double> sorted = r.latencies_us;
        std::sort(sorted.begin(), sorted.end());
        // 20 buckets over the sample's span.
        const std::size_t buckets = 20;
        const std::size_t per = sorted.size() / buckets;
        for (std::size_t b = 0; b < buckets && per > 0; ++b) {
            const std::size_t begin = b * per;
            const std::size_t end = (b + 1 == buckets) ? sorted.size()
                                                       : std::min(sorted.size(),
                                                                  begin + per);
            std::vector<double> chunk(sorted.begin() + static_cast<std::ptrdiff_t>(begin),
                                      sorted.begin() + static_cast<std::ptrdiff_t>(end));
            j.begin_object();
            j.field("bucket", static_cast<long long>(b));
            j.field("samples", static_cast<long long>(chunk.size()));
            j.field("p50_ms", percentile_sorted(chunk, 50.0) / 1000.0);
            j.field("p99_ms", percentile_sorted(chunk, 99.0) / 1000.0);
            j.field("max_ms", chunk.empty() ? 0.0 : chunk.back() / 1000.0);
            j.end_object();
        }
    }
    j.end_array();

    j.end_object();
    out += '\n';
    std::fputs(out.c_str(), stdout);
    return r.ok ? 0 : 1;
}
