# Slice: v1-sdk-sample

## Summary

**一个 ≤200 行的样例，跑完一通话 —— 证明宿主除了信令与产品 UI 什么都不用写。**

今天的 `examples/remote-desktop/` 已经证明了引擎能做很多事，但它的三个 `.cpp` 加起来 1475 非注释行（`rd_device.cpp` 488 / `rd_verify.cpp` 556 / `rd_viewer.cpp` 431），而光 `rd_signalling.hpp` 就 233 行 —— **超了 N6 的预算**。这不说明引擎不行，说明**那个样例做的比 N6 要求的**多（设备采集、渲染、输入回传、验证）。

N6 要的是一个**更小**的样例：信令交换 + `EngineConfig` + 三个回调 + TURN 凭据提供者 + N8 的重协商，**其余全是引擎的**（设备、编解码、BWE、RTX、恢复、渲染、AEC）。

行数**每次跑都重新量**，涨过 10% 就失败 —— 一个会漂的限制等于一个漂了的说法（计划 §5 N6）。

架构决策在 `docs/adr/ADR-036-sdk-sample-line-budget.md`（**Proposed**，待签字）。

## 目标

**让「≤200 行跑完一通话」成为可验证的事实，而不是口号。**

两件事必须同时成立：

1. **样例真的 ≤200 非注释行**（CI 每次重新量，10% 漂移余量）。
2. **样例真的跑完一通话** —— CI 的验收腿起**两个进程**（同一台机器上，模拟 NAT），过 TLS-only 的 TURN 中继，通音频 + 视频 + 控制消息。

**为什么行数是判据而不是口号**：它证明的是「**应用只贡献信令与产品 UI**」这件事。行数涨了，说明应用开始做引擎该做的事了 —— 那时要**改计划**，不是改行数。

### 现状核实（写规格时的树状态）

- **`examples/remote-desktop/` 太重**：`rd_device.cpp` 488 + `rd_verify.cpp` 556 + `rd_viewer.cpp` 431 = 1475 非注释行；`rd_signalling.hpp` 233 行（**已超 200**）；另有 `rd_local_input.hpp` 与 `game_source.hpp`。它做的是「远程桌面」—— 采集、渲染、输入回传、验证 —— N6 要的是**一通话**。
- **`examples/loopback-p2p/loopback-p2p.cpp` 是 128 非注释行** —— 一个**没有信令**的 loopback 对等连接。它证明 200 行**装得下**一个通话的引擎侧，但它**没有**信令，所以不是 N6 的答案。
- **`examples/demo-p2p/demo-p2p.cpp` 是 914 行** —— 远超预算。
- **参考信令服务在 `examples/turn-server/`**（N5 的产物：本地 coturn 容器）—— N6 的 TURN 侧依赖它。
- **N9 会给 macOS 补上 install + downstream proof** —— N6 的样例**要能在安装树上编译**（计划 §N9：「N6 的样例在**只有安装树**上编译」）。

### 与其它片的边界（本片**不做**）

- **不做** 远程桌面功能（采集 / 渲染 / 输入回传）—— 那是 `remote-desktop` 的活，已存在。
- **不做** 一个**新的**信令服务 —— 复用 `examples/remote-desktop/signalling.py` / `signalling_core.py` 与 `tools/rd_pair.py`。
- **不做** TURN 服务器 —— N5 已经交付（`examples/turn-server/`）。
- **不做** 改引擎 —— 本片只写样例与它的验收。
- **不做** 双机真跑 —— 真机跑是**归档证据**，CI 腿是两个进程在同一台机器上。

## 范围

- examples/sdk-sample/sdk-sample.cpp
- examples/sdk-sample/README.md
- examples/sdk-sample/CMakeLists.txt
- examples/sdk-sample/sdk_signalling.hpp
- examples/sdk-sample/sample_line_budget.json
- tests/acceptance/v1-sdk-sample/acceptance.test.cjs
- tests/acceptance/v1-sdk-sample/line_budget.js
- tools/check_sample_line_budget.py
- tools/check_sample_line_budget_self_test.py
- .github/workflows/ci.yml
- docs/adr/ADR-036-sdk-sample-line-budget.md
- docs/api/engine_api.md
- CHANGELOG.md
- specs/slices/v1-sdk-sample.md

> 只读 `examples/remote-desktop/` —— 本片**复用**它的信令，**不改**它。
> 只读 `examples/turn-server/` —— 本片**复用**它的 TURN，**不改**它。
> 只读 `src/` —— 本片**不改**任何 C++ 源码。

## 参考

以下是本片**只读**的路径 —— agent 读它们、仿照它们，但**不改**它们：

- 只读 `rd_device.cpp` —— `remote-desktop` 的设备侧（488 行，超出本片预算；只作参照）。
- 只读 `rd_verify.cpp` —— `remote-desktop` 的验证侧（556 行，同上）。
- 只读 `rd_viewer.cpp` —— `remote-desktop` 的观看侧（431 行，同上）。
- 只读 `rd_signalling.hpp` —— `remote-desktop` 的 C++ 信令头（233 行，**已超 200**；本片只写一个更小的客户端）。
- 只读 `rd_local_input.hpp` —— `remote-desktop` 的输入回传（不做）。
- 只读 `game_source.hpp` —— `remote-desktop` 的游戏源（不做）。
- 只读 `examples/loopback-p2p/loopback-p2p.cpp` —— 128 行的成熟 loopback 形状（参照它的做法）。
- 只读 `examples/demo-p2p/demo-p2p.cpp` —— 914 行，超出预算（只作反面参照）。
- 只读 `examples/remote-desktop/signalling.py` —— 参考信令服务（**复用**，不改）。
- 只读 `signalling_core.py` —— 参考信令服务的核心（**复用**，不改）。
- 只读 `signalling.py` —— 同上（信令服务）。
- 只读 `tools/rd_pair.py` —— 现有的配对工具（参照，不改）。
- 只读 `scripts/api_freeze_scan.js` —— CI 已有的门，本片跑它、不改它。
- 只读 `scripts/check_api_manifest.py` —— CI 已有的门，本片跑它、不改它。
- 只读 `check_readme_release.py` —— 既有的自检套路先例（`ci.yml` 里已有 `--self-test`）。
- 只读 `check_release_tag_filter.py` —— 同上（`ci.yml` 里已有 `--self-test`）。

## 设计

### 一、样例的形状

**两个进程**（`--role caller` / `--role callee`），共享一段信令：

```
caller                                    callee
  |----- offer (SDP) ------------------->|
  |<---- answer (SDP) -------------------|
  |===== ICE / DTLS / SRTP ==============|
  |<---- control message ("ping") --------|
  |----- control message ("pong") ------->|
```

**宿主写的**（就是那 200 行）：

| 部分 | 大概多少行 |
|---|---|
| 信令交换（offer / answer / candidates） | ~90 |
| `EngineConfig`（D6 的 profile 名 + 三个回调 + TURN 提供者） | ~25 |
| 三个回调（call-state / control / path-change） | ~25 |
| `start()` / 主循环 / 收尾 | ~25 |
| 杂项（参数解析、错误处理） | ~25 |

**引擎做的**：设备、编解码、BWE、RTX、恢复、渲染、AEC。

**关键设计决定**：信令**复用** `examples/remote-desktop/signalling.py`（Python 服务），C++ 侧只写**客户端**。信令协议不重新发明。

### 二、行数预算怎么量

**量的对象**：`examples/sdk-sample/` 下的**全部** `.cpp` / `.hpp`（不含 `README.md`、不含 `CMakeLists.txt`）。

**怎么算「非注释行」**：

- 空行**不算**。
- `//` 开头的行**不算**。
- `/* ... */` 块注释的**每一行**都不算（**含**块注释的第一行与最后一行）。
- 块注释**之后**同一行还有代码的 → **算**（如 `/* x */ int y = 1;` 算 1 行）。
- 字符串字面量里的 `//` **不**当注释（真注释器要处理这个；朴素的 `split('//')` 会错）。

**为什么不能用朴素的 `split('//')`**：URL 字符串（信令地址里就有 `http://`）会被误判 —— 那一行的后半段会被当成注释丢掉，**行数被低估**，预算就漏了。这是**测量工具的诚实性问题**。

**判定**：

- 预算 **200 行**（硬上限）。
- **10% 漂移余量**：实际 ≤ **220 行** 时**通过**，但**打警告**（并把警告写进 CI 日志）。
- 实际 > 220 行 → **失败**（门禁红）。
- 计划 §5 **N6-1** 的已决：「≤ 200 非注释行，CI 每次重新量，涨过 10% 就要先改计划」。

**为什么有 10% 余量而不是死守 200**：一次「加个错误处理」不该让 CI 红。余量让小的、合理的改动能过，但**连续的**小改动会在 220 那道线前被叫住 —— 那时**改计划**（计划的余量本身也是决定）。

### 三、测量工具

**为什么要有工具**：行数判定要在**每次 CI 跑**时重新算，还要能**在本地跑**。放进 CI 脚本里就没人能在提交前查了。

**`tools/check_sample_line_budget.py`** —— 纯静态，不编译、不跑样例：

1. 读 `examples/sdk-sample/sample_line_budget.json`（预算声明：`budget: 200`、`drift_allowance: 0.10`、豁免列表）。
2. 数样例目录下的 `.cpp` / `.hpp` 的非注释行。
3. 超 220 → 退出码非零 + 说清是**哪个文件**、**哪一行数**。
4. 在 200~220 → 退出码 0，但打警告。

**`tools/check_sample_line_budget_self_test.py`** —— 自检：闸门只报它认识的形态，所以自检要**钉住那个行为**：
- 一个「含 `http://` 的字符串」的行 → **算** 1 行（不是 0）。
- 一个「块注释多行」→ **不算**。
- 一个「块注释后同行有代码」→ **算** 1 行。
- 一个「超预算的样本」→ **被报出来**。
- 一个「正常样本」→ **干净**。

**与既有的自检套路同形**（`ci.yml` 里已有 `check_readme_release.py --self-test` / `check_release_tag_filter.py --self-test`）。

### 四、CI 的验收腿

**两个进程，同一台机器，模拟 NAT**：

```yaml
      # v1.4 N6: the SDK sample -- two processes on one host, behind a
      # simulated NAT that only relays over TLS.  The sample's line
      # budget is re-measured on every run; a limit that drifts is a
      # claim that drifted (plan §5 N6).
      - name: SDK sample -- line budget
        run: python3 tools/check_sample_line_budget.py

      - name: SDK sample -- self-test
        run: python3 tools/check_sample_line_budget_self_test.py

      - name: SDK sample -- two-process call behind the TLS relay
        run: node --test tests/acceptance/v1-sdk-sample/acceptance.test.cjs
```

**验收测试做什么**（`tests/acceptance/v1-sdk-sample/acceptance.test.cjs`）：

1. **静态**：样例存在、≤220 行、包含**引擎**头、**不**直接 include `nimrtc/ice/`（接缝纪律）、**不**自己写设备/编解码逻辑（静态检查：无 `PortAudio` / `VideoCapture` 之类）。
2. **静态**：样例用 `set_on_call_state`（D7）、`set_on_control_message`、`set_on_ice_recovery`（N8）、`EngineConfig::turn_credentials`（N7）—— 计划 §5 N6 点名的四个。
3. **执行**：起两个进程（caller / callee），中间过 TLS-only 的 TURN 中继与参考信令服务，通音频 + 视频 + 控制消息，判定成功。
4. **行数**：调 `tools/check_sample_line_budget.py`，断言它过。

**「没设备」怎么算**：计划 §5 N6 的诚实规则 —— 「**没设备**是 `SKIPPED`，永远不是 `PASS`」。CI 上没有设备，所以音频/视频的**采集与渲染**那部分在 CI 上是 `SKIPPED`；但**通话本身**（信令 + ICE + DTLS + SRTP + 控制消息）必须是 `PASS`。CI 腿证明的是「**引擎的管道通**」，真机跑证明的是「**设备到设备**」—— 两者分开归档。

### 五、样例的 CMake 接线

样例要能**在安装树上编译**（N9 的下游证明形状）：

- `examples/sdk-sample/CMakeLists.txt` —— 独立工程，`find_package(NimRTC REQUIRED CONFIG)` + 链接 `NimRTC::engine`。
- 树内构建时也能编（CI 的 Build 步骤之后）。

**为什么样例是独立工程而不是 `add_subdirectory`**：它必须**只**看见安装树 —— 那是「宿主项目」的样子。

## 验收标准

### 一、行数预算

1. `examples/sdk-sample/` 存在，含样例源码。
2. 样例的**非注释行** ≤ **220**（预算 200 + 10% 漂移）。
3. `tools/check_sample_line_budget.py` 存在、跑绿、退出码 0。
4. 超过 220 行的样本 → 工具**退出码非零**，并说清是**哪个文件**、多少行。
5. 在 200~220 之间的样本 → 工具退出码 0 **但打警告**。
6. `tools/check_sample_line_budget_self_test.py` 存在且跑绿。
7. 自检里：含 `http://` 的字符串行**算** 1 行（不误判为注释）。
8. 自检里：多行块注释**不算**行数。
9. 自检里：块注释后同行有代码**算** 1 行。
10. 预算声明在 `examples/sdk-sample/sample_line_budget.json` 里（`budget: 200`、`drift_allowance: 0.10`）。
11. CI 每次跑都**重新量**（CI 里有这一步，不是只在合并时量一次）。

### 二、样例的形状

12. 样例 ≤220 非注释行，**且**它建立**音频 + 视频 + 控制**三路。
13. 样例**包含**引擎头（`<nimrtc/engine/engine.hpp>`）。
14. 样例**不**直接 include `nimrtc/ice/` 的任何具体头（接缝纪律，L4）。
15. 样例**不**自己写设备 / 编解码 / 渲染逻辑（静态检查：无 `PortAudio` / `VideoCapture` / `VideoRenderer` 之类）。
16. 样例**不**写设备 id（「没有设备 id 配置」是 N6 的条件之一）。
17. 样例用 `set_on_call_state`（D7）、`set_on_control_message`、`set_on_ice_recovery`（N8）、`EngineConfig::turn_credentials`（N7）。
18. 样例**不**自己实现连通性策略（用 D6 的 profile）。
19. 样例**不**自己实现重连策略（用 N3 的引擎侧策略）。
20. 样例的 `CMakeLists.txt` 用 `find_package(NimRTC REQUIRED CONFIG)` + `NimRTC::engine`。

### 三、CI 验收腿

21. CI 有「行数预算」这一步。
22. CI 有「自检」这一步。
23. CI 有「两个进程通话」这一步。
24. 验收测试真的起**两个**进程（caller + callee）。
25. 通话经过 **TLS-only** 的 TURN 中继（不是明文 TCP）。
26. 通话建立后**控制消息**双向可达。
27. **没设备**时，采集/渲染那部分判 `SKIPPED`，**不**判 `PASS`。

### 四、边界与回归

28. 本片**不改** `src/` 的任何 C++ 源码（静态检查）。
29. 本片**不改** `examples/remote-desktop/` 与 `examples/turn-server/`（只复用）。
30. `examples/sdk-sample/` 的行数声明与实际一致（工具跑绿）。
31. `scripts/api_freeze_scan.js` 跑绿（CI 已有的门，本片不能碰坏）。
32. `scripts/check_api_manifest.py` 跑绿。

### 五、文档

33. `docs/adr/ADR-036-sdk-sample-line-budget.md` 记录决策与**怎么量**。
34. `examples/sdk-sample/README.md` 写明：怎么跑、需要什么、行数预算是多少。
35. `docs/api/engine_api.md` 加一节「一个 ≤200 行的宿主长什么样」。
36. `CHANGELOG.md` 记录样例与行数预算。

## 本片不做

- **不做** 远程桌面功能 —— `remote-desktop` 已存在，本片不重做。
- **不做** 新的信令服务 —— 复用现有的。
- **不做** TURN 服务器 —— N5 已交付。
- **不做** 改引擎 —— 本片只写样例。
- **不做** 双机真跑的自动化 —— 真机跑是**归档证据**（人工执行、归档结果）。
- **不做** 把行数上限提到 200 以上 —— 要提就**先改计划**（§5 N6-1）。

## 接口变更

**无 C++ 接口变更。** 本片只写样例、工具与 CI。

| 文件 | 变化 |
|---|---|
| `examples/sdk-sample/sdk-sample.cpp` | **新增**（样例主文件） |
| `examples/sdk-sample/sdk_signalling.hpp` | **新增**（信令客户端） |
| `examples/sdk-sample/CMakeLists.txt` | **新增** |
| `examples/sdk-sample/README.md` | **新增** |
| `examples/sdk-sample/sample_line_budget.json` | **新增**（预算声明） |
| `tests/acceptance/v1-sdk-sample/acceptance.test.cjs` | **新增** |
| `tests/acceptance/v1-sdk-sample/line_budget.js` | **新增**（CI 侧的计数） |
| `tools/check_sample_line_budget.py` | **新增** |
| `tools/check_sample_line_budget_self_test.py` | **新增** |
| `.github/workflows/ci.yml` | 加三步（行数 / 自检 / 通话） |
| `docs/adr/ADR-036-sdk-sample-line-budget.md` | **新增** |

## 测试策略

### 本地可跑

- `python tools/check_sample_line_budget.py` —— 行数预算。
- `python tools/check_sample_line_budget_self_test.py` —— 计数器的自检（**这是最容易写错的地方**，所以要钉住）。

### CI 上跑

- 三步：行数预算 → 自检 → 两个进程通话。

### 回归

- CI 已有的三道 API freeze 门。
- CI 已有的 Build / Test。

## 风险

| 风险 | 缓解 |
|---|---|
| **200 行装不下**（信令本身就 90+ 行） | `rd_signalling.hpp` 233 行说明**成熟的**信令实现装不下 —— 但那个实现做的事**比 N6 多**（设备、输入回传）。N6 的样例**只做信令**，且**复用** `signalling.py` 服务端（C++ 侧只写客户端）。若仍装不下：**先问**，不要偷偷加行数（§5 N6-1：涨过余量就要改计划）。 |
| **行数计数器被朴素实现骗了**（`http://`） | 自检专门钉这个（验收标准 7）。宁可漏判**行**也不能**低估** —— 低估会让预算漏。 |
| **CI 上没设备** → 看起来通过了 | 计划 §5 N6 的诚实规则：**没设备是 `SKIPPED`，永远不是 `PASS`**（验收标准 27）。 |
| **两个进程的验收在 CI 上不稳** | 参照 `examples/loopback-p2p` 的成熟形状（128 行，已在 CI 跑）。超时要有**明确的失败原因**，不许「重试到过为止」。 |
| **样例依赖了引擎不该暴露的东西** | 静态检查：`src/engine/` 的公开面够不够用。**不够**是**引擎的缺口**，要**另开一片**（不是让样例绕过接缝纪律）。 |
| **行数预算被人绕过**（把代码拆到 `headers/` 外的目录） | 工具数的是**样例目录下的全部** `.cpp` / `.hpp`，不是某几个指定文件。豁免列表必须**为空**（或只剩 `README.md`）。 |

## 归属

**`version/v3`**。本片不碰 `src/` —— 只写样例、工具与 CI。**不撞** N8 / N9 的区域，但波次 6 依赖前面全部的成果。

**前置**：N3 / N7 / N8 / N9 / D7 已合入 `version/v3`。N6 的样例**用** D7 的 call-state、N7 的凭据提供者、N8 的重协商 —— 三者缺一，样例就写不出来。N9 给了「在安装树上编译」的证明形状。

**后续**：这是 v1.4 的**最后一片**。做完之后 v1.4 的十六条全部落地，版本可以进 M2（合并到主干）。
