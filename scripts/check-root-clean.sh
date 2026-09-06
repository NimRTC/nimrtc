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
#
# Installation (once):
#   cp scripts/check-root-clean.sh .git/hooks/pre-commit
#   chmod +x .git/hooks/pre-commit
#
# To bypass temporarily: git commit --no-verify

ROOT_BLOCKED='\.ps1$|\.sh$|\.bat$|\.cmd$|\.py$|\.log$|\.err$|\.out$|\.zip$|\.tar\.(gz|bz2|xz)$|^_loopback|^_looperr\.|^_loopout\.|^_loopback_test\.|^_server_wrap\.|^_signaling_server\.log|^_last_|^_run_server\.py|^_test_|^_wait_subagents|^_run_all_video_tests|^_build_|^_full_build|^_reconf|^_cmake|^check_encoding\.|^build_output|^rebuild|^reconfigure|^_cmake_reconf|^final_build\.|^full_build|^test_opus_build|^opus_build|^opus_test_build'
ROOT_ALLOWED='CMakeLists\.txt|CMakePresets\.json|^\.github/'

blocked=$(git diff --cached --name-only --diff-filter=A | grep -E "$ROOT_BLOCKED" | grep -v -E "$ROOT_ALLOWED")

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
    echo "To bypass temporarily: git commit --no-verify"
    echo ""
    exit 1
fi
