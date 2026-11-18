# 交接事项：本地 v1–v5 版本分支的合并与推送

**状态**：待办（等 release/1.2 稳定后执行）
**日期**：2026-11-18
**相关**：`docs/plan/v1.2-next-plan.md`（1.2 开发线）、ADR-056（版本分支是追溯锚点）、ADR-079、D-70

## 背景

v1–v5 这五个版本的**产物代码**已经通过 PR #91 / PR #102 走 squash 合并进了 `NimRTC/nimrtc` 的 `main`，
tag `v1`…`v5` 也已推送到 GitHub。

但**版本分支本身只存在于本地**：

```
$ git branch -r | grep version/     # 远端 0 个
$ git ls-remote --heads origin      # 没有 version/* 分支
```

| 项 | 本地 | GitHub |
|---|---|---|
| v1–v5 的产物代码 | ✅ | ✅（squash 进 main） |
| v1–v5 的 **tag** | ✅ | ✅ |
| v1–v5 的 **版本分支**（原始 commit 链） | ✅ | ❌ |

### 为什么这是个问题

squash 合并**重写了 main 的历史**，于是：

1. `version/v1`…`version/v5` 的 tip commit **不在 `origin/main` 的祖先链上**；
2. tag `v1`…`v5` 指向的 commit 在 GitHub 上是「孤儿」——
   从 tag 出发看不出它与 main 的关系（那条关系被 squash 抹掉了）；
3. 这正是 **D-70** 的成因：版本分支与主干有**两套祖先**，
   `version ship` 只能报「主干已前进」，报不出「已分叉」。

## 已做的离线备份（2026-11-18）

在动任何远端之前，先做了**不碰远端**的备份：

| 备份物 | 路径 | 说明 |
|---|---|---|
| git bundle | `D:\MyOpen\nimrtc-backup\nimrtc-version-branches-20261118.bundle` | 10 个 ref（5 分支 + 5 tag），`git bundle verify` 通过，63 MB |
| 状态目录 | `D:\MyOpen\nimrtc-backup\.zelto-20261118\` | 22 个事件流 + 切片/版本计划/签字锁 |

bundle 可完整还原：

```bash
git clone nimrtc-version-branches-20261118.bundle restored/
cd restored && git checkout version/v5
```

## 待办：等 1.2 稳定后合并推送

**触发条件**：`release/1.2` 稳定（该线上的别的 agent 收工）。

**为什么等**：现在推会与 1.2 的开发线撞车——main 落后 origin/main 5 个提交，
1.2 还在动。此时动 main / 版本分支，容易把别人正在做的线搅乱。

### 步骤（**不做 rebase**）

> **决定：不 rebase。**
> rebase 会重写版本分支的提交历史，而版本分支的 commit 已经被
> `v1`…`v5` 的 tag 指着——重写会让 tag 指向的东西变了，
> 追溯锚点就动了。tag 是不可变锚点（ADR-056 D6）。

1. **同步 main**

   ```bash
   git fetch origin --tags
   git checkout main
   git merge --ff-only origin/main      # 本地 0 领先、5 落后 → 快进即可
   ```

2. **把 5 个版本分支推上去**（不动它们的 commit）

   ```bash
   git push origin version/v1 version/v2 version/v3 version/v4 version/v5
   ```

   tag `v1`…`v5` 已在远端，**不要**重推（`git fetch` 时报过
   `v1.2.0 ... would clobber existing tag`——那是 nimrtc 自己的历史 tag，与 v1–v5 无关）。

3. **验证三方一致**

   ```bash
   git ls-remote --heads origin | grep version/   # 应有 5 个
   git ls-remote --tags origin | grep -E "v[1-5]$" # 应有 5 个
   # 本地 tag 指向的 commit 应与远端一致
   ```

4. **确认 main 上已有全部产物**

   ```bash
   git merge-base --is-ancestor version/v5 origin/main   # 预期：非 0（squash 过的正常现象）
   ```

   > 这一条**预期失败**。它失败是对的——说明 v1–v5 的产物是通过
   > squash 进 main 的，原始 commit 链与 main 是两条线。
   > **这不是缺陷，是 squash 的必然结果**，别试图用 rebase 去"修"它。

## 与 zelto 状态的关系

`.zelto/` 状态目录**不进 git**（`.gitignore` 第 78 行），已在上面单独备份。

追溯链的证据载体（切片记录、事件流、签字锁、版本计划）**不外传**——
这是 C1「状态只在文件里」的落地。GitHub 上留下的是**代码与 tag**，
zelto 自己的过程记录留在本地。

## 相关缺陷

- **D-70**：`version ship` 区分不了「主干前进」与「主干历史被改写」。
  已在 zelto 侧修好（ADR 见 `D:\MyOpen\Zelto\docs\adr\`），但由于本条不做 rebase，
  nimrtc 这边**不会触发**那个修复路径 —— 版本分支保持原样推送。
