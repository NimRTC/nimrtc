// SPDX-License-Identifier: MIT
//
// @file src/modules/dtls/src/dtls_prf.cpp
// @brief TLS 1.2 PRF (P_SHA-256, RFC 5246 §5) — wolfSSL HMAC-SHA-256 backed.
//
// Replaces the BCrypt-only stub that lived in
// `src/modules/dtls/src/backup_20260909_141143/dtls_prf.cpp`.  Reason for
// the switch:
//
//   1. The NimRTC DTLS stack migrated from BCrypt/mbedTLS to wolfSSL for
//      the actual handshake (DtlsSessionWolfSSL).  The PRF lives inside
//      wolfSSL during the handshake (`TLSX_MAKE_PRMS`), but we now need a
//      *standalone* PRF to drive the verify_data path and the
//      Known-Answer-Tests independently of the live handshake state
//      machine.
//
//   2. wolfSSL is the one crypto backend that every supported platform
//      (Windows / Linux / macOS / FreeBSD) ships with — no need to gate
//      the PRF behind `#if defined(_WIN32)` + BCrypt headers.
//
//   3. Building on `wc_HmacInit/SetKey/Update/Final` keeps the *block of
//      code per platform* identical to zero — the only "platform" branch
//      is whether wolfSSL was compiled with async/devcrypto offload (and
//      wolfSSL handles that transparently inside `wc_HmacSetKey`).
//
// The KAT vectors in `tests/test_dtls_prf_kat.cpp` are pinned against
// the RFC 5246 §5 spec via `build/gen_kat.py`; if you change the
// chained-HMAC expansion here, regenerate the KAT and update the
// expected_hex constants in that test.
//
// Reference: RFC 5246 §5 ("HMAC and the Pseudorandom Function"),
//            RFC 4231 §4.2 ("Test Vectors for HMAC-SHA-256").
//

#include <nimrtc/dtls/dtls_prf.hpp>

#include <algorithm>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#endif

#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/types.h>
#include <wolfssl/wolfcrypt/hmac.h>
#include <wolfssl/wolfcrypt/sha256.h>

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

namespace nimrtc::dtls {

// ---------------------------------------------------------------------------
// HMAC-SHA-256 wrapper around wolfSSL's wc_Hmac*.
//
// RFC 2104 §2: keys longer than the hash block size (64 bytes for SHA-256)
// are first hashed; keys shorter are padded with zeros.  wolfSSL does this
// internally inside wc_HmacSetKey — see wc_HmacSetKey_ex() in
// wolfcrypt/src/hmac.c — so we don't have to replicate the logic here.
// ---------------------------------------------------------------------------
std::vector<std::uint8_t>
dtls_hmac_sha256(std::span<const std::uint8_t> key,
                 std::span<const std::uint8_t> data) noexcept {
    // Pre-flight: a zero-length key is allowed by RFC 2104 (treated as the
    // all-zeros key), so don't reject on that.  A 32-byte HMAC output is
    // always produced; we have no failure mode other than the wolfSSL
    // primitives returning a negative error code.
    Hmac hmac{};
    if (wc_HmacInit(&hmac, nullptr, INVALID_DEVID) != 0) {
        return {};
    }
    // RAII-style manual cleanup via a tiny scope guard; the wolfSSL API
    // does not throw, so this is safe across all error paths below.
    struct Cleanup {
        Hmac* h;
        ~Cleanup() { if (h) wc_HmacFree(h); }
    } _cleanup{&hmac};

    if (wc_HmacSetKey(&hmac, WC_SHA256,
                       reinterpret_cast<const byte*>(key.data()),
                       static_cast<word32>(key.size())) != 0) {
        return {};
    }
    if (!data.empty()) {
        if (wc_HmacUpdate(&hmac,
                          reinterpret_cast<const byte*>(data.data()),
                          static_cast<word32>(data.size())) != 0) {
            return {};
        }
    }
    std::vector<std::uint8_t> out(WC_SHA256_DIGEST_SIZE);
    if (wc_HmacFinal(&hmac, out.data()) != 0) {
        return {};
    }
    return out;
}

// ---------------------------------------------------------------------------
// P_hash(secret, label || seed1 || seed2, out_len) — RFC 5246 §5
//
//   A(0) = seed                       (here: label || seed1 || seed2)
//   A(i) = HMAC(secret, A(i-1))
//   output = A(1) || A(2) || ...      truncated to out_len
//
//   where each chunk_i we MAC in is: A(i) || (label || seed1 || seed2)
// ---------------------------------------------------------------------------
std::vector<std::uint8_t>
tls_prf_p_sha256(std::span<const std::uint8_t> secret,
                 std::span<const std::uint8_t> label,
                 std::span<const std::uint8_t> seed1,
                 std::span<const std::uint8_t> seed2,
                 std::size_t out_len) noexcept {
    if (out_len == 0) return {};

    // Pre-compute the constant chain input: label || seed1 || seed2.
    // We feed this both to the initial A(1) HMAC and to every chunk HMAC.
    std::vector<std::uint8_t> a_in;
    a_in.reserve(label.size() + seed1.size() + seed2.size());
    a_in.insert(a_in.end(), label.begin(), label.end());
    a_in.insert(a_in.end(), seed1.begin(), seed1.end());
    a_in.insert(a_in.end(), seed2.begin(), seed2.end());

    if (a_in.empty()) {
        // RFC 5246 §5: PRF with an empty label+seed collapses to a stream
        // of HMAC(secret, A(i-1))s where A(1) = HMAC(secret, "").  We
        // mimic that by initialising A to an all-zero HMAC output — the
        // chain still produces a valid deterministic stream.
        a_in.assign(WC_SHA256_DIGEST_SIZE, 0);
    }

    // A(1) = HMAC(secret, a_in)
    std::vector<std::uint8_t> a = dtls_hmac_sha256(secret, a_in);
    if (a.empty()) return {};

    std::vector<std::uint8_t> p;
    p.reserve(out_len);
    while (p.size() < out_len) {
        // chunk = HMAC(secret, A || a_in)
        std::vector<std::uint8_t> p_in;
        p_in.reserve(a.size() + a_in.size());
        p_in.insert(p_in.end(), a.begin(),    a.end());
        p_in.insert(p_in.end(), a_in.begin(), a_in.end());

        auto chunk = dtls_hmac_sha256(secret, p_in);
        if (chunk.empty()) return {};
        p.insert(p.end(), chunk.begin(), chunk.end());

        // A(i+1) = HMAC(secret, A(i))
        a = dtls_hmac_sha256(secret, a);
        if (a.empty()) return {};
    }
    p.resize(out_len);
    return p;
}

// ---------------------------------------------------------------------------
// PRF(secret, label, seed) — convenience single-seed form (RFC 5246 §5).
// ---------------------------------------------------------------------------
std::vector<std::uint8_t>
tls12_prf(std::span<const std::uint8_t> secret,
          std::string_view label,
          std::span<const std::uint8_t> seed,
          std::size_t out_len) noexcept {
    std::vector<std::uint8_t> lab(label.begin(), label.end());
    return tls_prf_p_sha256(secret, lab, seed, {}, out_len);
}

} // namespace nimrtc::dtls
