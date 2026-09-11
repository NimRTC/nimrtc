# NimRTC

> **Status: 0.9.0-rc1 (Release Candidate).** Tested on Windows / Linux / macOS / aarch64 — see [CHANGELOG](CHANGELOG.md) "Platform support matrix" for details. Chrome DTLS interop is incomplete (see [CHANGELOG](CHANGELOG.md) "Known issues").

**Native C++ WebRTC alternative — C++20, embeddable, scene-assembled.**

一个 codebase 既发 P2P 客户端、又发 SFU 网关、又能跑在 aarch64 嵌入式 Linux 上；DTLS / RTP / 3A 后端可替换，crypto 路径可切国密。

---

## Quick Start

**Prerequisites**: CMake ≥ 3.20 · **MSVC 19.30+ (Windows 10/11 only for this RC)** · Ninja · Python 3.8+ (for the e2e harness).

> ⚠️ The "GCC 10+ / Clang 12+ / Apple Clang 14+" prerequisite listed in earlier
> drafts is **not** valid for 0.9.0-rc1. The project may configure on those
> platforms, but build + test + e2e acceptance have only been run on Windows.
> See `CHANGELOG.md` "Platform support notice".

**Clone and configure** (Windows, MSVC + Ninja, x64 dev prompt):

```bat
git clone --recurse-submodules https://github.com/NimRTC/nimrtc.git
cd nimrtc
cmake --preset dev.msvc
cmake --build build --config Debug -j
```

**Run all tests** (after build):

```bat
ctest --preset tests.msvc --output-on-failure
```

**Run the loopback-p2p smoke test** (two in-process agents handshake over real UDP):

```bat
cmake -B build -DNIMRTC_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target loopback-p2p
.\build\examples\Debug\loopback-p2p.exe
```

**Run the full end-to-end acceptance suite** (includes NimRTC↔Chrome via Playwright):

```bat
python tools\run_e2e_acceptance.py
```

Artifacts land in `build/e2e/`.

> **First-build note**: libopus 1.6.1 is fully vendored under `src/third_party/libopus/src/`. No external download required. The vendored versions of `libjuice`, `libsrtp`, and `mbedtls` are pinned in [`src/third_party/SOURCE_VERSIONS`](src/third_party/SOURCE_VERSIONS).

---

## 它是什么、不是什么

| NimRTC 是 | NimRTC **不是** |
|---|---|
| 一个**分层**实时媒体引擎（L0–L3，编译时剪裁） | 一个完整的浏览器内核 |
| 一个**场景组装器**（Profile = transport / sfu / agent / agent-gateway / cloudgame）| 一个大一统的"万能"协议栈 |
| 一个**ABI 友好的 native 替代**（libwebrtc 之外的选择，crypto / codec 都不黑盒）| 另一个 libwebrtc 的 fork |
| 一个**为 AI Agent / 工业遥操作 / 嵌入式**设计的差异化栈 | 一个"通用" WebRTC |

---

## 架构差异化（与同类项目相比）

下面这些**单点都不新**，但**组合在一起**在 2026 年的开源 WebRTC 生态里是少见的：

1. **分层剪裁 + Profile 组合**——L0/L1/L2/L3 模块化，编译时选层。同一份代码既能发出 P2P 客户端（全栈），也能发出 SFU 网关（**跳过 L2**）。LiveKit / mediasoup 是 server-only，libwebrtc 是 monolithic，不能切层切到这个粒度。
2. **首期平台：Windows-only（0.9.0-rc1）** —— 0.9.0-rc1 阶段只在 Windows 10 / MSVC 上验证过构建、单测、e2e 互通（含真实 Chrome）。Linux/macOS/aarch64 路线图上是 P1/P2 目标，**当前 RC 不要在那上面部署**。
3. **Crypto 后端可替换**——DTLS 后端接口允许在同一 codebase 内替换为 OpenSSL / mbedTLS / 国密（GMSSL / WoTrCrypt）。这是大多数开源 WebRTC 栈**没有**的设计点——crypto 后端通常直接焊死。
4. **三层 + Profile 显式公开**——`docs/zh/NimRTC-V2-技术文档.md` §2.6 把组合形态写进首版定位，避免"用户拿到 README 不知道能拼出什么"的常见歧途。

> **plugin 接口是这套架构的"接缝"设计**——和上面四条组合搭配才出差异化。详见下面 [§ plugin 接口的目的](#plugin-接口的目的--一个被低估的架构特色)。

---

## plugin 接口的目的——一个被低估的架构特色

NimRTC 几乎所有"可替换"的能力都通过 plugin 接口（`src/plugins/include/nimrtc/plugins/*.hpp`，ADR-001）暴露：ITransport、IICETransport、IRTP、ISDP、IJB、IAudio3A、ICodec、IVideoSource、IVideoSink、IVideoReceiver/IVideoSender、IDataChannel、IHw*（hw_seam）。

| 维度 | 显式 plugin 接口的价值 | 不做 plugin 接口的代价 |
|---|---|---|
| **后端替换** | ICE ↔ QUIC transport、RTP 内核调试器、3A 旁路实现——同一份 engine 业务代码切换 | 每个后端都要 fork engine，违反 OCP（开闭原则） |
| **测试隔离** | 测试用 `MockTransport` / `NullAudio3A` / `CountingJitterBuffer` 注入，**不需要 mock framework** | 必须用 gmock / virtual mock 类污染生产代码 |
| **企业版插桩** | 国密 DTLS、3A 旁路、SLA 监控——企版编译时**仅替换 plugin 实现**，核心仓主干不分裂 | 企版要么 fork 主干（漂移），要么用 `#ifdef`（不可维护） |
| **第三方生态** | 用户可写 `MyAudio3A : plugins::IAudio3A` 注入（`-DNIMRTC_MODULE_AUDIO3A=MyAudio3A`），不需要碰 engine | 用户必须 `#include <nimrtc/audio3a/...>` 才能扩展，破坏封装 |
| **故障域隔离** | plugin 接口的 status code 是契约，编译期强制实现者处理错误路径 | 错误码五花八门，跨模块失败原因追踪极其痛苦 |

**对比基线**：GStreamer / FFmpeg / OBS / PipeWire 都用 plugin 架构，但那是在多媒体生态里；WebRTC 生态里（libwebrtc / Pion / LiveKit / mediasoup / janus）**只有 Pion 和 libwebrtc 内部**有类似抽象。**P2P 客户端和 SFU 跑在同一个 codebase + 同一套 plugin 接口**——这条线在 2026 年的开源 WebRTC 生态里几乎没有第二家。

plugin 是 NimRTC **可演进性**的核心机制：P0–P1 主线用内置实现，P2–P3 引入的"3A 双 tap / 严格优先级 / ref_frame"差异化能力也以 plugin 形式呈现（P2 `IJB::set_render_delivered`、P3 `IRTP::set_ref_frame`）。

详细设计见 `docs/adr/ADR-001-plugin-system.md`。

---

## C++ 标准：v0.8 选 C++17，v0.12 实操迁到 C++20

| 时间点 | 标准 | 触发原因 |
|---|---|---|
| v0.8 文档 | C++17 | 最大编译器覆盖（GCC 9 / Clang 9 / MSVC 19.20+）；嵌入式 / 政企老环境最广 |
| v0.12 代码 | **C++20** | 见下面"为什么迁" |

**v0.12 迁到 C++20 的两个核心理由**：

1. **`std::span` 必须有**——`src/core/bytes.hpp` 已经是 `using ByteSpan = std::span<const std::uint8_t>`，整个 RTP / RTCP / SDP / ICE / 3A 的零拷贝视图都基于它。C++17 里只有 `gsl::span`（非标准）或手写 pointer+length。`std::span` 是 C++20 标准库，是 zero-copy 字节视图的"终态"。
2. **`<chrono>` 在 C++20 才稳定**——`std::chrono::steady_clock` 的 `to_stream`、calendar types、`hh_mm_ss` 等 P0 暂时用不上，但 P1 起 RTCP NTP 时间戳对齐、ref_frame 时间线（§8.4）会需要 C++20 chrono 的精度和格式化能力。

**为什么 v0.8 当时选 C++17**：当时 P0 还在做 scaffolding，**还没用到 span**，chrono 也不需要 C++20 特性——选 C++17 是保守"先把代码写出来"。**现在（v0.12）P1 已经定型了 `std::span` + C++20 chrono 的使用面**，回退 C++17 收益是负的（要重写 bytes.hpp、改所有 `BufferView` 用法、损失 P3 计划的格式化能力）。

**给使用者的结论**：
- **新代码 / 新项目用 NimRTC → 直接 C++20**，无成本。
- **如果你的环境锁死 C++17**（如某些信创 GCC 8.x）→ 暂时不可用，P2 才会做"降级到 C++17 的"shim"。这条已经写进 `docs/zh/NimRTC-V2-技术文档.md` §15.2 待决问题。

---

## 三个差异化能力（针对 AI Agent / 遥操作 / 嵌入式场景）

| 需求 | NimRTC 差异化 | 当前同类项目状态 |
|---|---|---|
| **3A 旁路**：Agent 需要把语音喂给 ASR | `pre-3a / post-3a` **双 PCM tap** 原语（§8.7）——3A 不再黑盒，可旁路 | WebRTC APM 不可配置，PCM 中段无 hook |
| **控制消息不被视频挤占**：遥操作指令不能丢 | 严格优先级调度契约（§8.3）——指令帧可声明 QoS 不被挤带宽 | DTLS-SCTP / DataChannel 仅有尽力而为语义 |
| **采集-决策对齐**：AI Agent 决策要与视频帧关联 | `ref_frame` 时间线 API（§8.4）——帧号 ↔ 决策时刻对齐 | libwebrtc 无此抽象，application 层必须自建 |

这三项是 P2 / P3 的目标交付，不是 P0 已具备。

---

## 场景 Profile（P0 文档规划，P1 起逐步落地）

| Profile | 包含层 | 用途 |
|---|---|---|
| `transport` | L0 + L1 + L2 + L3 | 完整 P2P 客户端（Chrome 互通） |
| `sfu` | L0 + L1 + L3（**跳过 L2**）| 服务器转发（不重编解码） |
| `agent-gateway` | L0 + L1 + L2(tap 打开) | AI Agent 接入（PCM 双 tap + 旁路） |
| `cloudgame` | L0 + L1 + L2 + L3（高码率主线 + 输入渲染对齐）| 遥操作 / 云游戏（v0.10 列为远期候选） |

Profile 是**编译期配置**，不是运行时分发。详见 `docs/zh/NimRTC-V2-技术文档.md` §2.6。

---

## 路线（P1–P4 简化版）

- **P1 传输 MVP**：Chrome ↔ NimRTC P2P 音视频互通；vendor libsrtp + libopus + mbedTLS + WebRTC APM；**aarch64 编译可过 ≠ 互通可过**（§13.1 口径）
- **P2 场景差异化**：pre/post-3a 双 PCM tap；严格优先级调度契约；usrsctp DataChannel 基础互通；首批 Profile 库
- **P3 客户端质量 + ref_frame**：自适应 JB + Goog-CC 风格 BWE；ref_frame 时间线完整实现；首个付费标杆客户
- **P4 生产化 + 国密企版**：双链路 / 接管框架企版；国密后端落地；首份商业合同

详细路线图见 `docs/zh/NimRTC-V2-技术文档.md` §13。

---

## 当前进度（P0 真实状态）

| 项 | 状态 |
|---|---|
| 文档 v0.12 设计 | ✅ 完成 |
| P0 脚手架（CMake / CI / vendor 集成） | 🚧 进行中 |
| vendor 库落地（mbedTLS / libsrtp / libopus） | ⏳ P0 |
| Chrome 互通 P2P demo | ⏳ P1 |
| 三项差异化落地（PCM tap / 严格优先级 / ref_frame） | ⏳ P2 / P3 |

---

## Build 配置 / Repository Layout

### CMake options (defaults in **bold**)

| Option | Values | Effect |
|---|---|---|
| `NIMRTC_BUILD_TESTS` | **ON** / OFF | Build gtest-based unit tests |
| `NIMRTC_BUILD_EXAMPLES` | ON / **OFF** | Build the `loopback-p2p` smoke example |
| `NIMRTC_BUILD_DOCS` | ON / **OFF** | Build Doxygen API docs (requires Doxygen installed) |
| `NIMRTC_VENDORED` | **ON** / OFF | Use vendored `src/third_party/*` libraries |
| `NIMRTC_ASAN` | ON / **OFF** | AddressSanitizer (Debug + GCC/Clang/MSVC ≥ 2019) |
| `NIMRTC_UBSAN` | ON / **OFF** | UndefinedBehaviorSanitizer |
| `NIMRTC_WARNINGS_AS_ERRORS` | **ON** / OFF | Treat all warnings as errors |

### Directory layout

```
nimrtc/
├── CMakeLists.txt             # Root CMake (C++20, options above)
├── CMakePresets.json          # Presets: dev, debug, release, asan, ci.{linux,windows,macos}
├── docs/                      # Technical doc (zh/en), ADRs, security notes
├── examples/
│   └── loopback-p2p/          # Two-agent ICE handshake smoke test
├── src/
│   ├── core/                  # bytes.hpp, log, time, status codes (L0)
│   ├── engine/                # NimRTCEngine façade (top-level entry)
│   ├── modules/
│   │   ├── ice/               # ICE state machine
│   │   ├── sdp/               # SDP offer/answer parser
│   │   ├── rtp/               # RTP packet builder / parser / munger
│   │   ├── srtp/              # SRTP encryption (wraps libsrtp)
│   │   ├── jb/                # Jitter buffer
│   │   └── audio3a/           # 3A (AEC/ANS/AGC) audio processing
│   ├── plugins/               # Public plugin interfaces (see ADR-001)
│   └── third_party/           # Vendored: libjuice, libsrtp, mbedtls, libopus (1.6.1)
├── tests/                     # Cross-module gtest integration tests
├── interop/                   # Chrome / Firefox baseline interop harness
├── cmake/                     # Shared CMake helpers (NimRTCOptions, NimRTCTest, …)
├── tools/                     # Vendor scripts, fuzzers, profiling helpers
└── .github/
    └── workflows/ci.yml       # CI matrix (linux × gcc/clang, windows × msvc, macos × 2)
```

### Vendored third-party versions

Pinned in [`src/third_party/SOURCE_VERSIONS`](src/third_party/SOURCE_VERSIONS):

| Library | Version | Upstream |
|---|---|---|
| `libjuice` | `77daa8b` (master) | github.com/paullouisageneau/libjuice |
| `libsrtp`  | `2f82ec0` (master) | github.com/cisco/libsrtp |
| `mbedtls`  | `4.2.0` (release)   | github.com/Mbed-TLS/mbedtls |
| `libopus`  | `1.6.1` (release)      | github.com/xiph/opus — fully vendored |

---

## 协议 / License

- **License**: Apache-2.0（见 `LICENSE`）
- **第三方依赖**: 见 `NOTICE` 与 `docs/zh/NimRTC-V2-技术文档.md` §11（借用策略）
- **借用策略**: 密码件 / 编解码 / SCTP / 3A 等成熟模块一律 vendor；RTP / RTCP / SDP / JB / BWE / ICE 状态机 / timeline 调度等核心协议层一律自研。详见 `docs/zh/NimRTC-V2-技术文档.md` §11。
- **贡献合规**: DCO 签名（`git commit -s`），不采用 CLA。详见 `CONTRIBUTING.md`。

---

## 加入 / 反馈

- **GitHub Issues**: 报告 bug / 提 feature / 互通兼容性反馈（首选通道）
- **Pull Requests**: 提交前阅读 `CONTRIBUTING.md`，commit 必须 `-s` DCO 签名
- **互通测试**: `interop/` 目录常驻 CI，Chrome（fixed stable）+ Firefox（fixed stable）是基准矩阵
- **安全报告**: 私密渠道——`SECURITY.md`
- **品牌反馈**: 商标 / 命名相关走 `SECURITY.md` 同渠道

详细贡献流程与治理纪律见 `docs/zh/NimRTC-V2-技术文档.md` §16。
