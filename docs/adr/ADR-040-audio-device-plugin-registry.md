# ADR-040: 音频设备后端并入 PluginRegistry — 设备接缝与其它接缝同构

| | |
|---|---|
| Number | 040 (renumbered from 028 — collided with ADR-028-ice-over-tcp-nimrtc-side) |
| Status | **Accepted** |
| Date | 2026-11-25 |
| Phase | v1.4 (D1) |
| Bound to | v1.4.0 |
| Supersedes | — |
| Related | `docs/plan/v1.4-plan.md` §4 (D1) 和 §5 (D1-1); `docs/plan/post-v1.4-gap-inventory.md` §1.2.6; `docs/adr/ADR-025-sdp-rtp-not-pluggable.md`; `src/plugins/include/nimrtc/plugins/audio_source.hpp`; `src/plugins/include/nimrtc/plugins/audio_sink.hpp`; `src/core/include/nimrtc/core/registry.hpp`; `src/modules/audio_device/` |

## Context

NimRTC 的接缝分两层：`src/plugins/` 放**接口**（header-only，稳定的词汇表），
`src/modules/<name>/` 放**实现**。引擎（`src/engine/`）按 Layout Invariant 4
只允许通过 `plugins::I*` 访问具体能力，具体模块的后端由
`core::PluginRegistry` 按 id 解析——`EngineConfig` 里每一个 `*_name` 键
（`transport_name` / `jb_name` / `codec_name` / `video_sink_name` /
`video_capture_name` …）背后都是这样一条链：

```
EngineConfig::video_sink_name
  → engine/pal/engine_plugin_resolver.hpp :: resolve_video_sink(id)
    → core::PluginRegistry::instance().get_video_sink(id)
      → <module>::register_default_plugins() 里注册进去的工厂
```

`src/modules/audio_device/` 是这条链上的**唯一例外**。

它的模块实现是完整的：`portaudio_backend.cpp`（真实麦克风 / 扬声器）、
`system_audio.cpp`（系统级回环）、`synthetic_backend.cpp`（CI 无声卡替身）、
`mixer.cpp` / `resampler.cpp` / `ring_buffer.cpp` / `aec_delay.cpp` /
`playback_progress.cpp`，公共头 8 个，单元测试 6 个，PortAudio 作为 vendored
依赖登记在 `src/third_party/vendor.json` 里。接口也在 `plugins/` 层：

```
src/plugins/include/nimrtc/plugins/audio_source.hpp  → IAudioSource / IAudioSourceFactory
src/plugins/include/nimrtc/plugins/audio_sink.hpp    → IAudioSink   / IAudioSinkFactory
```

**接口的形状和其他接缝完全一致**（`create(cfg)` 返回 `IPlugin*`，
工厂是 `const` 的、带 `id()` / `display_name()`）。

但是**发现路径**不是。模块自带三个自由函数：

```
src/modules/audio_device/include/nimrtc/audio_device/audio_device.hpp:292-317
    available_source_backends()   → std::vector<std::string>
    available_sink_backends()     → std::vector<std::string>
    source_factory(id)            → const plugins::IAudioSourceFactory*
    sink_factory(id)              → const plugins::IAudioSinkFactory*
```

`src/modules/audio_device/src/audio_device.cpp:32-83` 里，这几个函数用一串
`if (id == "portaudio")` 分支把 id 映射到模块内的 `const` 静态工厂对象。
模块**没有** `register_default_plugins()`：

```console
$ git grep -n "register_default_plugins" -- src/modules/audio_device/
（空）
```

对比 `video_sink/src/video_sink.cpp:292`、`video_capture/src/synthetic_capture.cpp:345`
都有。

后果是引擎无法接入它。`src/engine/` 对整个音频设备层零引用：

```console
$ git grep -ln "audio_device\|AudioDevice\|IAudioSource\|IAudioSink" -- src/engine/
（空）
```

`EngineConfig` 里没有 `audio_source_name` / `audio_sink_name`，
`register_all_default_plugins()` 的注册表里也没有音频设备的槽位。
模块被编译进库，但**没有一条从 `Engine` 到它的路径**。

`docs/plan/post-v1.4-gap-inventory.md` §1.2.6 把这条记为「`audio_device`
是全树里唯一不走 `core::PluginRegistry` 的插件类模块」，并把它列为
**D1 的 owner 决策项**。

### 为什么现在必须决定

D1 的 DoD 是「不配任何设备 id 时，引擎采集并播放音频；统计里能看到两个设备」。
这个 DoD 要求引擎能通过 `EngineConfig` 里的一个名字解析到一个设备工厂。
**`EngineConfig` 的形态一旦冻结就难改**（ADR-024 已经为 `EngineConfig`
的字符串所有权做过一次专门决策），所以发现路径必须在写 `EngineConfig`
之前定下来。

## 难点的实质

引擎**不能**直接 include 音频设备模块的头。

ADR-025 把引擎的具体 include 收敛成一份**枚举过的例外清单**：

```
sdp / rtp / ice / dtls / srtp / audio3a / transport / web_transport
```

`audio_device` **不在**这份清单里。所以：

- 引擎调 `audio_device::source_factory(id)` ⇒ 必须 `#include <nimrtc/audio_device/audio_device.hpp>`
  ⇒ 违反 ADR-025 ⇒ 要么改架构决策文档，要么把 `audio_device` 加进例外清单。
- 引擎通过 `plugins::IAudioSourceFactory` 拿工厂 ⇒ 需要**有人**把工厂注册到
  引擎能看见的地方 ⇒ 那就是 `core::PluginRegistry`。

**两条路都通向注册表。** 唯一的问题是：注册是走 `core::PluginRegistry`
（与全树一致），还是引擎自己维护一张平行的小表。

## 两个选项

### Option A — 并入 `core::PluginRegistry`

给注册表加两个槽位（`kAudioSource` / `kAudioSink`），照
`kVideoCapture` 的既有形状写；音频设备模块补一个
`register_default_plugins()`，把它的工厂注册进去；引擎通过
`pal::resolve_audio_source()` / `resolve_audio_sink()` 解析。

### Option B — 引擎自建查找

引擎自己持有一张 `id → factory` 表，在 `engine_impl.hpp` 里直接查
`audio_device` 模块的自由函数。

## Decision

**Option A。** 理由，按权重排列：

1. **B 不满足 ADR-025，除非改架构文档。** B 必然要求引擎 include
   `<nimrtc/audio_device/audio_device.hpp>`（工厂对象在模块里，引擎拿不到
   指针），也就是把 `audio_device` 加进 ADR-025 的例外清单。而 ADR-025
   的例外清单是**枚举出来的、窄的**——加进去要重开一次架构决策讨论。
   A 不需要碰任何架构文档。

2. **A 让 `EngineConfig` 的形态零风险。** D1 的配置键会长成
   `audio_source_name` / `audio_sink_name`，和 `video_sink_name` /
   `video_capture_name` 一模一样。D2 刚把视频 sink 用这个形状接完，
   音频照抄，**实现路径上没有设计风险**。B 则会让音频成为唯一一个
   「配置键长得一样、解析路径不一样」的例外。

3. **注册表是「可插拔后端」这个卖点的载体。** AGENTS.md 把可插拔后端列为
   NimRTC 的核心定位。不接注册表，第三方**无法**替换音频设备后端——
   它只能自己改引擎的代码。

4. **不一致本身就是要消除的状态，不是要保护的特性。**
   `post-v1.4-gap-inventory.md` §1.2.6 的原文把它记成「与其它接缝不一致」
   并列为待决项——它描述的是**现状的缺陷**，不是**特性的承诺**。

5. **注册表是已验证的机制。** `kDefaultRegistrars[]` 表
   （`src/core/src/pal_default_registrars.cpp`）加上
   `NIMRTC_REGISTER_*` 宏加上 `Category` 枚举加上 `pal::resolve_*` forwarder，
   这四层已经为 20 多个接缝服务过。新加两个槽位是**第五次**做同一件事。

### 旧自由函数的处置

`available_source_backends()` / `available_sink_backends()` /
`source_factory(id)` / `sink_factory(id)` 这四个函数是
`NIMRTC_API_STABLE` 的**已发布符号**（v1.3.1 及更早）。按
`RELEASING.md` §1.1，移除公开符号是破坏性改动，不能进 MINOR。

**决定：保留这四个函数，标记为 deprecated，内部改为转发到注册表。**

- **保留**：`RELEASING.md` §1.1 的兼容性承诺。
- **deprecated**（而不是静默转发）：一个已经公开的自由函数，在没有任何
  标记的情况下继续存在，会让新代码继续用它——而它正是这条 ADR 要消除的
  那条路径。标记让旧代码继续编译，同时让新代码走注册表。
- **内部转发**（而不是各自维护一份分支表）：否则注册表和自由函数会给出
  两个答案，在有人调用 `register_audio_source("my-mic", …)` 覆盖默认之后
  尤其明显。

弃用不等于立刻移除。移除的时机是下一个 MAJOR 或一个明确记录的
DEPRECATED 窗口。

### 设备枚举不进注册表

`enumerate_capture_devices()` / `enumerate_playback_devices()` 是**查询**，
不是**工厂**。注册表存的是「按 id 拿到一个工厂」，不存「列出这台机器上
有什么设备」。

**决定：设备枚举不进 `PluginRegistry`。** 它留在音频设备模块里，
但引擎要暴露它给宿主（D1 的一部分，gap inventory §1.2.5），只能通过
`plugins/` 层的接缝或 `Engine` 的公共 API，不能让引擎直接 include 模块头
（同 ADR-025）。

具体形态由 D1 的实施拆解决定，不在本 ADR 锁定。

## Consequences

### 正面

- 全树的接缝形状**统一**：`EngineConfig` 的 `*_name` → `pal::resolve_*` →
  `PluginRegistry::get_*` → 模块的 `register_default_plugins()`。
- 第三方可以 `register_audio_source("my-mic", &factory)` 覆盖默认后端。
- `PluginRegistry::list_audio_sources()` 让宿主能枚举可用后端。
- ADR-025 的例外清单**不变**——`audio_device` 不需要进去。

### 负面

- `src/core/include/nimrtc/core/registry.hpp` 要改。它是 v1.4 plan §8a.2
  点名的三个热点共享文件之一（N3/N4/N7/N8/D1/D2/D6/D7 都动它）。
  按 §8a.5 的规则，D1 的 PR 必须声明它改的是哪一节。
- 音频设备模块的旧自由函数要留一段时间，两个入口并存。
  **两个入口必须永远给出一致的答案**——这需要一条测试钉住。

### 中性

- `register_all_default_plugins()` 会注册音频设备后端。这意味着
  `audio_device` 模块被链接进引擎的**默认**路径。若宿主不需要音频设备，
  它仍然可以设 `audio_source_name = "none"`（D1-1 决定的显式 opt-out）。

## 实施清单

| # | 落点 | 改动 |
|---|---|---|
| 1 | `src/core/include/nimrtc/core/registry.hpp` | 加 `register_audio_source` / `register_audio_sink`、`get_*` / `list_*`、`Category::kAudioSource` / `kAudioSink`、`NIMRTC_REGISTER_AUDIO_SOURCE` / `_SINK` 宏、switch 分支、`namespace audio_device { void register_default_plugins() noexcept; }` 前向声明 |
| 2 | `src/core/src/pal_default_registrars.cpp` | `kDefaultRegistrars[]` 加 `audio_device::register_default_plugins`（`#ifdef NIMRTC_HAS_AUDIO_DEVICE`） |
| 3 | `src/modules/audio_device/src/audio_device.cpp` | 新增 `register_default_plugins()`，注册 `portaudio` / `synthetic` / `system-audio` / `synthetic-system-audio`（source）与 `portaudio` / `synthetic`（sink） |
| 4 | `src/modules/audio_device/include/nimrtc/audio_device/audio_device.hpp` | 四个旧自由函数标记 deprecated，内部转发注册表 |
| 5 | `src/engine/include/nimrtc/engine/pal/engine_plugin_resolver.hpp` | 加 `resolve_audio_source` / `resolve_audio_sink` 两个 inline forwarder |
| 6 | `src/engine/include/nimrtc/engine/engine.hpp` | `EngineConfig` 加 `audio_source_name` / `audio_sink_name`（空 = 默认设备，`"none"` = 显式 opt-out，见 D1-1） |

落点 1 触及热点文件；落点 6 触及 `engine.hpp`（另一个热点文件）。
按 v1.4 plan §8a.5 的规则，这两个落点必须在 PR 描述里声明。

## 验证

本 ADR 的每条断言都可复核：

```console
# 决定前：模块没有注册入口，引擎零引用
git grep -n "register_default_plugins" -- src/modules/audio_device/          # → 空
git grep -ln "audio_device\|IAudioSource\|IAudioSink" -- src/engine/         # → 空
git grep -n "register_audio_source\|register_audio_sink" -- src/core/         # → 空

# 决定后：三者都应命中
git grep -n "register_default_plugins" -- src/modules/audio_device/          # → 命中
git grep -ln "IAudioSource\|IAudioSink" -- src/engine/                      # → 命中
git grep -n "register_audio_source\|register_audio_sink" -- src/core/         # → 命中
```

一致性（两个入口必须给出一致的答案）由一条新测试钉住，
放在 `src/modules/audio_device/tests/` 下。

## 文档版本

2026-11-25 初稿。
