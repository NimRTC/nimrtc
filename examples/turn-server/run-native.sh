#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# examples/turn-server/run-native.sh
# ---------------------------------------------------------------------------
#
# Starts the reference TURN server **without Docker**, on a machine that
# has coturn installed.  This is the path CI's Linux leg uses (v1.4 plan
# §5 **N5-2** -- the hosted runners have no Docker daemon), and it is
# also how the N1 acceptance run is reproduced by hand.
#
#   ./run-native.sh                      # this directory
#   ./run-native.sh /path/to/turn-server # an explicit root
#
# It writes the substituted configuration to a temporary file and starts
# turnserver with it.  The substitution is the reason this script
# exists: coturn does **not** expand environment variables in its config
# file, so the paths in `turnserver.wsl.conf` are placeholders that
# something has to fill in.
#
# Which config it uses: `turnserver.wsl.conf` when the root is a WSL
# DrvFs path (the container paths do not apply there), and
# `turnserver.conf` otherwise -- with the container's absolute paths
# rewritten to local ones, which is what §2 of the README describes.
# ---------------------------------------------------------------------------

set -euo pipefail

cd "$(dirname "$0")"

root="${1:-$PWD}"

# The WSL mount points at a Windows drive (/mnt/...).  The container
# paths do not exist there, so the WSL variant is the right one.
if [[ "$root" == /mnt/* ]]; then
    template=turnserver.wsl.conf
else
    template=turnserver.conf
fi

if [[ ! -f "$root/secrets/tls/turn.pem" ]]; then
    echo "run-native.sh: no certificate at $root/secrets/tls/turn.pem" >&2
    echo "  Run ./gen-certs.sh first." >&2
    exit 1
fi

conf="$(mktemp -t nimrtc-turn-XXXXXX.conf)"
trap 'rm -f "$conf"' EXIT

# The two substitutions:
#   @TURN_ROOT@    the directory holding secrets/
#   the container's absolute paths (/etc/coturn/...)  the local ones
#
# The second one is done with sed rather than a placeholder, because
# `turnserver.conf` is also the file the container reads and must keep
# its container paths.
sed -e "s|@TURN_ROOT@|$root|g" \
    -e "s|/etc/coturn|$root/secrets|g" \
    "$template" > "$conf"

if grep -q '@TURN_ROOT@' "$conf"; then
    echo "run-native.sh: the placeholder survived the substitution" >&2
    exit 1
fi

echo "run-native.sh: config $conf"
echo "run-native.sh: log    $root/secrets/logs/turnserver.log"

exec turnserver -c "$conf"
