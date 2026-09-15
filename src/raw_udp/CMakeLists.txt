# src/raw_udp/CMakeLists.txt
# ----------------------------------------------------------------------------
# Raw-UDP bypass module (transport-selection §6.3 — Slice 6).
#
# Scope (Slice 6, forward-looking skeleton):
#   - IRawUdpDatagram interface (header-only, no impl dep on Slice 4/5)
#   - ArqRawUdp skeleton: selective-repeat ARQ + DTLS-PSK stub + 100 Hz
#     loopback test
#   - ArqRawUdpFactory (id="arq")
#   - register_default_plugins() idempotent entry point
#
# Out of scope (deferred to Slice 7 / 7.5):
#   - Real DTLS-PSK key exchange (Slice 4 IDtlsSession PSK extension)
#   - ICE-selected-pair reuse (Slice 7 ITransportStack::raw_control())
#   - Production-grade ARQ retransmit + congestion control
#
# Dependencies:
#   - core (always — for logging)
#   - plugins (BufferView, Addr, IPlugin base types)
# ----------------------------------------------------------------------------
add_library(nimrtc_raw_udp STATIC
    src/arq_raw_udp.cpp
    src/arq_raw_udp_factory.cpp
    src/raw_udp_plugin.cpp
    src/udp_socket.cpp      # cross-platform UDP wrapper (slice 6.5 follow-up)
)

nimrtc_apply_options(nimrtc_raw_udp)

target_link_libraries(nimrtc_raw_udp
    PUBLIC
        nimrtc::core
        nimrtc::plugins
)

target_include_directories(nimrtc_raw_udp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_raw_udp PUBLIC NIMRTC_HAS_RAW_UDP=1)

add_library(nimrtc::raw_udp ALIAS nimrtc_raw_udp)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
