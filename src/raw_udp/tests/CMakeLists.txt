# src/raw_udp/tests/CMakeLists.txt
# =============================================================================
# Raw-UDP bypass unit tests (Slice 6).
#
# The 100 Hz simulated loopback test (test_raw_udp_arq.cpp) reports
# p50 / p99 / p999 latency. The skeleton is allowed to be slow — this
# test exists to prove the skeleton works, not to defend an SLO.
# =============================================================================

enable_testing()

find_package(GTest QUIET)

if(NOT GTEST_FOUND)
    if(NIMRTC_FETCH_GTEST)
        if(NOT TARGET GTest::gtest_main)
            set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
            set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
            set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
            add_subdirectory(${CMAKE_SOURCE_DIR}/src/third_party/googletest
                             ${CMAKE_BINARY_DIR}/_googletest_raw_udp_subbuild)
        endif()
    else()
        message(FATAL_ERROR "GoogleTest not found")
    endif()
endif()

add_executable(test_raw_udp_arq test_raw_udp_arq.cpp)
set_target_properties(test_raw_udp_arq PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_raw_udp_arq)

target_link_libraries(test_raw_udp_arq PRIVATE
    nimrtc::raw_udp
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_raw_udp_arq
        PROPERTIES TIMEOUT 180      # 100 Hz loopback, skeleton known slow
        DISCOVERY_TIMEOUT 10)
endif()

# -----------------------------------------------------------------------------
# End-to-end loopback test for the real-socket path (Slice 6.5 follow-up).
# Two ArqRawUdp instances talk via 127.0.0.1 ephemeral ports; this catches
# any ACK-frame parse / build byte-offset regression that the in-process
# skeleton test cannot.
# -----------------------------------------------------------------------------
add_executable(test_raw_udp_real_loopback test_raw_udp_real_loopback.cpp)
set_target_properties(test_raw_udp_real_loopback PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_raw_udp_real_loopback)

target_link_libraries(test_raw_udp_real_loopback PRIVATE
    nimrtc::raw_udp
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_raw_udp_real_loopback
        PROPERTIES TIMEOUT 60
        DISCOVERY_TIMEOUT 15)
endif()
