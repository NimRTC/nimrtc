# ADR-012: 中文文档布局 — `docs/zh/` 子目录先行，单文件架构文档，独立站点延后

| | | |
|---|---|---|
| Number | 012 |
| Status | **Accepted (amended 2026-09-14 for path)** |
| Date | 2026-09-14 |
| Phase | P1.1 |
| Bound to | v0.10.0 |
| Related | `docs/zh/architecture.md` §16.5 (双语策略), §13 (P1 README 镜像), ADR-009 / ADR-010 / ADR-011 |

## Context

v0.9 留下一个开放问题：**中文文档形态**——`docs/zh-hans/` 子目录 vs
独立站点（docs-zh.nimrtc.dev）？影响 §16.5 双语维护成本与 SEO 策略。

v0.9.2 状态（重写前快照）：

- `docs/zh/NimRTC-V2-技术文档.md` 单文件 ~1300 行（唯一的技术文档）。
- `README.zh-CN.md` 暂未写（§13 P1 DoD 中提到，但未开工）。
- `docs/zh-hans/` 不存在（仅作为候选名被提及）。
- 没有任何独立 docs 站点工具（Docusaurus / MkDocs / mdBook）。

两条候选路径的取舍：

| 选项 | 优点 | 缺点 |
|---|---|---|
| `docs/zh-hans/` 子目录 | 维护成本最低；与现有布局一致；无新工具链；git-native diff / review | 与 `docs/zh/` 路径冲突（已有文件占名）；未来若启用独立站点需要再迁移 |
| 独立站点 (docs-zh.nimrtc.dev) | SEO 友好；可独立部署；与英文 docs 站点对齐 | 资源受限项目维护成本翻倍；引入 docs 框架（Docusaurus / VitePress）；CI 多一份构建 |

§16.5 已确立 "英文优先，中文作为内部与中文社区补充"。这条隐含
**英文 README / docs 站点是 canonical，中文是 supplementary**——独立
中文站点会反转这个优先级而没有充分理由。

## Decision

1. **`docs/zh/` 作为中文 canonical 目录。** v0.9 的单文件被**替换**
   （非 `git mv`——v0.10 公开版发布时同步剔除不公开内容）为一个新文件
   `docs/zh/architecture.md`。后续中文文档都进 `docs/zh/`：

   ```
   docs/
   ├── zh/
   │   ├── architecture.md           (canonical 中文架构文档, v0.10)
   │   └── README.md                 (中文入口 — deferred)
   ├── adr/                          (English ADRs — canonical)
   ├── api/                          (Doxygen 输出, 语言中立)
   └── plan/                         (English 规划文档 — canonical)
   ```

   **为什么不是 `architecture/V2.md` 子目录**：v0.10 的 reorganisation
   是「剔除内部内容」，不是「拆开公开内容」。公开侧最干净的形态就是单
   文件；要触发分拆看下面的 "When to revisit"。

2. **独立站点（docs-zh.nimrtc.dev）延后到 v1.0+。** 理由：
   - 资源受限基线（§16.5）撑不住两套 build pipeline 与两套 SEO 表面。
   - pre-v1.0 无 SEO 收益——`docs/zh/` 已经被 Baidu / 必应 / Google 全索引，无须额外基建。
   - 若 v1.0+ 真出现需要独立站点的受众，到时 reopen 本 ADR。

3. **README.zh-CN.md 是 v1.0 DoD 硬性要求**（§13 P1 row："中文镜像
   `README.zh-CN.md` 同步"）。撰写工作 deferred 到 v0.11.0 / v0.12.0
   —— v0.10 资源集中在 §2.6 Profile 库与 DataChannel interop。

4. **`docs/zh-hans/` 被否。** 用 `docs/zh/` 以保留既有 path 习惯，避免
   中划线命名漂移。

5. **旧文件 `docs/zh/NimRTC-V2-技术文档.md` 被 stub 替换**——一行 redirect
   指向新路径。v0.9 文件里不公开内容（商业化 / 内部待决）走内部仓；公开
   子集在 `architecture.md` 中重新整理，§X.Y 编号相应平移。

## When to revisit（分拆阈值）

`docs/zh/architecture.md` 当前 ~1080 行，以单文件形式被接受。出现**任一
条件**时再考虑分拆：

| 触发条件 | 当前状态 |
|---|---|
| 文件总量 > **~2000 行**（约当前 1.5x） | 1080 ✓ 未触发 |
| 单个 subsection 自成密集参考（如 §11 vendor 战略占总量 > 30%） | §11 ~190 行 / 1080 ≈ 18% ✓ 未触发 |
| 因其他原因引入 docs 框架（Docusaurus / MkDocs / mdBook / VitePress） | 未引入 ✓ |

分拆的窄候选（未来如触发）：

- **§11 vendor 战略**（§11.5 / §11.6 / §11.7 / §11.8 密度最高）→ 抽到 `docs/zh/vendor-strategy.md`，§11.1–§11.4（实验性声明 / 矩阵 / 专利 / 商标）留在主文档。
- **§16 开源策略**（~280 行）→ 抽到 `docs/zh/open-source-strategy.md`，独立站点决策附在该 doc。

**v0.10 不做上述分拆。** 阈值记到此 ADR 里，下一个追这条 ADR 的人会一
并看到「分拆是有意识的 not-yet」。

## Consequences

### Positive

- 给出与资源约束匹配的明确低成本选择。
- `docs/zh/` 成为可发现、可索引的中文 canonical home，不再只是孤立单文件。
- 无新工具链；无新 CI 流水线。
- 不公开内容与公开内容分离清晰；公开仓库只发 GitHub 该看见的子集。

### Negative / risks

- 任何外部链接指向 `docs/zh/NimRTC-V2-技术文档.md`（例如外部博客）会断。
  缓解：旧路径留下一行 redirect note 指向新路径。
- v0.10 的架构文档是**重写**而非**改路径**——可以预见 v0.9.2 → v0.10
  之间有内容漂移（section 移除、重编号、对外措辞微调）。这是预期内的。
- 若 v1.0+ 真需要 docs-zh.nimrtc.dev，分拆成本延后到那时。可接受：成本
  由 ADR amendment 兜底，不会出 surprise 重新架构。

### Neutral

- 无代码改动。
- 无新依赖。
- 无 public API 改动。

## Verification

| Check | Where | Required |
|---|---|---|
| `docs/zh/architecture.md` 存在并作为 v0.10 canonical | 新文件 | yes |
| 旧路径被 stub 替换（一行 redirect note） | `docs/zh/NimRTC-V2-技术文档.md` | yes |
| 没有引入 docs 框架 / 工具链 | review | zero new tools |
| 源码 / cmake / docs 中无 `NimRTC-V2-技术文档` 残留（除 stub 文件自身） | `git grep NimRTC-V2-技术文档` | 仅 stub 一处 |

## Out of scope (this ADR)

- 拆分 `architecture.md` 为多个文件——按 "When to revisit" 表 defer。
- `README.zh-CN.md` 镜像——v0.11.0 / v0.12.0。
- 任何 docs 框架（Docusaurus / MkDocs / mdBook / VitePress）——不在 v0.10.0。
- v0.10 公开版剔除的内部内容（商业化 / 内部待决 / 附录 等）——其归宿
  是内部仓，不在本 ADR 范围。

## References

- `docs/zh/architecture.md` §16.5 (双语策略), §13 (P1 README 镜像)
- `docs/plan/v0.10-plan.md` (v0.10 计划 — 相关 work items)
- ADR-009 / ADR-010 / ADR-011 (P1.1 batch 同源决策)
- ADRs are append-only；本 ADR 仅就 "路径细化（`architecture/V2.md` →
  `architecture.md`）+ 阈值段" 在 v0.10 计划执行时同步修订，无实质内容
  变更即不视为 supersede。

## Amendment log

- **2026-09-14**: 决策 1 中路径由 `docs/zh/architecture/V2.md` 改为
  `docs/zh/architecture.md`（单文件更紧凑，符合 v0.10 reorganisation
  的最小变更原则）。新增 "When to revisit" 段，将分拆阈值（~2000 行）
  与窄候选明确写入。其他决策不变。
