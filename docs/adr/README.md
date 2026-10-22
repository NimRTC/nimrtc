# docs/adr/

Architectural Decision Records (ADRs).

Each ADR captures a significant technical decision: what was decided, why,
what alternatives were considered, and what the consequences are.

## Format

ADRs follow the lightweight format described in:
https://github.com/joelparkerhenderson/architecture-decision-record

Filename convention: `ADR-<number>-<short-title>.md`

## When to write an ADR

Write an ADR when a decision:
- Affects more than one module
- Introduces a new external dependency
- Changes an existing public API
- Has significant implications for performance or security
- Was reached after meaningful debate among contributors

## Current ADRs

| Number | Title | Phase |
|--------|-------|-------|
| [ADR-001](ADR-001-plugin-system.md) | Pluggable Module Architecture | P0 |
| [ADR-008](ADR-008-pcm-tap.md) | PCM audio tap insertion point | P1 |
| [ADR-009](ADR-009-pal-slice-1.md) | PAL Slice 1 — engine plugin resolver seam | P1.1 |
| [ADR-010](ADR-010-profile-json-format.md) | JSON as first-class declarative Profile format | P1.1 |
| [ADR-011](ADR-011-teleop-metrics-caliber.md) | 遥操作指标口径 — engine 单跳预算，端到端由集成方负责 | P1.1 |
| [ADR-012](ADR-012-zh-docs-layout.md) | 中文文档布局 — `docs/zh/` 子目录先行，独立站点延后 | P1.1 |
| [ADR-013](ADR-013-gmssl-dtls-backend.md) | GMSSL — 国密 DTLS 后端 | P1 |
| [ADR-014](ADR-014-web-transport-vendor.md) | WebTransport / QUIC vendor — msquic | P3 |
| [ADR-015](ADR-015-api-stability-policy.md) | NimRTC API Stability Policy | P2 |
| [ADR-017](ADR-017-cross-end-timeline.md) | Cross-end frame timeline — WebRTC abs-send-time + RTCP-SR anchoring | P2 |

### Reserved numbers

| Number | Reserved for | Source |
|--------|--------------|--------|
| 016 | "SFU 转发跳过 L2" BWE-interface ADR — only to be merged if the SFU track reopens in v1.x | `docs/plan/v1.0-plan.md` §12.5 |

## Process

1. Propose a decision in a GitHub Issue
2. Open a PR that adds `docs/adr/ADR-NNN-title.md`
3. Require at least one approving review from a module owner
4. Merge to record the decision (ADRs are append-only in P0/P1)
