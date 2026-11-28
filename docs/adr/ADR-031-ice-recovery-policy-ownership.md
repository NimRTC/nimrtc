# ADR-031: 重连策略的归属 —— 引擎管自动重启，宿主管重新协商

| | |
|---|---|
| Number | 031 |
| Status | **Accepted** |
| Date | 2026-11-23 |
| Phase | v1.4 (N3) |
| Bound to | v1.4.0 |
| Supersedes | — |
| Related | `docs/plan/v1.4-plan.md` §N3、§N8、§D6-1、§D7-1；`docs/adr/ADR-015-api-stability-policy.md`；`docs/adr/ADR-024-engineconfig-string-ownership.md`；`specs/slices/v3-n4-d6-connectivity.md`（D6/N4）；`specs/slices/v1-video-render-engine.md`（D2，私有协作者的先例） |

---

## 1. 背景

v1.4 计划书 §N3 的验收标准是：

> a network change recovers **without a manual restart**; restarts are bounded; one policy

计划书给了机制（大致的形状）：

> A named private collaborator (`src/engine/src/engine_ice_recovery.{hpp,cpp}`, the same shape `FeedbackGuard` took in v1.3.0): a grace window on `Disconnected`, an exponential backoff with a cap, a **terminal** state when the cap is reached (reported, not looped), driven by the engine's own ICE state. `restart_ice()` stays as the manual entry point and counts against the same budget.

但有**四件事计划书没有定死**，且它们互相耦合。本 ADR 显式记录选择与理由。

### 1.1 事实一：本地重启**不足以**让会话真的恢复

RFC 8445 §9.1.1.1：ICE 重启会换掉本端的 `ufrag` / `pwd`。对端只能通过 SDP 里的新凭据观察到这次重启，因此**对端必须重新协商**。换句话说：

- 引擎单方面重启，只能保证**本端**重新收集候选、重新连通能力恢复；
- **对端**必须拿到新凭据并重新协商，会话才真的回来。

这是计划书 §N8 写的那句「the load-bearing fact」的物理基础，也是 v1.2 计划书 C3（「统一重连策略」）**不可能**只靠传输层修好的原因。

### 1.2 事实二：树里已经有「对端重启」的一半，没有「本地重启」的一半

- `set_on_ice_restart(plugins::OnIceRestart)` 已存在，语义是**对端**的 SDP 换了凭据（`src/engine/src/engine_transport.cpp:517`）。
- `restart_ice()` 已存在，是**手动**入口（`src/engine/src/engine.cpp:865`），且它的两步契约（`restart_ice()` 然后 `send_to_peer(create_offer())`）写在公开头的文档里（`engine.hpp:3597`）。
- **没有**任何东西在引擎侧观察 ICE 掉线并自动重启。

### 1.3 事实三：`restart_ice()` 的两步契约**不能被引擎单方面完成**

第二步 `send_to_peer(create_offer())` 需要宿主的信令通道 —— 引擎**不知道**怎么把 SDP 发给对端。计划书 §N8-2 已经决定「**只报告，不代劳**」（report only）：「The SDP exchange belongs to the host's signalling」。

所以 N3 的「自动恢复」在**本端**是完整的（引擎自己重启、自己退避、自己封顶），在**对端**是**半截**的：引擎必须告诉宿主「我重启了，请发新 offer」。N8 补的是「路径变了」的细节（old/new pair + reason），N3 必须先补「该重新协商了」这件事本身。

### 1.4 事实四：D6 已经说了「auto profile 武装 N3」，但没说怎么关

计划书 §D6-1：

> `"auto"` means STUN on, TURN/TLS when credentials exist or gathering requires a relay, TCP after UDP, **N3 armed**. A host that wants to own the policy sets explicit keys and wins.

「N3 armed」是 D6 对 N3 的**唯一**交叉引用，但计划书**没有**定义一个宿主能显式设的 key。宿主想自己管重连（这是合理的 —— 有的产品有自己的重连策略和遥测），今天**没有任何开关**。

## 2. 决策

### 2.1 决策一：新增独立的回调 `set_on_ice_recovery`，**不**扩大 `set_on_ice_restart` 的语义

引擎自动重启后，调用一个**新的**回调，把「该重新协商了」这件事告诉宿主。

```cpp
/** 本地 ICE 重启的结果（v1.4 N3）。 */
struct IceRecoveryReport {
    /** 为什么重启："local-address-changed" / "ice-failed" / "manual" */
    std::string reason;
    /** 本次重启后到目前为止的重启次数（含手动重启）。 */
    std::uint32_t restart_count;
    /** 预算内还剩几次（0 = 已达终态）。 */
    std::uint32_t remaining_budget;
    /** 终态：预算用尽，重连不再继续。 */
    bool terminal;
    /** 终态原因（空 = 未达终态）。 */
    std::string terminal_reason;
};

using EngineIceRecoveryCallback = std::function<void(const IceRecoveryReport&)>;

NIMRTC_API_STABLE void set_on_ice_recovery(EngineIceRecoveryCallback cb) noexcept;
```

**为什么不复用 `set_on_ice_restart`：**

1. **语义不同，不该混。** `set_on_ice_restart` 的文档已经钉死「the *peer* restarts ICE」（`engine.hpp:3610`），宿主拿它判断「对端换了凭据，我要回一个 answer」。N3 的回调说的是「**我**换了凭据，我要发一个 **offer**」—— 宿主在两处要做的事**正好相反**（一个回 answer，一个发 offer）。混在一个回调里，宿主得靠传参或约定区分，那是**最容易出错**的形状。
2. **ADR-015 的稳定性。** `set_on_ice_restart` 已经在冻结面上。扩大它的语义 = 改变已冻结符号的行为，而 N3 只是**新增**。新增是加法（计划书 §6 明确「everything here is additive」），扩义不是。
3. **N8 会再给这个回调加细节。** §N8 说「the report carries the **old pair, the new pair, the reason** … `set_on_ice_restart` keeps its exact meaning … and gains the pair detail in the same struct」。N8 说的「the same struct」指的是**新回调的结构**（N8 的 old/new pair 落在这里），而不是去扩 `set_on_ice_restart`。本 ADR 把这个结构定成 `IceRecoveryReport`，N8 在**同一个结构**上加 `old_pair` / `new_pair` 两个字段即可 —— N8 是加法，不改已有字段的语义。

**回调跑在哪个线程**：tick 线程（引擎自己拥有时钟的那条），与 `set_on_state_change` / `set_on_control_message` 同一套规则（计划书 §D7：「the host's thread rule is the existing one」）。

### 2.2 决策二：新增 `EngineConfig::ice_recovery_policy`，默认 `"auto"`

```cpp
/** 重连策略：v1.4 N3。
 *
 *  "auto"（默认）—— 引擎管：ICE 掉线后引擎自己按退避策略重启 ICE，
 *                   并通过 set_on_ice_recovery 告诉宿主该重新协商了。
 *  "off"        —— 引擎不管：ICE 掉线时引擎什么都不做，宿主自己
 *                   调 restart_ice()（或用别的手段）。
 *
 *  未知名字当 "off" —— 重连策略**永远不是**会话失败的原因。 */
std::string ice_recovery_policy = "auto";
```

**为什么是独立的 key，而不是由 `connectivity_profile` 决定：**

计划书 §D6-1 说「auto profile ⇒ N3 armed」，但 D6 的 profile 有**三个**（`auto` / `conference` / `remote-desktop`），三个都 armed 的话「armed」这个信息就没有区分度；而「宿主想自己管」是一个**独立于连接 profile** 的意图（连接策略是「怎么连」，重连策略是「断了怎么办」）。两个维度合成一个 key 会让宿主要么两头都设要么两头都不设。

**为什么默认 `"auto"`：** 计划书 §N3 的验收标准是「a network change recovers **without a manual restart**」，「without a manual restart」就是「默认开着」的另一种说法。默认关掉的话，验收标准只在宿主**显式打开**时成立 —— 那是把计划书的验收标准降级成可选项。

**为什么未知名字当 `"off"` 而不是 `"auto"`：** 与 D6 同族（D6：「an unknown name falls back to "auto" — the policy is never the reason a session fails」）。但方向相反的理由是：**D6 的 fallback 是「开着」**，因为 D6 的失败模式是「连不上」（fail-closed 方向偏保守）；**N3 的 fallback 是「关着」**，因为 N3 的失败模式是「重启太频繁」（fail-safe 方向偏保守）。一个打错的 key 不该让引擎在一个宿主没预期的时刻偷偷重启 ICE。**这条不对称是刻意的**，并且必须写进 `docs/api/engine_api.md`。

### 2.3 决策三：手动 `restart_ice()` **计入同一份预算**，超预算返回 `false`

`restart_ice()` 保持它是手动入口，但它的行为被 N3 的预算约束：

- 预算内 → 正常重启，返回 `true`，并**触发** `set_on_ice_recovery`（`reason = "manual"`）；
- 预算已满（已达终态） → **不重启**，返回 `false`，并报终态（`on_error_` + stats + warn 日志）。

**为什么受限**：计划书 §N3 说「`restart_ice()` … counts against the same budget」与「there is exactly **one** policy in the engine」。如果手动调用能绕过预算，那么「一套策略」就是假的 —— 宿主可以在引擎判定「网络没救了」之后，靠连打 `restart_ice()` 把会话拖下去重试（每次重建 libjuice agent，是**有代价**的：重新收集候选、重新打洞）。受限之后，「引擎已经放弃」是一个**宿主能尊重的事实**。

**为什么不是「完全独立」**：那样会有两份额度、两个上限，宿主不知道该看哪个 —— 计划书要消灭的正是这种「三处推导一个状态」（§D7 原话）。**为什么不是「计数但不受限」**：那样预算就成了纯装饰（「引擎说 5 次，宿主可以打 100 次」），终态也就没有意义了。

**`restart_ice()` 的两步契约不变**（`restart_ice()` 然后 `send_to_peer(create_offer())`）。引擎**不**代劳第二步 —— SDP 交换是宿主的（§N8-2）。

**宿主怎么恢复预算**：终态之后，宿主要恢复自动重连必须显式重置。本 ADR 定一个新公开方法：

```cpp
/** 重置重连预算（v1.4 N3）。
 *
 *  终态之后，引擎不再自动重启 ICE。宿主如果想重新给一次机会
 *  （比如用户点了「重试」），显式调这个方法清零计数与终态。
 *
 *  预算窗口（10 分钟）**不**随它清零 —— 时间窗是滑动窗口，
 *  清零的只是「已用次数」与「终态标志」。 */
NIMRTC_API_STABLE void reset_ice_recovery_budget() noexcept;
```

**为什么需要它**：终态是一个**对外报告的状态**，不是死局 —— 用户的网络可能真的恢复了（人从 WiFi 走回工位），或者宿主换了一条更好的路径。没有重置口，宿主唯一的选择是**关掉引擎重建一个**，那比重启贵得多。

**时间窗不随重置清零的理由**：清零次数却保留窗口，宿主的「重试」按钮就仍然受「10 分钟最多 5 次」约束 —— 反复点不会把引擎变成重启永动机。这与 `CaptureReconnect`（D4）的 `note_succeeded()` 会清零计数不同：D4 清零是因为**设备回来了**（成功即重置），N3 的重置是**宿主显式要求再试一次**（未必成功），两者语义不同。

### 2.4 决策四：终态**三处**都报

预算用尽时（终态）：

1. **`on_error_` 回调** —— 带终态原因字符串（新增一个错误码，见下）；
2. **`Stats` 字段** —— 状态 / 已用次数 / 原因，让宿主**轮询**也能问出来（与 N4「a session that never connects says why from the stats alone」同一族精神）；
3. **一行 `warn` 日志** —— 与 D2 的降级日志同族（「never a silent drop」）。

**为什么三处都要**：D7 还没做（call-state 回调与错误码枚举是 D7 的活），N8 也还没做（old/new pair）。N3 不能等它们 —— 但 N3 也不能因为它们没做就**只报一处**。终态是「会话放弃了」这种级别的事实：宿主如果只轮询 stats 就该拿得到，宿主如果挂了 error 回调也该拿得到，日志也要有（线上排障的第一现场）。

**错误码**：新增 `kIceRecoveryExhausted = 0x1F01`（传输段 `0x1F00..0x1FFF` 目前空着 —— `engine_errors.hpp:74` 说「No transport-only codes are defined yet — add them here when the transport layer grows a richer error vocabulary」，正是现在）。

**Stats 字段**（N3 自这一层能回答的）：

```cpp
/** 重连状态（v1.4 N3）："idle" / "grace" / "backoff" / "terminal" / "disabled"。 */
std::string ice_recovery_state;
/** 本次预算窗口内已用的重启次数。 */
std::uint32_t ice_recovery_restarts;
/** 终态原因（空 = 未达终态）："budget-exhausted" / "restart-failed" / "manual-blocked"。 */
std::string ice_recovery_terminal_reason;
```

**明确不做**：不加「old pair / new pair」字段（那是 N8 的活，且 N8 会加在 `IceRecoveryReport` 上）；不加 call-state 枚举（D7 的活）。本 ADR 只保证 N3 这一层是**自洽且可观测**的。

**这三个字段落在**扁平 `Stats`** 上，不在 `Stats::transport` 里。**

**为什么**：重连策略是**策略事实**，不是传输面的测量结果。传输面的事实（gathering 状态、失败原因）住在 `TransportStats` 里是对的；「现在处于重连策略的哪个状态」不是传输面测出来的，是策略机自己的状态。宿主读 `stats().ice_recovery_state` 不应该还得先知道重连策略躲在 `stats().transport` 后面 —— 那是一条只有知道实现细节的人才走对的路径。规格 §六(b) 与本 ADR §3.2 的措辞都是 `Stats::ice_recovery_state`（不是 `TransportStats::`），实现按此对齐。

### 2.5 决策五：数字取计划书 §N3-1 的默认值

| 参数 | 值 | 来源 |
|---|---|---|
| grace（`Disconnected` 上的宽限窗） | 5 s | §N3-1 |
| 退避序列 | 1 / 2 / 4 / 8 s | §N3-1 |
| 退避上限（cap） | 8 s | 由序列末项推得 |
| 预算 | 5 次 / 10 分钟 | §N3-1 |
| 终态 | 达到预算 → 终态（上报，不循环） | §N3-1 |

**为什么 grace 存在**：ICE 掉线**常常**是瞬时的（WiFi 漫游的一次丢包、一次 ICE 检查超时）。没有 grace 窗，引擎会为一次 200 ms 的抖动就重启整个 ICE 会话 —— 而重启要重建 libjuice agent、重新收集候选，是**重**的操作。5 s 宽限给瞬时抖动一个自己恢复的机会。

**为什么退避要有 cap**：序列 1/2/4/8 之后若继续翻倍就是 16、32、64 s —— 一个会话掉线后等两分钟才重启，对实时通话没有意义。8 s 上限意味着「最坏情况下每 8 s 试一次」。

**为什么预算窗口是 10 分钟**：5 次 / 10 分钟 ≈ 每 2 分钟一次，是「还在认真尝试」与「已经没救了」之间的分界。窗口是**滑动的**（每次重启把窗口的起点往后推 10 分钟），不是从会话开始算起的一次性配额 —— 否则一个跑了 2 小时的会话突然掉线时会发现预算早就被历史消耗光了。

## 3. 后果

### 3.1 公开面的变化（全部是加法）

| 符号 | 变化 |
|---|---|
| `EngineConfig::ice_recovery_policy` | 新增字段，默认 `"auto"` |
| `Stats::ice_recovery_state` / `ice_recovery_restarts` / `ice_recovery_terminal_reason` | 新增三个字段 |
| `NimRTCEngine::set_on_ice_recovery(EngineIceRecoveryCallback)` | 新增 setter |
| `EngineIceRecoveryCallback` / `IceRecoveryReport` | 新增类型 |
| `NimRTCEngine::reset_ice_recovery_budget()` | 新增方法 |
| `core::kIceRecoveryExhausted` | 新增错误码（`0x1F01`） |

`set_on_ice_restart` **不动**（语义、签名、行为全不变）。`restart_ice()` 的签名不变，**行为**多了一条「预算满则拒绝」。

### 3.2 依赖方向

- 新增私有协作者 `src/engine/src/engine_ice_recovery.{hpp,cpp}`：只 include `core` / `plugins` 的东西，**不** include `nimrtc::ice` 的具体符号（与 `engine_capture_reconnect.hpp` 同形 —— 同一份规则，同一份确定性测试）。
- `src/core/` **不动**（错误码加在已有的 `engine_errors.hpp` 里，不是新文件）。
- 引擎的 tick 线程驱动；判定只读状态、纯计算（C3：不调模型、不触网）。

### 3.3 与 D6 的关系

D6 的 `connectivity_profile` 与 N3 的 `ice_recovery_policy` 是**两个正交的 key**：

- `connectivity_profile` = 「怎么连」（STUN / TURN / TCP 回落）；
- `ice_recovery_policy` = 「断了怎么办」。

`"auto"` profile 意味着 STUN 开着、TURN/TLS 优先、TCP 回落 —— 这些**与** N3 武装不冲突。宿主想两个都要默认，就两个都不设；想自己管重连，就设 `ice_recovery_policy = "off"`。

### 3.4 与 N8 的关系

N8 在 `IceRecoveryReport` 上**加** `old_pair` / `new_pair` 两个字段，并把 `reason` 的取值扩到 `"relay-fallback"`。本 ADR 的结构体是 N8 的落点，N8 是纯加法。

### 3.5 与 D7 的关系

D7 的 call-state 回调（`connecting` → `connected` → `reconnecting` → `failed` / `closed`）会把 N3 的 `reconnecting` 与 `failed` 接上。D7 落地后，本 ADR 的终态上报会**同时**出现在 call-state 回调里 —— 但那是 D7 的接线，N3 不预留也不猜测 D7 的枚举。

## 4. 备选方案与不选的理由

| 备选 | 不选的理由 |
|---|---|
| 复用 `set_on_ice_restart`（扩大语义） | 两种相反的宿主动作（回 answer vs 发 offer）混在一个回调里，最容易错；且是**扩**已冻结符号的语义，不是加法 |
| 引擎自动发新 offer（代劳信令） | §N8-2 已决定 report only；引擎不知道宿主的信令通道 |
| 手动 `restart_ice()` 不受预算约束 | 「一套策略」变成假的；终态没有意义 |
| 手动与自动各有独立额度 | 两个上限，宿主不知道看哪个 —— 正是要消灭的形状 |
| 用 `connectivity_profile` 决定是否武装 | 「怎么连」与「断了怎么办」是两个正交意图；合成一个 key 会让宿主两头都设或两头都不设 |
| 终态只报 `on_error_` | 只轮询 stats 的宿主拿不到；只挂回调的宿主在日志里看不到；线上排障需要日志 |
| 未知 policy 名字当 `"auto"` | 打错的 key 不该让引擎在宿主没预期的时刻偷偷重启；与 D6 的 fallback 方向相反是刻意的 |
| grace 窗取消 | 瞬时抖动会触发重 ICE（重建 libjuice agent），代价重 |

## 5. 落地时发现的两处「规格/实现不闭环」

本片落地时（agent 跑完、门禁②跑验收）暴露了两处**实现本身没写全**的地方。两处都记在这
里，因为它们不是「实现写错了」，而是**规格与写集没有把实现需要的东西列全** —— 写集少一
个文件，实现就少做一件事，而门禁要等验收跑起来才发现。

### 5.1 写集漏了 `src/engine/tests/CMakeLists.txt`

**病灶**：写集里有 `src/engine/tests/test_engine_ice_recovery.cpp`（测试**源文件**），却没
有 `src/engine/tests/CMakeLists.txt`（让这个源文件**被编译成二进制**的那张表）。结果：
新测试文件躺在树里，但**从来没有被编译过**。

**为什么门禁②还是绿的**：`test` 这一步跑的是 ctest —— ctest 跑的是**CMake 注册过**的
目标。没注册的目标，ctest 不知道它存在，于是「全绿」。**测试通过不代表测试跑过。**

**为什么验收能抓住**：验收测试用 `spawnSync` 直接跑 `build/src/engine/tests/Debug/
test_engine_ice_recovery.exe` 这个**路径**。二进制不存在 → `exit=null` → 断言失败。
验收层抓的是「二进制跑起来 + 用例 OK」，不看 ctest 的汇总。

**修法**：写集补 `src/engine/tests/CMakeLists.txt`（ADR-092 允许 frozen 下扩写集并留痕），
并按 `test_engine_connectivity` 的形状注册 `test_engine_ice_recovery` 目标。

**给后续切片的教训**：**凡是在写集里新增一个测试源文件，就必须同时把它的 CMake 注册
一并写进写集** —— 否则「测试写了但没编译」是静默失败。

### 5.2 写集漏了 `src/engine/src/engine_media.cpp`

**病灶**：三个 `Stats` 字段按 §2.4 落在**扁平 `Stats`** 上，而扁平 `Stats` 是由
`engine_media.cpp` 的 `stats()` 装配的（`append_transport_stats(out.transport)` 就在那里
被调）。实现最初把字段投影到了**嵌套的 `Stats::transport`** 上 —— 那里是唯一现成的投影
入口，且 N4 的 `ice_failure_reason` 也住在那里，看着「合群」。

**为什么这会编译不过**：验收测试读的是 `Stats::ice_recovery_state`（按规格），实现写的是
`TransportStats::ice_recovery_state` —— 编译期就报「不是 `NimRTCEngine::Stats` 的成员」。
**规格说的和实现做的不是同一个 struct，而 C++ 编译器替我们抓到了。**

**修法**：新增 `append_ice_recovery_stats(Stats&)`，在 `engine_media.cpp` 的 `stats()` 里
调用；三个字段从 `TransportStats` 移回扁平 `Stats`。写集补 `src/engine/src/engine_media.cpp`
与 `src/engine/src/engine_transport_stats.hpp`。

**给后续切片的教训**：**「往公开面上加字段」要连带确认「谁在装配那个面」**。本项目的
装配点分散在多个 TU（`engine_media.cpp` 装配扁平 `Stats`，`engine_transport_stats.cpp` 装配
`TransportStats`），猜错装配点就会把字段加到没人读的 struct 里。

### 5.3 顺带修掉的 v2-ice 遗留缺陷：`tcp_candidate.hpp` 少一个标记

验收判据 31-33 要求 `scripts/api_freeze_scan.js` 跑绿（0 missing）。它报了一个 missing：

```
src/modules/ice/include/nimrtc/ice/tcp_candidate.hpp
  → inline std::uint32_t TcpCandidate::priority() const noexcept   // 缺 NIMRTC_API_STABLE
```

**这不是本片造成的**：`priority()` 的**类内声明**带 `NIMRTC_API_STABLE`，但**类外的
out-of-line 定义**没带 —— 而扫描器按「符号出现处」判定，带标记的声明与不带标记的定义会
被判成两个符号，取无标记的那个报 missing。这个缺陷从 `v2-ice`（`dd396cd5`）就在基线上
（`15e1515`），`tests/acceptance/v2-ice/` 的验收**今天也照样失败**。

**为什么本片要顺手修**：判据 31-33 把它拦在门禁②上。不修就签字不了，而签字不了的
理由是「一个跟重连策略毫无关系的、别片留下的分类标记漏了」。

**修法**：给 out-of-line 定义补上 `NIMRTC_API_STABLE`（一行）。

**教训**：**API 分类扫描是「全库级别」的判据** —— 它扫所有公开头，所以任何一个别片留下的
漏标都会拦住当前片。扫描器不区分「谁写的」，只区分「标了没有」。
