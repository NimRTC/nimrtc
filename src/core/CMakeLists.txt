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
#           `register_*()` method.  See `src/core/src/plugin_registry.cpp`
#           for the file header explaining the Windows DLL rationale.
#
#           The one external symbol it references is
#           `nimrtc::core::log::Logger::instance()` — the ADR-023
#           Decision 4 rejection diagnostic in `TypedRegistry::register_one`
#           (registry.hpp) reports a malformed plugin id through the core
#           logger.  That makes `nimrtc_registry` depend on `nimrtc_log`,
#           and the dependency has to be *declared* below: a GNU-ld link
#           resolves archives left to right, so the archive holding
#           `Logger::instance()` must come after the one that references
#           it.  Without the declaration the Linux link fails with
#           "undefined reference to nimrtc::core::log::Logger::instance()"
#           even though the symbol is in the binary.  (MSVC does not care
#           about archive order, so this only showed up on Linux CI.)
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
        $<INSTALL_INTERFACE:include>
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/plugins/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/modules/video_frame/include>
        # Transport PAL Slice 8 (v0.10.2): the TypedRegistry<...> fields
        # in registry.hpp forward-declare the DTLS / SCTP / raw_udp /
        # transport-stack factory types but the static_cast calls in
        # `detail::Registrar::Registrar()` need the full type. The
        # matching headers live in the seam modules' include dirs.
        #
        # These are PRIVATE since the P1 hardening (2026-11-09): the
        # back-compat shim at the bottom of nimrtc/core/registry.hpp no
        # longer includes the optional-module seam headers, so the core
        # PUBLIC interface does not drag the optional modules in any
        # more.  A consumer that needs a seam type includes that seam's
        # header (and links that module) explicitly.
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/modules/dtls/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/modules/sctp/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/modules/raw_udp/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/modules/transport/include>
        # v0.14.0 alpha.2 (ADR-014): the WebTransport factory type is
        # needed by the static_cast in `detail::Registrar::invoke()` for
        # `Category::kWebTransport`.
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/modules/web_transport/include>
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
        $<INSTALL_INTERFACE:include>
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/plugins/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/modules/video_frame/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/modules/dtls/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/modules/sctp/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/modules/raw_udp/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/modules/transport/include>
        # v0.14.0 alpha.2 (ADR-014): the new `register_web_transport`
        # method in plugin_registry.cpp takes a
        # `const web_transport::IWebTransportSessionFactory*`
        # argument; the full type must be visible at the call
        # site so the TypedRegistry<...> member template can
        # resolve.  Same pattern as the Slice 8 / PAL Slice 2
        # dtls/sctp/raw_udp/transport include paths above.
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/modules/web_transport/include>)
target_link_libraries(nimrtc_registry
    PUBLIC
        # `PluginRegistry::instance()` and the register_* bodies only
        # reference PluginRegistry's own TypedRegistry<T> members, plus
        # the function-pointer-array `kDefaultRegistrars[]` from
        # `nimrtc_core_objects`.  Linking nimrtc_core_objects publicly
        # makes both .a / .lib archives transitively available to
        # consumers.
        nimrtc_core_objects
    PRIVATE
        # ADR-023 Decision 4: `TypedRegistry::register_one` (registry.hpp)
        # reports a malformed plugin id through
        # `nimrtc::core::log::Logger::instance()`, so this archive now
        # references a symbol defined in `nimrtc_log`.
        #
        # The dependency has to be *declared*, because a GNU-ld link
        # resolves static archives left to right: the archive holding
        # the definition has to come after the one that references it.
        # Without this line, CMake put `libnimrtc_log.a` *before*
        # `libnimrtc_registry.a` on the link line and every Linux target
        # that linked the registry failed with "undefined reference to
        # nimrtc::core::log::Logger::instance()" — a symbol that was
        # present in the binary the whole time.  MSVC does not reorder
        # archives the same way, so the breakage only showed up on the
        # Linux CI jobs.
        #
        # PRIVATE is right: the logger is an implementation detail of
        # this archive, not part of the public header surface.  For a
        # STATIC library CMake still propagates it to consumers (as
        # `$<LINK_ONLY:>`), which is exactly what puts `nimrtc_log` on
        # their link line — after this archive — and fixes the order.
        nimrtc_log)
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
# Remote-desktop plan A1: the capture-device module's registrar entry
# materialises only when the module is built.  Same shape as the
# video_source entry above.
if(NIMRTC_MODULE_VIDEO_CAPTURE)
    target_compile_definitions(nimrtc_core_objects PRIVATE NIMRTC_HAS_VIDEO_CAPTURE=1)
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
# v1.3 (video plan C4 / C8): the H.265 codec module.  Its registrar entry
# materialises only when the module is built; the engine then resolves it
# through the registry's video-codec slot.
if(NIMRTC_MODULE_H265)
    target_compile_definitions(nimrtc_core_objects PRIVATE NIMRTC_HAS_H265=1)
endif()
# v1.3 (video plan C6 / C8): the AV1 codec module.  Same shape as the
# H.265 entry above.
if(NIMRTC_MODULE_AV1)
    target_compile_definitions(nimrtc_core_objects PRIVATE NIMRTC_HAS_AV1=1)
endif()
# v1.2.0 (audio pipeline): the audio receive-pipeline module's own
# visibility macro is set by the module itself (src/modules/audio_rx/
# CMakeLists.txt, PUBLIC).  It is mirrored here so the kDefaultRegistrars[]
# entry for the module — added with WP-7 — materialises only when the
# module is actually built.  When the module is OFF, the engine's audio
# receive pipeline cannot be resolved, and the engine reports the hard
# error `kAudioReceiverMissing` (spec B-16, no fallback receiver).
if(NIMRTC_MODULE_AUDIO_RX)
    target_compile_definitions(nimrtc_core_objects PRIVATE NIMRTC_HAS_AUDIO_RX=1)
endif()
# v1.2.0 M1 (F2): the network-feedback collector module.  Its registrar
# entry materialises only when the module is built; the engine then
# resolves it through the registry's net-feedback slot.
if(NIMRTC_MODULE_NET_FEEDBACK)
    target_compile_definitions(nimrtc_core_objects PRIVATE NIMRTC_HAS_NET_FEEDBACK=1)
endif()
# v1.2.0 (session capability plan, N1 / T1): the session-model module.
# Its registrar entry materialises only when the module is built; the
# engine then resolves the session model through the registry's
# session slot.
if(NIMRTC_MODULE_SESSION)
    target_compile_definitions(nimrtc_core_objects PRIVATE NIMRTC_HAS_SESSION=1)
endif()
# v1.2.0 (session capability plan, D1 / D2): the RFC 4733 codec module.
# Its registrar entry materialises only when the module is built; the
# engine then resolves the codec through the registry's DTMF slot.
if(NIMRTC_MODULE_DTMF)
    target_compile_definitions(nimrtc_core_objects PRIVATE NIMRTC_HAS_DTMF=1)
endif()
# v1.2.0 (remediation plan B7 / Q3): the SRTP crypto-backend module.  Its
# registrar entry materialises only when the module is built; the engine
# then resolves the crypto backend through the registry's SRTP slot.
if(NIMRTC_MODULE_SRTP)
    target_compile_definitions(nimrtc_core_objects PRIVATE NIMRTC_HAS_SRTP=1)
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
        # The registry's public header (<nimrtc/core/registry.hpp>) only
        # forward-declares the factory interfaces, so it needs nothing
        # from the plugins layer at compile time.  Linking nimrtc::plugins
        # here still gives every consumer the plugins include path and
        # the seam headers it needs when it wants them -- and it keeps the
        # core header itself free of optional-module includes (P1
        # hardening, 2026-11-09).  Code that wants the old one-header
        # convenience includes <nimrtc/plugins/registry.hpp>, which pulls
        # in the 13 nimrtc/plugins/ seam interfaces.
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
