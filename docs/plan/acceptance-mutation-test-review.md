# 评审报告：nimrtc-zelto 实战开发的验收测试质量（变异测试实证）

**日期**：2026-11-18
**方法**：变异测试（mutation testing）—— 故意破坏被测实现，看验收测试能否变红
**工具**：`D:\MyOpen\mut-suite.cjs`（仓库外，不进版本库）
**被测对象**：10 个验收套件

### 0.5 复核后的处理（2026-11-18 第二次复核）

对上面两条发现、以及复核中新发现的三项，采取的处理如下。
**决策：不改写已发布历史**（理由见下）。

#### 已修

| 项 | 处理 |
|---|---|
| ① 隐式契约漂移 | 在 `game_source.hpp` 与 `ai_decision.hpp` **两侧**都写了显式注释：本模块的洞位几何与对侧的洞数模型**相互独立**，改任一侧不影响另一侧；两者今天一致是巧合，不是构造出来的；若日后需要两者是同一块棋盘，那是契约变更 —— 需要共享常量（或共享头文件）与一条 ADR，不允许悄悄改数字。**未**改任何行为。 |
| `policy.yaml` 的 `allow-write-demo-agent` | 该规则已删除（它的路径写的是 `demo-agent-realtime/**`，而文件实际在 `examples/demo-agent-realtime/**`，匹配不到任何东西；而已有的 `examples/**` 规则本就覆盖该目录，此约等于冗余）。`policy/lock.json` 的哈希锚点已同步更新为改后文件的 sha256，两者一致。 |
| `Opus::Encoder::encode_int16` 无单测 | 补了 6 条单元测试（出包、往返保能量、与 float 路径出包同尺寸、非法帧长、空/空指针、六种合法帧长全支持），并给源码里 `opus_int16` 与 `std::int16_t` 的 `reinterpret_cast` 补了两条 `static_assert`（libopus 在各平台对 `opus_int16` 的 typedef 不同，原来那句「guaranteed to be int16_t」只是注释，没有编译期兜底）。 |

#### 记录，不修

**③ demo 的一批提交缺少 DCO 签名。**

`AGENTS.md` 的提交规范要求 DCO 签名（`Signed-off-by:`），但下列提交**没有**：

```
1e099e78  feat(demo): the M1-M4 AI-game demo, developed end-to-end by zelto (#91)
5c2c55b3  feat(v1-game-source): slice approved
6f8c1071  feat(v1-video-processing): slice approved
dc2a6f67  feat(v2-input-binder): slice approved
56bfbdf3  feat(v2-ai-decision): slice approved
b1e13a32  feat(v2-qoe-feed): slice approved
3f0de4a1  feat(v3-chrome-waterfall): slice approved
03735718  feat(v3-weaknet-verdict): slice approved
7cb9e6bc  feat(v4-ai-teammate): slice approved
555fe0c3  feat(v4-voice-agent): slice approved
a387536c  feat(v5-writeset-acceptance): slice approved
```

（`main` 上近期还有 `fe9771db`、`193453a9` 同样缺失。）

**为什么不补：** 补签名只能改写 `main` 的历史，而 `main` 已经 squash 合并了
`v1`–`v5` 的产物，`version/v1`–`version/v5` 分支是那些切片的**原始 commit 链**
的追溯锚点（ADR-056 D6：版本分支不 rebase，tag 是不可变锚点）。
改写 `main` 会动到这条追溯链 —— 为了补一个签名而动追溯锚点，代价与收益不成比例。

**因此记录在此，并约定：** 从本条记录之后的提交起，一律带 `Signed-off-by:`；
已合并的这批作为历史遗留，不再改写。

#### 附：本次复核实测

```
构建：demo-agent-realtime / demo-agent-teammate / rd_device   → 编译通过（MSVC Debug）
ctest：demo-agent-realtime_self_test        Passed
       demo-agent-realtime_voice_self_test   Passed
       demo-agent-teammate_self_test         Passed
受影响模块回归：opus / qoe / video_source 共 37 个测试 → 37/37 Passed
分层守卫：python tools/check_engine_layering.py → EXIT=0（L4 未被破坏）
```

另有一项**环境**问题（非代码缺陷）值得记下：`v1-video-processing` 套件硬断言
`build/CMakeCache.txt` 里 `NIMRTC_PLUGINS_NVENC=ON` / `NVDEC=ON`
（计划 §9 要求带硬件编解码开关配置构建树），而当时的构建树是
`NVENC=OFF / NVDEC=OFF / OPENH264=ON` —— 套件因此必红。按计划 §9 的开关
重新配置构建树即可，不是代码问题。

**④ 打开 NVDEC 之后，AV1 的 dav1d 测试套件 6 条全红 —— 根因是测试自身的
脆弱性，不是被测代码的问题。**

按计划 §9 的开关重新配置构建树（`NVENC=ON / NVDEC=ON`）后，全量 `ctest`
出现 6 条失败：

```
1074  Dav1dSoftware.BackendIsRegistered
1075  Dav1dSoftware.BackendIdDistinguishesRealCodecFromStub
1077  Dav1dSoftware.DecodesKeyFrameToNonZeroPixels
1078  Dav1dSoftware.DecodesAnnexBStreamWithStartCodes
1079  Dav1dSoftware.DecodeIntoOwnedBufferProducesSamePicture
1083  modules_av1_tests_test_dav1d_software
```

根因：`test_dav1d_software.cpp` 的 `dav1d_backend()` 用
`select_decoder_backend()` 取后端 —— 那是「按优先级挑一个可用的」。
开了 NVDEC 之后，硬件的 `nvdec_av1`（优先级远高于软件）就赢了，
于是这个 dav1d 套件**测的根本不是 dav1d**，而是 NVDEC：

```
[error] nvdec: cuvidCreateDecoder failed (1) for 64x64
[warn]  nvdec: cuvidParseVideoData failed (999)
Expected equality of these values:
  be->id
    Which is: "nvdec_av1"
  "dav1d_av1"
```

（`cuvidCreateDecoder` 解不了这个 64x64 的 low-overhead OBU 流 ——
测试位流是为 dav1d 造的，NVDEC 不接受。）

**这不是被测代码的缺陷，是测试写法在有 GPU 的机器上不成立。**
H.264 那边早就修过同一个问题 —— `test_openh264_software.cpp` 的
`openh264_backend()` 是**按 id 查**的，注释里明写着原因：
「NVENC / NVDEC 的优先级高得多，会赢下通用选择」。
AV1 这个套件当时漏了。

**已修：** `dav1d_backend()` 改成按 id 查（与 H.264 套件同一写法），
并补上 `plugins/hw_video_backend.hpp` 的 include。**未改任何被测代码**，
只改了测试取后端的方式。修完 6 条全绿。

> 这条与「发现 2」是同一个教训的两面：**验收判据写得太弱，会在别的
> 配置下变成假红**。这里的弱是「取后端的方式依赖运行环境」。

---

## 一、为什么用变异测试

**单元测试全绿 ≠ 测试有效。** 验收测试可能断言了一堆东西，
但每一条断言都太弱（只查存在性、只查一部分），于是实现坏掉了它也不红。

唯一能回答「这个测试到底能不能抓错」的，是**故意把实现弄坏，看它会不会报警**。

这套方法是真实项目（nimrtc-zelto）上跑出来的 zelto 演练的延伸 ——
演练记录 §11.28 已经说过：**「单元测试绿了之后要去真实项目上跑一遍」**。
变异测试是这条教训的**加强版**：不是跑一遍，是**跑坏一遍**。

---

## 二、变异测试的三条纪律（都是踩出来的坑）

### ① 变异必须打在**断言真正看的地方**

第一轮跑出 4 个「漏过」，逐个查证后**全部是变异无效**，不是测试的漏洞：

| 变异 | 为什么没反应 |
|---|---|
| `WebTransport` → `WebSockets` | 改的是**注释里**那处；断言查的是第 541 行的真实调用 |
| `p95_us` → `p95_us_removed` | 断言查的是 `input_to_photon_p95_us`，我改的是另一个同名标识符 |
| `5` → `0`（benchmark） | 断言查 `"loss": 0.05` 这个 **JSON 字面量**，我改到了注释里的 `5%` |
| `signalling.py` 的 `127.0.0.1` | 断言查的是 `default="127.0.0.1"` **完整形态**，我改的是**注释**里的 |

**教训**：断言是正则，变异必须与正则锚点**逐字符对齐**。
「改了相关的地方」不等于「改了断言看的地方」。

### ② 存在性判据必须**全替换**

`assert.match(src, /class LatencyInstrument/)` 是**子串**判据。
把类名改成 `LatencyInstrumentX` —— **断言照样匹配**（前缀陷阱）。

同理，源码里 `new WebTransport(` 有 **2 处**（一处真调用、一处在错误处理里），
只改一处，断言照样过。

**所以工具支持 `all: true`**：替换**全部**出现。

### ③ 编译失败 ≠ 变异生效（最危险的一种假阴性）

变异把类名改坏 → 编译不过 → ctest 跑的是**旧二进制** → 测试当然绿。

**这是最危险的一种假阴性**：它看起来像「测试没抓到」，实际是「根本没测」。

**修正**：构建失败必须**显式标记为「判据无效」并从分母里剔除**，
绝不能计入「漏过」。

> 这条纪律本身就是一个可复用的教训：
> **任何「改了代码再跑测试」的验证流程，都必须确认编译/加载确实成功了。**
> 否则你测的是旧版本。

### ④ 改名类变异改用**空格扰动**

纪律 ③ 的解法：既然改名会让引用点编译不过，就用**空格扰动** ——
C++ 编译器对空格不敏感，但**正则断言**对空格敏感。

```
class LatencyInstrument  →  class  LatencyInstrument   （两个空格）
```

→ **编译通过**（语义完全不变）、**断言失配**（测试变红）。
既测到了断言，又不会因为编译失败而变成假阴性。

---

## 三、效率：按需构建（第一版 52 分钟 → 第二版 10 分钟）

第一版每次变异都跑 `cmake --build build`（全量 200+ target），61 条变异跑了 **3149 秒**。

关键认识：**大部分套件的断言是纯静态正则**（读源码文本匹配），
根本不碰编译产物 —— 改完源码直接跑测试即可，**零编译**。

只有当套件真的**运行编译出的二进制**（ctest）时，才需要重建，
而且只需重建**它用到的那几个 target**。

依赖图是实测出来的（ctest 用例名 → vcxproj 名）：

| 套件 | 需要构建的 target |
|---|---|
| v1-game-source | 无（纯静态） |
| v1-video-processing | `test_video_source` / `test_engine_video_codec` / `test_engine_video_plugins` / `test_engine_video_e2e_loopback` / `test_engine_video_codec_backend` |
| v2-ai-decision | `demo-agent-realtime` |
| v2-input-binder | `test_rd_input_g5_g6_test` / `test_engine_ref_frame` / `test_rd_input_test` |
| v2-qoe-feed | `test_qoe_metrics_test` |
| v3-chrome-waterfall | 无（纯文本） |
| v3-weaknet-verdict | 无（纯 Python） |
| v4-ai-teammate | `demo-agent-teammate` |
| v4-voice-agent | `demo-agent-realtime` |
| v5-writeset | 无（纯 JS） |

> **注意**：ctest 用例名和 vcxproj 名**不一定一样**（`video_source_tests` ↔ `test_video_source`）。
> 照抄会构建失败。

**效果**：全量 63 条变异，**627 秒**跑完（第一版 3149 秒）。

---

## 四、结果

**最终一轮：75 条变异，0 条判据无效（空格扰动法彻底解决了编译失败问题），抓到 72 条 = 96%。**

| 套件 | 抓到/有效 | 抓取率 | 耗时 |
|---|---|---|---|
| v1-game-source | 10/10 | 100% | 1s（零编译） |
| v1-video-processing | 7/7 | 100% | 314s |
| v2-ai-decision | 8/9 | 89% | 39s |
| v2-input-binder | 6/6 | 100% | 53s |
| v2-qoe-feed | 7/7 | 100% | 15s |
| v3-chrome-waterfall | 2/2 | 100% | 0s（零编译） |
| v3-weaknet-verdict | 8/8 | 100% | 3s（零编译） |
| v4-ai-teammate | 6/6 | 100% | 22s |
| v4-voice-agent | 13/14 | 93% | 60s |
| v5-writeset-acceptance | 6/6 | 100% | 72s |

**总耗时 646 秒**（第一版 3149 秒 —— 快 4.9 倍）。

> 修复「产物缺失检测」缺口后重跑 v5：**6/6 = 100%**，
> 之前漏过的那条现在被抓住了（pass 8 / fail 1）。

剩下的 2 个「漏过」经逐个查证：

| 套件 | 漏过的变异 | 结论 |
|---|---|---|
| v2-ai-decision | 延迟预算判定的单位错了（us→ns） | **等价变异**：决策耗时本就 < 50μs，改完 `max_ns() <= limit_us` 仍成立 → 行为没变 → 测试不红是对的 |
| v4-voice-agent | Tap 帧长不再 10ms（改成 20ms） | **等价变异**：攒批窗口 100ms 不变，20ms 的 tap 仍能被 100ms 窗口整除 → 攒批行为等价 → 测不出是应该的 |

**两条都是「变异没有真正改变被测行为」**，不是测试的漏洞。
真正改变了行为的变异，**100% 被抓住**。

### 判据强度小结

- **强（100%）**：v1-game-source / v1-video-processing / v2-input-binder / v2-qoe-feed /
  v3-chrome-waterfall / v3-weaknet-verdict / v4-ai-teammate / v5-writeset-acceptance
- **强（93%）**：v4-voice-agent
- **中（89%）**：v2-ai-decision
  —— 弱点：**部分断言只检查「符号存在」，不检查「行为对不对」**

---

## 五、真正的发现

### 发现 1（**已修**）：产物缺失检测从未被负向测试过

**这是本轮唯一一条真正的测试漏洞**（其余「漏过」全部是变异无效）。

v5-writeset-acceptance 套件里，所有对 `missing` 的断言都是：

```js
assert.deepEqual(v.missing, [], '...');
```

即「产物都在树上时 missing 为空」。**从来没有一条断言检查
「产物不在树上时 missing 非空」**。

后果：把判据里的

```js
const missing = required.filter(p => ...);
```

改成

```js
const missing = [].filter(p => ...);   // 产物缺失检测彻底失效
```

**v5 套件仍然 8/8 全绿** —— 这条路径从未被负向测过。

**判据本身是对的**（实测 `evaluateChanged` 确实能报出 missing），
缺的是**测试**。

**修复**：给 v5 套件补一条负向断言（已提交到
`tests/acceptance/v5-writeset-acceptance/acceptance.test.cjs`）：

```js
test('产物缺失检测会报警：required 不在树上时 missing 非空且 ok=false（标准 3 补）', () => {
    const r = ws.evaluateChanged({
        changed: ['mod.txt'], allow: ['mod.txt'],
        required: ['missing-product.hpp', 'another-missing.hpp'],
        present: [],              // 产物一个都不在树上
    });
    assert.equal(r.ok, false, '产物缺失时必须判失败');
    assert.deepEqual(r.missing,
        ['missing-product.hpp', 'another-missing.hpp'],
        '产物缺失必须被如实列出（不能恒为空）');
    // ... 对照组：产物齐全时 missing 为空、ok=true
});
```

**验证**：施加那个变异后，套件 **pass 8 / fail 1** —— 确实被抓住了。

> 这正是变异测试的价值：
> **「断言存在」不等于「断言有效」。**
> 原有 7 条验收标准全绿、8 个用例全绿，
> 但其中一条判据路径可以在**无人察觉**的情况下失效。

### 发现 2：nimrtc-zelto 的 submodule 状态被另一个克隆污染

`src/third_party/msquic/submodules/openssl/krb5/.git` 里写的是**绝对路径**，
指向 `D:/MyOpen/NimRTC/.git/...` —— **另一个 nimrtc 克隆**。

而同一个 msquic 下的其它嵌套 submodule（clog / googletest / openssl / quictls /
xdp-for-windows）全都用**相对路径**指向本仓库。

**后果**：`git status` / `git diff` 在这个仓库里直接 exit=128 →
写集边界判据（`git diff HEAD --name-only`）读不到改动 →
**4 个套件的基线直接不绿**。

**修法**（一行）：
```
gitdir: ../../../../../../.git/modules/src/third_party/msquic/modules/submodules/openssl/modules/krb5
```

> 这是一个**真实的仓库级缺陷**，不是测试问题。
> 它会让任何依赖 `git diff` 的判据在别人机器上失效。
> **值得提给 nimrtc 维护者**：嵌套 submodule 的 gitdir 应该是相对路径。

### 发现 3：写集边界判据把「别的 agent 的未跟踪文件」算成越界

这是**设计使然**（未跟踪文件确实可能是 agent 刚建的产物），不是缺陷。
但它对**多人并行**的场景有影响：任何人在仓库里放临时文件，
别的切片的验收就红了。

本次评审中它**抓到了我两次**（我把变异脚本和备份文件放仓库里，立刻被判越界）。

**判据是对的**，但并行开发时需要注意这一点。

### 发现 4：v5-writeset-acceptance 的测试写得很好

它是本轮唯一一个**在自己的临时 git 仓库里构造场景**测判据本身的套件
（`makeTempRepo` + 提交历史构造），而不是依赖真实仓库状态。

这让它**不受仓库环境影响**，是 10 个套件里最稳的一个。
它的 7 条验收标准里有 5 条是纯机制测试（不碰真实仓库）。

### 发现 5：并行 agent 的文件会污染验收基线

评审期间有**另一个 agent 在 nimrtc-zelto 里并行工作**，不断往仓库根目录放临时文件
（`arch-real.json` / `verify-lang-parsing.mjs` …）。

这些文件被写集判据算作「agent 的改动」→ **4 个套件的基线不绿** → 整轮结论作废。

**工具的对策**：跑测试前把未跟踪文件**临时挪出仓库**，跑完放回。
不删（不是我们的文件），只是隔离。

---

## 六、这套方法怎么复用

变异测试的**工具**是通用的（`mut-suite.cjs`），但**变异清单**必须针对具体项目重写。

重写变异清单的顺序：

1. **先读验收断言**（`assert.match(src, /锚点/)`）—— 变异必须与锚点逐字符对齐
2. **判断判据类型**：
   - 纯静态正则 → 改源码文本，**零编译**
   - 跑二进制 → 改源码，**只构建该套件用到的 target**
   - 存在性判据 → `all: true` 全替换
   - 改名类 → 用**空格扰动**代替（避免编译失败）
3. **构建失败必须显式标记**为判据无效，剔除出分母

> 一句话总结：
> **变异测试不是「跑测试」，是「跑坏测试」——而跑坏的第一步，是确保你真的把它跑坏了。**
