# Slice: v1-turn-tls-trust

## Summary

**宿主没法信任 TURN/TLS 中继的证书 —— 因为公开面上没有那个旋钮。**

N6（SDK 样例）在搭双进程验收腿时撞出这个缺口（2026-11-25 `ZELTO_QUESTION`）：

- `src/modules/ice/` **已经有** `IceConfig::turn_tls_verify_peer`（默认 `true`）与
  `IceConfig::turn_tls_ca_file`（`ice.hpp:257` / `ice.hpp:274`），
  `src/modules/ice/src/turn_tls.cpp:123-134` 也确实按它们决定 `WOLFSSL_VERIFY_PEER` / `VERIFY_NONE`。
- **引擎公开面没有透出来**：`src/engine/include/nimrtc/engine/engine.hpp` 的
  `TurnCredentials` 只有 `tls_server_name`（`engine.hpp:260`），**没有**
  `turn_tls_verify_peer` / `turn_tls_ca_file`。
- 于是宿主**无法表达**「我信任这个自签证书」或「这个中继我不要校验」。

后果是**必然**失败，不是「可能失败」：

- N5 参考中继是 **TLS-only + 自签证书**（ADR-027 Decision 4）。
- ice 默认 `verify_peer = true` + **空信任库**（`src/modules/ice/src/turn_tls.cpp:124-134`）。
- 没有 `ca_file` → 加载信任锚失败 → **必然拒绝**。
  证据：`TurnTls.MissingTrustAnchorDoesNotProduceARelayCandidate`（ctest #461）通过。

所以 TLS 中继这条腿今天**只能靠改引擎源码**才能跑通 —— 而那正是接缝纪律禁止的
（宿主不许绕过接缝）。N6 只能把执行腿标成 `SKIPPED`。

本片把**已经在 ICE 层存在的旋钮**透到引擎公开面，并让宿主能真正用上它。

## 目标

**让宿主能在引擎公开面上表达它对 TURN/TLS 证书的信任策略。**

具体地，三件事：

1. `TurnCredentials` 加上 `turn_tls_verify_peer`（bool）与 `turn_tls_ca_file`（路径）。
2. 引擎把它们**翻译**到 ICE 层的 `IceConfig` 对应字段（不新建插件接口字段，
   除非插件接口本身没有这些旋钮 —— 那时按 ADR-025 的接缝纪律处理，见「设计」）。
3. 宿主能因此**真的**让 N5 自签中继跑起来：要么给 `ca_file`（信任锚），
   要么 `turn_tls_verify_peer = false`（显式放弃校验，并**明确知道自己放弃了**）。

### 现状核实（写规格时的树状态）

- `IceConfig::turn_tls_verify_peer`（`src/modules/ice/include/nimrtc/ice/ice.hpp:274`）
  存在，默认 `true`。
- `IceConfig::turn_tls_ca_file`（`ice.hpp:257`）存在，默认空。
- `src/modules/ice/src/turn_tls.cpp:123-134`：`verify_peer` 为真且 `ca_file` 为空 → **没有**任何
  信任锚 → `WOLFSSL_VERIFY_PEER` 必然拒绝对端。
- `src/modules/ice/src/turn_tls.cpp:142`：`verify_peer` 为假 → `WOLFSSL_VERIFY_NONE`。
- `TurnCredentials`（`engine.hpp:236-262`）只有 `host` / `port` / `username` /
  `password` / `transport` / `tls_server_name`。
- `same_credentials()`（`src/engine/src/engine_turn_credentials.cpp:22`）逐字段比较
  六个字段 —— **本片要加的字段必须进这个比较**，否则「换了信任策略」不会被
  识别成「凭据变了」，N7 的重取语义就错了。
- `engine_turn_credentials.cpp` 的 `apply` 路径把 `TurnCredentials` 翻译到
  `plugins::IICETransport::add_turn_server_with_transport()`。

### 与其它片的边界（本片**不做**）

- **不做** 改 ICE 层的任何东西 —— `turn_tls_verify_peer` / `turn_tls_ca_file`
  **已经在** ICE 层了。本片只是**透**出来。
- **不做** 改 N5 参考中继 —— 它的自签证书是 ADR-027 的**已决**，不是本片推翻的。
- **不做** 改 `plugins::IICETransport` 的公开接口 —— 除非核实后发现插件层**没有**
  这两个旋钮（见「设计 §三」）。若要改，那是接口变更，按 AGENTS.md 写 ADR。
- **不做** 动 `src/modules/ice/src/turn_tls.cpp` 的校验逻辑 —— 空信任库拒绝是**正确**行为。
- **不做** 动安装树 —— 那是另一片（`v1-install-tree-video-deps`），写集不重叠。

## 范围

- src/engine/include/nimrtc/engine/engine.hpp
- src/engine/src/engine_turn_credentials.hpp
- src/engine/src/engine_turn_credentials.cpp
- src/engine/src/engine.cpp
- src/plugins/include/nimrtc/plugins/ice_transport.hpp
- src/modules/ice/src/ice.cpp
- src/engine/tests/test_engine_turn_credentials.cpp
- tests/acceptance/v1-turn-tls-trust/acceptance.test.cjs
- docs/adr/ADR-037-turn-tls-trust-seam.md
- docs/api/engine_api.md
- CHANGELOG.md
- specs/slices/v1-turn-tls-trust.md

> **只读** `src/modules/ice/include/nimrtc/ice/ice.hpp` —— ICE 层的
> `IceConfig::turn_tls_*` **已经在**那里了，本片只把旋钮**搬**过去。
> **只读** `src/modules/ice/src/turn_tls.cpp` —— 参考它的校验逻辑，**不改**它
> （空信任库拒绝是**正确**行为）。
> **只读** `src/modules/ice/tests/test_turn_tls.cpp` —— 既有测试是回归基线，
> **不改**它们。
> **只读** `AGENTS.md` —— 参考它的「契约变化要写 ADR」纪律。
> `src/modules/ice/src/ice.cpp` 是**唯一**要碰的 ICE 层文件 —— 只改
> `add_turn_server_with_transport()` 的实现（把新参数搬进 `IceConfig`），
> **不改** `create_agent()` 的 TLS 客户端创建逻辑。
> **只读** `cmake/` —— 安装树是另一片（`v1-install-tree-video-deps`）。

## 设计

### 一、公开面：两个字段进 `TurnCredentials`

```cpp
// src/engine/include/nimrtc/engine/engine.hpp

struct NIMRTC_API_STABLE TurnCredentials {
    /* ... 现有六个字段不变 ... */

    /** TURN/TLS 对端证书校验开关。默认 `true`。
     *
     *  `false` = **显式**放弃校验对端证书。只在测试中继 / 自签中继 /
     *  受信网络里用；宿主一旦关掉，就再也拿不到「中继身份被验证过」的保证。
     *  置 `false` 且 `turn_tls_ca_file` 非空是矛盾的 —— 后者会被忽略
     *  （校验都关了，要信任锚做什么）。 */
    bool turn_tls_verify_peer = true;

    /** TURN/TLS 信任锚文件（PEM 路径）。
     *
     *  空 = 不加载任何信任锚。配合 `turn_tls_verify_peer = true`，
     *  这意味着「对端证书一律不认」—— 与 `verify_peer = false` 的区别是
     *  **失败模式不同**：前者是**校验失败**（握手被拒），后者是**不校验**。
     *  宿主想要「信任这个自签中继」，正确做法是给它 `ca_file`。 */
    std::string turn_tls_ca_file;
};
```

**为什么这两个字段在 `TurnCredentials` 上，不在 `EngineConfig` 上**：
凭据是**每次拉取**都可能换的（N7），信任策略跟着凭据走才自洽 ——
「换了中继就换信任锚」是一句话。若放在 `EngineConfig` 上，则「凭据换了但信任策略
没换」会成为一种可能出现的、说不清的状态。

### 二、翻译路径：引擎 → 插件 → ICE

引擎**不**直接 include `nimrtc/ice/` 的头（接缝纪律 L4）。所以路径是：

```
TurnCredentials.turn_tls_verify_peer / .turn_tls_ca_file
    → 引擎内部翻译
    → plugins::IICETransport（插件接缝）
    → IceConfig::turn_tls_verify_peer / .turn_tls_ca_file
```

**核实结论（写规格时已确认，2026-11-25）**：`plugins::IICETransport` 的
`add_turn_server_with_transport()`（`src/plugins/include/nimrtc/plugins/ice_transport.hpp:361`）
**只有** `host` / `port` / `username` / `password` / `transport` 五个参数，
**没有**任何 TLS 信任旋钮。

→ **插件接口必须加字段。这是接口变更**，按 AGENTS.md 写 ADR-037，
并检查 ABI 冻结（`scripts/api_freeze_scan.js`）。

**但 ICE 层**`src/modules/ice/` **不需要改** —— 核实如下：

- `IceTransport::add_turn_server_with_transport()`（`src/modules/ice/src/ice.cpp:1128`）
  把参数装配成模块层的 `TurnServer` 后调 `add_turn_server(ts)`，后者存进
  `config.turn_servers`。
- TLS 客户端在 `create_agent()`（`ice.cpp:642`）里**才**创建，
  信任策略从 `config.turn_tls_ca_file` / `config.turn_tls_verify_peer`
  读（`ice.cpp:679-682`）。
- `create_agent()` 由 `open()` 调用（`ice.cpp:1222`）—— **在**
  `add_turn_server()` **之后**。

所以顺序是：`add_turn_server*()` 存 → `open()` 时 `create_agent()` 读配置建 TLS 客户端。
**只要把两个旋钮从插件接缝传到 `IceConfig`，ICE 层一行都不用改。**

**因此本片的改动面是**：`engine.hpp`（公开面）+ `ice_transport.hpp`（插件接缝）+
`ice.cpp` 的 `add_turn_server_with_transport` 实现（把新参数搬进 `IceConfig`）。
`src/modules/ice/` **只读不改**。

**接缝纪律的一个后果**：插件接缝的 `add_turn_server_with_transport` 是
`virtual` 且有默认实现（`ice_transport.hpp:365-371`）。加参数时**必须**保留
既有签名的兼容性 —— 用**新增的 `virtual` 重载**（带默认实现），
而不是改现有签名，否则**所有**已有的实现者（含 `src/engine/tests/test_engine_turn_credentials.cpp:337`
那个测试替身）都会静默走到错误路径。

### 三、`same_credentials()` 必须纳入新字段

`src/engine/src/engine_turn_credentials.cpp:22` 的 `same_credentials()` 逐字段比较。
新字段**必须**加进去：

- 只改 `ca_file` → 凭据**变了** → N7 应当重取 / 重新应用。
- 漏了这两个字段 → 宿主换了信任锚但引擎**静默沿用旧的** → 中继还在被拒，
  而宿主**看不到任何原因**。这正是本片要消灭的那类「静默」。

### 四、失败必须可见

宿主设了 `turn_tls_ca_file` 但文件不存在 / 不是有效 PEM → 宿主要**看得到**。
引擎侧已有 `last_error_`（`src/modules/ice/src/turn_tls.cpp:127`：`"failed to load the CA file ..."`）。
本片要求这条错误**透到宿主能读到的地方**（`set_on_error` 或 call-state 的
`failure_reason`），**不是**只躺在 ICE 层的字符串里。

具体形状服从 D7 已有的 `CallFailure` / `failure_reason` 词汇；**不新增**失败枚举值
（D7 的枚举是 v1.4 的公开面，本片不扩它）。

### 五、`turn_tls_verify_peer = false` 要能被看见

宿主关掉校验是**有代价**的决定。本片要求：引擎在 `Stats`（N4 的 `Stats::transport`
区）或 call-state 里**标明**「对端证书未校验」，让「这通话没验过中继身份」
成为**可查询的事实**，而不是只有读源码才知道。

具体字段名由实现者定（放在 `Stats::transport` 里最自然），但必须满足：
**能区分「校验通过」与「没校验」与「校验失败」三种**。

## 验收标准

### 一、公开面

1. `TurnCredentials` 含 `turn_tls_verify_peer`（bool，默认 `true`）与
   `turn_tls_ca_file`（std::string，默认空）。
2. 新增公开符号全部带 `NIMRTC_API_STABLE`（跑只读 `scripts/api_freeze_scan.js`）。
3. `TurnCredentials` 现有六个字段的**语义不变**（`host` / `port` / `username` /
   `password` / `transport` / `tls_server_name`）。
4. `set_on_control_message()` / `set_on_call_state()` 的签名与语义**不变**。

### 二、翻译路径

5. `TurnCredentials.turn_tls_verify_peer` 的值**真的**到达 ICE 层的
   `IceConfig::turn_tls_verify_peer`。
6. `TurnCredentials.turn_tls_ca_file` 的值**真的**到达 ICE 层的
   `IceConfig::turn_tls_ca_file`。
7. `turn_tls_ca_file` 非空时，ICE 层**加载**该文件为信任锚
   （`src/modules/ice/src/turn_tls.cpp:123-134` 的既有路径被走到）。
8. `turn_tls_verify_peer == false` 时，ICE 层走 `WOLFSSL_VERIFY_NONE` 分支
   （`src/modules/ice/src/turn_tls.cpp:142`）。
9. `src/engine/` **不** include `nimrtc/ice/` 的任何具体头（接缝纪律，L4）。
10. 插件接缝的既有 `add_turn_server_with_transport()` 签名**保持可用**
    （既有调用者不因本片而改变行为）—— 新旋钮走**新增的 `virtual` 重载**。
11. `src/modules/ice/` 的 `src/modules/ice/src/turn_tls.cpp` **不被修改**（校验逻辑不变）。
12. `IceConfig` 的既有字段语义**不变**。

### 三、变更识别

10. `same_credentials()` 纳入两个新字段：只改 `ca_file` → 判定为「凭据变了」。
11. 只改 `turn_tls_verify_peer` → 判定为「凭据变了」。
12. 六个原字段全同 + 两个新字段全同 → 判定为「没变」（不重复应用）。

### 四、失败可见

13. `ca_file` 指向不存在的文件 → 宿主**能读到**一条说清「哪个文件、什么错」的
    错误（不是静默失败）。
14. `ca_file` 存在但不是有效信任锚 → 宿主**能读到**一条错误。
15. 该错误**不**只在 ICE 层的内部字符串里（宿主从 `set_on_error` 或
    `failure_reason` 能拿到）。

### 五、可查询的事实

16. 宿主能查询到「对端证书**未**被校验」这个状态（`verify_peer == false` 时）。
17. 宿主能查询到「校验**通过**」与「校验**失败**」这两种不同状态，
    三者**互相可区分**。

### 六、边界与回归

21. `plugins::IICETransport` 的既有方法签名**不变**（新增的是重载）。
22. `src/modules/ice/include/nimrtc/ice/ice.hpp` **不被修改**。
23. `src/modules/ice/src/turn_tls.cpp` **不被修改**。
24. `src/modules/ice/tests/test_turn_tls.cpp` 原有测试**全部仍然通过**（回归）。
25. `src/engine/tests/` 一整套在新增后**仍然通过**（回归）。
26. 引擎的**既有** `add_turn_server(host, port, user, pass)` 路径**不变**
    （静态 TURN 主机仍然可用）。

### 七、文档与契约

27. `docs/adr/ADR-037-turn-tls-trust-seam.md` 存在，说明：两个旋钮**已在 ICE 层**
    这个核实结论、插件接口**必须**加字段（因此是接口变更）、ICE 层**不需要改**的
    顺序论证（`add_turn_server*()` → `open()` → `create_agent()`）、以及为什么信任
    策略跟着**凭据**走而不是跟着 `EngineConfig` 走。
28. `docs/api/engine_api.md` 记录两个新字段的语义，尤其是
    `verify_peer = false` 的代价。
29. `CHANGELOG.md` 有对应条目。
30. `specs/slices/v1-turn-tls-trust.md` 记录实施中的发现。

## 归属

**`version/v3`**。

**前置**：N7（`v1-turn-credentials`）已合入 —— 本片在 `TurnCredentials` 上加字段，
依赖它存在。N5（参考 TURN 部署）已合入 —— 缺口由它暴露。

**并行**：与 `v1-install-tree-video-deps` **写集不重叠**，可并行。
本片碰 `src/engine/` + `src/plugins/`，那一片碰 `cmake/`。

**后续**：本片合入后，N6（`v1-sdk-sample`）的执行腿（判据 24-27）**不再**因缺口 A
被阻塞 —— 验收测试里的 `blockedReason()` 对缺口 A 的检查
（`/(turn_tls_verify_peer|turn_tls_ca_file)/` 匹配 `engine.hpp`）应当**不再**命中。
N6 仍可能因缺口 B 被阻塞，那是另一片的事。
