# engine/CMakeLists.txt
# =============================================================================
# NimRTCEngine — the top-level composition layer.
#
# This is the primary public target for application code. It links all modules
# together and provides the single entry point (NimRTCEngine) documented in
# the user-facing API.
#
# Consumers only need:
#   target_link_libraries(my_app PRIVATE nimrtc::engine)
# =============================================================================

add_library(nimrtc_engine STATIC
    src/engine.cpp
)

nimrtc_apply_options(nimrtc_engine)
target_link_libraries(nimrtc_engine
    PUBLIC
        nimrtc::core           # base types
        nimrtc::rtp            # RTP parser/builder (P1)
        nimrtc::sdp            # SDP parser/munger (P1)
        nimrtc::jb             # JitterBuffer (P1)
        nimrtc::audio3a        # Audio 3A Null impl (P1)
        nimrtc::ice            # ICE transport (P1)
        nimrtc::dtls           # DTLS handshake (P1.1)
        nimrtc::srtp           # SRTP protect/unprotect (P1.1)
        nimrtc::opus           # Opus codec stub (P1) — header path
    PRIVATE
        nimrtc_vendor_libjuice # ICE vendor (P1)
)

# Propagate Opus include directory to consumers (since engine.hpp includes
# <nimrtc/opus/opus.hpp>).
target_include_directories(nimrtc_engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/modules/opus/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

add_library(nimrtc::engine ALIAS nimrtc_engine)
