# ADR-028: ICE-over-TCP — a NimRTC-side TCP candidate layer, and libjuice stays UDP-only

| | |
|---|---|
| Number | 028 |
| Status | **Accepted** |
| Date | 2026-11-21 |
| Phase | v1.4 (N2) |
| Bound to | v1.4.0 |
| Supersedes | — |
| Related | `docs/plan/v1.4-plan.md` §3 (N2) and §5 (N2-1); `docs/adr/ADR-027-turn-over-tls-libjuice-seam.md`; `src/modules/ice/`; `src/modules/sdp/`; `src/modules/raw_udp/src/udp_socket.cpp`; `src/third_party/libjuice/` |

## Context

N2 is **ICE-over-TCP** (RFC 6544): two peers that can only reach each
other over TCP nominate a TCP candidate pair, and the selected pair's
`protocol` reads `"tcp"`.

The plan's mechanism names three pieces:

1. `IceTransport` gathers **host TCP candidates** with an active /
   passive `tcptype` (RFC 6544 §4.5), prioritised against UDP
   (§4.3);
2. the **SDP writer** emits
   `a=candidate:… tcp <pri> <host> <port> typ host tcptype passive`
   — the piece that does not exist anywhere in the tree;
3. **N2-1**: TCP is attempted *after* UDP fails, not raced with it.

The first two are NimRTC-side work. The third — "two peers that can
only reach each other over TCP nominate a TCP pair" — is not, because
the ICE agent that nominates the pair is libjuice's, and libjuice's
ICE layer is **UDP-only**. That is not a gap that a NimRTC-side patch
can close.

### What libjuice actually supports

Verified in the vendored tree
(`src/third_party/libjuice/src/src/`, pinned at `77daa8be` plus the
NimRTC patches):

| Where | What it says |
|---|---|
| `ice.c` `parse_sdp_candidate()` | `if (strcmp(transport, "UDP") != 0) { JLOG_WARN("Ignoring candidate with transport %s", transport); return ICE_PARSE_IGNORED; }` — a TCP candidate on the wire is **dropped on arrival**. |
| `ice.c` `ice_generate_candidate_sdp()` | emits `typ host` / `typ srflx` / `typ prflx` / `typ relay` and nothing else — **no `tcptype`**, because there is no TCP candidate to describe. |
| `ice.h` `ice_candidate_t` | has `type`, `component`, `foundation`, `priority`, `hostname`, `service`, `related_address` — **no transport field and no `tcptype` field**. The candidate type is UDP-only by construction. |
| `conn.c` / `conn.h` | the connection abstraction is built on `udp_socket_config_t`; `conn_create()` takes a UDP socket config. There is **no TCP connection mode**. |
| `udp.c` / `socket.h` | the socket layer is a UDP socket (`SOCK_DGRAM`). |

The one TCP thing libjuice *does* have is `turn_tcp.c` — and that is
the **TURN control connection** (RFC 8656 §12), i.e. the client talks
to a TURN server over TCP while the *relayed transport address* stays
UDP. It is not ICE-over-TCP. It cannot be reused for it: the TURN
control connection carries TURN's own framing (Allocate, ChannelBind,
ChannelData), not STUN binding requests.

So: **a TCP candidate produced by NimRTC would be silently discarded
by libjuice**, and libjuice would go on to nominate a UDP pair or
nothing. The plan's acceptance criterion — "the pair's protocol is
`tcp`" — is unreachable through libjuice's ICE agent.

### What NimRTC already has

- `src/modules/raw_udp/` — a cross-platform socket wrapper
  (`UdpSocket`) with the exact shape a TCP socket needs: `open()` /
  `close()` / `send_to()` / `recv_from()` / `local_port()` /
  `local_endpoint()`, the deferred-physical-close discipline (the fd
  is *retired*, not closed, so a peer thread inside `recvfrom()` is
  never racing a `close()`), and the bounded-poll-then-syscall pattern
  that lets a stop flag be observed without closing a socket under a
  blocked reader. It is **IPv4-only** and **datagram-oriented** today.
- `src/modules/ice/` — the ICE module: candidate parsing/serialisation
  (`Candidate::to_sdp()`, `parse_candidate()`), the libjuice agent
  wiring, and the `plugins::IICETransport` seam.
- `src/modules/sdp/` — the SDP munger, which re-emits
  `a=candidate:<body>` verbatim from `MediaDescription::candidates`.
  Note: the munger is **protocol-agnostic** — it writes back whatever
  body it was handed. So the "SDP writer has no `tcptype`" gap in the
  plan (§8a, "No ICE-over-TCP" row) is really a gap in **who produces
  the candidate body**, not in the munger.

### The two options

**Option A — NimRTC implements its own ICE-over-TCP path.** A
NimRTC-side TCP candidate gatherer + a TCP connectivity-check driver
live beside libjuice's UDP ICE agent. NimRTC owns the TCP candidates
end to end: gathering, SDP serialisation, connectivity checks, and
nominating a TCP pair.

**Option B — teach libjuice ICE-over-TCP.** The vendored tree gains a
TCP socket mode, a `tcptype` field on `ice_candidate_t`, TCP
candidate generation and parsing, and a TCP connectivity-check
driver — carried as another NimRTC-local patch (a third one, on top of
`0001-so-reuseaddr.patch` and `0002-turn-tls-transport.patch`).

## Decision

**Option A.** The reasons, in order of weight:

1. **Option B is a fork, not a patch.** Adding a TCP transport mode to
   libjuice means touching its connection abstraction
   (`conn_mode_entry_t` gains a whole new set of function pointers:
   init/cleanup/lock/unlock/interrupt/send/get_addrs), its socket
   layer, its candidate structure, and its connectivity-check driver.
   That is not a seam — it is a second transport inside a vendored
   MPL-2.0 tree NimRTC does not own, in a codebase whose ICE layer is
   otherwise a faithful upstream copy. ADR-027 set the precedent for
   the *right* kind of vendor change (a narrow ops struct through which
   the host supplies the missing capability); this is not that.
2. **The re-vendoring cost is real and recurring.** ADR-027 already
   records the cost of two patches. A third, of this size, makes
   re-vendoring libjuice a project in itself — and the vendored tree
   is drifting from upstream faster than the team can absorb.
3. **NimRTC already owns the socket layer shape.** `UdpSocket` is a
   proven cross-platform wrapper with the fd-lifetime discipline
   already worked out (retire-don't-close, bounded poll). A TCP
   socket is the same wrapper with `SOCK_STREAM`, `connect()`,
   `listen()`/`accept()` and a **message framing** layer — because ICE
   over TCP carries STUN messages over a byte stream, so the framing
   (RFC 6544 §3.2: the 2-byte length prefix) is NimRTC's
   responsibility, exactly as RFC 8656 §12.4 framing is libjuice's
   for TURN-over-TCP.
4. **The ICE protocol on top of TCP is small.** STUN binding requests
   and responses are already implemented in libjuice — but a NimRTC-side
   TCP driver needs the STUN encoding too. That is the part that
   needs care.

### Decision 1 — the TCP path is a NimRTC collaborator, not a libjuice patch

The new code lives in `src/modules/ice/`:

| File | Role |
|---|---|
| `src/modules/ice/include/nimrtc/ice/tcp_candidate.hpp` | the TCP candidate shape (host, port, `tcptype` active/passive) + its SDP serialisation |
| `src/modules/ice/src/tcp_socket.{hpp,cpp}` | the cross-platform TCP socket (connect / listen / accept / send / recv), modelled on `UdpSocket` |
| `src/modules/ice/src/tcp_framing.{hpp,cpp}` | the RFC 6544 §3.2 stream framing (2-byte length prefix) |
| `src/modules/ice/src/tcp_ice.{hpp,cpp}` | the driver: gather TCP host candidates, exchange them, run connectivity checks, nominate a TCP pair |

`src/modules/sdp/` needs **no production change**: the munger already
writes back candidate bodies verbatim, so a body that carries
`tcptype passive` reaches the wire as-is. (A *test* may be added to
`src/modules/sdp/tests/test_sdp.cpp` to pin that behaviour, because
"it works because the munger does not filter" is a property worth
pinning.)

### Decision 2 — the seam stays additive, and `tcptype` becomes a first-class field

`plugins::IceCandidate` (the frozen seam) gains:

- `tcptype` — `"active"` / `"passive"` / `""` (not a TCP candidate).

`plugins::IceCandidate::protocol` stops being documented as
"always udp": the seam's comment currently says *"ICE-over-TCP (RFC
6544) is not implemented yet (plan C2), so this is "udp" for every
pair a v1.2.0 engine can select."* — that sentence is now false and
must change. This is an **additive** change to a frozen interface
(ADR-015): a new field with a default, no existing signature changes.

`ice::Candidate` (the concrete shape) gains the matching `tcptype`
field, and `Candidate::to_sdp()` / `parse_candidate()` learn to round
trip it. Note `parse_candidate()` currently stops at `typ <type>` and
then optionally reads `raddr`/`rport`; it must also accept the
`tcptype` token, and it must not mistake `tcptype` for `typ`.

### Decision 3 — TCP is attempted after UDP fails (N2-1)

The plan is explicit: *"TCP is attempted **after** UDP fails, not raced
with it."* and *"Racing makes a healthy network occasionally pay a TCP
setup and head-of-line blocking."*

So the TCP path is **not** gathered in parallel with UDP. It is
entered when the UDP path has failed to nominate a pair — the ICE
agent reports `Failed`, or the gathering round completes and no pair
is nominated within the connectivity-check budget. Only then does
NimRTC gather TCP host candidates, exchange them, and run TCP
connectivity checks.

Consequence for the SDP: an offer that is published **before** the
UDP failure is known cannot carry TCP candidates — trickle ICE
(RFC 8838) is how they get there later, and NimRTC's engine already
uses `a=ice-options:trickle`. The implementation must therefore
support **adding a remote TCP candidate after the fact**
(`add_remote_candidate()` already exists on the seam) and
**re-publishing a local description** with the TCP candidates
(`local_description()` already exists).

### Decision 4 — priority follows RFC 6544 §4.3

RFC 6544 §4.3 gives the relative preference: an **active** TCP
candidate (we connect) is preferred over a **passive** one (we
listen), and TCP candidates are ordered against UDP by the same
type-preference ladder RFC 8445 §5.1.2.1 uses (host 126 / prflx 110
/ srflx 100 / relay 0).

The `Candidate::priority` field is a single `std::uint32_t` that
today carries libjuice's UDP priority. The TCP path computes its own
priority and **must not** collide with the UDP path's values — two
candidates with the same priority and the same foundation would be
indistinguishable to a peer. The TCP foundation string is therefore
namespaced (e.g. `tcp-1`) and the priority is computed from the
RFC 6544 ladder.

### Decision 5 — the acceptance test is the TCP-only firewall shape

The plan's test shape: *"A loopback test where the UDP sockets are
bound to addresses that cannot route to each other and TCP is the
only path — the shape of a TCP-only firewall, deterministic in CI."*

Concretely: two `IceTransport`s on loopback; UDP is made unusable
(either bound to addresses that cannot reach each other, or disabled
outright); TCP is the only working path; the test asserts the
selected pair's `protocol` reads `"tcp"`.

The test must be **deterministic** — the requirement doc is explicit
("CI 测试应具备确定性，环回测试不依赖外部网络或 Docker daemon"). So it
must not depend on the host's firewall configuration, on NAT, or on
timing luck. Binding UDP to an unroutable address and asserting the
TCP path is the one that survives is the mechanism; the test must also
assert the **negative** direction (with UDP working, the selected pair
is UDP) — otherwise "TCP is the fallback" degenerates into "TCP is the
only thing", which would pass the acceptance criterion while being
wrong.

### Decision 6 — the vendored tree is untouched

No new patch on `src/third_party/libjuice/`. `src/third_party/vendor.json`
is unchanged. The libjuice agent continues to own the UDP candidates
and the UDP connectivity checks; the NimRTC TCP driver owns the TCP
candidates and the TCP connectivity checks, and the two are coordinated
by NimRTC above them.

The coordination point is the existing `IceTransport` seam: the TCP
driver is a collaborator inside `IceTransport::Impl`, and the
nominated-pair bookkeeping (`selected_local()` / `selected_remote()` /
`selected_pair()`) learns to project **either** source's candidate.

### Consequences

#### What gets better

- **The plan's acceptance criterion becomes reachable.** A TCP
  candidate pair can actually be nominated, and the seam can report
  `protocol == "tcp"`.
- **The vendored tree stays a faithful upstream copy** (modulo the two
  existing patches), so re-vendoring does not get harder than it
  already is.
- **The SDP writer gap closes with no SDP production change** — the
  munger was never the problem; the candidate *producer* was.

#### What gets worse, honestly

- **NimRTC now owns a second ICE connectivity-check path.** Two sets
  of state machines (UDP via libjuice, TCP via NimRTC) must agree on
  which pair won. This is the real cost of Option A and it is not
  small.
- **STUN encoding is implemented twice** — once in libjuice for UDP,
  once in NimRTC for TCP. The TCP side must encode binding requests
  and parse binding responses correctly (RFC 8489 §6). This is a
  duplicated protocol surface and a duplicated bug surface.
- **TCP candidates are host-only.** RFC 6544 §4.5 only defines host
  candidates with a `tcptype`; server-reflexive and relayed TCP
  candidates are out of scope for this slice (a TURN server allocates
  a UDP transport address — RFC 5766 — so there is no relayed TCP
  candidate to gather).
- **A TCP candidate pair cannot be used for media as-is.** RFC 6544
  carries STUN/DTLS over the TCP stream, but NimRTC's media path
  expects a datagram transport. Whether the selected TCP pair can
  carry RTP is a **separate question** this slice does not answer;
  the slice's scope is the *candidate and the nomination*.

#### Follow-up work

| Item | Where | Status |
|---|---|---|
| N2 — TCP host candidates + TCP nomination + SDP `tcptype` | v1.4-plan §3 (N2) | **Open** |
| N2-1 — TCP attempted after UDP fails | v1.4-plan §5 (N2-1) | **Open** |
| Whether a TCP-nominated pair can carry media (RTP over the TCP stream) | not planned | Open |
| A NimRTC fork of libjuice, so the patches become commits | vendor-migration Phase 3 | Open |

## Alternatives considered

### B — teach libjuice ICE-over-TCP (a third vendor patch)

Rejected, for the reasons in the Decision section: it is a fork's worth
of change inside a vendored MPL-2.0 tree, it re-implements the
connection abstraction rather than extending a seam, and it makes
re-vendoring libjuice progressively harder — the cost ADR-027 already
warned about, at a scale the patch mechanism was not designed for.

### C — shrink N2 to "candidate generation + SDP serialisation only"

Rejected. It would have been the cheap option, and it is exactly the
kind of scope reduction that makes an acceptance criterion look
satisfied while the thing it names is absent: the plan's criterion is
*"two peers that can only reach each other over TCP nominate a TCP
candidate pair"*, and a candidate generator that no agent ever
nominates does not satisfy it. The scope reduction is recorded here as
**considered and rejected**, not as a fallback.

### D — reuse libjuice's TURN-over-TCP stream for ICE-over-TCP

Rejected. `turn_tcp.c` implements the TURN control connection:
RFC 8656 §12.4/§12.5 framing, the TURN channel map, the long-term
credential mechanism, Allocate/Refresh/CreatePermission/ChannelBind.
ICE-over-TCP needs RFC 6544 §3.2 framing and STUN binding requests.
The two share a socket and nothing else.

### E — vendor libnice (as the design doc `dsg-1795166053036` suggests)

Rejected. The design document names *"ICE/TCP 穿透实现 (Libnice)"* as
a third-party service. But NimRTC's ICE module is built on libjuice
(ADR-027 territory), and swapping the ICE implementation for libnice
would mean replacing the module wholesale — including the TURN-over-TLS
work that ADR-027 just landed — to add one transport. The design
document predates the libjuice-based ICE module and its module
decomposition has not been updated to match; the **requirement** doc
(`req-1795167875048`) and the v1.4 plan are the authoritative sources
for N2, and neither names libnice.
