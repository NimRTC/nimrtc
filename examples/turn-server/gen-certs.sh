#!/usr/bin/env bash
#
# Generate a self-signed certificate for the coturn TLS listener
# (RFC 6062 TURN over TLS, conventional port 5349).
#
# The certificate carries the two reference identities the NimRTC ICE
# TURN-over-TLS client verifies (see docs/adr/ADR-027):
#   * dNSName SAN   "turn.nimrtc.local"
#   * iPAddress SAN "127.0.0.1"
# It is a self-signed RSA-2048 leaf, so a client must be told to skip
# certificate verification (the N5 acceptance client does exactly that;
# see the slice spec's risk note about untrusted certificates).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CERT_DIR="${TURN_CERT_DIR:-${SCRIPT_DIR}/certs}"
REALM="${TURN_REALM:-turn.nimrtc.local}"

mkdir -p "${CERT_DIR}"

CERT="${CERT_DIR}/turn-server-cert.pem"
KEY="${CERT_DIR}/turn-server-key.pem"

# openssl req 自签证书 —— N5 验收标准 2（自签证书生成命令）。
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout "${KEY}" \
  -out "${CERT}" \
  -days 365 \
  -subj "/CN=${REALM}" \
  -addext "subjectAltName=DNS:${REALM},IP:127.0.0.1" \
  -addext "basicConstraints=critical,CA:FALSE" \
  -addext "keyUsage=digitalSignature,keyEncipherment" \
  -addext "extendedKeyUsage=serverAuth" \
  >/dev/null 2>&1

chmod 600 "${KEY}"
chmod 644 "${CERT}"
printf 'Generated %s\n         %s\n' "${CERT}" "${KEY}"
