# third_party/libvpx/CMakeLists.txt
# =============================================================================
# libvpx — the WebM project's VP8 / VP9 codec (optional software codec).
#
# Vendored from: https://github.com/webmproject/libvpx (v1.17.0) under src/.
#
# WHY IT IS HERE (remote-desktop plan B11)
# ----------------------------------------
# B11 asks for an *optional* software VP9 encoder behind
# `NIMRTC_VENDORED_LIBVPX`, which defaults OFF.  libvpx is the reference
# VP9 implementation and the only one with a permissive licence
# (BSD-3-Clause) that clears the plan's RD-2 licence filter.
#
# Licence: BSD-3-Clause (see src/third_party/libvpx/src/LICENSE).  The
# notice is reproduced in the tree's NOTICE file.
#
# WHY A HAND-WRITTEN WRAPPER
# --------------------------
# Upstream libvpx ships a hand-rolled `configure` script (borrowed from
# FFmpeg), NOT a CMake project.  Its build also *requires*:
#
#   - `perl`  — every `rtcd.pl` script generates the per-function C
#               dispatch tables (vpx_dsp_rtcd_defs.pl and friends), and
#               the generated `*_rtcd.c` files are NOT checked in;
#   - `make`  — the generated Makefiles drive the compile;
#   - `yasm` / `nasm` — the x86-64 assembly fast paths.
#
# On a stock Windows CI host NONE of `perl`, `make` or `nasm` is present.
# R-11 (the plan's toolchain-graceful-degradation rule) says a vendored
# dependency that cannot build everywhere is option-gated and defaults
# OFF where it does not build — and, more precisely for this wrapper, a
# missing tool must degrade the *speed* or the *availability*, never the
# *build*.
#
# So the wrapper probes the prerequisites itself:
#
#   - `NIMRTC_VENDORED_LIBVPX=OFF`  → an empty INTERFACE target.  The
#     option is the switch; there is NO system fallback (L5 forbids
#     linking a system codec), so OFF means "not available", not "use
#     the system copy".
#   - the vendored tree absent      → an empty INTERFACE target.
#   - `perl` / `make` absent        → an empty INTERFACE target, with a
#     clear diagnostic naming the missing tool.  The build still
#     configures; the VP9 row is simply *absent* from the registry.
#   - everything present            → the upstream sources are compiled
#     directly with CMake, and the resulting static archive is
#     re-exported as `nimrtc_vendor_libvpx`.
#
# WHY THE SOURCES ARE COMPILED DIRECTLY
# -------------------------------------
# The upstream Makefile build depends on the generated Makefiles and on
# `make` itself, so it cannot be driven from CMake on a host without
# `make`.  Compiling the upstream translation units directly from CMake
# (the same pattern the openh264 / dav1d wrappers use) removes the
# `make` dependency; the `perl` dependency remains for the `rtcd.pl`
# generators, so the wrapper *pre-generates* the `*_rtcd.c` files with
# perl at configure time when perl is available.
#
# The assembler is never required: upstream's assembly is optional, and
# the wrapper builds the portable C path only (`--disable-asm`).
# =============================================================================

set(_src_dir "${CMAKE_CURRENT_SOURCE_DIR}/src")

# The vendored switch is the "use the vendored source build" contract.
# There is NO system fallback (L5 forbids linking a system codec), so
# switching it off means the codec is simply not available and the
# registry falls through to the VP9 software stub.
if(NOT NIMRTC_VENDORED_LIBVPX)
    add_library(nimrtc_vendor_libvpx INTERFACE)
    add_library(nimrtc::vendor::libvpx ALIAS nimrtc_vendor_libvpx)
    set(NIMRTC_VENDORED_LIBVPX_AVAILABLE OFF CACHE INTERNAL
        "The vendored libvpx codec was actually built (needs a perl interpreter)")
    return()
endif()

if(NOT EXISTS "${_src_dir}/vpx/vpx_encoder.h")
    # The submodule is not populated.  Declare an empty stub target so the
    # configure step succeeds; a consumer that links it fails at link time
    # with a clear "unresolved external" rather than a CMake error.
    add_library(nimrtc_vendor_libvpx INTERFACE)
    add_library(nimrtc::vendor::libvpx ALIAS nimrtc_vendor_libvpx)
    target_compile_definitions(nimrtc_vendor_libvpx INTERFACE NIMRTC_USE_LIBVPX=0)
    return()
endif()

# -----------------------------------------------------------------------------
# Probe the toolchain.
#
# `perl` is mandatory: upstream's `rtcd.pl` generators produce the
# per-function C dispatch tables, and those generated files are not
# checked in.  Without perl the codec cannot be built at all — so the
# wrapper degrades to the empty stub rather than failing the configure
# step (R-11).
#
# `make` is *not* needed: the wrapper compiles the upstream translation
# units directly with CMake.
# -----------------------------------------------------------------------------
find_program(NIMRTC_LIBVPX_PERL_EXE
             NAMES perl perl.exe
             DOC "Perl interpreter (libvpx's rtcd.pl code generators require it)")

if(NOT NIMRTC_LIBVPX_PERL_EXE)
    message(STATUS "  libvpx            : perl not found — the software VP9 "
                   "encoder is absent from the registry (R-11: the build "
                   "degrades, it does not fail)")
    add_library(nimrtc_vendor_libvpx INTERFACE)
    add_library(nimrtc::vendor::libvpx ALIAS nimrtc_vendor_libvpx)
    target_compile_definitions(nimrtc_vendor_libvpx INTERFACE NIMRTC_USE_LIBVPX=0)
    set(NIMRTC_VENDORED_LIBVPX_AVAILABLE OFF CACHE INTERNAL
        "The vendored libvpx codec was actually built (needs a perl interpreter)")
    return()
endif()

message(STATUS "  libvpx            : perl found (${NIMRTC_LIBVPX_PERL_EXE})")

# -----------------------------------------------------------------------------
# The upstream source layout.
#
# libvpx's own build splits the sources into three sets, all of which are
# needed:
#
#   vpx/     — the codec-independent core (bitstream, decoder, encoder
#              plumbing, the public API in vpx/vpx_encoder.h and
#              vpx/vpx_decoder.h)
#   vpx_dsp/ — the SIMD kernels
#   vp8/     — the VP8 codec
#   vp9/     — the VP9 codec
#   third_party/ — the bundled dependencies (libyuv, pthreadpool-ish
#              shims)
#
# The list is generated by globbing rather than by a hand-maintained
# list, because upstream adds and removes files between releases and a
# stale hand-written list is a silent build break.
# -----------------------------------------------------------------------------

file(GLOB_RECURSE _nimrtc_libvpx_sources
     "${_src_dir}/vpx/*.c"
     "${_src_dir}/vpx_dsp/*.c"
     "${_src_dir}/vp8/*.c"
     "${_src_dir}/vp9/*.c")

# The generated dispatch tables must NOT be compiled twice: upstream
# compiles `*_rtcd.c` and excludes the hand-written `*.c` that the
# generator produces *from* them.  The generator writes `<name>_rtcd.c`
# next to `<name>.c`; the hand-written file is the one WITHOUT the
# `_rtcd` suffix.  So the hand-written `vpx_dsp_rtcd_defs.c`-style
# files are excluded and the generated ones are added explicitly.
list(FILTER _nimrtc_libvpx_sources EXCLUDE REGEX "_rtcd\\.c$")

# The `*_rtcd.c` files are generated from `*.pl` scripts.  Upstream lists
# the generator scripts next to the sources; the mapping is mechanical
# (`foo.pl` -> `foo_rtcd.c`), so it is derived here.
file(GLOB_RECURSE _nimrtc_libvpx_rtcd_pl
     "${_src_dir}/vpx_dsp/*.pl"
     "${_src_dir}/vp8/*.pl"
     "${_src_dir}/vp9/*.pl")

set(_nimrtc_libvpx_rtcd_c)
set(_nimrtc_libvpx_rtcd_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/rtcd")
file(MAKE_DIRECTORY "${_nimrtc_libvpx_rtcd_gen_dir}")

foreach(_pl IN LISTS _nimrtc_libvpx_rtcd_pl)
    get_filename_component(_pl_name "${_pl}" NAME_WE)
    # `vpx_dsp_rtcd_defs.pl` -> `vpx_dsp_rtcd_defs_rtcd.c`
    set(_c "${_nimrtc_libvpx_rtcd_gen_dir}/${_pl_name}_rtcd.c")
    add_custom_command(
        OUTPUT "${_c}"
        COMMAND "${NIMRTC_LIBVPX_PERL_EXE}" "${_pl}"
                --outfile="${_c}"
        DEPENDS "${_pl}"
        COMMENT "Generating ${_pl_name}_rtcd.c"
        VERBATIM)
    list(APPEND _nimrtc_libvpx_rtcd_c "${_c}")
endforeach()

# -----------------------------------------------------------------------------
# The configuration header.
#
# libvpx's sources include "vpx/vpx_config.h", which upstream's build
# generates from `build/mk/config.h.in`.  The wrapper writes a minimal
# but *correct* one: it enables the features NimRTC needs and disables
# everything that would need an assembler or a system library.
#
# The generated header is written into the binary tree — the vendored
# source tree is never modified (the submodule must stay vendor-clean).
# -----------------------------------------------------------------------------
set(_cfg_dir "${CMAKE_CURRENT_BINARY_DIR}/include/vpx")
file(MAKE_DIRECTORY "${_cfg_dir}")
set(_cfg "${_cfg_dir}/vpx_config.h")

file(WRITE "${_cfg}"
"/* Generated by NimRTC's libvpx wrapper — do not edit.
 *
 * Upstream generates this file from build/mk/config.h.in during its
 * `configure` step.  NimRTC's wrapper writes a minimal, portable
 * configuration instead: the SIMD is compiled out (no assembler is
 * guaranteed on a build host — R-11), and the threading model is the
 * single-threaded one so no pthread / pthreads-win32 dependency is
 * needed.
 */
#ifndef VPX_CONFIG_H
#define VPX_CONFIG_H

/* --- Feature switches --------------------------------------------------- */
/* No assembler: the portable C paths are used everywhere. */
#define CONFIG_ASM 0

/* Threads: single-threaded.  NimRTC's video pipeline already owns the
 * thread that drives the codec, and a thread pool inside the codec
 * would fight it. */
#define CONFIG_PTHREADS 0
#define HAVE_PTHREAD_H 0

/* --- Feature detection -------------------------------------------------- */
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_UNISTD_H 0
#define HAVE_SYS_TYPES_H 0
#define HAVE_SYS_TIME_H 0
#define HAVE_SYS_MMAN_H 0
#define HAVE_SYS_SOCKET_H 0
#define HAVE_LINUX_IOMUX_H 0
#define HAVE_PTHREAD 0

/* The platform's default word size — libvpx asserts on it. */
#define SIZEOF_POINTER ${CMAKE_SIZEOF_VOID_P}

#endif /* VPX_CONFIG_H */
")

# -----------------------------------------------------------------------------
# The library.
# -----------------------------------------------------------------------------
add_library(nimrtc_vendor_libvpx STATIC
    ${_nimrtc_libvpx_sources}
    ${_nimrtc_libvpx_rtcd_c})

add_library(nimrtc::vendor::libvpx ALIAS nimrtc_vendor_libvpx)

target_include_directories(nimrtc_vendor_libvpx SYSTEM PUBLIC
    $<BUILD_INTERFACE:${_src_dir}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>)

# The generated `*_rtcd.c` files include their siblings by relative path,
# so the *source* directories must be on the include path too.
target_include_directories(nimrtc_vendor_libvpx SYSTEM PRIVATE
    "${_src_dir}"
    "${_src_dir}/vpx"
    "${_src_dir}/vpx_dsp"
    "${_src_dir}/vp8"
    "${_src_dir}/vp8/common"
    "${_src_dir}/vp9"
    "${_src_dir}/vp9/common")

target_compile_definitions(nimrtc_vendor_libvpx PUBLIC NIMRTC_USE_LIBVPX=1)

# The wrapper built the real codec: tell the consumers so they compile the
# encoder body and register the `libvpx_vp9` row.
set(NIMRTC_VENDORED_LIBVPX_AVAILABLE ON CACHE INTERNAL
    "The vendored libvpx codec was actually built (needs a perl interpreter)")

# Upstream's sources are not NimRTC's code: they must not be subject to
# NimRTC's own -Werror warning set.
if(MSVC)
    target_compile_options(nimrtc_vendor_libvpx PRIVATE /w)
else()
    target_compile_options(nimrtc_vendor_libvpx PRIVATE -w)
endif()

# The MSVC runtime must match the rest of the tree: upstream is built as
# part of the same process as every NimRTC target, so a /MD vs /MDd split
# produces two CRT heaps and an LNK4098 at link time.
if(MSVC AND CMAKE_MSVC_RUNTIME_LIBRARY)
    set_target_properties(nimrtc_vendor_libvpx PROPERTIES
        MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
endif()
