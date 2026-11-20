# Reference TURN deployment (TLS-only)

This directory carries the reference TURN deployment for NimRTC's
network tests (slice `v1-n5-turn-server`, plan N5).  It runs **coturn**
— the reference TURN implementation — configured to serve **only**
TURN over TLS (RFC 6062) on port `5349`, so a TURN/TLS test cannot pass
by falling back to a plaintext control connection.

Two ways to bring it up:

| Path | Who | File |
|---|---|---|
| Native install | the Linux CI leg (N5-2) | `install-coturn.sh` |
| Container | developers / archived evidence (N5-1) | `compose.yml` |

Both paths share the same `turnserver.conf` template and the same
self-signed certificate generator (`gen-certs.sh`).

## Native install (the CI default)

On a Debian/Ubuntu Linux runner with root:

```bash
bash examples/turn-server/install-coturn.sh
```

The script:

1. installs coturn and openssl from the distribution packages
   (`apt-get install coturn`, non-interactive);
2. generates a self-signed certificate with `openssl req` into
   `/etc/turn-server/certs/`;
3. writes `/etc/turn-server/turnserver.conf` from the template;
4. creates a long-term credential (RFC 5766 §8);
5. starts coturn in **TLS-only** mode and prints the access line:

```
NIMRTC_TURN_URI=turns:127.0.0.1:5349?transport=tcp
NIMRTC_TURN_USERNAME=turnuser
NIMRTC_TURN_PASSWORD=<hex>
```

Export those three variables and run the acceptance test:

```bash
node --test tests/acceptance/v1-n5-turn-server/acceptance.test.cjs
```

The acceptance test only reads the URI and the credential.  It does not
install, start, or stop coturn, and it does not know (or care) which of
the two paths brought the server up.

## Developer / archived-evidence path (container)

```bash
bash examples/turn-server/gen-certs.sh   # once, into ./certs/
docker compose -f examples/turn-server/compose.yml up
```

The container uses the `coturn/coturn:4.6` image, mounts the same
`turnserver.conf` and the generated `./certs/`, and refuses UDP/TCP/DTLS.
Default developer credential: user `turnuser`, password `turnpass`
(pass both to the acceptance test through
`NIMRTC_TURN_USERNAME`/`NIMRTC_TURN_PASSWORD`).

## Certificates

`gen-certs.sh` produces a self-signed RSA-2048 leaf carrying the two
reference identities the NimRTC ICE client verifies (ADR-027):

* dNSName SAN `turn.nimrtc.local`
* iPAddress SAN `127.0.0.1`

Because the certificate is self-signed, any TURN client that talks to
this server must **explicitly skip certificate verification** (or trust
the CA).  The acceptance test does this with
`tls.connect({ rejectUnauthorized: false })`; the NimRTC ICE client
does it with `turn_tls_verify_peer = false` (see ADR-027 Decision 4).

## TLS-only and `--tls-only` / `--require-tls`

The deployment is TLS-only by construction: the config disables UDP,
TCP and DTLS and opens only the TLS listener.  `install-coturn.sh` also
passes `--tls-only` on the command line when the installed coturn build
recognizes the flag, and otherwise falls back to the equivalent
`--no-udp --no-tcp --no-dtls` combination.  Parameter naming differs
across coturn versions — if a deployment switches to `--require-tls`,
verify the installed coturn version first (the slice spec's risk note
calls this out; `--require-tls` on a build that does not support it
will refuse to start).

## Layout

| File | Purpose |
|---|---|
| `install-coturn.sh` | native install + TLS-only start + prints the TURN URI/credential |
| `gen-certs.sh` | `openssl req` self-signed certificate generator |
| `turnserver.conf` | shared coturn config template (TLS listener, cert paths, credential) |
| `compose.yml` | developer / archived-evidence container path |
| `README.md` | this deployment document |
