# Slice: v1-video-render-engine

## Summary

宿主不写一行 D3D11 / Metal / EGL 呈现代码，`video_sink_name = "auto"`（**新默认值**）就能在三个桌面平台上把解码后的帧送上屏幕；平台渲染器起不来时**降级**为 headless 而不是让会话失败，且降级在 `Stats` 里可查（`video_sink_backend` + `video_sink_backend_reason`）。

三件事：① 新增私有的渲染后端解析（分平台偏好 + 逐级降级）；② `EngineConfig::video_sink_name` 的默认值改成 `"auto"`，取值表扩成 `""` / `"auto"` / `"none"` / `"headless"` / `"<id>"`；③ Windows 与 macOS 的渲染器补上「没有 `native_window` 就自建窗口」的路径，与 Linux 的 EGL 对齐 —— **引擎不碰窗口系统**（L4）。

这是 v1.4 计划书 §D2 的落地，架构决策在 `docs/adr/ADR-030-engine-driven-video-renderer.md`（**Proposed**，待签字）。

## 目标

**让引擎驱动渲染器**：宿主不写一行 D3D11 / Metal / EGL 呈现代码，`video_sink_name = "auto"`（默认）就能在三个桌面平台上把解码后的帧送上屏幕；平台渲染器起不来时**降级**而不是让会话失败，且降级在 stats 里可查。

这是 v1.4 计划书 §D2（"the engine runs the renderer"）的落地。架构决策在 `docs/adr/ADR-030-engine-driven-video-renderer.md`（**Proposed**，待签字）。

> **归属**：`version/v3`。D3/D4 在 `db2dc0de`、D5 在 `6b5439da`、D6 在 `59b09f02`、D1 在 `6a1e3a6e`，都已签进 `version/v3`。
>
> **为什么必须在 `version/v3`**：D2 要改 `EngineConfig::video_sink_name` 的默认值和 `Stats` 的字段 —— 而 `engine.hpp` 正是 D1/D6 也写过的那个文件。D2 与 D1/D6 撞同一片区域，必须**顺序**做，不能并行。
>
> **前置**：D1（`6a1e3a6e`）已经在 `version/v3` 上；D2 的音频侧无需再动。

### 现状核实（写规格时的树状态）

- **引擎不选平台渲染器**：`src/engine/src/engine_media.cpp:732` 把 `config_.video_sink_name` 直接丢给 `pal::resolve_video_sink`（`src/engine/include/nimrtc/engine/pal/engine_plugin_resolver.hpp:167`），后者是纯按 id 查注册表。没有 `"auto"` 的语义。
- **默认值是 `"headless"`**：`src/engine/include/nimrtc/engine/engine.hpp:725`。
- **三个平台渲染器都注册**：`src/modules/video_sink/src/video_sink.cpp:275-284` 注册 `headless` / `pinned` / `d3d11` / `metal` / `egl`，**无条件**注册（不分平台）；分平台是在 `open()` 时。
- **Windows / macOS 假定窗口是宿主给的**：`d3d11_renderer.cpp:439-440` 与 `metal_renderer.cpp:294` 都是 `native_window == nullptr` → 退到 **offscreen**（渲染到纹理，**不上屏**）。
- **Linux 的 EGL 自己建窗口**：`egl_renderer.cpp:643`（`wl_compositor_create_surface`）/ `egl_renderer.cpp:853`（`XCreateSimpleWindow`）。三平台**不对称**。
- **树里唯一的窗口创建代码在测试文件里**：`src/modules/video_capture/tests/test_windows_capture.cpp:189`（`CreateWindowExW`）。引擎路径上**没有**窗口。
- **没有 `video_sink_backend` 字段**：`Stats` 里只有 sink 自己的计数器（`video_sink_frames_rendered` 等，`engine.hpp:5017-5024`），**没有**「现在用的是哪个后端」和「为什么降级」。

### 与 D3/D4 的重叠（本片不重复做）

只读 `specs/slices/v1-device-policy.md` — 参考它的默认策略与降级语义，不改它。

`specs/slices/v1-device-policy.md`（D3/D4）做的是**采集侧**的默认策略（`video_capture_name`）与热插拔。**本片只做渲染侧**：

- D3 的策略表（只读 `src/engine/src/engine_device_policy.hpp`）是**采集**设备的策略表 —— `screen_capture_backend` / `camera_capture_backend` / 音频预留位。**渲染后端不在那张表里**，本片**不要**把渲染后端塞进 D3 的策略表（那会让「采集策略」和「渲染策略」混在一张表里，两者的失败原因不同：采集失败是「没摄像头」，渲染失败是「没显示器」）。
- D3 的 `resolve_auto_capture_backend()` 是本片 `resolve_auto_sink_backend()` 的**先例**，但**不是同一个函数**。
- D4 的热插拔是**采集**设备变更。渲染器的窗口重建**不在本片范围**（见「本片不做」）。

## 方案

### 一、`"auto"` 的解析（新增，私有）

新增 `src/engine/src/engine_render_policy.{hpp,cpp}`（**私有**，不进公开头）：

```cpp
namespace nimrtc::engine::detail {

/** 渲染后端的平台偏好。 */
struct RenderPolicy {
    std::string_view preferred_backend;   // "d3d11" / "metal" / "egl"
    std::string_view fallback_backend;    // "headless"
};

/** 当前构建的渲染策略。 */
RenderPolicy default_render_policy() noexcept;

/** 解析 "auto"。

 *  @param policy          平台偏好
 *  @param is_registered   注册表查询（注入，便于测试）
 *  @return 解析出的后端 id；解析不到时返回空串（调用方降级）
 *
 *  语义与 D3 的 `resolve_auto_capture_backend()` 对称：
 *  「auto」是**推荐**，不是要求。解析不到 → 空串 → 调用方降级，
 *  绝不报 kVideoSinkPluginMissing。 */
std::string resolve_auto_sink_backend(
    const RenderPolicy& policy,
    bool (*is_registered)(std::string_view id) noexcept) noexcept;

} // namespace nimrtc::engine::detail
```

**平台偏好表**：

| 平台 | `preferred_backend` | 说明 |
|---|---|---|
| Windows | `"d3d11"` | D3D11 / DXGI swap chain |
| macOS | `"metal"` | Metal / AVSampleBufferDisplayLayer |
| Linux | `"egl"` | EGL / Wayland / X11 |
| 其他 | `""` | 解析不到 → 降级 |

**降级顺序**（`headless` 是终点，永远有一个 sink）：

```
"auto"
  → 平台首选后端
  → 注册表里有吗？没有 → 降级 "headless"
  → 有 → make_plugin(open_fail_mode = Fallback)
  → open() 失败 → 降级 "headless"
```

**`open()` 的失败模式是 `Fallback`，不是 `Error`。** 与 D3 的采集侧（`Error`）**不同**，理由：sink 是**输出**设备，缺一个输出设备不该让会话开不起来。这与 D1 的音频降级同族（ADR-029 §3.2 第 4 条）。

**显式 id 打开失败仍然是硬错误**（`kVideoSinkOpenFailed` / `kVideoSinkPluginMissing`）—— 与 D3 一致：**显式是承诺，auto 是推荐**。

### 二、`EngineConfig::video_sink_name` 的取值语义

| 值 | 语义 | 有 sink 实例吗 | 帧去哪 | `open()` 失败？ |
|---|---|---|---|---|
| `""` | **今天的行为**（等价 `"headless"`） | 有 | 计数丢弃 | 否 |
| `"auto"`（**新默认值**） | 平台渲染器；失败则降级 | 有 | 渲染器 | 否 |
| `"none"`（**新增**） | 显式退出：不要 sink | **无** | 引擎不渲染（`set_on_video_frame` 仍照旧） | 否 |
| `"headless"` | 显式要「计帧但丢像素」 | 有 | 计数丢弃 | 否 |
| `"<id>"` | 显式点名后端 | 有 | 该后端 | **是**（硬错误） |

**`""` 的含义不变**（与 ADR-029 §2.3 备选 B 同一条硬规则）。**`"none"` 是新增的显式取值**。

**默认值改 `"headless"` → `"auto"`。**

### 三、stats 新增两个字段（可观测，降级不是静默的）

在 `Stats` 的视频段（`engine.hpp` 的 `video_sink_frames_rendered` 附近）加：

```cpp
/** 实际使用的渲染后端 id（"d3d11" / "metal" / "egl" / "headless" / ...）。
 *  空 = 没有 sink（"none"）。 */
std::string video_sink_backend;

/** 降级原因（空 = 未降级）。
 *  取值："no-platform-backend" / "backend-not-registered" /
 *        "backend-open-failed" / "no-display"。 */
std::string video_sink_backend_reason;
```

**这是 D2-1「never a silent drop」的直接落地**：降级必须能被宿主问到。

### 四、窗口由渲染器自己建（引擎不碰窗口系统）

**Windows 的 D3D11 与 macOS 的 Metal 渲染器补上「没有 `native_window` 就自己建一个」的路径**，与 Linux EGL 已有行为对齐。引擎**不**创建窗口，也**不**持有窗口句柄。

| 平台 | 现状 | 本片要做 |
|---|---|---|
| Windows | `native_window == nullptr` → offscreen（不上屏） | `native_window == nullptr` → **自建 `HWND`** + swap chain；**建不出来**（无显示设备/CI）→ 走**已有的** offscreen 分支 |
| macOS | `native_window == nullptr` → offscreen（不上屏） | `native_window == nullptr` → **自建 `NSWindow`** + `CAMetalLayer`；**建不出来** → 走**已有的** offscreen 分支 |
| Linux | **已经自建窗口**（Wayland surface / X11 window） | 无 compositor 也无 X11 时 `open()` 返回 `false` → 引擎按「`open()` 失败 → 降级」处理 |

**宿主仍然可以完全接管**：传了 `native_window` 就用宿主的（**今天的行为逐字不变**），没传就渲染器自建。**两条路都在。**

**为什么窗口归渲染器而不是引擎**：L4 说 `src/engine/` 只做插件编排，不该知道三个平台怎么建窗口；渲染器已经知道（EGL 已经会建 Wayland surface 和 X11 window）；窗口的生命周期归渲染器的 `close()`，引擎再加一套就与 §D7 计划给的引擎生命周期打架。

### 五、`"none"` 的接线

`video_sink_name == "none"` → 引擎**不**通过 `pal::make_plugin` 实例化任何 sink。后果：

- `video_sink()` 返回 `nullptr`
- `video_render_path` 的判定输入里 `sink_gpu_render` / `sink_cpu_render` 都为 `false` → 路径判定是 `kIncompatible` → 引擎**照既有逻辑**报 `kVideoSinkIncompatible`（`engine_media.cpp:1219-1230`）。**这个行为不变** —— 本片不新增分支。
- `set_on_video_frame` 回调**照旧触发**（那是宿主的解码帧回调，与 sink 无关）。
- `video_sink_backend` 为空串。

### 六、bench 显式设 headless

**三个 bench 必须显式设 `video_sink_name = "headless"`**，否则渲染器的 GPU 开销会污染热路径测量：

- `tests/bench_engine_hotpath.cpp`
- `src/engine/tests/bench_capture_encode.cpp`
- `src/engine/tests/bench_control_saturation.cpp`

（后两个**今天已经**显式设了 `"headless"`，见 `bench_capture_encode.cpp:694`、`bench_control_saturation.cpp:1377` —— 本片只需**确认**它们没被默认值改动影响，并写进验收。）

## 设计

三条线，各自独立可判定：

**线一：解析（引擎私有）。** `src/engine/src/engine_render_policy.{hpp,cpp}` 是**私有**头（不进 `docs/api/public_headers.manifest` 的公开面），只依赖 `core` / `plugins`，**不** include `nimrtc::video_sink`。它做一件事：把 `"auto"` 解析成一个后端 id，或返回空串让调用方降级。**它不碰窗口、不碰 sink 实例** —— 那些是 `engine_media.cpp` 的事。

```
"auto" → default_render_policy().preferred_backend   // Windows d3d11 / macOS metal / Linux egl
       → is_registered(id) ? id : ""                   // 注册表里没有 → 空串 → 降级
```

`engine_media.cpp` 拿到 id 之后：

```
id 非空 → pal::make_plugin<&pal::resolve_video_sink>(id, sink, cfg,
             {.open_fail_mode = Fallback,        // ← 关键：不是 Error
              .missing_mode   = Fallback,
              .on_error       = on_error_})
       → 成功：video_sink_backend = id，reason = ""
       → 失败：video_sink_backend = "headless"，reason = "backend-open-failed" / "backend-not-registered"
```

**`Fallback` 而不是 `Error` 是这条线的全部要点**：sink 是**输出**设备，缺一个输出设备不该让 `open()` 失败。显式 id 仍走 `Error`（`kVideoSinkOpenFailed` / `kVideoSinkPluginMissing`）—— **显式是承诺，auto 是推荐**。

**线二：取值语义（公开面）。** `engine.hpp` 两处改：`video_sink_name` 的默认值 `"headless"` → `"auto"`；`Stats` 加两个字段。取值表：

```
""      → "headless"（今天的行为，逐字不变）
"auto"  → 平台渲染器，可降级        ← 新默认值
"none"  → 无 sink（引擎不渲染）
"headless" → 计帧但丢像素
"<id>"  → 显式后端，失败是硬错误
```

`""` 的含义**完全不变**（与 ADR-029 §2.3 备选 B 同一条硬规则）；`"none"` 是新增的显式退出。

**线三：窗口（渲染器侧）。** `src/modules/video_sink/src/{d3d11,metal}_renderer.cpp` 补「`native_window == nullptr` 且非 offscreen → 自建窗口」。**引擎一行窗口代码都不写**（验收标准 16 用静态检查断言 `src/engine/` 不 include `windows.h` / `Cocoa` / `wayland-client.h` / `X11/`）。

无显示设备时（CI）：Windows / macOS 走渲染器**已有**的 offscreen 分支（`d3d11_renderer.cpp:439-440`、`metal_renderer.cpp:294` 已有），Linux 走降级（EGL 的 `init_wayland()` / `init_x11()` 都失败 → `open()` 返回 false → 引擎降级）。

**三平台的 CI 表现不同，但都满足「`open()` 成功 + 有 sink + 降级可观测」** —— 这正是验收标准 6–10 要断言的。

### 破坏面

默认值一改，**50 个**构造 `EngineConfig` 而不设 `video_sink_name` 的文件里有 **42 个 `.cpp`** 受影响。其中：

- **bench 要显式设 `"headless"`** —— `tests/bench/bench_capture_encode.cpp` 与 `tests/bench/bench_control_saturation.cpp` **已有**这一行（`bench_capture_encode.cpp:694`、`bench_control_saturation.cpp:1377`），本片只需**确认**它们没被默认值改动影响；`tests/bench_engine_hotpath.cpp` 需**新增**这一行。
- **单元测试大多无所谓** —— 它们断言别的字段。
- **example 会弹窗** —— 这是这一版的**预期**。

## 写集

| 文件 | 改什么 |
|---|---|
| `src/engine/include/nimrtc/engine/engine.hpp` | `video_sink_name` 默认值 `"headless"` → `"auto"`；`Stats` 加 `video_sink_backend` / `video_sink_backend_reason`；`video_sink_name` 的文档改成取值表 |
| `src/engine/src/engine_render_policy.hpp` | **新增**（私有）：`RenderPolicy` / `default_render_policy()` / `resolve_auto_sink_backend()` |
| `src/engine/src/engine_render_policy.cpp` | **新增**（私有）：上者的实现 |
| `src/engine/src/engine_media.cpp` | `init_video_plugins()` 里解析 `"auto"`；失败降级 headless + `warn`；填两个 stats 字段；处理 `"none"` |
| `src/engine/CMakeLists.txt` | 加 `src/engine_render_policy.cpp` |
| `src/modules/video_sink/src/d3d11_renderer.cpp` | `native_window == nullptr` 且非 offscreen 时**自建 `HWND`**；建不出来 → 走既有 offscreen 分支 |
| `src/modules/video_sink/src/metal_renderer.cpp` | `native_window == nullptr` 且非 offscreen 时**自建 `NSWindow`**；建不出来 → 走既有 offscreen 分支 |
| `src/modules/video_sink/src/egl_renderer.cpp` | 无 compositor 也无 X11 时的失败原因要可区分（供引擎报 `no-display`） |
| `src/engine/tests/test_engine_render_policy.cpp` | **新增**：解析与降级的单元测试 |
| `tests/bench/bench_capture_encode.cpp` | **确认**已有的 `video_sink_name = "headless"` 仍在（默认值改动不得影响它） |
| `tests/bench/bench_control_saturation.cpp` | 同上 |
| `tests/bench_engine_hotpath.cpp` | **新增** `video_sink_name = "headless"`（今天没有） |
| `tests/acceptance/v1-video-render-engine/acceptance.test.cjs` | **新增**：验收测试 |
| `docs/adr/ADR-030-engine-driven-video-renderer.md` | **新增**：ADR |
| `docs/api/engine_api.md` | `video_sink_name` 的新语义 + 两个 stats 字段 |
| `docs/api/public_headers.manifest` | 若公开面符号数变化，按 `scripts/check_api_manifest.py --write` 重新生成 |
| `CHANGELOG.md` | 记一笔 |

## 验收标准

（每条都必须是**可判定**的。）

### 一、取值语义

1. 默认构造的 `EngineConfig::video_sink_name == "auto"`。
2. `video_sink_name = ""` → sink 是 `headless`（**与今天逐字等价**）。
3. `video_sink_name = "none"` → `video_sink()` 返回 `nullptr`；`video_sink_backend` 为空串。
4. `video_sink_name = "headless"` → sink 是 `headless`。
5. `video_sink_name = "no-such-sink"` → `open()` **失败**，错误码 `kVideoSinkPluginMissing`。

### 二、`"auto"` 的解析与降级

6. Windows 构建上 `"auto"` 解析到 `d3d11`；macOS 上到 `metal`；Linux 上到 `egl`（平台条件编译断言）。
7. `"auto"` 且平台后端**不在注册表里** → `open()` **成功**；sink 是 `headless`；`video_sink_backend == "headless"`；`video_sink_backend_reason == "backend-not-registered"`；日志有一行 `warn`。
8. `"auto"` 且平台后端 `open()` **失败** → `open()` **成功**；sink 是 `headless`；`video_sink_backend_reason == "backend-open-failed"`；日志有一行 `warn`。
9. 非 Windows 构建上 `"auto"` 解析不到平台后端 → 降级 `headless`，原因 `"no-platform-backend"`。
10. `"auto"` 成功解析 → `video_sink_backend` 报**实际**后端 id，`video_sink_backend_reason` 为**空**。

### 三、显式 id 不降级

11. `video_sink_name = "d3d11"`（Windows）→ 解析到 `d3d11`，`video_sink_backend == "d3d11"`；**不**降级。
12. 显式 id 的 `open()` 失败 → `open()` **失败**（硬错误），错误码 `kVideoSinkOpenFailed` —— 与 `"auto"` 的降级**行为不同**。

### 四、窗口归渲染器

13. Windows：`video_sink_name = "auto"` 且 `native_window` 为空 → 渲染器**自建**窗口（或在无显示设备时走 offscreen），`open()` 成功。
14. macOS：同 13。
15. Linux：同 13（**今天已经是这样** —— 回归保护）。
16. 静态检查：`src/engine/` 不出现平台窗口头的 include —— 具体是这四个：只读 `windows.h`、只读 `wayland-client.h`、只读 `X11/Xlib.h`、以及 Cocoa 的 `NSWindow` 头（`src/engine/` 不碰窗口系统）。
17. 静态检查：`src/engine/` 不引用 `nimrtc::video_sink` 的任何具体符号（只用 `plugins::IVideoSink` 接缝）—— L4 的「引擎只做编排」仍然成立。

### 五、stats 可观测

18. `Stats::video_sink_backend` 报实际后端 id。
19. 降级时 `Stats::video_sink_backend_reason` 非空；未降级时为空串。

### 六、破坏面

20. 三个 bench 显式设 `video_sink_name = "headless"`（`tests/bench_engine_hotpath.cpp` 需要**新增**这一行；`tests/bench/bench_capture_encode.cpp` 与 `tests/bench/bench_control_saturation.cpp` **已有**）。
21. `tests/test_engine_*.cpp` 一整套在默认值改成 `"auto"` 后**仍然通过**（回归）。

### 七、文档与契约

22. `docs/api/engine_api.md` 记录新的取值表与两个 stats 字段。
23. `docs/api/public_headers.manifest` 与实际公开面一致（只读 `scripts/check_api_manifest.py` — 跑它，不改它）。

## 本片不做

- **不做**渲染器**之外**的窗口抽象（新 seam、新公开接口）—— 会扩大公开面，计划没要求。
- **不做**渲染器窗口的**重建**（显示器热插拔 / 窗口被关）—— 那是 D4 的热插拔族（D4 是**采集**设备），本片只管初次打开。
- **不做**多显示器 / 窗口位置 / 窗口标题策略 —— 宿主的产品决定。
- **不做**把渲染后端塞进 D3 的 `DevicePolicy` 表 —— 采集策略与渲染策略的失败原因不同，混表会让两者的证据都不干净。
- **不改** `IVideoSink` 接口 —— `native_window` 已经是可选的，渲染器自建窗口不需要新接口。
- **不做**「每个 example 都弹窗」的开关 —— 弹窗是这一版的**预期**。

## 依赖方向检查

- `src/engine/` 新增的私有头 `engine_render_policy.hpp` 只 include `core` / `plugins` 的东西，**不** include `nimrtc::video_sink`。
- `src/modules/video_sink/` 继续自包含（L3），窗口代码进它自己的 `.cpp`。
- `src/core/` **不动**（注册表已有 `kVideoSink` 槽位，本片不需要新槽位）。
