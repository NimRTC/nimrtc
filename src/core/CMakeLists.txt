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
# PAL Slice 2:
# compiled registration table (kept as OBJECT — see Split Note below).
#
# Split Note (Windows DLL boundary fix, v0.11.0):
#   Two competing requirements made us split this target:
#
#   1. Linux/macOS static-library archive step requires every external
#      symbol referenced in any `.o` member to be resolvable from the
#      same `ar` / `lib` invocation that produces the `.a` / `.lib`
#      archive. `pal_default_registrars.cpp` references
#      `nimrtc::audio3a::register_default_plugins()` and friends from
#      every concrete module library — i.e. ~16 unresolved external
#      references that are NOT satisfiable at archive time without
#      dragging every module into `nimrtc_core_objects` (which would
#      re-create the cyclic dep this CMakeLists explicit avoids:
#         nimrtc_log PUBLICly links nimrtc::core → nimrtc::core
#         INTERFACEly links nimrtc_core_objects → if
#         nimrtc_core_objects links nimrtc::audio3a... cycle).
#   2. Windows MSVC + MSVC function-local-static behaviour caused each
#      `nimrtc_dtls_seam.lib` / `test_engine_plugin_loading.exe` to
#      carry its own copy of the singleton because `PluginRegistry::
#      instance()`'s `static PluginRegistry inst` lived in an inline
#      function inside `<nimrtc/core/registry.hpp>`. The fix is to
#      move the singleton body into a single `.cpp` file compiled into
#      a single STATIC library so the linker unifies it across all
#      consumers.
#
# The split:
#
#   nimrtc_core_objects   OBJECT (this library, unchanged)
#        └─ src/pal_default_registrars.cpp
#           Holds `kDefaultRegistrars[]` — references the modules'
#           `register_default_plugins()` symbols, deferred until
#           final link as before. Cycles avoided.
#
#   nimrtc_registry       STATIC (new, added in v0.11.0)
#        └─ src/plugin_registry.cpp
#           Holds `PluginRegistry::instance()` and every
#           `register_*()` method.  No external symbol references —
#           clean archive on Linux/macOS, singleton unified on
#           Windows.  See `src/core/src/plugin_registry.cpp` for the
#           file header explaining the Windows DLL rationale.
#
# Both libraries are linked INTERFACEly by `nimrtc::core` below so
# downstream consumers pick up the registry singleton automatically.
# ---------------------------------------------------------------------------

add_library(nimrtc_core_objects OBJECT
    src/pal_default_registrars.cpp
)
target_include_directories(nimrtc_core_objects
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/plugins/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/modules/video_frame/include>
        # Transport PAL Slice 8 (v0.10.2): the TypedRegistry<...> fields
        # in registry.hpp forward-declare the DTLS / SCTP / raw_udp /
        # transport-stack factory types but the static_cast calls in
        # `detail::Registrar::Registrar()` need the full type. The
        # matching headers live in the seam modules' include dirs;
        # exposing them here lets pal_default_registrars.cpp compile
        # the static_cast without each TU needing to add the include
        # explicitly.
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/dtls/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/sctp/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/raw_udp/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/transport/include>
        $<INSTALL_INTERFACE:include>
)

# ---------------------------------------------------------------------------
# nimrtc_registry (NEW in v0.11.0 — Windows DLL boundary fix)
#
# STATIC library that owns the PluginRegistry singleton's out-of-line
# definitions (instance() and every register_* method body).  Marked as
# the build target via `NIMRTC_REGISTRY_EXPORTS` so the NIMRTC_API
# macro in `nimrtc/core/nimrtc_export.h` expands to dllexport here
# and to dllimport elsewhere.
# ---------------------------------------------------------------------------
add_library(nimrtc_registry STATIC
    src/plugin_registry.cpp
)
target_compile_definitions(nimrtc_registry PRIVATE
    NIMRTC_REGISTRY_EXPORTS=1)

target_include_directories(nimrtc_registry
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/plugins/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/modules/video_frame/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/dtls/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/sctp/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/raw_udp/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/transport/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(nimrtc_registry
    PUBLIC
        # `PluginRegistry::instance()` and the register_* bodies only
        # reference PluginRegistry's own TypedRegistry<T> members, plus
        # the function-pointer-array `kDefaultRegistrars[]` from
        # `nimrtc_core_objects`.  Linking nimrtc_core_objects publicly
        # makes both .a / .lib archives transitively available to
        # consumers.
        nimrtc_core_objects)
# PAL Slice 2 — `pal_default_registrars.cpp` preprocessor-guards some entries
# behind NIMRTC_HAS_* macros that are PUBLIC-ly defined on the per-module
# nimrtc_video_* / nimrtc_opus / nimrtc_h264 targets.  Because the .cpp is
# compiled into nimrtc_core_objects (not into each module), those macros are
# NOT visible at this TU's compile time — the registrations would silently
# vanish, breaking test_engine_video_plugins and test_engine_video_codec.
# Mirror every relevant NIMRTC_HAS_* define from the corresponding
# NIMRTC_MODULE_* option so the guarded entries materialise correctly.
#
# These macros only affect `pal_default_registrars.cpp` (the OBJECT library);
# they do NOT need to be mirrored on the new `nimrtc_registry` STATIC lib
# because `plugin_registry.cpp` does not reference `kDefaultRegistrars[]`.
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
nimrtc_apply_options(nimrtc_registry)

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
        # Windows DLL-boundary fix (v0.11.0): the PluginRegistry
        # singleton must come from a STATIC library so MSVC sees a
        # single, unified instance across all consumers
        # (`nimrtc_dtls_seam.lib`, `test_engine_plugin_loading.exe`,
        # etc).  Link order matters: putting `nimrtc_registry` first
        # lets the linker resolve `PluginRegistry::instance()` to the
        # one definition in `plugin_registry.cpp` before it consumes
        # the OBJECT library's `kDefaultRegistrars[]` indirect refs.
        nimrtc_registry
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

# ---------------------------------------------------------------------------
# Alias — `nimrtc::registry` exposes the singleton library under the
# `nimrtc::` namespace convention for downstream consumers that want
# the registry without the rest of nimrtc::core (rare; tests / tools).
# ---------------------------------------------------------------------------
add_library(nimrtc::registry ALIAS nimrtc_registry)

if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
