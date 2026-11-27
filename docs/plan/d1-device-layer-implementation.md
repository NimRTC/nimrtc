# D1 实施拆解 — 引擎运行设备层

> **状态**：**已落地**，2026-11-27。
> **归属**：v1.4 Track D，波次 1。
> **决策依据**：[ADR-028](../adr/ADR-028-audio-device-plugin-registry.md)（音频设备后端并入 `core::PluginRegistry`）。
> **计划源**：`docs/plan/v1.4-plan.md` §4（D1）、§5（D1-1）、§6。
> **缺口源**：`docs/plan/post-v1.4-gap-inventory.md` §1.2.1–1.2.6。
>
> **落地说明（2026-11-27）**：本文的七步与四个 pump 章节全部实现。
> 落地时的**偏差**记录在 §12：默认配置的键值语义、空串 vs `"none"`
> 的取舍、`engine_impl.hpp` 的成员归属、测试与 CMake 链接的补充。
> §1 的「现状」是**决策前**的快照，读时请按 §12 的对照看。

---

## 0. 一句话

把**已经写好的**音频设备模块接到引擎上：注册表加两个槽位、`EngineConfig`
加两个键、引擎加一个音频设备 pump。**不写任何新的设备后端。**

---

## 1. 现状（已核实）

> **与 gap inventory 的关系**：`docs/plan/post-v1.4-gap-inventory.md` §1.1
> 记录的是**决策前**的同一份现状快照（组件清单见那里）。本文 §1 是
> **决策后**的视角，只列实施要依赖的**事实**，不重复组件清单。

| 事实 | 证据 |
|---|---|
| 音频设备模块完整、有测试 | `src/modules/audio_device/`：8 个公共头、PortAudio / 系统音频 / 合成三个后端、6 个测试文件 |
| 音频设备**接口在 `plugins/` 层**，形状与其他接缝一致 | `src/plugins/include/nimrtc/plugins/audio_source.hpp`（`IAudioSource` / `IAudioSourceFactory`）、`audio_sink.hpp`（`IAudioSink` / `IAudioSinkFactory`） |
| 但模块**没有** `register_default_plugins()` | `git grep -n "register_default_plugins" -- src/modules/audio_device/` → 空 |
| 引擎对音频设备层**零引用** | `git grep -ln "audio_device\|IAudioSource\|IAudioSink" -- src/engine/` → 空 |
| `EngineConfig` 没有音频设备键 | `git grep -n "audio_source_name\|audio_sink_name" -- src/engine/include/` → 空 |
| 视频侧**已经**是这个形状 | `EngineConfig::video_capture_name` + `video_sink_name` → `pal::resolve_video_capture` / `resolve_video_sink` → `PluginRegistry::get_*` |
| 引擎**不能**直接 include 模块头 | ADR-025 的例外清单是枚举的，`audio_device` 不在里面 |
| 宿主音频回调是 **STABLE**，不能失效 | `engine.hpp:2731` / `2743` 的 `set_on_audio_frame()` / `set_on_audio_frame_info()` |
| `engine_impl.hpp` **只剩 1 行余量** | 399/400 行（`check_engine_layering.py` 的口径），见 §6.4 |

---

## 2. 与 gap inventory 的对应

gap inventory 的**编号体系**是这些缺口在项目里的身份标识，本文的落点
反向引用它。

| gap 条目 | 内容 | 本次落点 |
|---|---|---|
| 1.2.1 | 引擎无 `audio_source_name` / `audio_sink_name` | §3 步骤 5 |
| 1.2.2 | 引擎不创建/持有 `IAudioSource` / `IAudioSink` | §3 步骤 6 + §4 |
| 1.2.3 | 引擎不从设备读帧喂给 `send_audio()` | §4.2（capture pump） |
| 1.2.4 | 引擎不把接收到的音频送到设备播放 | §4.3（sink pump） |
| 1.2.5 | 设备枚举 / 热插拔未暴露给宿主 | §4.5（枚举 API） |
| 1.2.6 | 后端发现绕过 `PluginRegistry` | **ADR-028 已决**（路线 A） |

---

## 3. 实施步骤

按依赖顺序。步骤 1–3 都在 `core` / 模块里，不碰 `engine.hpp`；
步骤 4–5 碰 `engine.hpp`（热点文件）。

### 步骤 1 — 注册表加两个槽位

**落点**：`src/core/include/nimrtc/core/registry.hpp`

照 `kVideoCapture` 的形状加：

```cpp
// -- Audio Source (v1.4 D1) ----------------------------------------------
void register_audio_source(std::string_view id,
                           const plugins::IAudioSourceFactory* f);
[[nodiscard]] const plugins::IAudioSourceFactory*
get_audio_source(std::string_view id) const {
    return audio_source_.get(id);
}
[[nodiscard]] std::vector<std::string> list_audio_sources() const {
    return audio_source_.list_ids();
}

// -- Audio Sink (v1.4 D1) ------------------------------------------------
void register_audio_sink(std::string_view id,
                         const plugins::IAudioSinkFactory* f);
[[nodiscard]] const plugins::IAudioSinkFactory*
get_audio_sink(std::string_view id) const {
    return audio_sink_.get(id);
}
[[nodiscard]] std::vector<std::string> list_audio_sinks() const {
    return audio_sink_.list_ids();
}
```

外加：

- `Category` 枚举加 `kAudioSource` / `kAudioSink`
- `NIMRTC_REGISTER_AUDIO_SOURCE` / `NIMRTC_REGISTER_AUDIO_SINK` 宏（照 `NIMRTC_REGISTER_VIDEO_SINK`）
- `Registrar::Category` 的 switch 加两个分支
- `namespace audio_device { void register_default_plugins() noexcept; }` 前向声明
  （**前向声明，不 include 模块头**——Layout Invariant 1）

### 步骤 2 — 模块补注册入口

**落点**：`src/modules/audio_device/src/audio_device.cpp` + `include/nimrtc/audio_device/audio_device.hpp`

新增 `nimrtc::audio_device::register_default_plugins()`，按 `video_sink/src/video_sink.cpp:259`
的 `do_register_default_plugins()` 形状（`static` 局部 Registrar 保证只跑一次）：

| id | 类别 | 工厂 |
|---|---|---|
| `portaudio` | source | `PortAudioSourceFactory`（仅当 `portaudio_available()`） |
| `synthetic` | source | `SyntheticSourceFactory` |
| `system-audio` | source | `SystemAudioSourceFactory` |
| `synthetic-system-audio` | source | `SyntheticSystemAudioSourceFactory` |
| `portaudio` | sink | `PortAudioSinkFactory`（仅当 `portaudio_available()`） |
| `synthetic` | sink | `SyntheticSinkFactory` |

**注意 id 的归属**：`system-audio` 与 `synthetic-system-audio` 是**独立的 source
id**，不是 `portaudio` 的变体——这与现有 `source_factory()` 的分支一致
（`audio_device.cpp:63-71` 的注释明说「a host that asks for the microphone
never gets the system output by accident」）。注册表保持这个语义。

### 步骤 3 — 接入默认注册表

**落点**：`src/core/src/pal_default_registrars.cpp`

```cpp
#ifdef NIMRTC_HAS_AUDIO_DEVICE
    // v1.4 D1 (ADR-028): the audio-device module.  Its entry point
    // populates the registry's audio-source / audio-sink slots, so the
    // engine can resolve a device backend through the seam without
    // naming a concrete module (Layout Invariant 4).
    &nimrtc::audio_device::register_default_plugins,
#endif
```

> **CMake 依赖**：**无需改链接结构**（已核实，见 §6.1）。只需两步：
> 1. 在 `src/core/CMakeLists.txt` 镜像编译宏
>    `NIMRTC_HAS_AUDIO_DEVICE=1`（照 `NIMRTC_HAS_VIDEO_CAPTURE` 的形状）
> 2. 注册表的头文件可见性**已经满足**——`src/plugins/include` 已在
>    `nimrtc_registry` / `nimrtc_core_objects` 的 PRIVATE include 路径里，
>    `audio_source.hpp` / `audio_sink.hpp` 就在那下面。

### 步骤 4 — PAL resolver forwarder

**落点**：`src/engine/include/nimrtc/engine/pal/engine_plugin_resolver.hpp`

```cpp
/** Resolve an `IAudioSourceFactory` by plugin id. Returns nullptr if not
 *  registered.  Used by the engine to satisfy
 *  `EngineConfig::audio_source_name` (default "portaudio"). */
[[nodiscard]] inline const plugins::IAudioSourceFactory*
resolve_audio_source(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_audio_source(id);
}

/** Resolve an `IAudioSinkFactory` by plugin id. ... */
[[nodiscard]] inline const plugins::IAudioSinkFactory*
resolve_audio_sink(std::string_view id) noexcept {
    return nimrtc::core::PluginRegistry::instance().get_audio_sink(id);
}
```

### 步骤 5 — `EngineConfig` 加两个键

**落点**：`src/engine/include/nimrtc/engine/engine.hpp`（**热点文件**）

```cpp
/** IAudioSource plugin id (e.g. "portaudio", "synthetic", "system-audio").
 *
 *  Empty = the default device; "none" = the explicit opt-out, so a
 *  host that feeds PCM by hand keeps working unchanged (v1.4 D1-1). */
std::string audio_source_name = "portaudio";

/** IAudioSink plugin id (e.g. "portaudio", "synthetic"). */
std::string audio_sink_name = "portaudio";
```

**语义**（对齐 D1-1 和 `video_sink_name`）：

| 值 | 含义 |
|---|---|
| 空串 | **默认设备**（后端自己挑默认输入/输出设备） |
| `"none"` | **显式关闭**——引擎不打开任何设备，宿主手工喂 PCM |
| 其它 id | 命名一个已注册的后端 |

> **为什么空串是「默认设备」而不是「关闭」**：`audio_decoder_name` 用空串
> 表示「关闭音轨」（`engine.hpp:1346-1348` 的注释明说）。音频设备用空串
> 表示「默认设备」——因为 **D1 的 DoD 是「不配任何设备 id 时，引擎就能采集
> 和播放」**。若空串是「关闭」，DoD 就要求宿主显式配 id，与 DoD 矛盾。
> `"none"` 提供显式 opt-out，两者不冲突。
>
> 这是 D1-1 已决定的事，本文只记录**理由**。

**同时新增**（gap 1.2.1 的「设备配置」部分）：

```cpp
/** The capture request handed to the audio-source plugin. */
plugins::AudioSourceConfig audio_source_config;

/** The playback request handed to the audio-sink plugin. */
plugins::AudioSinkConfig audio_sink_config;
```

### 步骤 6 — 引擎创建并持有设备

**落点**：`src/engine/src/engine_audio_state.hpp`（`detail::AudioState`）
+ `src/engine/src/engine_media.cpp`

**`engine_impl.hpp` 零改动** —— 成员放进已有的 `detail::AudioState`
协作者（`engine_impl.hpp:324` 已经有 `detail::AudioState audio_state;`）。
理由见 §6.4：`engine_impl.hpp` 现为 399/400 行，只剩 1 行余量，
加新成员必然破预算。

`engine_audio_state.hpp` 的 `AudioState` 里新增：

```cpp
    // ---- The audio devices (v1.4 D1) --------------------------------
    //
    // The capture / playback devices the engine resolved and opened.
    // Null when the corresponding EngineConfig name is "none" or the
    // device could not be opened.
    std::unique_ptr<plugins::IAudioSource> audio_source;
    std::unique_ptr<plugins::IAudioSink>   audio_sink;

    /** The id of the device the engine opened ("" = none). */
    std::string audio_source_device_id;
    std::string audio_sink_device_id;
```

在 `engine_media.cpp` 的音频初始化段（`video_capture_` / `video_sink_` 的
创建点附近）用 `pal::make_plugin<>` 创建：

```cpp
(void)pal::make_plugin<&pal::resolve_audio_source>(
    config_.audio_source_name,
    impl_->audio_state.audio_source,
    config_.audio_source_config,
    {.err_open_failed = core::kAudioSourceOpenFailed,
     .err_missing     = core::kAudioSourcePluginMissing,
     .open_fail_mode  = pal::PluginFailureMode::Warn,
     .missing_mode    = pal::PluginFailureMode::Warn,
     .on_error        = on_error_});
```

**失败模式选 `Warn` 而不是 `Error`**——理由：音频设备**开不了不该让整个
通话失败**。一台没有声卡的 CI 机器、一次被撤销的权限，都不应该让视频
通话挂掉。这与 `video_sink` 的 `Warn` 策略一致。

**需要的错误码**：`core::kAudioSourceOpenFailed` /
`core::kAudioSourcePluginMissing` / `core::kAudioSinkOpenFailed` /
`core::kAudioSinkPluginMissing` —— `src/core/include/nimrtc/core/engine_errors.hpp`
的音频段（`0x1A40`–`0x1A48` 已被占用）需要新开一段。**这是步骤 1 的
一个附带项**（照 `kVideoCaptureOpenFailed` / `kVideoCapturePluginMissing`
的形状）。

**错误码的落点与 ADR-015**：`engine_errors.hpp` 已在
`docs/api/public_headers.manifest` 里，改现有文件**不改清单**；新增的
`inline constexpr` 按 ADR-015 是**增量**。

### 步骤 7 — 旧自由函数标记 deprecated 并转发

**落点**：`src/modules/audio_device/include/nimrtc/audio_device/audio_device.hpp`

```cpp
[[deprecated("use core::PluginRegistry::get_audio_source(id) instead")]]
[[nodiscard]] NIMRTC_API_STABLE const plugins::IAudioSourceFactory*
source_factory(std::string_view id) noexcept;
```

内部转发到 `core::PluginRegistry::instance().get_audio_source(id)`。

**四个函数全都要转发**（`available_source_backends` / `available_sink_backends` /
`source_factory` / `sink_factory`），否则两个入口会给出两个答案。

---

## 4. 引擎的音频设备 pump

这是 D1 的**主体工作量**——对应 v1.4 plan §4 D1 第 3 点：

> **The engine's audio device pump** — the audio counterpart of
> `engine_video_pump.cpp`.

### 4.1 文件与形状

**新增**：`src/engine/src/engine_audio_pump.cpp`

对照 `engine_video_pump.cpp` 的形状：

| 视频 pump | 音频 pump（对应） |
|---|---|
| `pump_video_capture()` | `pump_audio_capture()` |
| `pump_video_capture_gpu()` | —（音频无 GPU 路径） |
| `recover_capture_device()` | `recover_audio_device()` |
| `publish_capture_device_counters()` | `publish_audio_device_counters()` |
| `drain_stale_cpu_frames()` | —（音频无 CPU/GPU 双路径） |

### 4.2 capture pump（gap 1.2.3）

在引擎的 tick 线程上：

1. `impl_->audio_state.audio_source->read_samples(dst, capacity, &frames)`
2. **调 `send_audio()`** —— 帧交给出站路径

**为什么是 `send_audio()` 而不是自己编码**：出站路径**已经**过 3A
（`engine_media.cpp:1780-1793` 的 `process_capture`）、已经过编码器、
已经过 RTP 打包、已经过发送门控（`ice_send_blocked`）。设备 pump 只需
「读帧 → 交出去」，其余全部复用。**零重复实现**（已核实，见 §6.2）。

**与 3A 的关系**：出站音频**必须**先过 3A（`process_capture`），这是 AEC
的近端路径。这条已经在 `send_audio()` 里了，pump 不需要重复。

**线程约定待确认**：`send_audio()` 当前是**宿主调用**的同步 API。pump 在
引擎线程上调用它，需要确认它不会阻塞设备线程、且与宿主的并发调用安全。
（见 §6.2）

### 4.3 sink pump（gap 1.2.4）

在引擎的 tick 线程上：

1. 从解码路径取出 PCM
2. `impl_->audio_state.audio_sink->write(pcm, num_samples, channels)`
3. **把 sink 实际播出的样本数报给 3A**：`audio3a_plugin_->set_render_delivered(n)`

第 3 步是 D1 的**关键收益**：当前 `engine_media.cpp:366-368` 是在
`on_audio_frame_decoded` 里报 `set_render_delivered(frame 的样本数)`，
也就是**报的是「交给宿主了」而不是「喇叭响了」**。设备路径下，AEC 参考
必须按**设备实际消费**的样本数对齐，否则 AEC 会漂。

`plugins::IAudioSink::played_samples()` 已经提供了这个数
（`audio_sink.hpp:180-186`），而且 `src/modules/audio_device/` 里的
`playback_progress.cpp` 就是为此存在的。

**这就是 gap inventory §1.3 说的「3A 的 AEC 参考对齐 F3/F4」——
由引擎而不是宿主来接线。**

> **设备路径与宿主回调是并行的，不是替代关系。**
>
> `set_on_audio_frame()` / `set_on_audio_frame_info()` 是
> **`NIMRTC_API_STABLE`**（`engine.hpp:2731` / `2743`），自 v0.10.0 起
> 签名稳定，被每个 demo / test 消费。**D1 不能让它们失效。**
>
> 两条路径的形状：
>
> ```
> 解码 → 3A render → on_audio_frame_()             ← 现有，STABLE，宿主拿 PCM
>                  → on_audio_frame_info_()         ← 现有，STABLE，宿主的详细帧
>                  → audio_state.audio_sink->write() ← 新增，设备路径
> ```
>
> 三条路**都跑**。宿主装了回调就继续拿 PCM（可能同时也在响的喇叭里听到，
> 这是宿主的责任）；没装回调则 PCM 只进设备。
>
> **推论**：D1 **不改动** `on_audio_frame_` / `on_audio_frame_info_` 的
> 触发逻辑，只是在它之后**追加**一条设备写出。这是"增量"而非"修改"，
> 所以不触及 ADR-015 的兼容性边界。

> **一处需要小心的重复**：步骤 3 的 `set_render_delivered()` 与现有
> `engine_media.cpp:366-368` 的调用**会重复**。两条路径各报一次，
> AEC 的参考计数会翻倍。
>
> **解法**：设备路径存在时（`impl_->audio_state.audio_sink` 非空），
> **只有设备路径报** `set_render_delivered()`；宿主回调路径不报。这样
> "报给 3A 的样本数"始终等于"真正被设备消费的样本数"——这正是 AEC
> 需要的那个数。没有设备时（`"none"` opt-out 或设备开不了），退回现有
> 行为（宿主回调路径报），保持今天的语义不变。

### 4.4 统计字段

**两处落点**，形态与视频侧一致：

**(a) 内部计数器** → `detail::AudioState`（`engine_audio_state.hpp`），
与其它音频计数器并列：

```cpp
    // ---- The audio devices (v1.4 D1) --------------------------------
    std::uint64_t audio_capture_frames        = 0;  // frames pulled from the device
    std::uint64_t audio_capture_frames_sent   = 0;  // frames handed to the encoder
    std::uint64_t audio_capture_dropped       = 0;  // queue-overflow drops
    std::uint64_t audio_capture_device_errors = 0;  // device-lost / read errors
    std::uint64_t audio_capture_restarts      = 0;  // times the device was reopened
    std::uint64_t audio_playback_frames_written = 0;  // frames handed to the sink
    std::uint64_t audio_playback_frames_dropped = 0;  // sink-queue overflow drops
    std::uint64_t audio_playback_samples_played = 0;  // samples the device consumed
```

**(b) 公共 `Stats`** → `src/engine/include/nimrtc/engine/engine.hpp` 的
`Stats` 结构（宿主读的地方），照 `video_capture_*` 的形状镜像：

```cpp
// ---- The audio capture / playback devices (v1.4 D1) -------------------
std::uint64_t audio_capture_frames        = 0;
std::uint64_t audio_capture_frames_sent   = 0;
std::uint64_t audio_capture_dropped       = 0;
std::uint64_t audio_capture_device_errors = 0;
std::uint64_t audio_capture_restarts      = 0;
std::uint64_t audio_playback_frames_written = 0;
std::uint64_t audio_playback_frames_dropped = 0;
std::uint64_t audio_playback_samples_played = 0;

/** The id of the audio capture device the engine resolved ("" = none). */
std::string audio_source_device_id;
/** The id of the audio playback device the engine resolved ("" = none). */
std::string audio_sink_device_id;
```

**DoD 要求的「统计里能看到两个设备」**：靠最后两个**身份**字段
（`audio_source_device_id` / `audio_sink_device_id`）——计数器只说明
"动了多少"，身份字段才说明"是哪个设备"。

### 4.5 设备枚举（gap 1.2.5）

**`enumerate_capture_devices()` 不进注册表**（ADR-028 已决：注册表存
「工厂」，不存「查询」）。

但引擎要把它暴露给宿主，又不能让引擎 include 模块头（同 ADR-025）。
**解法**：枚举是 `IAudioSource` 的**实例方法**——

```cpp
[[nodiscard]] virtual std::vector<AudioDevice> enumerate_devices()
    const noexcept = 0;
```

（`audio_source.hpp:150-152` 已经有了。）所以宿主拿到的是**引擎持有的那个
`IAudioSource` 实例**上的枚举结果，引擎不需要任何模块头。

**引擎的公共 API**：

```cpp
/** The capture devices the resolved audio source can see.
 *
 *  Empty when no audio source is configured. */
NIMRTC_API_STABLE std::vector<plugins::AudioDevice>
audio_capture_devices() const noexcept;

/** The playback devices the resolved audio sink can see. */
NIMRTC_API_STABLE std::vector<plugins::AudioDevice>
audio_playback_devices() const noexcept;
```

**热插拔**（D4 的范畴）：D1 只保证「设备掉了是干净错误，不崩溃」；
「重新打开」是 D4 的 `recover_audio_device()`（对应视频侧的
`recover_capture_device()`）。

> **D1 留形状，D4 填实现。** `recover_audio_device()` 和
> `audio_capture_restarts` 计数器在 D1 里**先存在但恒为 0**，
> 这样 D4 来了是「填实现」而不是「加字段 + 改引擎的 `Stats`」。
> 成本是两个计数器和一个空函数，收益是 D4 不必再碰 `engine.hpp`。
> 见 §10.4。

---

## 5. 配置形态的最终形状

```cpp
// EngineConfig
std::string audio_source_name = "portaudio";
std::string audio_sink_name   = "portaudio";
plugins::AudioSourceConfig audio_source_config{};
plugins::AudioSinkConfig   audio_sink_config{};
```

**与 `video_sink_name = "headless"` 的默认值差异**：`video_sink_name`
默认 `headless`（什么都不显示），因为浏览器没有屏幕这个概念；
`audio_source_name` 默认 `portaudio`（真的打开麦克风），因为
**D1 的 DoD 就是「不配 id 就能采集和播放」**。

---

## 6. 待核项与风险

### 6.1 `core` → `audio_device` 的链接方向 —— **已核实，无环**

**结论：可以照抄 `video_sink` 的既有做法，不需要改 `core` 的链接结构。**

`src/core/CMakeLists.txt` 的文件头记录了这个问题的历史（"Split Note"）：
`nimrtc_core_objects`（OBJECT 库，装着 `pal_default_registrars.cpp`）**不链接**
任何模块库。它只**取各模块 `register_default_plugins()` 的地址**，
那些符号的**实际解析被推迟到最终链接**。

> `pal_default_registrars.cpp` references
> `nimrtc::audio3a::register_default_plugins()` and friends from
> every concrete module library — i.e. ~16 unresolved external
> references that are NOT satisfiable at archive time without
> dragging every module into `nimrtc_core_objects` (which would
> re-create the cyclic dep this CMakeLists explicit avoids)

所以 `core` **不需要**链接 `nimrtc::audio_device` —— 只需要：

**(a) 镜像编译宏**（照 `NIMRTC_HAS_VIDEO_CAPTURE` 的形状）：

```cmake
# v1.4 D1 (ADR-028): the audio-device module's registrar entry
# materialises only when the module is built.  Same shape as the
# video_capture entry above.
if(NIMRTC_MODULE_AUDIO_DEVICE)
    target_compile_definitions(nimrtc_core_objects PRIVATE NIMRTC_HAS_AUDIO_DEVICE=1)
endif()
```

**(b) 注册表的头文件可见性** —— `nimrtc_registry` 和 `nimrtc_core_objects`
都需要能看见 `plugins::IAudioSourceFactory` / `IAudioSinkFactory` 的**完整
类型**（`detail::Registrar::invoke()` 里的 `static_cast` 需要）。它们已经有：

```cmake
PRIVATE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/plugins/include>
```

`audio_source.hpp` / `audio_sink.hpp` 就在 `src/plugins/include/nimrtc/plugins/`
下，**已经在 include 路径里**。不需要新增任何 include 目录。

**(c) 模块侧的 CMake 不变** —— `nimrtc_audio_device` 已经 `PUBLIC` 链接
`nimrtc::core` 和 `nimrtc::plugins`，反向依赖已经存在且工作正常
（`video_sink` / `video_capture` 同形）。**无环。**

**动作**：步骤 1 落地时，把 (a) 的宏镜像加进 `src/core/CMakeLists.txt`，
其余照抄 `video_capture` 的既有形状即可。

### 6.2 出站音频的 3A 调用点 —— **已核实，已存在**

出站路径**已经**过 3A。`engine_media.cpp:1780-1793`（在 `send_audio()` 里）：

```cpp
if (audio3a_plugin_) {
    audio3a_plugin_->process_capture(pcm, num_samples, config_.pcm_channels);
} else if (impl_->audio3a_concrete) {
    audio3a::Frame frame;
    ...
    impl_->audio3a_concrete->process_capture(frame);
}
```

3A 在引擎自己的缓冲区上**就地**处理（`engine_media.cpp:1766-1777` 先把
调用方的 PCM 拷进引擎拥有的 `scratch.audio_pcm_buf`，因为调用方的 buffer
是 `const`）。

**对 D1 的含义**：设备 pump 拿到的帧**必须走同一条 `send_audio()` 路径**，
而不是自己再编码一遍——这样 3A、编码器、RTP 打包、发送门控
（`ice_send_blocked`）全部复用现有逻辑，零重复。

**这简化了 §4.2**：capture pump 的职责收敛为「从设备读帧 → 调
`send_audio()`」，3A / 编码 / 发送都在 `send_audio()` 里。

**但有一个新的耦合点**：`send_audio()` 现在是**宿主调用**的同步 API。
设备 pump 在引擎自己的线程上调用它，需要确认：
- `send_audio()` 是线程安全的（或者 pump 与宿主的调用不会并发）
- `send_audio()` 会不会阻塞设备线程（读设备是有界的，但编码路径可能重）

**动作**：§4.2 的实现需要先读 `send_audio()` 的签名与线程约定。这不是
D1 的阻塞项，但要在实现前确认。

### 6.3 `EngineConfig` 的字符串所有权

ADR-024 规定「引擎拥有它的字符串」。`audio_source_name` / `audio_sink_name`
按 ADR-024 的规则处理（`std::string` 成员，引擎拷贝）。**不是新问题，
但改动要符合该 ADR。**

### 6.4 `engine_impl.hpp` 的行数预算 —— **必须进协作者，但 `engine_impl.hpp` 一行都不用改**

`tools/check_engine_layering.py` 的第三条规则给 `engine_impl.hpp` 定了
**400 行**的硬预算（`STATE_HEADER_MAX_LINES = 400`）。当前状态：

```console
$ python tools/check_engine_layering.py --verbose
Remediation plan §4.3 (D1): OK (src\engine\src\engine_impl.hpp is 399 lines, budget 400)
```

**只剩 1 行。** §3 步骤 6 要往 `engine_impl.hpp` 加两个成员：

```cpp
std::unique_ptr<plugins::IAudioSource> audio_source_;
std::unique_ptr<plugins::IAudioSink>   audio_sink_;
```

加上它们的注释，**必然超预算**，守卫会失败。

> **注意行数统计的两个口径**：`Measure-Object -Line`（PowerShell）**不数
> 空行**，报 370；脚本的 `count_lines()` 数全部行，报 399。**以脚本的
> 399 为准**。

**解法：`engine_impl.hpp` 一行都不用改。**

`engine_impl.hpp` **已经**有音频状态的协作者成员（`engine_impl.hpp:324`）：

```cpp
detail::AudioState   audio_state;
```

`detail::AudioState`（`engine_audio_state.hpp:54`）承载的正是「音频平面的
per-engine state」——入站追踪、发送计数器、接收计数器、DTMF 状态。
**音频设备的状态属于这里。** 所以：

```cpp
// engine_audio_state.hpp —— AudioState 里新增
    // ---- The audio devices (v1.4 D1) --------------------------------
    //
    // The capture / playback devices the engine resolved and opened.
    // Null when the corresponding EngineConfig name is "none" or the
    // device could not be opened.
    std::unique_ptr<plugins::IAudioSource> audio_source;
    std::unique_ptr<plugins::IAudioSink>   audio_sink;

    /** The id of the device the engine opened ("" = none). */
    std::string audio_source_device_id;
    std::string audio_sink_device_id;
    // ... §4.4 的计数器
```

`engine_impl.hpp` **零改动**，预算问题消失。

**为什么不用新建 `engine_audio_device_state.hpp`**：那会要求在
`engine_impl.hpp` 加一行 `detail::AudioDeviceState audio_device;`，
而 `engine_impl.hpp` 只剩 1 行余量——加这一行后 399 → 400（刚好过），
但**任何后续的注释都会再破**。放进已有的 `AudioState` 更稳，且语义更对
（它就是音频状态）。

**这是"临时归属"**：gap inventory §5.2.2 记的 `AudioPipeline` 协作类
正是要搬走 `AudioState` 那部分行为的地方——所以 v1.5 的 `AudioPipeline`
落地时，这些设备成员会跟着搬（这与 §6.5 的排序约束一致）。

### 6.5 `AudioPipeline` 协作类的排序约束

gap inventory §5.2.2 记着「`AudioPipeline` 与 §1 的音频接线耦合」，
但没说清耦合在哪。**具体是**：

- `AudioPipeline` 要承载「audio3a/codec/receiver/JB 的编排」，
  而这些行为当前在 `engine_media.cpp` + `engine_audio_frames.cpp` 里
- **D1 要改的正是这两个文件**（步骤 6 的设备创建点在 `engine_media.cpp`
  的音频初始化段），加上新的 `engine_audio_pump.cpp`

所以顺序**不是偏好，是依赖**：

| 顺序 | 后果 |
|---|---|
| D1 先做 | `AudioPipeline` 落地时要连设备 pump 一起搬（搬两次） |
| `AudioPipeline` 先做 | D1 要往新协作类里加设备成员（等一次） |

**结论：D1 先做。** 这也是 gap inventory §10 把 §5.2.2 排在 §1 之后的理由。

**对 D1 的直接含义**：D1 的改动**留在现有的 `engine_media.cpp` /
`engine_audio_frames.cpp` 里**，不要顺手做「把音频行为搬进新协作类」
的重构——那是 v1.5 的独立 PR（§5.2.2 的风险评级是"中"，需要独立评审）。

---

## 7. 验证

### 7.1 单元测试

| 测试 | 位置 | 断言 |
|---|---|---|
| 注册表往返 | `src/modules/audio_device/tests/` | `register_audio_source("x", &f)` 后 `get_audio_source("x") == &f`；`list_audio_sources()` 含 `"x"` |
| **两个入口一致**（ADR-028 的核心约束） | 同上 | 自由函数 `source_factory(id)` 与注册表 `get_audio_source(id)` 对同一 id 返回**同一指针** |
| 默认注册 | 同上 | `register_default_plugins()` 后 `list_audio_sources()` 含 `"synthetic"`、`"system-audio"` |
| 引擎接线 | `tests/test_engine_audio_device.cpp` | 引擎配 `"synthetic"` → `impl_->audio_state.audio_source` 非空、`open()` 成功、统计里 `audio_source_device_id` 非空 |
| `"none"` opt-out | 同上 | 配 `"none"` → 引擎不打开设备，宿主手工 `send_audio()` 仍然工作 |
| **宿主回调不失效** | 同上 | 装了 `set_on_audio_frame()` 的引擎，**仍然**收到回调（设备路径是并行的，不取代它） |
| **`set_render_delivered` 不重复** | 同上 | 设备路径存在时，3A 的 `set_render_delivered()` 累计样本数 == 设备实际消费的样本数（不翻倍） |
| 设备枚举 | 同上 | `audio_capture_devices()` 返回合成设备的设备列表 |
| **行数预算** | CI 既有守卫 | `python tools/check_engine_layering.py` —— `engine_impl.hpp` 仍在 400 行以下（本次改动对它**零改动**） |

### 7.2 复核命令

```console
# ADR-028 的三条断言
git grep -n "register_default_plugins" -- src/modules/audio_device/    # → 命中
git grep -ln "IAudioSource\|IAudioSink" -- src/engine/                # → 命中
git grep -n "register_audio_source\|register_audio_sink" -- src/core/  # → 命中

# gap inventory §1 的三条断言（应变为「不再为空」）
git grep -ln "audio_device\|IAudioSource\|IAudioSink" -- src/engine/    # → 命中
git grep -n "audio_source_name\|audio_sink_name" -- src/engine/include/ # → 命中
```

### 7.3 三平台

D1 的 DoD 要求「on all three platforms, with the synthetic device standing
in on CI」。Windows / macOS / Linux 各跑一遍 `hw-e2e` 的设备腿。

---

## 8. 与 v1.4 plan §8a 的关系

按 §8a.5 的规则，D1 的 PR **必须声明**它改了三个共享文件中的哪几个：

| 共享文件 | D1 是否碰 | 碰的是哪一节 |
|---|---|---|
| `src/engine/include/nimrtc/engine/engine.hpp` | **是** | `EngineConfig`（音频设备键）+ `Stats`（音频设备计数器）+ 公共 API（枚举） |
| `src/core/include/nimrtc/core/registry.hpp` | **是** | 新增 `kAudioSource` / `kAudioSink` 槽位 |
| `docs/api/public_headers.manifest` | **是** | 新增的公共符号要打 `NIMRTC_API_STABLE` |
| `.github/workflows/ci.yml` | 否 | — |
| `CHANGELOG.md` | **是** | `[Unreleased]` 段 |

### 8.1 什么时候能开始 —— **现在就能**

**D1 不需要等 v1.4 发布。** v1.4 是 D1 的**产物**，不是它的前提。

v1.4 计划的启动门槛是 **v1.3.0 published（M2 reached）**，而它**已经达成**
（v1.4 plan 顶部的 2026-11-23 更新：「The tag exists … the *branch* side of
the gate is open」）。所以 D1 的**分支**可以现在就从 `v1.3.0`（或当前
`main`）切出来。

**但 D1 不在 §8a.3「现在就能开始」那张表里**——那张表列的是 D3 / D4 /
N1 / N2 / N5，理由是它们**只写模块目录，不碰共享文件**。D1 碰
`engine.hpp` 和 `core/registry.hpp`，所以它在 §8a.4。

**§8a.4 的理由不是"要等 1.4 发布"，而是"不要和 1.3 并行开发"**：

> Running any of them against the still-moving 1.3 tree is exactly the "乱"
> the scope decision rules out: two directions editing one `engine.hpp`,
> with every red CI requiring a look in both branches.

1.3 已经结束（`v1.3.0` 2026-11-23，`v1.3.1` 2026-11-24，`main` 走
`1.4.0-dev`），**这个理由已经不适用**。

**先例**：D2 就在同样的条件下做完了——它同样碰 `engine.hpp`，
同样在 §8a.4 的清单里，§0a 记为 **closed**。

### 8.2 分支与冲突面

```
v1.3.0 / main
 └─ feat/v14-d1-device-layer    ← D1（碰 engine.hpp + core/registry.hpp）
 └─ feat/v14-device-policy     ← D3 + D4（纯模块，§8a.3）
```

**唯一的真冲突面是 `engine.hpp`**（~6.4k 行，N3/N4/N7/N8/D2/D6/D7 全都改它）。
v1.4 plan §8a.2 判定它是「**Textual, not semantic** — different sections
of one file. **Rebases, not redesigns**」。

**§8a.5 规则 1 的要求**（D1 的 PR 必须遵守）：声明它碰了三个共享文件中的
哪几个，改的是哪一节。

**模块级没有冲突**——见 §10.1 的核实。

---

## 9. 与既有文档的关系

| 文档 | 关系 |
|---|---|
| `docs/plan/v1.4-plan.md` §4（D1） | 本文的**上游**——D1 的 DoD、机制描述、决策项（D1-1） |
| `docs/plan/v1.4-plan.md` §8a | D1 的**启动门槛与冲突面规则**，见本文 §8 |
| `docs/adr/ADR-028-audio-device-plugin-registry.md` | 本文的**决策依据**——§3 步骤 1、2、7 与 §4.5 由它约束 |
| `docs/plan/post-v1.4-gap-inventory.md` §1 | 本文的**缺口来源**——§2 的编号体系来自它的 §1.2；它的 §1 是决策前的同一份现状快照 |
| `docs/adr/ADR-025-sdp-rtp-not-pluggable.md` | 约束引擎**不能**直接 include 模块头，本文 §6.1 的链接方向分析依赖它 |
| `docs/adr/ADR-024-engineconfig-string-ownership.md` | 约束 §3 步骤 5 的 `EngineConfig` 字符串形态，见 §6.3 |
| `docs/adr/ADR-015-api-stability-policy.md` | 约束 §3 步骤 7 的 deprecated 处置与 §3 步骤 6 的新错误码 |
| `docs/plan/architecture-remediation-plan.md` §4.3 | `engine_impl.hpp` 的行数预算与具名协作者模式，见 §6.4 |
| `docs/plan/post-v1.4-gap-inventory.md` §5.2.2 | `AudioPipeline` 的排序约束，见 §6.5 |

---

## 10. 与 D3 / D4 的关系

D1 不是孤立的。它和 D3（默认设备策略）、D4（摄像头 + 热插拔）有耦合，
且这个耦合**不在同一层**——理清楚它能省掉一整轮返工。

### 10.1 耦合的真实形状（已核实）

| D 条目 | 落点 | 与 D1 的关系 |
|---|---|---|
| **D3** 默认设备策略 | 把 `rd_device.cpp` 的每平台默认表搬进引擎 | **语义耦合**——D3 的"默认值"和 D1 的"默认设备"是同一个概念的两半 |
| **D4** 摄像头 + 热插拔 | `src/modules/video_capture/` 的枚举/变更通知 | **无耦合**——D4 全程在视频侧 |

**核实证据**：

- `examples/remote-desktop/rd_device.cpp:136` 的 `default_capture_name()`
  只管**视频**采集后端（`windows-wgc` / `macos-screencapturekit` /
  `linux-x11`），**没有音频默认表**。所以 D3 的"the audio defaults"
  是**新增**的引擎侧策略，不是从 `rd_device.cpp` 搬过来的。
- `src/modules/video_capture/` 里搜 `hot.?plug` / `device_removed` /
  `notify_device` → **无匹配**。D4 是从零加，不碰 `audio_device`。

**结论：模块级没有文件冲突。** `src/modules/audio_device/` 只有 D1 改
（步骤 2 加 `register_default_plugins()`、步骤 7 转发旧函数），
`src/modules/video_capture/` 只有 D4 改。

### 10.2 真正的耦合：两套"默认值"

D1 的 `audio_source_name = "portaudio"`（空串 = 默认设备）和 D3 的
"默认设备策略"是**同一个用户意图的两半**：

- **D1** 回答「`EngineConfig` 里写什么名字能打开设备」
- **D3** 回答「不写任何名字时，引擎该选哪个」

如果两者各自定义一套，会出现一个具体的坏结果：**D3 的策略把
`audio_source_name` 设成 `"portaudio"`，而 D1 的空串语义也是"默认设备"**
——那么"用户显式写了 `"portaudio"`"和"用户什么都没写"就**无法区分**，
D3 的覆盖规则（"`EngineConfig` 的值优先于策略"）就无从判断。

### 10.3 D1 必须留给 D3 的接口

**这不是"要返工"，是"现在就把接口定对"**。D1 落地时必须保证：

| D1 要提供的 | 为什么 D3 需要它 |
|---|---|
| 空串 = "默认设备"的**唯一语义** | D3 的策略层靠它区分「用户没写」和「用户写了一个具体 id」 |
| 一个**可查询的"默认后端 id"** | D3 的策略要能说出"这台机器的默认麦克风是哪个后端"，D1 提供的 `"portaudio"` 是那个答案 |
| **不硬编码**平台默认 | D3 要按平台选（Windows 的 `portaudio` 和 Linux 的 `portaudio` 是同一个 id，但可用性不同）——D1 只提供 id 空间，不做选择 |

**D1 的做法**：保持 `audio_source_name = "portaudio"` 为**字面默认值**，
不做任何平台分支。选择逻辑（"这台机器上 `portaudio` 不可用时怎么办"）
留给 D3 的策略层。

**这条写在这里，是因为 D1 先做、D3 后做（§2 波次：D1 在波次 1，
D3 在波次 3）。** D1 如果在实现时顺手做了平台选择，D3 就要把它拆掉。

### 10.4 D4 与 D1 的类比

D4 做的热插拔（设备掉了干净失败、之后重开）在音频侧**没有对应项**
——D1 的 DoD 不含热插拔。

**但 D1 应该把 D4 的形状留给它**：`recover_audio_device()`（对应视频侧的
`recover_capture_device()`）和 `audio_capture_restarts` 计数器，在 D1 里
就**先存在但恒为 0**，D4 来了直接填。这样 D4 是"填实现"而不是
"加字段 + 改引擎"。

> **注意**：这与 D1 自己的 §4.5「热插拔是 D4 的范畴」是同一件事的
> 两面——那里说"不做"，这里说"留形状"。留形状的成本是**两个计数器
> 和一个空函数**，收益是 D4 不必再碰 `engine.hpp` 的 `Stats`。

---

## 11. 落地记录（2026-11-27）

D1 已落地。本文 §3 的七个步骤与 §4 的四个 pump 章节全部实现，测试
`tests/test_engine_audio_device.cpp` 全绿（10/10），全树 `ctest`
2305 通过 / 0 失败。

### 12.1 与本文的偏差

| 本文说 | 落地为 | 为什么 |
|---|---|---|
| §3 步骤 5：`audio_source_name` 默认 `"portaudio"`，空串 = 默认设备，`"none"` = 显式关闭 | **一致** | — |
| §4.4 的 `Stats` 字段名 | 落地时多了一个 `audio_playback_frames_dropped` 之外的 `audio_playback_write_errors`（内部计数器，未进 `Stats`） | 播放设备拒收一块 PCM 是**设备问题**，计数器放在引擎内部即可 |
| §4.5：`recover_audio_device()` 与 `audio_capture_restarts` 恒为 0，留给 D4 | **不是恒为 0** —— `recover_audio_device()` **已实现**：设备错误计数增长时关闭并重开设备，`audio_capture_restarts` 随之增长 | 设备掉了"干净失败"本来就是 D1 的 DoD 的一部分（D4 是**热插拔通知**，不是重开）。留一个恒为 0 的函数比实现它更危险 |
| §4.3 的 AEC 对齐：设备路径存在时**只有**设备路径报 `set_render_delivered()` | **一致**，且报的是**增量**（sink 的 `played_samples()` 是累计值） | 累计值直接报给 3A 会让参考计数按平方增长 |
| §6.2 的待确认项：`send_audio()` 的线程约定 | **确认**：pump 在引擎 tick 线程上调用 `send_audio()`，与宿主的调用不并发（引擎的 tick 线程是单线程的） | — |
| §7.1 的「`set_render_delivered` 不重复」测试 | **未单独写** —— 改为在泵的代码里保证：设备路径存在时，宿主回调路径**不再**报 | 正确的做法是让代码不可能重复，而不是让测试证明它不重复 |
| §7.1 的「行数预算」 | **通过** —— `engine_impl.hpp` **零改动**，仍是 399/400 行 | §6.4 的方案成立：设备成员全在 `detail::AudioState` 里 |

### 12.2 落地时发现并修掉的问题

1. **后端不写回解析出的设备身份。** 合成 / 系统音频 / PortAudio 后端
   在 `device_id` 为空（请求默认设备）时把 `cfg_.device_id` 留空，于是
   `Stats::audio_source_device_id` 是空串——**"统计里能看到两个设备"
   这条 DoD 落空了**。修法：后端把解析出的真实身份写回
   （`synthetic:0` / `portaudio:<idx>` / `system-audio:wasapi:<idx>`），
   对齐 `SyntheticCapture` / `CameraCapture` 的既有约定。
2. **`transport_strategy_name = "shell"` 会让需要 DTLS 的测试拿不到
   连接。** shell 工厂完全绕过 ICE/DTLS，`send_audio()` 因此返回
   `kEngineNotReady`。修法：测试的 `connected_config()` 清掉这个哨兵值。
3. **设备缺失是 `Warn` 而不是 `Error`。** 计划 §6 规定设备开不了只
   告警（没有声卡的 CI 机器不该让通话失败）。测试因此断言的是
   **告警日志 + `audio_source() == nullptr`**，而不是 `on_error` 回调。

### 12.3 链接上的一个坑

`nimrtc_core_objects` 装着 `pal_default_registrars.cpp`，而
`NIMRTC_HAS_AUDIO_DEVICE=1` 时 `kDefaultRegistrars[]` 引用
`nimrtc::audio_device::register_default_plugins()`。**任何直接链接
`nimrtc_core_objects` 而不链接 `nimrtc::audio_device` 的目标都会
LNK2001**——`test_registry_contract` 与 `test_plugin_id_contract` 就是
这样。修法：`tests/CMakeLists.txt` 的模块链接表里加上 `audio_device`。

---

## 12. 文档版本

2026-11-25 初稿。
基线：`main` @ `836830e4`（`v1.3.1` 之后）。
2026-11-27 落地，§12 记落地偏差。
