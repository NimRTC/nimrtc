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
| [ADR-018](ADR-018-bandwidth-signal-policy.md) | Bandwidth signal policy — transport-cc preferred, loss/RTT fallback | v1.2.0 (M1) |
| [ADR-023](ADR-023-plugin-id-naming-contract.md) | Plugin ID naming contract — spelling, validation and change discipline | Phase 5 (v1.2.0) |
| [ADR-024](ADR-024-engineconfig-string-ownership.md) | `EngineConfig` string ownership — the engine owns its strings | Phase 5 (v1.2.0) |
| [ADR-025](ADR-025-sdp-rtp-not-pluggable.md) | SDP / RTP are not pluggable — and the engine's concrete includes are enumerated, not hidden | Phase 4 (v1.2.0) |
| [ADR-026](ADR-026-scheduler-c-abi.md) | The scheduler's C ABI — a real `extern "C"` surface | Phase 4 (v1.2.0) |

### Reserved numbers

| Number | Reserved for | Source |
|--------|--------------|--------|
| 016 | "SFU 转发跳过 L2" BWE-interface ADR — only to be merged if the SFU track reopens in v1.x | `docs/plan/v1.0-plan.md` §12.5 |
| 019 | Encoder ownership (D3) + ladder modes (D2) + configuration mapping (OD-3(a)) | `docs/plan/video-pipeline-v2-plan.md` §B.7.3 (G2) |
| 020 | Codec plug-in architecture (R1), hardware-first selection (R2 / OD-2), no-bundled-codec posture (OD-1) | `docs/plan/video-pipeline-v2-plan.md` §B.7.3 (G3) |
| 021 | Codec bundling posture — permissive vendoring, direct vendor-SDK hardware | `docs/plan/remote-desktop-plan.md` (K1) |
| 022 | Screen content is a content type, not a camera | `docs/plan/remote-desktop-plan.md` (K2) |

> **Why 019–022 are reserved, not free.** The Phase 4 / Phase 5 remediation
> ADRs were originally written as ADR-019 … ADR-022 and were renumbered to
> ADR-023 … ADR-026 on 2026-11-13: the numbers 019–022 were already claimed
> by the video / remote-desktop track — `main`'s `video-pipeline-v2-plan.md`
> and `remote-desktop-plan.md` assign them, and `feature/m1c-platform-capture`
> carries committed `ADR-021-codec-bundling-posture.md` and
> `ADR-022-screen-content-as-content-type.md`. Two ADRs with the same number
> and different subjects is not a merge conflict, it is a governance failure:
> a reader who follows "ADR-021" gets whichever file their branch happens
> to have.
>
> The rule this establishes: **an ADR number is claimed the moment a plan
> document assigns it**, not when the ADR file is written. Check this table
> before drafting an ADR.

## Process

1. Propose a decision in a GitHub Issue
2. Open a PR that adds `docs/adr/ADR-NNN-title.md`
3. Require at least one approving review from a module owner
4. Merge to record the decision (ADRs are append-only in P0/P1)
