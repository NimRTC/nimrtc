# Slice: v1-dtls-cert-outside-submodule

## Summary

**构建期不再往 wolfSSL 子模块里写证书 —— 那是别人的仓库，而且写进去的那份证书是错的。**

本片收编一个**声明了归属却没落进任何版本分支**的提交：

- 提交 `8d502dd4`（"fix(dtls): 构建期证书不再写进 wolfSSL 子模块，CI 显式生成"，
  2026-11-22）带 `Refs-Slice: v1.4-n1` trailer，但 **`version/v3` 里没有它**。
- 它的 diff 至今只存在于分支 `feat/v14-turn-tls`。

核实（2026-11-25）：`version/v3` 的 `scripts/build_dtls_cert.py:59-62` 仍然把证书
写进 `src/third_party/wolfssl/src/certs/`，即**缺陷仍在 v3 上**。

这不是「补一份证书」的问题，是**两个独立缺陷**叠在一起：

### 缺陷 A：构建污染子模块

`build_dtls_cert.py` 把 `server-ecc.pem` / `ecc-key.pem` 覆盖进
`src/third_party/wolfssl/src/certs/`。这两个文件**在上游子模块里是被跟踪的**
（相对 v5.9.2-stable `ac01707`），所以覆盖的结果是：

- 子模块长期显示 `-dirty`；
- 主仓显示 `m src/third_party/wolfssl/src`；
- **`git submodule status` 再也不是干净的**，而这正是可复现构建的判据。

### 缺陷 B：运行期会用错的证书赢过对的证书

上游那份跟踪的旧证书是 `CN=www.wolfssl.com`、**没有 SAN**。而运行期需要的是
带 SAN 的新证书（Chrome / BoringSSL 对无 SAN 证书返回 `fatal alert 46`）。

若运行期查找候选表里**保留**子模块路径，则在某些 CWD 下旧证书会**赢过**新证书，
握手失败 —— 而症状（握手被拒）离原因（证书没 SAN）很远，属于最难查的那类问题。

所以本片不是「把缺失的证书补回去」——补回去只会让缺陷 B 更难查。

## 目标

**让构建期产物只落在本项目自己的目录里，并让 CI 显式生成它。**

三件事：

1. `build_dtls_cert.py` **不再**写 `src/third_party/wolfssl/src/certs/`，
   只写 `tests/wolfssl_dtls/certs/`（该目录被 `.gitignore` 忽略，纯构建产物）。
2. CMake（4 处）与运行期查找候选表改从新位置取证书，并**移除**候选表里的
   子模块路径 —— 留着它是有害的（见缺陷 B）。
3. CI 每个跑 DTLS 的 job 显式生成证书 —— 此前 CI 靠上游跟踪的 `ecc-key.pem`
   免掉这一步，新路径下 `tests/wolfssl_dtls/certs/` 在新 checkout 里不存在，
   `copy_if_different` 会因源缺失而失败。

### 现状核实（写规格时的树状态，2026-11-25）

| 位置 | v3 现状 | 本片之后 |
|---|---|---|
| `scripts/build_dtls_cert.py:59-62` | 定义 `WOLFSSL_CERT_PATH` / `WOLFSSL_KEY_PATH` 指向子模块 | 删除这两个常量 |
| `cmake/NimRTCTest.cmake` | 从子模块路径取证书 | 从 `tests/wolfssl_dtls/certs/` 取 |
| `cmake/NimRTCRuntime.cmake` | 同上（`NIMRTC_DTLS_CERT_STAGING_DIR`） | 同上 |
| `src/modules/dtls/CMakeLists.txt` | `NIMRTC_WOLFSSL_CERT_DIR` 指向子模块 | 指向新目录 |
| `dtls_wolfssl_session.cpp` | 运行期候选表含子模块路径 | 候选表**移除**该项 |
| `.github/workflows/*.yml`（6 个） | 未显式生成证书 | 每个跑 DTLS 的 job 显式生成 |

### 与其它片的边界（本片**不做**）

- **不做** 改 `src/third_party/wolfssl/` 的任何内容 —— 那是上游，且本片的**目的**
  就是不再碰它。
- **不做** 改 wolfSSL 的 pin —— pin 是正确的（`ac01707` = v5.9.2-stable），
  已被 `69bebb5f` 核实为与 feat 分支一致。
- **不做** 引入新依赖 —— `cryptography` 的版本上限是**已有**决策（见「设计 §四」），
  本片只是让它在 CI 里被正确安装。
- **不做** 改 GMSSL 后端的证书路径 —— 它用自己的生成器（`tests/test_dtls_gmssl_tlcp`）。

## 范围

- scripts/build_dtls_cert.py
- cmake/NimRTCTest.cmake
- cmake/NimRTCRuntime.cmake
- src/modules/dtls/CMakeLists.txt
- src/modules/dtls/backends/wolfssl/src/dtls_wolfssl_session.cpp
- .github/workflows/ci.yml
- .github/workflows/ci-fast.yml
- .github/workflows/interop.yml
- .github/workflows/release.yml
- .github/workflows/soak.yml
- .github/workflows/hardware.yml
- tests/acceptance/v1-dtls-cert-outside-submodule/acceptance.test.cjs
- specs/slices/v1-dtls-cert-outside-submodule.md

> **只读** `src/third_party/wolfssl/src/certs/` —— 那是上游被跟踪的文件，
> 只读不写。本片之后**任何**构建产物都不得落在这里。
>
> **只读** `.gitignore` —— `tests/wolfssl_dtls/certs/` 的忽略规则**已存在**
> （`.gitignore:227`），本片不新增忽略规则。
>
> **只读** `AGENTS.md` —— 参考它的 §8a 共享文件纪律，本片不改它。

**这是 v1.4 唯一被允许碰 `ci.yml` 的额外切片。** AGENTS.md §8a 的规则是
「all of v1.4's `ci.yml` edits land in one PR (N9)」—— N9 已签字合入，
它的 `ci.yml` 改动也已落地。本片是**在其之后**的必要补充，因为缺陷 A 使
「新 checkout 里证书不存在」，不补这条 CI 步骤则**所有 DTLS job 都会红**。
本片必须作为一个**单一提交簇**落地，不与任何其它切片共享 `ci.yml` 改动。

## 设计

### 一、脚本只写自己的目录

```python
# scripts/build_dtls_cert.py  —— 删除这两个常量
-# WOLFSSL_CERT_PATH = (ROOT / "src" / "third_party" / "wolfssl" / "src"
-#                      / "certs" / "server-ecc.pem")
-# WOLFSSL_KEY_PATH  = (ROOT / "src" / "third_party" / "wolfssl" / "src"
-#                      / "certs" / "ecc-key.pem")

# 保留（已在 v3 中存在）
CERT_PATH = ROOT / "tests" / "wolfssl_dtls" / "certs" / "server-ecc.pem"
KEY_PATH  = ROOT / "tests" / "wolfssl_dtls" / "certs" / "ecc-key.pem"
```

`tests/wolfssl_dtls/certs/` 是**纯构建产物目录**（被忽略）。脚本在那里可以
随意覆盖，不影响任何 git 状态。

### 二、CMake 四处统一指向新目录

| 文件 | 变量 | 新值 |
|---|---|---|
| `cmake/NimRTCTest.cmake` | `_nimrtc_wolfssl_cert_dir` | `${CMAKE_SOURCE_DIR}/tests/wolfssl_dtls/certs` |
| `cmake/NimRTCRuntime.cmake` | `NIMRTC_DTLS_CERT_STAGING_DIR` | 同上 |
| `src/modules/dtls/CMakeLists.txt` | `NIMRTC_WOLFSSL_CERT_DIR` | 同上（编译期兜底路径） |

**四处必须一致**。任何一处残留旧路径，都会让「某个构建配置下又去读子模块」，
本片的判据 6 会抓到它。

### 三、运行期候选表必须移除子模块路径

`dtls_wolfssl_session.cpp` 的运行期查找候选表里**删除**子模块路径项。

**理由必须写进代码注释**：留着它不是「多个兜底」，而是「一个会赢的陷阱」——
上游旧证书 `CN=www.wolfssl.com`、无 SAN，Chrome 会以 `alert 46` 拒绝。
症状离原因很远，正是最难查的类型。

### 四、CI：每个跑 DTLS 的 job 显式生成证书

新增步骤（在所有 6 个 workflow 里，凡是有 DTLS 测试的 job）：

```yaml
      - name: Generate DTLS test certificate
        run: |
          python -m pip install --upgrade "cryptography<42"
          python scripts/build_dtls_cert.py
```

**`cryptography` 必须钉在 `<42`**，这不是随意选的上限：

- `cryptography >= 42` 的 PyPI Windows wheel 是 Rust 实现，只带 `_rust.pyd`，
  **不带** `cryptography_rust.dll` → `import` 直接 `DLL load failed while
  importing _rust`。
- `41.x` 是最后一条纯 C 线，在干净 venv 里 `import` 正常、脚本跑通，
  产物为 `CN=localhost` + 8 个 SAN。

**因此约束写 `cryptography<42`（不是 `>=42,<43`）。** 这条已由 ADR-095 §7.5 实测确认。

**这条上限的理由必须写进 workflow 注释**，否则下次有人「顺手升级依赖」会踩回同一个坑。

## 验收标准

### 一、脚本行为

1. `scripts/build_dtls_cert.py` **不包含**任何指向子模块的**写入路径常量**
   （`WOLFSSL_CERT_PATH` / `WOLFSSL_KEY_PATH` 已删除）；
   `CERT_PATH` / `KEY_PATH` 只指向 `tests/wolfssl_dtls/certs/`。
   *注释里解释「为何不镜像到子模块」是允许且应当保留的* —— 判据针对的是可执行路径。
2. 运行脚本后，`src/third_party/wolfssl/src/certs/` 下文件的 mtime **不变**。
3. 运行脚本后，`tests/wolfssl_dtls/certs/server-ecc.pem` 与 `ecc-key.pem` 存在且为新生成。
4. 生成的证书含 `CN=localhost` **且**含 SAN 扩展（用 `openssl x509 -text` 断言）。

### 二、git 状态洁净

5. 在**干净** worktree 上跑完 prepare（含证书生成）后，
   `git submodule status` **无** `-dirty` 前缀。
6. 主仓 `git status --porcelain` 在证书生成后**无** `m src/third_party/wolfssl/src`。

### 三、CMake 与运行期路径

7. 四处 CMake 引用的证书变量**全部**指向 `tests/wolfssl_dtls/certs`；
   `cmake/` 与 `src/modules/dtls/` 下**没有任何 CMake 赋值/路径语句**引用
   `third_party/wolfssl/src/certs`（注释里解释「为何迁移」不算，且应当保留）。
8. `dtls_wolfssl_session.cpp` 的运行期候选表**不含**子模块路径项
   （该文件含一条解释为何移除的注释，注释提及路径是允许的）。
9. DTLS 既有测试**全部仍然通过**（回归）：`ctest -R 'Dtls|dtls'` 全绿。

### 四、CI

10. 6 个 workflow 中，凡跑 DTLS 测试的 job 都有「生成证书」步骤。
11. 该步骤里的 `cryptography` 版本约束为 `<42`，且有注释解释上限来源
    （ADR-095 §7.5 的实测定论）。
12. `ci.yml` 的 diff 只含「生成证书」相关行，**不夹带**任何无关改动
    （这是 §8a 共享文件纪律的落实）。

### 五、文档

13. `CHANGELOG.md` 有对应条目，说明「构建产物不再写进子模块」与「CI 显式生成证书」。
14. `specs/slices/v1-dtls-cert-outside-submodule.md` 在实施结束时补「实施发现」。

## 归属

**`version/v3`**。

**收编来源**：`feat/v14-turn-tls` 的 `8d502dd4`。实施时**以该提交为起点**
（`git cherry-pick -n 8d502dd4` 或等价），在其上补齐本规格的判据，
而不是从零重写 —— 该提交的推理已被实测验证（6 workflow / 14 job 的 CI 步骤、
`cryptography` 上限的实测定论），重写只会丢掉这些证据。

**前置**：N5（`v1-n5-turn-server`）已合入；`69bebb5f`（submodule pin 修复）
已在 v3，本片以其为基线。

**收尾影响**：本片合入后，`feat/v14-turn-tls` 上**唯一**未进 v3 的内容即为空，
该分支可以删除（待人工确认）。此前核查已确认该分支与 v3 的差异为
11 个文件 / +601 / −42，**全部**属于本片范围。

## 实施发现（2026-11-25 合入时记录）

**1. 判据 1/7/8 的原始措辞会把「解释性注释」判成违规 —— 已收窄到可执行路径。**
本片三处「不得引用子模块」的判据，初稿写的是「源码/目录下不得出现
`third_party/wolfssl/src/certs` 字符串」。实测发现：`8d502dd4` 在
`build_dtls_cert.py`、`NimRTCTest.cmake`、`NimRTCRuntime.cmake`、
`dtls_wolfssl_session.cpp` 里都留了**大段注释**解释「为何不再镜像到子模块」——
那是本片最有价值的防回退知识（尤其是「上游无 SAN 证书会在某些 CWD 下赢过
新证书、Chrome 报 alert 46」这条，症状离原因极远）。若判据按字面断言，
正确实现反而会被打回。

三条判据因此改为：**先剥掉 `#` 注释行，再判是否存在路径表达式/候选表项**。
教训：写「不得出现 X」类判据时，先想清楚「解释为什么不该有 X」算不算出现 X。
这个仓库大量使用「注释即证据」的写法，判据必须与之相容。

**2. `cryptography` 的上限是 `<42`，不是 `>=42,<43` —— 规格初稿写反了。**
我凭印象写成 `>=42,<43`，实施时对照 ADR-095 §7.5 才确认是反的：
42+ 的 PyPI Windows wheel 是 Rust 实现，**不带** `cryptography_rust.dll`，
`import cryptography` 直接 `DLL load failed while importing _rust`；`41.x`
才是最后一条纯 C 线。规格、设计示例、判据 11 三处均已改为 `cryptography<42`。
`8d502dd4` 原本就写对了，是我在转写成规格时引入的错误——**收编旧提交时，
不要重述它的结论，要从中抄。** 这正是不从零重写的价值：证据在提交里，不在记忆里。

**3. ���突出在 `hardware.yml`，解法是「保留 v3 的单步结构 + 在它前面插一步」。**
v3 侧把「调 MSVC + 跑 harness + 发布证据」合并成了**一步**
（`tools/run_hardware_job.py`，注释解释了为什么不能拆：MSVC 环境必须跨进构建），
而 `8d502dd4` 侧是在原来的两步之间插了一个证书步骤。直接取任一侧都错：
取 v3 侧会丢证书步骤，取 feat 侧会把已经合并的一步拆回两步。
正确解法是保留 v3 的一步，把证书步骤插在它**之前**——证书生成不依赖 MSVC 环境。

**4. 三个「提交声称归属 N1」的旁证：`Refs-Slice: v1.4-n1` 但不在 `version/v3`。**
`8d502dd4` 的 trailer 写的是 `Refs-Slice: v1.4-n1`，而 N1（`v1-turn-tls-trust`）
早已签字合入。也就是说：一个越界提交在签字时被漏掉，却在分支上留了 trailer，
看起来「有家」。收编时我确认了三件事，避免误判：
- 14 个 submodule pin 与 v3 **完全一致**（`69bebb5f` 的修复已在 v3）——
  这条不是本片的问题；
- `policy/lock.json` / `policy.yaml` 与 v3 **完全一致**——`0f2c719b` 已在 v3；
- 唯一真缺的就是 DTLS 证书这一簇（11 文件 / +601 / −42）。
**教训：`Refs-Slice` 只证明「提交者认为它属于某片」，不证明「它进了那个片」。**
核查归属要看树，不看 trailer。

**5. 本片是 v1.4 中唯一在 N9 之后仍碰 `ci.yml` 的切片，必须留痕。**
AGENTS.md §8a 规定 v1.4 的 `ci.yml` 改动集中在 N9 一次落地。本片违反这个
字面规则，但**不违反其目的**：N9 落地时，缺陷 A 尚未修，所以「新 checkout
没有证书」还没成为问题；本片让证书只存在于被忽略的目录后，**所有** DTLS job
都会因源文件缺失而红。不补这条 CI 步骤，本片本身就是不可交付的。
规格的「范围」章已写明这条理由，判据 12 要求每个生成步骤有独立 `name`，
便于事后按步骤名审计「这行是谁加的」。

**6. 判据 9（ctest 回归）不在本片验收文件里，是刻意的。**
本片不碰任何 C++ 行为（候选表只在**移除**一个坏项，TLS 校验逻辑未动），
所以验收文件只做静态断言 + 真跑一次证书脚本。判据 9 由门禁②的全量 ctest
腿覆盖——在本文件里再造一个「跑 ctest」的用例，只会得到一份不如门禁可靠的副本。
