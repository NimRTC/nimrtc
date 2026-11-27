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
# nimrtc_generate_dtls_certs()
#
# Generate the pair, as a build step, once per build tree.  This is the
# replacement for the `Generate the DTLS test certificate pair` step
# that every CI job carried by hand -- fifteen copies across five
# workflow files, all installing `cryptography<42` and running the
# generator, and one of them (`hardware.yml`) written differently from
# the other fourteen.  Fifteen copies is fifteen chances to drift, and
# the drift is invisible until the copy that was never run runs.
#
# **One target, not one custom command per consumer.**  The staging
# helpers below are called for twenty-six test targets; hanging a
# generate command off each of them would start twenty-six generators
# against `cryptography` whenever their POST_BUILD edges run close
# together in an incremental build.  Instead there is a single
# `nimrtc_dtls_certs` target, and every consumer only *depends* on it,
# so the generator runs exactly once per build and the copy steps read
# a directory that is complete by the time they run.
#
# The generator is `scripts/build_dtls_cert.py`, invoked with this
# CMake's own `Python3_EXECUTABLE`.   It writes
# `tests/wolfssl_dtls/certs/` in the *source tree* (that is where
# `nimrtc::dtls`'s compiled-in fallback points), so the target is
# marked with a byproduct there that the copy steps also name -- a
# multi-config build (Debug, then Release) does not regenerate.
#
# `cryptography` availability is probed once at configure time, with
# `execute_process` under `cmake -E env` so it sees the caller's
# environment.  When the probe says no, the target falls back to
# `scripts/generate_dtls_certs_ci.py` on PATH -- the one code path that
# knows the platform's pip invocation -- rather than failing the build
# with an interpreter traceback or, worse, skipping the pair silently.
# This is what makes the build own the artefact regardless of when the
# package was installed.
#
# `find_package(Python3)`'s interpreter (`-DPython3_EXECUTABLE`) is not
# always the `python` on PATH, and the two can have different
# site-packages.  The probe is therefore a *hint*, not a gate: a "no"
# selects the PATH fallback, and a "yes" only chooses the shorter path.
# -----------------------------------------------------------------------------
get_property(_nimrtc_certs_target GLOBAL PROPERTY NIMRTC_DTLS_CERTS_TARGET)
if(NOT _nimrtc_certs_target)
    set_property(GLOBAL PROPERTY NIMRTC_DTLS_CERTS_TARGET TRUE)

    find_package(Python3 COMPONENTS Interpreter QUIET)
    set(_nimrtc_py "")
    if(Python3_Interpreter_FOUND)
        # The `<42` pin is not cosmetic.  42+ ships a Rust-backed
        # Windows wheel whose `cryptography_rust.dll` is not in the
        # PyPI artefact, so `import cryptography` fails with
        # "DLL load failed while importing _rust".  41.x is the last
        # pure-C line -- the same reason every CI step that installed
        # it by hand pinned it.
        set(_nimrtc_py "${Python3_EXECUTABLE}")
        # `cmake -E env` runs the probe with the caller's environment,
        # the same way the build step will run the generator: a venv
        # the workflow activated is visible to both, or to neither.
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env
                    "${_nimrtc_py}" -c "import cryptography"
            RESULT_VARIABLE _nimrtc_have_cryptography
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT _nimrtc_have_cryptography EQUAL 0)
            set(_nimrtc_py "")
        endif()
    endif()

    if(_nimrtc_py)
        # `PYTHONDONTWRITEBYTECODE`: the generator lives in the source
        # tree, and a `__pycache__` next to it is a file the build has
        # no business creating there.
        set(_nimrtc_cert_cmd
            ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${_nimrtc_py} scripts/build_dtls_cert.py)
    else()
        # The PATH fallback: `generate_dtls_certs_ci.py` installs into
        # whichever of these two it finds, and that may not be the
        # interpreter `find_package` saw (or it may have been
        # installed *after* the configure that did not see it).  The
        # helper is asked for the command rather than re-implementing
        # its probe here, so the two cannot disagree.
        find_program(_nimrtc_cert_helper python3 PATHS ENV PATH)
        if(NOT _nimrtc_cert_helper)
            find_program(_nimrtc_cert_helper python PATHS ENV PATH)
        endif()
        if(_nimrtc_cert_helper)
            set(_nimrtc_cert_cmd
                ${_nimrtc_cert_helper} scripts/generate_dtls_certs_ci.py)
        else()
            set(_nimrtc_cert_cmd
                ${CMAKE_COMMAND} -E echo
                "the DTLS certificate pair was not generated: no Python interpreter is usable at configure time.  Install Python and cryptography, then reconfigure.")
        endif()
        message(WARNING
            "The DTLS test certificate generator cannot see `cryptography` "
            "with the interpreter CMake found.  The build will ask "
            "scripts/generate_dtls_certs_ci.py, which installs it into "
            "the first python3/python on PATH; if that fails, install it "
            "by hand with:\n"
            "  <python> -m pip install \"cryptography<42\"\n"
            "To pin the interpreter for both, pass "
            "-DPython3_EXECUTABLE=<path>.")
    endif()

    add_custom_target(nimrtc_dtls_certs
        COMMAND ${_nimrtc_cert_cmd}
        BYPRODUCTS
            "${NIMRTC_DTLS_CERT_STAGING_DIR}/server-ecc.pem"
            "${NIMRTC_DTLS_CERT_STAGING_DIR}/ecc-key.pem"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Ensuring the Chrome-friendly DTLS certificate pair exists"
        VERBATIM)
endif()

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

    # The pair is generated by the build (`nimrtc_generate_dtls_certs()`),
    # once per build tree; the copy below has to wait for it, or it reads
    # a directory that does not exist yet on a fresh checkout.
    if(TARGET nimrtc_dtls_certs)
        add_dependencies(${target} nimrtc_dtls_certs)
    endif()

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
