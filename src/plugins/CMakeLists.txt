# plugins/CMakeLists.txt
# =============================================================================
# Plugin interface definitions and the PluginRegistry.
#
# This directory produces a HEADER-ONLY INTERFACE library (nimrtc_plugins).
# No .cpp files here — plugins/ only exposes abstract C++ interfaces.
#
# All concrete implementations (built-in "standard" plugins) live in their
# respective modules under src/modules/. Those modules link nimrtc_plugins
# PUBLIC so users who link any module transitively get the interface headers.
#
# Modules that want to use the plugin system:
#   target_link_libraries(nimrtc_rtp PUBLIC nimrtc::plugins)
#
# @note P0 scaffold. This library is header-only; do NOT add .cpp files here
# until a binary-stable plugin ABI is designed (TBD P4).
# =============================================================================

add_library(nimrtc_plugins INTERFACE)
add_library(nimrtc::plugins ALIAS nimrtc_plugins)

target_include_directories(nimrtc_plugins
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

# plugins/base.hpp includes <nimrtc/core/bytes.hpp> to unify buffer types
# with the core layer (see plugins/base.hpp for rationale).
target_link_libraries(nimrtc_plugins
    INTERFACE
        nimrtc::core)

# No compile definitions needed — header-only.

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
