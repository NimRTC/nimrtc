# P1 UAF Investigation — `Impl::dtls` Lifetime in `NimRTCEngine`

**Status:** Investigation complete; **mitigations landed (v0.12.0 P0 + v0.13.0 weak_ptr)**.
**Author:** P1 subagent (UAF sweep, 2026-10-14)
**Scope:** `Impl::dtls` and `Impl::datachannel_transport_cookie` lifetime in `D:\MyOpen\NimRTC\src\engine\src\engine.cpp` and friends.
**Out of scope:** v0.12.0 release notes, registry.hpp, raw_udp_datagram.hpp, CHANGELOG, ADR-009, clean_build.ps1, build/* (all M/?? in git status — owned by other agents).

---

## 0. Resolution Status (post-investigation)

Both mitigations are **landed** as of 2026-10-14:

| # | Mitigation | Status | Landed in |
|---|---|---|---|
| 1 | v0.12.0 P0: `is_open()` / `state_ == kOpen` guards at API entry points (`on_transport_recv`, `process_remote_sdp`, `drain_dtls`, `maybe_install_srtp_keys`) | ✅ Done | v0.12.0 (CHANGELOG `### Fixed`) |
| 2 | v0.13.0 P1: `Impl::dtls` + `Impl::datachannel_transport_cookie` raw → `std::weak_ptr<IDtlsSession>`; `ITransportStack::dtls_weak()` interface added; 3 stack implementations migrated | ✅ Done | v0.13.0 (CHANGELOG `### Changed`) |
| 3 | Regression test `v013_dtls_weak_expires_after_close` guards the close+weak_ptr invariant | ✅ Added | `tests/test_engine_plugin_loading.cpp` |
| 4 | The "Critical" finding #4 (on_transport_recv no state guard) is closed by both #1 (state guard at line 2319) and #2 (weak_ptr.lock() at line 2335) | ✅ Defence-in-depth | — |
| 5 | The "Medium" finding #5 (`datachannel_transport_cookie` raw pointer) is closed by #2 (weak_ptr migration) | ✅ Done | — |

The UAF class is **eliminated**, not just patched. v0.12.0 P0 is the
short-circuit; v0.13.0 weak_ptr is the type-system enforcement. The
investigation's §6 v1.x follow-ups (DC-1 / DC-2 / TPAL-5 data-channel
wiring, AddressSanitizer CI) are **still pending** — they are
v1.x WebTransport/QUIC redesign work, not v0.12.0 or v0.13.0 scope.

---

## 1. TL;DR

`Impl::dtls` is a **raw non-owning pointer** into `impl_->session.media_stack` (`WebRtcClassicStack::dtls_`), set in `init_transport_session()` (engine.cpp:678) and **cleared to `nullptr` in `close()`** (engine.cpp:1363) **before** the session is torn down. The dominant residual risk is **not** the closed-engine path (it is well-guarded); it is the **mid-flight state transition into `Failed` or `Closed` after a DTLS alert** (engine.cpp:916-922 + 2294) where the engine flips `state_` to `kClosed` via `close()` but a `tick()` / `on_transport_recv()` / `send_audio()` call already in flight (or scheduled) can read `impl_->dtls` after the session has been torn down but before the local `nullptr` write lands. The highest-risk concrete access path is **`on_transport_recv()` invoked through the ICE recv callback** (engine.cpp:2294, 2321, 2353) which dereferences `impl_->dtls->state()` / `->feed_inbound()` **without a `state_ == kOpen` guard** — only an `impl_->dtls` nullness check (line 2292, 2349) — so any ICE recv callback fired after `close()` started but before the session was destroyed would dereference a dangling pointer.

**Recommended immediate mitigation (P0 for v0.12.0):** add a `state_ == kOpen` (or equivalent `is_open()`) short-circuit at the top of `on_transport_recv()`, `drain_dtls()`, and any other method that dereferences `impl_->dtls`, mirroring the existing guard in `send_audio()` (engine.cpp:2022) and `tick()` (engine.cpp:2187).

**Secondary mitigation (P1 for v0.13.0):** store `impl_->dtls` as `std::weak_ptr<IDtlsSession>` (the stack would then hold a `std::shared_ptr<IDtlsSession>`), making the dangling-after-teardown case a guaranteed `nullptr` rather than UB. This requires a Slice 7.6 follow-up PR because the stack currently owns `dtls_` via `unique_ptr` (webrtc_classic_stack.cpp:179).

---

## 2. Lifetime chain (ASCII diagram)

```
CONSTRUCTION (init_modules_once → init_transport_session)
─────────────────────────────────────────────────────────
  init_transport_session()                    [engine.cpp:613-712]
    │  impl_->session = TransportSession{ media_stack = WebRtcClassicStack, ... }
    │  ice_t_         = &impl_->session.media_stack->ice();   // line 677
    │  impl_->dtls    = &impl_->session.media_stack->dtls();  // line 678 ◄── RAW BORROW
    │  // (Rule 2 / raw-udp-arq: impl_->dtls may stay nullptr — line 681)
    ▼
  init_dtls_handshake()                       [engine.cpp:906-940]
    │  if (impl_->dtls) impl_->dtls->open();  // line 914 ◄── FIRST DEREF
    │  // (otherwise skip + warn — Rule 2)
    ▼
  open() → state_ = kOpen                     [engine.cpp:1022]

USE (state_ == kOpen, per-method guards below)
──────────────────────────────────────────────
  process_remote_sdp()   → impl_->dtls->set_role() / set_peer_fingerprint()
                           (lines 1818, 1834) [guard: `if (impl_->dtls)` line 1800]
  create_offer()         → impl_->dtls->local_fingerprint()
                           (lines 1505, 1563, 1661) [guard: `if (impl_->dtls)`]
  process_remote_sdp()   → impl_->dtls->local_fingerprint() in answer
                           (line 1904) [guard: `if (impl_->dtls)`]
  send_audio()           → impl_->dtls->is_connected()
                           (line 2023) [guard: `!impl_->dtls || ...` + is_open() line 2021]
  create_data_channel()  → assigns impl_->datachannel_transport_cookie = impl_->dtls
                           (line 1470) [guard: state_ == kOpen line 1415]
  on_transport_recv()    → impl_->dtls->state() / feed_inbound()
                           (lines 2294, 2321, 2353) [guard: ONLY `if (impl_->dtls)` — NO state_ check]
  tick() → drain_dtls()  → impl_->dtls->tick() / take_outbound() / state()
                           (lines 2394, 2396, 2406) [guard: drain_dtls() checks `if (!impl_->dtls)`,
                                                     tick() checks `is_open()` line 2187]
  maybe_install_srtp_keys() → impl_->dtls->state() / srtp_keying_material()
                           (lines 2447, 2456) [guard: `if (!impl_->dtls)` line 2439]
  local_dtls_fingerprint_sha256_base64() → impl_->dtls->local_fingerprint()
                           (line 2608 inside dtls_session_iface consumer) [guard: `if (impl_)` line 2601]
  dtls_state() / dtls_connected() → impl_->dtls->state() / is_connected()
                           (lines 2618, 2622) [guard: `impl_->dtls ? ...` ternary]

DESTRUCTION (close() or ~NimRTCEngine → close() → delete impl_)
────────────────────────────────────────────────────────────────
  close()                                     [engine.cpp:1340-1376]
    │  if (state_ != State::kOpen) return;    // line 1341 — early-out
    │  shutdown_video_plugins()                // line 1345
    │  shutdown_bwe_scheduler()                 // line 1346
    │  impl_->session.close();                 // line 1361 ◄── SESSION TEARDOWN
    │  │   // WebRtcClassicStack::close() releases dtls_ via dtls_.reset()
    │  │   // (webrtc_classic_stack.cpp:108-111)
    │  │   // AFTER this line, impl_->dtls is DANGLING.
    │  ice_t_         = nullptr;                // line 1362
    │  impl_->dtls    = nullptr;                // line 1363 ◄── NULL-OUT (too late for in-flight calls)
    │  impl_->datachannel_transport_cookie = nullptr;  // line 1373
    │  state_         = State::kClosed;         // line 1374
    ▼
  ~NimRTCEngine()                             [engine.cpp:354-360]
    │  if (state_ == State::kOpen) close();    // line 355
    │  delete impl_;                            // line 358
    │  // Impl dtor runs TransportSession dtor → media_stack dtor → IDtlsSession dtor
    │  // (idempotent: WebRtcClassicStack::close() already reset dtls_)
```

**Key observation:** The `state_` flip and the `impl_->dtls = nullptr` write happen **sequentially inside `close()`** (lines 1362-1374). Any callback that fires **between** `impl_->session.close()` (line 1361) and `impl_->dtls = nullptr` (line 1363) — a window of a few instructions — sees a non-null but dangling `impl_->dtls`. In practice this window is single-threaded (close() runs on the engine thread; ICE callbacks are also invoked on the engine thread per the stack design — see webrtc_classic_stack.cpp:67 "all public ITransportStack methods are called from the engine thread"), so the window is closed in practice. The risk is when `close()` is called **from a different thread** than a `tick()` in flight, or when `~NimRTCEngine` races with a still-running `tick()` on a worker thread.

---

## 3. Risk register

| # | Access path | Code location | State guard? | `impl_->dtls` null guard? | Risk | Notes | Recommended mitigation |
|---|---|---|---|---|---|---|---|
| 1 | `process_remote_sdp()` after `close()` | `engine.cpp:1800-1834` | ❌ none | ✅ `if (impl_->dtls)` line 1800 | **High** | Dereferences `impl_->dtls->set_role()` / `set_peer_fingerprint()`. The function does **not** check `is_open()` / `state_ == kOpen` at the top. After `close()` the `impl_->dtls` write to `nullptr` (line 1363) saves us — but **only** if `close()` has actually run. If a caller invokes `process_remote_sdp()` on a different thread while `close()` is mid-flight on the engine thread, or if `process_remote_sdp()` is called after `close()` but before `~NimRTCEngine`, the null guard is hit. The bigger risk is the brief window during teardown. | Add `if (state_ != State::kOpen) return std::nullopt;` at function entry (line ~1700). |
| 2 | `send_audio()` after `close()` | `engine.cpp:2015-2112` | ✅ `is_open()` line 2021 | ✅ `!impl_->dtls || !impl_->dtls->is_connected()` line 2023 | **Low** | This is the **canonical good pattern** in the codebase. Both checks fire. | None — use as the model for the other access paths. |
| 3 | `tick()` after `close()` | `engine.cpp:2186-2225` | ✅ `is_open()` line 2187 | ✅ `if (ice_t_)` line 2211 (scheduler drain), `if (impl_->dtls)` inside `drain_dtls()` line 2392 | **Low** | The early-out is correct. `drain_dtls()` and the scheduler drain both null-check their targets. | None. |
| 4 | `on_transport_recv()` after `close()` | `engine.cpp:2279-2357` | ❌ **NO** state guard | ✅ `if (impl_->dtls)` line 2292, 2349 | **Critical** | The ICE recv callback fires synchronously from `ice_t_->recv()` (engine.cpp:2224), which is itself called from `tick()`. Since `tick()` has an `is_open()` early-out, this is safe **if** the engine thread is the only caller. However: (a) the comment in the code (engine.cpp:2222) says `drain_dtls()` is called inside `tick()` only when `is_ice_connected()` is true, but the actual `drain_dtls` call inside `on_transport_recv` at lines 2323-2326 has no `is_open()` check, and (b) if a future PR moves `on_transport_recv` to a worker thread (the seam supports it — see webrtc_classic_stack.cpp:67 comment), the `state_` check is mandatory. **Today the dominant concrete UAF risk.** | Add `if (!is_open()) return;` at the top of `on_transport_recv()` (line ~2281). Also add it before each `drain_dtls()` call (lines 2323, 2355). |
| 5 | `create_data_channel()` accessing `Impl::dtls` and `Impl::datachannel_transport_cookie` | `engine.cpp:1411-1471` | ✅ `state_ != State::kOpen` returns nullptr (line 1415) | ✅ `state_ != State::kOpen` covers the null-guard case | **Medium** | The state guard is correct. The `impl_->datachannel_transport_cookie = impl_->dtls;` write (line 1470) is safe because we just verified `state_ == kOpen` and `impl_->dtls` is non-null in the same scope. **However:** if a future change moves `create_data_channel` to be callable before `open()` (the comment on line 1456 "Must be called after open() succeeds" suggests it shouldn't be), the `impl_->dtls` read at line 1470 could see a null. | Document the precondition in the public header more strongly. Consider making the function `[nodiscard]` + `noexcept` already; add a `[[deprecated("call after open()")]]` overload that gates. Low priority. |
| 6 | `~NimRTCEngine` destructor ordering | `engine.cpp:354-360` | ✅ `if (state_ == State::kOpen) close();` (line 355) | ✅ `close()` nulls `impl_->dtls` before `delete impl_` (line 1363) | **Low** | The order is: `close()` → `delete impl_`. `close()` is idempotent (`if (state_ != State::kOpen) return;` line 1341). The `delete impl_` at line 358 then runs `~TransportSession` → `~WebRtcClassicStack` → `~IDtlsSession`. **Risk:** if a caller invokes the destructor from one thread while another thread holds a non-owning reference (e.g. a callback `std::function` captured by value), the callback can still see `impl_->dtls` non-null. The std::function captures are by value (`[this](...)` lambdas at engine.cpp:959, 961, 1519, 1772, 2091, 2209, 2212, 2291) — they hold `this` (the engine), not `impl_`, so they can call `engine->tick()` and dereference `impl_->dtls` after `~NimRTCEngine` ran only if the lambda is still in flight on another thread. | Add a thread-affinity assertion to the public docs. The `[this]` captures are a known hazard; consider storing a `std::shared_ptr<NimRTCEngine>` in the callback registration API for v1.x. |
| 7 | Member destructors of plugins that may hold raw `dtls` references | `webrtc_classic_stack.cpp:107-115` (stack dtor) | n/a | n/a | **Low** | The stack's `dtls_` is `unique_ptr<IDtlsSession>` (webrtc_classic_stack.cpp:179). `WebRtcClassicStack::close()` calls `dtls_.reset()` (line 110) before `ice_.reset()` (line 111). The order is **SCTP → RTP → DTLS → ICE** — correct for the DTLS-over-ICE dependency. **Risk:** if any other plugin (e.g. a BWE or scheduler plugin) holds a raw `IDtlsSession*` reference, it would be invalidated. Grep of the repo shows no other holders; the field is read through `ice_t_->set_bwe(bwe_.get())` etc. but the BWE/Scheduler plugin headers don't touch `IDtlsSession`. | None. Document in the engine.hpp comment block on `ice_t_` that **only** the engine and the WebRtcClassicStack may hold DTLS pointers; any new plugin must reach DTLS through the stack accessor, never via cached `IDtlsSession*`. |
| 8 | `Impl::datachannel_transport_cookie` lifetime after `close()` | `engine.cpp:328` (decl), `engine.cpp:1373` (close null-out), `engine.cpp:1470` (assign) | ❌ no state guard at the field itself | n/a (pointer is a separate field) | **Medium** | The cookie is assigned `impl_->dtls` in `create_data_channel()` (line 1470) and nulled in `close()` (line 1373). Between `close()` setting it to `nullptr` and the next `create_data_channel()` call, no code reads the cookie. **However:** the v1.x wiring PR (per engine.cpp:301-328 + v0.11-plan.md §2.2) will read this cookie to wire SCTP-over-DTLS; that PR must include a `state_ == kOpen` guard at every cookie read site. The current skeleton is a v1.x **forward-declaration trap**. | Document the contract in the field comment: "v1.x wiring code must read this under `state_ == kOpen` AND `is_open()` AND `impl_->dtls != nullptr` — never cache across close()." |
| 9 | `local_dtls_fingerprint_sha256_base64()` after `close()` | `engine.cpp:2599-2616` (dtls_session_iface consumer) | ❌ no state guard | ✅ `if (!d) return {};` line 2604 | **Low** | Reads `impl_->dtls` (via the local `d` pointer) only for `local_fingerprint()`. The null-guard saves it. But the function is `const` and is callable from any thread; the engine has no thread-affinity guarantee for `const` accessors. | Add `if (!is_open()) return {};` to match the rest. |

---

## 4. State machine analysis

`state_` lives in the public header (engine.hpp:489) as `enum class State { kConstructed, kOpen, kClosed }`. All transitions:

| From | To | Trigger | Code |
|---|---|---|---|
| `kConstructed` | `kOpen` | `open()` success | `engine.cpp:1022` |
| `kOpen` | `kClosed` | `close()` (idempotent) | `engine.cpp:1374` |
| `kOpen` | (self) | `close()` early-out if already not `kOpen` | `engine.cpp:1341` |
| `kConstructed` | (early-out) | `open()` if not `kConstructed` | `engine.cpp:968` |
| `kConstructed` | (early-out) | `pre_open()` if not `kConstructed` | `engine.cpp:440` |
| any | (dtor) | `~NimRTCEngine` calls `close()` if `kOpen` | `engine.cpp:355` |

**Invariant checked:** every `impl_->dtls->` dereference site.

| Site | State guard? | Null guard? | Verdict |
|---|---|---|---|
| `init_dtls_handshake` line 914 | n/a (called from `init_modules_once` before `state_ = kOpen`) | ✅ `if (impl_->dtls)` line 913 | ✅ Safe |
| `create_offer` line 1505 | ❌ | ✅ `if (impl_->dtls)` line 1503 | ⚠️ Acceptable: `create_offer` is documented to be called before `open()`; the null-guard is the right pattern there. |
| `create_offer` line 1563 | ❌ | ✅ `if (impl_->dtls)` line 1561 | ⚠️ Acceptable (same reasoning). |
| `create_offer` line 1661 | ❌ | ✅ `if (impl_->dtls)` line 1659 | ⚠️ Acceptable (same reasoning). |
| `process_remote_sdp` line 1818 | ❌ | ✅ `if (impl_->dtls)` line 1800 | ❌ **Gap** — see finding #1. |
| `process_remote_sdp` line 1834 | ❌ | ✅ `if (impl_->dtls)` line 1800 | ❌ **Gap** — see finding #1. |
| `process_remote_sdp` line 1904 | ❌ | ✅ `if (impl_->dtls)` line 1902 | ⚠️ Acceptable (this is in the answer-SDP path; the null-guard is the right pattern). |
| `send_audio` line 2023 | ✅ `is_open()` line 2021 | ✅ combined | ✅ **Reference pattern.** |
| `on_transport_recv` line 2294 | ❌ | ✅ `if (impl_->dtls)` line 2292 | ❌ **Critical gap** — see finding #4. |
| `on_transport_recv` line 2321 | ❌ | ✅ `if (impl_->dtls)` line 2292 | ❌ **Critical gap** — see finding #4. |
| `on_transport_recv` line 2353 | ❌ | ✅ `if (impl_->dtls)` line 2349 (CCS branch) | ❌ **Critical gap** — see finding #4. |
| `drain_dtls` line 2394 | ⚠️ implicit via `tick()` `is_open()` line 2187 | ✅ `if (!impl_->dtls) return 0;` line 2392 | ⚠️ Acceptable: `tick()` gates the entry. The implicit guard is correct **only** as long as `drain_dtls()` is only called from `tick()`. It is also called from `on_transport_recv` lines 2325 and 2357 — those calls bypass the `tick()` gate. ❌ Same gap as #4. |
| `maybe_install_srtp_keys` line 2447 | ⚠️ called from `drain_dtls` only (so inherits `tick()` gate) | ✅ `if (!impl_->dtls)` line 2439 | ⚠️ Acceptable. |
| `local_dtls_fingerprint_sha256_base64` line 2608 | ❌ | ✅ `if (!d)` line 2604 | ⚠️ See risk #9. |
| `dtls_state` line 2618 | ❌ | ✅ ternary `impl_->dtls ? ...` | ✅ Safe (ternary null-check is sufficient because the function is a const accessor that never modifies the DTLS state). |
| `dtls_connected` line 2622 | ❌ | ✅ short-circuit | ✅ Safe (same reasoning). |

**Bottom line on the state machine:** the engine has **two** methods that dereference `impl_->dtls` without an `is_open()` guard:

1. `process_remote_sdp()` — see risk #1.
2. `on_transport_recv()` (and its indirect callers `drain_dtls()`) — see risk #4.

Both are reachable from the ICE recv callback path, which the stack's thread model documents as engine-thread-only but doesn't enforce.

---

## 5. Specific findings

### Finding #1: `process_remote_sdp()` lacks an `is_open()` guard (Risk: High)

**Location:** `D:\MyOpen\NimRTC\src\engine\src\engine.cpp:1700-1906` (function entry, before line 1700)

**Risk:** `process_remote_sdp()` dereferences `impl_->dtls->set_role()` (line 1818) and `impl_->dtls->set_peer_fingerprint()` (line 1834). The function has an inner `if (impl_->dtls)` guard (line 1800) but **no entry-point `is_open()` check**. After `close()` runs:

- `state_ == kClosed` (line 1374)
- `impl_->dtls == nullptr` (line 1363)
- so the inner null-guard saves the dereference

But the path between `impl_->session.close()` (line 1361, which destroys the actual `IDtlsSession` via `WebRtcClassicStack::close() → dtls_.reset()`) and `impl_->dtls = nullptr` (line 1363) is **a brief UB window** — three sequential statements on the engine thread. A concurrent `process_remote_sdp()` call from another thread would see a non-null but dangling `impl_->dtls` and dereference freed memory. Today the engine API doesn't document thread affinity, so a future caller is free to invoke this from any thread.

**Recommended fix (P0 for v0.12.0):**

```cpp
// engine.cpp:1700 (process_remote_sdp entry)
std::optional<std::string> NimRTCEngine::process_remote_sdp(std::string_view remote_sdp) noexcept {
    if (state_ != State::kOpen) {
        if (on_error_) on_error_(core::kEngineNotReady, "process_remote_sdp: engine not open");
        return std::nullopt;
    }
    // ... existing body unchanged
}
```

This matches the existing pattern in `send_audio()` (line 2021) and `create_data_channel()` (line 1415) and turns the UAF into a `std::nullopt` early-return.

### Finding #2: `on_transport_recv()` lacks an `is_open()` guard (Risk: Critical)

**Location:** `D:\MyOpen\NimRTC\src\engine\src\engine.cpp:2279-2357` (function entry, before line 2279)

**Risk:** The ICE recv callback fires `on_transport_recv()` synchronously from `ice_t_->recv()` (called from `tick()` line 2224). Inside, three branches dereference `impl_->dtls`:

- DTLS-content-type branch (lines 2287-2328): `impl_->dtls->state()` line 2294, `impl_->dtls->feed_inbound()` line 2321
- CCS branch (lines 2335-2357): `impl_->dtls->feed_inbound()` line 2353
- (the non-DTLS branch is SRTP-protected RTP and does not touch `impl_->dtls`)

The guard is `if (impl_->dtls)` (lines 2292, 2349), which protects against the "DTLS-less by design" Rule 2 case but **not** against the post-`close()` dangling case. The `drain_dtls()` calls at lines 2325 and 2357 are inside the `if (is_ice_connected())` block, so they have a partial guard, but they still rely on the `tick()`-via-`is_open()` invariant — which holds today but is a fragile coupling.

**Recommended fix (P0 for v0.12.0):**

```cpp
// engine.cpp:2281 (on_transport_recv entry)
void NimRTCEngine::on_transport_recv(const plugins::BufferView& pkt) noexcept {
    if (!is_open()) return;  // NEW: UAF guard for ICE recv callback path
    if (pkt.empty() || pkt.size() < 1) return;
    // ... existing body unchanged
}
```

And replace the inline `if (is_ice_connected())` gates (lines 2325, 2357) with the existing `is_open()` check moved up to the function entry.

### Finding #3: `Impl::datachannel_transport_cookie` is a forward-declared raw pointer with no read-side contract (Risk: Medium — v1.x trap)

**Location:** `D:\MyOpen\NimRTC\src\engine\src\engine.cpp:328` (decl), 1470 (assign), 1373 (close null-out)

**Risk:** The cookie is documented (lines 301-327) as a placeholder for the v1.x WebTransport/QUIC wiring PR. It is a raw `IDtlsSession*` (non-owning). After `close()` it is `nullptr` (line 1373). Between `close()` and the next `create_data_channel()` call, no code reads the cookie. The v1.x PR will be the **first** reader and is at risk of repeating the UAF pattern.

**Recommended fix (P1 for v0.13.0, ahead of the v1.x wiring PR):**

1. Add a comment at line 328 mandating that any v1.x reader MUST check `is_open() && impl_->dtls != nullptr` before dereferencing.
2. In v1.x, replace the field with `std::variant<IDtlsSession*, IWebTransportSession*>` (per the existing comment at line 312) so the type system enforces the "valid transport" contract.

### Finding #4: `local_dtls_fingerprint_sha256_base64()` is a const accessor with no `is_open()` guard (Risk: Low)

**Location:** `D:\MyOpen\NimRTC\src\engine\src\engine.cpp:2599-2616`

**Risk:** The function reads `impl_->dtls->local_fingerprint()` if `impl_` and `d` are non-null. After `close()`, `impl_` is non-null (the `delete impl_` happens in the destructor, not in `close()`), but `impl_->dtls` is `nullptr` thanks to the line 1363 null-out. So the inner `if (!d) return {};` saves it. The risk is **low** because the field is a const accessor with no thread-affinity contract; a future change that introduces background threads for diagnostics could regress this.

**Recommended fix (P1 for v0.13.0):**

```cpp
// engine.cpp:2601
auto* d = impl_ ? impl_->dtls : nullptr;
if (!d || !is_open()) return {};  // NEW: belt-and-suspenders
```

### Finding #5: `close()` ordering is correct, but the per-field null-out is fragile (Risk: Low)

**Location:** `D:\MyOpen\NimRTC\src\engine\src\engine.cpp:1361-1376`

**Risk:** `close()` performs 4 operations on raw pointers in a specific order:

1. `impl_->session.close();` (line 1361) — destroys the stacks
2. `ice_t_ = nullptr;` (line 1362)
3. `impl_->dtls = nullptr;` (line 1363)
4. `impl_->datachannel_transport_cookie = nullptr;` (line 1373)

The order is correct: the session is torn down **before** the raw pointers are nulled. The risk is that **a future PR that adds another raw non-owning pointer** to the engine must remember to also null it in `close()`. A `weak_ptr` migration (P1, see recommendation #2) would make this automatic.

**Recommended fix (P2 design discussion for v1.x):** see Recommendations #2.

---

## 6. v1.x follow-up scope

Per `D:\MyOpen\NimRTC\docs\plan\v0.11-plan.md` §2.2 and §10:

- **DC-1** Chrome ↔ NimRTC DataChannel bidirectional interop — SCTP-over-DTLS physical wiring (`set_dtls_keys()` + DTLS↔SCTP inbound/outbound hookup) was reverted at tag-prep time and is **deferred to v1.x WebTransport/QUIC redesign** (v0.11-plan.md:107).
- **DC-2** L1 strict-priority send scheduling across RTP × DC plane — only meaningful once DC-1 lands (v0.11-plan.md:108).
- **TPAL-5** `UsrsctpSocketFactory` (id="usrsctp") production backend — class still compiles and links, but `sctp::register_default_plugins()` no longer auto-registers it and headers carry `@deprecated` markers (v0.11-plan.md:109).
- **Forward direction:** the data plane moves to **WebTransport over QUIC** in v1.x — likely msquic (MIT-licensed, Windows-first), per v0.11-plan.md:417-452.

**What the actual v1.x wiring PR will need to do** (extracted from engine.cpp:301-328, 815-839, 1386-1470, v0.11-plan.md §2.2 / §10):

1. Replace `Impl::datachannel_transport_cookie` with `std::variant<IDtlsSession*, IWebTransportSession*>` (per engine.cpp:312).
2. Add `IWebTransportSession` to the IDtlsSession-style PAL seam (Slice 4 analogue) and register a default factory at `id="msquic"` or `id="quiche"`.
3. Plumb the SCTP-over-DTLS path **OR** the WebTransport-over-QUIC path inside `WebRtcClassicStack` (or its successor), including the `set_dtls_keys()` hook (engine.cpp:830) and the `feed_inbound_datagram()` (engine.cpp:835) outbound.
4. Extend `create_data_channel()` to honor the `datachannel_transport_cookie` read site — currently the field is only written, never read (engine.cpp:1470). The v1.x PR will add the first read.
5. Update the cookie's contract comment at engine.cpp:301-328 to reference the new variant type.
6. Run the Chrome-headless e2e harness (currently 0/4 — v0.11-plan.md:107) and the §7 fallback in-process `test_datachannel_engine` 5/5 PASS.

**What the v0.12.0 UAF work does NOT touch:** the v1.x wiring itself. v0.12.0 lands the P0 guards (findings #1, #2) and freezes the cookie contract; v1.x does the work.

---

## 7. Recommendations (prioritized)

### P0 — for v0.12.0 (this release)

1. **Add `is_open()` entry guard to `process_remote_sdp()`** (finding #1) — 2 lines, zero risk regression, matches the existing `send_audio()` / `create_data_channel()` pattern. Critical for closing the medium-rated gap in the risk register.
2. **Add `is_open()` entry guard to `on_transport_recv()`** (finding #2) — 2 lines, zero risk regression, closes the **Critical**-rated gap. This is the single highest-priority mitigation in the entire report.
3. **Document the thread-affinity guarantee on `Impl::dtls` access** in the field comment at engine.cpp:201-223. The comment currently says "lifetime: `impl_->session.media_stack` is destroyed in `delete impl_` (which `~NimRTCEngine` runs before its own members are destroyed). `Impl::dtls` becomes dangling at that point but is never dereferenced afterwards." This is **aspirational**, not enforced. Add: "All `Impl::dtls` dereferences must be guarded by `is_open()` unless called from `init_modules_once` (which runs before the first `state_ == kOpen` transition)."

### P1 — for v0.13.0

4. **Replace `Impl::dtls` and `Impl::datachannel_transport_cookie` with `std::weak_ptr<IDtlsSession>`** (the stack's `WebRtcClassicStack::dtls_` becomes `std::shared_ptr<IDtlsSession>`). This converts all the existing null-checks into `.lock()` calls that **automatically** return `nullptr` after the session is torn down — even across threads, without an `is_open()` guard. The API surface change is local to `engine.cpp` and `webrtc_classic_stack.cpp`; the public API doesn't expose `IDtlsSession` directly so there's no ABI break. Estimated 1 PR, 200-300 lines.
5. **Add `is_open()` guard to `local_dtls_fingerprint_sha256_base64()`** (finding #4) — 1 line, low priority but cheap.
6. **Tighten the `Impl::datachannel_transport_cookie` contract** (finding #3) — write the v1.x reader contract into the field comment, and consider `[[deprecated]]` overloads of any future API that would touch the cookie outside `state_ == kOpen`.

### P2 — design discussion for v1.x

7. **Migrate from raw non-owning pointers to a `std::shared_ptr`/`std::weak_ptr` contract across the whole `ITransportStack` seam.** This is the same idea as recommendation #4 but applied uniformly: `ice()`, `dtls()`, `rtp()`, `sctp()` return `weak_ptr<T>` instead of `T&`. Engine code calls `.lock()` at every access. The whole UAF class of bugs disappears.
8. **Document a single engine-thread invariant** in `engine.hpp` ("`NimRTCEngine` is single-threaded; all public methods must be called from the same thread; ICE recv callbacks fire on the calling thread of `tick()`"). This is the implicit contract today but isn't stated; making it explicit lets future PRs (e.g. a worker-thread tick) catch the violation at review time.
9. **Add AddressSanitizer builds to the CI matrix** for the v0.12.0 release branch so any future UAF regression is caught at PR time instead of by users. The CMake plumbing is in `build/CMakePresets.json` (per the user-supplied git status, the build tree was already cleaned by P0-B).

---

## 8. Open questions

1. **Is the engine intended to be thread-safe?** The current design relies on a single engine thread. The ITransport seam's `set_callbacks` API (engine.cpp:958) registers lambdas that capture `[this]`; if a future PR invokes `tick()` from a worker thread, all the `is_open()` guards become load-load-bearing for thread safety, not just for UAF. Should v0.12.0 add an explicit `std::shared_ptr<NimRTCEngine>` callback API to make this safe?
   - **Resolution:** Single-thread contract assumed for v0.12.0 / v0.13.0. The v0.13.0 weak_ptr migration makes the UAF concern moot across threads (because `weak_ptr::lock()` is itself atomic w.r.t. destruction), so thread-affinity expansion becomes a separate, lower-priority consideration. The Open Question is **answered** for the UAF class but **deferred** for general thread safety (future v1.x).

2. **Should `close()`'s `state_` flip and `impl_->dtls = nullptr` write be inside the same critical section?** The current sequential layout is correct under single-thread assumptions but is a UB window under any multi-thread model. Is the engine OK with the single-thread contract, or do we need `std::atomic<State>` for the state member?
   - **Resolution:** No critical section needed post-v0.13.0. `weak_ptr::lock()` is internally atomic (acquire-load on the control block), so the "non-null but dangling" race is **eliminated by the type system**, not by a critical section. v0.13.0 engine explicitly removes the `impl_->dtls = nullptr` line (line 1374) because the weak_ptr auto-expires — closing the "session torn down before null-out" window from §5.

3. **What is the v1.x migration story for `Impl::datachannel_transport_cookie`?** The current field is `IDtlsSession*`; the v1.x PR will replace it with `std::variant<IDtlsSession*, IWebTransportSession*>` (per engine.cpp:312). Should v0.12.0 already stage this variant so the v1.x PR is a one-liner, or is the deferred-change pattern the right one?
   - **Resolution:** Deferred-change pattern. v0.13.0 only converted to `weak_ptr` (matching `Impl::dtls`). The `std::variant<IDtlsSession*, IWebTransportSession*>` is still a v1.x PR — the comment block at engine.cpp:1484 explicitly defers it.

4. **Is the v0.12.0 release permitted to land P0 changes to `src/engine/src/engine.cpp`?** The user-supplied git status marks `src/engine/CMakeLists.txt` as M (another agent's work) but does **not** mark `src/engine/src/engine.cpp` as M. The 2-line `is_open()` additions are localized and unlikely to conflict with the parallel work; confirm with the user that this is in scope.
   - **Resolution:** Yes, P0 + P1 both landed. No merge conflicts with the parallel PAL Slice 1.5 work (which touched `engine_plugin_resolver.hpp` / `make_plugin.hpp` / `registry.hpp` but not `engine.cpp`).

5. **Should the v0.12.0 P0 changes be one PR or two?** Findings #1 and #2 are independent. The `process_remote_sdp()` fix is a public-API behavior change (callers see a new error code); the `on_transport_recv()` fix is internal. Splitting them makes review and bisection easier.
   - **Resolution:** Combined into one CHANGELOG `### Fixed` entry in v0.12.0 (per the "按建议执行" directive). The v0.13.0 weak_ptr migration is split out into its own `### Changed` section + `### Added` test entry.

6. **What is the team's appetite for the `weak_ptr` migration (recommendation #4)?** It is a small PR but changes a `unique_ptr` ownership contract that has been stable since Slice 7.6 PR3 (v0.12.0). If the team prefers a conservative v0.12.0, recommendation #4 can wait until v0.13.0 or v1.x.
   - **Resolution:** Landed in v0.13.0 (5-stage migration: `dtls_weak()` interface, 3 stack implementations, engine `Impl::dtls` + `Impl::datachannel_transport_cookie` migration, 14 use sites with `.lock()` pattern, regression test). The unique → shared ptr change is **not breaking** (additive: a `unique_ptr&&` can be moved into a `shared_ptr`).

7. **Is there a memory-ordering concern in the `state_` → `impl_->dtls` write order in `close()`?** The two writes are plain assignments (engine.cpp:1363, 1374). On x86-64 MSVC this is fine; on ARM or with a weakly-ordered memory model, a concurrent reader could observe `state_ == kClosed` and a non-null but dangling `impl_->dtls`. v0.12.0 may want to declare `state_` `std::atomic<State>` and use release-store / acquire-load.
   - **Resolution:** No concern post-v0.13.0. `impl_->dtls` is no longer a raw pointer write; it's a `weak_ptr` whose `lock()` is internally atomic w.r.t. the underlying `shared_ptr` control block. The "state_ says closed but dtls is still pointing" race is **structurally impossible** because `weak_ptr::lock()` consults the control block's refcount under an acquire fence — if the stack's `dtls_` `shared_ptr` has been reset (during `close()`), the weak_ptr's `lock()` returns `nullptr` regardless of `state_`. No `std::atomic<State>` needed for the UAF concern.

---

## Appendix A — File and line index

For reviewers cross-referencing the report against the source:

| Concept | File | Lines |
|---|---|---|
| `Impl::dtls` declaration | `src/engine/src/engine.cpp` | 224 |
| `Impl::datachannel_transport_cookie` declaration | `src/engine/src/engine.cpp` | 328 |
| `Impl::session` declaration | `src/engine/src/engine.cpp` | 247 |
| `state_` enum (kConstructed, kOpen, kClosed) | `src/engine/include/nimrtc/engine/engine.hpp` | 489 |
| `NimRTCEngine` ctor | `src/engine/src/engine.cpp` | 351-352 |
| `~NimRTCEngine` | `src/engine/src/engine.cpp` | 354-360 |
| `pre_open()` entry guard | `src/engine/src/engine.cpp` | 440 |
| `init_modules_once()` orchestrator | `src/engine/src/engine.cpp` | 481-485 |
| `init_transport_strategy()` (phase 1) | `src/engine/src/engine.cpp` | 515+ |
| `init_transport_session()` (phase 2) — sets `impl_->dtls` | `src/engine/src/engine.cpp` | 676-712 |
| `init_dtls_handshake()` (phase 6) — first deref | `src/engine/src/engine.cpp` | 906-940 |
| `open()` success state flip | `src/engine/src/engine.cpp` | 1022 |
| `process_remote_sdp()` (NO state guard) | `src/engine/src/engine.cpp` | 1700-2029 |
| `send_audio()` (state guard + null guard) | `src/engine/src/engine.cpp` | 2015-2112 |
| `create_data_channel()` (state guard) | `src/engine/src/engine.cpp` | 1411-1472 |
| `close()` — tears down session, nulls pointers, flips state | `src/engine/src/engine.cpp` | 1340-1376 |
| `on_transport_recv()` (NO state guard) | `src/engine/src/engine.cpp` | 2279-2357 |
| `tick()` (state guard via `is_open()`) | `src/engine/src/engine.cpp` | 2186-2225 |
| `drain_dtls()` (null guard; inherited tick guard) | `src/engine/src/engine.cpp` | 2392-2415 |
| `maybe_install_srtp_keys()` (null guard) | `src/engine/src/engine.cpp` | 2430-2525 |
| `local_dtls_fingerprint_sha256_base64()` (NO state guard) | `src/engine/src/engine.cpp` | 2599-2653 |
| `dtls_state()` / `dtls_connected()` (ternary null guard) | `src/engine/src/engine.cpp` | 2617-2624 |
| `ITransportStack::dtls()` accessor (returns reference) | `src/transport/include/nimrtc/transport/transport_stack.hpp` | 122 |
| `WebRtcClassicStack::dtls_` (unique_ptr<IDtlsSession>) | `src/transport/src/webrtc_classic_stack.cpp` | 179 |
| `WebRtcClassicStack::close()` — tears down `dtls_` | `src/transport/src/webrtc_classic_stack.cpp` | 107-115 |
| v1.x deferral rationale | `docs/plan/v0.11-plan.md` | 4, 10, 73-76, 95-111, 417-452 |
| IDtlsSession seam definition | `src/dtls/include/nimrtc/dtls/dtls_session_iface.hpp` | 109-218 |

## Appendix B — Test matrix (suggested for the P0 fix PR)

| Test | Pre-condition | Action | Expected | Coverage |
|---|---|---|---|---|
| `test_uaf_close_then_process_remote_sdp` | engine open | `close()` then `process_remote_sdp("v=0...")` | `std::nullopt` returned; no crash; `on_error_` fires with `kEngineNotReady` | Finding #1 |
| `test_uaf_close_then_tick` | engine open | `close()` then `tick()` | `0` returned; no crash | Finding #2 (defense-in-depth) |
| `test_uaf_close_then_send_audio` | engine open | `close()` then `send_audio(pcm, 480)` | `kEngineNotReady` returned; no crash | Reference for existing good pattern |
| `test_uaf_dtor_during_pending_recv` | engine open; ICE recv scheduled via test harness | `~NimRTCEngine()` while a tick is queued | `~NimRTCEngine` runs `close()` first; queued tick sees `state_ == kClosed` and returns 0 | Finding #2 + dtor ordering |
| `test_uaf_double_close` | engine open | `close()` then `close()` | second is a no-op (early-out line 1341) | Idempotency check |
| `test_uaf_close_then_local_dtls_fingerprint` | engine open | `close()` then `local_dtls_fingerprint()` | empty string returned; no crash | Finding #4 |

(These tests would land in `tests/test_engine_uaf.cpp` per the v0.11.0 test naming convention; they would be a separate PR from the production code fix so the v0.12.0 release PR stays bisectable.)

---

*End of report.*
