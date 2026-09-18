# src/transport/CMakeLists.txt
# ----------------------------------------------------------------------------
# Transport stack + selector module (transport-selection.md §6.4 — Slice 7).
#
# Scope (Slice 7):
#   - ITransportStack / ITransportStackFactory / TransportSession interface
#   - ITransportSelector + TransportRequirements + CapabilitySelector
#   - register_default_selector() idempotent entry point
#   - 4-rule priority table from §5.3 implemented as TODO-stubs that
#     return empty TransportSession (Slice 7.5 adds default stack factories)
#   - JSON Profile schema bump (v1.0 → v1.1, additive) and example profile
#     update — handled outside CMake as static files
#
# **Boundary discipline** (parallel-build isolation):
#   This module depends ONLY on core + plugins. It MUST NOT link Slice
#   4 (DTLS) / Slice 5 (SCTP) / Slice 6 (raw_udp) directly — those are
#   forward-declared in transport_stack.hpp so the headers compile
#   standalone, and the .cpp file references them only by id (not by
#   dereferencing the forward-declared type). Once Slice 4/5/6 default
#   impls land, the engine integration (Slice 8) will register stack
#   factories that wire the real types together.
#
# Dependencies:
#   - core (logging)
#   - plugins (Addr, BufferView, Status, etc.)
# ----------------------------------------------------------------------------
add_library(nimrtc_transport STATIC
    src/transport_selector.cpp
    src/transport_plugin.cpp
    src/default_transport_stack_factory.cpp   # Slice 8: shell ITransportStackFactory
)

nimrtc_apply_options(nimrtc_transport)

target_link_libraries(nimrtc_transport
    PUBLIC
        nimrtc::core
        nimrtc::plugins
        # Slice 8: the default-transport-stack-factory .cpp references
        # the DTLS / SCTP / raw_udp seam headers. They must be in the
        # PUBLIC link line so consumers that link only nimrtc::transport
        # can also resolve the seam types used by ITransportStack's
        # accessors (slice 7.5 will fold these into a `nimrtc::seam`
        # umbrella target — for Slice 8 we keep the direct dep list).
        nimrtc::dtls_seam
        nimrtc::sctp
        nimrtc::raw_udp
)

target_include_directories(nimrtc_transport
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

# Build-time guard so consumers can `#ifdef NIMRTC_HAS_TRANSPORT_SELECTOR`
# to detect Slice-7 availability without breaking on platforms where
# it's disabled.
target_compile_definitions(nimrtc_transport PUBLIC NIMRTC_HAS_TRANSPORT_SELECTOR=1)

add_library(nimrtc::transport ALIAS nimrtc_transport)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
