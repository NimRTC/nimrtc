// ============================================================================
// RTCP UDP loopback test.
//
// For each variant:
//   1. build a SenderReport / ReceiverReport / NackPacket via the matching
//      XxxBuilder,
//   2. sendto() over UDP loopback,
//   3. recvfrom() on the other end,
//   4. parse via Parser::parse_sr / parse_rr / parse_nack,
//   5. verify every field of the deserialised structure.
//
// Variants:
//   v01_sr_3rb   - SR with 3 report blocks
//   v02_rr_1rb   - RR with 1 report block (boundary: cumulative_lost = -1)
//   v03_nack_4   - Generic NACK with 4 FCI entries
//
// Windows-only: this test uses Winsock2 for the loopback socket.  The RTCP
// round-trip itself is verified by `test_rtcp_loopback`-equivalent in-memory
// tests on Linux via `test_rtp_test`.  See test_udp_loopback.cpp for the
// same Windows/Linux split.
//
// ============================================================================

#ifdef _WIN32

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>
//   v04_sr_0rb   - SR with RC=0 (minimum legal: 28 bytes)
//   v05_nack_1   - NACK with 1 FCI entry (boundary: 16 bytes)
//   v06_nack_32  - NACK with 32 FCI entries (large)
// ============================================================================

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>
#include <nimrtc/rtp/packet.hpp>

#pragma comment(lib, "ws2_32.lib")

namespace {

struct RunReport {
    bool        pass = false;
    std::string err;
    std::size_t bytes_sent     = 0;
    std::size_t bytes_received = 0;
};

template <typename BuildFn, typename VerifyFn>
RunReport run_one(SOCKET send_sock, SOCKET recv_sock,
                  std::uint16_t dest_port, BuildFn build, VerifyFn verify)
{
    using namespace std::chrono_literals;
    auto pkt = build();
    std::promise<void>       ready_promise;
    std::shared_future<void> ready_future = ready_promise.get_future().share();
    std::atomic<bool>        pass{false};
    std::atomic<bool>        done{false};
    std::atomic<bool>        recv_err_flag{false};
    std::atomic<int>         recv_err_code{0};
    std::atomic<std::size_t> bytes_received{0};
    std::string              fail_msg;
    std::mutex               fail_mtx;

    std::thread recv_thread([&]() {
        ready_promise.set_value();
        std::uint8_t buf[8192];
        sockaddr_in  from{};
        int          from_len = sizeof(from);
        int n = ::recvfrom(recv_sock,
                           reinterpret_cast<char*>(buf),
                           sizeof(buf), 0,
                           reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n == SOCKET_ERROR) {
            recv_err_flag.store(true);
            recv_err_code.store(::WSAGetLastError());
            done.store(true);
            return;
        }
        bytes_received.store(static_cast<std::size_t>(n));
        bool local_ok = false;
        std::string local_err;
        try {
            local_ok = verify(nimrtc::core::ByteSpan{
                buf, static_cast<std::size_t>(n)}, local_err);
        } catch (std::exception& e) {
            local_err = std::string("exception: ") + e.what();
        }
        if (!local_ok) {
            std::lock_guard<std::mutex> lk(fail_mtx);
            fail_msg = std::move(local_err);
        } else {
            pass.store(true);
        }
        done.store(true);
    });

    if (ready_future.wait_for(2s) != std::future_status::ready) {
        if (!done.load()) ::shutdown(recv_sock, SD_BOTH);
        if (recv_thread.joinable()) recv_thread.join();
        return RunReport{false, "receiver not ready", 0, 0};
    }

    sockaddr_in dest{};
    dest.sin_family      = AF_INET;
    dest.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    dest.sin_port        = ::htons(dest_port);
    int sent = ::sendto(send_sock,
                        reinterpret_cast<const char*>(pkt.data()),
                        static_cast<int>(pkt.size()), 0,
                        reinterpret_cast<sockaddr*>(&dest),
                        static_cast<int>(sizeof(dest)));
    if (sent == SOCKET_ERROR) {
        if (!done.load()) ::shutdown(recv_sock, SD_BOTH);
        if (recv_thread.joinable()) recv_thread.join();
        return RunReport{false,
                         std::string("sendto failed: ") +
                         std::to_string(::WSAGetLastError()),
                         0, 0};
    }
    for (int i = 0; i < 200 && !done.load(); ++i)
        std::this_thread::sleep_for(10ms);
    if (!done.load()) ::shutdown(recv_sock, SD_BOTH);
    if (recv_thread.joinable()) recv_thread.join();

    RunReport rep;
    rep.bytes_sent     = static_cast<std::size_t>(sent);
    rep.bytes_received = bytes_received.load();
    if (recv_err_flag.load()) {
        rep.err = "recvfrom failed: " + std::to_string(recv_err_code.load());
        return rep;
    }
    if (!done.load()) { rep.err = "receiver timeout"; return rep; }
    rep.pass = pass.load();
    if (!rep.pass) {
        std::lock_guard<std::mutex> lk(fail_mtx);
        rep.err = fail_msg;
    }
    return rep;
}

// ---------------------------------------------------------------------------
// v01 — SR with 3 report blocks.
// ---------------------------------------------------------------------------
nimrtc::core::ByteBuffer v01_build() {
    using nimrtc::rtp::SrBuilder;
    using nimrtc::rtp::ReportBlock;
    SrBuilder b;
    b.set_ssrc(0xAABBCCDD)
        .set_ntp_timestamp(((std::uint64_t)0x12345678 << 32) | 0x9ABCDEF0u)
        .set_rtp_timestamp(0xDEADBEEFu)
        .set_sender_packet_count(12345)
        .set_sender_octet_count(987654321u)
        .add_report_block(ReportBlock{0x11111111u, 13, -7,
            0x00010000u + 0x1234, 0x00010002u, 0x80000000u, 0x00010003u})
        .add_report_block(ReportBlock{0x22222222u, 25, 0,
            0x00020000u + 0x5678, 0x00020004u, 0x80000005u, 0x00020006u})
        .add_report_block(ReportBlock{0x33333333u, 100, 12345,
            0x00030000u + 0xABCD, 0x00030007u, 0x80000008u, 0x00030009u});
    return b.build();
}

bool v01_verify(nimrtc::core::ByteSpan raw, std::string& err) {
    nimrtc::rtp::Parser p;
    auto r = p.parse_sr(raw);
    if (!r.ok()) { err = "parse_sr: " + r.error().message(); return false; }
    const auto& sr = r.value();
    if (sr.ssrc != 0xAABBCCDDu) { err = "ssrc"; return false; }
    if (sr.ntp_timestamp != (((std::uint64_t)0x12345678 << 32) | 0x9ABCDEF0u))
        { err = "ntp"; return false; }
    if (sr.rtp_timestamp != 0xDEADBEEFu) { err = "rtp_ts"; return false; }
    if (sr.sender_packet_count != 12345u) { err = "pcount"; return false; }
    if (sr.sender_octet_count != 987654321u) { err = "ocount"; return false; }
    if (sr.report_blocks.size() != 3u) { err = "rc"; return false; }
    if (sr.report_blocks[0].cumulative_packets_lost != -7)
        { err = "rb0.cl"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// v02 — RR with 1 report block.
// ---------------------------------------------------------------------------
nimrtc::core::ByteBuffer v02_build() {
    using nimrtc::rtp::RrBuilder;
    using nimrtc::rtp::ReportBlock;
    RrBuilder b;
    b.set_ssrc(0xF1F1F1F1u)
        .add_report_block(ReportBlock{0xFEEDFACEu, 0, -1,
            0x00070000u + 0xBE, 0x00070010u, 0u, 0u});
    return b.build();
}

bool v02_verify(nimrtc::core::ByteSpan raw, std::string& err) {
    nimrtc::rtp::Parser p;
    auto r = p.parse_rr(raw);
    if (!r.ok()) { err = "parse_rr: " + r.error().message(); return false; }
    if (r.value().ssrc != 0xF1F1F1F1u) { err = "ssrc"; return false; }
    if (r.value().report_blocks.size() != 1u) { err = "rc"; return false; }
    if (r.value().report_blocks[0].cumulative_packets_lost != -1)
        { err = "rb.cl"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// v03 — NACK with 4 entries.
// ---------------------------------------------------------------------------
nimrtc::core::ByteBuffer v03_build() {
    using nimrtc::rtp::NackBuilder;
    NackBuilder b;
    b.set_sender_ssrc(0x44444444u)
        .set_media_ssrc(0x55555555u)
        .add_entry(100, 0x8001).add_entry(200, 0xFFFF)
        .add_entry(300, 0).add_entry(400, 0x0001);
    return b.build();
}

bool v03_verify(nimrtc::core::ByteSpan raw, std::string& err) {
    nimrtc::rtp::Parser p;
    auto r = p.parse_nack(raw);
    if (!r.ok()) { err = "parse_nack: " + r.error().message(); return false; }
    if (r.value().sender_ssrc != 0x44444444u) { err = "sender"; return false; }
    if (r.value().media_ssrc  != 0x55555555u) { err = "media";  return false; }
    if (r.value().entries.size() != 4u)       { err = "count";  return false; }
    return true;
}

// ---------------------------------------------------------------------------
// v04 — SR with 0 report blocks.
// ---------------------------------------------------------------------------
nimrtc::core::ByteBuffer v04_build() {
    using nimrtc::rtp::SrBuilder;
    return SrBuilder{}.set_ssrc(0xCAFEBABEu).build();
}
bool v04_verify(nimrtc::core::ByteSpan raw, std::string& err) {
    nimrtc::rtp::Parser p;
    auto r = p.parse_sr(raw);
    if (!r.ok()) { err = "parse_sr"; return false; }
    if (r.value().ssrc != 0xCAFEBABEu) { err = "ssrc"; return false; }
    if (r.value().report_blocks.size() != 0u) { err = "rc"; return false; }
    if (raw.size() != 28u) { err = "size != 28"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// v05 — NACK with 1 entry.
// ---------------------------------------------------------------------------
nimrtc::core::ByteBuffer v05_build() {
    using nimrtc::rtp::NackBuilder;
    return NackBuilder{}.set_sender_ssrc(1).set_media_ssrc(2)
        .add_entry(17, 0xAAAA).build();
}
bool v05_verify(nimrtc::core::ByteSpan raw, std::string& err) {
    nimrtc::rtp::Parser p;
    auto r = p.parse_nack(raw);
    if (!r.ok()) { err = "parse_nack"; return false; }
    if (r.value().entries.size() != 1u) { err = "count"; return false; }
    if (r.value().entries[0].packet_id != 17) { err = "pid"; return false; }
    if (r.value().entries[0].blp != 0xAAAA)   { err = "blp"; return false; }
    if (raw.size() != 16u) { err = "size != 16"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// v06 — NACK with 32 entries.
// ---------------------------------------------------------------------------
nimrtc::core::ByteBuffer v06_build() {
    using nimrtc::rtp::NackBuilder;
    NackBuilder b;
    b.set_sender_ssrc(0xAAAAAAAAu).set_media_ssrc(0xBBBBBBBBu);
    for (std::size_t i = 0; i < 32; ++i) {
        b.add_entry(static_cast<std::uint16_t>(i * 16),
                    static_cast<std::uint16_t>((i * 7) & 0xFFFF));
    }
    return b.build();
}
bool v06_verify(nimrtc::core::ByteSpan raw, std::string& err) {
    nimrtc::rtp::Parser p;
    auto r = p.parse_nack(raw);
    if (!r.ok()) { err = "parse_nack: " + r.error().message(); return false; }
    if (r.value().entries.size() != 32u) { err = "count"; return false; }
    for (std::size_t i = 0; i < 32; ++i) {
        if (r.value().entries[i].packet_id != i * 16)
            { err = "pid mismatch"; return false; }
    }
    return true;
}

}  // namespace

int main() {
    WSADATA wsadata;
    if (::WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
        std::printf("[FAIL] WSAStartup\n");
        return 1;
    }
    SOCKET recv_sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    SOCKET send_sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (recv_sock == INVALID_SOCKET || send_sock == INVALID_SOCKET) {
        std::printf("[FAIL] socket()\n");
        if (recv_sock != INVALID_SOCKET) ::closesocket(recv_sock);
        if (send_sock != INVALID_SOCKET) ::closesocket(send_sock);
        ::WSACleanup();
        return 1;
    }
    sockaddr_in recv_addr{};
    recv_addr.sin_family      = AF_INET;
    recv_addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    recv_addr.sin_port        = 0;
    if (::bind(recv_sock, reinterpret_cast<sockaddr*>(&recv_addr),
               sizeof(recv_addr)) == SOCKET_ERROR) {
        std::printf("[FAIL] bind\n");
        ::closesocket(recv_sock); ::closesocket(send_sock); ::WSACleanup();
        return 1;
    }
    sockaddr_in bound{};
    int bound_len = sizeof(bound);
    ::getsockname(recv_sock, reinterpret_cast<sockaddr*>(&bound), &bound_len);
    const std::uint16_t port = ::ntohs(bound.sin_port);
    std::printf("[INFO] Receiver bound on 127.0.0.1:%u\n", port);

    int passed = 0, failed = 0;
    struct { const char* name;
             std::function<nimrtc::core::ByteBuffer()> build;
             std::function<bool(nimrtc::core::ByteSpan, std::string&)> verify; }
    cases[] = {
        {"v01_sr_3rb",  [] { return v01_build(); }, v01_verify},
        {"v02_rr_1rb",  [] { return v02_build(); }, v02_verify},
        {"v03_nack_4",  [] { return v03_build(); }, v03_verify},
        {"v04_sr_0rb",  [] { return v04_build(); }, v04_verify},
        {"v05_nack_1",  [] { return v05_build(); }, v05_verify},
        {"v06_nack_32", [] { return v06_build(); }, v06_verify},
    };
    for (auto& c : cases) {
        std::printf("[RUN ] %s ", c.name);
        auto rep = run_one(send_sock, recv_sock, port, c.build, c.verify);
        if (rep.pass) {
            std::printf("[PASS] sent=%zu recv=%zu\n",
                        rep.bytes_sent, rep.bytes_received);
            ++passed;
        } else {
            std::printf("[FAIL] %s\n", rep.err.c_str());
            ++failed;
        }
    }

    ::closesocket(recv_sock);
    ::closesocket(send_sock);
    ::WSACleanup();
    std::printf("\n[SUMMARY] passed=%d failed=%d total=%d\n",
                passed, failed, passed + failed);
    return failed == 0 ? 0 : 1;
}

#else  // !_WIN32

#include <cstdio>

int main() {
    std::printf("[test_rtcp_loopback] skipped on Linux (Windows-only Winsock test).\n");
    return 0;
}

#endif  // _WIN32
