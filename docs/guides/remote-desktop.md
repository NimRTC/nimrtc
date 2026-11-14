# Remote Desktop — The Signalling Seam

> **Audience**: an engineer writing the signalling service that connects a
> remote-desktop **device** (the machine whose screen is shared) to a
> remote-desktop **viewer** (the operator's laptop).
>
> **Status**: v1.3.0 (plan Track I, items I1 / I2). This guide is the
> **canonical description** of the exchange. The reference implementation
> in [`examples/remote-desktop/`](../../examples/remote-desktop/) is its
> executable form; the plan's decision RD-7 keeps that service in
> `examples/` and **not** in `src/`.
>
> **Scope**: the signalling plane only. The media plane (screen capture,
> encode, transport) and the control plane (input events) are described
> where they are implemented; §7 points at the canonical code.

---

## 1. What the engine needs, and what it does not

NimRTC ships a signalling **seam**, not a signalling server (RD-7). The
engine gives a host everything it needs to *carry* an SDP / candidate
exchange and nothing about *who* carries it:

| Engine surface | Signature | Purpose |
|---|---|---|
| `NimRTCEngine::create_offer()` | `std::string create_offer() noexcept` | Produce the local offer SDP (RFC 8829). |
| `NimRTCEngine::process_remote_sdp()` | `std::optional<std::string> process_remote_sdp(std::string_view remote_sdp) noexcept` | Consume the peer's offer **or** answer; returns the local answer when the peer sent an offer. |
| `NimRTCEngine::add_turn_server()` | `int add_turn_server(std::string_view host, std::uint16_t port, std::string_view username, std::string_view password) noexcept` | Register a TURN relay (before `open()`). An overload takes a `plugins::TurnTransport`. |
| `NimRTCEngine::create_data_channel()` | `std::unique_ptr<plugins::IDataChannel> create_data_channel(std::string_view label) noexcept` | Allocate the control-plane channel. |
| `NimRTCEngine::restart_ice()` | `bool restart_ice() noexcept` | Perform an ICE restart (RFC 8445 §9.1.1.1). |
| `NimRTCEngine::set_on_ice_restart()` | `void set_on_ice_restart(plugins::OnIceRestart cb) noexcept` | Fired when the **peer** restarted ICE; the host must answer. |
| `NimRTCEngine::local_ufrag()` / `local_password()` | `std::string …() const noexcept` | The local ICE credentials (for diagnostics and for verifying a restart). |
| `NimRTCEngine::get_ice_transport()` | `plugins::IICETransport* get_ice_transport() noexcept` | The plugin-seam handle to the ICE transport. |

All of the above live in
[`src/engine/include/nimrtc/engine/engine.hpp`](../../src/engine/include/nimrtc/engine/engine.hpp).
The **candidate** surface is *not* on the engine — it is on the ICE
plugin seam, which the engine hands out:

```cpp
#include <nimrtc/plugins/ice_transport.hpp>

auto* ice = engine.get_ice_transport();          // non-owning
ice->wait_for_gathering(2000);                    // ms
for (const auto& c : ice->gathered_local_candidates()) {
    // c is a complete SDP attribute line, e.g.
    //   a=candidate:1 1 UDP 2122260223 192.168.1.20 54321 typ host
    send_to_peer(json_candidate(c));
}
ice->add_remote_candidate("a=candidate:…");       // inbound trickle
```

> **Do not strip the `a=` prefix.** `gathered_local_candidates()`
> returns complete `a=candidate:` attribute lines. libjuice parses a
> candidate against the `a=candidate:` prefix; stripping it corrupts
> the foundation field and the candidate is rejected.

Everything the engine does **not** provide, and a signalling service
therefore must:

- decide **who offers** (see §2),
- carry the SDP and the candidates between the two peers,
- identify the peers (pairing, §6),
- issue TURN credentials (§5),
- relay the ICE-restart and reconnect notifications (§4, §6).

---

## 2. The offer / answer exchange

### 2.1 The viewer offers, the device answers

**In the remote-desktop product the _viewer_ is the offerer and the
_device_ is the answerer.** This is the opposite of the usual
"the more capable side offers" heuristic, and the reason is
deliberate:

1. **The device is a server-shaped peer.** It sits behind the user's
   own NAT, on a network the operator does not control, and it is
   already the side that is *listening* for a pairing request. Making
   it the answerer means the device never has to reach out to the
   operator's address — the operator's client is the one that opens
   the outgoing path.
2. **The viewer is the side with the ephemeral state.** The viewer's
   SDP is generated from a fresh, per-session engine that is opened
   *after* the pairing succeeded, so `create_offer()` is the first
   thing that happens on it. The device's engine, by contrast, is
   long-lived and shared with the device's other duties; it should not
   have to hold a pending offer while an operator types a pairing
   code.
3. **The device's answer is a pure function of the offer.** The device
   contributes media (the screen) and the operator contributes
   control. The direction of the media flow is fixed by the product,
   so the offerer's SDP can be the viewer's, and the answer is the
   device's.
4. **It matches the transport strategy.** The remote-desktop profile
   follows the `teleop-ultra-low-latency` shape from
   [`docs/plan/transport-strategies.md`](../plan/transport-strategies.md):
   media on the `webrtc-classic` stack, a high-frequency control plane
   on its own stack. The viewer drives the session; the device
   responds.

### 2.2 The sequence

```
   viewer                                             device
     |                                                  |
     |-- pairing_request --------------------------->  |   (device side; see §6)
     |<-- pairing_code --------------------------------|
     |-- device_register(code) -----------------------> |
     |<-- device_registered {credential, room_id} ----|
     |                                                  |
     |-- join {credential} -------------------------->  |
     |<-- joined {role:"viewer", peer_present:false} --|
     |                                                  |
     |== media plane (ICE / DTLS / SRTP) ==============|
     |                                                  |
     |-- create_offer()  ->  offer {sdp} -------------> |   §3
     |<-- answer {sdp} ---------------------------------|
     |                                                  |
     |-- candidate {candidate} -----------------------> |   §4 (trickle, both ways)
     |<-- candidate {candidate} ------------------------|
```

On the device side the roles are mirrored: the device receives the
offer and calls `process_remote_sdp()`, which returns the answer.

### 2.3 The engine calls, exactly

**Viewer (offerer):**

```cpp
// 1. Configure the transport.  The STUN server goes into the config
//    before the engine is constructed; the TURN server is added
//    *after* pre_open() (the ICE transport has to exist) and *before*
//    open() (that is when gathering starts).
nimrtc::engine::EngineConfig cfg;
cfg.stun_server_host = "stun.l.google.com";
cfg.stun_server_port = 19302;
nimrtc::engine::NimRTCEngine engine(cfg);

engine.pre_open();          // creates the ICE transport, does not gather yet
engine.add_turn_server(turn_host, turn_port, turn_user, turn_pass);
engine.open();              // starts ICE gathering

// 2. Offer.
const std::string offer = engine.create_offer();
signalling_send({"type": "offer", "sdp": offer});

// 3. Candidates, once gathering has settled.
auto* ice = engine.get_ice_transport();
ice->wait_for_gathering(2000);
for (const auto& c : ice->gathered_local_candidates()) {
    signalling_send({"type": "candidate", "candidate": c,
                     "sdpMid": "0", "sdpMLineIndex": 0});
}

// 4. The peer's answer.
const auto answer = engine.process_remote_sdp(answer_sdp);
if (!answer) { /* the peer sent a bad SDP */ }
```

**Device (answerer):**

```cpp
nimrtc::engine::EngineConfig cfg;
cfg.stun_server_host = "stun.l.google.com";
cfg.stun_server_port = 19302;
nimrtc::engine::NimRTCEngine engine(cfg);

engine.pre_open();          // the ICE transport must exist before add_turn_server()
engine.add_turn_server(turn_host, turn_port, turn_user, turn_pass);
engine.open();              // gathering starts here

const auto answer = engine.process_remote_sdp(offer_sdp);   // → the answer
signalling_send({"type": "answer", "sdp": *answer});

// …then the same candidate exchange, in the opposite direction.
```

`process_remote_sdp()` returns the answer **only when the peer sent an
offer**. When the peer sent an answer (the offerer applying it), the
return value is empty and `engine.needs_negotiation()` tells the host
whether it has to produce a new offer.

> **`pre_open()` before `open()`.** `pre_open()` creates the ICE
> transport *without* starting candidate gathering, which is what lets
> the answerer learn the offerer's ICE credentials (`set_remote_ice()`)
> before its own agent starts. It also lets `add_turn_server()` land
> before gathering — **TURN servers must be registered before
> `open()`**, because that is when libjuice performs the Allocate
> request. A TURN server added after `open()` is silently useless.

### 2.4 Renegotiation

Neither side re-offers during a normal session. The two events that do
change the SDP are an **ICE restart** (§6) and a **track change**; both
are described where they occur. `NimRTCEngine::needs_negotiation()`
is the flag to poll after any mutating call.

---

## 3. The signalling message shapes

Every message is a JSON object with a `type` field. The reference
service relays the media-plane messages verbatim and stamps a `from`
field with the sender's role (`"device"` / `"viewer"`).

| `type` | Direction | Fields | Meaning |
|---|---|---|---|
| `pairing_request` | device → service | — | Ask for a pairing code. |
| `pairing_code` | service → device | `code`, `device_id`, `expires_at` | The short-lived code. |
| `device_register` | device → service | `code` | Bind the device to the code. |
| `device_registered` | service → device | `device_id`, `room_id`, `credential`, `expires_at` | The device's session credential. |
| `pair` | viewer → service | `code` | Exchange the code for a session credential. |
| `paired` | service → viewer | `credential`, `room_id`, `device_id`, `expires_at` | The viewer's session credential. |
| `join` | either → service | `credential` | Join the room. |
| `joined` | service → peer | `role`, `room_id`, `device_id`, `peer_present` | Room membership. |
| `peer_joined` / `peer_left` | service → peer | `role` | The other side appeared / vanished. |
| `offer` | viewer → device | `sdp` | The offer SDP. |
| `answer` | device → viewer | `sdp` | The answer SDP. |
| `candidate` | either → the other | `candidate`, `sdpMid`, `sdpMLineIndex` | One ICE candidate (trickle). |
| `ice_restart` | either → the other | `sdp` | A re-offer carrying new ICE credentials. |
| `reconnect` | either → the other | — | "My network changed; expect a new offer." |
| `turn_request` | either → service | — | Ask for a TURN credential. |
| `turn_credentials` | service → peer | `url`, `username`, `password`, `ttl`, `expires_at` | The issued TURN credential. |
| `bye` | either → service | — | Leave the room. |
| `error` | service → peer | `code`, `message` | A refusal. |

The `sdpMid` / `sdpMLineIndex` fields on `candidate` exist so that a
browser peer can call `RTCPeerConnection.addIceCandidate()` with the
association it demands. NimRTC-to-NimRTC ignores them, but sending
them costs nothing and makes the same service usable for both.

### 3.1 The exact candidate shape

```json
{
  "type": "candidate",
  "candidate": "a=candidate:1 1 UDP 2122260223 192.168.1.20 54321 typ host",
  "sdpMid": "0",
  "sdpMLineIndex": 0
}
```

The `candidate` value is the **complete SDP attribute line**, including
the `a=` prefix. See §1 for why the prefix must not be stripped.

---

## 4. ICE candidate trickle, in both directions

Trickle is symmetric: both peers send their candidates as ICE produces
them, and both peers install the other's with
`IICETransport::add_remote_candidate()`.

**Direction 1 — viewer → device.** The viewer sends `candidate`
messages as `gathered_local_candidates()` fills up. The device
installs each one:

```cpp
engine.get_ice_transport()->add_remote_candidate(line);   // "a=candidate:…"
```

**Direction 2 — device → viewer.** Identical, with the roles
reversed. The device's candidates are the ones that matter most in
the common case, because the device is the peer behind the NAT.

Two properties of the engine make this safe:

- `add_remote_candidate()` **remembers** every candidate it accepted
  (`impl_->remote_candidates_`), so an ICE restart re-applies them to
  the new agent without the host re-sending them.
- A candidate that arrives **before** the remote description does is
  queued by the ICE transport rather than dropped.

**Ordering.** The SDP must be applied before the candidates that
belong to it. The reference service therefore does **not** buffer
relayed messages for a peer that has not joined yet: a stale offer
must never be applied after an ICE restart. If the peer is absent, the
service answers `{"type":"error","code":"peer_absent"}` and the sender
retries once the peer is present.

**End-of-candidates.** The engine has no explicit
`a=end-of-candidates` marker in the trickle path: gathering completion
is observed locally (`wait_for_gathering()`), and the absence of more
candidates is the signal. A service that wants an explicit marker may
add one, but the reference service does not.

---

## 5. TURN credential issuance and refresh

### 5.1 The algorithm

The reference service issues **RFC 7635 style** ephemeral TURN
credentials. The shared secret is a byte string known to the
signalling service and to the TURN server, and to nobody else.

```
username = "<expiry-unix-timestamp>:<user-id>"
password = base64( HMAC-SHA1( shared_secret, username ) )
```

- The **expiry timestamp** is embedded in the username, so the TURN
  server can reject an expired credential without any state, and
  without a revocation round trip to the signalling service.
- The **HMAC** binds the password to the username. A client that
  tampers with either half produces a password that does not match.
- The **user-id** scopes the credential to one peer (`role:peer-id`),
  so the credential can be traced and, in a deployment that wants it,
  revoked per session.

The TURN server's own check is the mirror image of the issuance:

```
expected = base64( HMAC-SHA1( shared_secret, username_from_request ) )
accept   if  expected == password_from_request  and  now < timestamp
```

### 5.2 The parameters

| Parameter | Value in the reference service | Notes |
|---|---|---|
| Shared secret | 32 random bytes, or `$NIMRTC_TURN_SECRET` | Regenerated per run unless pinned. |
| TTL | 3600 s (1 h) | `--turn-ttl`. |
| URL | `turn:127.0.0.1:3478?transport=udp` | `--turn-url`, `$NIMRTC_TURN_URL`. |
| Username format | `<expiry>:<user-id>` | Expiry is an integer Unix timestamp. |

### 5.3 When the client asks, and when it re-asks

`add_turn_server()` is a **pre-`open()`** call, so the credential has
to be in hand *before* the ICE transport starts gathering. The flow is
therefore:

1. The client connects and authenticates (pairing, §6).
2. The client sends `{"type":"turn_request"}`.
3. The service answers `turn_credentials`.
4. The client calls `engine.add_turn_server(host, port, username,
   password)` **before** `open()`.

> **A TURN credential cannot be refreshed in place.** The ICE transport
> reads the TURN configuration when it creates its agent, so a
> *new* credential does not retro-actively replace the old one on an
> open transport. Refreshing means: **ICE restart**. The peer
> re-offers with a new `ice-ufrag` / `ice-pwd`, the transport is
> rebuilt, and the fresh TURN credential is configured on the new
> agent. That is why the TTL and the refresh cadence are tied to the
> ICE-restart cadence in §6.3.

In other words: **request the TURN credential early** (before the
first `open()`), and **re-request it together with every ICE restart**.

### 5.4 Consuming the credential

```cpp
engine.pre_open();                        // the ICE transport must exist
engine.add_turn_server(turn_host, turn_port, turn_username, turn_password);
engine.open();
```

The overload with an explicit transport is
`add_turn_server(host, port, username, password, plugins::TurnTransport)`.
Note that the vendored libjuice has **no TURN/TLS** path (RFC 6062);
`plugins::TurnTransport::Tls` is reported as unsupported by
`turn_transport_supported()`. TURN over UDP and TCP work.

---

## 6. The session lifecycle

### 6.1 Pairing: the credential is the identity

The identity of a peer is **the session credential it holds**, not the
address it connects from. A NAT rebind, a DHCP lease change, a VPN
that rewrites the source port — none of them change the identity.

The flow:

1. **The device asks for a pairing code.**
   `{"type":"pairing_request"}` → the service answers
   `{"type":"pairing_code","code":"123456","device_id":"dev-…","expires_at":…}`.
   The code is **six decimal digits** and **short-lived** (the
   reference service: 300 s). It is shown on the device's screen or
   read out of its settings page.
2. **The device registers with the code.**
   `{"type":"device_register","code":"123456"}` → the service answers
   `{"type":"device_registered","device_id":…,"room_id":"room-…","credential":"cred-…"}`.
   Registering does **not** consume the code — the operator still needs
   it. A code can register exactly one device.
3. **The operator exchanges the code for a session credential.**
   `{"type":"pair","code":"123456"}` → the service answers
   `{"type":"paired","credential":"cred-…","room_id":"room-…"}`.
   This **consumes** the code: it cannot be replayed.
4. **Both peers join** with `{"type":"join","credential":"cred-…"}`.

A room holds exactly **one device** and **one viewer**. A second peer
with the same role is refused with `room_full`. An unknown or expired
credential is refused with `unauthorized`.

Session credentials expire (the reference service: 3600 s). An expired
credential is refused; the operator re-pairs.

### 6.2 ICE restart — "my network changed"

The device (or the viewer) detects a network change — the operator's
laptop moving from Wi-Fi to Ethernet is the canonical case — and
restarts ICE.

**Who initiates.** Either side may initiate. The initiator is the one
whose network changed; the *other* side is the answerer of the restart
offer, per RFC 8445 §9.1.1.1, which requires **both** sides to change
their credentials.

**The device's side (the side that changed):**

```cpp
if (!engine.restart_ice()) {
    // no ICE transport yet, or the restart failed
}

// restart_ice() regenerates the local ICE credentials and re-gathers
// the candidates.  create_offer() picks the new credentials up.
auto* ice = engine.get_ice_transport();
ice->wait_for_gathering(2000);

const std::string offer = engine.create_offer();   // new ice-ufrag / ice-pwd
signalling_send({"type": "ice_restart", "sdp": offer});

// …and the new candidates, as in §4.
for (const auto& c : ice->gathered_local_candidates()) {
    signalling_send({"type": "candidate", "candidate": c});
}
```

**What changes in the SDP.** The `a=ice-ufrag:` and `a=ice-pwd:`
attributes in the media section carry **new values** — a restart is
observed by the peer *only* through the changed credentials. The
`a=fingerprint:` (DTLS) does **not** change: the DTLS association is
reused, and the SRTP keys are unaffected. The m-lines and the codecs
are unchanged unless the peer also changed them.

**The peer's side.** The peer applies the restart offer through
`process_remote_sdp()`. Inside that call the ICE transport notices the
changed remote credentials, regenerates its own (RFC 8445 §9.1.1.1
requires both sides to change), and fires the ICE-restart callback.
**The answer the same `process_remote_sdp()` call returns already
carries the new local credentials** — the engine builds the answer
*after* the transport has rebuilt itself.

So the host's job is only to send the returned answer:

```cpp
const auto answer = engine.process_remote_sdp(restart_offer);
if (answer) {
    // The answer carries our NEW ice-ufrag / ice-pwd, because the
    // transport regenerated them while applying the peer's offer.
    signalling_send({"type": "answer", "sdp": *answer});
}
```

The ICE-restart callback is the *notification* that this happened, so
the host can log it, update its telemetry, or re-request a TURN
credential for the new session:

```cpp
engine.set_on_ice_restart([]() {
    // The peer restarted ICE and we regenerated our credentials.
    // Nothing has to be sent from here — the answer produced by the
    // process_remote_sdp() call carries the new credentials.
    log("ICE restart: the peer changed the network");
});
```

> **Do not call `process_remote_sdp()` a second time from inside the
> callback.** The callback fires *during* the first call, and that
> call already produces the answer. A second application would apply
> the same offer twice.

`NimRTCEngine::ice_restart_count()` reports how many restarts the
transport has performed — the product's telemetry uses it to decide
whether the session is still healthy.

**The known remote candidates are re-applied automatically** by the
transport (they are remembered from `add_remote_candidate()`), so the
peer does not have to re-send them.

### 6.3 The `reconnect` path

`reconnect` is the *notification* that precedes a restart. It carries
no SDP; it says "my network is about to change, or has just changed,
expect a fresh offer".

```
   viewer (network changed)                          device
     |                                                  |
     |-- reconnect ------------------------------------> |   "expect a new offer"
     |                                                  |
     |== (the device or the viewer restarts ICE) =======|
     |                                                  |
     |-- ice_restart {sdp} ---------------------------> |   new ice-ufrag/pwd
     |<-- answer {sdp} ---------------------------------|
     |                                                  |
     |-- candidate {candidate} ------------------------> |   new candidates
     |<-- candidate {candidate} -------------------------|
```

The two sides do not have to agree on *who* restarts. The reference
service relays `reconnect` in both directions and lets whichever side
detected the change drive the restart. The important invariants:

- The **credential is untouched**. A reconnect is *not* a re-pair.
  The operator's session survives the network change without typing
  the pairing code again — that is the whole point of making the
  credential, and not the address, the identity (§6.1).
- The **room is untouched**. Both peers stay in the same room.
- A **stale offer must not be applied**. The service refuses to
  buffer media-plane messages for an absent peer (§4), so an offer
  from before the network change can never land after it.

A reconnect is also the moment to **re-request the TURN credential**
(§5.3): the new ICE session is the moment the TURN configuration is
re-applied, so the new credential goes in with it.

### 6.4 Session limits

The reference service is minimal and does not enforce the product's
session limits (plan item I6): maximum concurrent sessions, session
timeouts, idle disconnect, and an administrative kill switch are
product policy, and they belong in the service a product ships, not in
the reference example.

---

## 7. The control plane

### 7.1 The transport

In the remote-desktop product the **input events ride a DataChannel**.
The channel is created with the engine's DataChannel seam:

```cpp
auto dc = engine.create_data_channel("rd-input");
```

and its messages arrive through the engine's data callbacks
(`set_on_data_message()` / `set_on_data_message_ex()`).

> **The end state is a separate transport.** Plan RD-5 requires the
> control plane to have its own congestion-control domain — the
> `teleop-ultra-low-latency` strategy shape from
> [`docs/plan/transport-strategies.md`](../plan/transport-strategies.md),
> with the control path on its own `raw-udp-arq` stack and its own ICE
> selected pair. The DataChannel path is the **interim** one (plan
> item G1), explicitly a stepping stone. The *signalling* is the same
> in both cases, which is why the control plane needs nothing from the
> signalling service beyond what §4 already carries.

### 7.2 The payload format

The payload is **not** JSON. The canonical encoding lives in
[`src/modules/rd_input/`](../../src/modules/rd_input/) — plan item G3:

| Header | File | What it carries |
|---|---|---|
| `input_event.hpp` | [`include/nimrtc/rd_input/input_event.hpp`](../../src/modules/rd_input/include/nimrtc/rd_input/input_event.hpp) | The in-memory `InputEvent` struct and its `InputEventType` tags (`kMouseMove`, `kMouseButton`, `kMouseWheel`, `kKey`, `kText`). |
| `event_codec.hpp` | [`include/nimrtc/rd_input/event_codec.hpp`](../../src/modules/rd_input/include/nimrtc/rd_input/event_codec.hpp) | The **wire format** — the canonical encoding. |
| `coalescer.hpp` | [`include/nimrtc/rd_input/coalescer.hpp`](../../src/modules/rd_input/include/nimrtc/rd_input/coalescer.hpp) | Pointer coalescing to the display rate and the input path's own rate limit. |
| `safety_filter.hpp` | [`include/nimrtc/rd_input/safety_filter.hpp`](../../src/modules/rd_input/include/nimrtc/rd_input/safety_filter.hpp) | The allow / deny list that keeps a dangerous key combination off the device. |

The wire format, in brief:

```
  byte 0 : format version (currently 1)
  byte 1 : event type   (InputEventType)
  byte 2 : flags        (bit 0 = "a timestamp follows")
  ...    : type-specific body (LEB128 varints, zigzag for signed values)
```

A mouse-move delta is a handful of bytes. The version byte is first,
so a decoder rejects an event it does not understand before parsing
anything else. A *batch* frames a list of events as
`varint count` followed by the events, each independently decodable.

The encoder and decoder are `nimrtc::rd_input::encode_event()` /
`decode_event()` / `encode_batch()` / `decode_batch()`, in the
`nimrtc::rd_input` namespace.

### 7.3 The `ref_frame` binding

The product promise is *"the click happened on the frame the operator
was looking at"*. The engine already owns the machinery
(plan item G5):

```cpp
// Sender: the id of the frame that was just encoded and sent.
engine.send_video(frame);
engine.record_video_ref_frame(frame.frame_seq, capture_ts_us);

// Receiver: resolve an id back to the capture instant.
if (auto ts = engine.ref_frame_to_capture_ts(ref_frame_id)) {
    const auto age = engine.data_message_frame_age_us(ref_frame_id);
}
```

The input event carries the `ref_frame` id it was generated against;
the viewer resolves that id to the frame it displayed. The mapping is
bounded (the most recent 1024 entries) and
`record_video_ref_frame()` is idempotent.

> The `ref_frame` id travels **inside the control payload**, not
> through the signalling service. The signalling service never sees an
> input event.

---

## 8. Implementing this: the reference service

[`examples/remote-desktop/`](../../examples/remote-desktop/) is a
complete, working implementation of everything above:

| File | What it is |
|---|---|
| `signalling.py` | The asyncio WebSocket server. |
| `signalling_core.py` | The pure logic: pairing store, TURN issuer, message router. |
| `wsproto.py` | A hand-rolled RFC 6455 WebSocket layer (standard library only). |
| `client.py` | The example client; bridges the signalling channel to the `demo-p2p` media binary. |
| `test_signalling.py` | The self-running test of the pure logic and the live round trip. |

It is a **reference implementation**, not a production server: no TLS,
no authentication of the service itself, no rate limiting, no
persistence, no session limits (§6.4).

### 8.1 Running it

```bash
# Terminal 1 — the service.
python examples/remote-desktop/signalling.py --host 127.0.0.1 --port 8765

# Terminal 2 — the device.  It prints the pairing code.
python examples/remote-desktop/client.py --role device --url ws://127.0.0.1:8765

# Terminal 3 — the operator.  It types the pairing code.
python examples/remote-desktop/client.py --role viewer --code 123456 \
    --url ws://127.0.0.1:8765
```

### 8.2 The self-running test

```bash
python examples/remote-desktop/test_signalling.py
```

It covers the pairing-code lifecycle (issue / validate / expiry /
one-shot), the TURN credential (issuance / validation / expiry / tamper
detection), the message routing (pairing, join, candidate relay, ICE
restart, reconnect, TURN issuance, the error paths), and a live
WebSocket round trip through the hand-rolled RFC 6455 layer. The
script prints one line per check and exits non-zero on failure.

### 8.3 How a reader verifies the media session

The two example clients establish a media session by bridging the
signalling channel to the existing `demo-p2p` binary, which already
speaks the same message shapes (`offer` / `answer` / `candidate`) over
newline-delimited JSON on stdin/stdout. The bridge is transparent: the
signalling messages *are* the media messages.

To verify end to end:

1. **Build the media binary.** The examples are behind
   `NIMRTC_BUILD_EXAMPLES=ON`, so configure a build tree that has them
   on:

   ```bash
   cmake -S . -B build/msvc-full -G Ninja \
       -DCMAKE_BUILD_TYPE=Debug -DNIMRTC_BUILD_EXAMPLES=ON
   .\scripts\msvc_env.ps1 -Command "cmake --build build\msvc-full -j 4"
   ```

   This produces `build/msvc-full/examples/demo-p2p/demo-p2p.exe`
   (Ninja with a single-config generator puts the binary directly under
   the target's directory; a multi-config generator adds a
   `Debug/` / `Release/` subdirectory). The client searches the usual
   locations and can also be pointed at the binary explicitly with
   `--binary`.

2. **Start the service** (terminal 1):

   ```bash
   python examples/remote-desktop/signalling.py --port 8765
   ```

3. **Start the device** (terminal 2):

   ```bash
   python examples/remote-desktop/client.py --role device --url ws://127.0.0.1:8765
   ```

   It prints the pairing code:

   ```
   [device] pairing code: 123456 (valid for 300 s)
   [device] registered; room=room-…
   ```

4. **Start the viewer** (terminal 3), typing the code:

   ```bash
   python examples/remote-desktop/client.py --role viewer --code 123456 \
       --url ws://127.0.0.1:8765
   ```

   Both sides then log the media plane starting:

   ```
   [viewer] the device joined
   [media] started …\demo-p2p.exe (answerer=False)
   [viewer] <- offer (… chars)
   [device] <- offer (… chars)
   ```

   > **TURN is off by default.** The signalling service issues a TURN
   > credential unconditionally, but the client only passes it to the
   > media binary when `--use-turn` is given — pointing the ICE agent
   > at a TURN server that is not running only makes the gathering
   > round slower. To exercise the TURN path, run a TURN server and
   > start the service with the matching `--turn-url` and
   > `--turn-secret`.

5. **Confirm the session.** The `demo-p2p` binary prints the ICE and
   DTLS state transitions on stderr. A successful session reaches:

   ```
   [demo-p2p] offer SDP len=…
   [demo-p2p] got answer SDP len=… process_remote_sdp(answer) OK
   [demo-p2p] added remote candidate
   ```

   and the engine's own state log shows the transport coming up:

   ```
   [engine] icestate=Connected  dtlsstate=Connected  srtpstate=Active
   ```

   `demo-p2p` exits with `0` after its `--duration` elapses.

> **Without the binary**, the clients still complete the whole
> signalling flow — pairing, join, TURN issuance, candidate relay — and
> report `media plane skipped`. The signalling exchange is the part
> this guide is about.

---

## 9. Swapping the reference service out

The service is a product concern; the engine does not care which one
is in use. A product replaces the reference service by implementing the
same message contract:

- the same `type` names (§3),
- the same role model — one device, one viewer per room,
- the same credential-is-the-identity rule (§6.1),
- the same RFC 7635 TURN credential shape (§5.1), if it issues TURN
  credentials at all.

What a product is expected to add: TLS, authentication of the service
itself, rate limiting on the pairing code, persistence of the
credential store, session limits (plan item I6), and — for a product
that needs it — a second signalling transport for the browser case.

---

## 10. See also

- [`examples/remote-desktop/`](../../examples/remote-desktop/) — the
  reference implementation and its self-running test.
- [`docs/plan/remote-desktop-plan.md`](../plan/remote-desktop-plan.md) —
  the plan; Track I carries I1 / I2, and RD-7 is the decision that
  keeps the service out of `src/`.
- [`docs/plan/transport-strategies.md`](../plan/transport-strategies.md) —
  the five named transport strategies; the remote-desktop profile uses
  the `teleop-ultra-low-latency` shape.
- [`src/modules/rd_input/`](../../src/modules/rd_input/) — the
  canonical input-event encoding and the `ref_frame` substrate.
- [`docs/guides/datachannel.md`](datachannel.md) — what the data
  channel guarantees (message boundary, channel identity, partial
  reliability, priority).
- [`docs/api/engine_api.md`](../api/engine_api.md) — the engine's public
  API surface.
