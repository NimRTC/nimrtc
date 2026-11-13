# src/web_transport/tests/CMakeLists.txt
# =============================================================================
# WebTransport module tests.
#
#   - test_web_transport_alpn : the client (session) and the server (listener)
#                               must offer the same ALPN token.  v1.0.1 shipped
#                               "sm" on the client and "h3" on the server, which
#                               broke every NimRTC-to-NimRTC handshake; this
#                               test pins the invariant down.
#
# Uses GoogleTest (FetchContent if not installed) — mirrors the layout in
# src/modules/audio3a/tests/CMakeLists.txt.
# =============================================================================

enable_testing()

find_package(GTest QUIET)

if(NOT GTEST_FOUND)
    if(NIMRTC_FETCH_GTEST)
        # GTest::gtest_main is normally provided by cmake/NimRTCTest.cmake via
        # add_subdirectory(src/third_party/googletest). Only fall back here if
        # we're being built standalone (e.g. module-only build).
        if(NOT TARGET GTest::gtest_main)
            set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
            set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
            set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
            add_subdirectory(${CMAKE_SOURCE_DIR}/src/third_party/googletest ${CMAKE_BINARY_DIR}/_googletest_module_subbuild)
        endif()
    else()
        message(FATAL_ERROR "GoogleTest not found")
    endif()
endif()

add_executable(test_web_transport_alpn test_web_transport_alpn.cpp)

set_target_properties(test_web_transport_alpn PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_web_transport_alpn LENIENT)

target_link_libraries(test_web_transport_alpn PRIVATE
    nimrtc::web_transport
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

# The ALPN contract lives in an internal header (src/web_transport/src/), so
# the test needs that directory on the include path.
target_include_directories(test_web_transport_alpn PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src")

# nimrtc::web_transport links the msquic shared library, so the runtime DLL has
# to sit next to the test executable on Windows (see cmake/NimRTCTest.cmake).
if(COMMAND nimrtc_copy_msquic_dll)
    nimrtc_copy_msquic_dll(test_web_transport_alpn)
endif()

# -----------------------------------------------------------------------------
# test_web_transport_dc_capability — the v1.2.0 data-channel capability
# regression suite (WP-B framing / WP-C isolation / WP-D partial reliability
# / WP-E priority).  The test drives WebTransportDataChannel against an
# in-process mock session that implements the multicast inbound surface and
# the outbound priority surface.
#
# The target is named `..._dc_capability` (not `..._data_channel`) because the
# older DC-1 land test already owns the `test_web_transport_data_channel`
# target in the top-level tests/ directory — two targets cannot share a name.
# -----------------------------------------------------------------------------
add_executable(test_web_transport_dc_capability test_web_transport_dc_capability.cpp)

set_target_properties(test_web_transport_dc_capability PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_web_transport_dc_capability LENIENT)

target_link_libraries(test_web_transport_dc_capability PRIVATE
    nimrtc::web_transport
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

target_include_directories(test_web_transport_dc_capability PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src")

if(COMMAND nimrtc_copy_msquic_dll)
    nimrtc_copy_msquic_dll(test_web_transport_dc_capability)
endif()

# NOTE: No top-level add_test() for the wrapper executable.  CMake 4.0's
# multi-config generator wraps plain add_test() in a CTEST_CONFIGURATION_TYPE
# conditional that marks the wrapper as NOT_AVAILABLE when ctest is invoked
# without `-C <config>`.  The gtest_discover_tests() call below creates
# per-case add_test() entries at build time that bypass the conditional.
if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_web_transport_alpn
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
    gtest_discover_tests(test_web_transport_dc_capability
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()
