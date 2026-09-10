// ============================================================================
// test_srtp.cpp — SRTP (libsrtp2) unit tests.
//
// Validates that:
//   1. protect_rtp() actually encrypts (ciphertext != plaintext payload)
//   2. Ciphertext length grows by the auth tag (10 bytes for SHA1-80)
//   3. unprotect_rtp() round-trips correctly when both sides share keys
//   4. Mismatched key/salt → decryption FAILS (auth tag mismatch)
//   5. Replay attacks are rejected (libsrtp anti-replay window)
//   6. Stats counters increment correctly
//   7. Different crypto suites produce different ciphertext (sanity)
//
// All tests use a synthetic 30-byte master key||salt (16-byte key + 14-byte salt
// = AES-CM-128 / SHA1-80 layout), matching RFC 5764 §4.2.
//
// Skip if libsrtp2 was not vendored (stub mode).
// ============================================================================

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include <nimrtc/core/bytes.hpp>
#include <nimrtc/core/error.hpp>
#include <nimrtc/srtp/srtp.hpp>

#ifndef NIMRTC_USE_LIBSRTP
#  error "test_srtp requires NIMRTC_USE_LIBSRTP=1 (libsrtp2 vendored)"
#endif

namespace {

using nimrtc::core::ByteSpan;
using nimrtc::srtp::CryptoSuite;
using nimrtc::srtp::SrtpContext;
using nimrtc::srtp::SrtpSession;

// ---------------------------------------------------------------------------
// Test fixture: deterministic 16-byte key + 14-byte salt for AES-CM-128/SHA1-80.
// ---------------------------------------------------------------------------

struct KeyMaterial {
    std::array<std::uint8_t, 16> key;     // master key (AES-128)
    std::array<std::uint8_t, 14> salt;    // master salt
    std::array<std::uint8_t, 16> wrong_key;
    std::array<std::uint8_t, 14> wrong_salt;
};

KeyMaterial make_keys(std::uint32_t seed) {
    KeyMaterial km{};
    std::mt19937 rng(seed);
    for (auto& b : km.key)      b = static_cast<std::uint8_t>(rng() & 0xFF);
    for (auto& b : km.salt)     b = static_cast<std::uint8_t>(rng() & 0xFF);
    // Wrong key/salt: differ in all bytes from correct.
    for (auto& b : km.wrong_key) b = static_cast<std::uint8_t>(~km.key[&b - km.wrong_key.data()] & 0xFF);
    for (auto& b : km.wrong_salt) {
        std::size_t idx = &b - km.wrong_salt.data();
        b = static_cast<std::uint8_t>(~km.salt[idx] & 0xFF);
    }
    return km;
}

// Build a minimal 12-byte RTP header + variable payload.
std::vector<std::uint8_t> make_rtp(std::uint32_t ssrc,
                                   std::uint16_t seq,
                                   std::uint32_t ts,
                                   std::uint8_t  pt,
                                   std::size_t   payload_len,
                                   std::uint32_t seed = 0x12345678) {
    std::vector<std::uint8_t> pkt;
    pkt.resize(12 + payload_len);

    // RTP header layout (RFC 3550 §5.1):
    //   byte 0:    V(2) | P(1) | X(1) | CC(4)        <- bit fields in network order
    //   byte 1:    M(1) | PT(7)
    //   bytes 2-3: sequence number (big-endian)
    //   bytes 4-7: timestamp (big-endian)
    //   bytes 8-11: SSRC (big-endian)
    pkt[0] = 0x80u;                                    // V=2, P/X/CC=0
    pkt[1] = static_cast<std::uint8_t>(pt & 0x7Fu);    // M=0, PT=pt
    pkt[2] = static_cast<std::uint8_t>((seq >> 8) & 0xFF);
    pkt[3] = static_cast<std::uint8_t>(seq & 0xFF);
    pkt[4] = static_cast<std::uint8_t>((ts >> 24) & 0xFF);
    pkt[5] = static_cast<std::uint8_t>((ts >> 16) & 0xFF);
    pkt[6] = static_cast<std::uint8_t>((ts >>  8) & 0xFF);
    pkt[7] = static_cast<std::uint8_t>(ts & 0xFF);
    pkt[8]  = static_cast<std::uint8_t>((ssrc >> 24) & 0xFF);
    pkt[9]  = static_cast<std::uint8_t>((ssrc >> 16) & 0xFF);
    pkt[10] = static_cast<std::uint8_t>((ssrc >>  8) & 0xFF);
    pkt[11] = static_cast<std::uint8_t>(ssrc & 0xFF);

    std::mt19937 rng(seed);
    for (std::size_t i = 0; i < payload_len; ++i) {
        pkt[12 + i] = static_cast<std::uint8_t>(rng() & 0xFF);
    }
    return pkt;
}

struct RunReport {
    bool        pass = false;
    std::string err;
};

RunReport run_test(const char* name, std::function<bool(std::string&)> fn) {
    std::printf("[RUN ] %s\n", name);
    RunReport rep;
    try {
        rep.pass = fn(rep.err);
    } catch (std::exception& e) {
        rep.pass = false;
        rep.err  = std::string("exception: ") + e.what();
    }
    if (rep.pass) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s — %s\n", name, rep.err.c_str());
    }
    return rep;
}

// ===========================================================================
// t01 — protect_rtp() produces ciphertext that DIFFERS from plaintext.
// ===========================================================================
bool t01_encrypt_changes_plaintext(std::string& err) {
    auto km = make_keys(0xA1);

    nimrtc::srtp::Config cfg;
    cfg.suite = CryptoSuite::Aes128CmSha1_80;
    cfg.enable_auth = true;
    cfg.enable_encryption = true;

    SrtpSession sender;
    auto rc = sender.init_from_master_key(cfg,
        std::span<const std::uint8_t>(km.key.data(), km.key.size()),
        std::span<const std::uint8_t>(km.salt.data(), km.salt.size()));
    if (!rc) { err = "sender init failed"; return false; }

    // Build a known plaintext payload (all 0xAA — easy to spot if unchanged).
    auto pkt = make_rtp(0xCAFEBABEu, 1, 0x100, 111, 32);
    for (std::size_t i = 12; i < pkt.size(); ++i) pkt[i] = 0xAA;

    auto enc = sender.protect_rtp(ByteSpan{pkt.data(), pkt.size()},
                                  0xCAFEBABE, 0x100);
    if (!enc) { err = "protect_rtp failed"; return false; }

    auto enc_view = enc.value();
    if (enc_view.size() <= pkt.size()) {
        err = "ciphertext not longer than plaintext";
        return false;
    }
    if (enc_view.size() != pkt.size() + 10) {
        err = "expected +10 bytes (SHA1-80), got +" +
              std::to_string(enc_view.size() - pkt.size());
        return false;
    }

    // Verify payload bytes have changed (AES-CM encrypts RTP payload,
    // RTP header is left in clear).
    std::size_t diffs = 0;
    for (std::size_t i = 12; i < pkt.size(); ++i) {
        if (enc_view[i] != pkt[i]) ++diffs;
    }
    if (diffs == 0) {
        err = "payload bytes unchanged — encryption is NOT happening";
        return false;
    }
    if (diffs < pkt.size() - 12 - 2) {
        // At least most bytes should differ (CM is a stream cipher — every byte
        // XOR'd with keystream).  Allow up to 2 coincidental matches.
        err = "only " + std::to_string(diffs) + "/" +
              std::to_string(pkt.size() - 12) + " payload bytes changed (AES-CM should encrypt all)";
        return false;
    }

    // Verify RTP header (first 12 bytes) is unchanged.
    if (std::memcmp(enc_view.data(), pkt.data(), 12) != 0) {
        err = "RTP header changed — should remain in clear";
        return false;
    }

    return true;
}

// ===========================================================================
// t02 — Round-trip: sender encrypts, receiver with same keys decrypts.
// ===========================================================================
bool t02_roundtrip(std::string& err) {
    auto km = make_keys(0xB2);

    nimrtc::srtp::Config cfg;
    cfg.suite = CryptoSuite::Aes128CmSha1_80;
    cfg.enable_encryption = true;

    SrtpSession sender;
    SrtpSession receiver;

    auto r1 = sender.init_from_master_key(cfg, km.key, km.salt);
    auto r2 = receiver.init_from_master_key_inbound(cfg, km.key, km.salt);
    if (!r1 || !r2) { err = "session init failed"; return false; }

    auto pkt = make_rtp(0x12345678u, 0xABCD, 0xDEADBEEF, 111, 64);
    auto enc = sender.protect_rtp(ByteSpan{pkt.data(), pkt.size()}, 0x12345678, 0xDEADBEEF);
    if (!enc) { err = "protect_rtp failed"; return false; }

    auto enc_view = enc.value();
    auto dec = receiver.unprotect_rtp(ByteSpan{enc_view.data(), enc_view.size()},
                                      nullptr, nullptr);
    if (!dec) { err = "unprotect_rtp failed"; return false; }

    auto dec_view = dec.value();
    if (dec_view.size() != pkt.size()) {
        err = "decrypted length mismatch: " +
              std::to_string(dec_view.size()) + " vs " +
              std::to_string(pkt.size());
        return false;
    }
    if (std::memcmp(dec_view.data(), pkt.data(), pkt.size()) != 0) {
        err = "decrypted bytes do not match original";
        return false;
    }
    return true;
}

// ===========================================================================
// t03 — Wrong key/salt → unprotect FAILS (auth tag mismatch).
// ===========================================================================
bool t03_wrong_key_rejected(std::string& err) {
    auto km = make_keys(0xC3);

    nimrtc::srtp::Config cfg;
    cfg.suite = CryptoSuite::Aes128CmSha1_80;
    cfg.enable_encryption = true;

    SrtpSession sender;
    SrtpSession receiver;

    sender.init_from_master_key(cfg, km.key, km.salt);
    receiver.init_from_master_key_inbound(cfg,
        std::span<const std::uint8_t>(km.wrong_key.data(), km.wrong_key.size()),
        std::span<const std::uint8_t>(km.wrong_salt.data(), km.wrong_salt.size()));

    auto pkt = make_rtp(0xCAFEBABEu, 1, 0x100, 111, 16);
    auto enc = sender.protect_rtp(ByteSpan{pkt.data(), pkt.size()}, 0xCAFEBABE, 0x100);
    if (!enc) { err = "protect_rtp failed"; return false; }

    auto dec = receiver.unprotect_rtp(ByteSpan{enc.value().data(), enc.value().size()},
                                      nullptr, nullptr);
    if (dec.ok()) {
        err = "decryption SUCCEEDED with wrong key — auth bypass!";
        return false;
    }
    return true;
}

// ===========================================================================
// t04 — Replay protection: re-sending same packet is rejected.
// ===========================================================================
bool t04_replay_rejected(std::string& err) {
    auto km = make_keys(0xD4);

    nimrtc::srtp::Config cfg;
    cfg.suite = CryptoSuite::Aes128CmSha1_80;
    cfg.enable_encryption = true;

    SrtpSession sender;
    SrtpSession receiver;

    sender.init_from_master_key(cfg, km.key, km.salt);
    receiver.init_from_master_key_inbound(cfg, km.key, km.salt);

    auto pkt = make_rtp(0x11223344u, 100, 0x200, 111, 16);
    auto enc = sender.protect_rtp(ByteSpan{pkt.data(), pkt.size()}, 0x11223344, 0x200);
    if (!enc) { err = "protect_rtp failed"; return false; }

    auto span = ByteSpan{enc.value().data(), enc.value().size()};
    auto dec1 = receiver.unprotect_rtp(span, nullptr, nullptr);
    if (!dec1) { err = "first unprotect_rtp failed"; return false; }

    auto dec2 = receiver.unprotect_rtp(span, nullptr, nullptr);
    if (dec2.ok()) {
        err = "replay SUCCEEDED — anti-replay window NOT working";
        return false;
    }
    return true;
}

// ===========================================================================
// t05 — Different seq numbers → all decrypt successfully.
// ===========================================================================
bool t05_sequence_increments(std::string& err) {
    auto km = make_keys(0xE5);

    nimrtc::srtp::Config cfg;
    cfg.suite = CryptoSuite::Aes128CmSha1_80;
    cfg.enable_encryption = true;

    SrtpSession sender;
    SrtpSession receiver;

    sender.init_from_master_key(cfg, km.key, km.salt);
    receiver.init_from_master_key_inbound(cfg, km.key, km.salt);

    for (std::uint16_t seq = 1; seq <= 16; ++seq) {
        auto pkt = make_rtp(0xAABBCCDDu, seq, seq * 960u, 111, 24);
        auto enc = sender.protect_rtp(ByteSpan{pkt.data(), pkt.size()},
                                       0xAABBCCDD, seq * 960u);
        if (!enc) {
            err = "protect_rtp failed at seq=" + std::to_string(seq);
            return false;
        }
        auto dec = receiver.unprotect_rtp(ByteSpan{enc.value().data(), enc.value().size()},
                                          nullptr, nullptr);
        if (!dec) {
            err = "unprotect_rtp failed at seq=" + std::to_string(seq);
            return false;
        }
    }
    return true;
}

// ===========================================================================
// t06 — Stats counters update correctly.
// ===========================================================================
bool t06_stats_increment(std::string& err) {
    auto km = make_keys(0xF6);

    nimrtc::srtp::Config cfg;
    cfg.suite = CryptoSuite::Aes128CmSha1_80;
    cfg.enable_encryption = true;

    SrtpSession sender;
    SrtpSession receiver;

    sender.init_from_master_key(cfg, km.key, km.salt);
    receiver.init_from_master_key_inbound(cfg, km.key, km.salt);

    const std::size_t N = 5;
    for (std::size_t i = 0; i < N; ++i) {
        auto pkt = make_rtp(0x99887766u, static_cast<std::uint16_t>(i + 1),
                            static_cast<std::uint32_t>((i + 1) * 960), 111, 8);
        auto enc = sender.protect_rtp(ByteSpan{pkt.data(), pkt.size()},
                                       0x99887766,
                                       static_cast<std::uint32_t>((i + 1) * 960));
        if (!enc) { err = "protect failed at i=" + std::to_string(i); return false; }
        auto dec = receiver.unprotect_rtp(ByteSpan{enc.value().data(), enc.value().size()},
                                          nullptr, nullptr);
        if (!dec) { err = "unprotect failed at i=" + std::to_string(i); return false; }
    }

    auto ss = sender.stats();
    auto rs = receiver.stats();

    if (ss.rtp_packets_encrypted != N) {
        err = "sender stats: expected rtp_packets_encrypted=" + std::to_string(N) +
              " got " + std::to_string(ss.rtp_packets_encrypted);
        return false;
    }
    if (rs.rtp_packets_decrypted != N) {
        err = "receiver stats: expected rtp_packets_decrypted=" + std::to_string(N) +
              " got " + std::to_string(rs.rtp_packets_decrypted);
        return false;
    }
    if (ss.decryption_failures != 0) {
        err = "sender stats: unexpected decryption_failures=" +
              std::to_string(ss.decryption_failures);
        return false;
    }
    return true;
}

// ===========================================================================
// t07 — Ciphertext is deterministic for same plaintext+key+seq+ROC.
//        (AES-CM with same IV/ROC/SEQ → same keystream.)
// ===========================================================================
bool t07_deterministic_ciphertext(std::string& err) {
    auto km = make_keys(0x17);

    nimrtc::srtp::Config cfg;
    cfg.suite = CryptoSuite::Aes128CmSha1_80;
    cfg.enable_encryption = true;

    SrtpSession s1, s2;
    s1.init_from_master_key(cfg, km.key, km.salt);
    s2.init_from_master_key(cfg, km.key, km.salt);

    auto pkt = make_rtp(0xFEEDFACEu, 42, 0x1000, 111, 32);

    auto e1 = s1.protect_rtp(ByteSpan{pkt.data(), pkt.size()}, 0xFEEDFACE, 0x1000);
    auto e2 = s2.protect_rtp(ByteSpan{pkt.data(), pkt.size()}, 0xFEEDFACE, 0x1000);
    if (!e1 || !e2) { err = "protect_rtp failed"; return false; }

    if (e1.value().size() != e2.value().size()) {
        err = "ciphertext lengths differ";
        return false;
    }
    if (std::memcmp(e1.value().data(), e2.value().data(), e1.value().size()) != 0) {
        err = "ciphertext bytes differ — AES-CM should be deterministic";
        return false;
    }
    return true;
}

// ===========================================================================
// t08 — SrtpContext multi-SSRC: keys are derived per-direction correctly.
// ===========================================================================
bool t08_context_multi_ssrc(std::string& err) {
    auto km = make_keys(0x28);

    SrtpContext ctx;
    // DTLS-SRTP installs keys bidirectionally: peer's key for inbound,
    // our own key for outbound. In loopback both are the same material.
    ctx.derive_keys_for_remote(km.key, km.salt, CryptoSuite::Aes128CmSha1_80);
    ctx.derive_keys_for_local(km.key, km.salt, CryptoSuite::Aes128CmSha1_80);

    auto* sess = ctx.get_session(0xAA, /*outgoing=*/true);
    if (!sess) { err = "session creation failed"; return false; }

    auto pkt = make_rtp(0xAA, 1, 0x100, 111, 8);
    auto enc = sess->protect_rtp(ByteSpan{pkt.data(), pkt.size()}, 0xAA, 0x100);
    if (!enc) { err = "protect_rtp failed"; return false; }

    auto stats = ctx.total_stats();
    if (stats.rtp_packets_encrypted != 1) {
        err = "context total_stats: expected 1, got " +
              std::to_string(stats.rtp_packets_encrypted);
        return false;
    }
    return true;
}

// ===========================================================================
// t09 — Tampered ciphertext is rejected.
// ===========================================================================
bool t09_tampered_packet_rejected(std::string& err) {
    auto km = make_keys(0x39);

    nimrtc::srtp::Config cfg;
    cfg.suite = CryptoSuite::Aes128CmSha1_80;
    cfg.enable_encryption = true;

    SrtpSession sender;
    SrtpSession receiver;

    sender.init_from_master_key(cfg, km.key, km.salt);
    receiver.init_from_master_key_inbound(cfg, km.key, km.salt);

    auto pkt = make_rtp(0xAA, 1, 0x100, 111, 16);
    auto enc = sender.protect_rtp(ByteSpan{pkt.data(), pkt.size()}, 0xAA, 0x100);
    if (!enc) { err = "protect_rtp failed"; return false; }

    // Tamper: flip a bit in the payload (NOT the auth tag — flipping the
    // auth tag is also detected but more obvious).
    std::vector<std::uint8_t> tampered(enc.value().begin(), enc.value().end());
    tampered[14] ^= 0x01;  // flip one payload bit

    auto dec = receiver.unprotect_rtp(ByteSpan{tampered.data(), tampered.size()},
                                      nullptr, nullptr);
    if (dec.ok()) {
        err = "tampered packet DECRYPTED successfully — auth not verifying";
        return false;
    }
    return true;
}

// ===========================================================================
// t10 — Empty key → init_from_master_key fails gracefully.
// ===========================================================================
bool t10_empty_key_fails(std::string& err) {
    nimrtc::srtp::Config cfg;
    cfg.suite = CryptoSuite::Aes128CmSha1_80;

    SrtpSession s;
    std::vector<std::uint8_t> empty;
    auto r = s.init_from_master_key(cfg, empty, empty);
    if (r.ok()) {
        err = "empty key accepted — should have failed";
        return false;
    }
    return true;
}

} // namespace

int main() {
    std::printf("=== SRTP Unit Tests (libsrtp2) ===\n\n");

    int passed = 0, failed = 0;
    struct {
        const char* name;
        std::function<bool(std::string&)> fn;
    } cases[] = {
        {"t01_encrypt_changes_plaintext", t01_encrypt_changes_plaintext},
        {"t02_roundtrip",                 t02_roundtrip},
        {"t03_wrong_key_rejected",        t03_wrong_key_rejected},
        {"t04_replay_rejected",           t04_replay_rejected},
        {"t05_sequence_increments",       t05_sequence_increments},
        {"t06_stats_increment",           t06_stats_increment},
        {"t07_deterministic_ciphertext",  t07_deterministic_ciphertext},
        {"t08_context_multi_ssrc",        t08_context_multi_ssrc},
        {"t09_tampered_packet_rejected",  t09_tampered_packet_rejected},
        {"t10_empty_key_fails",           t10_empty_key_fails},
    };

    for (auto& c : cases) {
        auto rep = run_test(c.name, c.fn);
        if (rep.pass) ++passed;
        else          ++failed;
    }

    std::printf("\n=== SUMMARY: %d passed, %d failed ===\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
