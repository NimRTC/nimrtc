# third_party/mbedtls/CMakeLists.txt
# =============================================================================
# mbedtls — TLS / DTLS library (intended for WebRTC DTLS-SRTP keying).
#
# Populated by: git clone https://github.com/Mbed-TLS/mbedtls.git third_party/mbedtls/src
#
# Upstream version: mbedtls 4.2.0 (vendored commit: see src/.git).
#
# Target produced: nimrtc_vendor_mbedtls (INTERFACE).
#
# NOTE: For now we do NOT actually compile mbedtls. The upstream build needs
# Perl (for scripts/generate_*.pl) and a populated `framework` submodule,
# which the vendored archive is missing. When the DTLS module lands we will
# either (a) install Strawberry Perl, or (b) drop to an older mbedtls 3.x
# series that doesn't split framework into a submodule.
#
# Consumers see the public include path so any module that wants to include
# <mbedtls/...> still works at compile time. Linking against the stub will
# fail with a clear message — see NimRTCVendored.cmake.
# =============================================================================

set(_src_dir "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(nimrtc_vendor_mbedtls INTERFACE)
add_library(nimrtc::vendor::mbedtls ALIAS nimrtc_vendor_mbedtls)

target_include_directories(nimrtc_vendor_mbedtls
    INTERFACE
        $<BUILD_INTERFACE:${_src_dir}/include>
)

target_compile_definitions(nimrtc_vendor_mbedtls
    INTERFACE
        NIMRTC_USE_MBEDTLS=1
)
