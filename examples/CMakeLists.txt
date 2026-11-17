# examples/CMakeLists.txt
# ----------------------------------------------------------------------------
# Example / demo programs.  Built only when NIMRTC_BUILD_EXAMPLES=ON.
# ----------------------------------------------------------------------------

if(NOT NIMRTC_BUILD_EXAMPLES)
    return()
endif()

# -----------------------------------------------------------------------------
# The runtime staging (msquic.dll + the DTLS certificate).
#
# Every example below links `nimrtc::web_transport` (directly or through
# `nimrtc::engine`), which pulls in the msquic **shared** library.  On
# Windows that library is a DLL, and the loader looks for it next to the
# executable — so an example without `msquic.dll` beside it does not
# run: it dies with `STATUS_DLL_NOT_FOUND` (0xc0000135) *before* `main()`
# is entered, with no message.  The exit code is the only symptom, and
# it reads like a crash in the program rather than a missing file.
#
# The examples also need the *Chrome-friendly* DTLS certificate: the
# absolute fallback compiled into `nimrtc::dtls` points at the upstream
# wolfSSL certificate, which carries no `subjectAltName` — and Chrome
# (BoringSSL) rejects it with fatal alert 46 ("certificate_unknown").
# A NimRTC-to-NimRTC session works with either; a browser session only
# works with the staged one.
#
# `nimrtc_stage_runtime()` does both.  See cmake/NimRTCRuntime.cmake for
# the rationale.
# -----------------------------------------------------------------------------

# demo-p2p: P1 baseline — generate SDP offer, optionally process remote SDP.
add_executable(demo-p2p demo-p2p/demo-p2p.cpp)
nimrtc_apply_options(demo-p2p)
# PAL Slice 2 (v0.10.2) — nimrtc::engine pulls in the unified
# kDefaultRegistrars[] table which references every module's
# `register_default_plugins()` symbol. datachannel's symbol is one of
# those — link the lib so the example binary resolves cleanly.
# WebTransport (v0.14.0-beta.1) also registers via the same table, so
# the example must link the web_transport module too; otherwise the
# `nimrtc::web_transport::register_default_plugins()` symbol referenced
# by `pal_default_registrars.cpp` is unresolved at link time.
target_link_libraries(demo-p2p PRIVATE
    nimrtc::engine
    nimrtc::datachannel
    nimrtc::web_transport)

# Stage the msquic runtime DLL and the Chrome-friendly DTLS cert.
# Without the cert, demo-p2p launched by interop/signaling/signaling_proxy.py
# would fall back to the SAN-less upstream wolfSSL cert at the relative
# "certs/" path — which Chrome (BoringSSL) rejects with fatal alert 46
# ("certificate_unknown").
nimrtc_stage_runtime(demo-p2p)

# loopback-p2p: P1.1 — two NimRTCEngines connect over loopback.
add_executable(loopback-p2p loopback-p2p/loopback-p2p.cpp)
nimrtc_apply_options(loopback-p2p)
# See demo-p2p above for the PAL Slice 2 datachannel link rationale.
# WebTransport is also pulled in for the same reason (the engine's
# `kDefaultRegistrars[]` table references it).
target_link_libraries(loopback-p2p PRIVATE
    nimrtc::engine
    nimrtc::datachannel
    nimrtc::web_transport)

# Same staging for loopback-p2p — its in-process handshake exercises
# the same wolfSSL CTX code path, so it should use the Chrome-friendly
# cert even for synthetic tests.  Cheap, idempotent.
nimrtc_stage_runtime(loopback-p2p)

# demo-agent-gateway: PCM tap → streaming ASR → LLM mock flow demo.
add_subdirectory(demo-agent-gateway)

# demo-agent-realtime: real-time AI game decision processing demo
# (slice v2-ai-decision) — >=3 strategies, <=50ms decisions, >=90% accuracy,
# debug decision trace.  Self-contained; registers a self-test ctest case.
add_subdirectory(demo-agent-realtime)

# demo-wtserver: WebTransport echo server for Case E (Chrome ↔ NimRTC interop).
# Uses the msquic-backed IWebTransportServer to accept Chrome WebTransport
# connections over QUIC + HTTP/3 + WebTransport (RFC 9298).
add_executable(demo-wtserver demo-wtserver/demo-wtserver.cpp)
nimrtc_apply_options(demo-wtserver)
target_link_libraries(demo-wtserver PRIVATE
    nimrtc::web_transport  # IWebTransportServer + msquic listener
)

# The msquic runtime DLL and the Chrome-friendly DTLS cert — this server
# is the *browser-facing* end of the Case E interop, so the certificate
# it presents has to carry a subjectAltName.
nimrtc_stage_runtime(demo-wtserver)

# dbghelp (SymFromAddr) is used by the Windows crash backtrace.  The
# library must therefore be linked conditionally: an unconditional link
# fails with "cannot find -ldbghelp" on Linux and macOS, which breaks
# their CI jobs.
if(WIN32)
    target_link_libraries(demo-wtserver PRIVATE dbghelp)
endif()

# ---------------------------------------------------------------------------
# The remote-desktop examples (plan Track G, examples/remote-desktop/)
# ---------------------------------------------------------------------------
#
# `rd_device` is the **device** side of a remote-desktop session: the
# machine whose screen is shared.  It captures the screen through the
# engine's capture-device seam (with the engine's capture pump doing
# the encode), and it injects the operator's input into the local OS
# through the `rd_input_inject` module.
#
# `rd_viewer` is the **operator** side: it decodes the device's screen
# into a video sink and captures the operator's local mouse / keyboard
# to send over the control channel.
#
# Both speak the same newline-delimited JSON signalling dialect as
# `demo-p2p`, so the reference service's client (`client.py`) bridges
# them without a change.
#
# The `rd_input_inject` module is *optional* (a build without it gets
# a read-only device), so the target is only created when the module
# was built.
if(NIMRTC_MODULE_RD_INPUT_INJECT)
    add_executable(rd_device remote-desktop/rd_device.cpp)
    nimrtc_apply_options(rd_device)
    target_link_libraries(rd_device PRIVATE
        nimrtc::engine
        nimrtc::rd_input        # the event vocabulary + the control channel
        nimrtc::rd_input_inject # the OS input injection
        nimrtc::datachannel    # the engine's registrar table references it
        nimrtc::web_transport  # ditto
    )

    # The device needs the capture-device module (the screen backends)
    # and the video sink / codec modules.
    if(NIMRTC_MODULE_VIDEO_CAPTURE)
        target_link_libraries(rd_device PRIVATE nimrtc::video_capture)
    endif()
    if(NIMRTC_MODULE_VIDEO_SINK)
        target_link_libraries(rd_device PRIVATE nimrtc::video_sink)
    endif()

    # The msquic runtime DLL and the Chrome-friendly DTLS cert: without
    # them the device dies with STATUS_DLL_NOT_FOUND before `main()`.
    nimrtc_stage_runtime(rd_device)
endif()

# The viewer needs the `rd_input` module (the event vocabulary, the
# coalescer, the binder, the watchdog and the safety filter) — but not
# `rd_input_inject`: the operator side *captures* the input, it does
# not inject it.  So the viewer is built whenever the input module is.
if(NIMRTC_MODULE_RD_INPUT)
    add_executable(rd_viewer remote-desktop/rd_viewer.cpp)
    nimrtc_apply_options(rd_viewer)
    target_link_libraries(rd_viewer PRIVATE
        nimrtc::engine
        nimrtc::rd_input        # the event vocabulary + the input policy
        nimrtc::datachannel    # the engine's registrar table references it
        nimrtc::web_transport  # ditto
    )

    # The viewer renders the device's screen into a video sink.
    if(NIMRTC_MODULE_VIDEO_SINK)
        target_link_libraries(rd_viewer PRIVATE nimrtc::video_sink)
    endif()
    # The QoE collector (plan J3 / J6) measures the input-to-photon latency.
    if(NIMRTC_MODULE_RD_QOE)
        target_link_libraries(rd_viewer PRIVATE nimrtc::rd_qoe)
    endif()

    # The msquic runtime DLL and the Chrome-friendly DTLS cert: without
    # them the viewer dies with STATUS_DLL_NOT_FOUND before `main()`.
    nimrtc_stage_runtime(rd_viewer)
endif()

# ---------------------------------------------------------------------------
# `rd_verify` — the M1 sign-off program
# ---------------------------------------------------------------------------
#
# `rd_verify` drives the *whole* remote-desktop media chain in **one
# process** on **one real desktop**: the DXGI / WGC capture, the
# zero-copy GPU seam, NVENC, the RTP packetization, NVDEC and the D3D11
# renderer — and then prints a table whose most important line is
# `cpu bytes copied: 0`.
#
# It is the human-facing half of the M1 exit criterion: the tests prove
# the wiring and the driver facts, and this program proves that the two
# hold *together, on a real machine, with real pixels*.
#
# It needs the capture module (the screen backends), the codec module
# (the H.264 backends) and the sink module (the renderer).
if(NIMRTC_MODULE_VIDEO_CAPTURE AND NIMRTC_MODULE_VIDEO_SINK)
    add_executable(rd_verify remote-desktop/rd_verify.cpp)
    nimrtc_apply_options(rd_verify)
    target_link_libraries(rd_verify PRIVATE
        nimrtc::engine
        nimrtc::core
        nimrtc::plugins
        nimrtc::ice
        nimrtc::rtp
        nimrtc::sdp
        nimrtc::dtls_seam
        nimrtc::datachannel       # the engine's registrar table references it
        nimrtc::web_transport     # ditto
        nimrtc::session
        nimrtc::video_source      # the "memory" source the engine instantiates
        nimrtc::video_sink        # the D3D11 renderer
        nimrtc::video_pipeline    # the reference sender / receiver
        nimrtc::video_capture     # the DXGI / WGC screen backends
        nimrtc::video_frame
    )
    if(TARGET nimrtc::h264)
        target_link_libraries(rd_verify PRIVATE nimrtc::h264)
    endif()
    if(TARGET nimrtc::nvcodec)
        target_link_libraries(rd_verify PRIVATE nimrtc::nvcodec)
    endif()

    # The msquic runtime DLL and the Chrome-friendly DTLS cert.
    nimrtc_stage_runtime(rd_verify)
endif()
