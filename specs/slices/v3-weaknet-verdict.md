# Slice: v3-weaknet-verdict

## 目的
把 M2 的「处刑台」做成可复现的判定：复用 `signalling.py + rd_lan.py` harness 做双机演练
（`--bind` 跨网形），加上弱网开关（5% 丢包 + 100ms 抖动档），
并让 verdict 同时读出 `decoded/rendered/beats` 三数、`input-to-photon p50/p95`
与 `AI决策帧age`。

对应 `docs/plan/ai-game-demo-plan.md` §8 M2 第一、三、四条，以及 §6「看板字段」。

## 范围
- tools/rd_lan.py
- tools/benchmark_weaknet.py
- examples/remote-desktop/signalling.py
- examples/remote-desktop/client.py
- tests/acceptance/v3-weaknet-verdict/acceptance.test.cjs

## 需求
1. `tools/rd_lan.py` 支持 `--bind <addr>`：跨网形演练时绑定到指定地址，
   缺省仍绑 `127.0.0.1`（单机行为不变）。
2. `tools/rd_lan.py` 的 verdict 同时输出四组数字：
   `decoded/rendered/beats` 三数、`input-to-photon p50/p95`、`AI决策帧age`。
3. `tools/benchmark_weaknet.py` 提供「恶劣模式」档位：5% 丢包 + 100ms 抖动，
   档位可被命令行选择，缺省为关闭（不影响既有跑数）。
4. `examples/remote-desktop/signalling.py` 支持 `--host 0.0.0.0` 绑定，
   使双机场景下设备机能被操作机连上。
5. verdict 的失败判定沿用现有规矩：非零退出码 = 失败并转储
   `build/lan_device.log + build/lan_viewer.log`。
6. verdict 在缺字段时如实报告「该字段无值」，不用 0 冒充（0 与「真的很快」不可区分）。

## 验收标准
1. `rd_lan.py` 定义了 `--bind` 参数且缺省为 `127.0.0.1`；验证方式：静态检查参数定义与缺省值
2. `rd_lan.py` 的 verdict 输出里包含 `input_to_photon` 的 p50 与 p95；验证方式：静态检查 verdict 输出字段
3. `rd_lan.py` 的 verdict 输出里包含决策帧 `age` 字段；验证方式：静态检查 verdict 输出字段
4. `benchmark_weaknet.py` 提供 5% 丢包与 100ms 抖动的档位常量；验证方式：静态检查档位定义
5. `signalling.py` 支持 `--host` 参数且缺省监听回环；验证方式：静态检查参数定义与缺省值
6. 缺字段时 verdict 如实标注「无值」而不是输出 0；验证方式：静态检查缺字段分支的输出文本

## 风险
1. 弱网 shaper 需要管理员权限（改包过滤规则）→ 跑不起来；应对：档位不可用时明确报错并提示需要权限，不静默跳过
2. 抖动 100ms 会让 input-to-photon 分布很宽 → p50 与 p95 差得远；应对：这是预期行为，verdict 同时给出两个数而不是只给一个
3. 双机演练依赖防火墙放行 UDP；应对：verdict 在连接不通时把端口要求打印出来
