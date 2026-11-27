<!--
  ⚠️ 这是**修订稿**，不是已签字的规格。

  已签字的规格是 `specs/slices/v3-n4-d6-connectivity.md`
  （frozenHash: sha256:cf09ec41a3ed382857b265cfbbb9740bcd9b8c9eaf27a89a720b5889d0cdc95f）。
  那一份**保持原样不动** —— 它是签字时刻的证据锚点。

  本文件是它的事后修订稿：实现把连通性 profile 放在
  `profiles/connectivity/connectivity-profiles.json`，而 frozen spec 写的是
  `profiles/connectivity-profiles.json`。

  为什么偏离、偏离的范围、以及这次偏离暴露的追溯缺口，
  记在同目录的 `v3-n4-d6-connectivity.amendment.md`。

  差异仅限路径；运行时行为不变（引擎读的是代码里写死的
  `resolve_connectivity_profile()`，该 JSON 是文档性制品）。
-->

# Slice: v3-n4-d6-connectivity

## 目的

实现 **N4 连通性诊断** 与 **D6 连通性 profile**：

- **N4**：一个连不上的会话，光看 stats 快照就能说出**为什么**连不上。
- **D6**：一个只填了 profile 名字的引擎，在真实用户的网络上能建立会话——
  STUN 会话穿越、TURN/TLS（凭据可用时）、UDP 失败后 TCP 兜底；
  没有 STUN / TURN 可达时**仍然尝试并如实报告**（用 N4 的词汇），
  而不是悄悄降级。

## 背景与边界（读规格前必读）

**本片只加字段与加配置，不改 ICE 协议行为。**

已核实的事实：

- `src/engine/include/nimrtc/engine/engine.hpp` 的 `TransportStats`（约 2143 行）
  已有 `ice_state` / `ice_restart_count` / `selected_pair` / `relayed` /
  `rtt_ms` / `has_rtt` / `dtls_*` / `srtp_*`。
  **本片在 `TransportStats` 上加 3 组字段**（见「接口约定」§1）。
- `src/engine/src/engine_transport_stats.cpp` 的 `append_transport_stats()`
  是 `TransportStats` 的**唯一装配点**——引擎不存副本，
  ICE 线程的状态变化不会造成第二份答案。
- `src/plugins/include/nimrtc/plugins/ice_transport.hpp` 的 `IICETransport`
  有 `state()` / `gathered_local_candidates()` / `selected_pair()` /
  `restart_count()` / `turn_transport_supported()`。
  `gathered_local_candidates()` 返回的是**完整的 SDP 候选行**
  （`src/modules/ice/src/ice.cpp` 的 `on_candidate()` 里
  `local_candidates.emplace_back(std::move(line))`），
  所以**每类型的候选计数可以由引擎从候选行里数出来**，
  **不需要给 seam 加新方法**。
- `src/modules/ice/src/ice.cpp` 的 `resolve_mode()`：
  配了 TURN 服务器且没显式钉并发模式时，MUX 自动切成 POLL
  （libjuice 的 MUX 模式不支持 TURN 分配）。**这条已经是修好的事实**，
  本片不碰。
- `src/modules/ice/src/ice.cpp` 的 `turn_transport_supported()`：
  Udp / Tcp / Tls 三种**都返回 true**（TLS 客户端在本模块的 `turn_tls.cpp`）。
  所以 `turn_transport_used` 报的是**实际用的那种**，
  不是「支持哪种」。

**N4-1 的答案（已定）：不做被丢弃的候选对列表。**
libjuice 只看得到被提名的那一对；引擎自己维护一份列表，
描述的是引擎而不是网络。默认 `No`。

**D6-1 的答案（已定）：profile 默认开。**
`"auto"` = STUN 开、凭据存在或 gathering 需要中继时开 TURN/TLS、
UDP 失败后 TCP、N3 的恢复策略待命。
宿主显式设置的键（STUN / TURN 服务器列表）**优先于 profile 的默认值**。

## 本片实现期的三个决定（2026-11-27）

frozen spec 有三处没有落地，实现把它们定下来。**这三个决定改的是
frozen spec 没说清的地方，不是推翻 frozen spec 的结论**，
所以它们记在这里（修订稿），而不是改写 frozen spec。

### 决定一：`credentials-rejected` 走 **seam**，不是引擎推导

**frozen spec 的问题**：它要求 `ice_failure_reason` 是个六值封闭词汇表，
其中 `credentials-rejected` 表示「TURN 服务器拒绝了凭据」。但
**libjuice 不把 TURN Allocate 的错误码交给调用方**——
`src/third_party/libjuice/src/src/agent.c` 的
`agent_process_turn_allocate()` 在 401 分支只打一行
`"TURN authentication failed"` 的日志，然后
`entry->state = AGENT_STUN_ENTRY_STATE_FAILED`。错误码（401 / 438 /
其它）就此消失。

**决定**：**在 seam 上加一个 TURN 分配结果**，由 ICE 模块读 libjuice
的 Allocate 响应错误码分类后报出来。

**理由**：

* 引擎侧**推导不出**这个事实。引擎能看到的只有「配了 TURN 但没拿到
  relay 候选」——那是 `no-candidates`，与「凭据被拒」是不同的事实，
  而凭据被拒是宿主最需要知道的一种（换凭据 vs 换网络）。
* 归到 `no-candidates` 是**说谎**：它把一个可行动的失败报成一个模糊的
  症状。这正是 N4 要消灭的东西。
* 归到 `none`（不报）更糟。

**代价**：要改 vendored 的 libjuice 树。仓库已有这个机制
（`src/third_party/libjuice/patches/`，N1 的 TURN/TLS 就是
`0002-turn-tls-transport.patch`），本片新增
`0003-turn-allocation-failure-reason.patch`，用
`tools/make_libjuice_turn_failure_patch.ps1` 重新生成。

**补丁的形状**（窄，不改 libjuice 的行为）：

```c
// include/juice/juice.h
typedef enum juice_turn_failure {
    JUICE_TURN_FAILURE_NONE = 0,
    JUICE_TURN_FAILURE_UNAUTHORIZED = 1,   // 401: the credentials were refused
    JUICE_TURN_FAILURE_STALE_NONCE = 2,    // 438: the nonce went stale
    JUICE_TURN_FAILURE_TRANSPORT = 3,     // the transport could not be opened
    JUICE_TURN_FAILURE_ALLOCATE = 4,      // any other Allocate error
} juice_turn_failure_t;

JUICE_EXPORT juice_turn_failure_t juice_get_turn_failure(juice_agent_t *agent);
```

agent 多一个字段 `juice_turn_failure_t turn_failure`，在
`agent_process_turn_allocate()` 的失败分支里**就地**记下分类；
`agent_turn_state_t::failed`（TURN/TLS 没有可用客户端）记
`JUICE_TURN_FAILURE_TRANSPORT`。

**为什么在 libjuice 里分类、而不是把原始错误码透出去**：透出去要给
调用方一个 STUN 错误码的词汇（NimRTC 的 `ice_failure_reason` 只有六个
值，不该让 libjuice 的码表泄漏进公开面）。分类在**唯一知道上下文**
的地方做——那里知道 401 是凭据问题、438 是 nonce 问题、而 `failed`
标志是传输层问题。

### 决定二：`turn_transport_used` 从**引擎自己的配置**读

引擎经 `add_turn_server_with_transport()` 把 TURN 服务器交给
`ice_t_`，但 `IceConfig::turn_servers` 是**引擎自己**的一份副本
（`src/engine/src/engine_transport.cpp`），ICE 模块持有**另一份**。

**决定**：引擎从**自己的**配置读。

**理由**：

* 引擎知道自己配了什么；这个字段报的是「本会话用的 TURN 传输」。
* 改 seam 让它报告「实际用的」会更准，但**这一版没有必要**：
  一个引擎会话配一组 TURN 服务器，全部用同一种 transport。
* seam 的 `turn_transport_supported()` 已经存在，
  `transport-unsupported` 那一条从它读。

### 决定三：D6 的默认 STUN 服务器是**内置公共 STUN**

frozen spec 说「一个没设任何 STUN / TURN 键的引擎，用 `"auto"` 打开时，
STUN 被配上」——但**没有定义配哪个**。

**决定**：`"auto"` profile 配上 `stun.l.google.com:3478`。

**理由**：

* 仓库里已经有 4 处用这个默认值
  （`examples/remote-desktop/demo-p2p.cpp:1145`、
  `rd_device.cpp:348`、`rd_viewer.cpp:259`、`README.md:68`），
  但**都在示例里**——D3 那条「默认策略住在示例里」的批评，
  在 STUN 这一项上同样成立。
* 引擎是 SDK：宿主不该为了拿到一个 srflx 候选而知道
  `stun.l.google.com` 这个字符串。
* **宿主的显式值仍然赢**：`EngineConfig::stun_server_host` 非空时
  绝不被 profile 覆盖（验收标准 5）。

**端口为什么是 3478**：Google 的 STUN 服务也听 19302（仓库示例用的是
那个），但 3478 是 IANA 分配的 STUN 端口（RFC 8489 §9），
是所有 STUN 部署都同意的那个，所以默认该带它。

**不选「profile 里写具体地址」**：那让 profile 文件承载部署细节，
而 profile 的其它键（`stun` / `turn_tls` / `tcp_fallback`）都是**意图**
不是地址。**不选「不内置任何 STUN」**：那样 `"auto"` 就不是一个
「开」的 profile，D6-1 定义的「profile 默认开」落空。

### 决定四：`checks-failed` 排在 `no-pair` **前面**

frozen spec 的优先级表把 `no-pair`（第 4）排在 `checks-failed`（第 5）
前面。实现把它**倒过来**了。

**理由**：一个 agent 已经放弃的会话**同时**有候选和没有提名的 pair，
两个事实都在。但 `"no-pair"` 读起来像「**还在等**一个 pair」，
而一个已经放弃的会话不是在等。所以 `"no-pair"` 是**等待中**的答案，
是五个失败原因里最轻的，排在最后。

## 范围

写入：
- `src/modules/ice/**`
- `src/engine/**`
- `src/third_party/libjuice/**`（补丁 + 树内改动，见决定一）
- `profiles/**`
- `docs/api/**`
- `tests/acceptance/v3-n4-d6-connectivity/acceptance.test.cjs`

只读参考（**不得修改**）：
- 只读 `docs/plan/v1.4-plan.md` —— N4 / D6 的原始定义与决策表
- 只读 `src/plugins/include/nimrtc/plugins/ice_transport.hpp` —— seam 契约
- 只读 `src/engine/src/engine_transport_stats.cpp` —— stats 装配点

## 需求

- **N4**：`TransportStats` 增 `ice_gathering_state` / `ice_failure_reason` /
  `turn_transport_used` 三个字段，外加每类型候选计数
  （`host` / `srflx` / `relay`）。
- **N4**：`ice_failure_reason` 是一个**封闭的小词汇表**：
  `"none"` / `"no-candidates"` / `"no-pair"` / `"checks-failed"` /
  `"credentials-rejected"` / `"transport-unsupported"`。
- **N4-1**（已定，**不做**）：被丢弃的候选对列表**不在本片范围**。
- **D6**：`EngineConfig` 增 `connectivity_profile` 键，默认 `"auto"`。
- **D6**：`profiles/` 下预置连通性 profile 模板。
- **D6**：宿主显式设置的 STUN / TURN 键**优先于** profile 的默认值。
- **D6**：默认 STUN 服务器是 `stun.l.google.com:3478`（决定三）。
- **D6-1**（已定）：profile 默认**开**。

## 接口约定

### 1. `TransportStats` 的加法式扩展

`src/engine/include/nimrtc/engine/engine.hpp` 的 `TransportStats` 加这些字段
（**全部带默认值**，不改已有字段的语义，ADR-015 加法式）：

```
std::string ice_gathering_state;   // "new" / "gathering" / "complete"
std::uint32_t ice_host_candidates = 0;
std::uint32_t ice_srflx_candidates = 0;
std::uint32_t ice_relay_candidates = 0;
std::string ice_failure_reason;    // 封闭词汇表，见上
std::string turn_transport_used;   // "udp" / "tcp" / "tls"
```

**`ice_gathering_state` 与 `ice_state` 是两件事**，不能合并：
`ice_state` 是 ICE **agent** 的状态机
（`disconnected`/`gathering`/`connecting`/`connected`/`completed`/`failed`），
`ice_gathering_state` 是**候选收集**这一个动作的状态
（`new` = 还没开始收集 / `gathering` = 正在收集 / `complete` = 收集结束）。
一个 agent 可以在 `connected` 之后仍处于 `gathering`（reflexive 候选迟到），
所以两个字段各自独立取值。

**`ice_failure_reason` 的判定优先级**（先命中先报）：
1. `transport-unsupported` —— 宿主配的 TURN 传输这版不支持
2. `credentials-rejected` —— TURN 服务器拒绝了凭据
3. `no-candidates` —— 收集结束，`host` / `srflx` / `relay` **三个计数全为 0**
4. `checks-failed` —— ICE agent 进了 `failed` 状态
5. `no-pair` —— 有候选，但 `selected_pair` 为空（还没提名）
6. `none` —— 以上都不成立

**`checks-failed` 为什么排在 `no-pair` 前面**（本片修订，见决定四）：
一个 agent 已经放弃的会话**同时**有候选和没有提名的 pair，两个事实都在。
但 `"no-pair"` 读起来像「**还在等**一个 pair」，而一个已经放弃的会话
不是在等。所以 `"no-pair"` 是**等待中**的答案，是五个失败原因里最轻的，
排在最后。

**没有失败时不编造理由**：一切正常时是 `"none"`。

### 2. `IICETransport` 的加法式扩展

`src/plugins/include/nimrtc/plugins/ice_transport.hpp` 是 **STABLE 公开头**，
**不能**加纯虚函数（会破坏所有外部实现者）。因此新增的成员**全部带默认实现**：

```cpp
/** The candidate *gathering* action's state, independent of the agent's
 *  own state machine: "new" / "gathering" / "complete". */
virtual std::string_view gathering_state() const noexcept { return "new"; }

/** Why the last TURN allocation failed, if it did.  Empty = no failure. */
virtual std::string_view turn_failure_reason() const noexcept { return {}; }
```

两个方法的默认实现返回「没有信息」的值，所以**既有实现者不受破坏**
（这与 `IVideoCapture` 的处理方式一致——见
`specs/slices/v1-device-policy.md` §「接缝侧：新增『设备变更』的可选能力」：
新增能力用**独立接口**或**带默认实现的方法**，不用纯虚函数）。

`src/modules/ice/src/ice.cpp` 的 `IceTransport` 实现这两个方法：
`gathering_state()` 从 `impl_->gathering_done` 与 `impl_->state` 推出
（`gathering_done` 为真 → `"complete"`；agent 在 `Gathering` →
`"gathering"`；否则 `"new"`）；
`turn_failure_reason()` 从 libjuice 的 `juice_get_turn_failure()` 读。

### 3. `EngineConfig` 的加法式扩展

```
std::string connectivity_profile = "auto";
```

**`"auto"` 是默认值，也是「开」的意思**（D6-1）。
它意味着：STUN 开（`stun.l.google.com:3478`）、凭据存在时开 TURN/TLS、
UDP 失败后 TCP 兜底。

**profile 携带的是「策略默认值」，不是「seam 值」**：
宿主显式设置的 STUN / TURN 服务器列表**优先**，
profile 只在宿主**没设**的那些键上补默认值。

### 4. `profiles/` 下的连通性 profile 模板

`profiles/connectivity/connectivity-profiles.json`（新文件），承载各 profile 的**默认值**：

```
{
  "auto":            { "stun": true,  "turn_tls": "when_available", "tcp_fallback": true },
  "conference":     { "stun": true,  "turn_tls": "when_available", "tcp_fallback": true },
  "remote-desktop": { "stun": true,  "turn_tls": "always",         "tcp_fallback": true }
}
```

**`profiles/` 下的既有文件是 `profiles/*.json` 的引擎 profile（agent / sfu / call 等）**，
它们的 schema 在 `profiles/schema/profile-v1.2.json`。
**本片新增的 `profiles/connectivity/connectivity-profiles.json` 是另一类东西**，
它**不进入引擎 profile 的加载路径**——它只被 `connectivity_profile` 这个键读。
两者的区别必须在文件头注释里写清楚，
否则下一个读的人会以为它该被 `profiles/schema/` 管。

**为什么放在 `profiles/connectivity/` 子目录而不是 `profiles/` 根下**：
`profiles/` 根下的 `*.json` 是**引擎 profile 的命名空间** ——
`src/modules/assembly/tests/test_profile_schema.cpp` 的
`ProfileSchema.EveryShippedProfileValidatesUnderV12` 会把**根目录下每一个**
`*.json` 拿 `profiles/schema/profile-v1.2.json` 校验。
本文件是「profile 名 → 默认值」的映射表，不是单个引擎 profile，
按 v1.2 校验会因缺 `name` 而失败（门禁②实测：`missing required property 'name'`）。
放进子目录即在命名空间之外 —— 该测试只扫根目录。
（引擎侧读的是代码里写死的 `resolve_connectivity_profile()`，
这个 JSON 是文档性制品，所以位置不影响运行时行为。）

## 验收标准

1. **`ice_gathering_state` 取值正确**：
   收集开始前是 `"new"`，收集中是 `"gathering"`，收集结束后是 `"complete"`；
   且它与 `ice_state` **独立**（`ice_state == "connected"` 时
   `ice_gathering_state` 可以是 `"gathering"`）。
   验证方式：单元测试直接断言这三个取值。

2. **候选计数按类型分开**：
   收集到的候选里，`host` / `srflx` / `relay` 三个计数各自正确，
   总数等于 `gathered_local_candidates()` 的条数。
   验证方式：单元测试用固定的候选行集合断言三个计数。

3. **`ice_failure_reason` 走封闭词汇表**：
   六个取值都能被造出来，且**任一取值都不在这六个之外**。
   优先级的顺序也被钉住（`transport-unsupported` 压过 `no-candidates`）。
   验证方式：单元测试逐个造场景断言。

4. **`turn_transport_used` 报实际用的那种**：
   走 TURN/UDP 的会话报 `"udp"`，走 TURN/TLS 的报 `"tls"`。
   验证方式：单元测试。

5. **`connectivity_profile` 默认是 `"auto"`，且是「开」的**：
   一个**没设任何 STUN / TURN 键**的引擎，用 `"auto"` 打开时，
   STUN 被配上；一个显式设了 STUN 服务器的引擎，**它的显式值不被覆盖**。
   验证方式：单元测试断言「profile 补默认值」与「显式值优先」两件事。

6. **`profiles/connectivity/connectivity-profiles.json` 存在且完整**：
   文件存在，每个 profile 的键集一致（`stun` / `turn_tls` / `tcp_fallback`）。
   验证方式：验收脚本读文件并断言键集。

7. **不静默降级**：
   没有任何 STUN / TURN 可达时，引擎**仍然尝试**，
   并用 N4 的词汇**报告它试了什么**（`ice_failure_reason` 非 `"none"`，
   或 `ice_gathering_state` 显示收集确实跑过）。
   验证方式：验收脚本在无 STUN / TURN 环境下断言。

8. **API 契约同步**：
   `TransportStats` / `EngineConfig` 加了公开字段，
   `docs/api/public_headers.manifest` 与 `tests/test_api_classification.cpp`
   的基线**随之更新**（ADR-015）。
   验证方式：`tests/test_api_classification.cpp` 自身通过。

## 风险

- **加公开字段要动 ADR-015 的基线**：`TransportStats` / `EngineConfig` 在
  `src/engine/include/nimrtc/engine/engine.hpp` 里，是公开头。
  `tests/test_api_classification.cpp` 的 `kBaselineHeaderCount` 与
  `docs/api/public_headers.manifest` 的 count 必须跟着更新。
  **加字段不改头文件数量**，但分类扫描会把新字段算进公开面，
  基线数可能需要按 `v1-device-policy` 的先例核对一次。
- **词汇表是封闭的**：多写一个取值就是改了契约。
  本片只认上面列的六个。
- **`profiles/connectivity/connectivity-profiles.json` 不属于 `profiles/schema/` 管的那些文件**，
  别把它塞进引擎 profile 的加载路径。
- **改 vendored 的 libjuice 树**（决定一）：这是本片最大的风险面。
  补丁必须**窄**——只加一个分类字段与一个读函数，不改 libjuice 的任何
  行为；`vendor.json` 的 libjuice note 必须同步更新；
  补丁要能被 `tools/make_libjuice_turn_failure_patch.ps1` 重新生成。
- **默认 STUN 是公共服务器**（决定三）：`stun.l.google.com` 不可用时
  引擎仍然工作（只是没有 srflx 候选），N4 会报 `no-candidates`。
  这不是错误，是**如实报告**。
