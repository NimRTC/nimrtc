# src/sctp/tests/CMakeLists.txt
# =============================================================================
# SCTP seam module unit tests (Transport PAL Slice 5 — SEAM ONLY).
#
# Verifies the Slice 5 DoD:
#   - Factory creates a stub ISctpSocket.
#   - send_datagram / send_stream / send_partial_reliable all return
#     plugins::kErrNotReady.
#   - Factory id is exactly "stub" (NOT "usrsctp" — see DoD gate).
#   - register_default_plugins() is idempotent (Meyers-singleton latch).
#
# Built via GoogleTest (FetchContent if not installed) — mirrors the
# layout in src/modules/datachannel/tests/CMakeLists.txt.
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

add_executable(test_sctp_factory test_sctp_factory.cpp)

set_target_properties(test_sctp_factory PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_sctp_factory LENIENT)

target_link_libraries(test_sctp_factory PRIVATE
    nimrtc::sctp
    nimrtc::plugins
    nimrtc::core
    GTest::gtest_main
    GTest::gtest)

# Test needs the full `SctpStubFactory` definition (the public include root
# only forward-declares it; the concrete definition lives in src/, which
# is PRIVATE on nimrtc_sctp). Expose the private path only to the test
# target — production code does NOT see the SctpStubFactory type.
target_include_directories(test_sctp_factory PRIVATE
    ${CMAKE_SOURCE_DIR}/src/sctp/src)

# NOTE: No top-level add_test() —see modules/audio3a/tests/CMakeLists.txt
# for the rationale (CMake 4.0 multi-config conditional bug).
if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_sctp_factory
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()
