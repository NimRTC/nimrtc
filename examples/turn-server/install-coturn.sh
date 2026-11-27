#!/usr/bin/env bash
#
# Native coturn install for the NimRTC reference TURN deployment
# (slice v1-n5-turn-server / N5-2: the default CI shape on the Linux
# leg).
#
# What it does:
#   * installs coturn + openssl from the distribution packages
#   * generates a self-signed TLS certificate (gen-certs.sh)
#   * writes /etc/turn-server/turnserver.conf from the template
#   * creates a long-term credential
#   * starts coturn in TLS-only mode
#   * prints the TURN URI and the credential
#
# The acceptance test never runs this script.  It only consumes the
# printed URI and the credential from the environment variables
# NIMRTC_TURN_URI / NIMRTC_TURN_USERNAME / NIMRTC_TURN_PASSWORD — it
# does not care how the server was brought up.
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TURN_HOST="${TURN_HOST:-127.0.0.1}"
TURN_PORT="${TURN_PORT:-5349}"
TURN_REALM="${TURN_REALM:-turn.nimrtc.local}"
TURN_USER="${TURN_USER:-turnuser}"
TURN_PASSWORD="${TURN_PASSWORD:-$(openssl rand -hex 16)}"
TURN_CERT_DIR="${TURN_CERT_DIR:-/etc/turn-server/certs}"
TURN_CONF="${TURN_CONF:-/etc/turn-server/turnserver.conf}"

# N5 验收标准 1：Linux 原生安装 coturn（CI 默认形态）。
apt-get update -y
apt-get install -y coturn openssl

# N5 验收标准 2：生成自签证书（openssl req，TURN 的 TLS 监听使用）。
TURN_CERT_DIR="${TURN_CERT_DIR}" TURN_REALM="${TURN_REALM}" bash "${SCRIPT_DIR}/gen-certs.sh"

# Write the server config from the template.  The cert paths, realm and
# credential are pinned into the config that turnserver actually reads.
mkdir -p "$(dirname "${TURN_CONF}")"
sed -e "s|^realm=.*|realm=${TURN_REALM}|" \
    -e "s|^user=.*|user=${TURN_USER}:${TURN_PASSWORD}|" \
    -e "s|^cert=.*|cert=${TURN_CERT_DIR}/turn-server-cert.pem|" \
    -e "s|^pkey=.*|pkey=${TURN_CERT_DIR}/turn-server-key.pem|" \
    "${SCRIPT_DIR}/turnserver.conf" > "${TURN_CONF}"

# N5 验收标准 3：TLS-only 启动开关。
#
# coturn 版本间该参数名有差异：较新版本识别 --tls-only；若不识别，
# 回退到等价的 --no-udp --no-tcp --no-dtls 组合（只监听 TLS）。
# 若改用 --require-tls 需核对 coturn 版本 —— 见切片规格风险节。
if turnserver --help 2>&1 | grep -q -- '--tls-only'; then
  TLS_ONLY_ARGS=(--tls-only)
else
  TLS_ONLY_ARGS=(--no-udp --no-tcp --no-dtls)
fi

# Start the TLS-only TURN server in the foreground.
turnserver -c "${TURN_CONF}" "${TLS_ONLY_ARGS[@]}" &
TURNSERVER_PID=$!
echo "turnserver started (pid ${TURNSERVER_PID})"
echo
echo "NIMRTC_TURN_URI=turns:${TURN_HOST}:${TURN_PORT}?transport=tcp"
echo "NIMRTC_TURN_USERNAME=${TURN_USER}"
echo "NIMRTC_TURN_PASSWORD=${TURN_PASSWORD}"
echo
echo "The acceptance test (tests/acceptance/v1-n5-turn-server/) reads"
echo "exactly these three variables; it does not install, start or stop"
echo "coturn itself."
