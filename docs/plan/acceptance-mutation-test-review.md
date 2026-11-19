# 评审报告：AI 游戏 Demo（M1–M4）代码评审 + 验收测试质量（变异测试实证）

**日期**：2026-11-18
**对象**：AI 游戏 Demo 的实现代码（M1–M4）与守着它的 10 个验收套件
**方法**：
1. **代码评审** —— 逐模块读实现 + 跑全量 `ctest`；
2. **变异测试**（mutation testing）—— 故意破坏被测实现，看验收测试能否变红。

**工具**：`D:\MyOpen\mut-suite.cjs`（仓库外，不进版本库）
**被测对象**：10 个验收套件，61 条变异

---

## 〇、AI 游戏 Demo（M1–M4）代码评审

> 这部分是本次评审的正题：变异测试是**验收测试质量**的度量，
> 而 AI 游戏 Demo 是被这些验收测试守着的那批代码本身。

### 0.1 代码已经在主干上

先澄清一件事，避免重复劳动：

`feat/demo-m1-m4-v2` 分支（本地 + 远端）上的 M1–M4 成果
（`examples/demo-agent-realtime/**`、`examples/remote-desktop/game_source.hpp`、
`interop/chrome/demo_waterfall.html`，共约 7000 行）
**已经通过 PR #91 落进 `main`**，逐字节一致。

验证方式：

```bash
# 分支相对 main 没有任何新增文件
git diff --name-status --diff-filter=A origin/main origin/feat/demo-m1-m4-v2
# （无输出）

# demo 代码逐字节一致
git diff origin/main origin/feat/demo-m1-m4-v2 -- examples/demo-agent-realtime/ \
  examples/remote-desktop/game_source.hpp interop/chrome/demo_waterfall.html
# （无输出）
```

反过来，分支相对 `main` **少**约 16500 行（v1.2 / v1.3 的引擎工作：
rate window、transport stats、QoE、benchmark、release 工具链）。
也就是说该分支是一个**过期快照**，不是新工作 —— 它缺 `main` 上更新的
`v5-writeset-acceptance` 判据实现（D-68，PR #102）。

**结论：不需要再合一次。** 该分支可以直接删掉。

### 0.2 代码质量评价：好

| 维度 | 评价 |
|---|---|
| 自包含性 | `ai_decision.hpp` / `ai_teammate.hpp` 零 NimRTC 链接，纯 C++20 算术，headless 可跑 |
| 确定性 | 专家 oracle、地鼠生成、策略切换全部确定性，无随机源 |
| 可插拔 | `IASR` / `ILLM` / `ITts` 三个运行时接口，M1 的 mock 是默认实现，缺模型时**明确报错**而不是静默降级 |
| 帧锚定 | `ai_decision` JSON 严格按 plan §5.2 冻结格式产出 `frame_seq` / `capture_ts_us` / `age_us_at_decision` |
| 失败路径 | 模型不可用时**默认不动**（`no_silent_substitute`），错误被上报并计数 |

`v4-ai-teammate` 与 `v4-voice-agent` 两个套件的变异测试抓取率 **100%**
（12/12 与 6/6），从测试侧佐证了这两块的实现质量。

### 0.3 自测结果

三个自测全部通过（`ctest`，MSVC Debug，`Warnings→errors: ON`）：

```
demo-agent-realtime_self_test       Passed   (100 次决策 / 延迟仪表 / 策略切换 / 决策轨迹)
demo-agent-realtime_voice_self_test  Passed   (Tap 攒批 / 流式 LLM / Opus TTS / barge-in 10ms)
demo-agent-teammate_self_test        Passed   (白名单 / 帧锚定 / 时光作证 / 模型不可用)
```

全量 `ctest`：**2140 / 2140 通过，0 失败**（540 秒）。

### 0.4 评审发现

**① `kHoleColumns` 与棋盘格数的耦合是「隐式契约」，值得写注释。**

`game_source.hpp` 的洞位几何完全由 `kHoleColumns` / `kHoleRows` 参数化
（`cx = w * (2c+1) / (2 * kHoleColumns)`），改列数不会越界、不会崩。
但 `ai_decision.hpp` 那边是**另一套** 3 洞模型（`kHoles = 3`，
left / center / right）—— 两个模块**不互相链接**，所以编译期无约束。

后果：把棋盘改成 4 列，渲染层默默变了，而 AI 侧的「3 个洞」模型
没有任何东西提醒它。**这是两个模块之间的隐式契约漂移。**

建议（未实施，属于契约变更，需走 ADR）：要么把洞数做成两侧共享的常量，
要么在 `game_source.hpp` 里显式注释「本模块的洞数与 `ai_decision.hpp` 的
`kHoles` 独立，改这里不影响 AI 决策侧」。

**② `rd_device` 的 `--game-source` 开关是单向的，没有关掉的路。**

复核时出现过一个 `game_source` 默认值被改成 `true` 的改动（实测确认：
不带参数直接跑 `rd_device.exe`，`[rd_device] ready (capture=game-source ...)`）。
如果这样的改动被合进去：

- `rd_device` 就**再也不能采集真实屏幕**了 —— 它是远控链路的采集端，
  `lan-test-runbook.md`、`tools/rd_pair.py`、绿色部署包都依赖它采真实屏幕；
- `--capture windows-wgc` 会被静默忽略（游戏源分支先于 `capture_name` 生效）；
- `--game-source` 标志变成空操作，且**没有任何办法把游戏源关掉**。

结论：**`game_source` 必须保持默认 `false`**。当前 `main` 上是 `false`，
是正确的。

---

## 一、为什么用变异测试

**单元测试全绿 ≠ 测试有效。** 验收测试可能断言了一堆东西，
但每一条断言都太弱（只查存在性、只查一部分），于是实现坏掉了它也不红。

唯一能回答「这个测试到底能不能抓错」的，是**故意把实现弄坏，看它会不会报警**。

这套方法是真实项目（nimrtc-zelto）上跑出来的 zelto 演练的延伸 ——
演练记录 §11.28 已经说过：**「单元测试绿了之后要去真实项目上跑一遍」**。
变异测试是这条教训的**加强版**：不是跑一遍，是**跑坏一遍**。

---

## 二、变异测试的三条纪律（踩过的坑）

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
把类名改成 `LatencyInstrumentX` —— **断言照样匹配**。

同理，源码里 `new WebTransport(` 有 **2 处**（一处真调用、一处在错误处理里），
只改一处，断言照样过。

**所以工具支持 `all: true`**：替换**全部**出现。

### ③ 编译失败 ≠ 变异生效（最危险的一种假阴性）

第一轮日志里反复出现 `[构建失败]` —— 变异把类名改坏，编译不过，
于是 ctest 跑的是**旧二进制**，测试当然绿。

**这是最危险的一种假阴性**：它看起来像「测试没抓到」，实际是「根本没测」。

**修正**：构建失败必须**显式标记为「判据无效」并从分母里剔除**，
绝不能计入「漏过」。工具现在这么做。

> 这条纪律本身就是一个可复用的教训：
> **任何「改了代码再跑测试」的验证流程，都必须确认编译/加载确实成功了。**
> 否则你测的是旧版本。

---

## 三、结果

**总计：抓到 45 / 56，跳过 5（锚点未找到 = 变异无效，不计入分母），抓取率 80%。**

| 验收套件 | 抓到 | 抓取率 | 漏过的变异 |
|---|---|---|---|
| `v1-game-source` | 6/7 | 86% | 设备 id 不再是 `"game-source"` |
| `v1-video-processing` | 1/1 | 100% | （无） |
| `v2-ai-decision` | 4/7 | 57% | 事件总线类别；专家 oracle 基准 |
| `v2-input-binder` | 3/5 | 60% | — |
| `v2-qoe-feed` | 4/5 | 80% | viewer 不再投喂帧呈现事件 |
| `v3-chrome-waterfall` | 1/2 | 50% | 瀑布页不再用 WebTransport（改真实调用点） |
| `v3-weaknet-verdict` | 5/7 | 71% | p95 未接进 verdict 的格式化输出；harsh 档位「100ms 抖动被改 0」 |
| `v4-ai-teammate` | 6/6 | 100% | （无） |
| `v4-voice-agent` | 12/12 | 100% | （无） |
| `v5-writeset-acceptance` | 3/4 | 75% | 产物检查失效（required 永远算「在树上」） |

### 3.1 结论

**测试是有效的，但有两个套件明显偏弱：`v2-ai-decision`（57%）与
`v3-chrome-waterfall`（50%）。**

- **v2-ai-decision** 的三条漏过全部指向同一类弱点：断言查的是**存在性**
  （类在不在、字段在不在），不是**语义**（这个统计量算得对不对）。
  「p50/p95 统计」「max 统计」两条被抓住，是因为它们连存在性都保不住；
  而「专家 oracle 基准」和「事件总线类别」被漏过，说明
  「断言查错了地方」比「断言太弱」更致命。
- **v3-chrome-waterfall** 的漏过（改真实调用点却仍绿）说明网页侧的断言
  查的是**注释或另一处同名标识符**，不是真实调用点。
- **v4-ai-teammate 与 v4-voice-agent 拿到 100%** —— 这两个套件是写得最好的：
  它们断言的是**行为**（白名单拒绝、攒批窗口、Opus 编码路径），
  不是符号的存在。这正是应该抄的写法。

### 3.2 建议的下一步

1. **v2-ai-decision** 补一条**语义**断言：跑 100 次决策，
   断言 p95 ≤ 50ms **且** `remaining_ticks` 与 oracle 的选择序列逐次一致
   （而不是只查 `LatencyInstrument` 这个类存在）。
2. **v3-chrome-waterfall** 的 WebTransport 断言改为**锚定真实调用点**
   （例如断言 `new WebTransport(` 出现在 `<script>` 块内且其后跟着
   `await`/`open()` 调用），避免改注释或改另一处同名标识符就蒙混过关。
3. `v5-writeset-acceptance` 的「产物检查」补一条**否定**断言：
   把 required 指向一个不存在的文件，断言检查会失败。
4. 把「变异必须与断言锚点逐字符对齐」这条纪律写进
   `tests/acceptance/` 的 README —— 现在它只存在于本报告里，
   下一个写验收测试的人不会看到。

---

## 四、附带发现

### 发现 1：nimrtc-zelto 的 submodule 状态被另一个克隆污染（已修复，现为相对路径）

`src/third_party/msquic/submodules/openssl/krb5/.git` 里曾经写的是**绝对路径**，
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

> **现状（2026-11-18 复核）**：该路径**已改为相对路径**，与其它嵌套 submodule 一致，
> `git status` / `git diff` 恢复正常。这个缺陷已经不存在了。
>
> 保留这条记录是因为它有复用价值：**嵌套 submodule 的 gitdir 必须是相对路径**。
> 绝对路径会让任何依赖 `git diff` 的判据在别人机器上失效，
> 而且症状（`git diff` 直接 exit=128）离病因很远 —— 中间还隔着
> 「嵌套 submodule 的 gitdir 在 submodule 的 .git 文件里」这一层。
> 排查这类问题时先查 `.git` 文件的 `gitdir:` 行。

### 发现 2：写集边界判据把「别的 agent 的未跟踪文件」算成越界

这是**设计使然**（未跟踪文件确实可能是 agent 刚建的产物），不是缺陷。
但它对**多人并行**的场景有影响：任何人在仓库里放临时文件，
别的切片的验收就红了。

本次评审中它**抓到了我两次**（我把变异脚本和备份文件放仓库里，立刻被判越界）。

**判据是对的**，但并行开发时需要注意这一点。

### 发现 3：v5-writeset-acceptance 的测试写得很好

它是本轮唯一一个**在自己的临时 git 仓库里构造场景**测判据本身的套件
（`makeTempRepo` + 提交历史构造），而不是依赖真实仓库状态。

这让它**不受仓库环境影响**，是 10 个套件里最稳的一个。
它的 8 条用例里有 3 条是纯机制测试（不碰真实仓库）：
「判据三源并集」「越界检测不削弱」「判据确定性」。

> 它的漏过只有一条（见结果表），也正是唯一一条依赖真实仓库状态的用例
> ——「真实仓库：四片产物在树上」那条的**产物检查**分支。
> 纯机制的三条全部被抓住。这不是巧合：**测判据本身的用例，
> 比测真实仓库状态的用例更难蒙混过关**，因为前者可以自己造场景。
