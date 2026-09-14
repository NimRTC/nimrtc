# ADR-011: 遥操作指标口径 — engine 单跳预算，端到端由集成方负责

| | |
|---|---|
| Number | 011 |
| Status | **Accepted** |
| Date | 2026-09-14 |
| Phase | P1.1 |
| Bound to | v0.10.0 |
| Related | `docs/zh/architecture.md` §8.1, §14 |

## Context

v0.9 留下一个开放问题：**遥操作指标口径**——是否以 DB31/T 1505—2024、
T/SSITS 2003—2023 的端到端数值作为交付验收参考（引擎侧只背
"单跳预算"，见 `architecture.md` §8.1）？本 ADR 收口该问题。

This is the only remaining undecided question about teleop metrics
calibration. §8.1 (the technical document's "遥操作场景与外部标准" section)
already explicitly states:

> **诚实边界**：NimRTC 引擎 **只背"单跳预算"** —— 从 sender capture
> timestamp 到 receiver render timestamp 的单跳端到端延迟，由引擎
> 提供测量原语（RTCP-SR NTP↔RTP ts 映射 + frame capture ts），并
> **在 §14 给出本地精度承诺**。**跨端精度、网络规划、操控室与车端时钟
> 校准、链路冗余策略等，由集成方按其验收标准负责**——NimRTC 不背
> 端到端 E2E 数值。

So the architecture already pins the right discipline: NimRTC owns
single-hop budget + measurement primitives; **end-to-end acceptance
references** (DB31/T 1505—2024 for industrial teleop, T/SSITS 2003—2023
for ITS teleop) belong to the integrator who controls the network and
the room-side clock calibration.

The remaining decision is whether to **codify** this in writing (which
this ADR does) or leave it implicit (current state).

## Decision

1. **Engine backs only single-hop budget.** Defined as: sender capture
   timestamp → receiver render timestamp on the local engine's view of
   the world. Quantification lives in §14 (待实测后填入) — placeholder
   rows already exist; v0.10.0 release notes will not invent numbers.

2. **End-to-end acceptance references are the integrator's
   responsibility.** NimRTC explicitly does not commit to any
   end-to-end figure. The two commonly-cited references:

   - **DB31/T 1505—2024**（上海地标，工业遥操作）
   - **T/SSITS 2003—2023**（团体标准，智能交通系统遥操作）

   are documented in §8.1 as **reference standards an integrator may
   adopt**, not as acceptance gates for NimRTC releases.

3. **Engine measurement primitives** (RTCP-SR NTP↔RTP ts mapping, frame
   capture ts, render age) are the API surface that lets integrators
   compute their own end-to-end figures. These are listed in §8.1 and
   remain a NimRTC-owned deliverable.

4. **No new code** ships under this ADR. §8.1 wording update is in
   `architecture.md` already; this ADR records the decision so §8.1 / §14
   have a stable cross-reference.

5. **Future end-to-end benchmarks** (if any contributor wants to publish
   one) must be framed as **integrator-side reference numbers**, not
   as NimRTC acceptance criteria. README and any published benchmarks
   must carry the "experimental / integrator reference" disclaimer.

## Consequences

### Positive

- Codifies single-hop ownership with explicit reference standards.
  Removes the implicit "engine maybe backs E2E" reading that some
  external readers might pick up from §13 P3 wording.
- Preserves the §11.1 discipline: "P1–P3 README 一律
  experimental / not for production" — no number we publish is an
  acceptance gate we have to defend.
- Integrators get a clear hand-off point: read §8.1, use the listed
  primitives, validate against the standards they care about.

### Negative / risks

- Some prospective integrators may expect an "engine E2E number" on the
  README. This is mitigated by §16.2 README 6 段式 wording, which puts
  "Experimental — Not for production" in the Hero 区.

### Neutral

- No code change, no new module, no new public API.
- §14 rows remain "【待实测】" placeholders.

## Verification

| Check | Where | Required |
|---|---|---|
| §8.1 wording reflects single-hop ownership | `docs/zh/architecture.md` §8.1 | yes |
| README 6 段式 Hero 区 still carries experimental disclaimer | README.md | yes |
| No new "E2E acceptance" claims in v0.10.0 release notes | release notes (when authored) | yes |

## Out of scope (this ADR)

- §14 quantification (待实测; not invented by this ADR).
- DB31/T 1505 / T/SSITS 2003 implementation guidance — integrator
  responsibility, not in this codebase.
- Cross-host clock calibration APIs (a possible future addition;
  reopens this ADR if proposed).

## References

- `docs/zh/architecture.md` §8.1 (诚实边界原文), §14 (待实测)
- `docs/plan/v0.10-plan.md` (相关 work item)
