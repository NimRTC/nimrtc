# v1.4 Release Status — why there is no `v1.4.0` tag yet

> **This page exists to stop a specific misreading.**
> `docs/plan/v1.4-plan.md` §0a reports every one of the sixteen items as
> signed, and `scripts/check_v14_plan_status.py` exits 0.  Neither says
> anything about *release*.  The `v1.4.0` tag is **held by owner
> decision** — see §11 of the plan.  Anything that reads plan completion
> as release readiness is wrong, and this page is where that is written
> down outside the plan so it can be found without reading §11.

| | |
|---|---|
| **`v1.4.0` tag** | **not cut — held** (owner decision, 2026-11-26) |
| **Current git tags matching `v1.4*`** | none |
| **Released line** | `v1.3.1` (the last tag on this repository) |
| **`version/v3` branch** | still receiving merges — the hold is on the *tag*, not the branch |

## The three gates (all must hold)

From `docs/plan/v1.4-plan.md` §11.2.  They are independent.

| Gate | Requirement | Status |
|---|---|---|
| **A** | The supplementary items (§11.4, A1–A8) are developed, each via a frozen slice merged into `version/v3` | open — enumerated 2026-11-26, **no slice started** |
| **B** | M3's evidence exists (real-device real-NAT D7 call; archived two-machine run; no E/S/Q regression) | open — two items still open |
| **C** | `RELEASING.md` §3.3 + `scripts/check-release-freeze.sh` pass | not started — M2's seven-day window makes the earliest cut **2026-12-08** |

## What this page is *not* saying

It does **not** say the work is unfinished.  The sixteen planned items are
signed and merged; that is recorded in §0a and in the slice event
streams.  The hold is a *release* decision, not a *completion* verdict.

## The zelto state, and why it says `delivered`

The zelto version plan (`plan-1795166129234`) marks `v3` as **`delivered`**
with a `version.delivered` event at **2026-11-25T14:43:23Z**.

That event predates the hold by roughly ten hours:  the hold decision
(`05e2bb36`) landed on `version/v3` at 2026-11-26 08:53 +0800, and the
ship ran at 2026-11-25 22:43 +0800.  At the time the event was written,
no decision existed that said "do not ship".

**The state is kept, not rewritten.**  C4 makes the event stream
append-only, and the event tells the truth about what happened: `v3`'s
content did reach `feat/v14-turn-tls` (merge commit `3365b29b`), with
`build` and `test` both green on the target.  Rewriting it to
`integrated` would make the record say something that did not happen.

What the zelto state does **not** express, and cannot without a new event
type, is "delivered to trunk, but the release tag is held".  zelto's
`VersionStatus` has four values (`planned` / `in_progress` / `integrated`
/ `delivered`); none of them means "released".  So `delivered` here means
*shipped to trunk*, not *released*.  This page is the record of the
difference.

### The zelto version tag

`plan-1795166129234/v3` is zelto's **version address** (ADR-110 —
namespaced by plan), not a release tag.  It points at `a2bb408c`, the
`version/v3` tip at the moment of that ship.  It is deliberately *not*
re-pointed at later tips: it records which commit was shipped, the same
way the event does.

## If you are about to cut a tag

Read §11.3 of the plan first.  The failure mode it guards against is
exactly the one a reader who runs a green script and stops thinking will
walk into.  Lifting the hold is an owner decision recorded in §11 — not
an inference from any script exit code.
