# src/core/CMakeLists.txt
# ----------------------------------------------------------------------------
# Core: shared foundation — HEADER-ONLY INTERFACE library.
#
# Contains only stable types: bytes / error / time / log (header).
# No compiled code here — log implementation lives in src/log/ (nimrtc_log).
#
# Layout Invariant 1 (ARCHITECTURE.md):
#   "src/core/ is the only INTERFACE library and exposes only stable types.
#    No src/core/src/ files."
# ----------------------------------------------------------------------------
add_library(nimrtc_core INTERFACE)
add_library(nimrtc::core ALIAS nimrtc_core)

target_include_directories(nimrtc_core
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_definitions(nimrtc_core
    INTERFACE
        NIMRTC_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        NIMRTC_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        NIMRTC_VERSION_PATCH=${PROJECT_VERSION_PATCH})

# Log implementation lives in nimrtc_log (src/log/).
# Linking it PRIVATE means consumers of nimrtc::core get it transitively.
target_link_libraries(nimrtc_core
    INTERFACE
        nimrtc::log
        # The backward-compat shim at the bottom of nimrtc/core/registry.hpp
        # transitively includes nimrtc/plugins/*.hpp so that old code which
        # only included <nimrtc/core/registry.hpp> keeps working.  Declaring
        # the dependency here (rather than reaching into plugins from a core
        # source file) keeps the layering explicit and ensures downstream
        # consumers (engine, examples, tests) get the plugins include path
        # transitively via nimrtc::core.
        nimrtc::plugins)

if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
