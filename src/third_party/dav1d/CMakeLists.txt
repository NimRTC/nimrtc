# third_party/dav1d/CMakeLists.txt
# =============================================================================
# dav1d — AV1 decoder (VideoLAN / dav1d authors, BSD-2-Clause).
#
# Vendored from: https://code.videolan.org/videolan/dav1d (v1.5.1) under src/.
#
# WHY A HAND-WRITTEN WRAPPER
# --------------------------
# Upstream ships a Meson build, not a CMakeLists.txt.  Pulling Meson into
# the NimRTC build would add a second build system, a second dependency,
# and an out-of-tree artifact that CI would have to provision.  This file
# therefore compiles the upstream sources directly with CMake and exposes
# the target name the other vendors use:
#
#     nimrtc_vendor_dav1d   (alias nimrtc::vendor::dav1d)
#
# WHAT THE WRAPPER REPLICATES
# ----------------------------
# Upstream's Meson build does three things that a naive source list would
# miss; all three are reproduced here:
#
#   1. It generates `config.h` (a few dozen `#define`s describing the
#      platform: which bitdepths, whether assembly exists, whether
#      `posix_memalign` exists, which architecture macros are set).
#      This wrapper writes the same header with the same names.
#   2. It generates `vcs_version.h` (a one-line version string).  Written
#      here too, with the pinned release instead of a `git describe`.
#   3. It compiles the `*_tmpl.c` files ONCE PER BITDEPTH with
#      `-DBITDEPTH=8` / `-DBITDEPTH=16`.  This wrapper builds the 8-bit
#      instantiation only, which is what NimRTC's I420 pipeline needs.
#      The templated sources are compiled into a separate OBJECT library
#      so the per-bitdepth `-D` reaches them without leaking into the
#      non-templated sources.
#
# ASM
# ---
# `HAVE_ASM=0`: the x86 fast paths are NASM, the ARM ones are GAS `.S`.
# Neither assembler is guaranteed on a NimRTC build host, so the wrapper
# compiles the portable C paths.  The decoder is correct, just slower —
# which is exactly what the software baseline (remote-desktop plan B3)
# needs: a *real* AV1 decoder, not a stub.
#
# LICENCE
# -------
# BSD-2-Clause (see src/vendor/dav1d/src/COPYING).
# =============================================================================

set(_dav1d_root "${CMAKE_CURRENT_SOURCE_DIR}/src")

# The vendored switch is the "use the vendored source build" contract.
# There is NO system fallback (L5 forbids linking a system codec), so
# switching it off means the codec is simply not available and the
# registry falls through to the software stub.
if(NOT NIMRTC_VENDORED_DAV1D)
    add_library(nimrtc_vendor_dav1d INTERFACE)
    add_library(nimrtc::vendor::dav1d ALIAS nimrtc_vendor_dav1d)
    return()
endif()

if(NOT EXISTS "${_dav1d_root}/include/dav1d/dav1d.h")
    # The submodule is not populated.  Declare an empty stub target so the
    # configure step succeeds; a consumer that links it fails at link time
    # with a clear "unresolved external" rather than a CMake error.
    add_library(nimrtc_vendor_dav1d INTERFACE)
    add_library(nimrtc::vendor::dav1d ALIAS nimrtc_vendor_dav1d)
    target_compile_definitions(nimrtc_vendor_dav1d INTERFACE NIMRTC_USE_DAV1D=1)
    return()
endif()

set(_dav1d_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")

# -----------------------------------------------------------------------------
# config.h — the platform description upstream's Meson build generates.
#
# Every macro here is referenced by upstream sources; the names and the
# 0/1 semantics come straight from upstream's meson.build (cdata.set10).
# -----------------------------------------------------------------------------
set(_cfg "")
# `_cfg10(<name> <value>)` writes "#define <name> 1" or "#define <name> 0".
# The value is normalised so that an UNDEFINED variable (which is what
# check_include_file() / check_symbol_exists() leave behind on a "not
# found" probe) is treated as "false" rather than blowing up the
# `if()` inside the macro.
macro(_cfg10 name value)
    if("${value}" STREQUAL "" OR "${value}" STREQUAL "FALSE" OR "${value}" STREQUAL "OFF" OR "${value}" STREQUAL "0" OR "${value}" STREQUAL "NO")
        string(APPEND _cfg "#define ${name} 0\n")
    else()
        string(APPEND _cfg "#define ${name} 1\n")
    endif()
endmacro()

# --- bitdepths: 8-bit only (I420 is the only format NimRTC's video path
#     carries; the 16-bit instantiation would double compile time for a
#     format no NimRTC sink can consume).
_cfg10(CONFIG_8BPC  TRUE)
_cfg10(CONFIG_16BPC FALSE)

# --- assembly: always off (see the header comment).
_cfg10(HAVE_ASM FALSE)

# --- DSP trimming: off.  Upstream's default is "if-release", which removes
#     the generic C fallbacks on release builds — but our build IS the
#     generic C build (no ASM), so trimming would delete the only
#     implementations.  Must stay 0.
_cfg10(TRIM_DSP_FUNCTIONS FALSE)

# --- logging: off.  NimRTC routes codec diagnostics through its own logger
#     (nimrtc::core::log); dav1d's own stderr callback would bypass it.
_cfg10(CONFIG_LOG FALSE)

_cfg10(CONFIG_MACOS_KPERF FALSE)

# --- memory alignment primitives.
if(WIN32)
    # MSVC / MinGW: `_aligned_malloc` is used unconditionally (see
    # upstream src/mem.h), so the POSIX probes are all 0.
    _cfg10(HAVE_POSIX_MEMALIGN FALSE)
    _cfg10(HAVE_MEMALIGN       FALSE)
    _cfg10(HAVE_ALIGNED_ALLOC  FALSE)
else()
    include(CheckSymbolExists)
    check_symbol_exists(posix_memalign "stdlib.h" _dav1d_have_posix_memalign)
    check_symbol_exists(memalign       "malloc.h" _dav1d_have_memalign)
    check_symbol_exists(aligned_alloc  "stdlib.h" _dav1d_have_aligned_alloc)
    _cfg10(HAVE_POSIX_MEMALIGN "${_dav1d_have_posix_memalign}")
    _cfg10(HAVE_MEMALIGN       "${_dav1d_have_memalign}")
    _cfg10(HAVE_ALIGNED_ALLOC  "${_dav1d_have_aligned_alloc}")
    unset(_dav1d_have_posix_memalign CACHE)
    unset(_dav1d_have_memalign CACHE)
    unset(_dav1d_have_aligned_alloc CACHE)
endif()

# --- dynamic loading of libdlsym helpers (Linux only upstream).
_cfg10(HAVE_DLSYM FALSE)
if(UNIX AND NOT APPLE)
    include(CheckSymbolExists)
    check_symbol_exists(dlsym "dlfcn.h" _dav1d_have_dlsym)
    if(_dav1d_have_dlsym)
        _cfg10(HAVE_DLSYM "${_dav1d_have_dlsym}")
    endif()
    unset(_dav1d_have_dlsym CACHE)
endif()

# --- clock.
_cfg10(HAVE_CLOCK_GETTIME FALSE)
if(UNIX AND NOT APPLE)
    include(CheckSymbolExists)
    check_symbol_exists(clock_gettime "time.h" _dav1d_have_clock_gettime)
    if(_dav1d_have_clock_gettime)
        _cfg10(HAVE_CLOCK_GETTIME "${_dav1d_have_clock_gettime}")
    endif()
    unset(_dav1d_have_clock_gettime CACHE)
endif()

# --- headers.
include(CheckIncludeFile)
check_include_file("sys/types.h" _dav1d_have_sys_types_h)
check_include_file("unistd.h"    _dav1d_have_unistd_h)
check_include_file("io.h"        _dav1d_have_io_h)
_cfg10(HAVE_SYS_TYPES_H "${_dav1d_have_sys_types_h}")
_cfg10(HAVE_UNISTD_H    "${_dav1d_have_unistd_h}")
_cfg10(HAVE_IO_H        "${_dav1d_have_io_h}")
unset(_dav1d_have_sys_types_h CACHE)
unset(_dav1d_have_unistd_h CACHE)
unset(_dav1d_have_io_h CACHE)

# --- pthread extensions.  Upstream probes these at *library* level, not at
#     header level, so the "have" values below are conservative: a 0 only
#     means "the optional feature is not used".
_cfg10(HAVE_PTHREAD_NP_H FALSE)
_cfg10(HAVE_PTHREAD_GETAFFINITY_NP FALSE)
_cfg10(HAVE_PTHREAD_SETAFFINITY_NP FALSE)
_cfg10(HAVE_PTHREAD_SETNAME_NP FALSE)
_cfg10(HAVE_PTHREAD_SET_NAME_NP FALSE)

# --- ELF auxiliary vector (Linux only).
_cfg10(HAVE_GETAUXVAL FALSE)
_cfg10(HAVE_ELF_AUX_INFO FALSE)

# --- C11 `_Generic`.
_cfg10(HAVE_C11_GENERIC FALSE)

# --- endianness.
_cfg10(ENDIANNESS_BIG FALSE)

# --- architecture.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    _cfg10(ARCH_AARCH64 TRUE)
    _cfg10(ARCH_ARM FALSE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|ARM)")
    _cfg10(ARCH_AARCH64 FALSE)
    _cfg10(ARCH_ARM TRUE)
else()
    _cfg10(ARCH_AARCH64 FALSE)
    _cfg10(ARCH_ARM FALSE)
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    _cfg10(ARCH_X86   TRUE)
    _cfg10(ARCH_X86_64 TRUE)
    _cfg10(ARCH_X86_32 FALSE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(i[3-6]86|x86)$")
    _cfg10(ARCH_X86   TRUE)
    _cfg10(ARCH_X86_64 FALSE)
    _cfg10(ARCH_X86_32 TRUE)
else()
    _cfg10(ARCH_X86   FALSE)
    _cfg10(ARCH_X86_64 FALSE)
    _cfg10(ARCH_X86_32 FALSE)
endif()
_cfg10(ARCH_PPC64LE FALSE)
_cfg10(ARCH_RISCV FALSE)
_cfg10(ARCH_RV32 FALSE)
_cfg10(ARCH_RV64 FALSE)
_cfg10(ARCH_LOONGARCH FALSE)
_cfg10(ARCH_LOONGARCH32 FALSE)
_cfg10(ARCH_LOONGARCH64 FALSE)

# --- AArch64 assembly extensions (unused with HAVE_ASM=0, but upstream
#     references them from src/cpu.h).
_cfg10(HAVE_AS_FUNC FALSE)
_cfg10(HAVE_AS_ARCH_DIRECTIVE FALSE)
_cfg10(HAVE_DOTPROD FALSE)
_cfg10(HAVE_I8MM FALSE)
_cfg10(HAVE_SVE FALSE)
_cfg10(HAVE_SVE2 FALSE)

# --- symbol prefix (32-bit Windows x86 only, upstream).
_cfg10(PREFIX FALSE)

if(WIN32)
    # Upstream's Windows branch of meson.build sets these; they matter for
    # the win32 thread shim and for fseeko/ftello.
    string(APPEND _cfg "#define _WIN32_WINNT 0x0601\n")
    string(APPEND _cfg "#define UNICODE 1\n")
    string(APPEND _cfg "#define _UNICODE 1\n")
    string(APPEND _cfg "#define __USE_MINGW_ANSI_STDIO 1\n")
    string(APPEND _cfg "#define _CRT_DECLARE_NONSTDC_NAMES 1\n")
    string(APPEND _cfg "#define _FILE_OFFSET_BITS 64\n")
endif()

file(WRITE "${_dav1d_gen}/config.h"
     "/* Generated by src/third_party/dav1d/CMakeLists.txt — do not edit. */\n"
     "#ifndef DAV1D_VENDORED_CONFIG_H\n"
     "#define DAV1D_VENDORED_CONFIG_H\n"
     "${_cfg}"
     "#endif /* DAV1D_VENDORED_CONFIG_H */\n")

# vcs_version.h — upstream generates it from `git describe`; the vendored
# build has no git checkout at build time, so the pinned release is used.
file(WRITE "${_dav1d_gen}/vcs_version.h"
     "/* auto-generated, do not edit */\n"
     "#define DAV1D_VERSION \"1.5.1\"\n")

# -----------------------------------------------------------------------------
# Source list.
# -----------------------------------------------------------------------------
set(_dav1d_common_sources
    src/cdf.c
    src/cpu.c
    src/ctx.c
    src/data.c
    src/decode.c
    src/dequant_tables.c
    src/getbits.c
    src/intra_edge.c
    src/itx_1d.c
    src/lf_mask.c
    src/lib.c
    src/log.c
    src/mem.c
    src/msac.c
    src/obu.c
    src/pal.c
    src/picture.c
    src/qm.c
    src/ref.c
    src/refmvs.c
    src/scan.c
    src/tables.c
    src/thread_task.c
    src/warpmv.c
    src/wedge.c
)

# The templated DSP sources: one instantiation per bitdepth.  Upstream
# compiles these for both 8 and 16 bpc; NimRTC only needs 8 bpc.
set(_dav1d_tmpl_sources
    src/cdef_apply_tmpl.c
    src/cdef_tmpl.c
    src/fg_apply_tmpl.c
    src/filmgrain_tmpl.c
    src/ipred_prepare_tmpl.c
    src/ipred_tmpl.c
    src/itx_tmpl.c
    src/lf_apply_tmpl.c
    src/loopfilter_tmpl.c
    src/looprestoration_tmpl.c
    src/lr_apply_tmpl.c
    src/mc_tmpl.c
    src/recon_tmpl.c
)

if(WIN32)
    # dav1d's Win32 pthread shim: the upstream `thread.h` maps the pthread
    # API onto Win32 primitives, and `src/win32/thread.c` supplies the
    # few pieces the header cannot inline.
    list(APPEND _dav1d_common_sources src/win32/thread.c)
endif()

list(TRANSFORM _dav1d_common_sources PREPEND "${_dav1d_root}/")
list(TRANSFORM _dav1d_tmpl_sources   PREPEND "${_dav1d_root}/")

# -----------------------------------------------------------------------------
# The 8-bit instantiation of the templated DSP sources.  It has to be a
# separate OBJECT library so that `-DBITDEPTH=8` does not leak into the
# non-templated sources (which must not see BITDEPTH at all).
# -----------------------------------------------------------------------------
add_library(nimrtc_vendor_dav1d_bd8 OBJECT ${_dav1d_tmpl_sources})

# -----------------------------------------------------------------------------
# The library proper.
# -----------------------------------------------------------------------------
add_library(nimrtc_vendor_dav1d STATIC
    ${_dav1d_common_sources}
    $<TARGET_OBJECTS:nimrtc_vendor_dav1d_bd8>
)
add_library(nimrtc::vendor::dav1d ALIAS nimrtc_vendor_dav1d)

# SYSTEM: upstream headers must not be subject to NimRTC's own -Werror
# warning set (-Wsign-conversion, -Wconversion, -Wshadow, ...).  Without
# SYSTEM, GCC/Clang apply those flags to every upstream header include and
# turn upstream warnings into NimRTC build failures.
target_include_directories(nimrtc_vendor_dav1d SYSTEM PUBLIC
    "${_dav1d_root}/include"
    "${_dav1d_root}/include/dav1d"
)
target_include_directories(nimrtc_vendor_dav1d SYSTEM PRIVATE
    "${_dav1d_root}"
    "${_dav1d_gen}"
)

target_include_directories(nimrtc_vendor_dav1d_bd8 SYSTEM PRIVATE
    "${_dav1d_root}"
    "${_dav1d_root}/include"
    "${_dav1d_root}/include/dav1d"
    "${_dav1d_gen}"
)

# MSVC has no <stdatomic.h> in the C dialect used by dav1d; upstream ships
# a replacement in include/compat/msvc.  It is activated by the
# `#if !defined(__cplusplus) && defined(_MSC_VER)` guard inside the header,
# so adding the directory to the include path is enough.
if(MSVC)
    target_include_directories(nimrtc_vendor_dav1d SYSTEM PRIVATE
        "${_dav1d_root}/include/compat/msvc")
    target_include_directories(nimrtc_vendor_dav1d_bd8 SYSTEM PRIVATE
        "${_dav1d_root}/include/compat/msvc")
endif()

# dav1d is C99; the non-templated sources must NOT see BITDEPTH.
target_compile_definitions(nimrtc_vendor_dav1d PRIVATE
    _GNU_SOURCE=1
)
target_compile_definitions(nimrtc_vendor_dav1d_bd8 PRIVATE
    BITDEPTH=8
    _GNU_SOURCE=1
)

# Upstream compiles with warning_level=2; NimRTC's own set is stricter.
# The vendor sources must not fail the NimRTC build over upstream warnings.
if(MSVC)
    target_compile_options(nimrtc_vendor_dav1d PRIVATE /w)
    target_compile_options(nimrtc_vendor_dav1d_bd8 PRIVATE /w)
else()
    target_compile_options(nimrtc_vendor_dav1d PRIVATE -w)
    target_compile_options(nimrtc_vendor_dav1d_bd8 PRIVATE -w)
endif()

# The MSVC runtime must match the rest of the tree: upstream is built as
# part of the same process as every NimRTC target, so a /MD vs /MDd split
# produces two CRT heaps and an LNK4098 at link time.
if(MSVC AND CMAKE_MSVC_RUNTIME_LIBRARY)
    set_target_properties(nimrtc_vendor_dav1d PROPERTIES
        MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
    set_target_properties(nimrtc_vendor_dav1d_bd8 PROPERTIES
        MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
endif()

# Threads: POSIX pthreads on Unix, the Win32 shim on Windows.
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(nimrtc_vendor_dav1d PUBLIC Threads::Threads)
    if(NOT APPLE)
        target_link_libraries(nimrtc_vendor_dav1d PUBLIC ${CMAKE_DL_LIBS})
    endif()
endif()

# The public API surface of the wrapper.
target_compile_definitions(nimrtc_vendor_dav1d INTERFACE NIMRTC_USE_DAV1D=1)
