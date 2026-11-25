# ADR-076: 共享写集判据的签收边界 —— 「本片的提交算到哪儿为止」

| | |
|---|---|
| Number | 076 |
| Status | **Proposed** |
| Date | 2026-11-25 |
| Phase | v1.4（验收判据） |
| Bound to | v1.4.0 |
| Supersedes | — |
| Related | `docs/adr/ADR-075-*.md`（产物检查状态判据，本片是其边界补丁）；`tests/acceptance/v5-writeset-acceptance/writeset-boundary.cjs`（共享判据本体）；`specs/slices/v6-writeset-slice-boundary.md` |

---

## 1. 背景

共享写集判据（`tests/acceptance/v5-writeset-acceptance/writeset-boundary.cjs`）
认「本片的提交」只凭两样东西：

- 提交标题是 `feat(<id>): slice approved`；**或**
- 提交带 `Refs-Slice: <id>` trailer。

两者都**不看提交发生在签收之前还是之后**。

于是：签收**之后**写的**事务性**提交（本项目的普遍习惯是 `docs(plan): X is signed`）
如果误带了 `Refs-Slice: <id>` trailer，它的文件就会被算进本片的 `changed`。
那个文件通常**不在本片写集里** → 判据报**假越界**。

N6（`v1-sdk-sample`）的验收套件真实撞出这条越界：

```
AssertionError: 改动超出写集：    docs/plan/v1.4-plan.md
```

但 N6 **从来没碰过** `docs/plan/v1.4-plan.md`。查历史发现那是
`cdf25813 docs(plan): v1.4 -- all sixteen items signed ...` —— **签收之后**
的事务性计划文档提交，误带 `Refs-Slice: v1-sdk-sample` trailer。

这不是「个别提交写错了 trailer」—— 是**判据缺一条边界**。只要「事务性提交」
存在，只要有人误带 trailer，越界就会再发生。改提交只是补这次的洞，判据的
洞还在。

扫全部验收套件，**6 个**已签字切片被这条规则误伤：`v1-sdk-sample` /
`v1-ice-recovery` / `v1-path-change-report` / `v1-turn-credentials` /
`v1-video-render-engine` / `v1-turn-tls-trust`。

## 2. 决策

### 2.1 决策一：加一条「签收边界」

一条提交算不算「本片的工作」，规则从「带 trailer」改成「带 trailer **且**
在本片的签收边界之内」。

**签收边界**定义为：本片的**合并提交**（`Merge slice <id>`）或**签收提交**
（`feat(<id>): slice approved`）—— 该提交及其**全部祖先**。

- 提交在边界之内（是边界的祖先或就是边界本身）→ 算本片的工作。
- 提交在边界之外（签收之后才发生）→ **不算**。

### 2.2 决策二：边界怎么定 —— fallback 阶梯

有些切片的历史里没有 `Merge slice <id>` 也没有 `feat(<id>): slice approved`
（例如 `v1-audio-device-engine` 的工作提交是
`feat(v1-audio-device-engine): engine drives the audio device layer`）。
所以边界的查找必须有**确定的 fallback 阶梯**：

1. `Merge slice <id>` 提交（合并提交）；
2. `feat(<id>): slice approved` 提交（签收提交）；
3. `feat(<id>): ...` 提交（**任意** `feat` 标题的本片提交，取**最早**的那个）。

为什么阶梯 3 取「最早」：一个切片可能有多次 `feat` 提交（迭代开发）。
最早的那个是「这片从哪儿开始」的锚点 —— 边界应该包含全部工作，不该切在
中途。

三步都找不到 → **边界为 null** → 判据退回当前行为（不按边界过滤），
并**保持检查绿**。**不能因为找不到边界就判红** —— 那会让没有合并提交的
旧切片（如 `v1-video-processing`，工作被 squash 进大提交）全部失败。

### 2.3 决策三：过滤只发生在「来源 2：本片自己的提交」这一路

判据的改动清单有三个来源：

1. 工作区未提交差异（`git diff HEAD --name-only`）；
2. 本片自己的提交（带 marker 的提交，逐个 `git diff --name-only`）；
3. 未跟踪文件（`git ls-files --others`）。

过滤**只**加在来源 2：本片的提交只算签收边界之内的。

为什么只在这一处：

- 来源 1 与来源 3 **天然是「当前会话」** 的 —— 它们不属于任何历史提交，
  不该被签收边界过滤。
- 来源 2 是唯一会「把签收之后的事务提交算进来」的一路。
- 过滤在这一处做，**下面所有调用方**（`evaluateChanged` /
  `evaluateSliceWriteSet` / 各套件的边界断言）**自动受益**，不需要改。

### 2.4 决策四：祖先集合用 `git rev-list` 一次进程算

边界提交的祖先集合 = `git rev-list <boundary>`（一次 git 进程，返回边界及
全部祖先）。与「逐提交走父链」的集合相同（同一个 commit graph），但只需
**一次**进程启动 —— 父链遍历在一次 git log 之后就是纯内存集合判定。

为什么不用 `git merge-base --is-ancestor`：后者对每个候选提交要起一次 git
进程（17 个切片 × 多个候选 = 上百次进程启动，实测 8 分钟）。`git rev-list`
把整个可达集合一次拿回，判据对每个切片只有一次进程开销。

## 3. 为什么这个规则是对的（而不是「又一个白名单」）

**为什么不用路径白名单**：白名单要维护「哪些文件是事务性文档」，新来一个
事务性提交照样误伤 —— 那只是把这次的洞补上，不是把洞堵住。边界规则只看
提交时序与标题约定，**不需要知道任何具体路径**。

- **它区分的是「本片的工作」与「本片之后的事务」** —— 这两件事本来就不同，
  判据之前没区分。
- **它不削弱越界检查** —— 本片**签收之前**改到写集外的文件，照样被拦下。
  真越界发生在签收之前（那是 agent 干活的时候）。
- **它不需要知道任何具体路径** —— 不需要维护「哪些文件是事务性文档」的
  清单。新来一个事务性提交不会再次触发误伤。
- **它不需要改历史** —— 已合并的提交原样保留，只是判据不再把它们算进来。

本片**不做**的三件事：

- **不做** 改写已合并的历史提交（改历史，不是修判据）。
- **不做** 在各套件的 allow 清单里加事务性文档路径（把误伤合法化，漏洞还在）。
- **不做** 放宽越界检查本身（真越界必须继续被拦）。

## 4. 后果

- 6 个被误伤切片的写集边界断言**重新变绿**。
- 边界为 null 的切片（找不到边界）行为**不变**（不更差）。
- 越界检查的**强度不变**：签收之前的越界照旧被拦；签收之后的事务性提交
  不再算本片。
- 判据是**确定性**的（同一棵树、同样的提交历史，重复跑结果一致），
  且不触网、不调模型。

## 5. 验证

- `tests/acceptance/v6-writeset-slice-boundary/acceptance.test.cjs` 断言
  六个切片的边界（Merge / approved / 最早 feat / null），断言事务性提交被
  过滤、真实工作不被滤掉、越界检查不削弱、判据确定性。
- `tests/acceptance/v5-writeset-acceptance/acceptance.test.cjs` 的三源并集 /
  越界检测用例改为「边界之内」的构造，并新增边界阶梯断言。
