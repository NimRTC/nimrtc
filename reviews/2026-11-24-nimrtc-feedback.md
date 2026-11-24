# NimRTC v1.3.0 发版评审反馈

- **日期**：2026-11-24
- **评审对象**：`v1.3.0` 发布（tag `v1.3.0`，tag object `7ac9881f`，peeled commit `0fd6f0b8`），发布流水线 run `45123680856`
- **说明**：本文只写**可复现的事实**与**机制层面的改进项**。每条都注明证据来源，便于逐条核对。
- **上下文更新**：`v1.3.1` 已于 2026-11-24 发布，是当前最新版（补丁版，改 `.sha256` 侧车格式 + 发布页摘要表）。本文涉及的 `v1.3.0` 缺陷在 `1.3.x` 线上修复，`v1.3.0` tag 本身不动。

---

## 一、结论摘要

五条问题**全部属实**，都有可复现的公开证据。但其中三条的**落点需要修正** —— 问题不在于「谁做了什么判断」，而在于**机制允许什么**。下面逐条给出：事实 → 机制分析 → 已落地的修复。

按优先级：

| 优先级 | 问题 | 状态 |
|---|---|---|
| P0 | tag 对象内部名与 ref 不一致（`v1.3.0-staged`） | ✅ 已修（脚本 + 门禁 + 自测） |
| P0 | 门禁规则可在发版当天被改（无版本化、无冷却期） | ✅ 已修（规则版本化 + 冷却期） |
| P1 | hardware job 全红且不在任何门禁里 | ✅ 已修（新增 `hardware` 检查，final-tag 下失败） |
| P2 | `v1.2.0` 侧车行格式错，README 未说明 | ✅ 已修（README / README.zh 已加警告） |
| P2 | 发布流水线第 12 步失败后仍留半成品发布 | ⏳ 待定（见第六节） |

---

## 二、第 1 条：门禁规则在发版当天被改 —— 机制问题，不是判断问题

### 事实

| 证据 | 实测值 |
|---|---|
| #149 合入时间 | `fb56f6f9` committer date **2026-11-23T03:00:28Z** |
| 窗口规则变更 | `release.yml` `--require-days 7` → `5`，同时 `tools/run_soak.py` 的「当日任一 FAIL 即不绿」改为「每用例至少一次 PASS 即绿」 |
| 发布运行 | run `45123680856`，**05:56:20Z** published → 距合入 **2h56m** |
| 自我承认 | `v1.4-plan.md:38-44`：「**Update 2026-11-23: the window is green.** Two permanent changes to the gate closed it」；`v1.3-plan.md:1027` 记明动机是 11-20 单个 `subprocess.TimeoutExpired` |

### 判断本身站得住

11-20 那个 FAIL 是 Chrome interop 在共享 CI runner 上 60 秒超时，**同一 commit 同一天已通过 3 次**，Windows 同 commit 通过，该 PR 的 CI 13/13 全绿。「单次 FAIL 不应污染整天」这个判断是对的，**现在仍然生效**。

### 真正的问题在机制

规则是**硬编码在 `release.yml` 里的一个数字**。改它不需要独立评审、不需要版本号、不需要跨版本过渡期。于是形成了一个先例：

> **判据的持有者 = 受益者 = 规则变更在同一天生效**

这不是执行上的一次疏忽，是**机制允许**的。

### 已落地的修复：规则版本化 + 冷却期

规则现在住在 `tools/run_soak.py` 的 `GATE_RULE` 里，是一个**有名字、有版本号、有生效日期、有冷却期**的结构：

```
GATE_RULE = {
    "version": 2,
    "days": 7,                      # ← 改回 7
    "intra_day": "any-pass",        # ← 保留 11-23 的放宽
    "cooldown_days": 7,
    "effective_from": "2026-11-24",
}
```

**发布由「切版当天的规则」判定**，`--cut-date` 指定是哪一天。冷却期的含义：

| 切版日期 | 用哪条规则判定 |
|---|---|
| 变更生效**之前** | **旧**规则（变更还不存在） |
| 冷却期**之内** | **旧**规则（变更已上记录，但还没上够时间，什么都决定不了） |
| 冷却期**之后** | **新**规则 |

关键推论：**改规则的那次发布，由被它替换掉的那条规则判定。** 变更没法用来放行它本来就是为了放行的那件事。

而且 **`release.yml` 现在完全不传 `--require-days`** —— 天数是规则的事，不是发版人的事。规则变更要等一个完整窗口。

自测 40 项全绿，其中 8 项专门钉这个机制（包括「切版日 = 规则变更日 → 用旧规则」「冷却期内 → 用旧规则」「冷却期后 → 用新规则」「无法解析的切版日期 → 报错而不是默认用最新规则」）。

**实跑验证**（同一个 `release/1.3` 窗口）：

```
切版日 2026-11-24 → rule v1（严格 any-fail）→ 不通过：linux-x86_64 (2/7)
切版日 2026-12-01 → rule v2（any-pass）      → 通过：15 天 / 8 天连续绿
```

文档：`docs/plan/soak-runbook.md` §4.3.2。

---

## 三、第 2 条：tag 对象内部名与 ref 不一致 —— 已修

### 事实

```
$ git cat-file -p refs/tags/v1.3.0
object  0fd6f0b8873589989961b2abd9f4ed33e65e2d35
type    commit
tag     v1.3.0-staged          ← 内部名
tagger  Runshine <...>
```

ref 叫 `v1.3.0`，**对象自己叫 `v1.3.0-staged`**。

根因在 `scripts/recut_v130_tag.py`：先用**探针名** `v1.3.0-staged` 造出带注释的签名对象，再 `git update-ref` 把 `refs/tags/v1.3.0` 指过去 —— 对象内部的 `tag` 字段永久定格，没有重建。

**签名本身是好的**（`git verify-tag` 通过，type 仍是 `tag`）。所以只有**按对象名读**的下游会炸 —— 而它恰恰就是 GitHub 的 Latest 页面、`git describe`、`git tag -v`。

### 影响范围（已核实，不是推测）

- **只影响 `v1.3.0`。** `v1.3.1` 的对象名是干净的（`tag v1.3.1`），`v1.3.1` 走的是 `RELEASING.md` §3.3 标准流程，没有走重切脚本。
- 后续 tag 不会继续污染 —— 根因在 `recut_v130_tag.py` 的 `TAG = "v1.3.0"` 硬编码，是一次性脚本。

### 已落地的修复

**1. 根因**：`recut_v130_tag.py` 的 `sign()` 现在**直接用真名签名**（`git tag -s -a <candidate> <target>`），不再签探针名再挪 ref。失败时**恢复旧 tag 对象**（旧对象先记下来），不留「本地一个 tag 都没有」的状态。

**2. 门禁**：`tools/verify_tag_signature.py` 新增名称校验 —— tag 对象的内部名必须与它的 ref 一致。这是**形状属性**，所以 **`--shape-only` 下也生效**，而 `release.yml` 的 preflight 跑的就是 `--shape-only`。也就是说：**这样的 tag 现在会被发布流水线挡下来**。

自测新增 5 项，其中一项**原样复现了这个缺陷**（签一个探针名、`update-ref` 指向它），确认会被拒绝。

实跑验证：

```
v1.3.0 → FAIL：the tag object reached through v1.3.0 names itself 'v1.3.0-staged' …
v1.3.1 → OK
```

`RELEASING.md` §3.3 也补了说明：**签名时用将来发布的那个名字**，不要签了探针名再挪 ref。

---

## 四、第 3 条：活资产出自红流水线 —— 事实成立，措辞需要修正

### 事实

| 项 | 实测 |
|---|---|
| release `created_at` | **2026-11-23T06:21:54Z** |
| release `published_at` | **2026-11-23T05:56:20Z** ← 比 created **早 25 分钟** |
| 资产上传 | **06:39:52Z**（四个 bundle + `.asc` + `sbom.json`） |
| 最后一次 release 运行 | run `45123680856` = **failure** |
| 红在第几步 | publish job 第 **12** 步「Refresh the draft release body」= failure |

`published_at` 早于 `created_at`，只能解释为发布记录被删过重建。

### 措辞修正建议（原反馈单的「dispatch 重跑一次收口」不够准确）

准确的状态是：**资产已挂载，但最后一次发布流水线是红的** —— 这本身是个待确认状态，不是一个可以靠「重跑」掩盖的状态。

而且重跑有副作用：`release.yml` 里 softprops action 的行为是**总是新建 draft 再删掉重复的那个**（这正是第 12 步「Refresh the draft release body」存在的原因，`release.yml:1490-1499` 有注释说明）。所以重跑会**再替换一遍资产** —— 重跑前得确认第 12 步这次能过，否则就是第三次在同一个地方挂。

**建议的表述**：「发布资产已挂载，但最后一次发布流水线在最后一步失败，发布记录存在 `published_at` 早于 `created_at` 的异常。这需要人工确认发布页的最终状态，而不是自动重跑掩盖。」

**`v1.3.1` 已经把这个状态收口了** —— 它的流水线是绿的，资产与侧车一致。

---

## 五、第 4 条：`v1.2.0` 侧车行格式错 —— 已修

### 事实（下载四个平台侧车逐个验过）

```
v1.2.0 windows     : e492d8da...                                          ← 裸摘要，无文件名
v1.2.0 linux-x86_64: 6006a10c...  build/nimrtc-1.2.0-...tar.xz           ← 带 build/ 前缀
v1.2.0 linux-aarch64: 71e7821e... build/nimrtc-1.2.0-...tar.xz
v1.2.0 macos-arm64 : b0c00177...  nimrtc-1.2.0-macos-arm64-clang.tar.xz  ← 只有这个对
```

**摘要全对、3/4 行格式错** —— 所以摘要校验查不出，正是这个形状。

`v1.3.0` 四个侧车**现在全对**（#157 的在位替换生效了），时间戳也对：bundle 是 `06:39:52Z`，四个 `.sha256` 全是 `07:13:03Z` —— 发布后单独重写，连原本正确的 macOS 那个也被一并重写了。

### 已落地的修复

`README.md` 和 `README.zh.md` 现在明确写出：**`v1.2.0` 的四个侧车里三个不能用 `sha256sum -c`**、错在哪（Linux 带 `build/` 前缀 / Windows 裸摘要）、**摘要本身是正确的**（我核对了 GitHub 记录的 bundle digest，四个全部吻合），以及**手工怎么验**。并建议直接用 `v1.3.1` 或更新版本。

同时 `tools/check_sha256_sidecars.py` 已经作为切版门禁在跑 —— 这个范式（把 README 里写的命令变成 CI 真跑的门禁）是对的。

---

## 六、第 5 条：hardware job 三次全红，且不在任何门禁里 —— 已修

### 事实

ruleset `protect-main-and-releases` 的 `required_status_checks` **恰好 13 项**，逐项核对后确认：**没有 `hardware / hw-e2e (self-hosted GPU)`**。

runner **确实注册了**（`nimrtc-gpu-nvenc`，机器 `RUNSHINE-WORKST`），所以不是「没有机器」，是**网络层**挂了：

```
##[error]fatal: ambiguous argument 'HEAD': unknown revision or path not in the working tree.
##[error]error: RPC failed; curl 18 transfer closed with outstanding read data remaining
##[error]fatal: early EOF
##[error]fatal: fetch-pack: invalid index-pack output
```

失败原因日志能直接读到，不用猜：是 `git fetch` 的 RPC 中断（curl 18 / curl 56 SSL_read），加上 action 下载超时 100 秒。**一个 GPU 用例都没跑到。**

而 `hardware.yml:44-45` 自己的注释就是：

> a job that never reports is not a gate, it is a **decoration**

现在这条装饰每天都在报红，且没人看。

**补充核实（比原反馈单更强的数据）**：我把该 workflow 的**全部 22 次运行**列了一遍 —— **零次成功**。失败模式也不止 checkout：近期几次已经推进到 submodule 阶段（`fatal: could not get a repository handle for submodule 'src/third_party/nlohmann_json'`），但仍然一次都没跑完。

### 已落地的修复

**1. 新增 `hardware` 门禁检查**（`scripts/check-release-freeze.sh`）。

它读 `build/hardware/` 里的归档证据，要求它必须是**被切版那棵树上的 PASS**：

- **记录在别的 commit 上** → 不算数（和 soak 窗口同一个规则）
- **记录在脏树上** → 不算数（脏树不是它声称的那个 commit）
- **没有记录** → `--final-tag` 下是 **FAIL**，不是 SKIP

最后一条是刻意的，**和 `api-symbols` 检查对缺 `node` 的处理完全一致**，理由也一样：切版时没有后续 CI 跑来补。这把「硬件路径被证明过」和「没有任何东西说它没被证明过」区分开了 —— 而这正是原反馈单指出的缺口。

**2. `RELEASING.md` §3.3 新增第 12 步**：切版前先收集硬件证据（怎么跑 `hardware.yml`、怎么把记录取回来、怎么校验），并在步骤里直接写明「该 job 至今 22 次全败」这个事实。

**3. 判定语义明确化**：一条存在的记录如果是 SKIPPED（runner 丢了 GPU）或 FAIL，在两种模式下都是 **FAIL** —— 一条说「硬件路径没工作」的记录绝不能读成通过。

---

## 七、做得对的六条 —— 复核后全部成立

| 项 | 核实 |
|---|---|
| 口径 15 天来首次全对齐 | `main` HEAD 上 13 项 required 全 success，ruleset 13 项，无遗漏 |
| 发版日零产品代码改动 | `git log --since/--until` 覆盖发布窗口，**零 `src/` 改动**（#149/#152/#154/#155/#157 全是 workflow/脚本/文档） |
| 发布正文诚实披露门禁改动 | `v1.4-plan.md:38`、`v1.3-plan.md:1023-1034` 均明确记载 |
| #157 的用户路径门禁范式 | `tools/check_sha256_sidecars.py` + job 里真跑 `sha256sum -c`，把 README 命令变成 CI 真跑的门禁 —— **这个范式是对的，本轮沿用** |
| #152 的事故记录 | commit message 完整记录了 `EDITOR unset` 事故、signed-HEAD 缺陷、`update-ref` 换对象的原因 |
| Q1b 真硬件日志 | `v1.3-plan.md:870` 记 `bf618a8a`（RTX 3060 / driver 616.92） |

**本轮新增的一条正面项**：`v1.3.0` 的两次改动都**留了书面记录**（`v1.3-plan.md`、`v1.4-plan.md`、CHANGELOG），这是这次能复盘出机制问题的基础 —— 没有这些记录，第 1 条就只是「看起来像」。

---

## 八、待定项：第 12 步失败应否阻断发布

原反馈单把这条列为 P2（「第 12 步刷新正文失败应阻断发布而不是留半成品」）。

**现状**：`release.yml` 的第 12 步失败后，资产已经挂上去了，发布也已经是 published 状态 —— 也就是说这个失败**没有阻断发布**。

**本轮没有改**，原因是它有一个真实的取舍：

第 12 步存在的原因，是 softprops action **总是新建 draft 再删掉重复的那个** —— 所以「刷新正文」这一步失败时，**草稿和正本是两个不同的对象**。如果这时直接阻断，已经上传的资产会挂在一个不会被发布的 draft 上，等于把「半成品发布」换成「半成品不发布 + 要人工清理」。

两个选项：

- **A（当前行为）**：第 12 步失败 → job 红，但发布保留，人工复核发布页。
- **B**：第 12 步失败 → 阻断发布，同时把已上传的资产清理掉。

B 更严格，但需要在失败路径上做资产回滚，且回滚本身也会失败。**这需要锋哥定调**，本轮先不动。

**可以先做的无争议部分**：让第 12 步的失败**在日志里说清楚发生了什么**（正文没刷新成功、发布页当前是哪个对象、资产是否完整），这样人工复核时不用靠猜。这一条已经在本轮的工作范围内。

---

## 九、给锋哥的口头版（按优先级）

1. **tag 对象名那条最急，因为它会持续污染后续发版。** `v1.3.0` 的 tag 对象内部叫 `v1.3.0-staged`，ref 叫 `v1.3.0` —— 签名是好的，但 GitHub 页面、`git describe`、`git tag -v` 全都报 `-staged`。根因是重切脚本先签探针名再挪 ref。**已修**：脚本改成直接用真名签名，`verify_tag_signature.py` 加了对象名校验，而且这个校验在 `--shape-only` 下也生效 —— 也就是说发布流水线现在会挡下这种 tag。

2. **门禁规则那条，机制比判断重要。** 11-23 那次改动本身是对的（同 commit 同日过了 3 次），我完全支持保留「单次 FAIL 不污染整天」。问题在于规则是 workflow 里写死的一个数字，**改它不用版本号、不用评审、不用过渡期**，所以「判据持有者 = 受益者 = 当天生效」这个先例在机制上是允许的。**已修**：规则改成有版本号、有生效日期、有冷却期，冷却期内改规则决定不了任何发布，**改规则的那次发布由旧规则判定**。天数改回 7。而且 release.yml 现在**完全不传** `--require-days` 了。

3. **hardware 那条本质是「没人看它在报红」。** 这个 job 至今 22 次运行**零成功**，而且它根本不在 ruleset 的 13 项 required 里 —— 所以发版流水线从来不读它的证据，Q7 的硬件主张可以在什么都没有的情况下发出去。**已修**：freeze checklist 加了 `hardware` 检查，要求证据是**被切版那棵树上的 PASS**，别的 commit 上测的、脏树上测的都不算；没有记录在 `--final-tag` 下直接 FAIL（和 `api-symbols` 缺 node 一个待遇）。

4. **`v1.2.0` 侧车那个要跟用户说清楚**：三个侧车不能 `sha256sum -c`，但**摘要全对**，所以手工验是安全的。README 已经加警告了。

5. **第 12 步那条留给你定。** 现在失败不阻断发布，资产是挂上去了的。要不要改成「失败就撤资产、阻断发布」？这有个真实取舍 —— 撤资产本身也会失败。B 方案更严格但需要定调，我先没动。

6. **有一条我建议补一句正面评价**：这两次改动都**留了书面记录**（plan、CHANGELOG 都写了），这才让第 1 条能从「看起来像」变成「可复现的机制缺陷」。没有记录的话，这次复盘根本做不出来。

---

## 十、本轮改动清单

| 文件 | 改动 |
|---|---|
| `tools/run_soak.py` | 规则版本化（`GATE_RULE` / `PREVIOUS_GATE_RULE`）、`rule_in_force()` 冷却期判定、`--cut-date` / `--rule-version`、`intra_day` 规则可选；自测 +8 项（共 40 项全绿） |
| `tools/verify_tag_signature.py` | tag 对象名校验（`--shape-only` 下也生效）；自测 +5 项，含原样复现 `v1.3.0-staged` 缺陷 |
| `scripts/recut_v130_tag.py` | `sign()` 改为直接用真名签名；失败时恢复旧对象；不再传 `--require-days` |
| `scripts/check-release-freeze.sh` | 新增 `hardware` 检查（PASS-on-tree、脏树拒绝、final-tag 下缺记录 FAIL） |
| `.github/workflows/release.yml` | 门禁改用 `--cut-date` + 规则，不再硬编码 `--require-days`；tag 校验步骤说明补全 |
| `.github/workflows/soak.yml` | 头部注释更新（不再声称五天窗口） |
| `docs/plan/soak-runbook.md` | 新增 §4.3.2「哪条规则判定哪个发布」；全文五天 → 七天 |
| `RELEASING.md` | §3.3 新增第 12 步（收集硬件证据）；标签签名段补对象名说明；步骤编号顺延 |
| `CHANGELOG.md` | `## [Unreleased]` 新增三条修复记录 |
| `README.md` / `README.zh.md` | `v1.2.0` 侧车警告 |
| `docs/plan/v1.3-plan.md` / `v1.4-plan.md` | 更正五天窗口的记载，标注规则版本化 |
| `tools/README.md` | `run_soak.py` 说明更新 |

**验证状态**：`run_soak.py --self-test` 40/40、`verify_tag_signature.py --self-test` 10/10、`check_md_links.py` 162 文件 0 问题、`check_workflow_expressions.py` PASS、`check_ci_ruleset.py` PASS、三个 workflow YAML 均可解析、`check_encoding.py` 通过。
