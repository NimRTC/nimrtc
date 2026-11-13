# modules/dtls/CMakeLists.txt
# ----------------------------------------------------------------------------
# DTLS (Datagram TLS, RFC 6347) — the unified DTLS module.
#
# Layout (v1.2, architecture remediation plan §4.1 / D3 — "unify the DTLS
# seam + both backends into ONE module directory, no transition period"):
#
#   src/modules/dtls/
#   ├── include/nimrtc/dtls/     ← the ONE public include root (9 headers)
#   ├── src/                     ← the seam: registration + factories
#   ├── backends/wolfssl/src/    ← the wolfSSL session implementation
#   ├── backends/gmssl/src/      ← the GmSSL session implementation (opt-in)
#   └── tests/                   ← the module's unit tests
#
# Before the unification the DTLS code lived in TWO directories:
#
#   src/dtls/          — the seam (IDtlsSession / IDtlsSessionFactory /
#                        dtls_types) + the GmSSL backend + the wolfSSL
#                        *factory*.
#   src/modules/dtls/  — the wolfSSL *session* implementation.
#
# That split made "which file is authoritative?" a real question (see
# ARCHITECTURE.md §"The seam-family directories" before the unification).
# Now there is exactly ONE DTLS module and the split is expressed at the
# *source* level instead: `src/` owns the interface + backend selection,
# `backends/<name>/src/` owns how one backend behaves.
#
# ## Downstream contract (UNCHANGED by the unification)
#
#   - Public include paths: `#include <nimrtc/dtls/...>` — identical.
#   - CMake target names:   `nimrtc_dtls` (the wolfSSL session library)
#                           `nimrtc_dtls_seam` (the seam + factories)
#                           plus the `nimrtc::dtls` / `nimrtc::dtls_seam`
#                           ALIAS targets — identical.
#
# Only the on-disk location of the sources changed.  Consumers that
# `add_subdirectory(nimrtc)` and link `nimrtc::dtls_seam` need no change
# at all.
#
# Scope:
#   - DTLS 1.2 handshake via wolfSSL (ClientHello / ServerHello /
#     Certificate / ServerKeyExchange / CertificateVerify / Finished)
#   - ECDHE-ECDSA-AES128-GCM-SHA256 (RFC 5764 §5 mandatory profile)
#   - Fingerprint generation + verification (SHA-256 over local SPKI)
#   - SRTP keying material export (RFC 5764 §4.2)
#   - Optional GmSSL backend behind the same seam (ADR-013)
#
# Dependencies:
#   - core (always)
#   - nimrtc::vendor::wolfssl (always — same backend on Windows, Linux, macOS)
# ----------------------------------------------------------------------------

# --- GMSSL backend (ADR-013, v1.1 X1) — opt-in, default OFF -------------
# The `NIMRTC_ENABLE_DTLS_GMSSL` / `NIMRTC_GMSSL_USE_SYSTEM` options are
# declared in the ROOT CMakeLists.txt (next to the other module switches),
# NOT here: the vendored GmSSL wrapper (src/third_party/gmssl/
# CMakeLists.txt) reads `NIMRTC_ENABLE_DTLS_GMSSL` while it is being
# added, and src/third_party is processed BEFORE src/modules.  Declaring
# the option at the top level guarantees the cache variable already
# exists at that point.

# =============================================================================
# Target 1: nimrtc_dtls — the wolfSSL session implementation.
#
# This target owns *how one backend behaves*: the handshake state machine,
# the record layer and the PRF.  The seam (target 2 below) owns *what the
# interface looks like* and *how a backend is chosen*.
# =============================================================================
add_library(nimrtc_dtls STATIC
    backends/wolfssl/src/dtls_session.cpp
    backends/wolfssl/src/dtls_wolfssl_session.cpp
    backends/wolfssl/src/dtls_prf.cpp     # RFC 5246 §5 PRF (wolfSSL HMAC-SHA-256) —
                                           # exported so the KAT + verify_data paths can
                                           # run independently of the live handshake.
    )

nimrtc_apply_options(nimrtc_dtls)

# wolfSSL upstream headers use C-style casts in their public macros
# (e.g. WC_LAST_E comparisons in error-crypt.h / error-ssl.h).  Suppress
# the resulting -Wold-style-cast warning for this module — we can't
# modify upstream vendored code.
if(NOT MSVC)
    target_compile_options(nimrtc_dtls PRIVATE
        -Wno-old-style-cast
        -Wno-double-promotion)
endif()

target_link_libraries(nimrtc_dtls
    PUBLIC
        nimrtc::core
        nimrtc::vendor::wolfssl
    )
target_compile_definitions(nimrtc_dtls
    PUBLIC
        NIMRTC_HAS_DTLS=1
        NIMRTC_USE_WOLFSSL_DTLS=1
    PRIVATE
        # Absolute path to the directory holding the vendored ECC cert/key.
        # The wolfSSL session resolves its certificate relative to the
        # process working directory first (so a build-dir override wins),
        # then falls back to this absolute source-tree path.  The fallback
        # matters because CTest runs each test with the *test-file* directory
        # as its working directory, which is one level above the
        # `<config>/certs/` directory the build's copy rule populates.
        # Without it every DTLS-dependent engine test fails with
        # "wolfSSL_CTX setup failed" on multi-config generators.
        #
        # The path is spelled relative to *this* directory (not
        # CMAKE_SOURCE_DIR) so it stays correct when NimRTC is consumed
        # via add_subdirectory() from a larger project.
        NIMRTC_WOLFSSL_CERT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/wolfssl/src/certs")

target_include_directories(nimrtc_dtls
    PUBLIC
        # The ONE public include root of the DTLS module.  It carries the
        # seam headers (IDtlsSession / IDtlsSessionFactory / dtls_types),
        # the concrete DtlsSession + DtlsSessionWolfSSL headers and the
        # handshake-reassembly shim — so `#include <nimrtc/dtls/...>`
        # resolves for every DTLS consumer exactly as it did when the
        # seam and the wolfSSL implementation lived in two directories.
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

add_library(nimrtc::dtls ALIAS nimrtc_dtls)

# =============================================================================
# Target 2: nimrtc_dtls_seam — the DTLS seam.
#
# Owns the *interface* + the *backend selection*:
#   - IDtlsSession / IDtlsSessionFactory (the seam itself — the headers
#     live in the shared include root above).
#   - The default `WolfsslDtlsFactory` (id = "wolfssl").
#   - The optional `GmSSLDtlsFactory` (id = "gmssl").
#   - nimrtc::dtls::register_default_plugins() idempotent entry point.
#
# Architectural role (per docs/plan/transport-selection.md §5.2 / §6.1):
#   The seam is the first slice of transport-layer plugin-replaceability
#   for NimRTC.  Slice 5 (SCTP), Slice 6 (raw UDP), Slice 7 (Stack +
#   Selector), and Slice 8 (engine integration) sit on top of it.
#   Registration is now the Slice 8 typed slot
#   `PluginRegistry::register_dtls_session` / `get_dtls_session`;
#   `register_default_plugins()` still also fills the Slice 4
#   test-only accessor.
#
# Dependencies:
#   - core (always)
#   - plugins (header-only; plugins::Status used in seam return types)
#   - nimrtc::dtls (the concrete wolfSSL module — for DtlsSessionWolfSSL)
#   - nimrtc::vendor::wolfssl (transitive via nimrtc::dtls; declared here
#     so the seam's tests link the same crypto backend)
#   - GMSSL v3.x native lib (opt-in via NIMRTC_ENABLE_DTLS_GMSSL)
# =============================================================================
set(_dtls_seam_srcs
    src/dtls_plugin.cpp
    src/dtls_wolfssl_factory.cpp
    )

if(NIMRTC_ENABLE_DTLS_GMSSL)
    list(APPEND _dtls_seam_srcs
        backends/gmssl/src/dtls_gmssl_session.cpp
        backends/gmssl/src/dtls_gmssl_factory.cpp
        )
endif()

add_library(nimrtc_dtls_seam STATIC
    ${_dtls_seam_srcs}
    )

nimrtc_apply_options(nimrtc_dtls_seam)

target_link_libraries(nimrtc_dtls_seam
    PUBLIC
        nimrtc::core
        nimrtc::plugins       # plugins::Status
        nimrtc::dtls          # DtlsSessionWolfSSL (refactored to inherit IDtlsSession)
    )

# Same compile definitions as the concrete dtls module — keeps wolfSSL
# config in lockstep so a future build of either target sees the same
# cipher list / EMS / KEEP_PEER_CERT flags.  These definitions are
# PUBLIC on nimrtc_dtls already; we still mirror them locally so a
# consumer that links only the seam (rare) gets a consistent build.
target_compile_definitions(nimrtc_dtls_seam
    PUBLIC
        NIMRTC_HAS_DTLS=1
        NIMRTC_USE_WOLFSSL_DTLS=1
    )

if(NIMRTC_ENABLE_DTLS_GMSSL)
    # ADR-013 (v1.1 X1): link the vendored GMSSL v3.x STATIC library.
    # nimrtc_link_gmssl() prefers the submodule at
    # src/third_party/gmssl/src via nimrtc_vendor_gmssl; the
    # find_path/find_library system probe inside only runs when
    # NIMRTC_GMSSL_USE_SYSTEM=ON is set explicitly.
    nimrtc_link_gmssl(nimrtc_dtls_seam)
    target_compile_definitions(nimrtc_dtls_seam
        PUBLIC
            NIMRTC_HAS_DTLS_GMSSL=1
        )
    message(STATUS "GMSSL DTLS backend: enabled (NIMRTC_ENABLE_DTLS_GMSSL=ON)")
else()
    message(STATUS "GMSSL DTLS backend: disabled (default; set NIMRTC_ENABLE_DTLS_GMSSL=ON to enable)")
endif()

target_include_directories(nimrtc_dtls_seam
    PUBLIC
        # Same ONE public include root as nimrtc_dtls above — the seam
        # headers and the concrete headers now live side by side, so
        # there is a single include root to install and to expose.
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include)

add_library(nimrtc::dtls_seam ALIAS nimrtc_dtls_seam)

# Tests
if(NIMRTC_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    # ensure a CTestTestfile.cmake gets generated at this level so ctest
    # can recurse into the tests/ subdir from src/modules/ (CMake 4.0
    # VS generator quirk -- without enable_testing() here the file is
    # silently skipped and the module's tests never register).
    enable_testing()
    add_subdirectory(tests)
endif()
