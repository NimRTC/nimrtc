/**
 * @file tests/test_dtls_handshake_e2e.cpp
 * @brief End-to-end DTLS handshake test using two DtlsSession objects in
 *        the same process — one as Client, one as Server.  Verifies that
 *        wolfSSL populates peerCert (after switching from
 *        WOLFSSL_VERIFY_NONE to WOLFSSL_VERIFY_PEER + dummy callback) and
 *        that the SPKI pin path produces Connected state.
 *
 * Build target: tests/test_dtls_handshake_e2e
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

#include <nimrtc/dtls/dtls.hpp>

using clk = std::chrono::steady_clock;

namespace {

struct DtlsPipe {
    nimrtc::dtls::DtlsSession* a;
    nimrtc::dtls::DtlsSession* b;
    std::string a_host{"127.0.0.1"};
    std::uint16_t a_port{0};   // set after b.open() returns b's local port
    std::string b_host{"127.0.0.1"};
    std::uint16_t b_port{0};
};

// Drive both sessions, shuttling bytes from a.outbound -> b.inbound and
// b.outbound -> a.inbound until both reach Connected or timeout.
bool drive_until_connected(DtlsPipe& p, clk::time_point deadline) {
    using namespace nimrtc::dtls;
    while (clk::now() < deadline) {
        // a -> b
        for (auto& rec : p.a->take_outbound()) {
            DtlsAddr from{p.a_host, p.a_port};
            p.b->feed_inbound(rec.bytes, from);
        }
        // b -> a
        for (auto& rec : p.b->take_outbound()) {
            DtlsAddr from{p.b_host, p.b_port};
            p.a->feed_inbound(rec.bytes, from);
        }
        p.a->tick();
        p.b->tick();
        if (p.a->is_connected() && p.b->is_connected()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

}  // namespace

int main() {
    using namespace nimrtc::dtls;

    DtlsPipe pipe;

    Config cfgC{};
    cfgC.role = DtlsRole::Client;
    cfgC.srtp_profile = SrtpProfile::Aes128CmSha1_80;
    DtlsSession client(cfgC);
    if (auto r = client.open(); !r) {
        std::fprintf(stderr, "client.open failed: %s\n", r.error().message().c_str());
        return 1;
    }

    Config cfgS{};
    cfgS.role = DtlsRole::Server;
    cfgS.srtp_profile = SrtpProfile::Aes128CmSha1_80;
    DtlsSession server(cfgS);
    if (auto r = server.open(); !r) {
        std::fprintf(stderr, "server.open failed: %s\n", r.error().message().c_str());
        return 1;
    }

    // Exchange fingerprints so both sides can pin.
    auto fp_client = client.local_fingerprint();
    auto fp_server = server.local_fingerprint();
    {
        std::vector<std::uint8_t> raw_fp(fp_server.bytes.begin(),
                                         fp_server.bytes.end());
        client.set_peer_fingerprint("sha-256", std::move(raw_fp));
    }
    {
        std::vector<std::uint8_t> raw_fp(fp_client.bytes.begin(),
                                         fp_client.bytes.end());
        server.set_peer_fingerprint("sha-256", std::move(raw_fp));
    }

    pipe.a = &client;
    pipe.b = &server;
    // We don't actually need real ports for in-process pipe (the addresses
    // are only used to fill DtlsAddr; the I/O is byte-buffer based).  But
    // pick something so the records look plausible in any logging.
    pipe.a_port = 51001;
    pipe.b_port = 51002;

    auto deadline = clk::now() + std::chrono::seconds(10);
    bool ok = drive_until_connected(pipe, deadline);

    auto ca = client.state();
    auto sa = server.state();
    std::fprintf(stderr,
        "DTLS handshake result: client=%s server=%s ok=%d\n",
        DtlsSession::state_name(ca), DtlsSession::state_name(sa), ok);

    auto ck = client.srtp_keying_material();
    auto sk = server.srtp_keying_material();
    std::fprintf(stderr,
        "SRTP keying: client=%s server=%s\n",
        ck ? "present" : "missing", sk ? "present" : "missing");

    auto cstats = client.stats();
    auto sstats = server.stats();
    std::fprintf(stderr,
        "client stats: rec_in=%llu rec_out=%llu hs_ms=%llu retrans=%llu errs=%llu\n",
        static_cast<unsigned long long>(cstats.records_in),
        static_cast<unsigned long long>(cstats.records_out),
        static_cast<unsigned long long>(cstats.handshake_ms),
        static_cast<unsigned long long>(cstats.retransmits),
        static_cast<unsigned long long>(cstats.errors));
    std::fprintf(stderr,
        "server stats: rec_in=%llu rec_out=%llu hs_ms=%llu retrans=%llu errs=%llu\n",
        static_cast<unsigned long long>(sstats.records_in),
        static_cast<unsigned long long>(sstats.records_out),
        static_cast<unsigned long long>(sstats.handshake_ms),
        static_cast<unsigned long long>(sstats.retransmits),
        static_cast<unsigned long long>(sstats.errors));

    return (ok && ck && sk) ? 0 : 2;
}
