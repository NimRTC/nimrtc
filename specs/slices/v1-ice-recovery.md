# Slice: v1-ice-recovery

## Summary

网络断了（ICE 掉线）时，**引擎自己**按退避策略重启 ICE —— 宿主**不**调 `restart_ice()`，会话能自己回来。重启有**上限**（5 次 / 10 分钟），到顶进入**终态**并被**三处**报告（`on_error_` 回调 + `Stats` 字段 + 一行 `warn` 日志），而不是无限循环。

三件事：① 新增私有的重连状态机 `engine_ice_recovery.{hpp,cpp}`（grace 窗 → 1/2/4/8 s 退避 → 预算 → 终态）；② 新增 `EngineConfig::ice_recovery_policy`（默认 `"auto"` = 引擎管，`"off"` = 宿主管）；③ 新增 `set_on_ice_recovery` 回调 + 三个 `Stats` 字段 + `restart_ice()` 计入同一预算。

这是 v1.4 计划书 §N3 的落地，架构决策在 `docs/adr/ADR-031-ice-recovery-policy-ownership.md`（**Proposed**，待签字）。

## 目标

**让引擎管重连**：ICE 掉线后引擎自己退避重试并重启 ICE，重启有上限、到顶报告终态，宿主只在一个地方被告知「该重新协商了」。

这是 v1.4 计划书 §N3（"reconnection policy"）的落地。架构决策在 `docs/adr/ADR-031-ice-recovery-policy-ownership.md`（**Proposed**，待签字）。

> **归属**：`version/v3`。D3/D4 在 `db2dc0de`、D5 在 `6b5439da`、D6/N4 在 `59b09f02`、D1 在 `6a1e3a6e`、D2 在 `dbef30f9`，都已签进 `version/v3`。
>
> **为什么必须在 `version/v3`**：N3 要改 `EngineConfig`（加一个 key）、`Stats`（加三个字段）、公开头（加一个 setter + 一个方法 + 一个结构体）—— `engine.hpp` 正是 D1/D2/D6 都写过的那个文件。N3 与它们撞同一片区域，必须**顺序**做。
>
> **前置**：D6（`59b09f02`）已经在 `version/v3` 上 —— `connectivity_profile` 的解析在同一个 TU 族里，N3 的 policy 解析是它的兄弟。

### 现状核实（写规格时的树状态）

- **引擎不观察 ICE 掉线**：`src/engine/src/engine_transport.cpp:1720` 的 `poll_ice_flap_for_rehandshake()` 只在 `connected→disconnected→connected` 的**往返**上触发 DTLS rehandshake，**单向掉线不触发任何恢复**。
- **`restart_ice()` 是唯一入口，且是手动的**：`src/engine/src/engine.cpp:865`。全树**零**个非声明的调用点（`Get-ChildItem -Recurse | Select-String "restart_ice\(\)"` 只命中 `engine.hpp:3597` 的文档与 `engine.cpp:865` 的定义）—— 也就是说**今天没有任何人自动或手动地用它**。
- **`restart_ice()` 的两步契约**：公开头 `engine.hpp:3597` 明写 `restart_ice()` 然后 `send_to_peer(engine.create_offer())`。引擎**不知道**宿主的信令通道，所以第二步**必须**由宿主做。
- **`set_on_ice_restart` 的语义是「对端」**：`engine.hpp:3610` 明写 "the *peer* restarts ICE"；实现在 `engine_transport.cpp:517`，由 ICE 模块在**对端**换了凭据时触发。
- **ICE 模块的 `restart()` 会重建 libjuice agent**：`src/modules/ice/src/ice.cpp:1531` → `rebuild_agent()` —— 重新收集候选、重新打洞，是**重**的操作。这正是 N3 需要 grace 窗与退避的原因。
- **ICE 状态枚举在插件接缝上**：`plugins::IceState`（`src/plugins/include/nimrtc/plugins/ice_transport.hpp:104`）有 `Disconnected` / `Gathering` / `Connecting` / `Connected` / `Completed` / `Failed` —— 引擎**只**通过接缝看状态，不需要 `nimrtc::ice` 的具体类型。
- **传输段错误码空着**：`src/core/include/nimrtc/core/engine_errors.hpp:74` 明写 "No transport-only codes are defined yet — add them here when the transport layer grows a richer error vocabulary"。
- **N4 的失败原因词汇已经在 stats 里**：`Stats::ice_failure_reason`（`engine.hpp:2319`）是闭合词汇 `"none"` / `"no-candidates"` / `"no-pair"` / `"checks-failed"` / `"credentials-rejected"` / `"transport-unsupported"` —— N3 的终态原因是**另一个**维度（重连预算），不混进那个词汇。
- **私有协作者的先例**：只读 `src/engine/src/engine_capture_reconnect.hpp`（D4 的采集重连退避）—— 同一形状：私有头、时钟**显式传参**、确定性单元测试钉住规则。

### 与 N8 的边界（本片**不**做）

§N8 要做的是「路径变更上报」：old pair / new pair / reason（`"local-address-changed"` / `"peer-restart"` / `"relay-fallback"`）。N8 会在 `IceRecoveryReport` 上**加**字段。

**本片不做**：
- 不加 `old_pair` / `new_pair` 字段（那是 N8）；
- 不把 `reason` 扩到 `"relay-fallback"`（那是 N8）；
- 不做 call-state 枚举与 `reconnecting` 状态（那是 D7）。

**本片要做**的是 N3 自己这一层：`"local-address-changed"` / `"ice-failed"` / `"manual"` 三个 reason（够 N3 自己区分「为什么重启」），以及终态上报。

### 与 D7 的边界（本片**不**做）

D7 的 call-state 回调（`connecting` → `connected` → `reconnecting` → `failed` / `closed`）与错误码枚举（`kIceFailed` / `kDtlsFailed` / ...）是 D7 的活。本片只加**一个**传输段错误码 `kIceRecoveryExhausted`（传输段本来空着），**不**碰 D7 的枚举。

## 范围

- src/engine/src/engine_ice_recovery.hpp
- src/engine/src/engine_ice_recovery.cpp
- src/engine/src/engine_transport.cpp
- src/engine/src/engine.cpp
- src/engine/src/engine_session_state.hpp
- src/engine/src/engine_transport_stats.cpp
- src/engine/include/nimrtc/engine/engine.hpp
- src/core/include/nimrtc/core/engine_errors.hpp
- src/engine/CMakeLists.txt
- src/engine/tests/test_engine_ice_recovery.cpp
- tests/acceptance/v1-ice-recovery/acceptance.test.cjs
- docs/adr/ADR-031-ice-recovery-policy-ownership.md
- docs/api/engine_api.md
- CHANGELOG.md

> 只读 `specs/slices/v1-video-render-engine.md`（D2 的规格）与只读 `specs/slices/v3-n4-d6-connectivity.md`（D6/N4 的规格）—— 本片**不**改它们。N8（old/new pair）与 D7（call-state 枚举）**不在**本片范围，越界会被门禁拦下。

## 方案

### 一、私有协作者：`src/engine/src/engine_ice_recovery.{hpp,cpp}`

**私有**头（不进 `docs/api/public_headers.manifest` 的公开面），只 include `core` / `plugins`，**不** include `nimrtc::ice` 的具体符号。

```cpp
namespace nimrtc::engine::detail {

/** 重连策略的取值（引擎内部）。 */
enum class IceRecoveryPolicy : std::uint8_t {
    kOff = 0,   ///< 宿主自己管
    kAuto = 1,  ///< 引擎管（默认）
};

/** 解析 policy 名字。未知名字 → kOff（ADR-031 §2.2）。 */
IceRecoveryPolicy resolve_ice_recovery_policy(std::string_view name) noexcept;

/** 重连状态机。 */
class IceRecovery {
public:
    struct Config {
        /** Disconnected 上的宽限窗（微秒）。默认 5 s。 */
        std::int64_t grace_us = 5'000'000;
        /** 第一次退避（微秒）。默认 1 s。 */
        std::int64_t initial_backoff_us = 1'000'000;
        /** 退避上限（微秒）。默认 8 s。 */
        std::int64_t max_backoff_us = 8'000'000;
        /** 预算窗口（微秒）。默认 10 分钟。 */
        std::int64_t budget_window_us = 600'000'000;
        /** 窗口内最多几次重启。默认 5。 */
        std::uint32_t max_restarts = 5;
    };

    enum class State : std::uint8_t {
        kIdle = 0,      ///< 正常
        kGrace = 1,     ///< 掉线，宽限窗内
        kBackoff = 2,   ///< 宽限窗过了，等退避到期
        kTerminal = 3,  ///< 预算用尽，放弃
    };

    // ... 构造、config()、state()、restarts_used()、remaining_budget()
    //     terminal_reason()、reset_budget()、以及下面这组纯函数式入口：
};
```

**时钟显式传参**（与只读 `src/engine/src/engine_capture_reconnect.hpp` 同一条规则）：所有入口拿 `now_us`，测试用虚拟时钟，**不 sleep**。这是「A deterministic test on a virtual clock asserting grace, backoff, cap, terminal state」（§6 N3 验收标准）的前提。

**状态机**：

```
kIdle
  │ ICE → Disconnected（第一次掉线）
  ▼
kGrace ──── grace_us 内 ICE 回到 up ────► kIdle（计数清零？不，见下）
  │ grace_us 到期
  ▼
kBackoff ── backoff 到期 ──► 引擎执行 restart_ice()
  │                          │
  │                          ├─ 成功 → kGrace（重新等掉线）or kIdle
  │                          └─ 失败 → 退避加倍（cap 8 s）→ kBackoff
  │ 预算用尽
  ▼
kTerminal ──► 上报（on_error_ + stats + warn），不再循环
```

**三个必须钉死的语义**：

1. **grace 窗内恢复 → 计数**清零**吗？** **清零**。grace 窗内自己回来了，说明网络抖了一下，这**不是**一次「重启尝试」。与 D4 的 `note_succeeded()` 同族（成功即重置）。计数只在**真的执行了重启**之后才 +1。

2. **预算窗口是滑动的。** 每次**执行**一次重启，把窗口起点推到 `now + budget_window_us`。不是从会话开始算的一次性配额 —— 否则一个跑了 2 小时的会话突然掉线时会发现预算早被历史消耗光了（ADR-031 §2.5）。

3. **`restart_ice()` 失败也消耗预算吗？** **消耗**。失败的重启同样是一次尝试，同样是重建 libjuice agent 的代价。同样它也会让退避加倍。

### 二、`EngineConfig::ice_recovery_policy`

```cpp
/** 重连策略：v1.4 N3。
 *
 *  "auto"（默认）—— 引擎管：ICE 掉线后引擎按退避策略重启 ICE，
 *                   并通过 set_on_ice_recovery 告诉宿主该重新协商了。
 *  "off"        —— 引擎不管：ICE 掉线时引擎什么都不做。
 *
 *  未知名字当 "off"（ADR-031 §2.2）—— 与 connectivity_profile 的
 *  fallback 方向相反，是刻意的：打错的 key 不该让引擎在宿主没预期的
 *  时刻偷偷重启 ICE。 */
std::string ice_recovery_policy = "auto";
```

**与 `connectivity_profile` 正交**（ADR-031 §3.3）：一个是「怎么连」，一个是「断了怎么办」。两个 key 默认都开。

### 三、新增回调 `set_on_ice_recovery`

```cpp
/** 本地 ICE 重启的结果（v1.4 N3）。
 *
 *  这个结构是 N3 的落点；N8 会**加** old_pair / new_pair 两个字段
 *  （加法，不改已有字段的语义）。 */
struct IceRecoveryReport {
    /** 为什么重启："local-address-changed" / "ice-failed" / "manual"。 */
    std::string reason;
    /** 本次预算窗口内已用的重启次数（含这一次）。 */
    std::uint32_t restart_count;
    /** 预算内还剩几次（0 = 已达终态）。 */
    std::uint32_t remaining_budget;
    /** 终态：预算用尽，重连不再继续。 */
    bool terminal = false;
    /** 终态原因（空 = 未达终态）："budget-exhausted" / "restart-failed" / "manual-blocked"。 */
    std::string terminal_reason;
};

using EngineIceRecoveryCallback = std::function<void(const IceRecoveryReport&)>;

NIMRTC_API_STABLE void set_on_ice_recovery(EngineIceRecoveryCallback cb) noexcept;
```

**`set_on_ice_restart` 一个字都不动**（ADR-031 §2.1）。两者的宿主动作**相反**：对端重启 → 回 answer；本地重启 → 发 offer。

**回调跑在 tick 线程**（引擎拥有时钟的那条），与 `set_on_state_change` / `set_on_control_message` 同一套线程规则。

### 四、`restart_ice()` 计入同一预算

```cpp
NIMRTC_API_STABLE bool restart_ice() noexcept;   // 签名不变，行为变
```

- 预算内 → 重启，返回 `true`，**触发** `set_on_ice_recovery`（`reason = "manual"`）；
- 预算已满（终态） → **不重启**，返回 `false`，报终态（`on_error_` + stats + warn），`terminal_reason = "manual-blocked"`；
- 没有 ICE transport（今天的行为） → 返回 `false`（**不变**）。

**两步契约不变**：`restart_ice()` 然后 `send_to_peer(engine.create_offer())`。引擎**不**代劳第二步。

### 五、`reset_ice_recovery_budget()`

```cpp
/** 重置重连预算（v1.4 N3）。
 *
 *  终态之后引擎不再自动重启 ICE。宿主想重新给一次机会（用户点了
 *  「重试」）显式调这个方法。
 *
 *  预算窗口（10 分钟）**不**随它清零 —— 时间窗是滑动窗口，
 *  清零的只是「已用次数」与「终态标志」。 */
NIMRTC_API_STABLE void reset_ice_recovery_budget() noexcept;
```

### 六、终态三处报告

**（a）`on_error_` 回调** + 新错误码：

```cpp
// src/core/include/nimrtc/core/engine_errors.hpp
inline constexpr NIMRTC_API_STABLE std::uint32_t kIceRecoveryExhausted = 0x1F01;
```

**（b）`Stats` 三个字段**：

```cpp
/** 重连状态（v1.4 N3）："idle" / "grace" / "backoff" / "terminal" / "disabled"。 */
std::string ice_recovery_state;
/** 本次预算窗口内已用的重启次数。 */
std::uint32_t ice_recovery_restarts;
/** 终态原因（空 = 未达终态）："budget-exhausted" / "restart-failed" / "manual-blocked"。 */
std::string ice_recovery_terminal_reason;
```

**（c）一行 `warn` 日志**：与 D2 的降级日志同族（「never a silent drop」）。

**明确不加**：`old_pair` / `new_pair`（N8）、call-state 枚举（D7）。

### 七、接线：引擎的 tick

`poll_ice_flap_for_rehandshake()` 已经在 tick 里观察 ICE 状态变化（`engine_transport.cpp:1336` 调用，`:1720` 实现）。N3 的观察点**同一个地方** —— 但**独立**的状态机，**不**改 DTLS rehandshake 的逻辑。

- `Disconnected` 且 `ice_connected_once`（真的连上过）→ 进入 `kGrace`；
- grace 窗内回到 up → `kIdle`；
- grace 窗到期 → `kBackoff`，退避到期 → 引擎调 `ice_t_->restart()`（**引擎内部**，不是公开的 `restart_ice()`，免得递归）；
- `Failed` 状态 → 立刻进入 `kBackoff`（**跳过 grace**，因为 ICE 明确失败了，不是抖动）；
- 每次重启后 → `set_on_ice_recovery`。

**`ice_connected_once` 的门槛**：从未连上过就掉线（会话根本就没建起来）**不是**「网络变了」，是「会话没成」—— 那属于 N4 的失败诊断，**不**该触发重启风暴。这是 grace 之外的**第二道**闸门。

## 设计

**为什么是私有协作者**：与 `FeedbackGuard`（Q4）、`CaptureReconnect`（D4）同一族 —— 规则微妙、需要确定性测试钉住、且**不该**长在公开头上。规则与测试必须**是同一份规则**，所以两者都 include 这个头。

**为什么时钟显式传参**：「A deterministic test on a virtual clock asserting grace, backoff, cap, terminal state」（§6）。墙钟闸门没法确定性测试。

**为什么 `restart_ice()` 走公开路径而内部重启不走**：内部自动重启直接调 `ice_t_->restart()` + 自己记账；公开 `restart_ice()` 走**同一套**记账（ADR-031 §2.3）。两者共享同一个 `IceRecovery` 实例，因此预算是一份。

**为什么不用 `nimrtc::ice` 的具体类型**：L4（引擎只做编排）。ICE 状态通过 `plugins::IceState` 接缝读，重启通过 `plugins::IICETransport::restart()` 接缝调 —— 与 D1/D2 的接缝纪律一致。

### 破坏面

- `restart_ice()` 的**行为**变了（预算满 → 返回 `false`）。全树**零**个调用点，所以**没有**现有代码受影响；未来的宿主需要知道这条新规则（写进 `docs/api/engine_api.md`）。
- `EngineConfig` 加字段 → 所有构造 `EngineConfig` 的地方**照旧**编译（聚合初始化的具名设计器不受影响；只有**聚合位置初始化**会受影响 —— 全树没有这种写法，因为 `EngineConfig` 有几十个字段）。
- `Stats` 加字段 → 同上，具名赋值。
- 公开头加符号 → `docs/api/public_headers.manifest` 的**头文件列表不变**（不新增头文件），但只读 `scripts/api_freeze_scan.js` 的**符号分类**要更新（新符号必须是 `NIMRTC_API_STABLE` 或 `NIMRTC_API_UNSTABLE`）。

## 写集

| 文件 | 改什么 |
|---|---|
| `src/engine/src/engine_ice_recovery.hpp` | **新增**（私有）：`IceRecoveryPolicy` / `resolve_ice_recovery_policy()` / `IceRecovery` 状态机 |
| `src/engine/src/engine_ice_recovery.cpp` | **新增**（私有）：上者的实现 |
| `src/engine/src/engine_transport.cpp` | tick 里驱动 `IceRecovery`；自动重启；触发回调 |
| `src/engine/src/engine.cpp` | `restart_ice()` 计入预算；`set_on_ice_recovery()` / `reset_ice_recovery_budget()` |
| `src/engine/src/engine_session_state.hpp` | 加 `IceRecovery` 实例（或指向它的成员） |
| `src/engine/src/engine_transport_stats.cpp` | 填三个 stats 字段 |
| `src/engine/include/nimrtc/engine/engine.hpp` | `EngineConfig::ice_recovery_policy`；`IceRecoveryReport` / `EngineIceRecoveryCallback`；`set_on_ice_recovery()` / `reset_ice_recovery_budget()`；`Stats` 三个字段 |
| `src/core/include/nimrtc/core/engine_errors.hpp` | 新增 `kIceRecoveryExhausted = 0x1F01` |
| `src/engine/CMakeLists.txt` | 加 `src/engine_ice_recovery.cpp` |
| `src/engine/tests/test_engine_ice_recovery.cpp` | **新增**：确定性单元测试（虚拟时钟） |
| `tests/acceptance/v1-ice-recovery/acceptance.test.cjs` | **新增**：验收测试 |
| `docs/adr/ADR-031-ice-recovery-policy-ownership.md` | **新增**：ADR |
| `docs/api/engine_api.md` | 新 key / 新回调 / 新 stats 字段 / `restart_ice()` 的新行为 |
| `docs/api/public_headers.manifest` | 头文件列表**不变**（不新增头）—— 跑 `scripts/check_api_manifest.py` 确认，**不改** |
| `CHANGELOG.md` | 记一笔 |

## 验收标准

（每条都必须是**可判定**的。）

### 一、策略开关

1. 默认构造的 `EngineConfig::ice_recovery_policy == "auto"`。
2. `ice_recovery_policy = "off"` → 引擎**不**自动重启（ICE 掉线后 `ice_restart_count()` 不变）。
3. `ice_recovery_policy = "no-such-policy"` → 按 `"off"` 处理（**不是** `"auto"`）。
4. `ice_recovery_policy = ""` → 按 `"off"` 处理。

### 二、状态机（虚拟时钟，单元测试）

5. ICE 从 up 掉到 `Disconnected` → 状态进 `kGrace`；**不**立刻重启。
6. grace 窗（5 s）内回到 up → 状态回 `kIdle`，**且不消耗任何预算**（`restarts_used() == 0`）。
7. grace 窗到期 → 状态进 `kBackoff`；**第一次退避是 1 s**。
8. 退避序列依次为 **1 / 2 / 4 / 8 s**；**第 5 次起封顶在 8 s**（不再翻倍）。
9. 连续掉线 → 重启计数逐次 +1；**第 5 次**之后进入 `kTerminal`（**不多于** 5 次）。
10. 终态后再掉线 → **不再重启**（计数停在 5）。
11. 预算窗口是**滑动的**：每次重启把窗口往后推 10 分钟；窗口外的旧重启**不**占预算。
12. `restart()` 失败也**消耗**预算，且退避**加倍**。

### 三、自动重启（引擎侧）

13. ICE 掉线 + grace 到期 → 引擎**自动**调用 ICE 重启（`ice_restart_count()` 增加），**宿主全程没调 `restart_ice()`**。
14. ICE 进 `Failed` → **跳过 grace**，直接进 `kBackoff`。
15. **从未连上过**的会话掉线（`ice_connected_once == false`）→ **不**触发重启风暴（不是重连场景）。
16. 自动重启成功后 → 触发 `set_on_ice_recovery`，`reason == "local-address-changed"`（掉线场景）。

### 四、手动 `restart_ice()` 计入同一预算

17. 预算内的 `restart_ice()` → 返回 `true`；重启计数 +1；触发 `set_on_ice_recovery`，`reason == "manual"`。
18. 预算用尽（终态）后 `restart_ice()` → 返回 `false`，**不重启**（计数不变），报终态（`terminal_reason == "manual-blocked"`）。
19. `restart_ice()` 的**两步契约不变**：`engine.hpp` 的文档仍然是 `restart_ice()` 然后 `send_to_peer(engine.create_offer())`（静态检查：文档文本包含 `send_to_peer`）。
20. `reset_ice_recovery_budget()` → 计数清零、终态标志清除、状态回 `kIdle`；**但预算窗口不重置**（窗口起点仍按滑动规则）。

### 五、终态三处报告

21. 终态 → `on_error_` 被调用一次，错误码 == `kIceRecoveryExhausted`。
22. 终态 → `Stats::ice_recovery_state == "terminal"`；`ice_recovery_terminal_reason == "budget-exhausted"`。
23. 终态 → 日志里有一行 `warn`。
24. 非终态 → `Stats::ice_recovery_terminal_reason` 为**空串**。
25. `ice_recovery_policy == "off"` → `Stats::ice_recovery_state == "disabled"`。

### 六、边界与兼容

26. `set_on_ice_restart` 的**签名与语义不变**（静态检查：公开头里它的文档仍写着 "peer"，且没有新参数）。
27. `set_on_ice_recovery` 传 `nullptr` → 清除回调，不崩。
28. 引擎未 `pre_open()` 时 `restart_ice()` → 返回 `false`（**今天的行为不变**）。
29. `src/engine/` **不** include `nimrtc/ice/` 的任何具体头（接缝纪律，L4）。
30. `src/engine/tests/test_engine_*.cpp` 一整套在新增后**仍然通过**（回归）。

### 七、文档与契约

31. `docs/api/engine_api.md` 记录：新 key 的取值与**未知名字当 `"off"`** 的规则；新回调与结构体；三个 stats 字段；`restart_ice()` 的新行为（预算满 → `false`）；`reset_ice_recovery_budget()`。
32. `docs/api/public_headers.manifest` 与实际公开面一致（只读 `scripts/check_api_manifest.py` — 跑它，不改它；本片**不新增头文件**，所以清单**应当不变**）。
33. 新增的公开符号全部带 `NIMRTC_API_STABLE` 或 `NIMRTC_API_UNSTABLE` 标注（跑只读 `scripts/api_freeze_scan.js` — 跑它，不改它）。

## 本片不做

- **不做** old/new pair 的上报 —— 那是 N8，它会在 `IceRecoveryReport` 上**加**字段。
- **不做** call-state 枚举与 `reconnecting` 状态 —— 那是 D7。
- **不做** `"relay-fallback"` 这个 reason —— 那是 N8。
- **不做** 引擎代劳信令（自动 `create_offer()` + 发送）—— §N8-2 已决定 report only。
- **不做** 采集侧的重连（D4 已做，采集与 ICE 两条重连链**不合并** —— 它们的触发条件、代价、终态语义都不同）。
- **不做** 多路径并行探测（happy eyeballs 之外的 candidate 级竞速）—— 不在 §N3 的范围。
- **不改** `plugins::IICETransport` 接口 —— `restart()` / `restart_count()` / `state()` 都已经在接缝上。
- **不做** 凭据刷新 —— 那是 N7。

## 依赖方向检查

- 新增的私有头 `engine_ice_recovery.hpp` 只 include `core` / `plugins` 的东西，**不** include `nimrtc/ice/`。
- `src/core/` 只在**已有的** `engine_errors.hpp` 里加一个常量，不新增文件、不改判定逻辑。
- `src/engine/` 继续只做编排（L4）；ICE 的重启通过 `plugins::IICETransport::restart()` 接缝调。
- 判定只读状态、纯计算、时钟显式传参（C3：不调模型、不触网、确定性）。
