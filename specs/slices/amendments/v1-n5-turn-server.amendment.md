# 切片规格修订记录 —— `v1-n5-turn-server`

> 本文件是**修订记录**，不是规格。
> 已签字的规格 `specs/slices/v1-n5-turn-server.md` 保持原样不动 ——
> 它的 `frozenHash` 是签字时的证据锚点，改写它就等于抹掉签字那一刻的事实。
> 这里记录的是：实现与 frozen spec 的偏离、偏离的原因、以及偏离的范围。

## 一、偏离事实

| 项 | frozen spec（签字时） | 实际实现 |
|---|---|---|
| TLS-only 的表达方式 | 「命令或配置包含 `--tls-only`」 | **端口分离**：TLS 监听 5349，plain 监听 3478，客户端只被告知 5349 |

frozen spec 里提到 `--tls-only` 的地方共 2 处：
「需求 N5」条目、验收标准 3。

frozenHash：`sha256:（见 .zelto/slices/v1-n5-turn-server.json 的 spec.frozenHash）`

**修订稿的来历**：实现阶段读 coturn 自己的源码
（`src/apps/relay/mainrelay.c`）而不是它的文档，发现 **`--tls-only`
这个选项根本不存在** —— coturn 4.5.2 与 4.6.3 都没有。照 frozen spec
的字面要求去传这个开关，coturn 会打印 "unrecognized option" 并**退出**，
服务器根本起不来。

因此实现改用**语义等价**的端口分离，并把它记在这里。

## 二、为什么偏离

coturn 的 TLS 开关只有两个方向，没有第三个：

- `--no-tls` / `--no-dtls`：**关闭** TLS 监听（"Do not start TLS client listeners"）；
- `tls-listening-port=<n>`：**多开一个** TLS 监听端口。

coturn **没有**「只允许 TLS、不允许明文」的单向开关。原因是它的设计：
一个 coturn 进程可以同时持有 plain 监听和 TLS 监听，两者靠**端口**区分。
「TLS-only」在这套选项词汇里不是开关，而是**部署形态** —— 让明文落在
另一个端口上，客户端只被告知 TLS 端口。

实现选择的就是这个形态：

- `turnserver.conf`：`listening-port=3478`（plain）、`tls-listening-port=5349`（TLS）；
- `ci-acceptance.sh` phase 2：把 plain 服务器开到**同一个 5349** 端口上，
  用**同一个 TLS 客户端**去连 —— 它必须拿不到分配。

phase 2 就是「TLS-only」这个语义的可执行形式：不是靠一个开关声明，
而是靠**同一个端口上换成明文服务端、TLS 客户端就拿不到中继地址**来证明。

## 三、偏离的范围

**只影响「TLS-only 这个词怎么落地」，不影响任何可观察行为。**

TLS-only 要保证的性质是：**一个被告知「用 TLS」的客户端不能落到明文上**。
端口分离与 `--tls-only`（若它存在）保证的是同一个性质，且
`ci-acceptance.sh` 的负控（phase 2）验证的就是这个性质本身。

验收标准 3 因此从「字面量存在」改成了「**语义等价物存在且被验证**」：
断言 TLS 监听端口与 plain 监听端口**不同**，并断言部署里明说了这个意图。

## 四、这次偏离暴露了什么

frozen spec 的验收标准 3 写的是「存在 `--tls-only` 参数」——
一条**只有对照 coturn 真实选项词汇才能判定**的标准。
它写在规格里，但规格本身没有验证过 coturn 有这个选项。

这与 `v3-n4-d6-connectivity` 那次偏离是同一个形状：
**规格的措辞比现实宽**时，照字面实现会得到一个起不来的服务器；
而如果实现绕过了字面要求（像本次），一条只查字面量的验收判据会把
**正确的实现**判成不合格。

具体到本例：
- 实现是**对的**（负控证明了性质成立）；
- 但一条只 grep `--tls-only` 的判据会把它判成红的；
- 而一条真去 `turnserver --help` 查选项的判据，能在实现阶段就把这个偏离
  暴露出来 —— 不必等事后补记录。

**留给后续的改进方向**（尚未实现，不在 v1.4 范围内）：
凡验收标准点名某个**外部工具的选项**的，都应该对照那个工具自己的
选项表核对，而不是照需求文档里的措辞转写。
本仓库已有这个形态的先例 —— `tools/check_turn_server.py` 就是
「对照 coturn 的真实选项词汇」的静态守卫，它与本片的负控一起构成两道防线。

## 五、相关提交

| 提交 | 内容 |
|---|---|
| `14eeee08` | `feat(v1-n5-turn-server): slice approved` —— 含 frozen spec |
| `a2967b4b` | `feat(ice): the TURN-over-TLS transport seam (v1.4 N1)` (#139) —— TURN/TLS 客户端侧 |
| `cc216729` | `feat(examples): the TURN deployment is reviewed, fixed, and guarded (#147)` —— 部署侧的七处修正与静态守卫 |

## 六、修订后的验收标准 3

> 3. 参考部署启用 coturn 时启用 TLS-only 模式；验证方式：静态检查启动脚本或配置文件，
>    **TLS 监听端口与 plain 监听端口被分开配置**（coturn 无 `--tls-only` 选项，
>    端口分离是它的语义等价物），且 README 或注释里明说了该语义。

## 七、验收测试与实现验证的分工

`tests/acceptance/v1-n5-turn-server/acceptance.test.cjs` 断言**部署文件说得对**
（安装步骤、证书生成、TLS-only 语义、compose 只是归档、本文件不碰服务器生命周期）。

`examples/turn-server/ci-acceptance.sh` 断言**服务器做得对**
（TLS 握手能验证、真的分配到中继地址、plain 负控拿不到分配），
由 CI 的 `linux-turn` leg 在装了 coturn 的 Linux runner 上执行。

两者都要有：只做静态检查，服务器可以不 TLS 也照样「合规」；
只做行为检查，部署文件写错也没人发现。
