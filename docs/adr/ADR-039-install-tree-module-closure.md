# ADR-039: 安装树链接闭包 —— 「装好的 SDK 永远链得上」

| | |
|---|---|
| Number | 039 |
| Status | **Proposed** |
| Date | 2026-11-25 |
| Phase | v1.4（打包层） |
| Bound to | v1.4.0 |
| Supersedes | — |
| Related | `docs/adr/ADR-038-install-tree-video-dependencies.md`（N6 执行腿暴露缺口，本片收尾）；`docs/adr/ADR-036-sdk-sample-line-budget.md`（N6：缺口的发现者）；`docs/adr/ADR-034-macos-install-downstream-proof.md`（N9：装得上、能被消费）；`specs/slices/v1-install-tree-module-closure.md` |

---

## 1. 背景

N6（SDK 样例）的执行腿在 ADR-037 / ADR-038 闭合后第一次真的跑起来，暴露
本片的缺口：装好的 bundle 在 `build/_install` 存在（且带 openh264）时，
下游宿主链 `nimrtc::engine` 报 **14 个未解析外部符号** —— 9 个是模块
registrar、5 个是 `pacing::TokenBucket` 的方法。

根因不是产物缺失，而是**两份手写清单漂移**：

- `_nimrtc_install_shorts`（`cmake/NimRTCInstall.cmake`）决定**装什么**；
- `_nimrtc_module_shorts`（`cmake/NimRTCConfig.cmake.in`）决定**链什么**。

当一个模块被加进引擎（或模块之间新出现依赖）而两份清单没跟上，下游宿主
就在**链接期**撞出 LNK2019 / LNK2001 —— 而且只在真实消费时才发现，构建
树里一切正常。本 ADR 记录：**闭包是什么 / 两份清单怎么同步 / 为什么不自动
发现模块 / 为什么补不减**。

## 2. 决策

### 2.1 决策一：闭包是什么 —— 一个可计算的集合

「引擎链接闭包」= 下游宿主链 `nimrtc::engine` 时必须出现在同一串里的全部
`nimrtc_*` **静态 archive** 集合。它是三个来源的不动点：

1. **引擎的 PRIVATE 链接** —— `src/engine/CMakeLists.txt` 里所有
   `target_link_libraries(nimrtc_engine PRIVATE nimrtc::X)` 的 `X`；
2. **每个模块自己的链接** —— 只读 `src/modules/<name>/CMakeLists.txt` 里
   `target_link_libraries(... nimrtc::X ...)` 的 `X`；
3. **递归展开** —— 第 2 步拉进来的模块，它自己的依赖也算进来，直到不动点。

排除 `core` / `plugins` / `vendor`（`core` 是 header-only 基础库、`plugins`
是 header-only 接口集、`vendor` 是 vendored 三方归档的命名空间，都不是模块
archive）。对本仓库树，闭包实测 **42 个模块**。

闭包是**清单的契约**：`install_shorts` 必须覆盖它（漏 = 装漏）、
`module_shorts` 必须覆盖它（漏 = 链漏）。工具判的正是这两个集合差。

### 2.2 决策二：两份清单怎么同步 —— 机器守着手写清单

`_nimrtc_install_shorts` 与 `_nimrtc_module_shorts` 保持**手写**（历史原因：
构建树用 ALIAS / OBJECT 目标，不能便携导出；装好的 config 以
`find_library` 为消费契约 —— 见 `NimRTCConfig.cmake.in` 头部注释）。
漂移不可避免，所以新增 `tools/check_install_tree_module_closure.py`：

- **只读仓库树**即可判定缺口 —— 不需要构建、不需要暂存安装树。缺口是
  **清单**的缺口，不是**产物**的缺口。
- 报缺口时**说清是哪个模块**、**在哪份清单**里缺
  （如 `config list missing closure module: pacing (_nimrtc_module_shorts in
  cmake/NimRTCConfig.cmake.in)`）。
- **不触网、不调模型、判定确定性**（同一棵树跑两次结果一致）。
- 自测（`check_install_tree_module_closure_self_test.py`）用合成树**故意造出
  缺口**并断言工具报红 —— 工具不可能「永远绿」。

分工：工具判**「清单覆盖闭包」**（静态，本片负责）；「产物这次构建出来
没有」由安装规则自己的 `if(TARGET ...)` / `find_library()` 跳过逻辑负责。
一个模块在 module-OFF 构建里不存在是**正常的**，不该报红 —— 清单里的名字
是候选，安装与链接规则各自按自己的方式跳过不存在的。

### 2.3 决策三：为什么补不减 —— 闭包之外的名字不是冗余

`module_shorts` 里闭包之外的模块（`log` / `registry` / `ice` / `dtls` /
`srtp` / `transport` / `web_transport` …）**不是冗余**：宿主可能直接用
`nimrtc::ice`（虽然接缝纪律不许，但那是宿主的选择），而且 `engine` 的
PUBLIC 链接 `nimrtc::core` 的传递依赖里有它们。所以本片只**补缺口**，
不动已有的项。

本片补进两份清单的模块（对应那 14 个符号）：

- `pacing` —— `nimrtc_sched` 链 `nimrtc::pacing` PRIVATE，而它之前只在
  install 清单里、不在 config 清单里；
- `dtmf` / `video_bitrate_control` / `video_capture` / `video_sync` /
  `audio_device` / `rd_input` / `rd_qoe`（engine PRIVATE，registrar 被
  `kDefaultRegistrars[]` 引用）；
- `av1` / `h265` / `vp9`（与 `h264` 一样 PUBLIC 链 `nvcodec`）。

补进去的**位置**有讲究（GNU 静态链接器从左往右扫，archive 只扫一次）：
`video_capture` 链 `video_frame` 所以排在它前面；`pacing` 在 `sched` 之后；
`av1` / `h265` / `vp9` 在 `nvcodec` 之前（同 `h264`）；`video_sync` 跟在
`video_sink` 后，与 engine 的分组一致。MSVC 忽略 archive 顺序，这个顺序在
Windows 上是 no-op —— 修法对所有工具链无害（同 ADR-038 §2.3 的理由）。

### 2.4 决策四：为什么不做「自动发现模块」

自动发现（扫 `src/modules/*/CMakeLists.txt` 生成清单）听起来更治本，但它
让 `NIMRTC_MODULE_*` 开关的语义变复杂：`if(TARGET ...)` 的跳过逻辑是
**构建期**的，清单是**源码期**的，两者要合并就得在配置期跑一遍目标探测。

本片选**更小但够用**的方案：**清单照旧手写，但机器守着它**。缺口会被
工具**立刻报红**，而不是等到下游宿主 LNK2019 才发现。这满足「约束优先于
功能」：先让漂移**可见**，根治（自动发现）另开一片（见规格「遗留」）。

### 2.5 决策五：闭包里的**系统库**也要接回（2026-11-25 实施中的发现）

把 `video_capture` 补进闭包后，下游宿主立刻撞上 12 个 LNK2019
（`MFStartup` / `MFCreateMediaType` / `MFVideoFormat_NV12` …，全部来自
`nimrtc_video_capture.lib(mf_camera.obj)`）。

根因与 ADR-038 的 `d3d11`/`dxgi` **同形**，只是深一层：闭包模块自己
PRIVATE 链的 **Windows 系统库**在安装后也丢了。源侧
`src/modules/video_capture/CMakeLists.txt` 的 WIN32 分支链的是

```
d3d11 dxgi windowsapp mf mfplat mfreadwrite mfuuid
```

安装树 config 只接回了 `d3d11 dxgi`。同理 `audio_device`（闭包模块，
engine PRIVATE）PRIVATE 链的 `ole32`（WASAPI）也没接回。

**决策**：链接闭包 = 「哪些 `nimrtc_*` archive」**+**「这些 archive 引用了
哪些系统库」。安装树必须把**闭包模块自己的 PRIVATE 系统库**也接回最终
链接行，否则「装出来的 bundle 一定链得上」这句承诺是假的。

**动作**：在 `cmake/NimRTCConfig.cmake.in` 的 WIN32 分支补
`windowsapp mf mfplat mfreadwrite mfuuid ole32`。既有项一个没动、没删、
没改（`ws2_32 bcrypt iphlpapi winmm crypt32 d3d11 dxgi` 原样保留）——
这既守住 §3 的「既有链不动」，也保住了 ADR-038 的验收断言（它只断言这些
名字**在**，不断言**只有**这些）。

**边界**：本片的工具仍只判**模块 archive** 的闭包覆盖。系统库这一层的
漂移目前还是靠下游宿主撞 LNK2019 才发现 —— 与「两份清单漂移」同类、
同类治法（机器守着），值得另开一片把「闭包模块的 PRIVATE 系统库」也算进
工具的判定范围。

## 3. 后果

- 安装树里 `nimrtc::engine` 对**任何**闭包内模块都能链出下游宿主
  （全闭包宿主 0 个 LNK2019）—— 预修复 bundle 的 14 个未解析符号消失。
- 两份手写清单的漂移从「只有下游消费时才炸」变成「工具每次 CI 都报红」。
- `src/engine/` 与 `src/modules/**` **不碰**：引擎的链接声明是**闭包的
  定义源**，本片只读它，补清单是打包层的事。
- 音频 / 基础路径的既有链（vendor 名、APM 清单、openh264 清单）
  **不动** —— ADR-038 的验收套件仍然绿。
- **系统库**既有项也不动；只**补**闭包模块需要的（`mf` 组 + `ole32`），
  理由见 §2.5。

## 4. 验证

- `tools/check_install_tree_module_closure.py` 对仓库树跑绿、退出码 0，
  闭包覆盖两份清单、闭包里含 `pacing`；同一棵树连跑两次输出一致；
  源码无网络 / 模型 / 随机 / 时间依赖。
- `tools/check_install_tree_module_closure_self_test.py` 对合成树的
  负样本（install 缺 `pacing` / config 缺 `video_jb`）报红并点名，
  正样本与空闭包不误报。
- `tests/acceptance/v1-install-tree-module-closure/acceptance.test.cjs`
  真的暂存一个干净 prefix、检查 11 个闭包 archive 在树里、真的链一个
  全闭包下游宿主、断言 0 个 LNK2019，且只用 bundle 里的东西；
  `src/engine/` 与 `src/modules/**` 不在本片的改动里。
