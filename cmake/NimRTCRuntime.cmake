# NimRTCRuntime.cmake
# ----------------------------------------------------------------------------
# Runtime staging for the binaries that ship in the tree.
#
# ## Why this file exists
#
# A NimRTC executable that links `nimrtc::web_transport` pulls in the
# msquic **shared** library.  On Windows that library is a DLL, and the
# loader looks for it next to the executable — so a binary that is
# missing `msquic.dll` does not run at all: it dies with
# `STATUS_DLL_NOT_FOUND` (0xc0000135) *before* `main()` is entered,
# with no message and no log line.  The exit code is the only symptom,
# and it looks like a crash in the program rather than a missing file.
#
# The test tree already had this handled (`nimrtc_copy_msquic_dll()` in
# `cmake/NimRTCTest.cmake`), because `gtest_discover_tests` *runs* the
# executable to enumerate its cases and therefore noticed the missing
# DLL at build time.  The **examples** had no such handler — so every
# example in the tree compiled and then refused to start.  That is the
# worst shape for a shipped artefact: the build is green and the binary
# is unusable.
#
# So the helper lives here, in a module that both the test tree and the
# example tree include, and it is applied to **every** executable that
# links the web_transport module.
#
# ## The DTLS certificates
#
# The same shape applies to the wolfSSL certificates.  `nimrtc::dtls`
# resolves `certs/server-ecc.pem` relative to the process working
# directory, with an absolute source-tree fallback compiled into the
# library (`NIMRTC_WOLFSSL_CERT_DIR`).  The fallback means the binary
# *starts*, but the fallback certificate is the upstream wolfSSL one —
# and it has **no `subjectAltName` extension**, which Chrome (BoringSSL)
# rejects with fatal alert 46 ("certificate_unknown").  A NimRTC-to-
# NimRTC session works with it; a browser session does not.  The
# examples therefore stage the Chrome-friendly certificate next to the
# executable, exactly as `demo-p2p` already did by hand.
# ----------------------------------------------------------------------------

# -----------------------------------------------------------------------------
# The generated DTLS certificate directory
#
# The Chrome-friendly cert pair is produced by `scripts/build_dtls_cert.py`
# into `tests/wolfssl_dtls/certs/`.  That directory is gitignored (`.gitignore:227`),
# so it is a pure build artefact — it is NOT in the repository and does NOT
# survive into a fresh git worktree.
#
# It used to be mirrored into wolfSSL's own cert tree
# (`src/third_party/wolfssl/src/certs/`).  That was removed on 2026-11-22:
# `certs/server-ecc.pem` is **tracked** inside that submodule, so overwriting
# it left the submodule permanently `-dirty` (and the main repo showing
# `m src/third_party/wolfssl/src`).  A build artefact must not dirty a
# vendored dependency's tracked files.
#
# Consequence: the pair must be generated before the build.  `zelto` does that
# via `worktree.prepare` (ADR-095); a hand-driven build should run
# `python scripts/build_dtls_cert.py` first.  If the files are missing the
# staging step below fails loudly at build time rather than silently shipping
# a SAN-less cert.
# -----------------------------------------------------------------------------
set(NIMRTC_DTLS_CERT_STAGING_DIR
    "${CMAKE_SOURCE_DIR}/tests/wolfssl_dtls/certs"
    CACHE INTERNAL "The generated DTLS certificate directory")

# -----------------------------------------------------------------------------
# nimrtc_stage_msquic_dll(<target>)
#
# Copy the msquic runtime DLL next to <target>'s executable.
#
# The DLL is per-config (the vendored msquic build writes
# `bin/Debug/` and `bin/Release/`), so the source path uses a
# `$<CONFIG>` generator expression.
#
# The helper is a no-op when:
#
#   * the platform is not Windows (Linux / macOS resolve the shared
#     object through the loader's normal search path, and the vendored
#     build there is a static archive), and
#   * the vendored msquic target does not exist (the build was
#     configured without the vendored dependency).
# -----------------------------------------------------------------------------
function(nimrtc_stage_msquic_dll target)
    # Guard against a double-call.  A *global property* is the dedupe
    # key rather than a dummy target: the property has no build-graph
    # footprint, so staging does not add a target to the project.
    get_property(_already GLOBAL PROPERTY NIMRTC_MSQUIC_DLL_STAGED_${target})
    if(_already)
        return()
    endif()
    set_property(GLOBAL PROPERTY NIMRTC_MSQUIC_DLL_STAGED_${target} TRUE)

    if(NOT WIN32)
        return()
    endif()
    if(NOT TARGET msquic)
        return()
    endif()

    # The vendored msquic build writes its artefacts under
    # `bin/$<IF:$<CONFIG:Debug>,Debug,Release>` (see
    # src/third_party/msquic/CMakeLists.txt, QUIC_OUTPUT_DIR).
    set(_msquic_dll_src
        "${CMAKE_BINARY_DIR}/src/third_party/msquic/bin/$<CONFIG>/msquic.dll")

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<BOOL:$<CONFIG>>,${_msquic_dll_src},${_msquic_dll_src}>"
            "$<TARGET_FILE_DIR:${target}>/"
        COMMENT "Staging msquic.dll next to ${target}"
        VERBATIM)
endfunction()

# -----------------------------------------------------------------------------
# nimrtc_stage_dtls_certs(<target>)
#
# Copy the Chrome-friendly DTLS certificate + key into
# `<target-dir>/certs/`, which is where `nimrtc::dtls` looks for them
# (the lookup is relative to the process working directory, and the
# conventional working directory for a hand-run example is the
# directory it lives in).
#
# The copy is idempotent (`copy_if_different`), so it costs nothing on
# an incremental build.
# -----------------------------------------------------------------------------
function(nimrtc_stage_dtls_certs target)
    get_property(_already GLOBAL PROPERTY NIMRTC_DTLS_CERTS_STAGED_${target})
    if(_already)
        return()
    endif()
    set_property(GLOBAL PROPERTY NIMRTC_DTLS_CERTS_STAGED_${target} TRUE)

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${target}>/certs"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NIMRTC_DTLS_CERT_STAGING_DIR}/server-ecc.pem"
            "$<TARGET_FILE_DIR:${target}>/certs/server-ecc.pem"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NIMRTC_DTLS_CERT_STAGING_DIR}/ecc-key.pem"
            "$<TARGET_FILE_DIR:${target}>/certs/ecc-key.pem"
        COMMENT "Staging the Chrome-friendly DTLS certificate for ${target}"
        VERBATIM)
endfunction()

# -----------------------------------------------------------------------------
# nimrtc_stage_runtime(<target>)
#
# The one call a shipped executable needs: stage everything it needs to
# *start* — the msquic runtime DLL and the DTLS certificate.
#
# Call this on every executable that links `nimrtc::web_transport`
# (directly or through `nimrtc::engine`).
# -----------------------------------------------------------------------------
function(nimrtc_stage_runtime target)
    nimrtc_stage_msquic_dll(${target})
    nimrtc_stage_dtls_certs(${target})
endfunction()
