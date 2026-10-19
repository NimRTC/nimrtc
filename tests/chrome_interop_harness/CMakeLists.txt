# tests/chrome_interop_harness/CMakeLists.txt
# ==========================================
#
# Minimal CMake config for the Chrome ↔ NimRTC DTLS interop harness.
#
# This directory does NOT produce any compiled binaries — the test is a
# Python orchestrator that launches the existing demo-p2p binary.
#
# The CMakeLists.txt exists only so the directory is accepted by
# add_subdirectory() if a parent CMake file ever wants to enumerate
# test directories.  It declares no targets.
#
# To run the harness standalone:
#   python tests/chrome_interop_harness/run_harness.py
#
# To run via the e2e acceptance runner:
#   python tools/run_e2e_acceptance.py --case=e

cmake_minimum_required(VERSION 3.20)
project(chrome_interop_harness)

# No targets — this directory is Python-only.
# Document the required dependencies so CMake-based CI can verify them:
#
#   Python >= 3.8
#   playwright (pip install playwright && python -m playwright install chromium)
#   websockets  (pip install websockets)
#   Google Chrome (installed at --chrome-exe path)
#
# The harness is self-contained and reads its dependencies from the
# environment (NIMRTC_BUILD_DIR, NIMRTC_CHROME_EXE).
