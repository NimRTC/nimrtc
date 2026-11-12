#!/bin/bash
# Full configure+build with GCC 13 on WSL, mirroring the CI Linux leg.
#
# The single-include fix for registry.hpp was verified with a
# -fsyntax-only compile of the one failing TU.  This script goes
# further and builds the whole tree with GCC 13, so any *other*
# libstdc++-13-only breakage in the Phase 1 / Phase 2 changes surfaces
# locally instead of in CI.
#
#   wsl -d Ubuntu-22.04 -- bash /mnt/d/MyOpen/NimRTC/tools/build_gcc13.sh
#
# Uses a build directory under build/ (never the repo root).
set -u
ROOT=/mnt/d/MyOpen/NimRTC
BUILD="$ROOT/build/wsl-gcc13"

cd "$ROOT" || exit 2

# The vendored tree is large; keep the parallelism modest so the
# Windows host does not thrash.
JOBS="${JOBS:-4}"

cmake -S . -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=gcc-13 \
    -DCMAKE_CXX_COMPILER=g++-13 \
    -DNIMRTC_VENDORED=ON \
    -DNIMRTC_BUILD_TESTS=ON \
    -DNIMRTC_BUILD_EXAMPLES=OFF \
    -DNIMRTC_BUILD_FUZZERS=OFF \
    -DNIMRTC_VENDORED_WEBRTC_APM=OFF \
    > "$BUILD.configure.log" 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "CONFIGURE FAILED (exit $rc):"
    tail -40 "$BUILD.configure.log"
    exit "$rc"
fi
echo "configure OK"

cmake --build "$BUILD" -j "$JOBS" > "$BUILD.build.log" 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "BUILD FAILED (exit $rc):"
    grep -nE 'error:|Error [0-9]|FAILED:' "$BUILD.build.log" | head -40
    exit "$rc"
fi
echo "BUILD OK"
