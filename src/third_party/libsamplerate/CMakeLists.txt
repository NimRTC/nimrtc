# =============================================================================
# libsamplerate — the sample-rate conversion library (BSD-2-Clause).
#
# Vendored from: https://github.com/libsndfile/libsamplerate (master) under src/.
#
# WHY A HAND-WRITTEN WRAPPER
# --------------------------
# Upstream *does* ship a CMake build (its root CMakeLists.txt plus
# src/CMakeLists.txt), but it is a stand-alone project build: it calls
# `project()`, it defines its own `LIBSAMPLERATE_*` options, it pulls in
# pkg-config / sndfile / fftw3 discovery for its examples and tests, and
# it installs its own package config + CPack data.  Entering it with
# `add_subdirectory()` would:
#
#   - rename the project (the `PROJECT_NAME` it stamps into config.h
#     would become whatever the NimRTC tree happens to call itself),
#   - pollute the global option namespace with `LIBSAMPLERATE_*` cache
#     entries,
#   - and pull upstream's install() / CPack rules into the NimRTC bundle.
#
# So this wrapper compiles the upstream sources *directly* with CMake and
# exposes the same target name every other NimRTC vendor uses:
#
#     nimrtc_vendor_libsamplerate   (alias nimrtc::vendor::libsamplerate)
#
# WHAT IS COMPILED
# ----------------
# The upstream tree is four portable C translation units plus their
# coefficient headers:
#
#   src/samplerate.c   the public API (src_new / src_process / src_delete)
#   src/src_linear.c   the linear-interpolation converter
#   src/src_sinc.c     the windowed-sinc converters (best / medium / fastest)
#   src/src_zoh.c      the zero-order-hold converter
#
# There is NO platform-specific code in libsamplerate at all: it is pure
# C99 over <math.h>, so the SAME source builds everywhere NimRTC ships.
# That is exactly the property RD-10 asks for — one implementation, every
# platform.
#
# THE GENERATED config.h
# ----------------------
# Upstream's sources all `#include "config.h"`, which upstream's own
# CMake generates from config.h.cmake.  Entering upstream's build would
# produce it; compiling the sources directly means this wrapper has to
# produce it, so the handful of macros the four translation units
# actually consume are written out here.
#
# The macros that matter:
#
#   PACKAGE / VERSION        — stamped into src_get_version().
#   CPU_CLIPS_POSITIVE/NEG   — whether the CPU's float->int conversion
#                              saturates.  0 = "it does not", so upstream
#                              keeps its explicit clipping branches.
#   CPU_IS_BIG_ENDIAN        — unused by the compiled sources, kept so
#                              the header matches upstream's shape.
#   HAVE_STDBOOL_H           — upstream's common.h includes <stdbool.h>
#                              when it is available.
#   HAVE_UNISTD_H            — <unistd.h> is POSIX-only.
#   HAVE_IMMINTRIN_H         — upstream's SSE2 lrint path includes
#                              <immintrin.h> when it is available and
#                              <emmintrin.h> otherwise.
#   ENABLE_SINC_*_CONVERTER  — which of the three sinc converters are
#                              compiled in.
#
# THE SINC CONVERTERS
# -------------------
# Upstream defaults all three sinc converters ON.  The "best" converter's
# coefficient table (`high_qual_coeffs.h`) is a ~9 MB array of doubles and
# costs a *lot* of compile time and memory for a converter a remote
# desktop never selects, so this wrapper builds the FAST and MEDIUM
# converters and leaves BEST out.  `ResamplerQuality::kBest` therefore
# falls back to the MEDIUM converter inside libsamplerate; the NimRTC
# wrapper reports the *effective* converter through `Resampler::backend()`
# and `Resampler::stats()`.
#
# THE PLATFORM MATRIX (policy K8b)
# --------------------------------
#   Windows (MSVC)  — x86-64.  Pure C99 + <math.h>; the SSE2 lrint
#                     fast path is selected automatically on x86-64.
#   macOS           — arm64 / x86-64.  Same sources, no platform code.
#   Linux x86-64    — same sources; links libm.
#   Linux aarch64   — same sources; links libm.  No NEON fast path
#                     exists upstream (the library is scalar C), so the
#                     output is bit-comparable across the four platforms.
#
# LICENCE
# -------
# BSD-2-Clause (see src/third_party/libsamplerate/src/COPYING).  The
# licence clears the plan's RD-2 licence filter.
# =============================================================================

set(_lsr_dir "${CMAKE_CURRENT_SOURCE_DIR}/src")

# The vendored switch is the "use the vendored source build" contract.
# There is NO system fallback: L5 forbids bypassing the vendored
# dependency with a system samplerate library.  Switching the option off
# means the audio-device module compiles its pure-C++ linear fallback
# instead, and the module reports that honestly through
# `resampler_backend()`.
if(NOT NIMRTC_VENDORED_LIBSAMPLERATE)
    add_library(nimrtc_vendor_libsamplerate INTERFACE)
    add_library(nimrtc::vendor::libsamplerate ALIAS nimrtc_vendor_libsamplerate)
    target_compile_definitions(nimrtc_vendor_libsamplerate INTERFACE NIMRTC_HAS_LIBSAMPLERATE=0)
    return()
endif()

# The submodule is not populated: declare an empty stub so the configure
# step succeeds.  A consumer that links it fails at link time with a
# clear "unresolved external" rather than a CMake error.
if(NOT EXISTS "${_lsr_dir}/src/samplerate.c")
    add_library(nimrtc_vendor_libsamplerate INTERFACE)
    add_library(nimrtc::vendor::libsamplerate ALIAS nimrtc_vendor_libsamplerate)
    target_compile_definitions(nimrtc_vendor_libsamplerate INTERFACE NIMRTC_HAS_LIBSAMPLERATE=0)
    message(STATUS "  libsamplerate     : vendored submodule not populated — "
                   "the audio-device resampler falls back to linear interpolation")
    return()
endif()

# -----------------------------------------------------------------------------
# The generated config.h (see the header comment above).
# -----------------------------------------------------------------------------
set(_lsr_config_dir "${CMAKE_CURRENT_BINARY_DIR}/include")
file(MAKE_DIRECTORY "${_lsr_config_dir}")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" OR
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|ARM)$")
    set(_lsr_big_endian 0)
else()
    include(TestBigEndian)
    test_big_endian(_lsr_big_endian)
endif()

# <immintrin.h> is available on MSVC and on every GCC / Clang that ships
# x86 intrinsics headers.  When it is missing upstream falls back to
# <emmintrin.h>, which carries the same SSE2 lrint intrinsics.
include(CheckIncludeFile)
check_include_file("immintrin.h" _lsr_have_immintrin)

if(WIN32)
    set(_lsr_have_unistd 0)
else()
    check_include_file("unistd.h" _lsr_have_unistd)
endif()

file(WRITE "${_lsr_config_dir}/config.h"
"/* config.h — generated by NimRTC's libsamplerate vendor wrapper.
** Mirrors upstream's config.h.cmake for the handful of macros the four
** upstream translation units actually consume.
*/

#ifndef NIMRTC_LIBSAMPLERATE_CONFIG_H
#define NIMRTC_LIBSAMPLERATE_CONFIG_H

/* Name of package */
#define PACKAGE \"libsamplerate\"

/* Version number of package */
#define VERSION \"0.2.2\"

/* Target processor clips on negative float to int conversion. */
#define CPU_CLIPS_NEGATIVE 0

/* Target processor clips on positive float to int conversion. */
#define CPU_CLIPS_POSITIVE 0

/* Target processor is big endian. */
#define CPU_IS_BIG_ENDIAN ${_lsr_big_endian}

/* Define to 1 if you have the <stdbool.h> header file. */
#define HAVE_STDBOOL_H 1

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H ${_lsr_have_unistd}

/* Define to 1 if you have the <immintrin.h> header file. */
#define HAVE_IMMINTRIN_H ${_lsr_have_immintrin}

/* define fast samplerate convertor */
#define ENABLE_SINC_FAST_CONVERTER 1

/* define balanced samplerate convertor */
#define ENABLE_SINC_MEDIUM_CONVERTER 1

#endif /* NIMRTC_LIBSAMPLERATE_CONFIG_H */
")

# -----------------------------------------------------------------------------
# The library.
# -----------------------------------------------------------------------------
add_library(nimrtc_vendor_libsamplerate STATIC
    "${_lsr_dir}/src/samplerate.c"
    "${_lsr_dir}/src/src_linear.c"
    "${_lsr_dir}/src/src_sinc.c"
    "${_lsr_dir}/src/src_zoh.c"
)
add_library(nimrtc::vendor::libsamplerate ALIAS nimrtc_vendor_libsamplerate)

# SYSTEM includes: upstream headers must not be subject to NimRTC's own
# -Werror warning set.
target_include_directories(nimrtc_vendor_libsamplerate SYSTEM
    PUBLIC
        "${_lsr_dir}/include"
    PRIVATE
        "${_lsr_dir}/src"
        "${_lsr_config_dir}"
)

target_compile_features(nimrtc_vendor_libsamplerate PRIVATE c_std_99)

# Upstream's sources guard their `#include "config.h"` with
# HAVE_CONFIG_H (upstream's own build defines it globally).  The
# wrapper has to define it too, otherwise the sources compile without
# the generated config and the PACKAGE / VERSION macros are missing.
target_compile_definitions(nimrtc_vendor_libsamplerate PRIVATE HAVE_CONFIG_H=1)

# Upstream's own sources are not warning-clean under NimRTC's flags, so
# they are compiled with the warnings switched off (the vendor targets
# are excluded from nimrtc_apply_options() anyway — see
# cmake/NimRTCOptions.cmake).
if(MSVC)
    target_compile_options(nimrtc_vendor_libsamplerate PRIVATE /w)
else()
    target_compile_options(nimrtc_vendor_libsamplerate PRIVATE -w)
endif()

# The MSVC runtime must match the rest of the tree: upstream is built as
# part of the same process as every NimRTC target, so a /MD vs /MDd split
# produces two CRT heaps and an LNK4098 at link time.
if(MSVC AND CMAKE_MSVC_RUNTIME_LIBRARY)
    set_target_properties(nimrtc_vendor_libsamplerate PROPERTIES
        MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
endif()

# libm: the sinc / linear converters use lrint() and friends.
if(NOT WIN32)
    target_link_libraries(nimrtc_vendor_libsamplerate PUBLIC m)
endif()

# The public API surface of the wrapper.
target_compile_definitions(nimrtc_vendor_libsamplerate INTERFACE NIMRTC_HAS_LIBSAMPLERATE=1)
