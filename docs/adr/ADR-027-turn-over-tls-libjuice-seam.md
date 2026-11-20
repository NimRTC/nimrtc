# ADR-027: TURN over TLS — a narrow TLS seam in the vendored libjuice, and no plaintext fallback

| | |
|---|---|
| Number | 027 |
| Status | **Accepted** |
| Date | 2026-11-20 |
| Phase | v1.4 (N1) |
| Bound to | v1.4.0 |
| Supersedes | — |
| Related | `docs/plan/v1.4-plan.md` §3 (N1) and §5 (N1-2, as amended); `AGENTS.md` L5; `src/third_party/libjuice/patches/0002-turn-tls-transport.patch`; `src/modules/ice/src/turn_tls.cpp`; `src/modules/ice/include/nimrtc/ice/ice.hpp`; `docs/adr/ADR-014-web-transport-vendor.md` (vendor-pin precedent) |

## Context

NimRTC's ICE module can gather a TURN relay candidate and reach the
TURN server over UDP or over plain TCP. Both were implemented against
the vendored libjuice, and both work: the v1.4 plan's N5 leg was
verified against a real coturn 4.5.2.

What was missing was **TURN over TLS** — RFC 6062, the transport that a
TURN server offers on the conventional port 5349. A deployment that
only exposes 5349 (or that requires the TURN credentials to travel
inside an encrypted channel) cannot be reached by the current code.

The vendored libjuice *has* the enum value for it. `juice_turn_transport_t`
contains `JUICE_TURN_TRANSPORT_TLS`, and upstream's allocation path
handles it like this:

```c
if (turn_server->transport == JUICE_TURN_TRANSPORT_TLS) {
    JLOG_WARN("TURN over TLS is not supported, falling back to TCP");
    entry->turn->transport = JUICE_TURN_TRANSPORT_TCP;
}
entry->turn->stream = turn_tcp_stream_create(record);
```

That is the whole of upstream's TLS support: **a downgrade to plain
TCP**. On a TLS-only listener the connection fails, and on a server
that also listens in the clear it *succeeds* — with the TURN long-term
credential mechanism's username and password sent over an unencrypted
socket. A configuration knob that says "TLS" and then does plaintext is
worse than no knob at all: the operator believes the channel is
protected, and the code says so in a log line that looks like a
warning rather than a security failure.

So N1 could not be "turn the enum value on". It had to be: implement
TURN-over-TLS for real, **and** remove the downgrade, so that a client
configured for TLS either speaks TLS or fails.

## The two options

There were two ways to get a TLS-capable TURN client, and they differ in
*where* the TLS stack lives and *who* owns the TURN protocol.

### Option A — a narrow TLS seam in libjuice, wolfSSL on the NimRTC side

libjuice keeps the TURN protocol, the STUN message coding, and the
self-delimiting stream framing of RFC 8656 §12.4 / §12.5. It gains a
small ops structure — `juice_turn_tls_ops_t` — through which the host
supplies a TLS client. NimRTC implements that interface on top of the
already-vendored wolfSSL.

### Option B — NimRTC implements the TURN-over-TLS control connection itself

NimRTC opens the TLS connection, performs the TURN Allocate /
CreatePermission / ChannelBind transactions over it, and hands the
relayed address to libjuice as if it had come from a plain UDP TURN
server.

### Decision

**Option A.** The reasons, in order of weight:

1. **TURN is a protocol, not a transport detail.** libjuice already
   implements TURN: the Allocate transaction, the long-term credential
   mechanism, the channel map, the ChannelData framing, the refresh
   timer, the allocation lifetime. Option B would mean NimRTC
   re-implementing all of it — a second TURN stack living next to the
   first, with two sets of credentials handling, two sets of refresh
   timers, and two sets of bugs. The *only* thing Option B adds is the
   TLS wrapping.
2. **The seam is small and bounded.** The interface is five callbacks
   (`connect`, `handshake`, `read`, `write`, `close`) over a
   byte-oriented stream. libjuice's framing is self-delimiting, so the
   seam never has to understand a TURN message.
3. **The TLS stack is already vendored and already a NimRTC
   dependency.** wolfSSL is the DTLS backend (ADR-013 territory, and
   the vendor manifest's first entry). Option B would have needed a
   *second* TLS stack, or an awkward reuse of the DTLS one.

### Decision 1 — the seam is an ops struct, not a link-time dependency

libjuice does not link wolfSSL, does not include wolfSSL headers, and
does not know what a certificate is. The host hands it a struct of
function pointers:

```c
typedef struct juice_turn_tls_ops {
    void *ctx;
    void *(*connect)(void *ctx, int socket_fd, const char *host,
                      uint16_t port, const char *server_name);
    int  (*handshake)(void *session);
    int  (*read)(void *session, char *buf, size_t size);
    int  (*write)(void *session, const char *buf, size_t size);
    void (*close)(void *session);
} juice_turn_tls_ops_t;
```

This is the same shape as libjuice's existing `juice_conn_ops` /
`juice_mux_*` seams, and it keeps the vendored tree free of any TLS
dependency — which matters for L5 (`src/third_party/` is the only place
vendor code lives; a vendored MPL-2.0 tree is the wrong place to carry
a second crypto stack).

The `server_name` argument is the name presented as **SNI** and
verified against the server's certificate. `juice_turn_server_t` gains
a matching field; `NULL` means "use the host name".

### Decision 2 — the connection becomes three-phase

A TLS session can only be created once the TCP connection has
completed, and the handshake may need several round trips. The
original `turn_tcp_stream_check_connected()` had two states (still
connecting / connected). The seam adds two more, and the function is
split accordingly:

1. **TCP connect** — non-blocking `connect()`, `select()` for
   writability, `getsockopt(SO_ERROR)` to learn the outcome.
2. **TLS session** — created lazily, once the socket is connected. The
   ops take ownership of the socket at this point; from here on the
   stream does not close it.
3. **TLS handshake** — advanced one step at a time
   (`WOLFSSL_ERROR_WANT_READ` / `WANT_WRITE` are handled by the host
   side and reported as "pending").

Once the session exists, `turn_tcp_stream_recv()` and `flush_locked()`
route their bytes through the ops rather than `recv()` / `send()` on the
raw socket.

### Decision 3 — no plaintext fallback, and the failure is immediate

**This is the security-relevant decision, and it is the reason the
patch exists at all.**

A `JUICE_TURN_TRANSPORT_TLS` server with no TLS client configured (or
with a `connect` callback that is NULL) **fails the allocation**. It
does not fall back to plain TCP. The entry is marked with
`agent_turn_state_t::failed`, and `agent_bookkeeping()` turns that into
an immediate `AGENT_STUN_ENTRY_STATE_FAILED` — the entry does not wait
for the retransmission timeout to expire before reporting the failure.

The same holds for a *failed* handshake: the stream records
`failed = true`, the reader thread exits, and the Allocate never
succeeds. The observable outcome is "no relay candidate", never "a
relay candidate obtained over an unencrypted control connection".

This is verified negatively in
`src/modules/ice/tests/test_turn_tls.cpp`
(`TlsClientAgainstPlainTcpServerDoesNotAllocate`): a TLS-configured
client pointed at a *plain* TCP listener on the TLS port gets a
handshake failure (`error state on socket (-308)`) and no relay
candidate.

### Decision 4 — the host side is a NimRTC ICE-module collaborator

The ops are implemented by `TurnTlsClient`
(`src/modules/ice/src/turn_tls.cpp`), which lives in the ICE module
and is wired up in `IceConfig`:

| `IceConfig` field | Meaning |
|---|---|
| `turn_tls_ca_file` | the trust anchor (PEM) the server certificate is verified against |
| `turn_tls_client_cert_file` / `turn_tls_client_key_file` | optional client certificate (mTLS) |
| `turn_tls_verify_peer` | whether the peer certificate is verified at all (default: on) |

The client is TLS 1.2 (`wolfTLSv1_2_client_method()`), loads the CA
with `wolfSSL_CTX_load_verify_locations()`, sets SNI with
`wolfSSL_UseSNI()`, and verifies the peer identity per **RFC 6125
§6.4.4**, which splits the check by the *shape* of the reference
identity:

| Reference identity | wolfSSL call | Certificate field matched |
|---|---|---|
| A host name | `wolfSSL_set1_host()` | the `dNSName` SAN entries |
| An IP literal | `wolfSSL_X509_VERIFY_PARAM_set1_ip_asc()` | the `iPAddress` SAN entries |

The two paths are **exclusive**. A client that points at `127.0.0.1` is
not allowed to fall back to matching the `dNSName` `localhost` — that
would let a certificate issued for a different name satisfy an IP
reference, which is exactly the substitution RFC 6125 §6.4.4 forbids.

**Two vendored-dependency changes were needed.** wolfSSL was built with
`NO_RSA`, which made every *RSA* certificate fail verification. The
standard certificates produced by `examples/turn-server/gen-certs.sh`
are RSA 2048, so `WOLFSSL_RSA` is now enabled in
`src/third_party/wolfssl/CMakeLists.txt`. ECC was already enabled; both
families now verify.

`WOLFSSL_IP_ALT_NAME` was also enabled, and it was **not** cosmetic.
wolfSSL only parses `iPAddress` SAN entries into a comparable form when
that option is compiled in; with it off, the alt-name walk skips them
and an IP reference identity can never match anything. The first
implementation of the IP path therefore failed against the real server
with `peer ip address mismatch (-325)` even though the server's
certificate *does* carry `IP Address:127.0.0.1`. The option is now
forced **on** in `src/third_party/wolfssl/CMakeLists.txt`, and the
reason is recorded in `src/third_party/vendor.json`.

### Decision 5 — the vendor patch is a versioned artifact, not a fork

The change to the vendored libjuice tree is recorded as
`src/third_party/libjuice/patches/0002-turn-tls-transport.patch`, in
the same style as the existing `0001-so-reuseaddr.patch`. It carries a
prose header explaining *why* the change exists, because the next
person to re-vendor libjuice needs to know that the plaintext
downstream in `agent_resolve_servers()` is a defect and not a feature.

The patch is regenerable: `tools/make_libjuice_tls_patch.ps1` derives
it from the working-tree diff and rewrites the paths to be
libjuice-relative. It applies cleanly to the libjuice tree as it
stands after PR #34 (TURN over TCP) and PR #50 (0001) — *not* to a
pristine upstream checkout, and the patch's header says so.

`src/third_party/vendor.json` records the second patch in libjuice's
`notes`, so the manifest continues to describe what is actually pinned.

## Consequences

### What gets better

- **TURN over TLS actually works.** Verified against a real coturn
  4.5.2 on the conventional TLS port: TLS 1.2 handshake
  (`ECDHE-RSA-AES256-GCM-SHA384`), successful Allocate, relay address
  `127.0.0.1:49165`, and the handshake visible in the coturn log.  Both
  reference-identity shapes verify — the host name `turn.nimrtc.local`
  through the `dNSName` path, and the IP literal `127.0.0.1` through
  the `iPAddress` path.
- **The plaintext downgrade is gone.** The old behaviour — "TLS is not
  supported, falling back to TCP" — is removed from the vendor tree,
  and the failure mode when TLS is unavailable is now an explicit
  failure rather than an insecure success.  The negative control is a
  plain-only coturn on the **same** port: the client gets no relay
  candidate and does not fall back to plaintext.
- **The TLS stack is not duplicated.** One wolfSSL, used by DTLS and
  by the TURN control connection.

### What gets worse, honestly

- **The vendored tree now carries a second NimRTC-local patch.**
  Re-vendoring libjuice means re-applying both patches, and the
  `turn_tcp.c` / `turn_tcp.h` files (added by PR #34) are NimRTC
  additions, not upstream files. The vendor tree is drifting further
  from upstream with every change. This is tracked in the vendor
  manifest, and the endgame is a NimRTC fork of libjuice (like the
  msquic fork in ADR-014) — but that is its own piece of work.
- **The seam is C-ABI-shaped and therefore awkward to extend.** A
  future need — for instance a TLS 1.3-only deployment, or an ALPN
  negotiation, or a client-certificate refresh — is a change to the
  ops struct, which is a vendored-tree change, not a NimRTC-only
  change.
- **The wolfSSL build grew.** Enabling `WOLFSSL_RSA` costs binary
  size in every consumer that links wolfSSL, including those that only
  wanted DTLS.

### Follow-up work

| Item | Where | Status |
|---|---|---|
| N1 — TURN over TLS with a real coturn positive test and a plain-TCP negative control | v1.4-plan §3 (N1) | **Done** |
| Record the vendor patch | `src/third_party/libjuice/patches/0002-turn-tls-transport.patch` | **Done** |
| Update the vendor manifest | `src/third_party/vendor.json` (libjuice notes) | **Done** |
| A NimRTC fork of libjuice, so the patches become commits | vendor-migration Phase 3 | Open |
| N2 — ICE-over-TCP candidates (RFC 6544) | v1.4-plan §3 (N2) | Open |

## Alternatives considered

### B — NimRTC implements the TURN-over-TLS control connection itself

Rejected, for the reasons in the Decision section: it means a second
TURN stack (credentials, channel map, refresh timer, allocation
lifetime) living next to libjuice's, and it needs a TLS stack that
libjuice would then have to be told not to use.

### C — Keep the downgrade and document it

Rejected. The downgrade sends the TURN credentials in the clear while
the configuration says "TLS". A documented insecure default is still an
insecure default; the plan's N1 acceptance criterion is a *strict*
negative control precisely because the downgrade was considered
unacceptable.

### D — Implement TURN-over-TLS inside libjuice with a vendored TLS stack

Rejected. That would put a TLS stack into an MPL-2.0 vendored tree
that NimRTC does not own, duplicating the wolfSSL dependency, and it
would violate L5's spirit (one vendored copy per dependency).

### E — Fork libjuice outright instead of carrying patches

Rejected *for now*. A fork is the right endgame (and the msquic
precedent in ADR-014 shows the project is willing), but it is a
larger change than N1 needs, and the patch mechanism already in place
(0001) covers the interim. The fork is listed as follow-up work.
