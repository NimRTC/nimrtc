/**
 * @file tests/test_dtls_over_arq_udp.cpp
 * @brief Test B — DTLS 1.2 handshake over ArqRawUdp (selective-repeat ARQ
 *        over a real UDP socket).  Builds on Test A's socket bridge pattern;
 *        the only difference is that the transport is ArqRawUdp instead of
 *        a raw Winsock/POSIX socket.
 *
 * ## What this proves
 *
 * - The ARQ framing (4-byte header + payload) does not corrupt or drop
 *   DTLS records — each `send()` call wraps one DTLS record in one ARQ
 *   DATA frame, and the receiver demuxes and delivers the DTLS payload
 *   bytes intact.
 * - The ARQ retransmit mechanism does not interfere with the DTLS
 *   handshake — DTLS already has its own timeout/retransmit; ARQ adds
 *   another retransmit layer on top (ARQ DATA retransmit + DTLS retransmit
 *   on top of that).  The result is still correct: the handshake completes.
 * - Two ArqRawUdp instances on 127.0.0.1 ephemeral ports can establish
 *   a bidirectional ARQ channel and tunnel DTLS through it.
 *
 * ## What this does NOT prove
 *
 * - SRTP key install or encryption (see test_srtp_over_dtls_over_arq_udp.cpp).
 * - Loss recovery at scale (see test_raw_udp_real_loopback.cpp for ARQ
 *   state machine validation).
 * - Anything about NimRTCEngine (see test_rtp_srtp_arq_e2e.cpp).
 *
 * Build target: tests/test_dtls_over_arq_udp
 */

#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <nimrtc/dtls/dtls.hpp>
#include <nimrtc/raw_udp/arq_raw_udp.hpp>
#include <nimrtc/raw_udp/udp_socket.hpp>
#include <nimrtc/raw_udp/raw_udp_datagram.hpp>

using clk = std::chrono::steady_clock;

namespace {

// ===========================================================================
// ArqDtlsSide — one end of the stack: an ArqRawUdp + a DtlsSession.
// ArqRawUdp provides the recv thread that calls on_recv_.  Our pump loop
// drains the inbound-queue and feeds the DtlsSession, then drains
// take_outbound and sends through ArqRawUdp.
// ===========================================================================
struct ArqDtlsSide {
    std::unique_ptr<nimrtc::raw_udp::ArqRawUdp> arq;
    std::unique_ptr<nimrtc::dtls::DtlsSession> dtls;
    std::atomic<bool> connected{false};
    std::atomic<int>  arq_sent{0};
    std::atomic<int>  arq_recv{0};
    std::atomic<int>  dtls_rec_in{0};
    std::atomic<int>  dtls_rec_out{0};
    std::atomic<int>  dtls_retrans{0};

    // Endpoint of the PEER side.  Set after we know the peer's local port.
    std::uint16_t peer_port = 0;

    // Shared inbound queue — filled by ArqRawUdp's recv thread via
    // on_recv callback; drained by the pump loop.
    struct {
        std::mutex mtx;
        std::deque<std::vector<std::uint8_t>> q;
    } inbound;

    std::thread pump;
    std::atomic<bool> pump_stop{false};

    void start_pump() {
        pump = std::thread([this] {
            while (!pump_stop.load(std::memory_order_acquire)) {
                // 1. Drain ArqRawUdp's received-queue → invoke on_recv
                //    (which pushes into inbound.q).
                arq->recv();

                // 2. Feed whatever is in inbound.q into DtlsSession.
                std::deque<std::vector<std::uint8_t>> drained;
                {
                    std::lock_guard<std::mutex> lk(inbound.mtx);
                    drained.swap(inbound.q);
                }
                for (auto& bytes : drained) {
                    nimrtc::dtls::DtlsAddr addr{"127.0.0.1", peer_port};
                    auto consumed = dtls->feed_inbound(
                        std::span<const std::uint8_t>(bytes.data(), bytes.size()),
                        addr);
                    (void)consumed;
                }
                arq_recv.fetch_add(static_cast<int>(drained.size()),
                                  std::memory_order_relaxed);

                // 3. Drive DTLS state machine (tick → pumps retransmit timer).
                dtls->tick();

                // 4. Drain DTLS outbound → send through ArqRawUdp.
                //    The FIRST send on each side also sets the peer's endpoint
                //    (ArqRawUdp::send stores it).
                auto outs = dtls->take_outbound();
                for (auto& rec : outs) {
                    nimrtc::plugins::Endpoint ep =
                        nimrtc::raw_udp::UdpSocket::make_endpoint_v4(
                            "127.0.0.1", peer_port);
                    arq->send(ep,
                              std::span<const std::uint8_t>(rec.bytes.data(),
                                                           rec.bytes.size()));
                    ++arq_sent;
                }

                // 5. Check Connected state.
                if (dtls->is_connected() &&
                    !connected.exchange(true, std::memory_order_acq_rel)) {
                    auto stats = dtls->stats();
                    std::fprintf(stderr,
                        "[B] DTLS CONNECTED  recv=%d retrans=%llu\n",
                        arq_recv.load(),
                        (unsigned long long)stats.retransmits);
                }

                // Pump at ~100 Hz — DTLS is happy with any cadence up to ~1 kHz.
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }

    void stop_pump() {
        pump_stop.store(true, std::memory_order_release);
        if (pump.joinable()) pump.join();
    }
};

bool run_test() {
    std::fprintf(stderr, "[B] DTLS-over-ArqRawUdp test starting\n");

    // ---- Bind two ArqRawUdp on 127.0.0.1 ephemeral ports --------------
    ArqDtlsSide a, b;

    // We need to know each side's local port before we can set the peer's
    // endpoint.  ArqRawUdp::open() opens the underlying UDP socket; we
    // query local_endpoint() after open().
    //
    // To avoid a chicken-and-egg (A needs B's port before B is bound), we
    // bind both sides with local_port=0 (OS picks ephemeral), then query
    // each side's local_endpoint() and configure the peer's endpoint.
    //
    // ArqRawUdp::send() accepts an explicit `dst` argument — the first
    // send with a non-empty dst also installs the peer's address, so we
    // don't need a separate setter.

    // Side A.
    {
        nimrtc::raw_udp::RawUdpConfig cfg;
        cfg.local_host = "127.0.0.1";
        cfg.local_port = 0;  // OS picks ephemeral.
        cfg.enable_real_socket = true;
        cfg.rto_base_ms = 50;
        cfg.rx_window_packets = 32;
        cfg.max_retransmits = 5;
        a.arq = std::make_unique<nimrtc::raw_udp::ArqRawUdp>(cfg);

        // on_recv callback — push received bytes into the inbound queue.
        // Fires from the recv thread owned by ArqRawUdp.
        a.arq->on_recv([&a](nimrtc::plugins::BufferView bv) {
            std::vector<std::uint8_t> copy(bv.data(),
                                           bv.data() + bv.size());
            std::lock_guard<std::mutex> lk(a.inbound.mtx);
            a.inbound.q.push_back(std::move(copy));
        });

        if (a.arq->open() != nimrtc::plugins::kOk) {
            std::fprintf(stderr, "[B] A.open failed\n");
            return false;
        }
    }

    // Side B.
    {
        nimrtc::raw_udp::RawUdpConfig cfg;
        cfg.local_host = "127.0.0.1";
        cfg.local_port = 0;
        cfg.enable_real_socket = true;
        cfg.rto_base_ms = 50;
        cfg.rx_window_packets = 32;
        cfg.max_retransmits = 5;
        b.arq = std::make_unique<nimrtc::raw_udp::ArqRawUdp>(cfg);

        b.arq->on_recv([&b](nimrtc::plugins::BufferView bv) {
            std::vector<std::uint8_t> copy(bv.data(),
                                           bv.data() + bv.size());
            std::lock_guard<std::mutex> lk(b.inbound.mtx);
            b.inbound.q.push_back(std::move(copy));
        });

        if (b.arq->open() != nimrtc::plugins::kOk) {
            std::fprintf(stderr, "[B] B.open failed\n");
            a.arq->close();
            return false;
        }
    }

    // Now we know each side's ephemeral port.
    auto epA = a.arq->local_endpoint();
    auto epB = b.arq->local_endpoint();

    // Parse port from the endpoint string ("ip:port").
    // The endpoint is stored as "ip:port" ASCII in Addr.data.
    auto parse_port = [](const nimrtc::plugins::Endpoint& ep)
        -> std::uint16_t {
        std::string s(reinterpret_cast<const char*>(ep.data),
                      std::min<std::size_t>(ep.len, sizeof(ep.data)));
        auto colon = s.rfind(':');
        if (colon == std::string::npos) return 0;
        return static_cast<std::uint16_t>(
            std::stoi(s.substr(colon + 1)));
    };

    const std::uint16_t portA = parse_port(epA);
    const std::uint16_t portB = parse_port(epB);
    a.peer_port = portB;
    b.peer_port = portA;

    std::fprintf(stderr, "[B] A bound 127.0.0.1:%u  B bound 127.0.0.1:%u\n",
                 portA, portB);

    // ---- DtlsSession setup ----------------------------------------------
    nimrtc::dtls::Config cfgA{};
    cfgA.role = nimrtc::dtls::DtlsRole::Client;
    cfgA.srtp_profile = nimrtc::dtls::SrtpProfile::Aes128CmSha1_80;
    a.dtls = std::make_unique<nimrtc::dtls::DtlsSession>(cfgA);
    if (auto r = a.dtls->open(); !r) {
        std::fprintf(stderr, "[B] A.dtls open failed: %s\n",
                      r.error().message().c_str());
        return false;
    }

    nimrtc::dtls::Config cfgB{};
    cfgB.role = nimrtc::dtls::DtlsRole::Server;
    cfgB.srtp_profile = nimrtc::dtls::SrtpProfile::Aes128CmSha1_80;
    b.dtls = std::make_unique<nimrtc::dtls::DtlsSession>(cfgB);
    if (auto r = b.dtls->open(); !r) {
        std::fprintf(stderr, "[B] B.dtls open failed: %s\n",
                      r.error().message().c_str());
        return false;
    }

    // Exchange fingerprints (SPKI pin path requires peer fingerprint set
    // before the handshake begins).
    auto fpB = b.dtls->local_fingerprint();
    auto fpA = a.dtls->local_fingerprint();
    std::fprintf(stderr,
        "[B] fingerprints: A=%02x%02x... B=%02x%02x...\n",
        fpA.bytes[0], fpA.bytes[1], fpB.bytes[0], fpB.bytes[1]);

    a.dtls->set_peer_fingerprint("sha-256",
        std::vector<std::uint8_t>(fpB.bytes.begin(), fpB.bytes.end()));
    b.dtls->set_peer_fingerprint("sha-256",
        std::vector<std::uint8_t>(fpA.bytes.begin(), fpA.bytes.end()));

    // ---- Start pump threads ---------------------------------------------
    a.start_pump();
    b.start_pump();

    // ---- Wait for both to connect ----------------------------------------
    auto deadline = clk::now() + std::chrono::seconds(15);
    while (clk::now() < deadline) {
        if (a.connected.load() && b.connected.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    const bool a_ok = a.connected.load();
    const bool b_ok = b.connected.load();

    // Brief settle.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ---- Collect stats -------------------------------------------------
    a.stop_pump();
    b.stop_pump();

    auto a_stats = a.dtls->stats();
    auto b_stats = b.dtls->stats();
    auto a_keys = a.dtls->srtp_keying_material();
    auto b_keys = b.dtls->srtp_keying_material();
    auto a_arq_stats = a.arq->stats();
    auto b_arq_stats = b.arq->stats();

    std::fprintf(stderr,
        "[B] final: A=%s B=%s\n",
        nimrtc::dtls::DtlsSession::state_name(a.dtls->state()),
        nimrtc::dtls::DtlsSession::state_name(b.dtls->state()));
    std::fprintf(stderr,
        "[B] A: arq_sent=%d arq_recv=%d  "
        "dtls_in=%llu dtls_out=%llu retrans=%llu\n",
        a.arq_sent.load(), a.arq_recv.load(),
        (unsigned long long)a_stats.records_in,
        (unsigned long long)a_stats.records_out,
        (unsigned long long)a_stats.retransmits);
    std::fprintf(stderr,
        "[B] B: arq_sent=%d arq_recv=%d  "
        "dtls_in=%llu dtls_out=%llu retrans=%llu\n",
        b.arq_sent.load(), b.arq_recv.load(),
        (unsigned long long)b_stats.records_in,
        (unsigned long long)b_stats.records_out,
        (unsigned long long)b_stats.retransmits);
    std::fprintf(stderr,
        "[B] SRTP keys: A=%s B=%s\n",
        a_keys ? "present" : "missing",
        b_keys ? "present" : "missing");

    if (a_keys && b_keys) {
        // Debug: dump all 16 bytes of the client + server master keys so we
        // can distinguish "single byte happens to be 0" from "keying
        // material is genuinely all-zero (likely a DTLS export bug)".
        auto dump_hex = [](const char* label,
                           const nimrtc::dtls::SrtpKeyingMaterial& km) {
            auto hex = [](const std::array<std::uint8_t, 16>& key) {
                std::string s;
                s.reserve(32);
                static const char* h = "0123456789abcdef";
                for (auto b : key) { s.push_back(h[(b >> 4) & 0xF]); s.push_back(h[b & 0xF]); }
                return s;
            };
            std::fprintf(stderr,
                "[B] %s client_master_key=%s  server_master_key=%s\n",
                label, hex(km.client_master_key).c_str(),
                hex(km.server_master_key).c_str());
        };
        dump_hex("A", *a_keys);
        dump_hex("B", *b_keys);
    }

    // ARQ stats.
    std::fprintf(stderr,
        "[B] A ARQ: packets_sent=%llu recv=%llu retrans=%llu acks=%llu\n",
        (unsigned long long)a_arq_stats.packets_sent,
        (unsigned long long)a_arq_stats.packets_recv,
        (unsigned long long)a_arq_stats.packets_retransmit,
        (unsigned long long)a_arq_stats.acks_recv);
    std::fprintf(stderr,
        "[B] B ARQ: packets_sent=%llu recv=%llu retrans=%llu acks=%llu\n",
        (unsigned long long)b_arq_stats.packets_sent,
        (unsigned long long)b_arq_stats.packets_recv,
        (unsigned long long)b_arq_stats.packets_retransmit,
        (unsigned long long)b_arq_stats.acks_recv);

    // Cleanup.
    a.arq->close();
    b.arq->close();
    a.dtls->close();
    b.dtls->close();

    // PASS criteria:
    //  - Both sides connected.
    //  - Both have SRTP key material (proves key derivation happened correctly
    //    even with ARQ retransmits in the path).
    // The previous check `client_master_key[0] != 0` is a probabilistic
    // assertion: any single byte of a freshly-derived SRTP key can be
    // 0x00 ~1/256 of the time, causing a spurious test failure.  Verify
    // that at least ONE byte of the 16-byte master key is non-zero on
    // each side — proves key derivation ran end-to-end without relying
    // on a specific byte being non-zero.
    auto has_nonzero_byte = [](const auto& key) {
        for (auto b : key) if (b != 0) return true;
        return false;
    };
    const bool pass =
        a_ok && b_ok &&
        a_keys.has_value() && b_keys.has_value() &&
        has_nonzero_byte(a_keys->client_master_key) &&
        has_nonzero_byte(b_keys->client_master_key);

    std::fprintf(stderr, "[B] %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

}  // namespace

int main() {
    const bool ok = run_test();
    return ok ? 0 : 2;
}
