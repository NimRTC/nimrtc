#!/bin/bash
# Verify the registry.hpp fix in an arbitrary checkout of the repo.
#
#   wsl -d Ubuntu-22.04 -- bash /mnt/d/MyOpen/NimRTC/tools/verify_registry_fix.sh <repo-root> <compiler>
#
# Compiles src/core/src/pal_default_registrars.cpp -- the TU that
# actually failed in CI -- with the given compiler, and reports whether
# it succeeds.
set -u
ROOT="${1:-/mnt/d/MyOpen/NimRTC}"
CXX="${2:-g++-13}"

cd "$ROOT" || exit 2

if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "compiler '$CXX' not found in this image"
    exit 2
fi

"$CXX" -std=c++20 -fsyntax-only \
    -I src/core/include \
    -I src/plugins/include \
    -I src/log/include \
    src/core/src/pal_default_registrars.cpp
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "OK: pal_default_registrars.cpp compiles under $CXX in $ROOT"
else
    echo "FAIL: pal_default_registrars.cpp does not compile under $CXX in $ROOT (exit $rc)"
fi
exit "$rc"
