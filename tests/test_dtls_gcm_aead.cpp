// test_dtls_gcm_aead.cpp — verify BCrypt AES-128-GCM round-trip with our
// exact (salt, nonce, AAD, tag-length) layout so we can isolate whether
// the GCM impl actually opens what NimRTC's seal emits.

// NOTE: nimrtc_apply_options() already adds _CRT_SECURE_NO_WARNINGS via
// target_compile_definitions(); do NOT #define it here or MSVC warns
// "macro redefinition" which /WX promotes to a hard error.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <vector>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(x) (((NTSTATUS)(x)) >= 0)
#endif

static void hex(const char* tag, const uint8_t* data, size_t n) {
    std::printf("%s ", tag);
    for (size_t i = 0; i < n; ++i) std::printf("%02X", data[i]);
    std::printf("\n");
}

int main() {
    std::printf("=== BCrypt AES-128-GCM round-trip test ===\n");
    std::fflush(stdout);

    NTSTATUS s;
    BCRYPT_ALG_HANDLE alg = nullptr;
    s = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!NT_SUCCESS(s)) { std::printf("BCryptOpenAlgorithmProvider failed: 0x%08lX\n", s); return 1; }

    // Set chain mode to GCM
    s = BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                          (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    std::printf("SetProperty(GCM chain): 0x%08lX\n", s);
    std::fflush(stdout);

    // Try 16-byte tag — Windows 10 BCrypt rejects this (STATUS_NOT_SUPPORTED)
    // and the GCM provider is locked to 12-byte tag.
    {
        DWORD t16 = 16;
        NTSTATUS s16 = BCryptSetProperty(alg, BCRYPT_AUTH_TAG_LENGTH,
                                          reinterpret_cast<PUCHAR>(&t16), sizeof(t16), 0);
        std::printf("SetProperty(tag=16): 0x%08lX  (expect 0xC00000BB)\n", s16);
        std::fflush(stdout);
    }

    // 12-byte tag is the BCrypt default.
    {
        DWORD t12 = 12;
        NTSTATUS s12 = BCryptSetProperty(alg, BCRYPT_AUTH_TAG_LENGTH,
                                          reinterpret_cast<PUCHAR>(&t12), sizeof(t12), 0);
        std::printf("SetProperty(tag=12): 0x%08lX\n", s12);
        std::fflush(stdout);
    }

    // Generate key
    BCRYPT_KEY_HANDLE key = nullptr;
    uint8_t key_bytes[16];
    for (int i = 0; i < 16; ++i) key_bytes[i] = (uint8_t)(i + 1);
    s = BCryptGenerateSymmetricKey(alg, &key, nullptr, 0, key_bytes, 16, 0);
    if (!NT_SUCCESS(s)) { std::printf("BCryptGenerateSymmetricKey failed: 0x%08lX\n", s); return 1; }

    // 4-byte salt + 8-byte explicit_nonce = 12-byte IV
    uint8_t iv[12];
    for (int i = 0; i < 4; ++i) iv[i] = (uint8_t)(0xA0 + i);
    for (int i = 0; i < 8; ++i) iv[4 + i] = (uint8_t)(0xB0 + i);

    uint8_t plaintext[24];
    for (int i = 0; i < 24; ++i) plaintext[i] = (uint8_t)(0x40 + i);
    uint8_t aad[13];
    for (int i = 0; i < 13; ++i) aad[i] = (uint8_t)(0x80 + i);

    // Encrypt with cbTag=12 (BCrypt default).
    uint8_t tag[16] = {0};
    uint8_t ct[64] = {0};

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
    info.cbSize = sizeof(info);
    info.dwInfoVersion = BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO_VERSION;
    info.pbNonce = iv;
    info.cbNonce = 12;
    info.pbAuthData = aad;
    info.cbAuthData = 13;
    info.pbTag = tag;
    info.cbTag = 12;

    ULONG written = 0;
    s = BCryptEncrypt(key, plaintext, 24, &info,
                      nullptr, 0,
                      ct, sizeof(ct), &written, 0);
    std::printf("Encrypt(tag=12) written=%lu status=0x%08lX\n", written, s);
    std::fflush(stdout);
    hex("tag (12B from pbTag): ", tag, 12);
    hex("ct  (first 24 bytes): ", ct, 24);
    if (written == 36) std::printf("  -> Form B: ct(24) || tag(12) inline in pbOutput\n");
    else if (written == 24) std::printf("  -> Form A: ct in pbOutput, tag in pbTag\n");
    std::fflush(stdout);

    // Build NimRTC-style on-wire payload: ct (24) + real_tag (12) + 4-byte zero pad = 40 bytes
    std::vector<uint8_t> ciphertext_and_tag;
    ciphertext_and_tag.reserve(40);
    ciphertext_and_tag.insert(ciphertext_and_tag.end(), ct, ct + 24);
    ciphertext_and_tag.insert(ciphertext_and_tag.end(), tag, tag + 12);
    ciphertext_and_tag.insert(ciphertext_and_tag.end(), 4, 0);
    std::printf("\non-wire ciphertext_and_tag.size()=%zu (expect 40)\n",
                ciphertext_and_tag.size());
    std::fflush(stdout);

    // Now decrypt using NimRTC's exact logic: take last 16 bytes as tag_storage,
    // pass first 12 to BCrypt via cbTag=12.
    constexpr size_t kOnWireTagLen = 16;
    constexpr size_t kBcryptGcmTagLen = 12;
    size_t ct_len = ciphertext_and_tag.size() - kOnWireTagLen;
    std::vector<uint8_t> ct_open(ct_len);
    std::memcpy(ct_open.data(), ciphertext_and_tag.data(), ct_len);
    std::vector<uint8_t> tag_storage(kOnWireTagLen);
    std::memcpy(tag_storage.data(),
                ciphertext_and_tag.data() + ct_len, kOnWireTagLen);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO dinfo{};
    dinfo.cbSize = sizeof(dinfo);
    dinfo.dwInfoVersion = BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO_VERSION;
    dinfo.pbNonce = iv;
    dinfo.cbNonce = 12;
    dinfo.pbAuthData = aad;
    dinfo.cbAuthData = 13;
    dinfo.pbTag = tag_storage.data();
    dinfo.cbTag = kBcryptGcmTagLen;

    uint8_t plain[64] = {0};
    ULONG pwritten = 0;
    // Pass ct (24 bytes) — tag is in pbTag.
    s = BCryptDecrypt(key, ct_open.data(), (ULONG)ct_len, &dinfo,
                      nullptr, 0,
                      plain, sizeof(plain), &pwritten, 0);
    std::printf("\nDecrypt(ct=24, cbTag=12, tag_storage[0..12]) "
                "written=%lu status=0x%08lX\n", pwritten, s);
    std::fflush(stdout);
    hex("plain:  ", plain, pwritten);
    std::fflush(stdout);

    // Sanity: compare decrypted plaintext against the original plaintext.
    bool match = (pwritten == 24)
                 && (std::memcmp(plain, plaintext, 24) == 0);
    std::printf("\nROUND-TRIP MATCH: %s\n", match ? "YES" : "NO");
    std::fflush(stdout);

    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(alg, 0);
    return match ? 0 : 1;
}