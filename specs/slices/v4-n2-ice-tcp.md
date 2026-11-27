```markdown
# Slice: v4-n2-ice-tcp

## 目的
把 v1.4 计划里 N2 的另一半补齐：TCP 候选**真的被收集、发布进 SDP、被 ICE 检查驱动用上**，
使两个只能通过 TCP 互通的 peer 能建立一条 protocol 读作 "tcp" 的连接。
候选的形状、socket、framing 与 check 驱动在 `version/v3` 上已经存在 —— 本片只补「接线」与「策略」。

## 现状核实（写规格时的树状态，version/v3 @ a2bb408c）

- **已在**：`src/modules/ice/include/nimrtc/ice/tcp_candidate.hpp`（`TcpCandidateRole` Active/Passive、
  `TcpCandidate` 形状、foundation 命名 "tcp-N"）；`tcp_socket.*`（TCP 监听/连接 + 非阻塞）；
  `tcp_framing.*`（长度前缀帧）；`tcp_ice.*`（TCP 上的 STUN 检查）；单测 `test_ice_tcp.cpp` /
  `test_tcp_candidate.cpp` / `test_tcp_framing.cpp` / `test_tcp_stun.cpp`。
- **缺**：① 候选收集没有把 host TCP 候选**放进去**（active / passive 两种角色）；
  ② SDP writer 不发 `tcptype`；③ 解析器读 `tcptype` 后没有把它带进候选；
  ④ check 驱动**总是**先试 UDP、没有「UDP 失败后才尝试 TCP」的次序判据与可观测证据。

## 范围
要改的（这些是 agent 会去改的文件）：
- `src/modules/ice/include/nimrtc/ice/tcp_candidate.hpp`（只加「收集入口」声明，若需要）
- `src/modules/ice/src/tcp_ice.cpp` / `tcp_ice.hpp`
- `src/modules/ice/src/ice.cpp`（或收集器所在 TU —— 以现有 `IceTransport` 的候选收集实现为准）
- `src/modules/ice/tests/test_tcp_candidate.cpp` / `test_ice_tcp.cpp`
- `src/modules/sdp/src/parser.cpp`
- `src/modules/sdp/src/munger.cpp`（tcptype 的读与写）
- `tests/acceptance/v4-n2-ice-tcp/acceptance.test.cjs`

只读参考（**不得修改**，agent 只许读）：
- 只读 `src/modules/ice/` 里已有的 UDP 收集与优先级实现 —— 排序要与它同一套口径
- 只读 RFC 6544 §4.3 / §4.5 —— 类型优先级与候选优先级公式的依据

## 需求
- N2：`IceTransport` 收集带 active / passive tcptype 的 host TCP 候选，并按 RFC 6544 §4.3 与 UDP 排优先级。
  - SDP 发出 `a=candidate:… tcp <pri> <host> <port> typ host tcptype passive`（active 同理）
  - 解析器读回 `tcptype` 后，候选的 protocol 为 "tcp"
  - 两个只能 TCP 互通的 peer 选出 TCP 候选对，被选中对的 protocol 读作 "tcp"
  - **TCP 只在 UDP 失败之后才尝试**，且这个次序有可观测证据（不是注释）

## 验收标准
1. `IceTransport` 收集带 active/passive tcptype 的 host TCP 候选；验证方式：检查输出的 SDP 含
   `a=candidate:… tcp <pri> <host> <port> typ host tcptype passive` 格式的候选。
2. TCP 候选优先级按 RFC 6544 §4.3 与 UDP 候选正确排序；验证方式：单测断言**具体数值**（不是「单调」）。
3. 两个只能 TCP 互通的 peer 选出 TCP 候选对；验证方式：环回测试里让 UDP 绑到不可路由地址，
   断言最终连接的 protocol 为 "tcp"。
4. TCP 尝试仅在 UDP 失败后触发；验证方式：单测记录检查驱动的尝试次序（一个可断言的序列），
   断言所有 UDP 尝试先于第一个 TCP 尝试。

## 风险
1. TCP 优先级公式可能与 RFC 6544 不一致；应对：按 §4.3 实现并附规范引用注释，单测钉具体数值。
2. 环回测试的 UDP/TCP 路由控制不稳；应对：用确定的不可路由地址（如 192.0.2.0/24 TEST-NET），不用随机端口去赌。
```
