# plugins/CMakeLists.txt
# =============================================================================
# Plugin interface definitions and the PluginRegistry.
#
# Most of this directory is a HEADER-ONLY INTERFACE library (nimrtc_plugins).
# The HW backend registry (hw_video_backend.cpp) is a STATIC impl that
# owns the per-process backend selector singleton; it cannot be header-only
# because the selection logic (priority sort, available() probes, mutex,
# install_stub_backends) requires a single .cpp.
#
# All concrete plugin implementations (built-in "standard" plugins) live
# in their respective modules under src/modules/. Those modules link
# nimrtc_plugins PUBLIC so users who link any module transitively get the
# interface headers.
#
# Modules that want to use the plugin system:
#   target_link_libraries(nimrtc_rtp PUBLIC nimrtc::plugins)
#
# @note P0 scaffold. Originally header-only; hw_video_backend.cpp was added
#       in R3-Batch to back the HW-first backend selector.
# =============================================================================

add_library(nimrtc_plugins STATIC
    src/hw_video_backend.cpp
)
add_library(nimrtc::plugins ALIAS nimrtc_plugins)

target_include_directories(nimrtc_plugins
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

# plugins/base.hpp includes <nimrtc/core/bytes.hpp> to unify buffer types
# with the core layer (see plugins/base.hpp for rationale).
target_link_libraries(nimrtc_plugins
    PUBLIC
        nimrtc::core
        # plugins/video_codec.hpp re-exports nimrtc::video_frame types
        # (VideoFrame, EncodedVideoFrame, PixelFormat, GpuBuffer, ...) as
        # typedef aliases. Any TU that transitively includes a plugins/*.hpp
        # header therefore needs the video_frame include path AND — for
        # callers that actually invoke IVideoCodec::encode/decode — the
        # link dep. We declare it INTERFACE so both propagate.
        # (Layering note: plugins depends on video_frame; video_frame itself
        # depends only on core. No cycle.)
        nimrtc::video_frame)

# No compile definitions needed — header-only.

# plugins/base.hpp       — IPlugin, IPluginFactory, BufferView, Status
# plugins/transport.hpp   — ITransport, ITransportFactory
# plugins/ice_transport.hpp — IICETransport, IICETransportFactory
# plugins/rtp.hpp        — IRTP, IRTPFactory
# plugins/sdp.hpp        — ISDP, ISDPFactory
# plugins/jb.hpp         — IJB, IJBFactory
# plugins/audio3a.hpp    — IAudio3A, IAudio3AFactory
# plugins/codec.hpp      — ICodec, ICodecFactory
# plugins/video_codec.hpp — IVideoCodec, IVideoCodecFactory
# plugins/video_source.hpp — IVideoSource, IVideoSourceFactory
# plugins/video_sink.hpp — IVideoSink, IVideoSinkFactory
# plugins/video_pipeline.hpp — IVideoReceiver, IVideoSender
# plugins/bwe.hpp        — IBwe, IBweFactory
# plugins/scheduler.hpp  — IScheduler, ISchedulerFactory
# plugins/datachannel.hpp — IDataChannel, IDataChannelFactory (P1: interface only)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
