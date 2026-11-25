# cmake/NimRTCInstall.cmake — Track D install rules (v1.1.0).
#
# Centralised install logic so per-module CMakeLists stay untouched:
# the v1.1 plan asks for per-module `install()` coverage guarded by
# `NIMRTC_INSTALL`; this file provides exactly that coverage from one
# place (called at the END of the top-level CMakeLists, after every
# add_subdirectory, so `if(TARGET ...)` sees the real target set).
#
# What gets installed when NIMRTC_INSTALL=ON:
#   <prefix>/include/              — every src/*/include + src/modules/*/include
#   <prefix>/lib/                  — every nimrtc_* STATIC archive
#   <prefix>/lib/cmake/NimRTC/     — NimRTCConfig.cmake + NimRTCConfigVersion.cmake
#
# Deliberately NOT installed / exported:
#   - nimrtc_core_objects (OBJECT lib — objects are baked into the STATIC
#     archives already; OBJECT libs cannot be exported portably).
#   - nimrtc::core build-tree INTERFACE target (the installed config
#     re-creates `nimrtc::core` as an INTERFACE IMPORTED target).
#   - Most nimrtc_vendor_* targets (PRIVATE deps; L5 keeps them vendored,
#     the bundle already ships their stack per RELEASING.md §4 — wolfSSL /
#     libsrtp / libjuice / msquic install via their OWN upstream install()
#     rules, so downstream finds those archives in <prefix>/lib/ already).
#     EXCEPTION (§2b below): `opus` has no upstream install coverage
#     (it is added EXCLUDE_FROM_ALL), so it is installed here —
#     otherwise the downstream final link misses the opus_* symbols.
#   - No install(EXPORT ...) set: the hand-written NimRTCConfig.cmake.in
#     (find_library based) is the consumer contract, not a generated
#     export file.
#
# L1–L4 note: layout only, no new headers / impls / wiring.

if(NOT NIMRTC_INSTALL)
  return()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# --- 1. Public headers (one install per unique include root) ----------------
set(_nimrtc_install_headers
  "${CMAKE_SOURCE_DIR}/src/core/include"
  "${CMAKE_SOURCE_DIR}/src/plugins/include"
  "${CMAKE_SOURCE_DIR}/src/engine/include"
  "${CMAKE_SOURCE_DIR}/src/modules/sctp/include"
  "${CMAKE_SOURCE_DIR}/src/modules/dtls/include"
  "${CMAKE_SOURCE_DIR}/src/modules/raw_udp/include"
  "${CMAKE_SOURCE_DIR}/src/modules/transport/include"
  "${CMAKE_SOURCE_DIR}/src/modules/web_transport/include"
  "${CMAKE_SOURCE_DIR}/src/modules/rtp/include"
  "${CMAKE_SOURCE_DIR}/src/modules/sdp/include"
  "${CMAKE_SOURCE_DIR}/src/modules/net_feedback/include"
  "${CMAKE_SOURCE_DIR}/src/modules/jb/include"
  "${CMAKE_SOURCE_DIR}/src/modules/bwe/include"
  "${CMAKE_SOURCE_DIR}/src/modules/sched/include"
  "${CMAKE_SOURCE_DIR}/src/modules/pacing/include"
  "${CMAKE_SOURCE_DIR}/src/modules/jb_strategy/include"
  "${CMAKE_SOURCE_DIR}/src/modules/audio3a/include"
  "${CMAKE_SOURCE_DIR}/src/modules/audio_rx/include"
  "${CMAKE_SOURCE_DIR}/src/modules/ice/include"
  "${CMAKE_SOURCE_DIR}/src/modules/srtp/include"
  "${CMAKE_SOURCE_DIR}/src/modules/opus/include"
  "${CMAKE_SOURCE_DIR}/src/modules/session/include"
  "${CMAKE_SOURCE_DIR}/src/modules/assembly/include"
  "${CMAKE_SOURCE_DIR}/src/modules/datachannel/include"
  "${CMAKE_SOURCE_DIR}/src/modules/video_frame/include"
  "${CMAKE_SOURCE_DIR}/src/modules/video_payload/include"
  "${CMAKE_SOURCE_DIR}/src/modules/video_jb/include"
  "${CMAKE_SOURCE_DIR}/src/modules/h264/include"
  # v1-install-tree-video-deps (ADR-038): the shared NVIDIA / VideoToolbox
  # codec-adapter headers ship with the nvcodec archive so a downstream
  # host that registers the NVENC / NVDEC plugins can include them.
  "${CMAKE_SOURCE_DIR}/src/modules/nvcodec/include"
  "${CMAKE_SOURCE_DIR}/src/modules/video_sdp_attributes/include"
  "${CMAKE_SOURCE_DIR}/src/modules/video_source/include"
  "${CMAKE_SOURCE_DIR}/src/modules/video_stats/include"
  "${CMAKE_SOURCE_DIR}/src/modules/video_rtcp_feedback/include"
  "${CMAKE_SOURCE_DIR}/src/modules/rtx/include"
  "${CMAKE_SOURCE_DIR}/src/modules/video_pipeline/include"
  "${CMAKE_SOURCE_DIR}/src/modules/video_sink/include"
  "${CMAKE_SOURCE_DIR}/src/modules/audio_device/include"
  "${CMAKE_SOURCE_DIR}/src/modules/rd_input/include"
  "${CMAKE_SOURCE_DIR}/src/modules/rd_qoe/include"
  "${CMAKE_SOURCE_DIR}/src/modules/rd_session/include"
)

foreach(_inc IN LISTS _nimrtc_install_headers)
  if(EXISTS "${_inc}")
    install(DIRECTORY "${_inc}/"
      DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
      FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
  endif()
endforeach()
unset(_inc)

# --- 2. Static archives ------------------------------------------------------
# Uniform mapping: short `foo` -> target `nimrtc_foo`. `if(TARGET ...)`
# keeps module-OFF builds working (skipped modules simply install nothing).
set(_nimrtc_install_shorts
  engine
  registry
  plugins
  log
  rtp
  sdp
  net_feedback
  jb
  bwe
  sched
  pacing
  jb_strategy
  audio3a
  audio_rx
  ice
  srtp
  dtls
  opus
  session
  assembly
  datachannel
  video_frame
  video_payload
  video_jb
  h264
  # v1-install-tree-video-deps (ADR-038): `nimrtc_h264` links
  # `nimrtc::nvcodec` PUBLIC, so a downstream link of `nimrtc::engine`
  # that pulls in H.264 also needs the nvcodec archive on the flat chain.
  # The module builds on every platform (NVIDIA / VideoToolbox paths are
  # dynamic-load or "unavailable" stubs off their host), so the install
  # rule needs no platform branch.
  nvcodec
  video_sdp_attributes
  video_source
  video_stats
  video_rtcp_feedback
  rtx
  video_pipeline
  video_sink
  audio_device
  rd_input
  rd_qoe
  rd_session
  sctp
  dtls_seam
  raw_udp
  transport
  web_transport
)

foreach(_short IN LISTS _nimrtc_install_shorts)
  set(_tgt "nimrtc_${_short}")
  if(TARGET "${_tgt}")
    get_target_property(_type "${_tgt}" TYPE)
    # STATIC_LIBRARY only — INTERFACE (core) and OBJECT
    # (core_objects) are intentionally skipped, see header note.
    if(_type STREQUAL "STATIC_LIBRARY")
      install(TARGETS "${_tgt}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    endif()
    unset(_type)
  endif()
  unset(_tgt)
endforeach()
unset(_short)

# --- 2b. Vendored STATIC libs without upstream install coverage --------------
# `opus` (upstream libopus added EXCLUDE_FROM_ALL, so its own install()
# rules never run) would otherwise be missing from <prefix>/lib/, and the
# downstream final link would miss the opus_* symbols.
# wolfSSL / libsrtp / libjuice / msquic install via their OWN upstream
# install() rules — do NOT duplicate them here.
#
# v1.2.0 (plan E7): `nimrtc_vendor_usrsctp` used to be listed here too; the
# usrsctp vendor tree was removed with the usrsctp backend.
foreach(_vtgt opus)
  if(TARGET "${_vtgt}")
    get_target_property(_vtype "${_vtgt}" TYPE)
    if(_vtype STREQUAL "STATIC_LIBRARY")
      install(TARGETS "${_vtgt}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    endif()
    unset(_vtype)
  endif()
endforeach()
unset(_vtgt)

# --- 2c. WebRTC APM prebuilt archives ----------------------------------------
# The APM is prebuilt out-of-tree with Meson and has no upstream install()
# rules, so without this section the installed bundle would ship without
# libwebrtc-audio-processing-2 and the abseil archives it references: a
# downstream consumer linking nimrtc::engine would fail with undefined
# webrtc:: / absl:: symbols.
#
# The archive set AND their order come from the Meson build's own link line
# (tools/apm_link_order.py writes build_meson/apm_link_order.txt), so the
# installed tree reproduces the order Meson verified.  A static linker
# resolves symbols left-to-right, so the order is part of the contract.
#
# They land in <libdir>/webrtc_apm/ (not directly in <libdir>/) because the
# set is a dozen vendored archives no consumer should link by accident; the
# installed NimRTCConfig.cmake picks them up through the order manifest.
get_property(_apm_archives GLOBAL PROPERTY NIMRTC_APM_LINK_ARCHIVES)
if(_apm_archives)
  set(_apm_dest "${CMAKE_INSTALL_LIBDIR}/webrtc_apm")
  set(_apm_names "")
  foreach(_archive IN LISTS _apm_archives)
    get_filename_component(_name "${_archive}" NAME)
    list(FIND _apm_names "${_name}" _apm_dup_index)
    if(NOT _apm_dup_index EQUAL -1)
      message(FATAL_ERROR
        "Duplicate WebRTC APM archive basename '${_name}' in the install set. "
        "The bundle stores the archives by basename, so they must be unique: "
        "${_apm_archives}")
    endif()
    list(APPEND _apm_names "${_name}")
    install(FILES "${_archive}" DESTINATION "${_apm_dest}")
  endforeach()

  # The order manifest travels with the archives: find_library() returns
  # paths, not order, so the consumer needs the order spelled out.
  list(LENGTH _apm_names _apm_count)
  string(REPLACE ";" "\n" _apm_manifest_text "${_apm_names}")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/apm_link_order.txt"
       "${_apm_manifest_text}\n")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/apm_link_order.txt"
          DESTINATION "${_apm_dest}")
  message(STATUS
    "Installing WebRTC APM archives (${_apm_count}) into ${_apm_dest}")
endif()
unset(_apm_archives)

# --- 2d. OpenH264 vendored archives + link-order manifest ---------------------
# The OpenH264 wrapper (src/third_party/openh264/CMakeLists.txt) has no
# upstream install() rules, so without this section the installed bundle
# would ship without the three upstream archives and a video host linking
# nimrtc::engine would miss the Wels* / WelsCommon::* symbols.
#
# The wrapper publishes the archive set AND their link order as a GLOBAL
# property (NIMRTC_OPENH264_LINK_ARCHIVES) — the same shape as the WebRTC
# APM archives above (NIMRTC_APM_LINK_ARCHIVES, §2c): the module that
# builds the archives owns the order, not the install logic.
#
# A GNU static linker resolves symbols left-to-right, so the order is part
# of the contract: the encoder and the decoder reference `WelsLog()` /
# `g_kuiMbCountScan4Idx` from common, so common must come last.  MSVC
# ignores archive order (the wrapper documents this), so the order is
# harmless on Windows.  The manifest travels with the archives: the
# installed NimRTCConfig.cmake reads the order from it rather than
# guessing from find_library() (which returns paths, not order).
get_property(_openh264_targets GLOBAL PROPERTY NIMRTC_OPENH264_LINK_ARCHIVES)
if(_openh264_targets)
  set(_openh264_names "")
  foreach(_oh_target IN LISTS _openh264_targets)
    if(NOT TARGET "${_oh_target}")
      message(FATAL_ERROR
        "NIMRTC_OPENH264_LINK_ARCHIVES lists '${_oh_target}' but no such "
        "target exists — the OpenH264 wrapper must create all three "
        "archives together")
    endif()
    get_target_property(_oh_type "${_oh_target}" TYPE)
    if(NOT _oh_type STREQUAL "STATIC_LIBRARY")
      message(FATAL_ERROR
        "NIMRTC_OPENH264_LINK_ARCHIVES target '${_oh_target}' is "
        "${_oh_type}, expected STATIC_LIBRARY")
    endif()
    install(TARGETS "${_oh_target}"
      ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    # The installed archive keeps the target's default file name
    # (no OUTPUT_NAME is set on the wrapper targets), so the manifest
    # can spell out the platform prefix/suffix.
    get_target_property(_oh_outname "${_oh_target}" OUTPUT_NAME)
    if(_oh_outname)
      set(_oh_base "${_oh_outname}")
    else()
      set(_oh_base "${_oh_target}")
    endif()
    list(APPEND _openh264_names
      "${CMAKE_STATIC_LIBRARY_PREFIX}${_oh_base}${CMAKE_STATIC_LIBRARY_SUFFIX}")
    unset(_oh_outname)
    unset(_oh_base)
  endforeach()
  unset(_oh_target)

  list(LENGTH _openh264_names _oh_count)
  string(REPLACE ";" "\n" _oh_manifest_text "${_openh264_names}")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/openh264_link_order.txt"
       "${_oh_manifest_text}\n")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/openh264_link_order.txt"
          DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  message(STATUS
    "Installing OpenH264 archives (${_oh_count}) into ${CMAKE_INSTALL_LIBDIR}")
endif()
unset(_openh264_targets)
unset(_openh264_names)

# --- 3. Package config + version file ----------------------------------------
configure_package_config_file(
  "${CMAKE_SOURCE_DIR}/cmake/NimRTCConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/NimRTCConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/NimRTC"
  PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/NimRTCConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/NimRTCConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/NimRTCConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/NimRTC")

unset(_nimrtc_install_headers)
unset(_nimrtc_install_shorts)
