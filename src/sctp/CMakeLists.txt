# src/sctp/CMakeLists.txt
# ----------------------------------------------------------------------------
# SCTP seam module (Transport PAL Slice 5 — SEAM ONLY).
#
# Per `docs/plan/transport-selection.md` §5.2 / §6.2:
#   - ISctpSocket / ISctpSocketFactory interface (the SEAM).
#   - SctpStubSocket / SctpStubFactory default impl (id="stub"), returning
#     plugins::kErrNotReady from every send method.
#   - register_default_plugins() entry point (Meyers-singleton latch).
#
# Slice 5 scope:
#   - Lay the seam ONLY — do NOT link usrsctp yet (the production default
#     impl lands in v0.11.0 per §7).
#   - The stub factory is registered under id "stub" (NOT "usrsctp") so
#     downstream code cannot accidentally believe the v0.11.0 backend is
#     already integrated — see sctp_stub_factory.cpp header rationale.
#
# v0.11.0 scope (NOT in this file's registration body — see
#                  sctp_plugin.cpp "Why usrsctp is not the default" header):
#   - usrsctp source files (`usrsctp_socket.cpp`, `usrsctp_factory.cpp`) are
#     STILL BUILT and linked into `nimrtc_sctp.lib` so opt-in callers
#     (out-of-tree integrators, the v1.x WebTransport / QUIC comparison
#     work, the in-process `tests/test_sctp_usrsctp` regression test) can
#     keep using the class directly without an ABI break. The default
#     registration in `sctp_plugin.cpp` no longer publishes
#     `UsrsctpSocketFactory` into `core::PluginRegistry` — see
#     `docs/plan/v0.11-plan.md` §2.2 (DC-1 / DC-2 / TPAL-5 deferred) and
#     the `@deprecated` comments in the matching headers.
#
# v1.x (planned, NOT this file):
#   - Replace `UsrsctpSocket` with a WebTransport-over-QUIC backend
#     (likely msquic, MIT-licensed, Windows-first, cross-platform CI). The
#     `ISctpSocket` seam is the wrong shape for WebTransport streams and
#     will either be replaced by an `IWebTransportSession` seam or wrapped
#     behind a polymorphic `IDataPlane` facade. This CMakeLists will
#     probably delete `src/usrsctp_*.cpp` and `src/third_party/usrsctp/`
#     at v1.2.0.
#
# Mirrors `src/modules/ice/CMakeLists.txt` structurally (static lib, one
# include root, enable_testing() + add_subdirectory(tests) when tests dir
# exists). The path is `src/sctp/` (NOT `src/modules/sctp/`) per the
# Slice 5 task brief and §6.2 of the plan — Slice 5 is a forward-looking
# module that will sit alongside the existing module tree.
#
# Dependencies:
#   - core (always; nimrtc_apply_options + log + (future) registry hooks)
#   - plugins (IDataChannel / IPluginFactory — base types)
# ----------------------------------------------------------------------------
add_library(nimrtc_sctp STATIC
    src/sctp_stub_socket.cpp
    src/sctp_stub_factory.cpp
    src/sctp_plugin.cpp
    # usrsctp production backend (id="usrsctp"). @deprecated v0.11.0 —
    # see file header and sctp_plugin.cpp for the v0.11.0 cut rationale.
    # The sources are kept so the class compiles for opt-in callers
    # and the v1.x WebTransport / QUIC work can compare approaches
    # without reviving the upstream fork.
    src/usrsctp_socket.cpp
    src/usrsctp_factory.cpp
    )

nimrtc_apply_options(nimrtc_sctp LENIENT)

target_link_libraries(nimrtc_sctp
    PUBLIC
        nimrtc::core
        nimrtc::plugins       # plugin base types (Status, BufferView, ...)
    PRIVATE
        # TPAL-5: usrsctp backend is only linked into the production
        # library, not re-exported (consumers must go through the
        # ISctpSocket seam — the factory pointer in the registry).
        nimrtc::vendor::usrsctp
    )

target_include_directories(nimrtc_sctp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}/include)

target_compile_definitions(nimrtc_sctp PUBLIC NIMRTC_HAS_SCTP_SEAM=1)

add_library(nimrtc::sctp ALIAS nimrtc_sctp)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
