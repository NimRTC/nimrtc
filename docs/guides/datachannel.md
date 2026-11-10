# The Data Channel — What It Guarantees

> **Audience**: integrators that use the NimRTC data channel to carry
> control messages, telemetry, or bulk transfer alongside a call.
>
> **Status**: v1.2.0. Covers the WebTransport / QUIC data plane, the
> framing modes, channel identity, partial reliability, and priority.
> See [`docs/plan/datachannel-capability-plan.md`](../plan/datachannel-capability-plan.md)
> for the work packages that produced it.

---

## 1. The backend

The data plane rides **its own QUIC connection**, separate from the
ICE/DTLS 5-tuple that carries media. That single fact explains most
of the behaviour described below.

```cpp
nimrtc::engine::EngineConfig cfg;
cfg.datachannel_backend = "web_transport";   // the default
```

`"web_transport"` is the only backend that can reach a peer. The
`"sctp"` backend was retired from the public selectors in v1.2.0 — it
opened an in-process `usrsctp` loopback socket that never left the
process, so a channel created on it could never talk to a remote
participant. See
[`docs/api/engine_config.md` §2.6.0](../api/engine_config.md) for the
dispatch table.

---

## 2. Message boundary

A QUIC stream is a **byte stream**: the transport may split one
`send()` into several chunks, or coalesce several `send()`s into one.
NimRTC therefore offers two framing modes, and you pick one with
`EngineConfig::datachannel_framing`.

### 2.1 `kRaw` (default)

```cpp
cfg.datachannel_framing = nimrtc::plugins::DataChannelFraming::kRaw;
```

The payload reaches the transport verbatim. Consequences:

- two `send()` calls of 10 bytes each may surface as **one** 20-byte
  callback;
- one `send()` of 10 bytes may surface as **two** 5-byte callbacks;
- a byte stream carries **no channel identity**, so two channels on
  the same connection cannot be told apart by the payload alone.

This is the mode Chrome's `RTCDataChannel`-over-WebTransport speaks,
and it is the default so that existing deployments keep working
unchanged.

### 2.2 `kFramed`

```cpp
cfg.datachannel_framing = nimrtc::plugins::DataChannelFraming::kFramed;
```

Every payload is wrapped in a length-prefixed frame that also carries
the channel label:

```text
  0            4            8                        8+label_len
  +------------+------------+------------------------+-------------+
  | u32 BE     | u32 BE     | label bytes (UTF-8)    | payload     |
  | label_len  | payload_len|                        | bytes       |
  +------------+------------+------------------------+-------------+
```

- one `send()` == one inbound message, whatever the transport does to
  the bytes in between;
- the frame carries the channel identity, which is what makes several
  channels on one connection isolable;
- a length prefix larger than the negotiated maximum is a **protocol
  error**: the channel transitions to `error` instead of allocating an
  unbounded buffer.

**Both ends must agree.** The framing is a NimRTC-internal convention
(plan DEC-2 = (b)); there is no on-the-wire negotiation, because
Chrome will not speak one. A framed peer cannot read a raw stream and
vice versa.

If you need message boundaries with Chrome, keep `kRaw` and put your
own length prefix inside the payload.

---

## 3. Channel identity

The classic `set_on_data_message()` callback carries the payload only,
so a host that opens two channels cannot tell which one a message
arrived on. v1.2.0 adds the additive
`set_on_data_message_ex()`:

```cpp
engine.set_on_data_message_ex(
    [](const nimrtc::plugins::DataChannelMessage& msg) {
        // msg.label      — the channel's label
        // msg.channel_id — the engine-assigned id (1-based, per engine)
        // msg.data       — the payload (same bytes the plain callback gets)
    });
```

The plain `set_on_data_message()` keeps its exact signature and
behaviour: it is installed on every channel the engine creates, as a
fan-out shorthand.

With `kFramed`, the *peer's* label decides which channel a message
belongs to. With `kRaw` there is no identity on the wire, so every
channel of the connection sees the same bytes — use `kFramed` (or one
channel per connection) if you need isolation.

---

## 4. Partial reliability

`plugins::DataChannelReliability` selects one of four modes. On the
WebTransport backend:

| Mode | Behaviour |
|---|---|
| `kReliableOrdered` | Stream send; the QUIC stream retransmits internally. |
| `kUnreliable` | QUIC datagram; message-preserving by definition, no retransmission. |
| `kPartialReliableTTL` | Each **message** carries its own deadline (`max_packet_lifetime_ms`). An expired message is dropped; **the channel stays open**. |
| `kPartialReliableCount` | Each **message** carries its own retransmission budget (`max_retransmits`). `max_retransmits == 0` means **unlimited**, not "dropped on the first send". |

Two semantics worth stating explicitly, because v1.1.0 got both wrong:

- the deadline and the budget are **per message**, not per channel. A
  message that outlives its budget is dropped and the *next* message
  gets a fresh one — the channel does not die;
- `max_retransmits == 0` is the documented "unlimited" value.

**What is not claimed.** A QUIC stream has no per-message retry hook
at the application layer — msquic handles retransmission internally
and exposes no per-message retry callback. The budgets above are
therefore *send-side* policies: they decide whether a queued message
is still worth offering to the transport. They are **not** a
delivery guarantee and not an end-to-end acknowledgment protocol
(plan DEC-3 = (c)).

---

## 5. Priority

```cpp
cfg.datachannel_priority = 16;   // lower value = higher priority
```

The value reaches the backend and orders the **data plane's own**
outbound queue: a high-priority control channel is not stuck behind a
bulk-transfer channel on the same QUIC connection.

**What it does not do.** The media plane and the data plane run
independent congestion control on two different connections, so the
scheduler cannot arbitrate between them. The media scheduler's
per-stream priority API is not on the data path at all. The honest
statement is: *the data channel has its own connection and therefore
its own priority; the scheduler does not arbitrate between media and
data* (plan WP-E2).

Priority inside the SCTP backend does **not** select an SCTP stream:
RFC 8832 negotiates the stream id inside the DCEP handshake, so a
channel must use the id it announced. The SCTP backend is test-only
anyway — see §1.

---

## 6. Quick reference

```cpp
#include <nimrtc/engine/engine.hpp>

nimrtc::engine::EngineConfig cfg;
cfg.datachannel_backend = "web_transport";
cfg.datachannel_framing = nimrtc::plugins::DataChannelFraming::kFramed;
cfg.datachannel_priority = 16;

nimrtc::engine::NimRTCEngine engine(cfg);
engine.set_on_data_message_ex(
    [](const nimrtc::plugins::DataChannelMessage& msg) {
        // msg.label / msg.channel_id / msg.data
    });
engine.open();

auto ctl = engine.create_data_channel("control");
ctl->send(nimrtc::plugins::BufferView{payload, size});
```

---

## 7. See also

- [`docs/api/engine_config.md` §2.6](../api/engine_config.md) — the
  backend dispatch table and the `datachannel_*` config fields
- [`docs/api/engine_api.md`](../api/engine_api.md) — `create_data_channel()`
  and the data-message callbacks
- [`docs/plan/datachannel-capability-plan.md`](../plan/datachannel-capability-plan.md) —
  the work packages (WP-A…WP-E) and the owner decisions (DEC-1…DEC-5)
