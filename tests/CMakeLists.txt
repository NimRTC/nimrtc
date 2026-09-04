# tests/CMakeLists.txt — top-level cross-module integration tests
# ----------------------------------------------------------------------------
# Tests that span multiple modules (e.g. RTP+JB+SDP end-to-end) live here.
# Single-module tests live inside each module's `tests/` directory.
# ----------------------------------------------------------------------------

# E2E tests are standalone (custom main(), no GoogleTest). They each use
# raw sockets to verify round-trip behaviour. add_e2e_test is a small wrapper
# that wires the include dirs, links Winsock on Windows, and registers the
# test with CTest.
function(add_e2e_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        nimrtc::rtp
        ws2_32)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    nimrtc_apply_options(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endfunction()

# RTP round-trip over UDP loopback.
add_e2e_test(test_udp_loopback  test_udp_loopback.cpp)

# RTCP SR/RR/NACK round-trip over UDP loopback.
add_e2e_test(test_rtcp_loopback test_rtcp_loopback.cpp)

# Parser error-path matrix (no network).
add_e2e_test(test_error_matrix  test_error_matrix.cpp)

# ----------------------------------------------------------------------------
# GoogleTest-based tests for cross-module integration.
# These use GTest for richer assertions; link all modules they exercise.
# ----------------------------------------------------------------------------
function(nimrtc_add_engine_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        nimrtc::engine
        # R2.5: link every concrete module that has a register_default_plugins()
        # so the engine's open() → register_all_default_plugins() chain resolves.
        nimrtc::ice
        nimrtc::rtp
        nimrtc::sdp
        nimrtc::jb
        nimrtc::audio3a
        GTest::gtest_main
        ws2_32)
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/include"
        "${CMAKE_SOURCE_DIR}/src/plugins/include"
        "${CMAKE_SOURCE_DIR}/src/engine/include")
    if(WIN32)
        target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0601)
    endif()
    set_target_properties(${name} PROPERTIES
        VS_LINK_LIBRARY_DEPENDENCIES TRUE)
    nimrtc_apply_options(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    if(COMMAND gtest_discover_tests)
        gtest_discover_tests(${name}
            PROPERTIES TIMEOUT 30
            DISCOVERY_TIMEOUT 10)
    endif()
endfunction()

# R2.5 validation: engine consumes PluginRegistry for plugin slots.
nimrtc_add_engine_test(test_engine_plugin_loading test_engine_plugin_loading.cpp)
