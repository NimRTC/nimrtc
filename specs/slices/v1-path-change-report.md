# Slice: v1-path-change-report

## Summary

**路径变了要告诉宿主，而且要说清「从哪变到哪、为什么变」。** N3 让引擎能在本地重连（退避 + 上限 + 终态），但重连之后引擎只是「换了条路继续跑」——宿主不知道换路这件事发生了，只能自己轮询 `Stats::transport.selected_pair` 再 diff 字符串，才能猜出「正在恢复」还是「已经恢复」。

N8 补上**本地那一半**：路径切换时引擎回调宿主，报告里带 **old pair / new pair / reason / 已用重连次数**。

理由落在 RFC 8445 §9.1.1.1 上：本地重启会同时换掉**凭据**和 **ufrag/pwd**，所以对端**必须**重新协商 —— 引擎自己修不了远端。SDP 交换是宿主信令的事，引擎只负责**报告**（计划 §5 **N8-2** 的已决：*report only*）。

架构决策在 `docs/adr/ADR-033-path-change-reporting.md`（**Proposed**，待签字）。

## 目标

**让宿主在路径切换的那一刻就知道，而且一次拿全。** 报告要回答三件事：

- **从哪到哪**（old pair / new pair，都是完整的 `IceCandidatePairInfo` —— 两端各带地址/端口/协议/类型）
- **为什么**（reason：`"local-address-changed"` / `"peer-restart"` / `"relay-fallback"` / `"manual"` / N3 已有的 `"ice-failed"`）
- **还剩多少余地**（N3 的 `restart_count` / `remaining_budget` / `terminal`）

这样宿主不需要轮询、不需要 diff 字符串、不需要猜。

### 现状核实（写规格时的树状态）

- **`IceCandidatePairInfo` 已经存在**：`engine.hpp:2286`，两端各是 `IceCandidateInfo`（`address` / `port` / `protocol` / `type`）—— N8 要的 pair 形状**已经在公开面上了**，不需要新造。
- **`IceRecoveryReport` 已经预留了位置**：`engine.hpp:3724` 的注释明写 *"N8 will **add** `old_pair` / `new_pair` fields (an addition — the existing fields' semantics do not change)"* —— N8 是**纯加字段**，不改既有字段的语义。
- **`set_on_ice_restart` 是对端那一半**：它现在**没有载荷**（`plugins::OnIceRestart = std::function<void()>`，见只读 `src/plugins/include/nimrtc/plugins/ice_transport.hpp`），对端换了凭据时触发但宿主拿不到任何上下文。
- **当前 pair 的来源**：`ice_t_->selected_pair()`（`engine_transport_stats.cpp:180`）—— 引擎已经在用它填 `Stats::transport.selected_pair`。
- **N3 的重启点**：`execute_ice_recovery_restart()`（`engine_transport.cpp`）在 ICE 重启**之后**调 `fire_ice_recovery_report()`；`restart_ice()`（`engine.cpp:916`）在手动重启后也调它 —— 这两处是 N8 报告 old/new pair 的**两个时机**。
- **对端重启的检测在引擎里**：`engine_transport.cpp:518` 的 `ice_t_->set_on_ice_restart([this] { if (on_ice_restart_) on_ice_restart_(); })` —— N8 要在这里带上 pair 上下文。

### 与其它片的边界（本片**不做**）

- **不做** SDP 重协商的编排 —— 引擎不生成 offer 片段、不发信令（计划 §5 **N8-2**：*report only*）。
- **不做** call-state 枚举 —— 那是 D7（`D7-1` 的 `kIceFailed` 等）。
- **不做** 改 `plugins::IICETransport` 接口 —— 对端重启的检测回调在接缝上已经有了。
- **不改** `src/modules/ice/` —— 本片在 ICE 接缝**之上**。
- **不做** N3 的重连策略本身 —— 那是 N3（本片只**报**它发生的事）。

## 范围

- src/engine/src/engine_path_change.hpp
- src/engine/src/engine_path_change.cpp
- src/engine/src/engine.cpp
- src/engine/src/engine_transport.cpp
- src/engine/src/engine_session_state.hpp
- src/engine/src/engine_transport_stats.cpp
- src/engine/include/nimrtc/engine/engine.hpp
- src/engine/CMakeLists.txt
- src/engine/tests/CMakeLists.txt
- src/engine/tests/test_engine_path_change.cpp
- tests/acceptance/v1-path-change-report/acceptance.test.cjs
- docs/adr/ADR-033-path-change-reporting.md
- docs/api/engine_api.md
- CHANGELOG.md
- specs/slices/v1-path-change-report.md

> 只读 `specs/slices/v1-ice-recovery.md`（N3 的规格）—— 本片**不**改它。
> 只读 `specs/slices/v1-turn-credentials.md`（N7 的规格）—— 本片**不**改它。
> 只读 `src/modules/ice/`（ICE 模块）—— 本片**不**改它。
> 只读 `src/plugins/`（接缝）—— 本片**不**改它。

## 设计

### 一、公开面：`IceRecoveryReport` 加两个字段

```cpp
// src/engine/include/nimrtc/engine/engine.hpp

struct NIMRTC_API_STABLE IceRecoveryReport {
    std::string reason;
    std::uint32_t restart_count = 0;
    std::uint32_t remaining_budget = 0;
    bool terminal = false;
    std::string terminal_reason;

    // ---- v1.4 N8：路径变更的 old / new pair ---------------------------
    // 之前这条路径（"local-address-changed"）—— 只有 reason，
    // 宿主拿不到「从哪到哪」。现在带完整的两端 pair。

    /** 切换**前**的 pair。空 = 之前没有选中过 pair（第一次连接）。 */
    std::optional<IceCandidatePairInfo> old_pair;

    /** 切换**后**的 pair。空 = 新 pair 还没选出来（重启后 ICE
     *  还在 gathering）。 */
    std::optional<IceCandidatePairInfo> new_pair;
};
```

**为什么用 `std::optional`**：pair 是**可能不存在**的事实 —— 之前没选中过（第一次连接）、或新 pair 还没选出来。用空字符串或哨兵值都会让宿主去猜「空是「没变」还是「不知道」」。`std::optional` 说清楚「有没有」。

**为什么是 `IceCandidatePairInfo`（现成的）**：它已经是公开面上一等公民（`engine.hpp:2286`），两端各带 `address` / `port` / `protocol` / `type`（`type` 是 `"host"` / `"srflx"` / `"prflx"` / `"relay"`）。N8 不新造类型，也就不引入第二套 pair 词汇。

### 二、reason 的词汇

N8 报告的 `reason` 是**闭合词汇**（宿主可以 `switch`）：

| reason | 什么时候 | 计划的依据 |
|---|---|---|
| `"local-address-changed"` | 本地地址变了（ICE drop 触发 N3 重连） | 计划 §N8 明确列的 |
| `"peer-restart"` | 对端换了凭据（`set_on_ice_restart` 触发） | 计划 §N8 明确列的 |
| `"relay-fallback"` | 从直连切到中继（或反过来） | 计划 §N8 明确列的 |
| `"manual"` | 宿主调 `restart_ice()` | N3 已有 |
| `"ice-failed"` | ICE agent 显式失败 | N3 已有 |

**新增的两个不与 N3 冲突**：N3 的 `"local-address-changed"` / `"ice-failed"` / `"manual"` 语义**不变**，N8 只是给它们**补上 pair**。

### 三、对端重启：`set_on_ice_restart` 带上载荷

今天 `plugins::OnIceRestart = std::function<void()>` —— **没有载荷**。N8 要让宿主在对端重启时也拿到 pair。

**做法**：在**引擎**侧加一个新回调，而不是改 `plugins::OnIceRestart`（改接缝会让 ICE 模块也要改，且 ABI 破）。引擎侧：

```cpp
// engine.hpp

/** 对端重启 ICE 时报告给宿主的路径上下文（v1.4 N8）。 */
struct NIMRTC_API_STABLE PeerRestartReport {
    /** 对端重启前的 pair（空 = 之前没选中过）。 */
    std::optional<IceCandidatePairInfo> old_pair;
    /** 对端重启后的 pair（空 = 新 pair 还没选出来）。 */
    std::optional<IceCandidatePairInfo> new_pair;
    /** "peer-restart"（本片只有这一个值，闭合词汇留位）。 */
    std::string reason;
};

using NIMRTC_API_STABLE EnginePeerRestartCallback =
    std::function<void(const PeerRestartReport&)>;

/** 安装对端重启的回调（v1.4 N8）。
 *  `set_on_ice_restart()` 保持原义（对端 SDP 变了）不变；
 *  这个回调额外带 pair 上下文。 */
NIMRTC_API_STABLE void set_on_peer_restart(EnginePeerRestartCallback cb) noexcept;
```

**为什么加新回调而不改旧的**：`set_on_ice_restart` 是**已发布的公开 API**（ADR-015：ABI 稳定、只增不改）。改它的签名就是 ABI 破坏。加一个新回调是纯加法，且旧回调**继续照常触发** —— 依赖它的宿主不受影响。

### 四、path-change 记��：私有协作 `PathChangeTracker`

「哪个 pair 是**之前**的」这件事需要跨时刻记一个值：引擎在 N3 重启**前**记下当前 pair，重启**后**取新 pair 比对。

按 N3 的形状（私有 TU、时钟显式传参、确定性单测）：

```cpp
// src/engine/src/engine_path_change.hpp
namespace nimrtc::engine::detail {

/** 路径切换的记账（v1.4 N8）。
 *
 *  引擎在**重启之前**记下当前 pair，在**重启之后**取新的 pair，
 *  两边一比就知道「从哪变到哪」。没有这个记账点，
 *  「之前的那条路」就会散在各个调用点、各自漂移。
 *
 *  它不碰 transport、不碰时钟：pair 由调用方取进来，
 *  reason 由调用方判定。 */
class PathChangeTracker {
public:
    /** 记下「切换前」的 pair（重启之前调）。 */
    void note_before(std::optional<IceCandidatePairInfo> pair) noexcept;

    /** 「切换前」记下的 pair（还没记过 = nullopt）。 */
    const std::optional<IceCandidatePairInfo>& before() const noexcept;

    /** 清空记账（open() / close() 时调 —— 每个会话从零开始）。 */
    void reset() noexcept;

    /** 判定 reason：新旧 pair 的变化属于哪一类。
     *
     *  - 两边都空 / 旧空新空 → ""（没有路径变化可言）
     *  - 旧空、有新 → "local-address-changed"（第一次选中）
     *  - 旧有、新空 → "local-address-changed"（pair 掉了）
     *  - 两边有、**中继位不同** → "relay-fallback"
     *  - 两边有、中继位相同 → "local-address-changed"
     *
     *  「中继位」的判定与 `TransportStats::relayed` 同源：
     *  两端任一 `type == "relay"` 就是中继路径。 */
    static std::string_view classify(
        const std::optional<IceCandidatePairInfo>& old_pair,
        const std::optional<IceCandidatePairInfo>& new_pair) noexcept;

    /** 两个 pair 是否是同一条路（逐字段比）。 */
    static bool same_path(const IceCandidatePairInfo& a,
                           const IceCandidatePairInfo& b) noexcept;
};
}
```

**为什么 reason 的判定放在机器里而不是调用点**：N3 重启和手动重启是**两个**调用点，`set_on_ice_restart` 是第三个。判定散在三处就会漂 —— 同一件事在三个地方给出三个 reason。放进一个纯函数，三处共用一条判据。

**为什么 `classify` 是纯函数（不持有时钟）**：C3（判定必须确定性）。它只看两个 pair，不看时间、不看状态。

### 五、接线：三处报告点

**一、N3 重连重启**（`execute_ice_recovery_restart()`，在 `engine_transport.cpp`）：

```cpp
// 重启**之前**：记下当前 pair
impl_->session_state.path_change.note_before(current_pair());
// ... 执行 ICE 重启（N3 的退避逻辑）...
// 重启**之后**：取新 pair，报告
fire_ice_recovery_report(reason, false, "");   // ← 现在带上 old / new pair
```

**二、手动重启**（`restart_ice()`，在 `engine.cpp`）：同样在 `ice_t_->restart()` **之前**记下 pair，之后报告（reason `"manual"`）。

**三、对端重启**（`engine_transport.cpp` 的 `set_on_ice_restart` 转发处）：报 `PeerRestartReport`，reason `"peer-restart"`。

**报告前的一致性**：报告里的 `old_pair` 是**记下来的那个**，`new_pair` 是**当下取的**。若新 pair 还没选出来（重启后 ICE 还在 gathering），`new_pair` 是空 —— 宿主知道「还没选好」，而不是拿到一个假的。

### 六、`Stats` 侧：最后一次路径变更

宿主除了回调，也常常是**事后**才想知道「刚才换路了吗」。所以在扁平的 `Stats` 上留最后一条：

```cpp
// engine.hpp 的 Stats（扁平的那个，不是 TransportStats）

/** 最后一次路径变更的 reason（v1.4 N8）：空 = 没发生过路径变更。
 *  取值与 `IceRecoveryReport::reason` 同一套词汇。 */
std::string last_path_change_reason;

/** 最后一次路径变更后的 pair（空 = 没有或还没选出来）。 */
std::optional<IceCandidatePairInfo> last_path_change_new_pair;
```

**为什么放扁平 `Stats` 而不是 `Stats::transport`**：与 N3 的 `ice_recovery_*` 同理 —— 这是**策略事实**（发生过一次路径变更），不是**传输测量**（此刻的 RTT / 候选数）。N7 的 `turn_credentials_state` 也是同样的判断。

### 七、`ice_failure_reason` 的联动

路径变更**不是**失败 —— 不该被报成 `ice_failure_reason` 里的任何值。N8 的报告走的是**成功路径**上的回调，不碰失败原因词汇。

（计划 §5 **N8-1** 说：重启**失败**且 N3 到了终态时，报**最后一对**、**reason** 和一个到 `"failed"` 的 call-state 迁移 —— 「和终态本身的那份报告一样，不新增事件类型」。call-state 枚举是 D7 的活，所以本片只保证「终态报告里带最后一对」。）

## 验收标准

### 一、公开面

1. `IceRecoveryReport` 新增 `old_pair` / `new_pair` 两个字段，类型是 `std::optional<IceCandidatePairInfo>`。
2. `IceRecoveryReport` 的既有字段（`reason` / `restart_count` / `remaining_budget` / `terminal` / `terminal_reason`）语义**不变**。
3. `IceCandidatePairInfo` 的形状**不变**（不新造 pair 类型）。
4. `set_on_peer_restart()` 存在，类型是 `EnginePeerRestartCallback`（`std::function<void(const PeerRestartReport&)>`）。
5. `PeerRestartReport` 带 `old_pair` / `new_pair` / `reason` 三个字段。
6. `set_on_ice_restart()` 的签名与语义**不变**（ABI 稳定，ADR-015）。
7. 新增的公开符号全部带 `NIMRTC_API_STABLE` 标注（跑只读 `scripts/api_freeze_scan.js` — 跑它，不改它）。

### 二、报告什么

8. N3 的重连重启 → `set_on_ice_recovery` 回调被触发，`report.old_pair` 是**重启前**的 pair。
9. N3 的重连重启 → `report.new_pair` 是**重启后**当前的 pair（还没选出来时是空）。
10. 手动 `restart_ice()` → 同样带 old / new pair，reason 是 `"manual"`。
11. 对端重启 → `set_on_peer_restart` 被触发，`reason == "peer-restart"`，带 old / new pair。
12. 报告里带 N3 的 `restart_count` / `remaining_budget` / `terminal`（既有字段不变）。

### 三、reason 词汇

13. reason 是**闭合词汇**：`"local-address-changed"` / `"peer-restart"` / `"relay-fallback"` / `"manual"` / `"ice-failed"`，**没有**别的值。
14. 直连 → 中继（或中继 → 直连）→ reason 是 `"relay-fallback"`。
15. 别的 pair 变化 → reason 是 `"local-address-changed"`。
16. 之前没选中过 pair、现在选中了 → reason 是 `"local-address-changed"`。

### 四、边界

17. 没有旧 pair、没有新 pair → 不报路径变更（或报空 reason），**不**崩。
18. 新 pair 还没选出来（重启后还在 gathering）→ `new_pair` 是空，宿主能看到「还没选好」。
19. 路径变更**不**被报成 `ice_failure_reason`（它不是失败）。
20. `src/engine/` **不** include `nimrtc/ice/` 的任何具体头（接缝纪律，L4）。
21. 回调在引擎 tick 线程上被调用（**不是**宿主线程）—— 单测钉住。
22. `src/engine/tests/test_engine_*.cpp` 一整套在新增后**仍然通过**（回归）。
23. `plugins::IICETransport` 接口**不变**。

### 五、文档与契约

24. `docs/api/engine_api.md` 记录：新的回调与用法、两个新 `Stats` 字段、reason 的闭合词汇。
25. `docs/api/public_headers.manifest` 与实际公开面一致（跑只读 `scripts/check_api_manifest.py` — 跑它，不改它；本片**不新增头文件**，所以清单**应当不变**）。

## 本片不做

- **不做** SDP 重协商的编排 —— 引擎不生成 offer 片段（计划 §5 **N8-2**：*report only*）。
- **不做** call-state 枚举 —— 那是 D7。
- **不做** 改 `plugins::IICETransport` 接口。
- **不改** `src/modules/ice/`、**不改** `src/plugins/` —— 本片在接缝**之上**。
- **不做** N3 的重连策略本身。
- **不做** 历史路径变更的列表 —— 只留最后一次（宿主要历史就自己记）。

## 接口变更

| 符号 | 变化 | 说明 |
|---|---|---|
| `NimRTCEngine::IceRecoveryReport::old_pair` | **新增**（字段） | `std::optional<IceCandidatePairInfo>` |
| `NimRTCEngine::IceRecoveryReport::new_pair` | **新增**（字段） | `std::optional<IceCandidatePairInfo>` |
| `NimRTCEngine::PeerRestartReport` | **新增**（类型） | 对端重启的 pair 上下文 |
| `NimRTCEngine::EnginePeerRestartCallback` | **新增**（类型别名） | |
| `NimRTCEngine::set_on_peer_restart()` | **新增**（方法） | |
| `NimRTCEngine::Stats::last_path_change_reason` | **新增**（字段） | 扁平 `Stats` |
| `NimRTCEngine::Stats::last_path_change_new_pair` | **新增**（字段） | 扁平 `Stats` |

**无 ABI 破坏**：全部是新增字段/方法，既有符号的签名与语义不变（ADR-015）。

## 测试策略

### 单元测试（`src/engine/tests/test_engine_path_change.cpp`）

**机器级（`PathChangeTracker`，纯确定性）**：

- `classify` 的五种情形：无 pair / 首次选中 / pair 掉了 / 中继位翻转 / 同一路径。
- `same_path` 逐字段比：地址、端口、协议、类型任一不同 → 不同。
- `note_before` / `before` / `reset` 的记账往返。
- `classify` 是**纯函数**：同样两个输入 → 同样输出（调用两次比相等）。

**接线级（用假的 transport 驱动）**：

- 重连重启 → 回调带 old（重启前）/ new（重启后）。
- 手动重启 → reason `"manual"`，带 pair。
- 对端重启 → `set_on_peer_restart` 带 pair，reason `"peer-restart"`。
- 旧的 `set_on_ice_restart` **仍然照常触发**（没被新回调顶替）。
- 回调在 tick 线程上（断言线程 id）。

### 验收测试（`tests/acceptance/v1-path-change-report/acceptance.test.cjs`）

- 静态断言：`engine.hpp` 里 `IceRecoveryReport` 同时有 `old_pair` / `new_pair`。
- 静态断言：`set_on_ice_restart` 的签名**没变**（ABI 稳定）。
- 静态断言：新的公开符号带 `NIMRTC_API_STABLE`。
- 静态断言：reason 词汇在文档里是闭合的（五个值不多不少）。
- 静态断言：`src/engine/` 不 include `nimrtc/ice/`。
- 文档断言：`engine_api.md` 记录了新的回调与两个 `Stats` 字段。
- 执行只读脚本：`scripts/api_freeze_scan.js`、`scripts/check_api_manifest.py` 跑绿。

## 风险

| 风险 | 缓解 |
|---|---|
| ~~**改 `IceRecoveryReport` 会不会破 ABI**~~ | **已核实（ADR-015 Decision 4）**：MINOR bump（v1.3 → v1.4）**明确允许 ABI 变化** —— 表里写着 *"v1.0.x → v1.1.0 (MINOR bump)：ABI committed? **No**。Callers must rebuild. API is additive (no STABLE-symbol removal); ABI may change."*。本片是**加字段**（additive，无 STABLE 符号移除），符合该口径。**注意**：本片的 `Stats` 结构体按值传参，加字段会改大小 —— 在 MINOR bump 内这是**允许的**，但要在 `CHANGELOG.md` 里写明（宿主需重编）。**若将来打算做跨 MINOR 的 ABI 承诺，这一条要重新评估。** |
| **`set_on_ice_restart` 的 ABI** | 不动它的签名。新增独立回调。 |
| **对端重启时 new_pair 拿不到** | 对端重启是**对方**换了凭据，本端的 pair 可能**没变** —— 这是**正常**的。`new_pair` 空是合法的，宿主据此知道「本端路径没变，要重协商的是对端」。 |
| **relay-fallback 的判定与 `Stats::relayed` 不一致** | 两处用**同一个**判据（任一端 `type == "relay"`）。判据放在 `PathChangeTracker::classify` 里，`Stats::relayed` 保持原样但单测钉住两者一致。 |
| **在错误线程上调回调** | 与 N3 / N7 同一条规则：引擎 tick 线程。单测断言线程 id。 |
| **旧 pair 的记账被别处清掉** | 记账只在 `open()` / `close()` 时 `reset()`，其余时候只写不删。 |

## 归属

**`version/v3`**。本片改 `engine.hpp`（公开面）、`engine.cpp` / `engine_transport.cpp`（接线）、`engine_session_state.hpp`（记一笔）、`engine_transport_stats.cpp`（`Stats` 装配）—— 与 N3（`8abb815a`）、N7（`3529b44a`）、D1 / D2 / D6 撞同一片区域，必须**顺序**做。

**前置**：N3（`8abb815a`）与 N7（`3529b44a`）已进 `version/v3` —— 本片要读 N3 的重连点，且 `engine_transport.cpp` 的 `execute_ice_recovery_restart()` 已被 N7 改过（加了 `refresh_turn_credentials()`）。

**后续**：D7（call-state 回调）会读本片的 `last_path_change_reason` 与终态报告。
