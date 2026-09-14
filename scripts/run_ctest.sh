#!/bin/bash
# scripts/run_ctest.sh
# Convenience wrapper for ctest on multi-config MSVC builds.
# Equivalent to:  ctest -C Debug "$@"

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"

if [ ! -d "$BUILD_DIR" ]; then
    echo "Build directory not found: $BUILD_DIR" >&2
    echo "Configure first with:  cmake -B $BUILD_DIR -S $REPO_ROOT" >&2
    exit 1
fi

cd "$BUILD_DIR"
exec ctest -C Debug "$@"