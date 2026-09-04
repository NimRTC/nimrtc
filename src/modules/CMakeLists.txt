# modules/CMakeLists.txt
# ----------------------------------------------------------------------------
# Container directory: each module is its own subdirectory and may be
# enabled/disabled independently. Modules are isolated: they declare their
# own dependencies via target_link_libraries, and may NOT depend on each
# other (except where a documented cross-module header is required, e.g.
# `src/modules/jb` may include `src/modules/rtp/include/nimrtc/rtp/packet.hpp`).
#
# All modules link nimrtc::plugins (header-only) so they can:
#   - implement plugin interfaces (ITransport, IRTP, etc.)
#   - register themselves with PluginRegistry
# ----------------------------------------------------------------------------

# -----------------------------------------------------------------------------
# Module switch — disable a module to skip its subdirectory entirely.
# -----------------------------------------------------------------------------
option(NIMRTC_MODULE_RTP  "Build RTP module"  ON)
option(NIMRTC_MODULE_SDP  "Build SDP module"  ON)
option(NIMRTC_MODULE_JB   "Build jitter buffer module"  ON)
option(NIMRTC_MODULE_BWE  "Build bandwidth estimator module"  ON)
option(NIMRTC_MODULE_AUDIO3A  "Build audio 3A module"  ON)
option(NIMRTC_MODULE_ICE  "Build ICE module"  ON)
option(NIMRTC_MODULE_SRTP  "Build SRTP module"  ON)
option(NIMRTC_MODULE_DTLS  "Build DTLS module"  ON)
option(NIMRTC_MODULE_OPUS  "Build Opus codec module"  ON)

# -----------------------------------------------------------------------------
# Module targets — order is irrelevant; each is self-contained.
# -----------------------------------------------------------------------------
if(NIMRTC_MODULE_RTP)
    add_subdirectory(rtp)
endif()

if(NIMRTC_MODULE_SDP)
    add_subdirectory(sdp)
endif()

if(NIMRTC_MODULE_JB)
    add_subdirectory(jb)
endif()

if(NIMRTC_MODULE_BWE)
    add_subdirectory(bwe)
endif()

if(NIMRTC_MODULE_AUDIO3A)
    add_subdirectory(audio3a)
endif()

if(NIMRTC_MODULE_ICE)
    add_subdirectory(ice)
endif()

if(NIMRTC_MODULE_SRTP)
    add_subdirectory(srtp)
endif()

if(NIMRTC_MODULE_DTLS)
    add_subdirectory(dtls)
endif()

if(NIMRTC_MODULE_OPUS)
    add_subdirectory(opus)
endif()

# CTest discovery: generate a CTestTestfile.cmake here so ctest recurses
# into each module's tests/ subdirectory. Without this, the root-level
# subdirs("src/modules") entry has nothing to follow.
if(NIMRTC_BUILD_TESTS)
    enable_testing()
endif()

# Future modules:
# if(NIMRTC_MODULE_DTLS) add_subdirectory(dtls) endif()