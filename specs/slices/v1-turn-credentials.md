# Slice: v1-turn-credentials

## Summary

**TURN 凭据由宿主按需提供，引擎负责拉取。** 宿主不再需要「先 `pre_open()` → 再 `add_turn_server()` → 再 `open()`」这三步生命周期，也不必自己管凭据的过期 —— 引擎在**收集开始时**、以及**每一次需要重新分配中继时**，重新问宿主要一次凭据。

三件事：① `EngineConfig::turn_credentials` 回调（宿主塞进去一个函数指针）；② 引擎在 gathering 前与重连时**重新咨询**该回调；③ 凭据被拒时报 `"credentials-rejected"`（N4 的词汇），而不是笼统的 ICE 失败。

这是 v1.4 计划书 §N7 的落地，架构决策在 `docs/adr/ADR-032-turn-credentials-provider-seam.md`（**Proposed**，待签字）。

## 目标

**让凭据的拉取成为引擎的活**：宿主把「怎么拿凭据」交给引擎，引擎在需要的时候问；凭据过期了引擎会再问一次，而不是让会话哑掉。

这是 v1.4 计划书 §N7（"credentials as a first-class seam"）的落地。架构决策在 `docs/adr/ADR-032-turn-credentials-provider-seam.md`（**Proposed**，待签字）。

> **归属**：`version/v3`。N7 要改 `EngineConfig`（加一个回调字段）、公开头（加一个回调类型 + 一个 `Stats` 字段）、`engine.cpp` / `engine_transport.cpp`（接线）—— 与 N3（`8abb815a`）、D1 / D2 / D6 撞同一片区域，必须**顺序**做。
>
> **前置**：N3（`8abb815a`）已在 `version/v3` 上 —— N7 的「重连时重新咨询凭据」依赖 N3 的重连状态机（`engine_ice_recovery.hpp`）。

### 现状核实（写规格时的树状态）

- **`add_turn_server()` 是唯一入口，且是手动的**：`engine.hpp:3584` 明写 "Must be called BEFORE open()"；`engine.cpp:835` 的实现先查 `ice_t_` 是否存在，否则返回非零并打 "call pre_open() first"。
- **凭据进到 ICE 模块是一次性的**：`IceTransport::add_turn_server()`（`ice.cpp:1051`）把 `TurnServer` **push 进** `impl_->config.turn_servers`；`ice.cpp:655-671` 在**建 agent 时**（`juice_config`）把这份 vector 拷进 libjuice。**建 agent 之后就再也不会变** —— 过期了没有任何地方会更新它。
- **ICE 模块已经能识别「凭据被拒」**：`ice.cpp:63` 的进程级闩锁 `g_turn_auth_rejected`，由 `ice.cpp:78` 在 libjuice 的日志钩子里置位（agent.c 收到 401/403 时打 "TURN authentication failed"）；`ice.cpp:376` 的注释明写 "credentials rejection is only observable through the module's log hook"。
- **N4 的失败原因词汇已经包含它**：`Stats::ice_failure_reason` 的闭合词汇里有 `"credentials-rejected"`（`engine_transport_stats.cpp` 的 `ice_failure_reason()`）—— N7 要做的是让这个原因**真的能被报出来**，而不是笼统的 `"checks-failed"`。
- **TURN 传输的选择已经在接缝上**：`plugins::IICETransport::add_turn_server_with_transport()`（`ice_transport.hpp:361`）与 `TurnTransport` 枚举（`ice_transport.hpp:115`）—— N7 的回调要能带传输方式，否则宿主没法表达「TURN/TLS」。
- **`TurnServer` 是 ICE 模块的公开结构体**：`ice.hpp:155`，带 `host` / `port` / `username` / `password` / `transport` / `tls_server_name`。
- **引擎不直接依赖 `nimrtc/ice/`**：`src/engine/` 只通过 `plugins::IICETransport` 接缝看 ICE（接缝纪律 L4）—— N7 的回调类型必须定义在**引擎的公开头**里，不能把 `nimrtc::ice::TurnServer` 泄漏到 `engine.hpp`。

### 与其它片的边界（本片**不**做）

- **不做** 签发凭据的服务端（谁签发是宿主 / 信令服务的事，RD-7）—— N7 是**接缝**，不是服务器。
- **不做** old/new pair 上报 —— 那是 N8。
- **不做** call-state 枚举 —— 那是 D7。
- **不做** 重连策略本身 —— 那是 N3（本片只**挂**在 N3 的重连点上）。

## 范围

- src/engine/src/engine_turn_credentials.hpp
- src/engine/src/engine_turn_credentials.cpp
- src/engine/src/engine.cpp
- src/engine/src/engine_transport.cpp
- src/engine/src/engine_session_state.hpp
- src/engine/src/engine_transport_stats.cpp
- src/engine/include/nimrtc/engine/engine.hpp
- src/engine/CMakeLists.txt
- src/engine/tests/CMakeLists.txt
- src/engine/tests/test_engine_turn_credentials.cpp
- tests/acceptance/v1-turn-credentials/acceptance.test.cjs
- docs/adr/ADR-032-turn-credentials-provider-seam.md
- docs/api/engine_api.md
- CHANGELOG.md

> 只读 `specs/slices/v1-ice-recovery.md`（N3 的规格）—— 本片**不**改它。
> 只读 `src/modules/ice/`（ICE 模块）—— 本片**不**改它：凭据的重新咨询发生在**引擎侧**（在 ICE 接缝之上）。

## 设计

### 一、公开面：回调类型 + `EngineConfig::turn_credentials`

```cpp
// src/engine/include/nimrtc/engine/engine.hpp

/** 一份 TURN 凭据（宿主提供给引擎的）。 */
struct NIMRTC_API_STABLE TurnCredentials {
    /** TURN 服务器地址（主机名或 IP）。 */
    std::string host;
    /** TURN 服务器端口。0 = 默认（TURN/UDP 3478）。 */
    std::uint16_t port = 3478;
    /** 用户名。空 = 匿名分配（RFC 5766 允许）。 */
    std::string username;
    /** 密码。空 = 匿名分配。 */
    std::string password;
    /** 到达该服务器用的传输。默认 `Udp`（每个 TURN 服务器都支持）。 */
    plugins::TurnTransport transport = plugins::TurnTransport::Udp;
    /** TURN/TLS 时的 SNI / 证书主机名。空 = 用 `host`。 */
    std::string tls_server_name;
};
```

**为什么在引擎的公开头里定义一份，而不是复用 `nimrtc::ice::TurnServer`**：接缝纪律（L4）—— `engine.hpp` **不** include `nimrtc/ice/` 的任何具体头。复用会把这个依赖引进公开面。

**为什么不用 `std::function`**：公开头要跨 ABI 稳定（ADR-015）。`std::function` 的 ABI 不是稳定承诺的一部分。**函数指针 + 上下文指针**才是（与树里 `set_on_ice_recovery` / `set_on_error` 的形状一致）。

```cpp
/** 凭据提供者的回调类型。
 *
 *  引擎在**需要凭据时**调用它（gathering 前、重连时）。
 *  返回 `false` = 本次拿不到凭据（引擎不配 TURN，继续用直连）。
 *  返回 `true` = `out` 里有一份凭据（`out->host` 非空）。
 *
 *  **线程**：在引擎的 tick 线程上调用 —— 与 `set_on_ice_recovery` 同一线程。
 *  回调**必须**不阻塞、不重入引擎。 */
using TurnCredentialsProvider = bool (*)(void* ctx, TurnCredentials* out) noexcept;

/** 在 `EngineConfig` 里：
 *  凭据提供者。空 = 引擎不拉凭据（今天的静态 `add_turn_server()` 路径不变）。
 *  `ctx` 原样回传给回调。 */
TurnCredentialsProvider turn_credentials = nullptr;
void* turn_credentials_ctx = nullptr;
```

**为什么是 `EngineConfig` 上的回调而不是「注册的接缝」**（§N7-1 已决）：这样「有静态服务器列表」的宿主**继续调 `add_turn_server()`**，什么都不改 —— 加接缝会逼所有宿主迁移。

### 二、引擎侧：什么时候问

引擎在**两个时机**问凭据提供者：

1. **gathering 之前**（`open()` 阶段）：没有静态 TURN 配置、但配了 `turn_credentials` 时，问一次并 `add_turn_server()`。
2. **重连时**（N3 的重连点）：N3 的恢复重启了 ICE 之后，**重新问** —— 因为旧凭据可能已经过期。

**为什么第二个时机是关键**：凭据有 TTL。RFC 7635 那种静态用户名/密码没有寿命概念，但真实的实时 TURN 凭据有（`username:timestamp` + HMAC）。**寿命是提供者的业务，引擎必须再问一次。**

### 三、私有协作者 `engine_turn_credentials.{hpp,cpp}`

按 N3 的形状（私有 TU、时钟显式传参、确定性单测）：

```cpp
namespace nimrtc::engine::detail {

/** 凭据拉取的记账：最近一次拿到什么、被拒了几次。 */
class TurnCredentialsState {
public:
    /** 问一次提供者。返回 true = 拿到了一份（`out` 有效）。 */
    bool refresh(std::int64_t now_us, TurnCredentialsProvider provider,
                 void* ctx, TurnCredentials* out) noexcept;

    /** 提供者返回 false = 「拿不到」。计数 +1（连续失败数）。 */
    bool consecutive_failures() const noexcept;
    /** 被拒（401/403）过没有。 */
    bool rejected() const noexcept;
    void note_rejected() noexcept;
};
}
```

**为什么要有这个协作者**：引擎的 `open()` 与 tick 是**两条不同的路径**，都要问凭据。没有一个共同的记账点，「问了几次」「失败几次」「被拒过没有」就散在两处，会各自漂移。

### 四、凭据被拒的报法

- ICE 模块已经有 `g_turn_auth_rejected` 闩锁（进程级，`ice.cpp:63`）。
- 引擎在 N4 的失败原因投影里**读它** —— `ice_failure_reason()` 已经有 `"credentials-rejected"` 这个取值（`engine_transport_stats.cpp`），N7 让它**真的能被选中**。
- 新增 `Stats::turn_credentials_state`，取值：`"idle"` / `"ok"` / `"unavailable"` / `"rejected"` —— 让宿主**不解析日志**就知道结果。

### 五、接线点

- `engine.cpp` 的 `open()`：在 ICE 传输建好、gathering 开始之前，若 `cfg.turn_credentials != nullptr` 且**没有**静态 TURN 服务器 → 问一次 → `add_turn_server()`。
- `engine_transport.cpp` 的 N3 重连点：重启 ICE **之后**，重新问一次 → 若拿到新凭据且与旧的不同 → `add_turn_server()`（ICE 模块在 agent 重建时会重新读 `config.turn_servers`）。
- `engine_transport_stats.cpp`：投影 `Stats::turn_credentials_state`。

## 验收标准

### 一、公开面

1. `EngineConfig::turn_credentials` 是 `TurnCredentialsProvider` 类型，默认 `nullptr`；`turn_credentials_ctx` 默认 `nullptr`。
2. `TurnCredentials` 结构体带 `host` / `port` / `username` / `password` / `transport` / `tls_server_name` 六个字段。
3. `EngineConfig::turn_credentials` 的类型是**函数指针**（不是 `std::function`）—— 静态检查：`engine.hpp` 里不出现 `std::function`。

### 二、拉取时机

4. 配了 `turn_credentials` 且**没有**静态 TURN 服务器 → `open()` 时问提供者**一次**。
5. 提供者返回 `false`（拿不到）→ 引擎**不**配 TURN，**不崩**，会话继续走直连；`Stats::turn_credentials_state == "unavailable"`。
6. 提供者返回 `true` 且 `host` 非空 → 引擎按返回的凭据配 TURN（含 `transport`）。
7. 配了 `turn_credentials` **且**已有静态 TURN 服务器 → 静态的**优先**（不覆盖宿主的显式配置）；提供者**不被调用**。
8. N3 的重连重启 ICE **之后** → 引擎**重新问**提供者（一次）。
9. 重连时提供者返回的凭据与上次**不同** → 引擎用新凭据更新 TURN 配置。
10. 重连时提供者返回 `false` → 引擎**不**崩；保留旧凭据（旧的也许还有效）；`turn_credentials_state == "unavailable"`。

### 三、凭据被拒

11. ICE 报告凭据被拒 → `Stats::ice_failure_reason == "credentials-rejected"`（不是 `"checks-failed"`）。
12. 凭据被拒 → `Stats::turn_credentials_state == "rejected"`。
13. 凭据被拒**不**被报成 `"no-candidates"` 或 `"none"`。

### 四、边界与兼容

14. **不配** `turn_credentials` 的宿主 → 行为与今天**完全一致**（`add_turn_server()` 的静态路径仍可用、仍须在 `open()` 前调）。
15. `turn_credentials` 提供者传 `nullptr` → 引擎**不**调用它（不崩）。
16. 提供者返回 `true` 但 `host` 为空 → 引擎**不**配 TURN（当作拿不到）。
17. `src/engine/` **不** include `nimrtc/ice/` 的任何具体头（接缝纪律，L4）。
18. 提供者在引擎 tick 线程上被调用（**不是**宿主线程）—— 静态检查 + 单测钉住。
19. `src/engine/tests/test_engine_*.cpp` 一整套在新增后**仍然通过**（回归）。

### 五、文档与契约

20. `docs/api/engine_api.md` 记录：新的回调类型与用法、`EngineConfig` 的两个新字段、`Stats::turn_credentials_state` 的取值、静态 `add_turn_server()` 与新接缝的**优先级**。
21. `docs/api/public_headers.manifest` 与实际公开面一致（只读 `scripts/check_api_manifest.py` — 跑它，不改它；本片**不新增头文件**，所以清单**应当不变**）。
22. 新增的公开符号全部带 `NIMRTC_API_STABLE` 或 `NIMRTC_API_UNSTABLE` 标注（跑只读 `scripts/api_freeze_scan.js` — 跑它，不改它）。

## 本片不做

- **不做** 凭据签发服务 —— 谁签发是宿主的业务（RD-7）。
- **不做** 凭据的缓存与预取 —— 引擎问一次拿一次，不替宿主缓存。
- **不做** 改 `plugins::IICETransport` 接口 —— 重新咨询发生在**引擎侧**，接缝不动。
- **不改** `src/modules/ice/` —— 凭据的重新咨询在 ICE 接缝**之上**。
- **不做** old/new pair 上报 —— 那是 N8。
- **不做** call-state 枚举 —— 那是 D7。

## 接口变更

### 新增（全部是加法，ADR-015）

| 符号 | 位置 | 说明 |
|---|---|---|
| `TurnCredentials` | `engine.hpp` | 结构体：一份 TURN 凭据（`host` / `port` / `username` / `password` / `transport` / `tls_server_name`） |
| `TurnCredentialsProvider` | `engine.hpp` | 类型别名：`bool (*)(void* ctx, TurnCredentials* out) noexcept` |
| `EngineConfig::turn_credentials` | `engine.hpp` | 新增字段，默认 `nullptr` |
| `EngineConfig::turn_credentials_ctx` | `engine.hpp` | 新增字段，默认 `nullptr` |
| `Stats::turn_credentials_state` | `engine.hpp` | 新增字段，默认 `""`；取值 `"idle"` / `"ok"` / `"unavailable"` / `"rejected"` |

### 不变

- `NimRTCEngine::add_turn_server()`（两个重载）—— **签名与语义不变**，静态路径继续可用。
- `plugins::IICETransport` —— **不动**（重新咨询发生在引擎侧，接缝之上）。
- `nimrtc::ice::TurnServer` —— **不动**（引擎侧定义自己的 `TurnCredentials`，不把 ICE 的依赖引进公开面）。

## 测试策略

| 层次 | 手段 | 钉住什么 |
|---|---|---|
| 单元（`src/engine/tests/test_engine_turn_credentials.cpp`） | 私有协作者 `TurnCredentialsState` 的**纯函数**测试（时钟显式传参） | 拉取记账：连续失败计数、被拒闩锁、凭据变化的判定 |
| 单元 | 静态断言（读 `engine.hpp` 源码） | 回调是**函数指针**不是 `std::function`；`EngineConfig` 的默认值 |
| 验收（`tests/acceptance/v1-turn-credentials/acceptance.test.cjs`） | 跑真实 gtest 二进制 + 静态断言 | 22 条验收标准逐条落成真实断言（无 `todo` 用例） |
| 回归 | `src/engine/tests/test_engine_*.cpp` 一整套 | 新增之后**仍然全绿** |

**为什么单元测试用「纯函数 + 显式时钟」**：与 N3 的 `engine_ice_recovery` 同一形状（D4 的 `engine_capture_reconnect` 是它的先例）—— 状态机不该有真实时间，测试才能确定性。

**为什么验收要跑真实 gtest 二进制**：门禁②要求「每一条验收标准对应一条真正跑起来的用例」。验收层用 `spawnSync` 直接跑二进制并检查 `[       OK ] <case>`，不看 ctest 汇总 —— **ctest 汇总全绿不代表新测试跑过**（N3 的 ADR-031 §5.1 记了这个坑）。

## 风险

- **凭据提供者在引擎线程上被调用**：若宿主的实现阻塞，引擎的 tick 会卡住。规格钉住「回调必须不阻塞」（验收标准 18），并在 `engine_api.md` 写明。
- **凭据提供者与 `add_turn_server()` 混用**：两者的优先级必须**明确且可测**（验收标准 7）—— 静态配置优先，因为它是显式的。
- **凭据被拒的观测路径**：今天只能靠 ICE 模块的日志钩子（进程级闩锁）。N7 把它读出来变成 stats —— 但闩锁是**进程级**的，多引擎并存时可能互相污染。这在 v1.4 内**已知并接受**（记在 ADR-032 里）。
