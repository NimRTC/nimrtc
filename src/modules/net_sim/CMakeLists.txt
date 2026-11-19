# modules/net_sim/CMakeLists.txt
# ----------------------------------------------------------------------------
# The network-simulation module — the v1.3 plan's Q5 toolkit.
#
# Two pieces, both of them *tools* rather than engine capabilities:
#
#   - `impairment.hpp` — the impairment profile (loss / delay / jitter /
#     reorder / rate) as a deterministic, testable object.  It is the
#     portable model of what `tc qdisc netem` does, so the same profile
#     can be applied in-process on every platform and asserted in a
#     unit test.
#   - `nat_relay.hpp` — the **simulated NAT**: an address-translating
#     UDP relay with two sockets.  Neither peer sees the other's real
#     address, exactly as behind a NAT.
#
# Why a module and not a seam (the same argument as `rd_input` and
# `rd_session`): this is a *measurement* concern.  The engine never
# resolves it through the registry and never names it; the soak
# harness does.
#
# Dependencies:
#   - core (the stability macros and the time vocabulary)
# ----------------------------------------------------------------------------
set(NIMRTC_NET_SIM_SOURCES
    src/impairment.cpp
    src/nat_relay.cpp
    )

add_library(nimrtc_net_sim STATIC
    ${NIMRTC_NET_SIM_SOURCES}
    )

nimrtc_apply_options(nimrtc_net_sim)
target_link_libraries(nimrtc_net_sim
    PUBLIC
        nimrtc::core
    )

target_include_directories(nimrtc_net_sim
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
    )

target_compile_definitions(nimrtc_net_sim PUBLIC NIMRTC_HAS_NET_SIM=1)

# Winsock on Windows; the socket API is in libc elsewhere.
if(WIN32)
    target_link_libraries(nimrtc_net_sim PRIVATE ws2_32)
endif()

add_library(nimrtc::net_sim ALIAS nimrtc_net_sim)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
