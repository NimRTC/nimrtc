# ADR-035: call-state 汇合面 —— 一通话的生命周期在一条回调里

| | |
|---|---|
| Number | 035 |
| Status | **Accepted** |
| Date | 2026-11-24 |
| Phase | v1.4 (D7) |
| Bound to | v1.4.0 |
| Supersedes | — |
| Related | `docs/plan/v1.4-plan.md` §D7、§N3、§N4、§N8；`docs/adr/ADR-015-api-stability-policy.md`；`docs/adr/ADR-031-ice-recovery-policy-ownership.md`；`docs/adr/ADR-033-path-change-report.md`；`specs/slices/v1-ice-recovery.md`（N3）；`specs/slices/v3-n4-d6-connectivity.md`（N4/D6）；`specs/slices/v1-path-change-report.md`（N8） |

---

## 1. 背景

今天一个宿主想知道「这通话怎么样了」，得同时看三样东西：

- `set_on_state_change` —— 一个**粗信号**，只有 `"open"` / `"closed"` 两个触发点（`engine.cpp` 的 `"open"` 与 `"closed"`），不是 ICE 状态流；
- `set_on_error` —— 错误码；
- 轮询 `Stats` —— 诊断面（`ice_state_string()`、`ice_failure_reason()`、`last_path_change_reason` …）。

这三样分属三个形状。remote-desktop 示例今天就是**轮询 + diff** 出来的 —— 这说明「把通话状态拼出来」是宿主自己的活，不是便利性。D7 补上那个**汇合点**：一条回调跨越整栈，`connecting` → `connected` → `reconnecting` → `failed` / `closed`，带 N4（失败）与 N8（路径变更）的细节，让宿主**不需要**再轮询 diff。

计划书 §D7 已经钉了几件事：

- **D7-1**：一个失败枚举，宿主能 `switch`：`kIceFailed` / `kDtlsFailed` / `kTurnRefused` / `kCredentialsRejected` / `kMediaFailed`，加上 N4 已经定义的字符串原因（**枚举**给 `switch` 用，**字符串**给日志用）；
- **不做**新的事件流 / 新的 sink —— D7 只命名报什么、何时报；
- **不做**高频回调 —— 只在**状态迁移**时触发，不是每帧；
- `set_on_control_message` **保持原样继续工作** —— D7 只是让两个回调**读起来像一个面**。

本 ADR 记录 D7 落地时的选择与理由：状态机长什么样、五个触发点怎么接线、失败词汇怎么映射、为什么终态是吸收的、生命周期三个方法怎么落在已有的 `close()` 上。

## 2. 决策

### 2.1 决策一：公开面 = 状态枚举 + 失败枚举 + 报告结构 + 一条回调

```cpp
enum class NIMRTC_API_STABLE CallState : std::uint8_t {
    kConnecting = 0,   ///< establishing (gathering / connecting)
    kConnected = 1,    ///< up
    kReconnecting = 2, ///< down, the engine is recovering per the N3 policy
    kFailed = 3,       ///< failed (has a failure_reason)
    kClosed = 4,       ///< over (stop() / hangup() / destructor)
};

enum class NIMRTC_API_STABLE CallFailure : std::uint8_t {
    kNone = 0,
    kIceFailed = 1,           ///< ICE layer failed (checks-failed / no-pair)
    kDtlsFailed = 2,          ///< DTLS handshake failed
    kTurnRefused = 3,         ///< TURN server refused (transport-unsupported)
    kCredentialsRejected = 4, ///< credentials rejected (credentials-rejected)
    kMediaFailed = 5,         ///< media layer failed (codec / render / capture)
};

struct NIMRTC_API_STABLE CallStateReport {
    CallState state = CallState::kConnecting;
    CallFailure failure = CallFailure::kNone;
    std::string failure_reason;                        // N4 词汇
    std::optional<IceCandidatePairInfo> old_pair;      // N8 形状
    std::optional<IceCandidatePairInfo> new_pair;
    std::string path_change_reason;
    std::uint32_t restart_count = 0;                   // N3 形状
    std::uint32_t remaining_budget = 0;
    bool terminal = false;
};

using NIMRTC_API_STABLE EngineCallStateCallback =
    std::function<void(const CallStateReport&)>;

NIMRTC_API_STABLE void set_on_call_state(EngineCallStateCallback cb) noexcept;
```

**为什么枚举和字符串两个都给**：计划书 §D7-1 已决 —— 枚举是 `switch` 能动作的，字符串是日志能带的，两者说的是同一件事。

**为什么 `std::optional` 装 pair**：与 N8 一致 —— pair 是**可能不存在**的事实（还没选好 / 还没换路），不是占位符。

**为什么枚举是嵌套在 `NimRTCEngine` 里的**：与已有的嵌套 `IceRecoveryReport` / `PeerRestartReport` 同一形状 —— 公开面只有 `engine.hpp` 一个头，宿主写 `NimRTCEngine::CallState::kConnected` 读起来就是「引擎的通话状态」。私有协作者（§2.2）通过 using 别名引用它们，避免重复声明。

### 2.2 决策二：私有协作者 `CallStateMachine` —— 纯状态，只翻译迁移

「什么时候算 `reconnecting`」这个问题散在多处就会漂。按 N3 / N8 的形状（私有 TU、显式时钟、确定性单测）：

```cpp
namespace nimrtc::engine::detail {

/** 通话状态机（v1.4 D7）。
 *
 *  它只做一件事：把「引擎里发生的事」翻译成「一次状态迁移」，
 *  并且**只在真的迁移时**发一次 —— 同一个状态不重复报。
 *
 *  它不碰 transport、不碰时钟、不调模型（C3）。 */
class CallStateMachine {
public:
    bool observe(plugins::IceState ice_state) noexcept;
    bool enter_reconnecting() noexcept;
    bool fail(CallFailure failure, std::string_view reason) noexcept;
    bool close() noexcept;
    CallState state() const noexcept;
    CallFailure failure() const noexcept;
    const std::string& reason() const noexcept;
    void reset() noexcept;
};
}
```

**判据（纯函数）**：状态没变 → 不发（不重复报）；首次观察 → 发（当前是 `kConnecting`，即 `open()` 发的那条）；变了 → 发。

**为什么「同状态不重复报」**：宿主要的是**迁移**，不是**状态**。轮询式上报会让宿主自己去 diff（那就等于没做 D7）。这条判据也让回调的频率天然受限 —— 不是性能问题，是语义问题。

**为什么 `reconnecting` 不由 ICE 状态决定**：ICE 只知道「断了」，不知道「引擎正在按 5 s 宽限 + 退避恢复」。这是 N3 的知识，引擎有，ICE 没有。所以 `observe()` 只处理 ICE 观察的翻译（`Connected`/`Completed` → `kConnected`，`Disconnected`/`Gathering`/`Connecting` → `kConnecting`，`Failed` → `kFailed`），`kReconnecting` 单独从 N3 的重连点进入。

**机器不碰时钟**：报告里带的 N3 / N8 细节是 `fire_call_state()` 在报告时从 `SessionState` 的 `ice_recovery` / `path_change` 读的 —— 机器只决定**何时**该发，不决定带什么。

### 2.3 决策三：五个触发点 + recovering 闸门

| 触发点 | 位置 | 迁移 |
|---|---|---|
| ICE 状态变化 | `engine_transport.cpp` 的 tick | `observe(ice_state)` |
| N3 进入重连 | `execute_ice_recovery_restart()` | `enter_reconnecting()` |
| N3 终态 | `report_ice_recovery_terminal()` | `fail(kIceFailed, current_ice_failure_reason())` |
| DTLS / 媒体失败 | 各失败点 | `fail(kDtlsFailed / kMediaFailed, reason)` |
| stop() / hangup() / 析构 | `close()` | `close()` |

**tick 顺序**：`poll_ice_recovery()` **先**、`poll_call_state()` **后** —— 这样 recovering 闸门在同一 tick 就能看到 N3 机的状态。

**recovering 闸门**：N3 机处于 grace / backoff（`ice_recovery_active()`）时，ICE 掉线观察被**吸收**进正在进行的重连 episode —— 掉线是预期的、正在被修，此时报 `kConnecting` / `kFailed` 会跟 N3 自己的顺序打架（spec §风险）。只有 ICE **上来**（`Connected` / `Completed`）的观察才能结束 episode。

**为什么 N3 终态直接 `fail(kIceFailed, ...)`**：N3 的终态**就是**「这通话的 ICE 失败」。理由串接自 N4 的 `current_ice_failure_reason()`（`"checks-failed"` / `"no-pair"` / …），宿主一次回调里既拿到枚举（`kIceFailed`）也拿到字符串。顺序由 N3 的机决定 —— 终态报告在 `reconnecting` 之后，D7 只翻译，不排序。

**为什么 DTLS 失败被 recovering 闸门挡住**：DTLS 握手失败可能属于**重连后**的新路径（旧握手因路径断了而失败）。当 N3 正在恢复时，`drain_dtls()` 的 Failed 分支不报 `kDtlsFailed` —— 让 N3 自己的终态成为决定性的失败（ADR-035 §2.5 的同一逻辑）。

### 2.4 决策四：`failure_from_reason()` —— 一个纯函数钉住 N4 词汇

```cpp
CallFailure failure_from_reason(std::string_view reason) noexcept;
```

映射（一个地方实现，单测钉住六个 N4 词汇）：

```
"transport-unsupported"  → kTurnRefused
"credentials-rejected"   → kCredentialsRejected
"checks-failed"          → kIceFailed
"no-pair"                → kIceFailed
"no-candidates"          → kIceFailed
"none" / "" / unknown    → kNone  (no failure is *not* a failure)
```

**为什么**：`failure_reason` 的字符串与枚举不同步是 spec §风险列出的漂移点。映射集中成一个纯函数后，字符串词汇（N4 的 `TransportStatsProjector::ice_failure_reason()` 输出）与宿主 `switch` 的枚举**不会**漂。

### 2.5 决策五：终态是吸收的 —— 一个失败的通话不「复活」

`kFailed` / `kClosed` 是**吸收态**：迁移图在 `failed` / `closed` 终止，没有迁移能离开它们。`transition()` 判据里终态直接返回 false；`close()` 是唯一例外（不走 `transition()`，允许 `kFailed → kClosed`）。

**为什么必须吸收**：如果不吸收，DTLS 失败后（ICE 层还活着，`observe(Connected)` 仍可能报 `kConnected`）下一次 tick 就会让报告在 `kConnected` 和 `kFailed` 之间**振荡**。宿主要的是「这通话结束了」，不是「每 tick 换一个说法」。一个失败/关闭的通话由宿主挂断并开一通话新的，而不是「恢复」一个失败的。

**`close()` 幂等**：重复 `stop()` / `hangup()` / `close()` 只报一次 `kClosed`。

**首次观察**：`open()` 调 `poll_call_state()` 驱动第一次观察 —— 新鲜机器（`reported_ == false`）的首观察**总是**报 `kConnecting`（验收标准 7）。终态闸门在首观察捷径**之前**：失败/关闭之后的首观察不得再报（验收 `CloseIsTerminalAndIdempotent` 抓到的正是这个次序）。

### 2.6 决策六：生命周期 —— `start()` / `stop()` / `hangup()` 落在已有的 `close()` 上

```cpp
NIMRTC_API_STABLE int  start(const EngineConfig& config) noexcept;  // config → open
NIMRTC_API_STABLE void stop() noexcept;                             // close() 的生命周期名字
NIMRTC_API_STABLE bool hangup(std::string_view reason) noexcept;     // 先通知对端，再 close()
```

**`start()` 是加法**：`config_ = config; return open();`。`open()` 保持原样，已有宿主用 `open()` 不受影响（spec §风险）。

**`stop()` 不重排关闭顺序**：定义好的顺序（QoE → ICE → 音频插件 → 编解码 → 视频插件 → BWE → `TransportSession`）**已经在** `close()`（`engine.cpp`）里 —— D7 只是把它命名并变成**可测的**（单测用假对象记录顺序并断言，spec §四）。`stop()` = `close()`。

**`hangup()` 与 `stop()` 的区别**：`hangup()` **带原因**，且**先通知对端**（原因经 `send_control_message` 传出），再按 `stop()` 的顺序关。对端眼里「正常挂断」与「本地停止」是两件事 —— 计划书 §D7 的「one error code per failure」精神。通知失败（控制通道缺失 / 饱和）不阻止挂断。

**`kClosed` 从哪来**：`close()` 末尾（`state_ = kClosed` 之后）调 `call_state.close()`，真迁移时 `fire_call_state()` 一条 `kClosed`。`stop()` / `hangup()` / `close()` / 析构**都**落在这里，所以报告恰好一次。

### 2.7 决策七：`set_on_control_message` 挪个位，语义一个字不动

`set_on_control_message` 的声明**移到** `set_on_call_state` 旁边，文档写明「这两个回调读起来像一个面」。**签名与语义一字不动**（`std::function<void(std::span<const std::uint8_t>)>`）—— 已有宿主不受影响。

## 3. 后果

### 3.1 公开面的变化（全部是加法）

| 符号 | 变化 |
|---|---|
| `NimRTCEngine::CallState` | 新增枚举（五个值） |
| `NimRTCEngine::CallFailure` | 新增枚举（六个值） |
| `NimRTCEngine::CallStateReport` | 新增结构体（九字段） |
| `NimRTCEngine::EngineCallStateCallback` | 新增类型别名 |
| `NimRTCEngine::set_on_call_state()` | 新增 setter |
| `NimRTCEngine::start()` / `stop()` / `hangup()` | 新增生命周期方法 |

**无 ABI 破坏**：全部是新增符号；`set_on_state_change` / `set_on_error` / `set_on_control_message` / `restart_ice()` 的签名与语义不变（ADR-015）。

### 3.2 依赖方向

- 新增私有协作者 `src/engine/src/engine_call_state.{hpp,cpp}`：include `core` / `plugins` 与引擎公开头，**不** include `nimrtc::ice` 的具体符号（Layout Invariant 4 / ADR-025 —— 与 `engine_ice_recovery.hpp` 同形，同一份规则）。
- `src/core/` **不动**；`src/plugins/` **不动**；`src/modules/ice/` **不动**。
- 引擎的 tick 线程驱动；判定只读状态、纯计算（C3）。
- 回调跑在 tick 线程（与 `set_on_ice_recovery` 同一规则）。

### 3.3 与 N3 / N4 / N8 的关系

- **N3**：`kReconnecting` / `kFailed` 是 N3 报告的 **call-state 视图**。N3 的 `IceRecoveryReport` 保持原样；call-state 报告在 `fire_call_state()` 时读 `ice_recovery` 把 `restart_count` / `remaining_budget` / `terminal` 附上。
- **N4**：`failure_reason` 是 N4 的**原样词汇**，`failure_from_reason()` 一个纯函数做映射。
- **N8**：call-state 报告带**最后一条**路径变更（`last_path_change_reason` / `old_pair` / `new_pair`）。call-state 迁移与路径变更**不总是同时发生**（ICE 从 disconnected 回到 connected 时可能没换路）；带「最后一条」是这次迁移能看到的诚实上下文。

## 4. 备选方案与不选的理由

| 备选 | 不选的理由 |
|---|---|
| 只给字符串、不给枚举 | 计划书 §D7-1 已决给枚举；宿主 `switch` 需要稳定符号 |
| `kReconnecting` 由 ICE 观察决定 | ICE 只知道「断了」，不知道「引擎在恢复」—— 那是 N3 的知识，引擎有，ICE 没有 |
| call-state 每秒报一次（轮询式） | 宿主要迁移不是状态；轮询式等于没做 D7，且制造高频回调 |
| 复用 `set_on_state_change` 承载 call-state | 它的语义已钉死为粗信号（"open"/"closed"）；扩义 = 破坏既有宿主（ADR-015） |
| 终态可被后续观察覆盖（不吸收） | DTLS 失败后 ICE 层还活着，`kConnected`/`kFailed` 会逐 tick 振荡 |
| `stop()` 重排关闭顺序 | 顺序已经在 `close()` 里定好；重排引入漏数据风险（spec §风险） |
| `hangup()` 不通知对端 | 对端眼里挂断与停止是两件事；计划书 §D7 的「one error code per failure」精神 |
| N3 恢复中 DTLS 失败照报 `kDtlsFailed` | 失败的手握可能属于重连后的新路径；N3 自己的终态才是决定性的 |
| 媒体失败做成新触发点 | 引擎今天没有媒体失败的接线（编解码错误走 `on_error_`）；机器 `fail()` 已能承载 `kMediaFailed`，接线的触发点留给后续片 |

## 5. 落地时发现的两处「规格/实现不闭环」

### 5.1 `observe()` 的终态闸门必须先于首观察捷径

**病灶**：初版 `observe()` 把「首次观察 → 报 `kConnecting`」的捷径放在最前面 —— `close()` 之后（`reported_` 仍为 false）再来一次观察，捷径会绕过 kClosed 吸收态，**重新**报一条。测试 `CloseIsTerminalAndIdempotent` 当场抓住。

**修法**：终态（`kFailed` / `kClosed`）闸门提到捷径**之前**。失败/关闭之后任何观察都返回 false。

**给后续切片的教训**：**吸收态要挡在所有「发报告」捷径的前面**，不只是挡在 `transition()` 里 —— 一个机器如果有「首次观察必发」的特殊路径，它和「终态之后静默」必须在同一处判。

### 5.2 不吸收终态会让 `kConnected ↔ kFailed` 逐 tick 振荡

**病灶**：设计初稿只让「同状态不重复报」限制频率，没把 `kFailed` 设成吸收态。DTLS 失败（ICE 层还活着）后，下一次 tick `observe(Connected)` 会把状态从 `kFailed` 迁回 `kConnected`，再下一次……报告在两端振荡，宿主看到的是「失败 → 又通了 → 又失败」。

**修法**：`transition()` 把 `kFailed` / `kClosed` 设成吸收态（spec 的迁移图本来就结束在 `failed` / `closed`）；`close()` 是唯一例外（允许 `kFailed → kClosed`）。测试 `FailedIsAbsorbing` / `DtlsFailureReportsFailedAndStaysFailed` 钉住。

**给后续切片的教训**：**状态机终态必须显式判「吸收」** —— 迁移图「结束在某状态」在实现里要变成「没有任何迁移离开它」，否则图只是装饰。
