---
title: "为什么 2026 年还要从零写一个 WebRTC 引擎？"
date: 2026-10-10
author: NimRTC 维护者
tags: [webrtc, cpp20, 开源, 设计笔记]
status: 观点文
---

# 为什么 2026 年还要从零写一个 WebRTC 引擎？

> 配套长文：[v0.11.0 Beta 前哨 Release Notes](docs/releases/v0.11.0-beta.md)。
> 这篇讲"为什么"，release notes 讲"是什么"。如果只有 5 分钟，
> [README](../README.md) 和 [Show HN 草稿](docs/marketing/show-hn-post.md) 更紧凑。

---

## 那个"为什么不直接用 libwebrtc"的问题

每次 NimRTC 出现在公开讨论里，第一条评论都一样：*为什么不直接用 libwebrtc？*

这是合理的问题。libwebrtc 是地球上最完整的 WebRTC 实现，心情好的时候
我也会直接用它。心情好大约是这样：你在写 Web 应用；后端跑 x86_64
Linux；二进制里多几百 MB 不心疼；加密路径就是 libwebrtc 自带的、也不
需要审计；3A 也是 libwebrtc 自带的、你也不需要看里面。

这五条都成立的话，答案就是 *用 libwebrtc*。这个项目不打算跟它竞争。

我真正在优化的场景是**这五条里有至少一条不成立**。具体说：

- **嵌入式 Linux aarch64。** 机器人、工业 SBC、Raspberry Pi 级别的板子。
  libwebrtc 的 150–250 MB 在这个语境下不是个小数字。NimRTC 是 5.6 MB。
- **信创 / GM/T 监管场景。** 审计链是目的本身。fork libwebrtc 换 DTLS
  后端是一个月级别的工程，产出一个你需要长期维护的分支。NimRTC 的
  `EngineConfig::dtls_name` 让你写几百行头文件 + .cpp 就能注册一个新
  后端。
- **AI Agent / 远程操作 / 云游戏。** 我要的是干净的前/后 3A PCM tap，
  让唤醒词模型或流式 ASR 既能听原始麦克风也能听 3A 之后的信号。在
  NimRTC 这是一个 200 行的文件（`src/audio/3a/pcm_tap.cpp`）；在
  libwebrtc 是一个要动五个 BoringOwned 模块的四周集成。
- **可读性。** 我想要一个单个工程师能 hold 住脑子的实时媒体引擎。约
  5 万行 C++20，严格的 L0–L3 分层加 header-only 插件接口——我赌这
  就是那个东西。

我不会说 NimRTC 在 WebRTC 这件事上"比 libwebrtc 强"。libwebrtc 更完
整、更多编解码、更多平台、更多贡献者、更多用户、更多测试、更多安全
审计。我主张的是：**对上面那几个具体场景，从零实现是更小、更可审
计、更可适配的选择。**

---

## 架构赌注：L0–L3 + PAL

任何中等规模 C++ 项目最难的问题是：*缝放在哪？* 插件、回调、虚函数、
泛型接口、消息通道——选择太多，每一个都决定未来十年的可演化性。

NimRTC 的赌注是一个四层编译期拆分：

```
L3  引擎门面（NimRTCEngine）          ← 应用代码唯一看到的东西
L2  编解码 + 3A                      ← `sfu` profile 跳过
L1  协议 / 状态（RTP、RTCP、SDP、     ← WebRTC 协议兼容
     ICE、DTLS、SRTP、JB、BWE）          的那一层
L0  原语（bytes、log、time、status）
PAL 插件适配层                       ← 编译期解析
插件接口（header-only）               ← src/plugins/ 下的 header
```

每一层都是一个 CMake 的 INTERFACE / STATIC 库。**Profile** 也是编译
期选择：`transport`（P2P 客户端）/ `sfu`（仅转发，不含 L2）/
`agent-gateway`（PCM 双 tap）/ `cloudgame`（高码率 + 对齐输入）/
`sfu-agent`（组合）。

这有趣的地方是：同一份源码树同时输出一个 P2P 客户端**和**一个 SFU 网
关。LiveKit 和 mediasoup 都是纯服务端；libwebrtc 是单体。OSS WebRTC
生态里我没见过谁把"P2P 客户端和 SFU 放在同一份源码"做了，而这件事
对 AI Agent 场景是承重的——网关端经常要两个方向：到用户浏览器的瘦
客户端，加上到下游 worker 的转发路径。

PAL（Plugin Adaptation Layer）是第二个赌注：所有能力通过
`core::PluginRegistry` 在编译期解析。零运行时间接寻址，公开 API
不变。这**不是**运行时的 service-locator 模式，而是一个类型化 slot
表，链接器在 PAL Slice 2 的 `register_all_default_plugins()` 时看到
它。重点是：你可以把 DTLS 从 wolfSSL 换成 GMSSL，只换链接的翻译单元，
引擎感觉不到。

ADR-009 是完整的设计记录；关键不变量：

- `src/core/` 是唯一的 INTERFACE 库。
- `engine.hpp` 不包含任何具体模块的 header。
- 单例在 `nimrtc::core::` 下。

这三条把缝面保持得小且稳定。

---

## 加密赌注：注册，而不是 fork

DTLS 这个缝——我认为——是项目里最被低估的特性。今天你可以注册
wolfSSL、OpenSSL、mbedTLS 或 BoringSSL 后端；计划下一步是 GMSSL，
面向信创部署。

`engine.cpp` 里的代码路径长这样：

```cpp
auto factory = reg.get_dtls_session(cfg.dtls_name);  // 空的话默认 "wolfssl"
std::unique_ptr<IDtlsSession> dtls = factory->create();
```

就这样。引擎从不直接命名 wolfSSL。缝面很小（`IDtlsSession` 大约
12 个方法），`DtlsSessionWolfSSL` 是一个 ~800 LOC 的适配器，翻译
到 wolfSSL 的 DTLS 1.3 API。

明天你想注册 GMSSL：

```cpp
// 在 your_gmssl_backend.cpp
class GmsslDtlsFactory : public dtls::IDtlsSessionFactory { /* ... */ };
NIMRTC_REGISTER_DTLS_SESSION("gmssl", &GmsslDtlsFactory::singleton());
```

```cpp
// 在你的应用
EngineConfig cfg;
cfg.dtls_name = "gmssl";
NimRTCEngine engine(cfg);
```

不 fork，不 patch，不加编译选项。这是让监管审计可行的东西：审计
可以单独审 GMSSL 适配器、签字，引擎的其余部分完全不动。

权衡：缝面保守，能塞进 `IDtlsSession` 的才能换。这是有意的——缝
面更大意味着公开 API 更不稳定。代价是有些 TLS 层的特性（比如
WebRTC 身份模型之外的客户端证书链）塞不进缝里。如果你需要那些，
fork 还是在桌面上的。但至少 WebRTC 身份模型的加密路径，缝够用。

---

## 数据面延后：WebTransport over QUIC

v0.11.0 **不**交付 Chrome ↔ NimRTC DataChannel 互操作。这是没赶上
的标题项，值得单独一段讲清楚为什么。

原计划：把 `IDtlsSession::srtp_keying_material()` 接进
`ISctpSocket::set_dtls_keys()`，DTLS 传输 ↔ SCTP socket 的入站 /
出站都打通。物理层这条线从来没上 `main`；Chrome-headless e2e 一直
0/4；v0.11.0 用 in-process `test_datachannel_engine` 5/5 作为
`IDataChannel` 缝面的替代。

我决定不再硬推 SCTP-over-DTLS 路径有两个原因：

1. **数据面在迁移。** Chrome 明确的方向是 "DataChannel future =
   WebTransport over QUIC"。2026 年下半年还在 SCTP-over-DTLS 上
   投工程，意味着投在一个主导浏览器正在主动弃用的路径上。我倾向
   跟着 Chrome 的信号走。
2. **缝面已经在。** `IDataChannel` 在 v0.11.0 是稳定接口。它下面
   的传输可以从"in-process stub"（今天）换成"SCTP-over-DTLS"（遗
   产互操作）再换成"WebTransport-over-QUIC"（v1.x），公开 API 不
   变。

v1.x 的设计会落在 **msquic** 上作为 QUIC 协议栈（MIT 协议，
Windows 优先，跨平台 CI）。`IDataChannel` 缝面保持不变；下面的传
输换。这正是 L0–L3 拆分被设计来支持的那种替换。

---

## 我希望从社区得到什么

2026 年从零写一个 WebRTC 引擎，是件挺奇怪的事。上面列的那些
理由是正当的；剩下就是把它做出来。

我最希望从读到这里的读者那里得到：

- **架构批评。** L0–L3 + PAL 这个拆法对不对？mediasoup 选运行时
  组合，LiveKit 选服务隔离，Pion 选语言人体工学。我哪里想错了？
- **插件接口反馈。** `docs/plugin_author_guide.md` 是主要契约。
  什么会让你贡献一个后端更容易 / 更难？
- **v1.x 设计伙伴。** 如果你在做实时音视频、愿意聊聊"你希望
  WebTransport / QUIC 数据面是什么样子"，我的邮箱在 README 里。
- **GM/T 密码学协作。** 如果你有能上线的 SM2 / SM4 代码、想注
  册一个 DTLS 后端，缝是开的。完整 P4 工作还在前面，但契约已经
  稳定。
- **Star / 转发，如果你想要过一个小巧可审计的 C++ WebRTC。**
  随手读到的读者能给项目最有用的，是 GitHub star——它决定下一
  个搜"webrtc alternative"的人能不能找到我们。

---

## 附录：数字

- **源码 LOC**（L0–L3，不含 tests / examples / docs）：~5 万。
- **静态库大小**（Release，MSVC，x86_64）：5.6 MB。
- **CI 矩阵：** Windows / Linux x86_64 / macOS arm64 / Linux aarch64
  —— 每次 push 四个平台都过 build + 单元测试。
- **协议互操作：** Chrome、Firefox、libwebrtc（通过
  `tools/run_e2e_acceptance.py --case d` 验证）。
- **插件接口：** 12 个（ITransport、IICETransport、IRTP、ISDP、IJB、
  IAudio3A、ICodec、IVideoSource、IVideoSink、IDataChannel、IHwCodec、
  IHw3A）。

这些是我公开引用的唯一些数字；如果你发现哪个过时了，release
notes 是真值，README 是要先更新的。

---

*感谢读完。如果你能读到这里，你大概也会喜欢 [Show HN 草稿](
docs/marketing/show-hn-post.md)——同一个项目，同一个作者，不同的腔调。*
