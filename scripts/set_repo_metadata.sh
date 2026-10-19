#!/usr/bin/env bash
# set_repo_metadata.sh
# Idempotently applies the canonical NimRTC repo description + topics via
# the GitHub CLI (`gh`). Safe to re-run; `gh repo edit` is a no-op when
# nothing changes.
#
# Prerequisites:
#   - gh (>= 2.0) on PATH
#   - `gh auth login` against github.com with `repo` scope
#
# Usage:
#   bash scripts/set_repo_metadata.sh [OWNER/REPO]
#   # default OWNER/REPO: NimRTC/nimrtc (read from git remote if absent)

set -euo pipefail

TARGET="${1:-}"
if [ -z "${TARGET}" ]; then
    if command -v git >/dev/null 2>&1 && git rev-parse --git-dir >/dev/null 2>&1; then
        REMOTE_URL="$(git config --get remote.origin.url || true)"
        if [ -n "${REMOTE_URL}" ]; then
            TARGET="$(printf '%s\n' "${REMOTE_URL}" \
                | sed -E 's#^.*github.com[:/]([^/]+/[^/.]+)(\.git)?$#\1#')"
        fi
    fi
fi
TARGET="${TARGET:-NimRTC/nimrtc}"

DESCRIPTION="A from-scratch C++20 WebRTC engine. P2P client + relay gateway in one source tree. ~5.6 MB static lib. Chrome / Firefox / libwebrtc wire interop. Pluggable crypto: OpenSSL / wolfSSL / mbedTLS / GMSSL-ready for Xinchuang. 4-platform CI."

TOPICS=(
    webrtc
    cpp20
    realtime-communication
    rtc
    sfu
    p2p
    audio-video
    webrtc-alternative
    xinchuang
    gmssl
)

TOPIC_ARGS=()
for t in "${TOPICS[@]}"; do
    TOPIC_ARGS+=(--add-topic "${t}")
done

echo "Target repo:     ${TARGET}"
echo "Description:     ${DESCRIPTION}"
echo "Topics:          ${TOPICS[*]}"
echo
echo "Applying via: gh repo edit ${TARGET} --description ... --add-topic ..."
gh repo edit "${TARGET}" --description "${DESCRIPTION}" "${TOPIC_ARGS[@]}"

echo
echo "Verifying:"
gh repo view "${TARGET}" --json name,description,repositoryTopics \
    --template '{{.name}} | {{.description}}
topics:
{{- range .repositoryTopics }}
  - {{.name}}
{{- end }}'
