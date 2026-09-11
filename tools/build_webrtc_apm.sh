#!/usr/bin/env bash
#
# Build WebRTC Audio Processing Module (APM) as a static library on Linux
# and macOS. Windows users should use build_webrtc_apm.cmd instead.
#
# What this script does:
#   1. Locates the project root relative to itself (no hard-coded paths).
#   2. Patches meson.build to use C++20 (required by agc2/*.cc).
#   3. Configures with meson (auto-downloads abseil-cpp via wrap).
#   4. Builds with ninja (static library + abseil-cpp).
#
# Output:
#   src/third_party/webrtc_audio_processing/build_meson/
#     webrtc/modules/audio_processing/libwebrtc-audio-processing-2.a
#     subprojects/abseil-cpp-*/libabsl_*.a
#
# Requirements:
#   - Python 3.10+   (meson requires it since 1.12)
#   - Ninja 1.10+
#   - Meson 1.10+
#   - C/C++ compiler: GCC 11+ or Clang 14+ (for C++20 designated initializers)
#
# Usage:
#   tools/build_webrtc_apm.sh                 # default release build
#   tools/build_webrtc_apm.sh --debug         # debug build
#   tools/build_webrtc_apm.sh --clean         # wipe build_meson first
#
# Exit codes:
#   0  success
#   1  prerequisite missing
#   2  meson setup failed
#   3  ninja build failed
#
# This script is the canonical Linux/macOS build entry point; the legacy
# copy at build/build_webrtc_apm.sh is gitignored and will be removed.

set -euo pipefail

# ----------------------------------------------------------------------------
# Resolve project root relative to this script (no hard-coded paths).
# ----------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
VENDOR_DIR="${PROJECT_ROOT}/src/third_party/webrtc_audio_processing"
SRC_DIR="${VENDOR_DIR}/src"
BUILD_DIR="${VENDOR_DIR}/build_meson"
LOG_DIR="${PROJECT_ROOT}/build"

# Mirror: same as the Windows script. PulseAudio actively maintains this
# (last update 2025-11-10). The chromium.googlesource.com repo is the
# canonical source but is often firewalled in CI environments.
MIRROR_URL="${WEBRTC_APM_MIRROR:-https://gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing.git}"
MIRROR_BRANCH="${WEBRTC_APM_BRANCH:-master}"

BUILD_TYPE="release"

# ----------------------------------------------------------------------------
# Parse CLI
# ----------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug)   BUILD_TYPE="debug";;
        --clean)   echo "[INFO] --clean: removing ${BUILD_DIR}"; rm -rf "${BUILD_DIR}";;
        --help|-h)
            sed -n '2,40p' "$0"
            exit 0
            ;;
        *)
            echo "[WARN] Unknown option: $1"
            ;;
    esac
    shift
done

# ----------------------------------------------------------------------------
# Sanity checks
# ----------------------------------------------------------------------------
for tool in python3 ninja meson git; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        case "${tool}" in
            python3) echo "[ERROR] python3 not found in PATH";;
            ninja)   echo "[ERROR] ninja not found. Install: pip install --user ninja";;
            meson)   echo "[ERROR] meson not found. Install: pip install --user meson";;
            git)     echo "[ERROR] git not found";;
        esac
        exit 1
    fi
done

if ! command -v g++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1; then
    echo "[ERROR] No C++ compiler found (need g++ or clang++)."
    exit 1
fi

echo "[INFO] Project:  ${PROJECT_ROOT}"
echo "[INFO] Vendor:   ${VENDOR_DIR}"
echo "[INFO] Build:    ${BUILD_DIR} (${BUILD_TYPE})"
echo "[INFO] Mirror:   ${MIRROR_URL} (branch=${MIRROR_BRANCH})"
echo "[INFO] Python:   $(python3 --version)"
echo "[INFO] Meson:    $(meson --version)"
echo "[INFO] Ninja:    $(ninja --version)"
echo

# ----------------------------------------------------------------------------
# Step 1: source already cloned by tools/fetch_webrtc_apm.py?
# ----------------------------------------------------------------------------
if [[ ! -f "${SRC_DIR}/meson.build" ]]; then
    echo "[INFO] ${SRC_DIR}/meson.build missing."
    echo "       Run tools/fetch_webrtc_apm.py first to clone the source."
    exit 1
fi

# ----------------------------------------------------------------------------
# Step 2: patch meson.build for C++20
# ----------------------------------------------------------------------------
ROOT_MESON="${SRC_DIR}/meson.build"
APM_MESON="${SRC_DIR}/webrtc/modules/audio_processing/meson.build"

if grep -q "cpp_std=c++20" "${ROOT_MESON}" 2>/dev/null; then
    echo "[STEP 2] meson.build already patched for C++20"
else
    echo "[STEP 2] Patching meson.build for C++20..."
    sed -i.bak "s/'cpp_std=c++17'/'cpp_std=c++20'/" "${ROOT_MESON}"
    sed -i.bak "s|cpp_args: common_cxxflags + apm_flags + avx_flags$|cpp_args: common_cxxflags + apm_flags + avx_flags + ['-std=c++20']|" "${APM_MESON}"
    sed -i.bak "s|cpp_args: common_cxxflags + apm_flags,$|cpp_args: common_cxxflags + apm_flags + ['-std=c++20'],\n    override_options: ['cpp_std=c++20'],|" "${APM_MESON}"
    rm -f "${ROOT_MESON}.bak" "${APM_MESON}.bak"
fi

# ----------------------------------------------------------------------------
# Step 3: meson setup
# ----------------------------------------------------------------------------
mkdir -p "${BUILD_DIR}"
echo "[STEP 3] meson setup..."
if ! meson setup "${BUILD_DIR}" "${SRC_DIR}" \
        --buildtype="${BUILD_TYPE}" \
        --default-library=static \
        --wipe 2>&1 | tail -20; then
    echo "[ERROR] meson setup failed"
    exit 2
fi

# ----------------------------------------------------------------------------
# Step 4: ninja build
# ----------------------------------------------------------------------------
mkdir -p "${LOG_DIR}"
echo "[STEP 4] ninja build (logging to ${LOG_DIR}/webrtc_apm_build.log)..."
if ! ninja -C "${BUILD_DIR}" >"${LOG_DIR}/webrtc_apm_build.log" 2>&1; then
    echo "[ERROR] ninja build failed; last 30 lines:"
    tail -30 "${LOG_DIR}/webrtc_apm_build.log"
    exit 3
fi

echo
echo "[OK] WebRTC APM built successfully."
echo "[OK] Library: ${BUILD_DIR}/webrtc/modules/audio_processing/libwebrtc-audio-processing-2.a"
ls -lh "${BUILD_DIR}/webrtc/modules/audio_processing/libwebrtc-audio-processing-2.a"
