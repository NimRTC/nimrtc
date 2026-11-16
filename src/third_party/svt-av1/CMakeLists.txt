# third_party/svt-av1/CMakeLists.txt
# =============================================================================
# SVT-AV1 — the software AV1 encoder (Alliance for Open Media).
#
# Vendored from: https://github.com/AOMediaCodec/SVT-AV1 (v4.3.0) under src/.
#
# WHY IT IS HERE (remote-desktop plan B11)
# ----------------------------------------
# The plan's RD-2 decision allows vendoring permissive-licensed codecs, and
# its selection table names SVT-AV1 as the *only* cross-platform software
# AV1 encoder that carries the **screen-content tools** (intra block copy,
# `--scm`) a remote control needs.  libaom is excluded from the plan for
# build cost / latency; the hardware encoders (Track B5 / B5a / B6) are the
# primary path and SVT-AV1 is the CPU fallback.
#
# Licence: BSD-2-Clause Clear + AOM Patent License 1.0 (see
# src/third_party/svt-av1/src/LICENSE.md and PATENTS.md).  Both notices are
# reproduced in the tree's NOTICE file.
#
# WHY A HAND-WRITTEN WRAPPER
# --------------------------
# Upstream ships a full CMake project of its own, with a `project()` call,
# `install()` rules, an apps subdirectory and its own test registration.
# Entering it with `add_subdirectory()` would pull all of that into
# NimRTC's build — and upstream registers ctest entries unconditionally,
# which is exactly the failure mode the gmssl wrapper documents.
#
# So the wrapper enters upstream in a *sanitised* way:
#
#   - the upstream options NimRTC does not want are forced OFF
#     (BUILD_APPS, BUILD_TESTING, the PGO / coverage machinery),
#   - `BUILD_SHARED_LIBS` is forced OFF so the result is a static archive
#     (L5: NimRTC never links a shared codec),
#   - the resulting `SvtAv1Enc` target is re-exported as
#     `nimrtc_vendor_svt_av1`.
#
# NASM (R-11)
# -----------
# Upstream's x86-64 fast paths are NASM assembly, and NASM is **not**
# guaranteed on a NimRTC build host (the Windows CI image has none, and
# neither does a stock macOS or Linux container without `nasm`).
#
# Upstream's own CMakeLists.txt calls `enable_language(ASM_NASM)`
# unconditionally on x86, which makes the configure step **fail** without
# NASM.  The plan's R-11 rule says: "a vendored dependency that cannot
# build everywhere is option-gated and defaults OFF where it does not
# build" — and, more precisely for this wrapper, a missing assembler must
# degrade the *speed*, never the *build*.
#
# So the wrapper probes for NASM itself and sets upstream's
# `COMPILE_C_ONLY=ON` when it is absent.  With that flag upstream compiles
# its plain-C fallbacks and skips every `.asm` file: the encoder still
# works, it is simply slower.  The build never fails because of a missing
# assembler, and the option stays OFF by default anyway.
# =============================================================================

set(_src_dir "${CMAKE_CURRENT_SOURCE_DIR}/src")

# The vendored switch is the "use the vendored source build" contract.
# There is NO system fallback (L5 forbids linking a system codec), so
# switching it off means the codec is simply not available and the
# registry falls through to the next AV1 backend.
if(NOT NIMRTC_VENDORED_SVT_AV1)
    add_library(nimrtc_vendor_svt_av1 INTERFACE)
    add_library(nimrtc::vendor::svt_av1 ALIAS nimrtc_vendor_svt_av1)
    set(NIMRTC_VENDORED_SVT_AV1_AVAILABLE OFF CACHE INTERNAL
        "The vendored SVT-AV1 encoder was actually built")
    return()
endif()

if(NOT EXISTS "${_src_dir}/Source/API/EbSvtAv1Enc.h")
    # The submodule is not populated.  Declare an empty stub target so the
    # configure step succeeds; a consumer that links it fails at link time
    # with a clear "unresolved external" rather than a CMake error.
    add_library(nimrtc_vendor_svt_av1 INTERFACE)
    add_library(nimrtc::vendor::svt_av1 ALIAS nimrtc_vendor_svt_av1)
    target_compile_definitions(nimrtc_vendor_svt_av1 INTERFACE NIMRTC_USE_SVT_AV1=1)
    set(NIMRTC_VENDORED_SVT_AV1_AVAILABLE OFF CACHE INTERNAL
        "The vendored SVT-AV1 encoder was actually built")
    return()
endif()

# -----------------------------------------------------------------------------
# Probe for a NASM assembler.
#
# The probe mirrors upstream's own logic (build/make and the upstream
# CMakeLists.txt both accept nasm >= 2.14 or yasm), but it never fails:
# a host with no assembler gets the portable C build.
# -----------------------------------------------------------------------------
set(_nimrtc_svt_av1_has_nasm OFF)
if(NOT WIN32 OR CMAKE_GENERATOR MATCHES "Visual Studio")
    find_program(NIMRTC_SVT_AV1_NASM_EXE
                 NAMES nasm
                 HINTS ENV NASM
                 DOC "NASM assembler (used by SVT-AV1's x86 fast paths)")
    if(NIMRTC_SVT_AV1_NASM_EXE)
        set(_nimrtc_svt_av1_has_nasm ON)
    endif()
else()
    # On Windows, CMake's `ASM_NASM` language is the supported route and
    # CMake itself locates nasm.exe / nasm.com through PATH.  Probe it the
    # same way upstream does, but *without* enabling the language: a
    # missing nasm must degrade the build, not break the configure step.
    find_program(NIMRTC_SVT_AV1_NASM_EXE
                 NAMES nasm nasm.exe
                 HINTS ENV NASM
                 DOC "NASM assembler (used by SVT-AV1's x86 fast paths)")
    if(NIMRTC_SVT_AV1_NASM_EXE)
        set(_nimrtc_svt_av1_has_nasm ON)
    endif()
endif()

if(_nimrtc_svt_av1_has_nasm)
    message(STATUS "  SVT-AV1           : NASM found (${NIMRTC_SVT_AV1_NASM_EXE}) — "
                   "the x86 fast paths are compiled in")
    set(_nimrtc_svt_av1_combine_c_only OFF)
else()
    message(STATUS "  SVT-AV1           : NASM not found — building the portable C path "
                   "(slower, but the build does not fail)")
    set(_nimrtc_svt_av1_combine_c_only ON)
endif()

# -----------------------------------------------------------------------------
# The upstream options NimRTC forces.
#
# They are set as CACHE ... FORCE because upstream reads them with `option()`
# — a normal variable set in the parent scope would be shadowed by the
# cache entry upstream's `option()` call creates.
#
# `BUILD_TESTING` is the exception: it is a *standard* CMake variable, and
# CMake's own `include(CTest)` uses it to decide whether to call
# `enable_testing()` at the top level.  Forcing it off in the global cache
# would therefore silence NimRTC's *own* test discovery, and `ctest` in
# the affected build tree would report "No tests were found" even though
# every test binary had been built.  It is therefore scoped to a
# directory-local variable instead: upstream's `option(BUILD_TESTING ...)`
# call does not overwrite an existing *normal* variable, so the normal
# variable is what upstream reads, and the global cache entry is left
# alone.
# -----------------------------------------------------------------------------
set(BUILD_APPS       OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(COVERAGE         OFF CACHE BOOL "" FORCE)
set(SVT_AV1_PGO      OFF CACHE BOOL "" FORCE)
set(SVT_INTERNAL_PGO_GENERATE OFF CACHE BOOL "" FORCE)
set(SVT_INTERNAL_PGO_USE       OFF CACHE BOOL "" FORCE)
set(SVT_AV1_NVTX     OFF CACHE BOOL "" FORCE)

# Directory-local override: upstream's `option(BUILD_TESTING ...)` keeps an
# already-defined normal variable, so this is what upstream sees.
set(BUILD_TESTING OFF)

# LOG_QUIET must stay OFF.
#
# Upstream's `LOG_QUIET=ON` path routes every logging macro through
# `MAYBE_UNUSED_ARGS`, whose `GET_MACRO_NAME(x , ##__VA_ARGS__, ...)`
# trick relies on the GNU preprocessor's comma-swallowing extension.
# MSVC's traditional preprocessor does not implement it, so the macro
# expands to a syntax error ("expression expected") in every translation
# unit that logs anything — which is most of them.  Keeping the option at
# its upstream default (OFF) sidesteps that path entirely; the encoder
# still logs, which is what NimRTC's own logger wants to see anyway.
set(LOG_QUIET        OFF CACHE BOOL "" FORCE)

set(COMPILE_C_ONLY   ${_nimrtc_svt_av1_combine_c_only} CACHE BOOL "" FORCE)

# Upstream's own CMakeLists.txt calls `project(svt-av1 ...)`, which resets
# a number of the variables NimRTC relies on (CMAKE_PROJECT_NAME and
# friends).  Entering it from a subdirectory is what upstream expects, so
# the wrapper accepts the side effects — but it records the values NimRTC
# cares about *before* the call and restores them after.
set(_nimrtc_svt_av1_saved_project "${PROJECT_NAME}")

add_subdirectory("${_src_dir}" "${CMAKE_CURRENT_BINARY_DIR}/svt-av1-build" EXCLUDE_FROM_ALL)

set(PROJECT_NAME "${_nimrtc_svt_av1_saved_project}")

if(NOT TARGET SvtAv1Enc)
    message(WARNING
        "SVT-AV1: the upstream build did not create the 'SvtAv1Enc' target — "
        "the software AV1 encoder will be reported as unavailable")
    add_library(nimrtc_vendor_svt_av1 INTERFACE)
    add_library(nimrtc::vendor::svt_av1 ALIAS nimrtc_vendor_svt_av1)
    target_compile_definitions(nimrtc_vendor_svt_av1 INTERFACE NIMRTC_USE_SVT_AV1=1)
    set(NIMRTC_VENDORED_SVT_AV1_AVAILABLE OFF CACHE INTERNAL
        "The vendored SVT-AV1 encoder was actually built")
    return()
endif()

# -----------------------------------------------------------------------------
# The umbrella target.
#
# `nimrtc_vendor_svt_av1` is what the rest of NimRTC links against.  It
# is an INTERFACE target that wraps upstream's static archive and
# propagates the public API include directory (Source/API) plus the
# compile definition that flips the AV1 module's "real encoder" path on.
# -----------------------------------------------------------------------------
add_library(nimrtc_vendor_svt_av1 INTERFACE)
add_library(nimrtc::vendor::svt_av1 ALIAS nimrtc_vendor_svt_av1)
target_link_libraries(nimrtc_vendor_svt_av1 INTERFACE SvtAv1Enc)

# SYSTEM: upstream headers must not be subject to NimRTC's own -Werror
# warning set.  Without SYSTEM, GCC/Clang apply those flags to every
# upstream header include and turn upstream warnings into NimRTC build
# failures.
target_include_directories(nimrtc_vendor_svt_av1 SYSTEM INTERFACE
    $<BUILD_INTERFACE:${_src_dir}/Source/API>)

target_compile_definitions(nimrtc_vendor_svt_av1 INTERFACE NIMRTC_USE_SVT_AV1=1)

# The wrapper built the real encoder: tell the consumers so they compile the
# encoder body and register the `svt_av1` row.
set(NIMRTC_VENDORED_SVT_AV1_AVAILABLE ON CACHE INTERNAL
    "The vendored SVT-AV1 encoder was actually built")

# The MSVC runtime must match the rest of the tree: upstream is built as
# part of the same process as every NimRTC target, so a /MD vs /MDd split
# produces two CRT heaps and an LNK4098 at link time.
if(MSVC AND CMAKE_MSVC_RUNTIME_LIBRARY AND TARGET SvtAv1Enc)
    set_target_properties(SvtAv1Enc PROPERTIES
        MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
endif()
