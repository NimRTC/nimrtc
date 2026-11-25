# ADR-039: 渲染器自建窗口是**显式 opt-in**（修订 ADR-030 决策三）

**状态**：Accepted（2026-11-25，人签字）
**修订**：[ADR-030](ADR-030-engine-driven-video-renderer.md) §3.3 决策三、§4.3
**影响模块**：`src/plugins/`（seam）、`src/modules/video_sink/`（渲染器）、`src/engine/`（配置传递）

---

## 1. 背景

ADR-030 决策三规定：Windows 的 D3D11 与 macOS 的 Metal 渲染器在没有 `native_window` 时**自己建一个顶层窗口**，与 Linux 的 EGL 已有行为对齐。该决策的目标是让 §D2 的验收标准「`video_sink_name = "auto"` 时画面上屏」成立。

这个决策落地后出现了一个**实测问题**：

`EngineConfig` 的默认值是 `"auto"`（ADR-030 决策一），而全树有 50 余处构造 `EngineConfig` 不设 `video_sink_name`（ADR-030 §2.1 自己统计过：42 个 `.cpp` 真的开 `engine.open()`）。于是**每一个**这样的构造点 —— 包括 `tests/test_engine_*.cpp` 整套单元测试 —— 在 `open()` 解析出 `d3d11` 后，都会走 `create_self_window()`，在开发者的桌面上弹出一个白窗口。

结果是一次 ctest 回归能在桌面上**连续闪几百次**白窗口：窗口被建出来（`ShowWindow` 可见），但因为 swap chain 创建失败，**没有任何一帧被呈现**，窗口内容永远是空白，随即被 `close()` 销毁，下一个测试再建一个。

这不是「多个测试并发」才有的现象 —— 单线程跑一遍 ctest 也是一样的闪，只是频率低一点。

**这不是设计意图。** ADR-030 §4.3 写的是「**不做**『宿主完全不留痕』的呈现模式（每个 example 都弹窗是这一版的**预期**）」。ADR-030 预期的是**example 弹窗**，不是**单元测试弹窗**。决策三没有区分「宿主想要画面上屏」和「一个默认构造的 `EngineConfig` 恰好走到了渲染器」，于是把两者合并成了一条默认路径。

## 2. 决策

### 2.1 决策一：自建窗口**默认关闭**，且由宿主显式 opt-in

新增两个字段：

| 位置 | 字段 | 默认值 | 语义 |
|---|---|---|---|
| `plugins::VideoSinkConfig` | `create_viewport_window` | `false` | 渲染器是否可以自己建输出窗口 |
| `EngineConfig` | `video_sink_viewport` | `false` | 同上，引擎侧入口 |

`EngineConfig::video_sink_viewport` 通过 `resolve_sink_config()` 传给 `plugins::VideoSinkConfig::create_viewport_window`。渲染器的 `create_self_window()` 在开关为假时**立即返回 false**，落回渲染器**已有的** offscreen 路径 —— 呈现时钟照跑，帧照计数，但**屏幕上什么都不出现**。

**为什么用开关而不是改默认值。** 把 `video_sink_name` 的默认值改回 `"headless"` 会推翻 ADR-030 决策一（那需要人重新签字，而且会让「宿主不设任何配置」的体验从「自动上屏」变成「什么都不渲染」）。用**独立开关**把「上屏」这个维度从「用哪个渲染器」这个维度里拆出来，两个决策互不干扰。

### 2.2 决策二：真正要上屏的宿主显式声明

`examples/remote-desktop/rd_viewer.cpp` 设 `cfg.video_sink_viewport = (opts_.sink_name != "headless") && !opts_.sink_name.empty();` —— 即「sink 不是 headless 就要上屏」。这是 ADR-030 §3.3 想要的「example 弹窗」效果，**由宿主显式要**，而不是渲染器自己决定。

### 2.3 决策三：Windows 上自建窗口用 `WS_EX_TOOLWINDOW`

即使开关打开，Windows 侧创建的窗口带 `WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`：不进任务栏、不进 Alt-Tab 列表、不抢焦点。一个上屏窗口被聚焦是宿主的选择，不是渲染器的权利。

### 2.4 决策四：修掉 swap chain 永远失败

排查过程中发现 `d3d11_renderer.cpp` 的 `DXGI_SWAP_CHAIN_DESC` 用了 `DXGI_SWAP_CHAIN_DESC{}` 的值初始化，而该结构体的 `BufferCount` 成员**没有默认值**，于是它是 **0**。DXGI 拒绝 `BufferCount == 0` 的 swap chain，`D3D11CreateDeviceAndSwapChain` 返回 `DXGI_ERROR_UNSUPPORTED (0x887a0001)`。

也就是说：**自建窗口这条路径从来没有真正成功过**。窗口弹出来了，但永远没有一帧上屏。

修复是显式设置 `desc.BufferCount = 1;`（并显式写 `desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;`）。修复后 `CreatesTheViewportWindowWhenAsked` 测试从 SKIPPED 变为 PASSED，swap chain 成功创建。

顺带澄清：`desc.BufferDesc.RefreshRate` 的 `{0, 1}` 组合**不是**失败原因（已实测：`0/1` 与 `0/0` 在有窗口的路径上表现一致），失败的是 `BufferCount`。

## 3. 后果

### 3.1 正面

- 单元测试不再在桌面上弹窗。实测：修复后跑 26 个视频相关测试，**峰值可见视口窗口数 = 0**。
- 修掉了 swap chain 永远失败这个**独立的、更严重的问题** —— 也就是说自建窗口路径此前是「弹了个白窗但没上屏」，现在才真正能上屏。
- 「上屏」成为宿主的显式意图，而不是渲染器的副作用。

### 3.2 代价 / 风险

1. **`EngineConfig` 公开面变更**（新增 `video_sink_viewport`）→ 按 ADR-015 同步 `docs/api/engine_api.md` 与 `docs/api/engine_config.md`。
2. **默认行为变了**：默认构造的 `EngineConfig` 不再上屏。缓解：这是本 ADR 的**意图**；`stats.video_sink_backend` 仍会报出实际后端，宿主想上屏就设开关。
3. **依赖并发 worktree 的旧二进制**：修复必须同步到所有正在跑测试的 worktree，否则旧二进制会继续弹窗。实测教训：闪得更厉害的时候，源头不在当前 worktree，而在**另外两个并发 worktree** 的未修复二进制。

### 3.3 明确不做

- **不做**「渲染器猜测宿主是否想上屏」的启发式（例如「有显示设备就上屏」）—— 猜测就是默认值出错的根源。
- **不删** `create_self_window()` 路径 —— 它是 §D2 验收标准成立的前提，只是改成 opt-in。

## 4. 备选方案与否决理由

### 备选 A：把 `video_sink_name` 默认值改回 `"headless"`

否决：推翻 ADR-030 决策一（已签字），且把「上屏」和「用哪个渲染器」两个维度继续绑在一起 —— 宿主想要「用 d3d11 但不弹窗」仍然做不到。

### 备选 B：给渲染器加「本进程是不是测试进程」的探测

否决：环境探测进生产代码路径，且无法覆盖「宿主自己就是个没有屏幕的设备」（机器人、车载、服务器）。开关是宿主能表达的最诚实的东西。

### 备选 C：保留默认弹窗，只在 CI 上抑制

否决：问题不只是 CI —— 开发者在本地跑 ctest 一样被闪。抑制应该是默认行为，CI 只是更受益于它。

## 5. 验证方式

| 决策 | 验收方式 |
|---|---|
| 默认不建视口窗口 | 单元测试：`D3D11Renderer.DoesNotCreateAViewportWindowByDefault` —— 渲染后枚举进程内窗口，无 `NimRTCD3D11Viewport` |
| 开关打开才建窗口 | 单元测试：`D3D11Renderer.CreatesTheViewportWindowWhenAsked` —— 窗口存在**且** swap chain 创建成功（帧真的能上屏） |
| 引擎传递开关 | 单元测试：默认 `EngineConfig{}` → sink 配置的 `create_viewport_window` 为假 |
| 引擎层不碰窗口系统 | 静态检查：`src/engine/` 不 include `windows.h` / `Cocoa` |
| 端到端不闪窗 | 实测：跑视频相关 ctest，监测全局 `NimRTCD3D11Viewport` 可见窗口数，峰值须为 0 |

## 6. 状态

**Accepted**（2026-11-25）。本 ADR 修订 ADR-030 §3.3 决策三与 §4.3；ADR-030 的其余决策（默认值 `"auto"`、分平台解析、降级语义）**不变**。
