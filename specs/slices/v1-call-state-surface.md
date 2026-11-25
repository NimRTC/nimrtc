# Slice: v1-call-state-surface

## Summary

**宿主要能一通话的生命周期，而不是从三个观察里自己拼。** 今天一个宿主想知道「这通话怎么样了」，得同时看 `set_on_state_change`（ICE 状态字符串）、`set_on_error`（错误码）、以及轮询 `Stats`（诊断面）—— 而这三样分属三个形状。remote-desktop 示例今天就是**轮询 + diff** 出来的，这说明这是**宿主自己的活**，不是便利性。

D7 补上那个**汇合点**：

1. **一个 call-state 回调**，跨越整栈：`connecting` → `connected` → `reconnecting` → `failed` / `closed`，**带上 N4 / N8 的细节**。
2. **一个失败枚举**，宿主能 `switch`：`kIceFailed` / `kDtlsFailed` / `kTurnRefused` / `kCredentialsRejected` / `kMediaFailed`（计划 §5 **D7-1** 的已决），加上 N4 已经定义的字符串原因（**枚举**给 `switch` 用，**字符串**给日志用）。
3. **一个一通话的生命周期**：`start(config)` / `stop()` / `hangup(reason)` —— 按**定义好的顺序**关设备、传输与信令钩子。

`set_on_control_message` **保持原样继续工作** —— D7 只是让两个回调**读起来像一个面**，不改它的语义。

架构决策在 `docs/adr/ADR-035-call-state-surface.md`（**Proposed**，待签字）。

## 目标

**让宿主在一次回调里就知道「现在什么状态、为什么、有多严重」。**

具体地，一次 `CallState` 回调要同时回答：

- **状态是什么**：`connecting` / `connected` / `reconnecting` / `failed` / `closed`
- **失败的话是哪一种**（枚举 + 字符串）
- **路径变了的话**：N8 的 `old_pair` / `new_pair` / reason
- **重连的话**：N3 的 `restart_count` / `remaining_budget`

这样宿主**不需要**再轮询 `Stats` 去 diff 字符串。

### 现状核实（写规格时的树状态）

- **`set_on_state_change` 只有两个触发点**：`engine.cpp:424` 的 `"open"` 与 `engine.cpp:505` 的 `"closed"` —— 它**不是** ICE 状态流，只是生命周期两端的一个粗信号。ICE 状态本身在 `ice_state_string()`（`engine.cpp:783`），要**主动轮询**。
- **ICE 状态词汇已在**（`ice_state_string()`）：`"disconnected"` / `"gathering"` / `"connecting"` / `"connected"` / `"completed"` / `"failed"`。
- **N4 的失败原因词汇已在**（`TransportStatsProjector::ice_failure_reason()`，`engine_transport_stats.cpp:115`）：`"transport-unsupported"` > `"credentials-rejected"` > `"no-candidates"` > `"no-pair"` > `"checks-failed"` > `"none"`（**首个匹配优先**）。
- **N8 的路径变更报告已在**：`IceRecoveryReport` 带 `old_pair` / `new_pair`（`std::optional<IceCandidatePairInfo>`）+ `reason`。
- **N3 的重连报告已在**：`IceRecoveryReport` 带 `restart_count` / `remaining_budget` / `terminal` / `terminal_reason`。
- **`set_on_control_message` 已在**（`engine.hpp:4462`）—— D7 **不动**它。
- **`set_on_error` 已在**（`engine.hpp:3009`）：`(uint32_t err, std::string_view msg)`。

### 与其它片的边界（本片**不做**）

- **不做** 新的事件流或新的 sink —— D7 只**命名**报什么、何时报（计划 §D7 的「不做性能问题重开」的约束）。
- **不做** 改 `set_on_control_message` 的语义 —— 只是让它**读起来**与 call-state 并排。
- **不做** 自动合并 / 自动批准 —— 与本片无关。
- **不做** 高频回调 —— call-state 只在**状态迁移**时触发，不是每帧。
- **不做** N6 的样例 —— 那是波次 6。

## 范围

- src/engine/src/engine_call_state.hpp
- src/engine/src/engine_call_state.cpp
- src/engine/src/engine.cpp
- src/engine/src/engine_transport.cpp
- src/engine/src/engine_session_state.hpp
- src/engine/src/engine_transport_stats.cpp
- src/engine/include/nimrtc/engine/engine.hpp
- src/engine/CMakeLists.txt
- src/engine/tests/CMakeLists.txt
- src/engine/tests/test_engine_call_state.cpp
- tests/acceptance/v1-call-state-surface/acceptance.test.cjs
- docs/adr/ADR-035-call-state-surface.md
- docs/api/engine_api.md
- CHANGELOG.md
- specs/slices/v1-call-state-surface.md

> 只读 `src/modules/ice/` —— 本片**不**改它。
> 只读 `src/plugins/` —— 本片**不**改它。

## 设计

### 一、公开面：状态枚举 + 报告结构

```cpp
// src/engine/include/nimrtc/engine/engine.hpp

/** 通话状态（v1.4 D7）。
 *
 *  这是**整栈**的状态，不只是 ICE 的 —— 宿主想知道「这通话能不能用」，
 *  而不是「ICE 那一层现在在干什么」。
 *
 *  迁移顺序：`connecting` → `connected` →（`reconnecting` ↔ `connected`）*
 *  → `failed` / `closed`。 */
enum class NIMRTC_API_STABLE CallState : std::uint8_t {
    kConnecting = 0,  ///< 正在建立（gathering / connecting）
    kConnected = 1,   ///< 通了
    kReconnecting = 2, ///< 断了，引擎正在按 N3 的策略恢复
    kFailed = 3,      ///< 失败了（有 failure_reason）
    kClosed = 4,      ///< 结束了（stop() / hangup() / 析构）
};

/** 失败的原因（v1.4 D7，计划 §5 D7-1）。
 *
 *  **枚举**给 `switch` 用，**字符串**给日志用 —— 两者说的是同一件事，
 *  枚举是稳定的、字符串是可读的。 */
enum class NIMRTC_API_STABLE CallFailure : std::uint8_t {
    kNone = 0,
    kIceFailed = 1,           ///< ICE 层失败（checks-failed / no-pair）
    kDtlsFailed = 2,          ///< DTLS 握手失败
    kTurnRefused = 3,         ///< TURN 服务器拒绝（transport-unsupported）
    kCredentialsRejected = 4, ///< 凭据被拒（credentials-rejected）
    kMediaFailed = 5,         ///< 媒体层失败（编解码 / 渲染 / 采集）
};

/** 一次 call-state 迁移的报告（v1.4 D7）。 */
struct NIMRTC_API_STABLE CallStateReport {
    /** 新状态。 */
    CallState state = CallState::kConnecting;

    /** 失败原因（`state == kFailed` 时有意义；否则 kNone）。 */
    CallFailure failure = CallFailure::kNone;

    /** 失败原因的**字符串**（N4 的词汇）："transport-unsupported" /
     *  "credentials-rejected" / "no-candidates" / "no-pair" /
     *  "checks-failed" / "none"。 */
    std::string failure_reason;

    /** 路径变更的细节（N8 的形状）：old pair / new pair / reason。
     *  没有发生路径变更时是空的。 */
    std::optional<IceCandidatePairInfo> old_pair;
    std::optional<IceCandidatePairInfo> new_pair;
    std::string path_change_reason;

    /** 重连的细节（N3 的形状）：已用次数 / 剩余预算 / 是否终态。
     *  没有进入重连时是 0 / 0 / false。 */
    std::uint32_t restart_count = 0;
    std::uint32_t remaining_budget = 0;
    bool terminal = false;
};

using NIMRTC_API_STABLE EngineCallStateCallback =
    std::function<void(const CallStateReport&)>;

/** 安装 call-state 回调（v1.4 D7）。 */
NIMRTC_API_STABLE void set_on_call_state(EngineCallStateCallback cb) noexcept;
```

**为什么枚举而不是只有字符串**：计划 §5 **D7-1** 的已决 —— 「**枚举**是 `switch` 能动作的，**字符串**是日志能带的」。两个都给。

**为什么 `std::optional` 装 pair**：与 N8 一致 —— pair 是**可能不存在**的事实。

### 二、状态机：私有协作 `CallStateMachine`

「什么时候算 `reconnecting`」这个问题散在多处就会漂。按 N3 / N8 的形状（私有 TU、时钟显式传参、确定性单测）：

```cpp
// src/engine/src/engine_call_state.hpp
namespace nimrtc::engine::detail {

/** 通话状态机（v1.4 D7）。
 *
 *  它只做一件事：把「引擎里发生的事」翻译成「一次状态迁移」，
 *  并且**只在真的迁移时**发一次 —— 同一个状态不重复报。
 *
 *  它不碰 transport、不碰时钟、不调模型（C3）。 */
class CallStateMachine {
public:
    /** 记录一次观察，返回**是否**该发报告。
     *
     *  判据（纯函数，看新状态与当前状态）：
     *  - 状态没变 → **不发**（不重复报）
     *  - 首次观察 → 发（当前是 `kConnecting`）
     *  - 变了 → 发
     *
     *  `ice_state` 是 ICE 层的原始状态（`plugins::IceState`），
     *  由它翻译成 call-state：
     *  - `Disconnected` / `Gathering` / `Connecting` → `kConnecting`
     *  - `Connected` / `Completed` → `kConnected`
     *  - `Failed` → `kFailed`
     *
     *  `reconnecting` **不**由 ICE 状态决定 —— 它由 N3 的重连机
     *  决定（引擎知道自己在恢复，ICE 只知道它断了）。
     */
    bool observe(plugins::IceState ice_state) noexcept;

    /** 进入重连（N3 的重连点调）。 */
    bool enter_reconnecting() noexcept;

    /** 进入终态失败（N3 的终态 / DTLS 失败 / 媒体失败调）。 */
    bool fail(CallFailure failure, std::string_view reason) noexcept;

    /** 关闭（stop() / hangup() / 析构调）。 */
    bool close() noexcept;

    /** 当前状态。 */
    CallState state() const noexcept { return state_; }

    /** 上一次报告的失败信息。 */
    CallFailure failure() const noexcept { return failure_; }
    const std::string& reason() const noexcept { return reason_; }

    /** 清空（open() 时调 —— 每个会话从零开始）。 */
    void reset() noexcept;
};
}
```

**为什么「同状态不重复报」**：宿主要的是**迁移**，不是**状态**。轮询式上报会让宿主自己去 diff（那就等于没做 D7）。这条判据也让回调的频率天然受限 —— 不是性能问题，是语义问题。

**为什么 `reconnecting` 不由 ICE 状态决定**：ICE 只知道「断了」，**不知道**「引擎正在按 5s 宽限 + 退避恢复」。这是 N3 的知识，引擎有，ICE 没有。

### 三、接线：五个触发点

| 触发点 | 位置 | 迁移 |
|---|---|---|
| ICE 状态变化 | `engine_transport.cpp` 的 tick | `observe(ice_state)` |
| N3 进入重连 | `execute_ice_recovery_restart()` | `enter_reconnecting()` |
| N3 终态 | `report_ice_recovery_terminal()` | `fail(kIceFailed, reason)` |
| DTLS / 媒体失败 | 各失败点 | `fail(kDtlsFailed / kMediaFailed, reason)` |
| stop() / hangup() / 析构 | `close()` | `close()` |

**每次发报告时**，把 N3 / N4 / N8 的细节**附上**：

```cpp
void NimRTCEngine::fire_call_state(CallStateReport report) noexcept {
    // 附上 N3 的重连细节
    const std::int64_t now_us = detail::steady_now_us();
    auto& rec = impl_->session_state.ice_recovery;
    report.restart_count = rec.restarts_used(now_us);
    report.remaining_budget = rec.remaining_budget(now_us);
    report.terminal = rec.terminal();

    // 附上 N8 的路径变更细节（只有发生过路径变更才带）
    if (!impl_->session_state.last_path_change_reason.empty()) {
        report.path_change_reason = impl_->session_state.last_path_change_reason;
        report.old_pair = impl_->session_state.path_change.before();
        report.new_pair = impl_->session_state.last_path_change_new_pair;
    }

    if (on_call_state_) on_call_state_(report);
}
```

**为什么路径变更用「最后一条」而不是「本次」**：call-state 迁移与路径变更**不总是同时发生**（比如 ICE 从 disconnected 回到 connected 时可能没有换路）。报告要带的是**当前这次迁移**能看到的上下文，「最后一条」是诚实的答案。

### 四、一通话的生命周期

```cpp
// engine.hpp

/** 开始一通话（v1.4 D7）。
 *
 *  把「配置 → 打开 → 开始信令」的顺序封起来。 */
NIMRTC_API_STABLE int start(const EngineConfig& config) noexcept;

/** 结束一通话（v1.4 D7）。
 *
 *  按**定义好的顺序**关：先传输（ICE），再编解码与插件，最后
 *  `TransportSession`（它自己按 ICE → DTLS → RTP → SCTP 的顺序拆）。
 *  音频编解码器在 ICE 之后关 —— 传输还在时音频管线不能先死，
 *  否则在途的帧无处可去。 */
NIMRTC_API_STABLE void stop() noexcept;

/** 挂断（v1.4 D7）。
 *
 *  与 `stop()` 的区别：`hangup()` **带一个原因**，且**先通知对端**
 *  （把原因传出去），再按 `stop()` 的顺序关。
 *
 *  @param reason 挂断原因（给对端看的字符串）。
 *  @return true if the hangup was performed. */
NIMRTC_API_STABLE bool hangup(std::string_view reason) noexcept;
```

**为什么 `hangup()` 要带原因**：挂断与「本地停止」在**对端看来**是不同的两件事 —— 对端需要知道为什么（正常挂断 vs 出错 vs 主动切走）。计划 §D7 的「one error code per failure」的精神。

**关闭顺序是本片要钉住的东西**：现有的 `close()`（`engine.cpp:430`）已经有一套顺序 —— QoE → ICE → 音频插件 → 编解码 → 视频插件 → BWE → `TransportSession`（它自己按 ICE → DTLS → RTP → SCTP 拆）。D7 **不重排**它 —— `stop()` 就是 `close()` 的一个名字，把那个顺序**变成可测的**（单测用假对象记录顺序并断言）。

### 五、`set_on_control_message` 并排（**不改语义**）

D7 只做一件事：把 `set_on_control_message` 的声明**挪到** call-state 回调**旁边**，并在文档里写明「这两个回调构成一个面」。

**签名与语义一个字不动** —— 已有宿主不受影响。

## 验收标准

### 一、公开面

1. `CallState` 枚举存在，六个值里的五个是 `kConnecting` / `kConnected` / `kReconnecting` / `kFailed` / `kClosed`。
2. `CallFailure` 枚举存在，含 `kNone` / `kIceFailed` / `kDtlsFailed` / `kTurnRefused` / `kCredentialsRejected` / `kMediaFailed`。
3. `CallStateReport` 带 `state` / `failure` / `failure_reason` / `old_pair` / `new_pair` / `path_change_reason` / `restart_count` / `remaining_budget` / `terminal`。
4. `set_on_call_state()` 存在，类型是 `EngineCallStateCallback`。
5. 新增的公开符号全部带 `NIMRTC_API_STABLE`（跑只读 `scripts/api_freeze_scan.js`）。
6. `set_on_control_message()` 的签名与语义**不变**。

### 二、状态迁移

7. `open()` → 一次 `kConnecting` 报告。
8. ICE 连上 → 一次 `kConnected` 报告。
9. ICE 断了且 N3 开始恢复 → 一次 `kReconnecting` 报告。
10. N3 到了终态 → 一次 `kFailed` 报告，`failure == kIceFailed`。
11. `stop()` / `hangup()` → 一次 `kClosed` 报告。
12. **同一个状态不重复报**（连续两次观察到同一个 ICE 状态 → 只报一次）。
13. 每次报告**带** N3 的 `restart_count` / `remaining_budget` / `terminal`。
14. 发生过路径变更时，报告**带** N8 的 `old_pair` / `new_pair` / `path_change_reason`。

### 三、失败原因

15. N4 的失败字符串被**原样**带进 `failure_reason`（"credentials-rejected" / "checks-failed" / "no-candidates" / "no-pair" / "transport-unsupported"）。
16. `failure_reason == "credentials-rejected"` → `failure == kCredentialsRejected`。
17. `failure_reason == "transport-unsupported"` → `failure == kTurnRefused`。
18. `failure_reason` 是 "checks-failed" / "no-pair" / "no-candidates" 之一 → `failure == kIceFailed`。
19. `failure_reason == "none"` → `failure == kNone`（**不**误报成失败）。
20. DTLS 失败 → `failure == kDtlsFailed`。
21. 媒体失败 → `failure == kMediaFailed`。

### 四、生命周期

22. `start(config)` 存在，一次调用完成「配置 → open」。
23. `stop()` 存在，按**定义好的顺序**关闭（QoE → ICE → 音频插件 → 编解码 → 视频插件 → BWE → `TransportSession`）。
24. `hangup(reason)` 存在，带原因，且**先通知**再关。
25. `hangup()` / `stop()` 之后 `set_on_call_state` 收到 `kClosed`。

### 五、边界与回归

26. 回调在引擎 tick 线程上被调用（**不是**宿主线程）—— 单测钉住。
27. `src/engine/` **不** include `nimrtc/ice/` 的任何具体头（接缝纪律，L4）。
28. `set_on_state_change` / `set_on_error` / `set_on_control_message` 的既有行为**不变**。
29. `src/engine/tests/test_engine_*.cpp` 一整套在新增后**仍然通过**（回归）。
30. `plugins::IICETransport` 接口**不变**。

### 六、文档与契约

31. `docs/api/engine_api.md` 记录：新枚举、报告结构、生命周期三个方法、状态迁移图。
32. `docs/api/public_headers.manifest` 与实际公开面一致（跑只读 `scripts/check_api_manifest.py` — 跑它，不改它；本片**不新增头文件**，清单**应当不变**）。

## 本片不做

- **不做** 新的高频事件流 / 新的 sink —— D7 只命名报什么、何时报。
- **不做** 改 `set_on_control_message` 的语义 —— 只让它**读起来**并排。
- **不做** 改 `set_on_state_change` / `set_on_error` 的既有行为。
- **不做** 自动合并 / 自动批准 / 常驻 daemon。
- **不做** 改 `src/modules/ice/`、**不改** `src/plugins/`。
- **不做** N6 的样例。

## 接口变更

| 符号 | 变化 | 说明 |
|---|---|---|
| `CallState` | **新增**（枚举） | 五个状态 |
| `CallFailure` | **新增**（枚举） | 六个值 |
| `CallStateReport` | **新增**（结构体） | |
| `EngineCallStateCallback` | **新增**（类型别名） | |
| `NimRTCEngine::set_on_call_state()` | **新增**（方法） | |
| `NimRTCEngine::start()` | **新增**（方法） | |
| `NimRTCEngine::stop()` | **新增**（方法） | |
| `NimRTCEngine::hangup()` | **新增**（方法） | |

**无 ABI 破坏**：全部是新增符号，既有符号的签名与语义不变（ADR-015）。

## 测试策略

### 单元测试（`src/engine/tests/test_engine_call_state.cpp`）

**机器级（`CallStateMachine`，纯确定性）**：

- `observe()` 的翻译：六个 ICE 状态 → 五个 call-state。
- **同状态不重复报**：连续两次同状态 → 第二次返回 false。
- `enter_reconnecting()` / `fail()` / `close()` 的迁移。
- `reset()` 清空一切。
- 状态机是**纯状态**的：不碰时钟、不碰 transport。

**接线级（活引擎）**：

- `open()` → `kConnecting`；ICE 连上 → `kConnected`。
- N3 触发重连 → `kReconnecting`；终态 → `kFailed` + `kIceFailed`。
- 报告带 N3 的 `restart_count` / `remaining_budget`。
- 报告带 N8 的路径变更细节。
- `failure_reason` 的六个 N4 词汇 → 枚举的映射（含 "none" → `kNone`）。
- 回调在 tick 线程上（断言线程 id）。
- `stop()` → `kClosed`；关闭顺序（用假对象记录顺序）。

### 验收测试（`tests/acceptance/v1-call-state-surface/acceptance.test.cjs`）

- 静态断言：`engine.hpp` 里 `CallState` / `CallFailure` / `CallStateReport` / `set_on_call_state` / `start` / `stop` / `hangup` 都在。
- 静态断言：`set_on_control_message` 的签名**没变**。
- 静态断言：新的公开符号带 `NIMRTC_API_STABLE`。
- 静态断言：`src/engine/` 不 include `nimrtc/ice/`。
- 文档断言：`engine_api.md` 记录了新面。
- 执行只读脚本：`scripts/api_freeze_scan.js`、`scripts/check_api_manifest.py` 跑绿。

## 风险

| 风险 | 缓解 |
|---|---|
| **call-state 与 `set_on_state_change` 重复** | `set_on_state_change` 是**粗信号**（只有 "open" / "closed"），call-state 是**迁移流**。两者共存，但 call-state 更细。文档写清区别。**`set_on_state_change` 的既有行为一个字不改**（验收标准 28）。 |
| **`reconnecting` 与 `failed` 打架**（N3 恢复中失败） | N3 的终态**就是** `failed`，且终态报告**在** `reconnecting` 之后 —— 顺序由 N3 的机决定，不是 D7 决定。D7 只翻译，不排序。 |
| **`failure_reason` 的字符串与枚举不同步** | 映射是**一个纯函数**（`failure_from_reason()`），一个地方实现，单测钉住六个 N4 词汇。 |
| **`start()` 与 `open()` 混用** | `start()` 是**加法**（新方法），`open()` 保持原样。`start()` 内部调 `open()`。已有宿主用 `open()` 不受影响。 |
| **关闭顺序错了漏数据** | 顺序**已经在** `close()`（`engine.cpp:430`）里定好了 —— D7 **不重排**，只是把它变成**可测的**（单测用假对象记录顺序并断言）。 |
| **回调频率** | 「同状态不重复报」天然限制了频率。call-state 不是每帧事件。 |

## 归属

**`version/v3`**。本片改 `engine.hpp`（公开面 + 生命周期）、`engine.cpp` / `engine_transport.cpp`（接线）、`engine_session_state.hpp`（记一笔）、`engine_transport_stats.cpp`（失败原因投影）—— 与 N3 / N7 / N8 撞同一片区域，必须**顺序**做。

**前置**：N3（`8abb815a`）、N7（`3529b44a`）、N8（`b4aa77ec`）已进 `version/v3` —— 本片要读 N3 的重连机、N4 的失败词汇、N8 的路径变更细节。

**后续**：N6（SDK 样例，波次 6）会用这一层 —— 「≤200 行跑完一通话」的前提就是有一通话的生命周期与状态面。
