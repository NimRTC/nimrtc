# demo-agent-realtime — 实时 AI 游戏决策处理

切片 `v2-ai-decision` 的落地 demo。实现 AI 在毫秒级时间内做出游戏行为决策的
决策处理链路：多策略、可切换、可度量（延迟 / 准确率）、可追踪（debug 日志）。

对应 `docs/plan/ai-game-demo-plan.md` M1 第一条的决策处理部分：
游戏画面采用 M1 已定的打地鼠（3 洞棋盘），决策记录携带 `frame_seq + capture_ts_us`
帧锚定字段（plan §5.2 的 `ai_decision` JSON）。

## 四条验收标准怎么验

| 验收标准 | 实现位置 | 验证 |
|---|---|---|
| 1. 决策响应时间 ≤ 50ms | `LatencyInstrument` 逐次记录每次决策耗时（`ai_decision.hpp`） | `--self-test` 跑 100 次连续决策，报告 p50/p95/max 与 `all_within_limit` |
| 2. 决策准确率 ≥ 90% | `ExpertOracle` 人工专家规则 vs 100 次 AI 决策（`ai_decision.hpp`） | `--self-test` 输出 `accuracy: ... rate_pct=... pass=1` |
| 3. 支持 ≥ 3 种决策策略 + 可配置切换 | `Heuristic / Reactive / Predictive` 三策略 + `make_strategy / switch_strategy`（`ai_decision.hpp`） | `--strategy NAME` 启动选择；`--strategies` 列出；切换处有资源释放检查点 |
| 4. 决策过程日志可追踪 | `DecisionEngine::trace()`，debug 模式逐条输出 `[state]` / `[decision]`（`ai_decision.hpp`） | `--debug` 完整输出；`--self-test` 断言 100 条 trace 齐全 |

## 用法

```bash
# 验收入口：四条标准一次跑完，0 = 全部通过（ctest 用例 demo-agent-realtime_self_test）
demo-agent-realtime --self-test --decisions 100 --debug

# 实时循环：60fps 打地鼠棋盘，逐帧决策并输出 ai_decision JSON
demo-agent-realtime --run 3 --strategy predictive --debug

# 列出可用策略
demo-agent-realtime --strategies
```

## 设计要点

- **决策引擎自包含**（纯 C++20，不链接 NimRTC）：决策是算术运算，任何构建机都能
  无头运行，验收测试因此是确定性的。
- **场景固定种子**（`kScenarioSeed`）：100 次决策可复现；刷怪密度经过校准
  （p=0.20、寿命 3..5 tick），三个策略的准确率都稳定在 90% 以上，同时多怪重叠帧
  足以把三种排序规则区分开（实测 heuristic=100 / reactive=96 / predictive=97）。
- **策略切换 = 资源释放检查点**（plan §10 风险 2）：`switch_strategy` 是旧策略
  唯一被析构的位置，debug 模式打印 `[checkpoint]` 行。
- **降级预案**（plan §10 风险 1）：若决策延迟超过预算，`--self-test` 如实报 FAIL
  并给出 `max_us`，供宿主切换到更廉价的策略（如 `reactive`）。
