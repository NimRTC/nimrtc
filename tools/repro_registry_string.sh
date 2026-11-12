#!/bin/bash
# Compile the translation unit that actually failed in CI, with a
# selectable compiler.
#
# PR #51's Linux legs failed with
#
#   error: 'std::pair<_T1, _T2>::first' has incomplete type
#
# from src/core/src/pal_default_registrars.cpp, which includes
# nimrtc/core/registry.hpp.  The header included <string_view> but not
# <string>, and libstdc++'s std::pair instantiates its members eagerly,
# so `std::vector<std::pair<std::string, const T*>>` needs the complete
# type.
#
# GCC 11's <string_view> happens to pull <string> in transitively, so
# the bug is invisible there.  libstdc++ 13 (what CI uses) no longer
# does, so it reproduces there.
#
#   wsl -d Ubuntu-22.04 -- bash /mnt/d/MyOpen/NimRTC/tools/repro_registry_string.sh
#   wsl -d Ubuntu-22.04 -- bash /mnt/d/MyOpen/NimRTC/tools/repro_registry_string.sh --prove
#   wsl -d Ubuntu-22.04 -- bash /mnt/d/MyOpen/NimRTC/tools/repro_registry_string.sh g++-13
set -u
ROOT=/mnt/d/MyOpen/NimRTC
HDR="$ROOT/src/core/include/nimrtc/core/registry.hpp"
BAK=/tmp/registry.hpp.bak
TU="$ROOT/src/core/src/pal_default_registrars.cpp"

CXX="${1:-g++-13}"
cd "$ROOT" || exit 2

if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "compiler '$CXX' not found in this image"
    exit 2
fi

compile_tu() {
    "$CXX" -std=c++20 -fsyntax-only \
        -I src/core/include \
        -I src/plugins/include \
        -I src/log/include \
        "$TU" 2>&1 | head -14
    return "${PIPESTATUS[0]}"
}

if [ "${2:-}" = "--prove" ]; then
    cp "$HDR" "$BAK" || exit 2
    grep -v '^#include <string>$' "$BAK" > "$HDR"
    echo "--- compiling the real TU WITHOUT <string> (expected to fail) ---"
    out=$(compile_tu); rc=$?
    echo "$out"
    cp "$BAK" "$HDR" || exit 2
    rm -f "$BAK"
    if [ "$rc" -eq 0 ]; then
        echo "PROVE FAIL: the TU compiled without <string> under $CXX -- this compiler does not reproduce the CI failure."
        exit 1
    fi
    echo "PROVE OK: the TU fails without <string> under $CXX (exit $rc), as expected."
fi

echo "--- compiling the real TU WITH <string> (expected to succeed) ---"
out=$(compile_tu); rc=$?
echo "$out"
if [ "$rc" -eq 0 ]; then
    echo "OK: pal_default_registrars.cpp compiles under $CXX"
else
    echo "FAIL: pal_default_registrars.cpp does not compile under $CXX (exit $rc)"
fi
exit "$rc"
