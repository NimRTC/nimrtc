#!/usr/bin/env bash
#
# scripts/build.sh
# Interactive build script for NimRTC on Linux / macOS.
# Detects the host toolchain (GCC/Clang), configures CMake, builds,
# and optionally runs tests.
#
# Usage:
#   bash scripts/build.sh              -- configure + build
#   bash scripts/build.sh --configure  -- CMake configure only
#   bash scripts/build.sh --build      -- build only (no configure)
#   bash scripts/build.sh --test       -- run tests only
#   bash scripts/build.sh --rebuild    -- clean and rebuild everything
#   bash scripts/build.sh --preset=debug      -- use debug preset (default)
#   bash scripts/build.sh --preset=release    -- use release preset

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
MODE="full"
PRESET=""
BUILD_TYPE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --configure)  MODE="configure" ;;
        --build)      MODE="build" ;;
        --test)       MODE="test" ;;
        --rebuild)    MODE="rebuild" ;;
        --release)    BUILD_TYPE="Release" ;;
        --debug)      BUILD_TYPE="Debug" ;;
        --preset=*)   PRESET="${1#*=}" ;;
        -h|--help)    MODE="help" ;;
        *)            echo "[WARN] Unknown argument: $1" ;;
    esac
    shift
done

if [[ "${MODE}" == "help" ]]; then
    echo "NimRTC Build Script"
    echo ""
    echo "Usage:"
    echo "  bash scripts/build.sh              -- configure + build"
    echo "  bash scripts/build.sh --configure   -- CMake configure only"
    echo "  bash scripts/build.sh --build       -- build only (no configure)"
    echo "  bash scripts/build.sh --test        -- run tests only"
    echo "  bash scripts/build.sh --rebuild      -- clean and rebuild"
    echo "  bash scripts/build.sh --release     -- Release config"
    echo "  bash scripts/build.sh --debug       -- Debug config (default)"
    echo "  bash scripts/build.sh --preset=name -- use named CMake preset"
    exit 0
fi

# ---------------------------------------------------------------------------
# Detect OS
# ---------------------------------------------------------------------------
OS="$(uname -s)"
case "${OS}" in
    Linux*)  PLATFORM="Linux" ;;
    Darwin*) PLATFORM="macOS" ;;
    *)       echo "[ERROR] Unsupported platform: ${OS}"; exit 1 ;;
esac

# ---------------------------------------------------------------------------
# Step 0: Check prerequisites
# ---------------------------------------------------------------------------
echo ""
echo "============================================================"
echo " NimRTC Build  [${PLATFORM}]"
echo "============================================================"
echo ""

python3 "${PROJECT_ROOT}/tools/check_prerequisites.py" --compiler >/dev/null 2>&1 || true

# ---------------------------------------------------------------------------
# Step 1: Detect compiler and select preset
# ---------------------------------------------------------------------------
if [[ -n "${PRESET}" ]]; then
    SELECTED_PRESET="${PRESET}"
elif [[ -n "${BUILD_TYPE}" ]]; then
    SELECTED_PRESET="dev"
else
    echo "Build preset:"
    echo "  [1] debug    (default, full symbols, slower)"
    echo "  [2] release  (optimised, faster binaries)"
    echo "  [3] asan     (AddressSanitizer, for debugging memory issues)"
    echo "  [4] custom   (enter preset name)"
    echo ""
    read -r -p "Select [1/2/3/4] (default 1): " CHOICE
    CHOICE="${CHOICE:-1}"

    case "${CHOICE}" in
        1) SELECTED_PRESET="debug" ;;
        2) SELECTED_PRESET="release" ;;
        3) SELECTED_PRESET="asan" ;;
        4) read -r -p "Enter preset name (e.g. debug.clang): " SELECTED_PRESET ;;
        *) SELECTED_PRESET="debug" ;;
    esac
fi

echo "[INFO] Using preset: ${SELECTED_PRESET}"

# ---------------------------------------------------------------------------
# Step 2: Rebuild (clean)
# ---------------------------------------------------------------------------
if [[ "${MODE}" == "rebuild" ]]; then
    echo ""
    echo "[INFO] Rebuild requested -- removing build directory..."
    if [[ -d "${PROJECT_ROOT}/build" ]]; then
        rm -rf "${PROJECT_ROOT}/build"
        echo "[OK]  Build directory removed."
    fi
    MODE="configure"
fi

# ---------------------------------------------------------------------------
# Step 3: CMake configure
# ---------------------------------------------------------------------------
if [[ "${MODE}" == "configure" ]] || [[ "${MODE}" == "full" ]]; then
    echo ""
    echo "============================================================"
    echo " Configuring NimRTC (preset: ${SELECTED_PRESET})..."
    echo "============================================================"
    echo ""

    cd "${PROJECT_ROOT}"

    if ! cmake --preset "${SELECTED_PRESET}" >/dev/null 2>&1; then
        echo "[WARN] Preset '${SELECTED_PRESET}' not available."
        echo "       Falling back to direct cmake invocation..."

        # Auto-detect config type from preset name
        if [[ "${SELECTED_PRESET}" == *"release"* ]]; then
            CMAKE_BUILD_TYPE_ARG="-DCMAKE_BUILD_TYPE=Release"
        else
            CMAKE_BUILD_TYPE_ARG="-DCMAKE_BUILD_TYPE=Debug"
        fi

        # Try to detect compiler
        if [[ "${PLATFORM}" == "macOS" ]]; then
            CMAKE_PRESET_BASE="dev.macos.clang"
        else
            CMAKE_PRESET_BASE="dev"
        fi

        cmake -B build -S . -GNinja "${CMAKE_BUILD_TYPE_ARG:-}" || {
            echo ""
            echo "[ERROR] CMake configure failed."
            echo ""
            echo "Common fixes:"
            echo "  1. Run: python tools/check_prerequisites.py"
            echo "  2. Check: cmake --preset list"
            exit 1
        }
    else
        cmake --preset "${SELECTED_PRESET}"
    fi

    echo ""
    echo "[OK]  CMake configure succeeded."
fi

# ---------------------------------------------------------------------------
# Step 4: Build
# ---------------------------------------------------------------------------
if [[ "${MODE}" == "build" ]] || [[ "${MODE}" == "full" ]]; then
    echo ""
    echo "============================================================"
    echo " Building NimRTC..."
    echo "============================================================"
    echo ""

    cd "${PROJECT_ROOT}"

    # Detect multi-config presets (msvc)
    if cmake --preset "${SELECTED_PRESET}" -N >/dev/null 2>&1; then
        CMAKE_PRESET_OUT="$(cmake --preset "${SELECTED_PRESET}" -N 2>&1 || true)"
        if echo "${CMAKE_PRESET_OUT}" | grep -q "Visual Studio\|Ninja Multi-Config"; then
            cmake --build build --preset "${SELECTED_PRESET}" -j || {
                echo ""
                echo "[ERROR] Build failed."
                exit 1
            }
        else
            cmake --build build --preset "${SELECTED_PRESET}" -j || {
                echo ""
                echo "[ERROR] Build failed."
                exit 1
            }
        fi
    else
        cmake --build build -j || {
            echo ""
            echo "[ERROR] Build failed."
            exit 1
        }
    fi

    echo ""
    echo "[OK]  Build succeeded."
fi

# ---------------------------------------------------------------------------
# Step 5: Run tests
# ---------------------------------------------------------------------------
if [[ "${MODE}" == "test" ]] || [[ "${MODE}" == "full" ]]; then
    # For --full, ask; for --test, run automatically
    if [[ "${MODE}" == "full" ]]; then
        echo ""
        read -r -p "Run tests now? [Y/n]: " RUN_TESTS
        RUN_TESTS="${RUN_TESTS:-y}"
        if [[ "${RUN_TESTS}" =~ ^[Nn]$ ]]; then
            echo "[INFO] Skipping tests."
        fi
    fi

    if [[ "${MODE}" == "test" ]] || [[ "${RUN_TESTS:-y}" =~ ^[Yy]$ ]]; then
        echo ""
        echo "============================================================"
        echo " Running tests..."
        echo "============================================================"
        echo ""

        cd "${PROJECT_ROOT}"

        # Find the right test preset
        case "${PLATFORM}" in
            Linux)
                TEST_PRESET="tests"
                [[ "${SELECTED_PRESET}" == *"aarch64"* ]] && TEST_PRESET="tests.aarch64"
                ;;
            macOS) TEST_PRESET="tests.macos" ;;
            *)     TEST_PRESET="tests" ;;
        esac

        if ctest --preset "${TEST_PRESET}" --output-on-failure >/dev/null 2>&1; then
            ctest --preset "${TEST_PRESET}" --output-on-failure || {
                echo ""
                echo "[WARN] Some tests failed."
            }
        else
            echo "[WARN] Test preset '${TEST_PRESET}' not available. Trying direct ctest..."
            ctest --build build --output-on-failure || true
        fi

        echo ""
        echo "[OK]  Tests complete."
    fi
fi

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
echo ""
echo "============================================================"
echo " Done. Build artifacts are in:"
echo "   ${PROJECT_ROOT}/build/"
echo "============================================================"
echo ""
echo "Quick commands:"
echo "  ctest --preset ${TEST_PRESET:-tests}  -- run tests"
echo "  cmake --build build -j              -- rebuild"
echo ""
