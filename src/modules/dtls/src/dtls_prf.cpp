// SPDX-License-Identifier: MIT
//
// @file src/modules/dtls/src/dtls_prf.cpp
// @brief TLS 1.2 PRF (P_SHA-256, RFC 5246 §5) + BCrypt-backed HMAC-SHA-256.
//
// Extracted from `dtls.cpp` so the KAT test (tests/test_dtls_prf_kat.cpp)
// can drive the PRF directly without dragging the rest of the DTLS state
// machine in.  The only BCrypt primitive used here is HMAC-SHA-256; AES /
// ECDH / ECDSA still live in `dtls.cpp`.
//
#include <nimrtc/dtls/dtls_prf.hpp>

#include <vector>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
#  define NIMRTC_HAS_BCRYPT 1
#else
#  define NIMRTC_HAS_BCRYPT 0
#endif

namespace nimrtc::dtls {

#if NIMRTC_HAS_BCRYPT

// ---------------------------------------------------------------------------
// BCrypt HMAC-SHA-256 (lifted verbatim from dtls.cpp so this TU is standalone)
// ---------------------------------------------------------------------------

namespace {
struct BcryptAlg {
    BCRYPT_ALG_HANDLE h = nullptr;
    ~BcryptAlg() { if (h) ::BCryptCloseAlgorithmProvider(h, 0); }
};
} // namespace

std::vector<std::uint8_t>
dtls_hmac_sha256(std::span<const std::uint8_t> key,
                 std::span<const std::uint8_t> data) noexcept {
    BcryptAlg alg;
    NTSTATUS s = ::BCryptOpenAlgorithmProvider(
        &alg.h, BCRYPT_SHA256_ALGORITHM,
        MS_PRIMITIVE_PROVIDER, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(s)) return {};

    // Query the actual hash object size required by the provider rather
    // than hardcoding 64.  Hardcoded sizes were too small for HMAC-SHA-256
    // on Windows; BCryptCreateHash would fail with STATUS_INVALID_PARAMETER,
    // which our P_SHA256 expansion loop silently turned into an infinite
    // spin because the chained HMACs returned empty vectors.
    DWORD hash_obj_size = 0, result_size = 0;
    s = ::BCryptGetProperty(alg.h, BCRYPT_OBJECT_LENGTH,
                            reinterpret_cast<PUCHAR>(&hash_obj_size),
                            sizeof(hash_obj_size), &result_size, 0);
    if (!BCRYPT_SUCCESS(s) || hash_obj_size == 0) return {};

    BCRYPT_HASH_HANDLE h = nullptr;
    std::vector<unsigned char> hash_obj(hash_obj_size);
    s = ::BCryptCreateHash(
        alg.h, &h, hash_obj.data(), hash_obj_size,
        const_cast<PUCHAR>(reinterpret_cast<const unsigned char*>(key.data())),
        static_cast<ULONG>(key.size()), 0);
    if (!BCRYPT_SUCCESS(s)) return {};

    if (!data.empty()) {
        s = ::BCryptHashData(
            h,
            const_cast<PUCHAR>(reinterpret_cast<const unsigned char*>(data.data())),
            static_cast<ULONG>(data.size()), 0);
        if (!BCRYPT_SUCCESS(s)) {
            ::BCryptDestroyHash(h);
            return {};
        }
    }
    std::vector<std::uint8_t> out(32);
    s = ::BCryptFinishHash(h, out.data(), 32, 0);
    ::BCryptDestroyHash(h);
    if (!BCRYPT_SUCCESS(s)) return {};
    return out;
}

#else  // !NIMRTC_HAS_BCRYPT
// ---------------------------------------------------------------------------
// Portable stub — non-Windows builds get a deterministic zero-string PRF so
// the rest of the handshake fails fast and visibly rather than silently
// propagating wrong keys.  We do NOT implement a software SHA-256 here on
// purpose: this path is only used by the stub DtlsSession on platforms
// without BCrypt and we don't want to maintain a second crypto stack.
// ---------------------------------------------------------------------------
std::vector<std::uint8_t>
dtls_hmac_sha256(std::span<const std::uint8_t> /*key*/,
                 std::span<const std::uint8_t> /*data*/) noexcept {
    return {};
}
#endif  // NIMRTC_HAS_BCRYPT

// ---------------------------------------------------------------------------
// PRF (RFC 5246 §5)
// ---------------------------------------------------------------------------
//
//   P_hash(secret, seed) = A(1) || A(2) || ...
//     where A(0) = seed
//           A(i) = HMAC(secret, A(i-1))
//   PRF(secret, label, seed) = P_hash(secret, label || seed)
//
// We implement P_hash by re-feeding (label || seed) for the first HMAC and
// (A(i-1) || label || seed) for each subsequent one — this preserves the
// exact chaining from RFC 5246 §5.
//
std::vector<std::uint8_t>
tls_prf_p_sha256(std::span<const std::uint8_t> secret,
                 std::span<const std::uint8_t> label,
                 std::span<const std::uint8_t> seed1,
                 std::span<const std::uint8_t> seed2,
                 std::size_t out_len) noexcept {
    if (out_len == 0) return {};
    // Build label || seed1 || seed2 once; reused both for A(1) and for the
    // chaining HMAC inputs (RFC 5246 §5: P_hash in = A(i-1) || label || seed).
    std::vector<std::uint8_t> a_in;
    a_in.reserve(label.size() + seed1.size() + seed2.size());
    a_in.insert(a_in.end(), label.begin(),  label.end());
    a_in.insert(a_in.end(), seed1.begin(),  seed1.end());
    a_in.insert(a_in.end(), seed2.begin(),  seed2.end());

    // A(1) = HMAC(secret, a_in)
    std::vector<std::uint8_t> a = dtls_hmac_sha256(secret, a_in);
    if (a.empty()) return {};

    std::vector<std::uint8_t> p;
    p.reserve(out_len);
    while (p.size() < out_len) {
        // next chunk = HMAC(secret, A(i) || a_in)
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

std::vector<std::uint8_t>
tls12_prf(std::span<const std::uint8_t> secret,
          std::string_view label,
          std::span<const std::uint8_t> seed,
          std::size_t out_len) noexcept {
    std::vector<std::uint8_t> lab(label.begin(), label.end());
    return tls_prf_p_sha256(secret, lab, seed, {}, out_len);
}

} // namespace nimrtc::dtls
