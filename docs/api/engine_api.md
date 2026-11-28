# NimRTCEngine — API Reference

> Single-header reference for `nimrtc::engine::NimRTCEngine`.
>
> Header: `src/engine/include/nimrtc/engine/engine.hpp`
> Status: Public API for v0.12.0-alpha. Pre-1.0; minor breaking
> changes are noted in `CHANGELOG.md` and the per-method `@note`
> tags.

This document covers every public method, its parameters, return
values, and lifecycle expectations. For the configuration struct see
[engine_config.md](./engine_config.md).

---

## 1. Quick reference

```text
nimrtc::engine::NimRTCEngine
│
├── Lifecycle
│   ├── ctor(EngineConfig)
│   ├── ~NimRTCEngine()
│   ├── open() → Status
│   ├── pre_open() → Status
│   ├── close()
│   ├── is_open() → bool
│   ├── start(config) → int           -- v1.4 D7: config + open in one call
│   ├── stop()                        -- v1.4 D7: close() under its lifecycle name
│   └── hangup(reason) → bool         -- v1.4 D7: notify peer, then close()
│
├── Callbacks (set once at construction)
│   ├── set_on_audio_frame(cb)        -- legacy PCM tap
│   ├── set_on_audio_frame_info(cb)   -- detailed PCM frame
│   ├── set_on_video_frame(cb)
│   ├── set_on_error(cb)
│   ├── set_on_state_change(cb)
│   ├── set_on_call_state(cb)         -- v1.4 D7: the whole call's lifecycle
│   ├── set_on_ice_restart(cb)        -- peer restarted ICE, host re-answers
│   ├── set_on_ice_recovery(cb)       -- v1.4 N3: auto/manual restart reports
│   ├── set_on_peer_restart(cb)       -- v1.4 N8: peer restart + path context
│   ├── set_on_data_message(cb)
│   ├── set_on_data_message_ex(cb)    -- v1.2.0: adds the channel identity
│   └── set_on_data_state(cb)
│
├── SDP
│   ├── create_offer() → string
│   └── process_remote_sdp(string) → optional<string>
│
├── ICE inspection / control
│   ├── ice_state_string() → cstring
│   ├── local_ufrag() / local_password()
│   ├── add_turn_server(...)
│   ├── set_remote_ice(string) → bool
│   ├── restart_ice() → bool             -- v1.4 N3: budgeted manual restart
│   ├── ice_restart_count() → uint32
│   ├── set_on_ice_restart(cb)           -- peer restarted ICE
│   ├── set_on_ice_recovery(cb)          -- v1.4 N3: restart reports
│   ├── set_on_peer_restart(cb)          -- v1.4 N8: peer restart, with pairs
│   └── reset_ice_recovery_budget()      -- v1.4 N3: top up the restart budget
│
├── DTLS / SRTP inspection
│   ├── dtls_state() → DtlsState
│   ├── dtls_connected() → bool
│   ├── srtp_installed() → bool
│   ├── local_dtls_fingerprint_sha256_base64() → string
│   ├── drain_dtls() → int
│   └── maybe_install_srtp_keys()
│
├── Audio I/O
│   ├── send_audio(const float* pcm, size_t n) → Status
│   ├── remote_audio_format(AudioFormat* out) → bool
│   ├── audio_source() / audio_sink()   -- opened devices (v1.4, D1)
│   └── Stats: audio_device_pumped_frames / audio_device_pump_errors
│
├── DTMF (RFC 4733, optional — off by default)
│   ├── send_dtmf(event_code, duration_ms) → bool
│   ├── set_on_dtmf(cb)
│   └── negotiated_dtmf_payload_type() → uint8_t
│
├── Video I/O
│   ├── start_video() / stop_video()
│   └── send_video(const VideoSourceFrame&) → Status
│
├── Data / control plane
│   └── create_data_channel(label) → unique_ptr<IDataChannel>
│
├── Tick loop
│   └── tick() → int
│
├── Plugin accessors (read-only after open())
│   ├── transport() / ice_transport()
│   ├── audio3a() / codec_plugin()
│   ├── video_source() / sink() / receiver() / sender()
│   ├── bwe() / scheduler() / video_codec()
│   └── config() → const EngineConfig&
│
└── Runtime swaps
    ├── set_video_sink(unique_ptr<IVideoSink>)
    ├── set_video_source(unique_ptr<IVideoSource>)
    └── Stats (video + audio counters) / stats()
```

---

## 2. Construction & ownership

```cpp
#include <nimrtc/engine/engine.hpp>

nimrtc::engine::EngineConfig cfg;
cfg.local_bind_address = "127.0.0.1";
// ...

nimrtc::engine::NimRTCEngine engine(cfg);
```

- The engine is **move-friendly at the C++ level** but **`open()`
  must be called exactly once** on the live instance. After
  `close()`, the engine is reusable for a fresh SDP exchange but the
  ICE/DTLS state must be torn down on the network side.
- Copy and assignment are deleted.

---

## 3. Lifecycle

### 3.1 `open()`

```cpp
uint32_t open() noexcept;
```

Wires every plugin declared in `config_`, starts ICE candidate
gathering, and prepares the engine for SDP exchange. Returns
`kOk` (0) on success, otherwise a non-zero error code that is
also fired via `on_error_`. Inspect with `last_open_rc()`.

Internally:

1. calls `core::register_all_default_plugins()` so the built-in
   plugins are visible to the registry;
2. resolves all 13 typed factories (transport, RTP, SDP, JB, audio3A,
   codec, BWE, scheduler, DTLS, SCTP, video codecs, …);
3. opens the ICE transport, audio3A, codec, BWE, scheduler;
4. wires the DTLS session, SRTP context, audio encode/decode chain.

Failure modes (non-exhaustive, all returned as non-zero `uint32_t`):
`kErrResourceExhausted`, `kErrNotFound` (factory id not registered),
`kErrInternal`. See `last_open_rc()` and `on_error_` for diagnostics.

### 3.2 `pre_open()`

```cpp
uint32_t pre_open() noexcept;
```

Equivalent to `open()` **minus** the final `ice_t_->open()` call
(so ICE candidate gathering does not start). Use this when:

- You want to call `set_remote_ice(peer_block)` *before* ICE
  gathering — required for the loopback test to avoid ICE role
  conflict: the answerer must know the offerer's ICE ufrag/pwd
  before its own ICE starts, so libjuice picks `controlled` (instead
  of `controlling`).

After `pre_open()` you call `open()` normally; ICE gathering fires
on `open()`. `pre_open()` returns the same error codes as `open()`.

### 3.3 `close()`

```cpp
void close() noexcept;
```

Releases all plugin instances and tears down network state. Safe to
call multiple times; subsequent calls are no-ops. Idempotent with
the destructor.

### 3.4 `is_open()`

```cpp
bool is_open() const noexcept;
```

True iff the engine is in the `kOpen` state (after a successful
`open()`, before any `close()`).

### 3.5 `start(config)` — one call's lifecycle (v1.4 D7, ADR-035)

```cpp
int start(const EngineConfig& config) noexcept;
```

Wraps the "configure → open" sequence in one call: equivalent to
assigning `config` and calling `open()`. `open()` itself is unchanged
for existing hosts. Returns the same status codes as `open()` (0 on
success).

### 3.6 `stop()` — close under its lifecycle name (v1.4 D7, ADR-035)

```cpp
void stop() noexcept;
```

`stop()` **is** `close()`: the defined teardown order — QoE → ICE →
audio plugins → codecs → video plugins → BWE → `TransportSession`
(which itself tears down ICE → DTLS → RTP → SCTP in that order) —
already lives in `close()`; D7 does not reorder it, it just names it
and makes it testable. After `stop()`, the call-state callback receives
`kClosed` (exactly once).

### 3.7 `hangup(reason)` — end the call with a reason (v1.4 D7, ADR-035)

```cpp
bool hangup(std::string_view reason) noexcept;
```

The difference from `stop()`: `hangup()` **carries a reason** and
**notifies the peer first** (the reason is sent as a control message),
then closes in the same defined order. Returns `true` when the hangup
was performed; `false` when the engine was not open. A missing or
saturated control channel does not stop the hangup.

---

## 4. Callbacks

All callbacks are user-owned `std::function`s; pass `nullptr` to
clear. They are installed before `open()` and re-fired for the
lifetime of the engine. None of them may throw — `noexcept`-marked
methods are written defensively.

| Callback | Fires when | Signature |
|---|---|---|
| `on_audio_frame` | A decoded PCM frame is available (after SRTP decrypt + Opus decode + downmix + 3A post-process on the receive path) | `void(const float* pcm, size_t num_samples)` |
| `on_audio_frame_info` | The same decoded PCM frame, with its metadata (rate, channels, RTP timestamp, concealment / FEC flags, capture instant) | `void(const engine::AudioFrameInfo& info)` |
| `on_video_frame` | A decoded H.264 NAL is available on the receive path (pre-render) | `void(const uint8_t* nal, size_t len, bool keyframe)` |
| `on_error` | Any subsystem error fires (transport, DTLS, audio3A, codec, scheduler) | `void(uint32_t err, std::string_view msg)` |
| `on_state_change` | Engine state transitions (e.g. ICE → connected) | `void(const char* state)` |
| `on_call_state` | The whole call's lifecycle (v1.4 D7, ADR-035) — fires on a state **migration**, never the same state twice | `void(const engine::CallStateReport& report)` |
| `on_ice_restart` | The *peer* restarted ICE (RFC 8445 §9.1.1.1); the host must answer with a fresh offer | `void()` |
| `on_ice_recovery` | An ICE restart the engine initiated (v1.4 N3, ADR-031) — every success *and* every terminal report | `void(const engine::IceRecoveryReport& report)` |
| `on_peer_restart` | The *peer* restarted ICE, with the path context (v1.4 N8, ADR-033) — fires **alongside** `on_ice_restart`; carries the old/new pair + reason `"peer-restart"` | `void(const engine::PeerRestartReport& report)` |
| `on_data_message` | A DataChannel message arrives | `plugins::DataMessageCallback` |
| `on_data_state` | A DataChannel state transition (open / closed / error) | `plugins::DataChannelStateCallback` |
| `on_dtmf` | A completed DTMF key press arrives (RFC 4733; only when `EngineConfig::dtmf_enabled` is true) | `void(const plugins::DtmfEvent& event)` |

> **`on_audio_frame` is the legacy tap.**  Its `num_samples` argument is
> the **total number of interleaved float samples** —
> `num_samples_per_channel * channels` — and not the number of samples
> per channel.  The two are equal only for a mono stream.
>
> **`on_audio_frame_info` is the recommended surface.**  It carries the
> same PCM buffer plus the metadata a host needs to place the frame on
> a timeline.  When both callbacks are installed, the **legacy one fires
> first** and both receive the same buffer.  Installing one does not
> uninstall the other, and both may be absent.
>
> The `pcm` pointer of either callback points into a decoder-owned
> buffer that is reused for the next frame.  A host that needs to keep
> the audio must copy it.

> **Threading**: callbacks fire on the engine thread (the thread
> that called `open()` and drives `tick()`). Plugins may also call
> `on_error` from the ICE recv path; check per-family docs.

---

## 5. SDP

### 5.1 `create_offer()`

```cpp
std::string create_offer() noexcept;
```

Generates an RFC 8829-conformant SDP offer. Includes media
section(s) for audio and (if enabled) video. The offer is local:
it contains `c=IN IP4 0.0.0.0` and the engine's own ICE
credentials; the answerer reads them and routes its STUN
accordingly.

### 5.2 `process_remote_sdp()`

```cpp
std::optional<std::string> process_remote_sdp(std::string_view remote_sdp) noexcept;
```

Parses the remote SDP, validates it against the engine's
configuration (codec / transport compatibility), and produces the
matching answer. Returns `nullopt` if parsing fails; the error
fires via `on_error_`.

Once both sides have processed each other's SDP, ICE connectivity
checks begin. Loopback Case D, cross-Chrome, and the agent
gateway profile all use this exact flow.

---

## 6. ICE inspection & control

### 6.1 `ice_state_string()`

```cpp
const char* ice_state_string() const noexcept;
```

Returns one of `"disconnected"`, `"gathering"`, `"checking"`,
`"connected"`, `"completed"`, `"failed"`, `"closed"`. Convenient
for logging and `std::string(...)` interpolations.

### 6.2 `local_ufrag()`, `local_password()`

```cpp
std::string local_ufrag()    const noexcept;
std::string local_password() const noexcept;
```

The local ICE credentials. They are auto-included in our SDP
offer; use these accessors only if you're building a non-SDP
signaling path (e.g. custom JSON-only signaling).

### 6.3 `set_remote_ice()`

```cpp
bool set_remote_ice(std::string_view ice_block) noexcept;
```

Inject the peer's `a=ice-ufrag` / `a=ice-pwd` / `a=candidate`
lines into the local ICE agent. **Must** be called before `open()`
(or between `pre_open()` and `open()`) for the answerer; otherwise
libjuice defaults to `controlling`, and role conflict prevents
connectivity.

### 6.4 `add_turn_server()`

```cpp
int add_turn_server(std::string_view host, std::uint16_t port,
                    std::string_view username,
                    std::string_view password) noexcept;

int add_turn_server(std::string_view host, std::uint16_t port,
                    std::string_view username,
                    std::string_view password,
                    plugins::TurnTransport transport) noexcept;
```

Adds a TURN relay for candidate gathering. Requires `pre_open()`
first. Returns 0 on success; non-zero if the ICE transport is not
yet created.

The first overload reaches the server over plain TURN/UDP. The second
one selects the control-connection transport explicitly:

| `plugins::TurnTransport` | Meaning |
| --- | --- |
| `Udp` | TURN over UDP (default, RFC 5766) |
| `Tcp` | TURN over TCP, RFC 8656 §12 self-delimiting framing |
| `Tls` | TURN over TLS, same framing over a TLS stream |

`Tcp` is what enterprise networks need when UDP is blocked. `Tls`
adds transport security on top of the TURN channel; when the
underlying vendor build cannot negotiate TLS it falls back to plain
TCP.

The plugin-level seam exposes the same choice via
`plugins::IICETransport::add_turn_server_with_transport(...)` plus
`turn_transport_supported(TurnTransport)`, so a host application can
query whether a transport is usable before committing to it. The
plugin-layer entry point carries a distinct name rather than being an
overload of `add_turn_server()`: a second virtual with the same name
would be hidden by every existing `add_turn_server()` declaration in a
derived class, which breaks out-of-tree plugins that implement the
interface.

The engine's TURN-credentials provider seam (v1.4 N7, §6.9) never
overrides an explicit static list: once `add_turn_server()` has been
called, the provider is not consulted — a **static** server list
**wins** over the callback.

### 6.5 `restart_ice()`

```cpp
bool restart_ice() noexcept;
```

Force a local ICE restart (RFC 8445 §9.1.1.1): fresh ufrag / pwd,
fresh candidate gathering.  The host that calls `restart_ice()` still
owns the two-step contract — the engine does **not** re-negotiate for
the host:

```cpp
engine.restart_ice();
// then put the new ICE credentials in front of the peer:
engine.send_to_peer(engine.create_offer());
```

The engine's part is **budgeted** (v1.4 N3, ADR-031): every restart —
automatic or this manual one — draws from the same sliding 10-minute
budget of 5 restarts.  Returns `false` (and fires
`kIceRecoveryExhausted` once per terminal episode, §6.8) when the
budget is exhausted, before the transport is touched.  Returns `false`
too when the engine has no ICE transport yet (before `pre_open()`).

### 6.6 `ice_restart_count()`

```cpp
std::uint32_t ice_restart_count() const noexcept;
```

The ICE restarts performed so far on the live transport (RFC 8445
§9.1.1.1); 0 means the session never restarted.  This is the **ICE
agent's** lifetime count.  The engine's *recovery-policy* consumption
is a separate number, projected as `Stats::ice_recovery_restarts`
(§13.5) and scoped to the sliding window.

### 6.7 `set_on_ice_restart()`

```cpp
void set_on_ice_restart(plugins::OnIceRestart cb) noexcept;  // void()
```

Install the callback fired when the *peer* restarts ICE: the peer's
fresh ufrag / pwd arrive on the wire, and the host must answer with a
fresh offer (`create_offer()` picks the new credentials up).

This callback keeps its original no-payload shape (`void()`).  The
path context of a peer restart is delivered through the **sibling**
callback `set_on_peer_restart()` (§6.10, v1.4 N8) — which fires
**alongside** this one, so a host already relying on
`set_on_ice_restart` is unaffected.

### 6.8 The ICE recovery policy (v1.4 N3, ADR-031)

`EngineConfig::ice_recovery_policy` selects who restarts ICE when the
network flaps:

| Value | Meaning |
|---|---|
| `"auto"` (default) | The engine runs the reconnection policy: after a session that *has* connected, a `Disconnected` observation enters a 5-second grace window; when the grace lapses, the engine restarts ICE on a 1 / 2 / 4 / 8 / 8 s backoff, capped at **5 restarts per sliding 10-minute window**. |
| `"off"` | The engine never restarts ICE on its own (`TransportStats::ice_recovery_state` reports `"disabled"`). |
| anything else | Resolves to `"off"` — a misspelled policy must not silently run a policy the host did not ask for (deliberately the opposite of `connectivity_profile`'s `"auto"` fallback). |

Automatic and manual restarts draw from the same budget.  When it is
exhausted the machine reaches **terminal** and reports once:
`on_error_` receives `kIceRecoveryExhausted` (0x1F01), a warn log line
is written, and `set_on_ice_recovery` receives a terminal
`IceRecoveryReport` (the callback fires on *every* terminal report;
`on_error_` and the log line fire once per episode).

```cpp
struct NIMRTC_API_STABLE IceRecoveryReport {
    std::string reason;               // "local-address-changed" / "manual" / "relay-fallback"
    std::uint32_t restart_count = 0;       // restarts used in this window
    std::uint32_t remaining_budget = 0;    // restarts left in this window
    bool terminal = false;
    std::string terminal_reason;      // "budget-exhausted" / "manual-blocked"

    // ---- Path-change pair context (v1.4 N8, ADR-033) ----
    std::optional<IceCandidatePairInfo> old_pair;  // pair before the switch; empty = none yet
    std::optional<IceCandidatePairInfo> new_pair;  // pair after the switch; empty = still gathering
};

void set_on_ice_recovery(EngineIceRecoveryCallback cb) noexcept;  // void(const IceRecoveryReport&)
void reset_ice_recovery_budget() noexcept;
```

`reset_ice_recovery_budget()` tops the budget back up to 5 and clears
the terminal latch.  It does **not** reset the sliding window's start,
so an exhausted window does not instantly re-restart once the budget
returns.

**`old_pair` / `new_pair`** (v1.4 N8): the report now also carries
*which* path the session was on before the restart and *which* one it
is on after it.  `new_pair` empty means the restart is still gathering
— the host sees "not selected yet", not a placeholder.

### 6.9 The TURN-credentials provider seam (v1.4 N7, ADR-032)

Real TURN credentials carry a TTL (`username:timestamp` + HMAC), so the
engine cannot hold one set and be done.  `EngineConfig::turn_credentials`
hands the engine a provider callback; the engine *pulls* a fresh set at
the two moments a TURN server is (re)configured:

1. **before candidate gathering** (`open()`) — when no static TURN
   server was added;
2. **after every N3 reconnection restart** — the old set may have
   expired while the session was down.

```cpp
struct NIMRTC_API_STABLE TurnCredentials {
    std::string host;                                    // TURN server address
    std::uint16_t port = 3478;                           // 0 = TURN/UDP default
    std::string username;                                // empty = anonymous
    std::string password;                                // empty = anonymous
    plugins::TurnTransport transport = plugins::TurnTransport::Udp;
    std::string tls_server_name;                         // TURN/TLS SNI
    bool turn_tls_verify_peer = true;                    // false = skip cert verification
    std::string turn_tls_ca_file;                        // PEM trust-anchor bundle path
};

// A raw function pointer, not std::function — ABI-stable (ADR-015).
using NIMRTC_API_STABLE TurnCredentialsProvider = bool (*)(void* ctx, TurnCredentials* out) noexcept;

// in EngineConfig:
TurnCredentialsProvider turn_credentials = nullptr;   // null = never pull
void* turn_credentials_ctx = nullptr;                 // passed back verbatim
```

**TURN/TLS trust (v1.4, ADR-037).**  The credential set carries the
relay's certificate-trust policy, because the set is re-pulled on every
reconnect and the trust anchor goes with the server it trusts:

- `turn_tls_verify_peer == true` (default) — the engine verifies the
  relay's certificate against the anchors in `turn_tls_ca_file`.  An
  **empty** `turn_tls_ca_file` means *no* anchors are loaded, so every
  certificate is rejected — the safe default.  A host that wants to
  trust a **self-signed** relay hands the engine a `turn_tls_ca_file`
  holding that relay's certificate.
- `turn_tls_verify_peer == false` — **explicitly** skip verification.
  This is a *paid-for* decision: the host gives up the "the relay is who
  it says it is" guarantee.  Only for test relays, self-signed relays in
  a trusted network, or local experiments.  Setting `false` with a
  non-empty `turn_tls_ca_file` is contradictory — the `ca_file` is
  ignored (verification is off, so there is nothing to anchor it to).

A `turn_tls_ca_file` that is missing or not a valid PEM bundle makes the
engine fail **loudly**: it fires `set_on_error` with
`core::kEngineInvalidParam` naming the file and the problem, and it is
surfaced as a queryable fact (see below) — not a silent mystery.

**Queryable verification fact.**  `NimRTCEngine::turn_tls_peer_verification()`
answers "did this call verify the relay's identity":

| Value | Meaning |
|---|---|
| `""` | no TURN/TLS credential set is in effect (transport not `Tls`, or none applied) |
| `"verified"` | `verify_peer == true` and the anchor is loadable — the engine will verify, and a certificate that fails is rejected |
| `"unverified"` | `verify_peer == false` — the host explicitly chose not to verify |
| `"failed"` | `verify_peer == true` but the `ca_file` is missing / not a valid PEM — verification is configured but cannot pass |

The three states are mutually distinguishable, so the host can tell
"verified" from "never verified" from "configured but broken".  The fact
reflects the trust policy of the credentials most recently applied
through the provider seam; it does not observe the wire itself (an
online handshake failure is reported through the N4 stats / call-state
surface, and leaves the fact as it was).

**Static servers win.**  A host that calls `add_turn_server()` keeps the
pre-N7 behaviour exactly: the explicit static list is the host's own
configuration, the provider is **never consulted**, and the callback may
stay null.  The seam is pure addition.

**Callback contract.**  The provider runs on the engine's tick thread —
the same thread `set_on_ice_recovery` fires on.  It must not block and
must not re-enter the engine.  Return `false` to say "no credentials
right now" (the engine continues without TURN, reporting
`turn_credentials_state == "unavailable"`); return `true` with a
non-empty `out->host` to hand one set over.

**Observability.**  `Stats::turn_credentials_state` reports the pull:

| Value | Meaning |
|---|---|
| `""` | no provider configured (the static `add_turn_server()` path is in use) |
| `"idle"` | a provider is configured but no set was pulled yet |
| `"ok"` | the last pull produced a credential set |
| `"unavailable"` | the last pull failed (provider `false` / empty `host`) |
| `"rejected"` | the transport reported a 401/403 on the applied set (sticky for the engine's lifetime) |

When the ICE transport reports a rejected credential, the failure-reason
projection reports `"credentials-rejected"` (v1.4 N4 vocabulary) instead
of a generic `"checks-failed"`.

### 6.10 Path-change reporting (v1.4 N8, ADR-033)

Three report points carry the **path context** — *from where to where,
and why*.  A path change is **not** a failure: none of these feeds the
`ice_failure_reason` vocabulary (§13.5).

```cpp
void set_on_peer_restart(EnginePeerRestartCallback cb) noexcept;
// EnginePeerRestartCallback = std::function<void(const PeerRestartReport&)>
```

| Callback / method | Fires | Reason | Pair context |
|---|---|---|---|
| `set_on_ice_recovery` (§6.8) | local restart the engine ran (N3), or manual `restart_ice()` | `"local-address-changed"` / `"manual"` — refined to `"relay-fallback"` when the pair flipped direct ↔ relay | `IceRecoveryReport::old_pair` = pre-restart pair, `new_pair` = current pair |
| `set_on_peer_restart` | the *peer* restarted ICE — fires **alongside** `set_on_ice_restart` | `"peer-restart"` | `PeerRestartReport::old_pair` / `new_pair` — a peer restart changes *their* credentials, not the local path, so both are typically the current pair |

```cpp
struct NIMRTC_API_STABLE PeerRestartReport {
    std::optional<IceCandidatePairInfo> old_pair;  // empty = none selected yet
    std::optional<IceCandidatePairInfo> new_pair;  // empty = still gathering
    std::string reason;                            // "peer-restart"
};
```

**The reason vocabulary is closed** — a host can `switch` on it:

| reason | when |
|---|---|
| `"local-address-changed"` | local address changed (an ICE drop triggered the N3 reconnection) |
| `"peer-restart"` | the peer changed credentials (`set_on_ice_restart` fired) |
| `"relay-fallback"` | the pair switched direct → relay (or relay → direct) |
| `"manual"` | the host called `restart_ice()` |
| `"ice-failed"` | the ICE agent failed explicitly (N3) |

`old_pair` / `new_pair` empty has meaning: empty `old_pair` = no pair
was ever selected (the first connection); empty `new_pair` = the
restart is still gathering.  `Stats::last_path_change_reason` /
`last_path_change_new_pair` (§13.5) keep the **last** change for hosts
that poll instead of subscribing — the fields are populated even when
`ice_recovery_policy == "off"`.

### 6.11 The call-state surface (v1.4 D7, ADR-035)

The confluence point for "how is this call doing": **one callback** that
spans the whole stack, so a host does not assemble the answer from
`set_on_state_change` + `set_on_error` + polling `Stats`.

```cpp
void set_on_call_state(EngineCallStateCallback cb) noexcept;
// EngineCallStateCallback = std::function<void(const CallStateReport&)>
```

**The migration order** — `connecting` → `connected` →
(`reconnecting` ↔ `connected`)* → `failed` / `closed`:

```text
   connecting ──► connected ──► reconnecting ──► ... (N3 recovers)
       │             │              │
       │             │              └──► (N3 terminal) ──► failed
       │             └──► (DTLS / media failure) ───────► failed
       └──► stop() / hangup() / close() ───────────────► closed
```

`failed` / `closed` are **absorbing**: once the call has failed or
ended, no report can move it to another state — a host hangs up and
starts a new call rather than "recovering" a failed one.

```cpp
enum class NIMRTC_API_STABLE CallState : std::uint8_t {
    kConnecting = 0,   // establishing (gathering / connecting)
    kConnected = 1,    // up
    kReconnecting = 2, // down, the engine is recovering per the N3 policy
    kFailed = 3,       // failed (has a failure_reason)
    kClosed = 4,       // over (stop() / hangup() / destructor)
};

enum class NIMRTC_API_STABLE CallFailure : std::uint8_t {
    kNone = 0,
    kIceFailed = 1,           // ICE layer failed (checks-failed / no-pair)
    kDtlsFailed = 2,          // DTLS handshake failed
    kTurnRefused = 3,         // TURN server refused (transport-unsupported)
    kCredentialsRejected = 4, // credentials rejected (credentials-rejected)
    kMediaFailed = 5,         // media layer failed (codec / render / capture)
};

struct NIMRTC_API_STABLE CallStateReport {
    CallState state = CallState::kConnecting;
    CallFailure failure = CallFailure::kNone;
    std::string failure_reason;                  // the N4 vocabulary, verbatim
    std::optional<IceCandidatePairInfo> old_pair;  // N8: pair before the change
    std::optional<IceCandidatePairInfo> new_pair;  // N8: pair after the change
    std::string path_change_reason;
    std::uint32_t restart_count = 0;             // N3: restarts used this window
    std::uint32_t remaining_budget = 0;          // N3: restarts left this window
    bool terminal = false;                       // N3: the recovery is over
};
```

**When it fires** — only on a real **migration**, never the same state
twice (`set_on_state_change` stays the coarse "open"/"closed" signal;
this is the fine migration flow):

| Migration | Trigger | Details carried |
|---|---|---|
| `kConnecting` | `open()` — the very first observation | — |
| `kConnected` | ICE reaches `Connected` / `Completed` | — |
| `kReconnecting` | N3 restarts ICE (`execute_ice_recovery_restart`) | `restart_count` / `remaining_budget` / `terminal`, plus the N8 path context |
| `kFailed` | N3 terminal, DTLS failure, media failure | `failure` (`kIceFailed` / `kDtlsFailed` / `kMediaFailed` …) + `failure_reason` (N4 string) |
| `kClosed` | `stop()` / `hangup()` / `close()` / destructor | — |

**`failure_reason` is the N4 vocabulary verbatim**, mapped onto the enum
by one pure function:

| `failure_reason` | `failure` |
|---|---|
| `"credentials-rejected"` | `kCredentialsRejected` |
| `"transport-unsupported"` | `kTurnRefused` |
| `"checks-failed"` / `"no-pair"` / `"no-candidates"` | `kIceFailed` |
| `"none"` / `""` | `kNone` (no failure is *not* a failure) |

**`set_on_control_message` lives next to `set_on_call_state`** because
the two callbacks read as one surface — its signature and semantics are
unchanged.

---

## 7. DTLS / SRTP inspection

```cpp
nimrtc::dtls::DtlsState dtls_state() const noexcept;
bool                    dtls_connected() const noexcept;
bool                    srtp_installed() const noexcept;
std::string             local_dtls_fingerprint_sha256_base64() const noexcept;
```

The DTLS state machine runs from `Closed` → `HelloSent` →
`Connected` (`dtls::DtlsState` enum). The engine auto-installs SRTP
keys once `dtls_connected()` is true; `srtp_installed()` exposes the
post-install flag for diagnostics.

`drain_dtls()` and `maybe_install_srtp_keys()` are called by `tick()`
in the background; no manual invocation is required.

`local_dtls_fingerprint_sha256_base64()` returns the SPKI SHA-256
fingerprint in the WebRTC base64 form (matches Chrome's
`--ignore-certificate-errors-spki-list`).

---

## 8. Audio I/O

### 8.1 `send_audio()`

```cpp
uint32_t send_audio(const float* pcm_samples, size_t num_samples) noexcept;
```

Encode `pcm_samples` with the configured audio codec, RTP-packetise
it, SRTP-encrypt it, and submit to the scheduler. The buffer must
remain valid for the duration of the call (no copy made).

**The framing is the engine's business.**  `send_audio()` accepts any
block size: the PCM goes into a frame accumulator, and exactly one RTP
packet carrying exactly one frame of `EngineConfig::audio_ptime_ms`
milliseconds goes on the wire per frame.  A host that hands over one
whole frame at a time and a host that hands over seven samples at a
time put the *same* packets on the wire; only the moment of the call
differs.  A partial frame stays in the accumulator until the next call
completes it.

The media timestamp advances by the frame duration whether or not the
packet reached the wire, so a dropped frame leaves a gap in the
sequence number — never a hole in the timeline.

**Nothing fails silently.**  An empty buffer, a null buffer or an
encoder failure increments `Stats::audio_encode_errors` and fires
`on_error` (throttled to one report per second, so a broken stream
cannot flood the host).  The counter grows with *every* occurrence,
even the ones that are not reported.

Call cadence: `num_samples / (sample_rate_hz * channels)` matches
the frame duration in seconds; typical 10 ms / 20 ms Opus frames.
The engine handles framing internally as long as you keep the
cadence consistent — opportunistic re-pacing happens in `tick()`.

Returns `kOk` on success; non-zero on encode / encrypt failure
(surfaces via `on_error_`).

### 8.2 `set_on_audio_frame_info()`

```cpp
void set_on_audio_frame_info(AudioFrameInfoCallback cb) noexcept;
```

Installs the detailed audio-frame callback.  `AudioFrameInfo` carries
the decoded PCM plus its metadata:

| Field | Meaning |
|---|---|
| `pcm` | Interleaved PCM, float, `[-1.0, 1.0]`.  Valid only for the duration of the callback. |
| `num_samples_per_channel` | Samples per channel in `pcm`. |
| `sample_rate_hz` | The decoder's native rate (48000 for Opus).  Resample if the device rate differs. |
| `channels` | Channel count of `pcm`, after the downmix to `EngineConfig::pcm_channels`. |
| `rtp_timestamp` | RTP timestamp of the first sample in this frame. |
| `playout_time_us` | The engine-thread instant at which the frame was handed over (same epoch as `tick()`'s clock, **not** the Unix wall clock). |
| `capture_time_us` | The peer's capture instant, in Unix microseconds.  **0** when the cross-end timeline has not reconstructed it yet — the value is never faked from the local clock. |
| `concealed` | True when the frame is concealment (PLC) audio, i.e. the original packet never arrived. |
| `fec_recovered` | True when the frame was reconstructed from the peer's in-band FEC. |

Pass an empty function to remove the callback.  The legacy
`set_on_audio_frame()` stays, and when both are installed the legacy
one fires **first**, with the same buffer.

### 8.3 `remote_audio_format()`

```cpp
bool remote_audio_format(AudioFormat* out) const noexcept;
```

Reports the negotiated format of the inbound audio stream.  Returns
`false` when no audio track has been negotiated yet, in which case
`out` is left untouched.  `AudioFormat` carries
`sample_rate_hz`, `channels`, `payload_type` and `frame_duration_ms`.

A host that must know the rate before the first frame arrives — to
pre-allocate a resampler, say — asks the engine:

```cpp
nimrtc::engine::AudioFormat fmt{};
if (engine.remote_audio_format(&fmt)) {
    // fmt.sample_rate_hz, fmt.channels, fmt.payload_type,
    // fmt.frame_duration_ms are negotiated.
}
```

### 8.4 The receive path and the tick rate

The inbound audio is **decoded**: the host receives PCM, not the
compressed bit-stream.  The pipeline resolves its jitter buffer and
its decoder through the plugin registry, walks the media timeline on
the RTP timestamps, conceals the slots that never arrived, recovers a
lost frame from the peer's in-band FEC where possible, downmixes to the
configured output channel count and plays out **only** when the host's
`tick()` asks it to.

**The host must call `tick()` at ≥ 100 Hz for audio playout to be
smooth.**  A host that ticks at 30 Hz still gets continuous audio,
with a coarser granularity — the output cadence follows the media
time, not the arrival, so a slower tick delays the playout instead of
dropping frames.

There is **no fallback receiver**.  A missing decoder plugin or a
missing receive pipeline is a hard error (`kAudioDecoderMissing` /
`kAudioReceiverMissing`) that fires `on_error` exactly once and
disables the audio track; the inbound packets are then counted and
dropped, never decoded.

### 8.5 The engine-driven audio device layer (v1.4 / plan D1)

By default the engine is a **transport for PCM**: the host pushes
microphone samples with `send_audio()` and reads decoded playback
samples through `set_on_audio_frame_info()`.  With
`EngineConfig::audio_source_name` / `EngineConfig::audio_sink_name`,
the engine itself opens the capture and playback devices, so a host
that has a mic and a loudspeaker needs no PCM plumbing at all.

| Config field | Meaning |
|---|---|
| `audio_source_name` | The capture (microphone) backend id.  Empty or `"none"` (the default) — the host feeds PCM itself, exactly today's behavior. |
| `audio_sink_name` | The playback (loudspeaker) backend id.  Empty or `"none"` — the host plays PCM itself. |
| `audio_source_config` | `plugins::AudioSourceConfig` handed to the source plugin's `open()`. |
| `audio_sink_config` | `plugins::AudioSinkConfig` handed to the sink plugin's `open()`. |

**The `"auto"` rule.**  An empty value is *no device*; `"auto"` resolves
through the same availability table `video_capture_name = "auto"` uses
(§9.4), with the audio candidate order `"portaudio"`, then
`"synthetic"`.  When nothing is registered — a CI machine with no sound
card — the engine **warns and degrades**: `open()` still succeeds and
`audio_source()` / `audio_sink()` stay null.  A machine with no
microphone is a normal machine, not an error.

**An explicit id is a promise.**  A non-empty, non-`"auto"` id that
cannot be resolved or opened is a hard error: `open()` fails with
`kEngineInternal`, exactly like the video side's non-empty
`video_capture_name`.

**The capture pump.**  With a source opened, `tick()` pulls one frame
from the device's bounded queue and runs it through the **same**
outbound chain as `send_audio()` — 3A → codec → RTP → SRTP → transport —
so the automatic and the manual paths cannot drift apart.  The engine
only polls; it never installs the source's frame-delivery callback, so
the 3A / codec seams are touched from exactly one thread (RD-6).  The
proof that the path works is `Stats::audio_device_pumped_frames`
moving with no `send_audio()` call.  A frame the outbound chain rejects
increments `Stats::audio_device_pump_errors`.

**The playback path.**  With a sink opened, the decoded inbound PCM is
written to the device, and the host PCM callbacks
(`set_on_audio_frame_info()`) are suppressed — the sink is now the
renderer, and firing the host callback too would double-drive the 3A.
The 3A's render-delivered counter uses the device's own
`played_samples()` delta so the AEC feedback tracks what actually left
the loudspeaker.

**Inspection.**  `audio_source()` and `audio_sink()` return the opened
`plugins::IAudioSource*` / `plugins::IAudioSink*` (null when no device
is in use).  `Stats` reports the *negotiated* ids
(`audio_source_device_id` / `audio_sink_device_id`) and the resolved
backend ids (`audio_source_backend` / `audio_sink_backend`) — empty
strings when no device is in use.

### 8.6 DTMF (RFC 4733)

DTMF is an **optional, in-band** capability: the digits travel as
`telephone-event` payloads on the audio m-line, not as a separate
stream and not out of band.

```cpp
bool send_dtmf(std::uint8_t event_code,
               std::uint16_t duration_ms) noexcept;
void set_on_dtmf(DtmfEventCallback cb) noexcept;
std::uint8_t negotiated_dtmf_payload_type() const noexcept;
```

**DTMF is off by default.**  `EngineConfig::dtmf_enabled` has to be
`true` before the engine offers a `telephone-event` payload type at
all; a default engine's offer carries no RFC 4733 line, and a peer
that sends one anyway is treated as an unknown payload type (see the
counting rules below).  `EngineConfig::dtmf_codec_name` selects the
codec plugin (default `"dtmf"`); an empty string disables the path
even when `dtmf_enabled` is true.

**The negotiation.**  The payload type is negotiated in the SDP
(RFC 4733 §2.3.2), never assumed:

* the offer carries `a=rtpmap:<pt> telephone-event/8000` and
  `a=fmtp:<pt> 0-16`, and the payload type joins the m-line's format
  list (RFC 3550 §5.14 scopes a payload type to its m-section);
* the answer carries the *negotiated* event codes — the peer's list
  narrowed to the codes this side implements — not a blind echo of the
  offer;
* `negotiated_dtmf_payload_type()` reports the number both ends use,
  or `0` when nothing was negotiated.  `send_dtmf()` fails in exactly
  that state, so a host can ask before it presses a key.

**Sending.**  `send_dtmf(event_code, duration_ms)` encodes one key
press.  RFC 4733 §2.5.1.2 describes the event as a *sequence*: the
packet is repeated while the key is held, each repeat carrying a
larger duration, and the packet that closes the event carries the
end-of-event bit.  The first packet goes out immediately; the repeats
are paced by `tick()`, so **the host must keep ticking for the event
to finish**.  The event packets share the audio stream's sequence
number space and timestamp base (RFC 4733 §2.1) and carry the marker
bit on the first packet only (RFC 4733 §2.2.2).

`duration_ms` is the length of the key press; `0` is rejected (RFC
4733 §2.3.5 makes a zero duration a *state*, not a key press), and
event codes above `16` are rejected (RFC 4733 §2.1 defines 0..16).

**Receiving.**  `set_on_dtmf()` installs the callback that receives the
completed events — `(event_code, duration_ms, end)`.  The engine
accumulates the duration across the repeats and reports the event
**once**, when the packet with the end bit arrives.

**The counting rules** (the boundary with the payload-type routing
fix): a `telephone-event` payload is *not* audio — it is a four-byte
event header, and handing it to the audio decoder would push the
header into an Opus decoder.  So:

| state                            | a received telephone-event packet |
|----------------------------------|------------------------------------|
| DTMF off, nothing negotiated     | counted as an unknown audio payload type and dropped |
| DTMF on, nothing negotiated      | counted as an unknown audio payload type and dropped |
| DTMF on, payload type negotiated | decoded and reported to the host |

In other words: a negotiated payload type is never an anomaly, and an
un-negotiated one always is.  The telephone-event plane has counters
of its own in `Stats` — `dtmf_packets_sent`,
`dtmf_packets_received`, `dtmf_packets_malformed`,
`dtmf_events_delivered` and `dtmf_payload_type` — because the audio
counters say nothing about it (the payload never reaches the audio
pipeline).

---

### 8.7 RTCP Extended Reports (RFC 3611)

The **Extended Report** (XR, RTCP payload type 207) is an optional
QoE / network-statistics channel.  NimRTC both *receives and decodes*
it and — when `EngineConfig::rtcp_xr_enabled` is set — *sends* it.

```cpp
plugins::IXrReport* xr_report() noexcept;          // non-owning
const plugins::IXrReport* xr_report() const noexcept;

// Outbound side (v1.2.0, session capability plan X2).
std::uint64_t xr_reports_sent_count() const noexcept;
```

The engine demultiplexes every inbound RTCP compound datagram and
routes the XR sub-packets into this consumer.  Read the decoded
report blocks out of its snapshot:

```cpp
if (auto* xr = engine.xr_report()) {
    const auto snap = xr->snapshot();
    if (snap.has_data) {
        for (const auto& blk : snap.last_packet.blocks) {
            switch (blk.type) {
                case plugins::XrBlockType::kReceiverReferenceTime:
                    // blk.receiver_reference_time.ntp_timestamp
                    break;
                case plugins::XrBlockType::kDlrr:
                    // blk.dlrr.sub_reports[]
                    break;
                case plugins::XrBlockType::kStatisticsSummary:
                    // blk.statistics_summary.lost_packets, .max_jitter, ...
                    break;
                default:
                    break;   // recognised but not parsed
            }
        }
    }
}
```

**The supported block types** are RFC 3611's own registry
(RFC 3611 section 6.2):

| BT | Block | NimRTC |
|----|-------|--------|
| 1 | Loss RLE | recognised, **not parsed** |
| 2 | Duplicate RLE | recognised, **not parsed** |
| 3 | Packet Receipt Times | recognised, **not parsed** |
| 4 | Receiver Reference Time | **parsed** |
| 5 | DLRR (Delay since Last Receiver Report) | **parsed** |
| 6 | Statistics Summary | **parsed** |
| 7 | VoIP Metrics | recognised, **not parsed** |

A block NimRTC does not parse is **skipped by its length**, never an
error — that is what makes the format extensible.

> **Note on block numbering.**  There is **no** block type named
> "Receiver Estimated Quality" in RFC 3611; that was the numbering of an
> early draft (where BT=1 was it and BT=6 was DLRR).  A report built
> with the draft's numbering would not interoperate with a conforming
> peer, so NimRTC only speaks the standard's actual numbering.

**Configuration.**  `EngineConfig::xr_report_name` selects the
consumer plugin (default `"nimrtc_xr"`, the built-in one).  An empty
string disables the consumer entirely — the XR sub-packet is then
recognised by the demultiplexer and dropped, which is the pre-v1.2.0
behaviour.

**A malformed report is counted, not fatal.**  `snapshot()` reports
`packets_malformed`; the read path never fails because one peer sent a
broken report.  `Stats::xr_report_blocks` counts the XR sub-packets
that were routed to the consumer — a peer that sends extended reports
and a counter stuck at zero means the routing is not wired.

#### 8.7.1 The outbound side

Setting `EngineConfig::rtcp_xr_enabled = true` makes the engine emit
one XR report per RTCP interval (one second), carrying:

| BT | Block | When it is sent |
|----|-------|-----------------|
| 4 | Receiver Reference Time | the peer negotiated `rcvr-rtt` |
| 5 | DLRR | the peer negotiated `rcvr-rtt` **and** a Receiver Reference Time report has been received |
| 6 | Statistics Summary | the peer negotiated `stat-summary` **and** inbound audio has been seen |

The DLRR's sub-block names the SSRC of the *peer* whose Receiver
Reference Time report is being answered, together with the middle 32
bits of that report's NTP timestamp and the delay since its arrival
(RFC 3611 section 4.5) — the three values a peer needs to compute the
round-trip time.
**The SDP negotiates it.**  The offer carries

```
a=rtcp-xr:rcvr-rtt=all stat-summary=loss,jitt
```

on the audio m-section, and the answer echoes the subset the engine
implements (RFC 3611 section 5.2).  RFC 3611 section 5.1 says a
participant "SHOULD NOT send XR blocks other than the ones indicated
by the parameters", so the engine sends nothing until the peer has
named the format — an XR report that was never negotiated is not
merely wasteful, it is a protocol violation.

**The field list is the peer's, not the engine's.**  RFC 3611 section
5.1 says the `stat-summary` parameter "contains a list indicating which
fields SHOULD be included in the Statistics Summary report blocks that
are sent" — so the L and J flag bits of the block the engine emits
follow the *peer's* list.  A peer that asked for `loss` alone gets a
block with the loss flag set and the jitter flag clear; a peer that
asked for `loss,jitt` gets both.  The engine produces loss and jitter
and nothing else, so a list naming only fields it does not measure
(`dup`, `TTL`, `HL`) narrows to nothing and the format is not echoed.

If the peer offers the attribute and the host does not want XR at all,
the answer carries the **bare** attribute (`a=rtcp-xr`, no
parameters): that is the RFC's way of saying "I understand the
attribute but send me no XR blocks".

---

### 8.8 TMMBR / TMMBN (RFC 5104)

The **Temporary Maximum Media Bit Rate** request is the receiver's
statement of the highest rate it can accept; the **TMMBN** is the
sender's confirmation of the bounding set that actually applies.

```cpp
// Outbound (v1.2.0, session capability plan X4 / X5).
std::uint32_t peer_tmmbr_limit_bps() const noexcept;  // the peer's limit
std::uint32_t last_tmmbr_bps() const noexcept;         // what we last reported
std::uint64_t tmmbr_sent_count() const noexcept;       // TMMBR + TMMBN sent
std::uint64_t tmmbr_received_count() const noexcept;    // TMMBRs honoured
```

**What the engine sends.**  With `EngineConfig::tmmbr_enabled = true`
the engine emits a TMMBR carrying the bit rate its bandwidth
estimator is *currently using* — never a stale number and never a
limit the engine does not honour itself.  A changed rate is reported
at the next RTCP interval; an unchanged rate is repeated only every
five intervals, because the peer may have missed the previous one
(RFC 5104 section 4.2.1.2).

The FCI entry names the *media source* the limit applies to (the
peer's inbound audio SSRC), and the "SSRC of media source" field of
the RTPFB header is zero — RFC 5104 section 4.2.1.2 says it is
unused.

**What the engine obeys.**  An inbound TMMBR is a limit the engine
must respect, and the engine does:

* the scheduler's session budget is clamped to
  `peer_tmmbr_limit_bps()`;
* the per-class pacing rates are clamped the same way;
* the **audio encoder** is clamped the same way — the encoder is what
  decides how many bits go on the wire, so a limit that stopped at the
  pacer would only *lose* frames rather than keep the promise;
* the limit **overrides `EngineConfig::audio_min_bitrate_bps`**: the
  local floor protects the audio against a congestion collapse, while
  the peer's limit is a commitment the engine made on the wire (the
  TMMBN is the acknowledgement of it), so the two are not of the same
  kind — the peer's limit wins, and the encoder may go *below* the
  local floor;
* the tightest limit wins — a looser TMMBR never raises a ceiling a
  tighter one has set (RFC 5104 section 3.5.4.2);
* a tuple naming a media source the engine does not send is recorded
  but **not** applied: it is somebody else's constraint.

The peer's limit is honoured even when
`EngineConfig::audio_bitrate_adaptive` is `false` — that switch says
"do not move the encoder because of the *estimate*", not "break the
promises the engine already made on the wire".

Every inbound TMMBR is answered with exactly one TMMBN (RFC 5104
section 4.2.2.2), even when the tuple was not accepted — that is what
lets a participant which missed the last TMMBR recover the state.

The TMMBN's FCI entry names the **owner** of the bounding tuple
(RFC 5104 section 4.2.2.1).  In the unicast case the receiver that
issued the TMMBR becomes the owner of the one bounding tuple as soon
as it has sent its first TMMBR (RFC 5104 section 4.2.2.2), so the
owner is the SSRC of the peer's feedback — the SSRC the TMMBR's own
"SSRC of packet sender" field carried.

**The SDP negotiates it.**  RFC 5104 section 7.2 gives TMMBR the
codec-control-message form:

```
a=rtcp-fb:<pt> ccm tmmbr
```

Only the joint subset of the offer's and the answer's CCM parameters
may be used, so the engine sends no TMMBR unless the peer's
description carried the `tmmbr` parameter *and* the host's own
configuration keeps the feature on.  The answer removes the `tmmbr`
parameter when the answerer does not support it — the rest of the
entry survives (RFC 5104 section 7.2 says the answerer removes the
*parameters* it does not support, not the whole entry).

`EngineConfig::disable_tmmbr` vetoes the feature in both directions:
no TMMBR is sent and the SDP carries no `tmmbr` parameter.  It
overrides `tmmbr_enabled`.

---

### 8.9 The RTCP support matrix

What the engine understands on the RTCP plane, so a host does not have
to guess.  "Send" means the engine *emits* the message; "Receive"
means it *parses* it.

| Packet | PT | Format | NimRTC |
|--------|----|--------|--------|
| SR | 200 | Sender Report | receive (timeline anchor) + **send** |
| RR | 201 | Receiver Report | receive (bandwidth collector) + **send** |
| SDES | 202 | Source Description | recognised, not parsed |
| BYE | 203 | Goodbye | recognised, not parsed |
| APP | 204 | Application-defined | recognised, not parsed |
| RTPFB | 205 | FMT 1 — NACK | receive |
| RTPFB | 205 | FMT 3 — **TMMBR** | **receive + send** |
| RTPFB | 205 | FMT 4 — **TMMBN** | **send** |
| RTPFB | 205 | FMT 15 — transport-cc | receive (ADR-018 bandwidth signal) |
| PSFB | 206 | FMT 1 — PLI | receive (video keyframe request) |
| PSFB | 206 | FMT 2 — SLI | receive (video keyframe request) |
| PSFB | 206 | FMT 15 — REMB | receive (bandwidth collector) |
| XR | 207 | BT 4 / 5 / 6 | **receive + send** (see §8.7) |

The XR block types NimRTC does *not* parse (Loss RLE BT=1, Duplicate
RLE BT=2, Packet Receipt Times BT=3, VoIP Metrics BT=7) are skipped
by their length field, never an error — see §8.7.

The codec-control-message family (RFC 5104) is **partially**
implemented: `tmmbr` is supported, `fir` / `tstr` / `tstn` / `vbcm`
are not.  An `a=rtcp-fb:` entry that names an unsupported CCM
parameter has that *parameter* removed from the answer - the rest of
the entry survives (RFC 5104 section 7.2).

### 8.9 The audio device plane (v1.4 D1)

The engine owns the microphone and the loudspeaker.  It resolves them by
name through the registry (`EngineConfig::audio_source_name` /
`audio_sink_name`), opens them, and pumps them itself - so a host that
configures nothing gets a working capture-and-playback session.

```cpp
NIMRTC_API_UNSTABLE plugins::IAudioSource* audio_source() noexcept;
NIMRTC_API_UNSTABLE plugins::IAudioSink*   audio_sink() noexcept;

NIMRTC_API_STABLE std::vector<plugins::AudioDevice>
    audio_capture_devices() const noexcept;
NIMRTC_API_STABLE std::vector<plugins::AudioDevice>
    audio_playback_devices() const noexcept;
```

`audio_source()` / `audio_sink()` return the device the engine holds, or
`nullptr` when the name was `"none"` or the device could not be opened.
The enumeration calls return the devices the resolved device can see; an
**empty list is a legitimate answer** (a CI machine with no sound card),
not an error.

**The paths are parallel, not alternatives.**  The device path is
*appended* to the host's `set_on_audio_frame()` callback: a host that
installs the callback keeps receiving the decoded PCM, and the PCM also
goes to the loudspeaker.  A host that wants the device path *only* sets
`audio_sink_pump = false`.

**The AEC reference follows the loudspeaker.**  With a playback device
present, the engine reports the sink's **consumed** sample count to the
3A's render reference (not "the samples were handed to the host"), which
is what keeps the echo canceller's reference aligned with what was
actually played.  Without a device, the host-callback path reports it as
before.

**A device failure is not a session failure.**  A device that will not
open, or that disappears mid-call, is a warning and a counter - the
session keeps running, and the host can still feed PCM by hand.

The statistics live in `Stats`:
`audio_capture_frames`, `audio_capture_frames_sent`,
`audio_capture_dropped`, `audio_capture_device_errors`,
`audio_capture_restarts`, `audio_playback_frames_written`,
`audio_playback_frames_dropped`, `audio_playback_samples_played`, plus
the two **identity** fields `audio_source_device_id` /
`audio_sink_device_id` ("" = none).  The counters say "how much"; the
identity fields say "which device".

See [`engine_config.md` §2.4c](./engine_config.md) for the configuration
side.

---

## 9. Video I/O

### 9.1 `start_video()` / `stop_video()`

```cpp
plugins::Status start_video() noexcept;
void            stop_video()  noexcept;
```

Start / stop the configured `video_source_name`. Idempotent.

### 9.2 `send_video()`

```cpp
plugins::Status send_video(const plugins::VideoSourceFrame& frame) noexcept;
```

Manually push one raw frame through the pipeline. Used for feeding
camera frames directly when the configured `IVideoSource`
(`memory` default) doesn't drive its own capture loop. The frame
is encoded, RTP-packetised, and enqueued to the scheduler.

#### GPU zero-copy (Phase 3 / C3)

By default the engine *stages* the frame into a CPU `I420` buffer (a
full-frame copy, plus a de-interleave for an NV12 source) before
handing it to the codec. A hardware capture source can avoid that copy
by carrying its native GPU handle in the frame:

```cpp
plugins::VideoSourceFrame f{};
f.gpu_backend     = plugins::GpuBackend::kWindowsD3D11Texture;
f.gpu_handle      = native_texture;   // borrowed for the call
f.gpu_pitch_bytes = 1920;
```

With `EngineConfig::video_zero_copy = true`, a frame that carries such a
handle is passed straight to `IVideoCodec::encode_zero_copy()` — no
staging, no copy. The path is taken only when **all three** hold:

1. `EngineConfig::video_zero_copy` is true,
2. the frame carries a live GPU handle, and
3. the codec reports `supports_zero_copy_encode()`.

Otherwise the CPU staging path runs unchanged, so turning the flag on
is always safe. `Stats::video_zero_copy_frames` and
`Stats::video_zero_copy_fallbacks` make the decision observable.

### 9.3 The receive-side feedback bound (v1.3 / video plan T2)

The receiver's feedback to the peer is **bounded**. Without a bound, a
lossy peer could make the feedback grow without limit, and the two
reasons the receiver asks for a key frame — the *network* dropped a
picture, or the *decoder* cannot decode what it is being sent — were
indistinguishable.

The bound has three parts, all configurable through `EngineConfig`:

```cpp
// The minimum interval between two key-frame requests we send.
// Default = 250000 (4 Hz, RFC 5104 §4.3.1.1).  0 disables the limit.
int64_t video_feedback_min_pli_interval_us = 250'000;

// The maximum number of NACK sequence numbers one feedback report may
// carry.  Default = 64.  A negative value means "unbounded".
int64_t video_feedback_max_nack_seqs = 64;

// The maximum age of a pending feedback report, in microseconds.
// Default = 1000000 (1 s).  0 means "never expire".
int64_t video_feedback_max_report_age_us = 1'000'000;
```

The **key-frame rate limit is global**, not per-door: whichever door
raised the request, the peer sees at most one per interval. The peer's
encoder is the shared resource the limit protects, so a per-door limit
would let the two doors add up to twice the rate.

The **NACK cap keeps the newest losses**: RFC 4585 §6.2.1 shapes the
request around the recent losses, and the sender's retransmission cache
only holds the recent packets — a request for an old loss is a request
the peer cannot answer. When the list overflows, the *oldest* entries
are the ones dropped.

A **stale report is dropped**, not sent: a report that sat in the queue
while the link was down says nothing about the picture any more, and
sending it costs a round trip.

The bound is observable through `Stats`:

| Counter | What it says |
|---|---|
| `video_feedback_pli_requests` | the **demand** — every key-frame request the receiver raised |
| `video_feedback_pli_allowed` | what the rate limit let through (and therefore onto the wire) |
| `video_feedback_pli_suppressed` | what the rate limit dropped |
| `video_feedback_pli_from_loss` | the requests raised by the **loss** door (a frame the jitter buffer could not assemble) |
| `video_feedback_pli_from_decode_failure` | the requests raised by the **decode-failure** door (the decoder refused a frame that assembled perfectly) |
| `video_feedback_nack_seqs_dropped` | the NACK sequence numbers the per-report cap discarded |
| `video_feedback_reports_expired` | the pending reports dropped for being too old |

The two doors are counted separately because they call for completely
different fixes: a rising `..._from_loss` means the network is lossy,
a rising `..._from_decode_failure` means the bitstream is undecodable.

### 9.4 The default device policy (v1-device-policy D3)

```cpp
static DevicePolicyInfo default_device_policy() noexcept;
```

Read-only query for the current platform's default capture-backend ids —
the table `EngineConfig::video_capture_name = "auto"` resolves through.
`DevicePolicyInfo` is a value type carrying four `std::string`s:

| Field | Meaning |
|---|---|
| `screen_capture_backend` | default screen-capture backend plugin id |
| `camera_capture_backend` | default camera backend plugin id |
| `audio_capture_backend` | **reserved** (empty until D1 — the engine has no audio-device layer yet) |
| `audio_playback_backend` | **reserved** (empty until D1) |

The screen / camera values are `"windows-wgc"` / `"camera-mediafoundation"`
on Windows, `"macos-screencapturekit"` / `"camera-avfoundation"` on
macOS, `"linux-x11"` / `"camera-v4l2"` on Linux and
`"synthetic"` / `"synthetic-camera"` elsewhere.  A host that names a
device explicitly never consults this — the explicit id always wins.  A
policy backend that this build does not register is **not** an error:
`"auto"` degrades to "no capture device" (warn) instead of firing
`kVideoCapturePluginMissing`.

### 9.5 The engine-driven video renderer (v1.4 / plan D2)

By default the engine **runs the renderer**: `EngineConfig::video_sink_name`
resolves a video sink plugin, the same pluggable surface the host used to
fill by hand.  The host keeps the ownership rules from the old days — no
D3D11 / Metal / EGL code, and usually no window code either (the platform
renderers *can* create their own window, but only when the host asks for
one — see "Window ownership" below).

| Config field | Meaning |
|---|---|
| `video_sink_name` | Which sink the engine installs.  `"auto"` (the default) resolves the platform renderer and **degrades instead of failing**; `""` is today's behaviour verbatim (headless); `"none"` installs no sink at all; `"headless"` is the explicit spelling; any other value is an explicit backend id — a promise. |
| `video_sink_viewport` | Whether the decoded picture is meant to reach the screen.  **False (the default)** means the renderer runs offscreen; **true** means the renderer builds its own output window and presents into it.  See "Window ownership" below. |

**The value table.**

| value | behaviour |
|---|---|
| `""` | The headless sink — today's behaviour, **verbatim**.  `video_sink()` is non-null, `video_sink_backend == "headless"`. |
| `"auto"` | The new default.  Resolves the per-platform preference table (Windows `d3d11`, macOS `metal`, Linux `egl`, fallback `headless`).  **`open()` never fails under `"auto"`**: if the platform renderer cannot be used the engine warns and degrades to `"headless"`, and `Stats::video_sink_backend_reason` says why (`no-platform-backend` on a non-desktop build, `backend-not-registered` when the build registers no such backend, `backend-open-failed` when the renderer refused to open — a machine with no display is a normal machine, not an error). |
| `"none"` | An explicit opt-out: **no sink instance** is created.  `video_sink()` is null and `video_sink_backend` is empty. |
| `"headless"` | The explicit spelling of `""`. |
| `"<id>"` | An explicit backend id.  Explicit ids are **promises**: an unknown id fails `open()` with `kVideoSinkPluginMissing`, an id whose `open()` fails is a hard error (`kVideoSinkOpenFailed`) — the exact opposite of `"auto"`, never a silent degrade. |

**Window ownership.**  A presentation sink needs an output surface.  The
host's window always wins: when `plugins::VideoSinkConfig::native_window`
is set, the renderer presents into it and never creates anything.

When the host supplies no window, the renderer *can* build one — a
`CreateWindowExW` / `ShowWindow(SW_SHOWNOACTIVATE)` window on Windows
(carrying `WS_EX_TOOLWINDOW`, so it stays out of the taskbar and the
Alt-Tab list), an `NSWindow` backed by a `CAMetalLayer` on macOS, a
Wayland surface / X11 window for EGL.  **That is opt-in**: the renderer
creates its own window only when
`EngineConfig::video_sink_viewport` (or the seam's
`plugins::VideoSinkConfig::create_viewport_window`) says so.  The default
is **off**, and the reason is not cosmetic — a renderer that opens a
visible window on its own initiative flashes it on the developer's
desktop, and a test suite that runs hundreds of cases does that hundreds
of times.  With the viewport off the sink runs offscreen: the
presentation clock still ticks and the frames are still counted, but
nothing appears on the screen.

A host that wants the picture on the screen sets `video_sink_viewport =
true`; `examples/remote-desktop/rd_viewer.cpp` does exactly that whenever
its sink is a real renderer rather than `"headless"`.  A host that wants
no window at all keeps using `offscreen` or `"none"`.

**Inspection.**  `video_sink()` returns the installed
`plugins::IVideoSink*` (null only for `"none"` or an uninstallable
fallback).  `Stats` reports which backend actually runs:
`Stats::video_sink_backend` is the installed sink's id
(`"headless"`, the platform id, or the explicit id) and
`Stats::video_sink_backend_reason` is the degrade reason (empty when the
session did **not** degrade).

---

## 10. DataChannel control plane

```cpp
std::unique_ptr<plugins::IDataChannel>
create_data_channel(std::string_view label) noexcept;
```

Allocates a new `IDataChannel` from the resolved factory; opens it
with a default config (label, ordered, reliable, priority 128).
Returns `nullptr` if:

- the DataChannel backend is disabled (the resolved factory id is
  empty — see `docs/api/engine_config.md` §2.6.1);
- the engine is not `open()`ed yet;
- the factory id was not registered (caller forgot
  `core::register_all_default_plugins()`);
- the factory's `create()` returned null (catastrophic impl bug).

Must be called after `open()`. The returned channel is owned by the
caller; pass it to a transport-stack or use it directly via the
`IDataChannel` interface.

`set_on_data_message()` and `set_on_data_state()` install the
matching callbacks on **every** new channel created after the call.

### 10.1 `set_on_data_message_ex()` — the channel identity

```cpp
void set_on_data_message_ex(
    plugins::DataChannelMessageCallback cb) noexcept;
```

The classic `set_on_data_message()` callback carries the payload only,
so a host that opens two channels cannot tell which one a message
arrived on.  `set_on_data_message_ex()` is the **additive** counterpart
(v1.2.0, the plan's WP-C1): the plain callback keeps its exact
signature and behaviour — it is installed on every channel the engine
creates, as a fan-out shorthand.

```cpp
struct plugins::DataChannelMessage {
    std::string_view label;       // the channel's label
    std::uint32_t    channel_id;  // engine-assigned id (1-based, per engine)
    plugins::BufferView data;     // the payload (same bytes the plain callback gets)
};
```

The engine assigns `channel_id` (monotonically increasing per engine,
starting at 1) and fills `label` when the backend does not report one.

With the framed mode (`EngineConfig::datachannel_framing =
plugins::DataChannelFraming::kFramed`) the *peer's* label decides which
channel a message belongs to.  With the raw mode there is no identity
on the wire, so every channel of the connection sees the same bytes —
use the framed mode (or one channel per connection) if you need
isolation.  See [`docs/guides/datachannel.md`](../guides/datachannel.md).

> **Status**: the default backend is `"web_transport"` (WebTransport /
> QUIC), which is the only backend with a wire path. The `"sctp"` backend
> was **retired from the public selectors in v1.2.0** — it was an
> in-process loopback socket that could never reach a peer. Physical
> SCTP-over-DTLS plumbing was never landed and is not part of the
> current architecture — see `docs/adr/ADR-014-web-transport-vendor.md`.

---

## 11. Tick loop

```cpp
int tick() noexcept;
```

The single user-facing "drive everything forward" call. Each
invocation:

- drains the ICE socket and dispatches RTP / DTLS / STUN packets;
- feeds inbound RTP into the jitter buffer;
- runs the decoder for any playable frames;
- fires scheduled outbound frames from the BWE / scheduler;
- pumps the DTLS retransmit timer (~50 ms cadence is sufficient).

Typical cadence: 10 ms to 33 ms. The engine tolerates a missed
tick; long stalls just delay outbound packets.

Returns the number of packets dispatched in this tick.

---

## 12. Plugin accessors

Every accessor returns a non-owning pointer or a `const` reference.
Pointers can be `nullptr` if the configured id was not registered
**or** if `open()` was not called yet.

| Accessor | Returns | Useful for |
|---|---|---|
| `transport()` / `ice_transport()` | `plugins::ITransport*` / `IICETransport*` | Direct ICE inspection / extra candidate listening. |
| `audio3a()` | `IAudio3A*` | Setting up pre/post-3A PCM taps for ASR. See `examples/demo-agent-gateway`. |
| `audio_source()` / `audio_sink()` | `IAudioSource*` / `IAudioSink*` | The capture / playback devices the engine opened (v1.4 / plan D1), or `nullptr` when no device is in use. |
| `codec_plugin()` | `ICodec*` | Codec-specific statistics / introspection. |
| `video_source()` / `video_sink()` | `IVideoSource*` / `IVideoSink*` | Running your own capture / render loops outside the engine frame cadence. |
| `video_receiver()` / `video_sender()` | `IVideoReceiver*` / `IVideoSender*` | RTP-level instrumentation (NACK, FU-A). |
| `video_codec()` | `IVideoCodec*` | H.264 hardware vs software selection. |
| `bwe()` | `IBwe*` | Reading BWE stats (current estimate, loss rate). |
| `scheduler()` | `IScheduler*` | Reading scheduler queue depth. |
| `xr_report()` | `IXrReport*` | Reading the peer's decoded RFC 3611 extended reports. |
| `config()` | `const EngineConfig&` | Re-reading effective cfg (e.g. PCM rate for ASR sidecar). |

### Runtime swaps

```cpp
void set_video_sink(std::unique_ptr<plugins::IVideoSink> sink) noexcept;
void set_video_source(std::unique_ptr<plugins::IVideoSource> source) noexcept;
```

Replace the plugin instance at runtime (e.g. swap `"headless"` for
an SDL renderer). Closes the previous instance, transfers ownership
of the new one. Pass `nullptr` to detach.

---

## 13. Error codes & inspection

```cpp
uint32_t last_open_rc() const noexcept;

struct Stats {
    // ---- The reading's identity (v1.2.0, plan Track S / S1) ------------
    // When the rest of this struct was measured.  `snapshot_time_us`
    // is on the engine's monotonic clock (the same origin `tick()`'s
    // `now_us` uses); `snapshot_seq` increases by one per reading and
    // therefore orders two readings even when their timestamps are
    // identical.
    int64_t  snapshot_time_us = 0;
    uint64_t snapshot_seq     = 0;

    int srtp_drops = 0;

    // Video counters (v1.1 Track V) — cumulative since open().
    uint64_t video_frames_sent = 0;
    uint64_t video_packets_sent = 0;
    uint64_t video_bytes_sent = 0;
    uint64_t video_frames_received = 0;
    uint64_t video_nack_seqs_queued = 0;
    uint64_t video_keyframes_q = 0;
    uint64_t video_feedback_applied = 0;
    uint64_t video_keyframes_applied = 0;

    // Video zero-copy counters (Phase 3 / C3) — cumulative since open().
    uint64_t video_zero_copy_frames = 0;     // frames that took the GPU path
    uint64_t video_zero_copy_fallbacks = 0;  // GPU frames that fell back to CPU

    // Audio send counters (audio pipeline WP-5) — cumulative since open().
    uint64_t audio_frames_sent = 0;
    uint64_t audio_bytes_sent = 0;
    uint64_t audio_encode_errors = 0;
    uint64_t audio_samples_in = 0;
    uint64_t audio_samples_pending = 0;

    // Audio receive counters (audio pipeline WP-8 / spec B-13).
    uint64_t audio_packets_sent = 0;
    uint64_t audio_frames_received = 0;
    uint64_t audio_samples_received = 0;
    uint64_t audio_concealed_frames = 0;
    uint64_t audio_fec_recovered_frames = 0;
    uint64_t audio_packets_lost = 0;
    uint64_t audio_decode_errors = 0;
    uint64_t audio_packets_dropped = 0;
    uint32_t audio_jitter_ms = 0;
    uint32_t audio_playout_delay_ms = 0;

    // Adaptive audio bitrate — instantaneous, not cumulative.
    uint32_t audio_encoder_bitrate_bps = 0;

    // Network feedback / BWE loop (v1.2.0 M1) — mixed semantics.
    uint64_t net_feedback_blocks    = 0;  // RTCP blocks routed to the collector
    uint64_t bwe_feedback_samples  = 0;  // samples handed to IBwe::on_feedback()
    uint32_t bandwidth_estimate_bps = 0; // current BWE target, in bit/s

    // RTCP Extended Reports (v1.2.0, session plan X1).
    uint64_t xr_report_blocks      = 0;  // XR blocks routed to the consumer

    // The data channels the host opened (v1.2.0, plan Track S / S6).
    // One entry per channel, in creation order; empty when the host
    // never opened one.  See §13.1 for the entry's fields.
    std::vector<DataChannelStats> data_channels;
};
Stats stats() const noexcept;

// ---- The named, snapshot-able view (v1.2.0, plan Track S / S1) --------
//
// `stats_snapshot()` returns the same numbers as `stats()`, but keyed
// by name, stamped with the reading's instant and sequence number.
// `stats_delta_since()` diffs the current reading against an earlier
// one and answers with the *increment* over the interval.
//
// The types live in `nimrtc/core/stats_registry.hpp`
// (`core::StatSnapshot`, `core::StatsDelta`).
core::StatSnapshot stats_snapshot() const noexcept;
core::StatsDelta   stats_delta_since(const core::StatSnapshot& previous) const noexcept;
```

`last_open_rc()` returns the last error code from `open()` /
`pre_open()`. Useful in tests when the bool result is non-OK but
you want to know which subsystem failed:

| Bit region | Subsystem |
|---|---|
| `0x2000` | DTLS |
| `0x1FFF` | ICE / transport |
| `0x1A00` | audio3A |
| `0x1800` | codec |
| `0x1900` | scheduler |
| `0x1700` | JB |

`Stats::srtp_drops` increments on every SRTP auth-tag mismatch.

The remaining fields are **cumulative counters since `open()`**, except
`audio_encoder_bitrate_bps` and `bandwidth_estimate_bps`, which are the
**instantaneous** values (the outbound audio encoder's current bitrate and
the bandwidth estimator's current target respectively). `audio_jitter_ms`
and `audio_playout_delay_ms` are likewise instantaneous measurements
rather than running totals.

### 13.1 The snapshot lifecycle (v1.2.0, plan Track S / S1)

The flat struct answers *"how much has happened since `open()`"* and
nothing else.  A host that wants a **rate** — bytes per second, packets
per second — has to take two readings and work out how far apart they
were, and it has to know which fields are counters and which are gauges.
The snapshot lifecycle gives it both.

```cpp
auto base = engine.stats_snapshot();       // the baseline reading
// ... the session runs ...
auto d = engine.stats_delta_since(base);    // the increment over the interval

if (d.valid()) {
    std::int64_t frames = 0;
    if (d.counter_delta("", "video_frames_sent", frames)) {
        // `frames` is the increment that actually happened between the
        // two readings -- not the absolute value.
        const auto seconds = d.duration_us() / 1e6;
        const auto fps = seconds > 0.0 ? frames / seconds : 0.0;
    }

    std::int64_t bitrate = 0;
    if (d.gauge_value("", "video_tx_bitrate_bps", bitrate)) {
        // A gauge has no increment: the difference of two instantaneous
        // bitrates is not a bitrate.  The value at the end of the
        // interval is the answer.
    }
}
```

The contract:

- **`counter_delta()` refuses to answer for a gauge** and
  **`gauge_value()` refuses to answer for a counter.**  Getting the
  classification wrong is the classic statistics bug (subtracting two
  EMA bitrates and reporting a negative rate), so the semantics travel
  with the number and the API refuses to mix them up.
- **`duration_us()`** is the length of the interval, so a host can turn
  an increment into a rate without keeping its own clock.
- **A default-constructed snapshot has no usable baseline.**  The delta
  would be measured against a made-up zero, so `valid()` is false.
- **The sequence number orders two readings even when their timestamps
  are identical** (a coarse clock, or two readings inside the same
  microsecond), so "the interval between these two readings" is always
  well-defined.
- **A snapshot outlives the engine.**  It owns its data, so a host may
  keep it as a baseline after the engine is destroyed.

The registry is a **projection** of the flat struct, taken at reading
time: the flat struct stays the single authority, so the two views can
never drift apart.  The registry is an *instance*, not a singleton — two
engines in one process get two registries.

**This is not a standard `RTCStatsReport`.** The struct is flat and
engine-wide, with one exception: `inbound_rtp_streams` carries a
per-source breakdown (see §13.2). There is still no ICE
candidate-pair / RTT dimension. The snapshot lifecycle adds the
*time* and the *semantics*, not the missing dimensions. The
`src/modules/video_stats/` module **is** wired into the engine (the
private bridge `src/engine/src/video_stats_bridge.hpp`), so its freeze
counts, keyframe intervals, QP samples and drop-cause breakdown are
reachable through this accessor as `video_freeze_count` /
`video_freeze_total_ms` / `video_keyframe_interval_avg_ms` /
`video_qp_sum` / `video_qp_count` / `video_frames_dropped_jb` /
`video_frames_dropped_decoder`. For the full picture — the planned
stream-level shape and the missing dimensions — see
`docs/plan/stats-surface-plan.md` (landed later in the v1.2 stack, so
referenced here by path, not by link).

### 13.2 The inbound per-source breakdown (v1.2.0, plan Track S / S2)

The flat counters answer *"how much arrived"*, but not *"from
whom"*.  A peer that sends **simulcast** (RFC 5956 — several encodings
of the same picture on different SSRCs) puts three different
qualities into one counter, and a host that receives from more than
one source has the same problem.  There is no way to ask *"how many
packets arrived on SSRC X?"* — which is the question a loss
investigation actually asks.

`Stats::inbound_rtp_streams` is the receive-side counterpart of
`Stats::video_streams`: one entry per inbound RTP source.

```cpp
struct Stats {
    // ---- The inbound per-source breakdown (v1.2.0, plan Track S / S2) ----
    // One entry per inbound RTP source, keyed by SSRC.
    std::vector<InboundRtpStats> inbound_rtp_streams;
};

struct InboundRtpStats {
    std::uint32_t ssrc = 0;            // the RTP source identifier
    std::uint8_t  payload_type = 0;    // observed, not negotiated
    std::string   kind;                // "video", "audio" or "unknown"

    std::uint64_t packets_received = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t packets_lost = 0;      // the gaps in the sequence space
    std::uint64_t nacks_sent = 0;       // the retransmission requests sent
    std::uint64_t frames_received = 0;   // the assembled access units
    std::uint64_t frames_incomplete = 0; // arrived but not assemblable
    std::uint32_t jitter_ms = 0;
    std::uint32_t playout_delay_ms = 0;
};
```

The contract:

- **The SSRC is the key**, because the SSRC is the only thing on the
  wire that identifies a source.  The payload type says how to
  *decode* a packet, not who sent it.
- **The entry accumulates over the life of the source.**  A source
  that restarts (a new SSRC) gets a new entry; a source that
  disappears keeps its entry, so a host reading the list after the
  peer stopped still sees the totals it produced.
- **`packets_lost` is the loss figure**, counted by the receive
  pipeline — the only party that sees the sequence space.
- **The payload type is observed, not negotiated.**  A peer that
  changes its payload type mid-stream (RFC 3264 §5.1 allows a PT
  change after a re-INVITE) shows up as a changed value.

The outbound side keeps its existing `Stats::video_streams`
(`VideoStreamStats`: `ssrc` / `packets_sent` / `bytes_sent` /
`keyframes_sent`); this row adds the missing inbound counterpart.

### 13.3 The per-stream entries (v1.2.0, plan Track S / S2 and S4)

The flat counters above are **engine-wide**.  Two lists add the missing
*per-stream* dimension, and both are **additive** — the flat fields stay,
and they describe the same stream the list's first entry carries.

```cpp
// The video side (remote-desktop plan H4): one entry per active video
// stream, the default one included.
std::vector<VideoStreamStats> video_streams;

// The audio side (plan Track S / S4): one entry per active audio track.
std::vector<AudioStreamStats> audio_streams;
```

#### `VideoStreamStats`

| Field | Meaning |
|---|---|
| `stream_id` | The stable stream id (`"default"`, `"screen"`, `"camera"`, …). |
| `ssrc`, `payload_type`, `mid` | The stream's identity on the wire. |
| `codec_name`, `backend_id` | The encoding and the backend it runs on. |
| `bitrate_bps`, `keyframe_rate_hz` | The stream's current rates. |
| `ladder_level`, `bandwidth_share_bps`, `bandwidth_weight` | The rate controller's rung and the stream's share of the session budget. |
| `frames_sent`, `frames_received`, `frames_encoded` | The frame counts. |
| `packets_sent`, `bytes_sent`, `keyframes_sent` | The outbound RTP counts. |

#### `AudioStreamStats`

One **audio track** — the session's audio m-line — in *both* directions.

| Field | Meaning |
|---|---|
| `stream_id` | The stable track id (`"default"` for the engine's own track). |
| `ssrc` | The SSRC of the **outbound** encoding. |
| `inbound_ssrc` | The SSRC of the **inbound** source (0 when nothing arrived). The peer picks its own SSRC, so the two ends of the track differ. |
| `payload_type`, `codec_name` | The negotiated payload type and the encoding name. |
| `clock_rate_hz`, `channels`, `frame_duration_ms` | The stream's format. |
| `bitrate_bps`, `encoder_bitrate_bps` | The current outbound rate, and the encoder's current rate (the BWE-linked adaptation lowers it). |
| `packets_sent`, `bytes_sent` | The outbound RTP counts. |
| `encode_errors`, `samples_in` | The PCM the host handed over, and the frames that could not be encoded. |
| `packets_received`, `bytes_received` | The inbound RTP counts (the whole packet on the wire). |
| `frames_received`, `samples_received` | What the engine handed to the host. |
| `frames_concealed`, `frames_fec_recovered` | The frames produced by concealment and by in-band FEC. |
| `packets_lost`, `decode_errors` | The pipeline's own figures — it is the only party that sees the source's sequence space. |
| `jitter_ms`, `playout_delay_ms` | The pipeline's instantaneous measurements. |

**The audio list is not a second per-SSRC table.**  The per-SSRC inbound
breakdown lives in `Stats::inbound_rtp_streams` (plan Track S / **S2**),
which carries every inbound source — audio included — keyed by SSRC.  The
audio list adds the part S2 cannot know: the **outbound** half of the
track, and the *pipeline's* figures for the source the receive side is
actually working on.  A host that summed both lists would otherwise count
every inbound packet twice.

**The audio plane is single-stream today**, so the list normally holds
exactly one entry.  It is a **list** anyway: a second audio source (a
second microphone, a programme bus) would make the flat counters
unattributable, and a list grows where a single struct would have to be
redrawn — redrawing it would break every caller at once.

### 13.4 The data-channel statistics (v1.2.0, plan Track S / S6)

`Stats::data_channels` carries **one entry per data channel the host
opened** with `create_data_channel()`, in creation order.  The list is
empty when the host never opened a channel, so a host that does not use
the data plane sees no change in its telemetry.

```cpp
struct DataChannelStats {
    uint32_t channel_id = 0;   // the engine-assigned id (1-based)
    std::string label;         // the label the host gave the channel
    std::string reliability;    // "reliable-ordered" / "unreliable" /
                                // "partial-reliable-ttl" /
                                // "partial-reliable-count"

    uint64_t messages_sent = 0;          // the backend's count
    uint64_t bytes_sent = 0;             // the backend's count
    uint64_t messages_received = 0;      // the engine's count
    uint64_t bytes_received = 0;         // the engine's count
    uint64_t messages_dropped = 0;       // the backend's count
    uint64_t messages_retransmitted = 0; // the backend's count
    uint64_t messages_pending = 0;       // the backend's gauge

    std::string state;   // "connecting" / "open" / "closing" /
                         // "closed" / "error" ("" when none was reported)
};
```

**The two-sided model.**  The counters come from two different parties,
and that is the whole design:

- The **inbound** half (`messages_received` / `bytes_received`) is the
  *engine's* account: the engine installs the channel's callbacks, so it
  sees every inbound message and counts it.
- The **outbound** half (`messages_sent` / `bytes_sent` /
  `messages_dropped` / `messages_retransmitted` / `messages_pending`) is
  the *backend's* account, reached through the additive
  `plugins::IDataChannel::stats()` seam.  The engine cannot derive
  those numbers — it only sees the `send()` call, and what happens to
  the message afterwards (queued, retransmitted, dropped when its
  lifetime expired) happens inside the backend.

A backend that does not implement the seam reports zeroes there.  The
engine republishes those zeroes rather than inventing a number.

**The counters are cumulative** since the channel was created, so the
difference of two readings is the increment.  `messages_pending` is the
exception: it is a *gauge* — the depth of the outbound queue at the
moment of the reading.

**`reliability` is a name, not an enum.**  The engine publishes the mode
as the canonical string so a host does not have to know the enum's
numeric values.  A host that wants to know *why* a message was dropped
reads this field: the two partial modes are the only ones that drop.

### 13.5 The transport plane (v1.2.0, plan Track S / S5)

The statistics surface above describes the *media*.  `Stats::transport`
describes the **path the media travels over**: ICE (RFC 8445) → DTLS
(RFC 6347) → SRTP (RFC 3711).

Before S5 the transport had no statistics of its own: `Stats::srtp_drops`
counted the packets the outbound path could not encrypt, `dtls_state()`
exposed the DTLS state, and ICE — the plane that decides *where the
packets go* — had **nothing at all**: no candidate pair, no round-trip
time, no restart count.

```cpp
struct Stats {
    // ---- The transport plane (v1.2.0, plan Track S / S5) ----
    TransportStats transport;
};
```

#### `TransportStats`

**ICE (RFC 8445)**

| Field | Meaning |
|---|---|
| `ice_state` | The agent's state: `"disconnected"`, `"gathering"`, `"connecting"`, `"connected"`, `"completed"` or `"failed"`.  Empty before the engine has a transport.  The same vocabulary as `ice_state_string()`. |
| `ice_restart_count` | The ICE restarts performed so far (RFC 8445 §9.1.1.1).  0 means the session never restarted. |
| `ice_recovery_state` | The engine's reconnection-policy state (v1.4 N3, ADR-031): `"idle"` / `"grace"` / `"backoff"` / `"terminal"` — or `"disabled"` when `EngineConfig::ice_recovery_policy` is not `"auto"`. |
| `ice_recovery_restarts` | The restarts the *recovery policy* consumed in the current sliding 10-minute window (0 when disabled).  Distinct from `ice_restart_count` — the ICE agent's lifetime count. |
| `ice_recovery_terminal_reason` | `"budget-exhausted"` / `"manual-blocked"` once the machine is terminal; empty otherwise (including when disabled). |
| `turn_credentials_state` | The TURN-credentials provider pull (v1.4 N7, ADR-032): `""` (no provider configured) / `"idle"` / `"ok"` / `"unavailable"` / `"rejected"`.  A flat `Stats` field (like the `ice_recovery_*` trio above), reported here next to the policy surface. |
| `last_path_change_reason` | The reason of the **last** path change (v1.4 N8, ADR-033): `"local-address-changed"` / `"peer-restart"` / `"relay-fallback"` / `"manual"` — empty = never changed.  The same closed vocabulary as `IceRecoveryReport::reason`; populated even when `ice_recovery_policy == "off"`. |
| `last_path_change_new_pair` | The pair the session was on after the last path change (v1.4 N8, ADR-033): empty = none selected yet.  A flat `Stats` field like `last_path_change_reason`, reported here next to the pair surface. |
| `selected_pair` | The candidate pair the agent nominated, or empty.  Empty until the connectivity checks settle on a path, which is *after* the agent reaches Connected. |
| `relayed` | True when the selected pair goes through a TURN relay.  *Derived* from the pair's candidate types, published separately because it is the one bit a host actually branches on. |
| `rtt_ms` | The round-trip time in milliseconds, **instantaneous** (the latest measurement, not a running total).  0 until a Receiver Report with a usable LSR / DLSR pair has arrived. |
| `has_rtt` | True once a round-trip time has actually been *measured*.  This is what distinguishes "the RTT is 0 because the path is fast" from "the RTT is 0 because nothing was ever measured". |

**DTLS (RFC 6347)**

| Field | Meaning |
|---|---|
| `dtls_state` | The handshake's state: `"closed"`, `"initial"`, `"hello-verify"`, `"hello-sent"`, `"hello-received"`, `"certificate-received"`, `"key-exchange"`, `"change-cipher-spec"`, `"finished"`, `"connected"` or `"failed"`. |
| `dtls_role` | `"client"` or `"server"` — the role the negotiation resolved (RFC 5763 §5).  Empty before the negotiation has run: "the role is not negotiated yet" is a different fact from "we are the server". |
| `dtls_fingerprint` | The local certificate's fingerprint, in the colon-separated UPPER hex form of RFC 8122 (`"AA:BB:..."`). |

**SRTP (RFC 3711)**

| Field | Meaning |
|---|---|
| `srtp_installed` | True once the DTLS-derived keying material has been installed.  This is the *authority* on "is this session encrypted". |
| `srtp_rtp_packets_protected` / `_unprotected` | The RTP packets the SRTP layer encrypted / decrypted. |
| `srtp_rtcp_packets_protected` / `_unprotected` | The RTCP packets the SRTP layer protected / unprotected. |
| `srtp_auth_failures` | The packets the SRTP layer refused: an authentication-tag mismatch, a replay, or a malformed packet.  A non-zero count means the peer sent something the engine could not authenticate — the most security-relevant number on this surface. |
| `srtp_drops` | The packets the engine had to send but could not encrypt.  The same figure the flat `Stats::srtp_drops` carries, repeated so a host that reads only the transport entry has the complete picture. |

#### `IceCandidatePairInfo`

```cpp
struct IceCandidatePairInfo {
    IceCandidateInfo local;   // our end of the pair
    IceCandidateInfo remote;  // the peer's end of the pair
};

struct IceCandidateInfo {
    std::string   address;   // the IP literal (no brackets, even for IPv6)
    std::uint16_t port = 0;
    std::string   protocol;  // "udp" / "tcp", lower-cased
    std::string   type;      // "host" / "srflx" / "prflx" / "relay"
};
```

Both ends are published because a diagnosis usually needs both — *"where
does my traffic leave from, and where does it arrive?"* — and because the
two ends can differ in **kind**: a local host candidate reaching a remote
relay candidate is the shape of a TURN-relayed path.

#### What this surface deliberately does *not* do

**No new probes.**  The RTT is the one the engine already measures from
the peer's Receiver Reports (the LSR / DLSR pair against our own Sender
Report, RFC 3550 §6.4.1) — the *same* figure the bandwidth estimator
consumes.  The statistics surface republishes it; it does not measure it
a second time, because two measurements of one quantity would eventually
disagree.

**No per-candidate-pair list.**  libjuice nominates exactly one pair and
does not expose the others it tried, so a list of candidate pairs would
be a list of one — and would suggest the engine can enumerate paths it
cannot actually see.  What the engine publishes is the pair that is *in
use*, plus the counters that say whether it is working.

**No copies of another owner's state.**  The ICE state and the candidate
pair are owned by the ICE agent (read through the `plugins::IICETransport`
seam), the DTLS state by the DTLS session, the SRTP counters by the SRTP
context, and the round-trip time by the network-feedback collector.  The
surface republishes each of them; it keeps no second copy that could go
stale or disagree.

### 13.6 The remote-desktop pipeline breakdown (v1.2.0, plan Track S / S7)

The QoE block (`Stats::qoe`) measures what the operator **felt** — freezes,
latency, sharpness.  `Stats::remote_desktop` answers the operator's
follow-up question: **why**.  It is the video pipeline broken into its
stages, each reported in the unit a human reasons about.

A screen-share session has three stages that matter, and each one fails
in its own way: the **capture** is not grabbing the desktop fast enough,
the **encoder** cannot keep up with the capture cadence, or the **decoder**
on the peer cannot keep up with what we send.  Before S7 the answers were
scattered across a dozen prefixes (`video_stage_encode.mean_us`,
`video_capture_frames`, `video_frames_decoded`, …) and the host had to
know which prefix carried which half — and still could not answer the
question, because there was **no capture frame rate** and **no
decode-side cost** anywhere on the surface.

```cpp
struct Stats {
    // ---- The remote-desktop breakdown (v1.2.0, plan Track S / S7) ----
    RemoteDesktopStats remote_desktop;
};
```

#### `RemoteDesktopStats`

The shape follows the pipeline: three stage blocks, each with its own
rate, cost and count, plus the two counters that describe the **gap
between two stages**.

```cpp
struct RemoteDesktopStats {
    RemoteDesktopCaptureStats capture{};
    RemoteDesktopEncodeStats  encode{};
    RemoteDesktopDecodeStats  decode{};

    std::uint64_t frames_dropped_before_encode = 0;
    std::uint64_t frames_decoded_without_local_encode = 0;
};
```

A host compares the halves — "the capture ran at 30 fps and the encoder
only at 24" — without knowing which prefix carries which half.

##### `capture` — `RemoteDesktopCaptureStats`

| Field | Meaning |
|---|---|
| `capture.fps` | The rate the capture device delivered frames, in frames per second.  Measured on the **device's own capture timestamps**, over the device's *sequence span* (`last_seq - first_seq` over `last_ts - first_ts`), so a device that stamps frames itself still yields the rate it actually ran at — and a device whose queue overflowed (the pump fell behind and the device dropped frames) is still counted, because a delivery is identified by its **sequence number**, not by its timestamp.  0 when fewer than two frames were seen — "not measurable yet", not "zero frames per second".  0 also when the device's clock **moved backwards** during the session (a re-created source, a re-negotiated portal): the span then no longer describes the interval the count covers, so the rate is reported as unknown rather than as a plausible-looking wrong number. |
| `capture.ms` | The mean cost of the capture device handoff, in milliseconds. |
| `capture.frames` | The frames the capture device delivered.  A delivery is identified by its **sequence number**, not its timestamp: a bounded device queue drops the oldest frame when the pump falls behind, and such a frame's successor may carry a timestamp that is not greater than the last one the pump saw.  Those frames are still deliveries and are counted; only a repeated sequence number (the same frame handed over twice) is not. |

##### `encode` — `RemoteDesktopEncodeStats`

| Field | Meaning |
|---|---|
| `encode.fps` | The rate the encoder ran at, in frames per second — the stage's *throughput*, over the stage's whole life.  This is the number a "can this machine encode 1080p60?" question is really asking. |
| `encode.ms` | The mean cost of the codec's `encode()` call, in milliseconds. |
| `encode.p95_ms` | The p95 of the encode cost, in milliseconds — **the number a stutter shows up in**.  A good mean with a terrible p95 is a session that hiccups. |
| `encode.frames` | The frames the encoder processed. |

##### `decode` — `RemoteDesktopDecodeStats`

| Field | Meaning |
|---|---|
| `decode.fps` | The rate the decoder ran at, in frames per second. |
| `decode.ms` | The mean cost of the codec's `decode()` call, in milliseconds. |
| `decode.p95_ms` | The p95 of the decode cost, in milliseconds. |
| `decode.frames` | The frames the decoder processed. |

##### The cross-stage counters

These stay at the **top level** of `RemoteDesktopStats`, because they
are about the gap *between* two stages and belong to neither.

| Field | Meaning |
|---|---|
| `frames_dropped_before_encode` | `max(0, capture.frames - encode.frames)` — the frames that were captured but never reached the encoder. |
| `frames_decoded_without_local_encode` | `max(0, decode.frames - encode.frames)` — **informational only**, not an error counter.  A receive-only session has an empty encode stage and a busy decode stage, so this is deliberately kept out of the drop accounting. |

#### Everything here is *derived*, never measured twice

Each value is read from a measurement that **already exists**:

- The **costs** come from the per-stage instrumentation
  (`Stats::video_stage_capture` / `video_stage_encode` /
  `video_stage_decode`) — the same table the flat fields expose.  The
  breakdown converts its units (nanoseconds → milliseconds) and adds
  nothing, so the two views **cannot drift apart**.
- The **rates** come from two sources that the stage table does not
  carry: the capture rate from the device's own capture timestamps, and
  the encode / decode rates from each stage's wall-clock span (the time
  between its first and its latest sample).

That is why adding this block cost **no work on the frame path**: the
stage instrumentation already bracketed every stage, and the capture
timestamps already travelled with every frame.

#### The zero reading

Every field is 0 until the thing it describes has actually happened.  A
host that does not configure a capture device has no capture stage, and a
session that never received a video frame has no decode stage — the
zeros are the **correct** reading, not a missing value.

### 13.7 The GPU delivery path (v1.2.0, plan Track E / E5)

`VideoSinkStats` (the sink-side counters, not the engine `Stats`) carries
three counters that say **which route a decoded frame took to the
display**:

| Field | Meaning |
|---|---|
| `gpu_device_copies` | GPU surfaces copied **device-to-device** into the present target.  The picture never entered host memory between decode and present — this is the zero-copy hand-off, confirmed. |
| `gpu_staging_fallbacks` | GPU surfaces that could not go device-to-device and had to be **staged through host memory**.  The D3D11 sink counts here when a surface is not BGRA8: it presents from the surface's CPU mirror if there is one, and otherwise refuses it so the engine decodes to host pixels. |
| `gpu_surfaces_refused` | Surfaces refused outright: a foreign backend (a Metal IOSurface on Windows), a null handle, or a zero-size surface.  A non-zero value here is a **configuration error**, not a fallback. |

The split is the point.  On a healthy DXGI loop the first counter grows
and the other two stay at zero.  A host that sees
`gpu_staging_fallbacks` move is looking at a decoder or driver
configuration that is not zero-copy **all the way to the display** — the
capture → encode leg may still be zero-copy while the decoded NV12
surface is converted on the CPU.  The engine's render-path rule
(`video_render_path.hpp`) decides the *route* (GPU surface vs CPU
staging) once at `open()`; these counters describe what actually
happened per frame afterwards.

The GPU copy itself is performed on the sink's **presenter thread**, not
on the caller's: a GPU frame handed to `render(const GpuBuffer&)` costs
the producer a reference-count bump, and the device-to-device copy
happens on the presentation clock, exactly like a CPU frame.  That is
what keeps a sink that receives a mix of GPU and CPU frames from
presenting twice per tick or skewing `frames_dropped_present`.

---

### 13.8 The stable field set (v1.2.0, plan Track S / S8)

`Stats` grew one additive block at a time (S1 – S7 above).  This section
is where that growth is written down as a **contract**: every field
`Stats` carries today is listed below, with its type and the definition
the header gives it.  The table is **generated from the header** by
`tools/dump_stats_fields.py --markdown`, and a CI gate
(`--check`) fails when the table and the header disagree — so it cannot
go stale, and a field added to the header is visible here the same day.

#### What "stable field set" promises

- **It stays.**  A field is not renamed, retyped, or removed in a minor
  release.  If a measurement is wrong, it is fixed in place.
- **Its meaning does not change.**  `capture.ms` is the mean cost of the
  device handoff; it is not quietly redefined later.  When a field's
  *scope* needs to grow (the S7 `capture.fps` fix), it grows in the way
  the field always meant — not by reinterpreting the number.
- **It only grows.**  A new block is a *new* field (or a nested type
  such as `RemoteDesktopStats`), never a reuse of an old one — which is
  why S1, S2, S4, S5, S6 and S7 all added members.

The promise is about the **field set**, not the C++ layout: `Stats` is
not an ABI type, and a host reads it through the header it compiled
against.  The layout and the exported symbols are
`scripts/check_api_manifest.py`'s business; this table is the field
set's.

#### The one exception: the S7 block's own reshape

`RemoteDesktopStats` was **reshaped** rather than grown, and it is the
only time the promise above did not hold.  It shipped with the v1.2.0
**draft** release — a tag that was pushed but whose GitHub Release was
never published — so no released artifact ever carried the old shape
and no downstream host could have compiled against it.

| Before (the draft's flat shape) | After (the published shape) |
|---|---|
| `capture_fps` | `capture.fps` |
| `capture_ms` | `capture.ms` |
| `frames_captured` | `capture.frames` |
| `encode_fps` | `encode.fps` |
| `encode_ms` | `encode.ms` |
| `encode_p95_ms` | `encode.p95_ms` |
| `frames_encoded` | `encode.frames` |
| `decode_fps` | `decode.fps` |
| `decode_ms` | `decode.ms` |
| `decode_p95_ms` | `decode.p95_ms` |
| `frames_decoded` | `decode.frames` |
| `frames_dropped_before_encode` | *unchanged* (top level) |
| `frames_decoded_without_local_encode` | *unchanged* (top level) |

The **measurements** are identical — the reshape moved names, not
semantics; `capture.fps` is exactly what `capture_fps` was.  What
changed is the *shape*: the flat list gave every stage a prefix and
left the host to know which prefix carried which half, while the
nested form puts each stage's rate, cost and count together — so a host
compares the halves ("the capture ran at 30 fps and the encoder only at
24") instead of guessing.

The two cross-stage counters (`frames_dropped_before_encode` /
`frames_decoded_without_local_encode`) stayed at the top level on
purpose: they describe the gap *between* two stages and belong to
neither.

The rule this sets: **a reshape is allowed before a release is
published, and never after.**  From v1.2.0's publication onward the
promise above holds without exception.

#### The sample apps read the same fields

The sample apps are the field set's live check — they compile against
`Stats` and print what they read, so a field that is broken *in
practice* shows up in their output, not only in a unit test.

| App | What it prints from `Stats` |
|---|---|
| `examples/demo-p2p` | `audio-rx`: `audio_frames_received` / `audio_samples_received` / `audio_concealed_frames` / `audio_fec_recovered_frames` / `audio_packets_lost` / `audio_decode_errors` / `audio_jitter_ms` / `audio_playout_delay_ms`.  The `audio-rx summary` line is parsed by `tools/run_e2e_acceptance.py`, so its shape is an e2e-contract too. |
| `examples/rd_device` | the capture half: `video_capture_device_frames_captured` / `video_capture_pumped_frames` |
| `examples/rd_viewer` | the receive half: `video_frames_decoded` / `video_frames_rendered` / `video_frames_dropped_decoder` |
| `examples/rd_verify` | the remote-desktop pipeline row by row: capture, zero-copy, encode / decode backend and hardware flag, frames sent / received / decoded / rendered, and the pass / fail verdict |

#### The nested entries

The fields whose type is a struct or a list carry one entry per
*thing*, not one number for the whole session.  Their shapes:

**`VideoStreamStats`** — one per active video stream (the default stream
included): identity (`stream_id`, `ssrc`, `payload_type`, `mid`,
`codec_name`, `backend_id`), the live rate state (`bitrate_bps`,
`ladder_level`, `bandwidth_share_bps`, `bandwidth_weight`,
`keyframe_rate_hz`) and the counters (`frames_sent`,
`frames_received`, `frames_encoded`, `packets_sent`, `bytes_sent`,
`keyframes_sent`).  The flat `video_*` fields describe the default
stream, so a single-stream host reads either.

**`InboundRtpStats`** — one per inbound RTP source, keyed by SSRC and
accumulated for the life of the source: `ssrc`, `payload_type` (as
*observed* on the wire), `kind` (`"video"` / `"audio"` /
`"unknown"`), `packets_received`, `bytes_received`, `packets_lost`,
`nacks_sent`, `frames_received`, `frames_incomplete`, `jitter_ms`.
A source that restarts gets a new entry; one that disappears keeps
its totals.

**`AudioStreamStats`** — one per active audio stream: the outbound
identity (`ssrc`, `payload_type`, `codec_name`, `clock_rate_hz`,
`channels`, `frame_duration_ms`, `bitrate_bps`, `encoder_bitrate_bps`),
the send counters (`packets_sent`, `bytes_sent`, `encode_errors`,
`samples_in`) and the receive half (`inbound_ssrc`,
`packets_received`, `bytes_received`, `frames_received`,
`samples_received`, `frames_concealed`, `frames_fec_recovered`,
`packets_lost`, `decode_errors`, `jitter_ms`, `playout_delay_ms`).

**`DataChannelStats`** — one per channel opened with
`create_data_channel()`: `channel_id`, `label`, `reliability`, the
backend's outbound counts (`messages_sent`, `bytes_sent`,
`messages_dropped`, `messages_retransmitted`, `messages_pending`), the
engine's inbound counts (`messages_received`, `bytes_received`) and the
last reported `state`.

**`TransportStats`** — the session's path, three planes deep: **ICE**
(`ice_state`, `ice_restart_count`, `selected_pair`, `relayed`,
`rtt_ms` + `has_rtt`), **DTLS** (`dtls_state`, `dtls_role`,
`dtls_fingerprint`) and **SRTP** (`srtp_installed`, the protected /
unprotected RTP + RTCP counters, `srtp_auth_failures`, `srtp_drops`).
`selected_pair` (`IceCandidatePairInfo`) names both ends' `address` /
`port` / `protocol` / candidate `type`.

**`VideoSyncStats`** — the render clock's decisions: `frames_presented`,
`frames_dropped_sync` (the video was ahead of the audio),
`frames_delayed` (it was behind), `total_delay_us` and
`last_skew_ms` (positive = video behind).

**`RemoteDesktopStats`** — the pipeline breakdown, documented in §13.6.
Its shape is three stage blocks — `capture` (`RemoteDesktopCaptureStats`),
`encode` (`RemoteDesktopEncodeStats`) and `decode`
(`RemoteDesktopDecodeStats`), each carrying `fps` / `ms` / `frames` (plus
`p95_ms` on the two codec stages) — plus the two cross-stage counters
`frames_dropped_before_encode` and
`frames_decoded_without_local_encode`, which stay at the top level
because they describe the gap *between* two stages.

#### The field-set table

The authoritative copy lives in a generated file —
[`stats_field_reference.md`](./stats_field_reference.md) — and the CI
gate compares *that* file with the header:

```
python tools/dump_stats_fields.py --markdown > docs/api/stats_field_reference.md
python tools/dump_stats_fields.py --check    docs/api/stats_field_reference.md
```

The same table is reproduced here so this document stands alone.  A
field whose meaning column is `--` has no per-field comment in the
header; its section note above it is the definition.

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `snapshot_time_us` | `std::int64_t` | The reading's timestamp, in microseconds on the engine's monotonic clock (the `tick()` timebase). |
| 2 | `snapshot_seq` | `std::uint64_t` | The reading's sequence number; increases by one per reading. |
| 3 | `srtp_drops` | `int` | The legacy SRTP drop counter: every SRTP auth-tag mismatch since open(). The v1.2 transport block (`Stats::transport`) carries the same figure alongside the rest of the SRTP plane; this one stays because hosts read it. |
| 4 | `video_frames_sent` | `std::uint64_t` | send_video() frames |
| 5 | `video_packets_sent` | `std::uint64_t` | RTP packets emitted |
| 6 | `video_bytes_sent` | `std::uint64_t` | RTP bytes (hdr+payload) |
| 7 | `video_frames_received` | `std::uint64_t` | receiver frames |
| 8 | `video_nack_seqs_queued` | `std::uint64_t` | NACK seqs queued (out) |
| 9 | `video_keyframes_q` | `std::uint64_t` | keyframe reqs queued |
| 10 | `video_feedback_applied` | `std::uint64_t` | inbound RTCP actions |
| 11 | `video_keyframes_applied` | `std::uint64_t` | inbound keyframe reqs |
| 12 | `video_feedback_rx_pli` | `std::uint64_t` | PLI (picture-loss indication) the peer sent us. |
| 13 | `video_feedback_rx_fir` | `std::uint64_t` | FIR (full intra request) the peer sent us. |
| 14 | `video_feedback_rx_nack` | `std::uint64_t` | NACK (retransmission request) the peer sent us. |
| 15 | `video_feedback_rx_remb` | `std::uint64_t` | REMB (receiver bandwidth estimate) the peer sent us. |
| 16 | `video_feedback_rx_transport_cc` | `std::uint64_t` | transport-cc (RFC 8888) reports the peer sent us. |
| 17 | `video_feedback_tx_pli` | `std::uint64_t` | PLI we sent the peer. |
| 18 | `video_feedback_tx_nack` | `std::uint64_t` | NACK we sent the peer. |
| 19 | `video_feedback_pli_requests` | `std::uint64_t` | Key-frame requests the receiver raised (the demand). |
| 20 | `video_feedback_pli_allowed` | `std::uint64_t` | Key-frame requests the rate limit let through. |
| 21 | `video_feedback_pli_suppressed` | `std::uint64_t` | Key-frame requests the rate limit dropped. |
| 22 | `video_feedback_pli_from_loss` | `std::uint64_t` | Key-frame requests raised by the loss door (a frame the jitter buffer could not assemble). |
| 23 | `video_feedback_pli_from_decode_failure` | `std::uint64_t` | Key-frame requests raised by the decode-failure door (the decoder refused a frame that assembled perfectly). |
| 24 | `video_feedback_nack_seqs_dropped` | `std::uint64_t` | NACK sequence numbers the per-report cap discarded. |
| 25 | `video_feedback_reports_expired` | `std::uint64_t` | Pending feedback reports dropped for being too old to send. |
| 26 | `video_keyframes_limiter_requests` | `std::uint64_t` | offered |
| 27 | `video_keyframes_granted` | `std::uint64_t` | granted |
| 28 | `video_keyframes_deferred` | `std::uint64_t` | held back |
| 29 | `video_keyframes_rejected` | `std::uint64_t` | budget spent |
| 30 | `video_keyframes_forced` | `std::uint64_t` | bypassed |
| 31 | `video_incremental_frames_seen` | `std::uint64_t` | -- |
| 32 | `video_incremental_unchanged` | `std::uint64_t` | -- |
| 33 | `video_incremental_encoded` | `std::uint64_t` | -- |
| 34 | `video_incremental_full` | `std::uint64_t` | -- |
| 35 | `video_incremental_fallbacks` | `std::uint64_t` | -- |
| 36 | `video_incremental_skipped` | `std::uint64_t` | -- |
| 37 | `video_region_refreshes_sent` | `std::uint64_t` | -- |
| 38 | `video_zero_copy_frames` | `std::uint64_t` | Frames that took the GPU path (no CPU staging). |
| 39 | `video_zero_copy_fallbacks` | `std::uint64_t` | Frames that carried a GPU handle but fell back to the CPU. |
| 40 | `video_rx_gpu_frames` | `std::uint64_t` | -- |
| 41 | `video_rx_cpu_frames` | `std::uint64_t` | -- |
| 42 | `video_zero_copy_rejected` | `std::uint64_t` | A frame that carried a GPU surface the encoder could not consume at all, and that had no CPU pixels to fall back to. The frame was dropped -- this counter makes that visible. |
| 43 | `video_capture_frames` | `std::uint64_t` | frames pulled from the device |
| 44 | `video_capture_frames_pumped` | `std::uint64_t` | frames handed to the encoder |
| 45 | `video_capture_dropped` | `std::uint64_t` | queue-overflow drops |
| 46 | `video_capture_device_errors` | `std::uint64_t` | device-lost / read errors |
| 47 | `video_capture_restarts` | `std::uint64_t` | times the device was reopened |
| 48 | `video_capture_format_changes` | `std::uint64_t` | negotiated format changed |
| 49 | `video_capture_bytes_copied` | `std::uint64_t` | CPU<->GPU bytes (0 = zero-copy) |
| 50 | `video_codec_backend_id` | `std::string` | e.g. "nvenc", "h264" |
| 51 | `video_codec_hardware` | `bool` | a hardware encoder |
| 52 | `video_codec_zero_copy` | `bool` | the last frame skipped the CPU |
| 53 | `video_codec_bitrate_bps` | `std::uint32_t` | the bitrate it was asked for |
| 54 | `video_codec_actual_bps` | `std::uint32_t` | the bitrate it produced |
| 55 | `video_codec_encode_latency_us` | `std::uint32_t` | average encode latency |
| 56 | `video_codec_frames_encoded` | `std::uint64_t` | -- |
| 57 | `video_codec_bytes_encoded` | `std::uint64_t` | -- |
| 58 | `video_codec_encode_errors` | `std::uint64_t` | -- |
| 59 | `video_codec_zero_copy_frames` | `std::uint64_t` | frames that skipped the CPU |
| 60 | `video_codec_staged_frames` | `std::uint64_t` | frames staged through the CPU |
| 61 | `video_freeze_count` | `std::uint64_t` | Number of freezes detected (a gap > `freeze_threshold_ms` between two rendered frames). |
| 62 | `video_freeze_total_ms` | `std::uint64_t` | Total frozen time in milliseconds (sum of the freeze gaps). |
| 63 | `video_last_freeze_ms` | `std::uint64_t` | Duration of the most recent freeze in milliseconds. |
| 64 | `video_keyframe_interval_avg_ms` | `double` | Running mean of the inter-keyframe distance, in milliseconds (EMA). Zero when fewer than two keyframes were seen. |
| 65 | `video_qp_sum` | `std::uint64_t` | Sum of the per-frame QP samples (0 = best quality). These are the *outbound* samples — the QP the encoder chose for the frames we sent. The average QP is `video_qp_sum / video_qp_count`. |
| 66 | `video_qp_count` | `std::uint64_t` | Number of per-frame QP samples (i.e. frames with a known QP). Zero means the codec does not report a QP — not that the quality was perfect. |
| 67 | `video_frames_dropped_jb` | `std::uint64_t` | Frames dropped at the jitter buffer (never assembled). |
| 68 | `video_frames_dropped_decoder` | `std::uint64_t` | Frames dropped by the decoder (corrupt / undecodable). |
| 69 | `video_frames_dropped_dependency` | `std::uint64_t` | Frames dropped because a reference frame was lost. This is a subset of `video_frames_dropped_decoder`. |
| 70 | `video_current_bitrate_bps` | `std::uint32_t` | The current inbound video bitrate, in bits per second (EMA over a one-second window). |
| 71 | `video_avg_bitrate_bps` | `std::uint32_t` | The session-average inbound video bitrate, in bits per second. |
| 72 | `video_bytes_received` | `std::uint64_t` | Bytes of encoded video received since open(). |
| 73 | `video_frames_decoded` | `std::uint64_t` | Frames successfully decoded by the codec. |
| 74 | `video_frames_rendered` | `std::uint64_t` | Frames handed to the video sink (rendered). |
| 75 | `video_tx_frames_encoded` | `std::uint64_t` | -- |
| 76 | `video_tx_bytes_encoded` | `std::uint64_t` | -- |
| 77 | `video_tx_frames_dropped_dependency` | `std::uint64_t` | Dependency losses the *peer* reported about our stream. A PLI / FIR the peer sends is the peer saying "a reference frame of yours never arrived, so I cannot decode what follows". This is our *transmit* side losing a reference frame — the counterpart of the inbound `video_frames_dropped_dependency`. It is counted here rather than as an inbound drop because the two describe opposite directions: the inbound counter says "we could not decode what the peer sent", this one says "the peer could not decode what we sent". |
| 78 | `audio_frames_sent` | `std::uint64_t` | RTP packets emitted |
| 79 | `audio_bytes_sent` | `std::uint64_t` | RTP bytes (hdr+payload) |
| 80 | `audio_encode_errors` | `std::uint64_t` | frames that failed to encode |
| 81 | `audio_samples_in` | `std::uint64_t` | PCM samples handed to send_audio() |
| 82 | `audio_samples_pending` | `std::uint64_t` | samples still in the accumulator |
| 83 | `audio_packets_sent` | `std::uint64_t` | RTP packets emitted |
| 84 | `audio_frames_received` | `std::uint64_t` | decoded frames delivered |
| 85 | `audio_samples_received` | `std::uint64_t` | samples per channel delivered |
| 86 | `audio_concealed_frames` | `std::uint64_t` | PLC frames delivered |
| 87 | `audio_fec_recovered_frames` | `std::uint64_t` | frames rebuilt from in-band FEC |
| 88 | `audio_packets_lost` | `std::uint64_t` | inbound packets inferred lost |
| 89 | `audio_decode_errors` | `std::uint64_t` | decoder errors |
| 90 | `audio_packets_dropped` | `std::uint64_t` | The packets the receive jitter buffer dropped to stay inside its memory bound (v1.3 plan Q2 / video plan R4). A non-zero value says "the receiver could not keep up with the sender"; it is not an error count. It is the buffer's own counter, republished unchanged. packets the jitter buffer dropped to stay bounded |
| 91 | `audio_jitter_ms` | `std::uint32_t` | current interarrival jitter |
| 92 | `audio_playout_delay_ms` | `std::uint32_t` | current playout delay |
| 93 | `audio_encoder_bitrate_bps` | `std::uint32_t` | -- |
| 94 | `net_feedback_blocks` | `std::uint64_t` | RTCP blocks routed to the collector |
| 95 | `bwe_feedback_samples` | `std::uint64_t` | samples handed to IBwe::on_feedback() |
| 96 | `bandwidth_estimate_bps` | `std::uint32_t` | current BWE target, in bit/s |
| 97 | `dtmf_packets_sent` | `std::uint64_t` | outbound event packets |
| 98 | `dtmf_packets_received` | `std::uint64_t` | inbound event packets decoded |
| 99 | `dtmf_packets_malformed` | `std::uint64_t` | inbound packets that failed to decode |
| 100 | `dtmf_events_delivered` | `std::uint64_t` | completed events reported to the host |
| 101 | `dtmf_payload_type` | `std::uint8_t` | the negotiated telephone-event PT |
| 102 | `xr_report_blocks` | `std::uint64_t` | XR blocks routed to the consumer |
| 103 | `video_capture_device_frames_captured` | `std::uint64_t` | -- |
| 104 | `video_capture_device_frames_dropped` | `std::uint64_t` | -- |
| 105 | `video_capture_device_error_count` | `std::uint64_t` | -- |
| 106 | `video_capture_device_restarts` | `std::uint64_t` | -- |
| 107 | `video_capture_device_format_changes` | `std::uint64_t` | -- |
| 108 | `video_capture_device_bytes_copied` | `std::uint64_t` | -- |
| 109 | `video_capture_device_frames_zero_copy` | `std::uint64_t` | -- |
| 110 | `video_capture_device_frames_cpu_staged` | `std::uint64_t` | -- |
| 111 | `video_capture_pumped_frames` | `std::uint64_t` | -- |
| 112 | `video_capture_pump_errors` | `std::uint64_t` | -- |
| 113 | `video_capture_pumped_metadata` | `std::uint64_t` | -- |
| 114 | `video_capture_gpu_frames` | `std::uint64_t` | -- |
| 115 | `video_capture_gpu_errors` | `std::uint64_t` | -- |
| 116 | `video_capture_gpu_wired` | `bool` | -- |
| 117 | `video_format_conversions` | `std::uint64_t` | -- |
| 118 | `video_format_copies` | `std::uint64_t` | The frames that were copied without a colour conversion. |
| 119 | `video_format_zero_copy` | `std::uint64_t` | The frames that reached the encoder as a native GPU surface (so no format work happened at all). |
| 120 | `video_format_canonical` | `std::string` | The canonical format the encoder is fed (e.g. "i420"). |
| 121 | `video_format_source` | `std::string` | The format the capture device most recently delivered (e.g. "bgra"). A host compares it with the canonical format to see whether a conversion is happening. |
| 122 | `video_codec_backend` | `std::string` | The codec backend's id ("nvenc", "openh264", "stub_h264", ...). "unknown" when the codec does not name its backend. |
| 123 | `video_codec_hardware_accelerated` | `bool` | True iff the codec runs on a hardware accelerator. |
| 124 | `video_codec_backend_preference` | `std::string` | The backend class the host asked for (plan B2). "auto" / "hardware" / "software" — the value of `EngineConfig::video_codec_backend`. Read this together with `video_codec_backend`: a host that asked for "hardware" and got "openh264_h264" has a problem the engine would otherwise hide, and this field makes the mismatch visible without parsing a log. |
| 125 | `video_codec_frames_zero_copy` | `std::uint64_t` | Frames the codec encoded through the GPU zero-copy path. |
| 126 | `video_codec_frames_staged` | `std::uint64_t` | Frames the codec encoded through the CPU staging path. |
| 127 | `video_codec_keyframes` | `std::uint64_t` | Keyframes (IDR) the codec produced. |
| 128 | `video_codec_bytes_copied` | `std::uint64_t` | Bytes the codec copied CPU↔GPU (0 on a pure hardware path). |
| 129 | `video_codec_encode_latency_avg_us` | `std::uint64_t` | The mean encode latency, in microseconds. |
| 130 | `video_codec_encode_latency_max_us` | `std::uint64_t` | The largest single encode duration, in microseconds. |
| 131 | `video_tx_bitrate_bps` | `std::uint32_t` | The current outbound video bitrate, in bits per second. Derived from the encoded byte count over the last second. |
| 132 | `video_sink_frames_rendered` | `std::uint64_t` | -- |
| 133 | `video_sink_frames_dropped` | `std::uint64_t` | -- |
| 134 | `video_sink_frames_dropped_present` | `std::uint64_t` | -- |
| 135 | `video_sink_frames_presented` | `std::uint64_t` | -- |
| 136 | `video_sink_render_errors` | `std::uint64_t` | -- |
| 137 | `video_sink_present_fps` | `std::uint32_t` | -- |
| 138 | `video_sync` | `VideoSyncStats` | -- |
| 139 | `video_streams` | `std::vector<VideoStreamStats>` | -- |
| 140 | `inbound_rtp_streams` | `std::vector<InboundRtpStats>` | -- |
| 141 | `audio_streams` | `std::vector<AudioStreamStats>` | -- |
| 142 | `transport` | `TransportStats` | The ICE / DTLS / SRTP path the session runs over. |
| 143 | `data_channels` | `std::vector<DataChannelStats>` | ---- The per-channel data-channel entries (v1.2 plan Track S / S6) One entry per **data channel the host opened** with `create_data_channel()`, each with its own identity (the engine-assigned id and the label) and its own counters. The list is empty when the host never opened a channel, so a host that does not use the data plane sees no change in its telemetry. It is a list rather than a single struct so that the *shape* survives the arrival of a second channel — the alternative is to redraw the whole surface then, and every caller with it. |
| 144 | `video_stage_capture` | `StageTiming` | The time spent popping the frame from the capture device. |
| 145 | `video_stage_convert` | `StageTiming` | The time spent converting the picture (colour / format / scale). |
| 146 | `video_stage_encode` | `StageTiming` | The time spent inside the codec's encode call. |
| 147 | `video_stage_packetize` | `StageTiming` | The time spent packetizing (RTP header, STAP-A / FU-A, SRTP). |
| 148 | `video_stage_decode` | `StageTiming` | The time spent inside the codec's decode call. |
| 149 | `video_stage_render` | `StageTiming` | The time spent in the sink's render call. |
| 150 | `video_stage_total_us` | `std::uint64_t` | The summed cost of the outbound path, in microseconds. This is the number §B.6's budget is written against: the capture → convert → encode → packetize chain. It is the sum of the four stage means above, so a host can compare it with the budget directly: ``` const auto s = engine.stats(); const auto frame_cost_us = s.video_stage_capture.mean_us + s.video_stage_convert.mean_us + s.video_stage_encode.mean_us + s.video_stage_packetize.mean_us; ``` The p95 variant (`video_stage_total_p95_us`) sums the p95s instead — an upper bound on the *typical* worst frame, which is the number to compare with the frame interval (33 333 µs at 30 fps). |
| 151 | `video_stage_total_p95_us` | `std::uint64_t` | The summed p95 of the four outbound stages, in microseconds. |
| 152 | `qoe` | `plugins::QoeSnapshot` | The quality-of-experience metrics (plan J3 / J6). The freeze count, the end-to-end latency percentiles, the viewer's frame rate, the sharpness proxy and the input-to-photon latency. The metrics are measured whenever a QoE collector is resolved -- which, by default, is the built-in `"rd-qoe"` (see `EngineConfig::qoe_collector_name`). A host that turns the metrics off says so with `EngineConfig::qoe_collector_enabled` (or with an empty name). Without a collector this snapshot carries the **frame counters** -- the presented and dropped frame counts, which the engine keeps anyway -- and zeroes for the metrics, which is the honest answer to "what was the latency percentile?" when it was not measured. |
| 153 | `remote_desktop` | `RemoteDesktopStats` | The remote-desktop pipeline breakdown (v1.2 plan **S7**). The QoE block above answers "how did the session *feel*"; this block answers "**where did the time go**". It is a remote-desktop-shaped view of the per-stage instrumentation (`video_stage_*`): the capture / encode / decode means in milliseconds, the frame rates they imply, and the count of frames that were captured but never encoded (the gap between two stages is a frame that was dropped in between). The shape follows the pipeline: `capture`, `encode` and `decode`, each with its own rate, cost and count — so a host compares the halves ("the capture ran at 30 fps and the encoder only at 24") instead of guessing which prefix carries which half. Every value here is **derived** from a measurement that already exists — nothing is timed a second time. The definitions are in `RemoteDesktopStats` above. |
| 154 | `video_capture_device_changes` | `std::uint64_t` | Device-list changes (v1-device-policy D4): each "removed" or "added" event counts once, cumulative since open(). Distinct from `video_capture_restarts` — the physical world changed this many times, the engine reopened the device that many times. |
| 155 | `audio_source_device_id` | `std::string` | The *negotiated* capture device id (what the source plugin reports after `open()`), or empty when no source device is in use (v1.4 / plan D1). |
| 156 | `audio_source_backend` | `std::string` | The resolved capture backend factory id, or empty when no source device is in use. |
| 157 | `audio_sink_device_id` | `std::string` | The *negotiated* playback device id, or empty when no sink device is in use. |
| 158 | `audio_sink_backend` | `std::string` | The resolved playback backend factory id, or empty when no sink device is in use. |
| 159 | `video_sink_backend` | `std::string` | The id of the sink that is actually installed (v1.4 / plan D2): the platform id when `"auto"` resolved, `"headless"` when it degraded (or was asked for explicitly), the explicit id, or empty when `"none"` / no sink. |
| 160 | `video_sink_backend_reason` | `std::string` | Why `"auto"` did not use the platform renderer (`no-platform-backend`, `backend-not-registered`, `backend-open-failed`), or **empty** when the session did not degrade. |
| 159 | `audio_device_pumped_frames` | `std::uint64_t` | Frames the audio capture pump pulled from the source device and ran through the outbound chain (v1.4 / plan D1). Moving is the proof that the automatic path works: no `send_audio()` call from the host. |
| 160 | `audio_device_pump_errors` | `std::uint64_t` | Audio pump failures: a non-`kOk`/`kErrNotReady` device read, or an outbound chain rejection. |
**160 fields.**
| 12 | `video_rtx_packets_sent` | `std::uint64_t` | Retransmission packets we put on the wire (RFC 4588). |
| 13 | `video_rtx_packets_received` | `std::uint64_t` | Retransmission packets we received and unwrapped (RFC 4588). |
| 14 | `video_feedback_rx_pli` | `std::uint64_t` | PLI (picture-loss indication) the peer sent us. |
| 15 | `video_feedback_rx_fir` | `std::uint64_t` | FIR (full intra request) the peer sent us. |
| 16 | `video_feedback_rx_nack` | `std::uint64_t` | NACK (retransmission request) the peer sent us. |
| 17 | `video_feedback_rx_remb` | `std::uint64_t` | REMB (receiver bandwidth estimate) the peer sent us. |
| 18 | `video_feedback_rx_transport_cc` | `std::uint64_t` | transport-cc (RFC 8888) reports the peer sent us. |
| 19 | `video_feedback_tx_pli` | `std::uint64_t` | PLI we sent the peer. |
| 20 | `video_feedback_tx_nack` | `std::uint64_t` | NACK we sent the peer. |
| 21 | `video_feedback_pli_requests` | `std::uint64_t` | Key-frame requests the receiver raised (the demand). |
| 22 | `video_feedback_pli_allowed` | `std::uint64_t` | Key-frame requests the rate limit let through. |
| 23 | `video_feedback_pli_suppressed` | `std::uint64_t` | Key-frame requests the rate limit dropped. |
| 24 | `video_feedback_pli_from_loss` | `std::uint64_t` | Key-frame requests raised by the loss door (a frame the jitter buffer could not assemble). |
| 25 | `video_feedback_pli_from_decode_failure` | `std::uint64_t` | Key-frame requests raised by the decode-failure door (the decoder refused a frame that assembled perfectly). |
| 26 | `video_feedback_nack_seqs_dropped` | `std::uint64_t` | NACK sequence numbers the per-report cap discarded. |
| 27 | `video_feedback_reports_expired` | `std::uint64_t` | Pending feedback reports dropped for being too old to send. |
| 28 | `video_keyframes_limiter_requests` | `std::uint64_t` | offered |
| 29 | `video_keyframes_granted` | `std::uint64_t` | granted |
| 30 | `video_keyframes_deferred` | `std::uint64_t` | held back |
| 31 | `video_keyframes_rejected` | `std::uint64_t` | budget spent |
| 32 | `video_keyframes_forced` | `std::uint64_t` | bypassed |
| 33 | `video_incremental_frames_seen` | `std::uint64_t` | -- |
| 34 | `video_incremental_unchanged` | `std::uint64_t` | -- |
| 35 | `video_incremental_encoded` | `std::uint64_t` | -- |
| 36 | `video_incremental_full` | `std::uint64_t` | -- |
| 37 | `video_incremental_fallbacks` | `std::uint64_t` | -- |
| 38 | `video_incremental_skipped` | `std::uint64_t` | -- |
| 39 | `video_region_refreshes_sent` | `std::uint64_t` | -- |
| 40 | `video_zero_copy_frames` | `std::uint64_t` | Frames that took the GPU path (no CPU staging). |
| 41 | `video_zero_copy_fallbacks` | `std::uint64_t` | Frames that carried a GPU handle but fell back to the CPU. |
| 42 | `video_rx_gpu_frames` | `std::uint64_t` | -- |
| 43 | `video_rx_cpu_frames` | `std::uint64_t` | -- |
| 44 | `video_zero_copy_rejected` | `std::uint64_t` | A frame that carried a GPU surface the encoder could not consume at all, and that had no CPU pixels to fall back to. The frame was dropped -- this counter makes that visible. |
| 45 | `video_capture_frames` | `std::uint64_t` | frames pulled from the device |
| 46 | `video_capture_frames_pumped` | `std::uint64_t` | frames handed to the encoder |
| 47 | `video_capture_dropped` | `std::uint64_t` | queue-overflow drops |
| 48 | `video_capture_device_errors` | `std::uint64_t` | device-lost / read errors |
| 49 | `video_capture_restarts` | `std::uint64_t` | times the device was reopened |
| 50 | `video_capture_format_changes` | `std::uint64_t` | negotiated format changed |
| 51 | `video_capture_bytes_copied` | `std::uint64_t` | CPU<->GPU bytes (0 = zero-copy) |
| 52 | `video_codec_backend_id` | `std::string` | e.g. "nvenc", "h264" |
| 53 | `video_codec_hardware` | `bool` | a hardware encoder |
| 54 | `video_codec_zero_copy` | `bool` | the last frame skipped the CPU |
| 55 | `video_codec_bitrate_bps` | `std::uint32_t` | the bitrate it was asked for |
| 56 | `video_codec_actual_bps` | `std::uint32_t` | the bitrate it produced |
| 57 | `video_codec_encode_latency_us` | `std::uint32_t` | average encode latency |
| 58 | `video_codec_frames_encoded` | `std::uint64_t` | -- |
| 59 | `video_codec_bytes_encoded` | `std::uint64_t` | -- |
| 60 | `video_codec_encode_errors` | `std::uint64_t` | -- |
| 61 | `video_codec_zero_copy_frames` | `std::uint64_t` | frames that skipped the CPU |
| 62 | `video_codec_staged_frames` | `std::uint64_t` | frames staged through the CPU |
| 63 | `video_freeze_count` | `std::uint64_t` | Number of freezes detected (a gap > `freeze_threshold_ms` between two rendered frames). |
| 64 | `video_freeze_total_ms` | `std::uint64_t` | Total frozen time in milliseconds (sum of the freeze gaps). |
| 65 | `video_last_freeze_ms` | `std::uint64_t` | Duration of the most recent freeze in milliseconds. |
| 66 | `video_keyframe_interval_avg_ms` | `double` | Running mean of the inter-keyframe distance, in milliseconds (EMA). Zero when fewer than two keyframes were seen. |
| 67 | `video_qp_sum` | `std::uint64_t` | Sum of the per-frame QP samples (0 = best quality). These are the *outbound* samples — the QP the encoder chose for the frames we sent. The average QP is `video_qp_sum / video_qp_count`. |
| 68 | `video_qp_count` | `std::uint64_t` | Number of per-frame QP samples (i.e. frames with a known QP). Zero means the codec does not report a QP — not that the quality was perfect. |
| 69 | `video_frames_dropped_jb` | `std::uint64_t` | Frames dropped at the jitter buffer (never assembled). |
| 70 | `video_frames_dropped_decoder` | `std::uint64_t` | Frames dropped by the decoder (corrupt / undecodable). |
| 71 | `video_frames_dropped_dependency` | `std::uint64_t` | Frames dropped because a reference frame was lost. This is a subset of `video_frames_dropped_decoder`. |
| 72 | `video_current_bitrate_bps` | `std::uint32_t` | The current inbound video bitrate, in bits per second (EMA over a one-second window). |
| 73 | `video_avg_bitrate_bps` | `std::uint32_t` | The session-average inbound video bitrate, in bits per second. |
| 74 | `video_bytes_received` | `std::uint64_t` | Bytes of encoded video received since open(). |
| 75 | `video_frames_decoded` | `std::uint64_t` | Frames successfully decoded by the codec. |
| 76 | `video_frames_rendered` | `std::uint64_t` | Frames handed to the video sink (rendered). |
| 77 | `video_tx_frames_encoded` | `std::uint64_t` | -- |
| 78 | `video_tx_bytes_encoded` | `std::uint64_t` | -- |
| 79 | `video_tx_frames_dropped_dependency` | `std::uint64_t` | Dependency losses the *peer* reported about our stream. A PLI / FIR the peer sends is the peer saying "a reference frame of yours never arrived, so I cannot decode what follows". This is our *transmit* side losing a reference frame — the counterpart of the inbound `video_frames_dropped_dependency`. It is counted here rather than as an inbound drop because the two describe opposite directions: the inbound counter says "we could not decode what the peer sent", this one says "the peer could not decode what we sent". |
| 80 | `audio_frames_sent` | `std::uint64_t` | RTP packets emitted |
| 81 | `audio_bytes_sent` | `std::uint64_t` | RTP bytes (hdr+payload) |
| 82 | `audio_encode_errors` | `std::uint64_t` | frames that failed to encode |
| 83 | `audio_samples_in` | `std::uint64_t` | PCM samples handed to send_audio() |
| 84 | `audio_samples_pending` | `std::uint64_t` | samples still in the accumulator |
| 85 | `audio_packets_sent` | `std::uint64_t` | RTP packets emitted |
| 86 | `audio_frames_received` | `std::uint64_t` | decoded frames delivered |
| 87 | `audio_samples_received` | `std::uint64_t` | samples per channel delivered |
| 88 | `audio_concealed_frames` | `std::uint64_t` | PLC frames delivered |
| 89 | `audio_fec_recovered_frames` | `std::uint64_t` | frames rebuilt from in-band FEC |
| 90 | `audio_packets_lost` | `std::uint64_t` | inbound packets inferred lost |
| 91 | `audio_decode_errors` | `std::uint64_t` | decoder errors |
| 92 | `audio_packets_dropped` | `std::uint64_t` | The packets the receive jitter buffer dropped to stay inside its memory bound (v1.3 plan Q2 / video plan R4). A non-zero value says "the receiver could not keep up with the sender"; it is not an error count. It is the buffer's own counter, republished unchanged. packets the jitter buffer dropped to stay bounded |
| 93 | `audio_jitter_ms` | `std::uint32_t` | current interarrival jitter |
| 94 | `audio_playout_delay_ms` | `std::uint32_t` | current playout delay |
| 95 | `audio_encoder_bitrate_bps` | `std::uint32_t` | -- |
| 96 | `ice_send_blocked_events` | `std::uint32_t` | How many times the engine has stopped sending because the transport died. The Q5 soak found the failure mode this counts: the ICE consent freshness expired, libjuice moved the agent to `failed`, and the engine kept offering media at full rate anyway — 26 627 refused sends in the remaining 21 minutes of a 24 h run. The value is a count of **outages**, not of packets: one outage that lasts an hour is one, and a link that flaps is several. The engine reports each one once, rather than once per packet. |
| 97 | `net_feedback_blocks` | `std::uint64_t` | RTCP blocks routed to the collector |
| 98 | `bwe_feedback_samples` | `std::uint64_t` | samples handed to IBwe::on_feedback() |
| 99 | `bandwidth_estimate_bps` | `std::uint32_t` | current BWE target, in bit/s |
| 100 | `dtmf_packets_sent` | `std::uint64_t` | outbound event packets |
| 101 | `dtmf_packets_received` | `std::uint64_t` | inbound event packets decoded |
| 102 | `dtmf_packets_malformed` | `std::uint64_t` | inbound packets that failed to decode |
| 103 | `dtmf_events_delivered` | `std::uint64_t` | completed events reported to the host |
| 104 | `dtmf_payload_type` | `std::uint8_t` | the negotiated telephone-event PT |
| 105 | `xr_report_blocks` | `std::uint64_t` | XR blocks routed to the consumer |
| 106 | `video_capture_device_frames_captured` | `std::uint64_t` | -- |
| 107 | `video_capture_device_frames_dropped` | `std::uint64_t` | -- |
| 108 | `video_capture_device_error_count` | `std::uint64_t` | -- |
| 109 | `video_capture_device_restarts` | `std::uint64_t` | -- |
| 110 | `video_capture_device_format_changes` | `std::uint64_t` | -- |
| 111 | `video_capture_device_bytes_copied` | `std::uint64_t` | -- |
| 112 | `video_capture_device_frames_zero_copy` | `std::uint64_t` | -- |
| 113 | `video_capture_device_frames_cpu_staged` | `std::uint64_t` | -- |
| 114 | `video_capture_pumped_frames` | `std::uint64_t` | -- |
| 115 | `video_capture_pump_errors` | `std::uint64_t` | -- |
| 116 | `video_capture_pumped_metadata` | `std::uint64_t` | -- |
| 117 | `video_capture_gpu_frames` | `std::uint64_t` | -- |
| 118 | `video_capture_gpu_errors` | `std::uint64_t` | -- |
| 119 | `video_capture_gpu_wired` | `bool` | -- |
| 120 | `video_format_conversions` | `std::uint64_t` | -- |
| 121 | `video_format_copies` | `std::uint64_t` | The frames that were copied without a colour conversion. |
| 122 | `video_format_zero_copy` | `std::uint64_t` | The frames that reached the encoder as a native GPU surface (so no format work happened at all). |
| 123 | `video_format_canonical` | `std::string` | The canonical format the encoder is fed (e.g. "i420"). |
| 124 | `video_format_source` | `std::string` | The format the capture device most recently delivered (e.g. "bgra"). A host compares it with the canonical format to see whether a conversion is happening. |
| 125 | `video_codec_backend` | `std::string` | The codec backend's id ("nvenc", "openh264", "stub_h264", ...). "unknown" when the codec does not name its backend. |
| 126 | `video_codec_hardware_accelerated` | `bool` | True iff the codec runs on a hardware accelerator. |
| 127 | `video_codec_backend_preference` | `std::string` | The backend class the host asked for (plan B2). "auto" / "hardware" / "software" — the value of `EngineConfig::video_codec_backend`. Read this together with `video_codec_backend`: a host that asked for "hardware" and got "openh264_h264" has a problem the engine would otherwise hide, and this field makes the mismatch visible without parsing a log. |
| 128 | `video_codec_frames_zero_copy` | `std::uint64_t` | Frames the codec encoded through the GPU zero-copy path. |
| 129 | `video_codec_frames_staged` | `std::uint64_t` | Frames the codec encoded through the CPU staging path. |
| 130 | `video_codec_keyframes` | `std::uint64_t` | Keyframes (IDR) the codec produced. |
| 131 | `video_codec_bytes_copied` | `std::uint64_t` | Bytes the codec copied CPU↔GPU (0 on a pure hardware path). |
| 132 | `video_codec_encode_latency_avg_us` | `std::uint64_t` | The mean encode latency, in microseconds. |
| 133 | `video_codec_encode_latency_max_us` | `std::uint64_t` | The largest single encode duration, in microseconds. |
| 134 | `video_tx_bitrate_bps` | `std::uint32_t` | The current outbound video bitrate, in bits per second. Derived from the encoded byte count over the last second. |
| 135 | `video_sink_frames_rendered` | `std::uint64_t` | -- |
| 136 | `video_sink_frames_dropped` | `std::uint64_t` | -- |
| 137 | `video_sink_frames_dropped_present` | `std::uint64_t` | -- |
| 138 | `video_sink_frames_presented` | `std::uint64_t` | -- |
| 139 | `video_sink_render_errors` | `std::uint64_t` | -- |
| 140 | `video_sink_present_fps` | `std::uint32_t` | -- |
| 141 | `video_sync` | `VideoSyncStats` | -- |
| 142 | `video_streams` | `std::vector<VideoStreamStats>` | -- |
| 143 | `inbound_rtp_streams` | `std::vector<InboundRtpStats>` | -- |
| 144 | `audio_streams` | `std::vector<AudioStreamStats>` | -- |
| 145 | `transport` | `TransportStats` | The ICE / DTLS / SRTP path the session runs over. |
| 146 | `data_channels` | `std::vector<DataChannelStats>` | ---- The per-channel data-channel entries (v1.2 plan Track S / S6) One entry per **data channel the host opened** with `create_data_channel()`, each with its own identity (the engine-assigned id and the label) and its own counters. The list is empty when the host never opened a channel, so a host that does not use the data plane sees no change in its telemetry. It is a list rather than a single struct so that the *shape* survives the arrival of a second channel — the alternative is to redraw the whole surface then, and every caller with it. |
| 147 | `video_stage_capture` | `StageTiming` | The time spent popping the frame from the capture device. |
| 148 | `video_stage_convert` | `StageTiming` | The time spent converting the picture (colour / format / scale). |
| 149 | `video_stage_encode` | `StageTiming` | The time spent inside the codec's encode call. |
| 150 | `video_stage_packetize` | `StageTiming` | The time spent packetizing (RTP header, STAP-A / FU-A, SRTP). |
| 151 | `video_stage_decode` | `StageTiming` | The time spent inside the codec's decode call. |
| 152 | `video_stage_render` | `StageTiming` | The time spent in the sink's render call. |
| 153 | `video_stage_total_us` | `std::uint64_t` | The summed cost of the outbound path, in microseconds. This is the number §B.6's budget is written against: the capture → convert → encode → packetize chain. It is the sum of the four stage means above, so a host can compare it with the budget directly: ``` const auto s = engine.stats(); const auto frame_cost_us = s.video_stage_capture.mean_us + s.video_stage_convert.mean_us + s.video_stage_encode.mean_us + s.video_stage_packetize.mean_us; ``` The p95 variant (`video_stage_total_p95_us`) sums the p95s instead — an upper bound on the *typical* worst frame, which is the number to compare with the frame interval (33 333 µs at 30 fps). |
**156 fields.**
| 155 | `qoe` | `plugins::QoeSnapshot` | The quality-of-experience metrics (plan J3 / J6). The freeze count, the end-to-end latency percentiles, the viewer's frame rate, the sharpness proxy and the input-to-photon latency. The metrics are measured whenever a QoE collector is resolved -- which, by default, is the built-in `"rd-qoe"` (see `EngineConfig::qoe_collector_name`). A host that turns the metrics off says so with `EngineConfig::qoe_collector_enabled` (or with an empty name). Without a collector this snapshot carries the **frame counters** -- the presented and dropped frame counts, which the engine keeps anyway -- and zeroes for the metrics, which is the honest answer to "what was the latency percentile?" when it was not measured. |
| 156 | `remote_desktop` | `RemoteDesktopStats` | The remote-desktop pipeline breakdown (v1.2 plan **S7**). The QoE block above answers "how did the session *feel*"; this block answers "**where did the time go**". It is a remote-desktop-shaped view of the per-stage instrumentation (`video_stage_*`): the capture / encode / decode means in milliseconds, the frame rates they imply, and the count of frames that were captured but never encoded (the gap between two stages is a frame that was dropped in between). The shape follows the pipeline: `capture`, `encode` and `decode`, each with its own rate, cost and count — so a host compares the halves ("the capture ran at 30 fps and the encoder only at 24") instead of guessing which prefix carries which half. Every value here is **derived** from a measurement that already exists — nothing is timed a second time. The definitions are in `RemoteDesktopStats` above. |

---
## 14. Lifecycle diagram

```
┌─────────────────────────────┐
│ EngineConfig cfg;           │
│ NimRTCEngine engine(cfg);   │  ctor — no I/O yet
└─────────────────────────────┘
                │
                ▼
        set_on_audio_frame(...)
        set_on_error(...)
        set_on_state_change(...)
        set_on_call_state(...)          -- v1.4 D7: the call's lifecycle
                │
                ▼
   ┌─── pre_open() ───┐    (optional)
   │ set_remote_ice() │
   └────────┬─────────┘
            ▼
        open() ──────────────────────────────────────┐
            │                                        │
            ▼                                        │
   while (tick() && !connected) {                    │
       tick();                                       │
       std::this_thread::sleep_for(10ms);            │
   }                                                 │
            │                                        │
            ▼                                        │
   create_offer() ──► answer                          │
   process_remote_sdp(answer)                        │
            │                                        │
            ▼                                        │
   send_audio(pcm);                                  │
            │                                        │
            ▼                                        │
   close()    ◄──── (always, even on error)          │
                                                    err
```

---

## 15. Threading model

- The engine serialises all of its work on the **caller's thread**
  (the thread that called `open()` and drives `tick()`).
- Plugins called from `tick()` may dispatch to worker threads only
  if their interface documentation explicitly allows it.
- The audio / video frame callbacks fire on the engine thread.
- The DataChannel callbacks fire on the engine thread.

> **Implication**: if you bind the same `IDataChannel::set_on_message`
> callback from two engines, you'll get calls from two threads.
> Protect with a mutex.

---

## 16. Migration guide — the audio pipeline (v1.2.0)

The audio pipeline's public SDK is **additive**.  An existing host
keeps compiling and keeps working; this section says what it *gains*
and what it *should change*.

### 16.1 Nothing must change

A host that only uses `set_on_audio_frame()` and `send_audio()`
continues to work unchanged.

**One behavioural change is worth calling out.**  The legacy callback
now receives genuinely **decoded** PCM.  Before v1.2.0 the receive
path handed the host the compressed RTP payload reinterpreted as
floating-point samples, so a host that appeared to "work" against the
old build was in fact processing garbage.  After the change it
processes real audio.  This is a **fix, not a regression** — but the
bytes on the wire changed, and a host that had grown a workaround
around the noise may need to drop it.

### 16.2 Recommended migration

Install the new callback, and keep the legacy one only if a legacy
consumer still needs it:

```cpp
NimRTCEngine engine(cfg);

engine.set_on_audio_frame([](const float* pcm, size_t n) {
    // Legacy: n = total interleaved floats (samples_per_channel * channels).
    legacy_sink(pcm, n);
});

engine.set_on_audio_frame_info([](const nimrtc::engine::AudioFrameInfo& f) {
    // f.pcm is valid only for the duration of this call.
    // f.sample_rate_hz / f.channels tell you what you actually got.
    // f.concealed / f.fec_recovered let you distinguish synthetic audio.
    modern_sink(f.pcm, f.num_samples_per_channel * f.channels,
                f.sample_rate_hz, f.channels, f.rtp_timestamp);
});
```

### 16.3 Buffer lifetime

`f.pcm` points into a decoder-owned buffer that is reused for the next
frame.  A host that needs to keep the audio must copy it.  This
matches the existing contract of the legacy callback and is stated
explicitly because it is the most common integration mistake.

### 16.4 Tick rate

The host must call `tick()` at **≥ 100 Hz** for audio playout to be
smooth.  A host that calls it at 30 Hz still gets continuous audio,
with coarser granularity — the playout follows the media time, not the
arrival, so a slower tick delays the output instead of dropping it.

### 16.5 Reading the negotiated format

See §8.3 — `remote_audio_format()`.

### 16.6 Backwards-compatibility rules

- The legacy callback fires **before** the new one, with the same
  buffer.
- Installing the new callback does **not** uninstall the legacy one,
  and vice versa.  Both may be absent.
- When neither is installed, the receiver still runs — the decoding
  happens regardless, because the statistics and the 3A render
  reference depend on it.  The frames are simply dropped.

---

## 17. 一个 ≤200 行的宿主长什么样（what a ≤200-line host looks like）

`examples/sdk-sample/sdk-sample.cpp`（153 非注释行）+ `sdk_signalling.hpp`（38 行）
是两个进程、同一台机器、跑完一通话的完整宿主。它是「应用只贡献信令与产品 UI」
的最小证明 —— 设备、编解码、BWE、RTX、恢复、渲染、AEC 全在引擎里。

宿主写的（就是那 ≤200 行）：

- **信令客户端**：newline-delimited JSON（offer / answer / candidate），复用
  `examples/remote-desktop/signalling.py` 的协议方言；
- **`EngineConfig`**：`connectivity_profile = "auto"`（D6）、
  `ice_recovery_policy = "auto"`（N3）、`control_channel_name`、`turn_credentials`
  提供者（N7）、`video_source_name = "memory"`、`video_sink_name = "headless"`、
  `video_codec_name = "h264"`；
- **四个钩子**：`set_on_call_state`（D7）、`set_on_control_message`、
  `set_on_ice_recovery`（N8）、`EngineConfig::turn_credentials`（N7）；
- **收尾**：`pre_open()` / `open()`、`create_offer()` / `process_remote_sdp()`、
  `get_ice_transport()` 的候选交换、`send_audio()` / `send_video()` /
  `send_control_message()`、`tick()`。

宿主**不写**的（也正是行数能压下来的原因）：

- 不写设备 id，不采集、不渲染 —— `memory` 源 + `headless` sink，CI 上无设备时
  采集/渲染只 SKIPPED 永不 PASS；
- 不实现连通性策略（D6 profile）与重连策略（N3 引擎侧）—— 归引擎；
- 不直接 include `nimrtc/ice/` 的任何具体头（L4 接缝纪律）—— 只通过引擎公开面。

行数预算是**每次 CI 重新量**的事实（预算 200，10% 漂移，上限 220；
`tools/check_sample_line_budget.py`），详见 `docs/adr/ADR-036-sdk-sample-line-budget.md`。

---

## 18. 安装树里的视频依赖（what the install tree provides for video, v1.4 / ADR-038）

安装出来的 SDK bundle（`find_package(NimRTC REQUIRED CONFIG)` + `nimrtc::engine`）
现在提供**完整的视频链**，一个 h264 + headless sink 的宿主可以直接链通
（0 个 LNK2019；ADR-038）。bundle 在 `<prefix>/lib/` 里装了什么：

- **`nimrtc_nvcodec`** —— 共享的 NVIDIA / VideoToolbox 编解码适配器 archive，
  公开头（`<nimrtc/nvcodec/nv_backends.hpp>`、`nv_codec_kind.hpp`）随 bundle
  安装。`nimrtc_h264` 用 PUBLIC 链它，所以平铺链上它排在 `h264` 之后。
- **三个 OpenH264 archive** —— `nimrtc_vendor_openh264_encoder.lib` /
  `_decoder.lib` / `_common.lib`，顺序由 `<prefix>/lib/openh264_link_order.txt`
  清单写明（encoder / decoder / common），config 按清单读，不靠
  `find_library()` 的返回顺序。
- **Windows 系统库 `d3d11` / `dxgi`** —— D3D11 视频 sink 与 NVENC 路径需要，
  随 `ws2_32 bcrypt iphlpapi winmm crypt32` 一起补在链尾。

宿主怎么用：`EngineConfig` 里 `video_source_name = "memory"` +
`video_sink_name = "headless"` + `video_codec_name = "h264"`（§17），或者
直接注册插件 / 探针：

```cpp
#include <nimrtc/h264/codec_plugin.hpp>
#include <nimrtc/h264/hw_backends.hpp>
#include <nimrtc/nvcodec/nv_backends.hpp>
#include <nimrtc/video_sink/video_sink.hpp>

nimrtc::h264::register_default_plugins();
const bool nvenc_ok =
    nimrtc::nvcodec::nvenc_codec_available(nimrtc::nvcodec::NvCodec::kH264);
auto sink = nimrtc::video_sink::create_headless_sink(sink_cfg);
```

NVIDIA 的驱动 import lib（`nvidia-encode` / `nvidia-video`）**不需要**：
NVENC / NVDEC 用 `LoadLibrary` + `GetProcAddress` 动态加载驱动，没有链接期
引用（ADR-038 决策四）。

---

## 19. 安装树的模块闭包（what the install tree's link closure is, v1.4 / ADR-041）

「链接闭包」= 下游宿主链 `nimrtc::engine` 时，**必须**出现在同一条链接行上的
全部 `nimrtc_*` 静态 archive。它由三个来源的不动点算出：引擎自己的 PRIVATE
`nimrtc::X` 链接、每个模块自己的 `nimrtc::X` 链接、以及递归展开到不动点
（排除 `core` / `plugins` / `vendor` —— 那三个不是 module archive）。

装出来的 bundle 里，`lib/` 装了什么由 `cmake/NimRTCInstall.cmake` 的
`_nimrtc_install_shorts` 决定，config 链了什么由
`cmake/NimRTCConfig.cmake.in` 的 `_nimrtc_module_shorts` 决定 —— 两份清单
必须**都**覆盖闭包，缺一个下游宿主就撞 LNK2019 / LNK2001。本仓库树上的
闭包是 42 个模块。

```text
tools/check_install_tree_module_closure.py   # 只读仓库树即可判定，不需要构建
```

它读仓库树、算闭包、比对两份清单，缺口**点名模块 + 点名清单**。同一棵树跑
两次输出一致（判定确定，不触网、不调模型、不依赖随机与时间）。

闭包里有两个容易漏的成员，它们不是「多一个 archive」那么简单：

- **`pacing`** —— `nimrtc_sched` PRIVATE 链 `nimrtc::pacing`，而
  `pacing::TokenBucket` 的方法（`set_config` / `available` / `try_consume` /
  `refund` / `reset`）是**宿主自己**会调的。所以它必须在链上，且在 `sched`
  **之后**（GNU 静态链接器从左往右扫）。
- **闭包模块自己的 PRIVATE 系统库** —— 装好的 archive 保留了目标代码，但
  **丢掉了链接属性**。所以 config 在 Windows 上除了 `ws2_32 bcrypt iphlpapi
  winmm crypt32 d3d11 dxgi`，还要补 Media Foundation 那一组
  （`windowsapp mf mfplat mfreadwrite mfuuid`，`video_capture` 需要）与
  `ole32`（`audio_device` 的 WASAPI 需要）—— 少了它们，下游宿主链上
  `video_capture` 就是 12 个 LNK2019（`MFStartup` / `MFCreateMediaType` /
  `MFVideoFormat_NV12` …）。

  运行时还有一件：安装树把 MsQuic 装在 `<prefix>/bin/msquic.dll`（它是动态
  库，`lib/msquic.lib` 只是导入桩）。宿主跑起来时那个 `bin/` 得在 `PATH`
  上，否则进程以 `0xC0000135`（`STATUS_DLL_NOT_FOUND`）起不来 —— 那是
  「找不到 DLL」，不是「链接失败」。

---

## 20. See also

- `engine_config.md` — full field reference for `EngineConfig`.
- `plugin_author_guide.md` — how to write your own plugin (DTLS,
  audio3A, video, BWE).
- `examples/loopback-p2p/loopback-p2p.cpp` — full minimal program.
- `examples/demo-agent-gateway/` — Profile + PCM tap demo.
- `src/engine/include/nimrtc/engine/engine.hpp` — header.
- ADR-001 (plugin seam), ADR-009 (PAL Slice 1).

> API is **frozen at the source-level for v0.12.0**. Additions come
> in `[[deprecated]]`-marked overloads. Source-breaking changes go
> through a deprecation cycle (see
> `docs/zh/architecture.md` §12).
