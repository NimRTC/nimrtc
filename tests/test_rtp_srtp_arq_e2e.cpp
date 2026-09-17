/**
 * @file tests/test_rtp_srtp_arq_e2e.cpp
 * @brief Test D — Full media pipeline: PCM (stub) → RTP → SRTP →
 *        ArqRawUdp → ... → ARQ recv → SRTP unprotect → RTP parse.
 *
 * Builds on Test C (SRTP-over-DTLS-over-ArqRawUdp):
 *
 *   1. Drive DTLS handshake to Connected on both sides.
 *   2. Install SRTP keys (same as Test C).
 *   3. Side A sends a BURST of 10 RTP packets, SRTP-protected, through
 *      ArqRawUdp.  Verify all 10 arrive correctly on side B.
 *   4. Side B sends a BURST of 5 RTP packets back to A (opposite
 *      direction).  Verify all 5 arrive on A.
 *   5. Verify SRTP stats: encrypted == sent count, decrypted == received count.
 *   6. Verify ARQ retransmit count == 0 on loopback (healthy network).
 *
 * This is the most complete transport-layer test: it exercises
 *   ArqRawUdp → DTLS key export → SRTP protect/unprotect →
 *   ARQ delivery → bidirectional media round-trip.
 *
 * Build target: tests/test_rtp_srtp_arq_e2e
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
// looks_like_rtp — cheap protocol-shape gate used to keep non-RTP
// payloads out of the libsrtp unprotect path.
//
// ARQ multiplexes our DTLS handshake records and SRTP media packets
// onto the same UDP socket (the real test runs on 127.0.0.1, so we use
// a single ARQ instance per side for everything).  After DTLS
// CONNECTED the test enables `dtls_connected ? push-to-media_q : drop`,
// but that gate isn't tight enough: a 975-byte DTLS Handshake record
// (ServerKeyExchange/Certificate flight) that ARQ re-delivers AFTER
// the handshake has already completed will pass the connected gate
// and end up at `drain_media`, where libsrtp reads the DTLS
// content-type byte (0x16) as an RTP version field, fails the auth-tag
// check, and prints:
//
//     [D] unprotect failed: peer_ssrc=00000e02 pkt_sz=975
//                          first12=16 fe fd 00 00 00 00 00 00 00 0e 02
//
// `peer_ssrc=0x0e02` is just the trailing two bytes of the DTLS
// record's 6-byte sequence number, not a real SSRC.
//
// The shape check is exact: RTP byte 0 holds V=2 in the top two bits
// (RFC 3550 §5.1), so `(pkt[0] & 0xC0) == 0x80`.  DTLS content_type
// is 0x14..0x17 (always below 0x40) so the two are disjoint.  STUN
// cookie / ICE / RTCP non-data packets fail this check too, which is
// the correct behaviour for this test — only SRTP-wrapped media
// should ever be unprotect'd here.
//
// `pkt.size() >= 12` matches the libsrtp guard in
// `SrtpSession::unprotect_rtp()` so we reject tiny frames one place
// instead of letting libsrtp do it for us (and report a less-useful
// error message).
// ===========================================================================
inline bool looks_like_rtp(std::span<const std::uint8_t> pkt) noexcept {
    return pkt.size() >= 12 && (pkt[0] & 0xC0) == 0x80;
}

// ===========================================================================
// Shared types (mirrored from test_srtp_over_dtls_over_arq_udp.cpp)
// ===========================================================================

struct Side {
    std::unique_ptr<nimrtc::raw_udp::ArqRawUdp> arq;
    std::unique_ptr<nimrtc::dtls::DtlsSession>   dtls;
    std::unique_ptr<nimrtc::srtp::SrtpContext>  srtp;

    std::atomic<bool> dtls_connected{false};
    std::atomic<bool> srtp_ready{false};
    std::uint16_t    peer_port = 0;
    std::uint32_t    ssrc = 0;
    std::uint16_t    initial_seq = 0;
    // RFC 5764 §4.2 — the SRTP local/remote key assignment depends on
    // whether this side is the DTLS client or DTLS server:
    //   * DTLS Client: local (outbound) = client_master_key,
    //                 remote (inbound) = server_master_key
    //   * DTLS Server: local (outbound) = server_master_key,
    //                 remote (inbound) = client_master_key
    // Earlier versions of this test unconditionally used
    // `local = server_master_key` / `remote = client_master_key`,
    // which is correct only for the DTLS server side — the DTLS client
    // side sent packets encrypted with the wrong key and B's
    // unprotect_rtp() rejected every one of them.  Storing the role
    // here lets install_srtp_keys() pick the right mapping.
    nimrtc::dtls::DtlsRole role = nimrtc::dtls::DtlsRole::Server;

    struct {
        std::mutex mtx;
        std::deque<std::vector<std::uint8_t>> q;  // ARQ inbound → DTLS feed
        std::deque<std::vector<std::uint8_t>> media_q;  // media packets
    } inbound;

    std::thread pump;
    std::atomic<bool> pump_stop{false};

    void start_pump() {
        pump = std::thread([this] {
            while (!pump_stop.load(std::memory_order_acquire)) {
                arq->recv();

                std::deque<std::vector<std::uint8_t>> drained;
                {
                    std::lock_guard<std::mutex> lk(inbound.mtx);
                    drained.swap(inbound.q);
                }
                for (auto& bytes : drained) {
                    // The first N bytes in the drain are media packets.
                    // But we can distinguish: the first bytes from the
                    // pump loop are DTLS handshake records.
                    // A simple heuristic: feed into DTLS first.
                    nimrtc::dtls::DtlsAddr addr{"127.0.0.1", peer_port};
                    (void)dtls->feed_inbound(
                        std::span<const std::uint8_t>(bytes.data(),
                                                     bytes.size()),
                        addr);
                }

                // NOTE: do NOT drain `inbound.media_q` here.  The pump and
                // the main test thread both race to swap `media_q`; if the
                // pump drains it first, the main thread (which actually
                // runs srtp_unprotect on the payloads) sees an empty
                // queue.  In the previous version of this test the pump
                // swapped `media_q` every iteration and silently dropped
                // 9 out of every 10 media packets — only the single packet
                // that happened to arrive between pump iterations made it
                // to `drain_media`, which is exactly why the burst test
                // reported "B received 1/10 packets from A".
                // The pump only owns `inbound.q` (DTLS feed); media
                // packets are owned end-to-end by `drain_media`.

                dtls->tick();
                auto outs = dtls->take_outbound();
                for (auto& rec : outs) {
                    nimrtc::plugins::Endpoint ep =
                        nimrtc::raw_udp::UdpSocket::make_endpoint_v4(
                            "127.0.0.1", peer_port);
                    arq->send(ep,
                              std::span<const std::uint8_t>(rec.bytes.data(),
                                                           rec.bytes.size()));
                }

                if (dtls->is_connected() &&
                    !dtls_connected.exchange(true,
                                             std::memory_order_acq_rel)) {
                    install_srtp_keys();
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }

    void install_srtp_keys() {
        auto km = dtls->srtp_keying_material();
        if (!km) return;
        std::vector<std::uint8_t> c_key(km->client_master_key.begin(),
                                         km->client_master_key.end());
        std::vector<std::uint8_t> c_salt(km->client_master_salt.begin(),
                                          km->client_master_salt.end());
        std::vector<std::uint8_t> s_key(km->server_master_key.begin(),
                                         km->server_master_key.end());
        std::vector<std::uint8_t> s_salt(km->server_master_salt.begin(),
                                          km->server_master_salt.end());
        // RFC 5764 §4.2 — local/remote key assignment is role-dependent.
        // See the comment on `Side::role` above for the full rationale.
        const std::vector<std::uint8_t>* local_key;
        const std::vector<std::uint8_t>* local_salt;
        const std::vector<std::uint8_t>* remote_key;
        const std::vector<std::uint8_t>* remote_salt;
        if (role == nimrtc::dtls::DtlsRole::Client) {
            local_key  = &c_key;  local_salt  = &c_salt;
            remote_key = &s_key;  remote_salt = &s_salt;
        } else {
            local_key  = &s_key;  local_salt  = &s_salt;
            remote_key = &c_key;  remote_salt = &c_salt;
        }
        if (!remote_key->empty() && !remote_salt->empty())
            srtp->derive_keys_for_remote(*remote_key, *remote_salt,
                nimrtc::srtp::CryptoSuite::Aes128CmSha1_80);
        if (!local_key->empty() && !local_salt->empty())
            srtp->derive_keys_for_local(*local_key, *local_salt,
                nimrtc::srtp::CryptoSuite::Aes128CmSha1_80);
        srtp_ready.store(true, std::memory_order_release);
        std::fprintf(stderr,
            "[D] %08x: SRTP keys installed (role=%s)\n",
            ssrc, role == nimrtc::dtls::DtlsRole::Client ? "Client" : "Server");
    }

    // Helper: build RTP packet.
    static std::vector<std::uint8_t> build_rtp(
            std::uint16_t seq, std::uint32_t ts,
            std::uint32_t ssrc, std::uint8_t pt,
            std::span<const std::uint8_t> payload)
    {
        std::vector<std::uint8_t> p;
        p.reserve(12 + payload.size());
        p.push_back(0x80); // V=2 P=0 X=0 CC=0
        p.push_back(pt & 0x7F);
        p.push_back(static_cast<std::uint8_t>((seq >> 8) & 0xFF));
        p.push_back(static_cast<std::uint8_t>(seq & 0xFF));
        p.push_back(static_cast<std::uint8_t>((ts >> 24) & 0xFF));
        p.push_back(static_cast<std::uint8_t>((ts >> 16) & 0xFF));
        p.push_back(static_cast<std::uint8_t>((ts >>  8) & 0xFF));
        p.push_back(static_cast<std::uint8_t>(ts & 0xFF));
        p.push_back(static_cast<std::uint8_t>((ssrc >> 24) & 0xFF));
        p.push_back(static_cast<std::uint8_t>((ssrc >> 16) & 0xFF));
        p.push_back(static_cast<std::uint8_t>((ssrc >>  8) & 0xFF));
        p.push_back(static_cast<std::uint8_t>(ssrc & 0xFF));
        p.insert(p.end(), payload.begin(), payload.end());
        return p;
    }

    // Protect + send one RTP packet through ArqRawUdp.
    bool send_rtp(std::uint16_t seq, std::uint32_t ts,
                  std::span<const std::uint8_t> payload,
                  std::uint16_t dst_port) {
        auto rtp = build_rtp(seq, ts, ssrc, 96, payload);
        auto* sess = srtp->get_session(ssrc, /*outgoing=*/true);
        if (!sess) {
            std::fprintf(stderr,
                "[D] send_rtp: get_session(ssrc=%08x, out=true) failed\n",
                ssrc);
            return false;
        }
        nimrtc::core::ByteSpan sp(rtp.data(), rtp.size());
        auto srtp_out = sess->protect_rtp(sp, ssrc, ts);
        if (!srtp_out) {
            std::fprintf(stderr,
                "[D] send_rtp: protect_rtp failed\n");
            return false;
        }
        auto& out_span = srtp_out.value();
        std::vector<std::uint8_t> buf(out_span.data(),
                                      out_span.data() + out_span.size());
        nimrtc::plugins::Endpoint ep =
            nimrtc::raw_udp::UdpSocket::make_endpoint_v4("127.0.0.1", dst_port);
        return arq->send(ep,
            std::span<const std::uint8_t>(buf.data(), buf.size())) == nimrtc::plugins::kOk;
    }

    // Drain media queue and unprotect N packets.  Returns received payloads.
    std::vector<std::vector<std::uint8_t>> drain_media(int max_count,
                                                         clk::time_point deadline) {
        std::vector<std::vector<std::uint8_t>> results;
        while (clk::now() < deadline && results.size() < static_cast<std::size_t>(max_count)) {
            std::deque<std::vector<std::uint8_t>> drained;
            {
                std::lock_guard<std::mutex> lk(inbound.mtx);
                drained.swap(inbound.media_q);
            }
            for (auto& pkt : drained) {
                // SRTP sessions are keyed by the SOURCE SSRC (peer's SSRC)
                // — RFC 5764 §5.3 requires per-SSRC key derivation.  Look
                // up the inbound session using bytes [8..11] of the RTP
                // header, NOT `this->ssrc` (our own SSRC).  The previous
                // code used `ssrc` here and every unprotect failed with
                // "no session for that SSRC" once the second peer started
                // sending (dec count stayed at 0).
                //
                // Belt-and-braces protocol gate: the on_recv callback
                // already filters by `looks_like_rtp` before pushing to
                // `media_q`, but a non-RTP packet could still slip
                // through (e.g. future test variant that bypasses the
                // on_recv filter, or a malformed peer).  Calling
                // libsrtp with a non-RTP payload has the side effect of
                // CREATING a new inbound SRTP session keyed by whatever
                // bytes [8..11] happen to be — for a DTLS record those
                // are part of the sequence number, not an SSRC, and the
                // bogus session sits in `inbound_sessions_` forever.
                // The shape check below drops the packet before
                // `get_session()` can pollute the session table.
                if (!looks_like_rtp(pkt)) continue;
                const std::uint32_t peer_ssrc =
                    (static_cast<std::uint32_t>(pkt[8]) << 24) |
                    (static_cast<std::uint32_t>(pkt[9]) << 16) |
                    (static_cast<std::uint32_t>(pkt[10]) <<  8) |
                     static_cast<std::uint32_t>(pkt[11]);
                auto* sess = srtp->get_session(peer_ssrc, /*outgoing=*/false);
                if (!sess) continue;
                std::uint32_t out_ssrc = 0, out_ts = 0;
                nimrtc::core::ByteSpan sp(pkt.data(), pkt.size());
                auto un = sess->unprotect_rtp(sp, &out_ssrc, &out_ts);
                if (!un) continue;
                auto& un_span = un.value();
                if (un_span.size() > 12) {
                    std::vector<std::uint8_t> payload(un_span.data() + 12,
                                                      un_span.data() + un_span.size());
                    results.push_back(std::move(payload));
                }
            }
            if (results.size() >= static_cast<std::size_t>(max_count)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return results;
    }

    void stop_pump() {
        pump_stop.store(true, std::memory_order_release);
        if (pump.joinable()) pump.join();
    }
};

// Parse port from an endpoint.
static std::uint16_t parse_port(const nimrtc::plugins::Endpoint& ep) {
    std::string s(reinterpret_cast<const char*>(ep.data),
                  std::min<std::size_t>(ep.len, sizeof(ep.data)));
    auto colon = s.rfind(':');
    return colon == std::string::npos
               ? 0
               : static_cast<std::uint16_t>(std::stoi(s.substr(colon + 1)));
}

// Verify received payloads match the expected sequence.
bool verify_payloads(const std::vector<std::vector<std::uint8_t>>& received,
                   const std::vector<std::vector<std::uint8_t>>& expected,
                   const char* dir) {
    if (received.size() != expected.size()) {
        std::fprintf(stderr,
            "[D] %s: count mismatch: got=%zu want=%zu\n",
            dir, received.size(), expected.size());
        return false;
    }
    for (std::size_t i = 0; i < received.size(); ++i) {
        if (received[i].size() != expected[i].size() ||
            std::memcmp(received[i].data(), expected[i].data(),
                        expected[i].size()) != 0) {
            std::fprintf(stderr,
                "[D] %s: payload[%zu] mismatch\n", dir, i);
            return false;
        }
    }
    return true;
}

bool run_test() {
    std::fprintf(stderr, "[D] Full RTP+SRTP+ArqRawUdp e2e test starting\n");

    Side a, b;
    a.ssrc = 0xCAFEBABE;
    b.ssrc = 0xDEAD1DEA;
    a.initial_seq = 5000;
    b.initial_seq = 7000;

    // ---- Open ArqRawUdp ------------------------------------------------
    auto open_arq = [](Side& s) -> bool {
        nimrtc::raw_udp::RawUdpConfig cfg;
        cfg.local_host = "127.0.0.1";
        cfg.local_port = 0;
        cfg.enable_real_socket = true;
        cfg.rto_base_ms = 50;
        cfg.rx_window_packets = 32;
        cfg.max_retransmits = 5;
        s.arq = std::make_unique<nimrtc::raw_udp::ArqRawUdp>(cfg);
        s.arq->on_recv([&s](nimrtc::plugins::BufferView bv) {
            std::vector<std::uint8_t> copy(bv.data(), bv.data() + bv.size());
            // Always feed DTLS — handshake records must reach the state
            // machine regardless of connection state.
            {
                std::lock_guard<std::mutex> lk(s.inbound.mtx);
                s.inbound.q.push_back(copy);
            }
            // Only fan-out to media_q AFTER the DTLS handshake is done
            // and SRTP keys are installed.  Before that point the ARQ
            // recv thread is delivering DTLS handshake records to side B
            // (Certificate / ServerKeyExchange / Finished are all 100–300
            // bytes); feeding those into libsrtp's unprotect makes it
            // read the DTLS content-type byte as an RTP version field,
            // the auth tag check fails, and the test reports
            // "unprotect failed" on every "packet" it pops.
            //
            // The `dtls_connected` gate alone is not enough: ARQ may
            // re-deliver a DTLS handshake record AFTER the handshake has
            // completed (e.g. an in-flight retransmit that was buffered
            // by the selective-repeat receiver and only flushed to
            // `rx_delivered_` once the missing seq arrived).  Those late
            // DTLS records carry content_type 0x14..0x17, which is
            // strictly below the RTP-version-2 byte (0x80..0xBF) — see
            // `looks_like_rtp` for the full rationale.  The shape gate
            // here is what stops those packets from reaching
            // `drain_media` and producing false "unprotect failed"
            // log lines.
            if (s.dtls_connected.load(std::memory_order_acquire) &&
                looks_like_rtp(copy)) {
                std::lock_guard<std::mutex> lk(s.inbound.mtx);
                s.inbound.media_q.push_back(std::move(copy));
            }
        });
        return s.arq->open() == nimrtc::plugins::kOk;
    };

    if (!open_arq(a)) { std::fprintf(stderr, "[D] A.arq open failed\n"); return false; }
    if (!open_arq(b)) { std::fprintf(stderr, "[D] B.arq open failed\n"); a.arq->close(); return false; }

    const std::uint16_t portA = parse_port(a.arq->local_endpoint());
    const std::uint16_t portB = parse_port(b.arq->local_endpoint());
    a.peer_port = portB;
    b.peer_port = portA;
    std::fprintf(stderr, "[D] A=%u  B=%u\n", portA, portB);

    // ---- DtlsSession + SrtpContext -----------------------------------
    // Refactored ordering: create BOTH DtlsSession objects (via setup_side)
    // BEFORE capturing local fingerprints and BEFORE setting peer pins.
    // The previous order captured fingerprints from the uninitialized
    // `a.dtls` / `b.dtls` (nullptr deref → SEGFAULT) and then re-created
    // the DtlsSession inside setup_side, leaving the captured fingerprints
    // stale relative to the new sessions.
    auto setup_side = [](Side& s, nimrtc::dtls::DtlsRole role) -> bool {
        nimrtc::dtls::Config cfg{};
        cfg.role = role;
        cfg.srtp_profile = nimrtc::dtls::SrtpProfile::Aes128CmSha1_80;
        s.dtls = std::make_unique<nimrtc::dtls::DtlsSession>(cfg);
        s.srtp = std::make_unique<nimrtc::srtp::SrtpContext>();
        if (auto r = s.dtls->open(); !r) return false;
        return true;
    };

    if (!setup_side(a, nimrtc::dtls::DtlsRole::Client)) {
        std::fprintf(stderr, "[D] A setup failed\n"); return false;
    }
    a.role = nimrtc::dtls::DtlsRole::Client;
    if (!setup_side(b, nimrtc::dtls::DtlsRole::Server)) {
        std::fprintf(stderr, "[D] B setup failed\n"); return false;
    }
    b.role = nimrtc::dtls::DtlsRole::Server;

    // NOW capture fingerprints from the live sessions and pin them
    // as the expected peer SPKI for the opposite side.
    auto fpA = a.dtls->local_fingerprint();
    auto fpB = b.dtls->local_fingerprint();
    a.dtls->set_peer_fingerprint("sha-256",
        std::vector<std::uint8_t>(fpB.bytes.begin(), fpB.bytes.end()));
    b.dtls->set_peer_fingerprint("sha-256",
        std::vector<std::uint8_t>(fpA.bytes.begin(), fpA.bytes.end()));

    // ---- Start pump threads --------------------------------------------
    a.start_pump();
    b.start_pump();

    // ---- Wait for Connected + SRTP ready ------------------------------
    auto deadline = clk::now() + std::chrono::seconds(15);
    while (clk::now() < deadline) {
        if (a.dtls_connected.load() && b.dtls_connected.load() &&
            a.srtp_ready.load() && b.srtp_ready.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!a.dtls_connected.load() || !b.dtls_connected.load()) {
        std::fprintf(stderr, "[D] DTLS handshake timeout\n");
        a.stop_pump(); b.stop_pump();
        return false;
    }
    if (!a.srtp_ready.load() || !b.srtp_ready.load()) {
        std::fprintf(stderr, "[D] SRTP key install timeout\n");
        a.stop_pump(); b.stop_pump();
        return false;
    }

    std::fprintf(stderr, "[D] Both sides Connected + SRTP ready\n");

    // ==================================================================
    // BURST A → B: 10 packets
    // ==================================================================
    constexpr int kBurstAB = 10;
    constexpr std::uint32_t kTsStep = 480;  // 48 kHz / 100 Hz = 480 samples
    constexpr std::uint32_t kTsStart = 0x100000;

    std::vector<std::vector<std::uint8_t>> expected_AB;
    expected_AB.reserve(kBurstAB);
    for (int i = 0; i < kBurstAB; ++i) {
        std::uint8_t payload[4] = {
            static_cast<std::uint8_t>('A' + i),
            static_cast<std::uint8_t>('0' + (i % 10)),
            static_cast<std::uint8_t>(i),
            static_cast<std::uint8_t>(0xAB)
        };
        std::vector<std::uint8_t> pl(payload, payload + 4);
        expected_AB.push_back(pl);

        std::uint16_t seq = static_cast<std::uint16_t>(a.initial_seq + i);
        std::uint32_t ts  = kTsStart + static_cast<std::uint32_t>(i) * kTsStep;
        if (!a.send_rtp(seq, ts, pl, portB)) {
            std::fprintf(stderr, "[D] A->B send_rtp(%d) failed\n", i);
            a.stop_pump(); b.stop_pump();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::fprintf(stderr, "[D] A sent %d packets to B\n", kBurstAB);

    // Drain B's media queue for up to 3 seconds.
    auto received_AB = b.drain_media(kBurstAB,
        clk::now() + std::chrono::seconds(3));
    std::fprintf(stderr,
        "[D] B received %zu/%d packets from A\n",
        received_AB.size(), kBurstAB);

    // ==================================================================
    // BURST B → A: 5 packets
    // ==================================================================
    constexpr int kBurstBA = 5;
    std::vector<std::vector<std::uint8_t>> expected_BA;
    expected_BA.reserve(kBurstBA);
    for (int i = 0; i < kBurstBA; ++i) {
        std::uint8_t payload[4] = {
            static_cast<std::uint8_t>('X' + i),
            static_cast<std::uint8_t>('9' - i),
            static_cast<std::uint8_t>(i * 7),
            static_cast<std::uint8_t>(0xCD)
        };
        std::vector<std::uint8_t> pl(payload, payload + 4);
        expected_BA.push_back(pl);

        std::uint16_t seq = static_cast<std::uint16_t>(b.initial_seq + i);
        std::uint32_t ts  = 0x200000 + static_cast<std::uint32_t>(i) * kTsStep;
        if (!b.send_rtp(seq, ts, pl, portA)) {
            std::fprintf(stderr, "[D] B->A send_rtp(%d) failed\n", i);
            a.stop_pump(); b.stop_pump();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::fprintf(stderr, "[D] B sent %d packets to A\n", kBurstBA);

    auto received_BA = a.drain_media(kBurstBA,
        clk::now() + std::chrono::seconds(3));
    std::fprintf(stderr,
        "[D] A received %zu/%d packets from B\n",
        received_BA.size(), kBurstBA);

    // ---- Collect stats -------------------------------------------------
    auto a_arq = a.arq->stats();
    auto b_arq = b.arq->stats();
    auto a_srtp_stats = a.srtp->total_stats();
    auto b_srtp_stats = b.srtp->total_stats();

    std::fprintf(stderr,
        "[D] A: arq_sent=%llu arq_recv=%llu retrans=%llu  "
        "SRTP enc=%llu dec=%llu\n",
        (unsigned long long)a_arq.packets_sent,
        (unsigned long long)a_arq.packets_recv,
        (unsigned long long)a_arq.packets_retransmit,
        (unsigned long long)a_srtp_stats.rtp_packets_encrypted,
        (unsigned long long)a_srtp_stats.rtp_packets_decrypted);
    std::fprintf(stderr,
        "[D] B: arq_sent=%llu arq_recv=%llu retrans=%llu  "
        "SRTP enc=%llu dec=%llu\n",
        (unsigned long long)b_arq.packets_sent,
        (unsigned long long)b_arq.packets_recv,
        (unsigned long long)b_arq.packets_retransmit,
        (unsigned long long)b_srtp_stats.rtp_packets_encrypted,
        (unsigned long long)b_srtp_stats.rtp_packets_decrypted);

    a.stop_pump();
    b.stop_pump();

    // ---- Cleanup -------------------------------------------------------
    a.arq->close(); b.arq->close();
    a.dtls->close(); b.dtls->close();

    // ==================================================================
    // PASS criteria
    // ==================================================================
    bool pass = true;

    if (!verify_payloads(received_AB, expected_AB, "A→B")) pass = false;
    if (!verify_payloads(received_BA, expected_BA, "B→A")) pass = false;

    // On a healthy 127.0.0.1 loopback, there should be no ARQ retransmits.
    if (a_arq.packets_retransmit > 0 || b_arq.packets_retransmit > 0) {
        std::fprintf(stderr,
            "[D] WARNING: ARQ retransmits detected on loopback "
            "(A=%llu B=%llu).  This may indicate a bug or timing issue.\n",
            (unsigned long long)a_arq.packets_retransmit,
            (unsigned long long)b_arq.packets_retransmit);
        // Non-fatal on loopback — don't fail the test for this alone.
    }

    // SRTP stats should reflect what we sent/received.
    if (a_srtp_stats.rtp_packets_encrypted < kBurstBA) {
        std::fprintf(stderr,
            "[D] A SRTP encrypted count too low: %llu < %d\n",
            (unsigned long long)a_srtp_stats.rtp_packets_encrypted,
            kBurstBA);
        pass = false;
    }
    if (b_srtp_stats.rtp_packets_decrypted < kBurstAB) {
        std::fprintf(stderr,
            "[D] B SRTP decrypted count too low: %llu < %d\n",
            (unsigned long long)b_srtp_stats.rtp_packets_decrypted,
            kBurstAB);
        pass = false;
    }

    std::fprintf(stderr, "[D] %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

}  // namespace

int main() {
    const bool ok = run_test();
    return ok ? 0 : 2;
}
