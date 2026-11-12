#!/bin/bash
# Build the SCTP module with GCC 13 in WSL, to check the in-flight
# callback guard compiles outside MSVC.
#
#   wsl -d Ubuntu-22.04 -- bash /mnt/d/MyOpen/NimRTC/tools/build_sctp_gcc13.sh
#
# Only the sctp module and its dependencies are built -- the full tree
# takes ~10 minutes and needs the msquic quictls submodule, which is
# not the subject of this change.
set -u
ROOT=/mnt/d/MyOpen/NimRTC
BUILD="$ROOT/build/wsl-sctp-gcc13"

cd "$ROOT" || exit 2

cmake -S . -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=gcc-13 \
    -DCMAKE_CXX_COMPILER=g++-13 \
    -DNIMRTC_VENDORED=ON \
    -DNIMRTC_BUILD_TESTS=OFF \
    -DNIMRTC_BUILD_EXAMPLES=OFF \
    -DNIMRTC_BUILD_FUZZERS=OFF \
    -DNIMRTC_VENDORED_WEBRTC_APM=OFF \
    > "$BUILD.configure.log" 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "CONFIGURE FAILED (exit $rc):"
    tail -30 "$BUILD.configure.log"
    exit "$rc"
fi
echo "configure OK"

cmake --build "$BUILD" --target nimrtc_sctp -j 4 > "$BUILD.build.log" 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "BUILD FAILED (exit $rc):"
    grep -nE 'error:|FAILED:' "$BUILD.build.log" | head -30
    exit "$rc"
fi
echo "BUILD OK: nimrtc_sctp"
