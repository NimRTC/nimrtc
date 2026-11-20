# The reference TURN server

A one-command TURN deployment that **refuses plain TCP**, so a TURN/TLS
test cannot pass by silently falling back.  It is the deployment side of
`v1.4` plan item **N5** (`docs/plan/v1.4-plan.md` §3), and it is the
target N1's positive test and N1's negative control both point at.

This directory is a *deployment*, not a library.  coturn is **not**
vendored into `src/third_party/`, nothing here is compiled into the
library, and the only thing the SDK and this server share is a TURN URI
and a credential.

---

## 1. Why this exists

The v1.4 survey found that `IceTransport::add_turn_server()` accepts a
`TurnTransport::Tls` server, warns, and then connects over **plain TCP**
(`src/modules/ice/src/ice.cpp`; `turn_transport_supported(Tls)` returns
`false`).  Every TURN claim in the tree so far has been asserted against
the ICE agent's own test fixture, so nothing in CI could have caught the
fallback: there was no server that *enforced* TLS.

N5 is that server.  It exists so that N1 has a test whose failure mode is
observable — a client pointed at this server that nevertheless connects
over plain TCP is caught, because this server has no plain TCP to
connect to on 5349.

---

## 2. Bring it up

Requirements: Docker (with Compose v2) for the container path, and
`openssl` for the certificate generator.

```bash
# Once: generate the CA, the server certificate and the credential file.
./gen-certs.sh

# The positive server: TLS-only on 5349 (and 443).
docker compose up -d

# The negative control: plain TURN on the SAME port 5349, TLS off.
# This *replaces* the positive server — they cannot both hold 5349.
docker compose --profile plain up -d

# Stop everything.
docker compose down
```

What lands on disk (all under `secrets/`, which `.gitignore` keeps out of
the tree):

| Path | What it is |
|---|---|
| `secrets/ca/ca.pem` | The local CA.  **This is what a verifying client trusts.** |
| `secrets/ca/ca.key` | The CA's private key.  Test-only; regenerate with `./gen-certs.sh --force`. |
| `secrets/tls/turn.pem` | The server chain coturn presents (leaf, then CA). |
| `secrets/tls/turn.key` | The server's private key. |
| `secrets/creds.txt` | The realm and the static long-term credential. |
| `secrets/logs/turnserver.log` | The server log — N1's "TLS handshake visible in the log" evidence. |

Regenerating the certificate rotates the trust anchor and breaks every
client pinned to the old one, so it is an explicit act (`--force`), not
a side effect.

### The native path (what CI uses)

The same `turnserver.conf` runs without Docker:

```bash
turnserver -c /path/to/turnserver.conf
```

with the three container paths rewritten to local ones (`cert=`,
`pkey=`, `log-file=`).  This is what CI's Linux leg does (v1.4 plan §5
**N5-2**), because GitHub's hosted runners have no Docker daemon.

---

## 3. What the server is configured to be

`turnserver.conf` is short and every non-obvious line is commented.  The
load-bearing decisions:

* **TLS 1.2 is the floor** (`no-tlsv1=1`, `no-tlsv1_1=1`).  RFC 8656
  § 7.2 requires TLS 1.2 or better, and the client's DTLS stack
  (ADR-013) is configured to the same floor.  A test that passes at
  TLS 1.0 is testing a protocol the client cannot negotiate in the field.
* **No TURN-over-DTLS** (`no-dtls=1`).  N1's scope is RFC 5766
  long-term credentials over a TCP control stream wrapped in TLS
  (RFC 6062 § 3.2) — no alternate auth, no DTLS.
* **Loopback peers are allowed** (`allow-loopback-peers=1`).  The sample
  and the tests run on one host, so the relay peer *is* loopback; without
  this the allocation is refused and the failure looks like a credential
  problem.
* **The relay range is small and published on loopback only** (41 ports,
  `x-relay-ports` in `docker-compose.yml`, mirrored by `min-port` /
  `max-port`).  A relayed allocation forwards datagrams from an
  unauthenticated peer, so a range bound to `0.0.0.0` is an open relay.
  Widen the binding only if peers on other hosts must be served, and
  firewall it when you do.
* **Static credentials, published on purpose** — `nimrtc` /
  `nimrtc-test-secret` in realm `nimrtc.local`.  A test credential that
  has to be fetched is a test that cannot run from a clean checkout.
  The *real* credential lifecycle is N7's job (a provider consulted at
  gathering time and re-consulted on expiry); this file is exactly the
  static server list that N7 keeps working (§5 N7-1).
* **`lt-cred-mech` is explicit.**  coturn's default is the *short-term*
  mechanism, where the "password" is a credential the server hands out
  — there is nothing for a test to configure up front.  The long-term
  mechanism is the one RFC 5766 §4 describes, and it is what makes the
  `user=` line mean anything.
* **`cli-password` is required, and not optional politeness.**  coturn
  refuses to start with *"allow_loopback_peers and empty cli password
  cannot be used together"*.  The CLI password guards coturn's
  administrative console, not the TURN protocol — but without it the
  server will not run with the loopback peers the tests need.

---

## 4. How the client uses it

The library side already has the vocabulary; N1 is what makes
`TurnTransport::Tls` true.

```cpp
nimrtc::ice::TurnServer turn;
turn.host     = "127.0.0.1";
turn.port     = 5349;
turn.username = "nimrtc";
turn.password = "nimrtc-test-secret";
turn.transport      = nimrtc::ice::TurnTransport::Tls;
turn.tls_server_name = "turn.nimrtc.local";   // SNI + the name verified

// Existing lifecycle: pre_open(), then add_turn_server(), then open().
```

Verification pins `secrets/ca/ca.pem`.  The certificate is valid for
`turn.nimrtc.local`, `localhost`, `127.0.0.1` and `::1`, so a client that
connects by address verifies too.

Until N1 lands, `turn_transport_supported(TurnTransport::Tls)` is
`false`, `add_turn_server()` warns, and the connection is plain TCP to
port 5349 — **which this server does not accept**, so the observable is
"the session does not establish", not "the session established over a
transport nobody checked".

---

## 5. The positive and negative controls

| N1's case | Server | Expected |
|---|---|---|
| Positive | `coturn` (TLS-only, 5349) | The session establishes; `Stats::transport.turn_transport_used == "tls"`; the server log names the TLS handshake |
| Negative | `coturn-plain` (TLS off, **also 5349**) | The session **fails to establish** — a client that connects here was told to use 5349 and fell back |

The negative control is the one that matters, and **the port matters**.
A control on a *different* port cannot catch the defect: the client
would fail to reach the wrong port, which is a connection error, not a
protocol downgrade.  The control has to be a plain server **on the port
the client was told to use**, so the only thing between the client and a
working session is whether it actually negotiated TLS.

That is also why the two services are mutually exclusive — one host
port, two listeners — and why the control is brought up *instead of* the
positive server rather than beside it.

---

## 6. CI

CI does **not** use this compose file.  GitHub's hosted runners have no
Docker daemon, so the Linux leg installs `coturn` natively and runs it
with `--tls-only` against a certificate from this directory's
`gen-certs.sh` (v1.4 plan §5 **N5-2**).  The compose file is the
*developer* path and the *archived-evidence* path: the two-machine run
that N6 files records the server configuration it used, and this is that
configuration.

Both paths present the same thing to the test — a TURN URI and a
credential — which is what keeps the claim honest regardless of which
path brought the server up.

---

## 7. Tearing it down

```bash
docker compose --profile plain down
rm -rf secrets/          # also drops the generated trust anchor
```

Removing `secrets/` is safe: nothing in the repository depends on a
particular certificate, and `gen-certs.sh` recreates all of it.
