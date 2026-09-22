/**
 * @file tests/test_dtls_gmssl_tlcp.cpp
 * @brief GMSSL TLCP DTLS 1.2 handshake round-trip test.
 *
 * OH-DOD-4 (v0.12.0 准入门槛): 验证 DtlsSessionGmSSL 在同进程内完成
 * TLCP（GM/T 0044，基于 TLS 1.2）握手，导出对称 SRTP 密钥材料，
 * 与 DtlsSessionWolfSSL 行为一致。
 *
 * 结构镜像自 tests/test_dtls_handshake_e2e.cpp：两个 DtlsSessionGmSSL
 * 实例（Client + Server）通过 feed_inbound / take_outbound / tick 驱动
 * 握手直到双方 Connected，然后验证 SRTP keying material 对称性。
 *
 * 编译条件：`NIMRTC_ENABLE_DTLS_GMSSL=ON` 时才编译。
 * OH-DOD-5（OHOS-aarch64 上跑通）使用相同的测试二进制。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

// 仅在 GMSSL 后端编译时才包含本测试。
// DtlsSessionGmSSL 已在 dtls_gmssl_session.cpp 中包装了 GMSSL 依赖。
#include <nimrtc/dtls/dtls_gmssl_session.hpp>
#include <nimrtc/dtls/dtls_types.hpp>

using clk = std::chrono::steady_clock;

namespace {

// ===========================================================================
// SM2 certificate generation helpers
// ===========================================================================

// Generates an SM2 key pair + self-signed certificate using the system
// `gmssl` CLI tool.  Returns true on success.  All three output files
// are written to the same directory as the test binary.
//
// Certificate algorithm: SM2 with SM3 (GM/T 0003 compliant).
// Password for encrypted PKCS#8 key: "P@ssw0rd" (safe for tests).
bool generate_sm2_cert(const char* basename, const char* common_name,
                       std::string& out_cert_path,
                       std::string& out_key_path,
                       std::string& out_pass) {
    // Resolve the test binary directory from argv[0]
    char exe_path[512] = {0};
#ifdef _WIN32
    ::GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path) - 1);
    // Strip filename to get directory
    char* last_sep = strrchr(exe_path, '\\');
    if (last_sep) *last_sep = '\0';
#else
    if (::readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1) < 0)
        strncpy(exe_path, ".", sizeof(exe_path) - 1);
    char* last_sep = strrchr(exe_path, '/');
    if (last_sep) *last_sep = '\0';
#endif

    char cert_file[512] = {0};
    char key_file[512]  = {0};
    snprintf(cert_file, sizeof(cert_file), "%s/%s.crt", exe_path, basename);
    snprintf(key_file,  sizeof(key_file),  "%s/%s.key",  exe_path, basename);

    const char* pass = "P@ssw0rd";

    // gmssl sm2keygen -pass <pass> -out <key>
    char cmd_keygen[768] = {0};
    snprintf(cmd_keygen, sizeof(cmd_keygen),
             "gmssl sm2keygen -pass %s -out \"%s\" 2>&1", pass, key_file);

    // gmssl certgen -key <key> -pass <pass> -sig_alg sm2sign-with-sm3
    //       -sm2_id test -CN <name> -days 365 -out <cert>
    char cmd_certgen[1024] = {0};
    snprintf(cmd_certgen, sizeof(cmd_certgen),
             "gmssl certgen -key \"%s\" -pass %s "
             "-sig_alg sm2sign-with-sm3 -sm2_id test "
             "-CN \"%s\" -days 365 -out \"%s\" 2>&1",
             key_file, pass, common_name, cert_file);

    std::printf("[setup] generating SM2 key: %s\n", cmd_keygen);
    if (std::system(cmd_keygen) != 0) {
        std::fprintf(stderr, "[setup] FAIL: sm2keygen failed\n");
        return false;
    }

    std::printf("[setup] generating SM2 cert: %s\n", cmd_certgen);
    if (std::system(cmd_certgen) != 0) {
        std::fprintf(stderr, "[setup] FAIL: certgen failed\n");
        return false;
    }

    out_cert_path = cert_file;
    out_key_path  = key_file;
    out_pass      = pass;
    return true;
}

// ===========================================================================
// DTLS pipe: two GMSSL sessions bridged via feed_inbound / take_outbound
// ===========================================================================

struct DtlsPipe {
    nimrtc::dtls::DtlsSessionGmSSL* a = nullptr;  // Client
    nimrtc::dtls::DtlsSessionGmSSL* b = nullptr;  // Server
    std::string a_host{"127.0.0.1"};
    std::uint16_t a_port{40000};
    std::string b_host{"127.0.0.1"};
    std::uint16_t b_port{40001};
};

// 驱动两个 session 直到双方 Connected 或超时。
bool drive_until_connected(DtlsPipe& p, clk::time_point deadline) {
    using namespace nimrtc::dtls;
    while (clk::now() < deadline) {
        // a → b
        for (auto& rec : p.a->take_outbound()) {
            DtlsAddr from{p.a_host, p.a_port};
            p.b->feed_inbound(rec.bytes, from);
        }
        // b → a
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
    std::printf("=== GMSSL TLCP DTLS handshake round-trip test ===\n");
    std::fflush(stdout);

    // =========================================================================
    // Setup: generate SM2 certificates for client and server
    // =========================================================================
    std::string client_cert, client_key, client_pass;
    std::string server_cert, server_key, server_pass;

    if (!generate_sm2_cert("gmssl-client", "GMSSL-Test-Client",
                           client_cert, client_key, client_pass)) {
        std::fprintf(stderr, "FAIL: could not generate client SM2 certificate\n");
        return 1;
    }
    std::printf("[setup] client cert: %s\n", client_cert.c_str());

    if (!generate_sm2_cert("gmssl-server", "GMSSL-Test-Server",
                           server_cert, server_key, server_pass)) {
        std::fprintf(stderr, "FAIL: could not generate server SM2 certificate\n");
        return 1;
    }
    std::printf("[setup] server cert: %s\n", server_cert.c_str());

    // =========================================================================
    // Create sessions with socket addresses and SM2 cert paths
    // =========================================================================
    DtlsPipe pipe;

    nimrtc::dtls::Config cfgC{};
    cfgC.role = nimrtc::dtls::DtlsRole::Client;
    cfgC.srtp_profile = nimrtc::dtls::SrtpProfile::Aes128CmSha1_80;
    cfgC.verbose = false;
    cfgC.bind_host  = "127.0.0.1";
    cfgC.bind_port  = 40000;
    cfgC.peer_host  = "127.0.0.1";
    cfgC.peer_port  = 40001;
    cfgC.cert_path  = client_cert;
    cfgC.key_path   = client_key;
    cfgC.key_pass   = client_pass;

    nimrtc::dtls::Config cfgS{};
    cfgS.role = nimrtc::dtls::DtlsRole::Server;
    cfgS.srtp_profile = nimrtc::dtls::SrtpProfile::Aes128CmSha1_80;
    cfgS.verbose = false;
    cfgS.bind_host  = "127.0.0.1";
    cfgS.bind_port  = 40001;
    cfgS.peer_host  = "127.0.0.1";
    cfgS.peer_port  = 40000;
    cfgS.cert_path  = server_cert;
    cfgS.key_path   = server_key;
    cfgS.key_pass   = server_pass;

    nimrtc::dtls::DtlsSessionGmSSL client(cfgC);
    nimrtc::dtls::DtlsSessionGmSSL server(cfgS);

    // =========================================================================
    // Open sessions
    // =========================================================================
    if (auto r = client.open(); !r) {
        std::fprintf(stderr, "FAIL: client.open failed: %s\n",
            r.error().message().c_str());
        return 1;
    }

    if (auto r = server.open(); !r) {
        std::fprintf(stderr, "FAIL: server.open failed: %s\n",
            r.error().message().c_str());
        return 1;
    }

    // =========================================================================
    // Exchange fingerprints so both sides pin each other's cert (MITM protection)
    // =========================================================================
    auto fp_client = client.local_fingerprint();
    auto fp_server = server.local_fingerprint();

    std::printf("client local fingerprint: %s\n", fp_client.hex_colon.c_str());
    std::printf("server local fingerprint: %s\n", fp_server.hex_colon.c_str());
    std::fflush(stdout);

    {
        std::vector<std::uint8_t> raw(fp_server.bytes.begin(), fp_server.bytes.end());
        client.set_peer_fingerprint("sha-256", std::move(raw));
    }
    {
        std::vector<std::uint8_t> raw(fp_client.bytes.begin(), fp_client.bytes.end());
        server.set_peer_fingerprint("sha-256", std::move(raw));
    }

    pipe.a = &client;
    pipe.b = &server;

    // =========================================================================
    // Handshake pump
    // =========================================================================
    std::printf("starting handshake pump (10s timeout)...\n");
    std::fflush(stdout);

    clk::time_point deadline = clk::now() + std::chrono::seconds(10);
    bool ok = drive_until_connected(pipe, deadline);

    std::printf("client state: %s\n",
        nimrtc::dtls::DtlsSessionGmSSL::state_name(client.state()));
    std::printf("server state: %s\n",
        nimrtc::dtls::DtlsSessionGmSSL::state_name(server.state()));
    std::fflush(stdout);

    if (!ok) {
        std::fprintf(stderr, "FAIL: handshake did not complete within 10s\n");
        return 1;
    }

    std::printf("PASS: both sides reached Connected\n");
    std::fflush(stdout);

    // =========================================================================
    // SRTP keying material verification
    // =========================================================================
    auto km_client = client.srtp_keying_material();
    auto km_server = server.srtp_keying_material();

    if (!km_client.has_value()) {
        std::fprintf(stderr, "FAIL: client SRTP keying material not available\n");
        return 1;
    }
    if (!km_server.has_value()) {
        std::fprintf(stderr, "FAIL: server SRTP keying material not available\n");
        return 1;
    }

    std::printf("PASS: SRTP keying material available on both sides\n");
    std::fflush(stdout);

    // Symmetry: client and server master keys must differ
    bool keys_differ = false;
    for (std::size_t i = 0; i < km_client->client_master_key.size(); ++i) {
        if (km_client->client_master_key[i] != km_server->client_master_key[i]) {
            keys_differ = true;
            break;
        }
    }
    if (!keys_differ) {
        std::fprintf(stderr, "FAIL: client and server master keys are identical\n");
        return 1;
    }
    std::printf("PASS: client/server master keys are distinct\n");
    std::fflush(stdout);

    // PAL Slice 4 seam method: export_srtp_key_material()
    std::uint8_t km_bytes[60] = {0};
    auto rc = client.export_srtp_key_material(
        std::span<std::uint8_t, 60>(km_bytes, 60));
    if (rc != nimrtc::plugins::kOk) {
        std::fprintf(stderr, "FAIL: export_srtp_key_material returned %u\n",
            static_cast<unsigned>(rc));
        return 1;
    }
    std::printf("PASS: export_srtp_key_material returned kOk\n");
    std::fflush(stdout);

    // =========================================================================
    // on_handshake_complete callback verification
    // =========================================================================
    std::atomic<bool> client_called{false};
    std::atomic<bool> server_called{false};
    client.on_handshake_complete([&](nimrtc::plugins::Status s) {
        if (s == nimrtc::plugins::kOk) client_called = true;
    });
    server.on_handshake_complete([&](nimrtc::plugins::Status s) {
        if (s == nimrtc::plugins::kOk) server_called = true;
    });

    if (!client_called || !server_called) {
        std::fprintf(stderr,
            "FAIL: handshake_complete callback not fired "
            "(client_called=%d, server_called=%d)\n",
            static_cast<int>(client_called),
            static_cast<int>(server_called));
        return 1;
    }
    std::printf("PASS: on_handshake_complete fired on both sides\n");
    std::fflush(stdout);

    // =========================================================================
    // Stats sanity check
    // =========================================================================
    auto s_client = client.stats();
    auto s_server = server.stats();
    std::printf("client stats: records_in=%llu records_out=%llu "
                 "handshake_ms=%llu retransmits=%llu errors=%llu\n",
        static_cast<unsigned long long>(s_client.records_in),
        static_cast<unsigned long long>(s_client.records_out),
        static_cast<unsigned long long>(s_client.handshake_ms),
        static_cast<unsigned long long>(s_client.retransmits),
        static_cast<unsigned long long>(s_client.errors));
    std::printf("server stats: records_in=%llu records_out=%llu "
                 "handshake_ms=%llu retransmits=%llu errors=%llu\n",
        static_cast<unsigned long long>(s_server.records_in),
        static_cast<unsigned long long>(s_server.records_out),
        static_cast<unsigned long long>(s_server.handshake_ms),
        static_cast<unsigned long long>(s_server.retransmits),
        static_cast<unsigned long long>(s_server.errors));
    std::fflush(stdout);

    if (s_client.errors > 0 || s_server.errors > 0) {
        std::fprintf(stderr, "FAIL: non-zero error counts\n");
        return 1;
    }
    std::printf("PASS: zero error counts on both sides\n");
    std::fflush(stdout);

    // tick() no-op after Connected
    client.tick();
    server.tick();
    std::printf("PASS: tick() no-op after Connected\n");
    std::fflush(stdout);

    std::printf("\n=== ALL CHECKS PASSED ===\n");
    return 0;
}
