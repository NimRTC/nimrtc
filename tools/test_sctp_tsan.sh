#!/bin/bash
# Build and run the SCTP usrsctp regression test under ThreadSanitizer
# in WSL.
#
#   wsl -d Ubuntu-22.04 -- bash /mnt/d/MyOpen/NimRTC/tools/test_sctp_tsan.sh
#
# The test under test:
#   SctpUsrsctpTest.DestructorWaitsForInFlightRecvCallback
#
# It pins the library contract that ~UsrsctpSocket does not return
# while a user recv callback is still running.  Before the fix the
# destructor returned immediately and the callback raced with the
# destruction of the state it captured -- which ThreadSanitizer
# reported as a data race between SctpRecvQueue::push and
# ~SctpRecvQueue.
#
# Toolchain notes
# ---------------
# CI's tsan leg uses Clang's compiler-rt, because GCC 11's libtsan does
# not intercept pthread_cond_clockwait() -- which libstdc++ uses for
# condition_variable::wait_for() -- and reports bogus "double lock"
# findings for every timed wait in the tree.
#
# This image has clang-14 with the compiler-rt TSan runtime, but the
# image's *default* libstdc++ is GCC 13's, whose headers use `consteval`
# in a way clang-14 cannot compile (gtest fails to build).  So the
# libstdc++ headers are pinned to GCC 11's -- the same pairing CI's
# ubuntu-22.04 image has -- via --gcc-toolchain plus an explicit include
# path.  GCC 11's libtsan is NOT used: the compiler is clang, so the
# compiler-rt TSan runtime is what gets linked.
set -u
ROOT=/mnt/d/MyOpen/NimRTC
BUILD="$ROOT/build/wsl-sctp-tsan"

cd "$ROOT" || exit 2

CXX="${CXX:-clang-14}"
GCC11_INC="-I/usr/include/c++/11 -I/usr/include/x86_64-linux-gnu/c++/11"
if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "compiler '$CXX' not found in this image"
    exit 2
fi

cmake -S . -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang-14 \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_CXX_FLAGS="$GCC11_INC" \
    -DCMAKE_C_FLAGS="$GCC11_INC" \
    -DNIMRTC_VENDORED=ON \
    -DNIMRTC_BUILD_TESTS=ON \
    -DNIMRTC_BUILD_EXAMPLES=OFF \
    -DNIMRTC_BUILD_FUZZERS=OFF \
    -DNIMRTC_VENDORED_WEBRTC_APM=OFF \
    -DNIMRTC_ASAN=OFF \
    -DNIMRTC_UBSAN=OFF \
    -DNIMRTC_TSAN=ON \
    > "$BUILD.configure.log" 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "CONFIGURE FAILED (exit $rc):"
    tail -30 "$BUILD.configure.log"
    exit "$rc"
fi
echo "configure OK"

cmake --build "$BUILD" --target test_sctp_usrsctp -j 4 > "$BUILD.build.log" 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "BUILD FAILED (exit $rc):"
    grep -nE 'error:|FAILED:' "$BUILD.build.log" | head -30
    exit "$rc"
fi
echo "build OK"

echo "--- running the SCTP usrsctp suite under TSan ---"
TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1" \
    "$BUILD/tests/test_sctp_usrsctp" 2>&1 | tail -50
exit "${PIPESTATUS[0]}"
