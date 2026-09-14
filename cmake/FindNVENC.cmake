# FindNVENC.cmake
# ----------------------------------------------------------------------------
# Locate NVIDIA Video Codec SDK (NVENC + NVDEC) — header-only SDK from
# https://developer.nvidia.com/nvidia-video-codec-sdk
#
# Defines:
#   NVENC_FOUND          — TRUE if SDK (header + library) detected
#   NVENC_INCLUDE_DIRS   — directory containing nvEncodeAPI.h / nvcuvid.h
#   NVENC_LIBRARIES      — full path to nvEncodeAPI.lib / libnvidia-encode.so
#   NVENC_DEFINITIONS    — compile definitions (none required by upstream)
#   NVENC_VERSION        — detected version string (best-effort from VERSION
#                          file inside the SDK; "unknown" if absent)
#   NimRTC::nvenc        — IMPORTED target for consumers to link against
#
# Cache variables (for user override):
#   NVENC_INCLUDE_DIR    — pre-set header directory
#   NVENC_LIBRARY        — pre-set library path
#
# Detection logic:
#   1. Honour user-supplied NVENC_INCLUDE_DIR / NVENC_LIBRARY first.
#   2. Probe the canonical SDK install path:
#        Windows : C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v*/include
#                  C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v*/lib/x64
#                  C:/Program Files/NVIDIA Video Codec SDK/*/Include (legacy)
#        Linux   : /usr/include /usr/lib/x86_64-linux-gnu /usr/local/cuda/include
#                  /opt/nvidia/video-sdk/*/include
#   3. Probe env: NVIDIA_VIDEO_CODEC_SDK_DIR, CUDA_PATH.
#   4. find_path / find_library as the last resort.
# ----------------------------------------------------------------------------

# Honour user-supplied paths first.
if(NOT NVENC_INCLUDE_DIR)
    set(_probe_paths)

    if(DEFINED ENV{NVIDIA_VIDEO_CODEC_SDK_DIR})
        list(APPEND _probe_paths
            "$ENV{NVIDIA_VIDEO_CODEC_SDK_DIR}/include"
            "$ENV{NVIDIA_VIDEO_CODEC_SDK_DIR}/Samples/NvCodec/NvEncoder/Include")
    endif()

    if(DEFINED ENV{CUDA_PATH})
        list(APPEND _probe_paths "$ENV{CUDA_PATH}/include")
    endif()

    if(DEFINED ENV{CUDA_HOME})
        list(APPEND _probe_paths "$ENV{CUDA_HOME}/include")
    endif()

    list(APPEND _probe_paths
        # Windows — legacy Video Codec SDK install (pre-CUDA 12)
        "C:/Program Files/NVIDIA Video Codec SDK"
        "C:/Program Files (x86)/NVIDIA Video Codec SDK"
        # SDK 11.1.5 (legacy ffnvcodec header pack — what driver 591.x
        # and many older Game Ready drivers were built against).
        # Probe both the ffnvcodec/ subdir layout and the SDK's canonical
        # Interface/ layout, since the two projects ship the same files
        # under different names.
        "D:/MyOpen/NimRTC/build/nv_sdk/Video_Codec_Interface_11.1.5/include/ffnvcodec"
        "D:/MyOpen/NimRTC/build/nv_sdk/Video_Codec_Interface_11.1.5/include"
        "D:/MyOpen/NimRTC/build/nv_sdk/Video_Codec_Interface_11.1.5/Interface"
        # SDK 13.1.15 (current full NVIDIA Video Codec SDK)
        "D:/MyOpen/NimRTC/build/nv_sdk/Video_Codec_Interface_13.1.15/Interface"
        # CUDA include (header is also shipped with the CUDA toolkit)
        "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.6/include"
        "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.5/include"
        "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.4/include"
        "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.3/include"
        "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.2/include"
        "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.1/include"
        "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.0/include"
        # Linux
        "/usr/include"
        "/usr/local/cuda/include"
        "/opt/cuda/include"
        "/usr/local/include"
        "/opt/nvidia/video-sdk/include")

    find_path(NVENC_INCLUDE_DIR
        NAMES nvEncodeAPI.h nvcuvid.h
        PATHS ${_probe_paths}
        DOC "Directory containing NVENC headers"
        NO_DEFAULT_PATH)
endif()

if(NOT NVENC_LIBRARY)
    set(_probe_lib_paths)
    if(DEFINED ENV{NVIDIA_VIDEO_CODEC_SDK_DIR})
        if(WIN32)
            list(APPEND _probe_lib_paths "$ENV{NVIDIA_VIDEO_CODEC_SDK_DIR}/Lib/x64")
        else()
            list(APPEND _probe_lib_paths "$ENV{NVIDIA_VIDEO_CODEC_SDK_DIR}/lib")
        endif()
    endif()
    if(DEFINED ENV{CUDA_PATH})
        if(WIN32)
            list(APPEND _probe_lib_paths "$ENV{CUDA_PATH}/lib/x64")
        else()
            list(APPEND _probe_lib_paths "$ENV{CUDA_PATH}/lib64")
        endif()
    endif()

    list(APPEND _probe_lib_paths
        # Windows — CUDA redistrib lib
        "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.6/lib/x64"
        "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.5/lib/x64"
        "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.4/lib/x64"
        # Linux
        "/usr/lib/x86_64-linux-gnu"
        "/usr/lib64"
        "/usr/local/cuda/lib64")

    find_library(NVENC_LIBRARY
        NAMES
            nvidia-encode nvEncodeAPI
        PATHS ${_probe_lib_paths}
        DOC "Path to NVENC runtime library"
        NO_DEFAULT_PATH)

    # Fallback: if no .lib was found but the runtime DLL is on the system
    # path, accept the DLL directly.  CMake generates a DLL-side import
    # library from it automatically (the same mechanism that handles
    # kernel32.dll / user32.dll linking).
    if(NOT NVENC_LIBRARY)
        if(WIN32)
            find_file(_dll NAMES nvEncodeAPI64.dll PATHS ENV PATH)
            if(_dll)
                set(NVENC_LIBRARY "${_dll}" CACHE FILEPATH
                    "NVENC DLL (used as import lib)" FORCE)
                message(STATUS "  NVENC DLL        : using ${_dll} as import library")
            endif()
        endif()
    endif()
endif()

# Standard search fallback (system PATHs) when the probe above missed.
find_path(NVENC_INCLUDE_DIR NAMES nvEncodeAPI.h)
find_library(NVENC_LIBRARY
    NAMES nvidia-encode nvEncodeAPI)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(NVENC
    REQUIRED_VARS NVENC_INCLUDE_DIR
    VERSION_VAR   NVENC_VERSION
    HANDLE_COMPONENTS)

if(NVENC_FOUND)
    set(NVENC_INCLUDE_DIRS ${NVENC_INCLUDE_DIR})
    set(NVENC_LIBRARIES    ${NVENC_LIBRARY})
    set(NVENC_DEFINITIONS  "")

    if(NOT TARGET NimRTC::nvenc)
        if(NVENC_LIBRARY MATCHES "\\.dll$")
            # Only the runtime DLL is available (no import .lib). The
            # encoder loads the DLL via LoadLibrary at runtime, so we don't
            # want to add the DLL to the consumer's link line — that
            # triggers LNK1107 ("invalid or corrupt file") in MSVC because
            # a .dll is not a valid import library.  Use an INTERFACE
            # target that only contributes the include directory.
            add_library(NimRTC::nvenc INTERFACE IMPORTED)
            set_target_properties(NimRTC::nvenc PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${NVENC_INCLUDE_DIR}"
                INTERFACE_COMPILE_DEFINITIONS "${NVENC_DEFINITIONS}")
        else()
            add_library(NimRTC::nvenc UNKNOWN IMPORTED)
            set_target_properties(NimRTC::nvenc PROPERTIES
                IMPORTED_LOCATION             "${NVENC_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${NVENC_INCLUDE_DIR}"
                INTERFACE_COMPILE_DEFINITIONS "${NVENC_DEFINITIONS}")
        endif()
    endif()

    mark_as_advanced(NVENC_INCLUDE_DIR NVENC_LIBRARY)
endif()
