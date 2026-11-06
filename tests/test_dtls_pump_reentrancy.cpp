/**
 * @file tests/test_dtls_pump_reentrancy.cpp
 * @brief v1.1 Dtls3/D4 — DTLS pump reentrancy + export/retransmit observability.
 *
 * D3: consecutive pump()/tick()/take_outbound() calls and a
 *     on_handshake_complete callback that re-enters pump() must not
 *     deadlock or crash (Impl::pumping_ guard).
 * D4: export_srtp_key_material() returns kErrNotReady pre-handshake,
 *     kOk with non-zero 60-byte material post-handshake; stats().retransmits
 *     is observable and non-decreasing across pump cycles.
 *
 * Standalone main() style (same as test_dtls_retransmit_no_duplicate):
 * registered via add_dtls_handshake_test() so wolfSSL certs are copied.
 */

#include <array>
#include <chrono>
#include <cstdio>
#include <span>
#include <thread>
#include <vector>

#include <nimrtc/dtls/dtls.hpp>
#include <nimrtc/plugins/base.hpp>

using clk = std::chrono::steady_clock;

namespace {

void shuttle(nimrtc::dtls::DtlsSession& out, nimrtc::dtls::DtlsSession& in) {
    for (auto& rec : out.take_outbound()) {
        nimrtc::dtls::DtlsAddr from{"127.0.0.1", 51000};
        in.feed_inbound(rec.bytes, from);
    }
}

bool drive_until_connected(nimrtc::dtls::DtlsSession& a,
                           nimrtc::dtls::DtlsSession& b,
                           std::vector<nimrtc::dtls::DtlsRecord>& pending_a,
                           std::vector<nimrtc::dtls::DtlsRecord>& pending_b,
                           clk::time_point deadline) {
    // Records drained inside a completion callback (see D3b below) must
    // still reach the peer — deliver them before every shuttle step.
    auto flush_pending = [&]() {
        for (auto& rec : pending_a) {
            nimrtc::dtls::DtlsAddr from{"127.0.0.1", 51001};
            b.feed_inbound(rec.bytes, from);
        }
        pending_a.clear();
        for (auto& rec : pending_b) {
            nimrtc::dtls::DtlsAddr from{"127.0.0.1", 51002};
            a.feed_inbound(rec.bytes, from);
        }
        pending_b.clear();
    };
    while (clk::now() < deadline) {
        flush_pending();
        shuttle(a, b);
        flush_pending();
        shuttle(b, a);
        flush_pending();
        a.tick();
        b.tick();
        flush_pending();
        if (a.is_connected() && b.is_connected()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    flush_pending();
    return a.is_connected() && b.is_connected();
}

}  // namespace

int main() {
    using namespace nimrtc::dtls;

    Config cfgS{};
    cfgS.role = DtlsRole::Server;
    DtlsSession server(cfgS);
    if (auto r = server.open(); !r) {
        std::fprintf(stderr, "server.open failed: %s\n", r.error().message().c_str());
        return 1;
    }
    Config cfgC{};
    cfgC.role = DtlsRole::Client;
    DtlsSession client(cfgC);
    if (auto r = client.open(); !r) {
        std::fprintf(stderr, "client.open failed: %s\n", r.error().message().c_str());
        return 1;
    }

    // Pin fingerprints both ways (fail-open would also work, but pinning
    // keeps the handshake deterministic).
    {
        auto fp = server.local_fingerprint();
        std::vector<std::uint8_t> raw(fp.bytes.begin(), fp.bytes.end());
        client.set_peer_fingerprint("sha-256", std::move(raw));
    }
    {
        auto fp = client.local_fingerprint();
        std::vector<std::uint8_t> raw(fp.bytes.begin(), fp.bytes.end());
        server.set_peer_fingerprint("sha-256", std::move(raw));
    }

    // ---- D4a: pre-handshake export must be kErrNotReady ------------------
    {
        std::array<std::uint8_t, 60> out{};
        auto rc = client.export_srtp_key_material(
            std::span<std::uint8_t, 60>(out.data(), out.size()));
        if (rc != nimrtc::plugins::kErrNotReady) {
            std::fprintf(stderr, "FAIL: pre-handshake export rc=%u want kErrNotReady\n",
                         static_cast<unsigned>(rc));
            return 2;
        }
    }

    // ---- D3a: consecutive pump/tick/take_outbound must not crash --------
    // Use throwaway sessions so the discarded initial flight below does
    // not starve the real handshake pair (the client would otherwise
    // have to wait out the 1 s RFC 6347 retransmit timer to resend the
    // dropped ClientHello).
    {
        DtlsSession tmpS(cfgS), tmpC(cfgC);
        if (auto r = tmpS.open(); !r) {
            std::fprintf(stderr, "tmp server.open failed\n");
            return 1;
        }
        if (auto r = tmpC.open(); !r) {
            std::fprintf(stderr, "tmp client.open failed\n");
            return 1;
        }
        for (int i = 0; i < 10; ++i) {
            tmpC.pump();
            tmpC.tick();
            (void)tmpC.take_outbound();
            tmpS.pump();
            tmpS.tick();
            (void)tmpS.take_outbound();
        }
        // Repeated drain without tick must settle to empty (re-pump
        // gate in take_outbound() terminates: pumping_ guard + empty
        // outbound eventually).
        for (int i = 0; i < 5; ++i) {
            (void)tmpC.take_outbound();
            (void)tmpS.take_outbound();
        }
    }
    std::fprintf(stderr, "[d3] consecutive pump/tick/drain x10 ok\n");

    // ---- D3b: reentrant pump inside on_handshake_complete callback ------
    // The callback fires on Connected/Failed; invoking pump() inside it
    // exercises the Impl::pumping_ guard (must be a safe no-op, not a
    // deadlock). Records drained here are stashed into pending_c/pending_s
    // and delivered by drive_until_connected() below — discarding them
    // would drop the server's final flight and stall the client forever.
    std::vector<nimrtc::dtls::DtlsRecord> pending_c, pending_s;
    client.on_handshake_complete([&](nimrtc::plugins::Status st) {
        (void)st;
        client.pump();
        for (auto& r : client.take_outbound()) pending_c.push_back(std::move(r));
    });
    server.on_handshake_complete([&](nimrtc::plugins::Status st) {
        (void)st;
        server.pump();
        for (auto& r : server.take_outbound()) pending_s.push_back(std::move(r));
    });

    const auto r0 = client.stats().retransmits;
    const auto p0 = client.stats().pump_calls;

    // ---- D4b: full handshake, then export + retransmit observability ----
    auto deadline = clk::now() + std::chrono::seconds(10);
    const bool ok = drive_until_connected(client, server, pending_c, pending_s, deadline);
    std::fprintf(stderr, "[hs] connected=%d client=%s server=%s\n", ok ? 1 : 0,
                 DtlsSession::state_name(client.state()),
                 DtlsSession::state_name(server.state()));
    if (!ok) {
        std::fprintf(stderr, "FAIL: handshake did not complete in 10s\n");
        return 3;
    }

    // Post-handshake export must succeed with non-zero material, and both
    // sides must agree (same DTLS-SRTP keying material).
    std::array<std::uint8_t, 60> km_c{}, km_s{};
    auto rc_c = client.export_srtp_key_material(
        std::span<std::uint8_t, 60>(km_c.data(), km_c.size()));
    auto rc_s = server.export_srtp_key_material(
        std::span<std::uint8_t, 60>(km_s.data(), km_s.size()));
    if (rc_c != nimrtc::plugins::kOk || rc_s != nimrtc::plugins::kOk) {
        std::fprintf(stderr, "FAIL: post-handshake export rc_c=%u rc_s=%u\n",
                     static_cast<unsigned>(rc_c), static_cast<unsigned>(rc_s));
        return 4;
    }
    bool any_nonzero = false;
    for (auto b : km_c) { if (b != 0) { any_nonzero = true; break; } }
    if (!any_nonzero) {
        std::fprintf(stderr, "FAIL: exported client keying material is all zeros\n");
        return 5;
    }
    if (km_c != km_s) {
        std::fprintf(stderr, "FAIL: client/server keying material mismatch\n");
        return 6;
    }
    std::fprintf(stderr, "[d4] export ok, first bytes %02x:%02x:%02x:%02x\n",
                 km_c[0], km_c[1], km_c[2], km_c[3]);

    // Retransmit counter must be observable and monotonic (never decreases
    // across the handshake drive above).
    const auto r1 = client.stats().retransmits;
    const auto p1 = client.stats().pump_calls;
    std::fprintf(stderr, "[d4] retransmits %llu->%llu pumps %llu->%llu\n",
                 static_cast<unsigned long long>(r0),
                 static_cast<unsigned long long>(r1),
                 static_cast<unsigned long long>(p0),
                 static_cast<unsigned long long>(p1));
    if (r1 < r0) {
        std::fprintf(stderr, "FAIL: retransmits decreased\n");
        return 7;
    }
    if (p1 <= p0) {
        std::fprintf(stderr, "FAIL: pump_calls did not advance during handshake\n");
        return 8;
    }

    // Post-connected pump storm must stay Connected (no state corruption).
    for (int i = 0; i < 20; ++i) {
        client.pump();
        server.pump();
        (void)client.take_outbound();
        (void)server.take_outbound();
    }
    if (!client.is_connected() || !server.is_connected()) {
        std::fprintf(stderr, "FAIL: post-connected pump storm broke Connected state\n");
        return 9;
    }

    std::fprintf(stderr, "PASS test_dtls_pump_reentrancy\n");
    return 0;
}
