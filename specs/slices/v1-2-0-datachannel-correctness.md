# Slice: v1.2.0-datachannel-correctness

## 目的

修复 SCTP DataChannel（legacy 后端，工厂 id `"sctp"`）在 **seam 层**的
正确性缺陷。

当前状态下，`NimRTCEngine::create_data_channel()` 返回的通道是一个**空壳**：
它从未被打开（`send()` 恒返回 `kErrNotReady`），入站消息若能到达也会被
**投递两次**，DCEP 线上格式**不携带 unordered / priority 语义**，通道状态机
在握手完成之前就宣称 `"open"`。

本切片只修 seam 层的正确性。**SCTP-over-DTLS 传输层**（AF_CONN 后端、
association 建立、多通道用 stream id 复用）不在本切片范围内 —— 见
§「明确不做」。

## 缺陷清单

| 编号 | 位置 | 现象 | 影响 |
|---|---|---|---|
| DC-1 | `src/engine/src/engine.cpp` `create_data_channel()` 的 `"sctp"` 分支 | 只调用了 `IDataChannel::open(cfg)`，从未调用 `IPlugin::open()` | `SctpDataChannel::opened_` 恒为 `false` → `route_send()` 恒返回 `kErrNotReady` → 每次 `send()` 都触发 `on_state("error")`；同时 recv 回调从未接线 → 入站消息永远到不了应用 |
| DC-2 | `src/sctp/src/usrsctp_socket.cpp` `s_instance_recv_cb()` + `src/modules/datachannel/src/sctp_data_channel.cpp` `open()` | socket 把 PPID 回调与 legacy 回调**同时**触发（fan-out），而通道又同时注册了这两个回调 | 每条 string/binary 用户消息被 `on_msg_` **投递两次**；一条 DCEP OPEN 被处理两次 → 回两个 ACK、报两次 `"open"` |
| DC-3 | `src/sctp/src/dcep.cpp` `dcep_encode_open()` / `dcep_decode_open()` | DCEP Channel Type 只有 `0x00 / 0x01 / 0x02`；`kUnreliable` 被编码成 `0x00`（= reliable **in-order**）；`cfg.ordered` 从不上线；解码永远 `ordered = true` | 不可靠 / 无序通道在对端看来是「可靠有序」，对端按错误 QoS 收发。RFC 8832 §5.1 / §8.2.2 的 6 个取值（`0x00/0x80/0x01/0x81/0x02/0x82`，高位 `0x80` = unordered）没有实现 |
| DC-4 | `src/sctp/src/dcep.cpp` `dcep_decode_open()` | Priority 字段（byte 2..3）被读取后丢弃，`DcepOpenParams` 里没有对应字段 | 对端以 priority 16 开的通道，在本端退化成默认 128 |
| DC-5 | `src/modules/datachannel/src/sctp_data_channel.cpp` `send_dcep_open()` | 该函数**没有任何调用者** | 对端永远收不到 `DATA_CHANNEL_OPEN` → 不知道 label / QoS → 永远不回 ACK → 通道握手无法完成 |
| DC-6 | `src/modules/datachannel/src/sctp_data_channel.cpp` `open()` | 无条件依次上报 `"connecting"` → `"open"` | 状态机撒谎：此刻既没有 association，也没收到 DCEP ACK |
| DC-7 | `src/modules/datachannel/src/sctp_data_channel.cpp` `route_send()` | 握手完成前就按通道语义发 unordered 消息 | 违反 RFC 8832 §6：「在收到 DATA_CHANNEL_ACK 或该通道上任何其它消息之前，属于该通道的用户数据 MUST 有序发送，无论该通道本身是否有序」 |
| DC-8 | `src/modules/datachannel/src/sctp_data_channel.cpp` `on_sctp_recv_with_ppid()` | 把入站 stream id 硬编码成 `0` 再转发 | 入站消息的 stream 归属信息丢失 |
| DC-9 | `src/sctp/include/nimrtc/sctp/dcep.hpp` `dcep_ppid_for_config()` | 注释写「string=51 default」，实现恒返回 `kPpidBinary`（53） | 文档与实现不符 |
| DC-10 | `src/sctp/src/dcep.cpp` `dcep_encode_open()` | `reliability_param` 声明为 `uint16_t`，却按 `>> 24 / >> 16` 的 u32 链编码 | MSVC C4333「右移位数过大，数据将丢失」告警；当前取值恰好 ≤ 0xFFFF 所以线上字节碰巧正确，但依赖整型提升，是隐患 |

## 范围

- `src/sctp/include/nimrtc/sctp/dcep.hpp` — 新增 3 个 unordered Channel Type
  常量；`DcepOpenParams` 新增 `priority` 字段。
- `src/sctp/src/dcep.cpp` — Channel Type 编码 / 解码覆盖 RFC 8832 §8.2.2 的
  6 个取值；Priority 往返；Reliability Parameter 按 u32 编码。
- `src/sctp/src/usrsctp_socket.cpp` — 入站回调改为 **PPID 优先**（注册了
  PPID 回调就只投 PPID，否则投 legacy），消除重复投递。
- `src/modules/datachannel/include/nimrtc/datachannel/sctp_data_channel.hpp` —
  通道状态字段与文档同步。
- `src/modules/datachannel/src/sctp_data_channel.cpp` — 状态机、DCEP OPEN
  发送、握手前强制有序、stream id 透传。
- `src/engine/src/engine.cpp` — `create_data_channel()` 补上 `IPlugin::open()`
  调用并检查返回值。
- `src/modules/datachannel/tests/test_sctp_data_channel.cpp` — 新增用例。
- `src/sctp/tests/` — 新增 DCEP 编解码用例。
- `CHANGELOG.md` — `[Unreleased]` 记录行为变更。

## 设计

### DC-1 引擎必须真正打开通道

`create_data_channel()` 的 `"sctp"` 分支在 `ch->open(cfg)` 之后补上
`ch->open()`（`IPlugin::open()`）。返回码非 `kOk` 时通过 `on_error_` 上报
`core::kEngineInternal` 并返回 `nullptr` —— 调用方据此知道通道不可用，而不是
拿到一个永远 `send()` 失败的空壳。

WebTransport 分支本来就已经同时调用了两个 `open()`，本切片不动它。

### DC-2 入站只有一条投递路径

`UsrsctpSocket::s_instance_recv_cb()` 的 fan-out 改成**优先级**语义：

```
if (on_recv_ppid_)  → 只投 PPID 回调
else if (on_recv_)  → 投 legacy 回调
```

- 只注册了 legacy 回调的后端（AF_INET 回环测试、只实现 `set_on_recv` 的
  mock）行为不变。
- `SctpDataChannel` 仍然同时注册两个回调 —— legacy 是「不支持 PPID 的后端」
  的兜底路径 —— 但在后端支持 PPID 时只有 PPID 路径会触发，因此不再重复。

### DC-3 / DC-4 DCEP 线上格式对齐 RFC 8832

Channel Type 取值补齐为 IANA 注册表（RFC 8832 §8.2.2）的 6 个：

| 常量 | 值 | 含义 |
|---|---|---|
| `kDcepReliable` | `0x00` | reliable, in-order |
| `kDcepPartialReliableRexmit` | `0x01` | partial reliable (rexmit), in-order |
| `kDcepPartialReliableTimed` | `0x02` | partial reliable (timed), in-order |
| `kDcepReliableUnordered` | `0x80` | reliable, unordered |
| `kDcepPartialReliableRexmitUnordered` | `0x81` | partial reliable (rexmit), unordered |
| `kDcepPartialReliableTimedUnordered` | `0x82` | partial reliable (timed), unordered |

- **编码**：先按 `cfg.reliability` 选基值，再按 `!cfg.ordered` 置高位 `0x80`。
  `kUnreliable` 映射到 `0x80`（NimRTC 的「不可靠」在发送侧就是
  `SCTP_UNORDERED` 标志，association 本身仍然可靠重传，所以线上语义是
  *reliable unordered*，不是 partial reliable）。
- **解码**：6 个取值全覆盖，同时还原 `reliability` 与 `ordered`。
- **Priority**：解码读 byte 2..3（u16 BE）写进 `DcepOpenParams::priority`；
  入站 OPEN 时写回 `cfg_.priority`。编码侧 `cfg.priority`（uint8）落在低
  字节，高字节为 0 —— 与现状一致。
- **Reliability Parameter**：编码变量改成 `uint32_t`，消除 C4333。
  `DATA_CHANNEL_RELIABLE*` 的可靠性参数恒为 0（RFC 8832 §5.1 要求）。

### DC-5 / DC-6 通道状态机

`SctpDataChannel::open()` 的职责收窄为「接线 + 宣告连接中」：

```
open()  →  接线 recv 回调
        →  opened_ = true
        →  上报 "connecting"
        →  若已知 cfg，自动发一次 DATA_CHANNEL_OPEN（幂等）
```

`"open"` 改为**握手完成**时上报，两条路径：

1. 收到对端的 `DATA_CHANNEL_ACK`（对端确认了我们的 OPEN）。
2. 收到对端的 `DATA_CHANNEL_OPEN`（我们回 ACK，通道建立）。

`close()` 上报 `"closed"`。

**行为变更**：以前 `open()` 之后立刻就是 `"open"`，现在先是 `"connecting"`。
在 SCTP-over-DTLS 传输层落地之前（association 根本建立不起来），通道会停在
`"connecting"` —— 这是诚实的状态：旧行为报告的 `"open"` 是假的。

### DC-7 握手完成前强制有序

`route_send()` 在握手完成（收到 ACK 或对端 OPEN）之前，一律走
`send_stream_with_ppid()`（ordered + reliable），无论通道本身是否有序
（RFC 8832 §6）。握手完成后才按通道语义选 unordered / partial-reliable。

注意 DCEP 报文本身按 RFC 8832 §6 要求「MUST 使用有序投递与可靠传输」，
已经是 ordered。

### DC-8 stream id 透传

`on_sctp_recv_with_ppid()` 把真实的入站 stream id 传给 `on_sctp_recv()`，
不再硬编码 0。

### DC-9 / DC-10

`dcep_ppid_for_config()` 的注释与实现统一为 **binary（53）** —— NimRTC 的
DataChannel 只传二进制负载，不使用 UTF-8 string 语义。

## 验收标准

1. `create_data_channel()`（`datachannel_backend = "sctp"`）返回的通道
   `is_opened() == true`，且 `send()` 不再因为 `kErrNotReady` 而失败
   —— mock 场景下能观察到 `ISctpSocket` 上的发送调用。
2. 一条入站 string/binary 消息**只触发一次** `on_msg` 回调；一条入站 DCEP
   OPEN **只回一个 ACK**。
3. DCEP 编解码往返一致：
   - 6 个 Channel Type 全部可编码 / 可解码，`reliability` 与 `ordered`
     往返不丢；
   - `priority` 往返不丢；
   - reliable 通道的 Reliability Parameter 恒为 0。
4. `SctpDataChannel::open()` 上报 `"connecting"`；收到 DCEP ACK 或对端 DCEP
   OPEN 之后才上报 `"open"`；`close()` 上报 `"closed"`。
5. 握手完成前，unordered 通道的用户数据也走 ordered 发送路径；握手完成后
   恢复通道自身的语义。
6. `DATA_CHANNEL_OPEN` 在通道打开时被发出一次（重复调用不重复发）。
7. `dcep.cpp` 不再产生 MSVC C4333 告警。
8. 全量 `ctest` 通过；`test_api_classification` 的公共头基线不变（109，
   本切片不新增公共头）。
9. `AGENTS.md` L1–L7 硬约束保持成立。

## 风险

| 风险 | 应对 |
|---|---|
| `"open"` 状态上报时机变了，下游按「open 后立刻可用」假设写的代码会看到更长的 `"connecting"` 窗口 | 变更写进 `CHANGELOG.md` 的 `[Unreleased]`；在传输层落地之前 `"open"` 本来就是假的 |
| PPID 优先投递改变了 `UsrsctpSocket` 的回调语义 | 只注册 legacy 回调的调用方（AF_INET 回环测试、mock）行为不变；`UsrsctpSocket` 的头注释同步更新 |
| DCEP Channel Type 换成 RFC 8832 的取值后，与旧版 NimRTC 对端互通行为变化 | 旧版 NimRTC 只发 `0x00/0x01/0x02`，新版对这三个取值的解码结果与旧版一致；新增的三个取值旧版无法识别（旧版缺陷，不再兼容） |

## 明确不做

以下项属于 SCTP-over-DTLS 传输层切片，不在本切片范围内：

- `UsrsctpSocket` 的 AF_CONN 后端（`s_packet_output` 目前是空桩）。
- association 的建立（`listen()` / `connect()` 由引擎调用）与 DTLS 密钥
  注入（`ISctpSocket::set_dtls_keys()`）。
- **一个 peer connection 一条 association、多通道用 stream id 复用**
  （RFC 8831 §6）。当前 `SctpDataChannel` 是**每个通道自建一个 usrsctp
  socket**（= 一条 association），因此本切片不实现 stream id 分配与奇偶
  规则（RFC 8832 §6）—— 一条 association 上只有一个通道时，stream 0 即合规。
- 对端发起的通道（peer-initiated channel）的 accept 路径。
- Chrome / libwebrtc 互操作验收。
