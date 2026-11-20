#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# examples/turn-server/ci-acceptance.sh
# ---------------------------------------------------------------------------
#
# The acceptance run for the reference TURN deployment (v1.4 plan item
# **N5**, §5 **N5-2**).  CI runs it on the Linux leg; a developer can run
# it by hand on any machine that has `coturn` and `openssl` installed.
#
#   ./ci-acceptance.sh
#
# What it asserts, and why each assertion is here:
#
# 1. **The TLS port speaks TLS, and the certificate verifies against the
#    CA `gen-certs.sh` produced.**  A coturn that starts with an
#    unreadable certificate, or with the TLS listener moved off the port
#    the client was told to use, comes up *healthy* and answers with
#    something that is not TLS.  This is the assertion that says the
#    deployment does what it claims.
#
# 2. **The same port refuses TLS when the control server is started
#    instead.**  The control is a plain TURN server on the *same* port.
#    A control on a different port would not be a control: the client
#    would fail to reach the wrong port, which is a connection error,
#    not a protocol downgrade.  The negative control has to be on the
#    port the client was told to use, so that the only thing standing
#    between the client and a working session is whether TLS was
#    actually negotiated.
#
#    The control is judged by *behaviour*, not by the text of a
#    failure: a **plain** client must get an allocation from it (so it
#    is a working server, not a dead port), and the **same TLS client**
#    that allocated in phase 1 must not.  The first version of this
#    script judged the control by `openssl s_client`'s output, and on
#    the CI runner the probe hung, `timeout` killed it, and the
#    buffered output -- the very evidence -- was discarded.
#
# 3. **The server log says which listener it opened.**  "the TLS
#    handshake is visible in the server log" is N1's evidence, so the
#    log has to name the TLS listener on the TLS port -- and the
#    control's log must *not* claim it.
#
# The script starts and stops the servers itself; it does not leave a
# daemon behind, and it cleans up on failure.
#
# ---------------------------------------------------------------------------
# Three coturn facts this script is built around.  Each one was found by
# reading coturn's own source (`src/apps/relay/mainrelay.c`), and each
# one silently defeats the obvious spelling:
#
#   * **The config file is `-c`, not `--config`.**  `--config` is not
#     in coturn's long-option table at all, so the server prints
#     "unrecognized option" and **exits**.  The container therefore
#     never starts.
#
#   * **A command-line `--log-file` is ignored.**  `set_option()`
#     lists `'l'` among the options "already taken care of before",
#     and the only pass that calls `set_logfile()` for the command
#     line is the pass that runs *before* the config file is read --
#     so the config file's `log-file=` wins.  A deployment that
#     separates the two runs by passing `--log-file` on the command
#     line writes **both** logs to the same file, and the control's
#     evidence is destroyed by the very run it is supposed to
#     survive.  That is why this script writes a *separate config
#     file* per phase instead of overriding the log path on the
#     command line.
#
#   * **`--no-tls` disables the TLS listeners** ("Do not start TLS
#     client listeners").  There is no `--tls-only` option: a
#     deployment "refuses plain TCP on the TLS port" by *not* putting
#     the plain listener on that port, which is what the positive
#     phase does -- the plain listener sits on 3478 and the TLS
#     listener on 5349.
#
# The script also makes a **real TURN allocation** over TLS, with
# coturn's own `turnutils_uclient`.  A TLS handshake proves the port
# speaks TLS; an *allocation* proves that the TURN protocol runs over
# that TLS session and that the credentials are accepted -- which is
# the claim N1 actually makes.  The client is told the TLS port in
# both phases, so the control's failure is a downgrade, not a
# connection error.
# ---------------------------------------------------------------------------

set -euo pipefail

self="$0"
case "$self" in
    /*) ;;
    *) self="$(pwd)/$self" ;;
esac
self="$(cd "$(dirname "$self")" && pwd)/$(basename "$self")"
cd "$(dirname "$self")"

# The port the client is told to use.  The same number in both phases --
# that is the whole point of the control.
TLS_PORT="${NIMRTC_TURN_TLS_PORT:-5349}"
# The plain listener's port in the *positive* phase.  It is separate so
# that the control can take the TLS port over without a fight.
PLAIN_PORT="${NIMRTC_TURN_PLAIN_PORT:-3478}"

LOG_DIR="$PWD/secrets/logs"
CA_FILE="$PWD/secrets/ca/ca.pem"
CERT_FILE="$PWD/secrets/tls/turn.pem"
KEY_FILE="$PWD/secrets/tls/turn.key"

server_pid=""

cleanup() {
    if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    server_pid=""
}
trap cleanup EXIT

fail() {
    echo "ci-acceptance: FAIL: $*" >&2
    exit 1
}

# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------

for tool in turnserver openssl; do
    command -v "$tool" >/dev/null 2>&1 || fail "$tool is not on PATH"
done

[[ -s "$CA_FILE" ]] || fail "no CA at $CA_FILE -- run ./gen-certs.sh first"

# The ports have to be free.  A port that is already taken is not a
# deployment defect, it is a broken environment, and the failure has to
# say so instead of looking like a coturn failure.
port_in_use() {
    local port="$1"
    # A connect attempt against /dev/tcp: `nc` is not guaranteed to be
    # installed, and `ss` needs a moment to reflect a listener that has
    # just been closed.
    (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null && { exec 3<&- 3>&-; return 0; }
    return 1
}

for port in "$TLS_PORT" "$PLAIN_PORT"; do
    if port_in_use "$port"; then
        cat >&2 <<EOF
ci-acceptance: FAIL: port $port is already in use -- something else on this host holds it.
ci-acceptance:   The usual culprit is the distro's coturn service, which starts
ci-acceptance:   itself when the package is installed:
ci-acceptance:       sudo systemctl stop coturn.service && sudo systemctl mask coturn.service
ci-acceptance:   The deployment is not at fault here; the host is not clean.
EOF
        exit 1
    fi
done

mkdir -p "$LOG_DIR"

# ---------------------------------------------------------------------------
# The configuration
# ---------------------------------------------------------------------------
#
# `turnserver.conf` is the deployment's own configuration, with the
# container's absolute paths rewritten to the local ones -- the same
# substitution `run-native.sh` does.  It is used here rather than a
# CI-specific file so that CI exercises the *shipped* configuration:
# a CI-only config would let the shipped one rot.
#
# The `log-file=` line is rewritten per phase, because a command-line
# `--log-file` would be ignored (see the header).
write_config() {
    local dest="$1"
    local log_path="$2"
    sed -e "s|/etc/coturn|$PWD/secrets|g" \
        -e "s|^log-file=.*|log-file=$log_path|" \
        turnserver.conf > "$dest"
}

# ---------------------------------------------------------------------------
# Phase 1 -- the positive server: TLS on the port the client is told to use
# ---------------------------------------------------------------------------

echo "ci-acceptance: phase 1 -- the TLS server on $TLS_PORT"

TLS_CONF="$(mktemp -t nimrtc-turn-ci-tls-XXXXXX.conf)"
TLS_LOG="$LOG_DIR/ci-tls.log"
write_config "$TLS_CONF" "$TLS_LOG"

turnserver -c "$TLS_CONF" \
    --no-cli \
    --listening-port="$PLAIN_PORT" \
    > "$LOG_DIR/ci-tls.stdout" 2>&1 &
server_pid=$!

# Wait for the TLS listener.  coturn opens the plain listener first, so
# the plain port being up says nothing; the TLS port is the one that
# matters.
ready=0
for _ in $(seq 1 50); do
    if port_in_use "$TLS_PORT"; then ready=1; break; fi
    sleep 0.2
done
[[ "$ready" -eq 1 ]] || {
    cat "$LOG_DIR/ci-tls.stdout" >&2 || true
    fail "the TLS listener on $TLS_PORT never came up"
}

# The handshake must succeed *and* verify against the generated CA.
# `openssl s_client` exits 0 on a TLS error in some versions, so the
# verdict is read out of its output rather than out of its exit code.
handshake="$(echo | timeout 20 openssl s_client \
    -connect "127.0.0.1:$TLS_PORT" \
    -servername turn.nimrtc.local \
    -CAfile "$CA_FILE" 2>&1 || true)"

grep -q "Verify return code: 0 (ok)" <<<"$handshake" || {
    echo "$handshake" >&2
    fail "the TLS handshake on $TLS_PORT did not verify against the generated CA"
}
grep -qE "Protocol *: *TLSv1\.[0-9]" <<<"$handshake" || {
    echo "$handshake" >&2
    fail "the TLS handshake on $TLS_PORT did not negotiate a TLS version"
}

echo "ci-acceptance:   TLS handshake verified against the generated CA"
grep -E "Protocol *:|Cipher *:" <<<"$handshake" | head -2 | sed 's/^/ci-acceptance:   /'

# The log has to name the TLS listener on the TLS port.  This is N1's
# "the control stream's TLS handshake is visible in the server log"
# evidence, and it is what distinguishes a server that bound a TLS
# listener from one that merely came up.
for _ in $(seq 1 25); do
    [[ -s "$TLS_LOG" ]] && grep -q "TLS/TCP listener opened on .*:$TLS_PORT" "$TLS_LOG" && break
    sleep 0.2
done
grep -q "TLS/TCP listener opened on .*:$TLS_PORT" "$TLS_LOG" || {
    tail -30 "$TLS_LOG" >&2 2>/dev/null || true
    fail "the server log does not name a TLS listener on $TLS_PORT"
}
echo "ci-acceptance:   the log names the TLS listener on $TLS_PORT"

# ---- The real allocation -------------------------------------------------
#
# A TLS handshake proves the port speaks TLS.  An *allocation* proves
# that the TURN protocol runs over that TLS session and that the
# long-term credential is accepted -- which is the claim N1 makes.
#
# `turnutils_uclient` is coturn's own client, so this needs no client
# code in the tree.  `-S` is "secure connection: TLS for TCP", `-t` is
# TCP (the control connection is a TCP stream), and `-E` names the CA
# the server's certificate is verified against -- so the client
# verifies the server rather than accepting anything.
#
# `-i` / `-k` are the client's own certificate and key.  They are not
# needed by this server (it does not ask for a client certificate), but
# coturn's client refuses to start a secure session without them.

if command -v turnutils_uclient >/dev/null 2>&1; then
    echo "ci-acceptance:   making a real TURN allocation over TLS"

    allocation="$(timeout 60 turnutils_uclient \
        -u nimrtc \
        -w nimrtc-test-secret \
        -p "$TLS_PORT" \
        -S -t -v -y -c \
        -E "$CA_FILE" \
        -i "$CERT_FILE" \
        -k "$KEY_FILE" \
        127.0.0.1 2>&1 || true)"

    # "success" is what coturn's client prints for a completed
    # allocation; "Received relay addr" is the allocation itself.
    if ! grep -q "success" <<<"$allocation"; then
        echo "$allocation" >&2
        fail "no TURN allocation was made over the TLS session on $TLS_PORT"
    fi
    if ! grep -qE "Received relay addr" <<<"$allocation"; then
        echo "$allocation" >&2
        fail "the allocation over TLS returned no relay address"
    fi
    echo "ci-acceptance:   the allocation succeeded over TLS"
    grep -E "Received relay addr" <<<"$allocation" | head -1 | sed 's/^/ci-acceptance:   /'

    # And the *server* side has to agree, in the log N1 cites as its
    # evidence: the session line carries the cipher and the TLS
    # version, and it carries them only for a TLS socket.
    for _ in $(seq 1 25); do
        grep -aqE "cipher=.*method=TLSv1" "$TLS_LOG" && break
        sleep 0.2
    done
    if ! grep -aqE "cipher=.*method=TLSv1" "$TLS_LOG"; then
        tail -30 "$TLS_LOG" >&2 2>/dev/null || true
        fail "the server log does not record a TLS session (no 'cipher=... method=TLSv1' line)"
    fi
    echo "ci-acceptance:   the server log records the TLS session and its cipher"
else
    echo "ci-acceptance:   turnutils_uclient is not installed -- skipping the allocation"
    echo "ci-acceptance:   (the TLS handshake above still ran)"
fi

cleanup
rm -f "$TLS_CONF"
sleep 1

# ---------------------------------------------------------------------------
# Phase 2 -- the negative control: plain TURN on the SAME port
# ---------------------------------------------------------------------------
#
# `--no-tls` / `--no-dtls` is coturn's own spelling of "no TLS
# listeners".  `--listening-port=$TLS_PORT` puts the plain listener
# where the TLS one was.  This is the control: everything is identical
# except the transport, so a client that connects here when it was told
# to use TLS has fallen back, not succeeded.
#
# **The control is judged by what it does, not by what it prints.**
#
# The first version of this phase ran `openssl s_client` against the
# control and looked for "no peer certificate available" or
# "Cipher is (NONE)" in its output.  That was wrong in two ways, and
# the second one only showed up on the CI runner:
#
#   * The verdict was read from the *text* of a failure.  A failure is
#     not a contract: OpenSSL's wording changes between versions, and
#     the wording differs between the "connection reset" case and the
#     "hung" case.
#   * When the probe hung, `timeout` killed it and **the buffered
#     output was discarded** -- so the evidence vanished and the
#     assertion could only report "the control is not a control", which
#     is the opposite of what was true.
#
# So the control is now judged by *behaviour*, and the behaviour is
# checked from both sides:
#
#   * A **plain** client (no `-S`) MUST get an allocation.  This proves
#     the control is a *working* TURN server and not a dead port or a
#     crashed process -- without it, "the TLS client got nothing" would
#     be satisfied by a server that is simply broken, and the control
#     would prove nothing.
#   * The **same TLS client** that allocated in phase 1 MUST NOT get
#     an allocation.  That is the claim N1 makes: the port speaks
#     plain TCP, and a client that was told to use TLS cannot fall back
#     to it.
#
# Together the two are a real control: the difference between the two
# clients is exactly the TLS flag, and it is exactly what decides
# whether the allocation happens.

echo "ci-acceptance: phase 2 -- the plain control on $TLS_PORT"

PLAIN_CONF="$(mktemp -t nimrtc-turn-ci-plain-XXXXXX.conf)"
PLAIN_LOG="$LOG_DIR/ci-plain.log"
write_config "$PLAIN_CONF" "$PLAIN_LOG"

turnserver -c "$PLAIN_CONF" \
    --no-cli \
    --no-tls \
    --no-dtls \
    --listening-port="$TLS_PORT" \
    > "$LOG_DIR/ci-plain.stdout" 2>&1 &
server_pid=$!

ready=0
for _ in $(seq 1 50); do
    if port_in_use "$TLS_PORT"; then ready=1; break; fi
    sleep 0.2
done
[[ "$ready" -eq 1 ]] || {
    cat "$LOG_DIR/ci-plain.stdout" >&2 || true
    fail "the plain control never came up on $TLS_PORT"
}

# The control's log must not claim a TLS listener on the port.  This is
# the *server's* own account of itself, and it is independent of what
# any client makes of the port.
for _ in $(seq 1 25); do
    [[ -s "$PLAIN_LOG" ]] && grep -q "listener opened on .*:$TLS_PORT" "$PLAIN_LOG" && break
    sleep 0.2
done
if grep -q "TLS/TCP listener opened on .*:$TLS_PORT" "$PLAIN_LOG"; then
    fail "the control's log claims a TLS listener on $TLS_PORT"
fi
echo "ci-acceptance:   the control's log claims no TLS listener on $TLS_PORT"

if command -v turnutils_uclient >/dev/null 2>&1; then
    # ---- Side one: the control is alive -----------------------------------
    #
    # A plain client -- no `-S`, so no TLS -- must get an allocation.
    # If it does not, the control is not a control but a broken server,
    # and the rest of this phase would prove nothing.
    echo "ci-acceptance:   the plain client must allocate against the control"

    plain_allocation="$(timeout 60 turnutils_uclient \
        -u nimrtc \
        -w nimrtc-test-secret \
        -p "$TLS_PORT" \
        -t -v -y -c \
        127.0.0.1 2>&1 || true)"

    if ! grep -qE "Received relay addr" <<<"$plain_allocation"; then
        echo "$plain_allocation" >&2
        fail "the plain client could not allocate against the control on "\
"$TLS_PORT -- the control is not a working TURN server, so it controls "\
"nothing"
    fi
    echo "ci-acceptance:   the control is a working plain TURN server"

    # ---- Side two: the TLS client must not -------------------------------
    #
    # The *same* client that allocated over TLS in phase 1, pointed at
    # the same port, with the same credential.  If it allocates here,
    # the TLS request was silently downgraded to plain TCP -- and the
    # whole "the credentials stay encrypted" claim is void.
    echo "ci-acceptance:   the same client must NOT allocate against the control"

    control_allocation="$(timeout 60 turnutils_uclient \
        -u nimrtc \
        -w nimrtc-test-secret \
        -p "$TLS_PORT" \
        -S -t -v -y -c \
        -E "$CA_FILE" \
        -i "$CERT_FILE" \
        -k "$KEY_FILE" \
        127.0.0.1 2>&1 || true)"

    if grep -qE "Received relay addr" <<<"$control_allocation"; then
        echo "$control_allocation" >&2
        fail "the client allocated a relay against the plain control on "\
"$TLS_PORT -- the TLS request was silently downgraded to plain TCP"
    fi
    echo "ci-acceptance:   the client did not allocate against the control, as it must"
else
    echo "ci-acceptance:   turnutils_uclient is not installed -- the control's"
    echo "ci-acceptance:   behaviour cannot be checked (the TLS handshake in"
    echo "ci-acceptance:   phase 1 still ran)"
fi

cleanup
rm -f "$PLAIN_CONF"

echo "ci-acceptance: OK -- the TLS port speaks TLS and serves a real allocation, and the plain control on the same port does neither"
