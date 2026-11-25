# Slice: v1-device-policy

## 目的

把「同一个二进制不带参数运行就能干活」这件事从示例代码搬进引擎核心，并让设备中途消失/回来这件事**干净地**发生。

今天的状态（已核实）：

- **D3（默认设备策略）**：每平台默认表住在 `examples/remote-desktop/rd_device.cpp:137` 的 `default_capture_name()`（`windows-wgc` / `macos-screencapturekit` / `linux-x11` / `synthetic`）。它住在**示例**里 —— 意味着任何不是这个示例的宿主都得自己再抄一遍这张表，"没配设备 id" 是个坑而不是有意义的默认值。
- **D4（设备热插拔）**：`plugins::IVideoCapture::enumerate_devices()` 已存在（`src/plugins/include/nimrtc/plugins/video_capture.hpp:709`），但**没有变更检测** —— 引擎不知道设备被拔掉了。`recover_capture_device()`（`src/engine/src/engine_video_pump.cpp:272`）只在**设备自己报错**（`device_errors` 增长）时才重开，且**只重开一次**：重开失败就 `kVideoCaptureOpenFailed` 停泵。计数器 `video_capture_restarts` 已有（`engine.hpp:4527`），`video_capture_device_changes` **尚不存在**。

D3 是「默认是什么」，D4 是「默认的东西不见了怎么办」。两者一起做，是因为 D4 的「设备被拔掉」只有在 D3 选出来的设备是**可枚举的物理设备**时才有意义 —— 屏幕后端（DXGI device-lost）已经有恢复路径，**摄像头类（Media Foundation / AVFoundation / V4L2）没有**。

### 本片的边界（重要）

D3 的原始要求里含「音频默认」。**核实结论：引擎里没有音频设备的承载体** ——

- `EngineConfig` 无 `audio_source_name` / `audio_sink_name`；
- `core::Registry` 无 `kAudioSource` / `kAudioSink` 槽位（只有 `kVideoCapture`）；
- 引擎无音频设备泵（`send_audio()` 由宿主手推 PCM）。

那属于计划书 §D1「引擎驱动设备层」，而 **D1 不在 v1 版本计划里**。所以本片：

- **只做视频侧**（屏幕采集后端 + 摄像头后端）；
- 策略表里**给音频预留位置**，字段存在但值为空，并在文档与注释里写明「音频默认待 D1 落地后接入」；
- **不**新增注册表槽位、**不**新增 `EngineConfig` 音频字段、**不**写 ADR（不碰契约）。

## 设计

### 一、默认设备策略（engine 层）

#### 落点

`src/engine/src/engine_device_policy.hpp` + `src/engine/src/engine_device_policy.cpp`（**私有**，不进 `include/`，不进公开头清单）。

理由：策略是 `EngineConfig` 的默认值来源，只有引擎需要它；`core/` 不该知道具体插件 id 字符串；设备模块的依赖方向是 device→engine，反向不成立。

#### 形状

```cpp
namespace nimrtc::engine::detail {

/** 一份默认设备策略：每类设备一个「默认插件 id」。 */
struct DevicePolicy {
    /** 屏幕采集后端插件 id（"" = 本平台无默认后端）。 */
    std::string screen_capture_backend;
    /** 摄像头后端插件 id（"" = 本平台无默认摄像头后端）。 */
    std::string camera_capture_backend;
    /** 音频采集设备插件 id —— **预留**，D1 落地后填充（见「本片的边界」）。 */
    std::string audio_capture_backend;
    /** 音频播放设备插件 id —— **预留**，D1 落地后填充。 */
    std::string audio_playback_backend;
};

/** 当前平台的默认设备策略。 */
DevicePolicy default_device_policy() noexcept;

/**
 * 把「宿主给的 id」与策略合成最终 id。
 *
 * 优先级（与 profile 系统已有的覆盖规则一致）：
 *   1. `explicit` 非空 → 用 `explicit`（宿主显式指定，永远赢）；
 *   2. 否则用策略里的值；
 *   3. 策略也为空 → 返回空（引擎据此**不**实例化采集设备）。
 */
std::string resolve_capture_backend(std::string_view explicit_id,
                                    const DevicePolicy& policy,
                                    bool want_camera) noexcept;

} // namespace nimrtc::engine::detail
```

#### 每平台默认表（现状搬过来，不新增能力）

| 平台 | 屏幕采集后端 | 摄像头后端 |
|---|---|---|
| Windows | `windows-wgc` | `camera-mediafoundation` |
| macOS | `macos-screencapturekit` | `camera-avfoundation` |
| Linux | `linux-x11` | `camera-v4l2` |
| 其它 | `synthetic` | `synthetic-camera` |

这些 id **都已注册**（`windows_capture.hpp:104`、`unix_capture.hpp:284`、`camera_capture.hpp:500` 附近的 `CameraCaptureFactory::id()`），本片**不新增插件**，只把表从示例搬到引擎。

#### 覆盖规则 —— 这一条最容易做错

`EngineConfig::video_capture_name` 的**空值语义今天已经是「宿主自己喂帧」**（`src/engine/src/engine_media.cpp:519` 注释、519-531 的分支；只读 `tests/test_engine_video_capture.cpp:147`，该文件断言 `cfg.video_capture_name.empty()`）。

**所以「空值 = 用策略」不能直接改 `video_capture_name` 的空值含义** —— 那会让今天所有「不配置采集设备」的宿主（大量 `tests/test_engine_video_*.cpp` 里 `cfg.video_capture_name = ""`）突然开始自动开一个真实采集设备，CI 上直接炸。

**决定：用一个独立开关表达「请用默认策略」。**

```
EngineConfig::video_capture_name == ""
  → 语义不变：宿主自己喂帧（不实例化采集设备）

EngineConfig::video_capture_name == "auto"
  → 语义新增：按默认策略解析（平台默认后端，或按 video_content_hint 选屏幕/摄像头）

EngineConfig::video_capture_name == "<具体 id>"
  → 语义不变：宿主显式指定
```

理由：`"auto"` 是**显式**的意图，不会与今天「没配」的历史行为混淆；而「空值 = 用策略」会静默改变既有宿主的语义 —— 这正是 profile 系统里 `EngineConfig` 值胜过策略那条规则的**反面**：profile 那边是「空 = 策略」，但 EngineConfig 这边空值本来就有别的含义。

**并且**：`"auto"` 解析出来的后端 id 如果在本平台**没注册**（比如 Linux 构建里没有 `camera-v4l2`），**不是错误** —— 引擎继续按「没有采集设备」走，并 `warn` 一行说明策略没兑现。这与 `resolve_video_capture` 对未知 id 报 `kVideoCapturePluginMissing` 的行为**不同**：显式 id 报错（宿主拼错了），策略 id 不报错（策略是建议，平台可能没有）。

#### 屏幕还是摄像头：D3-1

计划书 §5 **D3-1** 已经定了：**「conference」profile 用摄像头，「remote-desktop」profile 用屏幕** —— profile 决定，因为产品意图已经住在那里。

所以策略解析的输入有两个：`video_capture_name == "auto"` 且 `EngineConfig::video_content_hint` 决定选屏幕还是摄像头：

- `video_content_hint == kText`（屏幕内容，remote-desktop profile 走这条）→ **屏幕**后端；
- 其它（含 `kUnspecified`）→ **摄像头**后端。

`video_content_hint` 的来源已经在只读 `src/modules/assembly/include/nimrtc/assembly/engine_bridge.hpp:479`（该文件不改）从 profile 的 `video_content_type` 映射好了，引擎侧不用改映射。

#### 与示例的关系

`examples/remote-desktop/rd_device.cpp` 的 `default_capture_name()` **删除**，改为读引擎策略（示例要能问引擎「默认是什么」，所以策略需要一个公开可见的入口 —— 见下）。

**公开入口**：`EngineConfig` 里不加新字段（避免扩契约），而是给引擎加一个**静态**查询函数：

```cpp
/** 当前平台的默认设备策略（只读）。 */
static DevicePolicyInfo default_device_policy() noexcept;
```

`DevicePolicyInfo` 是引擎公开头里的一个**只读值类型**（4 个 `std::string`），追加在 `engine.hpp` 末尾的 namespace 里即可 —— 它不改变任何既有 ABI 语义，只是新增一个类型和一个静态函数。

**注意**：这**确实**触及公开头 `src/engine/include/nimrtc/engine/engine.hpp`（该文件已在 `docs/api/public_headers.manifest` 里，属于公开头）。新增**类型**与**静态函数**是追加，不改既有字段 —— 按 ADR-015 属于可接受变更，但**必须在 `docs/api/engine_api.md` 补文档**，并跑一遍清单校验脚本：只读 `scripts/check_api_manifest.py`（该脚本不改动；它**不在**清单里，只加类型不改清单，所以清单不用动）。

#### 摄像头的设备 id

策略只给**后端插件 id**，不给设备 id —— 设备 id 由后端自己枚举并选第一个（`camera_capture.cpp:157` 的 `chosen = &devices.front()`）。摄像头「默认策略」指的就是**用哪个摄像头后端**。

### 二、D4：设备变更检测与重连

#### 接缝侧：新增「设备变更」的可选能力

`plugins::IVideoCapture` 是 **STABLE 公开头**，**不能**加纯虚函数（会破坏所有实现者）。

做法（与仓库既有模式一致，参照只读 `src/plugins/include/nimrtc/plugins/ice_transport.hpp:17`（该文件不改）注释里说的「`dynamic_cast` 修补」模式）：新增一个**独立的小接口**，放在**新头**里：

```cpp
// src/plugins/include/nimrtc/plugins/capture_device_events.hpp
namespace nimrtc::plugins {

/** 设备列表变化的种类。 */
enum class CaptureDeviceChange : std::uint8_t {
    kAdded = 0,    ///< 一个设备出现了
    kRemoved = 1,  ///< 一个设备消失了
};

/** 一次设备列表变化。 */
struct CaptureDeviceChangeEvent {
    CaptureDeviceChange change = CaptureDeviceChange::kAdded;
    std::string device_id;
    std::string device_name;
    CaptureDeviceKind kind = CaptureDeviceKind::kUnknown;
};

/**
 * 可选能力：实现了它的采集设备能报告设备列表的变化。
 *
 * 这是一个**独立接口**，不是 `IVideoCapture` 的成员 ——
 * `IVideoCapture` 是 STABLE 公开头，加纯虚函数会破坏所有既有实现
 * （外部宿主可能自己实现了它）。
 */
class NIMRTC_API_STABLE ICaptureDeviceEventSource {
public:
    virtual ~ICaptureDeviceEventSource() = default;

    /** 自上次调用以来发生的变化（返回后清空）。 */
    virtual std::vector<CaptureDeviceChangeEvent>
    take_device_changes() noexcept = 0;
};

} // namespace nimrtc::plugins
```

引擎用 `dynamic_cast` 探测设备是否实现了它 —— **没有**就是「该设备不支持变更通知」，引擎退回到**轮询枚举**。

**这个新头需要加进 `docs/api/public_headers.manifest`**（`python scripts/check_api_manifest.py --write`）—— 它是公开头。

#### 引擎侧：按 tick 轮询（不加新线程）

RD-6 硬约束：**不加新的引擎线程**。轮询挂在**已有的** tick 路径上：

`engine_transport.cpp:1217` 的 tick 已经调 `pump_video_capture()`。所以：

```
pump_video_capture()（每个 tick）
  ├─ recover_capture_device()        （现有：设备报错才重开）
  ├─ publish_capture_device_counters()（现有：计数器同步）
  └─ poll_capture_device_changes()   （新增：设备变更检测）
```

#### 变更检测的判据：两条路，取其一

**路 A（设备支持时）**：`dynamic_cast<ICaptureDeviceEventSource*>` 成功 → `take_device_changes()` 拿到结构化事件。

**路 B（设备不支持时）**：轮询 `enumerate_devices()`，把结果与**上次**的设备 id 集合比对，求差集：

- 新增的 id → `kAdded`；
- 消失的 id → `kRemoved`。

轮询**不是**每 tick 都做 —— 枚举在某些后端上不便宜（V4L2 走 ioctl、Media Foundation 走枚举 COM 对象）。所以**加一个间隔**：默认 **500 ms**（`CaptureConfig` 之外，引擎内部常量，可调）。上次快照在引擎的 `video_state` 里。

**路 B 是保底，路 A 是优化** —— 摄像头后端（MF / AVFoundation / V4L2）本片**不实现** `ICaptureDeviceEventSource`（那要接平台的原生热插拔通知：MF 的 `KSCaptureDevice`、`AVCaptureDevice` 的 `connectionWithMediaType:` 通知、V4L2 的 `VIDIOC_EVENT`），本片用轮询即可满足 D4 验收。**但接缝必须留出这个接口**，否则将来接原生通知时又是一次破坏性变更。

> 这一条要在规格里写清楚：**接缝留口、实现走轮询**。这是有意的取舍，不是省事。

#### 重连：退避上界（这是 D4 的核心，需求原文「重开是**有退避上界**的」）

现状：`recover_capture_device()` 在 `device_errors` 增长时**立即** `close()` + `open()`，失败就 `on_error_(kVideoCaptureOpenFailed)` 并停泵。没有退避 —— 一个「设备被拔掉」的场景会让引擎**每 tick 疯狂重试**（因为 `device_errors` 可能继续增长），或者第一次失败就永久停机。

**新语义**（在 `recover_capture_device()` 里改，或拆出独立的重连状态机）：

```
状态：
  kIdle          — 设备正常
  kBackoff       — 设备丢失，等待下一次重试
  kStopped       — 放弃（超过最大重试次数）

每次 tick：
  1. 设备报告了 device_errors 增长 或 检测到设备被移除
     → 进入 kBackoff（首次）
  2. kBackoff 状态下：
     - 若距上次重试 < backoff 当前间隔 → 不动（返回）
     - 否则：close() + open()
       - 成功 → kIdle，++video_capture_restarts
       - 失败 → backoff = min(backoff * 2, 上界)；重试次数 +1
         - 重试次数 > 上限 → kStopped，报 kVideoCaptureOpenFailed
```

**退避参数**（引擎内部常量，可调）：

| 参数 | 值 | 理由 |
|---|---|---|
| 初始退避 | 100 ms | 快速恢复一次（拔插抖动常见） |
| 退避上界 | 5 s | 上界是 D4 的要求；5 s 内恢复是可接受的会话体验 |
| 最大重试次数 | 20 | 100ms→5s 的翻倍序列下，约 60 s 后放弃；超过这个时间设备多半真的不在了 |
| 轮询间隔 | 500 ms | 设备变更检测的节流 |

**关键：退避状态必须是「设备被拔掉」和「设备正常」都能观察到的** —— 所以它放在引擎的 `video_state` 里（引擎私有状态，不是全局）。

#### 摄像头后端的「设备移除 / 设备恢复」路径

需求原文：**「摄像头类（Media Foundation / AVFoundation / V4L2）补上设备移除 / 设备恢复的路径 —— 屏幕后端已经有（DXGI device-lost）」**。

具体落点：

| 后端 | 设备移除的判据 | 设备恢复的判据 |
|---|---|---|
| Media Foundation（Windows） | `IMFSourceReader` 报错 / `device_errors` 增长；或枚举时该 id 消失 | 重新 `open()` 成功 |
| AVFoundation（macOS） | `AVCaptureSession` 的 input 失效 / `device_errors` 增长 | 重新 `open()` 成功 |
| V4L2（Linux） | `VIDIOC_STREAMON`/`VIDIOC_DQBUF` 失败（`ENODEV`/`EIO`）；或 `/dev/videoN` 消失 | 重新 `open()` 成功 |

**共同的处理**：这三条路径的**共同点在引擎侧** —— 引擎的退避重连机制（上面那段）对三个后端**一视同仁**。所以后端侧要做的是**准确上报「设备没了」**（`device_errors` 增长），**恢复**由引擎统一做（`close()` + `open()`）。

**这是本片的分界线**：后端不各自实现重连逻辑（那会是三份重复且各自漂移的退避实现），后端只负责**准确地说「我坏了」**。

> 检查现状：`camera_capture.cpp` 的 `capture_loop()` 里，`source_->next_frame()` 失败时是否 `++device_errors_`？需要 agent 核实并补齐 —— 若已有，本条就只需**验证**。

### 三、计数器

新增 `video_capture_device_changes`（需求原文点名）：

| 位置 | 变更 |
|---|---|
| `src/engine/src/engine_video_state.hpp` | 新增 `std::uint64_t video_capture_device_changes = 0;` |
| `src/engine/include/nimrtc/engine/engine.hpp`（`Stats`） | 新增 `std::uint64_t video_capture_device_changes = 0;` |
| `src/engine/src/engine_stats_state.hpp` | 新增 `X(video_capture_device_changes, core::StatSemantics::kCumulative)` |
| `src/engine/src/engine_media.cpp`（计数器发布处） | 赋值 |

**语义**：`video_capture_device_changes` 计**设备列表变化的次数**（一次「拔掉」或一次「插上」各算 1 次）。它是**累计**的（`kCumulative`）。

**与 `video_capture_restarts` 的区别**（这两个计数器容易被混）：
- `video_capture_restarts` = **重开设备的次数**（引擎重试了多少次）；
- `video_capture_device_changes` = **设备列表变化的次数**（物理世界变了多少次）。

一个「拔掉再插上」通常产生 2 次 device_changes 和 1~2 次 restarts。它们**不要求**成比例 —— 这正是要分成两个计数器的原因。

### 四、示例侧

`examples/remote-desktop/rd_device.cpp`：
- 删掉 `default_capture_name()`，改为向引擎查询策略（`NimRTCEngine::default_device_policy()`）；
- `--capture` 参数的默认值改为 `"auto"`（走策略），显式传 id 仍然直接用；
- 打印实际生效的采集后端（策略解析的结果），让人一眼看出「策略兑现了没有」。

## 接口变更清单（全部**追加**）

| 位置 | 变更 | 公开？ |
|---|---|---|
| `src/plugins/include/nimrtc/plugins/capture_device_events.hpp` | **新头**：`CaptureDeviceChange` / `CaptureDeviceChangeEvent` / `ICaptureDeviceEventSource` | 是 → 需加进 `public_headers.manifest` |
| `src/engine/include/nimrtc/engine/engine.hpp` | 新增 `DevicePolicyInfo` 类型 + `static DevicePolicyInfo default_device_policy()` | 是 → 需补 `docs/api/engine_api.md` |
| `src/engine/src/engine_video_state.hpp` | 新增 `video_capture_device_changes` | 否 |
| `src/engine/src/engine_stats_state.hpp` | 新增统计项 | 否 |
| `src/engine/src/engine_device_policy.hpp/.cpp` | **新私有文件** | 否 |

`plugins::IVideoCapture` **不动**（STABLE 公开头，加纯虚函数会破坏外部实现者）。

`EngineConfig` **不动**（`video_capture_name == "auto"` 是复用既有字符串字段的**新取值**，不改字段语义 —— 空值含义保持不变）。

## 范围
- src/modules/video_capture/**
- src/modules/audio_device/**
- src/engine/**
- src/plugins/include/nimrtc/plugins/capture_device_events.hpp
- src/plugins/include/nimrtc/plugins/video_capture.hpp
- examples/remote-desktop/**
- docs/api/engine_api.md
- docs/api/public_headers.manifest
- tests/acceptance/v1-device-policy/acceptance.test.cjs

> `src/modules/audio_device/**` 保留在写集里：本片**不改**它（音频默认不在本片范围内），但保留是为了让「预留字段」在需要时可落。**agent 不应修改它** —— 若 agent 认为必须改，说明它误解了范围，应回来问。

## 验收标准

1. **默认设备策略搬进引擎核心，且不再住在示例里**：`examples/remote-desktop/rd_device.cpp` 中**不再有** `default_capture_name()` 里的 `#ifdef _WIN32` 平台分支表；该表存在于引擎侧的策略实现里。验证方式：静态检查 —— `rd_device.cpp` 不含 `"windows-wgc"` / `"macos-screencapturekit"` / `"linux-x11"` 的平台选择逻辑（这些 id 作为**参数值**出现不算违例，但**不得**有 `#ifdef` 平台分支表）。

2. **覆盖优先级成立**：`EngineConfig::video_capture_name` 非空且不是 `"auto"` 时，引擎用的是**宿主给的那个 id**，不是策略的。验证方式：单元测试 —— 设 `video_capture_name = "synthetic"`（或任何已注册的测试用 id），断言解析结果等于该 id，**不是**平台默认。

3. **`"auto"` 走策略，且空值语义不变**：`video_capture_name` 为空字符串时，引擎**不**实例化任何采集设备（与今天一致）；为 `"auto"` 时，引擎按平台策略解析。验证方式：单元测试 —— 空值场景断言 `video_capture()` 返回 nullptr（既有 `tests/test_engine_video_capture.cpp` 已有该断言，不得回归）；`"auto"` 场景断言解析出的 id 等于策略值。

4. **策略没兑现时不是错误**：在**没有**注册该默认后端的构建里，`"auto"` 解析失败应**降级为「无采集设备」并 warn**，而不是报 `kVideoCapturePluginMissing`。验证方式：单元测试 —— 用一个策略 id 未注册的场景断言 `open()` 成功且不报 `kVideoCapturePluginMissing`。

5. **设备变化被检测到并计数**：设备列表变化时，`Stats::video_capture_device_changes` 递增。验证方式：单元测试 —— 用一个**测试用采集设备**（fake device，可注入设备列表）模拟「设备被移除」，断言计数从 0 变 1；再模拟「设备被插回」，断言变 2。

6. **重连有退避上界，不会疯狂重试**：设备持续不可用时，重试**间隔递增**且**有上界**，重试次数**有上限**；达到上限后报 `kVideoCaptureOpenFailed` 并停止。验证方式：单元测试 —— fake device 恒定 `open()` 失败，断言重试次数 ≤ 20、两次重试之间的间隔 ≤ 5 s（退避上界），且最终报了 `kVideoCaptureOpenFailed`。

7. **设备恢复后帧恢复、`video_capture_restarts` 递增**：fake device 从「失败」变为「成功」时，设备被重新打开、帧恢复流动、`video_capture_restarts` 递增。验证方式：单元测试 —— 断言 `video_capture_restarts` 从 0 变 1，且恢复后 `pop_frame()` 能拿到帧。

8. **`video_capture_device_changes` 与 `video_capture_restarts` 是两个独立计数器**：一次「拔掉再插上」不要求两者的比值是某个固定值，但两者都必须递增。验证方式：单元测试 —— 断言在「移除 + 恢复」序列后，`video_capture_device_changes >= 2` 且 `video_capture_restarts >= 1`。

9. **摄像头后端能上报「设备没了」**：Media Foundation / AVFoundation / V4L2 三个后端在设备消失时，`CaptureStats::device_errors` 递增（引擎据此触发退避重连）。验证方式：单元测试（跨平台可跑的部分 —— 合成设备路径）+ 静态检查 —— 三个后端的 `next_frame()` 失败路径都有 `device_errors` 递增；平台专属路径（真机拔插）用 `#ifdef` 隔离，CI 上只跑合成路径。

10. **接缝留了变更通知的口子**：`plugins::IVideoCapture` **没有**被加纯虚函数（外部实现者不受破坏）；新增的 `ICaptureDeviceEventSource` 是**独立接口**，引擎用 `dynamic_cast` 探测。验证方式：静态检查 —— `plugins/video_capture.hpp` 的 `IVideoCapture` 声明与本片开始前**逐字相同**（`git diff` 该头文件应只包含注释/新增独立接口的引用，不含 `IVideoCapture` 的成员变更）；且引擎侧存在 `dynamic_cast<...ICaptureDeviceEventSource*>` 的探测点。

11. **不新增引擎线程**：轮询挂在已有 tick 路径上。验证方式：静态检查 —— 设备变更轮询的调用点在 `pump_video_capture()` 链路上（由 `tick()` 驱动），**没有**新增 `std::thread` / `ThreadPool` / 定时器线程。

12. **示例反映设备状态变化**：`examples/remote-desktop/rd_device.cpp` 打印实际生效的采集后端 id。验证方式：静态检查 —— 示例里有该输出语句。

13. **验收测试文件是真实断言**：`tests/acceptance/v1-device-policy/acceptance.test.cjs` 里每条用例都是可执行的断言，**不允许残留 `test.todo`**。验证方式：运行 `node --test tests/acceptance/v1-device-policy/acceptance.test.cjs` 全部通过，且文件中 `test.todo` 出现次数为 0。

14. **不破坏既有行为**：既有的引擎视频测试（`tests/test_engine_video_capture.cpp` 等）与视频采集模块测试全部仍然通过。验证方式：`ctest --test-dir build --output-on-failure -C Debug` 无回归。

## 风险

1. **改 `video_capture_name` 的空值语义会炸 CI**：大量既有测试用 `cfg.video_capture_name = ""` 表示「不采集」。**应对**：`"auto"` 是独立取值，空值语义**完全不变**（验收标准 3 直接断言这一点）。
2. **枚举在某些后端上很贵**（V4L2 ioctl / MF COM 枚举）。**应对**：轮询间隔 500 ms，且优先用设备自带的变更通知（路 A）。
3. **退避参数定得太短会导致设备频繁拔插时抖动**。**应对**：初始 100 ms、上界 5 s、上限 20 次；参数集中在引擎一处，便于调整。
4. **新增公开头要同步 API 清单**，漏了会让 `scripts/check_api_manifest.py` 失败。**应对**：验收标准 10 与写集里都列了 `docs/api/public_headers.manifest`。
5. **agent 容易顺手做 D1（音频默认）**，那会扩到 `src/core/**` 并触及注册表契约。**应对**：范围里写明音频只做「预留字段」，且 `src/core/**` **不在**写集内 —— 越界会被门禁拦下。
6. **摄像头后端的设备移除路径在真机上才能验**，CI 上验不了。**应对**：验收标准 9 拆成「合成路径单测」+「静态检查平台路径」两半；真机验证留给 N6 的端到端样本。
