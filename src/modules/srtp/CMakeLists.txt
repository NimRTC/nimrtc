# modules/srtp/CMakeLists.txt
# ----------------------------------------------------------------------------
# SRTP (Secure Real-time Transport Protocol) module.
#
# Scope (P1):
#   - libsrtp2 integration when NIMRTC_USE_LIBSRTP=1
#   - Stub implementation for testing without vendor
#   - RFC 3711 crypto suites (AES-CM, AES-GCM)
#   - ROC management and key rollover
#
# Dependencies:
#   - core (always)
#   - nimrtc::vendor::libsrtp (when available)
# ----------------------------------------------------------------------------
add_library(nimrtc_srtp STATIC
    src/srtp.cpp
    src/libsrtp_backend.cpp
    src/srtp_plugin.cpp
    )

nimrtc_apply_options(nimrtc_srtp)

# Link vendor libsrtp if available, otherwise use stub
if(TARGET nimrtc::vendor::libsrtp)
    target_link_libraries(nimrtc_srtp
        PUBLIC
            nimrtc::core
            nimrtc::plugins
        PRIVATE
            nimrtc::vendor::libsrtp
    )
    target_compile_definitions(nimrtc_srtp PRIVATE NIMRTC_USE_LIBSRTP=1)
else()
    target_link_libraries(nimrtc_srtp
        PUBLIC
            nimrtc::core
            nimrtc::plugins
        )
    message(STATUS "SRTP: libsrtp not available, using stub mode")
endif()

target_include_directories(nimrtc_srtp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_srtp PUBLIC NIMRTC_HAS_SRTP=1)

if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    # ensure a CTestTestfile.cmake gets generated at this level so ctest
    # can recurse into the tests/ subdir from src/modules/ (CMake 4.0
    # VS generator quirk -- without enable_testing() here the file is
    # silently skipped and the module's tests never register).
    enable_testing()
    add_subdirectory(tests)
endif()

add_library(nimrtc::srtp ALIAS nimrtc_srtp)
