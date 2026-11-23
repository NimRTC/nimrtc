# ADR-029: 引擎驱动音频设备层（音频设备注册表槽位 + 引擎侧设备泵）

**状态**：Proposed
**日期**：2026-11-22
**相关**：`docs/plan/v1.4-plan.md` §D1（"the engine runs the device layer"）、`docs/adr/ADR-015-api-stability-policy.md`、`docs/adr/ADR-023-plugin-id-naming-contract.md`、`docs/adr/ADR-001-plugin-system.md`、`specs/slices/v1-device-policy.md`（D3/D4，其「本片的边界」一节明确把音频留给本 ADR）

---

## 1. 背景

远程桌面计划 F1 已经把音频设备的**接缝**（seam）建好了，并且标为 STABLE：

- `plugins::IAudioSource` / `plugins::IAudioSourceFactory` —— 麦克风（采集）
- `plugins::IAudioSink` / `plugins::IAudioSinkFactory` —— 扬声器（播放）

`src/modules/audio_device/` 也有 PortAudio 后端、环形缓冲、重采样、混音、系统音频回环、AEC 延迟估计、播放进度。

**但是引擎今天完全不知道它们的存在。** 已核实：

| 事实 | 位置 | 后果 |
|---|---|---|
| `core::Registrar::Category` 无 `kAudioSource` / `kAudioSink` | `src/core/include/nimrtc/core/registry.hpp:1539` | 注册表里没有音频设备槽位 |
| `EngineConfig` 无 `audio_source_name` / `audio_sink_name` | `src/engine/include/nimrtc/engine/engine.hpp` | 引擎无法要求一个音频设备 |
| 引擎无音频设备泵 | `src/engine/src/` | `send_audio()` 由宿主手推 PCM；解码出来的 PCM 由宿主手推给设备 |
| `audio_device` 模块用**模块内函数**解析工厂 | `src/modules/audio_device/src/audio_device.cpp:55`（`source_factory(id)` / `sink_factory(id)`） | 引擎**无法**解析音频设备：依赖方向是 device→engine，反向不成立 |

于是 v1.4 计划书 §D1 的验收标准 ——

> with **no device ids configured**, an engine opened with a "conference" config captures from the default microphone, plays decoded audio to the default speaker, and reports both devices in the stats

—— 今天**不可能**成立。

`specs/slices/v1-device-policy.md`（D3/D4，已完成）在「本片的边界」一节里**明确记下了这个缺口**：

> 核实结论：引擎里没有音频设备的承载体 …… 那属于计划书 §D1「引擎驱动设备层」，而 **D1 不在 v1 版本计划里**。所以本片：只做视频侧；策略表里给音频**预留**位置，字段存在但值为空。

本 ADR 补上这个洞。

## 2. 决策

### 2.1 决策一：注册表加两个音频设备槽位

`core::Registrar::Category` 增加 `kAudioSource` 与 `kAudioSink`，与 `kVideoCapture` 完全对称。

配套（同 `kVideoCapture` 的既有模式）：

- `PluginRegistry::register_audio_source()` / `get_audio_source()` / `list_audio_sources()`
- `PluginRegistry::register_audio_sink()` / `get_audio_sink()` / `list_audio_sinks()`
- 私有槽位 `TypedRegistry<plugins::IAudioSourceFactory> audio_source_{"audio-source"}` 等
- 注册宏 `NIMRTC_REGISTER_AUDIO_SOURCE(id, factory_ptr)` / `NIMRTC_REGISTER_AUDIO_SINK(...)`
- `Registrar` 的 `switch` 分支各加一条
- `src/core/src/plugin_registry.cpp` 各加一个实现（与 `register_video_capture` 同形）
- `src/engine/include/nimrtc/engine/pal/engine_plugin_resolver.hpp` 加 `resolve_audio_source()` / `resolve_audio_sink()`

**为什么不是「引擎侧可注入的解析器」**（备选方案 B）：视频侧已经是「注册表 → `pal::resolve_*`」这条路。音频走另一条路会产生两套心智模型，而且宿主要多做一件「把工厂交进来」的事 —— 而「没配就自动用默认设备」正是 D1 的核心承诺，那件事在注册表模型里是自然的，在注入模型里不是。

**为什么 `core` 依赖 `plugins` 不违反方向**：`core → plugins` 是**既有的**依赖（`src/core/CMakeLists.txt` 里 `nimrtc_core` 链接 `nimrtc::plugins`；`registry.hpp` 已经在用 `plugins::IAudioDecoderFactory` / `plugins::IVideoCaptureFactory`）。本决策只是在这个既有的边上再加两个类型。

### 2.2 决策二：`audio_device` 模块把工厂注册进去

`src/modules/audio_device/src/audio_device.cpp` 在其 `register_default_plugins()` 里调用 `NIMRTC_REGISTER_AUDIO_SOURCE` / `NIMRTC_REGISTER_AUDIO_SINK`，把已有的 4 个采集后端（`portaudio` / `synthetic` / `system-audio` / `synthetic-system-audio`）与 2 个播放后端（`portaudio` / `synthetic`）注册进去。

**模块内的 `source_factory(id)` / `sink_factory(id)` 保留**（它们是模块的公开 API，见 `src/modules/audio_device/include/nimrtc/audio_device/audio_device.hpp:306`），但**注册表成为引擎的解析路径**。两个入口解析到的是**同一批工厂对象**（注册表里放的指针就是模块里那几个静态工厂），不存在两套实现。

### 2.3 决策三：`EngineConfig` 加两个字段 + 设备配置

```
audio_source_name : std::string   // "" = 宿主自己喂 PCM（今天的行为）；"auto" = 平台默认麦克风；"<id>" = 显式后端
audio_sink_name   : std::string   // 同上，播放侧
audio_source_config : plugins::AudioSourceConfig
audio_sink_config   : plugins::AudioSinkConfig
```

**空值语义不变**：今天 `send_audio()` 由宿主手推 PCM，加了字段之后宿主不填就**还是**手推 —— 不填 `audio_source_name` 的宿主行为**逐字不变**。

**`"auto"` 是新增的显式取值**，理由与 D3 的 `video_capture_name == "auto"` 完全一样（见 `specs/slices/v1-device-policy.md` §覆盖规则）：

- 空值本来就有别的含义（"宿主自己喂"），直接改空值语义会让所有既有宿主静默开始开真实设备 —— CI 上直接炸；
- `"auto"` 是**显式**的意图，不与历史行为混淆。

**`"none"` 是显式退出**（计划书 §D1 的原话："`"none"` = the explicit opt-out"）—— 与 `"auto"` 分开：前者是「明确不要设备」，后者是「要默认设备」。

### 2.4 决策四：引擎侧音频设备泵（`engine_audio_device_pump.cpp`）

新文件 `src/engine/src/engine_audio_device_pump.cpp`，与 `engine_video_pump.cpp` 同形：

- **采集侧**：在引擎已有的 tick 路径上 `read_samples()` 拉帧 → 交给出站路径（`send_audio()` 的内部等价物）；
- **播放侧**：解码出的 PCM 写进 sink；
- **3A 参考对齐**：把 sink 的 `played_samples()` 喂给引擎的 `set_render_delivered()` —— 这是 F3/F4 建的 3A 参考对齐，**今天由宿主接线**，D1 之后**由引擎接**。

**RD-6 成立**：设备拥有自己的线程，引擎只在自己的线程上碰 codec 和 3A。

**不加新线程**：泵挂在已有的 tick 路径上（与 D4 的设备变更轮询同一个决定）。

## 3. 后果

### 3.1 正面

- D1 的验收标准首次可能成立：引擎自己开默认麦克风、自己播默认扬声器、并在 `Stats` 里报出两个设备。
- 音频设备与视频设备在架构上对称了 —— 一套心智模型。
- `audio_device` 模块的既有后端**零改动**即可被引擎使用（只加注册，不动实现）。

### 3.2 代价 / 风险

1. **动了 `src/core/**`** —— 注册表是核心契约。缓解：全部**追加**（新枚举值、新方法、新槽位），不改既有语义；按 ADR-015 属可接受变更。
2. **新增公开头**（若引擎要暴露新配置类型）→ 要同步 `docs/api/public_headers.manifest`。
3. **`EngineConfig` 加字段** → `docs/api/engine_api.md` 要补文档。
4. **端口占用**：引擎打开真实麦克风/扬声器会占独占设备（Windows 的 WASM 独占模式尤其明显）。缓解：设备打开失败**不是致命错误** —— 降级为「无设备」并 `warn`，而不是让整个 `open()` 失败（理由：音频是可选的，视频失败才是致命的）。**这一条与视频侧的既有语义不同**（视频侧非空 id 打开失败是 `kVideoCaptureOpenFailed`），需要在规格里写清楚并单独定测试。
5. **CI 机器没有声卡** → `enumerate_devices()` 返回空是**合法答案**（接缝注释里明说了）。CI 上必须用 `synthetic` 后端兜住，D1 的验收要在合成路径上跑。

## 4. 备选方案与否决理由

### 备选 A：引擎侧可注入的解析器（不动 `core/`）

**否决**。理由见 §2.1。而且它把「没配就用默认设备」这条 D1 的核心承诺从「引擎自己决定」变成「宿主必须先交进来」—— 而 D1 要的恰恰是**宿主什么都不配**。

### 备选 B：改 `audio_source_name` 的空值语义为「用默认」

**否决**。与 D3 的决定一致：空值含义**完全不变**（宿主自己喂），`"auto"` 是新增的显式取值。理由：今天所有不配采集设备的宿主（含大量既有测试）会突然开始自动开真实设备。

### 备选 C：把音频默认策略直接放进 D3 的 `DevicePolicy` 表

**否决**。D3 的策略表已经给音频留了预留字段（`audio_capture_backend` / `audio_playback_backend`），但那张表是**视频侧**的（`engine_device_policy.hpp`，私有）。D1 落地后**应该**回填那张表 —— 但那是 D1 范围内的**一个后续动作**，不改变本 ADR 的核心决策。回填与否在 D1 的规格里单独定。

## 5. 验证方式

本 ADR 的每条决策都有可判定的验收标准，落在 `specs/slices/v1-audio-device-engine.md`：

| 决策 | 验收方式 |
|---|---|
| 注册表加两个槽位 | 单元测试：注册一个假 `IAudioSourceFactory`，`get_audio_source(id)` 返回它；`list_audio_sources()` 含它 |
| `audio_device` 注册后端 | 单元测试：调 `register_default_plugins()` 后，`list_audio_sources()` 非空且含 `"synthetic"`；`list_audio_sinks()` 含 `"synthetic"` |
| 解析路径唯一 | 单元测试：`pal::resolve_audio_source("synthetic")` 与模块内 `source_factory("synthetic")` 返回**同一指针** |
| `EngineConfig` 字段 | 单元测试：`audio_source_name` 为空 → 引擎不实例化设备（与今天一致）；为 `"auto"` → 引擎按策略解析 |
| 引擎设备泵 | 单元测试：假 source 产帧 → 引擎出站路径收到；假 sink 收 PCM → `played_samples()` 前进 → `set_render_delivered()` 被调用 |
| 设备打开失败可降级 | 单元测试：`"auto"` 解析到的后端 `open()` 失败 → `open()` **成功**、`warn` 一行、音频降级为「无设备」 |
| 端口占用语义 | 同上，与视频侧语义**不同** —— 需要单独断言 |
| 不加新线程 | 静态检查：音频泵的调用点在 `tick()` 链路上；`src/engine/src/` 无新增 `std::thread` |
| 依赖方向 | 静态检查：`src/engine/` 不引用 `nimrtc::audio_device`；`src/core/` 不引用 `src/modules/` |

## 6. 状态

Proposed —— 等人签字。本 ADR 覆盖 §D1 的**架构决策**；D1 的切片规格在 `specs/slices/v1-audio-device-engine.md`。
