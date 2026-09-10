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
set(WOLFSSL_OPENSSLEXTRA "no" CACHE BOOL "Disable OpenSSL extra")
set(WOLFSSL_OPENSSLALL "no" CACHE BOOL "Disable OpenSSL all")
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
set(WOLFSSL_HAPROXY "no" CACHE BOOL "Disable HAProxy")
set(WOLFSSL_OCSP "no" CACHE BOOL "Disable OCSP")
set(WOLFSSL_CRL "no" CACHE BOOL "Disable CRL")
set(WOLFSSL_CRL_MONITOR "no" CACHE BOOL "Disable CRL monitor")
set(WOLFSSL_OCSP_STAPLING "no" CACHE BOOL "Disable OCSP stapling")
set(WOLFSSL_SESSION_CERTS "no" CACHE BOOL "Disable session certs")
set(WOLFSSL_VERIFY_CALLBACK "no" CACHE BOOL "Disable verify callback")
set(WOLFSSL_CHECK_PRIVATE_KEY "no" CACHE BOOL "Disable check private key")

# 添加 wolfSSL 官方构建
add_subdirectory(${_src_dir} wolfssl_build)

# 创建项目规范的目标名
if(NOT TARGET nimrtc_vendor_wolfssl)
    add_library(nimrtc_vendor_wolfssl ALIAS wolfssl)
endif()
if(NOT TARGET nimrtc::vendor::wolfssl)
    add_library(nimrtc::vendor::wolfssl ALIAS wolfssl)
endif()
