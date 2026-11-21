# Slice: v2-ice

## 目的

实现 **ICE-over-TCP**（RFC 6544）：两个只能通过 TCP 互通的 peer 选出 TCP 候选对，
被选中对的 `protocol` 读作 `"tcp"`。

这一片把三件事补齐：**TCP 候选的收集**（host + active/passive `tcptype`）、
**TCP 候选的 SDP 序列化**（`a=candidate:… tcp <pri> <host> <port> typ host tcptype passive`）、
**TCP 路径的连通性检查与提名**（N2-1：TCP 在 UDP 失败后才尝试）。

## 背景与边界（读规格前必读）

**libjuice 的 ICE 层是纯 UDP 的，本片不动它。** 核实到的事实：

- `src/third_party/libjuice/src/src/ice.c` 的 `parse_sdp_candidate()` 里
  `if (strcmp(transport, "UDP") != 0) return ICE_PARSE_IGNORED;`
  —— 收到 TCP 候选**直接丢弃**。
- 同一文件的 `ice_generate_candidate_sdp()` 只发出
  `typ host` / `typ srflx` / `typ prflx` / `typ relay`，**没有 `tcptype`**。
- 只读 `src/third_party/libjuice/src/src/ice.h` 的 `ice_candidate_t`
  **没有 transport 字段、也没有 tcptype 字段**。
- 只读 `src/third_party/libjuice/src/src/conn.c` 与
  `src/third_party/libjuice/src/src/conn.h`：连接抽象建在 `udp_socket_config_t` 上，
  **没有 TCP 连接模式**。
- 只读 `src/third_party/libjuice/src/src/turn_tcp.c`：
  那是 **TURN 控制连接**（RFC 8656 §12），不是 ICE-over-TCP。

所以：**NimRTC 侧自己建 TCP 传输层**，libjuice 继续管 UDP 那条路。
两条路在 `IceTransport` 这一层汇合——谁提名了对，seam 就报谁。

设计决策与理由见只读 `docs/adr/ADR-028-ice-over-tcp-nimrtc-side.md`。

**SDP munger 不需要改。** 只读 `src/modules/sdp/src/munger.cpp` 的
「ICE candidates」那段是把 `m.candidates` 里的候选体**原样**写回
（`out += "a=candidate:"; out += cand;`）。它从不过滤候选体。
所以「SDP writer 没有 `tcptype`」这个缺口在**产出候选体的人**那里，
不在 munger 里。本片**不改 munger 的生产代码**（可以加测试钉住这个行为）。

## 范围

写入：
- `src/modules/ice/**`
- `src/modules/sdp/**`
- `src/plugins/include/nimrtc/plugins/ice_transport.hpp`（**已授权**：加 `tcptype` 字段）

只读参考（**不得修改**）：
- 只读 `docs/adr/ADR-028-ice-over-tcp-nimrtc-side.md` —— 本片的设计依据
- 只读 `src/modules/raw_udp/src/udp_socket.cpp` —— TCP socket 照它的形状建
- 只读 `src/engine/src/engine_signaling.cpp` —— 候选怎么进 SDP 的
- 只读 `src/third_party/libjuice/src/src/ice.c` —— 只读参考：libjuice 的 UDP-only 事实
- 只读 `src/third_party/libjuice/src/src/ice.h` —— 只读参考：`ice_candidate_t` 没有 tcptype 字段
- 只读 `src/third_party/libjuice/src/src/conn.c` —— 只读参考：连接抽象建在 UDP 上
- 只读 `src/third_party/libjuice/src/src/conn.h` —— 只读参考：没有 TCP 连接模式
- 只读 `src/third_party/libjuice/src/src/turn_tcp.c` —— 只读参考：那是 TURN 控制连接，不是 ICE-over-TCP

新建文件（在本片写集内）：
- `src/modules/ice/src/tcp_socket.cpp` —— 跨平台 TCP socket
- `src/modules/ice/src/tcp_framing.cpp` —— RFC 6544 §3.2 的流帧
- `src/modules/ice/src/tcp_ice.cpp` —— TCP 候选收集与提名驱动

## 需求

- **N2**：IceTransport 收集带 active / passive `tcptype` 的 host TCP 候选
  （RFC 6544 §4.5），并按 RFC 6544 §4.3 与 UDP 排优先级。
- **N2**：SDP writer 发出 `a=candidate:… tcp <pri> <host> <port> typ host tcptype passive`，
  补齐当前仓库中不存在的代码路径。
- **N2-1**：TCP 在 UDP 失败之后才尝试，不是和 UDP 赛跑。
- **N2 验收**：两个只能通过 TCP 互通的 peer 选出 TCP 候选对，被选中对的 protocol 读作 "tcp"。
- **N2 测试形态**：环回测试里让 UDP socket 绑到互相不可路由的地址，TCP 是唯一通路，
  模拟 TCP-only 防火墙形状，在 CI 中保持确定性。

## 接口约定

### 1. seam 的加法式扩展（`plugins::IceCandidate`）

`src/plugins/include/nimrtc/plugins/ice_transport.hpp` 的 `plugins::IceCandidate` 加一个字段：

```
std::string tcptype;   // "active" / "passive" / ""（空 = 不是 TCP 候选）
```

**这是对冻结接口（ADR-015）的加法**：新字段带默认值，不改任何已有签名。

**已确认（2026-11-21，人拍板）**：授权扩这个冻结接口。
所以本片的写集**包含** `src/plugins/include/nimrtc/plugins/ice_transport.hpp`，
上面那个 `tcptype` 字段加在这里，验收标准 4 读 `protocol` / `tcptype`
走的就是 seam。

同时**必须改掉那句已经变成假话的注释**：现在写的是
*"ICE-over-TCP (RFC 6544) is not implemented yet (plan C2), so this is
"udp" for every pair a v1.2.0 engine can select."* —— 本片之后它就不成立了，
注释要改成实际语义。

### 2. `ice::Candidate` 的对应扩展（`src/modules/ice/include/nimrtc/ice/ice.hpp`）

```
struct Candidate {
    ...
    std::string tcptype;   // "active" / "passive" / ""
    ...
};
```

- `Candidate::to_sdp()` 要能发出 `tcptype`；
- `parse_candidate()` 要能读回 `tcptype`。
  **注意**：`parse_candidate()` 现在读到 `typ <type>` 之后就去认 `raddr` 了，
  必须先认 `tcptype`——而且**不能把 `tcptype` 误认成 `typ`**。

### 3. 新的 TCP 协作者（`src/modules/ice/`）

| 文件 | 职责 |
|---|---|
| `src/modules/ice/include/nimrtc/ice/tcp_candidate.hpp` | TCP 候选的形状（host / port / tcptype）+ SDP 序列化 |
| `src/modules/ice/src/tcp_socket.hpp` + `tcp_socket.cpp` | 跨平台 TCP socket（connect / listen / accept / send / recv） |
| `src/modules/ice/src/tcp_framing.hpp` + `tcp_framing.cpp` | RFC 6544 §3.2 的流帧（2 字节长度前缀） |
| `src/modules/ice/src/tcp_ice.hpp` + `tcp_ice.cpp` | 驱动：收集 TCP host 候选、交换、连通性检查、提名 |

**`tcptype` 两种都发（已确认，2026-11-21，人拍板）**：
`active`（本方主动连对方）和 `passive`（本方监听等对方连）**两种都要收集并发出**。
对端任一为 `passive` 即可成对 —— 所以两侧各发两种时，
总有一对能配上（一个 active 连一个 passive）。

**TCP socket 照只读 `src/modules/raw_udp/src/udp_socket.cpp` 的形状建**，两条纪律必须照抄：

1. **退役而不是关闭 fd**：`close()` 把 handle 从 atomic 里换出来、标成 invalid，
   真正的 `::close()` 推迟到析构。理由：另一个线程可能正在 `recv()` 里，
   在它读 fd 的时候关掉 fd 是内核 fd 状态的数据竞争。
2. **先轮询再进系统调用**：进 `recv()` 之前先 `poll()`/`select()` 一个**有界**的超时，
   这样调用方每个周期都能看见 stop flag，不必把 socket 从线程底下抽走。

### 4. 优先级（RFC 6544 §4.3）

- `active`（我们连对方）**优先于** `passive`（我们监听等对方连）。
- TCP 候选与 UDP 候选按 RFC 8445 §5.1.2.1 的类型阶梯排
  （host 126 / prflx 110 / srflx 100 / relay 0）。
- **TCP 候选的 foundation 必须与 UDP 候选区分开**（例如 `tcp-1`），
  优先级也必须落在与 UDP 候选**不冲突**的区间——
  否则同 foundation 同优先级的两个候选，对端分不出是哪一个。

### 5. N2-1：TCP 在 UDP 失败之后才尝试

**TCP 候选不是和 UDP 候选并行收集的。** 进入条件是「UDP 这条路没能提名」：

- **已确认（2026-11-21，人拍板）**：判据是 **ICE agent 报 `Failed` 状态**
  （`plugins::IceState::Failed`）才进入 TCP 路径。
  不是「超时」，也不是「预算耗尽没提名」。

**推论（必须处理）**：如果 offer 是在「UDP 失败」这个事实**之前**发出去的，
那它**不可能**带着 TCP 候选——TCP 候选只能靠 **trickle ICE**（RFC 8838）
后补。所以：

- 收到对方后补的 TCP 候选 → 走**已有的** `add_remote_candidate()`；
- 本方后补的 TCP 候选 → 走**已有的** `local_description()` 重新发出去。

## 验收标准

1. **TCP 候选的收集与序列化**：`IceTransport` 能收集本机 TCP host 候选，
   并发出带 `tcptype` 的 SDP 候选行，例如
   `a=candidate:tcp-1 1 TCP 2105458943 192.0.2.1 9 typ host tcptype passive`。
   验证方式：`local_description()` / `gathered_local_candidates()` 的输出里
   存在一行 `TCP` 传输、且带 `tcptype`（`active` 或 `passive`）的候选。

2. **`tcptype` 的往返（parse ⇄ to_sdp）**：`parse_candidate(to_sdp(c))` 之后，
   `tcptype` 与 `transport` 都不丢。
   验证方式：单元测试对 `active` / `passive` 各做一次往返断言。

3. **`tcptype` 不被误认成 `typ`**：`parse_candidate()` 读到 `tcptype passive` 时，
   `type` 仍然是 `Host`，不是别的。
   验证方式：单元测试断言解析 `... typ host tcptype passive` 得到的
   `type == Host` 且 `tcptype == "passive"`。

4. **优先级排序（RFC 6544 §4.3）**：`active` 候选的优先级 **>** `passive` 候选的优先级；
   TCP 候选的 foundation 与 UDP 候选的不同；TCP 候选的优先级与同传输的 UDP 候选**不冲突**。
   验证方式：单元测试对三种候选算优先级并断言大小关系与不相等。

5. **主验收 —— TCP-only 防火墙形状下选出 TCP 候选对**：环回测试，两个 transport，
   UDP 被弄成不可用（绑到互相不可路由的地址），TCP 是唯一通路。
   验证方式：两个 peer 各自选中一对，且**被选中对的 `protocol` 读作 `"tcp"`**
   （通过 `plugins::IICETransport::selected_pair()` 读）。

6. **负控制 —— UDP 可用时不得选 TCP**：同一测试里，UDP 正常的那一路，
   **被选中对的 `protocol` 读作 `"udp"`**。
   验证方式：断言 UDP 正常时选中对的 `protocol == "udp"`。
   （这条不能省：没有它，「TCP 是 fallback」会退化成「TCP 是唯一的东西」，
   而后者照样能通过验收标准 5 —— 却是错的。）

7. **N2-1 —— TCP 只在 UDP 失败之后才尝试**：在 UDP 仍然能提名的那条路上，
   **TCP 候选根本没有被收集**（不是「收集了但没被选中」）。
   验证方式：断言 UDP 正常时收集到的候选里没有任何 `TCP` 传输的候选。

8. **SDP writer 真的发得出 `tcptype`**：生成的 SDP 里那一行是完整的
   `a=candidate:<foundation> <component> TCP <priority> <host> <port> typ host tcptype <active|passive>`
   —— 字段一个不少，`tcptype` 在 `typ host` **之后**。
   验证方式：单元测试断言 munger 输出的候选行逐字段匹配。

9. **确定性**：测试**不依赖**外部网络、Docker daemon、宿主防火墙配置、NAT 或时序运气。
   验证方式：同一个二进制在同一台机器上反复跑，结论一致。

## 风险

1. **STUN 编码要写第二遍**：libjuice 的 UDP 路径已经有 STUN binding 的编解码，
   NimRTC 的 TCP 路径需要自己写一遍（RFC 8489 §6）。这是本片最大的重复面，
   写完要对着 RFC 8489 逐字段核。
2. **TCP 流的分帧**：ICE-over-TCP 走的是字节流，不是数据报 ——
   一次 `recv()` 不保证拿到一个完整的 STUN 消息。
   RFC 6544 §3.2 的 2 字节长度前缀必须实现对，否则解析会错位。
3. **fd 生命周期**：`UdpSocket` 里那两条纪律（退役不关闭、先轮询再进系统调用）
   是踩过坑才定下来的，TCP socket 照抄，不要自己发明。
4. **和 UDP 那条路的汇合**：两条路各有各的提名状态。
   「谁赢了」必须只有一个答案，`selected_pair()` 不能一会儿报 UDP 一会儿报 TCP。
5. **优先级撞车**：TCP 候选和 UDP 候选的 foundation / priority
   如果撞了，对端会挑错候选。

## 已拍板的事（2026-11-21）

规格里原本有三处没有答案。已由人拍板，写在这里以免实现时再猜：

1. **seam 的 `plugins::IceCandidate::tcptype` 字段**：**授权扩这个冻结接口**
   （ADR-015 的加法式：新字段带默认值，不改任何已有签名）。
   写集因此包含 `src/plugins/include/nimrtc/plugins/ice_transport.hpp`。
2. **N2-1 的「UDP 失败」判据**：**ICE agent 报 `Failed` 状态**
   （`plugins::IceState::Failed`）才进入 TCP 路径。
3. **TCP 候选的 `tcptype` 角色**：**`active` 和 `passive` 两种都发**。
