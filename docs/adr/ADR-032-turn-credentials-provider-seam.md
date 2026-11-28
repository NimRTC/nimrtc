# ADR-032: TURN 凭据的归属 —— 引擎拉取，宿主持有

| | |
|---|---|
| Number | 032 |
| Status | **Proposed** |
| Date | 2026-11-24 |
| Phase | v1.4 (N7) |
| Bound to | v1.4.0 |
| Supersedes | — |
| Related | `docs/plan/v1.4-plan.md` §N7；`docs/adr/ADR-015-api-stability-policy.md`；`docs/adr/ADR-025-sdp-rtp-not-pluggable.md`；`docs/adr/ADR-031-ice-recovery-policy-ownership.md`（N3）；`specs/slices/v1-ice-recovery.md`（N3）；`specs/slices/v1-turn-credentials.md`（本片） |

---

## 1. 背景

真实 TURN 凭据有 **TTL**（`username:timestamp` + HMAC，RFC 7635 那种）。而树里今天的路径是：

- `add_turn_server()` 是**一次性手动**入口（`engine.hpp` 明写 "Must be called BEFORE open()"）；凭据进到 ICE 模块也是**一次性**的 —— `IceTransport::add_turn_server()` 把 `TurnServer` push 进 `config.turn_servers`，**建 agent 时**拷进 libjuice，**建完之后再也不会变**（`ice.cpp:655-671`）。
- ICE 模块**已经能识别凭据被拒**：进程级闩锁 `g_turn_auth_rejected`（`ice.cpp:63`），由 libjuice 的日志钩子置位（收到 401/403 时打 "TURN authentication failed"）。
- N4 的失败原因词汇**已经有** `"credentials-rejected"`（`ice_failure_reason()`），但**从来选不中** —— 因为凭据过期后没有人更新 TURN 配置，会话只能报笼统的 `"checks-failed"`。

于是：**凭据过期 = 会话哑掉，没有任何地方会再问一次。** N7 的验收标准是 "credentials as a first-class seam" —— 引擎把「怎么拿凭据」交给宿主，在需要的时候问。

## 2. 决策

### 2.1 决策一：公开面新增 `TurnCredentials` 与 `TurnCredentialsProvider`（函数指针），定义在 `engine.hpp`

```cpp
struct NIMRTC_API_STABLE TurnCredentials {
    std::string host;
    std::uint16_t port = 3478;                 // 0 = 默认（TURN/UDP 3478）
    std::string username;                      // 空 = 匿名分配
    std::string password;                      // 空 = 匿名分配
    plugins::TurnTransport transport = plugins::TurnTransport::Udp;
    std::string tls_server_name;               // TURN/TLS 时的 SNI
};

using NIMRTC_API_STABLE TurnCredentialsProvider = bool (*)(void* ctx, TurnCredentials* out) noexcept;
```

**为什么是函数指针而不是 `std::function`**：公开头要跨 ABI 稳定（ADR-015），`std::function` 的 ABI 不是稳定承诺的一部分。函数指针 + 上下文指针与树里其它回调接缝同形（`set_on_ice_recovery` / `set_on_error`）。

**为什么在 `engine.hpp` 定义一份，而不是复用 `nimrtc::ice::TurnServer`**：接缝纪律（L4 / ADR-025）—— `engine.hpp` 不 include `nimrtc/ice/` 的任何具体头。复用会把 ICE 模块的具体类型引进公开面。

**为什么是 `EngineConfig` 上的回调而不是「注册的接缝」**（§N7-1 已决）：这样「有静态服务器列表」的宿主**继续调 `add_turn_server()`**，什么都不改 —— 加接缝会逼所有宿主迁移。

### 2.2 决策二：引擎在两个时机重新拉取

1. **gathering 之前**（`open()`）：没有静态 TURN 配置、但配了 `turn_credentials` 时，问一次并 `add_turn_server()`。
2. **重连时**（N3 的重连点）：N3 的恢复重启了 ICE **之后**，重新问 —— 因为旧凭据可能已经过期。

**为什么第二个时机是关键**：凭据有 TTL。静态用户名/密码没有寿命概念，但真实的实时 TURN 凭据有（`username:timestamp` + HMAC）。**寿命是提供者的业务，引擎必须再问一次。** 引擎在 N3 的重连点上**挂**这一次咨询（N3 的机器本身本片不改）。

**拉取是「问一次拿一次」**：引擎不替宿主缓存、不预取 —— 缓存与预取是宿主的业务。

### 2.3 决策三：私有协作者 `TurnCredentialsState` 做拉取记账

引擎的 `open()` 与 tick 是**两条不同的路径**，都要问凭据。没有一个共同的记账点，「问了几次」「失败几次」「被拒过没有」就散在两处，会各自漂移。按 N3 的形状（私有 TU、时钟显式传参、确定性单测）：

```cpp
namespace nimrtc::engine::detail {
class TurnCredentialsState {
public:
    bool refresh(std::int64_t now_us, TurnCredentialsProvider provider,
                 void* ctx, TurnCredentials* out) noexcept;
    bool consecutive_failures() const noexcept;
    bool rejected() const noexcept;
    void note_rejected() noexcept;
};
}
```

**`refresh()` 的语义**：
- 提供者为 `nullptr` → 返回 `false`，状态**不动**（不咨询、不崩）。
- 提供者返回 `true` 且 `host` 非空 → 记录凭据（`port` 0 归一化为 3478，变化检测比较**生效值**），清连续失败计数，状态 `kOk`。
- 提供者返回 `false`，或返回 `true` 但 `host` 为空 → 连续失败计数 +1，状态 `kUnavailable`。空 host **当作拿不到**（RFC 5766 的匿名分配由**空 username/password** 表达，不是空 host）。

### 2.4 决策四：静态服务器优先

`add_turn_server()`（两个重载）的**签名与语义不变**。引擎在 `SessionState` 里记 `has_static_turn_server`：一旦宿主显式加过静态服务器，提供者**永远不被咨询** —— 静态配置是宿主的显式意图，不会被回调覆盖。

**为什么静态优先**：回调是「引擎在需要时问」的便利，静态列表是「宿主明确指定」的配置。两者冲突时，显式的赢。

### 2.5 决策五：凭据被拒**真的能报出来**，且加 `Stats::turn_credentials_state`

- N4 的 `ice_failure_reason()` 已有 `"credentials-rejected"`。本片让它在凭据被拒时**真的被选中**（优先级：`transport-unsupported` → `credentials-rejected` → `no-candidates` → `no-pair` → `checks-failed` → `none`）。
- 新增扁平 `Stats::turn_credentials_state`，取值 `""`（未配提供者）/ `"idle"`（配了但没拉过）/ `"ok"` / `"unavailable"` / `"rejected"` —— 让宿主**不解析日志**就知道结果。
- `"rejected"` 是**粘性闩锁**：ICE 模块的 401/403 闩锁是进程级、会随重启清除，但协作者自己保留一份粘性的（`note_rejected()`），`open()` 时重置。

## 3. 后果

### 3.1 公开面的变化（全部是加法）

| 符号 | 变化 |
|---|---|
| `TurnCredentials` | 新增结构体（6 字段） |
| `TurnCredentialsProvider` | 新增类型别名（函数指针） |
| `EngineConfig::turn_credentials` / `turn_credentials_ctx` | 新增字段，默认 `nullptr` |
| `Stats::turn_credentials_state` | 新增字段，默认 `""` |

`add_turn_server()`（两个重载）**签名与语义不变**；`plugins::IICETransport` **不动**（重新咨询在引擎侧，接缝之上）；`nimrtc::ice::TurnServer` **不动**。

### 3.2 依赖方向

- 新增私有协作者 `src/engine/src/engine_turn_credentials.{hpp,cpp}`：只 include 引擎自己的公开头，**不** include `nimrtc::ice/` 的任何具体头（与 N3 的 `engine_ice_recovery.hpp` 同形）。
- `src/modules/ice/` **不改** —— 凭据的重新咨询在 ICE 接缝**之上**。
- 引擎的 tick / open 路径驱动；`refresh_turn_credentials()` 只有引擎侧两个调用点（`open()` 与 N3 重连重启），没有宿主线程的直接入口。

### 3.3 线程规则

提供者在**引擎的 tick 线程**上被调用（与 `set_on_ice_recovery` 同一线程），文档写明「必须不阻塞、不重入引擎」。宿主的实现若阻塞，引擎的 tick 会卡住 —— 这是本接缝的主要风险，规格验收标准 18 钉住它。

### 3.4 已知并接受的限制

ICE 模块的凭据被拒闩锁是**进程级**的（`g_turn_auth_rejected`），多引擎并存时可能互相污染 —— 引擎读的是自己那个 transport 的观测，但闩锁的置位来自 libjuice 日志钩子，不区分引擎。这在 v1.4 内**已知并接受**；宿主每进程通常只有一个引擎。

## 4. 备选方案与不选的理由

| 备选 | 不选的理由 |
|---|---|
| 复用 `nimrtc::ice::TurnServer` | 把 ICE 模块的具体类型泄漏进 `engine.hpp` 公开面，违反 L4 / ADR-025 |
| 用 `std::function` | ABI 不稳定（ADR-015）；树里其它回调接缝都是函数指针 + 上下文指针 |
| 「注册的接缝」（`set_turn_credentials_provider()`） | 逼所有宿主迁移；`EngineConfig` 上的字段让「有静态列表」的宿主继续用 `add_turn_server()`，什么都不改 |
| 引擎缓存凭据 / 预取 | 凭据有 TTL，缓存是宿主的业务；引擎问一次拿一次 |
| 凭据被拒仍报 `"checks-failed"` | 宿主无法区分「网络没救」与「凭据过期该换」；`"credentials-rejected"` 词汇已在 N4 存在，让它选不中是浪费 |
| 引擎自己签发 / 刷新凭据 | 谁签发是宿主的业务（RD-7）；N7 是接缝，不是服务器 |

## 5. 落地注意（给后续切片）

- **测试写了要连 CMake 注册**（ADR-031 §5.1 的教训）：本片新增 `test_engine_turn_credentials.cpp`，同时把它登记进 `src/engine/tests/CMakeLists.txt`。
- **公开字段要连「谁在装配那个面」**（ADR-031 §5.2 的教训）：扁平 `Stats` 由 `engine_media.cpp` 装配。本片把 `turn_credentials_state` 投影挂在 `append_ice_recovery_stats()` 之后（N3 已把它接进装配链），不再动 `engine_media.cpp`。
- **API 分类扫描是全库级的**（ADR-031 §5.3 的教训）：新公开符号必须带 `NIMRTC_API_STABLE`，否则 `scripts/api_freeze_scan.js` 会在门禁②上拦下本片。
