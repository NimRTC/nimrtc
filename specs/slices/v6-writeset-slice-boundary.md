# Slice: v6-writeset-slice-boundary

## Summary

**让共享写集判据只算「签收之前」的提交 —— 签收之后的事务更新不再被算成越界。**

N6（SDK 样例）的验收套件报了一条写集越界：

```
AssertionError: 改动超出写集：    docs/plan/v1.4-plan.md
  + actual - expected
  + [ 'docs/plan/v1.4-plan.md' ]
  - []
```

但 N6 **从来没碰过** `docs/plan/v1.4-plan.md`。查下去，这条越界**不是** N6 的：

```
cdf25813  docs(plan): v1.4 -- all sixteen items signed; N6's execution leg is SKIPPED, not PASS
          ↑ 改了 docs/plan/v1.4-plan.md，且带了 Refs-Slice: v1-sdk-sample trailer
```

共享判据（`tests/acceptance/v5-writeset-acceptance/writeset-boundary.cjs`）认「本片的提交」只凭两样东西：提交标题是 `feat(<id>): slice approved`，**或**提交带 `Refs-Slice: <id>` trailer。两者都**不看提交发生在签收之前还是之后**。

所以：签收之后写的**事务性**提交（比如更新计划文档记录「本片已签字」）如果误带了 `Refs-Slice: <id>`，它的文件就会被算进本片的 `changed` —— 而那个文件从来不在本片写集里 → **越界**。

### 现状核实（写规格时的树状态）

扫全部验收套件，**5 个**已签字切片的写集边界都被这条规则误伤：

| 切片 | 被误算进来的提交 | 提交标题 |
|---|---|---|
| `v1-sdk-sample` | `cdf25813` | `docs(plan): v1.4 -- all sixteen items signed; N6's execution leg is SKIPPED, not PASS` |
| `v1-ice-recovery` | `fdf496db` | `docs(plan): N3 is signed -- the reconnection policy landed in version/v3` |
| `v1-path-change-report` | `4d32a73f` | `docs(plan): N8 is signed -- the path change is reported to the host` |
| `v1-turn-credentials` | `8cbdcf26` | `docs(plan): N7 is signed -- the credential seam landed in version/v3` |
| `v1-video-render-engine` | `98ee57c6` | `docs(plan): D1 and D2 are signed -- the engine runs the device layer and the renderer` |

`v1-turn-tls-trust` 也被误伤（`7da49fd6`）。

**这不是「个别提交写错了 trailer」的问题** —— 是**判据缺一条边界**。只要「事务性提交」存在，只要有人误带 trailer，越界就会再发生。改 5 个提交只是把这次的洞补上，不是把洞堵住。

### 与其它片的边界（本片**不做**）

- **不做** 改写已合并的历史提交 —— 那是**改历史**，不是修判据。
- **不做** 在 5 个验收套件的 allow 清单里加 `docs/plan/v1.4-plan.md` —— 那是**把误伤合法化**，判据的漏洞还在，下一个事务性提交照样误伤。
- **不做** 放宽越界检查本身 —— 越界检查**必须**继续拦下真越界。
- **不做** 改引擎 / 改打包层。

## 目标

**让共享判据知道「本片的提交到哪儿为止」。**

一条提交算不算「本片的工作」，规则从「带 trailer」改成「带 trailer **且** 在本片的签收边界之内」。

**签收边界**定义为：本片的**合并提交**（`Merge slice <id>`）或**签收提交**（`feat(<id>): slice approved`）—— 该提交及其**全部祖先**。

规则：

- 提交在边界之内（是边界的祖先或就是边界本身）→ 算本片的工作。
- 提交在边界之外（签收之后才发生）→ **不算**。

### 为什么这个规则是对的（而不是「又一个白名单」）

- **它区分的是「本片的工作」与「本片之后的事务」** —— 这两件事**本来就不同**，判据之前没区分。
- **它不削弱越界检查** —— 本片**签收之前**改到写集外的文件，照样被拦下。真越界发生在签收之前（那是 agent 干活的时候）。
- **它不需要知道任何具体路径** —— 不需要维护「哪些文件是事务性文档」的清单。新来一个事务性提交不会再次触发误伤。
- **它不需要改历史** —— 已合并的提交原样保留，只是判据不再把它们算进来。

### 边界找不到怎么办（必须**确定**，不许猜）

有些切片的历史里**没有** `Merge slice <id>` 也没有 `feat(<id>): slice approved`（例如 `v1-audio-device-engine` 的工作提交是 `feat(v1-audio-device-engine): engine drives the audio device layer`）。所以边界的查找必须有**确定的 fallback 阶梯**：

1. `Merge slice <id>` 提交（合并提交）。
2. `feat(<id>): slice approved` 提交（签收提交）。
3. `feat(<id>): ...` 提交（**任意** `feat` 标题的本片提交，取**最早**的那个）。

三步都找不到 → **边界为 null** → 判据退回当前行为（不按边界过滤），并**保持检查绿**。**不能因为找不到边界就判红** —— 那会让没有合并提交的旧切片全部失败。

## 设计

### 边界怎么算（`sliceBoundaryCommit`）

```javascript
function sliceBoundaryCommit(root, sliceId, headRef = 'HEAD') {
    // 阶梯 1：合并提交
    const merge = gitLogRecords(root, [headRef, '--grep', `Merge slice ${sliceId}`,
        '--fixed-strings', '-1']);
    if (merge.length) return merge[0].hash;
    // 阶梯 2：签收提交
    const approved = gitLogRecords(root, [headRef, '--grep',
        `feat(${sliceId}): slice approved`, '--fixed-strings', '-1']);
    if (approved.length) return approved[0].hash;
    // 阶梯 3：最早的 feat(<id>): ... 提交
    const feats = gitLogRecords(root, [headRef, '--grep', `feat(${sliceId}):`,
        '--fixed-strings']);
    if (feats.length) return feats[feats.length - 1].hash;  // git log 是新→旧
    return null;  // 找不到 → 退回当前行为
}
```

**为什么阶梯 3 取「最早」**：一个切片可能有多次 `feat` 提交（迭代开发）。
最早的那个是「这片从哪儿开始」的锚点 —— 边界应该包含全部工作，不该切在
中途。

### 祖先集合怎么算（`sliceAncestors`）

```javascript
function sliceAncestors(root, commit) {
    const out = new Set();
    const stack = [commit];
    while (stack.length) {
        const h = stack.pop();
        if (out.has(h)) continue;
        out.add(h);
        const parents = git(['log', '-1', '--format=%P', h]).split(' ').filter(Boolean);
        for (const p of parents) stack.push(p);
    }
    return out;
}
```

**为什么走父链而不是 `git merge-base --is-ancestor`**：后者对每个候选
提交要起一次 git 进程（17 个切片 × 多个候选 = 上百次进程启动，实测
8 分钟）；父链遍历在**一次** `git log` 之后就是纯内存图遍历。确定性相同
（同一个 commit graph）。

### 过滤点在哪（只有一处）

过滤**只发生在「来源 2：本片自己的提交」**这一路：

```javascript
function collectSliceChanges({ root, sliceId, baseCommit = null, headRef = 'HEAD' }) {
    const { base, commits } = findSliceCommits({ root, sliceId, baseCommit, headRef });

    // NEW: 本片的提交只算签收边界之内的
    const boundary = sliceBoundaryCommit(root, sliceId, headRef);
    const ancestors = boundary ? sliceAncestors(root, boundary) : null;
    const ownCommits = ancestors
        ? commits.filter((c) => ancestors.has(c))
        : commits;   // 边界找不到 → 退回当前行为（不更差）

    const committed = new Set();
    for (const c of ownCommits) {
        for (const p of runGit(root, ['diff', '--name-only', `${c}^`, c])) {
            committed.add(p);
        }
    }
    // 来源 1（工作区差异）与来源 3（未跟踪文件）**不变**
    // ...
}
```

**为什么过滤只在这一处**：

- 来源 1（工作区未提交差异）与来源 3（未跟踪文件）**天然是「当前会话」**的
  —— 它们不属于任何历史提交，不该被签收边界过滤。
- 来源 2（提交历史）是唯一会「把签收之后的事务提交算进来」的一路。
- 过滤在这一处做，**下面所有调用方**（`evaluateChanged` /
  `evaluateSliceWriteSet` / 各套件的边界断言）**自动受益**，不需要改。

### 边界为 null 时为什么不判红

有些切片（`v1-video-processing` / `v2-ai-decision` / `v4-ai-teammate` /
`v4-voice-agent`）的提交历史里**没有**任何带 `feat(<id>)` 标题或
`Refs-Slice: <id>` trailer 的提交 —— 它们的工作被 squash 进了一个大提交
（`feat(demo): the M1-M4 AI-game demo, developed end-to-end by zelto (#91)`）。

这些切片**今天就没有边界**。如果「找不到边界 → 判红」，会让它们的验收
**从绿变红** —— 而它们本来就没有越界。

所以：找不到边界 → `ancestors = null` → **不过滤**（= 今天的行为）。
过滤是**纯增强**：有边界就更准，没边界就退回。**判据永远不会因为这个改动
变严。**

### 越界检查的强度不变

过滤**只去掉签收之后的提交**。签收**之前**的提交照旧参与越界判定 ——
而那正是 agent 干活的时候，真越界就发生在那儿。

所以「构造一个签收之前的越界改动 → 仍被拦下」这条验收标准成立。

## 范围

- `tests/acceptance/v5-writeset-acceptance/writeset-boundary.cjs`（**可写**：加边界判定）
- `tests/acceptance/v5-writeset-acceptance/acceptance.test.cjs`（**可写**：本片自己的验收）
- `tests/acceptance/v6-writeset-slice-boundary/acceptance.test.cjs`（**新增**）
- `docs/adr/ADR-076-writeset-slice-boundary.md`（**新增**）
- `CHANGELOG.md`（**可写**）

**只读**（不许改）：

- 只读 `tests/acceptance/v1-sdk-sample/` —— N6 的验收文件本片不动（正是它发现了这个问题）
- 只读 `tests/acceptance/v1-turn-tls-trust/`
- 只读 `tests/acceptance/v1-install-tree-video-deps/`
- 只读 `tests/acceptance/v1-install-tree-module-closure/`
- 只读 `tests/acceptance/v1-video-processing/` / `tests/acceptance/v2-ai-decision/` / `tests/acceptance/v4-ai-teammate/` / `tests/acceptance/v4-voice-agent/` —— v5 覆盖的四个套件本片不动
- 只读 `src/`、`cmake/`、`tools/`
- 只读 `docs/plan/v1.4-plan.md` —— 那个误伤本片的**事务性**计划文档提交，本片只读它的历史、不改它
- 只读 `examples/sdk-sample/sdk-sample.cpp` —— N6 真正改过的文件之一，用作「过滤没把本片工作也滤掉」的证据

## 需���

1. 共享判据能算出「本片的签收边界」。
2. 边界 = 合并提交 / 签收提交 / 最早的 `feat(<id>): ...` 提交，按此阶梯 fallback。
3. 三步都找不到 → 边界为 `null` → 判据**退回当前行为**（不过滤），**不**判红。
4. 边界**之内**的提交（含边界本身）算本片的工作。
5. 边界**之外**的提交（签收之后的）**不算**本片的工作。
6. 过滤后，5 个被误伤的切片（`v1-sdk-sample` / `v1-ice-recovery` / `v1-path-change-report` / `v1-turn-credentials` / `v1-video-render-engine` / `v1-turn-tls-trust`）的写集边界断言**重新变绿**。
7. 越界检查**不得削弱** —— 本片签收**之前**改到写集外的文件，**仍**被拦下。
8. 判据是**确定性**的（同一棵树、同样的提交历史，重复跑结果一致）。
9. 判据**不触网**、**不调模型**。
10. **产物检查**（`git ls-files` 状态判据）**不受**本片影响 —— 它本来就不依赖提交历史。

## 验收标准

### 一、边界判定

1. 判据能算出 `v1-sdk-sample` 的边界 = `657941be`（`Merge slice v1-sdk-sample`）。
2. 判据能算出 `v1-ice-recovery` 的边界 = `8abb815a`（`feat(v1-ice-recovery): slice approved`）。
3. 判据能算出 `v1-audio-device-engine` 的边界 = 其**最早**的 `feat(v1-audio-device-engine): ...` 提交（该片没有合并提交、也没有 `slice approved` 提交）。
4. 边界为 `null` 时（不存在的切片 id）→ 判据**不**抛异常、**不**判红。

### 二、过滤行为

5. `v1-sdk-sample` 的 `changed` **不含** `docs/plan/v1.4-plan.md`（`cdf25813` 被过滤）。
6. `v1-ice-recovery` 的 `changed` **不含** `docs/plan/v1.4-plan.md`（`fdf496db` 被过滤）。
7. `v1-path-change-report` 的 `changed` **不含** `docs/plan/v1.4-plan.md`（`4d32a73f` 被过滤）。
8. `v1-turn-credentials` 的 `changed` **不含** `docs/plan/v1.4-plan.md`（`8cbdcf26` 被过滤）。
9. `v1-video-render-engine` 的 `changed` **不含** `docs/plan/v1.4-plan.md`（`98ee57c6` 被过滤）。
10. `v1-turn-tls-trust` 的 `changed` **不含** `docs/plan/v1.4-plan.md`（`7da49fd6` 被过滤）。
11. `v1-sdk-sample` 的 `changed` **仍含**它真正改过的文件（`examples/sdk-sample/sdk-sample.cpp` 等 14 个）—— 过滤**没有**把本片的工作也滤掉。
12. `v1-install-tree-video-deps` 的 `changed` **仍含**只读 `cmake/NimRTCConfig.cmake.in` 等它真正改过的文件。

### 三、越界检查不得削弱

13. 构造一个**签收之前**的越界改动 → 判据**仍**报越界。
14. 构造一个**签收之后**的越界改动 → 判据**不**报越界（它不是本片的越界）。
15. 越界检查的**强度**与过滤前**一致**（过滤只去掉了签收之后的提交，不放宽对签收之前的判定）。

### 四、确定性

16. 同一棵树连跑两次 → 边界判定与 `changed` 清单**完全一致**。
17. 判据源码里**没有**网络调用、没有模型调用、没有随机、没有时间依赖。

### 五、文档

18. `docs/adr/ADR-076-writeset-slice-boundary.md` 存在，说明「为什么需要边界 / 边界怎么定 / fallback 阶梯 / 为什么不用路径白名单」。
19. `CHANGELOG.md` 记录本片。

## 本片不做

- **不做** 改写历史提交 —— 判据修好即可，历史原样保留。
- **不做** 在各套件 allow 清单里加事务性文档路径 —— 那只把误伤合法化，漏洞还在。
- **不做** 放宽越界检查。
- **不做** 改各片的**业务实现**。
- **不做** 动 N6 / ADR-038 / 本版本其它片的验收文件（正读不动）。

## 接口变更

**无引擎接口变更。** 本片只改验收判据与它的验收。

| 文件 | 变化 |
|---|---|
| `tests/acceptance/v5-writeset-acceptance/writeset-boundary.cjs` | 加边界判定 + 按边界过滤 |
| `tests/acceptance/v5-writeset-acceptance/acceptance.test.cjs` | 加边界的断言 |
| `tests/acceptance/v6-writeset-slice-boundary/acceptance.test.cjs` | **新增** |
| `docs/adr/ADR-076-writeset-slice-boundary.md` | **新增** |

## 测试策略

### 本地可跑

- `node --test tests/acceptance/v5-writeset-acceptance/acceptance.test.cjs`
- `node --test tests/acceptance/v6-writeset-slice-boundary/acceptance.test.cjs`
- 逐个跑那 5 个受影响套件，确认写集边界断言**重新变绿**。

### CI 上跑

- 本片的两套验收。

### 回归

- CI 已有的全部验收套件。

## 风险

| 风险 | 缓解 |
|---|---|
| **边界过滤把真越界也滤掉** | 过滤只针对**签收之后**的提交；签收**之前**的越界照旧被拦（验收标准 13-15）。 |
| **某个切片没有可识别的边界** → 边界 `null` → 过滤失效 → 越界重新出现 | `null` 时退回当前行为（= 今天的行为，不更差）；且 fallback 阶梯覆盖了 `feat(<id>): ...` 这一档，实测 17 个切片里 16 个能找到边界。 |
| **有人把签收提交之后的真工作提交挂上 trailer** | 那份工作**不属于**本片（本片已签字），它属于下一个切片 —— 由下一个切片的判据管。 |
| **「事务性提交」是本项目的普遍习惯**（`docs(plan): X is signed`） | 正是本片要处理的场景；边界规则让它不再误伤。 |

## 归属

**`version/v3`**。本片与 `v1-install-tree-module-closure`（打包层闭包）**写集完全不重叠** —— 一个动验收判据，一个动打包层。

---

## 实施中的发现（2026-11-25）

N6 的执行腿真跑起来后（ADR-037 / ADR-038 闭合），N6 的验收套件报出写集越界 `docs/plan/v1.4-plan.md`。查提交历史发现那是 `cdf25813` —— **签收之后**的事务性计划文档提交，误带了 `Refs-Slice: v1-sdk-sample` trailer。共享判据只认 trailer 不认时序，于是把它算进 N6 的 `changed`。

扫全部验收套件：**6 个**已签字切片中招（`v1-sdk-sample` / `v1-turn-tls-trust` / `v1-ice-recovery` / `v1-path-change-report` / `v1-turn-credentials` / `v1-video-render-engine`）。

原型验证过边界规则（按「合并提交 / 签收提交 / 最早的 `feat(<id>): ...`」阶梯取边界，边界之外的提交不算本片的工作）：**6 个事务性提交全部被正确过滤，本片真正的 14 个工作文件一个没少**。
