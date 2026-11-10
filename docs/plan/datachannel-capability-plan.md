# Data Channel Capability Plan — Closing the Five Gaps

> **What this document is.** The executable counterpart to the capability-gap
> table raised for the data channel in the v1.1.0 review. The table lists
> five capabilities that `libwebrtc` has and NimRTC v1.1.0 does not (or has
> incorrectly). This document fixes *what the actual current state is*,
> *how the work is cut up*, *what proves each piece*, and *which release
> line carries it*.
>
> | | |
> |---|---|
> | Plan opened | 2026-11-09 |
> | Baseline | `main` @ `v1.1.0` tag |
> | Scope class | Data channel — wire-format semantics, channel routing, reliability correctness, priority plumbing |
> | Status | **WP-A / WP-B / WP-C / WP-D / WP-E landed 2026-11-10 (DEC-1 = (a), DEC-2 = (b), DEC-3 = (c), DEC-4 = A1, DEC-5 = (a))** |
> | Owner | BDFL |
> | Related | `docs/plan/v1.1-plan.md` §5.6 (version ladder), `docs/plan/v0.11-plan.md` §2.2 / §10 (SCTP → WebTransport re-design), `docs/plan/v1.0-plan.md` §2.1.1 (DC-1 / DC-2), ADR-014 (WebTransport vendor), ADR-015 (API stability), `docs/plan/video-pipeline-v2-plan.md`, `docs/plan/audio-pipeline-dev-plan.md`, `docs/plan/session-capability-plan.md`（会话能力第 5/6/7/9 项，落位已定为 **v1.2.0**） |

---

## 1. The gap table, verified against the tree

The table below is the input table with every cell re-checked against the
source. Two cells do not survive the check and are corrected in §1.2.

| # | Capability | libwebrtc | NimRTC v1.1.0 (verified) | Verdict | NimRTC v1.2.0 (current) |
|---|---|---|---|---|---|
| DC-A | Dual backend (SCTP + WebTransport/QUIC) | ✅ (SCTP) | **Two factories exist; only one is a wire backend** | **corrected** (§1.2.1) | ✅ the claim is honest — WebTransport/QUIC is the only wire backend, the SCTP selector is retired (WP-A / DEC-4 = A1) |
| DC-B | Message boundary (message vs. stream semantics) | ✅ | ❌ byte-stream pass-through on the default backend | confirmed | ✅ `kFramed` restores the boundary (WP-B / DEC-2 = (b)); `kRaw` stays byte-stream for Chrome |
| DC-C | Multi-channel isolation | ✅ | ❌ shared callback, cross-wired | confirmed, **worse than stated** (§1.2.2) | ✅ the session's inbound routes are multicast, and the message callback carries the channel identity (WP-C) |
| DC-D | Partial reliability (PR-TTL / maxRetransmits) | ✅ | ❌ **implemented incorrectly**; expiry / count exhaustion kills the channel | confirmed | ✅ the TTL and the budget are per message and an expired message no longer kills the channel (WP-D / DEC-3 = (c)) |
| DC-E | Priority scheduling reaching the data channel | — | ❌ `priority` is written to DCEP and otherwise ignored | confirmed | ✅ the priority is settable and orders the data plane's own outbound queue (WP-E); it does **not** arbitrate against the media plane |

### 1.1 Evidence per item

**DC-B — message boundary.** `WebTransportDataChannel` opens one QUIC
bidirectional stream per channel and forwards whatever the stream delivers
straight into the user callback (`WebTransportDataChannel::on_stream_data`,
`src/web_transport/src/web_transport_data_channel.cpp`). A QUIC stream is a
byte stream; there is no length prefix anywhere on the path. Two `send()`
calls of 10 bytes each may surface as one 20-byte callback, or one 10-byte
call may surface as two 5-byte callbacks, depending on how QUIC happened to
frame them. The SCTP backend does not have this problem — usrsctp delivers
one `receive_cb` per message — but see DC-A for why that path is not
reachable in a real connection.

**DC-C — multi-channel isolation.** Two independent defects stack here:

1. *The engine-level callback carries no channel identity.* `IDataChannel`'s
   message callback is `std::function<void(BufferView)>` — no channel id, no
   label, no stream id (`src/plugins/include/nimrtc/plugins/datachannel.hpp`).
   `NimRTCEngine::create_data_channel()` installs the *same*
   `on_data_message_` into every channel it creates
   (`src/engine/src/engine.cpp`). A caller that opens two channels cannot
   tell which one a message arrived on.
2. *The WebTransport session has one slot per callback kind, and it is
   overwritten.* `WebTransportDataChannel::attach_session_callbacks()`
   calls `session_->set_on_datagram(...)` and
   `session_->set_on_peer_stream(...)`, which **replace** the session's
   single `on_datagram_cb_` / `on_peer_stream_cb_` members
   (`src/web_transport/src/web_transport_msquic_session.hpp`). All channels
   of one engine share one QUIC session (`Impl::wt_session`,
   `src/engine/src/engine_impl.hpp`), so the *last* channel created wins the
   inbound datagram and peer-stream routing, and every earlier channel goes
   deaf. Worse, `WebTransportDataChannel::close()` calls
   `set_on_datagram({})` / `set_on_peer_stream({})` — closing any one
   channel tears down inbound routing for **all** of them.

**DC-D — partial reliability.** The WebTransport implementation of both PR
modes is wrong, in two different ways:

* *PR-TTL*: `WebTransportDataChannel::open(cfg)` computes
  `pr_ttl_deadline_ = now + max_packet_lifetime_ms` **once, at open time**,
  and stores it as a channel-lifetime absolute deadline. Every subsequent
  `send()` compares against that same deadline. Once the deadline passes,
  `send()` resets the stream and drops the message — and the deadline is
  never renewed, so every later `send()` does the same. The channel is
  permanently dead, and the semantics are "the channel dies N ms after it
  opened", not "each message may live at most N ms".
* *PR-count*: `open(cfg)` sets `pr_count_remaining_ = cfg.max_retransmits`.
  The documented default for `max_retransmits` is `0` ("unlimited"), and
  the implementation treats `0` as "count exhausted" — it resets the
  stream and drops the message on the first `send()`. The unbounded
  sentinel `0xFFFF` exists in the code but is never assigned. The counter
  is also decremented per *send*, not per *retransmission*, so it does not
  implement the count policy even when the value is non-zero.

On the SCTP backend, `kPartialReliableCount` is approximated as
"retransmit count × 100 ms of TTL" (`SctpDataChannel::route_send`), which
is a different policy wearing the count policy's name — but the SCTP path
is unreachable in a real connection (DC-A).

**DC-E — priority.** `DataChannelConfig::priority` is read in exactly two
places: it is encoded into the DCEP `DATA_CHANNEL_OPEN` message
(`src/sctp/src/dcep.cpp`), and it is passed to
`s_stream_from_priority()` — which **ignores its argument** and returns
`0` for every channel (`src/modules/datachannel/src/sctp_data_channel.cpp`).
The engine hard-codes `cfg.priority = 128` at both creation sites and
exposes no setter, so the value cannot even be set to anything else from
the public API. The scheduler's per-stream (S-family) priority API
(`sched::Scheduler::set_stream_priority` / `enqueue_for_stream`,
`src/modules/sched/include/nimrtc/sched/sched.hpp`) is **never called by
any data-channel code path**; `tests/test_sched_priority_dc.cpp` exercises
the scheduler in isolation, which is why DC-2 was recorded as a v1.0.0
DoD gate that the data plane never actually consumes.

### 1.2 Two corrections to the input table

#### 1.2.1 DC-A — "SCTP ✅" overstates the SCTP backend

Both factories are registered and both are reachable through the plugin
registry (`"sctp"` → `SctpDataChannelFactory`, `"web_transport"` →
`WebTransportDataChannelFactory`), so the *plumbing* is dual-backend. But
the SCTP backend is **not a wire backend**:

* `SctpDataChannel`'s constructor creates its own `usrsctp` socket bound
  to `127.0.0.1` (`src/sctp/src/usrsctp_socket.cpp`,
  `s_bind_and_configure`). It is an in-process loopback socket.
* The engine's SCTP-over-DTLS wiring is a placeholder. `init_datachannel_registry()`
  documents that hooking the DTLS transport's outbound side to the SCTP
  socket and its inbound side to `feed_inbound_datagram()` is **not
  implemented** — only a `void*` cookie is reserved
  (`src/engine/src/engine_transport.cpp`).
* `UsrsctpSocket::feed_inbound_packet()` returns `kErrUnsupported` on the
  AF_INET path, and `SctpDataChannel` never opens its own association
  (`listen()` / `connect()` are not called on the engine path). The DCEP
  `DATA_CHANNEL_OPEN` is only sent if a caller explicitly invokes
  `send_dcep_open()`, which the engine does not do.

So: SCTP today is a **loopback test backend**, exercised by
`tests/test_sctp_usrsctp.cpp` and `src/modules/datachannel/tests/test_sctp_data_channel.cpp`.
The default production backend is WebTransport/QUIC
(`EngineConfig::datachannel_backend = "web_transport"`).

**Correction.** The honest cell is "❌ / no wire backend" for SCTP, and
the correct remedy is *not* to build SCTP-over-DTLS. See DC-A in §2.

#### 1.2.2 DC-C is worse than "shared callback cross-wired"

"Shared callback cross-wiring" describes the symptom at the API level. The
mechanism is worse than a shared callback: the last channel created
*hijacks* the session's inbound routing, and closing *any* channel
*disables* inbound routing for *all* channels. A caller who opens two
channels and closes one does not get a mis-routed message — they get a
silently dead peer. §2's WP-C3 fixes the ownership model; the API change
(WP-C1) is what makes the fix expressible.

---

## 2. Work packages

Five work packages, one per gap. They are ordered by dependency, not by
table row. Each is independently reviewable, independently testable, and
has a defined "done".

| WP | Closes | Depends on | Target line |
|---|---|---|---|
| WP-A | DC-A (dual-backend claim) | — | `v1.2.0` (**landed 2026-11-10**) |
| WP-B | DC-B (message boundary) | — | `v1.2.0` (**landed 2026-11-10**) |
| WP-C | DC-C (multi-channel isolation) | WP-B | `v1.2.0` (**landed 2026-11-10**) |
| WP-D | DC-D (partial reliability) | — | `v1.2.0` (**landed 2026-11-10**; the trunk moved onto the `v1.2.0` line, see DEC-1) |
| WP-E | DC-E (priority → scheduler) | WP-B, WP-C | `v1.2.0` (**landed 2026-11-10**, DEC-5 = (a)) |

### WP-A — Make the backend claim honest

**Problem.** §1.2.1: the SCTP backend is a loopback test backend, but the
public surface presents it as a peer of the production WebTransport
backend, and `EngineConfig::datachannel_name = "sctp"` is documented as
"the legacy SCTP-over-DTLS path".

**Decision taken (§7 DEC-4 = A1, 2026-11-10): retire SCTP from the public
backend selector.** The SCTP backend stays in-tree as a *test* backend (it
carries the PR-SCTP TTL implementation and the DCEP codec, which the
WebTransport path does not), but `"sctp"` was removed from the accepted
values of `datachannel_backend` / `datachannel_name`, and
`docs/api/engine_config.md` states plainly that the production data plane is
WebTransport/QUIC only. The usrsctp removal scheduled for `v1.2.0`
(`v0.11-plan.md` §2.2) becomes a clean removal of a test-only dependency,
and the PR-SCTP reference implementation must first be salvaged as a test
oracle for WP-D.

* **A2 — Build SCTP-over-DTLS (not taken).** Substantially larger: an
  `AF_CONN` usrsctp backend, DTLS key export into usrsctp, the inbound
  `usrsctp_conninput()` path, and a Chrome SCTP interop harness. This is the
  path `v0.11-plan.md` §2.2 deliberately walked away from in favour of
  WebTransport, and re-opening it would re-litigate a closed decision.

**Acceptance (A1).**
* `docs/api/engine_config.md` and the `EngineConfig` header comments state
  which backends are production-capable and which are test-only. ✔
* A regression test asserts that the default `datachannel_backend` resolves
  to a backend that has a *wire* path (i.e. that the default is not the
  loopback SCTP socket). ✔
* `"sctp"` passed to `datachannel_backend` returns `kEngineInternal` with a
  message that names the test-only status, rather than silently resolving to
  a backend that cannot reach a peer. ✔
  (The legacy v0.x spelling — `datachannel_backend` empty plus
  `datachannel_name = "sctp"` — is rejected the same way, and
  `EngineConfig::datachannel_name` now defaults to `""` so an unset field
  never selects the retired family.)

### WP-B — Message boundary: frame the WebTransport payload

**Problem.** §1.1 DC-B: the default backend delivers a byte stream, so
`send()` is not message-preserving. This breaks the `IDataChannel`
contract's implication ("Send a binary message") and makes every
message-oriented use of the data channel unsafe.

**Fix.** Introduce a length-prefixed frame on the WebTransport data
plane, symmetric on send and receive:

* **B1 — Frame format.** A 4-byte big-endian length prefix followed by the
  payload. The prefix is *not* part of the user-visible payload; the
  callback receives the payload only. 4 bytes covers a 4 GiB message,
  which is far above the `a=max-message-size:6556` the engine advertises.
* **B2 — Send path.** `WebTransportDataChannel::send()` writes the
  prefix and the payload. Because a QUIC stream may split a write, the
  frame is assembled into one contiguous buffer before handing it to
  `IBidirectionalStream::send()` — the stream implementation already
  takes ownership of a contiguous span, so this is a local buffer, not a
  protocol change.
* **B3 — Receive path.** A per-channel reassembly buffer consumes the
  prefix, then waits for exactly that many payload bytes, then fires the
  user callback with the payload. A frame whose length prefix arrives
  split across QUIC callbacks is handled by the same "wait for more"
  rule the WebTransport stream-type header already uses
  (`on_peer_stream_data`).
* **B4 — Malformed input.** A length prefix that would exceed the
  configured maximum is a protocol error: the channel transitions to
  `error` rather than allocating an unbounded buffer. The maximum is
  taken from the negotiated `a=max-message-size`, defaulting to 6556 (the
  value the engine already advertises in
  `src/engine/src/engine_signaling.cpp`).
* **B5 — Datagram mode is exempt.** `kUnreliable` already rides QUIC
  datagrams, which are message-preserving by definition. No framing is
  added there, and the datagram payload limit is the session's
  `max_send_length`.

**Interaction with Chrome interop.** The frame is a NimRTC-internal
convention on top of the WebTransport stream. Chrome's `RTCDataChannel`
over WebTransport sends raw bytes on the stream; a NimRTC peer speaking to
Chrome must therefore be able to *send* unframed bytes and *receive* them
unframed. **This is the open design question in §7 (DEC-2):** either the
frame is negotiated, or the framing is NimRTC-only and the Chrome path is
explicitly "byte-stream mode, no message boundary". The plan does not
assume an answer; §7 asks.

**Acceptance (all met — v1.2.0).**
* Unit test: two sends of 10 bytes each surface as **two** callbacks of
  10 bytes each, on the same channel. ✔
  (`WebTransportDataChannelFraming.TwoSendsSurfaceAsTwoMessages`)
* Unit test: one send of 10 000 bytes surfaces as **one** callback. ✔
  (`...OneBigSendSurfacesAsOneMessage`)
* Unit test: a payload delivered in arbitrary 1-byte chunks surfaces as
  one callback (reassembly across callbacks). ✔
  (`...MessageSplitAcrossChunksIsReassembled`)
* Unit test: an oversized length prefix drives the channel to `error`
  and allocates nothing beyond the cap. ✔
  (`...OversizedFrameDrivesTheChannelToError`)
* The existing `tests/test_web_transport_data_channel.cpp` suite is
  updated to the framed contract, and the send/receive round-trip is
  unchanged from the caller's point of view. ✔ (the new
  `src/web_transport/tests/test_web_transport_dc_capability.cpp`
  suite drives the framed contract; the raw contract keeps its own
  cases).

All four cases live in
`src/web_transport/tests/test_web_transport_dc_capability.cpp`.

### WP-C — Multi-channel isolation

**Problem.** §1.1 / §1.2.2: channels of one engine share one QUIC session,
one session-level datagram callback slot, one peer-stream callback slot,
one engine-level message callback, and a message payload that carries no
channel identity.

**Fix.** Four sub-items, in dependency order.

* **C1 — Give the message callback a channel identity (API change).**
  Extend the data-channel message surface so a receiver can identify the
  source channel. Options, in preference order:
  1. *Additive — a new callback type.* Add
     `plugins::DataChannelMessage { std::string_view label; std::uint16_t channel_id; BufferView data; }`
     and a `set_on_message_ex()` on `IDataChannel`, keeping
     `set_on_message()` as the existing single-argument form. `IDataChannel`
     is classified **STABLE** (ADR-015), so this must be *additive* — the
     existing four-member contract must keep its exact signatures.
  2. *A separate registry of per-channel callbacks at the engine level.*
     `NimRTCEngine` gains `set_on_data_message_for(label, cb)`. More
     engine surface, and it does not fix the underlying session
     ownership.

  **Recommendation: option 1**, with the engine keeping
  `set_on_data_message()` as a fan-out shorthand that installs the same
  callback on every channel (today's behaviour, preserved).
* **C2 — One inbound route per channel, not per session.** The session's
  single `on_datagram_cb_` / `on_peer_stream_cb_` slots must become
  *multicast* (a list) or the channel must stop using the session-level
  slots entirely and route by stream id. The cleanest form:
  `IWebTransportSession` gains a *subscriber list* per callback kind, and
  `WebTransportDataChannel` subscribes on open and unsubscribes on close.
  A channel's inbound datagrams are then delivered to that channel only.
* **C3 — Peer-stream demultiplexing.** Today
  `WebTransportDataChannel::on_peer_stream()` calls `detach_peer_stream()`
  before adopting a new one — so a second peer stream *evicts* the first.
  With multiple channels, the peer stream must be routed to the channel
  that owns it. Since the WebTransport stream header carries a session id
  and not a channel id, the routing key must be the **stream id**
  (the QUIC stream's own identifier), carried in the NimRTC frame
  (WP-B) or in the channel's stream-type header. The frame is the
  natural home: add a channel id to the frame header, and both B and C
  become solvable together.
* **C4 — Close must not disable siblings.** `WebTransportDataChannel::close()`
  must unsubscribe only its own routes. Covered by the C2 subscriber list.

**Acceptance (all met — v1.2.0).**
* Unit test: two channels on one session; a datagram addressed to
  channel A surfaces on A's callback and **not** on B's. ✔
  (`WebTransportDataChannelIsolation.MessageGoesToItsOwnChannelOnly`,
  `...DatagramGoesToItsOwnChannelOnly`)
* Unit test: two channels; closing A leaves B receiving. ✔
  (`...ClosingOneChannelLeavesTheOtherAlive`)
* Unit test: two peer streams arrive; each is delivered to its own
  channel. ✔ (`...TwoPeerStreamsReachTheirOwnChannel`)
* Engine-level test: `create_data_channel("a")` and
  `create_data_channel("b")`; a message on "a" is delivered with
  channel identity "a". ✔
  (`DataChannelEngine.ChannelIdentityIsReportedPerChannel` in
  `tests/test_datachannel_engine.cpp`)
* The API-freeze scan (`scripts/api_freeze_scan.js`) stays green: the
  change is additive, and `tests/test_api_classification.cpp` picks up the
  new symbols with a classification. ✔ (`node scripts/api_freeze_scan.js`
  reports 0 missing markers; the new symbols are classified STABLE in
  `src/plugins/include/nimrtc/plugins/datachannel.hpp`.)

### WP-D — Partial reliability: correct the implementation

**Problem.** §1.1 DC-D: PR-TTL is a channel-lifetime deadline and PR-count
kills the channel on its first send when `max_retransmits == 0` (the
documented default). Both are wrong.

**Fix.** This is a **correction of wrong behaviour**, not a new feature,
which places it in the `v1.1.x` line under the ladder rule ("every change
must be a fix for something that is *wrong* in `v1.1.0`") — see §7 DEC-1.

* **D1 — PR-TTL becomes per-message.** Each message carries its own
  deadline, set at `send()` time, not one channel-wide deadline set at
  `open()` time. A message whose deadline has passed is dropped (that is
  what partial reliability means) and the **channel stays open**; the next
  message gets a fresh deadline. The expiry must not reset the stream.
* **D2 — PR-count becomes per-message retransmission accounting.** The
  counter is per message, decremented when a retransmission is needed,
  not per `send()`. `max_retransmits == 0` must mean *unlimited* (the
  documented meaning in `DataChannelConfig`), not *exhausted on the first
  send*. The `0xFFFF` unbounded sentinel becomes the explicit
  representation of "unlimited".
* **D3 — The SCTP approximation is named for what it is.** While the SCTP
  backend is a loopback test backend (WP-A), its
  `kPartialReliableCount` implementation must either be corrected or the
  mode documented as "TTL-approximated". If WP-A selects A1, the SCTP
  implementation becomes the *reference oracle* the WebTransport
  implementation is tested against — in which case its approximation is
  acceptable **as a test double**, and must be labelled as such.
* **D4 — The datagram/stream boundary.** `kUnreliable` stays on QUIC
  datagrams. `kPartialReliableTTL` and `kPartialReliableCount` need
  retransmission, which QUIC streams do not expose at the application
  layer: msquic handles the stream's own retransmission internally and
  gives the application no per-message retry hook. Partial reliability
  therefore requires either an application-level retransmit protocol
  (sequence numbers + ack, on top of the WP-B frame) or a QUIC datagram
  with an application-level retry window. **This is the second half of
  DEC-3 in §7.**

**Acceptance (all met — v1.2.0).**
* Unit test: PR-TTL channel, message sent at T, message sent at T+2×TTL —
  the second is dropped, the first was delivered, and the channel is
  **still open** afterwards (regression against the current
  permanently-dead behaviour). ✔
  (`WebTransportDataChannelReliability.ExpiredMessageIsDroppedAndTheChannelStaysOpen`)
* Unit test: PR-count channel with `max_retransmits = 0` sends and
  delivers successfully (regression against the current
  drop-on-first-send behaviour). ✔
  (`...UnlimitedRetransmitsStillDeliver`)
* Unit test: PR-count channel with `max_retransmits = 2` allows exactly
  two retransmissions of a given message and drops it after the third. ✔
  (`...RetransmitBudgetIsPerMessage`)
* The SCTP-side tests (`test_sctp_usrsctp.cpp`) stay green and are
  re-labelled as the oracle. ✔ (the SCTP backend stays in-tree as the
  reference implementation; the public *selector* was retired by WP-A,
  not the implementation.)

### WP-E — Priority reaching the data channel

**Problem.** §1.1 DC-E: `priority` is DCEP-encoded and otherwise ignored;
the scheduler's per-stream API has no caller in the data plane.

**Fix — and the honest caveat.** Two distinct problems hide under this row,
and only one of them is solvable:

* **E1 — `priority` is not settable and not used.** Add a public setter for
  `DataChannelConfig::priority` (the engine currently hard-codes 128 and
  offers no way to change it), and stop pretending the SCTP stream id
  derives from it. `s_stream_from_priority()` must either be implemented
  (one stream per priority bucket, as its own comment has promised since
  v0.12) or be deleted along with its comment. The latter is more honest
  while the SCTP backend is a loopback test backend.
* **E2 — the scheduler is not on the data path at all.** Under the default
  WebTransport backend, the media path (DTLS-SRTP over ICE/UDP) and the
  data path (a separate QUIC connection) run **independent congestion
  control**. The scheduler's per-stream buckets order packets *within one
  connection*; they cannot order across two connections. This is stated
  in `src/modules/sched/include/nimrtc/sched/sched.hpp` §"Scope note" and
  it means the honest form of DC-E under the current architecture is:
  *"the data channel has its own connection and therefore its own
  priority; the scheduler does not arbitrate between media and data."*
  Making the scheduler arbitrate across both planes requires **joint
  pacing above two connections** — a transport-layer project, not a
  data-channel project.

  The tractable version of E2: priority *within* the data plane. The
  WebTransport session's outbound datagram and stream sends can be
  ordered by the scheduler's per-stream buckets using the channel id as the
  stream id, so that a high-priority control channel is not queued behind
  a bulk-transfer channel on the same QUIC connection. That is a real,
  useful, and honest capability.

**Recommendation.** Ship E1 + the tractable E2 (intra-data-plane
ordering), and state the cross-plane limitation in the public docs rather
than claiming a wire-level guarantee the architecture cannot provide.
**This is what shipped in v1.2.0** — with one deviation: the ordering is
done by the **WebTransport session's own priority-source list** (a lower
priority value is offered to the transport first), not by the media
scheduler's per-stream buckets, because the media scheduler is not on
the data path at all (§E2).  The observable behaviour the plan asked
for is the same.

**Acceptance (all met — v1.2.0).**
* Public API test: a channel can be created with a non-default priority
  and the value round-trips to the backend. ✔
  (`DataChannelEngine.PriorityAndFramingReachTheBackend` in
  `tests/test_datachannel_engine.cpp`;
  `WebTransportDataChannelPriority.PriorityReachesTheSession`)
* Scheduler test: two channels on one session with priorities High and
  Low; the High channel's payload is offered to the transport first. ✔
  (`WebTransportDataChannelPriority.HigherPriorityIsOfferedFirst`)
* A documentation statement in `docs/api/engine_config.md` (or a new
  `docs/guides/datachannel.md`) that says explicitly what the priority
  does and does not guarantee. ✔
  (`docs/api/engine_config.md` §2.6.6 and `docs/guides/datachannel.md` §5)

---

## 3. Constraint compliance

| Constraint | How the plan satisfies it |
|---|---|
| **L1** `src/core/` is header-only and exposes only stable types | No `src/core/` change. The new channel-identity type lives in `src/plugins/` (WP-C1), which is already an INTERFACE library. |
| **L2** `src/plugins/` holds interfaces only | WP-C1's new struct is a type, not an implementation. WP-C2's subscriber list lives in the **WebTransport implementation** (`src/web_transport/`), not in `src/plugins/`. |
| **L3** modules are self-contained | WP-B / WP-C land in `src/web_transport/`; WP-D in `src/web_transport/` plus `src/modules/datachannel/`. |
| **L4** `src/engine/` orchestrates only | The engine's role in WP-C is limited to installing per-channel callbacks (C1) and to holding the channel registry. |
| **L5** vendored dependencies stay vendored | No new dependency. WP-A/A1 *removes* a vendored one (`usrsctp`) at `v1.2.0`, as already scheduled. |
| **L6** no global mutable state | The session's callback slots become a subscriber list guarded by the existing `cb_mutex_`. No new globals. |
| **L7** protected paths | `policy/`, `.zelto/`, `AGENTS.md` untouched. |

---

## 4. Verification matrix

| Claim | Test | File | Status |
|---|---|---|---|
| Message boundary preserved (B) | two sends → two callbacks; one 10 kB send → one callback; 1-byte chunk reassembly; oversized prefix → `error` | `src/web_transport/tests/test_web_transport_dc_capability.cpp` | ✔ |
| Channel isolation (C) | datagram to A not delivered to B; closing A leaves B alive; two peer streams → two channels | `src/web_transport/tests/test_web_transport_dc_capability.cpp` | ✔ |
| PR-TTL is per-message (D) | late message dropped, channel still open | `src/web_transport/tests/test_web_transport_dc_capability.cpp` + `src/modules/datachannel/tests/` | ✔ |
| PR-count semantics (D) | `max_retransmits == 0` delivers; `== 2` allows exactly two retries | `src/web_transport/tests/test_web_transport_dc_capability.cpp` | ✔ |
| Backend honesty (A) | default backend has a wire path; `"sctp"` is rejected or marked test-only | `tests/test_datachannel_engine.cpp` | ✔ |
| Priority is settable (E) | non-default priority round-trips | `tests/test_datachannel_engine.cpp` | ✔ |
| Intra-data-plane ordering (E) | High-priority channel offered before Low | `src/web_transport/tests/test_web_transport_dc_capability.cpp` | ✔ |
| Channel identity reaches the host (C) | `set_on_data_message_ex()` reports the right label and id | `tests/test_datachannel_engine.cpp` | ✔ |
| No API regression | freeze scan + classification test green | `scripts/api_freeze_scan.js`, `tests/test_api_classification.cpp` | ✔ |
| Chrome interop not regressed | Case E still passes after the framing change | `tools/run_e2e_acceptance.py --case e` | pending — the Chrome path runs in `kRaw` mode, so the frame never touches it; the harness must be re-run in CI |

---

## 5. Risks

| Risk | Mitigation |
|---|---|
| WP-B's framing breaks Chrome interop (Case E) | The framing decision is DEC-2 in §7 and is resolved **before** WP-B is implemented. If the answer is "NimRTC-only framing", the Chrome path is explicitly byte-stream mode and Case E's assertions are updated to match — not left red. |
| WP-C1's API change trips the freeze scan | The change is additive only; `IDataChannel`'s four existing member signatures are untouched. Baseline the freeze script before starting. |
| WP-C2's subscriber list touches the session's callback surface, which is `NIMRTC_API_UNSTABLE` | Acceptable: the WebTransport seam is explicitly unstable until the v1.0 line freezes, so this is the right place to change it. |
| WP-D's per-message retransmission needs an application-level retry protocol | DEC-3 in §7. If the owner declines the protocol, the honest fallback is to keep PR modes as *send-side policies only* (drop-old / drop-new at the sender) and to say so in the docs — no delivery guarantee is claimed. |
| WP-E's cross-plane limitation disappoints | The plan states the limitation up front rather than shipping a claim the architecture cannot back. The intra-data-plane ordering is the deliverable that is actually achievable. |
| The `v1.1.x` line carries WP-A and WP-D (behaviour changes) | Both are framed as corrections of wrong behaviour, which the ladder permits. Each needs its own regression test, and the ladder's rule is that a `v1.1.x` release must be `v1.1.0` plus fixes — so WP-A/WP-D must not pull in new public API. WP-A/A1 changes the *meaning* of an existing config value, which is a behaviour change that needs the owner's explicit blessing (DEC-1). |

---

## 6. Not in scope

* **SFU-style data-channel forwarding** (the `DC-3` item, closed as NO on
  2026-10-19 per `docs/plan/v1.0-plan.md` §7).
* **A second wire backend** beyond the decision in DEC-4 (WP-A).
* **Cross-connection joint pacing** between the media path and the data
  path — a transport-layer project, out of scope here (see E2).
* **The `cloudgame` official Profile**, deferred to `v1.2+` and unrelated
  to the data channel.

---

## 7. Open items — owner decisions

| ID | Question | Options | Recommendation | Status |
|---|---|---|---|---|
| **DEC-1** | Does the `v1.1.x` line carry the correctness fixes (WP-D) and the backend-honesty change (WP-A/A1)? | (a) yes — both are "fixes of something wrong in `v1.1.0`"; (b) no — push everything to `v1.2.0` | **(a)**, provided WP-A/A1 changes no public API and the `"sctp"` selector change is called out in the release notes | **decided 2026-11-10: (a)** — WP-A/A1 adds and removes no public symbol (it changes the *meaning* of two existing config values, which `CHANGELOG.md` `## [Unreleased]` spells out). The trunk has since moved onto the `v1.2.0` line (the audio body and the session/data-channel/stats surfaces all land there), so the change ships in `v1.2.0`; a `v1.1.x` backport is optional and would need its own release-notes entry. |
| **DEC-2** | How is the message boundary reconciled with Chrome's raw-byte WebTransport stream? | (a) negotiated framing (both peers agree); (b) NimRTC-only framing, Chrome path stays byte-stream; (c) no framing, document the loss | **(b)** — negotiation needs a wire format Chrome will not speak; (c) leaves DC-B open | **decided 2026-11-10: (b)** — implemented as `plugins::DataChannelFraming` with **two modes**: `kRaw` (the default — the payload reaches the transport verbatim, the Chrome-compatible byte stream) and `kFramed` (a length-prefixed frame that also carries the channel label). The mode is selected by `EngineConfig::datachannel_framing`; there is no on-the-wire negotiation, so **both ends must agree**. A framed peer cannot read a raw stream and vice versa. The frame layout and the isolation rules are documented in `docs/guides/datachannel.md` §2. |
| **DEC-3** | How is per-message partial reliability implemented on a transport with no per-message retry hook? | (a) application-level sequence/ack protocol on top of the frame; (b) QUIC datagram + application retry window; (c) send-side-only policy (drop-old / drop-new), no delivery guarantee | **(a)** if the product needs a delivery guarantee; **(c)** otherwise | **decided 2026-11-10: (c)** — the PR modes are **send-side policies** and the docs say so plainly: the TTL and the retransmit budget are per *message*, an expired message is dropped and **the channel stays open**, and neither mode is a delivery guarantee or an end-to-end acknowledgment protocol. A QUIC stream exposes no per-message retry hook (msquic handles the retransmission internally), so an application-level ack protocol (option (a)) is a separate project and is **not** claimed. |
| **DEC-4** | Which direction does WP-A take — retire SCTP from the public backend selector (A1), or build SCTP-over-DTLS (A2)? | A1 / A2 | **A1** — the default backend already has Chrome interop evidence; A2 re-opens a closed decision from `v0.11-plan.md` §2.2 | **decided 2026-11-10: A1** — implemented: `datachannel_backend = "sctp"` (and the legacy `datachannel_name = "sctp"` spelling) now fails `open()` with `kEngineInternal` and a message that names the backend as **test-only**; `EngineConfig::datachannel_name` now defaults to `""`. The `SctpDataChannelFactory` stays registered as the in-tree reference implementation (the PR-SCTP oracle WP-D needs); only the public *selector* was retired. |
| **DEC-5** | Which release line carries WP-B, WP-C, WP-E? | (a) `v1.2.0` — alongside the video Part B and audio WP-0…WP-11 body; (b) `v1.3.0` — a dedicated data-channel release | **(a)** — `v1.2.0` is already the first feature release and these are the gaps most visible to integrators; but the line is already large | **decided 2026-11-10: (a)** — WP-B, WP-C and WP-E ship in `v1.2.0` (commit `f04ff2c`), alongside the session / DTMF / ICE-restart / pacing bodies. The gap table was the most visible one to integrators, so the release line carries it rather than deferring it to `v1.3.0`. |

---

## 8. Definition of done

The data channel is considered to have closed the gap table when:

1. The table in §1 reflects reality: DC-A is truthful about which
   backends reach a peer; DC-B, DC-C, DC-D, DC-E are marked as delivered.
   ✔ (the §1 table carries a "NimRTC v1.2.0 (current)" column)
2. Every row has a green regression test in §4's matrix, and the test
   fails against `v1.1.0` for the bug-shaped items (WP-D). ✔
3. The public docs state what the data channel guarantees and what it does
   not — message boundary, channel isolation, partial reliability,
   priority — with no claim the architecture cannot back. ✔
   (`docs/guides/datachannel.md`, `docs/api/engine_config.md` §2.6.5 /
   §2.6.6, `docs/api/engine_api.md` §10.1)
4. The Chrome interop case (Case E) is green, with its assertions matched
   to the DEC-2 outcome. — the Chrome path runs in `kRaw` mode, so the
   frame never reaches it; the harness re-run in CI is the last open
   item.

---

## 9. References

- `src/plugins/include/nimrtc/plugins/datachannel.hpp` — the `IDataChannel`
  contract, `DataChannelConfig`, the STABLE classification
- `src/web_transport/src/web_transport_data_channel.cpp` — the WebTransport
  channel: framing absence (WP-B), session slot overwrite and
  close-disables-siblings (WP-C), PR-TTL / PR-count defects (WP-D)
- `src/web_transport/src/web_transport_msquic_session.hpp` — the single
  `on_datagram_cb_` / `on_peer_stream_cb_` slots (WP-C2)
- `src/modules/datachannel/src/sctp_data_channel.cpp` —
  `s_stream_from_priority()` (WP-E1), the count-mode TTL approximation
  (WP-D3)
- `src/sctp/src/usrsctp_socket.cpp` — the loopback-only SCTP binding
  (WP-A)
- `src/engine/src/engine.cpp` — `create_data_channel()`, the shared
  `on_data_message_` install, the hard-coded `priority = 128` (WP-C1, WP-E1)
- `src/engine/src/engine_transport.cpp` — the un-implemented SCTP-over-DTLS
  wiring (WP-A)
- `src/engine/src/engine_signaling.cpp` — `a=sctp-port`, `a=max-message-size`
  (WP-B4)
- `src/modules/sched/include/nimrtc/sched/sched.hpp` — the per-stream
  (S-family) priority API and its cross-plane scope note (WP-E2)
- `src/modules/sched/tests/test_sched_priority_dc.cpp` — the DC-2 test that
  exercises the scheduler without the data plane (WP-E)
- `docs/plan/v1.1-plan.md` §5.6 — the version ladder
- `docs/plan/v0.11-plan.md` §2.2 / §10 — the SCTP → WebTransport re-design
- `docs/plan/v1.0-plan.md` §2.1.1 / §7 — DC-1, DC-2, the SFU closure
- ADR-014 (WebTransport vendor), ADR-015 (API stability)
