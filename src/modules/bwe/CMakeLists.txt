# modules/bwe/CMakeLists.txt
# ----------------------------------------------------------------------------
# Bandwidth Estimator module.
#
# Scope (P1):
#   - Abstract IBwe interface
#   - AIMD (Additive Increase / Multiplicative Decrease) default implementation
#   - Fixed-rate fallback
#
# P3 will replace AIMD with Goog-CC mainline as the default.
#
# Dependencies:
#   - core (always)
#   - modules/rtp (optional, for future loss estimation helpers)
# ----------------------------------------------------------------------------
add_library(nimrtc_bwe STATIC
    src/bwe.cpp
    )

nimrtc_apply_options(nimrtc_bwe)
target_link_libraries(nimrtc_bwe
    PUBLIC
        nimrtc::core
        nimrtc::plugins       # plugin interface
    PRIVATE
    )

target_include_directories(nimrtc_bwe
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_bwe PUBLIC NIMRTC_HAS_BWE=1)

add_library(nimrtc::bwe ALIAS nimrtc_bwe)
