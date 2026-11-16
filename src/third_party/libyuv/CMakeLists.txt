# third_party/libyuv/CMakeLists.txt
# =============================================================================
# libyuv — YUV / RGB scale + convert library (BSD-3-Clause).
#
# Vendored from: https://github.com/lemenkov/libyuv (a maintained fork of
# the original chromium.googlesource.com/external/libyuv) under src/.
#
# WHY libyuv (remote-desktop plan C1 / RD-10)
# --------------------------------------------
# The remote-desktop plan names libyuv as the *default* cross-platform
# scale / colour library: one implementation covers Windows / macOS /
# Linux / Android / iOS, with SSSE3 + AVX2 on x86-64 and NEON / SVE2 / SME
# on arm64.  It is BSD-3-Clause, so it clears the RD-2 licence filter.
#
# The plan's Track C needs exactly what libyuv ships:
#   - I420Scale() with the four filter modes the plan names
#     (point / bilinear / box) — kFilterBox for the *display* path,
#     kFilterBilinear for the *encode* path.
#   - ARGBToI420() / I420ToARGB() — the capture → encode colour conversion
#     in SIMD, instead of the module's hand-written scalar reference.
#
#     (Note the naming: libyuv spells its packed formats by *word* order,
#     so its "ARGB" is the word 0xAARRGGBB, which on every
#     little-endian target NimRTC supports is laid out in memory as
#     B, G, R, A — exactly NimRTC's `PixelFormat::kBGRA`.  libyuv's own
#     "BGRA" is the word 0xBBGGRRAA, i.e. memory order A, R, G, B.)
#
# WHAT THIS WRAPPER DOES
# ----------------------
# Upstream ships a CMakeLists.txt of its own, so (unlike openh264 / dav1d)
# we do NOT have to compile the sources by hand.  We add_subdirectory() it
# with the upstream switches that keep the build to the library only:
#
#   UNIT_TEST=OFF              — upstream's gtest suite must not build
#                                 (and must not pull in a second gtest).
#   LIBYUV_DISABLE_JPEG=ON     — the MJPEG decoder is dead weight for us
#                                 and would add a libjpeg dependency.
#
# Upstream also creates the `yuv_shared` SHARED library, three command
# line tools (cpuid / yuvconvert / yuvconstants) and a set of CPack
# packaging rules.  The whole upstream directory is added with
# EXCLUDE_FROM_ALL, so only the target NimRTC actually links (the static
# `yuv` archive) is ever compiled — the shared archive and the three
# tools never build.  Upstream's own install() rules are suppressed for
# the same reason (see below).
#
# The target we expose is the standard NimRTC vendor shape:
#
#     nimrtc_vendor_libyuv   (alias nimrtc::vendor::libyuv)
#
# `nimrtc_vendor_libyuv` is an INTERFACE library that links upstream's
# static `yuv` archive and propagates the upstream include directory as
# a SYSTEM include (upstream headers must not be subject to NimRTC's own
# -Werror warning set).  This mirrors the libopus wrapper exactly.
#
# CPACK
# -----
# Upstream's CMakeLists.txt ends with `include(CM_linux_packages.cmake)`,
# which calls `include(CPack)` and sets CPACK_GENERATOR to "DEB;RPM".
# CPack is included from the upstream subdirectory scope, so the
# CPACK_* variables it sets stay local to that scope and cannot change
# the packaging configuration of the NimRTC project itself (NimRTC does
# not ship DEB/RPM packages).  The one global side effect we DO neutralise
# is upstream's own install() rules — see "CMAKE_SKIP_INSTALL_RULES" below.
#
# IMPORTANT: this assumes src/ is populated.  If src/ is empty (fresh
# clone without `git submodule update --init`), the wrapper declares an
# empty INTERFACE stub so the configure step still succeeds; the
# video_frame module then compiles its "unavailable" path and reports
# available() == false.
# =============================================================================

set(_src_dir "${CMAKE_CURRENT_SOURCE_DIR}/src")

# The vendored switch is the "use the vendored source build" contract.
# There is NO system fallback (L5 forbids linking a system codec), so
# switching it off means libyuv is simply not available and the
# video_frame scaler reports "unavailable".
if(NOT NIMRTC_VENDORED_LIBYUV)
    add_library(nimrtc_vendor_libyuv INTERFACE)
    add_library(nimrtc::vendor::libyuv ALIAS nimrtc_vendor_libyuv)
    target_compile_definitions(nimrtc_vendor_libyuv INTERFACE NIMRTC_USE_LIBYUV=0)
    return()
endif()

if(NOT EXISTS "${_src_dir}/include/libyuv/scale.h")
    # The submodule is not populated.  Declare an empty stub target so the
    # configure step succeeds; a consumer that links it fails at link time
    # with a clear "unresolved external" rather than a CMake error.
    add_library(nimrtc_vendor_libyuv INTERFACE)
    add_library(nimrtc::vendor::libyuv ALIAS nimrtc_vendor_libyuv)
    target_compile_definitions(nimrtc_vendor_libyuv INTERFACE NIMRTC_USE_LIBYUV=0)
    return()
endif()

# ---------------------------------------------------------------------------
# Upstream build switches.
#
# Upstream always creates BOTH a static archive (`yuv`) and a shared one
# (`yuv_shared`); it does not consult BUILD_SHARED_LIBS.  The shared target
# is never built here: the whole upstream directory is added with
# EXCLUDE_FROM_ALL, so only the targets NimRTC actually links (`yuv`)
# are compiled.  Upstream's own install() rules are suppressed for the
# same reason (see below).
#
# These are CACHE variables, so they are forced to the values we need
# rather than merely set: a stale cache entry from a previous configure
# must not silently re-enable upstream's unit tests.
# ---------------------------------------------------------------------------
set(UNIT_TEST          OFF CACHE BOOL "" FORCE)
set(LIBYUV_DISABLE_JPEG ON CACHE BOOL "" FORCE)

# Upstream's install() rules would otherwise add libyuv's headers and
# archives to NimRTC's install set (the wrapper target is INTERFACE, so
# the archive would be missing from the bundle anyway).  Suppress them for
# the duration of the upstream add_subdirectory() and restore afterwards.
set(_nimrtc_saved_skip_install "${CMAKE_SKIP_INSTALL_RULES}")
set(CMAKE_SKIP_INSTALL_RULES TRUE)
add_subdirectory(${_src_dir} _nimrtc_libyuv_build EXCLUDE_FROM_ALL)
set(CMAKE_SKIP_INSTALL_RULES "${_nimrtc_saved_skip_install}")
unset(_nimrtc_saved_skip_install)

if(NOT TARGET yuv)
    message(FATAL_ERROR
        "libyuv: upstream did not create the expected static target 'yuv'. "
        "The pinned submodule tree at ${_src_dir} looks incomplete.")
endif()

# ---------------------------------------------------------------------------
# The NimRTC vendor target.
#
# INTERFACE (not STATIC) so the upstream archive stays upstream's: the
# same shape the libopus wrapper uses.
# ---------------------------------------------------------------------------
add_library(nimrtc_vendor_libyuv INTERFACE)
add_library(nimrtc::vendor::libyuv ALIAS nimrtc_vendor_libyuv)

target_link_libraries(nimrtc_vendor_libyuv INTERFACE yuv)

# SYSTEM: upstream headers must not be subject to NimRTC's own -Werror
# warning set (-Wsign-conversion, -Wconversion, ...).  Without SYSTEM,
# GCC/Clang apply those flags to every upstream header include and turn
# upstream warnings into NimRTC build failures.
target_include_directories(nimrtc_vendor_libyuv SYSTEM INTERFACE
    $<BUILD_INTERFACE:${_src_dir}/include>)

# NIMRTC_USE_LIBYUV=1 tells the video_frame module that the real library
# is linked; the module compiles its "unavailable" path otherwise.
target_compile_definitions(nimrtc_vendor_libyuv INTERFACE
    NIMRTC_USE_LIBYUV=1)
