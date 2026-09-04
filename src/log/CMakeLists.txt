# src/log/CMakeLists.txt
# =============================================================================
# Logger implementation — the only module in src/ that has compiled code.
#
# Per ARCHITECTURE.md Layout Invariant 1:
#   "src/core/ is the only INTERFACE library and exposes only stable types.
#    No src/core/src/ files — when log sinks get implementations in P1,
#    a new nimrtc_log target is added; src/core/ stays header-only."
#
# This library provides the out-of-line Logger/Sink definitions that cannot
# be header-only (virtual destructor, non-trivial inline bodies).
#
# Any module that needs logging links PRIVATE nimrtc::log.
# src/core/ links it PRIVATE so that nimrtc::core consumers get it transitively.
# =============================================================================

add_library(nimrtc_log STATIC
    src/log.cpp
)

nimrtc_apply_options(nimrtc_log)
target_link_libraries(nimrtc_log
    PUBLIC
        nimrtc::core)

add_library(nimrtc::log ALIAS nimrtc_log)
