# NimRTCVendored.cmake
# ----------------------------------------------------------------------------
# Standardised integration of vendored third-party libraries.
#
# Vendored libraries are checked into src/third_party/<name>/ as source.
# Each has a CMakeLists.txt that creates an INTERFACE/STATIC target with
# a well-known name. We declare those target names here so other code
# can use them without knowing paths.
#
# All vendor targets:
#   - are STATIC (compiled into nimrtc, never dynamically linked to vendor)
#   - propagate their include directories via INTERFACE
#   - propagate their compile definitions (NIMRTC_USE_<NAME>, etc.)
#   - are optional: if third_party/<name>/ is absent, the target is a
#     stub that fails the build with a clear message
# ----------------------------------------------------------------------------

# -----------------------------------------------------------------------------
# Public target names (primary)
# -----------------------------------------------------------------------------
set(NIMRTC_VENDOR_LIBS
    nimrtc_vendor_libsrtp
    nimrtc_vendor_libopus
    nimrtc_vendor_mbedtls
    nimrtc_vendor_libjuice
    # P1+
    # nimrtc_vendor_libvpx
    # P2+
    # nimrtc_vendor_usrsctp
    # nimrtc_vendor_webrtc_apm
)

# Convenience aliases (nimrtc::vendor::<name>) — created by src/third_party/CMakeLists.txt
# when the upstream source is populated. Consumers may use either form:
#   target_link_libraries(my_app PRIVATE nimrtc_vendor_libsrtp)          # primary
#   target_link_libraries(my_app PRIVATE nimrtc::vendor::libsrtp)         # alias

# -----------------------------------------------------------------------------
# Helper: require a vendored library to exist
# -----------------------------------------------------------------------------
function(nimrtc_require_vendor name)
    set(vendor_dir "${CMAKE_SOURCE_DIR}/src/third_party/${name}")
    if(NOT EXISTS "${vendor_dir}/CMakeLists.txt")
        message(FATAL_ERROR
            "Vendor library '${name}' not found at ${vendor_dir}.\n"
            "Either populate it (see docs/zh/NimRTC-V2-技术文档.md §11) "
            "or disable the dependent module in CMakeLists.txt.")
    endif()
endfunction()

# -----------------------------------------------------------------------------
# Helper: convenience wrappers
# -----------------------------------------------------------------------------
function(nimrtc_link_libsrtp target)
    target_link_libraries(${target} PRIVATE nimrtc_vendor_libsrtp)
    target_compile_definitions(${target} PRIVATE
        NIMRTC_USE_LIBSRTP=1
        # Suppress libsrtp's deprecation warnings if any
        SRTP_NO_DEPRECATED=1)
endfunction()

function(nimrtc_link_libopus target)
    target_link_libraries(${target} PRIVATE nimrtc_vendor_libopus)
    target_compile_definitions(${target} PRIVATE NIMRTC_USE_LIBOPUS=1)
endfunction()

function(nimrtc_link_mbedtls target)
    target_link_libraries(${target} PRIVATE nimrtc_vendor_mbedtls)
    target_compile_definitions(${target} PRIVATE NIMRTC_USE_MBEDTLS=1)
endfunction()

function(nimrtc_link_libjuice target)
    target_link_libraries(${target} PRIVATE nimrtc_vendor_libjuice)
    target_compile_definitions(${target} PRIVATE NIMRTC_USE_LIBJUICE=1)
endfunction()

# Future:
# function(nimrtc_link_usrsctp target) ...
# function(nimrtc_link_webrtc_apm target) ...
# function(nimrtc_link_libvpx target) ...