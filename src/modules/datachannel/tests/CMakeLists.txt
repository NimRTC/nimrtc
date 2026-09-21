# modules/datachannel/tests/CMakeLists.txt
# =============================================================================
# DataChannel unit tests.
#
# Two test binaries:
#   - test_datachannel         (P1): IDataChannel interface + SimpleDataChannelFactory
#                                    + DataChannelConfig / Reliability enum.
#   - test_sctp_data_channel   (P2): SctpDataChannel + SctpDataChannelFactory +
#                                    register_default_plugins() — backed by a
#                                    MockSctpSocket so no real usrsctp handshake
#                                    is required.
#
# Built via GoogleTest (FetchContent if not installed) — mirrors the
# layout in src/sctp/tests/CMakeLists.txt.
# =============================================================================

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

# -----------------------------------------------------------------------------
# test_datachannel — interface + SimpleDataChannelFactory (P1 surface)
# -----------------------------------------------------------------------------
add_executable(test_datachannel test_datachannel.cpp)

set_target_properties(test_datachannel PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_datachannel LENIENT)

target_link_libraries(test_datachannel PRIVATE
    nimrtc::datachannel
    nimrtc::plugins
    nimrtc::core
    GTest::gtest_main
    GTest::gtest)

# -----------------------------------------------------------------------------
# test_sctp_data_channel — SctpDataChannel / SctpDataChannelFactory / registration (P2)
#
# P2 needs both nimrtc::datachannel (the SctpDataChannel class) and
# nimrtc::sctp (the ISctpSocket seam type used in the test injection
# constructor) — both are PUBLIC dependencies of nimrtc::datachannel
# (the latter transitively via the module's PUBLIC link to nimrtc::sctp).
# -----------------------------------------------------------------------------
add_executable(test_sctp_data_channel test_sctp_data_channel.cpp)

set_target_properties(test_sctp_data_channel PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_sctp_data_channel LENIENT)

target_link_libraries(test_sctp_data_channel PRIVATE
    nimrtc::datachannel
    nimrtc::plugins
    nimrtc::core
    nimrtc::sctp
    GTest::gtest_main
    GTest::gtest)

# NOTE: No top-level add_test() —see modules/audio3a/tests/CMakeLists.txt
# for the rationale (CMake 4.0 multi-config conditional bug).
if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_datachannel
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
    gtest_discover_tests(test_sctp_data_channel
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()
