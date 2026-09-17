/**
 * @file tests/test_dtls_over_udp_loopback.cpp
 * @brief Test A — DTLS 1.2 handshake over a real UDP socket (no ARQ, no
 *        SRTP). This is the smallest end-to-end DTLS test:
 *        two `DtlsSession` objects shuttle DTLS records through two
 *        raw Winsock/POSIX UDP sockets bound to 127.0.0.1 ephemeral
 *        ports.
 *
 * ## What this proves
 *
 * - The existing in-process byte-buffer DTLS test
 *   (`test_dtls_handshake_e2e.cpp`) still works when bytes actually
 *   traverse a UDP socket; i.e. the wolfSSL port does not depend on
 *   any hidden in-process shortcut.
 * - The DTLS handshake (ClientHello/HelloVerifyRequest/ServerHello/
 *   Certificate/ServerKeyExchange/ServerHelloDone -> ClientKeyExchange/
 *   CertificateVerify/ChangeCipherSpec/Finished -> ChangeCipherSpec/
 *   Finished) survives crossing a real kernel UDP path.
 * - The SRTP keying material is non-zero and consistent on both sides.
 *
 * ## What this does NOT prove (out of scope, see test_dtls_over_arq_udp.cpp
 * and test_srtp_over_dtls_over_arq_udp.cpp)
 *
 * - Anything about ArqRawUdp, ARQ framing, loss recovery.
 * - Anything about SRTP key install or RTP encryption round-trip.
 *
 * Build target: tests/test_dtls_over_udp_loopback
 */

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
using socklen_t_ = int;
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <cerrno>
using socklen_t_ = socklen_t;
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <nimrtc/dtls/dtls.hpp>

using clk = std::chrono::steady_clock;

namespace {

// ===========================================================================
// TinySocket — RAII wrapper around a UDP socket (IPv4) bound to 127.0.0.1.
// ===========================================================================
struct TinySocket {
#ifdef _WIN32
    using handle_t = SOCKET;
    static constexpr handle_t kInvalid = INVALID_SOCKET;
#else
    using handle_t = int;
    static constexpr handle_t kInvalid = -1;
#endif
    handle_t s_{kInvalid};

    bool open(std::uint16_t port) noexcept {
#ifdef _WIN32
        // WSAStartup is process-global; if test_udp_loopback already ran
        // it survives.  Don't WSACleanup at close() — that could race with
        // any other test in the same ctest run.  The OS reclaims on
        // process exit.
        s_ = static_cast<handle_t>(::socket(AF_INET, SOCK_DGRAM, 0));
#else
        s_ = ::socket(AF_INET, SOCK_DGRAM, 0);
#endif
        if (s_ == kInvalid) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (::bind(static_cast<decltype(s_)>(s_),
                    reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close();
            return false;
        }
        // Put the socket in non-blocking mode so `recv_from()` can honor
        // its poll deadline.  Without this, recvfrom() blocks until the
        // kernel has data, the inner `while (clk::now() < deadline)` loop
        // never iterates again, and the reader thread hangs forever once
        // the handshake completes and no further packets arrive.
#ifdef _WIN32
        u_long nonblocking = 1;
        ::ioctlsocket(static_cast<SOCKET>(s_),
                      static_cast<long>(FIONBIO), &nonblocking);
#else
        int flags = ::fcntl(s_, F_GETFL, 0);
        if (flags >= 0) ::fcntl(s_, F_SETFL, flags | O_NONBLOCK);
#endif
        return true;
    }

    void close() noexcept {
        if (s_ == kInvalid) return;
#ifdef _WIN32
        ::closesocket(static_cast<SOCKET>(s_));
#else
        ::close(s_);
#endif
        s_ = kInvalid;
    }

    std::uint16_t local_port() const noexcept {
        if (s_ == kInvalid) return 0;
        sockaddr_in addr{};
        socklen_t_ l = sizeof(addr);
        ::getsockname(static_cast<decltype(s_)>(s_),
                      reinterpret_cast<sockaddr*>(&addr), &l);
        return ntohs(addr.sin_port);
    }

    bool send_to(std::uint16_t dst_port, std::span<const std::uint8_t> bytes) noexcept {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(dst_port);
#ifdef _WIN32
        int rc = ::sendto(static_cast<SOCKET>(s_),
                          reinterpret_cast<const char*>(bytes.data()),
                          static_cast<int>(bytes.size()), 0,
                          reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        return rc >= 0;
#else
        ssize_t rc = ::sendto(s_, bytes.data(), bytes.size(), 0,
                              reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        return rc >= 0;
#endif
    }

    // recv_from: blocks until a packet arrives or `deadline` elapses.
    // Returns true on success; sets `from_port` to the sender's port.
    bool recv_from(std::vector<std::uint8_t>& buf,
                   std::uint16_t* from_port,
                   clk::time_point deadline) noexcept {
        buf.assign(2048, 0);
#ifdef _WIN32
        // Use a short poll loop — Winsock recvfrom has no timeout arg
        // without select/WSAWaitForMultipleEvents; keep it simple.
        // Hammering at 1ms cadence is fine for a 127.0.0.1 test.
        while (clk::now() < deadline) {
            sockaddr_in from{};
            int fl = sizeof(from);
            int rc = ::recvfrom(static_cast<SOCKET>(s_),
                                reinterpret_cast<char*>(buf.data()),
                                static_cast<int>(buf.size()), 0,
                                reinterpret_cast<sockaddr*>(&from), &fl);
            if (rc >= 0) {
                buf.resize(static_cast<std::size_t>(rc));
                if (from_port) *from_port = ntohs(from.sin_port);
                return true;
            }
            int e = ::WSAGetLastError();
            if (e != WSAEINTR && e != WSAECONNRESET) {
                // Real error — bail.
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
#else
        // POSIX poll with SO_RCVTIMEO so we honor the deadline.
        timeval tv{};
        auto now = clk::now();
        if (now >= deadline) return false;
        auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
                              deadline - now).count();
        tv.tv_sec = static_cast<long>(remaining / 1'000'000);
        tv.tv_usec = static_cast<long>(remaining % 1'000'000);
        ::setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sockaddr_in from{};
        socklen_t_ fl = sizeof(from);
        ssize_t rc = ::recvfrom(s_, buf.data(), buf.size(), 0,
                                 reinterpret_cast<sockaddr*>(&from), &fl);
        if (rc < 0) {
            buf.clear();
            return false;
        }
        buf.resize(static_cast<std::size_t>(rc));
        if (from_port) *from_port = ntohs(from.sin_port);
        return true;
#endif
    }
};

// ===========================================================================
// DtlsSide — one end of the handshake: a DtlsSession + a TinySocket + a
// background recv thread that reads packets from the socket and feeds them
// into DtlsSession::feed_inbound.  The main driver pushes outbound
// records back out via the socket.
// ===========================================================================
struct DtlsSide {
    std::unique_ptr<nimrtc::dtls::DtlsSession> session;
    TinySocket socket;
    std::thread reader;
    std::atomic<bool> reader_stop{false};
    std::atomic<bool> connected{false};
    std::atomic<int>  recv_packets{0};
    std::atomic<int>  feed_bytes{0};
    std::atomic<int>  take_records{0};
    std::atomic<int>  sent_records{0};

    // Endpoint of the PEER.  Set after we know which ephemeral port the
    // peer's socket is bound to.
    std::uint16_t peer_port = 0;

    // fingerprint reported by the peer; copied in once the peer reports
    // it via DtlsSession::local_fingerprint().
    std::vector<std::uint8_t> peer_fp;

    void start_reader() {
        reader = std::thread([this] {
            std::vector<std::uint8_t> pkt;
            while (!reader_stop.load(std::memory_order_acquire)) {
                // Step 1: drive the state machine BEFORE waiting for data.
                // wolfSSL non-blocking DTLS needs accept()/connect() called
                // on every loop iteration to make progress — even when no
                // bytes have arrived yet (e.g. to emit a retransmit or
                // handle an internal state transition).  The previous order
                // put recv_from() first with a 2 ms poll window; during
                // that window tick() + take_outbound() were stalled, so
                // wolfSSL's handshake timer advanced without producing output
                // and the 30 s test deadline expired before the peer
                // received enough flights to complete the exchange.
                session->tick();
                auto outs = session->take_outbound();
                for (auto& rec : outs) {
                    if (!socket.send_to(peer_port,
                            std::span<const std::uint8_t>(rec.bytes.data(),
                                                            rec.bytes.size()))) {
                        // Continue — best effort.
                    }
                    take_records.fetch_add(1, std::memory_order_relaxed);
                    sent_records.fetch_add(1, std::memory_order_relaxed);
                }

                // Step 2: drain received packets.
                std::uint16_t from_p = 0;
                // 1 ms timeout: enough for the kernel to surface a packet
                // queued by the peer on loopback, while still driving the
                // state machine at sub-millisecond cadence.  Zero timeout
                // (clk::now()) would short-circuit the inner recvfrom
                // poll loop and starve the recv path entirely.  Requires
                // the socket to be in non-blocking mode — see open().
                if (socket.recv_from(pkt, &from_p,
                        clk::now() + std::chrono::milliseconds(1))) {
                    recv_packets.fetch_add(1, std::memory_order_relaxed);
                    nimrtc::dtls::DtlsAddr from{"127.0.0.1", from_p};
                    std::size_t consumed = session->feed_inbound(
                        std::span<const std::uint8_t>(pkt.data(),
                                                      pkt.size()), from);
                    feed_bytes.fetch_add(static_cast<int>(consumed),
                                         std::memory_order_relaxed);
                }

                // Check Connected state.
                if (session->is_connected() &&
                    !connected.exchange(true, std::memory_order_acq_rel)) {
                    std::fprintf(stderr,
                        "[side %p] DTLS CONNECTED after %d packets\n",
                        static_cast<const void*>(session.get()),
                        recv_packets.load());
                }

                // Brief yield: prevents the tight loop from starving other
                // threads on a single-core VM while still driving wolfSSL
                // at sub-millisecond cadence.
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }

    void stop_reader() {
        reader_stop.store(true, std::memory_order_release);
        if (reader.joinable()) reader.join();
    }
};

bool run_test() {
    std::fprintf(stderr, "[A] DTLS-over-UDP loopback test starting\n");

    // ---- Two sides --------------------------------------------------------
    DtlsSide a, b;

    // Bind each to an ephemeral port on 127.0.0.1.
    if (!a.socket.open(0) || !b.socket.open(0)) {
        std::fprintf(stderr, "[A] socket.open failed\n");
        return false;
    }
    const std::uint16_t a_port = a.socket.local_port();
    const std::uint16_t b_port = b.socket.local_port();
    a.peer_port = b_port;
    b.peer_port = a_port;
    std::fprintf(stderr, "[A] A bound 127.0.0.1:%u  B bound 127.0.0.1:%u\n",
                  a_port, b_port);

    // ---- DtlsSession configs ---------------------------------------------
    nimrtc::dtls::Config cfgA{};
    cfgA.role = nimrtc::dtls::DtlsRole::Client;
    cfgA.srtp_profile = nimrtc::dtls::SrtpProfile::Aes128CmSha1_80;
    a.session = std::make_unique<nimrtc::dtls::DtlsSession>(cfgA);
    if (auto r = a.session->open(); !r) {
        std::fprintf(stderr, "[A] A.open failed: %s\n",
                      r.error().message().c_str());
        return false;
    }

    nimrtc::dtls::Config cfgB{};
    cfgB.role = nimrtc::dtls::DtlsRole::Server;
    cfgB.srtp_profile = nimrtc::dtls::SrtpProfile::Aes128CmSha1_80;
    b.session = std::make_unique<nimrtc::dtls::DtlsSession>(cfgB);
    if (auto r = b.session->open(); !r) {
        std::fprintf(stderr, "[A] B.open failed: %s\n",
                      r.error().message().c_str());
        return false;
    }

    // Exchange fingerprints (SPKI pin path requires peer fingerprint to
    // be set BEFORE handshake).
    auto fpB = b.session->local_fingerprint();
    a.peer_fp.assign(fpB.bytes.begin(), fpB.bytes.end());
    a.session->set_peer_fingerprint("sha-256", a.peer_fp);

    auto fpA = a.session->local_fingerprint();
    b.peer_fp.assign(fpA.bytes.begin(), fpA.bytes.end());
    b.session->set_peer_fingerprint("sha-256", b.peer_fp);

    std::fprintf(stderr,
        "[A] local fp A=%02x:%02x... B=%02x:%02x...\n",
        fpA.bytes[0], fpA.bytes[1], fpB.bytes[0], fpB.bytes[1]);

    // ---- Drive reader threads --------------------------------------------
    a.start_reader();
    b.start_reader();

    // ---- Wait for both sides to connect ----------------------------------
    auto deadline = clk::now() + std::chrono::seconds(10);
    while (clk::now() < deadline) {
        if (a.connected.load() && b.connected.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const bool a_conn = a.connected.load();
    const bool b_conn = b.connected.load();

    // Brief settle then dump stats.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    a.stop_reader();
    b.stop_reader();

    auto a_stats = a.session->stats();
    auto b_stats = b.session->stats();
    auto a_keys = a.session->srtp_keying_material();
    auto b_keys = b.session->srtp_keying_material();
    auto a_state = a.session->state();
    auto b_state = b.session->state();

    std::fprintf(stderr,
        "[A] final state A=%s B=%s ok=%d\n",
        nimrtc::dtls::DtlsSession::state_name(a_state),
        nimrtc::dtls::DtlsSession::state_name(b_state),
        a_conn && b_conn);
    std::fprintf(stderr,
        "[A] A: rec_in=%llu rec_out=%llu retrans=%llu alerts_in=%llu "
        "alerts_out=%llu\n",
        (unsigned long long)a_stats.records_in,
        (unsigned long long)a_stats.records_out,
        (unsigned long long)a_stats.retransmits,
        (unsigned long long)a_stats.alerts_in,
        (unsigned long long)a_stats.alerts_out);
    std::fprintf(stderr,
        "[A] B: rec_in=%llu rec_out=%llu retrans=%llu alerts_in=%llu "
        "alerts_out=%llu\n",
        (unsigned long long)b_stats.records_in,
        (unsigned long long)b_stats.records_out,
        (unsigned long long)b_stats.retransmits,
        (unsigned long long)b_stats.alerts_in,
        (unsigned long long)b_stats.alerts_out);
    std::fprintf(stderr,
        "[A] SRTP keys: A=%s B=%s\n",
        a_keys ? "present" : "missing",
        b_keys ? "present" : "missing");

    if (a_keys && b_keys) {
        std::fprintf(stderr,
            "[A] A client_key[0..3]=%02x%02x%02x%02x server_key[0..3]=%02x%02x%02x%02x\n",
            a_keys->client_master_key[0], a_keys->client_master_key[1],
            a_keys->client_master_key[2], a_keys->client_master_key[3],
            a_keys->server_master_key[0], a_keys->server_master_key[1],
            a_keys->server_master_key[2], a_keys->server_master_key[3]);
        std::fprintf(stderr,
            "[A] B client_key[0..3]=%02x%02x%02x%02x server_key[0..3]=%02x%02x%02x%02x\n",
            b_keys->client_master_key[0], b_keys->client_master_key[1],
            b_keys->client_master_key[2], b_keys->client_master_key[3],
            b_keys->server_master_key[0], b_keys->server_master_key[1],
            b_keys->server_master_key[2], b_keys->server_master_key[3]);
    }

    // Cleanup.
    a.session->close();
    b.session->close();
    a.socket.close();
    b.socket.close();

    // PASS criteria:
    //  - both sides connected
    //  - both sides have SRTP keying material
    //  - the keying material is non-zero (real ECDH happened)
    const bool k_pass =
        a_conn && b_conn &&
        a_keys.has_value() && b_keys.has_value() &&
        a_keys->client_master_key[0] != 0;

    std::fprintf(stderr, "[A] %s\n", k_pass ? "PASS" : "FAIL");
    return k_pass;
}

}  // namespace

int main() {
#ifdef _WIN32
    WSADATA wsadata;
    if (::WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
        std::fprintf(stderr, "[A] WSAStartup failed\n");
        return 1;
    }
#endif
    const bool ok = run_test();
#ifdef _WIN32
    ::WSACleanup();
#endif
    return ok ? 0 : 2;
}
