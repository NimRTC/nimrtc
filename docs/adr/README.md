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
| [ADR-021](ADR-021-codec-bundling-posture.md) | Codec bundling posture — permissive vendoring, direct vendor-SDK hardware (amends the video plan's OD-1) | M1 (v1.3.0) |
| [ADR-022](ADR-022-screen-content-as-content-type.md) | Screen content is a content type, not a camera | M1 (v1.3.0) |

| [ADR-021](ADR-021-codec-bundling-posture.md) | Codec bundling posture — permissive vendoring, direct vendor-SDK hardware | v1.3.0 (M1) |
| [ADR-022](ADR-022-screen-content-as-content-type.md) | Screen content is a content type, not a camera | v1.3.0 (M1) |
| [ADR-023](ADR-023-plugin-id-naming-contract.md) | Plugin ID naming contract — spelling, validation and change discipline | Phase 5 (v1.2.0) |
| [ADR-024](ADR-024-engineconfig-string-ownership.md) | `EngineConfig` string ownership — the engine owns its strings | Phase 5 (v1.2.0) |
| [ADR-025](ADR-025-sdp-rtp-not-pluggable.md) | SDP / RTP are not pluggable — and the engine's concrete includes are enumerated, not hidden | Phase 4 (v1.2.0) |
| [ADR-026](ADR-026-scheduler-c-abi.md) | The scheduler's C ABI — a real `extern "C"` surface | Phase 4 (v1.2.0) |
| [ADR-027](ADR-027-turn-over-tls-libjuice-seam.md) | TURN over TLS — a narrow TLS seam in the vendored libjuice, and no plaintext fallback | v1.4 (N1) |
| [ADR-028](ADR-028-ice-over-tcp-nimrtc-side.md) | ICE-over-TCP — a NimRTC-side TCP candidate layer, and libjuice stays UDP-only | v1.4 (N2) |
| [ADR-029](ADR-029-engine-driven-audio-device-layer.md) | 引擎驱动音频设备层 — 音频设备的注册表槽位与引擎侧设备泵 | v1.4 (D1) |
| [ADR-030](ADR-030-engine-driven-video-renderer.md) | 引擎驱动视频渲染器 — `video_sink_name = "auto"` 的平台解析与降级 | v1.4 (D2) |
| [ADR-031](ADR-031-ice-recovery-policy-ownership.md) | ICE 恢复策略的归属 — 重连状态机在引擎侧 | v1.4 (N3) |
| [ADR-032](ADR-032-turn-credentials-provider-seam.md) | TURN 凭据提供方接缝 — 引擎主动拉取而非宿主静态配置 | v1.4 (N7) |
| [ADR-033](ADR-033-path-change-reporting.md) | 路径变更上报 — 旧 pair / 新 pair / 原因 | v1.4 (N8) |
| [ADR-034](ADR-034-macos-install-downstream-proof.md) | macOS 安装树的下游证明 — `find_package` 在三个平台同样成立 | v1.4 (N9) |
| [ADR-035](ADR-035-call-state-surface.md) | 通话状态面 — `on_call_state` 与失败枚举 | v1.4 (D7) |
| [ADR-036](ADR-036-sdk-sample-line-budget.md) | SDK 样例的行数预算 — 一个能跑通两进程的最小样例 | v1.4 (N6) |
| [ADR-037](ADR-037-turn-tls-trust-seam.md) | TURN/TLS 的信任策略接缝 | v1.4 (N1) |
| [ADR-038](ADR-038-install-tree-video-dependencies.md) | 安装树的视频依赖闭包 | v1.4 (N9) |
| [ADR-039](ADR-039-viewport-window-opt-in.md) | 渲染器自建窗口是显式 opt-in（修订 ADR-030 决策三） | v1.4 (D2) |
| [ADR-040](ADR-040-audio-device-plugin-registry.md) | 音频设备后端并入 PluginRegistry — 设备接缝与其它接缝同构（原 ADR-028，撞号后重编） | v1.4 (D1) |
| [ADR-041](ADR-041-install-tree-module-closure.md) | 安装树链接闭包 — 「装好的 SDK 永远链得上」 | v1.4 (N9) |

### Reserved numbers

| Number | Reserved for | Source |
|--------|--------------|--------|
| 016 | "SFU 转发跳过 L2" BWE-interface ADR — only to be merged if the SFU track reopens in v1.x | `docs/plan/v1.0-plan.md` §12.5 |
| 019 | Encoder ownership (D3) + ladder modes (D2) + configuration mapping (OD-3(a)) | `docs/plan/video-pipeline-v2-plan.md` §B.7.3 (G2) |
| 020 | Codec plug-in architecture (R1), hardware-first selection (R2 / OD-2), no-bundled-codec posture (OD-1) | `docs/plan/video-pipeline-v2-plan.md` §B.7.3 (G3) |

> **Why 019–020 are reserved, not free.** The Phase 4 / Phase 5 remediation
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
> ADR-021 and ADR-022 have since been written on `docs/architecture-guards`
> and are listed above; only 019 and 020 remain reserved.
>
> The rule this establishes: **an ADR number is claimed the moment a plan
> document assigns it**, not when the ADR file is written. Check this table
> before drafting an ADR.

## Process

1. Propose a decision in a GitHub Issue
2. Open a PR that adds `docs/adr/ADR-NNN-title.md`
3. Require at least one approving review from a module owner
4. Merge to record the decision (ADRs are append-only in P0/P1)
