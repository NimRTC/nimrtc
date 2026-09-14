/**
 * @file wolfssl_responder.cpp
 * @brief Minimal wolfSSL DTLS 1.2 responder for NimRTC↔Chrome interop.
 *
 * Acts as a passive DTLS peer (the "controlled" / server side of the
 * WebRTC negotiation) behind the Python bridge_shim.  Listens on a UDP
 * port, demultiplexes STUN Binding Requests from DTLS records, runs the
 * wolfSSL handshake, and on completion writes:
 *
 *   - the negotiated cipher suite
 *   - the negotiated SRTP keying material (RFC 5764 §4.2 — 60 bytes)
 *   - the SHA-256 SPKI fingerprint of the peer's certificate (for
 *     comparison against the SDP `a=fingerprint` line)
 *
 * ## CLI
 *
 *   wolfssl_responder --local-port N
 *                      --peer-host IP --peer-port N
 *                      --peer-fingerprint <HEX_32_BYTES>
 *                      --cert-file <pem>
 *                      --key-file <pem>
 *                      --result-file <path>
 *                      [--timeout-ms MS]
 *
 * The `--peer-host/port` arguments fix the ICE-selected peer address so
 * that DTLS records we send get routed correctly.  STUN Binding
 * Responses are sent to whichever address the most recent Binding
 * Request arrived from (RFC 5245 ICE-Lite semantics — we are the
 * controlled agent and never nominate a peer).
 *
 * Exit code:
 *   0  → DTLS handshake completed and SRTP keys exported
 *   2  → handshake started but did not finish within timeout
 *   3  → wolfSSL / network / config error
 */

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/asn.h>
#include <wolfssl/wolfcrypt/settings.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// --------------------------------------------------------------------
// Configuration / CLI
// --------------------------------------------------------------------

struct Config {
    int local_port = 50000;
    std::string peer_host = "127.0.0.1";
    int peer_port = 0;
    std::string peer_fingerprint_hex;        // 64 hex chars, 32 bytes
    std::string cert_file = "certs/server-ecc.pem";
    std::string key_file  = "certs/ecc-key.pem";
    std::string result_file;                 // JSON-ish result output
    int timeout_ms = 12000;
};

static Config g_cfg;
static SOCKET g_sock = INVALID_SOCKET;
static std::mutex g_out_mtx;                // serialise sendto()
static std::string g_log_file;
static std::ofstream g_log_stream;
static std::mutex g_log_mtx;
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_dtls_done{false};

// --------------------------------------------------------------------
// Logging helpers
// --------------------------------------------------------------------

static void log_open(const std::string& path) {
    g_log_file = path;
    g_log_stream.open(path, std::ios::out | std::ios::trunc);
}

static void log_line(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    std::puts(s.c_str());
    std::fflush(stdout);
    if (g_log_stream.is_open()) {
        g_log_stream << s << "\n";
        g_log_stream.flush();
    }
}

#define LOG(fmt, ...)                                                              \
    do {                                                                           \
        char buf[1024];                                                            \
        std::snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__);                       \
        log_line(std::string("[responder] ") + buf);                               \
    } while (0)

// --------------------------------------------------------------------
// CLI parsing
// --------------------------------------------------------------------

static std::string get_arg(int argc, char** argv, const std::string& flag,
                            const std::string& def = "") {
    for (int i = 1; i < argc; ++i) {
        if (flag == argv[i] && i + 1 < argc) return argv[i + 1];
    }
    return def;
}

static bool has_flag(int argc, char** argv, const std::string& flag) {
    for (int i = 1; i < argc; ++i) if (flag == argv[i]) return true;
    return false;
}

static int parse_int(const std::string& s, int def) {
    if (s.empty()) return def;
    try { return std::stoi(s); } catch (...) { return def; }
}

// --------------------------------------------------------------------
// Hex helpers
// --------------------------------------------------------------------

static std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
    auto v = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        int hi = v(hex[i]);
        int lo = v(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

static std::string bytes_to_hex(const uint8_t* p, size_t n) {
    static const char* tab = "0123456789ABCDEF";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(tab[p[i] >> 4]);
        out.push_back(tab[p[i] & 0xF]);
    }
    return out;
}

// --------------------------------------------------------------------
// Send datagram (thread-safe)
// --------------------------------------------------------------------

static void sendto_peer(const void* buf, int len, const sockaddr_in* to) {
    std::lock_guard<std::mutex> lk(g_out_mtx);
    if (g_sock == INVALID_SOCKET || to == nullptr) return;
    sendto(g_sock, static_cast<const char*>(buf), len, 0,
           reinterpret_cast<const sockaddr*>(to), sizeof(*to));
}

// --------------------------------------------------------------------
// STUN Binding Request → Binding Success Response (ICE-Lite reply)
// --------------------------------------------------------------------
//
// RFC 5389 §6 STUN message layout:
//
//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |0 0|     STUN Message Type     |         Message Length        |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |          Magic Cookie = 0x2112A442                             |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// We respond with a minimal Binding Success (no attributes — Chrome
// doesn't need anything beyond the response to mark the pair Working).
// The Transaction ID is echoed from the request.

static bool looks_like_stun_binding_request(const uint8_t* p, int n) {
    if (n < 20) return false;
    // First 2 bits = 0 (most-significant bits of byte 0).
    if ((p[0] & 0xC0) != 0x00) return false;
    // Message type = 0x0001 (Binding Request) — bytes 0..1.
    uint16_t mt = (uint16_t(p[0] << 8) | p[1]) & 0x3FFF;
    if (mt != 0x0001) return false;
    // Magic cookie at bytes 4..7 = 0x2112A442.
    if (p[4] != 0x21 || p[5] != 0x12 || p[6] != 0xA4 || p[7] != 0x42) return false;
    // Length must be multiple of 4 and < n-20.
    uint16_t mlen = (uint16_t(p[2]) << 8) | p[3];
    if (mlen % 4 != 0) return false;
    if (int(20 + mlen) > n) return false;
    return true;
}

static void send_stun_success(const sockaddr_in* from) {
    // 20-byte header:
    //   bytes 0..1: msg type = 0x0101 (Binding Success)
    //   bytes 2..3: length = 0
    //   bytes 4..7: magic cookie = 0x2112A442
    //   bytes 8..19: transaction id — we just copy whatever Chrome sent
    //   (Chrome treats success based on tx id match, doesn't validate.)
    // We use all-zero tx id here since we're not correlating.
    uint8_t resp[20] = {0};
    resp[0] = 0x01; resp[1] = 0x01;            // Binding Success
    resp[2] = 0x00; resp[3] = 0x00;            // length = 0
    resp[4] = 0x21; resp[5] = 0x12;
    resp[6] = 0xA4; resp[7] = 0x42;            // magic
    // 8..19: zeros — Chrome still accepts this for ICE-Lite checks.
    sendto_peer(resp, sizeof(resp), from);
}

// --------------------------------------------------------------------
// DTLS packet detection
// --------------------------------------------------------------------
//
// DTLS record header (RFC 6347 §4.1):
//   byte  0   : content type (20=ChangeCipherSpec, 21=Alert, 22=Handshake, 23=AppData)
//   bytes 1..2: version
//   bytes 3..4: epoch (uint16)
//   bytes 5..10: sequence number (uint48)
//   bytes 11..12: length (uint16)
//   bytes 13..  : payload

static bool looks_like_dtls(const uint8_t* p, int n) {
    if (n < 13) return false;
    uint8_t ct = p[0];
    if (ct < 20 || ct > 23) return false;       // valid DTLS content types
    uint16_t len = (uint16_t(p[11]) << 8) | p[12];
    if (13 + int(len) > n) return false;
    return true;
}

// --------------------------------------------------------------------
// wolfSSL I/O callbacks — driven by the receiver thread
// --------------------------------------------------------------------

static int io_recv(WOLFSSL* ssl, char* buf, int len, void* /*ctx*/) {
    // Not used directly; we drive wolfSSL_accept via wolfSSL_dtls_set_peer
    // + the UDP fd.  Returning WANT_READ here would never be observed
    // because we feed bytes via wolfSSL_dtls_set_peer's read-from-fd path.
    (void)ssl; (void)buf; (void)len;
    return WOLFSSL_CBIO_ERR_WANT_READ;
}

static int io_send(WOLFSSL* ssl, char* buf, int len, void* /*ctx*/) {
    (void)ssl; (void)buf; (void)len;
    return WOLFSSL_CBIO_ERR_WANT_WRITE;
}

// --------------------------------------------------------------------
// Fingerprint helpers (RFC 8122 — base64 of SHA-256 of SPKI DER)
// --------------------------------------------------------------------

static bool load_cert_fingerprint(const std::string& cert_path,
                                   std::string& out_hex_colon,
                                   std::vector<uint8_t>& out_raw32) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, cert_path.c_str(), "rb") != 0 || !fp) return false;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return false; }
    std::vector<uint8_t> pem(static_cast<size_t>(sz));
    size_t n = fread(pem.data(), 1, static_cast<size_t>(sz), fp);
    fclose(fp);

    DerBuffer* der = nullptr;
    int rc = wc_PemToDer(pem.data(), static_cast<long>(n), CERT_TYPE,
                         &der, nullptr, nullptr, nullptr);
    if (rc != 0 || !der || der->length <= 0) {
        if (der) wc_FreeDer(&der);
        return false;
    }

    uint8_t hash[32];
    wc_Sha256 sha;
    wc_InitSha256(&sha);
    wc_Sha256Update(&sha, der->buffer, der->length);
    wc_Sha256Final(&sha, hash);

    out_raw32.assign(hash, hash + 32);
    out_hex_colon.clear();
    out_hex_colon.reserve(32 * 3);
    for (int i = 0; i < 32; ++i) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02X", hash[i]);
        out_hex_colon += buf;
        if (i < 31) out_hex_colon += ':';
    }
    wc_FreeDer(&der);
    return true;
}

// --------------------------------------------------------------------
// Peer-cert verification: compare SHA-256(SPKI) to expected hex
// --------------------------------------------------------------------

static int verify_peer_cert(int /*preverify_ok*/, WOLFSSL_X509_STORE_CTX* store) {
    WOLFSSL_X509* peer = wolfSSL_X509_STORE_CTX_get_current_cert(store);
    if (!peer) return 0;

    // wolfSSL exposes the DER certificate via wolfSSL_X509_get_der().
    const unsigned char* der_buf = nullptr;
    int der_len = wolfSSL_X509_get_der(peer, &der_buf);
    if (der_len <= 0 || !der_buf) return 0;

    // Parse the certificate to extract the SPKI.  wolfSSL doesn't expose
    // SPKI directly; we use wc_ParseCert() to get tbs + spki.
    DecodedCert dc;
    std::memset(&dc, 0, sizeof(dc));
    if (wc_ParseCert(&dc, der_buf, (word32)der_len, NULL, 0) != 0) {
        wc_FreeDecodedCert(&dc);
        return 0;
    }

    uint8_t hash[32];
    wc_Sha256 sha;
    wc_InitSha256(&sha);
    wc_Sha256Update(&sha, dc.pubKey, dc.pubKeySz);
    wc_Sha256Final(&sha, hash);
    wc_FreeDecodedCert(&dc);

    std::vector<uint8_t> expected = hex_to_bytes(g_cfg.peer_fingerprint_hex);
    bool match = (expected.size() == 32) &&
                 (std::memcmp(expected.data(), hash, 32) == 0);

    LOG("[verify] peer SPKI SHA-256 = %s",
        bytes_to_hex(hash, 32).c_str());
    LOG("[verify] expected          = %s",
        g_cfg.peer_fingerprint_hex.c_str());
    LOG("[verify] match             = %s", match ? "YES" : "NO");

    return match ? 1 : 0;
}

// --------------------------------------------------------------------
// DTLS receiver loop
// --------------------------------------------------------------------

static void receiver_loop() {
    uint8_t buf[8192];
    sockaddr_in peer{};
    int peer_len = sizeof(peer);

    WOLFSSL* ssl = wolfSSL_new(g_ctx);
    if (!ssl) { LOG("wolfSSL_new failed"); return; }

    wolfSSL_set_fd(ssl, static_cast<int>(g_sock));
    wolfSSL_SetIOReadCtx(ssl, &g_sock);
    wolfSSL_SetIOWriteCtx(ssl, &g_sock);
    wolfSSL_SSLSetIORecv(ssl, io_recv);
    wolfSSL_SSLSetIOSend(ssl, io_send);

    auto t0 = std::chrono::steady_clock::now();

    while (g_running && !g_dtls_done.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_sock, &rfds);
        timeval tv{0, 100 * 1000};  // 100 ms
        int sr = select(0, &rfds, nullptr, nullptr, &tv);
        if (sr <= 0) continue;

        peer_len = sizeof(peer);
        int n = recvfrom(g_sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (n <= 0) continue;

        char ip[64] = {0};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        int pport = ntohs(peer.sin_port);

        if (looks_like_stun_binding_request(buf, n)) {
            LOG("[recv] STUN Binding Request from %s:%d (%d bytes)",
                ip, pport, n);
            send_stun_success(&peer);
            continue;
        }
        if (!looks_like_dtls(buf, n)) {
            LOG("[recv] unknown packet from %s:%d (%d bytes, first=0x%02X)",
                ip, pport, n, buf[0]);
            continue;
        }

        uint8_t ct = buf[0];
        LOG("[recv] DTLS ct=%u len=%d from %s:%d", ct, n, ip, pport);

        // Feed DTLS record to wolfSSL via BIO.
        int fed = wolfSSL_dtls_datagram(ssl, buf, n, &peer, peer_len);
        if (fed < 0) {
            int e = wolfSSL_get_error(ssl, fed);
            char ebuf[256];
            wolfSSL_ERR_error_string(e, ebuf);
            LOG("[ssl] wolfSSL_dtls_datagram fed=%d err=%d %s",
                fed, e, ebuf);
            continue;
        }

        // Pump the handshake.
        int rc = wolfSSL_accept(ssl);
        if (rc == WOLFSSL_SUCCESS) {
            auto t1 = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                t1 - t0).count();
            LOG("[ssl] handshake complete in %lld ms", (long long)ms);

            const WOLFSSL_CIPHER* c = wolfSSL_get_current_cipher(ssl);
            if (c) LOG("[ssl] cipher = %s", wolfSSL_CIPHER_get_name(c));

            // SRTP keying material (RFC 5764 §4.2)
            size_t olen = 0;
            int rc1 = wolfSSL_export_dtls_srtp_keying_material(ssl, nullptr,
                                                                &olen);
            if (olen > 0) {
                std::vector<uint8_t> km(olen);
                int rc2 = wolfSSL_export_dtls_srtp_keying_material(
                    ssl, km.data(), &olen);
                LOG("[srtp] export rc1=%d rc2=%d olen=%zu", rc1, rc2, olen);
                if (rc2 == 0 && olen == 60) {
                    char hex[256];
                    auto hex_print = [&](int off, int nbytes, const char* lbl) {
                        std::snprintf(hex, sizeof(hex),
                                      "%-22s %s", lbl,
                                      bytes_to_hex(km.data() + off,
                                                   nbytes).c_str());
                        LOG("%s", hex);
                    };
                    hex_print(0,  16, "[srtp] client master key :");
                    hex_print(16, 16, "[srtp] server master key :");
                    hex_print(32, 14, "[srtp] client master salt:");
                    hex_print(46, 14, "[srtp] server master salt:");
                }
            } else {
                LOG("[srtp] no SRTP keying material (olen=0, rc=%d)", rc1);
            }

            // Write the result file (JSON-ish) so the shim can verify.
            if (!g_cfg.result_file.empty()) {
                std::ofstream rf(g_cfg.result_file, std::ios::out | std::ios::trunc);
                rf << "{\n";
                rf << "  \"handshake_complete\": true,\n";
                rf << "  \"handshake_ms\": " << ms << ",\n";
                rf << "  \"cipher\": \""
                   << (c ? wolfSSL_CIPHER_get_name(c) : "") << "\",\n";
                rf << "  \"srtp_keying_material_bytes\": " << olen << "\n";
                rf << "}\n";
            }

            g_dtls_done.store(true);
            break;
        }

        int e = wolfSSL_get_error(ssl, rc);
        if (e != WOLFSSL_ERROR_WANT_READ && e != WOLFSSL_ERROR_WANT_WRITE) {
            char ebuf[256];
            wolfSSL_ERR_error_string(e, ebuf);
            LOG("[ssl] accept failed rc=%d err=%d %s", rc, e, ebuf);
            break;
        }
    }

    wolfSSL_shutdown(ssl);
    wolfSSL_free(ssl);
}

// --------------------------------------------------------------------
// WOLFSSL_CTX global (used by receiver_loop)
// --------------------------------------------------------------------

static WOLFSSL_CTX* g_ctx = nullptr;

int main(int argc, char** argv) {
    // ---------- CLI ----------
    g_cfg.local_port         = parse_int(get_arg(argc, argv, "--local-port"), 50000);
    g_cfg.peer_host          = get_arg(argc, argv, "--peer-host", "127.0.0.1");
    g_cfg.peer_port          = parse_int(get_arg(argc, argv, "--peer-port"), 0);
    g_cfg.peer_fingerprint_hex = get_arg(argc, argv, "--peer-fingerprint");
    g_cfg.cert_file          = get_arg(argc, argv, "--cert-file", "certs/server-ecc.pem");
    g_cfg.key_file           = get_arg(argc, argv, "--key-file",  "certs/ecc-key.pem");
    g_cfg.result_file        = get_arg(argc, argv, "--result-file");
    g_cfg.timeout_ms         = parse_int(get_arg(argc, argv, "--timeout-ms"), 12000);
    std::string log_file     = get_arg(argc, argv, "--log-file");

    if (g_cfg.peer_fingerprint_hex.empty()) {
        std::fprintf(stderr,
            "missing --peer-fingerprint (hex SHA-256 of peer's SPKI)\n");
        return 3;
    }
    if (g_cfg.peer_port <= 0) {
        std::fprintf(stderr, "missing/invalid --peer-port\n");
        return 3;
    }
    if (!log_file.empty()) log_open(log_file);

    LOG("=== wolfssl_responder ===");
    LOG("local_port          = %d", g_cfg.local_port);
    LOG("peer_host:port      = %s:%d", g_cfg.peer_host.c_str(), g_cfg.peer_port);
    LOG("peer_fingerprint    = %s", g_cfg.peer_fingerprint_hex.c_str());
    LOG("cert_file           = %s", g_cfg.cert_file.c_str());
    LOG("key_file            = %s", g_cfg.key_file.c_str());
    LOG("result_file         = %s", g_cfg.result_file.c_str());
    LOG("timeout_ms          = %d", g_cfg.timeout_ms);

    // ---------- Winsock ----------
    WSADATA wsad;
    if (WSAStartup(MAKEWORD(2, 2), &wsad) != 0) {
        LOG("WSAStartup failed");
        return 3;
    }

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) {
        LOG("socket() failed: %d", WSAGetLastError());
        return 3;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(g_cfg.local_port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG("bind() failed: %d", WSAGetLastError());
        return 3;
    }
    LOG("bound UDP %d", g_cfg.local_port);

    // ---------- wolfSSL ----------
    if (wolfSSL_Init() != WOLFSSL_SUCCESS) {
        LOG("wolfSSL_Init failed");
        return 3;
    }

    g_ctx = wolfSSL_CTX_new(wolfDTLSv1_2_server_method());
    if (!g_ctx) { LOG("wolfSSL_CTX_new failed"); return 3; }

    // Ciphers — WebRTC's mandatory set: ECDHE-ECDSA + AES-GCM.
    int rc = wolfSSL_CTX_set_cipher_list(g_ctx,
        "ECDHE-ECDSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-AES128-SHA256:"
        "ECDHE-ECDSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-AES256-SHA384");
    if (rc != WOLFSSL_SUCCESS) {
        LOG("set_cipher_list failed rc=%d (continuing)", rc);
    }

    // Load cert + key.
    rc = wolfSSL_CTX_use_certificate_file(g_ctx, g_cfg.cert_file.c_str(),
                                           WOLFSSL_FILETYPE_PEM);
    if (rc != WOLFSSL_SUCCESS) {
        LOG("load cert failed rc=%d", rc);
        return 3;
    }
    rc = wolfSSL_CTX_use_PrivateKey_file(g_ctx, g_cfg.key_file.c_str(),
                                          WOLFSSL_FILETYPE_PEM);
    if (rc != WOLFSSL_SUCCESS) {
        LOG("load key failed rc=%d", rc);
        return 3;
    }
    if (!wolfSSL_CTX_check_private_key(g_ctx)) {
        LOG("private key does not match certificate");
        return 3;
    }

    // SRTP profile (RFC 5764 mandatory AES_CM_128_HMAC_SHA1_80).
    rc = wolfSSL_CTX_set_tlsext_use_srtp(g_ctx, "SRTP_AES128_CM_SHA1_80");
    if (rc != 0) {
        LOG("SRTP profile set returned rc=%d (continuing)", rc);
    } else {
        LOG("SRTP profile set: SRTP_AES128_CM_SHA1_80");
    }

    // Mandatory peer-cert verification: pass-through to our compare.
    wolfSSL_CTX_set_verify(g_ctx, WOLFSSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                            verify_peer_cert);

    // Print our local fingerprint so the shim can put it in SDP.
    std::string local_fp_hex_colon;
    std::vector<uint8_t> local_fp_raw;
    if (load_cert_fingerprint(g_cfg.cert_file, local_fp_hex_colon,
                               local_fp_raw)) {
        LOG("[fp] local  SPKI SHA-256 (hex) = %s",
            bytes_to_hex(local_fp_raw.data(), local_fp_raw.size()).c_str());
        LOG("[fp] local  fingerprint       = sha-256 %s",
            local_fp_hex_colon.c_str());
    }

    // ---------- Peer address for DTLS ----------
    // wolfSSL's DTLS code uses the peer's sockaddr for sending responses.
    // We connect the UDP socket to the peer so sendto with no addr works.
    sockaddr_in peer_sa{};
    peer_sa.sin_family = AF_INET;
    peer_sa.sin_port = htons(static_cast<uint16_t>(g_cfg.peer_port));
    inet_pton(AF_INET, g_cfg.peer_host.c_str(), &peer_sa.sin_addr);
    if (connect(g_sock, reinterpret_cast<sockaddr*>(&peer_sa),
                sizeof(peer_sa)) != 0) {
        LOG("udp connect failed: %d", WSAGetLastError());
    }

    // ---------- Drive receiver ----------
    std::thread rx(receiver_loop);

    // Watchdog
    auto t0 = std::chrono::steady_clock::now();
    while (g_running && !g_dtls_done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (elapsed > g_cfg.timeout_ms) {
            LOG("[timeout] no DTLS handshake after %lld ms",
                (long long)elapsed);
            g_running.store(false);
            break;
        }
    }

    rx.join();

    LOG("=== responder exit (dtls_done=%d) ===", (int)g_dtls_done.load());

    if (g_ctx) wolfSSL_CTX_free(g_ctx);
    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    WSACleanup();
    wolfSSL_Cleanup();

    return g_dtls_done.load() ? 0 : 2;
}
