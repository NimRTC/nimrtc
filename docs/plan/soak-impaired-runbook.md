# Soak Runbook (Q5) — the impaired soak across NAT + netem

> **What this document is.** The v1.3 plan's **Q5** asks for "a 24 h+
> run [that] completes green under reordering + loss + jitter, and the
> run is **archived with its netem parameters**" (v1.3 plan §3 Q5).
> This runbook is the operating procedure for producing that evidence:
> what runs, where it writes, how the verdict is derived, and — just as
> important — what the evidence does **not** say.
>
> The collector is `tools/run_soak_impaired.py`. The summary is read
> with `python tools/run_soak_impaired.py --summary`.
>
> The **seven-day clean-network** window has its own runbook
> ([`soak-runbook.md`](soak-runbook.md)). The two windows are different
> measurements and are not substitutes for one another: this one
> *impairs* the network and drives the *engine*, that one runs the
> end-to-end acceptance cases on a clean link.

## 1. The shape of the window

| | |
|---|---|
| Requirement | one run of **≥ 24 h** (86400 s), green, under **loss + jitter + reorder**, on a **clean tree** |
| What counts as green | the harness exited 0 **and** the archived numbers re-evaluate green (the recorder does not trust the harness's own flag) |
| What does not count | a shorter run (a smoke test), a run on a dirty tree, a run missing any one of the three impairments |
| Where the evidence lives | `build/soak-impaired/<platform>.jsonl` (one line per run) plus one `run-<UTC>.json` artifact per run |
| Where the summary comes from | `python tools/run_soak_impaired.py --summary --expect-q5` |

The duration is part of the verdict, not a detail: `--expect-q5`
counts only runs whose **measured elapsed time** is at least 24 h, whose
profile had all three impairments non-zero, and whose tree was clean.

## 2. What actually runs

`examples/soak-p2p` — two real `NimRTCEngine`s (ICE + DTLS + SRTP +
RTP) streaming audio across a **`net_sim::NatRelay`**:

* the relay is the **simulated NAT**: neither engine ever sees the
  other's real address, so each one sees only the relay's endpoint —
  exactly what a peer behind a NAT sees;
* the relay applies the **impairment profile** (loss / delay / jitter /
  reorder / rate) to every packet it forwards, so the impairments hit
  the **real RTP path**, not a transport-floor socket;
* the harness prints one JSON object with the parameters it was given
  and the counters it measured.

The recorder (`tools/run_soak_impaired.py`) folds that JSON together
with the **git provenance** of the tree, re-evaluates the green
criteria from the archived numbers, and appends one record to the
evidence file.

### 2.1 The profiles

| Profile | Loss | Delay | Jitter | Reorder |
|---|---|---|---|---|
| `--profile default` | the flag values (all zero by default) | | | |
| `--profile mild` | 2 % | 20 ms | ±10 ms | 1 % |
| `--profile hostile` | 5 % | 80 ms | ±40 ms | 10 % |

The 24 h window is normally run at `--profile hostile` (the plan's
worst case) or at explicit flag values that name the three impairments.

## 3. The invocation

```bash
# Build the harness.
cmake -S . -B build -DNIMRTC_MODULE_NET_SIM=ON -DNIMRTC_BUILD_EXAMPLES=ON
cmake --build build --config Release --target soak-p2p

# The 24 h window (on the box that stays up).
python tools/run_soak_impaired.py --seconds 86400 --profile hostile \
    --out-dir build/soak-impaired

# What has been recorded so far.
python tools/run_soak_impaired.py --summary
python tools/run_soak_impaired.py --summary --expect-q5   # the gate
```

`--seconds 86400` is the plan's bar. A shorter run is a **smoke
test**: it is recorded as such and it does not satisfy `--expect-q5`.

### 3.1 The honesty rules

The recorder encodes three rules, and they are the reason the archive
can be trusted:

1. **A run that could not be measured is INCONCLUSIVE, never PASS.** A
   harness that died, could not bind, or ran out of time says nothing
   about the engine; crediting it would let a broken runner look like a
   green day.
2. **The green criteria are re-derived from the archived numbers**, not
   read from the harness's `"green"` field. A harness bug that faked a
   pass is still caught by the archive.
3. **A record on a dirty tree is marked.** A dirty tree is not the
   commit it claims, so `--expect-q5` does not accept it.

## 4. What "green" means here

The invariant set, evaluated over the run:

| Invariant | The bound |
|---|---|
| the media path carried media | the sender emitted RTP and the receiver decoded frames |
| the relay carried traffic | the relay forwarded packets |
| memory stayed bounded (Q2) | the receive path held at most 4 s of PCM |
| the feedback rate stayed bounded (Q4) | outbound NACK + PLI did not outnumber the RTP they describe |
| the loss was recovered | no decode errors, and a run that dropped packets recovered at least one frame |
| a 100 % profile dropped | a 100 % loss run that reports zero drops fails (a profile that never drops makes the soak a lie) |

The thresholds live in `examples/soak-p2p/soak-p2p.cpp` **and** are
re-derived in `tools/run_soak_impaired.py`; a change to one must be made
to the other.

## 5. Scheduling

The 24 h window runs on a **self-hosted / scheduled machine** — a
GitHub-hosted runner is not guaranteed to be up for 24 h (v1.3 plan §5,
Q5-3). The GitHub Actions leg that runs the **seven-day clean-network**
window is `.github/workflows/soak.yml`; the impaired window has its own
procedure, and the local box is the one that owns a 24 h run.

## 6. What this evidence does **not** say

* **It is not a real-TURN measurement.** The NAT is a *simulated*
  address-translating relay (`net_sim::NatRelay`), not a TURN server.
  The v1.3 plan's Q5-1 default is exactly this — the **real TURN/TLS
  leg is deferred to v1.4.0** and is recorded here as a known gap. A
  green run says the engine survives a NAT-shaped network, not that it
  interoperates with a TURN server.
* **It is not `tc qdisc netem`.** The impairment model is the
  *in-process* one (`net_sim::ImpairmentConfig`), which is the portable
  equivalent: same loss / delay / jitter / reorder / rate semantics, on
  every platform, without root. The kernel-side counterpart is
  `tools/benchmark_weaknet_netem.sh`, and it still measures the
  transport floor, not the engine.
* **It does not certify the codec path.** The soak carries audio; the
  video path's soak evidence is a separate question.

## 7. When a day goes wrong

| Symptom | What it means | What to do |
|---|---|---|
| `INCONCLUSIVE` | the harness could not be measured | read the job log; a bind failure or a crash is a *runner* problem, not an engine result |
| `FAIL` with `the receiver never decoded a frame` | the media path did not come up | check the harness's stdout in the artifact; usually an ICE failure in the harness's own setup |
| `FAIL` with `the relay dropped packets but the receiver recovered no frame` | the loss was not recovered | this is the finding the soak exists for — file it, do not re-run it away |
| `the harness did not return in time` | the run was killed | raise `--timeout-slack`, or check that the box did not sleep |

A `FAIL` is **evidence**, not a nuisance: the archive keeps every
record, so a failing run cannot be erased by a later pass.

## 8. Verifying the collector itself

```bash
python tools/run_soak_impaired.py --self-test
```

The self-test builds a synthetic evidence file and asserts that the
recorder still distinguishes `PASS` / `FAIL` / `INCONCLUSIVE`, that a
dirty-tree run is marked, and that `--expect-q5` refuses a short run. A
guard that cannot fail is a decoration, so the self-test runs in CI.
