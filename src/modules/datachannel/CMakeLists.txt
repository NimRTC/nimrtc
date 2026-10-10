# modules/datachannel/CMakeLists.txt
# ----------------------------------------------------------------------------
# DataChannel module.
#
# Scope (P1, shipped):
#   - IDataChannel / IDataChannelFactory interface (from nimrtc::plugins)
#   - SimpleDataChannelFactory<T> template usage in tests
#
# Scope (P2, this revision):
#   - usrsctp-backed `SctpDataChannel : plugins::IDataChannel`
#   - `SctpDataChannelFactory : plugins::IDataChannelFactory` (id="sctp")
#   - `register_default_plugins()` entry point (Meyers-singleton latch)
#     that publishes the factory under id="sctp" in core::PluginRegistry.
#
# The channel wraps a `nimrtc::sctp::ISctpSocket` (resolved at
# construction time via `core::PluginRegistry::get_sctp_socket("usrsctp")`)
# so the SCTP transport layer lives in src/sctp/ — the datachannel
# module owns only the IDataChannel glue + factory + registration.
#
# Dependencies:
#   - core (always)
#   - plugins (IDataChannel / IDataChannelFactory)
#   - sctp (PUBLIC — the ISctpSocket seam type is part of the channel's
#     public surface, so callers that inject a custom socket via
#     `SctpDataChannel(std::unique_ptr<ISctpSocket>)` need the sctp
#     include path transitively)
# ----------------------------------------------------------------------------
add_library(nimrtc_datachannel STATIC
    src/sctp_data_channel.cpp
    src/plugin.cpp
    )

nimrtc_apply_options(nimrtc_datachannel)

target_link_libraries(nimrtc_datachannel
    PUBLIC
        nimrtc::core
        nimrtc::plugins       # plugin interface (IDataChannel / IDataChannelFactory)
        nimrtc::sctp          # ISctpSocket seam type — also lets tests inject a MockSctpSocket
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
