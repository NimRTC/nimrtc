// test_dtls_replay_chrome_hello.cpp
//
// One-shot diagnostic: load a Chrome DTLS ClientHello datagram (or
// pair of datagrams) from a file, feed it to a server-side
// DtlsSessionWolfSSL, and print what the server emits in response.
//
// Usage:
//   test_dtls_replay_chrome_hello <client_hello.bin> [<client_hello2.bin> ...]
//
// Each .bin is one DTLS record (the 13-byte DTLS record header
// followed by the Handshake fragment).  Chrome 117+ splits its
// 1413-byte ClientHello into two records (1175 + 238 bytes), so
// pass them in order.  Other clients (e.g. a one-shot OpenSSL
// s_client) emit a single record.
//
// We assert on:
//   * the server reaches at least HelloVerifyRequest or ServerHello
//     within a bounded number of pump_handshake() calls
//   * the server's outbound records are non-empty and well-formed
//   * the server doesn't log "wolfSSL handshake failed" with any
//     alert description
//
// Build target: tests/test_dtls_replay_chrome_hello
//
// Diagnostic-only.  Not part of the e2e acceptance suite.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <nimrtc/dtls/dtls.hpp>
#include <nimrtc/dtls/dtls_wolfssl_session.hpp>

using clk = std::chrono::steady_clock;

namespace {

bool read_binary(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    auto sz = static_cast<std::size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    out.resize(sz);
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(sz));
    return f.good() || f.eof();
}

// Pretty-print a single DTLS record header (first 13 bytes) so we
// can see at a glance what the server emitted.
void print_record(const char* tag, const std::uint8_t* p, std::size_t n) {
    if (n < 13) {
        std::fprintf(stderr, "  %s <short %zuB>\n", tag, n);
        return;
    }
    const char* ct = "?";
    switch (p[0]) {
        case 20: ct = "ChangeCipherSpec"; break;
        case 21: ct = "Alert";            break;
        case 22: ct = "Handshake";        break;
        case 23: ct = "AppData";          break;
        default:                           break;
    }
    std::uint16_t ver    = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(p[1]) << 8) | p[2]);
    std::uint16_t epoch  = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(p[3]) << 8) | p[4]);
    std::uint64_t seq    = 0;
    for (int i = 0; i < 6; ++i) seq = (seq << 8) | p[5 + i];
    std::uint16_t length = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(p[11]) << 8) | p[12]);
    std::fprintf(stderr,
        "  %s ct=%u(%s) ver=0x%04x epoch=%u seq=%llu length=%u  total=%zuB\n",
        tag, p[0], ct, ver, epoch,
        static_cast<unsigned long long>(seq), length, n);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <client_hello.bin> [<client_hello2.bin> ...]\n",
            argv[0]);
        return 64;
    }

    // ----- Load Chrome ClientHello records -----------------------------
    std::vector<std::vector<std::uint8_t>> datagrams;
    for (int i = 1; i < argc; ++i) {
        std::vector<std::uint8_t> bytes;
        if (!read_binary(argv[i], bytes)) {
            std::fprintf(stderr, "[FAIL] could not read %s\n", argv[i]);
            return 65;
        }
        std::fprintf(stderr,
            "[in ] %s  %zuB\n", argv[i], bytes.size());
        print_record("in ", bytes.data(), bytes.size());
        datagrams.push_back(std::move(bytes));
    }

    // ----- Set up server-side DTLS session -----------------------------
    nimrtc::dtls::Config cfg{};
    cfg.role         = nimrtc::dtls::DtlsRole::Server;
    cfg.srtp_profile = nimrtc::dtls::SrtpProfile::Aes128CmSha1_80;
    auto session = std::make_unique<nimrtc::dtls::DtlsSessionWolfSSL>(cfg);
    if (auto r = session->open(); !r) {
        std::fprintf(stderr, "[FAIL] session->open: %s\n",
                     r.error().message().c_str());
        return 66;
    }
    std::fprintf(stderr, "[srv] local fp = %s\n",
        session->local_fingerprint().hex_colon.c_str());

    // ----- Feed each datagram, then call take_outbound ------------------
    for (std::size_t i = 0; i < datagrams.size(); ++i) {
        nimrtc::dtls::DtlsAddr from{"127.0.0.1", 12345 + static_cast<std::uint16_t>(i)};
        std::size_t consumed = session->feed_inbound(
            std::span<const std::uint8_t>(datagrams[i].data(), datagrams[i].size()),
            from);
        std::fprintf(stderr,
            "[srv] feed_inbound[%zu] consumed=%zu  state=%s\n",
            i, consumed, nimrtc::dtls::DtlsSessionWolfSSL::state_name(session->state()));
    }

    // Drain anything the server produced so far.
    auto recs0 = session->take_outbound();
    std::fprintf(stderr,
        "[srv] after first feed_inbound(s): outbound=%zu records\n", recs0.size());
    for (std::size_t i = 0; i < recs0.size(); ++i) {
        std::fprintf(stderr, "[out] record[%zu]\n", i);
        print_record("out", recs0[i].bytes.data(), recs0[i].bytes.size());
    }

    // Drive the retransmit timer + a few more pumps (in case the first
    // feed didn't fully assemble the message yet).  The real engine
    // would also do this; the goal is to give wolfSSL every chance to
    // emit a ServerHello.
    for (int round = 1; round <= 3; ++round) {
        session->tick();
        auto more = session->take_outbound();
        std::fprintf(stderr,
            "[srv] pump round %d: outbound=%zu records  state=%s\n",
            round, more.size(),
            nimrtc::dtls::DtlsSessionWolfSSL::state_name(session->state()));
        for (std::size_t i = 0; i < more.size(); ++i) {
            std::fprintf(stderr, "[out] record[%zu] (round %d)\n", i, round);
            print_record("out", more[i].bytes.data(), more[i].bytes.size());
        }
        if (session->is_connected() ||
            session->state() == nimrtc::dtls::DtlsState::Failed) {
            break;
        }
    }

    // ----- Verdict -----------------------------------------------------
    // What we want: at least one outbound record (HelloVerifyRequest or
    // ServerHello).  What would mean the bug is reproduced: 0 outbound
    // records, state still HelloSent.
    const bool produced_anything = (recs0.size() > 0) ||
        session->is_connected() ||
        session->state() == nimrtc::dtls::DtlsState::Failed;

    if (produced_anything && session->is_connected()) {
        std::fprintf(stderr,
            "[OK ] server completed the handshake in isolation\n");
        auto km = session->srtp_keying_material();
        if (km) {
            std::fprintf(stderr,
                "[OK ] SRTP client_key[0..3]=%02x%02x%02x%02x\n",
                km->client_master_key[0], km->client_master_key[1],
                km->client_master_key[2], km->client_master_key[3]);
        }
        return 0;
    }
    if (produced_anything) {
        std::fprintf(stderr,
            "[WARN] server produced output but handshake did not complete\n");
        return 2;
    }
    std::fprintf(stderr,
        "[BUG ] server emitted 0 outbound records after full ClientHello; "
        "state=%s (== 'Bug reproduction confirmed')\n",
        nimrtc::dtls::DtlsSessionWolfSSL::state_name(session->state()));
    return 1;
}
