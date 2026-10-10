#!/usr/bin/env bash
# =============================================================================
# record_loopback_demo.sh — record a REAL asciinema session of loopback-p2p
#
# Replaces the synthesized asciicast in examples/wasm-demo/www/loopback-p2p.cast
# with a real terminal capture. Requires:
#   1. asciinema (`brew install asciinema` / `apt install asciinema` / pipx)
#   2. A built `loopback-p2p` binary
#   3. CMake ≥ 3.25 + GCC/Clang/MSVC + Ninja
#
# Usage:
#   bash tools/record_loopback_demo.sh                    # auto-detect build
#   bash tools/record_loopback_demo.sh build/examples/Debug/loopback-p2p
#   LOOPBACK_BIN=path/to/binary bash tools/record_loopback_demo.sh
#
# Output: examples/wasm-demo/www/loopback-p2p.cast (overwritten in place)
#         Then `git diff examples/wasm-demo/www/loopback-p2p.cast` to review,
#         and commit + push to trigger the wasm-demo-pages workflow.
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CAST_PATH="${REPO_ROOT}/examples/wasm-demo/www/loopback-p2p.cast"
LOOPBACK_BIN="${LOOPBACK_BIN:-${1:-}}"

log()  { printf '\033[1;34m[record]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[record]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[record]\033[0m %s\n' "$*" >&2; exit 1; }

command -v asciinema >/dev/null 2>&1 \
  || die "asciinema not found. Install: brew install asciinema | apt install asciinema | pipx install asciinema"

# --- locate the binary ---------------------------------------------------------
if [[ -z "${LOOPBACK_BIN}" ]]; then
  log "No LOOPBACK_BIN given; looking for a built loopback-p2p under build/..."
  CANDIDATE="$(find "${REPO_ROOT}/build" -type f \( -name loopback-p2p -o -name loopback-p2p.exe \) \
                -executable 2>/dev/null | head -n1 || true)"
  if [[ -z "${CANDIDATE}" ]]; then
    die "loopback-p2p binary not found under build/.
Hint:  cmake --preset debug && cmake --build build --target loopback-p2p
       then re-run this script with: $0 build/examples/Debug/loopback-p2p"
  fi
  LOOPBACK_BIN="${CANDIDATE}"
fi
[[ -x "${LOOPBACK_BIN}" ]] || die "binary not executable: ${LOOPBACK_BIN}"
log "binary: ${LOOPBACK_BIN}"

# --- smoke test: 2s run to make sure the binary works before recording ---------
log "smoke test (2s)..."
"${LOOPBACK_BIN}" 2 >/dev/null && rc=0 || rc=$?
if [[ ${rc} -ne 0 ]]; then
  warn "smoke test exited ${rc}. The recording may not show 'connected'."
  warn "Press Ctrl-C to abort, or any key to continue anyway."
  read -r _
fi

# --- record --------------------------------------------------------------------
BACKUP="${CAST_PATH}.synthesized.bak"
if [[ -f "${CAST_PATH}" ]]; then
  cp "${CAST_PATH}" "${BACKUP}"
  log "backed up current cast → ${BACKUP}"
fi

log "recording to ${CAST_PATH}"
log "press Ctrl-C after the loopback example finishes (typically ~8s)."

# asciinema rec flags:
#   -q, --quiet         : don't print info / replay hints
#   -c, --command       : command to record
#   --title             : shown in the player header
#   --idle-time-limit   : cap idle gaps so the cast doesn't drag
#   --cols / --rows     : terminal size (matches our synthesized cast)
asciinema rec \
  --quiet \
  --title "NimRTC loopback-p2p" \
  --cols 132 --rows 30 \
  --idle-time-limit 2 \
  --command "${LOOPBACK_BIN}" \
  "${CAST_PATH}"

log "recorded. file: ${CAST_PATH}"
log "verify locally:  python -m http.server -d examples/wasm-demo/www"
log "                 open http://127.0.0.1:8000/"
log "then:  git add ${CAST_PATH} && git commit && git push"
