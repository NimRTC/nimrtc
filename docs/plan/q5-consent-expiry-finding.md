# Q5 — what the 24 h impaired soak found: ICE consent expiry under loss

> **Status:** open finding.  The 24 h window has **not** produced a green
> run, and this document records why: the run does not survive its own
> impairment profile.  A `v1.3.0` re-cut waits on this.
>
> **Scope:** this is the Q5 evidence the v1.3 plan asks for, and the
> first real defect the soak has caught.  It is written as evidence, not
> as a post-mortem of a fixed bug — the defect is **not fixed yet**.

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

## 2. The network was not the cause

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

So the question is narrow and answerable: **on a link that is losing
5 % of its packets and holding nothing else wrong, why did the ICE
consent freshness timer run out?**

## 3. What the numbers say the code asks for

The consent machinery is libjuice's, and it follows RFC 7675:

| constant | value | where |
|---|---|---|
| `CONSENT_TIMEOUT` | 30 000 ms | `src/third_party/libjuice/src/src/agent.h:46` |
| `MIN_CONSENT_CHECK_PERIOD` | 4 000 ms | `agent.h:52` |
| `MAX_CONSENT_CHECK_PERIOD` | 6 000 ms | `agent.h:53` |

The refresh period is randomised per request, as RFC 7675 §4.2
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

So within one 30 s consent window there are roughly **five to seven**
independent chances to refresh the consent.  With 5 % loss and each
request carrying its own fate, the probability that *all* of them are
lost is \(0.05^{5\ldots7} \approx 10^{-9\ldots-11}\).  Over 3.8 h
(≈ 2 700 consent cycles) the expected number of such events is
≈ \(3 \times 10^{-6}\).

**The observed failure cannot be explained by independent loss.**  So
either the losses are *not* independent, or the responses that did come
back were **discarded on the way in** rather than lost on the way out.

## 4. The two candidate mechanisms

### 4.1 A single transaction-id slot (rejected by the logs)

`agent_find_entry_from_transaction_id` matches a response to an entry
with a `memcmp` against **one** transaction id per entry:

```c
// agent.c:2784
if (memcmp(transaction_id, entry->transaction_id, STUN_TRANSACTION_ID_SIZE) == 0)
```

and the consent path overwrites that slot on every send:

```c
// agent.c:1162 — the consent request path
juice_random(entry->transaction_id, STUN_TRANSACTION_ID_SIZE);
entry->transaction_id_expired = false;
```

So a *late* response for an earlier consent request does not match the
current id and is dropped with `No STUN entry matching transaction ID`.
That is the mechanism by which reordering could turn a delivered
response into a discarded one.

**The logs reject it.**  Over a healthy 25-minute stretch of the same
profile:

```
No STUN entry matching transaction ID : 0
Candidate pair check failed (non-symmetric source address) : 0
STUN integrity check failed : 0
```

Zero mismatches.  If this were the mechanism, the counter would be
non-zero long before the consent expired.

### 4.2 The retransmission budget exceeds the consent timeout (open)

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
transaction id — the next period replaces it.

That makes the five-to-seven chances per window the *entire* budget,
and each one is a single packet with no redundancy.  The statistics in
§3 still say that cannot explain a 3.8 h run, so this is not yet a
complete explanation either.  What it does explain is why the design
has **no margin**: a single unlucky window ends the session, and
nothing in the agent recovers from it.

RFC 7675 §4.2 asks for the consent interval to be a fraction of the
timeout *precisely so that* several attempts fit in one window.  The
interval is right; the **absence of any retry within a period** is what
leaves the design with a single point of failure per period.

## 5. What is confirmed about the failure's consequences

The failure is not graceful, and this part is a NimRTC-side
observation:

* libjuice moves the agent to `failed` and stops delivering
  (`Lost connectivity` → `Changing state to failed`).
* The engine keeps sending.  Every send afterwards fails with
  `Send while ICE is not connected` — **26 627 times**, at full media
  rate, for the remaining 21 minutes of the run.

There is no "the call ended, stop sending" path here: the engine
notices the ICE state change but does not stop the RTP source, so a
failed call costs the host a full-rate send loop that produces nothing.
That is worth fixing on its own merits, independently of the consent
question.

## 6. What happens next

1. **The defect has to be reproduced on purpose.**  The 24 h run is a
   poor reproducer: it cost 3.8 h and left no artifact.  A short,
   deterministic probe that drives the impairment harder (the same
   profile with a much higher loss rate) is the right instrument, and
   the harness must write its artifact on the way out — *including when
   the run fails* — so that a failure is as well-evidenced as a pass.
2. **The consent request needs redundancy inside one period.**  A
   consent request whose response is lost should be retried under the
   same transaction id (RFC 5389's ladder) before the next period
   replaces it, so that one period is not one point of failure.
3. **The engine must stop sending when ICE fails.**  26 627 pointless
   sends is a bug of its own.
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
  rejects §4.1.

What it does **not** rest on:

* the raw 3.8 h log, which is no longer on disk.  If the reproduction
  in step 1 above turns up something that contradicts §2, §2 is wrong
  and this document says so.
* any claim about *which* mechanism produced the expiry.  §4.1 is
  rejected on the evidence; §4.2 is open.  The next run must settle it.

This is what the soak is for: a 24 h run on an impaired link found a
failure that no unit test, no acceptance case, and no 20-minute smoke
test would have found.  The cost of the finding is that the 24 h
window is still open.

## Related

- `docs/plan/soak-impaired-runbook.md` — how the soak is run and what
  its verdict means
- `docs/plan/v1.3-plan.md` §Q5 — the DoD this run was supposed to meet
- `docs/plan/v1.4-plan.md` §8a.4 — why the v1.4 merges wait on it
