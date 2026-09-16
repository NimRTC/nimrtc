/**
 * @file tests/test_srtp_over_dtls_over_arq_udp.cpp
 * @brief Test C — SRTP encryption/decryption round-trip over ArqRawUdp.
 *
 * Builds on Test B (DTLS-over-ArqRawUdp handshake):
 *   1. Drive DTLS handshake to Connected on both sides.
 *   2. Export SRTP keying material from each DtlsSession.
 *   3. Install keys into each side's SrtpContext:
 *        - derive_keys_for_remote(client_master_key, client_master_salt)
 *          → B's INBOUND session (unprotecting A→B packets)
 *        - derive_keys_for_local(server_master_key, server_master_salt)
 *          → A's OUTBOUND session (protecting A→B packets)
 *        (Symmetrically from B's perspective.)
 *   4. Side A builds a synthetic RTP packet (12-byte header +
 *      4-byte payload "ABCD"), SRTP-protects it, and sends it
 *      through ArqRawUdp.
 *   5. Side B receives the ARQ datagram, SRTP-unprotects it, parses
 *      the RTP header, and verifies the payload.
 *
 * ## What this proves
 *
 * - DTLS-SRTP keying material derivation survives being tunneled through
 *   ArqRawUdp (ARQ retransmit + ARQ ack frame overhead doesn't corrupt
 *   the key material export).
 * - SRTP protect/unprotect work with keys installed from the DTLS session
 *   via SrtpContext::derive_keys_for_remote / derive_keys_for_local.
 * - The 4-byte ARQ frame header does not conflict with the 12-byte RTP
 *   header or the SRTP auth tag.
 *
 * Build target: tests/test_srtp_over_dtls_over_arq_udp
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include <nimrtc/dtls/dtls.hpp>
#include <nimrtc/raw_udp/arq_raw_udp.hpp>
#include <nimrtc/raw_udp/raw_udp_datagram.hpp>
#include <nimrtc/srtp/srtp.hpp>

using clk = std::chrono::steady_clock;

namespace {

// ===========================================================================
// ArqDtlsSrtpSide — ArqRawUdp + DtlsSession + SrtpContext.
// Same pattern as test_dtls_over_arq_udp.cpp::ArqDtlsSide, plus SrtpContext.
// ===========================================================================
struct ArqDtlsSrtpSide {
    std::unique_ptr<nimrtc::raw_udp::ArqRawUdp> arq;
    std::unique_ptr<nimrtc::dtls::DtlsSession> dtls;
    std::unique_ptr<nimrtc::srtp::SrtpContext> srtp;

    std::atomic<bool> dtls_connected{false};
    std::atomic<bool> srtp_ready{false};

    std::uint16_t peer_port = 0;
    std::uint32_t ssrc = 0xDEADBEEF;

    // Inbound queue (filled by ArqRawUdp's recv thread).
    struct {
        std::mutex mtx;
        std::deque<std::vector<std::uint8_t>> q;
    } inbound;

    std::thread pump;
    std::atomic<bool> pump_stop{false};

    // A small recv queue for SRTP-encrypted packets coming from ArqRawUdp.
    // Filled by the pump loop; consumed by the SRTP test thread.
    struct {
        std::mutex mtx;
        std::deque<std::vector<std::uint8_t>> q;
    } srtp_inbound;

    void start_pump() {
        pump = std::thread([this] {
            while (!pump_stop.load(std::memory_order_acquire)) {
                // 1. Drain ArqRawUdp recv → push to inbound.q
                arq->recv();

                std::deque<std::vector<std::uint8_t>> drained;
                {
                    std::lock_guard<std::mutex> lk(inbound.mtx);
                    drained.swap(inbound.q);
                }
                for (auto& bytes : drained) {
                    // Push to srtp_inbound for the test thread to consume.
                    {
                        std::lock_guard<std::mutex> lk(srtp_inbound.mtx);
                        srtp_inbound.q.push_back(std::move(bytes));
                    }
                    // Also feed into DTLS.
                    nimrtc::dtls::DtlsAddr addr{"127.0.0.1", peer_port};
                    dtls->feed_inbound(
                        std::span<const std::uint8_t>(drained[0].data(),
                                                     drained[0].size()),
                        addr);
                }

                // 2. Drive DTLS state machine.
                dtls->tick();

                // 3. Drain DTLS outbound → send through ArqRawUdp.
                auto outs = dtls->take_outbound();
                for (auto& rec : outs) {
                    nimrtc::plugins::Endpoint ep =
                        nimrtc::raw_udp::UdpSocket::make_endpoint_v4(
                            "127.0.0.1", peer_port);
                    arq->send(ep,
                              std::span<const std::uint8_t>(rec.bytes.data(),
                                                           rec.bytes.size()));
                }

                // 4. Check state transitions.
                if (dtls->is_connected() &&
                    !dtls_connected.exchange(true,
                                             std::memory_order_acq_rel)) {
                    install_srtp_keys();
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }

    // Install SRTP keys once DTLS is Connected.
    void install_srtp_keys() {
        auto km = dtls->srtp_keying_material();
        if (!km) {
            std::fprintf(stderr,
                "[C] WARNING: DTLS Connected but no SRTP keying material\n");
            return;
        }

        // RFC 5764 §4.2: each side uses its OWN direction's keys for
        // outbound (protect), peer's keys for inbound (unprotect).
        //
        // Our side is either Client or Server depending on the DTLS role.
        // From our own srtp_keying_material():
        //   - client_master_key/salt = our peer's keys (for inbound/unprotect)
        //   - server_master_key/salt = our own keys (for outbound/protect)
        //
        // derive_keys_for_remote(peer_keys) → enables unprotect (incoming)
        // derive_keys_for_local(own_keys)   → enables protect  (outgoing)

        std::vector<std::uint8_t> client_key(km->client_master_key.begin(),
                                             km->client_master_key.end());
        std::vector<std::uint8_t> client_salt(km->client_master_salt.begin(),
                                              km->client_master_salt.end());
        std::vector<std::uint8_t> server_key(km->server_master_key.begin(),
                                             km->server_master_key.end());
        std::vector<std::uint8_t> server_salt(km->server_master_salt.begin(),
                                              km->server_master_salt.end());

        // Install peer's keys for inbound (unprotect).
        if (!client_key.empty() && !client_salt.empty()) {
            srtp->derive_keys_for_remote(client_key, client_salt,
                nimrtc::srtp::CryptoSuite::Aes128CmSha1_80);
        }
        // Install our own keys for outbound (protect).
        if (!server_key.empty() && !server_salt.empty()) {
            srtp->derive_keys_for_local(server_key, server_salt,
                nimrtc::srtp::CryptoSuite::Aes128CmSha1_80);
        }

        srtp_ready.store(true, std::memory_order_release);
        std::fprintf(stderr,
            "[C] SRTP keys installed  server_key[0]=%02x  client_key[0]=%02x\n",
            server_key[0], client_key[0]);
    }

    // Build an RTP packet (12-byte header + payload).
    static std::vector<std::uint8_t> build_rtp_packet(
            std::uint16_t seq,
            std::uint32_t ts,
            std::uint32_t ssrc,
            std::uint8_t pt,
            std::span<const std::uint8_t> payload)
    {
        std::vector<std::uint8_t> pkt;
        pkt.reserve(12 + payload.size());
        // byte 0: V=2 P=0 X=0 CC=0 → 0x80
        pkt.push_back(0x80);
        // byte 1: M=0 PT=pt
        pkt.push_back(pt & 0x7F);
        // bytes 2-3: seq (big-endian)
        pkt.push_back(static_cast<std::uint8_t>((seq >> 8) & 0xFF));
        pkt.push_back(static_cast<std::uint8_t>(seq & 0xFF));
        // bytes 4-7: timestamp (big-endian)
        pkt.push_back(static_cast<std::uint8_t>((ts >> 24) & 0xFF));
        pkt.push_back(static_cast<std::uint8_t>((ts >> 16) & 0xFF));
        pkt.push_back(static_cast<std::uint8_t>((ts >>  8) & 0xFF));
        pkt.push_back(static_cast<std::uint8_t>(ts & 0xFF));
        // bytes 8-11: ssrc (big-endian)
        pkt.push_back(static_cast<std::uint8_t>((ssrc >> 24) & 0xFF));
        pkt.push_back(static_cast<std::uint8_t>((ssrc >> 16) & 0xFF));
        pkt.push_back(static_cast<std::uint8_t>((ssrc >>  8) & 0xFF));
        pkt.push_back(static_cast<std::uint8_t>(ssrc & 0xFF));
        pkt.insert(pkt.end(), payload.begin(), payload.end());
        return pkt;
    }

    void stop_pump() {
        pump_stop.store(true, std::memory_order_release);
        if (pump.joinable()) pump.join();
    }
};

bool run_test() {
    std::fprintf(stderr,
        "[C] SRTP-over-DTLS-over-ArqRawUdp test starting\n");

    ArqDtlsSrtpSide a, b;
    a.ssrc = 0xCAFEBABE;
    b.ssrc = 0xDEADBEEF;

    // ---- Open ArqRawUdp instances ----------------------------------------
    {
        nimrtc::raw_udp::RawUdpConfig cfg;
        cfg.local_host = "127.0.0.1";
        cfg.local_port = 0;
        cfg.enable_real_socket = true;
        cfg.rto_base_ms = 50;
        cfg.rx_window_packets = 32;
        cfg.max_retransmits = 5;
        a.arq = std::make_unique<nimrtc::raw_udp::ArqRawUdp>(cfg);
        a.arq->on_recv([&a](nimrtc::plugins::BufferView bv) {
            std::vector<std::uint8_t> copy(bv.data(),
                                           bv.data() + bv.size());
            std::lock_guard<std::mutex> lk(a.inbound.mtx);
            a.inbound.q.push_back(std::move(copy));
        });
        if (a.arq->open() != nimrtc::plugins::kOk) {
            std::fprintf(stderr, "[C] A.arq open failed\n");
            return false;
        }
    }

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
            std::fprintf(stderr, "[C] B.arq open failed\n");
            return false;
        }
    }

    auto epA = a.arq->local_endpoint();
    auto epB = b.arq->local_endpoint();
    auto parse_port =
        [](const nimrtc::plugins::Endpoint& ep) -> std::uint16_t {
        std::string s(reinterpret_cast<const char*>(ep.data),
                      std::min<std::size_t>(ep.len, sizeof(ep.data)));
        auto colon = s.rfind(':');
        return colon == std::string::npos
                   ? 0
                   : static_cast<std::uint16_t>(std::stoi(s.substr(colon + 1)));
    };
    const std::uint16_t portA = parse_port(epA);
    const std::uint16_t portB = parse_port(epB);
    a.peer_port = portB;
    b.peer_port = portA;
    std::fprintf(stderr, "[C] A bound %u  B bound %u\n", portA, portB);

    // ---- DtlsSession + SrtpContext setup -------------------------------
    nimrtc::dtls::Config cfgA{};
    cfgA.role = nimrtc::dtls::DtlsRole::Client;
    cfgA.srtp_profile = nimrtc::dtls::SrtpProfile::Aes128CmSha1_80;
    a.dtls = std::make_unique<nimrtc::dtls::DtlsSession>(cfgA);
    a.srtp = std::make_unique<nimrtc::srtp::SrtpContext>();
    if (auto r = a.dtls->open(); !r) {
        std::fprintf(stderr, "[C] A.dtls open failed\n");
        return false;
    }

    nimrtc::dtls::Config cfgB{};
    cfgB.role = nimrtc::dtls::DtlsRole::Server;
    cfgB.srtp_profile = nimrtc::dtls::SrtpProfile::Aes128CmSha1_80;
    b.dtls = std::make_unique<nimrtc::dtls::DtlsSession>(cfgB);
    b.srtp = std::make_unique<nimrtc::srtp::SrtpContext>();
    if (auto r = b.dtls->open(); !r) {
        std::fprintf(stderr, "[C] B.dtls open failed\n");
        return false;
    }

    // Exchange fingerprints.
    auto fpA = a.dtls->local_fingerprint();
    auto fpB = b.dtls->local_fingerprint();
    a.dtls->set_peer_fingerprint("sha-256",
        std::vector<std::uint8_t>(fpB.bytes.begin(), fpB.bytes.end()));
    b.dtls->set_peer_fingerprint("sha-256",
        std::vector<std::uint8_t>(fpA.bytes.begin(), fpA.bytes.end()));

    // ---- Start pump threads --------------------------------------------
    a.start_pump();
    b.start_pump();

    // ---- Wait for DTLS Connected on both sides -------------------------
    auto deadline = clk::now() + std::chrono::seconds(15);
    while (clk::now() < deadline) {
        if (a.dtls_connected.load() && b.dtls_connected.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!a.dtls_connected.load() || !b.dtls_connected.load()) {
        std::fprintf(stderr, "[C] DTLS handshake timed out\n");
        a.stop_pump();
        b.stop_pump();
        return false;
    }

    // ---- Wait for SRTP keys to be installed ----------------------------
    deadline = clk::now() + std::chrono::seconds(2);
    while (clk::now() < deadline) {
        if (a.srtp_ready.load() && b.srtp_ready.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const bool a_srtp_ok = a.srtp_ready.load();
    const bool b_srtp_ok = b.srtp_ready.load();
    std::fprintf(stderr,
        "[C] DTLS Connected: A=%s B=%s  SRTP ready: A=%s B=%s\n",
        a.dtls_connected.load() ? "yes" : "no",
        b.dtls_connected.load() ? "yes" : "no",
        a_srtp_ok ? "yes" : "no",
        b_srtp_ok ? "yes" : "no");

    if (!a_srtp_ok || !b_srtp_ok) {
        std::fprintf(stderr, "[C] SRTP key install failed\n");
        a.stop_pump();
        b.stop_pump();
        return false;
    }

    // ---- Build and send one SRTP-protected RTP packet from A → B -----
    std::fprintf(stderr, "[C] A: building SRTP-protected RTP packet\n");

    constexpr std::uint8_t kPayloadType = 96;
    constexpr std::uint16_t kSeq = 1234;
    constexpr std::uint32_t kTs = 0x00012340;

    // Raw RTP packet (no SRTP yet).
    const std::uint8_t raw_payload[] = {'A', 'B', 'C', 'D'};
    auto rtp_pkt = ArqDtlsSrtpSide::build_rtp_packet(
        kSeq, kTs, a.ssrc, kPayloadType,
        std::span<const std::uint8_t>(raw_payload,
                                       sizeof(raw_payload)));

    // SRTP-protect: get the outgoing session (A uses its own server keys).
    auto* a_out_sess = a.srtp->get_session(a.ssrc, /*outgoing=*/true);
    if (!a_out_sess) {
        std::fprintf(stderr, "[C] A: no SRTP outgoing session\n");
        a.stop_pump();
        b.stop_pump();
        return false;
    }

    // protect_rtp takes a ByteSpan header+payload and returns the
    // SRTP-encrypted version (in-place, but the returned span is the
    // encrypted data including the auth tag).
    nimrtc::core::ByteSpan in_span(rtp_pkt.data(), rtp_pkt.size());
    auto srtp_out = a_out_sess->protect_rtp(in_span, a.ssrc, kTs);
    if (!srtp_out) {
        std::fprintf(stderr, "[C] A: SRTP protect failed\n");
        a.stop_pump();
        b.stop_pump();
        return false;
    }

    // Copy the encrypted bytes to a flat buffer for sending.
    auto& srtp_span = srtp_out.value();
    std::vector<std::uint8_t> srtp_bytes(srtp_span.data(),
                                         srtp_span.data() + srtp_span.size());
    std::fprintf(stderr,
        "[C] A: SRTP protected: %zu bytes (RTP %zu + auth tag)\n",
        srtp_bytes.size(), rtp_pkt.size());

    // Send through ArqRawUdp.
    nimrtc::plugins::Endpoint epB_ep =
        nimrtc::raw_udp::UdpSocket::make_endpoint_v4("127.0.0.1", portB);
    auto send_rc = a.arq->send(epB_ep,
        std::span<const std::uint8_t>(srtp_bytes.data(),
                                       srtp_bytes.size()));
    if (send_rc != nimrtc::plugins::kOk) {
        std::fprintf(stderr, "[C] A: ArqRawUdp send failed: %d\n",
                     static_cast<int>(send_rc));
        a.stop_pump();
        b.stop_pump();
        return false;
    }

    // ---- Wait for B to receive the SRTP packet ------------------------
    // Drain B's srtp_inbound queue until we get a packet or timeout.
    std::vector<std::uint8_t> received_srtp;
    auto recv_deadline = clk::now() + std::chrono::seconds(5);
    while (clk::now() < recv_deadline) {
        std::deque<std::vector<std::uint8_t>> drained;
        {
            std::lock_guard<std::mutex> lk(b.srtp_inbound.mtx);
            if (!b.srtp_inbound.q.empty()) {
                received_srtp = std::move(b.srtp_inbound.q.front());
                b.srtp_inbound.q.pop_front();
            }
        }
        if (!received_srtp.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    a.stop_pump();
    b.stop_pump();

    if (received_srtp.empty()) {
        std::fprintf(stderr, "[C] B: no SRTP packet received (timeout)\n");
        return false;
    }

    std::fprintf(stderr,
        "[C] B: received SRTP packet: %zu bytes\n",
        received_srtp.size());

    // ---- SRTP-unprotect on B -------------------------------------------
    // B uses its inbound session (incoming, which is the peer's client keys).
    auto* b_in_sess = b.srtp->get_session(a.ssrc, /*outgoing=*/false);
    if (!b_in_sess) {
        std::fprintf(stderr, "[C] B: no SRTP inbound session for SSRC=%08x\n",
                     a.ssrc);
        return false;
    }

    std::uint32_t out_ssrc = 0;
    std::uint32_t out_ts = 0;
    nimrtc::core::ByteSpan recv_span(received_srtp.data(),
                                      received_srtp.size());
    auto unprotect_out = b_in_sess->unprotect_rtp(recv_span,
                                                    &out_ssrc, &out_ts);
    if (!unprotect_out) {
        std::fprintf(stderr, "[C] B: SRTP unprotect failed\n");
        return false;
    }

    auto& unprotect_span = unprotect_out.value();

    std::fprintf(stderr,
        "[C] B: SRTP unprotect OK: %zu bytes  SSRC=%08x  TS=%08x\n",
        unprotect_span.size(), out_ssrc, out_ts);

    // ---- Verify RTP header and payload ---------------------------------
    if (unprotect_span.size() < 12) {
        std::fprintf(stderr, "[C] B: unprotect result too short (%zu)\n",
                     unprotect_span.size());
        return false;
    }

    const std::uint8_t* hdr = unprotect_span.data();
    if (hdr[0] != 0x80) {
        std::fprintf(stderr, "[C] B: wrong RTP version: 0x%02x\n", hdr[0]);
        return false;
    }
    std::uint16_t got_seq = static_cast<std::uint16_t>((hdr[2] << 8) | hdr[3]);
    std::uint32_t got_ts  = (static_cast<std::uint32_t>(hdr[4]) << 24) |
                             (static_cast<std::uint32_t>(hdr[5]) << 16) |
                             (static_cast<std::uint32_t>(hdr[6]) <<  8) |
                             static_cast<std::uint32_t>(hdr[7]);
    std::uint32_t got_ssrc = (static_cast<std::uint32_t>(hdr[8]) << 24) |
                               (static_cast<std::uint32_t>(hdr[9]) << 16) |
                               (static_cast<std::uint32_t>(hdr[10]) << 8) |
                               static_cast<std::uint32_t>(hdr[11]);

    if (got_seq != kSeq) {
        std::fprintf(stderr, "[C] B: seq mismatch: got=%u want=%u\n",
                     got_seq, kSeq);
        return false;
    }
    if (got_ts != kTs) {
        std::fprintf(stderr, "[C] B: timestamp mismatch: got=%08x want=%08x\n",
                     got_ts, kTs);
        return false;
    }
    if (got_ssrc != a.ssrc) {
        std::fprintf(stderr, "[C] B: SSRC mismatch: got=%08x want=%08x\n",
                     got_ssrc, a.ssrc);
        return false;
    }

    // Verify payload "ABCD".
    const std::size_t payload_len = unprotect_span.size() - 12;
    if (payload_len != 4) {
        std::fprintf(stderr, "[C] B: payload length mismatch: got=%zu want=4\n",
                     payload_len);
        return false;
    }
    const std::uint8_t* payload_bytes = unprotect_span.data() + 12;
    if (std::memcmp(payload_bytes, raw_payload, 4) != 0) {
        std::fprintf(stderr, "[C] B: payload mismatch: got '%c%c%c%c' want 'ABCD'\n",
                     payload_bytes[0], payload_bytes[1],
                     payload_bytes[2], payload_bytes[3]);
        return false;
    }

    // ---- ARQ stats -----------------------------------------------------
    auto a_arq = a.arq->stats();
    auto b_arq = b.arq->stats();
    std::fprintf(stderr,
        "[C] A ARQ: sent=%llu recv=%llu retrans=%llu\n",
        (unsigned long long)a_arq.packets_sent,
        (unsigned long long)a_arq.packets_recv,
        (unsigned long long)a_arq.packets_retransmit);
    std::fprintf(stderr,
        "[C] B ARQ: sent=%llu recv=%llu retrans=%llu\n",
        (unsigned long long)b_arq.packets_sent,
        (unsigned long long)b_arq.packets_recv,
        (unsigned long long)b_arq.packets_retransmit);

    // ---- Cleanup -------------------------------------------------------
    a.arq->close();
    b.arq->close();
    a.dtls->close();
    b.dtls->close();

    std::fprintf(stderr, "[C] PASS\n");
    return true;
}

}  // namespace

int main() {
    const bool ok = run_test();
    return ok ? 0 : 2;
}
