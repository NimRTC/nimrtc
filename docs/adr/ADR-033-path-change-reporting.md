# ADR-033: 路径变更上报 —— 报告「从哪到哪、为什么变」，只报告不代劳

| | |
|---|---|
| Number | 033 |
| Status | **Proposed** |
| Date | 2026-11-24 |
| Phase | v1.4 (N8) |
| Bound to | v1.4.0 |
| Supersedes | — |
| Related | `docs/plan/v1.4-plan.md` §N8；`docs/adr/ADR-031-ice-recovery-policy-ownership.md`（N3，报告载体）；`docs/adr/ADR-032-turn-credentials-provider-seam.md`（N7）；`docs/adr/ADR-015-api-stability-policy.md`；`specs/slices/v1-path-change-report.md` |

---

## 1. 背景

N3（ADR-031）让引擎能**自动重启 ICE**（退避 + 上限 + 终态），并把「该重新协商了」通过
`set_on_ice_recovery` 告诉宿主。但 N3 的报告只有 `reason` —— 宿主不知道**从哪条路换到哪条路**。
今天宿主想知道路径变化，只能轮询 `Stats::transport.selected_pair` 再 diff 字符串，才能猜出
「正在恢复」还是「已经恢复」。

计划书 §N8 要求：**路径切换时引擎回调宿主，报告里带 old pair / new pair / reason / 已用重连次数**。
RFC 8445 §9.1.1.1 是物理基础：本地重启会换掉凭据与 ufrag/pwd，对端**必须**重新协商 —— 引擎自己
修不了远端，SDP 交换是宿主信令的事。§N8-2 已决：**report only**（引擎只报告，不代劳）。

写规格时的树状态（`specs/slices/v1-path-change-report.md` §现状核实）：

- `IceCandidatePairInfo` 已是公开面的一等公民（`engine.hpp:2286`），两端各带
  `address` / `port` / `protocol` / `type` —— N8 不新造 pair 类型。
- `IceRecoveryReport`（`engine.hpp:3724`）已预留位置：注释明写 *"N8 will **add**
  `old_pair` / `new_pair` fields"* —— N8 是**纯加字段**。
- 对端重启的检测已在接缝上（`ice_t_->set_on_ice_restart`，`plugins::OnIceRestart =
  std::function<void()>`，**无载荷**）；当前 pair 的来源是 `ice_t_->selected_pair()`。

## 2. 决策

### 2.1 决策一：`IceRecoveryReport` 加 `old_pair` / `new_pair`（纯加法）

在 N3 的 `IceRecoveryReport` 上**新增**两个 `std::optional<IceCandidatePairInfo>` 字段：

```cpp
/** 切换**前**的 pair。空 = 之前没有选中过 pair（第一次连接）。 */
std::optional<IceCandidatePairInfo> old_pair;
/** 切换**后**的 pair。空 = 新 pair 还没选出来（重启后还在 gathering）。 */
std::optional<IceCandidatePairInfo> new_pair;
```

既有字段（`reason` / `restart_count` / `remaining_budget` / `terminal` / `terminal_reason`）
语义**不变**。

**为什么是 `std::optional` 而不是空哨兵**：pair 是**可能不存在**的事实 —— 之前没选中过、或
新 pair 还没选出来。空字符串或哨兵值会让宿主猜「空是『没变』还是『不知道』」；`std::optional`
把「有没有」说清楚（规格 §一）。

**为什么落在 N3 的 `IceRecoveryReport` 而不是新建一个本地重启报告**：N3 的 `set_on_ice_recovery`
回调已经承载「本地重启发生了」，N8 只是给它补上路径细节 —— 同一个事件、同一个回调、同一个
结构，宿主一处监听拿全（ADR-031 §2.1 已把这个结构定为 N8 的落点）。

**ABI**：ADB-015 Decision 4 —— MINOR bump（v1.3 → v1.4）**允许** ABI 变化（`Callers must
rebuild`），本片是**加字段**（additive，无 STABLE 符号移除），符合该口径。`Stats` 按值传参、
加字段会改大小 —— 在 `CHANGELOG.md` 里写明宿主需重编（规格 §风险）。

### 2.2 决策二：新增独立回调 `set_on_peer_restart`，**不**改 `set_on_ice_restart` / 接缝

对端重启（`set_on_ice_restart` 触发）时，宿主今天拿不到任何上下文。N8 要在引擎侧**新增**一个
回调带上 pair：

```cpp
/** 对端重启 ICE 时报告给宿主的路径上下文（v1.4 N8）。 */
struct NIMRTC_API_STABLE PeerRestartReport {
    std::optional<IceCandidatePairInfo> old_pair;
    std::optional<IceCandidatePairInfo> new_pair;
    /** "peer-restart"（本片只有这一个值，闭合词汇留位）。 */
    std::string reason;
};

using NIMRTC_API_STABLE EnginePeerRestartCallback =
    std::function<void(const PeerRestartReport&)>;

NIMRTC_API_STABLE void set_on_peer_restart(EnginePeerRestartCallback cb) noexcept;
```

**为什么加新回调而不给 `set_on_ice_restart` 加参数**：

1. `set_on_ice_restart` 是**已发布**的公开 API（ADR-015：ABI 稳定、只增不改）。改签名 =
   ABI 破坏。加新回调是纯加法。
2. **接缝不动**：`plugins::OnIceRestart` 保持 `std::function<void()>`。改接缝会让 ICE 模块
   也要改，且破坏插件 ABI —— 本片在接缝**之上**（规格 §边界：不改 `src/plugins/`）。
3. 旧回调**继续照常触发** —— 依赖它的宿主不受影响（验收判据 11 钉住：新旧两个回调都触发）。

**对端重启的 pair 怎么取**：对端重启是**对方**换了凭据，本端的 pair **通常没变** —— 报告里
`old_pair` 与 `new_pair` 都取**当下**的 `current_pair()`（同一对）。`new_pair` 空是合法的：
本端路径没变、要重协商的是对端（规格 §风险）。这不引入第二次对 `selected_pair()` 的采样。

### 2.3 决策三：路径判定的私有协作者 `PathChangeTracker`（纯函数）

「哪个 pair 是**之前**的」需要跨时刻记账：引擎在重启**前**记下当前 pair，重启**后**取新 pair
比对。按 N3 的形状（私有 TU、确定性单测），新增 `src/engine/src/engine_path_change.{hpp,cpp}`：

```cpp
namespace nimrtc::engine::detail {

class PathChangeTracker {
public:
    void note_before(std::optional<IceCandidatePairInfo> pair) noexcept;
    const std::optional<IceCandidatePairInfo>& before() const noexcept;
    void reset() noexcept;

    static std::string_view classify(
        const std::optional<IceCandidatePairInfo>& old_pair,
        const std::optional<IceCandidatePairInfo>& new_pair) noexcept;
    static bool same_path(const IceCandidatePairInfo& a,
                          const IceCandidatePairInfo& b) noexcept;
};
}
```

**`classify` 的判定**（规格 §四）：

| old_pair | new_pair | 结果 |
|---|---|---|
| 空 | 空 | `""`（没有路径变化可言） |
| 空 | 有 | `"local-address-changed"`（第一次选中） |
| 有 | 空 | `"local-address-changed"`（pair 掉了） |
| 有 | 有，中继位不同 | `"relay-fallback"` |
| 有 | 有，中继位相同 | `"local-address-changed"` |

**中继位的判据与 `TransportStats::relayed` 同源**：两端任一 `type == "relay"` 就是中继路径。
判据放在 `PathChangeTracker::classify` 里，单测钉住与 `relayed` 一致（规格 §风险）。

**为什么判定在机器里而不是调用点**：N3 重连重启、手动重启、对端重启是**三个**调用点；判定
散在三处就会漂 —— 同一件事三处给出三个 reason。放进一个纯函数，三处共用一条判据
（规格 §四）。`classify` 不持有时钟、不碰 transport —— C3（判定必须确定性）。

### 2.4 决策四：`classify` 是**细化**，不是**替代** —— 只有 `"relay-fallback"` 覆盖事件 reason

三处接线报告 `reason` 时，规则是：

> 事件自带的 reason（`"manual"` / `"local-address-changed"` / `"ice-failed"`）**默认保留**；
> 只有当 `classify(old, new) == "relay-fallback"` 时，reason 被**细化**成 `"relay-fallback"`。

**为什么**：N3 的 `"local-address-changed"`（ICE drop 触发）与 `"manual"`（宿主手动调）的语义
是**事件**层面的 —— 「这件事为什么发生」，N8 的 `"relay-fallback"` 是**路径形态**层面的 ——
「从直连切到中继」。两者正交：一次 ICE drop 可能是直连→中继，也可能不是。**保留事件 reason**
让 N3 既有宿主的语义不变（验收判据 10/11：手动 → `"manual"`、对端重启 → `"peer-restart"`）；
**细化成 relay-fallback** 让验收判据 14 成立。若 `classify` 全量替代事件 reason，
`"manual"` 就消失了 —— 那会破 N3 的契约。

实现上：`fire_ice_recovery_report` 里先按 N3 的逻辑填 reason，再
`if (classified == "relay-fallback") report.reason = std::string(classified);`。
对端重启路径的 reason 恒为 `"peer-restart"`，不受细化影响。

### 2.5 决策五：`Stats` 侧留「最后一次路径变更」（扁平 `Stats`）

宿主除了回调，也常常**事后**想知道「刚才换路了吗」。在**扁平** `Stats` 上留最后一条：

```cpp
/** 最后一次路径变更的 reason（v1.4 N8）：空 = 没发生过路径变更。
 *  取值与 IceRecoveryReport::reason 同一套词汇。 */
std::string last_path_change_reason;
/** 最后一次路径变更后的 pair（空 = 没有或还没选出来）。 */
std::optional<IceCandidatePairInfo> last_path_change_new_pair;
```

**为什么放扁平 `Stats` 而不是 `Stats::transport`**：与 N3 的 `ice_recovery_*`、N7 的
`turn_credentials_state` 同理 —— 这是**策略事实**（发生过一次路径变更），不是**传输测量**
（此刻的 RTT / 候选数）。宿主读 `stats().last_path_change_reason` 不该先知道策略躲在
`stats().transport` 后面（ADR-031 §2.4 同一判断）。

**投影时机**：`append_ice_recovery_stats` 在 policy-switch **之前**投影这两个字段 —— 即使
`ice_recovery_policy == "off"`（引擎不管重连），手动/对端重启的路径变更**也要**留痕
（`restart_ice()` 在 policy="off" 下仍工作）。宿主不依赖 policy 就能读到路径变更。

### 2.6 决策六：路径变更**不是失败** —— 不碰 `ice_failure_reason`

路径变更走的是**成功路径**上的回调与 Stats，**不**被报成 `ice_failure_reason` 的任何值
（规格 §七）。`ice_failure_reason` 继续只由 `TransportStatsProjector` 按失败输入计算
（pair 选中数、重启计数、超时等）；`append_ice_recovery_stats` 不写它。

计划 §N8-1 说：重启**失败**且 N3 到终态时，报**最后一对** + reason + 到 `"failed"` 的
call-state 迁移 —— 「和终态本身的那份报告一样，不新增事件类型」。call-state 枚举是 D7 的活，
本片只保证「终态报告里带最后一对」（`old_pair` = 记账的 before；终态时 `new_pair` 通常空，
表示再没选出来）。

## 3. 后果

### 3.1 公开面（全部是加法，无 ABI 破坏）

| 符号 | 变化 |
|---|---|
| `IceRecoveryReport::old_pair` / `new_pair` | 新增字段（`std::optional<IceCandidatePairInfo>`） |
| `PeerRestartReport` | 新增类型（`old_pair` / `new_pair` / `reason`） |
| `EnginePeerRestartCallback` | 新增类型别名 |
| `NimRTCEngine::set_on_peer_restart()` | 新增方法 |
| `Stats::last_path_change_reason` / `last_path_change_new_pair` | 新增字段（扁平 `Stats`） |

`set_on_ice_restart` / `plugins::OnIceRestart` / `plugins::IICETransport` **不动**。
`Stats` 结构体**大小变了**（按值传参）—— MINOR bump 内允许，`CHANGELOG.md` 写明宿主需重编。

### 3.2 依赖方向

- 新增私有协作者 `src/engine/src/engine_path_change.{hpp,cpp}`：只 include `core` / `plugins`
  的东西，**不** include `nimrtc::ice` 的具体头（L4 接缝纪律，与 `engine_ice_recovery.hpp` 同形）。
- `src/modules/ice/`、`src/plugins/` **不动** —— 本片在接缝**之上**。
- 三处接线都在引擎内：`execute_ice_recovery_restart()`（N3 重连重启）、`restart_ice()`（手动）、
  `set_on_ice_restart` 转发（对端）。全部由引擎 tick 线程驱动（验收判据 21 钉住线程 id）。

### 3.3 与 N3 / N7 / D7 的关系

- **N3**：`IceRecoveryReport` 的既有字段语义不变；`restart_count` / `remaining_budget` /
  `terminal` 原样带在报告里（验收判据 12）。
- **N7**：`execute_ice_recovery_restart()` 已被 N7 加了 `refresh_turn_credentials()`；N8 只在这
  个函数里**重启前**多记账一次，不碰 N7 的逻辑。
- **D7**：D7 的 call-state 回调会读 `last_path_change_reason` 与终态报告 —— N8 不预留也不猜测
  D7 的枚举（ADR-031 §3.5 同款措辞）。

## 4. 备选方案与不选的理由

| 备选 | 不选的理由 |
|---|---|
| 改 `plugins::OnIceRestart` 带载荷 | 破接缝 ABI，ICE 模块也要改；本片在接缝之上 |
| 给 `set_on_ice_restart` 加参数 | ABI 破坏（ADR-015）；且新旧语义混在一个回调里 |
| 新建「本地重启」报告类型 | N3 的 `set_on_ice_recovery` 已承载同一事件；重复载体让宿主多处监听 |
| `classify` 全量替代事件 reason | `"manual"` / `"peer-restart"` 会消失，破 N3 契约 |
| 路径变更放进 `Stats::transport` | 是策略事实不是传输测量；宿主读错路径 |
| 只用回调不留 `Stats` 字段 | 事后轮询的宿主拿不到「刚才换路了吗」 |
| 用空字符串表示「pair 不存在」 | 宿主猜「没变还是不知道」；`std::optional` 说清有没有 |

## 5. 落地时对规格的几处实现取舍

以下三处规格没写死，实现按本 ADR 定：

1. **peer-restart 报告里 old == new**：对端重启换的是**对方**凭据，本端路径通常不变 ——
   `old_pair` 与 `new_pair` 都取当下的 `current_pair()`。这不是 bug，是语义。
2. **`new_pair` 空的两种情形**：重启后还在 gathering（没选出来）＝「还没选好」；终态报告里
   `new_pair` 空＝「再没选出来」。宿主靠 `terminal` 字段区分。
3. **`last_path_change` 在 policy="off" 下也投影**：路径变更留痕不依赖 `ice_recovery_policy`。
