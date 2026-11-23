# ADR-030: 引擎驱动渲染器（`video_sink_name = "auto"` 的平台解析与降级）

| | |
|---|---|
| Number | 030 |
| Status | **Accepted** |
| Date | 2026-11-23 |
| Phase | v1.4 (D2) |
| Bound to | v1.4.0 |
| Supersedes | — |
| Related | `docs/plan/v1.4-plan.md` §D2（"the engine runs the renderer"）；`docs/adr/ADR-015-api-stability-policy.md`；`docs/adr/ADR-023-plugin-id-naming-contract.md`；`docs/adr/ADR-029-engine-driven-audio-device-layer.md`；`specs/slices/v1-device-policy.md`（D3/D4，其默认策略与降级语义是本 ADR 的先例）；`specs/slices/v1-audio-device-engine.md`（D1） |

---

## 1. 背景

v1.4 计划书 §D1 的验收标准是：

> with **no device ids configured**, an engine … opens without the host enumerating anything

D1 已经落地（`6a1e3a6e`）：音频设备层由引擎驱动。**但视频侧还没有对应的半边** —— 已核实：

| 事实 | 位置 | 后果 |
|---|---|---|
| `EngineConfig::video_sink_name` 默认值是 `"headless"` | `src/engine/include/nimrtc/engine/engine.hpp:725` | 默认配置下引擎**根本不渲染**，画面没人看 |
| 引擎**不选**平台渲染器 | `src/engine/src/engine_media.cpp:732` 直接把 `config_.video_sink_name` 丢给 `pal::resolve_video_sink` | 纯按 id 查注册表；没有 `"auto"` 的语义 |
| 三个平台渲染器**都存在且都注册** | `src/modules/video_sink/src/video_sink.cpp:275-284` 注册 `headless` / `pinned` / `d3d11` / `metal` / `egl` | 引擎**能**拿到它们，只是没人去选 |
| **Windows / macOS 没有窗口** | `d3d11_renderer.cpp:439-440`、`metal_renderer.cpp:294`：`native_window == nullptr` → 退到 **offscreen**（渲染到纹理，不上屏） | 即使宿主显式写 `"d3d11"`，**没有 `HWND` 也上不了屏** |
| 树里唯一的窗口创建代码在**测试文件**里 | `src/modules/video_capture/tests/test_windows_capture.cpp:189` | 引擎路径上**没有任何**窗口 |
| Linux 的 EGL 渲染器**自己会建窗口** | `egl_renderer.cpp:643`（`wl_compositor_create_surface`）/ `egl_renderer.cpp:853`（`XCreateSimpleWindow`） | Linux 侧**已经**能自己上屏 —— 三平台不对称 |

于是 §D2 的验收标准 ——

> with `video_sink_name = "auto"` (the default), a decoded frame reaches the screen on all three platforms through the engine — the host never touches D3D11 / Metal / EGL

—— 今天**不可能**成立：宿主要么什么都不做（画面停在 headless），要么自己写 D3D11 / Metal / EGL 呈现代码（正是这一版要消灭的东西）。

## 2. 需要人定的两个问题

本 ADR 的两个决策点是**计划书没有定死**的，且互相耦合，所以在这里显式记录选择与理由。

### 2.1 问题一：默认值改不改？

计划书 §D2 写的是「`EngineConfig::video_sink_name`（empty or `"auto"` = the platform renderer resolved through the registry）」，§6 的验收标准写的是「With `video_sink_name = "auto"`, frames reach the screen」。**两处都把 `"auto"` 当成默认**。

但树上的默认值是 `"headless"`，改成 `"auto"` 会影响**所有没有显式配置的构造点**。实测：50 个文件构造 `EngineConfig` 而不设 `video_sink_name`，其中 42 个是 `.cpp`（真的开 `engine.open()` 的会话），含 `tests/test_engine_*.cpp` 一整套、bench、demo。

### 2.2 问题二：窗口归谁建？

Linux 的 EGL 渲染器自己建窗口；Windows / macOS 的渲染器**假定窗口是宿主给的**（`native_window`）。若引擎只做「选择 + 接线」，那么 Windows / macOS 上 `"auto"` 只能拿到 offscreen 渲染器 —— **画面还是上不了屏**，验收标准不成立。

## 3. 决策

### 3.1 决策一：默认值改成 `"auto"`，**但** `"auto"` 是一个**可降级的推荐**，不是要求

`EngineConfig::video_sink_name` 的默认值从 `"headless"` 改为 `"auto"`。

**理由一：计划书是契约。** §D2 与 §6 两处都把 `"auto"` 当默认；偏离就得改计划书，而计划书改动需要理由 —— 这里的理由只有「怕 CI 挂」，而 CI 的问题有解（见决策二）。

**理由二：D3 已经建立了同一个模式的先例，而且验证过。** `specs/slices/v1-device-policy.md`（D3/D4）的 `resolve_auto_capture_backend()` 明写：

> The policy named a backend this build does not register. That is a *recommendation*, not a host requirement, so it degrades to "no capture device" instead of firing kVideoCapturePluginMissing.

D2 照抄这个语义：`"auto"` 解析不到、或解析到的渲染器 `open()` 失败 → **降级为 `headless` + `warn` 一行 + 在 stats 里报出实际用的后端与降级原因**，`open()` **不失败**。

**理由三：只有音频 + 视频两条设备策略一半 auto 一半不 auto 是不自洽的。** D1 的 `audio_source_name` 留空 = 宿主自己喂 PCM（空值含义**不变**，是历史包袱）；`video_capture_name` 留空 = 不开采集设备（也是历史包袱）。但**渲染**没有历史包袱要保 —— 今天的 `"headless"` 默认值从来不是「宿主自己渲染」，它是「**没人渲染**」。把它当成需要保护的语义是误读。

**破坏面的处理。** 默认值改变会影响 42 个 `.cpp` 构造点，但影响**不是均匀的**：

- **bench 必须显式设 `"headless"`** —— `tests/bench_engine_hotpath.cpp`、`src/engine/tests/bench_capture_encode.cpp`、`bench_control_saturation.cpp` 这三个测的是引擎热路径，渲染器的 GPU 开销会污染测量。规格里点名要求它们显式设 `"headless"`。
- **单元测试大多无所谓** —— 它们断言的是别的字段，`"auto"` 解析出来的 sink 行为与 `"headless"` 在「帧被计数」这一层等价。
- **example 会弹窗** —— 这正是这一版要的效果（宿主不碰 D3D11/Metal/EGL）。

### 3.2 决策二：`"auto"` 的解析是**分平台的**，且每一步都有降级

解析顺序（与 D3 的 `resolve_auto_capture_backend` 同形，但新增「平台偏好」这一维）：

```
"auto"
  → 按平台选首选后端：Windows "d3d11" / macOS "metal" / Linux "egl"
  → 该 id 在注册表里吗？不在 → 降级 headless + warn
  → 在 → make_plugin(..., open_fail_mode = ???)
  → open() 失败 → 降级 headless + warn + 记原因
  → 都失败 → 降级 headless（永远有一个 sink 可用）
```

**关键：`open()` 的失败模式是 `Fallback`，不是 `Error`。** D3 的采集侧是 `Error`（显式 id 打开失败是硬错误），但 sink 侧不同 —— sink 是**输出**设备，缺一个输出设备不该让整个会话开不起来。理由与 D1 的音频降级一致（ADR-029 §3.2 第 4 条）：**设备打开失败不是致命错误，降级为「无设备」并 `warn`**。

**降级必须可观测**（D2-1 的原话：「never a silent drop」）。新增 stats 字段：

```
video_sink_backend      : std::string   // 实际用的后端 id（"d3d11" / "headless" / ...）
video_sink_backend_reason: std::string   // 降级原因（空 = 未降级）
```

### 3.3 决策三：窗口由**渲染器自己**建，引擎不碰窗口系统

**决定：Windows 的 D3D11 与 macOS 的 Metal 渲染器补上「没有 `native_window` 就自己建一个」的路径**，与 Linux 的 EGL 已有行为对齐。引擎**不**创建窗口，也**不**持有任何窗口句柄。

**为什么归渲染器而不是归引擎：**

1. **渲染器已经知道自己的窗口系统怎么说话。** EGL 渲染器已经会建 Wayland surface 和 X11 window；D3D11 渲染器已经会建 D3D11 设备；Metal 渲染器已经会建 `CAMetalLayer`。让引擎去建窗口，引擎就得知道每个平台怎么建 —— 那是把三份平台代码搬进编排层，违反 L4「`src/engine/` 只做插件编排」。
2. **窗口的生命周期归渲染器。** 渲染器有 `close()`，窗口跟着它关。引擎要管窗口，就得再加一对生命周期 —— 而 §D7 已经在给引擎加生命周期了，不能在 D2 提前引入第二套。
3. **宿主仍然可以完全接管。** 宿主传了 `native_window` 就用宿主的（今天的行为不变）；没传就渲染器自建。**两条路都在。**

**三平台的具体形态：**

| 平台 | 后端 | 无 `native_window` 时 |
|---|---|---|
| Windows | `d3d11` | 建一个 `WS_OVERLAPPEDWINDOW` 的 `HWND`（`CreateWindowExW`）+ D3D11 swap chain。**没有显示设备时**（CI）→ 走渲染器**已有的** offscreen 路径（`d3d11_renderer.cpp:439-440` 已有该分支，不上屏但计帧、跑呈现时钟） |
| macOS | `metal` | 建一个 `NSWindow` + `CAMetalLayer`。**没有显示设备时**（CI）→ 走渲染器**已有的** offscreen 路径（`metal_renderer.cpp:294` 已有该分支） |
| Linux | `egl` | **今天已经是这样**（`wl_compositor_create_surface` / `XCreateSimpleWindow`）。但**没有 compositor 也没有 X11** 时（CI）→ `init_wayland()` / `init_x11()` 都失败 → 现在的 `open()` 返回 `false` → 引擎按决策二降级 headless |

**注意 CI 上的差异**：Windows / macOS 在 CI 上会**成功**开一个 offscreen 渲染器（D3D11 设备 / Metal 设备在 CI 上可创建），Linux 在 CI 上会**失败**并降级。三平台的 stats 表现不同，但**都满足**「`open()` 成功 + 有 sink + 降级可观测」。

### 3.4 决策四：`"none"` 与 `"headless"` 的分工

`"none"` 与 `"headless"` **不是同一个东西**，规格里要写死：

| 值 | 语义 | 有 sink 实例吗 | 帧去哪 |
|---|---|---|---|
| `"auto"`（**默认**） | 平台渲染器；失败则降级 | 有（至少 headless） | 渲染器 |
| `"headless"` | 显式要一个「计帧但丢像素」的 sink | 有 | 计数丢弃 |
| `"none"` | **显式退出**：不要 sink | 无 | 引擎不解码后渲染（`set_on_video_frame` 回调仍照旧触发） |
| `"<id>"` | 显式点名一个后端 | 有 | 该后端 |

**显式 id 打开失败仍然是硬错误**（`kVideoSinkOpenFailed`）—— 与 D3 的采集侧一致：**显式是承诺，auto 是推荐**。

**`"none"` 是新增的取值**，理由与 D1 的 `"none"` 完全一样（ADR-029 §2.3）：空值本来就有别的含义，改空值语义会让所有既有宿主静默改变行为。

### 3.5 决策五：`""`（空值）的含义不变

空值 = **今天的行为**（`"headless"` 的等价物）。理由与 ADR-029 §2.3 的备选 B 完全相同：空值含义**完全不变**是硬规则。

所以取值表是：

```
""      → "headless"（今天的行为，逐字不变）
"auto"  → 平台渲染器，可降级      ← 默认值
"none"  → 无 sink
"<id>"  → 显式后端，失败是硬错误
```

## 4. 后果

### 4.1 正面

- D2 的验收标准首次可能成立：`"auto"` + 引擎驱动 + 三平台各自上屏（Linux 早已能，Windows / macOS 补齐）。
- 宿主写「几行代码」的承诺在视频侧兑现：宿主不再碰 D3D11 / Metal / EGL。
- 降级路径可观测：宿主能问「现在用的是哪个渲染器、为什么不是它」。
- 与 D3 的默认策略机制**同形**：一套心智模型。

### 4.2 代价 / 风险

1. **改了 `EngineConfig` 的默认值** → 既有宿主静默从「不渲染」变成「渲染」。缓解：这是**这一版的意图**，不是意外；降级路径保证不渲染的宿主拿到的是 headless 而非失败。
2. **bench 被污染** → 规格点名要求三个 bench 显式设 `"video_sink_name = "headless"`。
3. **CI 上 Linux 会走降级分支** → 规格的验收要在 Linux 上断言「降级 + 原因」，在 Windows / macOS 上断言「offscreen + 后端 id」。
4. **窗口代码进 `src/modules/video_sink/`** → 那个模块要新增平台窗口代码。缓解：与 EGL 渲染器**今天已经做的事**同形，不是新范式。
5. **动了 `src/engine/include/nimrtc/engine/engine.hpp`**（默认值 + 两个 stats 字段）→ 公开面变更，要同步 `docs/api/engine_api.md` 与 `docs/api/public_headers.manifest`（ADR-015）。

### 4.3 明确不做

- **不做**渲染器**之外**的窗口抽象（新 seam、新接口）—— 那会扩大公开面，而计划没要求。
- **不做**「宿主完全不留痕」的呈现模式（每个 example 都弹窗是这一版的**预期**）。
- **不改** `IVideoSink` 接口 —— 渲染器自建窗口不需要新接口（`native_window` 已经是可选的）。
- **不做**多显示器 / 窗口位置 / 标题策略 —— 那是宿主的产品决定。

## 5. 备选方案与否决理由

### 备选 A：默认值保持 `"headless"`，`"auto"` 作为显式 opt-in

**否决**。理由：

1. 计划书 §D2 与 §6 两处都把 `"auto"` 当默认，偏离需要改计划书。
2. N6 的样例要求「≤ 200 行、不写渲染器代码」 —— 如果 `"auto"` 不是默认，样例还得写一行 `"auto"`，等于把宿主的活又推回宿主。
3. 与 D3 的 `video_capture_name = "auto"` 不对称。

**它唯一的好处**（零兼容性风险）被降级路径化解了：风险不在「默认值改了」，在「降级没写」。决策二把降级写死。

### 备选 B：窗口归引擎建

**否决**。见 §3.3 的三条理由 —— 最强的一条是 L4：`src/engine/` 只做插件编排，不该知道三个平台怎么建窗口。

### 备选 C：`"auto"` 解析失败时让 `open()` 失败

**否决**。与 D3 的降级语义、ADR-029 的音频降级语义都不一致；而且会让「CI 上 Linux 没 compositor」变成「整个测试套件跑不起来」。**输出设备缺失不是会话失败的理由。**

## 6. 验证方式

| 决策 | 验收方式 |
|---|---|
| 默认值是 `"auto"` | 单元测试：默认构造的 `EngineConfig::video_sink_name == "auto"` |
| `"auto"` 分平台解析 | 单元测试：Windows → `d3d11`、macOS → `metal`、Linux → `egl`（平台条件编译断言） |
| 解析不到 → 降级 | 单元测试：注册表里没有该后端 → `open()` **成功**、sink 是 `headless`、`video_sink_backend_reason` 非空 |
| `open()` 失败 → 降级 | 单元测试：假后端 `open()` 返回 `kErrUnsupported` → `open()` **成功**、降级 headless、原因非空 |
| 显式 id 失败 → 硬错误 | 单元测试：`video_sink_name = "no-such-sink"` → `open()` 失败 + `kVideoSinkPluginMissing` |
| `"none"` → 无 sink | 单元测试：引擎不实例化任何 sink；`video_sink()` 返回 `nullptr` |
| `""` → 今天的行为 | 单元测试：`video_sink_name = ""` → 等价于 `"headless"` |
| stats 可观测 | 单元测试：`video_sink_backend` 报实际后端；降级时 `video_sink_backend_reason` 非空 |
| bench 不被污染 | 静态检查：三个 bench 显式设 `video_sink_name = "headless"` |
| 窗口归渲染器 | 静态检查：`src/engine/` 不 include `windows.h` / `Cocoa` / `wayland-client.h` / `X11` |
| 依赖方向 | 静态检查：`src/engine/` 不引用 `nimrtc::video_sink` 的具体实现（只用 `plugins::IVideoSink` 接缝） |
| 不加新线程 | 静态检查：窗口创建不引入 `std::thread`（渲染器的 presenter 线程是既有的） |

## 7. 状态

**Accepted**（2026-11-23，人签字）。本 ADR 覆盖 §D2 的**架构决策**；D2 的切片规格在 `specs/slices/v1-video-render-engine.md`（已冻结，frozenHash `sha256:986a2063a5fe37b4513a595a0a8705b90f798e530ce2b323b0a079fb2cc0c7d6`）。

**签字要点**：本 ADR 含公开面变更（`EngineConfig::video_sink_name` 默认值 + 两个 `Stats` 字段），按 ADR-015 需要人批准 —— **已批准**。
