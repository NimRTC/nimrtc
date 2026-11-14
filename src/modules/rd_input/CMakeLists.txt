# modules/rd_input/CMakeLists.txt
# ----------------------------------------------------------------------------
# Remote-desktop input module — plan Track G (G1 / G2 / G3 / G4 / G5 / G6 / G7).
#
# Scope:
#   - G1 / G2: the control channel's framing + reassembly (the mechanism
#          behind the control channel; the engine owns the transport)
#   - G3: the compact, versioned binary encoding of an input event
#          (mouse move / button / wheel / key down-up / text)
#   - G4: the pointer coalescer and the input path's own rate limit
#   - G5: the input <-> frame binding (the ref-frame binder)
#   - G6: the dead-man switch / heartbeat (the watchdog + the emitter)
#   - G7: the configurable allow / deny safety filter
#
# NOT in scope (other items in Track G, owned by other work):
#   - G1b: the browser-interoperable DataChannel (a DTLS-SCTP
#     association — a transport-layer project, not a plug-in flip)
#
# Why a module and not a seam (plan G3):
#   "It is a module, not a seam: the transport is DataChannel, and only the
#    payload format is NimRTC's."  So there is no `plugins::I*` interface
#    and nothing in src/plugins/ — the module is self-contained (L2 / L3).
#
# The one exception is the control channel (G1 / G2): the engine has to
# drive it, so the *interface* is a seam (`plugins::IControlChannel` in
# src/plugins/) and the *mechanism* is here.  The engine never includes
# this module's headers (Layout Invariant 4 / ADR-025).
#
# Dependencies:
#   - core    (always) — the byte / error / log vocabulary
#   - plugins (PRIVATE) — the control-channel seam's interface only
# ----------------------------------------------------------------------------
add_library(nimrtc_rd_input STATIC
    src/coalescer.cpp
    src/control_channel.cpp
    src/deadman_watchdog.cpp
    src/event_codec.cpp
    src/ref_frame_binder.cpp
    src/safety_filter.cpp
    )

nimrtc_apply_options(nimrtc_rd_input)
target_link_libraries(nimrtc_rd_input
    PUBLIC
        nimrtc::core
    PRIVATE
        # The control-channel seam (`plugins::IControlChannel`) — the
        # engine reaches the module's control plane through it, so the
        # module must not be included by the engine (Layout Invariant 4).
        nimrtc::plugins
    )

target_include_directories(nimrtc_rd_input
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_rd_input PUBLIC NIMRTC_HAS_RD_INPUT=1)

add_library(nimrtc::rd_input ALIAS nimrtc_rd_input)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    # ensure a CTestTestfile.cmake gets generated at this level so ctest can
    # recurse into the tests/ subdir from src/modules/ (CMake 4.0 VS
    # generator quirk — same pattern as modules/rtp).
    enable_testing()
    add_subdirectory(tests)
endif()
