// ============================================================================
// test_dtls_prf_kat.cpp — Known-Answer-Test for the TLS 1.2 PRF (P_SHA-256).
//
// What this test verifies
// -----------------------
// 1. dtls_hmac_sha256 against RFC 4231 §4.2 test case 1 + 2  (the well-known
//    "Hi There" / "Jefe" HMAC-SHA-256 vectors).  Catches the long-running
//    hardcoded-hash-object-size bug we already found on Windows.
// 2. tls12_prf and tls_prf_p_sha256 against fixed (secret, label, seed) →
//    bytes.  Cross-checked against an independent Python reference
//    implementation (see build/gen_kat.py) so a regression in the P_hash
//    chaining would surface as a hard failure here rather than as a
//    12-byte verify_data mismatch against Chrome.
// 3. Output length policy: tls12_prf for non-zero out_len produces exactly
//    out_len bytes; for out_len == 0 produces an empty vector.
// 4. TLS 1.2 PRF does NOT abort on out_len > 32 (i.e. the chained HMAC
//    expansion spans multiple A(i) iterations).
//
// Build + run
// -----------
//   cmake --build build --target test_dtls_prf_kat
//   build/tests/Debug/test_dtls_prf_kat.exe
//
// All KATs below reference build/gen_kat.py.  If you change the PRF, run
// gen_kat.py and update the expected_hex strings here.
// ============================================================================

#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/dtls/dtls_prf.hpp>

namespace {

using nimrtc::dtls::dtls_hmac_sha256;
using nimrtc::dtls::tls12_prf;
using nimrtc::dtls::tls_prf_p_sha256;

int g_fail = 0;

std::string bytes_to_hex(std::span<const std::uint8_t> b) {
    std::string out;
    out.reserve(b.size() * 2);
    char buf[4];
    for (auto x : b) { std::snprintf(buf, sizeof(buf), "%02x", x); out += buf; }
    return out;
}

#define EXPECT_EQ_HEX(actual_bytes, expected_hex, label) do {                   \
    auto _a = (actual_bytes);                                                  \
    std::string _g = bytes_to_hex(_a);                                         \
    std::string _e = (expected_hex);                                           \
    if (_g != _e) {                                                            \
        std::printf("[FAIL] %s\n  got:  %s\n  want: %s\n", label,               \
                    _g.c_str(), _e.c_str());                                   \
        ++g_fail;                                                              \
    } else {                                                                   \
        std::printf("[ OK ] %s  ==  %s\n", label, _e.c_str());                 \
    }                                                                          \
} while (0)

#define EXPECT_EQ_SIZE(actual, expected_size, label) do {                      \
    if ((actual).size() != (expected_size)) {                                  \
        std::printf("[FAIL] %s: size %zu != expected %zu\n", label,            \
                    (actual).size(), size_t(expected_size));                   \
        ++g_fail;                                                              \
    } else {                                                                   \
        std::printf("[ OK ] %s  (%zu bytes)\n", label, (actual).size());       \
    }                                                                          \
} while (0)

std::vector<std::uint8_t> hex_to_bytes(std::string_view h) {
    std::vector<std::uint8_t> out;
    out.reserve(h.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i + 1 < h.size(); i += 2) {
        int hi = nibble(h[i]);
        int lo = nibble(h[i + 1]);
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

// -----------------------------------------------------------------------------
// HMAC-SHA-256 (RFC 4231 §4.2 test cases 1 + 2)
// -----------------------------------------------------------------------------
//   Test case 1: key = 0x0b (20 times), data = "Hi There"
//        HMAC = b0344c61d8db38535ca8afceaf0bf12b
//               881dc200c9833da726e9376c2e32cff7
//   Test case 2: key = "Jefe", data = "what do ya want for nothing?"
//        HMAC = 5bdcc146bf60754e6a042426089575c7
//               5a003f089d2739839dec58b964ec3843
// -----------------------------------------------------------------------------
int check_hmac_kat() {
    int fails_local = 0;

    // Test case 1
    {
        std::vector<std::uint8_t> key(20, 0x0b);
        std::string hi = "Hi There";
        std::vector<std::uint8_t> data(hi.begin(), hi.end());
        auto mac = dtls_hmac_sha256(key, data);
        EXPECT_EQ_SIZE(mac, 32, "hmac.rfc4231.tc1.size");
        EXPECT_EQ_HEX(mac,
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
            "hmac.rfc4231.tc1");
    }

    // Test case 2
    {
        std::string j = "Jefe";
        std::vector<std::uint8_t> key(j.begin(), j.end());
        std::string q = "what do ya want for nothing?";
        std::vector<std::uint8_t> data(q.begin(), q.end());
        auto mac = dtls_hmac_sha256(key, data);
        EXPECT_EQ_SIZE(mac, 32, "hmac.rfc4231.tc2.size");
        EXPECT_EQ_HEX(mac,
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
            "hmac.rfc4231.tc2");
    }
    (void)fails_local;
    return 0;
}

// -----------------------------------------------------------------------------
// TLS 1.2 PRF (P_SHA-256) — locked against the build/gen_kat.py reference.
// -----------------------------------------------------------------------------
//   KAT vector 1 ("master_secret"): secret=32x00, label="master secret",
//                                  seed=64x00, len=48
//   KAT vector 2 ("key_block"):     secret=32x00, label="key expansion",
//                                  seed=64x00, len=40
//   KAT vector 3 ("verify_data_sf"): secret=32x00, label="server finished",
//                                    seed=64x00, len=12
//   KAT vector 4 ("verify_data_nontriv"): secret=32xAA, label="client finished",
//                                        seed=32x01||32x02, len=12
//   KAT vector 5 ("100 bytes"): secret=48x11, label="server finished",
//                               seed=100x22, len=100   (multi-chained P_hash)
// -----------------------------------------------------------------------------
int check_prf_kat() {
    auto zeros32 = hex_to_bytes("00000000000000000000000000000000"
                                "00000000000000000000000000000000");
    auto zeros64 = hex_to_bytes(std::string(128, '0'));
    auto aa32    = hex_to_bytes(std::string(64, 'a'));
    auto elevens48 = hex_to_bytes(std::string(96, '1'));
    auto twos100   = hex_to_bytes(std::string(200, '2'));

    // 1. master_secret derivation (48 bytes)
    {
        auto out = tls12_prf(zeros32, "master secret", zeros64, 48);
        EXPECT_EQ_SIZE(out, 48, "prf.master_secret.size");
        EXPECT_EQ_HEX(out,
            "49cfaee55b8692d3bb6dd6ee6b536f2f17afbc8418094763bcb5bed6b005adf"
            "888d060e48c5eb2266c73cb1a3d2d4b68",
            "prf.master_secret");
    }

    // 2. key_block (40 bytes)
    {
        auto out = tls12_prf(zeros32, "key expansion", zeros64, 40);
        EXPECT_EQ_SIZE(out, 40, "prf.key_block.size");
        EXPECT_EQ_HEX(out,
            "53b5dbc854207d752942787542e4ebebf5fa3efd1ad6fa6d5dc104481daea509"
            "a9fc881d4d3bc14a",
            "prf.key_block");
    }

    // 3. verify_data — "server finished" / 12 bytes
    {
        auto out = tls12_prf(zeros32, "server finished", zeros64, 12);
        EXPECT_EQ_SIZE(out, 12, "prf.verify_data.sf.size");
        EXPECT_EQ_HEX(out, "7bfba8b6eee3bab24dc014d3", "prf.verify_data.sf");
    }

    // 4. verify_data with non-trivial inputs (label, seed) to exercise
    //    secret + label + seed wiring beyond the zero-vector lock-down.
    //    seed = 32 bytes of 0x01 || 32 bytes of 0x02  (true RFC-style 64-byte seed).
    {
        std::vector<std::uint8_t> seed_01_02;
        seed_01_02.reserve(64);
        for (int i = 0; i < 32; ++i) seed_01_02.push_back(0x01);
        for (int i = 0; i < 32; ++i) seed_01_02.push_back(0x02);
        auto out = tls12_prf(aa32, "client finished", seed_01_02, 12);
        EXPECT_EQ_SIZE(out, 12, "prf.verify_data.nontriv.size");
        EXPECT_EQ_HEX(out, "f0ec9f5dc944acd0a00c7e35", "prf.verify_data.nontriv");
    }

    // 5. P_hash that spans multiple HMAC iterations (100 bytes > 32).
    //    Catches any bug where the loop doesn't increment A(i).
    {
        auto out = tls12_prf(elevens48, "server finished", twos100, 100);
        EXPECT_EQ_SIZE(out, 100, "prf.multi_block.size");
        EXPECT_EQ_HEX(out,
            "2dc04a3f4f8090b635e0629898f936d4491ae1d2e981433d0967bd9b61a5e94b"
            "c97e52d078d8cd95cfff1b7a55d5ba732f21c46c80e88b7544e540805f49d14a"
            "1b14a235d5204f6421970c82ad19546738dd66609185c8bbcc89b9b04ddc350"
            "d1f13d8a7",
            "prf.multi_block");
    }

    // 6. tls_prf_p_sha256 with two seed chunks — covers the 4-arg path
    //    with a non-trivial seed1 (32x01) and seed2 (32x02) so we don't
    //    accidentally match vector 2's input shape.
    {
        std::vector<std::uint8_t> seed1, seed2;
        seed1.reserve(32); seed2.reserve(32);
        for (int i = 0; i < 32; ++i) seed1.push_back(0x01);
        for (int i = 0; i < 32; ++i) seed2.push_back(0x02);
        auto out = tls_prf_p_sha256(zeros32,
                                    hex_to_bytes("6b657920657870616e73696f6e"),  // "key expansion"
                                    seed1, seed2, 40);
        EXPECT_EQ_SIZE(out, 40, "prf.p_sha256.4arg.size");
        EXPECT_EQ_HEX(out,
            "a860143d3dd9ff38f112037fbed57b2d166068a4f95393223ddf7a49341dc24e"
            "63e7d1e90533ec91",
            "prf.p_sha256.4arg");
    }

    // 7. out_len == 0 returns empty (defensive).
    {
        auto out = tls12_prf(zeros32, "master secret", zeros64, 0);
        if (!out.empty()) {
            std::printf("[FAIL] prf.outlen0: expected empty, got %zu bytes\n", out.size());
            ++g_fail;
        } else {
            std::printf("[ OK ] prf.outlen0\n");
        }
    }
    return 0;
}

}  // namespace

int main() {
    std::printf("=== NimRTC TLS 1.2 PRF Known-Answer Test ===\n");
    check_hmac_kat();
    check_prf_kat();
    if (g_fail == 0) {
        std::printf("PASS: all KAT vectors match the Python reference\n");
        return 0;
    }
    std::printf("FAIL: %d KAT vector(s) mismatch\n", g_fail);
    return 1;
}
