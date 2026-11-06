# third_party/gmssl/CMakeLists.txt
# =============================================================================
# GmSSL v3.x — SM2/SM3/SM4 crypto + TLCP DTLS backend (v1.1 X1, ADR-013).
#
# Populated by: git submodule update --init src/third_party/gmssl/src
# Upstream: https://github.com/guanzhi/GmSSL.git, pinned at v3.2.0
# (see src/third_party/vendor.json + src/third_party/SOURCE_VERSIONS).
# License: Apache-2.0 (upstream LICENSE file verified 2026-11-05).
#
# This wrapper is active ONLY when NIMRTC_ENABLE_DTLS_GMSSL=ON
# (default OFF).  When OFF, only an empty INTERFACE stub exists so the
# default build pays zero cost (no extra compile time, no extra binary
# size for non-GM users).
#
# Integration note: upstream's own CMakeLists registers ~30 test
# executables via add_test() unconditionally and there is no option to
# turn them off; entering it via add_subdirectory() would pollute
# NimRTC's ctest run, and shadowing add_test()/install() with function()
# would leak globally (CMake function definitions are global, not
# directory-scoped) and break every other test in the tree.  So this
# wrapper does NOT enter the upstream build at all: it compiles the
# upstream sources directly into a STATIC nimrtc_vendor_gmssl library,
# mirroring the upstream default feature set (every ENABLE_* option at
# its upstream default: SM4 modes / SM9 / CMS / SECP256R1 / LMS / XMSS /
# SPHINCS / KYBER / SHA1 / SHA2 / AES / ChaCha20 / ZUC / GHASH / TLS /
# SKF / SDF on; arch-specific ASM and debug options off).  The
# supported test for this backend is tests/test_dtls_gmssl_tlcp.
# =============================================================================

if(NOT NIMRTC_ENABLE_DTLS_GMSSL)
    if(NOT TARGET nimrtc_vendor_gmssl)
        add_library(nimrtc_vendor_gmssl INTERFACE)
        add_library(nimrtc::vendor::gmssl ALIAS nimrtc_vendor_gmssl)
    endif()
    return()
endif()

set(_gmssl_root "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(NOT EXISTS "${_gmssl_root}/CMakeLists.txt")
    message(FATAL_ERROR
        "NIMRTC_ENABLE_DTLS_GMSSL=ON but src/third_party/gmssl/src is not populated.\n"
        "  Run: git submodule update --init src/third_party/gmssl/src")
endif()

# Mirror of the upstream default source set (see src/CMakeLists.txt in
# the submodule: `set(src ...)` plus the list(APPEND src ...) blocks
# for every option whose upstream default is ON).  Arch-specific ASM
# files (.S / _arm64 / _avx2 / _aesni / _sse / _ce / _cl) are excluded:
# all arch ASM options default to OFF upstream.
set(_gmssl_sources
    src/version.c
    src/debug.c
    src/sm4.c
    src/sm4_cbc.c
    src/sm4_ctr.c
    src/sm4_gcm.c
    src/sm4_ecb.c
    src/sm4_ofb.c
    src/sm4_cfb.c
    src/sm4_ccm.c
    src/sm4_xts.c
    src/sm4_cbc_mac.c
    src/sm3.c
    src/sm3_hmac.c
    src/sm3_kdf.c
    src/sm3_pbkdf2.c
    src/sm3_digest.c
    src/sm2_z256.c
    src/sm2_z256_table.c
    src/sm2_key.c
    src/sm2_sign.c
    src/sm2_enc.c
    src/sm2_exch.c
    src/block_cipher.c
    src/digest.c
    src/hmac.c
    src/hkdf.c
    src/pbkdf2.c
    src/gf128.c
    src/ghash.c
    src/sm4_cbc_sm3_hmac.c
    src/sm4_ctr_sm3_hmac.c
    src/pkcs8.c
    src/ec.c
    src/asn1.c
    src/hex.c
    src/base64.c
    src/pem.c
    src/x509_alg.c
    src/x509_cer.c
    src/x509_ext.c
    src/x509_vrf.c
    src/x509_req.c
    src/x509_crl.c
    src/x509_new.c
    src/ocsp.c
    src/x509_key.c
    src/rsa.c
    src/socket.c
    src/file.c
    src/sm9_z256.c
    src/sm9_z256_table.c
    src/sm9_key.c
    src/sm9_sign.c
    src/sm9_enc.c
    src/sm9_exch.c
    src/cms.c
    src/bn.c
    src/secp256r1.c
    src/secp256r1_key.c
    src/ecdsa.c
    src/ecdh.c
    src/lms.c
    src/xmss.c
    src/sphincs.c
    src/kyber.c
    src/sha1.c
    src/sha256.c
    src/sha512.c
    src/sha256_hmac.c
    src/aes.c
    src/aes_modes.c
    src/chacha20.c
    src/zuc.c
    src/zuc_modes.c
    src/tls.c
    src/tls_cert.c
    src/tls_alpn.c
    src/tls_ext.c
    src/tls_psk.c
    src/tls_sni.c
    src/sct.c
    src/tls_sct.c
    src/tls_ocsp.c
    src/tls_cookie.c
    src/tls_trace.c
    src/tls_vrf.c
    src/tlcp.c
    src/tls12.c
    src/tls13.c
    src/skf/skf.c
    src/skf/skf_lib.c
    src/skf/skf_meth.c
    src/skf/skf_ext.c
    src/skf/skf_prn.c
    src/skf/skf_wisec.c
    src/sdf/sdf.c
    src/sdf/sdf_lib.c
    src/sdf/sdf_meth.c
    src/sdf/sdf_ext.c
    src/sdf/sdf_sansec.c
)

# Platform randomness + HTTP source, mirroring the upstream selection:
# WIN32 -> rand_win + http_win; APPLE -> rand_apple + http; otherwise
# getentropy(3) decides between rand_unix and rand.
if(WIN32)
    list(APPEND _gmssl_sources src/rand_win.c src/http_win.c)
elseif(APPLE)
    list(APPEND _gmssl_sources src/rand_apple.c src/http.c)
else()
    include(CheckSymbolExists)
    check_symbol_exists(getentropy "unistd.h" _gmssl_have_getentropy)
    if(_gmssl_have_getentropy)
        list(APPEND _gmssl_sources src/rand_unix.c src/http.c)
    else()
        list(APPEND _gmssl_sources src/rand.c src/http.c)
    endif()
    unset(_gmssl_have_getentropy CACHE)
endif()

# Prefix with the submodule root (the paths above are relative to it,
# matching the layout of the upstream build).
list(TRANSFORM _gmssl_sources PREPEND "${_gmssl_root}/")

add_library(nimrtc_vendor_gmssl STATIC ${_gmssl_sources})
add_library(nimrtc::vendor::gmssl ALIAS nimrtc_vendor_gmssl)

# Upstream default feature defines (the add_definitions(-DENABLE_*)
# lines for every option that defaults to ON upstream).  PUBLIC so
# consumer TUs see the same guarded declarations (sm4.h modes,
# x509_key.h algorithm slots, digest.h/block_cipher.h families).
target_compile_definitions(nimrtc_vendor_gmssl PUBLIC
    ENABLE_SM4_ECB
    ENABLE_SM4_OFB
    ENABLE_SM4_CFB
    ENABLE_SM4_CCM
    ENABLE_SM4_XTS
    ENABLE_SM4_CBC_MAC
    ENABLE_SM9
    ENABLE_CMS
    ENABLE_SECP256R1
    ENABLE_LMS
    ENABLE_XMSS
    ENABLE_XMSS_CROSSCHECK
    ENABLE_SPHINCS
    ENABLE_SPHINCS_CROSSCHECK
    ENABLE_KYBER
    ENABLE_SHA1
    ENABLE_SHA2
    ENABLE_AES
    ENABLE_AES_CCM
    ENABLE_ZUC
    ENABLE_GHASH
    ENABLE_TLS
    ENABLE_ASM_UNDERSCORE_PREFIX
    ENABLE_SKF
    ENABLE_SDF
)
if(WIN32)
    # Upstream socket.h selects its Windows branch on the `WIN32`
    # preprocessor macro (no underscore), which MSVC does not predefine;
    # define it here so both the vendored sources and consumer TUs see
    # the SOCKET-based typedefs.
    target_compile_definitions(nimrtc_vendor_gmssl PUBLIC
        WIN32
        _WINSOCK_DEPRECATED_NO_WARNINGS)
endif()

# SYSTEM: upstream headers must not be subject to NimRTC's -Werror set.
target_include_directories(nimrtc_vendor_gmssl SYSTEM PUBLIC
    $<BUILD_INTERFACE:${_gmssl_root}/include>)
target_include_directories(nimrtc_vendor_gmssl PRIVATE
    ${_gmssl_root}/src)

if(MSVC)
    target_compile_options(nimrtc_vendor_gmssl PRIVATE /utf-8 /FS)
    target_compile_definitions(nimrtc_vendor_gmssl PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(WIN32)
    target_link_libraries(nimrtc_vendor_gmssl PUBLIC Ws2_32)
elseif(APPLE)
    target_link_libraries(nimrtc_vendor_gmssl PUBLIC "-framework Security")
    # SDF sources are always compiled (upstream ENABLE_SDF defaults ON).
    target_link_libraries(nimrtc_vendor_gmssl PUBLIC dl)
else()
    target_link_libraries(nimrtc_vendor_gmssl PUBLIC dl)
endif()

# Consumer-facing flags (mirrors nimrtc_link_gmssl vendored branch).
target_compile_definitions(nimrtc_vendor_gmssl PUBLIC
    NIMRTC_USE_GMSSL=1
    NIMRTC_HAS_DTLS_GMSSL=1)

# `gmssl` CLI (EXCLUDE_FROM_ALL): tests/test_dtls_gmssl_tlcp.cpp shells
# out to `gmssl sm2keygen` + `gmssl certgen` for SM2 fixtures, so the CLI
# must be buildable from the vendored tree.  Source list mirrors the
# upstream `tools` set at default options (base list + every
# list(APPEND tools ...) block whose option defaults to ON upstream).
set(_gmssl_cli_sources
    tools/gmssl.c
    tools/version.c
    tools/sm4.c
    tools/sm4_cbc.c
    tools/sm4_ctr.c
    tools/sm4_gcm.c
    tools/sm4_cbc_mac.c
    tools/sm4_cbc_sm3_hmac.c
    tools/sm4_ctr_sm3_hmac.c
    tools/sm3.c
    tools/sm3hmac.c
    tools/sm3_pbkdf2.c
    tools/sm2keygen.c
    tools/sm2sign.c
    tools/sm2verify.c
    tools/sm2encrypt.c
    tools/sm2decrypt.c
    tools/rand.c
    tools/certgen.c
    tools/certparse.c
    tools/certverify.c
    tools/certrevoke.c
    tools/ocspreq.c
    tools/ocspget.c
    tools/ocspsign.c
    tools/ocspverify.c
    tools/reqgen.c
    tools/reqparse.c
    tools/reqsign.c
    tools/crlgen.c
    tools/crlget.c
    tools/crlparse.c
    tools/crlverify.c
    tools/sm4_ecb.c
    tools/sm4_ofb.c
    tools/sm4_cfb.c
    tools/sm4_ccm.c
    tools/sm4_xts.c
    tools/sm9setup.c
    tools/sm9keygen.c
    tools/sm9sign.c
    tools/sm9verify.c
    tools/sm9encrypt.c
    tools/sm9decrypt.c
    tools/cmssign.c
    tools/cmsverify.c
    tools/cmsencrypt.c
    tools/cmsdecrypt.c
    tools/cmsparse.c
    tools/p256keygen.c
    tools/lmskeygen.c
    tools/lmssign.c
    tools/lmsverify.c
    tools/hsskeygen.c
    tools/hsssign.c
    tools/hssverify.c
    tools/xmsskeygen.c
    tools/xmsssign.c
    tools/xmssverify.c
    tools/xmssmtkeygen.c
    tools/xmssmtsign.c
    tools/xmssmtverify.c
    tools/sphincskeygen.c
    tools/sphincssign.c
    tools/sphincsverify.c
    tools/kyberkeygen.c
    tools/kyberencap.c
    tools/kyberdecap.c
    tools/zuc.c
    tools/ghash.c
    tools/tlcp_client.c
    tools/tlcp_server.c
    tools/tls12_client.c
    tools/tls12_server.c
    tools/tls13_client.c
    tools/tls13_server.c
    tools/sctverify.c
    tools/skfutil.c
    tools/sdfinfo.c
    tools/sdfdigest.c
    tools/sdfexport.c
    tools/sdfsign.c
    tools/sdfencrypt.c
    tools/sdfdecrypt.c
    tools/sdftest.c
)
list(TRANSFORM _gmssl_cli_sources PREPEND "${_gmssl_root}/")
add_executable(nimrtc_gmssl_cli EXCLUDE_FROM_ALL ${_gmssl_cli_sources})
set_target_properties(nimrtc_gmssl_cli PROPERTIES
    OUTPUT_NAME gmssl
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/gmssl-bin")
target_link_libraries(nimrtc_gmssl_cli PRIVATE nimrtc_vendor_gmssl)
if(MSVC)
    target_compile_options(nimrtc_gmssl_cli PRIVATE /utf-8 /FS)
endif()

set(NIMRTC_GMSSL_VENDORED ON CACHE BOOL
    "GmSSL vendored submodule build is active (v1.1 X1)" FORCE)
message(STATUS "GMSSL vendored backend: enabled (submodule v3.2.0, static)")
