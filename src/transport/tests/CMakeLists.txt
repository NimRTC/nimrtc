# src/transport/tests/CMakeLists.txt
# =============================================================================
# Transport Selector unit tests (Slice 7) + strategy + raw-udp-arq stack.
#
# Slice 7:  test_transport_selector  — §5.3 four-rule priority table
# Slice 7.6: test_transport_strategy   — 5 named strategies + strategy_by_name
# Slice 7.6: test_raw_udp_arq_stack    — control-plane factory + lifecycle
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
                             ${CMAKE_BINARY_DIR}/_googletest_transport_subbuild)
        endif()
    else()
        message(FATAL_ERROR "GoogleTest not found")
    endif()
endif()

# --- test_transport_selector (Slice 7) --------------------------------------
add_executable(test_transport_selector test_transport_selector.cpp)
set_target_properties(test_transport_selector PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_transport_selector LENIENT)

target_link_libraries(test_transport_selector PRIVATE
    nimrtc::transport
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_transport_selector
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()

# --- test_transport_strategy (Slice 7.6: 5 named strategies) ----------------
add_executable(test_transport_strategy test_transport_strategy.cpp)
set_target_properties(test_transport_strategy PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_transport_strategy LENIENT)

target_link_libraries(test_transport_strategy PRIVATE
    nimrtc::transport
    nimrtc::core
    nimrtc::plugins
    GTest::gtest_main
    GTest::gtest)

if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_transport_strategy
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()

# --- test_raw_udp_arq_stack (Slice 7.6: control-plane factory) -------------
# Requires the raw_udp and dtls_seam libraries for the ARQ impl + the
# null-instance singletons in the control stack's accessor stubs.
add_executable(test_raw_udp_arq_stack test_raw_udp_arq_stack.cpp)
set_target_properties(test_raw_udp_arq_stack PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_raw_udp_arq_stack LENIENT)

target_link_libraries(test_raw_udp_arq_stack PRIVATE
    nimrtc::transport
    nimrtc::core
    nimrtc::plugins
    nimrtc::raw_udp
    nimrtc::dtls_seam
    nimrtc::sctp
    GTest::gtest_main
    GTest::gtest)

if(COMMAND gtest_discover_tests)
    gtest_discover_tests(test_raw_udp_arq_stack
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 10)
endif()
