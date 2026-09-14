// minimal_webrtc_peer.cpp — Tiny Windows console tool that acts as a minimal
// STUN-driven ICE peer.  Listens on --listen 127.0.0.1:PORT, responds with
// Binding Success to any incoming STUN Binding Request, and periodically
// sends Binding Requests to --remote HOST:PORT with the configured
// USERNAME.
//
// Built standalone:  cl /EHsc minimal_webrtc_peer.cpp ws2_32.lib
//
// This avoids Python's WSAIoctl quirks for SIO_UDP_CONNRESET; C code calls
// it cleanly via WSAIoctl and disables the ICMP-unreachable side effect so
// recv() keeps working after NimRTC's first send.
//
// Usage:
//   minimal_webrtc_peer --listen 127.0.0.1:62000 \
//                       --remote 127.0.0.1:56369 \
//                       --remote-ufrag XFDQ \
//                       --local-ufrag  ChromeUfrag \
//                       --duration 30

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>     // SIO_UDP_CONNRESET
#include <windows.h>
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <random>
#include <chrono>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

static constexpr uint32_t STUN_MAGIC = 0x2112A442;
static constexpr uint16_t STUN_BINDING_REQUEST  = 0x0001;
static constexpr uint16_t STUN_BINDING_SUCCESS  = 0x0101;

static constexpr uint16_t STUN_ATTR_USERNAME        = 0x0006;
static constexpr uint16_t STUN_ATTR_PRIORITY        = 0x0024;
static constexpr uint16_t STUN_ATTR_USE_CANDIDATE   = 0x0025;
static constexpr uint16_t STUN_ATTR_ICE_CONTROLLED  = 0x8029;
static constexpr uint16_t STUN_ATTR_XOR_MAPPED_ADDR = 0x0020;

struct Args {
    std::string listen_host = "127.0.0.1";
    uint16_t    listen_port = 62000;
    std::string remote_host = "127.0.0.1";
    uint16_t    remote_port = 56369;
    std::string remote_ufrag;
    std::string local_ufrag = "ChromeUfrag";
    int         duration_sec = 30;
    int         send_interval_ms = 100;
};

static void die(const char* msg) {
    fprintf(stderr, "fatal: %s (WSA err=%d)\n", msg, WSAGetLastError());
    exit(2);
}

static bool parse_args(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto need = [&](int& i) -> const char* {
            if (i + 1 >= argc) return nullptr;
            return argv[++i];
        };
        if (k == "--listen") {
            const char* v = need(i); if (!v) return false;
            // Parse host:port
            std::string s = v; auto colon = s.find(':');
            if (colon == std::string::npos) return false;
            a.listen_host = s.substr(0, colon);
            a.listen_port = (uint16_t)atoi(s.substr(colon+1).c_str());
        } else if (k == "--remote") {
            const char* v = need(i); if (!v) return false;
            std::string s = v; auto colon = s.find(':');
            if (colon == std::string::npos) return false;
            a.remote_host = s.substr(0, colon);
            a.remote_port = (uint16_t)atoi(s.substr(colon+1).c_str());
        } else if (k == "--remote-ufrag") {
            const char* v = need(i); if (!v) return false;
            a.remote_ufrag = v;
        } else if (k == "--local-ufrag") {
            const char* v = need(i); if (!v) return false;
            a.local_ufrag = v;
        } else if (k == "--duration") {
            const char* v = need(i); if (!v) return false;
            a.duration_sec = atoi(v);
        } else if (k == "--send-interval") {
            const char* v = need(i); if (!v) return false;
            a.send_interval_ms = atoi(v);
        }
    }
    return true;
}

static void write_be16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xFF; }
static void write_be32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static void write_be64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = v >> (56 - 8*i);
}

static std::vector<uint8_t> build_request(const std::string& username,
                                          uint64_t tie_breaker) {
    std::vector<uint8_t> out;
    out.resize(20);
    // Random transaction id (12 bytes) — fill after header
    std::mt19937_64 rng(std::random_device{}());
    uint64_t a = rng(); uint32_t b = (uint32_t)rng();
    memcpy(out.data() + 8,  &a, 8);
    memcpy(out.data() + 16, &b, 4);
    auto add_attr = [&](uint16_t t, const uint8_t* data, size_t len) {
        size_t pad = (4 - (len & 3)) & 3;
        size_t off = out.size();
        out.resize(off + 4 + len + pad);
        write_be16(out.data()+off,   t);
        write_be16(out.data()+off+2, (uint16_t)len);
        memcpy(out.data()+off+4, data, len);
        if (pad) memset(out.data()+off+4+len, 0, pad);
    };
    auto add_u16 = [&](uint16_t t, uint16_t v) {
        uint8_t tmp[2]; write_be16(tmp, v);
        add_attr(t, tmp, 2);
    };
    auto add_u32 = [&](uint16_t t, uint32_t v) {
        uint8_t tmp[4]; write_be32(tmp, v);
        add_attr(t, tmp, 4);
    };
    auto add_u64 = [&](uint16_t t, uint64_t v) {
        uint8_t tmp[8]; write_be64(tmp, v);
        add_attr(t, tmp, 8);
    };
    add_attr(STUN_ATTR_USERNAME,
             (const uint8_t*)username.data(), username.size());
    add_u32(STUN_ATTR_PRIORITY, 1862270975);
    add_u64(STUN_ATTR_ICE_CONTROLLED, tie_breaker);
    // Total length in header
    uint16_t total_len = (uint16_t)(out.size() - 20);
    write_be16(out.data(),   STUN_BINDING_REQUEST);
    write_be16(out.data()+2, total_len);
    write_be32(out.data()+4, STUN_MAGIC);
    return out;
}

static std::vector<uint8_t> build_success(const uint8_t* txn_id,
                                          const sockaddr_in* mapped) {
    std::vector<uint8_t> out(20);
    memcpy(out.data()+8, txn_id, 12);
    // XOR-MAPPED-ADDRESS (IPv4)
    size_t off = 20;
    out.resize(off + 12);
    // 8-bit reserved, 8-bit family (1=IPv4), 16-bit xor port, 32-bit xor ip
    out[off]   = 0;
    out[off+1] = 1;
    uint16_t port = ntohs(mapped->sin_port) ^ (STUN_MAGIC >> 16);
    write_be16(out.data()+off+2, port);
    uint32_t ip  = ntohl(mapped->sin_addr.s_addr);
    for (int i = 0; i < 4; ++i) {
        out[off+4+i] = ((ip >> (24 - 8*i)) & 0xFF) ^
                       ((STUN_MAGIC >> (24 - 8*i)) & 0xFF);
    }
    write_be16(out.data()+off-4+0, STUN_ATTR_XOR_MAPPED_ADDR);
    write_be16(out.data()+off-4+2, 8);  // length
    (void)0;
    uint16_t total_len = (uint16_t)(out.size() - 20);
    write_be16(out.data(),   STUN_BINDING_SUCCESS);
    write_be16(out.data()+2, total_len);
    write_be32(out.data()+4, STUN_MAGIC);
    return out;
}

static bool parse_request(const uint8_t* data, size_t len, std::string& username) {
    if (len < 20) return false;
    // STUN fields are big-endian (network byte order).  Use ntohl/ntohs
    // to compare against STUN_MAGIC, otherwise a packet from a real
    // STUN agent (NimRTC, libjuice, Chrome, etc.) gets rejected as
    // "non-STUN" because we read the magic as little-endian host order.
    uint32_t magic_be; memcpy(&magic_be, data+4, 4);
    if (ntohl(magic_be) != STUN_MAGIC) return false;
    uint16_t type_be; memcpy(&type_be, data, 2);
    if (ntohs(type_be) != STUN_BINDING_REQUEST) return false;
    uint16_t mlen_be; memcpy(&mlen_be, data+2, 2);
    uint16_t mlen = ntohs(mlen_be);
    if (20 + mlen > len) return false;
    size_t pos = 20;
    while (pos + 4 <= 20 + mlen) {
        uint16_t at_be, al_be;
        memcpy(&at_be, data+pos, 2);
        memcpy(&al_be, data+pos+2, 2);
        uint16_t at = ntohs(at_be);
        uint16_t al = ntohs(al_be);
        pos += 4;
        if (at == STUN_ATTR_USERNAME && al > 0) {
            username.assign((const char*)data+pos, al);
        }
        pos += al;
        pos += (4 - (al & 3)) & 3;
    }
    return true;
}

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) {
        fprintf(stderr, "usage: --listen H:P --remote H:P --remote-ufrag U "
                        "--local-ufrag U [--duration N] [--send-interval MS]\n");
        return 64;
    }
    WSADATA wsad; if (WSAStartup(MAKEWORD(2,2), &wsad) != 0) die("WSAStartup");

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) die("socket");

    // Disable SIO_UDP_CONNRESET so recvfrom() doesn't fail with 10054
    // after the destination sends ICMP-unreachable.
    {
        BOOL b = FALSE;
        DWORD br = 0;
        // SIO_UDP_CONNRESET = 0x9800000C (defined in mstcpip.h on Win10+)
        DWORD sio = 0x9800000C;
        int rc = WSAIoctl(sock, sio,
                          &b, sizeof(b),
                          nullptr, 0, &br,
                          nullptr, nullptr);
        if (rc == SOCKET_ERROR) {
            fprintf(stderr, "SIO_UDP_CONNRESET failed err=%d (continuing)\n",
                    WSAGetLastError());
        } else {
            fprintf(stderr, "SIO_UDP_CONNRESET disabled\n");
        }
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));

    sockaddr_in local{}; local.sin_family = AF_INET;
    local.sin_port = htons(args.listen_port);
    inet_pton(AF_INET, args.listen_host.c_str(), &local.sin_addr);
    if (bind(sock, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR)
        die("bind");

    sockaddr_in remote{}; remote.sin_family = AF_INET;
    remote.sin_port = htons(args.remote_port);
    inet_pton(AF_INET, args.remote_host.c_str(), &remote.sin_addr);

    fprintf(stderr, "[peer] listen=%s:%d remote=%s:%d user=%s:%s dur=%ds\n",
            args.listen_host.c_str(), args.listen_port,
            args.remote_host.c_str(), args.remote_port,
            args.remote_ufrag.c_str(), args.local_ufrag.c_str(),
            args.duration_sec);

    if (args.remote_ufrag.empty()) {
        fprintf(stderr, "[peer] waiting for remote-ufrag via SIGHUP... (not supported, exiting)\n");
        closesocket(sock); WSACleanup(); return 1;
    }

    const std::string username = args.remote_ufrag + ":" + args.local_ufrag;
    const uint64_t tie_breaker = 0xDEADBEEF12345678ULL;

    auto start = std::chrono::steady_clock::now();
    auto end   = start + std::chrono::seconds(args.duration_sec);
    auto last_send = start;

    int sent = 0, rx_packets = 0, rx_stun = 0, tx_success = 0;
    auto last_log = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < end) {
        auto now = std::chrono::steady_clock::now();
        if (now - last_send >= std::chrono::milliseconds(args.send_interval_ms)) {
            auto pkt = build_request(username, tie_breaker);
            int n = sendto(sock, (const char*)pkt.data(), (int)pkt.size(), 0,
                           (sockaddr*)&remote, sizeof(remote));
            if (n > 0) ++sent;
            else if (n < 0) fprintf(stderr, "[peer] sendto err=%d\n", WSAGetLastError());
            last_send = now;
        }
        timeval tv{0, 50 * 1000};
        fd_set r; FD_ZERO(&r); FD_SET(sock, &r);
        int sret = select(0, &r, nullptr, nullptr, &tv);
        if (sret > 0 && FD_ISSET(sock, &r)) {
            uint8_t buf[1500];
            sockaddr_in from{}; int fromlen = sizeof(from);
            int n = recvfrom(sock, (char*)buf, sizeof(buf), 0,
                             (sockaddr*)&from, &fromlen);
            if (n < 0) {
                int err = WSAGetLastError();
                fprintf(stderr, "[peer] recvfrom err=%d\n", err);
                continue;
            }
            ++rx_packets;
            std::string user;
            if (parse_request(buf, n, user)) {
                ++rx_stun;
                if (rx_stun <= 3 || rx_stun % 10 == 0)
                    fprintf(stderr, "[peer] RX STUN req #%d from %s:%d user=%s\n",
                            rx_stun, inet_ntoa(from.sin_addr),
                            ntohs(from.sin_port), user.c_str());
                auto resp = build_success(buf+8, &from);
                int m = sendto(sock, (const char*)resp.data(), (int)resp.size(), 0,
                               (sockaddr*)&from, fromlen);
                if (m > 0) ++tx_success;
            } else {
                if (rx_packets <= 3)
                    fprintf(stderr, "[peer] RX non-STUN (%d bytes) first=%02x\n",
                            n, buf[0]);
            }
        }
    }
    fprintf(stderr, "[peer] done: sent=%d rx_packets=%d rx_stun=%d tx_success=%d\n",
            sent, rx_packets, rx_stun, tx_success);
    closesocket(sock); WSACleanup();
    return 0;
}
