#!/bin/sh
#
# pre-commit hook: block committing scratch / log / archive files at the
# project root. Allowed at root: CMake config, docs, .git metadata.
#
# Blocked at root:
#   *.ps1, *.sh, *.bat, *.cmd, *.py              (scripts → scripts/ or tools/)
#   *.log, *.err, *.out                           (captured stdio / build logs)
#   build_output*.txt, rebuild*.txt, rebuild*.log, reconfigure*.log,
#     _cmake_reconf*.log, final_build.log, full_build*.log,
#     test_opus_build*.log, opus_build*.log, opus_test_build*.log
#   _loopback*.*, _looperr.txt, _loopout.txt, _loopback_test.*,
#     _server_wrap.*, _signaling_server.log, _last_*.*
#   _run_server.py, _test_*.*, _wait_subagents.ps1, _run_all_video_tests.ps1,
#     _build_*, _full_build.*, _reconf*, _cmake*, check_encoding.ps1
#   *.zip, *.tar.gz, *.tar.bz2, *.tar.xz          (downloaded archives)
#   Testing/                                       (CTest scratch; see .gitignore)
#   *keylog*.log, sslkeylog*.log                   (TLS session keys)
#
# Blocked anywhere:
#   binary files                                    (see below)
#
# Installation (once):
#   cp scripts/check-root-clean.sh .git/hooks/pre-commit
#   chmod +x .git/hooks/pre-commit
#
# To bypass temporarily: git commit --no-verify

ROOT_BLOCKED='\.ps1$|\.sh$|\.bat$|\.cmd$|\.py$|\.log$|\.err$|\.out$|\.zip$|\.tar\.(gz|bz2|xz)$|^Testing/|^keylog|^sslkeylog|^_loopback|^_looperr\.|^_loopout\.|^_loopback_test\.|^_server_wrap\.|^_signaling_server\.log|^_last_|^_run_server\.py|^_test_|^_wait_subagents|^_run_all_video_tests|^_build_|^_full_build|^_reconf|^_cmake|^check_encoding\.|^build_output|^rebuild|^reconfigure|^_cmake_reconf|^final_build\.|^full_build|^test_opus_build|^opus_build|^opus_test_build'
ROOT_ALLOWED='CMakeLists\.txt|CMakePresets\.json|^\.github/'

# The root rules apply to the project root only: a `.py` under
# `scripts/` or `tools/` is exactly where it belongs, and only the
# root is forbidden.  So the candidate list is first narrowed to
# root-level entries (no `/` in the path), plus the `Testing/` scratch
# directory, and only those are matched against the patterns.
root_entries=$(git diff --cached --name-only --diff-filter=A \
    | grep -E '^[^/]+$|^Testing/')

blocked=$(echo "$root_entries" | grep -E "$ROOT_BLOCKED" | grep -v -E "$ROOT_ALLOWED")
if [ -n "$blocked" ]; then
    echo ""
    echo "ERROR: The following scratch / log / archive files are staged at the project root:"
    echo "$blocked" | sed 's/^/  /'
    echo ""
    echo "Project root must stay clean. Allowed at root:"
    echo "  CMake config  : CMakeLists.txt CMakePresets.json"
    echo "  Markdown docs : README.md ARCHITECTURE.md CHANGELOG.md CONTRIBUTING.md SECURITY.md"
    echo "  Config        : .gitignore .gitattributes .clang-format .editorconfig CODEOWNERS LICENSE NOTICE"
    echo ""
    echo "Move helpers to scripts/ or tools/; redirect output to build/ instead."
    echo "TLS keylogs (*keylog*.log) must never be committed -- they decrypt"
    echo "  the matching TLS traffic. Point SSLKEYLOGFILE outside the repo."
    echo "To bypass temporarily: git commit --no-verify"
    echo ""
    exit 1
fi

# ---------------------------------------------------------------------------
# Binary files must never be committed (outside the vendored third-party
# tree, where upstream ships its own test data and fuzz corpora).
#
# Binary blobs review badly, they merge badly, and in this project every
# one of them is reproducible from a generator script: the fuzz corpus
# seeds are checked in as `.hex` text and decoded into the build tree by
# `tests/fuzz/tools/make_corpus.py --materialize`.
#
# The test is a NUL byte in the first 8 KiB: every text file we ship is
# UTF-8 / ASCII, and every binary blob we would want to catch contains
# NULs early.  The check compares the size of the file with the size of
# the same file with all NUL bytes removed -- if they differ, the file
# contains a NUL and is binary.
# ---------------------------------------------------------------------------
BIN_ALLOWED='^src/third_party/'

binaries=$(git diff --cached --name-only --diff-filter=A \
    | grep -v -E "$BIN_ALLOWED" \
    | while IFS= read -r f; do
        [ -f "$f" ] || continue
        total=$(LC_ALL=C wc -c < "$f" | tr -d ' ')
        stripped=$(LC_ALL=C tr -d '\000' < "$f" | LC_ALL=C wc -c | tr -d ' ')
        [ "$total" != "$stripped" ] && echo "$f"
    done)

if [ -n "$binaries" ]; then
    echo ""
    echo "ERROR: The following binary files are staged (outside src/third_party/):"
    echo "$binaries" | sed 's/^/  /'
    echo ""
    echo "Binary files do not belong in this repository. Store the data as"
    echo "text and regenerate the binary form into the build tree -- for the"
    echo "fuzz corpus that means .hex seeds plus:"
    echo "  python3 tests/fuzz/tools/make_corpus.py --materialize <build-dir>"
    echo "To bypass temporarily: git commit --no-verify"
    echo ""
    exit 1
fi
