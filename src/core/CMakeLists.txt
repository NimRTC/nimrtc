# src/core/CMakeLists.txt
# ----------------------------------------------------------------------------
# Core: shared foundation — INTERFACE library + PAL Slice 2 compiled code.
#
# Contains only stable types: bytes / error / time / log (header).
# No compiled code here — log implementation lives in src/log/ (nimrtc_log).
#
# Layout Invariant 1 exception (ARCHITECTURE.md):
#   "src/core/ is the only INTERFACE library and exposes only stable types."
#   The sole exception is `src/core/src/pal_default_registrars.cpp` (PAL
#   Slice 2), which holds a static-initialised array (kDefaultRegistrars[])
#   required at program startup.  Compiled into nimrtc_core_objects and
#   linked transitively via nimrtc::core.
# ----------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# PAL Slice 2: compiled registration table (must be compiled, not header-only)
# ---------------------------------------------------------------------------
#
# This OBJECT library does NOT link nimrtc::log / nimrtc::plugins PRIVATEly
# because doing so creates a cyclic dependency:
#   nimrtc_log PUBLICly links nimrtc::core → nimrtc::core INTERFACElinks
#   nimrtc_core_objects → nimrtc_core_objects links nimrtc::log → cycle.
#
# Instead, we explicitly add the include paths the .cpp needs (transitively
# pulled in by <nimrtc/core/registry.hpp>'s back-compat shim).  No transitive
# library linking happens here — register_all_default_plugins() is `inline`
# in the header, so the .cpp just needs the headers at compile time.
add_library(nimrtc_core_objects OBJECT
    src/pal_default_registrars.cpp
)
target_include_directories(nimrtc_core_objects
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/plugins/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/modules/video_frame/include>
        $<INSTALL_INTERFACE:include>
)
# PAL Slice 2 — `pal_default_registrars.cpp` preprocessor-guards some entries
# behind NIMRTC_HAS_* macros that are PUBLIC-ly defined on the per-module
# nimrtc_video_* / nimrtc_opus / nimrtc_h264 targets.  Because the .cpp is
# compiled into nimrtc_core_objects (not into each module), those macros are
# NOT visible at this TU's compile time — the registrations would silently
# vanish, breaking test_engine_video_plugins and test_engine_video_codec.
# Mirror every relevant NIMRTC_HAS_* define from the corresponding
# NIMRTC_MODULE_* option so the guarded entries materialise correctly.
if(NIMRTC_MODULE_VIDEO_SOURCE)
    target_compile_definitions(nimrtc_core_objects PRIVATE NIMRTC_HAS_VIDEO_SOURCE=1)
endif()
if(NIMRTC_MODULE_VIDEO_SINK)
    target_compile_definitions(nimrtc_core_objects PRIVATE NIMRTC_HAS_VIDEO_SINK=1)
endif()
if(NIMRTC_MODULE_VIDEO_PIPELINE)
    target_compile_definitions(nimrtc_core_objects PRIVATE NIMRTC_HAS_VIDEO_PIPELINE=1)
endif()
if(NIMRTC_MODULE_OPUS)
    target_compile_definitions(nimrtc_core_objects PRIVATE NIMRTC_HAS_OPUS=1)
endif()
if(NIMRTC_MODULE_H264)
    target_compile_definitions(nimrtc_core_objects PRIVATE NIMRTC_HAS_H264=1)
endif()
nimrtc_apply_options(nimrtc_core_objects)

# ---------------------------------------------------------------------------
# nimrtc::core — INTERFACE library (headers only)
# ---------------------------------------------------------------------------
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

# nimrtc_core_objects links log + plugins PRIVATE so they are transitively
# available to all consumers of nimrtc::core without polluting the public
# header surface.
target_link_libraries(nimrtc_core
    INTERFACE
        nimrtc_core_objects
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
