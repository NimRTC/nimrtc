# third_party/wolfssl/CMakeLists.txt
# wolfSSL - TLS/DTLS library for WebRTC DTLS-SRTP keying
# =============================================================================

set(_src_dir "${CMAKE_CURRENT_SOURCE_DIR}/src")

# 设置 wolfSSL 官方 CMake 选项
set(WOLFSSL_DTLS "yes" CACHE BOOL "Enable DTLS")
set(WOLFSSL_TLS13 "yes" CACHE BOOL "Enable TLS 1.3")
set(WOLFSSL_ECC "yes" CACHE BOOL "Enable ECC")
set(WOLFSSL_AESGCM "yes" CACHE BOOL "Enable AES-GCM")
set(WOLFSSL_AESCBC "yes" CACHE BOOL "Enable AES-CBC")
set(WOLFSSL_SHA256 "yes" CACHE BOOL "Enable SHA-256")
set(WOLFSSL_SHA384 "yes" CACHE BOOL "Enable SHA-384")
set(WOLFSSL_HKDF "yes" CACHE BOOL "Enable HKDF")
set(WOLFSSL_CHACHA "yes" CACHE BOOL "Enable ChaCha20")
set(WOLFSSL_POLY1305 "yes" CACHE BOOL "Enable Poly1305")
set(WOLFSSL_ERROR_STRINGS "yes" CACHE BOOL "Enable error strings")
set(WOLFSSL_SINGLE_THREADED "yes" CACHE BOOL "Single threaded")
set(WOLFSSL_RNG "yes" CACHE BOOL "Enable RNG")
set(WOLFSSL_SRTP "yes" CACHE BOOL "Enable SRTP")

# 禁用不需要的功能
set(WOLFSSL_RSA "no" CACHE BOOL "Disable RSA")
set(WOLFSSL_DSA "no" CACHE BOOL "Disable DSA")
set(WOLFSSL_PSK "no" CACHE BOOL "Disable PSK")
set(WOLFSSL_ANON "no" CACHE BOOL "Disable Anonymous DH")
set(WOLFSSL_MD5 "no" CACHE BOOL "Disable MD5")
set(WOLFSSL_OLD_TLS "no" CACHE BOOL "Disable old TLS")
set(WOLFSSL_SRP "no" CACHE BOOL "Disable SRP")
set(WOLFSSL_SSH "no" CACHE BOOL "Disable SSH")
set(WOLFSSL_SFTP "no" CACHE BOOL "Disable SFTP")
# We need the X509 DER export + WOLFSSL_X509_STORE_CTX compat symbols
# (wolfSSL_X509_STORE_CTX_get_current_cert / wolfSSL_X509_get_der) so
# NimRTC can hash the peer's SPKI inside the verify callback.
set(WOLFSSL_OPENSSLEXTRA "yes" CACHE BOOL "Enable OpenSSL compatibility API")
set(WOLFSSL_OPENSSLALL "no" CACHE BOOL "Disable OpenSSL all")
# Disable wolfcrypt bench/test binaries — their source contains UTF-8
# literals that MSVC reads as CP1252 and fails to compile.
set(WOLFSSL_CRYPT_TESTS "no" CACHE BOOL "Disable wolfcrypt benchmark/test" FORCE)
set(WOLFSSL_CERTGEN "no" CACHE BOOL "Disable cert gen")
set(WOLFSSL_CERTREQ "no" CACHE BOOL "Disable cert req")
set(WOLFSSL_CERTEXT "no" CACHE BOOL "Disable cert ext")
set(WOLFSSL_KEYGEN "no" CACHE BOOL "Disable key gen")
set(WOLFSSL_QT "no" CACHE BOOL "Disable Qt")
set(WOLFSSL_HAPROXY "no" CACHE BOOL "Disable HAProxy")
set(WOLFSSL_APACHE_HTTPD "no" CACHE BOOL "Disable Apache HTTPD")
set(WOLFSSL_ERROR_QUEUE "no" CACHE BOOL "Disable error queue")
set(WOLFSSL_ASM "no" CACHE BOOL "Disable ASM")
set(WOLFSSL_DEBUG "no" CACHE BOOL "Disable debug")
set(WOLFSSL_WOLFSSH "no" CACHE BOOL "Disable wolfSSH")
set(WOLFSSL_OCSP "no" CACHE BOOL "Disable OCSP")
set(WOLFSSL_CRL "no" CACHE BOOL "Disable CRL")
set(WOLFSSL_CRL_MONITOR "no" CACHE BOOL "Disable CRL monitor")
set(WOLFSSL_OCSP_STAPLING "no" CACHE BOOL "Disable OCSP stapling")
set(WOLFSSL_SESSION_CERTS "no" CACHE BOOL "Disable session certs")
set(WOLFSSL_VERIFY_CALLBACK "no" CACHE BOOL "Disable verify callback")
set(WOLFSSL_CHECK_PRIVATE_KEY "no" CACHE BOOL "Disable check private key")
# Enable Extended Master Secret (RFC 7627).  Required for DTLS-SRTP per
# RFC 5764 §5 ("the use of the Extended Master Secret extension is
# REQUIRED").  Chrome has enforced EMS since M76 — without this option
# wolfSSL silently drops the EMS extension and Chrome rejects the
# handshake with "EMS_REQUIRED".  The flag is force-exported via
# PUBLIC compile-definitions below so every consumer TU that includes
# <wolfssl/ssl.h> sees `HAVE_EXTENDED_MASTER_SECRET` defined.
set(WOLFSSL_EXTENDED_MASTER_SECRET "yes" CACHE BOOL "Enable EMS (RFC 7627)" FORCE)

# Enable KEEP_PEER_CERT so we can call wolfSSL_get_peer_certificate() after
# a successful handshake to extract and SPKI-pin the peer cert fingerprint.
# This is required by WebRTC's SDP "a=fingerprint" semantics (RFC 8122).
# wolfSSL adds -DKEEP_PEER_CERT to CMAKE_C_FLAGS when this is set.
set(WOLFSSL_KEEP_PEER_CERT "yes" CACHE BOOL "Keep peer certificate after handshake")

# Don't build wolfssl's example servers / tests (server.c, echoclient.c,
# test.h, etc.).  Those source files contain hard-coded UTF-8 byte
# sequences (emoji / Chinese sample log strings) which MSVC reads as
# CP1252 by default and fails with:
#   error C2001: newline in constant
# at every example compile.  We only need libwolfssl for DTLS.
set(WOLFSSL_EXAMPLES "no" CACHE BOOL "Disable wolfssl examples" FORCE)
set(WOLFSSL_TEST_SUBMIT "no" CACHE BOOL "Disable wolfssl tests" FORCE)

# 添加 wolfSSL 官方构建
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${_src_dir} wolfssl_build)

# 强制启用 KEEP_PEER_CERT，使 wolfSSL 在握手完成后保留 peer X509 证书。
# 这是 WebRTC SDP fingerprint 校验的必要条件：握手后调
# wolfSSL_get_peer_certificate() + wc_GetSubjectPubKeyInfoDerFromCert() 来
# 计算 RFC 8122 规定的 SPKI SHA-256，再与应用层存储的 expected fingerprint 对比。
#
# 重要：必须用 PUBLIC，否则在多目标 MSVC 工程中只有 wolfssl 自己看得到这个宏，
# 其他 include <wolfssl/ssl.h> 的 .cpp（如 nimrtc_dtls）仍按未定义编译，
# 就会找不到 wolfSSL_get_peer_certificate/wolfSSL_X509_free/wolfSSL_X509_get_der。
target_compile_definitions(wolfssl PUBLIC KEEP_PEER_CERT)

# 创建项目规范的目标名
if(NOT TARGET nimrtc_vendor_wolfssl)
    add_library(nimrtc_vendor_wolfssl ALIAS wolfssl)
endif()
if(NOT TARGET nimrtc::vendor::wolfssl)
    add_library(nimrtc::vendor::wolfssl ALIAS wolfssl)
endif()

# Force synchronous PDB writes to fix MSB6003 / C1041 PDB contention errors
# when multiple CL.exe processes try to write to the same .pdb during parallel builds.
if(MSVC)
    target_compile_options(wolfssl PRIVATE /FS)
endif()
