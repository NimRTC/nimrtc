# src/transport/tests/CMakeLists.txt
# =============================================================================
# Transport Selector unit tests (Slice 7).
#
# Tests cover all four priority rules from transport-selection.md §5.3
# using mock ITransportStackFactory implementations so the rule engine
# is verifiable without standing up the real PluginRegistry slot
# (Slice-8 work).
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

add_executable(test_transport_selector test_transport_selector.cpp)
set_target_properties(test_transport_selector PROPERTIES
    VS_LINK_LIBRARY_DEPENDENCIES TRUE)

nimrtc_apply_options(test_transport_selector)

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
