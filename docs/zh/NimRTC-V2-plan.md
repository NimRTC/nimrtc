# NimRTC v2 — 架构文档（旧路径，已迁移）

| | |
|---|---|
| 文件 | `docs/zh/NimRTC-V2-技术文档.md` |
| 状态 | **Deprecated** — 自 v0.10 起 canonical 已迁移到 [`docs/zh/architecture.md`](./architecture.md) |

> **本文件已迁移。** v0.10 起的中文架构文档 canonical 在
> [`docs/zh/architecture.md`](./architecture.md)；本 stub 保留仅为
> 兼容外部链接（外部博客、issue 引用、归档邮件），不在公开仓库继续
> 维护。
>
> 迁移决策与背景见 `docs/adr/ADR-012-zh-docs-layout.md`
> （"中文文档布局 — `docs/zh/` 子目录先行，单文件架构文档，
> 独立站点延后"）。

---

## 为什么保留 stub 而不是删除

- **外部链接兜底**：指向旧路径的链接需要可解析的目标，而不是 404。
- **git history 保留**：`git log --follow` 仍可追溯到原 v0.5–v0.9
  的全部修订，对回溯历史决策有用。
- **避免双入口歧义**：本 stub 明确标 "Deprecated"，区别于平级副本；
  canonical 始终是 [`docs/zh/architecture.md`](./architecture.md)。

---

如果你**是新读者**，请直接访问 [`docs/zh/architecture.md`](./architecture.md)。
如果你**通过外部链接到达这里**，上面的链接会带你到正确的 v0.10
架构文档。
