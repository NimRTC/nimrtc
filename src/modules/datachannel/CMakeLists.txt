# modules/datachannel/CMakeLists.txt
# ----------------------------------------------------------------------------
# DataChannel module (P1: interface + factory, no concrete SCTP).
#
# P1 scope:
#   - IDataChannel / IDataChannelFactory interface (from nimrtc::plugins)
#   - SimpleDataChannelFactory template usage in tests
#   - register_default_plugins() no-op stub
#
# P2 scope (TBD):
#   - usrsctp-backed SCTP/PR-SCTP implementation
#   - Plugin adapter registering SCTP factory
#
# Dependencies:
#   - core (always)
#   - plugins (IDataChannel / IDataChannelFactory)
# ----------------------------------------------------------------------------
add_library(nimrtc_datachannel STATIC
    src/datachannel.cpp
    )

nimrtc_apply_options(nimrtc_datachannel)
target_link_libraries(nimrtc_datachannel
    PUBLIC
        nimrtc::core
        nimrtc::plugins       # plugin interface (IDataChannel / IDataChannelFactory)
    )

target_include_directories(nimrtc_datachannel
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_datachannel PUBLIC NIMRTC_HAS_DATACHANNEL=1)

add_library(nimrtc::datachannel ALIAS nimrtc_datachannel)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
