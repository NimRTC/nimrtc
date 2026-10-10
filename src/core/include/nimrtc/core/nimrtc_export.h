/**
 * @file nimrtc/core/nimrtc_export.h
 * @brief DLL export/import macro for NimRTC public symbols (PAL Slice 8 fix).
 *
 * Why this exists
 * ---------------
 *
 * Prior to Slice 8, NimRTC built exclusively as static libraries.  The
 * `PluginRegistry` class was defined inline in `registry.hpp`, which
 * meant every translation unit that included the header emitted its
 * own copy of `PluginRegistry::instance()`'s function-local static.
 *
 * On GCC/Clang this works because the linker folds identical inline
 * function bodies across TUs (the static local ends up unified in the
 * final binary).  On MSVC, however, function-local statics in inline
 * functions are NOT subject to COMDAT folding — each TU gets its own
 * copy, and when multiple static libraries are linked together, each
 * static library embeds its own copy of the .obj.  Concretely:
 *
 *   nimrtc_dtls_seam.lib     → writes "wolfssl" into PluginRegistry A
 *   test_engine_plugin_loading.exe → reads from PluginRegistry B  ← different instance!
 *
 * The fix is structural: move `instance()` to a .cpp file and put it in
 * a single STATIC library that's transitively linked by every consumer.
 * That requires `nimrtc_core_objects` to change from CMake `OBJECT` to
 * `STATIC` (see `src/core/CMakeLists.txt`).
 *
 * But there is a longer-term story too: the registry could be moved to
 * a SHARED library to give it true cross-DLL singletons for the
 * Slice-7 / Slice-8 transport-stack composition.  This header prepares
 * for that by providing a single `NIMRTC_API` macro that means
 *
 *   - on Windows + shared library : `__declspec(dllimport)` /
 *                                   `__declspec(dllexport)` (decided
 *                                   per-translation-unit by CMake);
 *   - everywhere else            : empty (no special visibility).
 *
 * It is consumed by `src/core/include/nimrtc/core/registry.hpp` to mark
 * `PluginRegistry`'s public methods.  Once the registry moves to a
 * SHARED library the same macro continues to work without header
 * changes — CMake's per-target `*_EXPORTS` definition flips the
 * macro's expansion automatically.
 *
 * ## Usage
 *
 * ```cpp
 * #include <nimrtc/core/nimrtc_export.h>
 *
 * class NIMRTC_API PluginRegistry {
 *     NIMRTC_API static PluginRegistry& instance();
 *     NIMRTC_API void register_dtls_session(std::string_view, const IDtlsSessionFactory*);
 *     // ...
 * };
 * ```
 *
 * @note P1 — added as part of the Windows DLL boundary fix for the
 *       Transport PAL Slice 4 / Slice 8 registry singleton (v0.11.0).
 */

#ifndef NIMRTC_CORE_NIMRTC_EXPORT_H
#define NIMRTC_CORE_NIMRTC_EXPORT_H

// ---------------------------------------------------------------------------
// NIMRTC_API
// ---------------------------------------------------------------------------
//
// CMake's `generate_export_header` and the per-target
// `<target_name>_EXPORTS` define combine to give us:
//
//   - When building `nimrtc_core_objects` (i.e. the DLL is being built):
//       NIMRTC_CORE_OBJECTS_EXPORTS is defined by CMake →
//       NIMRTC_API expands to __declspec(dllexport) on MSVC.
//   - When consuming `nimrtc_core_objects` (every other target): the
//     macro expands to __declspec(dllimport) on MSVC, and is empty on
//     GCC / Clang / Apple Clang.
//
// We rely on CMake's standard `<target_name>_EXPORTS` convention rather
// than a hand-written `#ifdef nimrtc_core_objects_EXPORTS` block so the
// macro tracks whatever library the symbol happens to live in — which
// keeps `registry.hpp` agnostic to whether `nimrtc_core_objects` ends
// up as STATIC or SHARED in the future.
//
// The fallback branch (no `_EXPORTS` defined) treats NIMRTC_API as a
// no-op so that consumers building against a header-only / static-only
// tree (e.g. an external downstream using NimRTC as a header library)
// still compile.
// ---------------------------------------------------------------------------

#if defined(_WIN32) || defined(__CYGWIN__)
  // Only one of two scenarios exists per consumer:
  //   (a) Building `nimrtc_registry` (the DLL/SO that emits the
  //       singleton)         : NIMRTC_REGISTRY_EXPORTS is set by CMake →
  //                             expand to __declspec(dllexport).  This is
  //                             what makes the symbols externally visible.
  //   (b) All other targets  : NIMRTC_REGISTRY_EXPORTS is NOT set → expand
  //                             to nothing.  Importantly this is also
  //                             true for static-library consumers
  //                             (`nimrtc_dtls_seam.lib`,
  //                             `nimrtc_sctp.lib`, every .exe / .dll that
  //                             statically links `nimrtc_registry.lib`):
  //                             there is no DLL boundary here, so no
  //                             dllimport decorator is needed.  Using
  //                             `__declspec(dllimport)` for static
  //                             consumers would emit `__imp_*` symbols
  //                             and break the link with LNK2019 because
  //                             no `.dll` exists to import from.
  //
  // If the registry is later converted to a real SHARED library (Slice
  // 7.5 follow-up), add a CMake-defined `nimrtc_registry_EXPORTS`-like
  // guard for the consumer side (`__declspec(dllimport)`) AND make sure
  // the import library (`nimrtc_registry.lib`) is linked in addition to
  // (or instead of) the static archive.
  #if defined(NIMRTC_REGISTRY_EXPORTS)
    #define NIMRTC_API __declspec(dllexport)
  #else
    #define NIMRTC_API
  #endif
  // MinGW / Cygwin symbol-visibility hint (harmless on MSVC proper).
  #define NIMRTC_HIDDEN __declspec(dllexport)
#else
  // GCC / Clang / Apple Clang — symbol visibility is controlled via
  // -fvisibility=hidden at the project level (see cmake/NimRTCOptions.cmake).
  // NIMRTC_API is therefore an empty attribute on non-Windows toolchains.
  #if defined(__GNUC__) && (__GNUC__ >= 4)
    #define NIMRTC_API          __attribute__((visibility("default")))
    #define NIMRTC_HIDDEN       __attribute__((visibility("hidden")))
  #else
    #define NIMRTC_API
    #define NIMRTC_HIDDEN
  #endif
#endif

#endif // NIMRTC_CORE_NIMRTC_EXPORT_H
