# Slice: v1-install-tree-module-closure

## Summary

**让暂存安装树链上引擎链接闭包里的每一个模块 —— 少一个就是一个 LNK2019。**

N6（SDK 样例）的执行腿在两个引擎缺口（ADR-037 TURN/TLS 信任、ADR-038 安装树视频依赖）闭合后**第一次真的跑起来**，随即撞上第三个缺口：

```
LNK2001: 无法解析的外部符号 "void __cdecl nimrtc::h265::register_default_plugins(void)"
LNK2001: 无法解析的外部符号 "void __cdecl nimrtc::av1::register_default_plugins(void)"
LNK2001: 无法解析的外部符号 "void __cdecl nimrtc::vp9::register_default_plugins(void)"
LNK2001: 无法解析的外部符号 "void __cdecl nimrtc::video_capture::register_default_plugins(void)"
LNK2001: 无法解析的外部符号 "void __cdecl nimrtc::video_bitrate_control::register_default_plugins(void)"
LNK2001: 无法解析的外部符号 "void __cdecl nimrtc::rd_input::register_default_plugins(void)"
LNK2001: 无法解析的外部符号 "void __cdecl nimrtc::rd_qoe::register_default_plugins(void)"
LNK2001: 无法解析的外部符号 "void __cdecl nimrtc::video_sync::register_default_plugins(void)"
LNK2001: 无法解析的外部符号 "void __cdecl nimrtc::dtmf::register_default_plugins(void)"
LNK2019: 无法解析的外部符号 "nimrtc::pacing::TokenBucket::set_config(...)"
LNK2019: 无法解析的外部符号 "nimrtc::pacing::TokenBucket::available(...)"
LNK2019: 无法解析的外部符号 "nimrtc::pacing::TokenBucket::try_consume(...)"
LNK2019: 无法解析的外部符号 "nimrtc::pacing::TokenBucket::refund(...)"
LNK2019: 无法解析的外部符号 "nimrtc::pacing::TokenBucket::reset(...)"
LNK1120: 14 个无法解析的外部命令
```

根因不是「漏了几个库」，而是**两份手工维护的清单不同步**：

| 清单 | 位置 | 职责 |
|---|---|---|
| `_nimrtc_install_shorts` | `cmake/NimRTCInstall.cmake` | 决定**装什么** archive |
| `_nimrtc_module_shorts` | `cmake/NimRTCConfig.cmake.in` | 决定**链什么** archive |

这两份清单**没有任何机制保证它们覆盖引擎的链接闭包**。ADR-038 补 `nvcodec` 时只往两份清单各加了一行 —— 那治的是**症状**，没治**病**。本片治的是病。

## 目标

**让「装出来的 bundle 一定链得上」成为可验证的事实，而不是一份要人肉同步的清单。**

两件事必须同时成立：

1. **闭包对齐** —— `install shorts` 与 `config shorts` 覆盖引擎链接闭包里的每一个模块。
2. **闭包被机器守着** —— 一条校验工具算闭包、比对两份清单、报出缺口；**不能只有一个「永远绿」的检查**。

**为什么闭包是判据而不是清单**：清单是手写的，手写的东西会漂。引擎加了新模块 → 忘了加清单 → 下游宿主 LNK2019，而**引擎自己的树内测试全绿**（树内链接用的是 CMake target，不是这两份清单）。安装树的问题只有下游能发现，所以判据必须站在下游那一侧。

### 现状核实（写规格时的树状态）

**引擎链接闭包**（`src/engine/CMakeLists.txt` 的 PRIVATE 链接 + 递归展开每个模块自己的 `target_link_libraries`，去掉 `core` / `plugins` / `vendor`）：

```
audio_device audio_rx av1 dtmf h264 h265 net_feedback nvcodec opus rd_input
rd_qoe rtp rtx sdp session video_bitrate_control video_capture video_frame
video_jb video_payload video_pipeline video_rtcp_feedback video_sdp_attributes
video_sink video_source video_stats video_sync vp9
```

（28 个模块）

**闭包 ∩ 安装清单的缺口**：

| 清单 | 缺失模块 |
|---|---|
| `_nimrtc_install_shorts`（42 项） | `av1` `dtmf` `h265` `video_bitrate_control` `video_capture` `video_sync` `vp9` |
| `_nimrtc_module_shorts`（36 项） | `audio_device` `av1` `dtmf` `h265` `rd_input` `rd_qoe` `video_bitrate_control` `video_capture` `video_sync` `vp9` |

**两份清单本身也不同步**：`install shorts` 有而 `config shorts` 没有的：`engine` `pacing` `audio_device` `rd_input` `rd_qoe` `rd_session`。其中 `pacing` 正是 LNK2019 报出的 `TokenBucket` 所在模块 —— **`nimrtc_sched` 链 `nimrtc_pacing`，但 `pacing` 只在安装清单里、没在链接清单里**，所以静态链接链上找不到它。

### 与其它片的边界（本片**不做**）

- **不做** 改引擎（`src/engine/` 一行不动）—— 缺口在**打包层**，不在引擎层。
- **不做** 改任何模块的 `src/modules/<name>/CMakeLists.txt` —— 本片只动**安装层与配置层**。
- **不做** 删掉「闭包之外」的模块（`log` / `registry` / `ice` / `dtls` / `srtp` / `transport` 等在闭包外但引擎**间接**用到或宿主可能直接用）—— 本片只**补**，不**减**。
- **不做** 引入「从目录自动发现模块」的机制 —— 那会让 `if(TARGET ...)` 的模块开关语义变复杂，是另一片的事（见「遗留」）。
- **不做** 重新签字 ADR-038 —— 它已签字，其范围内的东西（`nvcodec` / openh264 / `d3d11` / `dxgi`）本片不动。

## 设计

### 闭包怎么算（工具的核心）

工具读三个来源，算出「引擎链接闭包」：

1. **引擎的 PRIVATE 链接** —— `src/engine/CMakeLists.txt` 里所有
   `target_link_libraries(nimrtc_engine PRIVATE nimrtc::X)` 的 `X`。
2. **每个模块自己的链接** —— 只读 `src/modules/<name>/CMakeLists.txt` 里
   `target_link_libraries(... nimrtc::X ...)` 的 `X`。
3. **递归展开** —— 第 2 步拉进来的模块，它自己的依赖也要算进来，直到不动点。

排除掉 `core` / `plugins` / `vendor`（这三个不是模块 archive：`core` 是
INTERFACE 头文件库、`plugins` 是头文件接口、`vendor` 是三方归档的命名空间）。

得到闭包 `C`（实测 28 个模块）之后：

- `C \ install_shorts` = **装漏了** → 报红
- `C \ config_shorts` = **链漏了** → 报红

**工具判的是「清单覆盖闭包」（静态），不是「产物存在」（动态）。** 两者的分工：

| 判据 | 回答的问题 | 谁负责 |
|---|---|---|
| 清单覆盖闭包 | 清单**写对**了吗 | 本片的工具（不需要构建） |
| 产物存在 | 那个模块**这次构建出来**了吗 | 安装规则自己的 `if(TARGET ...)` / `find_library()` 跳过逻辑 |

一个模块在 module-OFF 构建里不存在是**正常的**，不该报红 —— 所以清单里
的名字是**候选**，安装与链接规则各自按自己的方式跳过不存在的。

### 为什么清单是「补」不是「重写」

`config shorts` 里闭包之外的模块（`log` / `registry` / `ice` / `dtls` /
`srtp` / `transport` / `web_transport` …）**不是冗余** —— 宿主可能直接用
`nimrtc::ice`（虽然接缝纪律不许，但那是宿主的选择），而且 `engine` 的
PUBLIC 链接 `nimrtc::core` 的传递依赖里有它们。所以本片只**补缺口**，
不动已有的项。

### 补进去的顺序

静态链接链是**从左往右**解析的。补进去的新模块**放在哪里**有讲究：

- **模块之间**（`av1` / `h265` / `vp9` / `dtmf` / `video_capture` /
  `video_sync` / `video_bitrate_control`）—— 它们**互不依赖**
  （`av1`/`h265`/`vp9` 只依赖 `video_frame` / `video_payload` / `nvcodec`，
  而那三个已经在链上），所以顺序不敏感。但**必须**在 `engine` **之后**
  （`engine` 是链的起点），且在依赖它们的模块**之后**。
- **`pacing`** —— `nimrtc_sched` 链 `nimrtc::pacing`，所以 `pacing` 必须在
  `sched` **之后**（GNU 静态链接器从左往右扫，`sched` 的 object 被拉进来时
  才会引用 `TokenBucket` 的符号，`pacing` 在后面才能解析它）。
- **`audio_device` / `rd_input` / `rd_qoe`** —— 与 `engine` 无依赖关系，
  但它们各自的 registrar 在只读 `src/core/src/pal_default_registrars.cpp` 里被引用，
  所以只要在链上即可。

**具体位置**：`install shorts` 与 `config shorts` 各自按「`sched` 之后、
`engine` 之后、其余按字母序插入既有列表」的规则补。

### 为什么不做「自动发现模块」

自动发现（扫 `src/modules/*/CMakeLists.txt` 生成清单）听起来更治本，但它
让 `NIMRTC_MODULE_*` 开关的语义变复杂：`if(TARGET ...)` 的跳过逻辑是
**构建期**的，清单是**源码期**的，两者要合并就得在配置期跑一遍目标探测。

本片选**更小但够用**的方案：**清单照旧手写，但机器守着它**。缺口会被
工具**立刻报红**，而不是等到下游宿主 LNK2019 才发现。这满足计划的
「约束优先于功能」：先让漂移**可见**，根治（自动发现）另开一片。

## 范围

- `cmake/NimRTCInstall.cmake`（**可写**：`_nimrtc_install_shorts` 补齐闭包缺失模块）
- `cmake/NimRTCConfig.cmake.in`（**可写**：`_nimrtc_module_shorts` 补齐闭包缺失模块）
- `tools/check_install_tree_module_closure.py`（**新增**：闭包校验工具）
- `tools/check_install_tree_module_closure_self_test.py`（**新增**：工具自测）
- `tests/acceptance/v1-install-tree-module-closure/acceptance.test.cjs`（**新增**：本片验收）
- `docs/adr/ADR-039-install-tree-module-closure.md`（**新增**：本片 ADR）
- `docs/api/engine_api.md`（**可写**：只记安装树怎么用）
- `CHANGELOG.md`（**可写**）
- `specs/slices/v1-install-tree-module-closure.md`（**可写**）

**只读**（不许改）：

- 只读 `src/engine/CMakeLists.txt` —— 引擎的链接声明是**闭包的定义源**，本片只读它、不改它
- 只读 `src/engine/`
- 只读 `src/modules/` —— 每个模块自己的 `target_link_libraries` 只读
- 只读 `cmake/NimRTCInstall.cmake` 的**安装规则本体**（`install(TARGETS ...)` / `install(DIRECTORY ...)`）—— 本片只改清单，不改规则
- 只读 `src/third_party/`
- 只读 `tests/acceptance/v1-sdk-sample/acceptance.test.cjs` —— N6 的验收文件本片不动
- 只读 `tests/acceptance/v1-install-tree-video-deps/` —— ADR-038 的验收文件本片不动
- 只读 `docs/adr/ADR-038-install-tree-video-dependencies.md`

## 需���

1. `_nimrtc_install_shorts` 覆盖引擎链接闭包里的**每一个**模块。
2. `_nimrtc_module_shorts` 覆盖引擎链接闭包里的**每一个**模块。
3. 两份清单**都**覆盖 `pacing`（`nimrtc_sched` 链它；ADR-038 没碰过它，但 ADR-038 的证据里 `pacing` 已经在安装清单里）。
4. `tools/check_install_tree_module_closure.py` 存在，对仓库树**跑绿**、退出码 0。
5. 该工具**自测**（对故意造出缺口的清单**必须**报红）—— 不能只有一个「永远绿」的检查。
6. 该工具**只读**仓库树即可判定缺口（不需要先构建 / 不需要暂存安装树）—— 缺口是**清单**的缺口，不是**产物**的缺口。
7. 该工具报出缺口时**说清是哪个模块**、在**哪份清单**里缺。
8. 工具**不触网**、**不调模型**、判定**确定性**（同一棵树跑两次结果一致）。
9. 安装树里 `nimrtc::engine` **能**被一个**带视频**的宿主链接 —— **0 个 LNK2019**（沿用 ADR-038 的证明方式，回归不破）。
10. 音频 / 基础路径**不回归**（现有的安装 + 下游证明 CI 腿仍然绿）。
11. `src/engine/` **不**被修改。
12. `src/modules/**` **不**被修改。
13. `src/engine/include/` 的公开面**不**变（公开头集合不变）。
14. `docs/adr/ADR-039-install-tree-module-closure.md` 存在，记录「闭包是什么 / 两份清单怎么同步 / 为什么不自动发现」。
15. `docs/api/engine_api.md` 记录安装树怎么用（不写引擎 API 的新东西）。
16. `CHANGELOG.md` 记录本片。

## 验收标准

### 一、闭包对齐

1. `tools/check_install_tree_module_closure.py` 存在且跑绿、退出码 0。
2. 工具算出的引擎链接闭包里，**没有一个**模块被 `_nimrtc_install_shorts` 漏掉。
3. 工具算出的引擎链接闭包里，**没有一个**模块被 `_nimrtc_module_shorts` 漏掉。
4. 闭包里**含** `pacing`（`nimrtc_sched` → `nimrtc::pacing`）。
5. 工具报出缺口时**说清是哪个模块**、**在哪份清单**里缺。
6. 工具对**故意缺一个模块**的清单 → **退出码非零**，并点名那个模块。

### 二、工具的自测（不能只有「永远绿」）

7. `tools/check_install_tree_module_closure_self_test.py` 存在且跑绿。
8. 自测里：故意从 `_nimrtc_install_shorts` 拿掉一个闭包模块 → 工具**报红**并点名它。
9. 自测里：故意从 `_nimrtc_module_shorts` 拿掉一个闭包模块 → 工具**报红**并点名它。
10. 自测里：两份清单都完整 → 工具**报绿**。
11. 自测里：闭包**空**（引擎不链任何模块）→ 工具**报绿**且**不**误报。

### 三、确定性

12. 同一棵树连跑两次 → 退出码与逐条输出**一致**。
13. 工具源码里**没有**网络调用、没有模型调用、没有随机、没有时间依赖。

### 四、产物与回归

14. 暂存安装树里 `nimrtc::engine` **能**被一个**带视频**的宿主链接 —— **0 个 LNK2019**。
15. 现有的安装 + 下游证明 CI 腿**仍然绿**。
16. `src/engine/**` **不**被修改（静态检查）。
17. `src/modules/**` **不**被修改（静态检查）。
18. `src/engine/include/` 的公开面**不**变（公开头集合不变）。
19. ADR-038 的验收套件**仍然绿**（本片不破它的证据）。

### 五、文档

20. `docs/adr/ADR-039-install-tree-module-closure.md` 存在，说明「闭包是什么 / 两份清单的关系 / 为什么不自动发现模块」。
21. `docs/api/engine_api.md` 记录安装树怎么用。
22. `CHANGELOG.md` 记录本片。

## 本片不做

- **不做** 改引擎 —— 缺口在打包层。
- **不做** 改模块的 `src/modules/<name>/CMakeLists.txt` —— 本片只动安装层与配置层。
- **不做** 从目录**自动发现**模块来生成清单 —— 那会让模块开关（`if(TARGET ...)`）的语义复杂化，是另一片的事。
- **不做** 删掉闭包之外的模块 —— 只补不减。
- **不做** 重新签字 ADR-038。
- **不做** 改 N6 的验收文件。

## 接口变更

**无 C++ 接口变更。** 本片只改打包层、加工具与验收。

| 文件 | 变化 |
|---|---|
| `cmake/NimRTCInstall.cmake` | `_nimrtc_install_shorts` 补 8 个闭包模块 |
| `cmake/NimRTCConfig.cmake.in` | `_nimrtc_module_shorts` 补 10 个闭包模块 |
| `tools/check_install_tree_module_closure.py` | **新增** |
| `tools/check_install_tree_module_closure_self_test.py` | **新增** |
| `tests/acceptance/v1-install-tree-module-closure/acceptance.test.cjs` | **新增** |
| `docs/adr/ADR-039-install-tree-module-closure.md` | **新增** |

## 测试策略

### 本地可跑

- `python tools/check_install_tree_module_closure.py` —— 闭包校验（不需要构建）。
- `python tools/check_install_tree_module_closure_self_test.py` —— 工具自测。

### CI 上跑

- 两步：闭包校验 → 自测。
- 加进现有的「Install + downstream proof」腿之后（那腿已经会建暂存树并链接一个下游宿主）。

### 回归

- CI 已有的 Build / Test。
- CI 已有的安装 + 下游证明腿。
- ADR-038 的验收套件。

## 风险

| 风险 | 缓解 |
|---|---|
| **补了清单，但某个模块是 module-OFF 构建的**（`if(TARGET ...)` 跳过）→ 清单里的名字在某些配置下不存在 | 清单里的名字**只是候选**：安装规则用 `if(TARGET ...)` 守卫，链接规则用 `find_library()` + 「找不到就跳过」。本片的校验器判的是**清单覆盖闭包**（静态），不是**产物存在**（动态）—— 两者分工不同。 |
| **自动发现模块**听起来更治本，但它会让 `NIMRTC_MODULE_*` 开关的语义变复杂 | 本片**明确不做**，写进 ADR-039 的「不做」清单。治本是「机器守着清单」，不是「不要清单」。 |
| **工具自己解析 CMake 的正则太脆** | 工具解析的是**本仓库自己的三个文件**（只读 `src/engine/CMakeLists.txt` + 各模块的 `src/modules/<name>/CMakeLists.txt` + 两份清单），格式受本仓库控制；自测钉住行为。 |
| **只补不减 → 清单会越滚越大** | 清单里有闭包之外的模块是**对的**（宿主可能直接用 `nimrtc::ice` 等），不是冗余。 |
| **本片破了 ADR-038 的证据** | ADR-038 的验收套件纳入本片回归（验收标准 19）。 |

## 归属

**`version/v3`**。本片与 `v6-writeset-slice-boundary`（共享判据的签收边界）**写集完全不重叠** —— 一个动打包层，一个动验收判据。

## 遗留（不在本片，但必须被记下）

- **「从目录自动发现模块」**：更治本的方案，但会改 `NIMRTC_MODULE_*` 开关的语义，需要单独一片。
- **两份清单仍然可能漂移** —— 本片只是让机器**看得见**漂移（工具会报红），没有让漂移**不可能**。真正的根治是自动发现。

---

## 实施中的发现（2026-11-25）

N6 的执行腿在 ADR-037 / ADR-038 闭合后第一次真的跑起来，暴露本片的缺口。实证：`node --test tests/acceptance/v1-sdk-sample/acceptance.test.cjs` 在 `build/_install` 存在（且带 openh264）时报 14 个未解析外部符号，其中 9 个是模块 registrar、5 个是 `pacing::TokenBucket`。

同时发现 `pacing` 的特殊性：`nimrtc_sched` 链 `nimrtc::pacing`，而 `pacing` **只在** `_nimrtc_install_shorts` 里、**不在** `_nimrtc_module_shorts` 里 —— 静态链接链因此少了它。ADR-038 补 `nvcodec` 时两份清单各加一行，是**症状**治疗；本片是对齐闭包，并加工具守着。

### 发现二：闭包不止是模块 archive —— 系统库也在闭包里（2026-11-25）

本片把 `video_capture` 补进闭包后，下游宿主立刻撞上 **12 个 LNK2019**（全部来自 `nimrtc_video_capture.lib(mf_camera.obj)`）：`MFStartup` / `MFShutdown` / `MFCreateAttributes` / `MFCreateMediaType` / `MFCreateSourceReaderFromMediaSource` / `MFEnumDeviceSources`，以及 6 个 GUID 常量 `MFVideoFormat_NV12` / `MF_MT_MAJOR_TYPE` / `MF_MT_SUBTYPE` / `MF_MT_FRAME_SIZE` / `MF_MT_FRAME_RATE` / `MFMediaType_Video`。

根因与 ADR-038 的 `d3d11`/`dxgi` **同形**，只是更深一层：闭包模块自己 PRIVATE 链的 **Windows 系统库**在安装后也丢了。源侧 `src/modules/video_capture/CMakeLists.txt` 的 WIN32 分支链的是

```
d3d11 dxgi windowsapp mf mfplat mfreadwrite mfuuid
```

而安装树 config（`cmake/NimRTCConfig.cmake.in` 的 WIN32 分支）只接回了 `d3d11 dxgi` —— Media Foundation 那一组漏了。同理 `audio_device`（闭包模块，engine PRIVATE）PRIVATE 链的 `ole32`（WASAPI 的 `IMMDeviceEnumerator` / `IAudioClient`）也没接回。

**结论**：链接闭包不只是「哪些 `nimrtc_*` archive」，还是「这些 archive 引用了哪些系统库」。安装树必须把**闭包模块自己的 PRIVATE 系统库**也接回最终链接行，否则「装出来的 bundle 一定链得上」这句承诺是假的。

**本片的处理**：在 config 的 WIN32 分支补上 `windowsapp mf mfplat mfreadwrite mfuuid ole32`。既有项（`ws2_32 bcrypt iphlpapi winmm crypt32 d3d11 dxgi`）一个没动 —— 符合 ADR-039 §3「既有链不动」，也保住了 ADR-038 的验收断言。

**留给后续**：本片的工具只判**模块 archive** 的闭包覆盖，不判**系统库**。系统库这一层的漂移目前仍靠下游宿主撞 LNK2019 才发现 —— 与「两份清单漂移」是同一类问题、同一类治法（机器守着）。值得另开一片把「闭包模块的 PRIVATE 系统库」也算进工具的判定范围。

### 发现三：验收宿主的构建树不能放在 worktree 里（Windows 路径长度，2026-11-25）

下游宿主的 configure 一开始直接失败在 CMake 的编译器自检上：

```
CMake Error at .../CMakeTestCXXCompiler.cmake:73 (message):
  正在创建目录"cmTC_d560c.dir\Debug\cmTC_d560c.tlog"…
  ...Microsoft.CppCommon.targets(781,5): error MSB6003:
    指定的任务无法执行文件"CL.exe"，也不能运行。
    System.IO.DirectoryNotFoundException
```

**不是** MSVC 环境没初始化（`vcvars64.bat` 装好之后 `where cl` 能找到 `cl.exe`），**是路径长度**：zelto 的 worktree 路径本身 ~110 字符（`.zelto/version-worktrees/v3/.zelto/worktrees/<slice-id>/`），再叠加 CMake 的 `CMakeFiles\CMakeScratch\TryCompile-xxxxxx\cmTC_xxxxxx.dir\Debug\` 后缀，MSBuild 建 tlog 目录时撞上上限。**实证**：同一份工程放 `C:\Temp\zhost` configure 干净通过，放 worktree 内的深路径失败。

**处理**：宿主的**源码目录**留在 worktree 里（可追溯），**构建树**放短临时目录。已安装的 bundle 仍在 worktree 内 —— 它是**只读**依赖，不参与 MSBuild 的相对输出路径计算，不受影响。

### 发现四：安装树里的运行时 DLL 必须进 PATH（2026-11-25）

链接通过后宿主仍起不来，退出码 `0xC0000135`（`STATUS_DLL_NOT_FOUND`），输出为空。查 exe 的导入表：`msquic.dll` —— MsQuic 是**动态库**，安装树把它装在 `bin/msquic.dll`（`lib/msquic.lib` 只有 1.9 KB，是导入桩）。宿主运行时若没有 bundle 的 `bin/` 在 PATH 上，就找不到它。

**处理**：跑宿主时把 bundle 的 `bin/` 加进 PATH（**不**把 DLL 拷到宿主旁边 —— 那会让宿主「碰巧」能跑，却掩盖「bundle 自带运行时」这个事实）。判据 10 要证明的正是「宿主只链 bundle 里的东西」，运行时 DLL 也该来自 bundle。

### 发现五：临时构建目录不能是固定路径（2026-11-25）

第一次用固定路径 `%TEMP%/nimrtc-closure-host/` 时，门禁跑到一半报：

```
Error: EBUSY: resource busy or locked, open 'C:\...\Temp\nimrtc-closure-host\src\main.cpp'
```

**不是**「这一轮的代码有问题」，是**上一轮**残留的句柄（msbuild / cl.exe 还挂着）把这一轮的文件写入卡住了。报错完全指不到真正的原因。

**处理**：临时目录名带 **pid + 时间戳**（每次运行一个全新目录），并在断言**通过后**删掉它。断言**失败时不删** —— 那样失败现场（CMakeCache / vcxproj / 链接日志）还在目录里，可以直接翻。
