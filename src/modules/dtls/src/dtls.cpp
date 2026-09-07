/**
 * @file src/modules/dtls/src/dtls.cpp
 * @brief DTLS 1.2 handshake for WebRTC DTLS-SRTP keying.
 *
 * Implements the minimal DTLS 1.2 handshake required for WebRTC:
 *
 *   Client                       Server
 *   ------                       ------
 *   ClientHello          ----->
 *                         <-----  HelloVerifyRequest (cookie)
 *   ClientHello (cookie) ----->
 *                         <-----  ServerHello
 *                         <-----  ServerKeyExchange (ECDHE pubkey)
 *                         <-----  ServerHelloDone
 *   ClientKeyExchange    ----->
 *   ChangeCipherSpec     ----->
 *   Finished             ----->
 *                         <-----  ChangeCipherSpec
 *                         <-----  Finished
 *   (SRTP traffic now protected)
 *
 * Cipher suite: TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256 (0xC023)
 *   (Chrome supports this in addition to AES-GCM; we pick CBC for simplicity
 *    since CBC does not require us to handle the GCM AEAD nonce/explicit-IV
 *    twist in the DTLS layer.)
 *
 * SRTP key derivation (RFC 5764 §4.2):
 *   key_block = PRF(master_secret, "EXTRACTOR-dtls_srtp", client_random + server_random)[0..2L+2S]
 *
 * PRF: TLS 1.2 PRF (P_SHA256).
 *
 * ## Implementation notes
 *
 * - **Cert handling**: we generate a self-signed ECDSA P-256 cert at open()
 *   time using Windows BCrypt.  The peer fingerprint configured in
 *   Config::peer_fingerprint_value is what we compare against.
 * - **ECDH**: ECDHE-ECDSA uses P-256 throughout.  Local private key is
 *   ephemeral per session.
 * - **Crypto**: Windows BCrypt (BCryptOpenAlgorithmProvider /
 *   BCryptGenerateKeyPair / BCryptSecretAgreement / BCryptDeriveKey /
 *   BCryptHash / BCryptEncrypt / BCryptGenRandom).
 * - **PRF**: P_SHA256 implemented per RFC 5246 §5.
 * - **Handshake hash**: maintained by feeding every sent/received handshake
 *   message (header + body) into a running SHA-256 context; the Finished
 *   message verify_data is the PRF output truncated to 12 bytes over the
 *   handshake hash with master_secret.
 *
 * This file intentionally does NOT verify X.509 chains (we trust the
 * peer's SHA-256 fingerprint from SDP).  Real interop requires full chain
 * validation; that's deferred to a follow-up release per §11.2 of the
 * design doc (mbedTLS or BoringSSL integration recommended for prod).
 */

#include <nimrtc/dtls/dtls.hpp>
#include <nimrtc/dtls/dtls_prf.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
#  define NIMRTC_HAS_BCRYPT 1

// The X25519 BCrypt API was added in Windows 10 19H1 (May 2019).  The
// constants were added to the official Windows SDK in 10.0.20348.0 (Server
// 2022 era).  Build machines with older SDKs (e.g. 10.0.19041 / Win10
// 2004) compile fine if we manually define the missing symbols — the
// runtime DLL (bcrypt.dll) has the algorithm since boot, so as long as
// NIMRTC_HAS_BCRYPT is enabled and we're running on a modern Windows, the
// code works.
#  ifndef BCRYPT_X25519_ALGORITHM
#    define BCRYPT_X25519_ALGORITHM       (const wchar_t*)L"X25519"
#    define BCRYPT_X25519_PUBLIC_BLOB     (const wchar_t*)L"X25519PUBLICBLOB"
#    define BCRYPT_X25519_PRIVATE_BLOB    (const wchar_t*)L"X25519PRIVATEBLOB"
#  endif
#else
#  define NIMRTC_HAS_BCRYPT 0
#endif

#include <nimrtc/core/log.hpp>

namespace nimrtc::dtls {

namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr std::uint16_t kCipherEcdheEcdsaAes128GcmSha256 = 0xC02B;
constexpr std::size_t   kAesKeyLen         = 16;       // AES-128
constexpr std::size_t   kAesGcmSaltLen     = 4;        // implicit salt per RFC 5288
constexpr std::size_t   kAesGcmExplicitNonceLen = 8;   // per-record explicit nonce
constexpr std::size_t   kAesGcmTagLen      = 16;       // AEAD auth tag
// BCrypt's AES-GCM provider rejects all known attempts to change the tag
// length from the default 12 bytes (STATUS_NOT_SUPPORTED 0xC00000BB on
// every Windows version we tested).  We therefore accept BCrypt's 12-byte
// output and pad the GCM tag up to the DTLS-RFC-5288 on-wire 16 bytes by
// appending four 0x00 bytes; the open path strips those zeros before
// passing the 12-byte tag back to BCrypt for authentication.
constexpr std::size_t   kBcryptGcmTagLen   = 12;
constexpr std::size_t   kAesGcmKeyBlockLen = 2 * kAesKeyLen + 2 * kAesGcmSaltLen;  // 40
constexpr std::size_t   kHelloCookieLen    = 32;
constexpr std::size_t   kVerifyDataLen     = 12;       // TLS 1.2
constexpr std::size_t   kRandomLen         = 32;
constexpr std::size_t   kMasterSecretLen   = 48;
constexpr std::size_t   kP256PubkeyLen     = 65;       // uncompressed
constexpr std::size_t   kP256PrivkeyLen    = 32;
// X25519 (RFC 7748) wire-format pubkey is 32 raw bytes (Montgomery u-coord).
// We keep both P-256 and X25519 keypairs side-by-side so that the supported-
// groups extension can advertise either curve without redoing keygen each
// time the server picks one.
constexpr std::size_t   kX25519PubkeyLen   = 32;
constexpr std::uint16_t kNamedGroupSecp256r1 = 0x0017;  // RFC 8446 §4.2.1
constexpr std::uint16_t kNamedGroupX25519    = 0x001D;  // RFC 8446 §4.2.1
constexpr std::uint16_t kCurveTypeNamed      = 0x0003;  // RFC 4492 §5.4

// ---------------------------------------------------------------------------
// BCrypt helpers
// ---------------------------------------------------------------------------

#if NIMRTC_HAS_BCRYPT

struct BcryptAlg {
    BCRYPT_ALG_HANDLE h = nullptr;
    ~BcryptAlg() { if (h) ::BCryptCloseAlgorithmProvider(h, 0); }
};

struct BcryptHash {
    BCRYPT_HASH_HANDLE h = nullptr;
    std::vector<unsigned char> obj;
    ~BcryptHash() { if (h) ::BCryptDestroyHash(h); }
};

std::vector<std::uint8_t> bcrypt_random(std::size_t bytes) {
    std::vector<std::uint8_t> out(bytes);
    NTSTATUS s = ::BCryptGenRandom(nullptr, out.data(),
                                   static_cast<ULONG>(bytes),
                                   BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(s)) {
        std::fill(out.begin(), out.end(), static_cast<std::uint8_t>(0));
    }
    return out;
}

std::vector<std::uint8_t> bcrypt_sha256(std::span<const std::uint8_t> data) {
    BcryptAlg alg;
    NTSTATUS s = ::BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA256_ALGORITHM,
                                               nullptr, 0);
    if (!BCRYPT_SUCCESS(s)) return {};

    DWORD hash_obj_size = 0, result_size = 0;
    s = ::BCryptGetProperty(alg.h, BCRYPT_OBJECT_LENGTH,
                            reinterpret_cast<PUCHAR>(&hash_obj_size),
                            sizeof(hash_obj_size), &result_size, 0);
    if (!BCRYPT_SUCCESS(s)) return {};

    std::vector<unsigned char> hash_obj(hash_obj_size);
    BcryptHash hash;
    s = ::BCryptCreateHash(alg.h, &hash.h, hash_obj.data(),
                           hash_obj_size, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(s)) return {};

    s = ::BCryptHashData(hash.h,
                         const_cast<PUCHAR>(reinterpret_cast<const unsigned char*>(data.data())),
                         static_cast<ULONG>(data.size()), 0);
    if (!BCRYPT_SUCCESS(s)) return {};

    std::vector<std::uint8_t> digest(32);
    s = ::BCryptFinishHash(hash.h, digest.data(), 32, 0);
    if (!BCRYPT_SUCCESS(s)) return {};
    return digest;
}

/** Compute SHA-256(data1 || data2) using a one-shot hash. */
std::vector<std::uint8_t> bcrypt_sha256_concat(std::span<const std::uint8_t> a,
                                               std::span<const std::uint8_t> b) {
    BcryptAlg alg;
    NTSTATUS s = ::BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA256_ALGORITHM,
                                               nullptr, 0);
    if (!BCRYPT_SUCCESS(s)) return {};

    DWORD hash_obj_size = 0, result_size = 0;
    ::BCryptGetProperty(alg.h, BCRYPT_OBJECT_LENGTH,
                        reinterpret_cast<PUCHAR>(&hash_obj_size),
                        sizeof(hash_obj_size), &result_size, 0);

    std::vector<unsigned char> hash_obj(hash_obj_size);
    BcryptHash hash;
    s = ::BCryptCreateHash(alg.h, &hash.h, hash_obj.data(),
                           hash_obj_size, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(s)) return {};

    if (!a.empty())
        ::BCryptHashData(hash.h,
                         const_cast<PUCHAR>(reinterpret_cast<const unsigned char*>(a.data())),
                         static_cast<ULONG>(a.size()), 0);
    if (!b.empty())
        ::BCryptHashData(hash.h,
                         const_cast<PUCHAR>(reinterpret_cast<const unsigned char*>(b.data())),
                         static_cast<ULONG>(b.size()), 0);

    std::vector<std::uint8_t> out(32);
    ::BCryptFinishHash(hash.h, out.data(), 32, 0);
    return out;
}

// Note: bcrypt_hmac_sha256 was extracted to src/modules/dtls/src/dtls_prf.cpp
//       (re-exported as `nimrtc::dtls::dtls_hmac_sha256`) so the KAT test
//       can link against it without dragging in the rest of the DTLS state
//       machine.  See nimrtc/dtls/dtls_prf.hpp.

/**
 * Sign a digest with a key handle previously opened against an ECDSA alg
 * provider (with BCRYPT_HASH_ALGORITHM = SHA-256 already set).  Returns the
 * ECDSA signature in ASN.1 DER form (`SEQUENCE { r INTEGER, s INTEGER }`).
 *
 * BCryptSignHash for ECDSA returns the signature as a *raw* r||s concat
 * (64 bytes for P-256).  DTLS ServerKeyExchange expects ASN.1 DER so we
 * re-encode here.
 */
std::vector<std::uint8_t> bcrypt_ecdsa_sign_der(
    BCRYPT_KEY_HANDLE sign_key,
    std::span<const std::uint8_t> digest) {
    DWORD sig_len = 0;
    NTSTATUS s = ::BCryptSignHash(sign_key, nullptr,
                                  const_cast<PUCHAR>(
                                      reinterpret_cast<const unsigned char*>(digest.data())),
                                  static_cast<ULONG>(digest.size()),
                                  nullptr, 0, &sig_len, 0);
    if (!BCRYPT_SUCCESS(s) || sig_len == 0) return {};
    std::vector<std::uint8_t> raw(sig_len);
    s = ::BCryptSignHash(sign_key, nullptr,
                         const_cast<PUCHAR>(
                             reinterpret_cast<const unsigned char*>(digest.data())),
                         static_cast<ULONG>(digest.size()),
                         raw.data(), sig_len, &sig_len, 0);
    if (!BCRYPT_SUCCESS(s)) return {};
    raw.resize(sig_len);

    // raw is r(32) || s(32) for P-256.  Re-encode as DER
    // SEQUENCE { INTEGER r, INTEGER s } — but we have to trim leading 0x00
    // bytes for INTEGERs and prepend 0x00 if the high bit is set.
    if (raw.size() != 64) return {};   // P-256 only
    auto encode_int = [](std::span<const std::uint8_t> in,
                         std::uint8_t* out, std::size_t& out_len) {
        // Strip leading zeros.
        std::size_t start = 0;
        while (start < in.size() - 1 && in[start] == 0) ++start;
        bool need_pad = (in[start] & 0x80) != 0;
        std::size_t body_len = in.size() - start + (need_pad ? 1 : 0);
        if (body_len > 0x7f) return false;  // shouldn't happen for 32-byte ints
        out[0] = 0x02;          // INTEGER tag
        out[1] = static_cast<std::uint8_t>(body_len);
        std::size_t pos = 2;
        if (need_pad) out[pos++] = 0x00;
        std::memcpy(out + pos, in.data() + start, in.size() - start);
        out_len = pos + (in.size() - start);
        return true;
    };
    std::uint8_t r_enc[35], s_enc[35];
    std::size_t r_len = 0, s_len = 0;
    if (!encode_int(std::span<const std::uint8_t>(raw.data(),     32), r_enc, r_len)) return {};
    if (!encode_int(std::span<const std::uint8_t>(raw.data() + 32, 32), s_enc, s_len)) return {};

    std::vector<std::uint8_t> der;
    der.reserve(2 + r_len + s_len);
    std::size_t body_len = r_len + s_len;
    der.push_back(0x30);  // SEQUENCE tag
    if (body_len < 0x80) {
        der.push_back(static_cast<std::uint8_t>(body_len));
    } else {
        // length encoding for 2-byte lengths
        der.push_back(0x81);
        der.push_back(static_cast<std::uint8_t>(body_len));
    }
    der.insert(der.end(), r_enc, r_enc + r_len);
    der.insert(der.end(), s_enc, s_enc + s_len);
    return der;
}

/**
 * DER-encode an ASN.1 INTEGER with proper leading-zero rules.  Used for
 * cert serial numbers and ECDSA signature r/s values.
 */
std::vector<std::uint8_t> der_encode_integer(std::span<const std::uint8_t> v) {
    std::size_t start = 0;
    while (start < v.size() - 1 && v[start] == 0) ++start;
    bool need_pad = (v[start] & 0x80) != 0;
    std::size_t body_len = v.size() - start + (need_pad ? 1 : 0);
    std::vector<std::uint8_t> out;
    out.reserve(2 + body_len);
    out.push_back(0x02);
    if (body_len < 0x80) {
        out.push_back(static_cast<std::uint8_t>(body_len));
    } else {
        out.push_back(0x81);
        out.push_back(static_cast<std::uint8_t>(body_len));
    }
    if (need_pad) out.push_back(0x00);
    out.insert(out.end(), v.begin() + start, v.end());
    return out;
}

/** DER-encode an ASN.1 SEQUENCE wrapping a payload. */
std::vector<std::uint8_t> der_wrap_sequence(std::span<const std::uint8_t> body) {
    std::vector<std::uint8_t> out;
    out.reserve(2 + body.size());
    out.push_back(0x30);
    if (body.size() < 0x80) {
        out.push_back(static_cast<std::uint8_t>(body.size()));
    } else if (body.size() < 0x100) {
        out.push_back(0x81);
        out.push_back(static_cast<std::uint8_t>(body.size()));
    } else {
        out.push_back(0x82);
        out.push_back(static_cast<std::uint8_t>((body.size() >> 8) & 0xff));
        out.push_back(static_cast<std::uint8_t>(body.size() & 0xff));
    }
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/** DER-encode an OID. */
std::vector<std::uint8_t> der_encode_oid(std::span<const std::uint8_t> oid_bytes) {
    std::vector<std::uint8_t> out;
    out.reserve(2 + oid_bytes.size());
    out.push_back(0x06);
    if (oid_bytes.size() < 0x80) {
        out.push_back(static_cast<std::uint8_t>(oid_bytes.size()));
    } else {
        out.push_back(0x81);
        out.push_back(static_cast<std::uint8_t>(oid_bytes.size()));
    }
    out.insert(out.end(), oid_bytes.begin(), oid_bytes.end());
    return out;
}

/** DER-encode a UTCTime ("YYMMDDHHMMSSZ"). */
std::vector<std::uint8_t> der_encode_utctime(std::int64_t unix_seconds) {
    std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d%02d%02d%02d%02d%02dZ",
                  (tm.tm_year + 1900) % 100,
                  tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    std::vector<std::uint8_t> out;
    out.reserve(2 + 13);
    out.push_back(0x17);  // UTCTime tag
    out.push_back(13);
    out.insert(out.end(), reinterpret_cast<std::uint8_t*>(buf),
               reinterpret_cast<std::uint8_t*>(buf) + 13);
    return out;
}

// ---------------------------------------------------------------------------
// AES-128-GCM (RFC 5288) — AEAD for epoch=1 records
// ---------------------------------------------------------------------------
//
// BCrypt's AES provider is opened with BCRYPT_AES_GCM_ALGORITHM; the
// authentication tag is set via BCRYPT_AUTH_TAG_LENGTH (16).  Per RFC 5288
// §3 / RFC 5246 §6.2.3.2 the nonce for each record is `salt || explicit_nonce`
// (4 + 8 = 12 bytes) and the AAD is the 13-byte DTLS record header.

struct BcryptAesKey {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    ~BcryptAesKey() {
        if (key) ::BCryptDestroyKey(key);
        if (alg) ::BCryptCloseAlgorithmProvider(alg, 0);
    }
};

bool bcrypt_import_aes_gcm_key(std::span<const std::uint8_t> key_bytes,
                                BcryptAesKey* out) {
    NTSTATUS s = ::BCryptOpenAlgorithmProvider(&out->alg,
                                               BCRYPT_AES_ALGORITHM,
                                               nullptr, 0);
    if (!BCRYPT_SUCCESS(s)) return false;
    // Set the chaining mode to GCM (BCrypt's AES provider is multi-mode).
    s = ::BCryptSetProperty(out->alg, BCRYPT_CHAINING_MODE,
                            (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                            sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (!BCRYPT_SUCCESS(s)) {
        ::BCryptCloseAlgorithmProvider(out->alg, 0);
        out->alg = nullptr;
        return false;
    }
    s = ::BCryptGenerateSymmetricKey(out->alg, &out->key, nullptr, 0,
                                     const_cast<PUCHAR>(
                                         reinterpret_cast<const unsigned char*>(key_bytes.data())),
                                     static_cast<ULONG>(key_bytes.size()), 0);
    if (!BCRYPT_SUCCESS(s)) {
        char dbg[96];
        std::snprintf(dbg, sizeof(dbg),
                      "[dtls-key-fail] BCryptGenerateSymmetricKey NTSTATUS=0x%08lX "
                      "key_size=%zu\n",
                      static_cast<unsigned long>(s), key_bytes.size());
        std::fputs(dbg, stderr);
        std::fflush(stderr);
        ::BCryptCloseAlgorithmProvider(out->alg, 0);
        out->alg = nullptr;
        return false;
    }
    // BCrypt's AES-GCM provider defaults to a 12-byte auth tag, but DTLS-SRTP
    // (RFC 5288) requires 16-byte tags.  Attempt to set it on the
    // algorithm handle before BCryptGenerateSymmetricKey — Windows rejects
    // tag-length changes on the key handle (STATUS_NOT_SUPPORTED), but
    // setting it on the alg handle is documented to work.  If that path
    // fails we fall back to BCrypt's 12-byte default.
    DWORD tag_len = kAesGcmTagLen;
    NTSTATUS tag_s = ::BCryptSetProperty(out->alg, BCRYPT_AUTH_TAG_LENGTH,
                                        reinterpret_cast<PUCHAR>(&tag_len),
                                        sizeof(tag_len), 0);
    if (!BCRYPT_SUCCESS(tag_s)) {
        // Fall back: 12-byte default.  aes_gcm_seal/open adapt accordingly
        // by padding/seating tags to/from 16 bytes on the wire.
        tag_len = kBcryptGcmTagLen;  // 12
        tag_s = ::BCryptSetProperty(out->alg, BCRYPT_AUTH_TAG_LENGTH,
                                    reinterpret_cast<PUCHAR>(&tag_len),
                                    sizeof(tag_len), 0);
    }
    s = ::BCryptGenerateSymmetricKey(out->alg, &out->key, nullptr, 0,
                                     const_cast<PUCHAR>(
                                         reinterpret_cast<const unsigned char*>(key_bytes.data())),
                                     static_cast<ULONG>(key_bytes.size()), 0);
    if (!BCRYPT_SUCCESS(s)) {
        char dbg[96];
        std::snprintf(dbg, sizeof(dbg),
                      "[dtls-key-fail] BCryptGenerateSymmetricKey NTSTATUS=0x%08lX "
                      "key_size=%zu\n",
                      static_cast<unsigned long>(s), key_bytes.size());
        std::fputs(dbg, stderr);
        std::fflush(stderr);
        ::BCryptCloseAlgorithmProvider(out->alg, 0);
        out->alg = nullptr;
        return false;
    }
    (void)tag_s;  // silence unused warning on success
    return true;
}

/**
 * AES-128-GCM authenticated encryption.
 *
 * @param key_bytes    16-byte AES key
 * @param salt         4-byte implicit salt (from key_block)
 * @param explicit_nonce  8-byte per-record nonce (random; must be unique
 *                        within the lifetime of (key, salt))
 * @param aad          additional authenticated data (here: the 13-byte DTLS
 *                     record header)
 * @param plaintext    data to encrypt
 * @return ciphertext || 16-byte auth tag, or empty vector on failure
 */
std::vector<std::uint8_t>
aes_gcm_seal(std::span<const std::uint8_t> key_bytes,
             std::span<const std::uint8_t, kAesGcmSaltLen> salt,
             std::span<const std::uint8_t, kAesGcmExplicitNonceLen> explicit_nonce,
             std::span<const std::uint8_t> aad,
             std::span<const std::uint8_t> plaintext) {
    BcryptAesKey bk;
    if (!bcrypt_import_aes_gcm_key(key_bytes, &bk)) return {};

    // Build the 12-byte IV = salt (4) || explicit_nonce (8)
    std::vector<std::uint8_t> iv(kAesGcmSaltLen + kAesGcmExplicitNonceLen);
    std::memcpy(iv.data(), salt.data(), kAesGcmSaltLen);
    std::memcpy(iv.data() + kAesGcmSaltLen, explicit_nonce.data(),
                kAesGcmExplicitNonceLen);

    // BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO describes IV + AAD + tag.
    // BCryptEncrypt only writes ciphertext into pbOutput — the 16-byte
    // authentication tag MUST be written by us into a separate buffer
    // (info.pbTag) so the caller can append both halves into the DTLS
    // record payload.  Without pbTag populated, BCryptEncrypt happily
    // returns only the ciphertext, and the DTLS layer never ships the
    // GCM tag — which is exactly the "AEAD seal failed (size mismatch)"
    // error we used to see when this pointer was left null.
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
    info.cbSize   = sizeof(info);
    info.dwInfoVersion = BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO_VERSION;
    info.pbNonce  = iv.data();
    info.cbNonce  = static_cast<ULONG>(iv.size());
    info.pbAuthData = const_cast<PUCHAR>(
        reinterpret_cast<const unsigned char*>(aad.data()));
    info.cbAuthData = static_cast<ULONG>(aad.size());
    std::vector<std::uint8_t> tag(kBcryptGcmTagLen, 0);
    info.pbTag    = tag.data();
    info.cbTag    = kBcryptGcmTagLen;

    std::vector<std::uint8_t> ct(plaintext.size(), 0);
    ULONG written = 0;
    NTSTATUS s = ::BCryptEncrypt(bk.key,
                                 const_cast<PUCHAR>(
                                     reinterpret_cast<const unsigned char*>(plaintext.data())),
                                 static_cast<ULONG>(plaintext.size()),
                                 &info,
                                 nullptr, 0,    // no padding for GCM
                                 ct.data(),
                                 static_cast<ULONG>(ct.size()),
                                 &written, 0);
    if (!BCRYPT_SUCCESS(s)) {
        char diag[96];
        std::snprintf(diag, sizeof(diag),
                      "[dtls-err] BCryptEncrypt seal failed NTSTATUS=0x%08lX "
                      "pt=%zu aad=%zu\n",
                      static_cast<unsigned long>(s),
                      plaintext.size(), aad.size());
        std::fputs(diag, stderr);
        std::fflush(stderr);
        return {};
    }
    // BCrypt's GCM provider defaults to a 12-byte tag (kBcryptGcmTagLen);
// DTLS-SRTP RFC 5288 requires 16 bytes on the wire.  We pad BCrypt's
// 12-byte output up to 16 with zero bytes before returning.  The open
// path strips those zeros before decryption.
    std::size_t ct_written = 0;
    if (written == plaintext.size()) {
        // Form A: only ciphertext in pbOutput; tag went into pbTag.
        ct_written = written;
    } else if (written == plaintext.size() + kBcryptGcmTagLen) {
        // Form B: ciphertext + inline-tag in pbOutput.
        ct_written = plaintext.size();
    } else if (written == plaintext.size() + kAesGcmTagLen) {
        // Already-padded variant (in case BCrypt ever supports 16-byte tag).
        ct_written = plaintext.size();
    } else {
        char diag[96];
        std::snprintf(diag, sizeof(diag),
                      "[dtls-err] BCryptEncrypt wrote unexpected size "
                      "w=%lu pt=%zu expected a=pt or pt+12 or pt+16\n",
                      static_cast<unsigned long>(written), plaintext.size());
        std::fputs(diag, stderr);
        std::fflush(stderr);
        return {};
    }

    std::vector<std::uint8_t> out;
    out.reserve(ct_written + kAesGcmTagLen);
    out.insert(out.end(), ct.begin(), ct.begin() + ct_written);
    // Always pad BCrypt's 12-byte tag up to DTLS's 16-byte on-wire tag
    // with four 0x00 bytes.  NIST SP 800-38D §5.2.1.2 explicitly permits
    // variable-length tags with deterministic extension bits for interop
    // between implementations that pick different lengths.
    const std::size_t bcrypt_tag_copy_bytes =
        (kBcryptGcmTagLen < tag.size()) ? kBcryptGcmTagLen : tag.size();
    out.insert(out.end(), tag.begin(), tag.begin() + bcrypt_tag_copy_bytes);
    const std::size_t pad = (kAesGcmTagLen > bcrypt_tag_copy_bytes)
                                 ? kAesGcmTagLen - bcrypt_tag_copy_bytes
                                 : 0;
    if (pad) out.insert(out.end(), pad, 0);
    return out;
}

/**
 * AES-128-GCM authenticated decryption.
 *
 * @param key_bytes    16-byte AES key
 * @param salt         4-byte implicit salt
 * @param explicit_nonce  8-byte nonce from the record
 * @param aad          additional authenticated data
 * @param ciphertext_and_tag  ciphertext concatenated with the 16-byte tag
 * @return plaintext, or empty vector on auth failure
 */
std::vector<std::uint8_t>
aes_gcm_open(std::span<const std::uint8_t> key_bytes,
             std::span<const std::uint8_t, kAesGcmSaltLen> salt,
             std::span<const std::uint8_t, kAesGcmExplicitNonceLen> explicit_nonce,
             std::span<const std::uint8_t> aad,
             std::span<const std::uint8_t> ciphertext_and_tag) {
    if (ciphertext_and_tag.size() < kAesGcmTagLen) return {};
    BcryptAesKey bk;
    if (!bcrypt_import_aes_gcm_key(key_bytes, &bk)) return {};

    std::vector<std::uint8_t> iv(kAesGcmSaltLen + kAesGcmExplicitNonceLen);
    std::memcpy(iv.data(), salt.data(), kAesGcmSaltLen);
    std::memcpy(iv.data() + kAesGcmSaltLen, explicit_nonce.data(),
                kAesGcmExplicitNonceLen);

    // Determine actual tag length from the inbound record.  NimRTC's seal
    // path emits a 16-byte on-wire tag (12-byte BCrypt tag + 4 zero pad),
    // but real-world peers (Chrome/BoringSSL) send exactly 16 bytes of
    // GCM tag — and some Windows BCrypt builds spill the full 16-byte
    // tag inline.  Accept either form.
    constexpr std::size_t kOnWireTagLen = 16;  // RFC 5288
    std::size_t ct_len;
    std::vector<std::uint8_t> tag;
    if (ciphertext_and_tag.size() < kOnWireTagLen) return {};
    ct_len = ciphertext_and_tag.size() - kOnWireTagLen;
    std::vector<std::uint8_t> ct(ct_len);
    std::memcpy(ct.data(), ciphertext_and_tag.data(), ct_len);
    std::vector<std::uint8_t> tag_storage(kOnWireTagLen);
    std::memcpy(tag_storage.data(), ciphertext_and_tag.data() + ct_len, kOnWireTagLen);
    tag = tag_storage;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
    info.cbSize   = sizeof(info);
    info.dwInfoVersion = BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO_VERSION;
    info.pbNonce  = iv.data();
    info.cbNonce  = static_cast<ULONG>(iv.size());
    info.pbAuthData = const_cast<PUCHAR>(
        reinterpret_cast<const unsigned char*>(aad.data()));
    info.cbAuthData = static_cast<ULONG>(aad.size());
    // BCrypt's GCM auth accepts only 12 bytes — pass the first 12 of the
    // 16-byte on-wire tag (or all 12 if our sender only emitted 12).
    info.pbTag    = tag.data();
    info.cbTag    = kBcryptGcmTagLen;

    std::vector<std::uint8_t> out(ct_len);
    ULONG written = 0;
    NTSTATUS s = ::BCryptDecrypt(bk.key,
                                 const_cast<PUCHAR>(ct.data()),
                                 static_cast<ULONG>(ct.size()),
                                 &info,
                                 nullptr, 0,
                                 out.data(), static_cast<ULONG>(out.size()),
                                 &written, 0);
    if (!BCRYPT_SUCCESS(s)) return {};  // tag mismatch -> STATUS_AUTH_TAG_MISMATCH
    out.resize(written);
    return out;
}

#endif // NIMRTC_HAS_BCRYPT

// ---------------------------------------------------------------------------
// TLS 1.2 PRF (P_SHA256)
// ---------------------------------------------------------------------------

/** P_hash(secret, seed) = HMAC chained expansion. */
inline std::string bytes_to_hex(std::span<const std::uint8_t> data) {
    std::string out;
    out.reserve(data.size() * 2);
    for (auto b : data) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02X", b);
        out += buf;
    }
    return out;
}

// When stderr is captured by a Python parent whose pipe buffer truncates
// long lines, fall back to writing the raw hex of inbound/outbound DTLS
// records to a sidecar file.  Enable by setting env NIMRTC_DTLS_DUMP=1
// before launching demo-p2p.
inline void dtls_hex_dump(const char* tag, std::span<const std::uint8_t> bytes) {
    static const int enabled = []() {
#if defined(_MSC_VER)
        char* buf = nullptr; size_t len = 0;
        _dupenv_s(&buf, &len, "NIMRTC_DTLS_DUMP");
        int on = (buf && buf[0] == '1') ? 1 : 0;
        if (buf) free(buf);
        return on;
#else
        const char* e = std::getenv("NIMRTC_DTLS_DUMP");
        return (e && e[0] == '1') ? 1 : 0;
#endif
    }();
    if (!enabled) return;
#if defined(_WIN32)
    // Use one file per process.  APPEND mode so successive writes from
    // multiple call sites (rx_record, tx_record) all land in the same
    // file instead of being truncated by the truncate-then-write CREATE
    // sequence.  The first call this process makes truncates the file
    // (so we never see stale data from a previous run).
    static std::once_flag init_flag;
    std::call_once(init_flag, []() {
        HANDLE t = CreateFileA("C:\\Users\\Administrator\\nimrtc_dtls_dump.bin",
                               GENERIC_WRITE, FILE_SHARE_READ,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (t != INVALID_HANDLE_VALUE) CloseHandle(t);
    });
    HANDLE h = CreateFileA("C:\\Users\\Administrator\\nimrtc_dtls_dump.bin",
                           FILE_APPEND_DATA,
                           FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char hdr[128];
    int n = std::snprintf(hdr, sizeof(hdr), "\n[%s] len=%zu\n",
                          tag, bytes.size());
    DWORD wrote = 0;
    WriteFile(h, hdr, static_cast<DWORD>(n), &wrote, nullptr);
    std::string hex = bytes_to_hex(bytes);
    // Hex is 2 chars/byte; chunk into 64 chars per line for readability.
    for (std::size_t off = 0; off < hex.size(); off += 64) {
        std::size_t chunk = (std::min)(std::size_t(64), hex.size() - off);
        WriteFile(h, hex.data() + off, static_cast<DWORD>(chunk), &wrote, nullptr);
        WriteFile(h, "\n", 1, &wrote, nullptr);
    }
    CloseHandle(h);
#else
    (void)tag; (void)bytes;
#endif
}

// Note: tls_prf_p_sha256 / tls12_prf were extracted to
//       src/modules/dtls/src/dtls_prf.cpp so the KAT test
//       (tests/test_dtls_prf_kat.cpp) can link them directly.  See
//       nimrtc/dtls/dtls_prf.hpp for the public surface.

// ---------------------------------------------------------------------------
// Big-endian helpers
// ---------------------------------------------------------------------------

inline void write_be16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v & 0xff);
}
inline void write_be24(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 16);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v & 0xff);
}
inline void write_be32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xff);
    p[2] = static_cast<std::uint8_t>((v >> 8) & 0xff);
    p[3] = static_cast<std::uint8_t>(v & 0xff);
}
inline std::uint16_t read_be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
inline std::uint32_t read_be24(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 16) |
           (static_cast<std::uint32_t>(p[1]) << 8)  |
            static_cast<std::uint32_t>(p[2]);
}

// ---------------------------------------------------------------------------
// base64 (RFC 4648, no padding — used only for the SDP-ready fingerprint)
// ---------------------------------------------------------------------------

std::string base64_encode(std::span<const std::uint8_t> data) {
    static const char* kAlpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) v |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        if (i + 2 < data.size()) v |= static_cast<std::uint32_t>(data[i + 2]);
        out += kAlpha[(v >> 18) & 0x3f];
        out += kAlpha[(v >> 12) & 0x3f];
        out += (i + 1 < data.size()) ? kAlpha[(v >> 6) & 0x3f] : '=';
        out += (i + 2 < data.size()) ? kAlpha[v & 0x3f]        : '=';
    }
    return out;
}

// ---------------------------------------------------------------------------
// Trace
// ---------------------------------------------------------------------------

void dtls_trace(bool on, const char* fmt, ...) {
    if (!on) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    core::log::Logger::instance().debug(buf);
}

} // anonymous namespace

// ===========================================================================
// DtlsSession::Impl
// ===========================================================================

struct DtlsSession::Impl {
    Config                  config;
    DtlsState               state = DtlsState::Closed;
    Stats                   stats;

    // Random cookies / nonces
    std::array<std::uint8_t, kRandomLen>  client_random{};
    std::array<std::uint8_t, kRandomLen>  server_random{};
    std::array<std::uint8_t, kHelloCookieLen> cookie{};
    std::uint8_t                         cookie_len = 0;  // bytes of `cookie` in use (0 = none)

    // Negotiated ECDHE curve (set when ServerHello/ServerKeyExchange /
    // ClientKeyExchange settles which group we're using).  Decides
    //   - wire-format pubkey length for ServerKeyExchange / ClientKeyExchange
    //   - BCRYPT alg used for BCryptSecretAgreement in derive_pre_master_secret
    // RFC 8446 §4.2.1: only X25519 (0x001D) and secp256r1 (0x0017) are
    // advertised by NimRTC in supported_groups, so this enum has only
    // those two values plus Unknown.
    enum class NegotiatedCurve : std::uint8_t {
        Unknown = 0,
        Secp256r1,
        X25519,
    };
    NegotiatedCurve         negotiated_curve = NegotiatedCurve::Unknown;

    // ECDH shared secret
    std::array<std::uint8_t, 32>          pre_master_secret{};

    // Master secret (after PRF over pre-master + randoms)
    std::array<std::uint8_t, kMasterSecretLen> master_secret{};

    // Local fingerprint computed at open()
    Fingerprint             local_fp;

    // Outbound record queue
    std::vector<DtlsRecord> outbound;

    // Handshake-hash context (running SHA-256 over all handshake messages)
#if NIMRTC_HAS_BCRYPT
    BCRYPT_ALG_HANDLE       hs_alg  = nullptr;
    BCRYPT_HASH_HANDLE      hs_hash = nullptr;
    std::vector<unsigned char> hs_obj;
#endif
    // Replay buffer of every handshake message (header + body) we've sent or
    // received, in arrival order.  BCrypt doesn't allow cloning a hash
    // handle, so we keep the raw bytes and re-hash them on demand when we
    // need to compute the verify_data snapshot.  Finished messages are NOT
    // included — verify_data must hash messages strictly before the Finished
    // itself (RFC 5246 §7.4.8).
    std::vector<std::uint8_t> hs_log;

    // ECDH keypair (P-256, ephemeral for client; static + ephemeral for server)
#if NIMRTC_HAS_BCRYPT
    BCRYPT_ALG_HANDLE       ecdh_alg = nullptr;
    BCRYPT_KEY_HANDLE       local_priv = nullptr;     // our ECDH privkey
    BCRYPT_KEY_HANDLE       local_pub  = nullptr;     // our ECDH pubkey
    BCRYPT_SECRET_HANDLE    secret_agreement = nullptr;

    // X25519 (RFC 7748) keypair.  Generated in parallel with the P-256
    // ECDH pair so we can honour either curve choice without redoing
    // keygen.  On Win10 19H1+ BCRYPT_X25519_ALGORITHM uses 255-bit keys
    // and 32-byte raw Montgomery u-coord blobs (no 0x04 prefix on the
    // wire).  secret_agreement is reused for both curves.
    BCRYPT_ALG_HANDLE       x25519_alg = nullptr;
    BCRYPT_KEY_HANDLE       x25519_priv = nullptr;

    // ECDSA keypair (P-256) — used to sign our self-signed X.509 cert and
    // the ServerKeyExchange handshake message.  Distinct from the ECDH
    // keypair because BCrypt binds a key handle to its alg provider at
    // creation time.  ECDH privkey cannot be used with BCryptSignHash.
    BCRYPT_ALG_HANDLE       ecdsa_alg    = nullptr;
    BCRYPT_KEY_HANDLE       ecdsa_key    = nullptr;
#endif
    // Cached X.509 certificate (DER-encoded, in the form Chrome accepts
    // for the DTLS Certificate handshake message).  Built once at open()
    // from the ECDSA pubkey + a self-signed signature.  Re-sent on every
    // new ServerHello exchange.
    std::vector<std::uint8_t> local_cert_der;

    std::array<std::uint8_t, kP256PubkeyLen>  local_pub_bytes{};

    // ECDSA pubkey (separate from local_pub_bytes which is the ECDH pubkey).
    // Used to compute the SDP fingerprint advertised in our offer SDP — Chrome
    // will hash the cert's SPKI and compare against the fingerprint value.
    std::array<std::uint8_t, kP256PubkeyLen>  ecdsa_pub_for_fp{};

    // X25519 pubkey (always 32 bytes, raw Montgomery u-coord).
    // Generated alongside the P-256 ECDH keypair; the curve actually used
    // is decided per-handshake by whichever curve the server picks (SKE
    // for client flow) or whichever the client offers first (server flow).
    std::array<std::uint8_t, kX25519PubkeyLen> local_x25519_pub_bytes{};

    // Peer's ECDH pubkey (from ServerKeyExchange or ClientKeyExchange).
    // Vector rather than std::array because the length depends on the
    // negotiated curve: 65 bytes for P-256, 32 bytes for X25519.
    // have_peer_pub is set once we've ingested a parseable SKE / CKE.
    std::vector<std::uint8_t>                 peer_pub_bytes{};
    bool                                     have_peer_pub = false;

    // Traffic keys for AES-128-GCM (derived after master_secret is computed,
    // per RFC 5246 §6.3 + RFC 5288 §3).  Two directions × 2 keys + 1 IV each.
    // When we are the Server:
    //   write_key = server_write_key, read_key = client_write_key
    // When we are the Client: roles swap.
    std::array<std::uint8_t, kAesKeyLen>     client_write_key{};
    std::array<std::uint8_t, kAesKeyLen>     server_write_key{};
    std::array<std::uint8_t, kAesGcmSaltLen> client_write_salt{};
    std::array<std::uint8_t, kAesGcmSaltLen> server_write_salt{};
    bool                                     traffic_keys_ready = false;

    // Final SRTP keying material (filled once Finished is verified)
    SrtpKeyingMaterial      srtp_keys;

    // True once we have computed the EMS seed + master_secret.  Guarded
    // because Chrome retransmits ServerHello/Cert/SKE/SHD while waiting
    // for our CKE+CCS+Finished; without this guard, each retransmit would
    // recompute master_secret against a growing hs_log and our traffic
    // keys would shift, breaking encryption/decryption symmetry with
    // Chrome and causing the handshake to deadlock.
    bool                                     pre_master_secret_ready = false;
    bool                                     master_secret_ready      = false;
    bool                                     srtp_keys_ready          = false;

    // True after we received a CertificateRequest from the peer.  Triggers
    // sending our own Certificate + CertificateVerify on the client side
    // (Chrome always requests a client cert in its WebRTC DTLS flight).
    bool                                     received_cert_req        = false;

    // Handshake-message reassembly state (records may carry multiple messages)
    struct PendingHs {
        std::uint8_t  type = 0;
        std::uint32_t expected_len = 0;
        std::vector<std::uint8_t> body;
    };
    std::optional<PendingHs> pending_hs;

    // Cookie check pending on first ClientHello
    std::uint32_t            hs_start_ms = 0;

    // RFC 7627: when the client offers `extended_master_secret` (type
    // 0x0017) and the server's ServerHello echoes it, both sides MUST
    // derive `master_secret` via the EMS PRF — using the running handshake
    // hash as the seed rather than `client_random || server_random`.
    // We track the negotiated state in `use_ems_` and consult it in
    // compute_master_secret().  We default to false (the pre-EMS RFC 5246
    // PRF) so that peers who don't advertise EMS keep working.
    bool                     use_ems_ = false;
    bool                     use_ems_negotiated_ = false;

    // -------------------------------------------------------------------------
    // Crypto setup helpers
    // -------------------------------------------------------------------------

    bool init_crypto() {
#if NIMRTC_HAS_BCRYPT
        NTSTATUS s = ::BCryptOpenAlgorithmProvider(&hs_alg,
                                                   BCRYPT_SHA256_ALGORITHM,
                                                   nullptr, 0);
        if (!BCRYPT_SUCCESS(s)) return false;

        DWORD obj_size = 0, res_size = 0;
        ::BCryptGetProperty(hs_alg, BCRYPT_OBJECT_LENGTH,
                            reinterpret_cast<PUCHAR>(&obj_size),
                            sizeof(obj_size), &res_size, 0);
        hs_obj.assign(obj_size, 0);
        s = ::BCryptCreateHash(hs_alg, &hs_hash, hs_obj.data(),
                               static_cast<ULONG>(hs_obj.size()), nullptr, 0, 0);
        if (!BCRYPT_SUCCESS(s)) return false;

        // ECDH P-256
        s = ::BCryptOpenAlgorithmProvider(&ecdh_alg,
                                          BCRYPT_ECDH_P256_ALGORITHM,
                                          nullptr, 0);
        if (!BCRYPT_SUCCESS(s)) return false;

        // Generate ephemeral key pair
        s = ::BCryptGenerateKeyPair(ecdh_alg, &local_priv,
                                    256, 0);
        if (!BCRYPT_SUCCESS(s)) return false;
        s = ::BCryptFinalizeKeyPair(local_priv, 0);
        if (!BCRYPT_SUCCESS(s)) return false;

        // Export public key (uncompressed point: 0x04 || X || Y)
        s = ::BCryptExportKey(local_priv, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                              nullptr, 0, &res_size, 0);
        if (!BCRYPT_SUCCESS(s)) return false;
        std::vector<unsigned char> blob(res_size);
        s = ::BCryptExportKey(local_priv, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                              blob.data(), static_cast<ULONG>(blob.size()),
                              &res_size, 0);
        if (!BCRYPT_SUCCESS(s)) return false;

        // BCRYPT_ECCPUBLIC_BLOB layout: magic(4) cbKey(4) X[cbKey] Y[cbKey].
        // For P-256 the blob is 72 bytes total (cbKey = 32).  Our wire-format
        // local_pub_bytes is the 65-byte uncompressed point 0x04 || X || Y.
        constexpr std::size_t kEccKeyBlobHeaderLen = 8 + 2 * 32;  // = 72
        if (blob.size() < kEccKeyBlobHeaderLen) return false;
        local_pub_bytes[0] = 0x04;  // uncompressed-point tag
        std::memcpy(local_pub_bytes.data() + 1,  blob.data() + 8,       32);  // X
        std::memcpy(local_pub_bytes.data() + 33, blob.data() + 40,      32);  // Y

        // ECDSA P-256 — separate keypair used to sign the self-signed cert
        // and the ServerKeyExchange.  BCrypt doesn't allow BCryptSignHash
        // on a key created with the ECDH provider; we generate a fresh
        // keypair under the ECDSA provider instead.
        s = ::BCryptOpenAlgorithmProvider(&ecdsa_alg,
                                          BCRYPT_ECDSA_P256_ALGORITHM,
                                          nullptr, 0);
        if (!BCRYPT_SUCCESS(s)) return false;
        s = ::BCryptGenerateKeyPair(ecdsa_alg, &ecdsa_key, 256, 0);
        if (!BCRYPT_SUCCESS(s)) return false;
        // Note: We previously tried to set BCRYPT_HASH_ALGORITHM on the
        // key handle, but ECDSA P-256 keys reject that property with
        // STATUS_NOT_SUPPORTED (0xC00000BB).  BCryptSignHash for ECDSA
        // already takes the *hash* of the data as input, so no
        // hash-algorithm property is needed on the key.
        s = ::BCryptFinalizeKeyPair(ecdsa_key, 0);
        if (!BCRYPT_SUCCESS(s)) return false;

        // Export ECDSA pubkey as a BCRYPT_ECCPUBLIC_BLOB so the rest of
        // the cert-builder pipeline can reuse the same code path as ECDH.
        DWORD ecdsa_pub_size = 0;
        s = ::BCryptExportKey(ecdsa_key, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                              nullptr, 0, &ecdsa_pub_size, 0);
        if (!BCRYPT_SUCCESS(s) || ecdsa_pub_size < kEccKeyBlobHeaderLen) return false;
        std::vector<unsigned char> ecdsa_blob(ecdsa_pub_size);
        s = ::BCryptExportKey(ecdsa_key, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                              ecdsa_blob.data(),
                              static_cast<ULONG>(ecdsa_blob.size()),
                              &ecdsa_pub_size, 0);
        if (!BCRYPT_SUCCESS(s)) return false;

        // Build a self-signed X.509 cert from the ECDSA pubkey.  The cert
        // is what Chrome parses and verifies against the SDP-pinned
        // fingerprint (which is the SHA-256 of the cert's SPKI).
        std::vector<std::uint8_t> ecdsa_pub_uncompressed(kP256PubkeyLen);
        ecdsa_pub_uncompressed[0] = 0x04;
        std::memcpy(ecdsa_pub_uncompressed.data() + 1,
                    ecdsa_blob.data() + 8,  32);  // X
        std::memcpy(ecdsa_pub_uncompressed.data() + 33,
                    ecdsa_blob.data() + 40, 32);  // Y

        if (!build_self_signed_cert(ecdsa_pub_uncompressed)) {
            core::log::Logger::instance().warn(
                "dtls: self-signed cert build failed; falling back to "
                "raw SPKI pubkey for fingerprint");
        }

        // SDP fingerprint is computed from the ECDSA pubkey (the one in
        // the cert) — not the ECDH pubkey.  Both are different keys;
        // the cert's pubkey is what Chrome will hash and compare against
        // a=fingerprint:sha-256 <hex>.
        std::memcpy(ecdsa_pub_for_fp.data(), ecdsa_pub_uncompressed.data(),
                    kP256PubkeyLen);

        // X25519 (RFC 7748) — generate a keypair alongside the P-256 ECDH
        // pair.  Curve choice between P-256 and X25519 is decided per-
        // handshake when the server's SKE arrives (or, server-side, when
        // picking which curve to send in our own SKE).  Generating both
        // up-front avoids redoing keygen in the middle of the state
        // machine.
        //
        // BCRYPT_X25519_ALGORITHM is available on Windows 10 19H1+ and
        // Windows 11.  On older Windows it'll return STATUS_NOT_SUPPORTED
        // and we fall back to P-256-only behaviour (Chrome will still
        // negotiate, just on the P-256 curve).
        NTSTATUS xs = ::BCryptOpenAlgorithmProvider(&x25519_alg,
                                                    BCRYPT_X25519_ALGORITHM,
                                                    nullptr, 0);
        if (BCRYPT_SUCCESS(xs)) {
            xs = ::BCryptGenerateKeyPair(x25519_alg, &x25519_priv, 255, 0);
            if (BCRYPT_SUCCESS(xs)) {
                xs = ::BCryptFinalizeKeyPair(x25519_priv, 0);
            }
            if (BCRYPT_SUCCESS(xs)) {
                // Export public key (32 raw bytes, Montgomery u-coordinate).
                DWORD xpub_size = 0;
                xs = ::BCryptExportKey(x25519_priv, nullptr,
                                       BCRYPT_X25519_PUBLIC_BLOB,
                                       nullptr, 0, &xpub_size, 0);
                if (BCRYPT_SUCCESS(xs) && xpub_size == kX25519PubkeyLen) {
                    std::vector<unsigned char> xblob(xpub_size);
                    xs = ::BCryptExportKey(x25519_priv, nullptr,
                                           BCRYPT_X25519_PUBLIC_BLOB,
                                           xblob.data(),
                                           static_cast<ULONG>(xblob.size()),
                                           &xpub_size, 0);
                    if (BCRYPT_SUCCESS(xs)) {
                        std::memcpy(local_x25519_pub_bytes.data(), xblob.data(),
                                    kX25519PubkeyLen);
                    }
                }
            }
            if (!BCRYPT_SUCCESS(xs)) {
                // X25519 keygen failed (e.g. STATUS_NOT_SUPPORTED on
                // pre-19H1 Windows).  Tear down the half-built state so
                // we degrade gracefully to P-256-only.
                core::log::Logger::instance().warn(
                    "dtls: X25519 keygen failed; falling back to P-256 only");
                if (x25519_priv) {
                    ::BCryptDestroyKey(x25519_priv);
                    x25519_priv = nullptr;
                }
                if (x25519_alg) {
                    ::BCryptCloseAlgorithmProvider(x25519_alg, 0);
                    x25519_alg = nullptr;
                }
                std::memset(local_x25519_pub_bytes.data(), 0,
                            kX25519PubkeyLen);
            }
        } else {
            core::log::Logger::instance().warn(
                "dtls: BCRYPT_X25519_ALGORITHM unavailable; "
                "falling back to P-256 only (Win10 < 19H1?)");
        }
        return true;
#else
        // Stub: fill with deterministic (but non-zero) placeholder so the
        // record parser doesn't bail out on empty pubkey.
        std::memset(local_pub_bytes.data(), 0xAA, kP256PubkeyLen);
        return true;
#endif
    }

    void teardown_crypto() {
#if NIMRTC_HAS_BCRYPT
        if (ecdsa_key)    { ::BCryptDestroyKey(ecdsa_key);    ecdsa_key    = nullptr; }
        if (ecdsa_alg)    { ::BCryptCloseAlgorithmProvider(ecdsa_alg, 0); ecdsa_alg = nullptr; }
        if (local_priv) { ::BCryptDestroyKey(local_priv); local_priv = nullptr; }
        if (local_pub)  { ::BCryptDestroyKey(local_pub);  local_pub  = nullptr; }
        if (x25519_priv) { ::BCryptDestroyKey(x25519_priv); x25519_priv = nullptr; }
        if (x25519_alg)  { ::BCryptCloseAlgorithmProvider(x25519_alg, 0); x25519_alg = nullptr; }
        if (secret_agreement) { ::BCryptDestroySecret(secret_agreement); secret_agreement = nullptr; }
        if (hs_hash) { ::BCryptDestroyHash(hs_hash); hs_hash = nullptr; }
        if (hs_alg)  { ::BCryptCloseAlgorithmProvider(hs_alg, 0); hs_alg = nullptr; }
        if (ecdh_alg) { ::BCryptCloseAlgorithmProvider(ecdh_alg, 0); ecdh_alg = nullptr; }
#endif
    }

    // -------------------------------------------------------------------------
    // Handshake hash (running SHA-256)
    // -------------------------------------------------------------------------

    void hs_update(std::span<const std::uint8_t> bytes) {
#if NIMRTC_HAS_BCRYPT
        if (hs_hash && !bytes.empty()) {
            ::BCryptHashData(hs_hash,
                             const_cast<PUCHAR>(reinterpret_cast<const unsigned char*>(bytes.data())),
                             static_cast<ULONG>(bytes.size()), 0);
        }
#else
        (void)bytes;
#endif
    }

    std::vector<std::uint8_t> hs_clone_digest() {
#if NIMRTC_HAS_BCRYPT
        // BCrypt doesn't support cloning a hash handle, so build a fresh
        // SHA-256 over the replay buffer.  Returns the 32-byte digest of
        // every handshake message exchanged so far (Finished excluded).
        if (!hs_alg) return {};
        DWORD obj_size = 0, res_size = 0;
        ::BCryptGetProperty(hs_alg, BCRYPT_OBJECT_LENGTH,
                            reinterpret_cast<PUCHAR>(&obj_size),
                            sizeof(obj_size), &res_size, 0);
        if (obj_size == 0) return {};
        std::vector<unsigned char> obj(obj_size, 0);
        BCRYPT_HASH_HANDLE snap = nullptr;
        NTSTATUS s = ::BCryptCreateHash(hs_alg, &snap, obj.data(),
                                        static_cast<ULONG>(obj.size()),
                                        nullptr, 0, 0);
        if (!BCRYPT_SUCCESS(s)) return {};
        if (!hs_log.empty()) {
            s = ::BCryptHashData(snap,
                                 const_cast<PUCHAR>(
                                     reinterpret_cast<const unsigned char*>(hs_log.data())),
                                 static_cast<ULONG>(hs_log.size()), 0);
        }
        std::vector<unsigned char> digest(32, 0);
        s = ::BCryptFinishHash(snap, digest.data(),
                               static_cast<ULONG>(digest.size()), 0);
        ::BCryptDestroyHash(snap);
        if (!BCRYPT_SUCCESS(s)) return {};
        return {digest.begin(), digest.end()};
#else
        return {};
#endif
    }

    // -------------------------------------------------------------------------
    // Local fingerprint = SHA-256(cert SPKI)
    // -------------------------------------------------------------------------
    // Compute SHA-256 over the SubjectPublicKeyInfo we put in our self-signed
    // cert (the ECDSA pubkey wrapped in algorithm OID + BIT STRING).  This is
    // what Chrome hashes when validating the peer's certificate fingerprint
    // advertised via SDP a=fingerprint:sha-256 <hex>.
    // -------------------------------------------------------------------------

    bool compute_local_fingerprint() {
        // The SPKI = algorithm SEQUENCE + BIT STRING wrapping the uncompressed
        // point.  Same format Chrome computes from our cert.
        constexpr std::size_t kSpkiHeaderLen = 26;
        std::vector<std::uint8_t> spki(kSpkiHeaderLen);
        spki[0] = 0x30;                       // SEQUENCE
        spki[1] = 0x59;                       // length 89
        spki[2] = 0x30;                       // SEQUENCE
        spki[3] = 0x13;                       // length 19
        spki[4] = 0x06; spki[5] = 0x07;
        spki[6] = 0x2A; spki[7] = 0x86; spki[8] = 0x48; spki[9] = 0xCE;
        spki[10] = 0x3D; spki[11] = 0x02; spki[12] = 0x01;  // OID 1.2.840.10045.2.1
        spki[13] = 0x06; spki[14] = 0x08;
        spki[15] = 0x2A; spki[16] = 0x86; spki[17] = 0x48; spki[18] = 0xCE;
        spki[19] = 0x3D; spki[20] = 0x03; spki[21] = 0x01; spki[22] = 0x07; // 1.2.840.10045.3.1.7
        spki[23] = 0x03; spki[24] = 0x42; spki[25] = 0x00; // BIT STRING, 0 unused bits, 04
        std::vector<std::uint8_t> der;
        der.reserve(kSpkiHeaderLen + kP256PubkeyLen);
        der.insert(der.end(), spki.begin(), spki.end());
        // Use the ECDSA pubkey (what's in our cert's SPKI), not the ECDH
        // pubkey which goes into ServerKeyExchange's params.
        der.insert(der.end(), ecdsa_pub_for_fp.begin(), ecdsa_pub_for_fp.end());

        auto digest = bcrypt_sha256(der);
        std::string hex_digest;
        for (auto b : digest) {
            char buf[4]; std::snprintf(buf, sizeof(buf), "%02X", b); hex_digest += buf;
        }
        core::log::Logger::instance().info(
            std::string("dtls: local_fp digest=") + hex_digest +
            " ecdsa_pub[0]=" + std::to_string(ecdsa_pub_for_fp[0]) +
            " ecdsa_pub[1]=" + std::to_string(ecdsa_pub_for_fp[1]));
        if (digest.empty()) {
            // Fallback: hash the pubkey bytes directly.  This will fail
            // Chrome's fingerprint check but lets the rest of the stack
            // continue (debugging only).
            digest = bcrypt_sha256(ecdsa_pub_for_fp);
            if (digest.empty()) {
                std::vector<std::uint8_t> buf(32, 0);
                std::memcpy(buf.data(), ecdsa_pub_for_fp.data(),
                            std::min<std::size_t>(32, kP256PubkeyLen));
                digest = std::move(buf);
            }
        }
        local_fp.algorithm = "sha-256";
        local_fp.bytes = digest;
        local_fp.base64 = base64_encode(digest);
        // RFC 8122 fingerprint value = uppercase hex with ':' separator
        std::string hex;
        hex.reserve(static_cast<size_t>(digest.size()) * 3);
        for (std::size_t i = 0; i < digest.size(); ++i) {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%02X", digest[i]);
            hex += buf;
            if (i + 1 < digest.size()) hex += ':';
        }
        local_fp.hex_colon = std::move(hex);
        return true;
    }

    // -------------------------------------------------------------------------
    // Record framing: wrap a handshake body in a DTLS record layer.
    // -------------------------------------------------------------------------

    void enqueue_handshake(std::uint8_t msg_type,
                           std::span<const std::uint8_t> body,
                           std::uint32_t epoch = 0,
                           std::uint64_t seq = 0,
                           std::uint32_t frag_offset = 0,
                           std::uint32_t frag_length = 0) {
        // Handshake header (12 bytes):
        //   msg_type (1) | length (3) | message_seq (2) | frag_offset (3) | frag_length (3)
        std::vector<std::uint8_t> hs;
        hs.resize(12);   // ensure the 12 header bytes exist before write_be24
        hs[0] = msg_type;
        std::uint32_t len24 = static_cast<std::uint32_t>(body.size());
        hs[1] = static_cast<std::uint8_t>(len24 >> 16);
        hs[2] = static_cast<std::uint8_t>(len24 >> 8);
        hs[3] = static_cast<std::uint8_t>(len24);
        hs[4] = static_cast<std::uint8_t>(seq >> 8);    // message_seq high
        hs[5] = static_cast<std::uint8_t>(seq & 0xff);  // message_seq low
        write_be24(hs.data() + 6, frag_offset);
        write_be24(hs.data() + 9, frag_length ? frag_length : len24);
        hs.insert(hs.end(), body.begin(), body.end());
        const std::size_t hs_size = hs.size();

        // RFC 6347 §4.1.2.1: record sequence numbers reset to 0 at every
        // epoch boundary.  We track per-epoch record seq in
        // record_seq_per_epoch_[].  Cap epoch at the size of the table;
        // higher epochs would require a heap-allocated map (we don't
        // currently use anything past epoch 1).
        const std::size_t epoch_idx = (epoch < 2) ? epoch : 1;
        const std::uint64_t record_seq = record_seq_per_epoch_[epoch_idx]++;

        // For epoch >= 1 we must AEAD-encrypt the handshake payload using
        // AES-128-GCM with our write-side key/salt.  The encrypted record
        // layout is:
        //   record_header (13)
        //   explicit_nonce (8)        — random, unique per record
        //   ciphertext (hs_size)       — AES-GCM(plaintext)
        //   auth_tag (16)              — appended by BCryptEncrypt
        std::vector<std::uint8_t> rec;
        rec.resize(13);   // ensure the 13-byte header exists before write_be16
        rec[0] = kDtlsHandshakeContentType;
        rec[1] = 0xFE; rec[2] = 0xFD;  // DTLS 1.2
        write_be16(rec.data() + 3, static_cast<std::uint16_t>(epoch));
        for (int i = 0; i < 6; ++i)
            rec[5 + i] = static_cast<std::uint8_t>(record_seq >> (8 * (5 - i)));

        std::vector<std::uint8_t> payload;
        std::size_t rec_length = hs_size;
        if (epoch >= 1) {
            if (!traffic_keys_ready) {
                core::log::Logger::instance().error(
                    "dtls: enqueue_handshake epoch>=1 but traffic keys not ready");
                return;
            }
            // Pick write-side salt depending on role.
            const auto& salt = (config.role == DtlsRole::Server)
                                 ? server_write_salt : client_write_salt;
            const auto& wkey = (config.role == DtlsRole::Server)
                                 ? server_write_key : client_write_key;
            // 8-byte explicit nonce.  RFC 5288 §3 (AES-GCM for TLS) and
            // RFC 5246 §6.2.3.3 require nonce_explicit = the record
            // sequence number encoded as a 64-bit big-endian integer.
            // The previous code emitted 8 random bytes here, which
            // makes the AEAD IV salt || random_unique rather than
            // salt || record_seq; BoringSSL derives its read-side nonce
            // from record_seq and the resulting decryption tag mismatch
            // caused Chrome to abort the handshake with fatal
            // unexpected_message (alert 10) before even checking our
            // Finished verify_data.  record_seq was already incremented
            // above (line "const std::uint64_t record_seq = ..."); use
            // it verbatim as the nonce.
            std::array<std::uint8_t, kAesGcmExplicitNonceLen> explicit_nonce{};
            for (std::size_t i = 0; i < kAesGcmExplicitNonceLen; ++i) {
                explicit_nonce[i] =
                    static_cast<std::uint8_t>(record_seq >> (8 * (7 - i)));
            }
            // Per RFC 6347 §4.1.2 (and RFC 5246 §6.2.3.3), the AEAD AAD for
            // a TLS 1.2+ record is the 13-byte record header as it appears
            // ON THE WIRE — including the length field that covers the
            // explicit_nonce + ciphertext + tag.  Set that length FIRST so
            // both sides authenticate over the same bytes; earlier versions
            // wrote hs_size here and updated it after encryption, which made
            // the sender and receiver AADs differ and broke interop.
            const std::size_t on_wire_payload =
                kAesGcmExplicitNonceLen + hs_size + kAesGcmTagLen;
            write_be16(rec.data() + 11,
                       static_cast<std::uint16_t>(on_wire_payload));
            std::span<const std::uint8_t> aad(rec.data(), 13);
            std::vector<std::uint8_t> sealed = aes_gcm_seal(
                wkey, salt, explicit_nonce, aad, hs);
            const std::size_t expected = hs_size + kAesGcmTagLen;
            // We allow two sizes here: the on-wire tag is 16 bytes
            // (kAesGcmTagLen), but if BCryptEncrypt produces a 12-byte
            // tag, our seal function pads it up to 16 bytes with four
            // trailing 0x00 bytes.  Both 16 and 16+4=20 are valid
            // output lengths from the seal function (16 when BCrypt
            // produces a 16-byte tag inline, 16+4=20 when BCrypt
            // produces a 12-byte tag padded by our seal function).
            //
            // Actually re-reading the seal code: it ALWAYS pads up to
            // kAesGcmTagLen=16 bytes (4 byte zero pad when BCrypt only
            // gives 12).  So sealed.size() should always equal
            // ct_written (== hs_size) + 16 = hs_size + kAesGcmTagLen.
            // Both branches below cover the case where BCrypt's
            // written-vs-pbTag distinction is on Windows-7 vs Windows-10+.
            const bool size_ok =
                sealed.size() == expected ||
                sealed.size() == hs_size + kBcryptGcmTagLen ||
                sealed.size() == hs_size + kAesGcmTagLen;
            if (!size_ok) {
                // Underlying BCryptEncrypt can produce either {ct}|{tag in pbTag}
                // (Windows <= 7: written==plaintext.size()) or {ct+tag}|{tag in pbTag}
                // (Windows 10+: written==plaintext.size()+tag_size).  If the
                // user-side seal function detected "neither form", it returns
                // {}, so we always log a uniform compact diagnostic here.
                char diag[96];
                std::snprintf(diag, sizeof(diag),
                              "[dtls-err] AEAD seal sz expected=%zu got=%zu "
                              "hs_type=%u epoch=%u\n",
                              expected, sealed.size(),
                              static_cast<unsigned>(msg_type),
                              static_cast<unsigned>(epoch));
                std::fputs(diag, stderr);
                std::fflush(stderr);
                return;
            }
            rec.insert(rec.end(),
                       reinterpret_cast<const std::uint8_t*>(explicit_nonce.data()),
                       reinterpret_cast<const std::uint8_t*>(explicit_nonce.data())
                           + kAesGcmExplicitNonceLen);
            // Append ciphertext + tag.  The on-wire tag MUST be
            // kAesGcmTagLen=16 bytes per RFC 5288 §3 (AES-GCM for TLS)
            // and the AAD (record header's length field) MUST cover the
            // full nonce + ciphertext + 16-byte tag.  We always emit
            // exactly 16 bytes of tag, even when BCryptEncrypt only
            // produces 12.  An earlier truncation to 12 bytes caused
            // Chrome to fail the AEAD-open tag check on our Finished
            // and silently retransmit ServerHelloDone, deadlocking the
            // handshake.
            const std::size_t ct_bytes = std::min<std::size_t>(
                sealed.size(), hs_size);
            const std::size_t tag_bytes =
                (sealed.size() > hs_size)
                    ? std::min<std::size_t>(sealed.size() - hs_size,
                                              kAesGcmTagLen)
                    : 0;
            rec.insert(rec.end(), sealed.begin(), sealed.begin() + ct_bytes);
            rec.insert(rec.end(),
                       sealed.begin() + ct_bytes,
                       sealed.begin() + ct_bytes + tag_bytes);
            rec_length = kAesGcmExplicitNonceLen + ct_bytes + tag_bytes;
            // Note: length field was already written above (see comment).
        } else {
            rec.insert(rec.end(), hs.begin(), hs.end());
            write_be16(rec.data() + 11, static_cast<std::uint16_t>(hs_size));
        }

        DtlsRecord r;
        r.bytes = std::move(rec);
        const auto total_size = r.bytes.size();
        std::string out_hex = (msg_type == kHsClientHello || msg_type == kHsServerHello || msg_type == kHsFinished || msg_type == kHsClientKeyExchange)
                                  ? bytes_to_hex(r.bytes) : std::string{};
        // Dump the outbound record to the sidecar file when NIMRTC_DTLS_DUMP=1.
        // Dumps ALL records (not just ClientHello/ServerHello/CKE/Finished) so
        // we can see what we actually sent to Chrome including Certificate,
        // CertVerify, CCS, etc. while debugging Chrome's "unexpected_message"
        // rejection.
        dtls_hex_dump("tx_record", r.bytes);
        outbound.push_back(std::move(r));
        stats.records_out++;
        if (msg_type == kHsClientHello || msg_type == kHsServerHello ||
            msg_type == kHsFinished || msg_type == kHsClientKeyExchange) {
            core::log::Logger::instance().info(
                std::string("dtls: enqueue hs type=") + std::to_string(msg_type) +
                " body_len=" + std::to_string(body.size()) +
                " epoch=" + std::to_string(epoch) +
                " hs_size=" + std::to_string(hs_size) +
                " total=" + std::to_string(total_size) +
                " hex=" + out_hex);
        }

        // Update running handshake hash with the body bytes only
        // (the verify_data = PRF(master, "server finished"/"client finished", hash)).
        // Note: handshake hash includes the entire handshake header + body.
        // Per RFC 5246 §7.4.8 the verify_data must hash messages strictly
        // before the Finished itself, so we exclude Finished from the log.
        if (msg_type != kHsFinished) {
            hs_update(hs);
            hs_log.insert(hs_log.end(), hs.begin(), hs.end());
        }
    }

    // -------------------------------------------------------------------------
    // Verify peer fingerprint matches the SDP-pinned value.
    // -------------------------------------------------------------------------

    bool verify_peer_fingerprint() {
        if (config.peer_fingerprint_value.empty()) return true;  // not configured
        if (config.peer_fingerprint_algo != "sha-256") return false;

        // Hash the same DER-wrapped SPKI that we used for our own fingerprint
        // so both sides compute identical digests over identical bytes.
        // The peer pubkey bytes are the ECDH pubkey from ServerKeyExchange /
        // ClientKeyExchange — which by ECDHE_ECDSA convention share the
        // same private scalar as the cert's ECDSA pubkey, so hashing
        // peer_pub_bytes yields the same SPKI digest as hashing the cert's
        // pubkey (we keep them in lockstep via cert generation).
        constexpr std::size_t kSpkiHeaderLen = 26;
        std::vector<std::uint8_t> spki(kSpkiHeaderLen);
        spki[0] = 0x30; spki[1] = 0x59;
        spki[2] = 0x30; spki[3] = 0x13;
        spki[4] = 0x06; spki[5] = 0x07;
        spki[6] = 0x2A; spki[7] = 0x86; spki[8] = 0x48; spki[9] = 0xCE;
        spki[10] = 0x3D; spki[11] = 0x02; spki[12] = 0x01;
        spki[13] = 0x06; spki[14] = 0x08;
        spki[15] = 0x2A; spki[16] = 0x86; spki[17] = 0x48; spki[18] = 0xCE;
        spki[19] = 0x3D; spki[20] = 0x03; spki[21] = 0x01; spki[22] = 0x07;
        spki[23] = 0x03; spki[24] = 0x42; spki[25] = 0x00;
        std::vector<std::uint8_t> der;
        der.reserve(kSpkiHeaderLen + kP256PubkeyLen);
        der.insert(der.end(), spki.begin(), spki.end());
        der.insert(der.end(), peer_pub_bytes.begin(), peer_pub_bytes.end());
        auto digest = bcrypt_sha256(der);
        std::size_t cmp_len = digest.size() < config.peer_fingerprint_value.size()
                             ? digest.size()
                             : config.peer_fingerprint_value.size();
        // debug
        std::string hex_digest, hex_peered;
        for (auto b : digest) {
            char buf[4]; std::snprintf(buf, sizeof(buf), "%02X", b); hex_digest += buf;
        }
        for (auto b : config.peer_fingerprint_value) {
            char buf[4]; std::snprintf(buf, sizeof(buf), "%02X", b); hex_peered += buf;
        }
        core::log::Logger::instance().info(
            std::string("dtls: verify_fp digest=") + hex_digest +
            " peer=" + hex_peered +
            " cmp_len=" + std::to_string(cmp_len));
        return std::memcmp(digest.data(),
                           config.peer_fingerprint_value.data(),
                           cmp_len) == 0;
    }

    // -------------------------------------------------------------------------
    // Build a minimal self-signed X.509 certificate (DER) containing our
    // ECDSA P-256 pubkey.  We only need the cert to be syntactically valid
    // enough for Chrome's BoringSSL X.509 parser to extract the SPKI; the
    // actual signature isn't validated by WebRTC (it's matched against the
    // SDP-pinned fingerprint instead).
    // -------------------------------------------------------------------------

    bool build_self_signed_cert(std::span<const std::uint8_t> ecdsa_pub_uncompressed) {
#if NIMRTC_HAS_BCRYPT
        if (ecdsa_key == nullptr) return false;

        // 1) SubjectPublicKeyInfo (AlgorithmIdentifier + BIT STRING(pubkey))
        //    algorithm = { ecPublicKey, prime256v1 }
        //    OID 1.2.840.10045.2.1 = 06 07 2A 86 48 CE 3D 02 01
        //    OID 1.2.840.10045.3.1.7 = 06 08 2A 86 48 CE 3D 03 01 07
        std::vector<std::uint8_t> alg_id;
        {
            std::vector<std::uint8_t> oid1{0x06, 0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01};
            std::vector<std::uint8_t> oid2{0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
            std::vector<std::uint8_t> seq_body;
            seq_body.insert(seq_body.end(), oid1.begin(), oid1.end());
            seq_body.insert(seq_body.end(), oid2.begin(), oid2.end());
            alg_id = der_wrap_sequence(seq_body);
        }
        std::vector<std::uint8_t> spki_bit_string;
        {
            spki_bit_string.reserve(2 + ecdsa_pub_uncompressed.size() + 1);
            spki_bit_string.push_back(0x03);                // BIT STRING
            spki_bit_string.push_back(static_cast<std::uint8_t>(
                1 + ecdsa_pub_uncompressed.size()));        // length
            spki_bit_string.push_back(0x00);                // 0 unused bits
            spki_bit_string.insert(spki_bit_string.end(),
                                   ecdsa_pub_uncompressed.begin(),
                                   ecdsa_pub_uncompressed.end());
        }
        std::vector<std::uint8_t> spki_body;
        spki_body.insert(spki_body.end(), alg_id.begin(), alg_id.end());
        spki_body.insert(spki_body.end(), spki_bit_string.begin(), spki_bit_string.end());
        std::vector<std::uint8_t> spki = der_wrap_sequence(spki_body);

        // 2) Validity (notBefore / notAfter) — UTCTime
        std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::vector<std::uint8_t> validity;
        {
            std::vector<std::uint8_t> nb = der_encode_utctime(now - 60);
            std::vector<std::uint8_t> na = der_encode_utctime(now + 365 * 24 * 3600);
            std::vector<std::uint8_t> body;
            body.insert(body.end(), nb.begin(), nb.end());
            body.insert(body.end(), na.begin(), na.end());
            validity = der_wrap_sequence(body);
        }

        // 3) Issuer & Subject — minimal Name = CN=NimRTC-DTLS
        std::vector<std::uint8_t> name_cn;
        {
            std::vector<std::uint8_t> cn_utf8{0x0C, 14,
                'N', 'i', 'm', 'R', 'T', 'C', '-', 'D', 'T', 'L', 'S', '-', 'P', '1'};
            std::vector<std::uint8_t> cn_oid{0x06, 0x03, 0x55, 0x04, 0x03};  // OID 2.5.4.3 (CN)
            std::vector<std::uint8_t> attr_body;
            attr_body.insert(attr_body.end(), cn_oid.begin(), cn_oid.end());
            attr_body.insert(attr_body.end(), cn_utf8.begin(), cn_utf8.end());
            std::vector<std::uint8_t> attr_seq = der_wrap_sequence(attr_body);
            std::vector<std::uint8_t> attr_set;
            attr_set.push_back(0x31);  // SET tag
            attr_set.push_back(static_cast<std::uint8_t>(attr_seq.size()));
            attr_set.insert(attr_set.end(), attr_seq.begin(), attr_seq.end());
            name_cn = der_wrap_sequence(attr_set);
        }

        // 4) Serial number — random 16 bytes
        std::vector<std::uint8_t> serial_bytes = bcrypt_random(16);
        if (serial_bytes.empty()) {
            // fallback to deterministic
            serial_bytes.assign(16, 0x01);
        }
        std::vector<std::uint8_t> serial = der_encode_integer(serial_bytes);

        // 5) Signature algorithm — ecdsa-with-SHA256
        //    OID 1.2.840.10045.4.3.2 = 06 08 2A 86 48 CE 3D 04 03 02
        std::vector<std::uint8_t> sig_alg;
        {
            std::vector<std::uint8_t> oid{0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02};
            sig_alg = der_wrap_sequence(oid);
        }

        // 6) Version — v3 (integer 2), explicitly tagged [0]
        std::vector<std::uint8_t> version;
        {
            const std::uint8_t v2_data[] = {0x02};
            std::vector<std::uint8_t> v2 = der_encode_integer(
                std::span<const std::uint8_t>(v2_data, 1));
            version.push_back(0xA0);  // [0] EXPLICIT
            version.push_back(static_cast<std::uint8_t>(v2.size()));
            version.insert(version.end(), v2.begin(), v2.end());
        }

        // 7) Compose TBSCertificate
        std::vector<std::uint8_t> tbs_body;
        tbs_body.insert(tbs_body.end(), version.begin(), version.end());
        tbs_body.insert(tbs_body.end(), serial.begin(), serial.end());
        tbs_body.insert(tbs_body.end(), sig_alg.begin(), sig_alg.end());
        tbs_body.insert(tbs_body.end(), name_cn.begin(), name_cn.end());  // issuer
        tbs_body.insert(tbs_body.end(), validity.begin(), validity.end());
        tbs_body.insert(tbs_body.end(), name_cn.begin(), name_cn.end());  // subject
        tbs_body.insert(tbs_body.end(), spki.begin(), spki.end());
        std::vector<std::uint8_t> tbs = der_wrap_sequence(tbs_body);

        // 8) Sign TBSCertificate with our ECDSA key — SHA-256 hash then
        //    ECDSA signature, encoded as BIT STRING wrapping ASN.1 DER.
        auto tbs_digest = bcrypt_sha256(tbs);
        if (tbs_digest.empty()) return false;
        auto sig_der = bcrypt_ecdsa_sign_der(ecdsa_key, tbs_digest);
        if (sig_der.empty()) {
            core::log::Logger::instance().warn(
                "dtls: ECDSA sign of cert failed; cert will be unsigned");
            sig_der = std::vector<std::uint8_t>(72, 0);  // placeholder empty sig
        }
        std::vector<std::uint8_t> sig_bit_string;
        sig_bit_string.reserve(2 + sig_der.size() + 1);
        sig_bit_string.push_back(0x03);
        if (sig_der.size() < 0x80) {
            sig_bit_string.push_back(static_cast<std::uint8_t>(1 + sig_der.size()));
        } else {
            sig_bit_string.push_back(0x81);
            sig_bit_string.push_back(static_cast<std::uint8_t>(1 + sig_der.size()));
        }
        sig_bit_string.push_back(0x00);  // 0 unused bits
        sig_bit_string.insert(sig_bit_string.end(), sig_der.begin(), sig_der.end());

        // 9) Compose the full Certificate = SEQUENCE { tbs, sigAlg, sigBitString }
        std::vector<std::uint8_t> cert_body;
        cert_body.insert(cert_body.end(), tbs.begin(), tbs.end());
        cert_body.insert(cert_body.end(), sig_alg.begin(), sig_alg.end());
        cert_body.insert(cert_body.end(), sig_bit_string.begin(), sig_bit_string.end());
        local_cert_der = der_wrap_sequence(cert_body);

        core::log::Logger::instance().info(
            std::string("dtls: built self-signed cert der_len=") +
            std::to_string(local_cert_der.size()) +
            " ecdsa_pub[0..7]=" + [&]() {
                std::string s;
                for (std::size_t i = 0; i < 8; ++i) {
                    char b[4]; std::snprintf(b, sizeof(b), "%02X", ecdsa_pub_uncompressed[i]);
                    s += b; s += (i + 1 < 8 ? ":" : "");
                }
                return s;
            }());
        return true;
#else
        (void)ecdsa_pub_uncompressed;
        return false;
#endif
    }

    // -------------------------------------------------------------------------
    // Wrap the cached local_cert_der into a DTLS Certificate handshake
    // message body.  Layout per RFC 5246 §7.4.2:
    //   Certificate = SEQUENCE { cert_list }
    //     cert_list = SEQUENCE { cert }
    //       cert = SEQUENCE { cert_len (3 bytes) | cert_body }
    // -------------------------------------------------------------------------
    std::vector<std::uint8_t> build_certificate_msg() {
        // RFC 5246 §7.4.2 (Certificate handshake message):
        //   opaque ASN.1Cert<1..2^24-1>;             // single DER cert
        //   struct {
        //     ASN.1Cert certificate_list<0..2^24-1>; // TLS-level wire format
        //   } Certificate;
        // The TLS wire format is NOT an ASN.1 SEQUENCE wrapper — it is
        // just length-prefixed raw bytes:
        //   [3-byte length of entire list]
        //   [3-byte length of cert 1][cert 1 bytes]
        //   [3-byte length of cert 2][cert 2 bytes] ...
        // We previously wrapped the cert entry in der_wrap_sequence()
        // which adds a 0x30 ASN.1 SEQUENCE tag — Chrome (BoringSSL)
        // rejects this with fatal unexpected_message because the
        // handshake layer does not expect an outer ASN.1 wrapper.
        if (local_cert_der.empty()) return {};
        std::vector<std::uint8_t> body;
        body.reserve(6 + local_cert_der.size());
        // Total list length (currently 1 cert = 3 + cert size).
        const std::uint32_t list_len = 3 + static_cast<std::uint32_t>(local_cert_der.size());
        body.push_back(static_cast<std::uint8_t>((list_len >> 16) & 0xff));
        body.push_back(static_cast<std::uint8_t>((list_len >> 8)  & 0xff));
        body.push_back(static_cast<std::uint8_t>(list_len & 0xff));
        // Per-cert length + bytes.
        body.push_back(static_cast<std::uint8_t>((local_cert_der.size() >> 16) & 0xff));
        body.push_back(static_cast<std::uint8_t>((local_cert_der.size() >> 8)  & 0xff));
        body.push_back(static_cast<std::uint8_t>(local_cert_der.size() & 0xff));
        body.insert(body.end(), local_cert_der.begin(), local_cert_der.end());
        return body;
    }

    // -------------------------------------------------------------------------
    // Build ClientHello (role = Client) or ServerHello (role = Server)
    // -------------------------------------------------------------------------

    std::vector<std::uint8_t> make_client_hello(bool with_cookie) {
        std::vector<std::uint8_t> hello;
        hello.reserve(64 + kRandomLen + 32);

        // legacy_version = DTLS 1.2 (0xFEFD).  Chrome rejects a
        // ClientHello whose legacy_version is 0xFEFF (DTLS 1.0) and
        // whose only supported_versions entry is also DTLS 1.0 with
        // protocol_version alert 70.  Use DTLS 1.2 for the
        // legacy_version field and advertise only 1.2 in
        // supported_versions.
        hello.push_back(0xFE); hello.push_back(0xFD);

        // random (32)
        hello.insert(hello.end(), client_random.begin(), client_random.end());

        // session_id (0 bytes for DTLS)
        hello.push_back(0x00);

        // cookie (cookie_len byte is always present; body is empty if no cookie)
        hello.push_back(cookie_len);
        if (with_cookie) {
            hello.insert(hello.end(), cookie.begin(), cookie.begin() + cookie_len);
        }

        // cipher_suites.  Advertise every ECDHE_ECDSA + AEAD suite
        // Chrome supports, in Chrome's preference order.  Chrome picks
        // the first one it likes; NimRTC only implements AES-128-GCM
        // (0xC02B) and rejects the rest at handshake time, but offering
        // the full list avoids Chrome's "no acceptable ciphers" path
        // (which has historically surfaced as decode_error rather than
        // handshake_failure in some Chrome versions).
        //
        // We lead with AES-128-GCM (which NimRTC actually implements)
        // rather than ChaCha20-Poly1305 (which we do not yet implement).
        // Chrome still negotiates ChaCha20 when it prefers it on platforms
        // without AES-NI hardware acceleration; on x86_64 desktops that
        // means we get AES-GCM.  Falling back to a peer-selected
        // ChaCha20 cipher would leave our AEAD encrypt step unable to
        // produce a plaintext Chrome can decrypt.
        //
        //   0xC02B  TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256  (preferred)
        //   0xCCA9  TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256
        //   0xC02C  TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384
        hello.push_back(0x00); hello.push_back(0x06);  // length 6
        hello.push_back(0xC0); hello.push_back(0x2B);  // AES-128-GCM
        hello.push_back(0xCC); hello.push_back(0xA9);  // ChaCha20-Poly1305
        hello.push_back(0xC0); hello.push_back(0x2C);  // AES-256-GCM

        // compression_methods
        hello.push_back(0x01);
        hello.push_back(0x00);  // null

        // Extensions: emit in the order Chromium expects (signature_algorithms,
        // DTLS-SRTP extensions.  For Chrome interop we emit signature_algorithms
        // (mandatory so Chrome knows how to verify the cert's ECDSA
        // signature), supported_versions (so Chrome stays on DTLS 1.2
        // rather than renegotiating to DTLS 1.3 — which would force
        // EncryptedExtensions and a wholly different handshake shape
        // NimRTC does not yet implement), supported_groups (so Chrome
        // picks secp256r1 for the ECDHE key exchange), and use_srtp
        // (mandatory for DTLS-SRTP).
        std::vector<std::uint8_t> exts;

        // signature_algorithms (RFC 5246 §7.4.1.4.1) — type 0x000D
        // Chrome rejects ClientHellos missing this extension with
        // decode_error (alert 50).  Advertise ECDSA + SHA256/P-256 (the
        // only scheme we actually emit from our self-signed P-256
        // ECDSA cert) plus the SHA384 variants in case the cipher-suite
        // negotiation picks AES-256-GCM-SHA384.
        {
            std::vector<std::uint8_t> sa;
            sa.push_back(0x00); sa.push_back(0x0D);  // ext type
            std::vector<std::uint8_t> algos;
            algos.push_back(0x05); algos.push_back(0x03);  // ecdsa_secp256r1_sha384
            algos.push_back(0x04); algos.push_back(0x03);  // ecdsa_secp256r1_sha256
            algos.push_back(0x08); algos.push_back(0x07);  // ed25519
            algos.push_back(0x08); algos.push_back(0x09);  // ed448
            algos.push_back(0x06); algos.push_back(0x03);  // ecdsa_secp384r1_sha384
            std::uint16_t algos_len = static_cast<std::uint16_t>(algos.size());
            std::uint16_t body_len  = static_cast<std::uint16_t>(2 + algos.size());
            sa.push_back(static_cast<std::uint8_t>(body_len >> 8));
            sa.push_back(static_cast<std::uint8_t>(body_len & 0xff));
            sa.push_back(static_cast<std::uint8_t>(algos_len >> 8));
            sa.push_back(static_cast<std::uint8_t>(algos_len & 0xff));
            sa.insert(sa.end(), algos.begin(), algos.end());
            exts.insert(exts.end(), sa.begin(), sa.end());
        }

        // supported_versions (RFC 8446 §4.2.1) — type 0x002B.
        //
        // The RFC specifies just `<2..254>` bytes of versions, but Chrome's
        // BoringSSL parser expects a 1-byte `length` prefix followed by
        // the version bytes — same as RFC 8446 §4.2.1's draft form.
        // Wire format Chrome accepts:
        //   uint8 length;            // = 2 for a single DTLS 1.2 entry
        //   uint8 version_major;     // 0xFE
        //   uint8 version_minor;     // 0xFD
        //
        // Without this extension Chrome negotiates DTLS 1.3, which
        // NimRTC's handshake state machine does not implement.
        {
            std::vector<std::uint8_t> sv;
            sv.push_back(0x00); sv.push_back(0x2B);  // ext type
            sv.push_back(0x00); sv.push_back(0x03);  // ext length 3
            sv.push_back(0x02);                       // list_length = 2 bytes
            sv.push_back(0xFE); sv.push_back(0xFD);  // DTLS 1.2
            exts.insert(exts.end(), sv.begin(), sv.end());
        }

        // supported_groups (RFC 7919 / RFC 8446 §4.2.1) — type 0x000A
        // List every group we can actually complete the ECDH for.  We
        // intentionally place x25519 *first* because Chrome prefers the
        // first supported group it recognises; x25519 is Chrome's modern
        // curve of choice (RFC 8446) and gives faster ECDHE on platforms
        // without hardware AES.  P-256 is kept as a fallback in case X25519
        // keygen failed at open() (e.g. Win10 < 19H1).
        {
            std::vector<std::uint8_t> sg;
            sg.push_back(0x00); sg.push_back(0x0A);  // ext type
            std::vector<std::uint8_t> groups;
            if (x25519_priv) {
                groups.push_back(static_cast<std::uint8_t>(kNamedGroupX25519 >> 8));
                groups.push_back(static_cast<std::uint8_t>(kNamedGroupX25519 & 0xff));
            }
            groups.push_back(static_cast<std::uint8_t>(kNamedGroupSecp256r1 >> 8));
            groups.push_back(static_cast<std::uint8_t>(kNamedGroupSecp256r1 & 0xff));
            std::uint16_t groups_len = static_cast<std::uint16_t>(groups.size());
            std::uint16_t body_len   = static_cast<std::uint16_t>(2 + groups.size());
            sg.push_back(static_cast<std::uint8_t>(body_len >> 8));
            sg.push_back(static_cast<std::uint8_t>(body_len & 0xff));
            sg.push_back(static_cast<std::uint8_t>(groups_len >> 8));
            sg.push_back(static_cast<std::uint8_t>(groups_len & 0xff));
            sg.insert(sg.end(), groups.begin(), groups.end());
            exts.insert(exts.end(), sg.begin(), sg.end());
        }

        // use_srtp (RFC 5764 §4.1.3) — type 0x000E
        //
        // Wire format (RFC 5764 §4.1.1):
        //
        //   SRTPProtectionProfile SRTPProtectionProfiles<2..2^16-1>;
        //   struct {
        //       SRTPProtectionProfiles SRTPProtectionProfiles;
        //       opaque srtp_mki<0..255>;
        //   } UseSRTPData;
        //
        // `SRTPProtectionProfiles<2..2^16-1>` is a TLS-style vector with
        // a 2-byte length prefix; each element is a 2-byte profile.
        // `srtp_mki<0..255>` is an opaque vector with a 1-byte length
        // prefix.  Body for a single profile (0x0001) and an empty MKI is
        // therefore exactly 5 bytes:
        //   00 02  00 01  00
        // (list_len=2, profile=0x0001, mki_len=0).
        {
            std::vector<std::uint8_t> srtp_ext;
            srtp_ext.push_back(0x00); srtp_ext.push_back(0x0E);  // ext type
            std::uint16_t prof = static_cast<std::uint16_t>(config.srtp_profile);
            std::vector<std::uint8_t> srtp_ext_body;
            std::uint16_t prof_list_len = static_cast<std::uint16_t>(sizeof(std::uint16_t));
            srtp_ext_body.push_back(static_cast<std::uint8_t>(prof_list_len >> 8));
            srtp_ext_body.push_back(static_cast<std::uint8_t>(prof_list_len & 0xff));
            srtp_ext_body.push_back(static_cast<std::uint8_t>(prof >> 8));
            srtp_ext_body.push_back(static_cast<std::uint8_t>(prof & 0xff));
            srtp_ext_body.push_back(0x00);  // srtp_mki length = 0
            std::uint16_t elen = static_cast<std::uint16_t>(srtp_ext_body.size());
            srtp_ext.push_back(static_cast<std::uint8_t>(elen >> 8));
            srtp_ext.push_back(static_cast<std::uint8_t>(elen & 0xff));
            srtp_ext.insert(srtp_ext.end(), srtp_ext_body.begin(), srtp_ext_body.end());
            exts.insert(exts.end(), srtp_ext.begin(), srtp_ext.end());
        }

        // extended_master_secret (RFC 7627) — type 0x0017, empty body.
        // Chrome's BoringSSL always includes this extension in its
        // DTLS-SRTP ClientHello and aborts the handshake with
        // illegal_parameter (alert 47) if the server's ServerHello omits
        // it.  Per RFC 7627 §5.2: when the client offers EMS, the server
        // MUST either include the same extension in ServerHello (which
        // switches the master_secret derivation to the EMS PRF) or alert
        // and abort.  We opt in by emitting the extension here.
        {
            std::vector<std::uint8_t> ems;
            ems.push_back(0x00); ems.push_back(0x17);  // ext type 0x0017
            ems.push_back(0x00); ems.push_back(0x00);  // ext length 0
            exts.insert(exts.end(), ems.begin(), ems.end());
        }
        // Note: signature_algorithms, supported_versions, supported_groups,
        // and ec_point_formats were all previously emitted but Chrome's
        // strict TLS parser keeps rejecting them with fatal decode_error
        // regardless of ordering.  Boiling the ClientHello down to just
        // the use_srtp extension is the minimal DTLS-SRTP handshakes that
        // Chrome accepts (matches what aiortc and BoringSSL-derived
        // clients emit for DTLS-SRTP over WebRTC).

        hello.push_back(static_cast<std::uint8_t>(exts.size() >> 8));
        hello.push_back(static_cast<std::uint8_t>(exts.size() & 0xff));
        hello.insert(hello.end(), exts.begin(), exts.end());
        dtls_hex_dump("client_hello", hello);
        core::log::Logger::instance().info(
            std::string("dtls: make_client_hello with_cookie=") +
            std::to_string(with_cookie ? 1 : 0) +
            " exts=" + std::to_string(exts.size()) +
            " hello=" + std::to_string(hello.size()) +
            " hex=" + bytes_to_hex(hello));
        return hello;
    }

    std::vector<std::uint8_t> make_server_hello(const std::vector<std::uint8_t>& /*ch_random*/) {
        // Use a fresh server_random
        auto rnd = bcrypt_random(kRandomLen);
        if (rnd.size() == kRandomLen) std::memcpy(server_random.data(), rnd.data(), kRandomLen);

        std::vector<std::uint8_t> hello;
        // legacy_version = DTLS 1.2 (0xFEFD).  Chrome / Firefox / WebRTC
        // peers reject ServerHello with legacy_version=0xFEFF (DTLS 1.0)
        // because their ClientHello advertised only DTLS 1.2 / 1.3 in
        // the supported_versions extension; the legacy_version is the
        // wire-format placeholder, but a server responding "DTLS 1.0"
        // is treated as a version mismatch and the peer aborts with
        // "wrong version", causing ClientHello retransmits forever.
        hello.push_back(0xFE); hello.push_back(0xFD);
        hello.insert(hello.end(), server_random.begin(), server_random.end());
        hello.push_back(0x00);  // session_id len 0

        // cipher_suite (chosen)
        hello.push_back(static_cast<std::uint8_t>(kCipherEcdheEcdsaAes128GcmSha256 >> 8));
        hello.push_back(static_cast<std::uint8_t>(kCipherEcdheEcdsaAes128GcmSha256 & 0xff));

        hello.push_back(0x00);  // compression method: null

        // extensions: use_srtp + supported_versions
        std::vector<std::uint8_t> exts;
        std::vector<std::uint8_t> srtp_ext;
        srtp_ext.push_back(0x00); srtp_ext.push_back(0x0E);
        std::uint16_t prof = static_cast<std::uint16_t>(config.srtp_profile);
        std::vector<std::uint8_t> srtp_ext_body;
        srtp_ext_body.push_back(0x00); srtp_ext_body.push_back(0x02);
        srtp_ext_body.push_back(static_cast<std::uint8_t>(prof >> 8));
        srtp_ext_body.push_back(static_cast<std::uint8_t>(prof & 0xff));
        std::uint16_t elen = static_cast<std::uint16_t>(srtp_ext_body.size());
        srtp_ext.push_back(static_cast<std::uint8_t>(elen >> 8));
        srtp_ext.push_back(static_cast<std::uint8_t>(elen & 0xff));
        srtp_ext.insert(srtp_ext.end(), srtp_ext_body.begin(), srtp_ext_body.end());
        exts.insert(exts.end(), srtp_ext.begin(), srtp_ext.end());

        // supported_versions (RFC 8446) — ext type 0x002B, value DTLS 1.2 (0xFEFD).
        // Modern Chrome (>=M150) requires this to confirm the selected version;
        // without it Chrome aborts with "wrong version" since the ClientHello's
        // legacy_version=0xFEFF would be treated as DTLS 1.0.
        {
            std::vector<std::uint8_t> sv;
            sv.push_back(0x00); sv.push_back(0x2B);  // ext type
            sv.push_back(0x00); sv.push_back(0x02);  // ext length 2 (single version)
            sv.push_back(0xFE); sv.push_back(0xFD);  // DTLS 1.2
            exts.insert(exts.end(), sv.begin(), sv.end());
        }

        // extended_master_secret (RFC 7627) — type 0x0017, empty body.
        // Mirror what we emit in the ClientHello.  Chrome includes this
        // extension in its ClientHello and refuses to proceed without it
        // in the ServerHello.  Both sides then derive master_secret via
        // the EMS PRF (see compute_master_secret_impl() below).
        {
            std::vector<std::uint8_t> ems;
            ems.push_back(0x00); ems.push_back(0x17);  // ext type 0x0017
            ems.push_back(0x00); ems.push_back(0x00);  // ext length 0
            exts.insert(exts.end(), ems.begin(), ems.end());
        }

        hello.push_back(static_cast<std::uint8_t>(exts.size() >> 8));
        hello.push_back(static_cast<std::uint8_t>(exts.size() & 0xff));
        hello.insert(hello.end(), exts.begin(), exts.end());
        core::log::Logger::instance().info(
            std::string("dtls: make_server_hello srtp_ext_len=") +
            std::to_string(srtp_ext.size()) +
            " exts=" + std::to_string(exts.size()) +
            " hello=" + std::to_string(hello.size()));
        return hello;
    }

    // -------------------------------------------------------------------------
    // Handshake message handling
    // -------------------------------------------------------------------------

    void handle_client_hello(std::span<const std::uint8_t> body) {
        core::log::Logger::instance().info(
            std::string("dtls: handle_client_hello body_size=") +
            std::to_string(body.size()));
        if (body.size() < 2) return;
        std::size_t off = 0;
        // legacy_version
        off += 2;
        if (off + kRandomLen > body.size()) return;
        std::memcpy(client_random.data(), body.data() + off, kRandomLen);
        off += kRandomLen;

        if (off + 1 > body.size()) return;
        std::uint8_t sid_len = body[off++];
        if (off + sid_len > body.size()) return;
        off += sid_len;

        if (off + 1 > body.size()) return;
        std::uint8_t cookie_len_in = body[off++];
        if (off + cookie_len_in > body.size()) return;
        if (cookie_len_in == cookie_len) {
            std::memcpy(cookie.data(), body.data() + off, cookie_len_in);
        }
        // Advance by what the peer actually sent (cookie_len_in), NOT by our
        // stored cookie_len.  cookie_len is what we *expect* (e.g. 32 bytes),
        // but on a retransmit of the initial ClientHello the peer sends
        // cookie_len_in=0 because we never delivered the HelloVerifyRequest
        // (ICE wasn't connected when we tried to send).  Using cookie_len
        // here would skip 32 bytes of actual cipher_suites/compression data
        // and the subsequent cs_len read would land on garbage, hitting an
        // early-return on the second-and-later ClientHellos.
        off += cookie_len_in;

        if (off + 2 > body.size()) return;
        std::uint16_t cs_len = read_be16(body.data() + off);
        off += 2;
        if (off + cs_len > body.size()) return;
        off += cs_len;

        if (off + 1 > body.size()) return;
        std::uint8_t comp_len = body[off++];
        if (off + comp_len > body.size()) return;
        off += comp_len;

        // extensions — we only need to detect the RFC 7627
        // `extended_master_secret` (EMS) advertisement.  When the
        // client offers EMS we MUST include it in our ServerHello and
        // use the EMS PRF for master_secret derivation; without it
        // Chrome aborts the handshake with illegal_parameter (alert 47).
        bool client_ems_offered = false;
        if (off + 2 <= body.size()) {
            std::uint16_t ext_len = read_be16(body.data() + off);
            off += 2;
            if (off + ext_len > body.size()) return;
            std::size_t ext_end = off + ext_len;
            while (off + 4 <= ext_end) {
                std::uint16_t et = read_be16(body.data() + off);
                std::uint16_t el = read_be16(body.data() + off + 2);
                off += 4;
                if (off + el > ext_end) return;
                if (et == 0x0017 && el == 0) {
                    client_ems_offered = true;
                }
                off += el;
            }
        }
        use_ems_ = client_ems_offered;  // finalized when we echo EMS in ServerHello

        core::log::Logger::instance().info(
            std::string("dtls: handle_client_hello parsed, state=") +
            std::to_string(static_cast<int>(state)));

        // State machine guard: only initiate the cookie exchange (HelloVerify)
        // when we are in Initial state.  Once we have sent HelloVerifyRequest
        // (state -> HelloVerify) or completed even a partial handshake
        // (state -> HelloReceived / Finished / Connected), any further inbound
        // ClientHello from the same peer is a DTLS retransmit or a
        // reordered duplicate — discard it.
        //
        // The previous guard `first_client_hello_seen` was a one-shot boolean:
        // it flipped false on the first inbound ClientHello and never toggled
        // back, so a third (or fourth) ClientHello retransmit from Chrome
        // would re-enter the full-handshake else-branch and emit a second
        // ServerHello/Cert/SKE/SHD flight with fresh msg_seq numbers.  Chrome
        // received duplicate ServerHello (msg_seq already used) and aborted with
        // fatal illegal_parameter (alert 47).
        //
        // Fix: gate on state so only the very first inbound ClientHello
        // (state == Initial) triggers HelloVerify; all later arrivals are
        // dropped.
        if (state == DtlsState::Initial) {
            auto rnd = bcrypt_random(kHelloCookieLen);
            if (rnd.size() == kHelloCookieLen) {
                std::memcpy(cookie.data(), rnd.data(), kHelloCookieLen);
                cookie_len = kHelloCookieLen;
            }

            // Body: legacy_version(2) + cookie_len(1) + cookie
            std::vector<std::uint8_t> hvr;
            hvr.push_back(0xFE); hvr.push_back(0xFD);  // DTLS 1.2
            hvr.push_back(cookie_len);
            hvr.insert(hvr.end(), cookie.begin(), cookie.begin() + cookie_len);
            enqueue_handshake(kHsHelloVerify, hvr, 0, 1);
            state = DtlsState::HelloVerify;
        } else if (state == DtlsState::HelloVerify) {
            // ClientHello-with-cookie after our HelloVerifyRequest: emit the
            // full server flight exactly once.  We must NOT match
            // "state != Initial" here because subsequent inbound ClientHellos
            // (Chrome's DTLS retransmits) carry state == HelloReceived and
            // would re-enter this branch and emit a *second*
            // ServerHello/Cert/SKE/SHD flight with fresh msg_seq numbers.
            // Chrome rejects the duplicate ServerHello with fatal
            // illegal_parameter (alert 47).  Restricting the full handshake
            // to state == HelloVerify ensures we send the flight exactly
            // once per connection.
            // Move on to full handshake: ServerHello + Certificate +
            // ServerKeyExchange + ServerHelloDone.  The Certificate message
            // was missing before — without it Chrome aborts with
            // "missing certificate" and retransmits ClientHello forever.
            auto sh = make_server_hello({});
            enqueue_handshake(kHsServerHello, sh, 0, 2);
            state = DtlsState::HelloReceived;

            // Certificate (RFC 5246 §7.4.2): wrap the self-signed X.509 cert
            // built at init_crypto() in the DTLS Certificate envelope.
            auto cert_body = build_certificate_msg();
            if (!cert_body.empty()) {
                enqueue_handshake(kHsCertificate, cert_body, 0, 3);
            }

            // ServerKeyExchange (RFC 4492 §5.4): ECC params (curve_type=3
            // named_curve, named_curve=group, public_key N bytes), followed
            // by a real ECDSA signature over (client_random ||
            // server_random || params).  Previously this code wrote a raw
            // SHA-256 hash which Chrome rejects as a malformed signature.
            //
            // Curve pick: prefer X25519 if we generated a keypair (typical
            // modern Windows).  Falls back to secp256r1 when BCRYPT X25519
            // wasn't available at open() — Chrome's preference order still
            // matches our first-offered group, so on x25519-capable Windows
            // Chrome will negotiate 0x001D here.
            std::vector<std::uint8_t> ske_params;
            std::uint16_t              picked_named_curve = kNamedGroupSecp256r1;
            const std::uint8_t*       picked_pubkey_bytes = local_pub_bytes.data();
            std::size_t               picked_pubkey_len   = kP256PubkeyLen;
#if NIMRTC_HAS_BCRYPT
            if (x25519_priv) {
                picked_named_curve = kNamedGroupX25519;
                picked_pubkey_bytes = local_x25519_pub_bytes.data();
                picked_pubkey_len   = kX25519PubkeyLen;
                negotiated_curve    = NegotiatedCurve::X25519;
            } else {
                negotiated_curve    = NegotiatedCurve::Secp256r1;
            }
#else
            negotiated_curve = NegotiatedCurve::Secp256r1;
#endif

            ske_params.push_back(kCurveTypeNamed);                  // curve_type: named_curve
            ske_params.push_back(static_cast<std::uint8_t>(picked_named_curve >> 8));
            ske_params.push_back(static_cast<std::uint8_t>(picked_named_curve & 0xff));
            ske_params.push_back(static_cast<std::uint8_t>(picked_pubkey_len));
            ske_params.insert(ske_params.end(),
                              picked_pubkey_bytes,
                              picked_pubkey_bytes + picked_pubkey_len);

            std::vector<std::uint8_t> sig_data;
            sig_data.insert(sig_data.end(), client_random.begin(), client_random.end());
            sig_data.insert(sig_data.end(), server_random.begin(), server_random.end());
            sig_data.insert(sig_data.end(), ske_params.begin(), ske_params.end());
            auto sig_hash = bcrypt_sha256(sig_data);

            // ECDSA sign with our ECDSA key.  Fall back to a placeholder
            // signature if signing fails — Chrome will reject the cert,
            // but it lets us see further along the failure cascade.
            std::vector<std::uint8_t> sig_der;
#if NIMRTC_HAS_BCRYPT
            if (ecdsa_key != nullptr) {
                sig_der = bcrypt_ecdsa_sign_der(ecdsa_key, sig_hash);
            }
#endif
            if (sig_der.empty()) {
                core::log::Logger::instance().warn(
                    "dtls: ECDSA sign of ServerKeyExchange failed; using empty sig");
                sig_der.assign(8, 0);
            }
            std::vector<std::uint8_t> ske;
            ske.reserve(ske_params.size() + 2 + sig_der.size());
            ske.insert(ske.end(), ske_params.begin(), ske_params.end());
            // Signature: 2-byte length + ASN.1 DER ECDSA signature
            std::uint16_t sig_len16 = static_cast<std::uint16_t>(sig_der.size());
            ske.push_back(static_cast<std::uint8_t>(sig_len16 >> 8));
            ske.push_back(static_cast<std::uint8_t>(sig_len16 & 0xff));
            ske.insert(ske.end(), sig_der.begin(), sig_der.end());
            enqueue_handshake(kHsServerKeyExchange, ske, 0, 4);

            enqueue_handshake(kHsServerHelloDone, {}, 0, 5);
        }
    }

    void handle_server_hello(std::span<const std::uint8_t> body) {
        if (body.size() < 2 + kRandomLen + 1) return;
        std::size_t off = 2;  // skip legacy_version
        std::memcpy(server_random.data(), body.data() + off, kRandomLen);
        off += kRandomLen;
        std::uint8_t sid_len = body[off++];
        off += sid_len;
        if (off + 2 + 1 > body.size()) return;
        const std::uint16_t chosen_cipher =
            static_cast<std::uint16_t>((body[off] << 8) | body[off + 1]);
        off += 2;  // cipher_suite
        off += 1;  // compression
        // Validate the server-selected cipher.  NimRTC's traffic key
        // derivation is hardcoded to AES-128-GCM (RFC 5288) — so if the
        // server picked anything else (e.g. ChaCha20-Poly1305) we would
        // happily derive the wrong keys, send AEAD-encrypted records
        // that Chrome can't decrypt, and Chrome would answer with an
        // unexpected_message alert.
        //
        // We pre-empt that by aborting the handshake here when the
        // chosen cipher isn't ours.  Reordering make_client_hello() so
        // AES-128-GCM comes first avoids this path in practice, but
        // the validation is still important: it surfaces a misconfigured
        // peer (or future Chrome versions that stop preferring AES-GCM
        // in our list) as a clear "unsupported_cipher" rather than a
        // mysterious alert.
        if (chosen_cipher != kCipherEcdheEcdsaAes128GcmSha256) {
            core::log::Logger::instance().error(
                std::string("dtls: server chose unsupported cipher 0x") +
                (chosen_cipher >> 8 ? "" : "0") +
                std::to_string(chosen_cipher) +
                " (only AES-128-GCM/0xC02B implemented)");
            state = DtlsState::Failed;
            return;
        }
        // Walk the ServerHello extensions and detect RFC 7627
        // extended_master_secret.  We always advertise EMS in our
        // ClientHello, so a server that omits it in ServerHello is
        // rejecting EMS — fall back to the pre-EMS PRF in that case.
        bool server_ems = false;
        if (off + 2 <= body.size()) {
            std::uint16_t ext_len = read_be16(body.data() + off);
            std::size_t ext_off = off + 2;
            std::size_t ext_end = ext_off + ext_len;
            if (ext_end <= body.size()) {
                while (ext_off + 4 <= ext_end) {
                    std::uint16_t et = read_be16(body.data() + ext_off);
                    std::uint16_t el = read_be16(body.data() + ext_off + 2);
                    ext_off += 4;
                    if (ext_off + el > ext_end) break;
                    if (et == 0x0017 && el == 0) server_ems = true;
                    ext_off += el;
                }
            }
        }
        use_ems_ = server_ems;
        state = DtlsState::HelloReceived;
    }

    void handle_server_key_exchange(std::span<const std::uint8_t> body) {
        if (body.size() < 4) return;
        std::size_t off = 0;
        // curve_type (1) — we only support named_curve (RFC 4492 §5.4 = 0x03)
        if (body[off++] != kCurveTypeNamed) return;
        std::uint16_t named_curve = static_cast<std::uint16_t>(
            (body[off] << 8) | body[off + 1]);
        off += 2;
        std::uint8_t pk_len = body[off++];
        if (body.size() < off + pk_len) return;

        // Lock in the negotiated curve so derive_pre_master_secret(),
        // handle_client_key_exchange() and any future ECDH-related code
        // agree on which local privkey to use.
        if (named_curve == kNamedGroupX25519) {
            if (pk_len != kX25519PubkeyLen) return;
            if (!x25519_priv) {
                core::log::Logger::instance().error(
                    "dtls: server chose X25519 but no local X25519 key "
                    "(upgrade Windows to 19H1 or build with BCrypt X25519 "
                    "support for x25519-advertising peers)");
                state = DtlsState::Failed;
                return;
            }
            negotiated_curve = NegotiatedCurve::X25519;
        } else if (named_curve == kNamedGroupSecp256r1) {
            if (pk_len != kP256PubkeyLen) return;
            negotiated_curve = NegotiatedCurve::Secp256r1;
        } else {
            core::log::Logger::instance().error(
                std::string("dtls: server chose unsupported curve 0x") +
                std::to_string(named_curve) +
                " (NimRTC only implements secp256r1/0x0017 and X25519/0x001D)");
            state = DtlsState::Failed;
            return;
        }

        peer_pub_bytes.assign(body.data() + off, body.data() + off + pk_len);
        have_peer_pub = true;

        // NOTE: fingerprint verification is skipped here — to do it
        // properly we'd need to parse the preceding Certificate message,
        // extract its SubjectPublicKeyInfo, and SHA-256-hash it.  For P1
        // interop we rely on TLS session integrity (the verify_data check
        // on the Finished message) and the SDP-pinned fingerprint is
        // verified at the application layer above DTLS.
        // TODO(P1.1): properly extract ECDSA pubkey from peer's Certificate
        // and verify fingerprint + signature.

        // Derive pre_master_secret via ECDH (X25519 or P-256 depending on
        // negotiated_curve).  We CANNOT derive master_secret here because
        // Chrome packs CertReq + ServerHelloDone into the same datagram
        // after SKE; computing master_secret now would snapshot hs_log
        // BEFORE those messages, and that yields a master_secret that
        // differs from Chrome's (which is computed AFTER Chrome has seen
        // the full pre-CKE flight, i.e. up to and including CKE).
        //
        // Defer master_secret + traffic_keys + srtp_keys to
        // case kHsServerHelloDone, which runs AFTER all five server
        // messages have been appended to hs_log (and the client has
        // appended its own CKE to hs_log via enqueue_handshake).
        //
        // Note: derive_pre_master_secret_idempotent() is safe to call here
        // because Chrome retransmits SKE while waiting for our flight; the
        // guard ensures we only do the ECDH key agreement once.
        derive_pre_master_secret_idempotent();
        state = DtlsState::KeyExchange;
    }

    // Called on the client side, AFTER we have enqueued CKE+CCS+Finished
    // in case kHsServerHelloDone.  At this point hs_log includes:
    //   ClientHello, ServerHello, Certificate, ServerKeyExchange,
    //   CertificateRequest, ServerHelloDone, ClientKeyExchange
    // (CertReq is included even though RFC 7627 §4 excludes it from the
    //  EMS seed — the verify_data computation still hashes CertReq, so
    //  Chrome will agree on the same exclusion because both sides run
    //  the same SHA-256 over the same byte stream).
    //
    // On the server side the equivalent computation happens inside
    // handle_client_key_exchange (we wait for CKE there, then enqueue
    // CCS+Finished, both of which the case-kHsServerHelloDone equivalent
    // for the server has already appended to hs_log before the secrets
    // derivation fires).
    void derive_session_secrets_idempotent() {
        if (master_secret_ready) return;
        if (!pre_master_secret_ready) {
            core::log::Logger::instance().warn(
                "dtls: derive_session_secrets called without pre_master_secret");
            return;
        }
        compute_master_secret();
        derive_traffic_keys();
        compute_srtp_keying_material();
    }

    void handle_client_key_exchange(std::span<const std::uint8_t> body) {
        // Server picks the curve in ServerKeyExchange; that decision is
        // captured in negotiated_curve.  Use the matching pubkey length
        // when ingesting the client's reply.  Currently we always emit
        // ServerKeyExchange with whichever curve we generated up-front in
        // init_crypto() (X25519 if we have a key, else P-256), so the
        // matching PK length is unambiguous on the server side.
        std::size_t expected = (negotiated_curve == NegotiatedCurve::X25519)
                                ? kX25519PubkeyLen
                                : kP256PubkeyLen;
        if (body.size() < expected) return;
        peer_pub_bytes.assign(body.data(), body.data() + expected);
        have_peer_pub = true;

        // Skip fingerprint verification on the client-key-exchange path:
        // ECDHE_ECDSA clients are typically anonymous (no client cert),
        // so we'd have nothing to verify anyway.  Session security rests
        // on the Finished verify_data check.

        derive_pre_master_secret_idempotent();
        compute_master_secret();
        derive_traffic_keys();
        compute_srtp_keying_material();
        // Server: send ChangeCipherSpec + Finished ("server finished").
        // The server's first outbound handshake message is Finished, so use
        // seq=0 here (its own counter).  The CCS record carries epoch=1.
        // We deliberately do NOT flip state to Connected yet — we still
        // need to receive the client's Finished and validate its
        // verify_data.  The state is promoted to Connected when that
        // handshake message is processed (see Finished case in
        // dispatch_handshake).
        state = DtlsState::Finished;
        enqueue_change_cipher_spec(/*epoch=*/1, /*seq=*/0);
        enqueue_handshake(kHsFinished,
                          make_finished(/*label=*/"server finished"),
                          /*epoch=*/1, /*seq=*/1);
    }

    // -------------------------------------------------------------------------
    // PRF computations
    // -------------------------------------------------------------------------

    void derive_pre_master_secret() {
#if NIMRTC_HAS_BCRYPT
        if (!have_peer_pub) return;
        // Dispatch on negotiated_curve.  Both branches use 32-byte raw
        // output (BCRYPT_KDF_RAW_SECRET) into pre_master_secret, which is
        // exactly the wire-format pre-master-secret for TLS 1.2 ECDHE
        // (RFC 5246 §7.4.7.1 for NIST curves; RFC 8422 §5.10 → RFC 7748 §6.1
        // for X25519).  No hashing step is required.

        if (negotiated_curve == NegotiatedCurve::X25519 && x25519_priv) {
            // X25519 peer pubkey blob is just 32 raw bytes (Montgomery
            // u-coordinate, per RFC 7748 §5).  We construct an X25519 key
            // pair handle directly from the raw byte buffer using
            // BCRYPT_X25519_PUBLIC_BLOB; the alg provider expects exactly
            // 32 bytes for this blob type.
            std::vector<unsigned char> xblob(peer_pub_bytes.begin(),
                                             peer_pub_bytes.begin() + kX25519PubkeyLen);
            BCRYPT_KEY_HANDLE peer_key = nullptr;
            NTSTATUS s = ::BCryptImportKeyPair(x25519_alg, nullptr,
                                               BCRYPT_X25519_PUBLIC_BLOB,
                                               &peer_key, xblob.data(),
                                               static_cast<ULONG>(xblob.size()), 0);
            if (!BCRYPT_SUCCESS(s)) {
                core::log::Logger::instance().error(
                    "DTLS: failed to import peer X25519 pubkey");
                return;
            }
            s = ::BCryptSecretAgreement(x25519_priv, peer_key,
                                        &secret_agreement, 0);
            if (!BCRYPT_SUCCESS(s)) {
                ::BCryptDestroyKey(peer_key);
                core::log::Logger::instance().error(
                    "DTLS: BCryptSecretAgreement (X25519) failed");
                return;
            }
            DWORD agreed_len = 0, res_size = 0;
            s = ::BCryptDeriveKey(secret_agreement, BCRYPT_KDF_RAW_SECRET,
                                  nullptr, nullptr, 0, &agreed_len, 0);
            if (!BCRYPT_SUCCESS(s)) {
                ::BCryptDestroyKey(peer_key);
                return;
            }
            std::vector<unsigned char> agreed(agreed_len);
            s = ::BCryptDeriveKey(secret_agreement, BCRYPT_KDF_RAW_SECRET,
                                  nullptr, agreed.data(),
                                  static_cast<ULONG>(agreed.size()),
                                  &res_size, 0);
            ::BCryptDestroyKey(peer_key);
            if (!BCRYPT_SUCCESS(s)) return;
            std::memcpy(pre_master_secret.data(), agreed.data(),
                        std::min<std::size_t>(pre_master_secret.size(), agreed.size()));
            pre_master_secret_ready = true;
            return;
        }

        // Default: P-256 / secp256r1.
        // Reconstruct BCRYPT_ECCPUBLIC_BLOB from the wire-format point
        // peer_pub_bytes (65 bytes: 0x04 || X || Y).
        // Blob layout: magic(4) cbKey(4) X[cbKey] Y[cbKey] = 72 bytes.
        constexpr std::size_t kEccKeyBlobHeaderLen = 8 + 2 * 32;
        std::vector<unsigned char> blob(kEccKeyBlobHeaderLen);
        *reinterpret_cast<ULONG*>(blob.data())     = 0x314B4345;  // BCRYPT_ECDH_PUBLIC_P256_MAGIC
        *reinterpret_cast<ULONG*>(blob.data() + 4) = 32;
        std::memcpy(blob.data() + 8,  peer_pub_bytes.data() + 1,  32);  // X
        std::memcpy(blob.data() + 40, peer_pub_bytes.data() + 33, 32);  // Y

        BCRYPT_KEY_HANDLE peer_key = nullptr;
        NTSTATUS s = ::BCryptImportKeyPair(ecdh_alg, nullptr,
                                           BCRYPT_ECCPUBLIC_BLOB,
                                           &peer_key, blob.data(),
                                           static_cast<ULONG>(blob.size()), 0);
        if (!BCRYPT_SUCCESS(s)) {
            core::log::Logger::instance().error("DTLS: failed to import peer pubkey");
            return;
        }

        s = ::BCryptSecretAgreement(local_priv, peer_key, &secret_agreement, 0);
        if (!BCRYPT_SUCCESS(s)) {
            ::BCryptDestroyKey(peer_key);
            core::log::Logger::instance().error("DTLS: BCryptSecretAgreement failed");
            return;
        }

        DWORD agreed_len = 0, res_size = 0;
        s = ::BCryptDeriveKey(secret_agreement, BCRYPT_KDF_RAW_SECRET,
                              nullptr, nullptr, 0, &agreed_len, 0);
        if (!BCRYPT_SUCCESS(s)) return;
        std::vector<unsigned char> agreed(agreed_len);
        s = ::BCryptDeriveKey(secret_agreement, BCRYPT_KDF_RAW_SECRET,
                              nullptr, agreed.data(),
                              static_cast<ULONG>(agreed.size()),
                              &res_size, 0);
        if (!BCRYPT_SUCCESS(s)) return;

        std::memcpy(pre_master_secret.data(), agreed.data(),
                    std::min<std::size_t>(pre_master_secret.size(), agreed.size()));
        pre_master_secret_ready = true;
        ::BCryptDestroyKey(peer_key);
#else
        std::memset(pre_master_secret.data(), 0, pre_master_secret.size());
#endif
    }

    // Guard wrapper around derive_pre_master_secret() — Chrome retransmits
    // ServerHello/SKE several times while waiting for our CKE+CCS+Finished,
    // and each retransmit calls derive_pre_master_secret() from
    // handle_server_key_exchange / handle_client_key_exchange.  Without
    // idempotency those repeated calls would each redo the ECDH key
    // agreement and rewrite pre_master_secret with a fresh (but identical,
    // modulo BCrypt internal nonce) buffer — which is harmless on its own
    // but the *next* step in those handlers (compute_master_secret) IS
    // order-dependent on the running handshake hash, so the guard needs to
    // live there too.  See compute_master_secret() below.
    void derive_pre_master_secret_idempotent() {
        if (pre_master_secret_ready) return;
        derive_pre_master_secret();
    }

    // -------------------------------------------------------------------------
    // Diagnostic emitters (Step B: capture infrastructure)
    // -------------------------------------------------------------------------
    //
    // Emit secrets to disk when the corresponding env var is set, gated on
    // `compute_master_secret()` so they fire exactly once per handshake.
    // Wireshark reads the NSS keylog format directly; the trace dump is
    // for cross-checking NimRTC's PRF values against a Python reference.
    //
    // SECURITY: these flags leak the master_secret in cleartext.  Do not
    // ship with them enabled in production.  They are meant only for the
    // Chrome interop debugging session.

#if defined(_WIN32)
    static std::string get_env_or_empty(const char* name) {
        char* buf = nullptr; size_t len = 0;
        _dupenv_s(&buf, &len, name);
        std::string out;
        if (buf) { out.assign(buf, len); free(buf); }
        return out;
    }
#else
    static std::string get_env_or_empty(const char* name) {
        const char* v = std::getenv(name);
        return v ? std::string(v) : std::string();
    }
#endif

    static std::string hexline(std::span<const std::uint8_t> b) {
        std::string s; s.reserve(b.size() * 2);
        char buf[4];
        for (auto x : b) { std::snprintf(buf, sizeof(buf), "%02x", x); s += buf; }
        return s;
    }

    void dtls_emit_keylog_if_enabled() {
        static const std::string path = get_env_or_empty("NIMRTC_DTLS_KEYLOG");
        if (path.empty()) return;
        // Append-only; truncate on first call per process.  Same lifecycle
        // pattern as dtls_hex_dump() so successive handshakes don't pile up.
        static std::once_flag init_flag;
        std::call_once(init_flag, [&]() {
            std::ofstream t(path, std::ios::binary | std::ios::trunc);
        });
        std::ofstream f(path, std::ios::binary | std::ios::app);
        if (!f.is_open()) return;
        // RFC 5246 §A.4.1 NSS Key Log Format:
        //   "CLIENT_RANDOM " <client_random_hex> " " <master_secret_hex>
        // Wireshark recognises this verbatim for both TLS and DTLS.
        f << "CLIENT_RANDOM "
          << hexline(client_random) << " "
          << hexline(master_secret) << "\n";
    }

    void dtls_emit_trace_if_enabled() {
        static const std::string path = get_env_or_empty("NIMRTC_DTLS_TRACE");
        if (path.empty()) return;
        static std::once_flag init_flag;
        std::call_once(init_flag, [&]() {
            std::ofstream t(path, std::ios::binary | std::ios::trunc);
        });
        std::ofstream f(path, std::ios::binary | std::ios::app);
        if (!f.is_open()) return;

        const char* curve =
            (negotiated_curve == NegotiatedCurve::X25519) ? "x25519" :
            (negotiated_curve == NegotiatedCurve::Secp256r1) ? "secp256r1" : "unknown";
        f << "--- NimRTC DTLS handshake state ---\n"
          << "role:        " << (config.role == DtlsRole::Client ? "client" : "server") << "\n"
          << "curve:       " << curve << "\n"
          << "use_ems:     " << (use_ems_ ? "true" : "false") << "\n"
          << "client_rand: " << hexline(client_random) << "\n"
          << "server_rand: " << hexline(server_random) << "\n"
          << "pre_master:  " << hexline(pre_master_secret) << "\n"
          << "master_sec:  " << hexline(master_secret) << "\n";
        // Pre-compute the verify_data we'd send (so the user can compare
        // with what's in Wireshark).  We call make_finished() for both
        // labels because the user can be either peer.
        std::stringstream ssb;
        ssb << "verify_data(server finished): "
            << hexline(make_finished("server finished")) << "\n";
        f << ssb.str();
        f.flush();
    }

    void compute_master_secret() {
        // Idempotency: master_secret MUST be computed exactly once per
        // handshake.  Chrome retransmits ServerHello+Cert+SKE+SHD 6+ times
        // while waiting for our CKE+CCS+Finished (typical DTLS retransmit
        // cadence).  Without this guard each retransmit would re-snapshot
        // hs_log (which now includes the retransmitted messages), compute
        // a different EMS seed, and rewrite master_secret to a different
        // value — which then shifts our traffic keys and SRTP keying
        // material and breaks the verify_data check.  We cache the first
        // computed master_secret and use it for the rest of the handshake.
        if (master_secret_ready) {
            return;
        }
        // master_secret = PRF(pre_master_secret, "master secret",
        //                     ClientHello.random || ServerHello.random)[0..47]
        //
        // RFC 7627 §4 (extended master secret): when both sides have
        // negotiated EMS, the seed is the SHA-256 hash of every handshake
        // message exchanged up to (but not including) the
        // ClientKeyExchange — i.e. the same digest used for the Finished
        // verify_data, but computed at the point where the EMS extension
        // is the last message seen by both peers.  We snapshot the
        // running handshake hash *now* (before CKE / CCS / Finished) so
        // the EMS seed is identical on both sides.
        std::vector<std::uint8_t> ms;
        if (use_ems_) {
            auto handshake_hash = compute_handshake_hash_snapshot();
            ms = tls12_prf(pre_master_secret, "extended master secret",
                           handshake_hash, kMasterSecretLen);
        } else {
            std::vector<std::uint8_t> seed;
            seed.reserve(kRandomLen * 2);
            seed.insert(seed.end(), client_random.begin(), client_random.end());
            seed.insert(seed.end(), server_random.begin(), server_random.end());
            ms = tls12_prf(pre_master_secret, "master secret", seed,
                           kMasterSecretLen);
        }
        std::memcpy(master_secret.data(), ms.data(),
                    std::min<std::size_t>(master_secret.size(), ms.size()));
        master_secret_ready = true;

        // Diagnostic — emit an NSS-style keylog line and a verbose per-
        // handshake line so the user can decrypt DTLS records in Wireshark
        // and compare verify_data byte-for-byte with Chrome's Finished.
        // Gated on env vars to avoid leaking keys in normal operation.
        //
        //   NIMRTC_DTLS_KEYLOG    -> write NSS keylog (CLIENT_RANDOM <cr> <ms>)
        //                            to %NIMRTC_DTLS_KEYLOG%.  Wireshark
        //                            recognizes this format automatically
        //                            when "Pre-Master-Secret log file" is
        //                            configured.
        //   NIMRTC_DTLS_TRACE     -> write a multi-line trace including
        //                            client_random || server_random || pre_master
        //                            || master_secret || negotiated_curve to
        //                            %NIMRTC_DTLS_TRACE%.  Used to cross-
        //                            check against a Python PRF computation.
        //
        // Note: do NOT emit in production — these are secrets.
        dtls_emit_keylog_if_enabled();
        dtls_emit_trace_if_enabled();
    }

    /**
     * Derive AES-128-GCM traffic keys + implicit salts (RFC 5246 §6.3 +
     * RFC 5288 §3).  Called once after master_secret is ready.
     *
     *   key_block = PRF(master, "key expansion",
     *                   server_random || client_random)[0..40]
     *   client_write_key  = key_block[0..16]
     *   server_write_key  = key_block[16..32]
     *   client_write_salt = key_block[32..36]
     *   server_write_salt = key_block[36..40]
     */
    void derive_traffic_keys() {
        if (traffic_keys_ready) return;
        std::vector<std::uint8_t> seed;
        seed.reserve(kRandomLen * 2);
        seed.insert(seed.end(), server_random.begin(), server_random.end());
        seed.insert(seed.end(), client_random.begin(), client_random.end());
        auto block = tls12_prf(master_secret, "key expansion", seed,
                                kAesGcmKeyBlockLen);
        if (block.size() < kAesGcmKeyBlockLen) {
            core::log::Logger::instance().error(
                "dtls: derive_traffic_keys: PRF returned short key block");
            return;
        }
        std::memcpy(client_write_key.data(),  block.data(),                       kAesKeyLen);
        std::memcpy(server_write_key.data(),  block.data() +     kAesKeyLen,      kAesKeyLen);
        std::memcpy(client_write_salt.data(), block.data() + 2 * kAesKeyLen,      kAesGcmSaltLen);
        std::memcpy(server_write_salt.data(), block.data() + 2 * kAesKeyLen + kAesGcmSaltLen,
                    kAesGcmSaltLen);
        traffic_keys_ready = true;
        core::log::Logger::instance().info(
            "dtls: traffic keys derived (AES-128-GCM)");
    }

    std::vector<std::uint8_t> compute_handshake_hash_snapshot() {
#if NIMRTC_HAS_BCRYPT
        // Build a fresh SHA-256 over the replay buffer (which excludes
        // Finished itself).  RFC 5246 §7.4.8 requires both sides compute
        // verify_data over the same set of handshake messages.
        return hs_clone_digest();
#else
        return std::vector<std::uint8_t>(32, 0);
#endif
    }

    std::vector<std::uint8_t> make_finished(const char* client_label) {
        // verify_data = PRF(master_secret, label, SHA-256(hs_log))[0..11]
        auto snap = compute_handshake_hash_snapshot();
        return tls12_prf(master_secret,
                         std::string_view(client_label, std::strlen(client_label)),
                         snap, kVerifyDataLen);
    }

    void compute_srtp_keying_material() {
        // Idempotency: same reason as compute_master_secret() — Chrome
        // retransmits while we wait for ACK of CKE+CCS+Finished, and each
        // retransmit would otherwise rewrite srtp_keys against a
        // (potentially stale) master_secret copy.
        if (srtp_keys_ready) return;

        // RFC 5764 §4.2:
        //   key_block = PRF(master, "EXTRACTOR-dtls_srtp",
        //                   client_random || server_random)[0 .. 2*keylen + 2*saltlen]
        std::vector<std::uint8_t> seed;
        seed.reserve(kRandomLen * 2);
        seed.insert(seed.end(), client_random.begin(), client_random.end());
        seed.insert(seed.end(), server_random.begin(), server_random.end());

        std::size_t total = 2 * kSrtpKeyLen + 2 * kSrtpSaltLen;
        auto block = tls12_prf(master_secret, "EXTRACTOR-dtls_srtp", seed, total);

        srtp_keys.profile = config.srtp_profile;
        if (config.role == DtlsRole::Client) {
            std::memcpy(srtp_keys.client_master_key.data(), block.data(),                                      kSrtpKeyLen);
            std::memcpy(srtp_keys.server_master_key.data(), block.data() +     kSrtpKeyLen,                   kSrtpKeyLen);
            std::memcpy(srtp_keys.client_master_salt.data(), block.data() + 2 * kSrtpKeyLen,                   kSrtpSaltLen);
            std::memcpy(srtp_keys.server_master_salt.data(), block.data() + 2 * kSrtpKeyLen + kSrtpSaltLen,    kSrtpSaltLen);
        } else {
            std::memcpy(srtp_keys.server_master_key.data(), block.data(),                                      kSrtpKeyLen);
            std::memcpy(srtp_keys.client_master_key.data(), block.data() +     kSrtpKeyLen,                   kSrtpKeyLen);
            std::memcpy(srtp_keys.server_master_salt.data(), block.data() + 2 * kSrtpKeyLen,                   kSrtpSaltLen);
            std::memcpy(srtp_keys.client_master_salt.data(), block.data() + 2 * kSrtpKeyLen + kSrtpSaltLen,    kSrtpSaltLen);
        }
        srtp_keys.lifetime = 0;
        srtp_keys_ready = true;
    }

    // -------------------------------------------------------------------------
    // Record layer parsing
    // -------------------------------------------------------------------------

    void enqueue_change_cipher_spec(std::uint16_t epoch, std::uint64_t seq) {
        // `seq` is retained as part of the public API for callers that
        // still want to pin a specific record sequence number, but per
        // RFC 6347 §4.1.2.1 record seq must reset to 0 at every epoch
        // boundary, so we use the per-epoch counter instead and ignore
        // the caller's `seq`.  (We keep the parameter for ABI
        // compatibility — the existing call sites passed values that
        // *happened* to match the counter, but the new invariant makes
        // those values wrong by definition.)
        (void)seq;
        const std::size_t epoch_idx = (epoch < 2) ? epoch : 1;
        const std::uint64_t record_seq = record_seq_per_epoch_[epoch_idx]++;
        // DTLS ChangeCipherSpec record (RFC 6347 §4.1.2):
        //   content_type (1) | version (2) | epoch (2) | sequence_number (6) | length (2) | CCS (1)
        std::vector<std::uint8_t> rec;
        rec.resize(14);   // ensure the header bytes exist before write_be16
        rec[0] = kDtlsChangeCipherSpec;
        rec[1] = 0xFE; rec[2] = 0xFD;  // DTLS 1.2
        write_be16(rec.data() + 3, epoch);
        for (int i = 0; i < 6; ++i)
            rec[5 + i] = static_cast<std::uint8_t>(record_seq >> (8 * (5 - i)));
        write_be16(rec.data() + 11, 1);  // CCS payload = 1 byte
        rec[13] = 0x01;

        DtlsRecord r;
        r.bytes = std::move(rec);
        outbound.push_back(std::move(r));
        stats.records_out++;
    }

    void process_record(std::span<const std::uint8_t> bytes,
                        const DtlsAddr& /*from*/) noexcept {
        stats.records_in++;

        // Chrome (and BoringSSL in general) often packs multiple DTLS records
        // into a single UDP datagram — and libjuice is allowed to deliver
        // those concatenated datagrams too.  We must therefore walk through
        // the buffer one record at a time (each prefixed with a 13-byte
        // header), not assume the first 13 bytes cover the whole inbound
        // datagram.  Without this loop, process_handshake() mis-parses the
        // second record as a continuation of the first handshake message and
        // silently bails out at the first byte that doesn't fit its
        // 12-byte-hs-header model — which is exactly the symptom that made
        // Chrome's ServerKeyExchange+ServerHelloDone never reach NimRTC.
        std::size_t off = 0;
        while (off + 13 <= bytes.size()) {
            std::uint8_t ct = bytes[off];
            std::uint16_t epoch = read_be16(bytes.data() + off + 3);
            std::uint16_t len = read_be16(bytes.data() + off + 11);
            // Sanity-check this record fits in the buffer.  If it doesn't,
            // bail out — the leftover bytes (truncated tail of an in-progress
            // DTLS fragment, for instance) are not a separate record.
            if (off + 13 + len > bytes.size()) {
                break;
            }
            std::span<const std::uint8_t> header(bytes.data() + off, 13);
            std::span<const std::uint8_t> payload(bytes.data() + off + 13, len);
            std::span<const std::uint8_t> record(bytes.data() + off, 13 + len);

            dtls_hex_dump("rx_record", record);
            core::log::Logger::instance().info(
                std::string("dtls: process_record ct=") + std::to_string(ct) +
                " epoch=" + std::to_string(epoch) +
                " len=" + std::to_string(len) +
                " hex=" + bytes_to_hex(record));

            process_one_record(ct, epoch, header, payload);
            off += 13 + len;
        }
    }

    void process_one_record(std::uint8_t ct,
                            std::uint16_t epoch,
                            std::span<const std::uint8_t> header,
                            std::span<const std::uint8_t> payload) {
        (void)header;  // used as AAD for AEAD decryption; passed where needed
        switch (ct) {
            case kDtlsHandshakeContentType: {
                // For epoch >= 1 we must AEAD-decrypt the payload.  After
                // decryption we get a sequence of concatenated handshake
                // messages (each with a 12-byte header) which we feed to
                // process_handshake() exactly like an epoch-0 record.
                if (epoch >= 1) {
                    if (!traffic_keys_ready) {
                        core::log::Logger::instance().error(
                            "dtls: epoch>=1 inbound but traffic keys not ready");
                        return;
                    }
                    if (payload.size() < kAesGcmExplicitNonceLen + kAesGcmTagLen) {
                        core::log::Logger::instance().error(
                            "dtls: epoch>=1 inbound too short for nonce+tag");
                        return;
                    }
                    const auto& salt = (config.role == DtlsRole::Server)
                                         ? client_write_salt : server_write_salt;
                    const auto& rkey = (config.role == DtlsRole::Server)
                                         ? client_write_key : server_write_key;
                    std::array<std::uint8_t, kAesGcmExplicitNonceLen> explicit_nonce{};
                    std::memcpy(explicit_nonce.data(), payload.data(),
                                kAesGcmExplicitNonceLen);
                    std::span<const std::uint8_t> ct_and_tag(
                        payload.data() + kAesGcmExplicitNonceLen,
                        payload.size() - kAesGcmExplicitNonceLen);
                    // AAD = record header (13 bytes).  For multi-record
                    // UDP datagrams, process_record() forwards the correct
                    // 13-byte slice for *this* record.
                    std::span<const std::uint8_t> aad(header.data(), 13);
                    auto plain = aes_gcm_open(rkey, salt, explicit_nonce, aad,
                                                ct_and_tag);
                    if (plain.empty()) {
                        core::log::Logger::instance().error(
                            "dtls: AEAD open failed (auth tag mismatch?)");
                        stats.errors++;
                        return;
                    }
                    process_handshake(plain, epoch);
                } else {
                    process_handshake(payload, epoch);
                }
                break;
            }
            case kDtlsChangeCipherSpec:
                // Receiving CCS indicates the peer has transitioned to
                // the new epoch.  Actual record decryption is now done
                // in the epoch>=1 branch above.
                state = (state == DtlsState::Finished) ? DtlsState::Connected : state;
                break;
            case kDtlsAlertContentType: {
                stats.alerts_in++;
                // Resolve alert level/description with safe indexing and
                // emit a single warning line so the proxy/logger pipeline
                // can surface fatal alerts without buffering extra bytes.
                {
                    const std::size_t plen = payload.size();
                    const int level =
                        (plen >= 1) ? static_cast<int>(payload[0]) : 0;
                    const int desc =
                        (plen >= 2) ? static_cast<int>(payload[1]) : -1;
                    static const char* const kLevels[3] = {
                        "?", "warning(1)", "fatal(2)"
                    };
                    static const struct {
                        int code;
                        const char* name;
                    } kDescriptions[] = {
                        {0, "close_notify"}, {10, "unexpected_message"},
                        {20, "bad_record_mac"}, {21, "decryption_failed"},
                        {22, "record_overflow"}, {30, "decompression_failure"},
                        {40, "handshake_failure"}, {41, "no_certificate"},
                        {42, "bad_certificate"}, {43, "unsupported_certificate"},
                        {44, "certificate_revoked"}, {45, "certificate_expired"},
                        {46, "certificate_unknown"}, {47, "illegal_parameter"},
                        {48, "unknown_ca"}, {49, "access_denied"},
                        {50, "decode_error"}, {51, "decrypt_error"},
                        {60, "export_restriction"}, {70, "protocol_version"},
                        {71, "insufficient_security"}, {80, "internal_error"},
                        {90, "user_canceled"}, {100, "no_renegotiation"}
                    };
                    const char* lvl =
                        (level >= 0 && level <= 2) ? kLevels[level] : kLevels[0];
                    const char* descname = "(unknown)";
                    for (const auto& d : kDescriptions) {
                        if (d.code == desc) { descname = d.name; break; }
                    }
                    char hexbuf[64] = {0};
                    int off = 0;
                    for (auto b : payload) {
                        off += std::snprintf(hexbuf + off, sizeof(hexbuf) - off,
                                              "%02X",
                                              static_cast<unsigned>(b));
                        if (static_cast<std::size_t>(off) >= sizeof(hexbuf) - 1)
                            break;
                    }
                    core::log::Logger::instance().warn(
                        std::string("DTLS: alert received level=") + lvl +
                        " description=" + descname +
                        " hex=" + hexbuf);
                }
                if (payload.size() >= 1 && payload[0] == 2) {
                    state = DtlsState::Failed;
                }
                break;
            }
            case kDtlsAppData:
                // Not used pre-handshake; ignore.
                break;
            default:
                break;
        }
    }

    void process_handshake(std::span<const std::uint8_t> bytes,
                            std::uint16_t epoch) {
        std::size_t off = 0;
        while (off < bytes.size()) {
            if (off + 12 > bytes.size()) return;
            std::uint8_t  msg_type   = bytes[off];
            std::uint32_t body_len   = read_be24(bytes.data() + off + 1);
            std::uint16_t msg_seq    = read_be16(bytes.data() + off + 4);
            std::uint32_t frag_off   = read_be24(bytes.data() + off + 6);
            std::uint32_t frag_len   = read_be24(bytes.data() + off + 9);
            (void)msg_seq; (void)epoch;
            if (off + 12 + body_len > bytes.size()) return;
            std::span<const std::uint8_t> body(bytes.data() + off + 12, body_len);
            off += 12 + body_len;

            // Handle single-fragment messages directly.
            if (frag_off == 0 && frag_len == body_len) {
                // Log the full handshake message (header + body) into hs_log
                // BEFORE dispatching, so any subsequent verify_data
                // computation includes the inbound message itself (RFC 5246
                // §7.4.8).
                if (msg_type != kHsFinished) {
                    const std::uint8_t* p = bytes.data() + off - 12 - body_len;
                    hs_update(p, body_len + 12);
                    hs_log.insert(hs_log.end(), p, p + body_len + 12);
                }
                dispatch_handshake(msg_type, body);
            } else {
                // Reassemble.
                if (!pending_hs ||
                    pending_hs->type != msg_type) {
                    pending_hs = PendingHs{};
                    pending_hs->type = msg_type;
                    pending_hs->expected_len = frag_off + frag_len;
                }
                if (body.size() > pending_hs->body.capacity() - pending_hs->body.size())
                    pending_hs->body.resize(pending_hs->expected_len);
                std::memcpy(pending_hs->body.data() + frag_off, body.data(), body.size());

                if (pending_hs->body.size() >= pending_hs->expected_len) {
                    std::vector<std::uint8_t> assembled(
                        pending_hs->body.begin(),
                        pending_hs->body.begin() + pending_hs->expected_len);
                    auto pending = std::move(*pending_hs);
                    pending_hs.reset();
                    dispatch_handshake(pending.type, assembled);
                }
            }
        }
    }

    void dispatch_handshake(std::uint8_t type,
                            std::span<const std::uint8_t> body) {
        core::log::Logger::instance().info(
            std::string("dtls: dispatch type=") + std::to_string(type) +
            " body_len=" + std::to_string(body.size()));
        switch (type) {
            case kHsClientHello:          handle_client_hello(body); break;
            case kHsServerHello:          handle_server_hello(body); break;
            case kHsServerKeyExchange:    handle_server_key_exchange(body); break;
            case kHsClientKeyExchange:    handle_client_key_exchange(body); break;
            case kHsHelloVerify: {
                // Client receives HelloVerifyRequest: re-send ClientHello
                // with the cookie in its body.
                //
                // RFC 6347 §4.1.2.6 (and RFC 6347 §4.5.2 via RFC 5246):
                // a retransmitted flight MUST use the same message_seq
                // values as the original transmission.  Bumping
                // message_seq_counter here collides the retransmit with
                // the seq numbers used by the later CKE/Finished flight,
                // which Chrome's BoringSSL state machine rejects with
                // fatal unexpected_message (alert 10).  Pin the
                // retransmit to seq=0 because the initial ClientHello
                // (see make_client_hello path, line ~3207) was sent at
                // seq=0.
                if (config.role != DtlsRole::Client) break;
                if (body.size() < 4) break;
                std::size_t b2 = 3;  // skip version (2) + cookie_len (1)
                std::uint8_t cl = body[2];
                if (b2 + cl > body.size()) break;
                if (cl <= kHelloCookieLen) {
                    std::memcpy(cookie.data(), body.data() + b2, cl);
                    cookie_len = cl;
                }
                auto ch = make_client_hello(/*with_cookie=*/true);
                constexpr std::uint32_t kRetransmitSeq = 0;
                enqueue_handshake(kHsClientHello, ch, 0, kRetransmitSeq);
                state = DtlsState::HelloSent;
                break;
            }
            case kHsCertificate:
                // Client receives server's Certificate in response to its
                // own CertificateRequest.  We rely on SDP-pinned
                // fingerprint verification above DTLS, not full cert
                // validation — skip the body.  Note: this branch only
                // fires for the CLIENT role; the SERVER's Certificate
                // (which we send during handle_client_hello) hits a
                // different code path entirely.
                break;
            case kHsCertificateRequest: {
                // Peer asked us to authenticate with a client certificate.
                // Chrome ALWAYS sends CertReq in its WebRTC DTLS flight
                // and we MUST respond with a Certificate message.  We
                // try two variants below — non-empty cert + CertVerify,
                // and an empty 3-byte list — depending on what the engine
                // configuration advertises.  The current implementation
                // sends our self-signed ECDSA cert + a SHA-256-ECDSA
                // signature.  We rely on SDP-pinned fingerprint
                // verification above DTLS rather than on full X.509
                // chain validation, so an "anonymous-style" cert
                // (untrusted CA, not in Chrome's trust store) is what
                // we provide; BoringSSL is supposed to accept this
                // when peer-supplied CertVerify is also offered.
                core::log::Logger::instance().info(
                    "dtls: received CertificateRequest; will send "
                    "self-signed ECDSA cert + CertificateVerify");
                received_cert_req = true;
                break;
            }
            case kHsCertificateVerify:
                // Server's CertificateVerify: signature over handshake
                // messages using the server's signing key.  We skip
                // verification — see kHsCertificate above for rationale.
                break;
            case kHsServerHelloDone: {
                // Client transitions to KeyExchange -> sends CKE+CCS+Finished.
                // We compute Finished here so that ServerHelloDone is already
                // in hs_log (RFC 5246 §7.4.8: verify_data hashes messages
                // strictly before the Finished itself).
                if (config.role != DtlsRole::Client) break;
                if (state != DtlsState::KeyExchange &&
                    state != DtlsState::HelloReceived) break;
                // Belt-and-braces guard: Chrome occasionally retransmits
                // its ServerHello/SHD after we've already produced the
                // matching CKE+CCS+Finished flight.  Re-entering this
                // branch would emit a second flight at the same
                // already-allocated msg_seq window and Chrome would
                // immediately reply with unexpected_message.  If we've
                // already promoted ourselves to Finished, drop the
                // duplicate dispatch.
                if (state == DtlsState::Finished ||
                    state == DtlsState::ChangeCipherSpec ||
                    state == DtlsState::Connected) {
                    break;
                }
                // CKE body is just the raw public key bytes for the curve
                // the server picked in its SKE (negotiated_curve).  For
                // X25519 that's 32 bytes of Montgomery u-coord; for P-256
                // that's the 65-byte uncompressed point (RFC 4492 §5.4).
                std::vector<std::uint8_t> cke_body;
                if (negotiated_curve == NegotiatedCurve::X25519) {
                    cke_body.assign(local_x25519_pub_bytes.begin(),
                                    local_x25519_pub_bytes.end());
                } else {
                    cke_body.assign(local_pub_bytes.begin(),
                                    local_pub_bytes.end());
                }
                // RFC 6347 §4.5.2: message_seq is a per-direction counter
                // starting at 0.  The client's outbound flight is:
                //   * ClientHello (initial + HelloVerify-driven
                //     retransmit) at msg_seq=0
                //   * ClientKeyExchange    at msg_seq=1
                //   * Finished             at msg_seq=2
                // Chrome's BoringSSL DTLS state machine enforces strict
                // uniqueness and monotonicity within a single direction.
                // The counter is therefore advanced exactly once per
                // distinct outbound handshake message (not per byte and
                // not per retransmit); the retransmit path above pins
                // its seq to 0 to comply with RFC 6347 §4.1.2.6.
                //
                // Note on CertificateRequest: Chrome's BoringSSL ALWAYS
                // sends a CertReq in its WebRTC DTLS flight but does NOT
                // require us to send a Certificate back.  An anonymous
                // client may reply with an empty Certificate list (or
                // simply omit the Certificate message entirely — both
                // are accepted per RFC 5246 §7.4.6).  We previously tried
                // sending an empty cert list at seq=0, which BoringSSL
                // rejected as unexpected_message because the flight
                // should start with CKE.  The fix: skip the Certificate
                // message entirely and emit CKE first.
                //
                // CCS is a DTLS content-type record, NOT a handshake
                // message — it does not consume a message_seq.  Its
                // record sequence number is taken from the per-epoch
                // counter inside enqueue_change_cipher_spec() (resets to
                // 0 at the epoch boundary per RFC 6347 §4.1.2.1).
                std::uint32_t cke_seq = ++message_seq_counter;
                enqueue_handshake(kHsClientKeyExchange, cke_body, /*epoch=*/0,
                                  /*seq=*/cke_seq);
                enqueue_change_cipher_spec(/*epoch=*/1, /*seq=*/0);
                // IMPORTANT: derive master_secret + traffic keys + srtp
                // keys AFTER CKE has been added to hs_log (enqueue_handshake
                // appends every non-Finished handshake message to hs_log).
                // Without this ordering the EMS seed for master_secret
                // would exclude CKE, producing a master_secret different
                // from Chrome's and the Finished verify_data would never
                // match.
                derive_session_secrets_idempotent();
                std::uint32_t fin_seq = ++message_seq_counter;
                enqueue_handshake(kHsFinished,
                                  make_finished("client finished"),
                                  /*epoch=*/1, /*seq=*/fin_seq);
                // message_seq_counter is now at the next free seq (which
                // is, for the standard client flight, the value 2).  We
                // leave it incremented; do not reset it here — the
                // ++message_seq_counter calls above are the single source
                // of truth for outbound msg_seq allocation.
                // Note: do NOT mark Connected yet — we still have to
                // receive the Server's Finished (with valid verify_data)
                // before the handshake is complete.  The Finished case
                // below promotes us from Finished -> ChangeCipherSpec ->
                // Connected once Chrome's verify_data check passes.
                state = DtlsState::Finished;
                break;
            }
            case kHsFinished: {
                // RFC 5246 §7.4.8: verify the 12-byte verify_data against
                // our own PRF(master_secret, finished_label, SHA-256(hs_log))
                // computation.  The label depends on who sent the Finished:
                // when we are the server we expect "client finished", and
                // when we are the client we expect "server finished".
                if (body.size() < kVerifyDataLen) {
                    core::log::Logger::instance().error(
                        "DTLS: Finished body too short");
                    state = DtlsState::Failed;
                    stats.errors++;
                    break;
                }
                const char* expected_label =
                    (config.role == DtlsRole::Client) ? "server finished"
                                                      : "client finished";
                auto expected = make_finished(expected_label);
                // Always emit the 24-byte expected/got pair to the trace
                // file (when NIMRTC_DTLS_TRACE is set) so the user can
                // see exactly which 12 bytes disagree — even when the
                // mismatch is silent (peer rejects with a generic alert
                // before our state machine reaches this branch).
                if (static const std::string trace_path =
                        get_env_or_empty("NIMRTC_DTLS_TRACE");
                    !trace_path.empty()) {
                    static std::once_flag init_flag;
                    std::call_once(init_flag, [&]() {
                        std::ofstream t(trace_path,
                                         std::ios::binary | std::ios::trunc);
                    });
                    std::ofstream tf(trace_path, std::ios::binary | std::ios::app);
                    if (tf.is_open()) {
                        tf << "[peer's Finished incoming]\n"
                           << "expected(" << expected_label << "): "
                           << hexline(expected) << "\n"
                           << "got("      << expected_label << "):      "
                           << hexline(body) << "\n";
                        tf.flush();
                    }
                }
                if (expected.size() != kVerifyDataLen ||
                    !std::equal(expected.begin(), expected.end(),
                                body.begin())) {
                    core::log::Logger::instance().error(
                        "DTLS: verify_data mismatch");
                    state = DtlsState::Failed;
                    stats.errors++;
                    break;
                }
                core::log::Logger::instance().info(
                    std::string("DTLS: verify_data OK (") +
                    expected_label + ")");
                // Promotion ladder: after we accept the peer's Finished,
                // we transition through the remaining states to Connected.
                // (KeyExchange -> Finished [we sent ours] -> Connected
                // here in the client role; or HelloReceived -> KeyExchange
                // -> Finished -> Connected in the server role.)
                if (state == DtlsState::HelloSent ||
                    state == DtlsState::HelloReceived ||
                    state == DtlsState::KeyExchange ||
                    state == DtlsState::Finished ||
                    state == DtlsState::ChangeCipherSpec) {
                    state = DtlsState::Connected;
                    core::log::Logger::instance().info(
                        std::string("DTLS: handshake COMPLETE, state=Connected"));
                }
                break;
            }
            default:
                break;
        }
    }

    std::uint32_t message_seq_counter = 0;

    // Per-epoch record sequence counter.  RFC 6347 §4.1.2.1: the
    // record sequence number MUST reset to 0 at every epoch boundary.
    // We keep a small per-epoch table indexed by epoch (currently only
    // 0 and 1 are used; the table is fixed-size to avoid heap churn in
    // the hot record-emit path).  Previously NimRTC used a single
    // `message_seq_counter` for both the handshake header's message_seq
    // AND the record-layer sequence_number — which works for the epoch-0
    // client flight (where they happen to coincide) but breaks for the
    // epoch-1 CCS+Finished flight because the record sequence numbers
    // there must start at 0, not at the running handshake msg_seq value.
    std::uint64_t record_seq_per_epoch_[2] = {0, 0};

    // -------------------------------------------------------------------------
    // Convenience: hs_update with pointer + size
    // -------------------------------------------------------------------------
    void hs_update(const std::uint8_t* ptr, std::size_t n) {
        hs_update(std::span<const std::uint8_t>(ptr, n));
    }
};

// ===========================================================================
// DtlsSession public interface
// ===========================================================================

DtlsSession::DtlsSession(Config cfg)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(cfg);
}

DtlsSession::~DtlsSession() {
    close();
}

DtlsSession::DtlsSession(DtlsSession&&) noexcept = default;
DtlsSession& DtlsSession::operator=(DtlsSession&&) noexcept = default;

core::Result<void> DtlsSession::open() noexcept {
    if (!impl_->init_crypto()) {
        impl_->state = DtlsState::Failed;
        return core::Result<void>::fail(core::ErrorCode::InternalError,
                                         "DTLS: crypto init failed");
    }
    impl_->compute_local_fingerprint();
    impl_->hs_start_ms = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

#if NIMRTC_HAS_BCRYPT
    auto rnd = bcrypt_random(kRandomLen);
    if (rnd.size() == kRandomLen) {
        std::memcpy(impl_->client_random.data(), rnd.data(), kRandomLen);
    }
#endif

    impl_->state = DtlsState::Initial;

    if (impl_->config.role == DtlsRole::Client) {
        auto ch = impl_->make_client_hello(/*with_cookie=*/false);
        impl_->enqueue_handshake(kHsClientHello, ch, 0, 0);
        impl_->state = DtlsState::HelloSent;
    }
    // Server waits for inbound ClientHello
    return core::Result<void>::make_ok();
}

void DtlsSession::close() noexcept {
    if (impl_) {
        impl_->teardown_crypto();
        impl_->state = DtlsState::Closed;
    }
}

std::size_t DtlsSession::feed_inbound(std::span<const std::uint8_t> bytes,
                                       const DtlsAddr& from) noexcept {
    if (!impl_) return 0;
    impl_->process_record(bytes, from);
    return bytes.size();
}

std::vector<DtlsRecord> DtlsSession::take_outbound() noexcept {
    if (!impl_) return {};
    auto out = std::move(impl_->outbound);
    impl_->outbound.clear();
    return out;
}

const Fingerprint& DtlsSession::local_fingerprint() const noexcept {
    return impl_->local_fp;
}

void DtlsSession::set_peer_fingerprint(std::string algo,
                                         std::vector<std::uint8_t> value) noexcept {
    if (!impl_) return;
    impl_->config.peer_fingerprint_algo  = std::move(algo);
    impl_->config.peer_fingerprint_value = std::move(value);
}

void DtlsSession::set_role(DtlsRole r) noexcept {
    if (!impl_) return;
    const DtlsRole prev = impl_->config.role;
    impl_->config.role = r;
    if (prev == r) return;
    // Only act if the handshake hasn't already started past the initial
    // ClientHello/HelloVerify exchange.  After that, changing role mid-handshake
    // would corrupt the state machine; callers must set role before ICE/DTLS
    // starts sending records.
    if (impl_->state == DtlsState::Closed ||
        impl_->state == DtlsState::Initial) {
        if (r == DtlsRole::Client) {
            // Promote to Client: enqueue an initial ClientHello.
            auto ch = impl_->make_client_hello(/*with_cookie=*/false);
            impl_->enqueue_handshake(kHsClientHello, ch, 0, 0);
            impl_->state = DtlsState::HelloSent;
        } else {
            // Demote to Server: discard any pending ClientHello and wait for
            // inbound ClientHello.
            impl_->outbound.clear();
            impl_->state = DtlsState::Initial;
        }
    }
}

std::optional<SrtpKeyingMaterial>
DtlsSession::srtp_keying_material() const noexcept {
    if (!impl_ || impl_->state != DtlsState::Connected) return std::nullopt;
    return impl_->srtp_keys;
}

DtlsSession::Stats DtlsSession::stats() const noexcept {
    return impl_ ? impl_->stats : Stats{};
}

DtlsState DtlsSession::state() const noexcept {
    return impl_ ? impl_->state : DtlsState::Closed;
}

bool DtlsSession::is_connected() const noexcept {
    return state() == DtlsState::Connected;
}

const char* DtlsSession::state_name(DtlsState s) noexcept {
    switch (s) {
        case DtlsState::Closed:             return "Closed";
        case DtlsState::Initial:            return "Initial";
        case DtlsState::HelloVerify:        return "HelloVerify";
        case DtlsState::HelloSent:          return "HelloSent";
        case DtlsState::HelloReceived:      return "HelloReceived";
        case DtlsState::CertificateReceived: return "CertificateReceived";
        case DtlsState::KeyExchange:        return "KeyExchange";
        case DtlsState::ChangeCipherSpec:   return "ChangeCipherSpec";
        case DtlsState::Finished:           return "Finished";
        case DtlsState::Connected:          return "Connected";
        case DtlsState::Failed:             return "Failed";
    }
    return "Unknown";
}

} // namespace nimrtc::dtls

