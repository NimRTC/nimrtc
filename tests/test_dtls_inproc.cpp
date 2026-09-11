// test_dtls_inproc.cpp — same-process seal/open round-trip with the exact
// NimRTC aes_gcm_seal / aes_gcm_open implementation, to isolate whether
// the AEAD primitives themselves are broken (vs. e.g. wrong key derivation
// between two engines).
//
// Windows-only: this test drives BCrypt's AES-GCM directly to validate the
// expected tag layout.  On Linux the same coverage is provided by
// test_engine_plugin_loading and the wolfssl-backed dtls session tests.

#ifdef _WIN32

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <vector>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

namespace nimrtc {

constexpr std::size_t   kAesKeyLen         = 16;
constexpr std::size_t   kAesGcmSaltLen     = 4;
constexpr std::size_t   kAesGcmExplicitNonceLen = 8;
constexpr std::size_t   kAesGcmTagLen      = 16;
constexpr std::size_t   kBcryptGcmTagLen   = 12;

struct BcryptAesKey {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    ~BcryptAesKey() {
        if (key) ::BCryptDestroyKey(key);
        if (alg) ::BCryptCloseAlgorithmProvider(alg, 0);
    }
};

bool bcrypt_import_aes_gcm_key(const uint8_t* key_bytes, size_t klen, BcryptAesKey* out) {
    NTSTATUS s = ::BCryptOpenAlgorithmProvider(&out->alg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(s)) return false;
    s = ::BCryptSetProperty(out->alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                            sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (!BCRYPT_SUCCESS(s)) { ::BCryptCloseAlgorithmProvider(out->alg, 0); out->alg=nullptr; return false; }
    s = ::BCryptGenerateSymmetricKey(out->alg, &out->key, nullptr, 0,
                                     const_cast<PUCHAR>(key_bytes), (ULONG)klen, 0);
    if (!BCRYPT_SUCCESS(s)) { ::BCryptCloseAlgorithmProvider(out->alg, 0); out->alg=nullptr; return false; }
    return true;
}

std::vector<uint8_t> aes_gcm_seal(const uint8_t* key, size_t klen,
                                    const uint8_t* salt, const uint8_t* nonce,
                                    const uint8_t* aad, size_t aad_len,
                                    const uint8_t* pt, size_t pt_len) {
    BcryptAesKey bk;
    if (!bcrypt_import_aes_gcm_key(key, klen, &bk)) return {};

    uint8_t iv[kAesGcmSaltLen + kAesGcmExplicitNonceLen];
    std::memcpy(iv, salt, kAesGcmSaltLen);
    std::memcpy(iv + kAesGcmSaltLen, nonce, kAesGcmExplicitNonceLen);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
    info.cbSize = sizeof(info);
    info.dwInfoVersion = BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO_VERSION;
    info.pbNonce = iv;
    info.cbNonce = sizeof(iv);
    info.pbAuthData = const_cast<PUCHAR>(aad);
    info.cbAuthData = (ULONG)aad_len;
    std::vector<uint8_t> tag(kBcryptGcmTagLen, 0);
    info.pbTag = tag.data();
    info.cbTag = kBcryptGcmTagLen;

    std::vector<uint8_t> ct(pt_len + kAesGcmTagLen, 0);
    ULONG written = 0;
    NTSTATUS s = ::BCryptEncrypt(bk.key, const_cast<PUCHAR>(pt), (ULONG)pt_len,
                                 &info, nullptr, 0,
                                 ct.data(), (ULONG)ct.size(), &written, 0);
    if (!BCRYPT_SUCCESS(s)) {
        std::printf("seal BCryptEncrypt failed 0x%08lX\n", (unsigned long)s);
        return {};
    }
    std::size_t ct_written;
    if (written == pt_len) ct_written = written;
    else if (written == pt_len + kBcryptGcmTagLen) ct_written = pt_len;
    else if (written == pt_len + kAesGcmTagLen) ct_written = pt_len;
    else {
        std::printf("seal unexpected written=%lu pt=%zu\n", written, pt_len);
        return {};
    }
    std::vector<uint8_t> out;
    out.reserve(ct_written + kAesGcmTagLen);
    out.insert(out.end(), ct.begin(), ct.begin() + ct_written);
    out.insert(out.end(), tag.begin(), tag.begin() + kBcryptGcmTagLen);
    out.insert(out.end(), kAesGcmTagLen - kBcryptGcmTagLen, 0);
    return out;
}

std::vector<uint8_t> aes_gcm_open(const uint8_t* key, size_t klen,
                                    const uint8_t* salt, const uint8_t* nonce,
                                    const uint8_t* aad, size_t aad_len,
                                    const uint8_t* ct_and_tag, size_t total_len) {
    if (total_len < kAesGcmTagLen) return {};
    BcryptAesKey bk;
    if (!bcrypt_import_aes_gcm_key(key, klen, &bk)) return {};

    uint8_t iv[kAesGcmSaltLen + kAesGcmExplicitNonceLen];
    std::memcpy(iv, salt, kAesGcmSaltLen);
    std::memcpy(iv + kAesGcmSaltLen, nonce, kAesGcmExplicitNonceLen);

    constexpr std::size_t kOnWireTagLen = 16;
    size_t ct_len = total_len - kOnWireTagLen;
    std::vector<uint8_t> ct(ct_len);
    std::memcpy(ct.data(), ct_and_tag, ct_len);
    std::vector<uint8_t> tag_storage(kOnWireTagLen);
    std::memcpy(tag_storage.data(), ct_and_tag + ct_len, kOnWireTagLen);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
    info.cbSize = sizeof(info);
    info.dwInfoVersion = BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO_VERSION;
    info.pbNonce = iv;
    info.cbNonce = sizeof(iv);
    info.pbAuthData = const_cast<PUCHAR>(aad);
    info.cbAuthData = (ULONG)aad_len;
    info.pbTag = tag_storage.data();
    info.cbTag = kBcryptGcmTagLen;

    std::vector<uint8_t> out(ct_len);
    ULONG written = 0;
    NTSTATUS s = ::BCryptDecrypt(bk.key, ct.data(), (ULONG)ct_len, &info,
                                  nullptr, 0,
                                  out.data(), (ULONG)out.size(), &written, 0);
    if (!BCRYPT_SUCCESS(s)) {
        std::printf("open BCryptDecrypt failed 0x%08lX\n", (unsigned long)s);
        return {};
    }
    out.resize(written);
    return out;
}

} // namespace

static void hex(const char* tag, const uint8_t* data, size_t n) {
    std::printf("%s ", tag);
    for (size_t i = 0; i < n; ++i) std::printf("%02X", data[i]);
    std::printf("\n");
}

int main() {
    // Simulate a Finished record (24 bytes plaintext) with both endpoints
    // using the same key/salt/nonce — this should be a perfect round-trip.
    uint8_t key[16];
    for (int i = 0; i < 16; ++i) key[i] = (uint8_t)(i + 1);
    uint8_t salt[4]   = {0xAA, 0xBB, 0xCC, 0xDD};
    uint8_t nonce[8]  = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    uint8_t aad[13];
    for (int i = 0; i < 13; ++i) aad[i] = (uint8_t)(0x80 + i);
    uint8_t pt[24];
    for (int i = 0; i < 24; ++i) pt[i] = (uint8_t)(0x40 + i);

    auto sealed = nimrtc::aes_gcm_seal(key, 16, salt, nonce, aad, 13, pt, 24);
    std::printf("sealed size=%zu\n", sealed.size());
    hex("sealed:", sealed.data(), sealed.size());

    auto opened = nimrtc::aes_gcm_open(key, 16, salt, nonce, aad, 13,
                                          sealed.data(), sealed.size());
    std::printf("opened size=%zu\n", opened.size());
    hex("opened:", opened.data(), opened.size());

    bool match = opened.size() == 24 && std::memcmp(opened.data(), pt, 24) == 0;
    std::printf("\nROUND-TRIP MATCH: %s\n", match ? "YES" : "NO");
    std::fflush(stdout);
    return match ? 0 : 1;
}

#else  // !_WIN32

#include <cstdio>

int main() {
    std::printf("[test_dtls_inproc] skipped on Linux (Windows-only BCrypt test).\n");
    return 0;
}

#endif  // _WIN32