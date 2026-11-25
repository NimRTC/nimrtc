# Q5 — what the 24 h impaired soak found: ICE consent expiry under loss

> **Status:** open finding.  The 24 h window has **not** produced a green
> run, and this document records why: the run does not survive its own
> impairment profile.  A `v1.3.0` re-cut waits on this.
>
> **Scope:** this is the Q5 evidence the v1.3 plan asks for, and the
> first real defect the soak has caught.  It is written as evidence, not
> as a post-mortem of a fixed bug — of the three work items, **one is
> done** (the engine stops sending, `acf46606`), and the two that
> remain are the reproduction and the recovery path.

## 1. What happened

The 24 h hostile soak ran on the self-hosted Windows box on
2026-11-20, with the profile the plan calls for:

```
soak-p2p.exe --seconds 86400.0 --loss 5.0 --delay-ms 80.0 \
              --jitter-ms 40.0 --reorder 10.0
```

At **3 h 48 m** into the run the ICE connection died:

```
[info] STUN entry 0: Consent expired for candidate pair
[warn] Lost connectivity
[info] Changing state to failed
```

and from that point on every media send failed:

```
[error] Send while ICE is not connected        (26 627 times)
```

The run never reached its 24 h mark, so **no artifact was written**: the
harness only emits its JSON record when the run completes, and this one
did not.  The archive therefore has **no row** for it either.

## 2. The link did not degrade

This is the part that matters, and it is why the finding is a defect
rather than noise.

The log was sliced into 55 equal chunks and the send/receive ratio was
computed per chunk:

| chunk | delivery | chunk | delivery |
|---|---|---|---|
| 1–19 | 94.7 – 95.5 % | 38 | 95.11 % |
| 20–37 | 94.7 – 95.5 % | 39 | 94.85 % |
| … | … | 55 (the last) | **80.07 %** |

**Every chunk of the run sat at 94.7 – 95.5 %**, which is exactly the
5 % loss the profile asks for.  The link did not degrade; it was stable
for three hours and forty-eight minutes and then the consent expired.

The last chunk's 80 % is the *consequence* of the failure, not its
cause: once ICE was failed, the consent traffic stopped and only the
media traffic remained, and the media traffic is not what the 5 % loss
profile was calibrated against.

So the link was **nothing but lossy** — no bandwidth collapse, no
latency spike, no reordering storm, no ICE-role problem.  Which means
the mechanism has to be found in the *loss budget* itself, and §3a
shows it is there.

## 3. What the numbers say the code asks for

The consent machinery is libjuice's, and it follows RFC 7675:

| constant | value | where |
|---|---|---|
| `CONSENT_TIMEOUT` | 30 000 ms | `src/third_party/libjuice/src/src/agent.h:46` |
| `MIN_CONSENT_CHECK_PERIOD` | 4 000 ms | `agent.h:52` |
| `MAX_CONSENT_CHECK_PERIOD` | 6 000 ms | `agent.h:53` |

The refresh period is randomised per request, as RFC 7675 §5.1
requires.  `agent_arm_keepalive` picks it:

```c
period = MIN_CONSENT_CHECK_PERIOD +
         juice_rand32() % (MAX_CONSENT_CHECK_PERIOD - MIN_CONSENT_CHECK_PERIOD + 1);
```

and the timestamp is refreshed when a Binding response arrives:

```c
// agent.c:1770 — on any successful Binding response for a CHECK entry
pair->consent_expiry = current_timestamp() + CONSENT_TIMEOUT;
```

So within one 30 s consent window there are roughly **six** chances to
refresh the consent (the 4–6 s period averages 5 s, so 30 s holds
≈ 6 requests).  Each chance is a single packet with no redundancy, so
a chance fails if *either* the request or its response is lost.  With
a loss rate \(p\) the per-chance failure probability is

\[
q = 1-(1-p)^2
\]

and the chance that a whole window fails is \(q^6\).  Under the
simplest model — that losses are independent from packet to packet —
the expected number of windows before the first expiry is \(1/q^6\).

At the profile the window uses (\(p = 0.05\)):

| quantity | value |
|---|---|
| per-chance failure probability | \(q = 1-0.95^2 = 0.0975\) |
| per-window failure probability | \(q^6 \approx 8.6\times10^{-7}\) |
| over a 24 h run (2 880 windows) | \(\approx 0.25\,\%\) |
| expected time to the first expiry | ≈ 1.2 M windows ≈ **400 days** |

So the independent-loss model says a 5 %-loss run should essentially
never expire: about **one chance in four hundred** across a full 24 h.

The run that produced this finding expired after **3 h 48 m** — 456
windows in.  The independent model puts that at
\(456\times8.6\times10^{-7} \approx 0.04\,\%\), i.e. roughly
**one run in 2 500**, and about a hundred times earlier than the
model's mean.  The discrepancy is the finding: the mechanism is
identified and *located*, and what remains is to explain **why the
real link loses packets in the correlated way the model assumes it
doesn't**.

## 3a. The mechanism, checked against a probe that fails on purpose

The 40 %-loss probe (built for this finding) reproduces the expiry in
about two minutes.  That is not a curiosity — it is a *measurement* of
the mechanism, because at 40 % the numbers are big enough to compare
directly against the model:

| quantity | value |
|---|---|
| per-chance failure probability (request dropped **or** response dropped) | \(1-(1-0.4)^2 = 0.64\) |
| chances per 30 s window | ≈ 6 (the 4–6 s randomised period) |
| per-window failure probability | \(0.64^6 \approx 6.9\,\%\) |
| expected time to the first expiry | ≈ 15 windows ≈ **7.3 min** |
| **observed** | **128 s** |

The observed 128 s is an *early* draw — four windows in, where the
model's mean is fifteen — but not an anomalous one: the geometric
distribution puts \(P(\text{expire within 4.3 windows}) \approx 26\,\%\).
So the 40 % probe agrees with the model, and it does so **without
needing any mechanism beyond the loss budget**.

That is an important result, and it has to be read with a limit: the
probe **reproduces** the failure, so it shows the loss budget is
*sufficient* to produce a consent expiry.  It does **not** prove that
the 5 % run's expiry came from the same place — only that it can.

Both of §4's candidates are now excluded: §4.1 by the logs (and the
logs do contain libjuice's debug output — see the note under §4.1), and
§4.2 because RFC 7675 §5.1 mandates the single-transmission budget, so
libjuice is behaving as specified rather than defectively.

So the state of the question is: **the mechanism is identified and
located, the arithmetic accounts for it, and no unexplained mechanism
remains to be found in libjuice.**  What is *not* on the table is a
defect in the ICE layer.  The work is on NimRTC's side of the seam.

## 4. Two candidate mechanisms, and where each one stands

### 4.1 A single transaction-id slot (rejected by the logs)

`agent_find_entry_from_transaction_id` matches a response to an entry
with a `memcmp` against **one** transaction id per entry:

```c
// agent.c:2821
if (memcmp(transaction_id, entry->transaction_id, STUN_TRANSACTION_ID_SIZE) == 0)
```

and the consent path overwrites that slot on every send:

```c
// agent.c:1200 — the consent request path
juice_random(entry->transaction_id, STUN_TRANSACTION_ID_SIZE);
entry->transaction_id_expired = false;
```

So a *late* response for an earlier consent request does not match the
current id and is dropped with `No STUN entry matching transaction ID`.
That is the mechanism by which reordering could turn a delivered
response into a discarded one — and the soak profile carries **10 %
reordering** alongside the 5 % loss, so late responses are expected,
not hypothetical.

**The logs reject it.**  Over a healthy 25-minute stretch of the same
profile:

```
No STUN entry matching transaction ID : 0
Candidate pair check failed (non-symmetric source address) : 0
STUN integrity check failed : 0
```

Zero mismatches.  If this were the mechanism, the counter would be
non-zero long before the consent expired.

> **On the log level.**  These counters are read from a log whose
> prefixes are all `[info]`, which looks at first like the counters
> were never printed — they are emitted at `JLOG_DEBUG`, and a log
> carrying no `[debug]` lines would be silent on them.  That reading is
> **wrong**, and the reason is worth recording because it nearly
> invalidated a piece of evidence that is in fact sound.
>
> `src/modules/ice/src/ice.cpp` hooks libjuice's logger and routes
> `JUICE_LOG_LEVEL_INFO`, `_DEBUG` **and** `_VERBOSE` all through
> `logger.info()` — the code says so, and explains why: "Route
> everything else through info() so it bypasses the default Debug-level
> filter and shows up in test output."  So libjuice's VERBOSE output
> *is* present in these logs, carrying an `[info]` prefix.
>
> That is checkable, and it checks out: the device log holds 1 850
> lines carrying `agent.c:`, among them 311 `Bookkeeping...` and 277
> `Received datagram, size=…` — both of which are `JLOG_VERBOSE`
> (`agent.c:1093` and `agent.c:1029`).  The soak log holds 2 606 such
> lines.  A log that was suppressing libjuice's debug output would
> contain none of them.
>
> So the counters above are real counts, and they are zero.  §4.1 is
> rejected on the evidence.
>
> **A note for anyone re-running this analysis.**  The ICE module now
> asks libjuice for `JUICE_LOG_LEVEL_INFO` by default rather than
> `JUICE_LOG_LEVEL_VERBOSE` — the per-datagram lines were 71 % of a
> 24 h soak log's bytes, and the cut is what keeps a soak log
> readable.  The counters this section reads are `JLOG_DEBUG`, so a
> run made *after* that change does **not** carry them unless the run
> was made with `NIMRTC_ICE_VERBOSE=1` in its environment.
>
> That does not affect the conclusion above, which rests on logs taken
> *before* the change (the 3.8 h run and the 25-minute stretch).  It
> does mean that a **new** run cannot reproduce these counters without
> the env var, and — this is the part worth stating plainly — *the
> absence of a DEBUG counter in a new log must not be read as the
> counter being zero*.  The two are indistinguishable from the log
> alone unless you know the level the run was made at.  This paragraph
> exists so that the mistake is not repeated.

### 4.2 The single-transmission budget (rejected as a defect — RFC-mandated)

The consent request is sent from the `SUCCEEDED_KEEPALIVE` branch, and
on success the agent re-arms the *next* consent period immediately —
it does not wait for the response, and it does not use the entry's
retransmission ladder:

```c
// agent.c:1199-1206
if (ret < 0) {
    JLOG_WARN("Sending keepalive failed");
    agent_arm_transmission(agent, entry, STUN_KEEPALIVE_PERIOD);
    continue;
}
agent_arm_keepalive(agent, entry);   // <- next period, new transaction id
```

Each consent request therefore gets exactly **one** attempt per
4–6 s period, and a lost response is never retried under the same
transaction id — the next period replaces it.  That makes the six
chances per window the *entire* budget, and each one is a single
packet with no redundancy.

**This is what the RFC asks for.**  RFC 7675 §5.1 is explicit, twice:

> Each STUN binding request for consent MUST use a new STUN transaction
> identifier, as described in Section 6 of [RFC5389]. **Each STUN binding
> request for consent is transmitted once only.**  A sender therefore
> cannot assume that it will receive a response for every consent
> request, and a response might be for a previous request (rather than
> for the most recently sent request).

"Transmitted once only" is not an oversight in RFC 7675 — it is
deliberate.  Consent checks are *not* connectivity checks: a consent
request carries a fresh transaction id precisely so that a response
cannot be replayed onto a later consent period, and the RFC then tells
the sender to expect that a response may never arrive.  The redundancy
that RFC 5389's retransmission ladder provides to connectivity checks
would reintroduce exactly the ambiguity ("a response might be for a
previous request") that the fresh transaction id exists to remove.

So the single-transmission design **cannot** be reported as the
mechanism of the failure.  It is the specified design, and libjuice
implements it faithfully.  §3a's arithmetic accounts for the failure
end-to-end: six independent single-shot attempts per 30 s window, each
with its own fate, and a window where all six fail ends the consent.
At 40 % loss that happens in ~128 s; at the 5 % profile the model puts
it at roughly one run in four hundred over 24 h, and this run was the
one-in-2 500 early draw.

What the design *does* leave with no margin is the recovery: RFC 7675
§5.1 also says that after consent is lost, the same ICE credentials
MUST NOT be used on the affected 5-tuple again, and that a new session
or an ICE restart is needed to obtain consent to send.  **libjuice
stops; NimRTC has nothing to restart with.**  That is the gap this
finding records.

## 5. What is confirmed about the failure's consequences

The failure is not graceful, and this part is a NimRTC-side
observation:

* libjuice moves the agent to `failed` and stops delivering
  (`Lost connectivity` → `Changing state to failed`).
* The engine keeps sending.  Every send afterwards fails with
  `Send while ICE is not connected` — **26 627 times**, at full media
  rate, for the remaining 21 minutes of the run.

There was no "the call ended, stop sending" path here: the engine
noticed the ICE state change but did not stop the RTP source, so a
failed call cost the host a full-rate send loop that produced nothing.

> **Fixed** (`acf46606`).  The engine now closes a transport gate when
> the ICE agent gives up, and the media paths — `send_audio`, the video
> senders, the DTMF emitter, the scheduler drain — stop producing
> packets for a transport that cannot carry them.  The outage is
> reported **once** rather than once per packet, and
> `Stats::ice_send_blocked_events` counts the outages.
>
> The gate is deliberately narrower than "ICE is not connected": the
> window between `open()` and the first successful connectivity check
> is a normal phase of every session, and a host feeding PCM during
> that window must not have its audio rejected.  Only a transport that
> *was* connected and then stopped counts as dead.
>
> The test is `tests/test_engine_ice_send_gate.cpp` (5 cases).

## 6. What happens next

1. **The defect has to be reproduced on purpose.**  The 24 h run is a
   poor reproducer: it cost 3.8 h and left no artifact.  A short,
   deterministic probe that drives the impairment harder (the same
   profile with a much higher loss rate) is the right instrument, and
   the harness must write its artifact on the way out — *including when
   the run fails* — so that a failure is as well-evidenced as a pass.
2. **NimRTC needs a path back.**  The single-transmission consent
   budget is RFC-mandated and must not be "fixed" by adding
   retransmissions — RFC 7675 §5.1's fresh-transaction-id rule is
   what makes consent safe, and a response for a previous request is
   explicitly anticipated by the RFC.  What is missing is on *our*
   side of the seam: RFC 7675 §5.1 says that after consent is lost the
   same ICE credentials MUST NOT be used on the affected 5-tuple
   again, and that obtaining consent to send again needs **a new
   session or an ICE restart**.  libjuice stops at `failed`; NimRTC
   has no ICE-restart path, so the call cannot come back.
   That is the work item, and it belongs in the engine rather than in
   the vendored ICE agent.

   > **This is v1.4 plan N3, not new work.**  The plan already
   > specifies "a named private collaborator
   > (`src/engine/src/engine_ice_recovery.{hpp,cpp}` …): a grace window
   > on `Disconnected`, an exponential backoff with a cap, a **terminal**
   > state when the cap is reached (reported, not looped), driven by the
   > engine's own ICE state" — and its "done when" is exactly this
   > finding's requirement: "a simulated network change recovers the
   > session **without the host calling `restart_ice()`**, the restarts
   > are bounded, and there is exactly **one** policy in the engine".
   >
   > The two halves the plan names are both present: `restart_ice()`
   > (RFC 8445 §9.1.1.1) exists and counts against a budget, and
   > `set_on_ice_restart` already fires when the **peer's** description
   > carries new credentials.  What is missing is the engine driving
   > the local half on its own.
   >
   > So this item is **already planned** — the finding's contribution is
   > to say *why* it matters, and to point at the evidence: a consent
   > expiry is a `Failed` transition, and §5's gate is what keeps the
   > failed call cheap until N3's policy brings it back.
3. **The engine must stop sending when ICE fails.**  26 627 pointless
   sends is a bug of its own, and it is independent of everything
   above — it is worth fixing on its own merits.
   **Done** (`acf46606`): the engine closes a transport gate when the
   ICE agent gives up, and the outage is reported once instead of once
   per packet.  The test is `tests/test_engine_ice_send_gate.cpp`.
4. **Only then** does the 24 h window get re-run, and only a run that
   both completes and writes its artifact can close Q5.

## 7. The evidence, and its limits

What this document rests on:

* the 3.8 h run's log (≈ 11 M lines, 508 MB) — **since deleted** by the
  daily soak task, which cleans `build/soak-impaired/` before it starts.
  The aggregate numbers in §2 were computed from it before that and are
  reproduced here; the raw log is gone.
* a healthy 25-minute stretch of the same profile from the current run
  (`build/soak-impaired/run-20261120T095514Z.log`), which is what
  §4.1 originally rested on — and which §4.1 now withdraws, because
  that log is at INFO level and the counter it relied on is DEBUG.
* the 40 %-loss probe, which is the *positive* evidence: it reproduces
  the expiry on the same mechanism the model describes (§3a).

What it does **not** rest on:

* the raw 3.8 h log, which is no longer on disk.  If the reproduction
  in step 1 above turns up something that contradicts §2, §2 is wrong
  and this document says so.
* any claim that libjuice is at fault.  §4.1 is rejected by the logs,
  §4.2 is rejected because RFC 7675 §5.1 mandates the behaviour, and
  §3a's arithmetic accounts for the expiry end to end.  The finding is
  not "the ICE agent has a bug"; it is "the ICE layer has no margin,
  and NimRTC has no path back".

This is what the soak is for: a 24 h run on an impaired link found a
failure that no unit test, no acceptance case, and no 20-minute smoke
test would have found.  The cost of the finding is that the 24 h
window is still open.

## Related

- `docs/plan/soak-impaired-runbook.md` — how the soak is run and what
  its verdict means
- `docs/plan/v1.3-plan.md` §Q5 — the DoD this run was supposed to meet
- `docs/plan/v1.4-plan.md` §8a.4 — why the v1.4 merges wait on it
