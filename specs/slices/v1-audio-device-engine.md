# Slice: v1-audio-device-engine

## 目的

让**引擎自己**驱动音频设备层：没配设备 id 也能从默认麦克风采集、把解码出的音频播到默认扬声器、并在 `Stats` 里报出这两个设备。

对应 v1.4 计划书 §D1「the engine runs the device layer」。架构决策见 `docs/adr/ADR-029-engine-driven-audio-device-layer.md`（Proposed）。

> **基线分支**：本片从 **`version/v3`** 起（D3/D4 的 `db2dc0de`、D5 的 `6b5439da`、D6 的 `59b09f02` 都已签进 `version/v3`）。
> **不要**从 `feat/v14-turn-tls` 起 —— 那条分支只带 N1，**不带** D3/D4，本片要复用的 `engine_device_policy.hpp` / `"auto"` 解析在那边**不存在**。

今天的状态（已核实，on `version/v3`）：

- **注册表没有音频设备槽位**：`core::Registrar::Category` 只有 `kVideoCapture` 一类设备槽位（`src/core/include/nimrtc/core/registry.hpp:1539` 的 `enum class Category {`），没有 `kAudioSource` / `kAudioSink`。
- **引擎不认识音频设备**：`EngineConfig` 无 `audio_source_name` / `audio_sink_name`（`src/engine/include/nimrtc/engine/engine.hpp`）。
- **引擎没有音频设备泵**：`src/engine/src/` 只有 `engine_video_pump.cpp`；采集是宿主手推 `send_audio()`，播放是宿主从 `on_audio_frame()` 接 PCM 自己写设备。
- **工厂只能模块内解析**：`src/modules/audio_device/src/audio_device.cpp:55` 的 `source_factory(id)` / `sink_factory(id)` 是**模块的公开函数**（`src/modules/audio_device/include/nimrtc/audio_device/audio_device.hpp:306`），引擎**不能**用它 —— 依赖方向是 device→engine，反向不成立（已核实：`src/engine/` 里没有任何 `nimrtc::audio_device` 引用）。

### 与 D3/D4 的关系（别重复做、别越界）

只读 `specs/slices/v1-device-policy.md`（D3/D4，已完成）—— 它的「本片的边界」一节**明确**把音频留给本片：

> 核实结论：引擎里没有音频设备的承载体 …… 那属于计划书 §D1「引擎驱动设备层」，而 **D1 不在 v1 版本计划里**。所以本片：只做视频侧；策略表里**给音频预留位置**，字段存在但值为空。

所以：

- **本片负责**：注册表槽位、引擎配置字段、引擎侧设备泵、3A 参考对齐的接线。
- **本片不负责**：D3 的默认设备策略表的**视频**部分（已完成）、设备热插拔（D4，已完成）。
- **本片与 D3 预留字段的关系**：`engine_device_policy.hpp` 里预留的 `audio_capture_backend` / `audio_playback_backend` 两个字段，本片**可以**回填 —— 但那是**可选的**，回填与否不阻塞 D1 的验收（见「与 D3 预留字段的关系」一节）。

## 设计

### 一、注册表：两个新槽位

完全照 `kVideoCapture` 的既有模式（`src/core/include/nimrtc/core/registry.hpp` 的 `kVideoCapture` 那一组 + `src/core/src/plugin_registry.cpp:155` + `src/core/include/nimrtc/core/pal_default_registrars.cpp`）。

| 位置 | 变更 |
|---|---|
| `src/core/include/nimrtc/core/registry.hpp` | `Registrar::Category` 加 `kAudioSource` / `kAudioSink`；`PluginRegistry` 加 `register_audio_source()` / `get_audio_source()` / `list_audio_sources()`（及 sink 三件套）；私有槽位加 `TypedRegistry<plugins::IAudioSourceFactory> audio_source_{"audio-source"}`（及 sink）；`Registrar` 的 `switch` 加两条分支；加 `NIMRTC_REGISTER_AUDIO_SOURCE` / `NIMRTC_REGISTER_AUDIO_SINK` 两个宏 |
| `src/core/src/plugin_registry.cpp` | 两个 `register_*` 的实现（与 `register_video_capture` 同形） |
| `src/engine/include/nimrtc/engine/pal/engine_plugin_resolver.hpp` | 加 `resolve_audio_source(id)` / `resolve_audio_sink(id)`（与 `resolve_video_capture` 同形） |

**`core → plugins` 是既有依赖**（只读 `src/core/CMakeLists.txt` 里 `nimrtc_core` 链接 `nimrtc::plugins`；`registry.hpp` 已经在用 `plugins::IVideoCaptureFactory`），本片只是在这个既有的边上再加两个类型 —— **不引入新的依赖方向**。

**枚举值的位置**：`Category` 是**无值的枚举**（`enum class Category { kTransport, ... }`），加两个值**不改变**既有值 —— 但注意 `Registrar` 的 `switch` 必须补齐，否则 `-Wswitch` 会报错（本项目 `Warnings→errors: ON`）。

### 二、`audio_device` 模块：把工厂注册进去

`src/modules/audio_device/src/audio_device.cpp` 的 `register_default_plugins()` 里加：

```
NIMRTC_REGISTER_AUDIO_SOURCE(portaudio, &g_portaudio_source_factory);      // 仅当 portaudio_available()
NIMRTC_REGISTER_AUDIO_SOURCE(synthetic, &g_synthetic_source_factory);
NIMRTC_REGISTER_AUDIO_SOURCE(system-audio, &g_system_audio_source_factory);
NIMRTC_REGISTER_AUDIO_SOURCE(synthetic-system-audio, &g_synthetic_system_audio_source_factory);
NIMRTC_REGISTER_AUDIO_SINK(portaudio, &g_portaudio_sink_factory);          // 仅当 portaudio_available()
NIMRTC_REGISTER_AUDIO_SINK(synthetic, &g_synthetic_sink_factory);
```

**模块内的 `source_factory(id)` / `sink_factory(id)` 保留**（它们是模块的公开 API，有别的调用方），但**注册表成为引擎的解析路径**。

**关键不变式**：两个入口解析到的是**同一批工厂对象** —— 注册表里放的就是模块里那几个静态工厂指针。验收标准里有一条专门断言这点（`resolve_audio_source("synthetic") == source_factory("synthetic")`），防止将来两边分叉成两套实现。

**端口音频可用性**：`portaudio_available()` 为假时**不注册** `portaudio` 后端 —— 跟 `available_source_backends()` 的既有行为一致。

### 三、`EngineConfig`：两个名字 + 两份设备配置

```
audio_source_name    : std::string                    // 见下表
audio_sink_name      : std::string                    // 见下表
audio_source_config  : plugins::AudioSourceConfig     // 设备配置（device_id / format / queue_frames）
audio_sink_config    : plugins::AudioSinkConfig       // 设备配置（device_id / format / queue_frames / label）
```

**取值语义**（与 D3 的 `video_capture_name` 决定一致）：

| 值 | 语义 |
|---|---|
| `""`（空） | **宿主自己喂 PCM** —— 今天的行为，**逐字不变** |
| `"none"` | **显式退出** —— 不开设备（与空值的区别：空值是「没提要求」，`"none"` 是「明确不要」） |
| `"auto"` | **平台默认设备** —— 按默认策略解析 |
| `"<具体 id>"` | **显式后端** —— 比如 `"portaudio"` / `"synthetic"` |

**空值语义不变是硬要求**：今天所有不填音频设备的宿主（含大量既有测试）行为**必须逐字不变**。直接改空值语义会让它们突然开始开真实麦克风 —— CI 上直接炸。

**`"none"` 与空值为什么要分开**：计划书 §D1 的原话是 `"none"` = the explicit opt-out。空值是「没提要求」（默认按今天的语义走），`"none"` 是「明确不要设备」。两者在**今天**效果相同，但语义不同 —— 显式 opt-out 让「我不想要设备但我也不想继承默认」这个意图可表达。

### 四、引擎侧设备泵（`src/engine/src/engine_audio_device_pump.cpp`）

新文件 `src/engine/src/engine_audio_device_pump.cpp`，与 `src/engine/src/engine_video_pump.cpp` 同形。

**采集侧**（源 → 出站）：

```
pump_audio_capture()（每个 tick）
  └─ audio_source_->read_samples(buf, capacity, &n)
       → 走引擎内部出站路径（与 send_audio() 等价，但不经过宿主）
```

**播放侧**（入站解码 → sink）：

```
引擎收到解码后的音频帧
  └─ audio_sink_->write(pcm, samples_per_ch, channels)
```

**3A 参考对齐**：这是本片最容易被做错的一处。

今天 `engine_media.cpp:367` 的 `audio3a_plugin_->set_render_delivered(samples_per_ch)` 传的是**这一帧的样本数** —— 它假设「帧交给宿主 = 帧已经播出去了」。

**有设备 sink 时这个假设不成立**：帧写进 sink 的队列 ≠ 已经出声。所以：

- 有 sink 时，`set_render_delivered()` 传 **sink 的 `played_samples()` 增量**（真正到达扬声器的样本数）；
- 无 sink 时（今天的行为），保持传 `samples_per_ch`。

这正是 F3/F4 建的 3A 参考对齐 —— **今天由宿主接线，D1 之后由引擎接**。

**RD-6 成立**：设备拥有自己的线程；引擎只在自己的线程上碰 codec 和 3A。

**不加新线程**：泵挂在已有的 tick 路径上（`src/engine/src/engine_transport.cpp:1217` 的 `pump_video_capture()` 旁边），与 D4 的设备变更轮询同一个决定。

### 五、设备打开失败：**可降级**（与视频侧语义不同！）

**这一条与视频侧刻意不同，必须单独写测试。**

视频侧：非空 `video_capture_name` 的设备打不开 → `kVideoCaptureOpenFailed`，`open()` **失败**（见 `src/engine/src/engine_media.cpp:526-529` 的注释：非空名字是「一个硬承诺」）。

音频侧：`"auto"` 解析到的默认设备打不开 → **降级为「无设备」并 `warn` 一行**，`open()` **成功**。

理由：

1. **默认设备不是承诺**。`"auto"` 的语义是「用默认的」，不是「必须用某个特定设备」—— 主机上没插麦克风、扬声器被别的程序独占、CI 机器没声卡，这些都是**正常情况**，不是配置错误。
2. **音频是可选的，视频不是**。一个没有麦克风的视频通话仍然有意义（只收不发）；但视频侧 D3 的决定已经定了「显式 id 打不开 = 错误」，所以那边报错。
3. **降级必须可观测** —— `warn` 一行 + `Stats` 里能看出来（见下），不能静默。

**注意**：显式 id（`audio_source_name == "portaudio"`）打不开 → **仍然报错**（宿主拼错了 id 或点名了不存在的设备）。只有 `"auto"` 才降级。理由与视频侧一致：显式 id 是承诺。

### 六、`Stats`：报出两个设备

D1 的验收标准原文：「reports both devices in the stats」。

新增：

| 位置 | 变更 |
|---|---|
| `src/engine/src/engine_audio_state.hpp`（或既有音频状态结构） | 新增 `std::string audio_source_device_id;` / `audio_source_backend;` / `audio_sink_device_id;` / `audio_sink_backend;` |
| `src/engine/include/nimrtc/engine/engine.hpp`（`Stats`） | 新增同样的四个字段 + 计数器 `audio_device_pumped_frames` / `audio_device_pump_errors` |
| `src/engine/src/engine_stats_state.hpp` | 新增统计项（`audio_device_pumped_frames` 用 `kCumulative`；设备 id 是**瞬时值**，不是计数器） |

**设备 id 是「实际协商出来的」** —— 不是宿主请求的。`IAudioSource::config()` 返回的是 `open()` 之后**实际生效**的配置（接缝注释里明说了：「the caller must read it back rather than assume the request was granted」）。所以 `Stats` 里报的是 `config().device_id` 与后端 id。

**计数器**（与视频侧的 `video_capture_pumped_frames` / `video_capture_pump_errors` 对称）：

- `audio_device_pumped_frames` —— 引擎从采集设备取走的帧数（累计）；
- `audio_device_pump_errors` —— 泵出错的次数（累计）。

### 七、`"auto"` 的解析：照抄 D3 的 `resolve_auto_capture_backend` 形状

D3 已经把「auto 怎么解析」这件事做过了，形状在 `src/engine/src/engine_device_policy.hpp`：

```cpp
/** The engine's "auto" rule (D3 + the degrade rule).
 *
 * "auto" resolves through the policy exactly like
 * `resolve_capture_backend("", ...)`, then applies the one extra rule:
 * a policy backend id that is **not registered** on this build is not an
 * error -- the engine degrades to "no capture device" (warn + "") rather
 * than firing `kVideoCapturePluginMissing`.  A policy with no default
 * backend also degrades to "".
 *
 * `is_registered` is the registry-membership check, injected so the
 * rule can be unit-tested without a process-wide registry. */
std::string resolve_auto_capture_backend(const DevicePolicy& policy,
                                         bool want_camera,
                                         bool (*is_registered)(
                                             std::string_view id) noexcept) noexcept;
```

**本片照抄这个形状做音频侧**，一个对应的 `resolve_auto_audio_backend(...)`：

- 输入：`DevicePolicy`（或音频后端候选表）+ 方向（采集 / 播放）+ `is_registered` 回调；
- 输出：可用的后端 id，或空串（降级）。

**降级规则与 D3 一致**：解析出来的后端 id 在本构建**没注册** → **不是错误**，降级为「无设备」并 `warn`。这与 §五的「设备打开失败可降级」是**两条不同的降级**：

| 阶段 | 情况 | 行为 |
|---|---|---|
| 解析 | 策略里的后端 id 本构建没注册 | 降级为「无设备」+ `warn`（D3 的既有规则） |
| 打开 | 后端注册了但 `open()` 失败（设备被独占 / 被拔） | 降级为「无设备」+ `warn`（§五，本片新增） |

**为什么音频侧的「auto」不查 `DevicePolicy` 的预留字段**：

D3 的 `DevicePolicy` 里预留了 `audio_capture_backend` / `audio_playback_backend` 两个字段，注释明说「**reserved**: filled in by D1. Deliberately empty today.」。

本片**可以**回填它们，但**回填的值有一个真问题**：

- 音频侧的后端 id 是 `portaudio` / `synthetic` / `system-audio` —— **跨平台同一套**（PortAudio 自己封装了 WASM / CoreAudio / ALSA）；
- 但 `"auto"` 在 CI 机器上要解析成 `synthetic`（没声卡），在真机上要解析成 `portaudio`；
- 而 `portaudio_available()` 是**运行时**判断（`src/modules/audio_device/src/portaudio_backend.cpp:150`），不是编译期。

**决定**：`"auto"` 的音频解析**用运行时可用性**，而不是策略表：

1. `portaudio_available()` 为真 → `"portaudio"`；
2. 否则 → `"synthetic"`。

理由：策略表要表达的是「本平台默认后端」，而这个答案是**运行时才知道**的；把它塞进策略表就得在策略里塞一个假值。

**所以回填 `DevicePolicy` 的两个预留字段不是本片的必要项。** 若 agent 认为应该回填，那是**另一个决定**（策略表要表达「本平台默认音频后端」），应回来问，不许自己填。

> 提醒：`resolve_auto_capture_backend` 的 `is_registered` 是**注入的函数指针**，不是直接查注册表 —— 这样规则本身可以脱离进程级注册表做单测。音频侧照抄这个可测性。

### 八、示例侧

本片**不强制**改示例 —— 但如果 `examples/remote-desktop/` 里有手写音频设备接线的地方（本片 agent 需**核实**），应改为用引擎的设备泵，否则会**双重驱动**（宿主手推 + 引擎泵推 → 音频重复）。

**核实要求**：agent 必须检查 `examples/` 下有没有手写的 `IAudioSource` / `IAudioSink` 使用点。若有，要么改用引擎配置，要么显式设 `"none"`。

## 范围

- src/core/include/nimrtc/core/registry.hpp
- src/core/src/plugin_registry.cpp
- src/core/src/pal_default_registrars.cpp
- src/modules/audio_device/**
- src/engine/**
- src/plugins/include/nimrtc/plugins/audio_source.hpp
- src/plugins/include/nimrtc/plugins/audio_sink.hpp
- src/plugins/include/nimrtc/plugins/audio_device.hpp
- examples/remote-desktop/**
- docs/adr/ADR-029-engine-driven-audio-device-layer.md
- docs/api/engine_api.md
- docs/api/public_headers.manifest
- tests/acceptance/v1-audio-device-engine/acceptance.test.cjs

> `src/plugins/include/nimrtc/plugins/audio_*.hpp` 在写集里，是因为**可能**需要追加。但接缝是 **STABLE** 公开头 —— **agent 不应修改它们**。若 agent 认为必须改接缝，说明它误解了范围，应回来问。
>
> `src/modules/audio_device/**` 在写集里，是因为本片要**注册**后端 —— 但**不改**后端实现（PortAudio / synthetic / system-audio 的行为一律不动）。
>
> `examples/remote-desktop/**` 在写集里，是因为 §八 要**核实**（并可能修）示例里的手写音频设备接线 —— 双重驱动会导致远端听到双份音频。
>
> **`src/engine/src/engine_device_policy.*` 在 `src/engine/**` 之内** —— 本片要照着它加一个音频侧的 `resolve_auto_audio_backend(...)`（§七）。**但**：`DevicePolicy` 结构本身**不动**（它的两个预留音频字段**不回填**，见 §七）。

## 验收标准

1. **注册表有两个音频设备槽位**：注册一个假 `IAudioSourceFactory`，`get_audio_source(id)` 返回**同一个指针**；`list_audio_sources()` 含该 id。sink 侧同理。验证方式：单元测试。

2. **`audio_device` 注册后端**：调 `register_default_plugins()` 后，`list_audio_sources()` **非空**且含 `"synthetic"`；`list_audio_sinks()` 含 `"synthetic"`。验证方式：单元测试。

3. **解析路径唯一**：`pal::resolve_audio_source("synthetic")` 与模块内 `source_factory("synthetic")` 返回**同一指针**；sink 侧同理。验证方式：单元测试（这条防的是「注册表和模块各有一套工厂」的分叉）。

4. **注册表槽位的枚举不影响既有值**：`Registrar::Category` 的既有枚举值（`kTransport` 等）语义不变，且 `switch` 补齐所有新分支（`-Wswitch` 无警告，本项目 warnings→errors）。验证方式：编译通过 + 既有注册表测试无回归。

5. **`EngineConfig` 空值语义不变**：`audio_source_name` 为空字符串时，引擎**不**实例化任何采集设备；`send_audio()` 的行为与今天**逐字相同**。验证方式：单元测试 —— 断言引擎没有音频采集设备，且既有音频测试全部通过。

6. **`"auto"` 走运行时解析**：解析优先 `portaudio`，不可用时回落 `synthetic`。验证方式：单元测试 —— 构造「portaudio 不可用」的场景，断言解析结果是 `"synthetic"`。

6b. **`"auto"` 的解析规则可脱离注册表单测**：音频侧的 auto 解析函数**接受注入的 `is_registered` 回调**（与 D3 的 `resolve_auto_capture_backend` 同形），规则本身可在没有进程级注册表的情况下测。验证方式：单元测试 —— 传一个假的 `is_registered`（返回 false / true）断言解析结果。

6c. **策略 id 未注册 → 降级不报错**：解析出的后端 id 在本构建**没注册**时，`"auto"` **不**报错，降级为「无设备」+ `warn`（与 D3 的既有规则一致）。验证方式：单元测试。

7. **`"none"` 是显式退出**：显式设 `"none"` 时不实例化设备，且**不**因为设备相关的原因失败。验证方式：单元测试。

8. **引擎驱动采集**：假 `IAudioSource` 产帧 → 引擎的出站路径收到这些帧（不需要宿主调 `send_audio()`）。验证方式：单元测试 —— 假 source 推 N 帧，断言引擎出站侧计数前进。

9. **引擎驱动播放**：解码出的 PCM 被写进假 `IAudioSink`（不需要宿主接 `on_audio_frame()`）。验证方式：单元测试 —— 断言假 sink 的 `write()` 被调用，样本数与解码帧一致。

10. **3A 参考对齐用 `played_samples()`**：有 sink 时，`set_render_delivered()` 传的是 sink **实际消费**的样本数（`played_samples()` 的增量），**不是**帧的样本数。验证方式：单元测试 —— 假 sink 报告「消费了一半」，断言 `set_render_delivered()` 收到的是消费数；**无 sink 时**断言收到的是帧样本数（今天的行为不变）。

11. **设备打开失败可降级（音频）**：`"auto"` 解析到的后端 `open()` 失败 → `open()` **成功**、`warn` 一行、音频降级为「无设备」、`Stats` 里能看出没有设备。验证方式：单元测试。

12. **显式 id 打不开报错**：显式设 `"portaudio"`（或任何不存在的 id）→ `open()` **失败**并报对应错误。验证方式：单元测试 —— 与 11 的语义**相反**，两条都要有。

13. **`Stats` 报出两个设备**：`Stats` 里有音频采集设备与播放设备的 id 与后端 id，且是**实际协商出来的**（不是宿主请求的）。验证方式：单元测试 —— 假设备协商出的 device_id 与请求的不同时，断言 `Stats` 报的是**协商出来的**那个。

14. **泵计数器存在**：`audio_device_pumped_frames` / `audio_device_pump_errors` 存在，前者在采到帧时递增、后者在泵出错时递增。验证方式：单元测试。

15. **不加新线程**：音频泵的调用点在 `tick()` 链路上（与 `pump_video_capture()` 并列）。验证方式：静态检查 —— `src/engine/src/` 无新增 `std::thread` / `ThreadPool` / 定时器线程。

16. **依赖方向不破**：静态检查 —— `src/engine/` 不引用 `nimrtc::audio_device`；`src/core/` 不引用 `src/modules/` 的任何东西。

17. **接缝没被破坏**：`plugins/audio_source.hpp` / `audio_sink.hpp` / `audio_device.hpp` 的 `IAudioSource` / `IAudioSink` 声明与本片开始前**逐字相同**（`git diff` 这三个头不含成员变更）。验证方式：静态检查。

18. **公开头清单同步**：`docs/api/public_headers.manifest` 与实际公开头一致。验证方式：只读 `scripts/check_api_manifest.py`，跑它确认通过。

19. **`EngineConfig` 新字段有文档**：`docs/api/engine_api.md` 里 `audio_source_name` / `audio_sink_name` / `audio_source_config` / `audio_sink_config` 四个字段都有说明，且**说明了空值语义**。验证方式：静态检查 —— 文档里出现这四个字段名，且提到空值 = 宿主自己喂。

20. **验收测试是真实断言**：`tests/acceptance/v1-audio-device-engine/acceptance.test.cjs` 里每条用例都是可执行的断言，**不允许残留 `test.todo`**。验证方式：`node --test tests/acceptance/v1-audio-device-engine/acceptance.test.cjs` 全部通过，且 `test.todo` 出现次数为 0。

21. **既有行为不回归**：既有的音频测试、引擎测试、注册表测试全部仍然通过。验证方式：`ctest --test-dir build --output-on-failure -C Debug` 无回归。

22. **CI 无声卡也能过**：在**没有声卡**的构建里，`"auto"` 解析到 `synthetic` 且引擎正常工作。验证方式：单元测试 —— 模拟 `portaudio_available()` 为假。

## 风险

1. **端口占用导致引擎开不了设备**。Windows 的 WASM 独占模式、Linux 的 ALSA 独占会让「默认设备」打不开。应对：验收标准 11 —— `"auto"` 打不开是**降级**，不是失败；显式 id 才报错。

2. **双重驱动**：宿主已经在手推音频、引擎又泵一份 → 远端听到双份音频。应对：验收标准 5（空值不变）+ 验收标准 8/9（走引擎配置时宿主不推）+ 示例侧的核实要求。

3. **`set_render_delivered()` 语义被改坏**：3A 的 AEC 参考对齐依赖这个数，改错会让回声消除器不收敛（表现为「越说越响」）。应对：验收标准 10 两条都断言（有 sink / 无 sink）。

4. **改 `audio_source_name` 空值语义会炸 CI**：与 D3 的风险 1 同构。应对：验收标准 5 直接断言空值时**不**实例化设备。

5. **枚举加值影响既有 switch**：`Category` 的 `switch` 补不齐就是编译错误（warnings→errors）。应对：验收标准 4。

6. **agent 顺手做 D2（渲染器）**：那会扩到 `video_sink` 与 GPU mailbox，超出本片。应对：范围里没有 `src/modules/video_*`，越界会被门禁拦下。

7. **agent 顺手回填 `DevicePolicy` 的音频字段**：那是另一个决定（§七）。应对：规格里明说「若认为应该回填，回来问，不许自己填」。

8. **`core` 的改动看着像「动契约」**：注册表是核心契约。缓解：全部**追加**，不改既有语义；ADR-029 记录了这个决定并等签字。
