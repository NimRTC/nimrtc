/**
 * @file tests/test_dtls_app_data_loopback.cpp
 * @brief DTLS ApplicationData (ct=23) loopback test.
 *
 * Two in-process DtlsSession objects (client + server) complete a real
 * wolfSSL DTLS 1.2 handshake over the byte-buffer pipe, then:
 *   1. client.send_app_data("hello") → ciphertext via take_outbound()
 *   2. server.feed_inbound(ciphertext) → plaintext via take_app_data()
 *   3. server.send_app_data("world") → client.take_app_data() == "world"
 *
 * Also verifies the pre-Connected guard: send_app_data() before the
 * handshake completes returns kErrNotReady.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <nimrtc/dtls/dtls.hpp>

using clk = std::chrono::steady_clock;

namespace {

struct DtlsPipe {
    nimrtc::dtls::DtlsSession* a;
    nimrtc::dtls::DtlsSession* b;
    std::string a_host{"127.0.0.1"};
    std::uint16_t a_port{0};
    std::string b_host{"127.0.0.1"};
    std::uint16_t b_port{0};
};

// Drive both sessions, shuttling bytes from a.outbound -> b.inbound and
// b.outbound -> a.inbound until both reach Connected or timeout.
// Mirrors test_dtls_handshake_e2e.cpp's harness.
bool drive_until_connected(DtlsPipe& p, clk::time_point deadline) {
    using namespace nimrtc::dtls;
    while (clk::now() < deadline) {
        for (auto& rec : p.a->take_outbound()) {
            DtlsAddr from{p.a_host, p.a_port};
            p.b->feed_inbound(rec.bytes, from);
        }
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

// Shuttle all pending outbound records in both directions once.
void pump_once(DtlsPipe& p) {
    using namespace nimrtc::dtls;
    for (auto& rec : p.a->take_outbound()) {
        DtlsAddr from{p.a_host, p.a_port};
        p.b->feed_inbound(rec.bytes, from);
    }
    for (auto& rec : p.b->take_outbound()) {
        DtlsAddr from{p.b_host, p.b_port};
        p.a->feed_inbound(rec.bytes, from);
    }
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
        std::fprintf(stderr, "client.open failed: %s\n",
                     r.error().message().c_str());
        return 1;
    }

    Config cfgS{};
    cfgS.role = DtlsRole::Server;
    cfgS.srtp_profile = SrtpProfile::Aes128CmSha1_80;
    DtlsSession server(cfgS);
    if (auto r = server.open(); !r) {
        std::fprintf(stderr, "server.open failed: %s\n",
                     r.error().message().c_str());
        return 1;
    }

    // Pre-Connected guard: send_app_data before the handshake must fail
    // with kErrNotReady, not crash or silently encrypt.
    {
        const std::uint8_t early[] = {0xAA};
        auto st = client.send_app_data(
            std::span<const std::uint8_t>{early, sizeof(early)});
        if (st != nimrtc::plugins::kErrNotReady) {
            std::fprintf(stderr,
                "FAIL: pre-handshake send_app_data returned 0x%x, "
                "expected kErrNotReady\n", st);
            return 1;
        }
        if (!client.take_app_data().empty()) {
            std::fprintf(stderr,
                "FAIL: pre-handshake take_app_data() non-empty\n");
            return 1;
        }
        std::fprintf(stderr, "pre-handshake guard ok\n");
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
    pipe.a_port = 51001;
    pipe.b_port = 51002;

    auto deadline = clk::now() + std::chrono::seconds(10);
    if (!drive_until_connected(pipe, deadline)) {
        std::fprintf(stderr, "FAIL: handshake did not complete\n");
        return 2;
    }
    std::fprintf(stderr, "handshake ok: client=%s server=%s\n",
        DtlsSession::state_name(client.state()),
        DtlsSession::state_name(server.state()));

    // Drain any stragglers so the pipe is quiet before AppData.
    pump_once(pipe);
    (void)client.take_app_data();
    (void)server.take_app_data();

    // Direction 1: client -> server ("hello").
    {
        const char kMsg[] = "hello";
        auto st = client.send_app_data(std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(kMsg), sizeof(kMsg) - 1});
        if (st != nimrtc::plugins::kOk) {
            std::fprintf(stderr,
                "FAIL: client send_app_data returned 0x%x\n", st);
            return 1;
        }
        pump_once(pipe);
        auto got = server.take_app_data();
        if (got.size() != 1 ||
            std::string(got[0].begin(), got[0].end()) != "hello") {
            std::fprintf(stderr,
                "FAIL: server take_app_data got %zu records\n", got.size());
            return 1;
        }
        std::fprintf(stderr, "client->server ok\n");
    }

    // Direction 2: server -> client ("world").
    {
        const char kMsg[] = "world";
        auto st = server.send_app_data(std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(kMsg), sizeof(kMsg) - 1});
        if (st != nimrtc::plugins::kOk) {
            std::fprintf(stderr,
                "FAIL: server send_app_data returned 0x%x\n", st);
            return 1;
        }
        pump_once(pipe);
        auto got = client.take_app_data();
        if (got.size() != 1 ||
            std::string(got[0].begin(), got[0].end()) != "world") {
            std::fprintf(stderr,
                "FAIL: client take_app_data got %zu records\n", got.size());
            return 1;
        }
        std::fprintf(stderr, "server->client ok\n");
    }

    // Queue is drained: second take must be empty.
    if (!client.take_app_data().empty() ||
        !server.take_app_data().empty()) {
        std::fprintf(stderr, "FAIL: take_app_data not drained\n");
        return 1;
    }

    std::fprintf(stderr, "PASS: DTLS AppData loopback\n");
    return 0;
}
