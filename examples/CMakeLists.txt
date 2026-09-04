# examples/CMakeLists.txt
# ----------------------------------------------------------------------------
# Example / demo programs.  Built only when NIMRTC_BUILD_EXAMPLES=ON.
# ----------------------------------------------------------------------------

if(NOT NIMRTC_BUILD_EXAMPLES)
    return()
endif()

# demo-p2p: P1 baseline — generate SDP offer, optionally process remote SDP.
add_executable(demo-p2p demo-p2p/demo-p2p.cpp)
nimrtc_apply_options(demo-p2p)
target_link_libraries(demo-p2p PRIVATE nimrtc::engine)

# loopback-p2p: P1.1 — two NimRTCEngines connect over loopback.
add_executable(loopback-p2p loopback-p2p/loopback-p2p.cpp)
nimrtc_apply_options(loopback-p2p)
target_link_libraries(loopback-p2p PRIVATE nimrtc::engine)
