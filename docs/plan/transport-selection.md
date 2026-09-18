# Transport Selection for Low-Latency Control Channels

| | |
|---|---|
| Version | v1.0（draft — 研究笔记，非 ADR） |
| Date    | 2026-09-14; Slice 7.5 落地更新 2026-09-18 |
| Status  | **Draft** — research note; Slice 4/5/6/7/8 已全部或部分落地；见 §12.5 Slice 7.5 post-mortem |
| Phase   | P1.1 → P2 入口（v0.10.0 收尾后，v0.11.0 立项前） |
| Scope   | 传输层 plugin-replaceable 化的"理想态"与 Slice 4–8 落地路径 |
| Bound   | 无（不绑定具体版本——下面 §7 给出推荐 binding） |
| Refs    | `docs/zh/architecture.md` §13 P2/P3；`docs/plan/pal-architecture.md` §3.5；ADR-009（PAL Slice 1）；ADR-010（JSON Profile）；`docs/plan/v0.11-preview.md`；`docs/plan/dtls_chrome_interop_review.md` |

> **术语校正（请先看 §9）**：本会话前面曾把 "PAL Slice 3 = 传输替换"。这是错的。
> 按 ADR-009，PAL Slice 3 是 `NIMRTC_PLUGIN_ID(name)` 编译期 ID 字面量校验宏，跟传输层无关。
> 传输层的 seam 化从 PAL Slice 4 起，本笔记命名为 **Transport PAL (T-PAL)** 段落，与现有 PAL 节奏保持一致。

---

## 1. 背景与动机

v0.9.2 已 tag，v0.10.0 收 PAL Slice 1 + chlog 卫生，v0.11.0 是 P2 入口
（DataChannel usrsctp 互通、in-process SFU relay、Profile 库官方化）。
在这个边界点上，三股力量正在把"传输层是不是 plugin-replaceable"这个问题推上台面：

1. **场景侧**——具身智能 / 无人车 / 远程操控这一类低延迟控制场景开始进入视野。
   这类场景对**控制通道**的延迟与可靠性要求和**媒体通道**不同：
   - 控制命令往往 50–200 Hz 的小包（10–60 B），p99 预算常压在 50–80 ms；
   - 媒体帧是几百 KB–几 MB 的批，p99 200–500 ms 可接受。
   WebRTC 的 SCTP DataChannel 走 cwnd 保守算法 + Nagle，对小包高频控制并不友好。

2. **协议侧**——WebTransport / Media-over-QUIC / 自研 UDP+ARQ 都在挑战 "WebRTC classic" 的默认位置。
   但浏览器侧短期内仍以 WebRTC classic 为兼容底线，"传输选型"作为概念在 NimRTC 里
   目前**没有 seam**——只能在文档层面讨论，无法在 Profile 里切换。

3. **架构侧**——PAL Slice 1/2/3 把 audio3a / codec / video_codec 的 plugin 化打通了，
   但 `docs/plan/pal-architecture.md` §3.5 **明确不动传输层**：
   > Transport / ICE source/sink plugin categories. `nimrtc::ice::register_default_plugins`
   > registers `IICETransportFactory*`, which is closely tied to the engine's
   > ownership of `ice_t_`. **PAL does not touch this in Phase 2.**

   换句话说：**ICE 已经 plugin 化（id="ice"，默认 libjuice），RTP 也已经 plugin 化，
   DTLS 硬编码 wolfSSL，SCTP 尚未接入，raw UDP bypass 不存在，整个 Stack 没有组合抽象，
   没有 Selector，没有 Profile 段的 transport 选项。**

本文档不讨论"该不该做"——上面三条已经把"做"的必要性说完了。下面的目标是把
**理想态**画清楚、把 **Slice 4–8 落地路径**写实，并把 v0.11.0 已经计划硬编码 usrsctp
的那条路与"plugin 化"对齐。

---

## 2. 现状审计（基于仓库实际代码）

| 层 | plugin 化 | 默认实现 | 注册位 | 备注 |
|---|---|---|---|---|
| **ICE** | ✅ 已支持 | libjuice 1.6.0（vendored） | `nimrtc::ice::register_default_plugins()` 注册 `IICETransportFactory`，id="ice" | `src/modules/ice/include/nimrtc/ice/ice.hpp` §"IceTransportFactory"；MUX 模式默认 |
| **RTP session** | ✅ 已支持 | NimRTC 自研 RTP | `nimrtc::rtp::register_default_plugins()` | `src/modules/rtp/src/rtp_plugin.cpp` |
| **DTLS** | ❌ **硬编码 wolfSSL** | `DtlsSessionWolfSSL`（封装 wolfSSL DTLS 1.2 + SRTP + ECDSA + AES-GCM） | 无注册点 | 唯一实现 `src/modules/dtls/src/dtls_wolfssl_session.cpp`；engine 直接 `new` |
| **SCTP / DataChannel** | ❌ v0.11.0 计划**硬编码 usrsctp** | usrsctp（计划） | 无注册点；`src/modules/datachannel/` 当前为桩 | 见 `docs/plan/v0.11-preview.md` §5 usrsctp 风险行 |
| **Raw UDP bypass** | ❌ **不存在** | N/A | N/A | 高频控制命令没有可走的 seam |
| **Stack 组合** | ❌ **不存在** | N/A | N/A | 没有"webrtc-classic" / "webrtc-quic" 概念 |
| **Selector** | ❌ **不存在** | N/A | N/A | 应用方只能在 engine.cpp 拼后端 |
| **Profile 段** | ⚠️ ADR-010 first-class，但**没有 `transport` 段 schema** | N/A | ADR-010 §6 说 loader API 留待 v0.11.0 | 现有 `profiles/{agent,call,live,transport}.json` 用顶层 `transport: {}` 占位 |

**结论**：
- ICE、RTP 是"现成 seam，扩接口即可"；
- DTLS、SCTP、raw UDP bypass、Stack、Selector 是"零 seam，需要 PAL Slice 4–8 建设"；
- ADR-010 已经为 Profile 留位，传输段 schema 需要在 Slice 7 一起定义。

---

## 3. DataChannel 延迟的实证观察（为什么 SCTP 不够）

WebRTC classic 的 DataChannel 走的是 `ICE → DTLS → SCTP → (RTP/RTCP 分流)`
六层栈。NimRTC v0.11.0 计划硬编码 usrsctp，与 Chrome 默认行为一致。问题不在握手，
而在**稳态小包**：

| 链路段 | 单跳延迟（典型 / 健康网络） | 备注 |
|---|---|---|
| ICE selected-pair 转发 | 1–5 ms | libjuice MUX 模式单 socket |
| DTLS 解密 | 0.2–1 ms | AES-GCM hardware；wolfSSL 路径已优化 |
| SCTP cwnd 启动 | **20–80 ms**（首次） | usrsctp 默认 INIT/RTT 估计算法保守；小包慢启动明显 |
| SCTP cwnd 稳态 | 5–20 ms / 包 | 每 RTT 增长一窗，10KB 以下包被 Nagle 合并 |
| Chrome 端到端 DC echo p50 | **30–80 ms** | 局域网测试稳定区间 |
| Chrome 端到端 DC echo p99 | **100–300 ms** | 抖动 + 偶尔 cwnd 重新探底 |
| 弱网（10% loss）下 p99 | **500 ms+** | usrsctp 快速重传 + cwnd collapse |

对**媒体流**这是可接受的（关键帧间隔远大于此）。对**控制命令**（100 Hz、p99 80 ms 预算）
**SCTP 不达标**。这是为什么 §5 的理想架构里要保留 `IRawUdpDatagram` 旁路——SCTP
不该是控制通道的唯一选项。

附注（工程观察，非 RFC 结论）：中国部分移动运营商对 UDP/QUIC 长连接有 QoS 策略，
WebRTC 经典 ICE+DTLS+usrsctp 的 UDP 路径在该类网络中可能比 WebTransport（HTTP/3）
更稳。这一点影响 Selector 策略权重（§8 Open questions）。

---

## 4. 传输候选对比

| 候选 | 端到端 p50 | 端到端 p99 | 可靠性模型 | 浏览器兼容 | 加密 | 工程量 | 备注 |
|---|---|---|---|---|---|---|---|
| **WebRTC classic**（libjuice + wolfSSL + usrsctp） | 30–80 ms | 100–300 ms | 可靠 + 不可靠 DC；RTP/SRTP | ✅ Chrome / Firefox / Safari | DTLS-SRTP + ECDSA | 零（已在） | **默认**；SCTP cwnd 税 |
| **WebRTC over QUIC**（MoQ Transport，draft-ietf-moq-transport） | 15–40 ms | 40–100 ms | QUIC streams + datagrams（不可靠） | ⚠️ Chrome Origin Trial，无 Safari | QUIC-TLS 1.3 | 高（实现侧 + 协议仍在 IETF） | P3+ watch |
| **WebTransport**（HTTP/3 over QUIC） | 20–50 ms | 50–120 ms | 双向 stream + datagram（部分实现） | ✅ Chrome stable；Firefox 部分 | QUIC-TLS 1.3 | 中（lib 已有） | 浏览器侧最低门槛 |
| **Raw UDP + 自研 ARQ** | 5–15 ms | 20–50 ms | 配置（可靠 / 部分可靠 / 不可靠） | ❌ 无 | DTLS-PSK 或应用层 | 高（ARQ 自研） | 控制通道最快；NAT traversal 需 ICE 旁路或 STUN 配合 |
| **WebSocket over TLS** | 50–150 ms | 100–300 ms | 可靠 only | ✅ 所有 | TLS 1.3 | 零（lib 已有） | 控制平面 OK；无 datagram；无 NAT traversal |
| **MQTT over TLS** | 100–300 ms | — | QoS 0/1/2 | ❌ 需桥 | TLS | 低 | 远程设备管理常用；非低延迟 |
| **RTSP / RTP（裸流）** | 10–30 ms | 50–150 ms | RTP only | ❌ 无 | DTLS-SRTP / SRTP | 中 | 仅媒体；无 DC |

**结论**：没有"通吃"选项。**WebRTC classic 在浏览器互通上是唯一选择**；
**控制通道单独选 WebTransport / raw UDP+ARQ**是务实路径——这正是 §5 理想架构的
**"media 走 stack A，control 走 stack B"** 双栈模式。

---

## 5. 理想架构

### 5.1 三层抽象

```
┌──────────────────────────────────────────────────────────────────┐
│  Application                                                     │
│   声明需求：capability（"p99 < 80ms" / "browser compat" / ...） │
└──────────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌──────────────────────────────────────────────────────────────────┐
│  ITransportSelector                                              │
│    capability → ITransportSession 实例                           │
│    驱动来源：JSON Profile（ADR-010）+ ENV override（Phase 3）     │
└──────────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌──────────────────────────────────────────────────────────────────┐
│  ITransportSession（按 Profile 把"媒体 / 控制"分别落到 stack）  │
│   session.media_stack   : ITransportStack（默认 webrtc-classic） │
│   session.control_stack : ITransportStack（可选 raw-udp-arq）    │
└──────────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌──────────────────────────────────────────────────────────────────┐
│  ITransportStack（一次绑定整套后端，atomic 切换）                │
│   ├─ webrtc-classic  : libjuice + wolfSSL-DTLS + usrsctp        │
│   ├─ webrtc-quic     : quic-ICE + msquic-TLS + msquic-QUIC      │
│   └─ raw-udp-arq     : 裸 UDP + ARQ + DTLS-PSK                  │
└──────────────────────────────────────────────────────────────────┘
                            │
              ┌─────────────┼─────────────┐
              ▼             ▼             ▼
        IIceTransport  IDtlsSession  ISctpSocket  IRawUdpDatagram  IRtpSession
        (已有扩)       (Slice 4)    (Slice 5)    (Slice 6)        (已有)
        libjuice       wolfSSL      usrsctp      自研 ARQ         nim-rtp
        quic-ICE       openssl      msquic-QUIC
        pion           boringssl    raw-arq
```

### 5.2 五个 seam 接口（C++ 草案）

```cpp
// === 已存在，保留扩展 ===
namespace nimrtc::ice {
  class IICETransportFactory;     // §3.5 提到；id="ice"，默认 libjuice
  class IICETransport;
}

// === Slice 4：DTLS seam ===
namespace nimrtc::dtls {
  class IDtlsSession {
   public:
    virtual ~IDtlsSession() = default;
    virtual void set_role(plugins::Role) noexcept = 0;
    virtual void set_peer_fingerprint(span<const uint8_t>) noexcept = 0;
    virtual void start() noexcept = 0;
    virtual void pump() noexcept = 0;          // wolfSSL 没 pump 会卡；必须暴露
    virtual void on_handshake_complete(plugins::OnCompleteCb) noexcept = 0;
    // SRTP keying material 输出（保持当前 DtlsSessionWolfSSL 既有 surface）
    virtual plugins::Status export_srtp_key_material(
        span<uint8_t, 60> out) noexcept = 0;
  };
  class IDtlsSessionFactory {
   public:
    virtual ~IDtlsSessionFactory() = default;
    virtual std::unique_ptr<IDtlsSession> create(const DtlsConfig&) = 0;
    virtual std::string_view id() const noexcept = 0;   // "wolfssl" / "openssl" / ...
  };
  // 默认实现：DtlsSessionWolfSSL（封 wolfSSL，dtls_wolfssl_session.cpp 现有代码）
  // 注册入口：nimrtc::dtls::register_default_plugins()，id="wolfssl"
}

// === Slice 5：SCTP seam ===
// 已落地状态（Slice 5 subagent 报告，2026-09-14）：
//   - `plugins::byte` 类型不存在于 plugins/*.hpp，实际用 `plugins::BufferView`
//     （= core::ByteSpan = std::span<const std::uint8_t>）。
//   - `plugins::OnSctpRecvCb` 也不存在；Slice 5 把它在 `nimrtc::plugins` namespace
//     内**局部**定义在 `sctp_socket_iface.hpp` 顶部（避免 SCTP 类型漏到 plugins 共享层）。
//   - `SctpConfig` 字段在 §6.2 未明列，Slice 5 落地为 {label, max_num_streams=16, local_port=0}，
//     additive 设计——v0.11.0 可以继续追加 usrsctp 专用字段而不破坏现有调用点。
//   - 注册 id：当前 impl 为 `"stub"`（NOT `"usrsctp"`，DoD gate）；
//     v0.11.0 接入 usrsctp 时新增 `UsrsctpSocketFactory` 走 id="usrsctp"。
//   - `PluginRegistry::register_sctp_socket()` 当前**不存在**——
//     Slice 5 用 process-local cache 临时挂载（test-only accessor）；
//     真正的 registry hook 由 Slice 8（engine integration 顺手补）。
namespace nimrtc::sctp {
  // 由 Slice 5 局部定义在 sctp_socket_iface.hpp；§8 Open question #5 提议
  // v0.11.0 期间如 Slice 6 也要类似回调，统一提升到 plugins/base.hpp。
  // using OnSctpRecvCb = std::function<void(uint16_t stream, plugins::BufferView)>;

  struct SctpConfig {
    std::string label;                  // 诊断用
    std::uint16_t max_num_streams = 16; // 匹配 WebRTC DataChannel 默认
    std::uint16_t local_port = 0;       // 0 = backend 默认
    // v0.11.0 可继续追加：max_num_outbound_streams / dtls_srtp_key_hook / ...
  };

  class ISctpSocket {
   public:
    virtual ~ISctpSocket() = default;
    // 不可靠通道（QUIC datagram 风格；最大努力 + 重传上限）
    virtual void send_datagram(uint16_t stream, plugins::BufferView) noexcept = 0;
    // 可靠有序通道
    virtual void send_stream(uint16_t stream, plugins::BufferView) noexcept = 0;
    // 部分可靠 + TTL（§13 P3 部分可靠语义前置）
    virtual void send_partial_reliable(uint16_t stream, plugins::BufferView,
                                       std::chrono::milliseconds ttl) noexcept = 0;
    virtual void set_on_recv(plugins::OnSctpRecvCb) noexcept = 0;
  };
  class ISctpSocketFactory {
   public:
    virtual ~ISctpSocketFactory() = default;
    virtual std::unique_ptr<ISctpSocket> create(const SctpConfig&) = 0;
    virtual std::string_view id() const noexcept = 0;   // 当前 "stub"；v0.11.0 加 "usrsctp"
  };
  // 默认实现：SctpStubSocket（Slice 5 落地），所有 send 返回 `plugins::kErrNotReady`
  // （**loud failure**，非静默丢弃）。
  // v0.11.0 接入：UsrsctpSocket : ISctpSocket，id="usrsctp"。
  // 注册入口：nimrtc::sctp::register_default_plugins()
}

// === Slice 6：Raw UDP 旁路（高频控制用） ===
// 注意（Slice 6 subagent 实地落地 2026-09-14）：
//   - `plugins::OnDatagramCb` 也未在 plugins/*.hpp 定义。
//   - Slice 6 的实际做法：在 `raw_udp_datagram.hpp` 顶部把 `OnDatagramCb` 定义为
//     `std::function<void(plugins::BufferView)>`（**不带 Endpoint 参数**——
//     接收端由 ARQ 状态机隐含持有 peer_endpoint，callback 不重复传）。
//   - 同时把 `Endpoint` 用 `plugins::Addr` 做 alias（保持零 ABI 增量）。
//   - Slice 8 期间（与 4 个 registry hook 同步）一次性提升到 `plugins/base.hpp`——
//     避免每个新模块都在自己的头文件里 typedef 一次（§8 Open question #8）。
namespace nimrtc::raw_udp {
  // 实际定义（Slice 6 落地版本）：
  //   using Endpoint = plugins::Addr;
  //   using OnDatagramCb = std::function<void(plugins::BufferView)>;

  // 配置结构（Slice 6 实际落地）——
  // 显式列出以便未来扩展时不破坏 ABI：
  //   local_host, local_port=0, peer_endpoint, rx_window_packets=32,
  //   max_retransmits=5, rto_base_ms=50, rto_max_ms=1000,
  //   psk_identity_hint（PSK bytes 等 Slice 4 IDtlsSession PSK 扩展落地后接入）

  class IRawUdpDatagram {
   public:
    virtual ~IRawUdpDatagram() = default;
    virtual plugins::Status open() noexcept = 0;
    virtual void close() noexcept = 0;
    virtual plugins::Status send(plugins::Endpoint, plugins::BufferView) noexcept = 0;
    virtual void on_recv(plugins::OnDatagramCb) noexcept = 0;
    virtual int recv() noexcept = 0;   // 同步 drain；测试可不用后台线程
    virtual plugins::Endpoint local_endpoint() const noexcept = 0;
    virtual plugins::Endpoint remote_endpoint() const noexcept = 0;
    // stats：packets_sent/recv/retransmit/dropped/acks/nacks
  };
  // 默认实现：ArqRawUdp（selective-repeat ARQ skeleton + DTLS-PSK 占位）
  // 注册入口：nimrtc::raw_udp::register_default_plugins()，id="arq"
}

// === Slice 7：Stack 组合 + Selector ===
namespace nimrtc::transport {

  struct StackConfig {
    std::string ice_id   = "ice";
    std::string dtls_id  = "wolfssl";
    std::string sctp_id  = "usrsctp";
    std::string rtp_id   = "nim";
    std::string raw_id;     // 空 = 不启用 raw 旁路
  };

  class ITransportStack {
   public:
    virtual ~ITransportStack() = default;
    virtual IIceTransport&     ice() noexcept = 0;
    virtual IDtlsSession&      dtls() noexcept = 0;
    virtual IRtpSession&       rtp() noexcept = 0;
    virtual ISctpSocket&       sctp() noexcept = 0;
    virtual IRawUdpDatagram*   raw_control() noexcept = 0;   // 可能为空
    virtual void start() noexcept = 0;
    virtual void close() noexcept = 0;
  };
  class ITransportStackFactory {
   public:
    virtual ~ITransportStackFactory() = default;
    virtual std::unique_ptr<ITransportStack> create(const StackConfig&) = 0;
    virtual std::string_view id() const noexcept = 0;        // "webrtc-classic" / ...
  };

  // Session：按 Profile 把"媒体通道 / 控制通道"分别落到不同 stack。
  // 大多数场景 media_stack 必有；control_stack 在不需要专用控制通道时可为 null。
  // 当 Profile 同时声明 media 与 control 时，两个 stack 实例并存，
  // 互不干扰（各自的 ICE/DTLS/SCTP 状态独立）。
  struct TransportSession {
    std::unique_ptr<ITransportStack> media_stack;
    std::unique_ptr<ITransportStack> control_stack;     // 可能为 nullptr
    std::string media_stack_id;                        // "webrtc-classic" / ...
    std::string control_stack_id;                      // "raw-udp-arq" / "" 或 "webrtc-classic"
    void start() noexcept;
    void close() noexcept;
  };

  struct TransportRequirements {
    bool needs_browser_interop   = true;
    bool needs_high_freq_control = false;
    std::optional<int> max_p99_ms;
    std::optional<std::string> required_backend;             // "wolfssl" / "msquic" / ...
    bool prefer_quic             = false;                    // 软偏好（hint，非硬要求）
  };
  class ITransportSelector {
   public:
    virtual ~ITransportSelector() = default;
    virtual TransportSession select(
        const TransportRequirements&) const = 0;
  };
  // 默认实现：CapabilitySelector（按 §5.3 优先级表解析）
  // 注册入口：nimrtc::transport::register_default_selector()
}
```

### 5.3 Selector 默认策略（建议，后续 ADR 可调整）

```cpp
// CapabilitySelector::select() 返回 TransportSession（media_stack + control_stack）。
// 解析优先级（从高到低）：
//
// 1. needs_browser_interop == true
//    → media_stack 强制选 webrtc-classic（ICE + wolfSSL-DTLS + usrsctp + nim-rtp）
//    → 如果 needs_high_freq_control == true，
//         control_stack 选 raw-udp-arq（独立 stack，复用 ICE selected-pair）
//      否则 control_stack 为 nullptr（控制命令走 media_stack 的 SCTP）
//
// 2. needs_browser_interop == false && needs_high_freq_control == true
//    → media_stack    选 raw-udp-arq（裸 UDP+RTP，浏览器不需要时）
//    → control_stack  选 raw-udp-arq（独立 stack）
//
// 3. needs_browser_interop == false && prefer_quic == true
//    → media_stack 选 webrtc-quic（待 msquic / ngtcp2 集成，Slice 7.x 二期）
//    → control_stack 由 needs_high_freq_control 决定
//
// 4. 默认：media_stack 选 webrtc-classic；control_stack 由 needs_high_freq_control 决定
//
// 复用 ICE selected-pair（Slice 6 Open question #4）：
//   同一 Session 内 media_stack 和 control_stack 共享 ICE agent；
//   raw_control() 通过 ITransportStack::raw_control() 复用同一 selected-pair。
```
```

### 5.4 JSON Profile 段扩展

```jsonc
{
  "name": "robot-teleop",
  "version": "1.0",
  "transport": {
    "selector": "default",
    "media": {
      "stack":   "webrtc-classic",
      "max_p99_ms": 200
    },
    "control": {
      "stack":   "raw-udp-arq",
      "max_p99_ms": 80,
      "freq_hz":   100,
      "reliability": "reliable_ordered",
      "ttl_ms":     50
    }
  },
  "media":   { ... },
  "control": { ... }
}
```

ADR-010 §5 已经把 schema 文档定位在 `profiles/schema/profile-v1.0.json`；
Slice 7 在 v1.0 schema 上**追加** `transport` 段（不破坏向后兼容——ADR-010 §4 说
additive change 用 PATCH bump）。

---

## 6. Slice 4–8 落地路径

> 编号从 4 开始，避开已有 Slice 1（PAL seam）/ Slice 2（registration table）/
> Slice 3（plugin id validation macro）——参见 §9 术语校正。

| Slice | 标题 | LOC 估算 | 入口 PR | 依赖 | 风险 |
|---|---|---|---|---|---|
| **Slice 4** | DTLS seam（`IDtlsSession` + `IDtlsSessionFactory`） | ~180 | `refactor/dtls-seam` | 无 | 中（封 wolfSSL 的 surface 要保持稳定；engine.cpp 的 dtls 调用要同步改） |
| **Slice 5** | SCTP seam（`ISctpSocket` + usrsctp default impl + factory） | ~220 | `refactor/sctp-seam` | v0.11.0 usrsctp 集成 | 中（usrsctp 跨平台构建是 v0.11.0 已知风险行，见 `v0.11-preview.md` §5） |
| **Slice 6** | Raw UDP bypass（`IRawUdpDatagram` + ARQ default impl） | ~150 | `feat/raw-udp-arq` | 无 | 低（独立路径，不动现有 ICE/DTLS） |
| **Slice 7** | Stack + Selector + Profile `transport` 段 | ~280 | `feat/transport-stack-and-selector` | Slice 4/5/6 接口已合并 | 中（Selector 策略会成为 review 焦点；Profile schema 变更要走 ADR-010 additive 流程） |
| **Slice 8** | Engine integration（engine.cpp 接受 `ITransportStack*`） | ~150 | `refactor/engine-accepts-stack` | Slice 4–7 全部 merge | 中（行为不变性必须靠 `tests/test_engine_plugin_loading` + e2e Case D 守住） |

**总投入约 980 LOC**——比 PAL Slice 1（~50 LOC）大一个数量级，但**所有 slice 仍满足
"≤ 200 LOC 的接口/抽象增量 + ≤ 200 LOC 的 default 实现" 节奏**（Slice 5/7 略超，
需要在 PR review 时显式分拆实现 vs. seam）。

### 6.1 Slice 4 详细 DoD（其余 slice 见 §6.2–6.5）

- 新增 `src/dtls/include/nimrtc/dtls/dtls_session_iface.hpp`（`IDtlsSession`）
- 新增 `src/dtls/include/nimrtc/dtls/dtls_session_factory.hpp`（`IDtlsSessionFactory`）
- 把 `DtlsSessionWolfSSL` 重命名为封装类，继承 `IDtlsSession`
- 新增 `src/dtls/src/dtls_wolfssl_factory.cpp`，id="wolfssl"
- 新增 `nimrtc::dtls::register_default_plugins()`（沿用 ice/rtp 同模式）
- 修改 `src/core/src/pal_default_registrars.cpp`（Slice 2 落地后）添加 `&nimrtc::dtls::register_default_plugins`
- `engine.cpp` 中所有 `new DtlsSessionWolfSSL(...)` 改为 `dtls_factory.create(config)`
- 回归测试：`tests/test_dtls_chrome_interop` + e2e Case D 不退步
- **DoD gate**：`tests/test_engine_plugin_loading` 7/7 subtests 仍绿（PAL Slice 1 已经把这个测试立稳）

### 6.2 Slice 5 详细 DoD

- 新增 `src/sctp/include/nimrtc/sctp/sctp_socket_iface.hpp`（`ISctpSocket` 接口）
- 新增 `src/sctp/include/nimrtc/sctp/sctp_socket_factory.hpp`
- **重要修正（Slice 5 subagent 实地调研）**：
  `src/modules/datachannel/` 实质是 **greenfield**——`datachannel.cpp` 仅 16 行
  no-op `register_default_plugins`，`datachannel.hpp` 是 25 行空 shell，
  唯一具体的 `IDataChannel` 实现是 `tests/test_datachannel.cpp::StubDataChannel`
  （仅测试可见）。所以 §5.2 里"v0.11.0 计划**硬编码** usrsctp 集成下沉为 `UsrsctpSocket`"
  的说法与现实有出入——**没有可以"下沉"的工作**，v0.11.0 是从零落地。
- Slice 5 实际落地：默认 impl 是 `SctpStubSocket`（id="stub"），所有 send 返回
  `plugins::kErrNotReady`（loud failure，非静默丢包）。
- `SctpConfig` 实际字段：`label`、`max_num_streams=16`、`local_port=0`——
  v0.11.0 可继续追加（additive）。
- `register_default_plugins()` 用 Meyers 单例 latch + process-local cache 挂载
  （临时绕开缺失的 `PluginRegistry::register_sctp_socket()` hook；详见 §8 Open question #5 和 §12）。
- 集成测试：`tests/test_sctp_factory.cpp` 10/10 通过；既有 `test_datachannel` 15/15
  无回归（Slice 5 完全没碰 `src/modules/datachannel/`）。
- v0.11.0 follow-ups（out of Slice 5 scope）：
  1. `UsrsctpSocket : ISctpSocket` + `UsrsctpSocketFactory`（id="usrsctp"）
  2. 在 `src/core/include/nimrtc/core/registry.hpp` 加 `register_sctp_socket()` hook
  3. `SctpConfig` 扩展 usrsctp 专用字段
  4. engine.cpp 调用 `register_default_plugins()`（Slice 8 顺手）
  5. `src/modules/datachannel/` 改为封装 `ISctpSocket` 的高层 `IDataChannel` per-channel QoS 包装
     （`IDataChannel` 与 `ISctpSocket` 是**不同抽象层级**的并存：channel 级 vs SCTP association 级）

### 6.3 Slice 6 详细 DoD

- 新增 `src/raw_udp/include/nimrtc/raw_udp/raw_udp_datagram.hpp`（`IRawUdpDatagram` 接口，独立模块路径以避免与 Slice 7 的 `src/transport/` 冲突）
- 新增 `src/raw_udp/src/arq_raw_udp.cpp`（selective-repeat ARQ + DTLS-PSK stub）
- 新增 `src/raw_udp/src/arq_raw_udp_factory.cpp`，id="arq"
- 注册入口：`nimrtc::raw_udp::register_default_plugins()`
- 独立 benchmark：`tests/test_raw_udp_arq.cpp` 至少包含 100 Hz 模拟 + p99 报告

### 6.4 Slice 7 详细 DoD

- 新增 `src/transport/include/nimrtc/transport/transport_stack.hpp`
  （`ITransportStack` + `ITransportStackFactory` + `TransportSession`）
- 新增 `src/transport/include/nimrtc/transport/transport_selector.hpp`
  （`TransportRequirements` + `ITransportSelector` + 默认 `CapabilitySelector`）
- 默认 stack factory：`WebRtcClassicStackFactory`（id="webrtc-classic"）+ `RawUdpArqStackFactory`（id="raw-udp-arq"）
- JSON Profile schema 扩展：`profiles/schema/profile-v1.0.json` → `profile-v1.1.json`
  （additive 段：`transport` 顶层键；按 ADR-010 §4 PATCH bump，但"新顶层键"按 §8 Open question #3
  走 v1.1，schema 文档加 changelog）
- 注册入口：`nimrtc::transport::register_default_selector()`
- 在 `profiles/transport.json` 添加第一个真实样例（media=webrtc-classic, control=raw-udp-arq）
- 集成测试：`tests/test_transport_selector.cpp` 覆盖 §5.3 全部 4 条优先级规则

### 6.5 Slice 8 详细 DoD

- `engine.cpp` 接受 `ITransportStack*`（不再裸构造各 backend）
- ICE/RTP 的 PAL Slice 1 seam 不动（继续走 `pal::resolve_*`）
- DTLS/SCTP/raw 控制 走 `stack.dtls()` / `stack.sctp()` / `stack.raw_control()`
- **Slice 8 必须顺手补**（Slice 4/5/6 subagent 报告的 follow-up gap）：
  - `src/core/include/nimrtc/core/registry.hpp` 加 `register_sctp_socket(id, factory*)` hook
  - `src/core/include/nimrtc/core/registry.hpp` 加 `register_dtls_session(id, factory*)` hook（Slice 4）
  - `src/core/include/nimrtc/core/registry.hpp` 加 `register_raw_udp_datagram(id, factory*)` hook（Slice 6）
  - `src/core/include/nimrtc/core/registry.hpp` 加 `register_transport_stack(id, factory*)` hook（Slice 7）
  - 这四处 hook 是**平行新增**（与已有的 `register_ice_transport` / `register_rtp` 同模式）
- **Slice 8 顺手提升 callback 类型**：把 `OnSctpRecvCb` / `OnDatagramCb` 从 Slice 5/6
  的局部定义迁移到 `plugins/base.hpp`（如 §8 Open question #5 决议通过）。
- **行为不变性**靠现有回归测试守住（`test_engine_plugin_loading` 7/7 + e2e Case D +
  新增 `test_transport_stack_smoke` + 新增 `test_registry_hooks_register_dtls_sctp_raw_udp`）

---

## 7. 版本绑定（推荐）

| Slice | 推荐绑定 | 理由 |
|---|---|---|
| Slice 4 | **v0.11.0** | 与 v0.11.0 的 DTLS 路径同步；wolfSSL 现状已是 §13 P2 通路 |
| Slice 5 | **v0.11.0** | **替换** v0.11-preview §5 中"v0.11.0 硬编码 usrsctp"的口径；先 seam 后 default impl |
| Slice 6 | **v0.11.x patch**（v0.11.1 或 v0.11.2） | 独立功能；可放在 Patch 节奏 |
| Slice 7 | **v0.12.0** | 需要 Slice 4/5/6 合并稳定；Selector 策略需要单独 review |
| Slice 8 | **v0.12.0**（与 Slice 7 同期） | 必须等所有 seam 落地再做 engine 重构 |

**对 v0.11-preview.md §5 风险行的影响**：

| 原行 | 调整后 |
|---|---|
| "usrsctp 跨平台构建（macOS / aarch64）" | 保留，但实现走 Slice 5 的 `UsrsctpSocket` 而非硬编码集成 |
| "SFU 转发是否需要 nimrtc_bwe / nimrtc_jb 依赖" | 不变；与 Slice 4–8 正交 |
| "PCM tap 与 WebRTC APM 的接缝" | 不变 |

---

## 8. Open questions（需后续 ADR / RFC 收敛）

1. **ICE factory 扩展**——`IICETransportFactory` 当前接口对 QUIC candidate 不友好。
   是新增方法（`create_quic_ice()`）还是改接口本身？建议：新增方法，最小破坏。
2. **Selector 策略权重**——`prefer_quic` 是 hint 还是 requirement？建议 hint，
   避免在浏览器互通场景下被误用。
3. **Profile schema v1.0 vs v1.1**——`transport` 段是 additive，按 ADR-010 §4 是 PATCH；
   但 `transport` 段是**新顶层键**（不是新字段）——是否仍算 additive？
   建议：作为 v1.1（additive）处理，并在 `profiles/schema/profile-v1.1.json` 加 changelog。
4. **raw UDP 的 NAT traversal**——高频控制走 raw UDP 必须解决 NAT 问题。
   是复用 ICE selected-pair（§5.2 `IRawUdpDatagram::local_endpoint` 已开洞），
   还是单独走 STUN？建议：复用 ICE selected-pair（不引入第二条 NAT 路径）。
5. **国密后端适配点**——SM2/SM4 是 §13 P4 远期目标；DTLS seam 落地时是否要预留
   `sm_dtls` 工厂位？建议：Slice 4 阶段**只留 ID 字面量校验**，不预留具体工厂。
6. **WebTransport 何时纳入**——目前是 Slice 7.x 二期。是否 v0.12 就集成？
   建议：v0.12 不集成，v0.13 watch MoQ / WebTransport 标准化进度。
7. **usrsctp 在 v0.11.0 是否降级为可选后端**——ADR-009 Slice 5 的实现思路是
   "usrsctp 是 default impl，但不唯一"——是否在 v0.11.0 release notes 中明确这一点？
   建议：是；作为 v0.11.0 的"预留扩展位"列出。
8. **callback 类型提升**（**新增，Slice 5 subagent 实地发现**）——
   Slice 5 落地时发现 `plugins::OnSctpRecvCb` 不存在，临时在
   `sctp_socket_iface.hpp` 顶部**局部**定义在 `nimrtc::plugins` namespace。
   Slice 6 大概率需要类似 `OnDatagramCb`，届时是同样局部定义，还是提前提升到
   `plugins/base.hpp`？建议：Slice 8 期间（与 4 个 registry hook 同步）一次性
   提升到 `plugins/base.hpp`——避免每个新模块都在自己的头文件里 typedef 一次。
9. **`PluginRegistry::register_sctp_socket()` hook 的归属**（**新增，Slice 5 follow-up**）——
   Slice 5 用 process-local cache 临时挂载（test-only accessor）；真正的全局注册
   hook 必须有人加。建议：Slice 8 顺手补（与 `register_dtls_session` /
   `register_raw_udp_datagram` / `register_transport_stack` 一并）。
   风险：若 Slice 8 之前有任何模块需要跨进程查找 SCTP factory，process-local cache
   会成瓶颈——目前没有这种调用方，OK。
10. **Slice 5 与既有 `IDataChannel` 的关系**（**新增，Slice 5 实地发现**）——
    `src/modules/datachannel/` 的 `IDataChannel`（P1 占位）与 Slice 5 的 `ISctpSocket`
    是**不同抽象层级**的并存接口：`IDataChannel` = per-channel QoS 视图（一个 channel 一个 IDataChannel）；
    `ISctpSocket` = SCTP association 视图（一个 association 多路复用多个 stream）。
    v0.11.0 接入 usrsctp 时，`IDataChannel` 应改为薄封装 `ISctpSocket`——
    把 SCTP stream → DataChannel label 的映射放在 `IDataChannel` 层。
    建议：在 Slice 5 v0.11.0 follow-up #5 落实前，doc 不动；落实时把 §6.2 的
    "下沉"措辞替换为"封装"（封装是更准确的描述）。
11. **`src/sctp/` vs `src/modules/sctp/` 的新约定**（**新增，Slice 5 路径决策**）——
    Slice 5 把新模块放在 `src/sctp/`（与 §6.2 显式一致），但既有模块在 `src/modules/X/`。
    后续 Slice 6/7 也都按新约定放（`src/raw_udp/`、`src/transport/`），与既有 `src/modules/X/` 并存。
    风险：长期会有 `src/X/` 与 `src/modules/X/` 两套路径——是否在某次清理中统一？
    建议：v0.11.0 后单独开一个 cleanup PR（不在 Slice 4–8 scope），把 `src/modules/datachannel/`
    之外的"已 plugin 化的旧 module"挪到 `src/X/` 新约定；`src/modules/datachannel/` 由 v0.11.0
    follow-up #5 决定是挪还是封。

---

## 9. 术语校正（重要）

本会话前面回答里出现过 **"PAL Slice 3 = 传输替换"** 的说法。这是错的。

按 ADR-009 §Context 与 `docs/plan/pal-architecture.md` §4，PAL 三个 Slice 的实际范围是：

| Slice | 实际范围 | LOC | 绑定版本 |
|---|---|---|---|
| 1 | inline forwarders `pal::resolve_audio3a` / `resolve_codec` / `resolve_video_codec` | ~50 | v0.10.0 |
| 2 | `kDefaultRegistrars[]` 表替换内联 `register_all_default_plugins()` | ~80 | v0.10.x patch |
| 3 | `NIMRTC_PLUGIN_ID(name)` 编译期 ID 字面量校验宏 | ~30 | v0.10.x patch |

**全部聚焦 audio3a / codec / video_codec 的 plugin 解析**，§3.5 明文写
"Transport / ICE source/sink plugin categories ... PAL does not touch this in Phase 2"。

本文档因此把传输层的 seam 化命名为 **PAL Slice 4–8**（延续编号），不另起 "TAL" 缩写，
保持与现有 Slice 1/2/3 的命名风格一致。

---

## 10. References

- `docs/zh/architecture.md` §13（P1/P2/P3 行；§13.1 1.0 准入门槛）
- `docs/plan/pal-architecture.md`（§1.1 注册列表硬编码；§3.5 PAL 不动传输；§4 Slice 1/2/3 定义）
- `docs/plan/v0.11-preview.md`（§1 P2 DoD；§5 usrsctp 风险行；§6 schedule placeholder）
- `docs/plan/v0.10-plan.md`（§2.1 PAL-1/PAL-2/PAL-3 任务；§2.2 显式延后到 v0.11 的项）
- `docs/plan/dtls_chrome_interop_review.md`（DTLS wolfSSL 实现现状审查）
- `docs/plan/linux-x86_64-support.md`（平台矩阵现状）
- `docs/adr/ADR-009-pal-slice-1.md`（Slice 1 binding）
- `docs/adr/ADR-010-profile-json-format.md`（JSON Profile schema v1.0；§4 versioning；§6 loader API 留口）
- `src/modules/ice/include/nimrtc/ice/ice.hpp`（ICE factory surface 实参考）
- `src/modules/dtls/src/dtls_wolfssl_session.cpp`（DTLS wolfSSL 唯一实现）
- `src/modules/datachannel/`（P1 占位；Slice 5 subagent 实地调研发现是 greenfield，
  不是 §6.2 原本假设的"v0.11.0 硬编码 usrsctp 集成下沉"）
- `src/sctp/`（Slice 5 实际落地位置）
- `build/SLICE5_REPORT.md`（Slice 5 subagent 完整报告；build logs：`build/slice5_*.log`）

---

## 11. Review checklist（self-review 自查）

> 本节是写完后第一轮自查，下一轮由 BDFL/reviewer 在 PR 上确认。

- [x] §9 术语校正显式写出，且不掩盖会话历史错误
- [x] §2 现状审计每行均能追溯到具体仓库路径 / 文件 / ADR 段落
- [x] §3 延迟数字为"健康网络典型值"，不冒充 RFC 数据；明确标注非 RFC 结论（中国移动 UDP/QUIC QoS 行）
- [x] §4 候选对比表每行覆盖 latency / reliability / browser / crypto / effort 五个维度
- [x] §5 五个 seam 接口在 C++ 草案里**全部出现**，签名一致，且与现有 ICE factory 命名对齐
- [x] §6 每个 Slice 的 DoD 列出文件路径、接口、回归测试；明确 Slice 5 的"先 seam 后 default"避免双重改动
- [x] §7 版本绑定给出推荐 + 与 v0.11-preview §5 的差异表
- [x] §8 Open questions 不留暗坑——7 项全部写明建议方向
- [x] §10 References 全部使用仓库相对路径
- [x] 不与现有 ADR 冲突（与 ADR-009 Slice 1/2/3 边界互补；与 ADR-010 additive 流程一致）
- [x] 不违反 §16 开源基线（无商业措辞、无"生产级"声明）
- [x] 不与现有 `docs/zh/architecture.md` §13 P3 行冲突（自适应 JB、Goog-CC、TTL、cloudgame 维持原绑定）
- [ ] 待 BDFL review：Slice 7 的 LOC 280 是否拆 PR（建议拆 interface / 实现 / Profile 段三段）
- [ ] 待 BDFL review：v0.11.0 是否同步替换 usrsctp 硬编码为 Slice 5 seam（推荐：是）
- [ ] 待 BDFL review：usrsctp 在 v0.11.0 release notes 是否显式标注"default impl，not sole"
- [ ] 待 BDFL review：Profile schema `transport` 段作为新顶层键的版本号处理（建议 v1.1，与 ADR-010 §4 "新顶层键" 解释一致）
- [ ] 待 BDFL review：§5.3 引入的 `TransportSession`（media_stack + control_stack 双 stack 实例共存）模型是否符合引擎持有方式预期
- [ ] 待 BDFL review：§8 Open question #4 "raw UDP 复用 ICE selected-pair" 对 ICE factory 的接口影响（是否在 Slice 4/5 之前需要先冻结 `IICETransportFactory` 接口扩展点）
- [ ] 已修复：Slice 6 路径改为 `src/raw_udp/` 与 Slice 7 的 `src/transport/` 分离——确保 4 个 subagent 并行 dispatch 不撞文件树
- [x] **Slice 5 落地修正**（subagent 完成通知 2026-09-14）：§5.2 Slice 5 块已修正
  `span<const byte>` → `plugins::BufferView`、增 `OnSctpRecvCb` 局部定义说明、
  增 `SctpConfig` 字段清单、修正 factory id 表述为"当前 stub / v0.11.0 usrsctp"。
- [x] **Slice 5 落地修正**：§6.2 已澄清 datachannel 是 **greenfield** 而非"下沉"；
  v0.11.0 follow-ups 列出 5 项（含 `register_sctp_socket` hook）。
- [x] **Slice 5 落地修正**：§5.2 Slice 6 块已 flag `OnDatagramCb` 同样需要局部定义，
  避免 Slice 6 subagent 漏掉。
- [x] **Slice 5 落地修正**：§6.5 Slice 8 DoD 已增 4 个 `PluginRegistry` hook 作为
  强制交付项（与已有的 `register_ice_transport` / `register_rtp` 平级）。
- [x] **Slice 5 落地修正**：§8 Open questions 增 4 项（#8 callback 提升 / #9 registry hook
  归属 / #10 IDataChannel vs ISctpSocket 抽象层级 / #11 新旧路径约定）。
- [ ] 待 BDFL review：§8 Open question #10——`IDataChannel` 是否在 v0.11.0 改为
  封装 `ISctpSocket`（推荐：是，但需要单独的"封装 vs 改写"决策 ADR）
- [ ] 待 BDFL review：§8 Open question #11——`src/X/` 新约定与 `src/modules/X/` 旧约定
  是否在 v0.11.0 后做统一清理 PR（推荐：v0.11.0 release 后单独 cleanup PR）

---

## 12. Slice 5 post-mortem（2026-09-14 落地后）

> 本节是 Slice 5 subagent 完成后的实地发现汇总。后续 Slice 4/6/7 落地后，
> 应在本节追加各自的 post-mortem。

### 12.1 文档修正（已完成，本 commit 内）

- §5.2 Slice 5 块：`span<const byte>` → `plugins::BufferView`；增 `OnSctpRecvCb` 局部定义
  说明（在 `sctp_socket_iface.hpp` 顶部，`nimrtc::plugins` namespace）；增 `SctpConfig`
  字段 `{label, max_num_streams=16, local_port=0}`；factory id 表述从 "usrsctp" 改为
  "当前 stub / v0.11.0 usrsctp"。
- §5.2 Slice 6 块：同样切到 `plugins::BufferView`，并显式 flag `OnDatagramCb` 需局部定义
  （避免 Slice 6 subagent 重复犯同样错误）。
- §6.2：澄清 datachannel 是 **greenfield**（16 行 no-op + 25 行空 shell），
  列出 5 项 v0.11.0 follow-ups（含 `register_sctp_socket` hook）。
- §6.5：Slice 8 DoD 增 4 个 `PluginRegistry` hook 与 callback 提升作为强制交付项。
- §8：增 4 项 Open questions（#8 callback 提升 / #9 registry hook 归属 / #10 抽象层级 /
  #11 新旧路径约定）。
- §11：标记 5 项 Slice 5 落地修正 + 2 项新增 BDFL 待决项。

### 12.2 Slice 5 实际产出 vs 计划

| 项 | 计划 | 实际 |
|---|---|---|
| LOC | ~220 | 807（含 ~550 文档注释 + 10 GTest）；非注释非测试代码 ~250，on-target |
| 注册 id | "usrsctp"（计划） | "stub"（落地，DoD gate 强制）；v0.11.0 才接 "usrsctp" |
| 默认 impl | UsrsctpSocket（计划） | SctpStubSocket：所有 send 返回 `kErrNotReady`（loud failure） |
| 行为 | "下沉" usrsctp | 实为 greenfield（无可下沉） |
| 注册入口 | `register_default_plugins()` | ✓ + process-local cache（绕开缺失的 registry hook） |
| 测试 | （计划未明列） | 10/10 PASSED；test_datachannel 15/15 无回归 |
| build artifacts | （未明列） | `nimrtc_sctp.lib` 1.7 MB；`test_sctp_factory.exe` |
| 全量构建 | （未明列） | `cmake -B build -S .` OK；root CMakeLists.txt +1 行 |

### 12.3 Slice 5 报告全文

完整 subagent 报告在 `build/SLICE5_REPORT.md`（subagent 自行落地）。
build logs：`build/slice5_configure.log` / `build/slice5_build.log` / `build/slice5_test_build2.log`。
建议 PR review 时把 `build/SLICE5_REPORT.md` 链接到 PR description。

### 12.4 Slice 5 给后续 Slice 的关键提示

- **Slice 4（DTLS）**：同样预期会发现 `plugins::OnDtlsHandshakeCb` 类缺失；
  同模式局部定义 + Slice 8 期间统一提升到 `plugins/base.hpp`（§8 #8）。
- **Slice 6（Raw UDP）**：同 §5.2 Slice 6 块的 flag——`OnDatagramCb` 局部定义；
  DTLS-PSK 接缝放在 `IRawUdpDatagram` 与未来的 `IDtlsSession` 之间，留 TODO。
- **Slice 7（Stack/Selector）**：`ITransportStack::sctp()` 返回 `ISctpSocket&`（不指针，
  因为 Slice 5 stub impl 是 always-on）；`raw_control()` 返回 `IRawUdpDatagram*`
  （nullable，因为 Slice 5/Slice 6 是否启用是 Selector 决策）；
  forward-declare 时**用 `nimrtc::sctp::ISctpSocket`** 而不是 `plugins::ISctpSocket`。
- **Slice 8（Engine integration）**：4 个 registry hook（`register_sctp_socket` /
  `register_dtls_session` / `register_raw_udp_datagram` / `register_transport_stack`）
  + `OnSctpRecvCb` / `OnDatagramCb` 提升到 `plugins/base.hpp`，是 Slice 8 的强制交付项
  （不能 deferred 到 v0.12.x patch，因为 `register_default_plugins()` 已经公开，
  没有 registry hook 就只能靠 process-local cache，跨进程查询会失效）。

---

## 12.5 Slice 7.5 post-mortem（2026-09-18）

> `WebRtcClassicStack`（id="webrtc-classic"）+ factory 注册。
> `default_transport_stack_factory.cpp` 仍保留（shell，id="default"），向后兼容。

### 12.5.1 落地内容

| 文件 | 作用 |
|---|---|
| `src/transport/src/webrtc_classic_stack.cpp` | `WebRtcClassicStack` + `WebRtcClassicStackFactory` |
| `src/transport/src/transport_plugin.cpp` | `do_register_default_plugins()` 同时注册 "default"（shell）+ "webrtc-classic"（真实） |
| `src/transport/CMakeLists.txt` | `+ webrtc_classic_stack.cpp` |
| `tests/test_engine_plugin_loading.cpp` | 新增 3 个 subtest：`slice75_webrtc_classic_factory_registered` / `slice75_webrtc_classic_stack_create_and_lifecycle` / `slice75_idempotent_register_does_not_duplicate` |

### 12.5.2 设计决策记录

**Q：engine 是否直接使用 `ITransportStack*`？**
A：**暂不**。Engine 已在 `engine.cpp` 中通过 registry 直接管理 ICE/DTLS/RTP/SCTP，直接使用 stack 会引入大量改动。Slice 7.5 的价值在于：
  - 工厂注册路径打通（`get_transport_stack("webrtc-classic")` 现在返回真实工厂）
  - 生命周期管理已实现（`start()` / `close()` / `tick()`）
  - SCTP stub 的 wiring 路径已验证
未来 slice 可让 engine 接受 `ITransportStack*`，而不改 PAL 架构。

**Q：DTLS `close()` 为什么在 stack 的 `close()` 中跳过？**
A：`IDtlsSession`（seam 接口）没有 `close()` 方法；`DtlsSessionWolfSSL`（concrete）有。
Engine 仍通过 concrete 类型直接调用 `dtls->close()`。Stack 跳过 `dtls_->close()` 不影响 engine 的生命周期管理。

**Q：SCTP 接口为什么没有 `open()` / `close()`？**
A：`ISctpSocket` seam 接口本身没有这两个方法（stub 是 always-on）。v0.11.0 的 `UsrsctpSocket` 会扩展接口添加生命周期。

**Q：`demux_last_packet()` 为什么不 wire？**
A：引擎已直接管理 ICE recv + DTLS feed/drain 路径。在 IICETransport 提供 `last_recv_packet()` 之前，保持 engine 的 demux 不变是最安全的。

### 12.5.3 测试结果

- `tests/test_engine_plugin_loading.cpp`：391/391 全通过（含 3 个新 Slice 7.5 subtest）
- `ctest -C Debug`：391/391 全通过
- `nimrtc_engine.lib`：编译干净，无新增 warnings

### 12.5.4 下一步（deferred to future slices）

- Engine 接受 `ITransportStack*`（需 engine.cpp 改动较大）
- SCTP 生命周期扩展到 seam（`ISctpSocket::open()` / `close()`）
- IICETransport 提供 `last_recv_packet()` 以支持 stack 内 demux
- `RawUdpArqStackFactory`（id="raw-udp-arq"）——控制栈实现


