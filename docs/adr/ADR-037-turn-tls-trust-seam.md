# ADR-037: TURN/TLS 信任策略的接缝 —— 旋钮在 ICE 层，本片把它透到公开面

| | |
|---|---|
| Number | 037 |
| Status | **Proposed** |
| Date | 2026-11-25 |
| Phase | v1.4（`v1-turn-tls-trust`） |
| Bound to | v1.4.0 |
| Supersedes | — |
| Related | `docs/adr/ADR-027-turn-server.md`（N5 参考中继：TLS-only + 自签证书）；`docs/adr/ADR-032-turn-credentials-provider-seam.md`（N7：凭据接缝）；`docs/adr/ADR-025-sdp-rtp-not-pluggable.md`（L4 接缝纪律）；`docs/adr/ADR-015-api-stability-policy.md`（公开面稳定性）；`specs/slices/v1-turn-tls-trust.md`（本片） |

---

## 1. 背景

N6（SDK 样例）搭双进程验收腿时撞出一个缺口：**宿主没法信任 TURN/TLS 中继的证书 —— 公开面上没有那个旋钮。**

核实结论（2026-11-25，写规格时的树状态）：

- `src/modules/ice/` **已经有** `IceConfig::turn_tls_verify_peer`（默认 `true`）与
  `IceConfig::turn_tls_ca_file`（`ice.hpp:257` / `ice.hpp:274`），
  `src/modules/ice/src/turn_tls.cpp:123-134` 也确实按它们决定
  `WOLFSSL_VERIFY_PEER` / `VERIFY_NONE`。
- **引擎公开面没有透出来**：`TurnCredentials` 只有 `tls_server_name`，**没有**
  `turn_tls_verify_peer` / `turn_tls_ca_file`。
- 于是宿主**无法表达**「我信任这个自签证书」或「这个中继我不要校验」。

后果是**必然**失败，不是「可能失败」：

- N5 参考中继是 **TLS-only + 自签证书**（ADR-027 Decision 4）。
- ice 默认 `verify_peer = true` + **空信任库** → 加载信任锚失败 → **必然拒绝**。
- 没有 `ca_file` → TLS 中继这条腿今天**只能靠改引擎源码**才能跑通 —— 而那正是
  接缝纪律禁止的（宿主不许绕过接缝）。

## 2. 决策

### 2.1 决策一：两个旋钮进 `TurnCredentials`，**跟着凭据走**，不跟着 `EngineConfig` 走

```cpp
// src/engine/include/nimrtc/engine/engine.hpp
struct NIMRTC_API_STABLE TurnCredentials {
    /* ... 既有六个字段不变 ... */
    bool turn_tls_verify_peer = true;      // false = 显式放弃校验对端证书
    std::string turn_tls_ca_file;          // PEM 信任锚路径；空 = 不加载任何锚
};
```

**为什么跟着凭据走**：凭据是**每次拉取**都可能换的（N7），信任策略跟着凭据走才
自洽 ——「换了中继就换信任锚」是一句话。若放在 `EngineConfig` 上，则「凭据换了但
信任策略没换」会成为一种可能出现的、说不清的状态。

### 2.2 决策二：插件接口**必须加字段**，这是接口变更 —— 用**新增的 7 参数 virtual 重载**

`plugins::IICETransport::add_turn_server_with_transport()`（`ice_transport.hpp:361`）
**只有** `host` / `port` / `username` / `password` / `transport` 五个参数，
**没有**任何 TLS 信任旋钮 → **插件接口必须加字段，这是接口变更**（AGENTS.md 的
接缝纪律：契约变化要写 ADR）。

按 ADR-015 的加法纪律，**不改**既有 5 参数签名，而是**新增**一个带默认实现的
7 参数 virtual 重载：

```cpp
virtual void add_turn_server_with_transport(std::string_view host,
                                            std::uint16_t port,
                                            std::string_view username,
                                            std::string_view password,
                                            TurnTransport transport,
                                            bool turn_tls_verify_peer,
                                            std::string_view turn_tls_ca_file) noexcept {
    turn_tls_verify_peer_ = turn_tls_verify_peer;
    turn_tls_ca_file_ = std::string(turn_tls_ca_file);
    add_turn_server_with_transport(host, port, username, password, transport);
}
```

默认实现把旋钮存进接口**自己的受保护成员**，再回落既有 5 参数重载。于是：

- **既有实现者不受影响**：只 override 5 参数形式的实现，行为不变（判据 10）。
- **内置 ICE 传输**不 override 7 参数形式 —— 它在**既有** 5 参数实现里把
  继承到的 `turn_tls_verify_peer_` / `turn_tls_ca_file_` 搬进 `impl_->config`
  （`src/modules/ice/src/ice.cpp`，本片唯一要碰的 ICE 层文件）。

### 2.3 决策三：ICE 层**不需要改** —— 顺序论证

核实结论（写规格时已确认）：

- `IceTransport::add_turn_server_with_transport()` 把参数装配成模块层的
  `TurnServer` 后调 `add_turn_server(ts)`，后者存进 `config.turn_servers`。
- TLS 客户端在 `create_agent()`（`ice.cpp:642`）里**才**创建，信任策略从
  `config.turn_tls_ca_file` / `config.turn_tls_verify_peer` 读（`ice.cpp:679-682`）。
- `create_agent()` 由 `open()` 调用（`ice.cpp:1222`）—— **在**
  `add_turn_server()` **之后**。

所以顺序是：`add_turn_server*()` 存 → `open()` 时 `create_agent()` 读配置建 TLS
客户端。**只要把两个旋钮从插件接缝传到 `IceConfig`，ICE 层一行都不用改。**
`src/modules/ice/` 的其它文件（含 `turn_tls.cpp` 的校验逻辑）**只读不改**。

### 2.4 决策四：失败必须可见

宿主设了 `turn_tls_ca_file` 但文件不存在 / 不是有效 PEM → 宿主要**看得到**。
ICE 层已有 `last_error_`（`turn_tls.cpp:127`：`"failed to load the CA file ..."`），
但那**只躺在 ICE 层的字符串里**。本片要求这条错误**透到宿主能读到的地方**
（`set_on_error`），不是只躺在 ICE 层。

引擎侧在应用凭据时**自己**做一次信任锚检查（`check_turn_tls_ca_file`：空路径 →
OK；不存在 → `kMissing`；存在但无 PEM 证书块 → `kNotPem`），失败时：

- 把验证事实标成 `failed`（`note_turn_tls_anchor_failed()`）；
- 经 `on_error_(core::kEngineInvalidParam, ...)` 透出，消息**说清哪个文件、什么错**。

检查是 **TURN/TLS-only**：`transport != Tls` 时 `ca_file` 无意义，被忽略 ——
与 `verify_peer == false` 时忽略 `ca_file` 一致（spec §设计 一）。

### 2.5 决策五：`turn_tls_verify_peer = false` 要能被看见 —— 可查询的事实

宿主关掉校验是**有代价**的决定。本片要求：引擎提供一个**可查询的事实**，
让「这通话没验过中继身份」不是只有读源码才知道。

实现为公开方法 `NimRTCEngine::turn_tls_peer_verification()`，取值：

- `""` —— 没有 TURN/TLS 凭据集在生效（transport 不是 `Tls`，或没应用过）。
- `"verified"` —— `verify_peer == true` 且信任锚可通过（或没配 `ca_file`）。
- `"unverified"` —— `verify_peer == false`：显式放弃校验。
- `"failed"` —— `verify_peer == true` 但 `ca_file` 缺失 / 非 PEM：校验配置了但过不了。

三种**互相可区分**。事实由 `TurnCredentialsState` 根据**已应用**的凭据计算，
不观察线上数据本身（那属于 N4 的 stats / call-state 面）。

**为什么不放进 `Stats::transport`**：`Stats` 的投影装配在
`engine_media.cpp` / `engine_transport_stats.cpp`，都不在本片写集内。把事实做成
独立公开方法是写集内的最优解 —— 它不扩 `Stats` 的 ABI，仍是「可查询的事实」。

### 2.6 决策六：`same_credentials()` 纳入两个新字段

`same_credentials()` 逐字段比较 —— 新字段**必须**加进去，否则「换了信任策略」
不会被识别成「凭据变了」，N7 的重取语义就错了。只改 `ca_file` 或只改
`verify_peer` → 判定为「凭据变了」→ 引擎重新应用。

## 3. 后果

### 3.1 公开面的变化（全部是加法）

| 符号 | 变化 |
|---|---|
| `TurnCredentials::turn_tls_verify_peer` | 新增字段，`bool`，默认 `true` |
| `TurnCredentials::turn_tls_ca_file` | 新增字段，`std::string`，默认空 |
| `NimRTCEngine::turn_tls_peer_verification()` | 新增公开方法，`NIMRTC_API_STABLE` |
| `plugins::IICETransport` | 新增 7 参数 virtual 重载（默认实现）；既有 5 参数签名不变 |
| `src/modules/ice/src/ice.cpp` | 既有 5 参数实现把接口继承到的旋钮搬进 `impl_->config` |

`TurnCredentials` 既有六字段**语义不变**；`set_on_control_message()` /
`set_on_call_state()` **签名不变**；`IceConfig` 既有字段**语义不变**；
`add_turn_server()` 两个重载**路径不变**。

### 3.2 依赖方向

- 引擎侧新增的检查与事实计算都在 `engine_turn_credentials.{hpp,cpp}`
  （本片写集内），只 include 引擎自己的公开头，**不** include `nimrtc::ice/`
  的具体头（L4）。
- `src/modules/ice/src/turn_tls.cpp` **不改** —— 校验逻辑不变（空信任库拒绝是
  正确行为）；本片只保证宿主**能提供**信任库或显式放弃校验。
- `src/modules/ice/include/nimrtc/ice/ice.hpp` **不改** —— 旋钮已经在 ICE 层。

### 3.3 线程规则

`turn_tls_peer_verification()` 是 `const noexcept` 查询，从会话状态读取；
信任锚检查发生在 `refresh_turn_credentials()`（引擎 tick / open 路径）内，
`on_error_` 的线程语义与既有 `set_on_error` 一致。

### 3.4 已知并接受的限制

- 引擎侧对 `ca_file` 的检查是**格式嗅探**（存在 + 含 PEM 证书块），不是完整
  X.509 解析 —— 真正的解析在 wolfSSL 加载信任锚时（`turn_tls.cpp:125`）。
- 检查是**应用时**的一次性判定；线上握手失败（证书链不被信任等）仍走 N4 的
  stats / call-state 面报告，不改变本片的事实（`"verified"` 表示「校验配置且
  锚可加载」，不保证「线上一定握手成功」）。

## 4. 备选方案与不选的理由

| 备选 | 不选的理由 |
|---|---|
| 把旋钮放在 `EngineConfig` 上 | 信任策略跟着凭据走才自洽；放在 `EngineConfig` 上会出现「凭据换了但信任策略没换」的说不清状态 |
| 改既有 5 参数 `add_turn_server_with_transport()` 签名 | 破坏所有既有实现者（含测试替身）；ADR-015 要求加法 |
| 复用 `nimrtc::ice::TurnServer` / 直接 include `ice.hpp` | 违反 L4 / ADR-025；引擎公开面不引入具体模块类型 |
| 只靠 ICE 层的 `last_error_` | 错误躺在模块内部字符串，宿主读不到 —— 正是本片要消灭的「静默」 |
| 事实放进 `Stats::transport` | 投影装配在写集之外的文件；独立公开方法在写集内且不扩 `Stats` ABI |
| 不改 `same_credentials()` | 宿主换了信任锚但引擎静默沿用旧的，中继还在被拒且宿主看不到原因 |

## 5. 落地注意（给后续切片）

- **测试写了要连 CMake 注册**（ADR-031 §5.1 的教训）：本片新增的测试用例都在
  `test_engine_turn_credentials.cpp`，已登记进 `src/engine/tests/CMakeLists.txt`。
- **API 分类扫描是全库级的**（ADR-031 §5.3 的教训）：新公开符号
  `turn_tls_peer_verification()` 与 `TurnCredentials` 的两个新字段必须带
  `NIMRTC_API_STABLE`，否则 `scripts/api_freeze_scan.js` 会在门禁②上拦下本片。
- **N6 的缺口 A 在本片合入后解除**：验收测试里的 `blockedReason()` 对缺口 A 的
  检查（`/(turn_tls_verify_peer|turn_tls_ca_file)/` 匹配 `engine.hpp`）应当**不再**命中。
