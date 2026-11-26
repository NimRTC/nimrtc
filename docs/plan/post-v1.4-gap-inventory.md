# v1.4 之后的能力缺口清单（Gap Inventory）

> **状态**：`v1.4.0-dev` 周期开始时的能力盘点（2026-11-25）。
> **基线**：`main` @ `cef0a0ab`（`v1.3.1` 之后的首个合并）。
> **范围**：本文**只**记录 v1.4 计划（`docs/plan/v1.4-plan.md` §8 已声明的 out-of-scope）
> **之外**的能力与架构缺口。v1.4 自身的交付项（N1–N9 / D1–D7）不在本文重复。
>
> **纪律**：
> - 每一条都标注了**验证方式**（命令或文件位置），以便任何人可以复核。
> - 本文区分三类条目，并在每类前说明判定标准：
>   - **【缺口】** = 树里有代码/接口，但**没有接进引擎或产品**，或**根本没有实现**。
>   - **【设计决策】** = 有意不做，理由已记录。**不是缺口**，列出仅为避免重复讨论。
>   - **【阻塞】** = 取决于外部条件（SDK、硬件、许可证、人力），树内无法推进。
>
> **与 v1.4 计划的关系**：v1.4 计划的 §8「Explicitly out of scope for v1.4.0」
> 已经把 FEC / simulcast / SVC / 软件 H.265 解码器 / 信令服务进 `src/` / ABI break
> 列为 out-of-scope。本文是那份清单的**扩充与勘误**：补充了 v1.4 §8 没有提到的缺口，
> 并**修正了两条 §8 的措辞**（见 §0.2）。

---

## 0. 摘要与勘误

### 0.1 一句话结论

> **v1.4 之后 NimRTC 最大的结构性缺口，不是功能，而是「引擎没有把已经写好的部件编起来」。**
>
> 具体是两条：
>
> 1. **音频设备层没有被引擎编排**（§1）——`src/modules/audio_device/` 已有 8 个公共头
>    （外加 `plugins/` 层的 `IAudioSource` / `IAudioSink` 接口）、完整构建、
>    有 PortAudio / 系统音频 / 合成后端、有单元测试，但 `src/engine/` 对它**零引用**，
>    `EngineConfig` 里也没有 `audio_source_name` / `audio_sink_name`。
>    v1.4 的 D1「引擎运行设备层」会动这块，所以 §1 里的项和 v1.4 D1 有交叠——
>    区别是本文记的是 **D1 之外的**部分（引擎之外的接线缺口）。
> 2. **引擎的行为拆分还有四项没做**（§5.1）——`VideoPipeline` / `AudioPipeline` /
>    `RtcpProcessor` / `TransportManager` 四个协作类仍未开始，
>    行为仍然散在 19 个 `engine_*.cpp` 里。
>
> 补充一条**容易被漏掉的前置**：音频后端**没有 `PluginRegistry` 槽位**
> （§1.2.6）。`core::PluginRegistry` 有 `register_video_source` /
> `register_audio_decoder`，但**没有** `register_audio_source` /
> `register_audio_sink`。所以 D1 若走注册表路线，要先给 `src/core/` 加两个槽位。
>
> 其余缺口（软件编解码器、OHOS、移动端、国密）都是**已知、已记录、有明确理由**的，
> 不是意外遗漏。

### 0.2 对 v1.4 计划 §8 的两处勘误

| v1.4 §8 的措辞 | 实际情况 | 结论 |
|---|---|---|
| 「A software H.265 decoder … recorded in the area plans, not scheduled here」 | **准确**，但理由被低估了：这不是「还没排期」，而是**许可证决策**——x265 是 GPL-2.0，被项目的许可证策略**排除**。见 §2.1。 | 需要补一句「为什么不做」的理由，而不只是「排在哪」 |
| 「The signaling *service* moving into `src/` — RD-7 stands. N6 consumes the reference service in `examples/`; the SDK does not ship a server.」 | **准确**，但「SDK does not ship a server」是**产品定位**而非技术限制 | 建议明确写成定位声明，避免后续被当成待办 |

> **不做的事也记在这里的理由**：v1.4 计划 §8 只有 8 条，且没有区分「有意不做」和
> 「还没排期」。本文把两者分开，是因为**前者不该反复重开讨论，后者需要排期**。

---

## 1. 【缺口】音频设备层没有被引擎编排

**级别**：高（影响 D1 的可行性与 D5 的 3A 契约）

### 1.1 现状（已核实）

`src/modules/audio_device/` 是一个**完整、可构建、有测试**的模块：

> **组件清单见 `docs/plan/d1-device-layer-implementation.md` §1。**
> 本文不重复那张表——它与本文 §1.1 是**同一份现状快照**，只是拍摄时间不同
> （本文是决策前，D1 拆解是决策后）。本文保留 §1.1 的价值在于记录
> 「模块写好了但引擎接不上」这个状态**是如何被发现和量化的**。

模块的两个关键事实（本文 §1.2.6 的依据）：

| 事实 | 说明 |
|---|---|
| 它的接口在 `plugins/` 层，形状与其他接缝一致 | `IAudioSource` / `IAudioSink` + 各自的 Factory，`create(cfg)` 返回 `IPlugin*` |
| 但它的**后端查找不走 `PluginRegistry`** | 模块自带 `available_source_backends()` / `source_factory(id)` / `sink_factory(id)`，见 §1.2.6 |

**但引擎完全不知道它的存在。** 核实命令与结果：

```console
$ git grep -ln "audio_device\|AudioDevice\|audio_source_name\|audio_sink_name" -- src/engine/
（空）

$ git grep -n "audio_source_name\|audio_sink_name" -- src/engine/include/
（空）

$ git grep -n "add_subdirectory(audio_device)" -- src/modules/CMakeLists.txt
src/modules/CMakeLists.txt:389:    add_subdirectory(audio_device)
```

即：**模块被编译进库，但没有任何一条从 `Engine` 到它的路径。**

> **独立佐证**：`tools/check_v14_plan_status.py`（v1.4 计划漂移探测器）
> 在 `main` 上对 D1 的三条断言全部返回 `ok`：
>
> ```console
> $ python tools/check_v14_plan_status.py
>   ok    D1   the engine never opens an audio device
>   ok    D1   the engine has no audio-source config key
>   ok    D1   the engine has no audio-sink config key
> ```
>
> 即：v1.4 计划自己**承认**引擎没开音频设备、也没有音频源/汇配置键。
> 这与本文 §1.2.1 / §1.2.2 的核实结果一致——是两条独立路径得到的同一结论。
>
> 同一探测器**当前全部 20 条断言都返回 `ok`，没有 STALE**——N5 与 D2
> 这两条曾长期报 STALE（它们的断言写的是「树里没有 TURN 服务器」、
> 「渲染是宿主的活」，而这两件事在 2026-11-24 就已不成立了）。2026-11-26
> 起，它们的断言行已随各自条目的落地一起翻转为「必须存在」：条目一旦落地，
> 断言行就翻转，否则它会**永远**报 STALE——一个常亮的假警报，只会教会人忽略
> 这个探测器。两者的漂移因此已由 N5 / D2 负责人在 v1.4 内消化完毕，
> **不在本文范围**。

### 1.2 缺口拆解

| # | 缺口 | 代码层落点 | 备注 |
|---|---|---|---|
| 1.2.1 | 引擎无 `audio_source_name` / `audio_sink_name` 配置键 | `src/engine/include/nimrtc/engine/engine.hpp`（`EngineConfig`） | 与 `transport_name` / `jb_name` / `codec_name` 等同名模式对齐即可 |
| 1.2.2 | 引擎不创建/持有 `IAudioSource` / `IAudioSink` 实例 | `src/engine/src/engine_impl.hpp` + 新协作类 | v1.4 D1 的核心工作量。**注意行数预算**：`engine_impl.hpp` 现为 399/400 行，加成员必然超——见 D1 拆解 §6.4 |
| 1.2.3 | 引擎不从设备读帧喂给 `send_audio()` | `src/engine/src/engine_audio_frames.cpp` | 现状是宿主调用 `send_audio()`，PCM 契约不变（见 `audio_source.hpp` 的说明）。**出站路径已过 3A**（`engine_media.cpp:1780-1793`），设备 pump 只需「读帧 → 调 `send_audio()`」 |
| 1.2.4 | 引擎不把接收到的音频送到设备播放 | `src/engine/src/engine_render_state.cpp` | 现状是引擎在 tick() 里调宿主的 `set_on_audio_frame()` / `set_on_audio_frame_info()` 回调（`engine.hpp:2727-2744`），**不推设备**。这两个回调是 STABLE，设备路径是**新增的并行路径**，不取代它们 |
| 1.2.5 | 设备枚举 / 热插拔未暴露给宿主 | `EngineConfig` / `Engine` 公共 API | 与 v1.4 D4「camera and hot-plug」同类。枚举走 `IAudioSource::enumerate_devices()` 实例方法，**不进注册表**（ADR-028 已决）。**热插拔属 D4**：D1 只留 `recover_audio_device()` 与 `audio_capture_restarts` 的形状（恒为 0），D4 填实现 |
| 1.2.6 | 后端发现走**模块自有查找函数**，不经 `core::PluginRegistry` | `src/modules/audio_device/src/audio_device.cpp` 的 `available_source_backends()` / `source_factory(id)` / `sink_factory(id)` | 见下 |

> **1.2.1–1.2.6 的实施落点**统一见
> `docs/plan/d1-device-layer-implementation.md` §2（缺口 → 落点映射）与
> §3（实施步骤）。本文保留这张表的**编号体系**——它是这些缺口在项目里的
> 身份标识，别的文档（D1 拆解、ADR-028）都反向引用它。

> **1.2.6 值得单独说明。**
>
> `audio_device` 是全树里**唯一**实现了 `plugins::I*` 工厂接口、
> 却**完全不经 `core::PluginRegistry`** 的模块。它自带三个查找函数
> （`audio_device.hpp:292-317`）：
> `available_source_backends()` / `available_sink_backends()` /
> `source_factory(id)` / `sink_factory(id)`。后端工厂是 `const` 静态对象
> （符合 L6：不可变全局，非可变全局状态）。
>
> （对照：`nvcodec` 虽然也不自注册，但它的 `HwCompositeCodec`
> 实现了 `plugins::IVideoCodec`，是由 `h264` / `h265` / `av1` 的
> `register_default_plugins()` 代为注册的。所以 `audio_device` 的
> 「无人代注册」是独有的。）
>
> **更根本的一点**：`core::PluginRegistry` **根本没有音频设备槽位**——
> `register_*` 列表里有 `register_video_source` / `register_video_capture` /
> `register_video_sink` / `register_audio_decoder` / `register_audio_receiver`，
> 但**没有** `register_audio_source` / `register_audio_sink`。
> 也就是说：**即使 v1.4 D1 想走注册表路线，也要先给 `PluginRegistry` 加两个槽位**
> （`src/core/include/nimrtc/core/registry.hpp` + `src/core/src/plugin_registry.cpp`）。
>
> 这不违反任何硬约束（`plugins::IAudioSourceFactory` 的接口在 `plugins/` 层，
> 符合 L2；音频后端不注册就不涉及 L6），但它让 **D1 多出两个决策项**：
> 1. 音频后端**并入** `PluginRegistry`（需加两个槽位，改 `src/core/`）
>    还是**保持独立查找**（零 `core/` 改动，但多一条特殊路径）？
> 2. `EngineConfig` 的形态：两种路线都是 `audio_source_name = "portaudio"`，
>    差别只在**解析路径**。
>
> **✅ 2026-11-25：这两个决策已经做出。**
>
> - **决策 1（并入还是独立查找）**：**并入 `core::PluginRegistry`**。
> - **决策 2（`EngineConfig` 形态）**：两个键都是 `std::string`，默认 `"portaudio"`；
>   **空串 = 默认设备**，`"none"` = 显式 opt-out（D1-1 已决定）。
>   注意这与 `audio_decoder_name` 的语义**不同**——后者的空串表示「关闭音轨」。
>   原因是 **D1 的 DoD 是「不配任何设备 id 时，引擎就能采集和播放」**，
>   空串若表示关闭则与 DoD 矛盾。
>
> **决策的完整论证在 [ADR-028](../adr/ADR-028-audio-device-plugin-registry.md)**，
> 实施落点在 `docs/plan/d1-device-layer-implementation.md`。
> 本文不重复两份文档的正文——它们是**决策**与**实施**，本文是**决策前的缺口记录**。
>
> 门禁提醒（决策的实现要过这些）：新增 `NIMRTC_API_STABLE` 成员按 ADR-015
> 是**增量**（不是 MAJOR 级破坏），但仍须给新符号打 STABLE 标记，并过
> `scripts/api_freeze_scan.js`（逐符号分类）与 `scripts/check_api_manifest.py`
> （公共头清单）两道门禁。`registry.hpp` 与 `audio_device/*` 都已在
> `docs/api/public_headers.manifest` 里，所以**改现有文件不改清单**——
> 只有新增公共头才需要 `--write` 重生成清单。

### 1.3 为什么重要

- **v1.4 D1「引擎运行设备层」的前提就是这些接线。** D1 不是新写设备层，
  是**把已有设备层接进引擎**。本文的 §1 是 D1 的**前置清单**，不是 D1 的重复。
- **D5（音频 P2：DTX 耦合与 3A 契约）依赖 §1.2.3** —— 设备的帧必须先进入引擎，
  3A 才有可作用的 PCM。
- **D6（连通性 profile）** 要求「麦克风可用」成为连接决策的输入，同样依赖 §1。

### 1.4 与 v1.4 的边界

> ⚠️ **v1.4 D1 会覆盖 §1 的大部分。** 本文刻意把 D1 的**交付内容**排除在外，
> 只记录 **D1 之外的接线缺口**。合并到 `v1.4-plan.md` 前请先与 D1 的 owner 确认边界，
> 避免两份计划抢同一块地。
>
> **✅ 2026-11-25：边界已划定。** §1.2.1–1.2.6 **全部**归 D1，
> 实施拆解见 `docs/plan/d1-device-layer-implementation.md`。
> 本文的 §1 保留为**决策前的现状记录**——它的价值在于记录
> 「模块写好了但引擎接不上」这个状态是如何被发现和量化的，
> 以及 §1.3 的四条理由（为什么 D5 / D6 依赖它）。

---

## 2. 【缺口】软件编解码器覆盖不足

**级别**：中（产品覆盖面，非架构缺陷）

### 2.1 H.265 / HEVC：只有硬件路径，**软件路径是许可证决策**

`src/modules/h265/` 的现状：

| 部件 | 状态 |
|---|---|
| `BitstreamParser`（Annex B 解析） | ✅ 完整实现（`src/decoder.cpp`） |
| `StubDecoder` | ⚠️ 只报 key-frame 属性，**不产出像素**（`valid() == false`） |
| NVENC / NVDEC 编码 | ✅ 硬件（`src/modules/nvcodec/`，H.264/H.265/AV1 共用驱动代码） |
| VAAPI / QSV / AMF 编码 | ✅ 硬件（`src/vaapi_h265_encoder.cpp` 等） |
| **软件编码器（x265）** | ❌ **不存在，且被许可证策略排除** |
| **软件解码器（libde265 / OpenH265 HEVC）** | ❌ **不存在** |

许可证决策的原文在 `src/modules/h265/CMakeLists.txt`：

```cmake
# Per ARCHITECTURE.md §1.4 + §11.3 and the remote-desktop plan's RD-2:
# NimRTC does NOT bundle a software HEVC decoder (x265 is GPL-2.0 and
# excluded by the project's licensing policy).  This module ships:
```

**这不是「还没做」，是「按策略不做」。** 但它有一个未被记录的后果：

> **v1.3 的 Q1b（参数集断言）因此被永久关闭。**
> v1.4 §8 写的是「the software-decoder half stays deferred because there is
> still no software H.265 decoder to assert on … so it is neither v1.4.0 work
> nor an open follow-up」。
> **准确的表述应是**：Q1b 的软件半边不是「deferred」也不是「closed」——
> 它是**被许可证策略否决**，除非将来改变许可证策略（采用 LGPL 的 HEVC 实现
> 或换用系统库）。

### 2.2 AV1：软件解码有，软件编码无

| 部件 | 状态 |
|---|---|
| `dav1d_decoder.cpp` | ✅ **软件解码器完整实现**（`libdav1d`，BSD-2）——但 `NIMRTC_PLUGINS_DAV1D` **默认 OFF**，需显式开启 |
| `svt_av1_encoder.cpp` | ⚠️ 有桩，但 `svt_av1_unavailable_reason()` 返回不可用 |
| AMF AV1 编码 | ⚠️ 桩，日志明说 "the AMF AV1 encoder body is not implemented yet" |
| VAAPI AV1 编码 | ✅ 硬件 |

> **注意「默认 OFF」这件事。** `NIMRTC_PLUGINS_DAV1D` 默认关闭
> （`CMakeLists.txt:96`），所以**默认构建里的 AV1 解码走的是 stub**，
> 不是 dav1d。CI 的三条腿（Windows / Linux / macOS）都显式带
> `-DNIMRTC_PLUGINS_DAV1D=ON`，所以软件路径在 CI 里是被证明的；
> 但**发行版的默认构建不含它**。这是「能力存在」与「能力默认可用」的区别，
> 值得在文档里说清楚。

SVT-AV1 的许可（BSD-3）和 dav1d 一致，**没有许可证障碍**——AV1 软件编码是
**纯工程量缺口**，与 H.265 不同。

### 2.3 Android MediaCodec：空桩

`src/modules/h264/src/hw_backends.cpp` / `h265` / `av1` 中有 MediaCodec 条目，
但 Android 平台本身是 roadmap 项（README：「iOS/Android are roadmap, not a
current-track item」），所以 Android 编解码后端整体排在移动端之后。

### 2.4 缺口清单

| # | 缺口 | 障碍类型 | 建议归属 |
|---|---|---|---|
| 2.4.1 | H.265 软件编码/解码 | **许可证（GPL-2.0）** | 需 owner 决策：维持策略则永久不做；否则需引入 LGPL/MIT HEVC 实现并做许可证审计 |
| 2.4.2 | AV1 软件编码（SVT-AV1） | 无（纯工程量） | v1.5 候选 |
| 2.4.3 | AMF AV1 编码器本体 | 需 AMD VCN 3.0/4.0 硬件验证 | 与 2.4.2 一起排在 v1.5 |
| 2.4.4 | Android MediaCodec 后端 | 依赖 Android 平台整体 | 排在移动端之后 |
| 2.4.5 | dav1d 软件解码默认 OFF（`NIMRTC_PLUGINS_DAV1D`） | 非缺口，是**发行策略**决策 | 若目标是「开箱即用的软件 AV1 解码」，需改默认值或拆发行变体 |

---

## 3. 【缺口】下游消费证明的平台覆盖不完整

**级别**：中（发版可信度）

### 3.1 现状（已核实）

CI 的下游消费证明（`find_package(NimRTC)` + 最小消费者构建）在 **Windows** 和
**Linux** 两条腿都有，**macOS 没有**：

```console
$ git show HEAD:.github/workflows/ci.yml | grep -n "find_package\|Install + downstream proof"
237: # bundle and build a minimal find_package(NimRTC) consumer against
249: - name: Install + downstream proof      # ← Windows
267: find_package(NimRTC 1.0 REQUIRED)
1104: # build a minimal find_package(NimRTC) consumer against it, so the
1106: - name: Install + downstream proof     # ← Linux
1123: find_package(NimRTC 1.0 REQUIRED)
```

macOS 腿（`ci.yml` 第 1634 行 `name: macos-clang / arm64`）的 Configure 步骤：

```yaml
      - name: Configure
        run: cmake --preset debug.macos -DNIMRTC_BUILD_EXAMPLES=ON -DNIMRTC_VENDORED_WEBRTC_APM=OFF -DNIMRTC_PLUGINS_OPENH264=ON -DNIMRTC_PLUGINS_DAV1D=ON
```

**缺少 `-DNIMRTC_INSTALL=ON`**，因此没有 install bundle，也就没有
`find_package` 消费者证明。

> 这也意味着：**v1.4 的 N9「the library installs, a host finds it, and every
> platform leg proves it」这条 DoD，与当前 macOS 腿的配置直接冲突。**
> v1.4 计划里 N9 要求「every platform leg」都证明，但 macOS 腿今天连 install 都没开。
> 这是 v1.4 内部必须解决的一致性问题，属于 N9 的工作内容。

### 3.2 缺口清单

| # | 缺口 | 落点 |
|---|---|---|
| 3.2.1 | macOS 腿缺 `-DNIMRTC_INSTALL=ON` | `.github/workflows/ci.yml` macOS 腿 Configure 步骤 |
| 3.2.2 | macOS 腿缺 `find_package(NimRTC)` 消费者证明 | 同上，需新增「Install + downstream proof」步骤 |
| 3.2.3 | macOS 腿缺 install bundle 归档 | v1.4 N9 要求归档 |

---

## 4. 【缺口】平台支持：OHOS 与移动端

### 4.1 OpenHarmony（OHOS）

**级别**：高（战略平台），但**被外部条件阻塞**。

| 资产 | 状态 |
|---|---|
| 工具链预设 | ✅ `debug.ohos-arm64` / `release.ohos-arm64`（`CMakePresets.json`） |
| 交叉编译工具链文件 | ✅ `cmake/toolchains/ohos-aarch64.cmake` |
| 计划文档 | ✅ `docs/plan/openharmony-support.md`（Stage 0 已合并，Stage 1 进行中） |
| 专项调研 | ✅ `docs/plan/ohos-audio-hal.md`、`docs/plan/ohos-musl-compat.md` |
| **构建验证** | ❌ **OH-DOD-1 阻塞**：需要 OHOS SDK + aarch64 交叉工具链 |
| CI job | ❌ OH-DOD-6（`ohos-arm64` GitHub Actions job）未接通 |

OHOS 的 DoD 表格（`docs/plan/openharmony-support.md` §6）共 11 条，
**OH-DOD-1 是阶段 1 的强制前置**——它需要用户在本机安装 OHOS SDK 后本地验证。
这不是代码缺口，是**环境缺口**。

### 4.2 移动端（iOS / Android）

README 明确：

> **Not a fit if:** you need a turnkey browser-grade SDK today, or you only
> target mobile (iOS/Android are roadmap, not a current-track item).

**无计划文档、无预设、无 CI 腿。** 属【阻塞】/ 长期路线。

### 4.3 缺口清单

| # | 缺口 | 障碍 | 建议归属 |
|---|---|---|---|
| 4.3.1 | OHOS SDK 环境未就绪 | **外部（需用户安装）** | 用户动作后即可推进 Stage 1 |
| 4.3.2 | `ohos-arm64` CI job 未接通 | 需自托管 runner（无官方 OHOS runner） | OH-DOD-6 |
| 4.3.3 | OHOS 音频 HAL 集成 | 依赖 4.3.1 | OH-DOD-9 / Stage 4 |
| 4.3.4 | iOS 平台支持 | 需 Xcode / Apple 生态决策 | 长期路线，无计划文档 |
| 4.3.5 | Android 平台支持 | 同上 + MediaCodec 后端 | 长期路线 |

---

## 5. 【缺口】架构层：引擎的行为拆分未完成

**级别**：中（技术债，非功能缺失）

### 5.1 D1 引擎拆分：状态已完成，行为未开始

`docs/plan/architecture-remediation-plan.md` §4.3 的原话：

> `VideoPipeline` / `AudioPipeline` / `RtpPacketizer` / `RtcpProcessor` /
> `TransportManager` 这几个类**没有**引入。
> ……
> **2026-11-15 更新**：`RtpPacketizer` 这一项**已经落地**（§5.1）…
> 其余四个协作类（`VideoPipeline` / `AudioPipeline` / `RtcpProcessor` /
> `TransportManager`）**仍未开始**。

`RtpPacketizer` 已落地并有守卫
（`tools/check_engine_layering.py` 第二条规则：`rtp::PacketBuilder`
只能在 `src/engine/src/rtp_packetizer.cpp` 里被命名）。

**其余四项**：

| 协作类 | 应承载的行为 | 当前所在 |
|---|---|---|
| `VideoPipeline` | video source/codec/sender/receiver/sink 的编排 + 生命周期 | `src/engine/src/engine_media.cpp` |
| `AudioPipeline` | audio3a/codec/receiver/JB 的编排 | `src/engine/src/engine_media.cpp` + `engine_audio_frames.cpp` |
| `RtcpProcessor` | RTCP 解析 / SR / RR / 反馈 | `src/engine/src/engine_rtcp.cpp` |
| `TransportManager` | ICE/DTLS/SRTP/调度器编排 | `src/engine/src/engine_transport.cpp` |

**为什么是缺口而不是重构洁癖**：每搬一个协作类都要把一段**行为**移出引擎，
属于需要独立评审的改动量。它们的价值是让 `src/engine/` 达到 ADR-025 描述的
目标形态（`engine_impl.hpp` < 400 行、状态与行为都按关注点分入具名协作者）。

### 5.2 缺口清单

| # | 缺口 | 风险 | 建议归属 |
|---|---|---|---|
| 5.2.1 | `VideoPipeline` 协作类 | 中（视频路径回归面大） | v1.5，独立 PR |
| 5.2.2 | `AudioPipeline` 协作类 | 中（与 §1 的音频接线耦合） | v1.5，**建议在 §1 完成之后** |
| 5.2.3 | `RtcpProcessor` 协作类 | 中（RTCP 反馈闭环） | v1.5，独立 PR |
| 5.2.4 | `TransportManager` 协作类 | 中（与 N1–N3 网络轨耦合） | v1.5，与 v1.4 网络轨合并后 |

> **排序建议**：`VideoPipeline` 和 `RtcpProcessor` 相对独立，可先做；
> `AudioPipeline` 应排在 §1 音频接线之后（否则要搬两次）；
> `TransportManager` 应排在 v1.4 网络轨（N1/N2/N3）合并之后。

> **`AudioPipeline` 与 D1 的耦合，具体是什么。**
>
> `AudioPipeline` 要承载「audio3a/codec/receiver/JB 的编排」，而这些行为
> 当前在 `src/engine/src/engine_media.cpp` + `engine_audio_frames.cpp` 里。
> **D1 要改的正是这两个文件**——D1 的设备创建点在 `engine_media.cpp` 的
> 音频初始化段（视频侧 `video_capture_` / `video_sink_` 的创建点旁），
> 设备 pump 是新的 `engine_audio_pump.cpp`。
>
> 所以两者的顺序**不是偏好，是依赖**：
> - **D1 先做** → `AudioPipeline` 落地时要连设备 pump 一起搬（搬两次）
> - **`AudioPipeline` 先做** → D1 要往新协作类里加设备成员（等一次）
>
> **结论：§1（D1）先做。** 这是 §10 排期表把 §5.2.2 排在 §1 之后的理由，
> 也是 D1 拆解 §4.6 的对应约束。

---

## 6. 【缺口】国密（GM/T）密码栈：SRTP 侧仍是接缝

**级别**：中（合规场景必需）

### 6.1 DTLS 侧：已实现（opt-in）

| 资产 | 位置 | 状态 |
|---|---|---|
| GmSSL DTLS 会话 | `src/modules/dtls/backends/gmssl/src/dtls_gmssl_session.cpp` | ✅ 实现 |
| GmSSL DTLS 工厂 | `src/modules/dtls/backends/gmssl/src/dtls_gmssl_factory.cpp` | ✅ 实现（id = `"gmssl"`） |
| 构建开关 | `NIMRTC_ENABLE_DTLS_GMSSL`（默认 OFF） | ✅ |
| 引擎选择 | `EngineConfig::dtls_name = "gmssl"` | ✅ |
| GmSSL vendored 库 | `src/third_party/gmssl/`（v3.x） | ✅ |

### 6.2 SRTP 侧：只有接缝，**无实现**

`src/modules/srtp/include/nimrtc/srtp/libsrtp_backend.hpp:16`：

```
 * SM4-GCM is out of scope — a v1.2+ national-crypto backend implements
```

`src/modules/srtp/include/nimrtc/srtp/srtp.hpp:131-134` 描述了接缝形状
（未来的国密后端实现 `plugins::ISrtpSessionBackend`，以自己的 id 注册，
由 `EngineConfig::srtp_backend_name` 选择）。

> **版本影响提示**：按 `RELEASING.md` §1.1，**加枚举值不是破坏性改动**
> （破坏性 = 要求下游改代码 / 换符号集 / 换编译标志）。
> 所以 6.3.1 属于 `MINOR`，可以进 `1.5.0`，
> **不需要**像「改已有函数签名」那样升 `MAJOR`。
> 但新增的枚举值必须打 `NIMRTC_API_STABLE` 标记，并过
> `scripts/api_freeze_scan.js` 的逐符号分类门禁。

**现状**：
- `srtp::SecuritySuite` 的实际枚举值只有四个：`Aes128CmSha1_80` /
  `Aes128CmSha1_32` / `Aes128Gcm_128` / `Aes256CmSha1_80`。
  **没有 SM4-GCM 套件枚举值。**
- libsrtp 后端在 `NIMRTC_LIBSRTP_HAS_GCM == 0` 时明确报
  "AES-128-GCM is not supported by this libsrtp build"。

### 6.3 缺口清单

| # | 缺口 | 落点 | 备注 |
|---|---|---|---|
| 6.3.1 | `SecuritySuite` 无 SM4-GCM 枚举值 | `src/modules/srtp/include/nimrtc/srtp/srtp.hpp` | **增量改动**（加枚举值），不是破坏性 → 属 `MINOR`，可进 `1.5.0` |
| 6.3.2 | 无 SM4-GCM SRTP 后端实现 | `src/modules/srtp/`（新 backend 子目录） | 需 GmSSL 的 SM4-GCM SRTP profile |
| 6.3.3 | 无 SM4-GCM 的 SRTP 测试向量 / 互通证明 | `src/modules/srtp/tests/` | 与 6.3.2 同一个 PR |
| 6.3.4 | 引擎无「国密 SRTP profile」的配置面 | `EngineConfig` | 与 v1.4 D6 的 connectivity profile 并列 |

---

## 7. 【缺口】生产就绪（P4）尚无正式定义

**级别**：高（决定何时能对外称「生产可用」）

### 7.1 现状

NimRTC 现有的是**版本级 DoD**（v1.0 / v1.3 / v1.4 各自的 §6），
但**没有一份跨版本的「生产就绪」判据**。README 提到 P2P client / relay gateway /
embedded Linux aarch64 三种形态，但每一项的 DoD 都散落在各自的计划文档里。

### 7.2 缺失的判据

| # | 缺失项 | 说明 |
|---|---|---|
| 7.2.1 | 正式外部安全审计 | 目前只有内部自审（ADR 系列 + `tools/` 门禁） |
| 7.2.2 | 多链路遥操作接管框架 | `profiles/teleop.json` 存在，但接管语义未产品化 |
| 7.2.3 | 官方 `cloudgame` profile | `docs/plan/ai-game-demo-plan.md:51` 明确「不做 `cloudgame` 官方 Profile（长期候选，未进 shipped 集合，不等它）」；`profiles/` 下现有 12 个 profile（另加 `profiles/schema/` 下 3 个 schema），**无 `cloudgame.json`** |
| 7.2.4 | 跨版本 DoD 汇总 | 每个版本各说各的，无法回答「现在离生产还差什么」 |

---

## 8. 【设计决策】不是缺口，但需要记一笔

以下条目**有意不做**，理由已记录。列出仅为避免后续被反复提出。

| 条目 | 决策 | 记录位置 |
|---|---|---|
| 软件 H.265 编解码 | 许可证：x265 是 GPL-2.0，与项目许可证策略冲突 | `src/modules/h265/CMakeLists.txt` 头部注释 |
| `set_ref_frame()` 作为时间线载体 | **ADR-017**：时间线载体是 WebRTC abs-send-time + RTCP-SR；`set_ref_frame()` 保留为 ABI 兼容的 no-op | `src/plugins/include/nimrtc/plugins/video_pipeline.hpp:923-943` |
| 信令服务进 `src/` | **RD-7**：SDK 不内置服务器；N6 消费 `examples/` 下的参考服务 | `docs/plan/v1.4-plan.md` §8 |
| 信号服务「SDK 不发行」 | 产品定位，非技术限制 | 同上 |
| FEC / ULPFEC / FlexFEC / RED | v1.5.0 线 | v1.4 §8 |
| simulcast / SVC | v1.5.0 线 | v1.4 §8 |
| ABI break | ADR-015 冻结（v1.4 全部为增量） | ADR-015 |
| 引擎 include 具体模块头 | ADR-025 的 8 模块白名单 | `docs/adr/ADR-025-sdp-rtp-not-pluggable.md` |

> **关于 `set_ref_frame` 的澄清**：v1.4 计划 §1.2 与 v1.3 计划都把
> `set_ref_frame` 描述成「P3 stub / 待实现」。**这是过时的**：
> 引擎侧已有真实实现（`src/engine/src/engine_timeline.cpp` 的
> `record_video_ref_frame` / `ref_frame_to_capture_ts` /
> `capture_ts_to_ref_frame`，基于无分配的定长开地址表
> `src/engine/src/engine_ref_frame_table.hpp`），
> 而插件侧的 no-op 是 ADR-017 要求的 ABI 兼容形态。
> **正确表述**：时间线已经落地；`set_ref_frame()` 这个**插件接口方法**按设计保留为 no-op。

---

## 9. 【阻塞】外部条件

| 条目 | 阻塞原因 | 谁来解 |
|---|---|---|
| OHOS 构建验证（OH-DOD-1） | 需本机安装 OHOS SDK + aarch64 交叉工具链 | **用户** |
| `ohos-arm64` CI runner（OH-DOD-6） | 无官方 OHOS GitHub runner，需自托管 | 需决策 |
| 外部安全审计（§7.2.1） | 需外部团队 + 预算 | 需决策 |
| 云游戏官方 profile（§7.2.3） | 产品决策 | 需决策 |
| H.265 软件编解码（§2.4.1） | 需改变/确认许可证策略 | **需 owner 决策** |

---

## 10. 建议的排期归属

| 归属 | 条目 | 理由 |
|---|---|---|
| **v1.4 内部** | §3.2.1–3.2.3（macOS install + 下游证明） | N9 的 DoD 明确要求「every platform leg」，这是 v1.4 内部的一致性问题 |
| **v1.4 内部** | §1 中 D1 范围内的接线（§1.2.1–1.2.6） | D1 的定义就是「引擎运行设备层」。**决策已落定**：见 [ADR-028](../adr/ADR-028-audio-device-plugin-registry.md)；实施落点见 `docs/plan/d1-device-layer-implementation.md` |
| **v1.5.0 候选** | §2.4.2（AV1 软件编码）、§5.2.1 / §5.2.3（两个协作类）、§6（国密 SRTP） | 无外部依赖，可并行推进 |
| **v1.5.0 候选** | §5.2.2（`AudioPipeline`） | **依赖 §1 完成**（见 §5.2 的排序说明） |
| **v1.5.0 候选（晚）** | §5.2.4（`TransportManager`） | 与网络轨耦合，待 v1.4 N1–N3 合并后 |
| **需决策后再排** | §2.4.1（H.265 许可证）、§4.3.4–4.3.5（移动端）、§7（生产就绪判据） | 依赖 owner / 产品决策 |
| **用户动作后可推进** | §4.3.1–4.3.3（OHOS） | 装上 SDK 即可 |
| **永久不做（除非改策略）** | §8 中标注的决策项 | 理由已归档 |

---

## 11. 复核方式

本文每条断言都可复核。核心复核命令：

```console
# §1 音频设备层未接线
git grep -ln "audio_device\|AudioDevice\|audio_source_name\|audio_sink_name" -- src/engine/    # → 空
git grep -n "audio_source_name\|audio_sink_name" -- src/engine/include/                       # → 空
git grep -n "add_subdirectory(audio_device)" -- src/modules/CMakeLists.txt                    # → 有

# §1.2.6 PluginRegistry 没有音频设备槽位
git grep -n "void register_" -- src/core/include/nimrtc/core/registry.hpp | grep -i "audio\|video_source\|video_sink"
#   → 有 register_video_source / register_audio_decoder / register_audio_receiver …
#   → 无 register_audio_source / register_audio_sink
git grep -n "available_source_backends\|source_factory\|sink_factory" -- src/modules/audio_device/

# §2 编解码器
git ls-files src/modules/h265/src/                                    # → 无软件编解码器
git grep -n "not implemented" -- src/modules/av1/src/amf_av1_encoder.cpp
git show HEAD:src/modules/h265/CMakeLists.txt | head -20               # → GPL-2.0 决策

# §3 macOS CI
git show HEAD:.github/workflows/ci.yml | grep -n "find_package"        # → 只有 Windows / Linux
git show HEAD:.github/workflows/ci.yml | grep -n "NIMRTC_INSTALL=ON"  # → 只有 Windows / Linux

# §5 引擎拆分
git show HEAD:docs/plan/architecture-remediation-plan.md | grep -n "仍未开始"

# §6 国密 SRTP
git show HEAD:src/modules/srtp/include/nimrtc/srtp/srtp.hpp | grep -n "Aes\|Sm4"   # → 仅 AES

# §8 set_ref_frame
git show HEAD:src/plugins/include/nimrtc/plugins/video_pipeline.hpp | grep -n -A14 "P3 — interface stable"
git show HEAD:src/engine/src/engine_timeline.cpp | grep -n "ref_frame"

# 全局佐证：v1.4 计划漂移探测器（独立核实 §1 的 D1 三条断言）
python tools/check_v14_plan_status.py
```

> **注意 `check_v14_plan_status.py` 的退出码**：它当前 **20 条全部 `ok`，没有 STALE**
> （N5 与 D2 这两条曾长期报 STALE——它们的断言写的是「树里没有 TURN 服务器」、
> 「渲染是宿主的活」，而这两件事在 2026-11-24 就已不成立；2026-11-26 起这两行
> 随各自条目的落地一起翻转为「必须存在」，否则它们会**永远**报 STALE——一个常亮
> 的假警报，只会教会人忽略这个探测器）。
> 本文 §1 的三条 D1 断言在该工具里返回 `ok`——即**计划承认缺口存在**，
> 与本文的核实结果一致。

---

## 12. 与既有文档的关系

| 文档 | 关系 |
|---|---|
| `docs/plan/v1.4-plan.md` | §8 是本文的**子集**；本文是扩充 + 勘误。§1 与其 D1 条目有交叠，边界见本文 §1.4 |
| `docs/plan/d1-device-layer-implementation.md` | §1.2.1–1.2.6 的**实施拆解**。本文是决策前的缺口记录，那份是决策后的实施计划 |
| `docs/adr/ADR-028-audio-device-plugin-registry.md` | §1.2.6 的**决策**（并入 `PluginRegistry`）。论证只在那一份里 |
| `docs/plan/architecture-remediation-plan.md` | §5 引用其 D1 的状态 |
| `docs/plan/openharmony-support.md` | §4.1 引用其 §6 DoD |
| `docs/plan/remote-desktop-plan.md` | §1 引用其 RD-2 |
| `docs/adr/ADR-017-cross-end-timeline.md` | §8 引用其时间线载体决策 |
| `docs/adr/ADR-025-sdp-rtp-not-pluggable.md` | §8 引用其引擎层白名单 |
| `docs/plan/ai-game-demo-plan.md` | §7.2.3 引用其 cloudgame 决策 |

---

**文档版本**：2026-11-25 初稿
**基线 commit**：`cef0a0ab`
**下次复核建议**：v1.4.0 发布前，或 `main` 有 ≥ 20 个新提交时
