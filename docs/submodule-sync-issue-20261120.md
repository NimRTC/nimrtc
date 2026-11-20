# nimrtc 仓库问题报告：submodule 本地缺 commit + SSH 通路挂死

> **报告人**：zelto 驾驶舱（自动开发流程）
> **日期**：2026-11-20
> **仓库**：`D:\MyOpen\nimrtc-v14`（分支 `feat/v14-turn-tls`，HEAD `ff9d64b0`）
> **影响**：zelto 无法在该仓库里为切片创建可用的 worktree → 无法开跑 → 无法继续 v1.4 开发
> **性质**：**仓库/环境问题，不是代码缺陷**。请仓库提交人处理。

---

## 1. 现象

在 nimrtc-v14 上用 zelto 给切片 `v1-n5-turn-server` 起跑时，**卡在 worktree 准备阶段超过 30 秒**，
Web 驾驶舱报：`❌ 起跑失败：发起运行超时（30 秒）`。

手工在终端跑 `git submodule update --init`，同样卡死：

```
$ git -c protocol.file.allow=always submodule update --init src/third_party/wolfssl/src
（无输出，60 秒后被外部超时中断）

$ git -c protocol.file.allow=always submodule update --init --no-fetch src/third_party/wolfssl/src
fatal: unable to read tree (ac01707f552c611fbd135cc723b2682b3e7f80f2)
fatal: Unable to checkout 'ac01707f552c611fbd135cc723b2682b3e7f80f2' in submodule path 'src/third_party/wolfssl/src'
```

14 个 submodule 串行同步，实测耗时 **126,988 ms（约 2 分钟）**，其中：

```
src/third_party/wolfssl/src    60,014 ms   ← 挂满 60 秒
src/third_party/msquic         60,021 ms   ← 挂满 60 秒
其余 12 个                          ~500-800 ms
```

---

## 2. 根因（三层，叠加）

### 2.1 目标 commit 在本地 module git 目录里**不存在**

主仓 index 记录的 gitlink（`git ls-files -s`）与本地 `.git/modules/<path>` 里的实际内容对不上：

| submodule | index 记录 | 本地有吗 | 本地 HEAD | origin URL |
|---|---|---|---|---|
| `src/third_party/wolfssl/src` | `ac01707f` | **否** | `da2d6760` | `git@github.com:wolfSSL/wolfssl.git` |
| `src/third_party/msquic` | `a5ae3a06` | **否** | `6a919ce4` | `https://github.com/microsoft/msquic.git` |
| `src/third_party/libopus/src` | `22244de5` | **否** | `acdb9e26` | （无） |
| `src/third_party/nlohmann_json` | `9cca280a` | **否** | `aebf3e51` | （无） |
| `src/third_party/googletest` | `15460959` | **否** | `05d7a9ba` | （无） |
| `src/third_party/libsrtp/src` | `2f82ec0e` | **否** | `49ab224f` | （无） |
| `src/third_party/openh264/src` | `652bdb77` | **否** | `d846cbfe` | （无） |
| `src/third_party/libyuv/src` | `209763ad` | **否** | `70a3bec8` | （无） |
| `src/third_party/portaudio/src` | `8b6333ff` | **否** | `ba5abd19` | （无） |
| `src/third_party/libsamplerate/src` | `0844c208` | **否** | `fdbd0668` | （无） |
| `src/third_party/libvpx/src` | `6df3ec34` | **目录都不存在** | — | — |
| `src/third_party/svt-av1/src` | `e3d63405` | **否** | `a847523c` | （无） |
| `src/third_party/gmssl/src` | `7c9f0290` | 是 ✅ | `7c9f0290` | `https://github.com/guanzhi/GmSSL.git` |
| `src/third_party/dav1d/src` | `42b2b24f` | 是 ✅ | `42b2b24f` | `https://code.videolan.org/videolan/dav1d.git` |

**14 个里 12 个的目标 commit 本地没有。** 只有 gmssl 和 dav1d 有——这正好对应了实测里
「快的两个几百毫秒就过、慢的两个挂满 60 秒」的分布。

这同时解释了主仓工作区的脏状态：

```
$ git status --porcelain
 M src/third_party/googletest
 M src/third_party/libopus/src
 M src/third_party/libsamplerate/src
 M src/third_party/libsrtp/src
 M src/third_party/libyuv/src
 M src/third_party/msquic
 M src/third_party/nlohmann_json
 M src/third_party/openh264/src
 M src/third_party/portaudio/src
 M src/third_party/svt-av1/src
 M src/third_party/wolfssl/src
```

这 11 个 `M` **不是谁改的**——是 submodule 检出在了一个与 index 不同的 commit 上
（index 要 `ac01707f`，工作区检出的是 `da2d676`）。

### 2.2 本地缺 commit → git 只能去联网取

`git submodule update --init` 找不到目标 commit 时会去 fetch。而 `.gitmodules` 里写的是
公开仓库 URL，**本地 `.git/config` 里 `submodule.<name>.url` 又把它覆盖成了 SSH**：

```
submodule.src/third_party/wolfssl/src.url=git@github.com:wolfSSL/wolfssl.git
submodule.src/third_party/msquic.url=git@github.com:NimRTC/msquic.git
submodule.src/third_party/googletest.url=git@github.com:google/googletest.git
...（共 12 条，全是 git@github.com:）
```

### 2.3 SSH 通路挂死，HTTPS 极慢

**SSH 不通**。挂死的进程栈：

```
git.exe -c protocol.file.allow=always submodule update --init src/third_party/wolfssl/src
└─ git submodule--helper update --init -- src/third_party/wolfssl/src
   └─ git fetch
      └─ ssh -o SendEnv=GIT_PROTOCOL git@github.com "git-upload-pack 'wolfSSL/wolfssl.git'"   ← 卡在这里
```

**HTTPS 通，但极慢**。实测：

```
$ git ls-remote https://github.com/wolfSSL/wolfssl.git HEAD
8d89da41ccda883fd93e41931dd1f34f9a8ee2e5        HEAD
（耗时 65,623 ms）
```

而且——**HTTPS 会被改写成 SSH**。全局 git 配置里有一条：

```
$ git config --global --get-regexp insteadof
url.git@github.com:.insteadof https://github.com/
```

所以：`.gitmodules` 里的 https URL → 被改写成 `git@github.com:` → 走 SSH → 挂死。
**两条路都不通。**

---

## 3. 影响

| 影响面 | 说明 |
|---|---|
| **zelto 无法开跑** | worktree 准备阶段（同步 submodule）耗时 > 30 秒，Web 端超时杀进程，切片卡在 `running` |
| **门禁必然失败** | vendor 源码树缺失 → CMake 配置阶段报「找不到 googletest / 找不到 wolfssl」 |
| **写集审计会误报** | 上面那 11 个 `M` 会被判成「agent 越界改了 vendor」 |
| **agent 会浪费时间** | agent 看到空的 vendor 目录，很可能自己手工拷一份源码进去——把 submodule 变成「非 submodule 的普通目录」，破坏主仓一致性（历史上已经发生过一次） |
| **机器上残留僵尸进程** | 每次超时会留下 `git fetch` / `ssh git-upload-pack` / `git index-pack` 进程与临时 pack 文件（本次清理出 20 个进程、114 MB 临时文件） |

---

## 4. 建议的修法

### 4.1 先解决 SSH 通路（二选一）

**方案 A —— 去掉改写规则，让 https 真正走 https**（推荐，改动最小）

```bash
git config --global --unset url.git@github.com:.insteadof
```

**方案 B —— 让 SSH 能用**

检查并配置 SSH key 与 agent：

```bash
ssh -T git@github.com          # 应该返回 "Hi <user>! You've successfully authenticated"
```

如果 SSH key 没配或没加载到 agent，会一直卡在等密码/等确认。

### 4.2 把 index 记录的 commit 补齐到本地

**方案 A —— 拉一次并检出到 index 记录的位置**（推荐，一次性对齐）

```bash
cd D:\MyOpen\nimrtc-v14

# 逐个 submodule 拉到 index 记录的 commit
git submodule update --init --recursive

# 确认对齐（应该全部没有 + 前缀）
git submodule status
```

**方案 B —— 把主仓工作区的 submodule 对齐到 index**（如果远程已不可达）

```bash
cd D:\MyOpen\nimrtc-v14
git submodule update --init --recursive
git submodule foreach --recursive 'git checkout --force $(git rev-parse :../../ 2>/dev/null || echo HEAD)'
```

> 若目标 commit 在远程**根本不存在**（例如分支被 force-push 或 tag 被删），
> 那需要先确认 `.gitmodules` 与 `src/third_party/vendor.json` 里记录的 pin 是否仍然有效。

### 4.3 处理 `src/third_party/libvpx/src`

这一条的 module git 目录**根本不存在**（其它 13 个至少有个目录），
说明它从未被初始化过，或者初始化失败后被删了。

```bash
git submodule update --init src/third_party/libvpx/src
git submodule status src/third_party/libvpx/src
```

注意 `.gitmodules` 里 libvpx 默认是 `OFF`（`NIMRTC_VENDORED_LIBVPX=OFF`），
但它仍然是一个真实 submodule，需要初始化到 index 记录的 `6df3ec34`。

### 4.4 顺带清理

仓库根目录有 4 个遗留的探测脚本（`probe-turn.js` / `probe-turn2.js` / `probe-turn3.js` /
`probe-turn4.js`）和 `arch-out.txt`，都是上一轮调试留下的，建议删掉或加进 `.gitignore`。
（`arch-out.txt` 看起来是 zelto 的架构检查输出。）

---

## 5. 怎么验证修好了

```bash
cd D:\MyOpen\nimrtc-v14

# 1. 目标 commit 都在本地了（每行的 commit 前不应有 + / -）
git submodule status

# 2. 主仓工作区干净了（那 11 个 M 应该消失）
git status --porcelain

# 3. 同步耗时在秒级
time git submodule update --init --recursive
```

三条都过了，zelto 就能正常为切片建 worktree、开跑、跑门禁。

---

## 6. 附：这次排查留下的现场

- 诊断过程中泄漏了 20 个 `git fetch` / `ssh` / `git index-pack` 进程与 10 个 `tmp_pack_*`
  临时文件（合计 114 MB），**已全部清理完毕**，仓库状态与排查前一致。
- 切片 `v1-n5-turn-server` 目前状态是 `running`（zelto 侧的残留），
  其 worktree 里的产物（`examples/turn-server/` 5 个文件 + 验收测试 8 条真断言）**完好无损**，
  门禁此前已通过一次。仓库问题修好后可以直接继续。
