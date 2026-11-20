#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# examples/turn-server/gen-certs.sh
# ---------------------------------------------------------------------------
#
# Generates the TLS material the reference TURN server presents, into
# ./secrets/:
#
#   secrets/tls/turn.key      the server's private key (RSA 2048)
#   secrets/tls/turn.pem      the *certificate chain* coturn reads:
#                             the server certificate, then the CA
#   secrets/ca/ca.pem         the CA that signed it — this is what a
#                             verifying client pins, or installs into its
#                             trust store
#
# and drops a `secrets/creds.txt` with the static credential the tests
# use, so that the credential has exactly one source.
#
# Why self-signed, and why a real CA file on top: a client that verifies
# the TURN control stream needs *something* to verify against.  A
# self-signed leaf certificate is the wrong shape for that — the leaf is
# its own trust anchor, and pinning it means re-pinning whenever it is
# regenerated.  A one-off CA makes the pin stable: the client trusts
# `ca.pem`, and the certificate may rotate underneath it.
#
# The certificate is valid for:
#
#   DNS: turn.nimrtc.local   the name the tests use (SNI, and what
#                            TurnServer::tls_server_name carries)
#   DNS: localhost
#   IP:  127.0.0.1           so a client that connects by address
#                            verifies too
#
# Idempotent: an existing key and certificate are left alone unless
# --force is passed.  Regenerating them rotates the trust anchor, which
# breaks every client pinned to the old one — so it is an explicit act.
#
# Cross-platform note: this runs under Git Bash on Windows, and under
# bash on macOS / Linux.  It needs `openssl` on PATH.  There is no
# PowerShell twin on purpose — one script, one behaviour.
# ---------------------------------------------------------------------------

set -euo pipefail

cd "$(dirname "$0")"

force=0
for arg in "$@"; do
    case "$arg" in
        --force|-f) force=1 ;;
        -h|--help)
            sed -n '2,45p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "gen-certs.sh: unknown argument: $arg" >&2
            exit 2
            ;;
    esac
done

if ! command -v openssl >/dev/null 2>&1; then
    echo "gen-certs.sh: openssl not found on PATH" >&2
    exit 1
fi

key=secrets/tls/turn.key
cert=secrets/tls/turn.pem
cacert=secrets/ca/ca.pem
cakey=secrets/ca/ca.key

if [[ -s "$cert" && -s "$key" && -s "$cacert" && $force -eq 0 ]]; then
    echo "gen-certs.sh: $cert already exists (use --force to rotate)"
    exit 0
fi

mkdir -p secrets/tls secrets/ca

echo "gen-certs.sh: generating a one-off CA"
openssl genrsa -out "$cakey" 2048 2>/dev/null
openssl req -x509 -new -nodes -key "$cakey" -sha256 -days 3650 \
    -subj "/CN=NimRTC test CA/O=nimrtc.local" \
    -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -out "$cacert" 2>/dev/null

echo "gen-certs.sh: generating the server key and request"
openssl genrsa -out "$key" 2048 2>/dev/null
openssl req -new -key "$key" \
    -subj "/CN=turn.nimrtc.local/O=nimrtc.local" \
    -out secrets/tls/turn.csr 2>/dev/null

# The SAN is what a client actually verifies against; CN alone has been
# ignored by every sane TLS stack for years.
cat > secrets/tls/turn.ext <<'EOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:turn.nimrtc.local,DNS:localhost,IP:127.0.0.1,IP:::1
EOF

echo "gen-certs.sh: signing the server certificate"
openssl x509 -req -in secrets/tls/turn.csr \
    -CA "$cacert" -CAkey "$cakey" -CAcreateserial \
    -days 825 -sha256 \
    -extfile secrets/tls/turn.ext \
    -out secrets/tls/turn-leaf.pem 2>/dev/null

# coturn reads `cert` as the full chain: leaf first, then the issuer.
cat secrets/tls/turn-leaf.pem "$cacert" > "$cert"

# The CSR and the extension file have done their job.
rm -f secrets/tls/turn.csr secrets/tls/turn.ext

# The credential has one source: turnserver.conf has the `user=` line,
# and this file is what the tests and the sample read it from.
cat > secrets/creds.txt <<'EOF'
# The static long-term credential the reference server accepts.
# Mirrors the `user=` line in turnserver.conf.  Test-only.
TURN_REALM=nimrtc.local
TURN_USERNAME=nimrtc
TURN_PASSWORD=nimrtc-test-secret
EOF

mkdir -p secrets/logs

echo "gen-certs.sh: done"
echo "  server chain : $cert"
echo "  server key   : $key"
echo "  client trust : $cacert"
echo "  credential   : secrets/creds.txt"
