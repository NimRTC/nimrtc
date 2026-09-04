# NimRTC

> ⚠️ **Status: experimental — not for production use.**
> 当前处于 P0 脚手架阶段（v0.12 文档设计期）。所有发布版本在 1.0 之前不得用于生产环境。详见 `docs/zh/NimRTC-V2-技术文档.md` §11.1 安全声明。

**Native C++ WebRTC alternative — C++20, embeddable, scene-assembled.**

一个 codebase 既发 P2P 客户端、又发 SFU 网关、又能跑在 aarch64 嵌入式 Linux 上；DTLS / RTP / 3A 后端可替换，crypto 路径可切国密。

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
2. **首期平台矩阵一致**——`x86_64-linux/macos/windows` + `aarch64-linux-gnu` **同一个 codebase**。aarch64 CI 编译由 GitHub Actions ARM runner 兜底（P0/P1 是编译验证，互通验证在 x86_64 CI）。
3. **Crypto 后端可替换**——DTLS 后端接口允许在同一 codebase 内替换为 OpenSSL / mbedTLS / 国密（GMSSL / WoTrCrypt）。这是大多数开源 WebRTC 栈**没有**的设计点——crypto 后端通常直接焊死。
4. **三层 + Profile 显式公开**——`docs/zh/NimRTC-V2-技术文档.md` §2.6 把组合形态写进首版定位，避免"用户拿到 README 不知道能拼出什么"的常见歧途。

> plugin 架构是行业基线（GStreamer / FFmpeg / OBS / PipeWire 都有），这里**不强调**。差异化是上面四条的**组合**。

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
