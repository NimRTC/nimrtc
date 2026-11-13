# NimRTHwPlugins.cmake
# ----------------------------------------------------------------------------
# Locate HW codec SDKs (NVENC, AMF, QSV/libvpl, DXVA, VA-API) and produce
# imported targets + compile definitions for downstream modules.
#
# Each backend has a per-host-platform guard:
#   - NVENC  : Win + Linux
#   - AMF    : Win only  (AMD's Linux AMF runtime is closed)
#   - QSV    : Win + Linux
#   - DXVA   : Win only
#   - VAAPI  : Linux only
#
# Detection is *silent by default* — a missing SDK does NOT raise a fatal
# error.  Module CMakeLists.txt should call `nimrtc_link_<backend>(target)`
# which becomes a no-op if the SDK is not present, allowing the build to
# still succeed (HW path disabled, software fallback active).
#
# Usage from a module CMakeLists.txt:
#
#   include(NimRTHwPlugins)
#   nimrtc_link_nvenc(nimrtc_h264)
#   nimrtc_link_vaapi(nimrtc_h264)
#   ...
#
# Variables exported to consumer scope (for compile-time #if checks):
#   NIMRTC_HW_HAVE_NVENC   /  NIMRTC_HW_HAVE_AMF   /  NIMRTC_HW_HAVE_QSV
#   NIMRTC_HW_HAVE_DXVA    /  NIMRTC_HW_HAVE_VAAPI /  NIMRTC_HW_HAVE_NVDEC
# ----------------------------------------------------------------------------

include(FindPackageHandleStandardArgs)

# -----------------------------------------------------------------------------
# Locator helpers — one per backend.  All honor the user-facing cache option
# NIMRTC_PLUGINS_<NAME>:
#   - If OFF: locator returns immediately with no result.
#   - If ON:  runs find_package-equivalent logic.
# -----------------------------------------------------------------------------

# --- NVENC --------------------------------------------------------------------
function(_nimrtc_locate_nvenc)
    if(NOT NIMRTC_PLUGINS_NVENC)
        return()
    endif()

    find_package(NVENC QUIET)
    if(NVENC_FOUND)
        set(NIMRTC_HW_HAVE_NVENC TRUE
            CACHE INTERNAL "NVENC SDK located and usable" FORCE)
        message(STATUS "  NVENC SDK        : found at ${NVENC_INCLUDE_DIRS}")
    else()
        message(STATUS "  NVENC SDK        : not found (NIMRTC_PLUGINS_NVENC=ON; "
                        "set NVENC_INCLUDE_DIR / NVENC_LIBRARY to override)")
    endif()
endfunction()

# --- AMF ----------------------------------------------------------------------
function(_nimrtc_locate_amf)
    if(WIN32 AND NIMRTC_PLUGINS_AMF)
        find_package(AMF QUIET)
        if(AMF_FOUND)
            set(NIMRTC_HW_HAVE_AMF TRUE
                CACHE INTERNAL "AMF SDK located and usable" FORCE)
            message(STATUS "  AMD AMF SDK      : found at ${AMF_INCLUDE_DIRS}")
        else()
            message(STATUS "  AMD AMF SDK      : not found (set AMF_INCLUDE_DIR to override)")
        endif()
    endif()
endfunction()

# --- QSV / libvpl -------------------------------------------------------------
function(_nimrtc_locate_qsv)
    if(NOT NIMRTC_PLUGINS_QSV)
        return()
    endif()

    find_package(LibVPL QUIET)
    if(LIBVPL_FOUND)
        set(NIMRTC_HW_HAVE_QSV TRUE
            CACHE INTERNAL "Intel QSV (libvpl) located" FORCE)
        message(STATUS "  Intel QSV (libvpl): found at ${LIBVPL_INCLUDE_DIRS}")
    else()
        message(STATUS "  Intel QSV (libvpl): not found (set LIBVPL_INCLUDE_DIR to override)")
    endif()
endfunction()

# --- DXVA ---------------------------------------------------------------------
# DXVA is just d3d11.h + d3d11.lib from the Windows SDK; detection is a
# trivial find_path.  We only consider it if we're on Windows AND the user
# flipped NIMRTC_PLUGINS_DXVA on.
function(_nimrtc_locate_dxva)
    if(WIN32 AND NIMRTC_PLUGINS_DXVA)
        # d3d11.h lives in the Windows 8+ SDK; always present on Windows
        # builds that have C++ compilers installed.  We use a single test
        # compile rather than find_path to capture the full include chain.
        include(CheckIncludeFileCXX)
        check_include_file_cxx("d3d11.h" NIMRTC_HW_HAVE_DXVA_H
            LANGUAGE CXX)
        if(NIMRTC_HW_HAVE_DXVA_H)
            set(NIMRTC_HW_HAVE_DXVA TRUE
                CACHE INTERNAL "DXVA headers available (d3d11.h)" FORCE)
            # We don't need to add a library — d3d11.lib is part of the
            # default MSVC link line for desktop apps.  Consumers still
            # need to call `d3d11.lib` explicitly when compiling with
            # /NODEFAULTLIB.
            message(STATUS "  Microsoft DXVA    : d3d11.h found")
        else()
            message(WARNING "  Microsoft DXVA    : d3d11.h NOT found; "
                             "install the Windows 10 SDK")
        endif()
    endif()
endfunction()

# --- VA-API --------------------------------------------------------------------
function(_nimrtc_locate_vaapi)
    if(UNIX AND NIMRTC_PLUGINS_VAAPI)
        find_package(LibVA QUIET)
        if(LIBVA_FOUND)
            set(NIMRTC_HW_HAVE_VAAPI TRUE
                CACHE INTERNAL "libva located and usable" FORCE)
            message(STATUS "  VA-API (libva)   : found at ${LIBVA_INCLUDE_DIRS}")
        else()
            message(STATUS "  VA-API (libva)   : not found (install libva-dev "
                            "or set LIBVA_INCLUDE_DIR to override)")
        endif()
    endif()
endfunction()

# --- NVDEC --------------------------------------------------------------------
# NVDEC is a *decoder* and needs the CUVID / CUDA headers (nvcuvid.h and
# friends).  Those ship in the same MIT-licensed header pack as the NVENC
# headers, and NimRTC vendors them under
# src/third_party/nv_codec_headers — so a NVDEC-only build needs no
# NVIDIA SDK install at all.
#
# The vendored pack is preferred over any system SDK install: it pins the
# struct layout to one known ABI (see the README next to the headers).
function(_nimrtc_locate_nvdec)
    if(NOT NIMRTC_PLUGINS_NVDEC)
        return()
    endif()

    set(_nvdec_vendor_include
        "${CMAKE_SOURCE_DIR}/src/third_party/nv_codec_headers/include/ffnvcodec")

    # The vendored pack is only usable when vendoring is enabled — the
    # NIMRTC_VENDORED=OFF escape hatch means "do not use the in-tree
    # copies", and the headers are an in-tree copy.
    if(NIMRTC_VENDORED AND EXISTS "${_nvdec_vendor_include}/dynlink_nvcuvid.h")
        set(NIMRTC_HW_HAVE_NVDEC TRUE
            CACHE INTERNAL "NVDEC headers available (vendored nv-codec-headers)" FORCE)
        set(NIMRTC_NVDEC_INCLUDE_DIR "${_nvdec_vendor_include}"
            CACHE INTERNAL "NVDEC header include dir" FORCE)
        message(STATUS "  NVIDIA NVDEC     : vendored nv-codec-headers")
        return()
    endif()

    # Fall back to a system SDK install (an NVIDIA Video Codec SDK or a
    # CUDA toolkit).  The CUVID headers are not in every SDK layout, so
    # probe for them explicitly.
    if(NOT NVENC_FOUND)
        find_package(NVENC QUIET)
    endif()

    if(NVENC_FOUND AND EXISTS "${NVENC_INCLUDE_DIR}/nvcuvid.h")
        set(NIMRTC_HW_HAVE_NVDEC TRUE
            CACHE INTERNAL "NVDEC runtime available (nvcuvid.h)" FORCE)
        set(NIMRTC_NVDEC_INCLUDE_DIR "${NVENC_INCLUDE_DIR}"
            CACHE INTERNAL "NVDEC header include dir" FORCE)
        message(STATUS "  NVIDIA NVDEC     : using system SDK at ${NVENC_INCLUDE_DIR}")
    else()
        message(STATUS "  NVIDIA NVDEC     : headers not found (the vendored "
                        "nv-codec-headers tree is missing and no system SDK "
                        "with nvcuvid.h was located)")
    endif()
endfunction()
# -----------------------------------------------------------------------------
# Top-level: run all locators.
# -----------------------------------------------------------------------------
function(nimrtc_locate_hw_plugins)
    _nimrtc_locate_nvenc()
    _nimrtc_locate_amf()
    _nimrtc_locate_qsv()
    _nimrtc_locate_dxva()
    _nimrtc_locate_vaapi()
    _nimrtc_locate_nvdec()
endfunction()

# -----------------------------------------------------------------------------
# Per-backend link helpers — module CMakeLists call these; they become no-ops
# if the corresponding SDK is not present.
# -----------------------------------------------------------------------------
function(nimrtc_link_nvenc target)
    if(NIMRTC_HW_HAVE_NVENC)
        target_link_libraries(${target} PRIVATE NimRTC::nvenc)
        target_compile_definitions(${target} PRIVATE NIMRTC_PLUGINS_NVENC_ON=1)
        target_include_directories(${target} PRIVATE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/modules/nvcodec/src")
        target_sources(${target} PRIVATE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/modules/nvcodec/src/nvenc_encoder.cpp")
    endif()
endfunction()

function(nimrtc_link_amf target)
    if(NIMRTC_HW_HAVE_AMF)
        target_link_libraries(${target} PRIVATE NimRTC::amf)
        target_compile_definitions(${target} PRIVATE NIMRTC_PLUGINS_AMF_ON=1)
        target_include_directories(${target} PRIVATE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/modules/h264/src")
        target_sources(${target} PRIVATE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/modules/h264/src/amf_encoder.cpp")
    endif()
endfunction()

function(nimrtc_link_qsv target)
    if(NIMRTC_HW_HAVE_QSV)
        target_link_libraries(${target} PRIVATE NimRTC::vpl)
        target_compile_definitions(${target} PRIVATE NIMRTC_PLUGINS_QSV_ON=1)
        target_include_directories(${target} PRIVATE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/modules/h264/src")
        target_sources(${target} PRIVATE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/modules/h264/src/qsv_encoder.cpp")
    endif()
endfunction()

function(nimrtc_link_dxva target)
    if(NIMRTC_HW_HAVE_DXVA)
        target_compile_definitions(${target} PRIVATE NIMRTC_PLUGINS_DXVA_ON=1)
        target_include_directories(${target} PRIVATE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/modules/h264/src")
        target_sources(${target} PRIVATE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/modules/h264/src/dxva_decoder.cpp")
        # d3d11.lib is in the default MSVC desktop link list, but we add it
        # explicitly so /NODEFAULTLIB builds still link.
        if(MSVC)
            target_link_libraries(${target} PRIVATE d3d11)
        endif()
    endif()
endfunction()

function(nimrtc_link_vaapi target)
    if(NIMRTC_HW_HAVE_VAAPI)
        target_link_libraries(${target} PRIVATE NimRTC::va)
        target_compile_definitions(${target} PRIVATE NIMRTC_PLUGINS_VAAPI_ON=1)
        target_include_directories(${target} PRIVATE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/modules/h264/src")
        target_sources(${target} PRIVATE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/modules/h264/src/vaapi_encoder.cpp")
    endif()
endfunction()

function(nimrtc_link_nvdec target)
    if(NIMRTC_HW_HAVE_NVDEC)
        target_compile_definitions(${target} PRIVATE NIMRTC_PLUGINS_NVDEC_ON=1)
        target_include_directories(${target} PRIVATE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/modules/nvcodec/src")
        # The CUVID / CUDA headers.  They are self-contained (the vendored
        # pack ships the dynamic-loader headers too), so no NVIDIA SDK
        # library is linked — nvcuvid.dll / nvcuda.dll are loaded at
        # runtime with LoadLibrary / dlopen.
        target_include_directories(${target} PRIVATE
            "${NIMRTC_NVDEC_INCLUDE_DIR}")
        target_sources(${target} PRIVATE
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/modules/nvcodec/src/nvdec_decoder.cpp")
    endif()
endfunction()

function(nimrtc_link_openh264 target)
    if(NIMRTC_PLUGINS_OPENH264)
        # The real OpenH264 binding: the vendored source build of Cisco's
        # codec (BSD-2-Clause).  The link helper is what decides whether
        # the real codec or the "unavailable" path gets compiled in —
        # see nimrtc_link_vendor_openh264() in cmake/NimRTCVendored.cmake.
        nimrtc_link_vendor_openh264(${target})
        # The plugin source is compiled ONLY when the vendored build is
        # really there: the vendored switch off, or the submodule not
        # populated, means there is no codec to link against — the
        # registry then falls through to the software stub.
        if(NIMRTC_VENDORED_OPENH264 AND TARGET nimrtc_vendor_openh264 AND
           EXISTS "${CMAKE_SOURCE_DIR}/src/third_party/openh264/src/codec/api/wels/codec_api.h")
            target_compile_definitions(${target} PRIVATE NIMRTC_PLUGINS_OPENH264_ON=1)
            target_include_directories(${target} PRIVATE
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/modules/h264/src")
            target_sources(${target} PRIVATE
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/modules/h264/src/openh264_encoder.cpp")
        endif()
    endif()
endfunction()

function(nimrtc_link_dav1d target)
    if(NIMRTC_PLUGINS_DAV1D)
        # The real dav1d binding: the vendored source build of VideoLAN's
        # AV1 decoder (BSD-2-Clause).  dav1d is decode-only, so only the
        # decoder source is added.
        nimrtc_link_vendor_dav1d(${target})
        if(NIMRTC_VENDORED_DAV1D AND TARGET nimrtc_vendor_dav1d AND
           EXISTS "${CMAKE_SOURCE_DIR}/src/third_party/dav1d/src/include/dav1d/dav1d.h")
            target_compile_definitions(${target} PRIVATE NIMRTC_PLUGINS_DAV1D_ON=1)
            target_include_directories(${target} PRIVATE
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/modules/av1/src")
            target_sources(${target} PRIVATE
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/modules/av1/src/dav1d_decoder.cpp")
        endif()
    endif()
endfunction()
